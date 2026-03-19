#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "telemetry.h"

/* Number of lines in the refreshable progress block. */
#define NLINES 5

/* --------------------------------------------------------------------------
 * Module-level globals for SIGINT forwarding.
 * Only one telemetry session may be active at a time (single client).
 * -------------------------------------------------------------------------- */
static spdchk_telemetry_t *g_active_tel   = NULL;
static void               (*g_prev_sigint)(int) = NULL;

static void tel_sigint_handler(int signo)
{
    (void)signo;
    /*
     * Move the cursor past the progress block so the shell prompt is not
     * printed on top of partial telemetry output.  Six newlines covers the
     * worst case where the cursor sits at the top of the NLINES block.
     */
    const char nl[] = "\n\n\n\n\n\n";
    (void)write(STDOUT_FILENO, nl, sizeof(nl) - 1);

    if (g_active_tel)
        g_active_tel->stop = 1;

    /* Restore the previous handler (interactive mode's sig_cleanup or
     * SIG_DFL) and re-deliver so the caller can do its own cleanup. */
    signal(SIGINT, g_prev_sigint);
    raise(SIGINT);
}

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */

static void fmt_rate(char *buf, size_t len, double bps)
{
    if (bps >= 1.0e9)
        snprintf(buf, len, "%.2f Gbps", bps / 1.0e9);
    else if (bps >= 1.0e6)
        snprintf(buf, len, "%.1f Mbps", bps / 1.0e6);
    else
        snprintf(buf, len, "%.1f Kbps", bps / 1.0e3);
}

/* Print `count` dashes (clamped to sizeof buf - 1) and a newline. */
static void print_sep(int count)
{
    char buf[64];
    int  n = (count < (int)sizeof(buf) - 1) ? count : (int)sizeof(buf) - 1;
    memset(buf, '-', (size_t)n);
    buf[n] = '\0';
    puts(buf);
}

/* --------------------------------------------------------------------------
 * Display thread
 * -------------------------------------------------------------------------- */
static void *telemetry_thread(void *arg)
{
    spdchk_telemetry_t *t = arg;

    /* Silent no-op on non-interactive output (pipes, file redirection,
     * CI/CD environments).  SDD §4.3 "isatty() check". */
    if (!isatty(STDOUT_FILENO))
        return NULL;

    /* --- Terminal geometry (SDD §4.3) --- */
    int term_width = 80;
    {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hout != INVALID_HANDLE_VALUE
                && GetConsoleScreenBufferInfo(hout, &csbi)) {
            int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            if (w > 0) term_width = w;
        }
#else
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
            term_width = (int)ws.ws_col;
#endif
    }

    /* Progress bar width: fit between "Progress: [" (12) and "] XXX%" (6),
     * leaving one character of right-margin.  Floor at 10, ceil at default. */
    int bar_width = TELEMETRY_BAR_WIDTH;
    {
        int avail = term_width - 12 - 6 - 1;
        if (avail >= 10 && avail < bar_width)
            bar_width = avail;
    }

    /* Separator width capped at 55 characters (matches the SDD mockup). */
    int sep_w = (term_width - 1 < 55) ? (term_width - 1) : 55;
    if (sep_w < 10) sep_w = 10;

    double total = (double)t->total_duration;

    struct timespec t_start;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    int64_t prev_bytes   = 0;
    int     first_render = 1;

    /* ------------------------------------------------------------------ */
    /* Refresh loop — SDD §2.1 "Telemetry Thread"                         */
    /* ------------------------------------------------------------------ */
    while (!t->stop) {
        struct timespec req = { .tv_sec  = 0,
                                .tv_nsec = TELEMETRY_REFRESH_MS * 1000000L };
        nanosleep(&req, NULL);

        /* Recheck stop flag after waking (set by telemetry_stop or SIGINT). */
        if (t->stop)
            break;

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        /* --- Temporal metrics (SDD §2.3) --- */
        double elapsed = (double)(now.tv_sec  - t_start.tv_sec)
                       + (double)(now.tv_nsec - t_start.tv_nsec) / 1.0e9;

        double rem = total - elapsed;
        if (rem < 0.0) rem = 0.0;

        double progress = (total > 0.0) ? (elapsed / total * 100.0) : 0.0;
        if (progress > 100.0) progress = 100.0;

        /* --- Instantaneous throughput R = Δbytes / Δt (SDD §2.3) --- */
        int64_t cur_bytes = atomic_load_explicit(&t->total_bytes,
                                                  memory_order_relaxed);
        double delta_bps = (double)(cur_bytes - prev_bytes)
                         / ((double)TELEMETRY_REFRESH_MS / 1000.0)
                         * 8.0;
        prev_bytes = cur_bytes;

        /* --- Build ASCII progress bar --- */
        char bar[64];
        int  filled = (int)(progress * (double)bar_width / 100.0);
        int  bmax   = (bar_width < (int)sizeof(bar) - 1)
                    ?  bar_width : (int)sizeof(bar) - 1;
        for (int i = 0; i < bmax; i++)
            bar[i] = (i < filled) ? '#' : '.';
        bar[bmax] = '\0';

        char rate_str[32];
        fmt_rate(rate_str, sizeof(rate_str), delta_bps);

        int streams = t->parallel_streams;

        /* Overwrite previous block after the first render. */
        if (!first_render)
            printf("\033[%dA", NLINES);

        printf("Progress: [%-*s] %3.0f%%\n", bar_width, bar, progress);
        printf("Status:   Running...  (%d stream%s)\n",
               streams, streams != 1 ? "s" : "");
        print_sep(sep_w);
        printf("Elapsed:  %-8.1fs        ETA:      %.1fs\n", elapsed, rem);
        printf("Rate:     %-20s Latency: %.1fms\n",
               rate_str, t->avg_latency_ms);
        fflush(stdout);

        first_render = 0;
    }

    /* ------------------------------------------------------------------ */
    /* Final frame — 100 % / Complete (SDD §3.1 terminal mockup)          */
    /* ------------------------------------------------------------------ */
    if (!first_render) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec  - t_start.tv_sec)
                       + (double)(now.tv_nsec - t_start.tv_nsec) / 1.0e9;

        int64_t final_bytes = atomic_load_explicit(&t->total_bytes,
                                                    memory_order_relaxed);
        /* Aggregate throughput over the full test duration */
        double final_bps = (total > 0.0)
                         ? ((double)final_bytes / total * 8.0)
                         : 0.0;

        char rate_str[32];
        fmt_rate(rate_str, sizeof(rate_str), final_bps);

        char bar[64];
        int  bmax = (bar_width < (int)sizeof(bar) - 1)
                  ?  bar_width : (int)sizeof(bar) - 1;
        for (int i = 0; i < bmax; i++)
            bar[i] = '#';
        bar[bmax] = '\0';

        int streams = t->parallel_streams;

        printf("\033[%dA", NLINES);
        printf("Progress: [%-*s] 100%%\n", bar_width, bar);
        printf("Status:   Complete    (%d stream%s)\n",
               streams, streams != 1 ? "s" : "");
        print_sep(sep_w);
        printf("Elapsed:  %-8.1fs        ETA:      0.0s\n", elapsed);
        printf("Rate:     %-20s Latency: %.1fms\n",
               rate_str, t->avg_latency_ms);
        /* Extra blank line to visually separate from the final stats block. */
        putchar('\n');
        fflush(stdout);
    }

    return NULL;
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

int telemetry_start(spdchk_telemetry_t *t, pthread_t *tid)
{
    /* Formally initialise the atomic counter (C11 §7.17.2.2). */
    atomic_init(&t->total_bytes, (int64_t)0);
    t->stop = 0;

    /* Install SIGINT forwarder only for interactive sessions. */
    if (isatty(STDOUT_FILENO)) {
        g_active_tel  = t;
        g_prev_sigint = signal(SIGINT, tel_sigint_handler);
    }

    if (pthread_create(tid, NULL, telemetry_thread, t) != 0) {
        /* Roll back signal handler on failure. */
        if (g_active_tel == t) {
            signal(SIGINT, g_prev_sigint);
            g_active_tel  = NULL;
            g_prev_sigint = NULL;
        }
        return -1;
    }
    return 0;
}

void telemetry_stop(spdchk_telemetry_t *t, pthread_t *tid)
{
    t->stop = 1;
    pthread_join(*tid, NULL);

    if (g_active_tel == t) {
        signal(SIGINT, g_prev_sigint);
        g_active_tel  = NULL;
        g_prev_sigint = NULL;
    }
}
