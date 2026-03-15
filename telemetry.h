#ifndef TELEMETRY_H
#define TELEMETRY_H

/*
 * telemetry.h — Live progress display for the Bandwidth Phase.
 *
 * A dedicated pthread wakes every TELEMETRY_REFRESH_MS milliseconds,
 * reads the shared spdchk_telemetry_t state, and redraws a fixed-height
 * ASCII block in-place using ANSI cursor-up escape sequences.
 *
 * The module is a silent no-op when stdout is not a TTY (pipes, CI/CD,
 * JSON-output mode with redirection, etc.).
 */

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#define TELEMETRY_REFRESH_MS 200  /* UI refresh interval — 5 Hz cap (SDD §5) */
#define TELEMETRY_BAR_WIDTH   30  /* default progress-bar character width     */

/*
 * spdchk_telemetry_t — shared state between the client worker threads
 * and the telemetry display thread.
 *
 * Caller must zero-initialise this struct, then populate
 * total_duration / parallel_streams / avg_latency_ms before calling
 * telemetry_start().  Do NOT read or write the remaining fields directly.
 */
typedef struct {
    int              total_duration;   /* configured test duration (seconds)    */
    volatile int     parallel_streams; /* active TCP stream count (display only) */
    double           avg_latency_ms;   /* ICMP average RTT shown in Latency row */

    /* --- internal, managed by telemetry.c --- */
    volatile int     stop;             /* set to 1 by telemetry_stop()          */
    _Atomic int64_t  total_bytes;      /* bytes aggregated from all streams     */
} spdchk_telemetry_t;

/*
 * telemetry_start — initialise *t and spawn the display thread.
 *
 * Must be called after populating total_duration / parallel_streams /
 * avg_latency_ms.  Also installs a SIGINT forwarder that prints a clean
 * newline before delegating to the previously-registered handler.
 *
 * Returns 0 on success, -1 if the thread could not be created.
 * On non-TTY stdout the function still returns 0 but the thread exits
 * immediately (safe to call telemetry_stop() unconditionally).
 */
int  telemetry_start(spdchk_telemetry_t *t, pthread_t *tid);

/*
 * telemetry_stop — signal the display thread to render its final frame,
 * join it, and restore the previous SIGINT handler.
 *
 * Always call this in every code path after a successful telemetry_start(),
 * including error paths, to avoid a dangling thread.
 */
void telemetry_stop(spdchk_telemetry_t *t, pthread_t *tid);

#endif /* TELEMETRY_H */
