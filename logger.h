#ifndef LOGGER_H
#define LOGGER_H

/*
 * Log verbosity levels (numeric values are intentional — they let the
 * -v counting logic map directly to a level).
 *
 *   0 = ERROR  — critical failures only
 *   1 = INFO   — standard operation messages (default)
 *   2 = DEBUG  — detailed state transitions / handshake signals
 *   3 = TRACE  — packet-level metadata and raw timing (rate-limited)
 */
typedef enum {
    LOG_LEVEL_ERROR = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_DEBUG = 2,
    LOG_LEVEL_TRACE = 3,
} log_level_t;

/*
 * logger_init  — open the syslog connection and set the active level.
 *               Call once before any log_* macro is used.
 * logger_close — flush and close the syslog connection.
 */
void logger_init(log_level_t level);
void logger_close(void);

/*
 * log_msg — thread-safe dual-output logger (syslog + stdout/stderr).
 *           Use the convenience macros below instead of calling directly.
 */
void log_msg(log_level_t level, const char *module, const char *fmt, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 3, 4)))
#endif
    ;

#define log_error(mod, ...) log_msg(LOG_LEVEL_ERROR, (mod), __VA_ARGS__)
#define log_info(mod, ...)  log_msg(LOG_LEVEL_INFO,  (mod), __VA_ARGS__)
#define log_debug(mod, ...) log_msg(LOG_LEVEL_DEBUG, (mod), __VA_ARGS__)
#define log_trace(mod, ...) log_msg(LOG_LEVEL_TRACE, (mod), __VA_ARGS__)

#endif /* LOGGER_H */
