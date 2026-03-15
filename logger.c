#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <syslog.h>

#include "logger.h"

/*
 * TRACE rate-limit: suppress messages beyond this many calls per second
 * to prevent disk exhaustion during high-speed tests (SDD §5).
 */
#define TRACE_RATE_LIMIT 1000

static log_level_t     g_level            = LOG_LEVEL_INFO;

/* Rate-limiter state — protected by g_trace_mtx. */
static pthread_mutex_t g_trace_mtx        = PTHREAD_MUTEX_INITIALIZER;
static time_t          g_trace_window     = 0;
static int             g_trace_count      = 0;
static int             g_trace_suppressed = 0;

static int to_syslog_priority(log_level_t level)
{
    switch (level) {
    case LOG_LEVEL_ERROR: return LOG_ERR;
    case LOG_LEVEL_INFO:  return LOG_INFO;
    case LOG_LEVEL_DEBUG: return LOG_DEBUG;
    case LOG_LEVEL_TRACE: return LOG_DEBUG;
    default:              return LOG_INFO;
    }
}

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

void logger_init(log_level_t level)
{
    g_level = level;
    /* LOG_PID embeds the PID; LOG_NDELAY opens the socket immediately so
     * the first message is never dropped.  LOG_DAEMON is the facility for
     * system daemons / background services. */
    openlog("spdchk", LOG_PID | LOG_NDELAY, LOG_DAEMON);
}

void logger_close(void)
{
    closelog();
}

void log_msg(log_level_t level, const char *module, const char *fmt, ...)
{
    if (level > g_level)
        return;

    /* TRACE rate-limiter (SDD §5) */
    if (level == LOG_LEVEL_TRACE) {
        pthread_mutex_lock(&g_trace_mtx);
        time_t now = time(NULL);
        if (now != g_trace_window) {
            if (g_trace_suppressed > 0) {
                syslog(LOG_DEBUG, "[TRACE] [LOGGER] %d message(s) suppressed in previous second",
                       g_trace_suppressed);
            }
            g_trace_window     = now;
            g_trace_count      = 0;
            g_trace_suppressed = 0;
        }
        if (g_trace_count >= TRACE_RATE_LIMIT) {
            g_trace_suppressed++;
            pthread_mutex_unlock(&g_trace_mtx);
            return;
        }
        g_trace_count++;
        pthread_mutex_unlock(&g_trace_mtx);
    }

    char    msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    /*
     * Dual-output (SDD §3.3):
     *   syslog  — persistent, tagged [LEVEL] [MODULE] per §4.3
     *   stdout  — real-time feedback for the operator
     *   stderr  — errors, to match Unix convention
     */
    syslog(to_syslog_priority(level), "[%s] [%s] %s", level_str(level), module, msg);

    if (level == LOG_LEVEL_ERROR)
        fprintf(stderr, "[%s] %s\n", module, msg);
    else
        printf("[%s] %s\n", module, msg);
}
