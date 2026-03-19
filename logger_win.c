/*
 * logger_win.c — Windows logger implementation (SDD §2, "logger_win.c").
 *
 * Replaces logger.c on Windows builds.  The public interface is identical
 * (logger_init / logger_close / log_msg as declared in logger.h) but the
 * syslog back-end is replaced by:
 *
 *   1. A local append-mode log file "spdchk.log" in the working directory.
 *   2. The same stdout / stderr console output as logger.c.
 *
 * Thread safety is provided by a CRITICAL_SECTION (lighter than a mutex
 * for this intra-process use-case).  The TRACE rate-limiter matches the
 * 1,000 calls/s cap implemented in logger.c.
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "logger.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define TRACE_RATE_LIMIT  1000
#define LOG_FILE_NAME     "spdchk.log"

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static log_level_t       g_level             = LOG_LEVEL_INFO;
static FILE             *g_log_file          = NULL;
static CRITICAL_SECTION  g_cs;
static int               g_cs_init           = 0;

/* TRACE rate-limiter — protected by g_cs */
static time_t  g_trace_window     = 0;
static int     g_trace_count      = 0;
static int     g_trace_suppressed = 0;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static const char *level_str(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return "ERROR";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_TRACE: return "TRACE";
    default:              return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void logger_init(log_level_t level)
{
    g_level = level;
    InitializeCriticalSection(&g_cs);
    g_cs_init = 1;

    /* Open (or create) the log file in append mode.  Non-fatal on failure
     * — console output continues to function regardless. */
    g_log_file = fopen(LOG_FILE_NAME, "a");
}

void logger_close(void)
{
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    if (g_cs_init) {
        DeleteCriticalSection(&g_cs);
        g_cs_init = 0;
    }
}

void log_msg(log_level_t level, const char *module, const char *fmt, ...)
{
    if (level > g_level)
        return;

    if (g_cs_init)
        EnterCriticalSection(&g_cs);

    /* TRACE rate-limiter: suppress beyond TRACE_RATE_LIMIT per second. */
    if (level == LOG_LEVEL_TRACE) {
        time_t now = time(NULL);
        if (now != g_trace_window) {
            if (g_trace_suppressed > 0 && g_log_file)
                fprintf(g_log_file,
                        "[TRACE] [LOGGER] %d message(s) suppressed "
                        "in previous second\n",
                        g_trace_suppressed);
            g_trace_window     = now;
            g_trace_count      = 0;
            g_trace_suppressed = 0;
        }
        if (g_trace_count >= TRACE_RATE_LIMIT) {
            g_trace_suppressed++;
            if (g_cs_init) LeaveCriticalSection(&g_cs);
            return;
        }
        g_trace_count++;
    }

    /* Format the message. */
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /* Write to log file with a timestamp prefix. */
    if (g_log_file) {
        time_t     now_t = time(NULL);
        struct tm *tm_p  = localtime(&now_t);
        char       ts[20];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_p);
        fprintf(g_log_file, "%s [%s] [%s] %s\n",
                ts, level_str(level), module, msg);
        fflush(g_log_file);
    }

    /* Console output — mirrors logger.c dual-output behaviour. */
    if (level == LOG_LEVEL_ERROR)
        fprintf(stderr, "[%s] %s\n", module, msg);
    else
        printf("[%s] %s\n", module, msg);

    if (g_cs_init)
        LeaveCriticalSection(&g_cs);
}
