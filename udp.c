/*
 * udp.c — UDP jitter and packet-loss test client (Phase B of spdchk).
 *
 * The client performs a three-phase UDP exchange:
 *   A. TCP handshake: negotiate parameters with the server.
 *   B. UDP send loop: constant bit-rate (CBR) datagram stream.
 *   C. TCP report: request the server's reception statistics.
 *
 * High-precision timing:
 *   Linux   — clock_gettime(CLOCK_MONOTONIC) + clock_nanosleep()
 *   Windows — QueryPerformanceCounter() + Sleep(1)/spin loop
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include "compat_win.h"
#ifndef _WIN32
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

#include "udp.h"
#include "spdchk.h"
#include "logger.h"

/* ------------------------------------------------------------------ */
/* High-precision monotonic timestamp in nanoseconds                   */
/* ------------------------------------------------------------------ */

static uint64_t udp_time_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int freq_init = 0;
    if (!freq_init) {
        QueryPerformanceFrequency(&freq);
        freq_init = 1;
    }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    /* Multiply before dividing to preserve precision. */
    return (uint64_t)(count.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/* ------------------------------------------------------------------ */
/* Sleep until an absolute nanosecond timestamp (absolute-time CBR)    */
/* ------------------------------------------------------------------ */

static void udp_sleep_until_ns(uint64_t target_ns)
{
#ifdef _WIN32
    /*
     * Hybrid: coarse Sleep(1) for the bulk of the wait, then spin
     * for the final ~2 ms using QPC for sub-millisecond accuracy.
     */
    for (;;) {
        uint64_t now = udp_time_ns();
        if (now >= target_ns)
            break;
        int64_t remaining = (int64_t)(target_ns - now);
        if (remaining > 2000000LL)
            Sleep(1);
        /* else spin — keeps CPU hot but is necessary for µs precision */
    }
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(target_ns / 1000000000ULL);
    ts.tv_nsec = (long)(target_ns   % 1000000000ULL);
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
#endif
}

/* ------------------------------------------------------------------ */
/* Non-blocking TCP connect with timeout                               */
/* (self-contained; mirrors connect_timed() in client.c)              */
/* ------------------------------------------------------------------ */

static int udp_tcp_connect(const char *ip, int port, int timeout_sec)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        sock_close(sock);
        return -1;
    }

#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket((SOCKET)sock, FIONBIO, &nb);
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        sock_close(sock);
        return -1;
    }
    if (rc != 0) {
        WSAPOLLFD pfd = { (SOCKET)sock, POLLOUT, 0 };
        if (WSAPoll(&pfd, 1, timeout_sec * 1000) <= 0) {
            sock_close(sock);
            return -1;
        }
        int err = 0; int errlen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
        if (err != 0) { sock_close(sock); return -1; }
    }
    nb = 0;
    ioctlsocket((SOCKET)sock, FIONBIO, &nb);
    DWORD rcv_ms = (DWORD)timeout_sec * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv_ms, sizeof(rcv_ms));
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        sock_close(sock);
        return -1;
    }
    if (rc != 0) {
        struct pollfd pfd = { .fd = sock, .events = POLLOUT };
        if (poll(&pfd, 1, timeout_sec * 1000) <= 0) {
            sock_close(sock);
            return -1;
        }
        int err = 0; socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err != 0) { errno = err; sock_close(sock); return -1; }
    }
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return sock;
}

/* ------------------------------------------------------------------ */
/* Phase A — TCP negotiation                                           */
/* ------------------------------------------------------------------ */

/*
 * Send "SPDCHK_UDP_REQ <bw_mbps> <pkt_size> <duration_sec>\n" and wait
 * for "OK\n" from the server.  This authorises the source IP for UDP
 * reception on the server side.
 */
static int udp_negotiate(const char *ip, int port,
                          double bw_mbps, int pkt_size, int duration_sec)
{
    int ctrl = udp_tcp_connect(ip, port, 10);
    if (ctrl < 0) {
        log_error("UDP", "negotiate: TCP connect failed: %s", strerror(errno));
        return -1;
    }

    char req[128];
    int  rlen = snprintf(req, sizeof(req),
                         "SPDCHK_UDP_REQ %.3f %d %d\n",
                         bw_mbps, pkt_size, duration_sec);
    if (rlen <= 0 || rlen >= (int)sizeof(req) ||
            send(ctrl, req, (size_t)rlen, MSG_NOSIGNAL) < 0) {
        log_error("UDP", "negotiate: send failed");
        sock_close(ctrl);
        return -1;
    }

    char resp[16] = {0};
    int  ri = 0;
    char c;
    while (ri < (int)sizeof(resp) - 1) {
        ssize_t r = recv(ctrl, &c, 1, 0);
        if (r <= 0) break;
        resp[ri++] = c;
        if (c == '\n') break;
    }
    resp[ri] = '\0';
    sock_close(ctrl);

    if (strncmp(resp, "OK", 2) != 0) {
        log_error("UDP", "negotiate: unexpected server response: %.15s", resp);
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Phase C — collect server report                                     */
/* ------------------------------------------------------------------ */

/*
 * Send "SPDCHK_UDP_DONE <packets_sent>\n" and parse the server's
 * "SPDCHK_UDP_REPORT <rx> <ooo> <jitter_us> <peak_us>\n" response.
 */
static int udp_collect_report(const char *ip, int port,
                               uint32_t packets_sent,
                               struct udp_result *result)
{
    int ctrl = udp_tcp_connect(ip, port, 5);
    if (ctrl < 0) {
        log_error("UDP", "collect: TCP connect failed: %s", strerror(errno));
        return -1;
    }

    char req[64];
    int  rlen = snprintf(req, sizeof(req),
                         "SPDCHK_UDP_DONE %u\n", packets_sent);
    if (rlen <= 0 || rlen >= (int)sizeof(req) ||
            send(ctrl, req, (size_t)rlen, MSG_NOSIGNAL) < 0) {
        sock_close(ctrl);
        return -1;
    }

    char resp[128] = {0};
    int  ri = 0;
    char c;
    while (ri < (int)sizeof(resp) - 1) {
        ssize_t r = recv(ctrl, &c, 1, 0);
        if (r <= 0) break;
        resp[ri++] = c;
        if (c == '\n') break;
    }
    resp[ri] = '\0';
    sock_close(ctrl);

    unsigned rx = 0, ooo = 0;
    double   jitter_us = 0.0, peak_us = 0.0;
    if (sscanf(resp, "SPDCHK_UDP_REPORT %u %u %lf %lf",
               &rx, &ooo, &jitter_us, &peak_us) < 2) {
        log_error("UDP", "collect: malformed server response: %.60s", resp);
        return -1;
    }

    result->packets_received = rx;
    result->out_of_order     = ooo;
    result->jitter_us        = jitter_us;
    result->peak_jitter_us   = peak_us;
    result->lost_packets     = (packets_sent >= rx) ? (packets_sent - rx) : 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* run_udp_client — public entry point                                  */
/* ------------------------------------------------------------------ */

int run_udp_client(const char *target_ip, int port,
                   double target_bw_mbps, int pkt_size,
                   int duration_sec, struct udp_result *result)
{
    memset(result, 0, sizeof(*result));
    result->target_bw_mbps = target_bw_mbps;

    int min_hdr = (int)sizeof(struct spdchk_udp_payload);
    if (pkt_size < min_hdr) {
        log_error("UDP",
                  "pkt_size %d below minimum header size %d; using %d",
                  pkt_size, min_hdr, min_hdr);
        pkt_size = min_hdr;
    }

    /* Phase A: negotiate */
    log_info("UDP",
             "negotiating with %s:%d  bw=%.1f Mbps  pkt=%d B  dur=%d s",
             target_ip, port, target_bw_mbps, pkt_size, duration_sec);
    if (udp_negotiate(target_ip, port,
                      target_bw_mbps, pkt_size, duration_sec) != 0) {
        log_error("UDP", "negotiation failed");
        return -1;
    }
    log_info("UDP", "negotiation OK — starting UDP transmission");

    /* Phase B: send UDP datagrams at CBR */
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        log_error("UDP", "socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, target_ip, &dest.sin_addr) != 1) {
        sock_close(udp_sock);
        return -1;
    }

    /* Pre-connect the UDP socket so send() needs no address each time. */
    if (connect(udp_sock,
                (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        log_error("UDP", "udp connect: %s", strerror(errno));
        sock_close(udp_sock);
        return -1;
    }

    char *pkt_buf = calloc(1, (size_t)pkt_size);
    if (!pkt_buf) {
        sock_close(udp_sock);
        return -1;
    }

    struct spdchk_udp_payload *hdr = (struct spdchk_udp_payload *)pkt_buf;
    hdr->magic_id = SPDCHK_UDP_MAGIC;

    /*
     * Inter-packet interval in nanoseconds for constant bit-rate sending:
     *   inter_ns = (pkt_size * 8 bits) / (target_bps) * 1e9 ns/s
     */
    double bits_per_pkt = (double)(pkt_size * 8);
    double target_bps   = target_bw_mbps * 1.0e6;
    uint64_t inter_ns   = (uint64_t)(bits_per_pkt / target_bps * 1.0e9);
    if (inter_ns == 0) inter_ns = 1;

    uint64_t start_ns   = udp_time_ns();
    uint64_t end_ns     = start_ns + (uint64_t)duration_sec * 1000000000ULL;
    uint64_t next_ns    = start_ns;
    uint32_t seq        = 0;
    uint64_t bytes_sent = 0;

    while (udp_time_ns() < end_ns) {
        udp_sleep_until_ns(next_ns);

        hdr->seq_number   = seq++;
        hdr->timestamp_ns = udp_time_ns();

        ssize_t n = send(udp_sock, pkt_buf, (size_t)pkt_size, MSG_NOSIGNAL);
        if (n > 0)
            bytes_sent += (uint64_t)n;

        next_ns += inter_ns;
    }

    uint64_t actual_dur_ns = udp_time_ns() - start_ns;
    free(pkt_buf);
    sock_close(udp_sock);

    result->packets_sent     = seq;
    result->achieved_bw_mbps = (actual_dur_ns > 0)
        ? (double)bytes_sent * 8.0 * 1.0e3 / (double)actual_dur_ns
        : 0.0;

    log_info("UDP", "sent %u packets  %.1f Mbps  %.3f s",
             seq, result->achieved_bw_mbps,
             (double)actual_dur_ns / 1.0e9);

    /*
     * Brief pause (200 ms) to allow the last datagrams to arrive at the
     * server before we request the report.  Acceptable for a diagnostic
     * tool; can be made configurable in a future release.
     */
#ifdef _WIN32
    Sleep(200);
#else
    {
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000L };
        nanosleep(&delay, NULL);
    }
#endif

    /* Phase C: collect server report */
    log_info("UDP", "collecting server report");
    if (udp_collect_report(target_ip, port, seq, result) != 0) {
        log_error("UDP",
                  "failed to retrieve server report — "
                  "receiver-side metrics unavailable");
        /*
         * Return 0 with sender-side data rather than failing entirely so
         * the caller can still display the achieved throughput.
         */
        return 0;
    }

    log_info("UDP",
             "result: rx=%u lost=%u ooo=%u jitter=%.3f us peak=%.3f us",
             result->packets_received, result->lost_packets,
             result->out_of_order, result->jitter_us, result->peak_jitter_us);
    return 0;
}
