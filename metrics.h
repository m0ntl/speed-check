#ifndef METRICS_H
#define METRICS_H

#include <stdio.h>
#include <stdint.h>
#include "udp.h"

/* ICMP phase summary */
struct ping_result {
    double avg_latency_ms;
    double packet_loss_pct;
};

/* TCP throughput phase summary */
struct bandwidth_result {
    double   throughput_gbps;
    int      duration_sec;
    int      parallel_streams; /* streams used for throughput (DSS optimal, or static count) */
    int      optimal_streams;  /* total streams DSS probed; 0 in static mode or when no extra probe ran */
    uint64_t bytes_sent;       /* bytes client pushed to the kernel                           */
    uint64_t bytes_received;   /* bytes server confirmed received; 0 when unverified           */
    double   reliability_score;/* (bytes_received / bytes_sent) * 100; 0 when unverified      */
    int      is_verified;      /* 1 = server confirmed via Phase 3; 0 = local estimate         */
};

/*
 * metrics_result — legacy RTT/jitter result kept for internal use.
 *
 * rtts[i] holds the RTT in milliseconds for packet i, or -1.0 if lost.
 */
struct metrics_result {
    int     count;    /* Total packets sent            */
    int     received; /* Packets with a valid echo     */
    double *rtts;     /* Array of length `count`       */
};

/*
 * print_metrics — compute and display:
 *   - Packet-loss percentage
 *   - Min / Avg / Max RTT
 *   - Jitter (average of |RTT[i+1] - RTT[i]| over received packets)
 */
void print_metrics(FILE *out, const struct metrics_result *r);

/* print_bandwidth — display throughput result in plain text. */
void print_bandwidth(FILE *out, const struct bandwidth_result *bw);

/*
 * print_results_json — emit a JSON object matching the spec schema:
 *   { timestamp, ping_stats, bandwidth_stats }
 */
void print_results_json(FILE *out, const struct ping_result *ping,
                        const struct bandwidth_result *bw);

/*
 * print_udp_metrics — display a UDP jitter/loss test result table:
 *   Sent/Received counts, Packet Loss (% and raw), Jitter (avg, peak),
 *   and Capacity (achieved vs. target throughput).
 */
void print_udp_metrics(FILE *out, const struct udp_result *r);

#endif /* METRICS_H */
