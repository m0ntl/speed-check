#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <errno.h>

#include "client.h"
#include "icmp.h"
#include "metrics.h"
#include "spdchk.h"

/* Return elapsed time between two CLOCK_MONOTONIC samples in milliseconds. */
static double timespec_diff_ms(const struct timespec *end,
                                const struct timespec *start)
{
    double sec  = (double)(end->tv_sec  - start->tv_sec);
    double nsec = (double)(end->tv_nsec - start->tv_nsec);
    return sec * 1000.0 + nsec / 1.0e6;
}

int run_client(const char *target_ip, int port, int count)
{
    /* ------------------------------------------------------------------ */
    /* Phase 1: ICMP reachability                                          */
    /* ------------------------------------------------------------------ */
    printf("[CLIENT] Phase 1 — ICMP reachability check → %s\n", target_ip);
    if (icmp_check(target_ip) != 0) {
        fprintf(stderr, "[CLIENT] Aborting: target unreachable.\n");
        return -1;
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2: TCP measurement                                            */
    /* ------------------------------------------------------------------ */
    printf("[CLIENT] Phase 2 — TCP measurement, port %d, %d packets\n",
           port, count);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("client: socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, target_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "client: invalid address '%s'\n", target_ip);
        close(sock);
        return -1;
    }

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("client: connect");
        close(sock);
        return -1;
    }
    printf("[CLIENT] TCP connection established.\n\n");

    /* Allocate RTT array; use -1.0 as sentinel for lost packets. */
    double *rtts = malloc((size_t)count * sizeof(double));
    if (!rtts) {
        perror("client: malloc");
        close(sock);
        return -1;
    }
    for (int i = 0; i < count; i++)
        rtts[i] = -1.0;

    int received = 0;

    for (int i = 0; i < count; i++) {
        struct spdchk_payload pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.seq_num = (uint32_t)i;

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);
        pkt.ts = t_start;

        if (send(sock, &pkt, sizeof(pkt), 0) < 0) {
            perror("client: send");
            break;
        }

        struct spdchk_payload echo;
        ssize_t n = recv(sock, &echo, sizeof(echo), MSG_WAITALL);
        clock_gettime(CLOCK_MONOTONIC, &t_end);

        if (n != (ssize_t)sizeof(echo)) {
            fprintf(stderr,
                    "[CLIENT] Packet %d: incomplete echo (%zd / %zu bytes)\n",
                    i + 1, n, sizeof(echo));
            /* rtts[i] stays -1.0 (lost) */
            continue;
        }

        rtts[i] = timespec_diff_ms(&t_end, &t_start);
        received++;
        printf("[CLIENT] Packet %3d: RTT = %.3f ms\n", i + 1, rtts[i]);
    }

    close(sock);

    struct metrics_result result = {
        .count    = count,
        .received = received,
        .rtts     = rtts,
    };
    print_metrics(&result);

    free(rtts);
    return 0;
}
