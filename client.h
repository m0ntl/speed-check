#ifndef CLIENT_H
#define CLIENT_H

/*
 * client_args — configuration bundle for the test client.
 */
struct client_args {
    const char *target_ip;
    int         port;
    int         ping_count;   /* ICMP pings to send          (default 4)  */
    int         duration;     /* bandwidth test duration, s  (default 10) */
    int         streams;      /* parallel TCP streams        (default 4)  */
    int         json_output;  /* 1 = emit JSON, 0 = plain text            */
    const char *output_path;  /* write statistics here; NULL = stdout     */
    int         dss_mode;     /* 1 = Dynamic Stream Scaling; 0 = static   */
    int         dss_window_ms;/* DSS sampling window in ms  (default 500) */
};

/*
 * run_client_result — structured result data populated by run_client_ex().
 * Allows callers such as interactive mode to process results programmatically.
 */
struct run_client_result {
    double throughput_gbps;   /* total bandwidth measured in Gbps  */
    double avg_latency_ms;    /* average ICMP RTT in milliseconds  */
    double packet_loss_pct;   /* packet-loss percentage from ICMP  */
};

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
 */
int run_client_ex(const struct client_args *args,
                  struct run_client_result  *result);

#endif /* CLIENT_H */
