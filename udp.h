#ifndef UDP_H
#define UDP_H

#include <stdint.h>

/*
 * udp_result — populated by run_udp_client() after a UDP jitter/loss test.
 *
 * All byte and packet counts are sender-side (what was sent) or
 * server-side (what was received), as labelled.  jitter_us and
 * peak_jitter_us are in microseconds.
 */
struct udp_result {
    uint32_t packets_sent;       /* datagrams sent by the client            */
    uint32_t packets_received;   /* datagrams received by the server        */
    uint32_t lost_packets;       /* packets_sent - packets_received         */
    uint32_t out_of_order;       /* packets arriving below the seq high-water*/
    double   jitter_us;          /* RFC 3550 smoothed jitter (microseconds) */
    double   peak_jitter_us;     /* maximum per-packet transit-time delta   */
    double   target_bw_mbps;     /* requested bit-rate in Mbps              */
    double   achieved_bw_mbps;   /* measured sender throughput in Mbps      */
};

/*
 * run_udp_client — perform a UDP jitter and packet-loss test.
 *
 * Phase A: Negotiates test parameters with the server via a short TCP
 *          control connection (SPDCHK_UDP_REQ greeting).
 * Phase B: Sends `duration_sec` seconds of UDP datagrams at the
 *          constant bit-rate `target_bw_mbps` Mbps using a high-precision
 *          absolute-time scheduling loop.
 * Phase C: Requests the server's reception statistics via a second TCP
 *          control connection (SPDCHK_UDP_DONE greeting).
 *
 * On success, populates `result` and returns 0.
 * Returns -1 on network or allocation error.
 */
int run_udp_client(const char *target_ip, int port,
                   double target_bw_mbps, int pkt_size,
                   int duration_sec, struct udp_result *result);

#endif /* UDP_H */
