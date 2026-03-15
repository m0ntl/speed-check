#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <time.h>

#include "metrics.h"
#include "logger.h"

void print_metrics(FILE *out, const struct metrics_result *r)
{
    fprintf(out, "\n--- spdchk statistics ---\n");

    if (r->count == 0) {
        fprintf(out, "No packets were sent.\n");
        return;
    }

    /* Collect valid (non-lost) RTTs into a compact array */
    double *valid = malloc((size_t)r->count * sizeof(double));
    if (!valid) {
        log_error("METRICS", "out of memory");
        return;
    }

    int    n_ok  = 0;
    double sum   = 0.0;
    double min_t = DBL_MAX;
    double max_t = 0.0;

    for (int i = 0; i < r->count; i++) {
        if (r->rtts[i] >= 0.0) {
            valid[n_ok++] = r->rtts[i];
            sum          += r->rtts[i];
            if (r->rtts[i] < min_t) min_t = r->rtts[i];
            if (r->rtts[i] > max_t) max_t = r->rtts[i];
        }
    }

    /* Packet-loss percentage: (1 - received/sent) * 100 */
    double loss = (1.0 - (double)r->received / (double)r->count) * 100.0;
    fprintf(out, "Packets: %d sent, %d received, %.1f%% loss\n",
           r->count, r->received, loss);

    if (n_ok > 0) {
        double avg = sum / (double)n_ok;
        fprintf(out, "RTT     : min=%.3f ms  avg=%.3f ms  max=%.3f ms\n",
               min_t, avg, max_t);

        /*
         * Jitter = average of consecutive RTT deltas (RFC-style):
         *   J = SUM |RTT[i+1] - RTT[i]| / (n-1)
         */
        double jitter = 0.0;
        if (n_ok > 1) {
            double delta_sum = 0.0;
            for (int i = 0; i < n_ok - 1; i++)
                delta_sum += fabs(valid[i + 1] - valid[i]);
            jitter = delta_sum / (double)(n_ok - 1);
        }
        fprintf(out, "Jitter  : %.3f ms\n", jitter);
    }

    free(valid);
}

void print_bandwidth(FILE *out, const struct bandwidth_result *bw)
{
    fprintf(out, "\n--- bandwidth statistics ---\n");
    if (bw->throughput_gbps >= 1.0)
        fprintf(out, "Throughput : %.3f Gbps\n", bw->throughput_gbps);
    else
        fprintf(out, "Throughput : %.1f Mbps\n", bw->throughput_gbps * 1000.0);
    fprintf(out, "Duration   : %d s\n", bw->duration_sec);
    if (bw->optimal_streams == 0)
        fprintf(out, "Streams    : %d\n", bw->parallel_streams);
    else if (bw->optimal_streams == bw->parallel_streams)
        fprintf(out, "Streams    : %d (DSS)\n", bw->parallel_streams);
    else
        fprintf(out, "Streams    : %d optimal (of %d probed, DSS)\n",
                bw->parallel_streams, bw->optimal_streams);
}

void print_results_json(FILE *out, const struct ping_result *ping,
                        const struct bandwidth_result *bw)
{
    time_t     now     = time(NULL);
    struct tm *tm_info = gmtime(&now);
    char       ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", tm_info);

    fprintf(out, "{\n");
    fprintf(out, "  \"timestamp\": \"%s\",\n", ts);
    fprintf(out, "  \"ping_stats\": {\n");
    fprintf(out, "    \"avg_latency_ms\": %.3f,\n", ping->avg_latency_ms);
    fprintf(out, "    \"packet_loss_pct\": %.1f\n",  ping->packet_loss_pct);
    fprintf(out, "  },\n");
    fprintf(out, "  \"bandwidth_stats\": {\n");
    fprintf(out, "    \"throughput_gbps\": %.3f,\n", bw->throughput_gbps);
    fprintf(out, "    \"duration_sec\": %d,\n",       bw->duration_sec);
    fprintf(out, "    \"parallel_streams\": %d",       bw->parallel_streams);
    if (bw->optimal_streams > 0)
        fprintf(out, ",\n    \"dss_probed_streams\": %d\n", bw->optimal_streams);
    else
        fprintf(out, "\n");
    fprintf(out, "  }\n");
    fprintf(out, "}\n");
}
