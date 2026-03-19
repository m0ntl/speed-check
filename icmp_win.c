/*
 * icmp_win.c — Winsock2 raw-socket ICMP implementation (SDD §2.1).
 *
 * Implements icmp_ping() for Windows using the same public interface as
 * icmp.c (see icmp.h).  Key differences from the Linux version:
 *
 *   • Socket API  : Winsock2 (SOCKET / INVALID_SOCKET / closesocket).
 *   • ICMP structs: defined locally; <netinet/ip_icmp.h> is not available.
 *   • Timing      : QueryPerformanceCounter for µs-precision RTT.
 *   • Privileges  : WSAEACCES on socket() maps to return code -2 so the
 *                   UI can display the same "Insufficient privileges" path
 *                   as the Linux build (SDD §2.1, §5 step 3).
 *
 * The RFC 1071 Internet Checksum logic is intentionally duplicated rather
 * than shared so that icmp.c and icmp_win.c remain independently buildable
 * with no shared translation unit (SDD §2.1 "Checksum" note).
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "icmp.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/* ICMP / IP wire structures (RFC 792 / RFC 791)                       */
/* ------------------------------------------------------------------ */

/* ICMP header — 8 bytes, same layout on all platforms */
#pragma pack(push, 1)
struct icmp_hdr {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
};

/* IPv4 header — 20 bytes minimum; IHL field gives the actual length */
struct ipv4_hdr {
    uint8_t  ver_ihl;      /* version (4 bits) + IHL in 32-bit words (4 bits) */
    uint8_t  tos;
    uint16_t tot_len;
    uint16_t ident;
    uint16_t frag_off;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_addr;
    uint32_t dst_addr;
};
#pragma pack(pop)

#define ICMP_ECHO       8
#define ICMP_ECHOREPLY  0

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* RFC 1071 one's-complement checksum */
static uint16_t icmp_checksum(void *buf, int len)
{
    uint16_t *ptr = (uint16_t *)buf;
    uint32_t  sum = 0;
    for (; len > 1; len -= 2)
        sum += *ptr++;
    if (len == 1)
        sum += *(uint8_t *)ptr;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)(~sum);
}

/*
 * qpc_diff_ms — elapsed time in milliseconds between two QPC snapshots
 * using the system's QueryPerformanceFrequency (SDD §4, §4.1):
 *
 *   Δt = (Count_end − Count_start) / Frequency
 */
static double qpc_diff_ms(LARGE_INTEGER start, LARGE_INTEGER end,
                           LARGE_INTEGER freq)
{
    return (double)(end.QuadPart - start.QuadPart)
         / (double)freq.QuadPart * 1000.0;
}

/* ------------------------------------------------------------------ */
/* icmp_ping                                                            */
/* ------------------------------------------------------------------ */

int icmp_ping(const char *target_ip, int count, struct icmp_stats *stats)
{
    SOCKET             sock;
    struct sockaddr_in dest;
    uint16_t           pid = (uint16_t)(GetCurrentProcessId() & 0xFFFF);

    stats->avg_latency_ms  = 0.0;
    stats->packet_loss_pct = 100.0;

    /* Raw ICMP socket — requires Administrator (SDD §2.1 "Constraint"). */
    sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock == INVALID_SOCKET) {
        int err = WSAGetLastError();
        log_error("ICMP", "socket: WSA error %d", err);
        return (err == WSAEACCES) ? -2 : -1;
    }

    /* 2-second receive timeout.  Winsock SO_RCVTIMEO takes a DWORD
     * timeout in milliseconds (not struct timeval as on POSIX). */
    DWORD rcv_ms = 2000;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&rcv_ms, sizeof(rcv_ms)) == SOCKET_ERROR) {
        log_error("ICMP", "setsockopt SO_RCVTIMEO: WSA error %d",
                  WSAGetLastError());
        closesocket(sock);
        return -1;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    if (inet_pton(AF_INET, target_ip, &dest.sin_addr) != 1) {
        log_error("ICMP", "invalid address '%s'", target_ip);
        closesocket(sock);
        return -1;
    }

    /* QPC frequency — queried once; constant after system boot. */
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int    received  = 0;
    double total_rtt = 0.0;

    for (int seq = 1; seq <= count; seq++) {
        uint8_t         pkt[sizeof(struct icmp_hdr)];
        struct icmp_hdr *hdr = (struct icmp_hdr *)pkt;

        memset(pkt, 0, sizeof(pkt));
        hdr->type     = ICMP_ECHO;
        hdr->code     = 0;
        hdr->id       = pid;
        hdr->seq      = (uint16_t)seq;
        hdr->checksum = icmp_checksum(pkt, (int)sizeof(pkt));

        LARGE_INTEGER t_start, t_end;
        QueryPerformanceCounter(&t_start);

        if (sendto(sock, (const char *)pkt, (int)sizeof(pkt), 0,
                   (struct sockaddr *)&dest, sizeof(dest)) == SOCKET_ERROR) {
            log_error("ICMP", "sendto: WSA error %d", WSAGetLastError());
            continue;
        }

        uint8_t reply[1500];
        int     slen = sizeof(dest);
        int     n    = recvfrom(sock, (char *)reply, (int)sizeof(reply), 0,
                                (struct sockaddr *)&dest, &slen);
        QueryPerformanceCounter(&t_end);

        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT)
                log_debug("ICMP", "seq %d: timeout", seq);
            else
                log_error("ICMP", "recvfrom: WSA error %d", err);
            continue;
        }

        if (n < (int)sizeof(struct ipv4_hdr) + (int)sizeof(struct icmp_hdr))
            continue;

        struct ipv4_hdr *iphdr  = (struct ipv4_hdr *)reply;
        int              iphlen = (iphdr->ver_ihl & 0x0F) * 4;

        if (n < iphlen + (int)sizeof(struct icmp_hdr))
            continue;

        struct icmp_hdr *rep = (struct icmp_hdr *)(reply + iphlen);
        if (rep->type != ICMP_ECHOREPLY || rep->id != pid)
            continue;

        double rtt = qpc_diff_ms(t_start, t_end, freq);
        total_rtt += rtt;
        received++;
        log_trace("ICMP", "seq %d: reply from %s  RTT = %.3f ms",
                  seq, target_ip, rtt);
    }

    closesocket(sock);

    if (received == 0) {
        log_error("ICMP", "Destination Unreachable — no replies received");
        return -1;
    }

    stats->avg_latency_ms  = total_rtt / received;
    stats->packet_loss_pct = (1.0 - (double)received / count) * 100.0;

    log_info("ICMP", "%d/%d packets received, %.1f%% loss, avg RTT %.3f ms",
             received, count,
             stats->packet_loss_pct, stats->avg_latency_ms);
    return 0;
}
