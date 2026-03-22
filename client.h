#ifndef CLIENT_H
#define CLIENT_H

#include "udp.h"

/*
 * client_args — configuration bundle for the test client.
 */
struct client_args {
    const char *target_ip;
    int         port;
    int         ping_count;         /* ICMP pings to send          (default 4)  */
    int         duration;           /* bandwidth test duration, s  (default 10) */
    int         streams;            /* parallel TCP streams        (default 4)  */
    int         json_output;        /* 1 = emit JSON, 0 = plain text            */
    const char *output_path;        /* write statistics here; NULL = stdout     */
    int         dss_mode;           /* 1 = Dynamic Stream Scaling; 0 = static   */
    int         dss_window_ms;      /* DSS sampling window in ms  (default 500) */
    int         skip_version_check; /* 1 = skip Phase 0 handshake (already done) */
    /* UDP test parameters (used when test_mode == TEST_MODE_UDP)               */
    int         test_mode;          /* TEST_MODE_TCP (0) or TEST_MODE_UDP (1)   */
    double      udp_target_bw;      /* target bit-rate in Mbps (default 100)    */
    int         udp_pkt_size;       /* datagram size in bytes (default 1472)    */
};

/*
 * run_client_result — structured result data populated by run_client_ex().
 * Allows callers such as interactive mode to process results programmatically.
 */
struct run_client_result {
    double   throughput_gbps;   /* total bandwidth measured in Gbps                        */
    double   reliability_score; /* (bytes_received / bytes_sent) * 100; 0 when unverified  */
    int      is_verified;       /* 1 = server confirmed via Phase 3; 0 = local estimate    */
    /* UDP test results — populated when test_mode == TEST_MODE_UDP; zero otherwise */
    struct udp_result udp;
};

/*
 * client_check_server_version — open a short TCP connection to the server
 * and exchange version strings.  When dss_mode is non-zero the greeting
 * includes the DSS capability flag.
 *
 * Returns  0 when the server version matches.
 * Returns -1 on connection failure, timeout, or unexpected response.
 * Returns -2 when the server explicitly reports a version mismatch.
 *
 * Interactive mode calls this once at start-up so that individual test
 * runs can set skip_version_check = 1 and avoid repeating the handshake.
 */
int client_check_server_version(const char *target_ip, int port, int dss_mode);

/*
 * run_client — execute the two-phase diagnostic sequence:
 *   1. ICMP multi-ping (aborts if target is unreachable).
 *   2. Parallel TCP bandwidth measurement for `duration` seconds.
 *
 * Returns  0 on success.
 * Returns -1 on network or allocation error.
 */
int run_client(const struct client_args *args);

/*
 * run_client_ex — same as run_client(); if result is non-NULL it is
 * populated with the measured throughput and ICMP statistics so that
 * callers can process the data programmatically (e.g. interactive mode).
 *
 * Returns  0 on success.
 * Returns -1 on network or allocation error.
 * Returns -2 when the ICMP phase fails due to insufficient privileges
 *            (EPERM / EACCES); run with sudo or set CAP_NET_RAW.
 * Returns -3 when the version handshake reveals a server/client version
 *            mismatch; both sides must be upgraded to the same version.
 */
int run_client_ex(const struct client_args *args,
                  struct run_client_result  *result);

#endif /* CLIENT_H */
