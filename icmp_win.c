/*
 * icmp_win.c — Windows ICMP implementation using the IcmpSendEcho API.
 *
 * Implements icmp_ping() for Windows using the same public interface as
 * icmp.c (see icmp.h).  Key design decisions:
 *
 *   • API       : IcmpCreateFile / IcmpSendEcho / IcmpCloseHandle from
 *                 iphlpapi — no raw SOCK_RAW socket is opened, so no
 *                 Administrator privilege is required on Windows Vista+.
 *   • Timing    : QueryPerformanceCounter wraps each IcmpSendEcho call
 *                 for sub-millisecond RTT resolution on fast local links.
 *   • Return -2 : Preserved for ERROR_ACCESS_DENIED on IcmpCreateFile in
 *                 extreme lockdown environments; practically never fires
 *                 on a normal Windows installation.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "icmp.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/* icmp_ping                                                            */
/* ------------------------------------------------------------------ */

int icmp_ping(const char *target_ip, int count, struct icmp_stats *stats)
{
    stats->avg_latency_ms  = 0.0;
    stats->packet_loss_pct = 100.0;

    /* Resolve target address. */
    struct in_addr dest;
    if (inet_pton(AF_INET, target_ip, &dest) != 1) {
        log_error("ICMP", "invalid address '%s'", target_ip);
        return -1;
    }

    /*
     * Open an ICMP handle via the documented iphlpapi path.
     * Unlike SOCK_RAW/IPPROTO_ICMP, IcmpCreateFile does not require
     * Administrator privileges on Windows Vista and later.
     */
    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        log_error("ICMP", "IcmpCreateFile: error %lu", err);
        return (err == ERROR_ACCESS_DENIED) ? -2 : -1;
    }

    /* Reply buffer: struct + payload + mandatory 8-byte overhead. */
    static const char send_data[] = "spdchk probe";
    DWORD  reply_size = sizeof(ICMP_ECHO_REPLY) + sizeof(send_data) + 8;
    BYTE  *reply_buf  = (BYTE *)malloc(reply_size);
    if (!reply_buf) {
        IcmpCloseHandle(hIcmp);
        return -1;
    }

    /* QPC frequency — constant after system boot. */
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    int    received  = 0;
    double total_rtt = 0.0;

    for (int seq = 1; seq <= count; seq++) {
        memset(reply_buf, 0, reply_size);

        LARGE_INTEGER t_start, t_end;
        QueryPerformanceCounter(&t_start);

        DWORD nreplies = IcmpSendEcho(
            hIcmp,
            dest.s_addr,
            (LPVOID)send_data,
            (WORD)sizeof(send_data),
            NULL,        /* IP options (TTL, TOS, …) — use defaults */
            reply_buf,
            reply_size,
            2000         /* 2-second per-ping timeout (ms) */
        );

        QueryPerformanceCounter(&t_end);

        if (nreplies == 0) {
            DWORD err = GetLastError();
            if (err == IP_REQ_TIMED_OUT)
                log_debug("ICMP", "seq %d: timeout", seq);
            else
                log_error("ICMP", "IcmpSendEcho: error %lu", err);
            continue;
        }

        ICMP_ECHO_REPLY *reply = (ICMP_ECHO_REPLY *)reply_buf;
        if (reply->Status != IP_SUCCESS) {
            log_debug("ICMP", "seq %d: reply status %lu", seq, reply->Status);
            continue;
        }

        /* Use QPC wall-clock delta for sub-millisecond precision. */
        double rtt = (double)(t_end.QuadPart - t_start.QuadPart)
                   / (double)freq.QuadPart * 1000.0;
        total_rtt += rtt;
        received++;
        log_trace("ICMP", "seq %d: reply from %s  RTT = %.3f ms",
                  seq, target_ip, rtt);
    }

    free(reply_buf);
    IcmpCloseHandle(hIcmp);

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
