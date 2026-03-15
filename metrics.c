#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>

#include "metrics.h"

void print_metrics(const struct metrics_result *r)
{
    printf("\n--- spdchk statistics ---\n");

    if (r->count == 0) {
        printf("No packets were sent.\n");
        return;
    }

    /* Collect valid (non-lost) RTTs into a compact array */
    double *valid = malloc((size_t)r->count * sizeof(double));
    if (!valid) {
        fprintf(stderr, "metrics: out of memory\n");
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
    printf("Packets: %d sent, %d received, %.1f%% loss\n",
           r->count, r->received, loss);

    if (n_ok > 0) {
        double avg = sum / (double)n_ok;
        printf("RTT     : min=%.3f ms  avg=%.3f ms  max=%.3f ms\n",
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
        printf("Jitter  : %.3f ms\n", jitter);
    }

    free(valid);
}
