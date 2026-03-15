#ifndef METRICS_H
#define METRICS_H

/* ICMP phase summary */
struct ping_result {
    double avg_latency_ms;
    double packet_loss_pct;
};

/* TCP throughput phase summary */
struct bandwidth_result {
    double throughput_gbps;
    int    duration_sec;
    int    parallel_streams;
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
void print_metrics(const struct metrics_result *r);

/* print_bandwidth — display throughput result in plain text. */
void print_bandwidth(const struct bandwidth_result *bw);

/*
 * print_results_json — emit a JSON object matching the spec schema:
 *   { timestamp, ping_stats, bandwidth_stats }
 */
void print_results_json(const struct ping_result *ping,
                        const struct bandwidth_result *bw);

#endif /* METRICS_H */
