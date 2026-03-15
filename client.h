#ifndef CLIENT_H
#define CLIENT_H

/*
 * run_client — execute the two-phase diagnostic sequence:
 *   1. ICMP reachability check (aborts if target is unreachable).
 *   2. TCP measurement: send `count` payloads, receive echoes,
 *      compute and display RTT / jitter / loss statistics.
 *
 * Returns  0 on success.
 * Returns -1 on network or allocation error.
 */
int run_client(const char *target_ip, int port, int count);

#endif /* CLIENT_H */
