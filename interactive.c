#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>

#include "interactive.h"
#include "client.h"
#include "icmp.h"
#include "spdchk.h"

/* ================================================================== */
/* ANSI escape codes                                                    */
/* ================================================================== */
#define A_CLEAR   "\033[2J\033[H"
#define A_BOLD    "\033[1m"
#define A_DIM     "\033[2m"
#define A_INVERT  "\033[7m"
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
    STATE_VIEW_RESULTS,
    STATE_VIEW_HISTORY,
    STATE_SETTINGS,
    STATE_EXIT
} AppState;

typedef enum { TEST_ICMP = 0, TEST_TCP = 1 } TestType;

/* ================================================================== */
/* Session history — volatile dynamic array, freed on exit             */
/* ================================================================== */
typedef struct {
    char   test_type[8];  /* "ICMP" or "TCP"                     */
    int    streams;       /* TCP parameter; 0 for ICMP-only runs */
    int    duration;      /* TCP parameter; 0 for ICMP-only runs */
    double throughput;    /* Gbps; 0 for ICMP-only runs          */
    double latency;       /* ms; -1 if target unreachable        */
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
/* Terminal raw mode (termios)                                         */
/* ================================================================== */
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

/* ================================================================== */
/* Input capture                                                       */
/* ================================================================== */
#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_ENTER 1002
#define KEY_ESC   1003
#define KEY_QUIT  1004

/*
 * capture_input — read one logical keypress.
 * Arrow keys are transmitted as a 3-byte ESC sequence; we peek for the
 * remaining two bytes with a 100 ms timeout so a bare ESC still works.
 *
 *   Up Arrow:   0x1B  0x5B  0x41
 *   Down Arrow: 0x1B  0x5B  0x42
 */
static int capture_input(void)
{
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

/* ================================================================== */
/* TUI rendering helpers                                               */
/* ================================================================== */
#define MENU_ITEMS     5
#define SETTINGS_ITEMS 4
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
static void render_main_menu(int sel, int streams, int duration,
                              int ping_count, const char *server_str)
{
    char extra[48];

    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  spdchk %s — Interactive Mode\n" A_RESET, SPDCHK_VERSION);
    printf("  Server: " A_CYAN "%s" A_RESET "\n", server_str);
    printf(THIN_LINE);

    snprintf(extra, sizeof(extra), "[pings: %d]", ping_count);
    render_item(0, sel, "Run Reachability (ICMP)", extra);

    snprintf(extra, sizeof(extra), "[streams: %d, %ds]", streams, duration);
    render_item(1, sel, "Run Bandwidth (TCP)", extra);

    snprintf(extra, sizeof(extra), "[%d test(s)]", test_count);
    render_item(2, sel, "View Session History", extra);

    render_item(3, sel, "Change Parameters", "");
    render_item(4, sel, "Exit", "");

    printf(THIN_LINE);
    printf(A_DIM "  \xe2\x86\x91/\xe2\x86\x93 navigate   ENTER select   Q quit\n"
           A_RESET);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */
static void render_settings(int sel, int streams, int duration, int ping_count)
{
    char extra[24];

    printf(A_CLEAR);
    printf(A_BOLD SEP_LINE A_RESET);
    printf(A_BOLD "  Change Parameters\n" A_RESET);
    printf(THIN_LINE);

    snprintf(extra, sizeof(extra), "(current: %d)", streams);
    render_item(0, sel, "TCP Streams", extra);

    snprintf(extra, sizeof(extra), "(current: %d s)", duration);
    render_item(1, sel, "TCP Duration", extra);

    snprintf(extra, sizeof(extra), "(current: %d)", ping_count);
    render_item(2, sel, "ICMP Ping Count", extra);

    render_item(3, sel, "Back", "");

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

    if (failed) {
        printf("  " A_YELLOW "Bandwidth test failed.\n" A_RESET);
    } else {
        printf("  Throughput:  " A_BOLD "%.3f Gbps\n" A_RESET, r->throughput_gbps);
        printf("  Avg RTT:     " A_BOLD "%.2f ms\n"   A_RESET, r->avg_latency_ms);
        printf("  Packet loss: " A_BOLD "%.1f%%\n"    A_RESET, r->packet_loss_pct);
        printf("  Streams:     " A_BOLD "%d\n"        A_RESET, streams);
        printf("  Duration:    " A_BOLD "%d s\n"      A_RESET, duration);
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
                printf(A_BOLD "  %-3s  %-19s  %-7s  %-4s  %-11s  %s\n" A_RESET,
                       "#", "Timestamp", "Streams", "Dur",
                       "Throughput", "Latency");
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
            hist_dbl(c->throughput,
                     prev_tcp ? prev_tcp->throughput : 0.0,
                     prev_tcp != NULL,
                     "%-8.3f Gbps");
            printf("  ");
            hist_dbl(c->latency,
                     prev_tcp ? prev_tcp->latency : 0.0,
                     prev_tcp != NULL,
                     "%.2f ms");
            printf("\n");
            prev_tcp = c;
        }
        if (tcp_printed == 0)
            printf("  No TCP tests recorded.\n");
    }

    printf(THIN_LINE);
    printf(A_DIM "  Press any key to return...\n" A_RESET);
    fflush(stdout);
}

/* ================================================================== */
/* Application context                                                 */
/* ================================================================== */
typedef struct {
    AppState state;
    int      sel;            /* cursor position in the active menu  */
    int      streams;
    int      duration;
    int      ping_count;
    TestType test_type;      /* test queued for STATE_RUNNING_TEST  */
    /* last ICMP result */
    struct icmp_stats        last_icmp;
    int                      last_icmp_rc;
    /* last TCP result */
    struct run_client_result last_bw;
    int                      last_bw_streams;
    int                      last_bw_duration;
    int                      last_bw_failed;
    /* server info */
    const char *target_ip;
    int         port;
    char        server_str[64];
} AppCtx;

/* ================================================================== */
/* execute_test — run the queued test and append to history            */
/* ================================================================== */
static void execute_test(AppCtx *ctx)
{
    if (ctx->test_type == TEST_ICMP) {
        render_running("ICMP Reachability", ctx->target_ip);

        struct icmp_stats s = {0};
        int rc = icmp_ping(ctx->target_ip, ctx->ping_count, &s);
        ctx->last_icmp    = s;
        ctx->last_icmp_rc = rc;

        TestResult r;
        strncpy(r.test_type, "ICMP", sizeof(r.test_type));
        r.test_type[sizeof(r.test_type) - 1] = '\0';
        r.streams    = 0;
        r.duration   = 0;
        r.throughput = 0.0;
        r.latency    = (rc == 0) ? s.avg_latency_ms : -1.0;
        get_timestamp(r.timestamp, sizeof(r.timestamp));
        history_append(&r);

    } else {
        render_running("TCP Bandwidth", ctx->target_ip);

        struct client_args args = {
            .target_ip          = ctx->target_ip,
            .port               = ctx->port,
            .ping_count         = ctx->ping_count,
            .duration           = ctx->duration,
            .streams            = ctx->streams,
            .json_output        = 0,
            .output_path        = NULL,
            .dss_mode           = 1,
            .dss_window_ms      = DSS_WINDOW_MS,
            .skip_version_check = 1,
        };

        struct run_client_result bw = {0};
        int rc = run_client_ex(&args, &bw);

        ctx->last_bw_streams  = ctx->streams;
        ctx->last_bw_duration = ctx->duration;
        ctx->last_bw_failed   = (rc != 0);
        ctx->last_bw          = bw;

        TestResult r;
        strncpy(r.test_type, "TCP", sizeof(r.test_type));
        r.test_type[sizeof(r.test_type) - 1] = '\0';
        r.streams    = ctx->streams;
        r.duration   = ctx->duration;
        r.throughput = (rc == 0) ? bw.throughput_gbps : -1.0;
        r.latency    = (rc == 0) ? bw.avg_latency_ms  : -1.0;
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
    case STATE_MAIN_MENU:
        if (key == KEY_UP)
            ctx->sel = UI_WRAP(ctx->sel - 1, MENU_ITEMS);
        else if (key == KEY_DOWN)
            ctx->sel = UI_WRAP(ctx->sel + 1, MENU_ITEMS);
        else if (key == KEY_ENTER) {
            switch (ctx->sel) {
            case 0: ctx->test_type = TEST_ICMP; ctx->state = STATE_RUNNING_TEST; break;
            case 1: ctx->test_type = TEST_TCP;  ctx->state = STATE_RUNNING_TEST; break;
            case 2: ctx->state = STATE_VIEW_HISTORY; break;
            case 3: ctx->state = STATE_SETTINGS; ctx->sel = 0; break;
            case 4: ctx->state = STATE_EXIT; break;
            }
        } else if (key == KEY_QUIT || key == KEY_ESC) {
            ctx->state = STATE_EXIT;
        }
        break;

    /* ---- SETTINGS ---- */
    case STATE_SETTINGS:
        if (key == KEY_UP)
            ctx->sel = UI_WRAP(ctx->sel - 1, SETTINGS_ITEMS);
        else if (key == KEY_DOWN)
            ctx->sel = UI_WRAP(ctx->sel + 1, SETTINGS_ITEMS);
        else if (key == KEY_ENTER) {
            switch (ctx->sel) {
            case 0:
                ctx->streams = read_int_field("TCP streams",
                                              1, DSS_MAX_STREAMS,
                                              ctx->streams);
                break;
            case 1:
                ctx->duration = read_int_field("TCP duration (s)",
                                               1, 3600,
                                               ctx->duration);
                break;
            case 2:
                ctx->ping_count = read_int_field("ICMP ping count",
                                                 1, 100,
                                                 ctx->ping_count);
                break;
            case 3:
                /* Back — return cursor to "Change Parameters" in main menu. */
                ctx->state = STATE_MAIN_MENU;
                ctx->sel   = 3;
                break;
            }
        } else if (key == KEY_ESC || key == KEY_QUIT) {
            ctx->state = STATE_MAIN_MENU;
            ctx->sel   = 3;
        }
        break;

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
        render_main_menu(ctx->sel, ctx->streams, ctx->duration,
                         ctx->ping_count, ctx->server_str);
        break;
    case STATE_VIEW_RESULTS:
        if (ctx->test_type == TEST_ICMP)
            render_icmp_results(&ctx->last_icmp, ctx->last_icmp_rc);
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
        render_settings(ctx->sel, ctx->streams, ctx->duration,
                        ctx->ping_count);
        break;
    default:
        break;
    }
}

/* ================================================================== */
/* interactive_main                                                    */
/* ================================================================== */
int interactive_main(const char *target_ip, int port, int ping_count)
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

    AppCtx ctx = {
        .state            = STATE_MAIN_MENU,
        .sel              = 0,
        .streams          = DEFAULT_STREAMS,
        .duration         = DEFAULT_DURATION,
        .ping_count       = ping_count,
        .target_ip        = target_ip,
        .port             = port,
        .last_icmp_rc     = -1,
        .last_bw_streams  = 0,
        .last_bw_duration = 0,
        .last_bw_failed   = 1,
    };
    snprintf(ctx.server_str, sizeof(ctx.server_str), "%s:%d", target_ip, port);

    /* Version check — runs once here so individual tests skip the handshake. */
    if (client_check_server_version(target_ip, port, 0) != 0) {
        fprintf(stderr,
                "spdchk: version check failed — cannot start interactive mode.\n");
        history_free();
        return -1;
    }

    setup_terminal_raw_mode();

    while (ctx.state != STATE_EXIT) {
        /* Execute any queued test (renders its own "running" screen). */
        if (ctx.state == STATE_RUNNING_TEST) {
            execute_test(&ctx);
            ctx.state = STATE_VIEW_RESULTS;
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
    printf("Exiting spdchk interactive mode. Session history discarded.\n");
    history_free();
    return 0;
}
