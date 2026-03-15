#ifndef ICMP_H
#define ICMP_H

/*
 * icmp_stats — aggregated result from an ICMP ping sequence.
 */
struct icmp_stats {
    double avg_latency_ms;   /* average RTT of received replies   */
    double packet_loss_pct;  /* (1 - received/sent) * 100         */
};

/*
 * icmp_ping — send `count` ICMP echo requests to target_ip, collect
 * per-packet RTT, and populate *stats.
 *
 * Returns  0 if at least one reply was received.
 * Returns -1 if all pings failed or on socket error.
 *
 * Requires CAP_NET_RAW / root privileges (SOCK_RAW).
 */
int icmp_ping(const char *target_ip, int count, struct icmp_stats *stats);

#endif /* ICMP_H */
