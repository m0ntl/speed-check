#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>
#include <time.h>

#include "icmp.h"

/* RFC 1071 one's-complement checksum */
static uint16_t icmp_checksum(void *buf, int len)
{
    uint16_t *ptr = buf;
    uint32_t  sum = 0;

    for (; len > 1; len -= 2)
        sum += *ptr++;
    if (len == 1)
        sum += *(uint8_t *)ptr;

    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)(~sum);
}

static double timespec_diff_ms(const struct timespec *end,
                                const struct timespec *start)
{
    return (double)(end->tv_sec  - start->tv_sec)  * 1000.0
         + (double)(end->tv_nsec - start->tv_nsec) / 1.0e6;
}

int icmp_ping(const char *target_ip, int count, struct icmp_stats *stats)
{
    int               sock;
    struct sockaddr_in dest;
    struct timeval     tv  = { .tv_sec = 2, .tv_usec = 0 };
    uint16_t           pid = (uint16_t)(getpid() & 0xFFFF);

    stats->avg_latency_ms  = 0.0;
    stats->packet_loss_pct = 100.0;

    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        perror("icmp: socket");
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        perror("icmp: setsockopt SO_RCVTIMEO");
        close(sock);
        return -1;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "icmp: invalid address '%s'\n", target_ip);
        close(sock);
        return -1;
    }

    int    received  = 0;
    double total_rtt = 0.0;

    for (int seq = 1; seq <= count; seq++) {
        uint8_t        pkt[sizeof(struct icmphdr)];
        struct icmphdr *hdr = (struct icmphdr *)pkt;

        memset(pkt, 0, sizeof(pkt));
        hdr->type             = ICMP_ECHO;
        hdr->code             = 0;
        hdr->un.echo.id       = pid;
        hdr->un.echo.sequence = (uint16_t)seq;
        hdr->checksum         = icmp_checksum(pkt, (int)sizeof(pkt));

        struct timespec t_start, t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        if (sendto(sock, pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&dest, sizeof(dest)) < 0) {
            perror("icmp: sendto");
            continue;
        }

        uint8_t   reply[1500];
        socklen_t slen = sizeof(dest);
        ssize_t   n    = recvfrom(sock, reply, sizeof(reply), 0,
                                   (struct sockaddr *)&dest, &slen);
        clock_gettime(CLOCK_MONOTONIC, &t_end);

        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                printf("[ICMP] Seq %d: timeout\n", seq);
            else
                perror("icmp: recvfrom");
            continue;
        }

        struct ip *iphdr    = (struct ip *)reply;
        int        iphdrlen = iphdr->ip_hl * 4;

        if (n < iphdrlen + (ssize_t)sizeof(struct icmphdr))
            continue;

        struct icmphdr *rep = (struct icmphdr *)(reply + iphdrlen);
        if (rep->type != ICMP_ECHOREPLY || rep->un.echo.id != pid)
            continue;

        double rtt = timespec_diff_ms(&t_end, &t_start);
        total_rtt += rtt;
        received++;
        printf("[ICMP] Seq %d: reply from %s  RTT = %.3f ms\n",
               seq, target_ip, rtt);
    }

    close(sock);

    if (received == 0) {
        fprintf(stderr, "[ICMP] Error: Destination Unreachable — no replies received.\n");
        return -1;
    }

    stats->avg_latency_ms  = total_rtt / received;
    stats->packet_loss_pct = (1.0 - (double)received / count) * 100.0;

    printf("[ICMP] %d/%d packets received, %.1f%% loss, avg RTT %.3f ms\n",
           received, count,
           stats->packet_loss_pct, stats->avg_latency_ms);
    return 0;
}
