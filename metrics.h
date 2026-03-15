#ifndef METRICS_H
#define METRICS_H

/*
 * metrics_result — aggregated measurement data passed to print_metrics().
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

#endif /* METRICS_H */
