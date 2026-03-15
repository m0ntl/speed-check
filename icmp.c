#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <arpa/inet.h>

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

int icmp_check(const char *target_ip)
{
    int               sock;
    struct sockaddr_in dest;
    struct timeval     tv = { .tv_sec = 2, .tv_usec = 0 };
    uint8_t            pkt[sizeof(struct icmphdr)];
    struct icmphdr    *hdr = (struct icmphdr *)pkt;

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

    /* Build ICMP echo request */
    memset(pkt, 0, sizeof(pkt));
    hdr->type             = ICMP_ECHO;
    hdr->code             = 0;
    hdr->un.echo.id       = (uint16_t)(getpid() & 0xFFFF);
    hdr->un.echo.sequence = 1;
    hdr->checksum         = icmp_checksum(pkt, (int)sizeof(pkt));

    if (sendto(sock, pkt, sizeof(pkt), 0,
               (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        perror("icmp: sendto");
        close(sock);
        return -1;
    }

    /* Wait for reply — raw socket delivers full IP packet */
    uint8_t   reply[1500];
    socklen_t slen = sizeof(dest);
    ssize_t   n    = recvfrom(sock, reply, sizeof(reply), 0,
                               (struct sockaddr *)&dest, &slen);
    close(sock);

    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            fprintf(stderr, "[ICMP] Target Unreachable: no reply within 2 s.\n");
        else
            perror("icmp: recvfrom");
        return -1;
    }

    /* Skip the IP header to reach the ICMP header */
    struct ip *iphdr    = (struct ip *)reply;
    int        iphdrlen = iphdr->ip_hl * 4;

    if (n < iphdrlen + (ssize_t)sizeof(struct icmphdr)) {
        fprintf(stderr, "[ICMP] Malformed reply (too short).\n");
        return -1;
    }

    struct icmphdr *rep = (struct icmphdr *)(reply + iphdrlen);
    if (rep->type != ICMP_ECHOREPLY) {
        fprintf(stderr, "[ICMP] Unexpected ICMP type %d (expected ECHOREPLY).\n",
                (int)rep->type);
        return -1;
    }

    printf("[ICMP] Echo reply received — target is reachable.\n");
    return 0;
}
