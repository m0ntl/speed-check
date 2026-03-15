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

#endif /* CLIENT_H */
