#ifndef INTERACTIVE_H
#define INTERACTIVE_H

#include <stdint.h>

/*
 * SessionEntry — one completed test run's parameters and results.
 * Stored in a volatile in-memory array; freed when the session exits.
 */
typedef struct {
    uint32_t streams;
    uint32_t duration_sec;
    double   throughput_gbps;
    double   rtt_ms;
    char     timestamp[32];
    int      is_icmp_only;  /* 1 = ICMP-only run (no TCP bandwidth data) */
} SessionEntry;

/*
 * interactive_main — entry point for interactive client mode.
 *
 *   target_ip  : IPv4 address of the spdchk server (required).
 *   port       : TCP port the server listens on.
 *   ping_count : initial ICMP ping count (can be changed from within the UI).
 *
 * Returns  0 on normal exit.
 * Returns -1 on initialisation error.
 */
int interactive_main(const char *target_ip, int port, int ping_count);

#endif /* INTERACTIVE_H */
