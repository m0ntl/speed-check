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
 * Returns -1 if all pings failed or on a non-permission socket error.
 * Returns -2 if insufficient privileges prevented the ICMP operation
 *            (Linux: EACCES/EPERM on SOCK_RAW; Windows: not applicable
 *            — IcmpSendEcho does not require elevation).
 *
 * On Linux requires CAP_NET_RAW or root for SOCK_RAW.
 * On Windows uses IcmpSendEcho (iphlpapi) — no elevation required.
 */
int icmp_ping(const char *target_ip, int count, struct icmp_stats *stats);

#endif /* ICMP_H */
