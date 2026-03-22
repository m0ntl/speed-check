#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#ifndef _WIN32
#include <termios.h>
#else
#include <windows.h>
#include "terminal_win.h"
#endif

#include "interactive.h"
#include "client.h"
#include "server.h"
#include "icmp.h"
#include "spdchk.h"
#include "udp.h"
#include "metrics.h"

/* ================================================================== */
/* ANSI escape codes                                                    */
/* ================================================================== */
#define A_CLEAR   "\033[2J\033[H"
#define A_BOLD    "\033[1m"
#define A_DIM     "\033[2m"
#define A_INVERT  "\033[7m"

#ifdef _WIN32
/*
 * \033[2m (dim) renders as near-invisible text on dark Windows console
 * themes.  Dark-gray (\033[90m) achieves the same visual weight and is
 * reliable across conhost, Windows Terminal, and VS Code terminal.
 */
#  undef  A_DIM
#  define A_DIM "\033[90m"
#endif
#define A_YELLOW  "\033[33m"
#define A_CYAN    "\033[36m"
#define A_RESET   "\033[0m"

#define SEP_LINE  "======================================================\n"
#define THIN_LINE "------------------------------------------------------\n"

/* ================================================================== */
/* AppState — drives render_current_screen() and update_logic()        */
/* ================================================================== */
typedef enum {
    STATE_MAIN_MENU,
    STATE_RUNNING_TEST,
    STATE_RUNNING_SERVER,
    STATE_VIEW_RESULTS,
    STATE_VIEW_HISTORY,
    STATE_SETTINGS,
    STATE_EXIT
} AppState;

typedef enum { TEST_ICMP = 0, TEST_TCP = 1, TEST_UDP = 2 } TestType;

/* ================================================================== */
/* Session history — volatile dynamic array, freed on exit             */
/* ================================================================== */
typedef struct {
    char   test_type[8];  /* "ICMP", "TCP", or "UDP"              */
    int    streams;       /* TCP parameter; 0 for ICMP/UDP runs  */
    int    duration;      /* TCP/UDP duration; 0 for ICMP runs   */
    double throughput;    /* Gbps for TCP; achieved Mbps for UDP */
    double latency;       /* ms; -1 if target unreachable        */
    double reliability_score; /* TCP only; 0 when unverified     */
    int    is_verified;       /* TCP only; 1 when Phase 3 confirmed */
    /* UDP-specific */
    uint32_t udp_sent;
    uint32_t udp_received;
    uint32_t udp_lost;
    double   udp_jitter_us;
    double   udp_target_bw;
    double   udp_achieved_bw;
    char   timestamp[20];
} TestResult;

static TestResult *session_history = NULL;
static int         test_count      = 0;
static int         hist_capacity   = 10;

static int history_init(void)
{
    session_history = malloc((size_t)hist_capacity * sizeof(TestResult));
    return session_history ? 0 : -1;
}

static void history_free(void)
{
    free(session_history);
    session_history = NULL;
    test_count = hist_capacity = 0;
}

static int history_append(const TestResult *r)
{
    if (test_count == hist_capacity) {
        int        new_cap = hist_capacity * 2;
        TestResult *tmp    = realloc(session_history,
                                     (size_t)new_cap * sizeof(TestResult));
        if (!tmp)
            return -1;
        session_history = tmp;
        hist_capacity   = new_cap;
    }
    session_history[test_count++] = *r;
    return 0;
}

static void get_timestamp(char *buf, size_t len)
{
    time_t    now = time(NULL);
    struct tm *t  = localtime(&now);
    strftime(buf, len, "%Y-%m-%d %H:%M:%S", t);
}

/* ================================================================== */
/* Terminal raw mode (termios on Linux, Win32 console API on Windows)  */
/* ================================================================== */
#ifdef _WIN32

/* On Windows, all state is managed inside terminal_win.c. */
static void setup_terminal_raw_mode(void) { win_set_raw_mode(); }
static void restore_terminal_mode(void)   { win_restore_mode(); }

static void sig_cleanup(int signo)
{
    (void)signo;
    win_restore_mode();
    const char msg[] = "\nInterrupted.\n";
    _write(STDOUT_FILENO, msg, (unsigned int)(sizeof(msg) - 1));
    _exit(1);
}



#else /* POSIX ---------------------------------------------------- */

static struct termios orig_termios;
static int            raw_mode_active = 0;

static void setup_terminal_raw_mode(void)
{
    /* Save the original termios only on the first call. */
    if (!raw_mode_active)
        tcgetattr(STDIN_FILENO, &orig_termios);

    struct termios raw = orig_termios;
    raw.c_lflag &= ~(unsigned)(ECHO | ICANON);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_mode_active = 1;
}

static void restore_terminal_mode(void)
{
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode_active = 0;
    }
}

/* Best-effort terminal restore on abnormal exit (signal handler). */
static void sig_cleanup(int signo)
{
    (void)signo;
    if (raw_mode_active)
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
    const char msg[] = "\nInterrupted.\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    _exit(1);
}

#endif /* _WIN32 */

/* ================================================================== */
/* Input capture                                                       */
/* ================================================================== */
/* On Windows, KEY_* are defined in terminal_win.h (included above). */
#ifndef KEY_UP
#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_ENTER 1002
#define KEY_ESC   1003
#define KEY_QUIT  1004
#endif

/*
 * capture_input — read one logical keypress.
 *
 * On Windows: delegates to win_read_key() which uses ReadConsoleInput()
 * with virtual key codes \u2014 no VT sequence parsing required.
 *
 * On Linux: arrow keys are transmitted as 3-byte ESC sequences and
 * detected via a 100 ms termios timeout peek:
 *   Up Arrow:   0x1B  0x5B  0x41
 *   Down Arrow: 0x1B  0x5B  0x42
 */
static int capture_input(void)
{
#ifdef _WIN32
    /*
     * Delegate entirely to win_read_key() which uses ReadConsoleInput()
     * with virtual key code mapping.  This avoids the WaitForSingleObject
     * race where mouse/resize/focus INPUT_RECORDs triggered WAIT_OBJECT_0
     * but produced no _read() bytes, causing every key to require two
     * presses to register.
     */
    return win_read_key();
#else
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return -1;

    if (c == 0x1B) {
        /* Temporarily lower timeout to 100 ms to peek for CSI sequence. */
        struct termios t;
        tcgetattr(STDIN_FILENO, &t);
        t.c_cc[VMIN]  = 0;
        t.c_cc[VTIME] = 1;   /* 1 = 100 ms */
        tcsetattr(STDIN_FILENO, TCSANOW, &t);

        unsigned char seq[2] = {0, 0};
        read(STDIN_FILENO, &seq[0], 1);
        read(STDIN_FILENO, &seq[1], 1);

        t.c_cc[VMIN]  = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);

        if (seq[0] == '[') {
            if (seq[1] == 'A') return KEY_UP;
            if (seq[1] == 'B') return KEY_DOWN;
        }
        return KEY_ESC;
    }

    if (c == '\r' || c == '\n') return KEY_ENTER;
    if (c == 'q'  || c == 'Q') return KEY_QUIT;
    return (int)c;
#endif
}

/*
 * read_int_field — temporarily restore canonical mode, prompt for an
 * integer within [min_val, max_val], and return the validated result
 * (or current if input is invalid).
 */
static int read_int_field(const char *label, int min_val, int max_val,
                           int current)
{
    restore_terminal_mode();
    printf("\n  %s (%d-%d) [current: %d]: ", label, min_val, max_val, current);
    fflush(stdout);

    int  val = current;
    char line[32];
    if (fgets(line, (int)sizeof(line), stdin)) {
        int parsed;
        if (sscanf(line, "%d", &parsed) == 1
                && parsed >= min_val && parsed <= max_val)
            val = parsed;
        else
            printf("  Out of range — keeping %d.\n", current);
    }

    setup_terminal_raw_mode();
    return val;
}

/*
 * read_str_field — temporarily restore canonical mode, prompt for a
 * string, and store it in buf.  Blank input clears the field (allows
 * the user to explicitly set an empty value, e.g. to reset Output File
 * to stdout).  Enter with a space to keep the current value.
 */
static void read_str_field(const char *label, char *buf, size_t len)
{
    restore_terminal_mode();
    printf("\n  %s [current: %s] (blank=clear): ",
           label, buf[0] ? buf : "<empty>");
    fflush(stdout);

    char line[256];
    if (fgets(line, (int)sizeof(line), stdin)) {
        /* Strip trailing newline and carriage return (handles CRLF on
         * Windows so neither \r nor \n leaks into paths or addresses). */
        size_t l = strlen(line);
        if (l > 0 && line[l - 1] == '\n') line[--l] = '\0';
        if (l > 0 && line[l - 1] == '\r') line[--l] = '\0';
        snprintf(buf, len, "%s", line);
    }

    setup_terminal_raw_mode();
}
/* ================================================================== */
/* Application context                                                 */
/* ================================================================== */
typedef struct {
    AppState state;
    int      sel;                /* cursor position in the active menu  */
    int      mode;               /* 0 = client, 1 = server              */
    /* client params */
    char     target_ip_buf[64];
    int      ping_count;
    int      duration;
    int      streams;
    int      dss_mode;
    int      dss_window_ms;
    int      json_output;
    char     output_path[256];
    int      version_checked;    /* 0 = handshake needed before next TCP run */
    /* UDP params */
    int      udp_test_mode;      /* 0 = TCP bandwidth, 1 = UDP jitter/loss  */
    double   udp_target_bw;      /* Mbps, default DEFAULT_UDP_BW            */
    int      udp_pkt_size;       /* bytes, default DEFAULT_PKT_SIZE         */
    /* server params */
    int      port;
    int      max_dur;
    /* display */
    char     server_str[64];
    /* test state */
    TestType test_type;          /* test queued for STATE_RUNNING_TEST  */
    /* last ICMP result */
    struct icmp_stats        last_icmp;
    int                      last_icmp_rc;
    /* last TCP result */
    struct run_client_result last_bw;
    int                      last_bw_streams;
    int                      last_bw_duration;
    int                      last_bw_failed;
} AppCtx;

/* ================================================================== */
#define MENU_ITEMS_CLIENT     6
#define MENU_ITEMS_SERVER     3
#define CLIENT_SETTINGS_ITEMS 14   /* 3 UDP items added after Output File */
#define SERVER_SETTINGS_ITEMS 4
#define UI_WRAP(x, n)  (((x) % (n) + (n)) % (n))

static void render_item(int idx, int sel, const char *label, const char *extra)
{
    if (idx == sel)
        printf(A_INVERT "  > %-34s" A_RESET, label);
    else
        printf("    %-34s", label);
    if (extra && *extra)
        printf(A_DIM " %s" A_RESET, extra);
    printf("\n");
}

/* ------------------------------------------------------------------ */
/* Main menu                                                           */
/* ------------------------------------------------------------------ */
static void render_main_menu(const AppCtx *ctx)
{
    char extra[48];
    int  sel = ctx->sel;

    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  spdchk %s — Interactive Mode\n" A_RESET, SPDCHK_VERSION);
    printf("  Mode:   " A_CYAN "%s" A_RESET "\n",
           ctx->mode == 0 ? "Client" : "Server");
    if (ctx->mode == 0)
        printf("  Server: " A_CYAN "%s" A_RESET "\n", ctx->server_str);
    printf(THIN_LINE);

    if (ctx->mode == 0) {
        snprintf(extra, sizeof(extra), "[pings: %d]", ctx->ping_count);
        render_item(0, sel, "Run Reachability (ICMP)", extra);

        if (ctx->udp_test_mode == 0) {
            snprintf(extra, sizeof(extra), "[streams: %d, %ds]",
                     ctx->streams, ctx->duration);
            render_item(1, sel, "Run Bandwidth (TCP)", extra);
        } else {
            snprintf(extra, sizeof(extra), "[%.0f Mbps, %ds, %dB]",
                     ctx->udp_target_bw, ctx->duration, ctx->udp_pkt_size);
            render_item(1, sel, "Run UDP (Jitter & Loss)", extra);
        }

        snprintf(extra, sizeof(extra), "[port: %d]", ctx->port);
        render_item(2, sel, "Start Server", extra);

        snprintf(extra, sizeof(extra), "[%d test(s)]", test_count);
        render_item(3, sel, "View Session History", extra);

        render_item(4, sel, "Settings", "");
        render_item(5, sel, "Exit", "");
    } else {
        snprintf(extra, sizeof(extra), "[port: %d]", ctx->port);
        render_item(0, sel, "Start Server", extra);

        render_item(1, sel, "Settings", "");
        render_item(2, sel, "Exit", "");
    }

    printf(THIN_LINE);
    printf(A_DIM "  \xe2\x86\x91/\xe2\x86\x93 navigate   ENTER select   Q quit\n"
           A_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */
static void render_settings(const AppCtx *ctx)
{
    char extra[64];
    int  sel = ctx->sel;

    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  Settings\n" A_RESET);
    printf(THIN_LINE);

    if (ctx->mode == 0) {
        /* Client settings */
        render_item(0, sel, "Mode",
                    "[Client \xe2\x80\x94 ENTER to switch to Server]");
        snprintf(extra, sizeof(extra), "(current: %s)",
                 ctx->target_ip_buf[0] ? ctx->target_ip_buf : "<not set>");
        render_item(1, sel, "Target IP", extra);
        snprintf(extra, sizeof(extra), "(current: %d)", ctx->port);
        render_item(2, sel, "Port", extra);
        snprintf(extra, sizeof(extra), "(current: %d)", ctx->ping_count);
        render_item(3, sel, "ICMP Ping Count", extra);
        snprintf(extra, sizeof(extra), "(current: %d s)", ctx->duration);
        render_item(4, sel, "TCP Duration", extra);
        snprintf(extra, sizeof(extra), "(current: %d)", ctx->streams);
        render_item(5, sel, "TCP Streams", extra);
        snprintf(extra, sizeof(extra), "(current: %s)",
                 ctx->dss_mode ? "On" : "Off");
        render_item(6, sel, "DSS Mode", extra);
        snprintf(extra, sizeof(extra), "(current: %d ms)", ctx->dss_window_ms);
        render_item(7, sel, "DSS Window", extra);
        snprintf(extra, sizeof(extra), "(current: %s)",
                 ctx->json_output ? "On" : "Off");
        render_item(8, sel, "JSON Output", extra);
        snprintf(extra, sizeof(extra), "(current: %s)",
                 ctx->output_path[0] ? ctx->output_path : "<stdout>");
        render_item(9, sel, "Output File", extra);
        snprintf(extra, sizeof(extra), "(current: %s)",
                 ctx->udp_test_mode ? "UDP (Jitter & Loss)" : "TCP (Bandwidth)");
        render_item(10, sel, "Test Type", extra);
        snprintf(extra, sizeof(extra), "(current: %.0f Mbps)", ctx->udp_target_bw);
        render_item(11, sel, "UDP Target BW", extra);
        snprintf(extra, sizeof(extra), "(current: %d B)", ctx->udp_pkt_size);
        render_item(12, sel, "UDP Pkt Size", extra);
        render_item(13, sel, "Back", "");
    } else {
        /* Server settings */
        render_item(0, sel, "Mode",
                    "[Server \xe2\x80\x94 ENTER to switch to Client]");
        snprintf(extra, sizeof(extra), "(current: %d)", ctx->port);
        render_item(1, sel, "Port", extra);
        if (ctx->max_dur > 0)
            snprintf(extra, sizeof(extra), "(current: %d s)", ctx->max_dur);
        else
            snprintf(extra, sizeof(extra), "(current: unlimited)");
        render_item(2, sel, "Max Duration", extra);
        render_item(3, sel, "Back", "");
    }

    printf(THIN_LINE);
    printf(A_DIM "  \xe2\x86\x91/\xe2\x86\x93 navigate   ENTER edit/select   ESC back\n"
           A_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Running indicator (shown synchronously before blocking test call)   */
/* ------------------------------------------------------------------ */
static void render_running(const char *type, const char *target)
{
    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  Running %s test\n" A_RESET, type);
    printf(THIN_LINE);
    printf("  Target:  " A_CYAN "%s" A_RESET "\n", target);
    printf("  Status:  Please wait...\n");
    printf(THIN_LINE);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Server running indicator                                            */
/* ------------------------------------------------------------------ */
static void render_running_server(int port, int max_dur)
{
    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  Server Mode\n" A_RESET);
    printf(THIN_LINE);
    printf("  Listening on port " A_CYAN "%d" A_RESET "\n", port);
    if (max_dur > 0)
        printf("  Max per-test duration: " A_BOLD "%d s\n" A_RESET, max_dur);
    else
        printf("  Max per-test duration: " A_DIM "unlimited\n" A_RESET);
    printf("  Status:  Running... (Ctrl+C to stop and exit)\n");
    printf(THIN_LINE);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* ICMP results                                                        */
/* ------------------------------------------------------------------ */
static void render_icmp_results(const struct icmp_stats *s, int rc)
{
    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  ICMP Reachability Results\n" A_RESET);
    printf(THIN_LINE);

    if (rc == 0) {
        printf("  Avg RTT:     " A_BOLD "%.2f ms\n"  A_RESET, s->avg_latency_ms);
        printf("  Packet loss: " A_BOLD "%.1f%%\n"   A_RESET, s->packet_loss_pct);
    } else if (rc == -2) {
        printf("  " A_YELLOW "Insufficient privileges — run with sudo or grant CAP_NET_RAW.\n" A_RESET);
    } else {
        printf("  " A_YELLOW "Target unreachable — all pings lost.\n" A_RESET);
    }

    printf(THIN_LINE);
    printf(A_DIM "  Press any key to return...\n" A_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Bandwidth results                                                   */
/* ------------------------------------------------------------------ */
static void render_bw_results(const struct run_client_result *r,
                               int streams, int duration, int failed)
{
    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  Bandwidth Results\n" A_RESET);
    printf(THIN_LINE);

    if (failed == -2) {
        printf("  " A_YELLOW "Insufficient privileges — run with sudo or grant CAP_NET_RAW.\n" A_RESET);
    } else if (failed == -3) {
        printf("  " A_YELLOW "Version mismatch — client and server must run the same version (%s).\n" A_RESET,
               SPDCHK_VERSION);
    } else if (failed) {
        printf("  " A_YELLOW "Bandwidth test failed.\n" A_RESET);
    } else {
        const char *tput_color = r->is_verified ? A_CYAN : A_YELLOW;
        const char *tput_tag   = r->is_verified ? "(Verified)" : "(Estimated)";
        printf("  Throughput:  %s" A_BOLD "%.3f Gbps" A_RESET " %s\n",
               tput_color, r->throughput_gbps, tput_tag);
        printf("  Streams:     " A_BOLD "%d\n"        A_RESET, streams);
        printf("  Duration:    " A_BOLD "%d s\n"      A_RESET, duration);
        if (r->is_verified) {
            const char *rating;
            if (r->reliability_score >= 99.9)      rating = "Optimal";
            else if (r->reliability_score >= 95.0) rating = "Stable";
            else if (r->reliability_score >= 90.0) rating = "Degraded";
            else                                   rating = "Unstable";
            printf("  Reliability: " A_BOLD "%.1f%% (%s)\n" A_RESET,
                   r->reliability_score, rating);
        }
    }

    printf(THIN_LINE);
    printf(A_DIM "  Press any key to return...\n" A_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* UDP results                                                         */
/* ------------------------------------------------------------------ */
static void render_udp_results(const struct run_client_result *r, int failed)
{
    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  UDP Jitter & Loss Results\n" A_RESET);
    printf(THIN_LINE);

    if (failed) {
        printf("  " A_YELLOW "UDP test failed.\n" A_RESET);
    } else {
        const struct udp_result *u = &r->udp;
        double loss_pct = (u->packets_sent > 0)
            ? (double)u->lost_packets / (double)u->packets_sent * 100.0
            : 0.0;

        printf("  Packets:     " A_BOLD "%u sent, %u received\n" A_RESET,
               u->packets_sent, u->packets_received);
        printf("  Pkt Loss:    " A_BOLD "%u (%.1f%%)\n" A_RESET,
               u->lost_packets, loss_pct);
        if (u->out_of_order > 0)
            printf("  Out-order:   " A_BOLD "%u\n" A_RESET, u->out_of_order);
        printf("  Jitter avg:  " A_BOLD "%.3f ms\n" A_RESET,
               u->jitter_us / 1000.0);
        printf("  Jitter peak: " A_BOLD "%.3f ms\n" A_RESET,
               u->peak_jitter_us / 1000.0);
        if (u->target_bw_mbps >= 1000.0)
            printf("  Capacity:    " A_BOLD "%.3f Gbps" A_RESET
                   " achieved / %.3f Gbps target\n",
                   u->achieved_bw_mbps / 1000.0,
                   u->target_bw_mbps   / 1000.0);
        else
            printf("  Capacity:    " A_BOLD "%.1f Mbps" A_RESET
                   " achieved / %.1f Mbps target\n",
                   u->achieved_bw_mbps, u->target_bw_mbps);
    }

    printf(THIN_LINE);
    printf(A_DIM "  Press any key to return...\n" A_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* History — columns highlighted in yellow when the value changed      */
/* relative to the previous row                                        */
/* ------------------------------------------------------------------ */

/* Print an integer column; apply yellow highlight when has_prev and value
 * differs from prev_val. */
static void hist_int(int cur, int prev_val, int has_prev, const char *fmt)
{
    int changed = has_prev && (prev_val != cur);
    if (changed)
        printf(A_YELLOW);
    printf(fmt, cur);
    if (changed)
        printf(A_RESET);
}

/* Same as hist_int but for double. */
static void hist_dbl(double cur, double prev_val, int has_prev,
                      const char *fmt)
{
    int changed = has_prev && (prev_val != cur);
    if (changed)
        printf(A_YELLOW);
    printf(fmt, cur);
    if (changed)
        printf(A_RESET);
}

static void render_history(void)
{
    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  Session History\n" A_RESET);
    printf(THIN_LINE);

    if (test_count == 0) {
        printf("  No tests recorded yet.\n");
    } else {
        /* ---- ICMP table ---- */
        printf(A_BOLD A_CYAN "  ICMP Results\n" A_RESET);
        printf(THIN_LINE);

        int icmp_printed = 0;
        const TestResult *prev_icmp = NULL;
        for (int i = 0; i < test_count; i++) {
            const TestResult *c = &session_history[i];
            if (strcmp(c->test_type, "ICMP") != 0)
                continue;
            if (icmp_printed == 0) {
                printf(A_BOLD "  %-3s  %-19s  %s\n" A_RESET,
                       "#", "Timestamp", "Latency");
                printf(THIN_LINE);
            }
            icmp_printed++;
            printf("  %-3d  %-19s  ", icmp_printed, c->timestamp);
            if (c->latency < 0.0) {
                printf("unreachable");
            } else {
                hist_dbl(c->latency,
                         prev_icmp ? prev_icmp->latency : 0.0,
                         prev_icmp != NULL,
                         "%.2f ms");
            }
            printf("\n");
            prev_icmp = c;
        }
        if (icmp_printed == 0)
            printf("  No ICMP tests recorded.\n");

        printf("\n");

        /* ---- TCP / Bandwidth table ---- */
        printf(A_BOLD A_CYAN "  TCP / Bandwidth Results\n" A_RESET);
        printf(THIN_LINE);

        int tcp_printed = 0;
        const TestResult *prev_tcp = NULL;
        for (int i = 0; i < test_count; i++) {
            const TestResult *c = &session_history[i];
            if (strcmp(c->test_type, "TCP") != 0)
                continue;
            if (tcp_printed == 0) {
                printf(A_BOLD "  %-3s  %-19s  %-7s  %-4s  %-14s  %s\n" A_RESET,
                       "#", "Timestamp", "Streams", "Dur",
                       "Throughput", "Reliability");
                printf(THIN_LINE);
            }
            tcp_printed++;
            printf("  %-3d  %-19s  ", tcp_printed, c->timestamp);
            hist_int(c->streams,
                     prev_tcp ? prev_tcp->streams : 0,
                     prev_tcp != NULL,
                     "%-7d");
            printf("  ");
            hist_int(c->duration,
                     prev_tcp ? prev_tcp->duration : 0,
                     prev_tcp != NULL,
                     "%-4d");
            printf("  ");
            /* Throughput — format to fixed-width buffer for column alignment */
            {
                char tput_str[32];
                if (c->throughput < 0.0)
                    snprintf(tput_str, sizeof(tput_str), "n/a");
                else
                    snprintf(tput_str, sizeof(tput_str), "%.3f Gbps", c->throughput);
                int changed = prev_tcp && (prev_tcp->throughput != c->throughput);
                if (changed) printf(A_YELLOW);
                printf("%-14s", tput_str);
                if (changed) printf(A_RESET);
            }
            printf("  ");
            /* Reliability — only meaningful when is_verified */
            if (c->is_verified) {
                const char *rating;
                if (c->reliability_score >= 99.9)      rating = "Optimal";
                else if (c->reliability_score >= 95.0) rating = "Stable";
                else if (c->reliability_score >= 90.0) rating = "Degraded";
                else                                   rating = "Unstable";
                int changed = prev_tcp && prev_tcp->is_verified &&
                              (prev_tcp->reliability_score != c->reliability_score);
                if (changed) printf(A_YELLOW);
                printf("%.1f%% (%s)", c->reliability_score, rating);
                if (changed) printf(A_RESET);
            } else {
                printf(A_DIM "\xe2\x80\x94" A_RESET); /* em dash */
            }
            printf("\n");
            prev_tcp = c;
        }
        if (tcp_printed == 0)
            printf("  No TCP tests recorded.\n");

        printf("\n");

        /* ---- UDP / Jitter table ---- */
        printf(A_BOLD A_CYAN "  UDP / Jitter & Loss Results\n" A_RESET);
        printf(THIN_LINE);

        int udp_printed = 0;
        for (int i = 0; i < test_count; i++) {
            const TestResult *c = &session_history[i];
            if (strcmp(c->test_type, "UDP") != 0)
                continue;
            if (udp_printed == 0) {
                printf(A_BOLD "  %-3s  %-19s  %-4s  %-8s  %-8s  %-10s\n"
                       A_RESET,
                       "#", "Timestamp", "Dur",
                       "Sent", "Lost%", "Jitter");
                printf(THIN_LINE);
            }
            udp_printed++;
            double loss_pct = (c->udp_sent > 0)
                ? (double)c->udp_lost / (double)c->udp_sent * 100.0 : 0.0;
            char jit_str[16];
            snprintf(jit_str, sizeof(jit_str), "%.3f ms",
                     c->udp_jitter_us / 1000.0);
            printf("  %-3d  %-19s  %-4d  %-8u  %-7.1f%%  %s\n",
                   udp_printed, c->timestamp, c->duration,
                   c->udp_sent, loss_pct, jit_str);
        }
        if (udp_printed == 0)
            printf("  No UDP tests recorded.\n");
    }

    printf(THIN_LINE);
    printf(A_DIM "  Press any key to return...\n" A_RESET);
    fflush(stdout);
}

/* ================================================================== */
/* execute_test — run the queued test and append to history            */
/* ================================================================== */
static void execute_test(AppCtx *ctx)
{
    if (ctx->test_type == TEST_ICMP) {
        render_running("ICMP Reachability", ctx->target_ip_buf);

        struct icmp_stats s = {0};
        int rc = icmp_ping(ctx->target_ip_buf, ctx->ping_count, &s);
        ctx->last_icmp    = s;
        ctx->last_icmp_rc = rc;

        TestResult r;
        strncpy(r.test_type, "ICMP", sizeof(r.test_type));
        r.test_type[sizeof(r.test_type) - 1] = '\0';
        r.streams            = 0;
        r.duration           = 0;
        r.throughput         = 0.0;
        r.latency            = (rc == 0) ? s.avg_latency_ms : -1.0;
        r.reliability_score  = 0.0;
        r.is_verified        = 0;
        get_timestamp(r.timestamp, sizeof(r.timestamp));
        history_append(&r);

    } else if (ctx->test_type == TEST_TCP) {
        render_running("TCP Bandwidth", ctx->target_ip_buf);

        struct client_args args = {
            .target_ip          = ctx->target_ip_buf,
            .port               = ctx->port,
            .ping_count         = ctx->ping_count,
            .duration           = ctx->duration,
            .streams            = ctx->streams,
            .json_output        = ctx->json_output,
            .output_path        = ctx->output_path[0] ? ctx->output_path : NULL,
            .dss_mode           = ctx->dss_mode,
            .dss_window_ms      = ctx->dss_window_ms,
            .skip_version_check = ctx->version_checked,
            .test_mode          = TEST_MODE_TCP,
        };

        struct run_client_result bw = {0};
        int rc = run_client_ex(&args, &bw);

        /* Reset flag on failure so the handshake is retried next run. */
        ctx->version_checked  = (rc == 0) ? 1 : 0;
        ctx->last_bw_streams  = ctx->streams;
        ctx->last_bw_duration = ctx->duration;
        ctx->last_bw_failed   = rc; /* 0 = ok, -1 = error, -2 = no privilege */
        ctx->last_bw          = bw;

        TestResult r;
        strncpy(r.test_type, "TCP", sizeof(r.test_type));
        r.test_type[sizeof(r.test_type) - 1] = '\0';
        r.streams            = ctx->streams;
        r.duration           = ctx->duration;
        r.throughput         = (rc == 0) ? bw.throughput_gbps : -1.0;
        r.latency            = -1.0;
        r.reliability_score  = (rc == 0) ? bw.reliability_score : 0.0;
        r.is_verified        = (rc == 0) ? bw.is_verified : 0;
        r.udp_sent           = 0;
        r.udp_received       = 0;
        r.udp_lost           = 0;
        r.udp_jitter_us      = 0.0;
        r.udp_target_bw      = 0.0;
        r.udp_achieved_bw    = 0.0;
        get_timestamp(r.timestamp, sizeof(r.timestamp));
        history_append(&r);

    } else if (ctx->test_type == TEST_UDP) {
        /* TEST_UDP */
        render_running("UDP (Jitter & Loss)", ctx->target_ip_buf);

        struct client_args args = {
            .target_ip          = ctx->target_ip_buf,
            .port               = ctx->port,
            .ping_count         = ctx->ping_count,
            .duration           = ctx->duration,
            .streams            = ctx->streams,
            .json_output        = ctx->json_output,
            .output_path        = ctx->output_path[0] ? ctx->output_path : NULL,
            .dss_mode           = 0,
            .dss_window_ms      = ctx->dss_window_ms,
            .skip_version_check = ctx->version_checked,
            .test_mode          = TEST_MODE_UDP,
            .udp_target_bw      = ctx->udp_target_bw,
            .udp_pkt_size       = ctx->udp_pkt_size,
        };

        struct run_client_result bw = {0};
        int rc = run_client_ex(&args, &bw);

        ctx->version_checked = (rc == 0) ? 1 : 0;
        ctx->last_bw_streams  = 0;
        ctx->last_bw_duration = ctx->duration;
        ctx->last_bw_failed   = rc;
        ctx->last_bw          = bw;

        TestResult r;
        strncpy(r.test_type, "UDP", sizeof(r.test_type));
        r.test_type[sizeof(r.test_type) - 1] = '\0';
        r.streams            = 0;
        r.duration           = ctx->duration;
        r.throughput         = 0.0;
        r.latency            = -1.0;
        r.reliability_score  = 0.0;
        r.is_verified        = 0;
        r.udp_sent           = (rc == 0) ? bw.udp.packets_sent     : 0;
        r.udp_received       = (rc == 0) ? bw.udp.packets_received : 0;
        r.udp_lost           = (rc == 0) ? bw.udp.lost_packets     : 0;
        r.udp_jitter_us      = (rc == 0) ? bw.udp.jitter_us        : 0.0;
        r.udp_target_bw      = ctx->udp_target_bw;
        r.udp_achieved_bw    = (rc == 0) ? bw.udp.achieved_bw_mbps : 0.0;
        get_timestamp(r.timestamp, sizeof(r.timestamp));
        history_append(&r);
    }
}

/* ================================================================== */
/* update_logic — state transitions and selection updates              */
/* ================================================================== */
static void update_logic(AppCtx *ctx, int key)
{
    switch (ctx->state) {

    /* ---- MAIN MENU ---- */
    case STATE_MAIN_MENU: {
        int menu_items = (ctx->mode == 0) ? MENU_ITEMS_CLIENT
                                          : MENU_ITEMS_SERVER;
        if (key == KEY_UP)
            ctx->sel = UI_WRAP(ctx->sel - 1, menu_items);
        else if (key == KEY_DOWN)
            ctx->sel = UI_WRAP(ctx->sel + 1, menu_items);
        else if (key == KEY_ENTER) {
            if (ctx->mode == 0) {
                switch (ctx->sel) {
                case 0:
                case 1:
                    if (!ctx->target_ip_buf[0]) {
                        read_str_field("Target IP", ctx->target_ip_buf,
                                       sizeof(ctx->target_ip_buf));
                        if (!ctx->target_ip_buf[0])
                            break; /* still empty — stay on main menu */
                        ctx->version_checked = 0;
                        snprintf(ctx->server_str, sizeof(ctx->server_str),
                                 "%s:%d", ctx->target_ip_buf, ctx->port);
                    }
                    if (ctx->sel == 0)
                        ctx->test_type = TEST_ICMP;
                    else
                        ctx->test_type = (ctx->udp_test_mode) ? TEST_UDP : TEST_TCP;
                    ctx->state = STATE_RUNNING_TEST;
                    break;
                case 2: ctx->state = STATE_RUNNING_SERVER;                             break;
                case 3: ctx->state = STATE_VIEW_HISTORY;                               break;
                case 4: ctx->state = STATE_SETTINGS; ctx->sel = 0;                    break;
                case 5: ctx->state = STATE_EXIT;                                       break;
                }
            } else {
                switch (ctx->sel) {
                case 0: ctx->state = STATE_RUNNING_SERVER;                             break;
                case 1: ctx->state = STATE_SETTINGS; ctx->sel = 0;                    break;
                case 2: ctx->state = STATE_EXIT;                                       break;
                }
            }
        } else if (key == KEY_QUIT || key == KEY_ESC) {
            ctx->state = STATE_EXIT;
        }
        break;
    }

    /* ---- SETTINGS ---- */
    case STATE_SETTINGS: {
        int settings_items = (ctx->mode == 0) ? CLIENT_SETTINGS_ITEMS
                                              : SERVER_SETTINGS_ITEMS;
        if (key == KEY_UP)
            ctx->sel = UI_WRAP(ctx->sel - 1, settings_items);
        else if (key == KEY_DOWN)
            ctx->sel = UI_WRAP(ctx->sel + 1, settings_items);
        else if (key == KEY_ENTER) {
            if (ctx->mode == 0) {
                switch (ctx->sel) {
                case 0:
                    ctx->mode = 1;
                    ctx->sel  = 0;
                    break;
                case 1:
                    read_str_field("Target IP", ctx->target_ip_buf,
                                   sizeof(ctx->target_ip_buf));
                    ctx->version_checked = 0;
                    snprintf(ctx->server_str, sizeof(ctx->server_str),
                             "%s:%d",
                             ctx->target_ip_buf[0] ? ctx->target_ip_buf
                                                   : "<not set>",
                             ctx->port);
                    break;
                case 2:
                    ctx->port = read_int_field("Port", 1, 65535, ctx->port);
                    snprintf(ctx->server_str, sizeof(ctx->server_str),
                             "%s:%d",
                             ctx->target_ip_buf[0] ? ctx->target_ip_buf
                                                   : "<not set>",
                             ctx->port);
                    break;
                case 3:
                    ctx->ping_count = read_int_field("ICMP ping count",
                                                     1, 100,
                                                     ctx->ping_count);
                    break;
                case 4:
                    ctx->duration = read_int_field("TCP duration (s)",
                                                   1, 3600,
                                                   ctx->duration);
                    break;
                case 5:
                    ctx->streams = read_int_field("TCP streams",
                                                  1, DSS_MAX_STREAMS,
                                                  ctx->streams);
                    break;
                case 6:
                    ctx->dss_mode = !ctx->dss_mode;
                    break;
                case 7:
                    ctx->dss_window_ms = read_int_field("DSS window (ms)",
                                                        1, 10000,
                                                        ctx->dss_window_ms);
                    break;
                case 8:
                    ctx->json_output = !ctx->json_output;
                    break;
                case 9:
                    read_str_field("Output file (blank = stdout)",
                                   ctx->output_path,
                                   sizeof(ctx->output_path));
                    break;
                case 10:
                    ctx->udp_test_mode = !ctx->udp_test_mode;
                    break;
                case 11:
                    {
                        int bw_int = (int)ctx->udp_target_bw;
                        bw_int = read_int_field("UDP Target BW (Mbps)",
                                                1, 100000, bw_int);
                        ctx->udp_target_bw = (double)bw_int;
                    }
                    break;
                case 12:
                    ctx->udp_pkt_size = read_int_field("UDP Pkt Size (bytes)",
                                                       16, 65507,
                                                       ctx->udp_pkt_size);
                    break;
                case 13:
                    ctx->state = STATE_MAIN_MENU;
                    ctx->sel   = 4;
                    break;
                }
            } else {
                switch (ctx->sel) {
                case 0:
                    ctx->mode = 0;
                    ctx->sel  = 0;
                    break;
                case 1:
                    ctx->port = read_int_field("Port", 1, 65535, ctx->port);
                    break;
                case 2:
                    ctx->max_dur = read_int_field("Max duration (s, 0=unlimited)",
                                                  0, 86400,
                                                  ctx->max_dur);
                    break;
                case 3:
                    ctx->state = STATE_MAIN_MENU;
                    ctx->sel   = 1;
                    break;
                }
            }
        } else if (key == KEY_ESC || key == KEY_QUIT) {
            ctx->state = STATE_MAIN_MENU;
            ctx->sel   = (ctx->mode == 0) ? 4 : 1;
        }
        break;
    }

    /* ---- VIEW_RESULTS / VIEW_HISTORY: any key returns to main ---- */
    default:
        ctx->state = STATE_MAIN_MENU;
        ctx->sel   = 0;
        break;
    }
}

/* ================================================================== */
/* render_current_screen                                               */
/* ================================================================== */
static void render_current_screen(const AppCtx *ctx)
{
    switch (ctx->state) {
    case STATE_MAIN_MENU:
        render_main_menu(ctx);
        break;
    case STATE_VIEW_RESULTS:
        if (ctx->test_type == TEST_ICMP)
            render_icmp_results(&ctx->last_icmp, ctx->last_icmp_rc);
        else if (ctx->test_type == TEST_UDP)
            render_udp_results(&ctx->last_bw, ctx->last_bw_failed);
        else
            render_bw_results(&ctx->last_bw,
                               ctx->last_bw_streams,
                               ctx->last_bw_duration,
                               ctx->last_bw_failed);
        break;
    case STATE_VIEW_HISTORY:
        render_history();
        break;
    case STATE_SETTINGS:
        render_settings(ctx);
        break;
    default:
        break;
    }
}

/* ================================================================== */
/* interactive_main                                                    */
/* ================================================================== */
int interactive_main(void)
{
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr,
                "spdchk: interactive mode requires an interactive terminal.\n");
        return -1;
    }

    if (history_init() != 0) {
        fprintf(stderr, "spdchk: interactive: out of memory\n");
        return -1;
    }

    signal(SIGINT,  sig_cleanup);
    signal(SIGTERM, sig_cleanup);

    AppCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.state            = STATE_MAIN_MENU;
    ctx.sel              = 0;
    ctx.mode             = 0;
    ctx.port             = DEFAULT_PORT;
    ctx.ping_count       = DEFAULT_COUNT;
    ctx.duration         = DEFAULT_DURATION;
    ctx.streams          = DEFAULT_STREAMS;
    ctx.dss_mode         = 1;
    ctx.dss_window_ms    = DSS_WINDOW_MS;
    ctx.json_output      = 0;
    ctx.version_checked  = 0;
    ctx.udp_test_mode    = 0;
    ctx.udp_target_bw    = DEFAULT_UDP_BW;
    ctx.udp_pkt_size     = DEFAULT_PKT_SIZE;
    ctx.max_dur          = 0;
    ctx.last_icmp_rc     = -1;
    ctx.last_bw_streams  = 0;
    ctx.last_bw_duration = 0;
    ctx.last_bw_failed   = -1;
    snprintf(ctx.server_str, sizeof(ctx.server_str),
             "<not set>:%d", DEFAULT_PORT);

    setup_terminal_raw_mode();

    while (ctx.state != STATE_EXIT) {
        /* Execute any queued test (renders its own "running" screen). */
        if (ctx.state == STATE_RUNNING_TEST) {
            execute_test(&ctx);
            ctx.state = STATE_VIEW_RESULTS;
        }

        /* Start server (blocking; terminal restored during the call). */
        if (ctx.state == STATE_RUNNING_SERVER) {
            restore_terminal_mode();
            render_running_server(ctx.port, ctx.max_dur);
#ifdef _WIN32
            printf("  " A_YELLOW "NOTE: Windows Firewall may block inbound connections.\n" A_RESET);
            printf("  If clients cannot reach this server, allow TCP on port "
                   A_BOLD "%d" A_RESET " (run as Administrator):\n", ctx.port);
            printf("    netsh advfirewall firewall add rule "
                   "name=\"spdchk\" "
                   "protocol=TCP dir=in action=allow "
                   "localport=%d\n", ctx.port);
            printf(THIN_LINE);
            fflush(stdout);
#endif
            run_server(ctx.port, ctx.max_dur);
            setup_terminal_raw_mode();
            ctx.state = STATE_MAIN_MENU;
            ctx.sel   = 0;
        }

        render_current_screen(&ctx);

        /* VIEW_RESULTS and VIEW_HISTORY: consume one keystroke then return. */
        if (ctx.state == STATE_VIEW_RESULTS
                || ctx.state == STATE_VIEW_HISTORY) {
            capture_input();
            ctx.state = STATE_MAIN_MENU;
            ctx.sel   = 0;
            continue;
        }

        int key = capture_input();
        update_logic(&ctx, key);
    }

    restore_terminal_mode();
    printf(A_CLEAR);
    printf("Exiting spdchk. Session history discarded.\n");
    history_free();
    return 0;
}
