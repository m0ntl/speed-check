#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>

/* Platform-specific network headers (SDD §2.3). */
#include "compat_win.h"   /* sock_close, MSG_NOSIGNAL, winsock2 on Windows */
#ifndef _WIN32
#  include <unistd.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

#include "server.h"
#include "logger.h"
#include "spdchk.h"
#include "udp.h"

#ifndef _WIN32
#  include <time.h>
#endif

#define DRAIN_BUF_SIZE (64 * 1024)

/* Maximum number of simultaneous client threads.  Connections beyond this
 * limit are refused immediately to prevent resource exhaustion (DoS). */
#define MAX_CONCURRENT_CONNECTIONS 256

static pthread_mutex_t g_conn_mtx   = PTHREAD_MUTEX_INITIALIZER;
static int             g_conn_count = 0;

struct conn_arg {
    int                fd;
    struct sockaddr_in peer;
    int                max_duration;
};

/* ------------------------------------------------------------------ */
/* Session tracking — Phase 3 Verified Receiver-Side Throughput        */
/* Each test run from a unique client IP gets one session slot.        */
/* ------------------------------------------------------------------ */

#define MAX_SESSIONS 64

typedef struct {
    uint32_t   ip;            /* client IP (network byte order); 0 = free */
    long long  total_bytes;   /* accumulates via __atomic_fetch_add        */
    int        stream_count;  /* active data streams; via __atomic ops     */
    struct timespec created_at;
    /* UDP test fields (zero when no UDP test active for this IP)           */
    int        udp_active;          /* 1 = UDP test negotiated, in progress */
    uint32_t   udp_seq_max;         /* highest seq number received          */
    uint32_t   udp_received;        /* total UDP datagrams received         */
    uint32_t   udp_out_of_order;    /* datagrams arriving below seq_max     */
    double     udp_jitter_ns;       /* RFC 3550 smoothed jitter (ns)        */
    double     udp_peak_jitter_ns;  /* maximum per-packet transit delta (ns)*/
    uint64_t   udp_last_rx_ns;      /* local arrival time of last packet    */
    uint64_t   udp_last_tx_ns;      /* sender timestamp of last packet      */
} sess_t;

static sess_t          g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_sess_mtx = PTHREAD_MUTEX_INITIALIZER;

/* UDP listener socket descriptor (-1 when not running). */
static volatile int    g_udp_fd   = -1;

/* High-precision monotonic timestamp used by the UDP listener. */
static uint64_t srv_time_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    static int           freq_init = 0;
    if (!freq_init) { QueryPerformanceFrequency(&freq); freq_init = 1; }
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return (uint64_t)(count.QuadPart * 1000000000LL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

/*
 * sess_acquire — find or create a session for the given client IP.
 * Increments stream_count.  Returns session index, or -1 if all slots full.
 */
static int sess_acquire(uint32_t ip)
{
    pthread_mutex_lock(&g_sess_mtx);
    int free_slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].ip == ip) {
            int sc = __atomic_load_n(&g_sessions[i].stream_count, __ATOMIC_RELAXED);
            if (sc > 0) {
                /* active session: join it */
                __atomic_fetch_add(&g_sessions[i].stream_count, 1, __ATOMIC_RELAXED);
                pthread_mutex_unlock(&g_sess_mtx);
                return i;
            }
            /* stale session (stream_count == 0): reuse the slot */
            free_slot = i;
            break;
        }
        if (free_slot < 0 && g_sessions[i].ip == 0)
            free_slot = i;
    }
    if (free_slot < 0) {
        pthread_mutex_unlock(&g_sess_mtx);
        return -1;
    }
    __atomic_store_n(&g_sessions[free_slot].total_bytes,  (long long)0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sessions[free_slot].stream_count, (int)1,       __ATOMIC_RELAXED);
    clock_gettime(CLOCK_MONOTONIC, &g_sessions[free_slot].created_at);
    g_sessions[free_slot].ip = ip;
    pthread_mutex_unlock(&g_sess_mtx);
    return free_slot;
}

/* Decrement active stream count for a session slot. */
static void sess_release(int idx)
{
    if (idx >= 0)
        __atomic_fetch_sub(&g_sessions[idx].stream_count, 1, __ATOMIC_RELAXED);
}

/*
 * sess_udp_authorize — mark a client IP as authorised for UDP packet
 * reception and reset its UDP counters.  Creates a new session if none
 * exists for that IP.  Returns the session index on success, -1 if all
 * slots are full.  Must NOT be called with g_sess_mtx held.
 */
static int sess_udp_authorize(uint32_t ip)
{
    pthread_mutex_lock(&g_sess_mtx);

    /* Find existing session or first free slot. */
    int idx = -1;
    int free_slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].ip == ip) {
            idx = i;
            break;
        }
        if (free_slot < 0 && g_sessions[i].ip == 0)
            free_slot = i;
    }
    if (idx < 0) {
        if (free_slot < 0) {
            pthread_mutex_unlock(&g_sess_mtx);
            return -1;
        }
        idx = free_slot;
        memset(&g_sessions[idx], 0, sizeof(g_sessions[idx]));
        g_sessions[idx].ip = ip;
        clock_gettime(CLOCK_MONOTONIC, &g_sessions[idx].created_at);
    }

    /* Reset UDP state and authorise. */
    g_sessions[idx].udp_active         = 1;
    g_sessions[idx].udp_seq_max        = 0;
    g_sessions[idx].udp_received       = 0;
    g_sessions[idx].udp_out_of_order   = 0;
    g_sessions[idx].udp_jitter_ns      = 0.0;
    g_sessions[idx].udp_peak_jitter_ns = 0.0;
    g_sessions[idx].udp_last_rx_ns     = 0;
    g_sessions[idx].udp_last_tx_ns     = 0;

    pthread_mutex_unlock(&g_sess_mtx);
    return idx;
}

/* ------------------------------------------------------------------ */
/* UDP listener thread                                                 */
/* ------------------------------------------------------------------ */

/*
 * udp_listener_thread — runs for the lifetime of the server process.
 *
 * Opens a SOCK_DGRAM socket on `g_udp_fd`, receives datagrams, validates
 * their magic word, checks that the source IP has performed the TCP
 * handshake (udp_active == 1 in its session), then updates the per-IP
 * sequence counter and RFC 3550 jitter estimate.
 *
 * A 1-second SO_RCVTIMEO prevents blocking forever when traffic is quiet
 * so the process can exit cleanly on signal.
 */
static void *udp_listener_thread(void *arg)
{
    (void)arg;

    /* Buffer large enough for the largest expected datagram. */
    uint8_t buf[65536];

    log_info("SERVER", "UDP listener started on fd %d", g_udp_fd);

    while (1) {
        struct sockaddr_in peer;
        socklen_t          plen = sizeof(peer);
        ssize_t n = recvfrom(g_udp_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &plen);
        if (n < 0) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == WSAEINTR)
                continue;   /* timeout / signal — keep running */
            /* WSAECONNRESET / WSAENETRESET: Windows delivers a previous
             * ICMP "port unreachable" reply via recvfrom on unconnected
             * UDP sockets.  Ignore it so the listener keeps running. */
            if (err == WSAECONNRESET || err == WSAENETRESET) {
                log_debug("SERVER", "UDP listener: ignoring ICMP error %d", err);
                continue;
            }
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;   /* timeout / signal — keep running */
#endif
            log_debug("SERVER", "UDP recvfrom: %s", strerror(errno));
            break;
        }

        /* Packet must carry at least the fixed-size header. */
        if ((size_t)n < sizeof(struct spdchk_udp_payload))
            continue;

        struct spdchk_udp_payload *pkt = (struct spdchk_udp_payload *)buf;

        /* Filter rogue traffic by magic word. */
        if (pkt->magic_id != SPDCHK_UDP_MAGIC)
            continue;

        uint64_t rx_ns    = srv_time_ns();
        uint32_t peer_ip  = (uint32_t)peer.sin_addr.s_addr;

        /* Security: only accept packets from authorised IPs. */
        pthread_mutex_lock(&g_sess_mtx);
        int found = -1;
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].ip == peer_ip && g_sessions[i].udp_active) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            pthread_mutex_unlock(&g_sess_mtx);
            log_trace("SERVER",
                      "UDP: dropping packet from unauthorised source");
            continue;
        }

        sess_t *s = &g_sessions[found];

        /* Sequence tracking — out-of-order detection.
         * udp_received == 0 guards the first packet: seq_number and
         * udp_seq_max are both 0 so without the guard the first packet
         * would always be mislabelled out-of-order. */
        if (s->udp_received == 0 || pkt->seq_number > s->udp_seq_max)
            s->udp_seq_max = pkt->seq_number;
        else
            s->udp_out_of_order++;
        s->udp_received++;

        /*
         * RFC 3550 inter-arrival jitter:
         *   D(i,j) = (Rj - Sj) - (Ri - Si)
         *   J = J + (|D| - J) / 16
         */
        if (s->udp_last_rx_ns != 0) {
            double d =   ((double)rx_ns            - (double)pkt->timestamp_ns)
                       - ((double)s->udp_last_rx_ns - (double)s->udp_last_tx_ns);
            if (d < 0.0) d = -d;
            s->udp_jitter_ns += (d - s->udp_jitter_ns) / 16.0;
            if (d > s->udp_peak_jitter_ns)
                s->udp_peak_jitter_ns = d;
        }
        s->udp_last_rx_ns = rx_ns;
        s->udp_last_tx_ns = pkt->timestamp_ns;

        pthread_mutex_unlock(&g_sess_mtx);
    }

    log_info("SERVER", "UDP listener exiting");
    return NULL;
}

static void *handle_connection(void *arg)
{
    struct conn_arg   *ctx         = arg;
    int                fd           = ctx->fd;
    int                max_duration = ctx->max_duration;
    struct sockaddr_in peer         = ctx->peer;
    free(ctx);

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
    log_info("SERVER", "client connected from %s:%d",
             addr_str, ntohs(peer.sin_port));

    if (max_duration > 0) {
        /* Winsock SO_RCVTIMEO takes DWORD milliseconds; POSIX takes timeval. */
#ifdef _WIN32
        DWORD rcv_ms = (DWORD)max_duration * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                   (const char *)&rcv_ms, sizeof(rcv_ms));
#else
        struct timeval tv = { .tv_sec = max_duration, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    /* -------------------------------------------------------------- */
    /* Greeting detection: version handshake or Phase 3 report request  */
    /* -------------------------------------------------------------- */
    char    peek[32];
    ssize_t pn = recv(fd, peek, sizeof(peek) - 1, MSG_PEEK);
    if (pn >= 11 && memcmp(peek, "SPDCHK_VER ", 11) == 0) {
        /* Read and consume the full greeting line. */
        char line[64];
        int  li = 0;
        char ch;
        while (li < (int)sizeof(line) - 1) {
            if (recv(fd, &ch, 1, 0) <= 0) break;
            line[li++] = ch;
            if (ch == '\n') break;
        }
        line[li] = '\0';

        const char *cv = line + 11;   /* skip "SPDCHK_VER " */

        /* Strip optional capability flags (e.g. " DSS") that follow the
         * version token so the strcmp only compares the version string. */
        char ver_buf[32];
        strncpy(ver_buf, cv, sizeof(ver_buf) - 1);
        ver_buf[sizeof(ver_buf) - 1] = '\0';
        int dss_flag = 0;
        char *sp = strchr(ver_buf, ' ');
        char *nl = strchr(ver_buf, '\n');
        if (nl) *nl = '\0';
        if (sp) {
            dss_flag = (strstr(sp + 1, "DSS") != NULL);
            *sp = '\0';
        }

        if (strcmp(ver_buf, SPDCHK_VERSION) == 0) {
            send(fd, "OK\n", 3, MSG_NOSIGNAL);
            if (dss_flag)
                log_info("SERVER", "client %s: Dynamic Stream Scaling enabled",
                         addr_str);
        } else {
            char resp[64];
            int  rlen = snprintf(resp, sizeof(resp),
                                 "ERR VERSION_MISMATCH %s\n", SPDCHK_VERSION);
            if (rlen > 0 && rlen < (int)sizeof(resp))
                send(fd, resp, (size_t)rlen, MSG_NOSIGNAL);
            log_info("SERVER", "rejected %s: version mismatch (client=%s server=%s)",
                     addr_str, ver_buf, SPDCHK_VERSION);
        }
        sock_close(fd);
        pthread_mutex_lock(&g_conn_mtx);
        g_conn_count--;
        pthread_mutex_unlock(&g_conn_mtx);
        return NULL;
    }

    if (pn >= 17 && memcmp(peek, "SPDCHK_REPORT_REQ", 17) == 0) {
        /* Consume the greeting line */
        char ch;
        for (int li = 0; li < 32; li++) {
            if (recv(fd, &ch, 1, 0) <= 0) break;
            if (ch == '\n') break;
        }

        /* Security: only return data accumulated from the requesting IP. */
        uint32_t  peer_ip = (uint32_t)peer.sin_addr.s_addr;
        long long bytes   = 0;
        uint32_t  dur_ms  = 0;

        pthread_mutex_lock(&g_sess_mtx);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].ip == peer_ip) {
                bytes = __atomic_load_n(&g_sessions[i].total_bytes, __ATOMIC_RELAXED);
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long long elapsed_ms =
                    (now.tv_sec  - g_sessions[i].created_at.tv_sec)  * 1000LL +
                    (now.tv_nsec - g_sessions[i].created_at.tv_nsec) / 1000000LL;
                dur_ms = (uint32_t)(elapsed_ms > 0 ? elapsed_ms : 0);
                g_sessions[i].ip = 0;   /* free slot for reuse */
                break;
            }
        }
        pthread_mutex_unlock(&g_sess_mtx);

        char resp[128];
        int  rlen = snprintf(resp, sizeof(resp), "SPDCHK_REPORT %lld %u\n",
                             (long long)bytes, (unsigned)dur_ms);
        if (rlen > 0 && rlen < (int)sizeof(resp))
            send(fd, resp, (size_t)rlen, MSG_NOSIGNAL);
        log_info("SERVER",
                 "Phase 3 report sent to %s: %lld bytes in %u ms",
                 addr_str, (long long)bytes, (unsigned)dur_ms);
        sock_close(fd);
        pthread_mutex_lock(&g_conn_mtx);
        g_conn_count--;
        pthread_mutex_unlock(&g_conn_mtx);
        return NULL;
    }

    /* -------------------------------------------------------------- */
    /* UDP test negotiation — client sends                              */
    /*   "SPDCHK_UDP_REQ <bw_mbps> <pkt_size> <duration_sec>\n"       */
    /* -------------------------------------------------------------- */
    if (pn >= 14 && memcmp(peek, SPDCHK_UDP_REQ_PREFIX, 14) == 0) {
        char line[128];
        int  li = 0;
        char ch;
        while (li < (int)sizeof(line) - 1) {
            if (recv(fd, &ch, 1, 0) <= 0) break;
            line[li++] = ch;
            if (ch == '\n') break;
        }
        line[li] = '\0';

        double   bw_mbps = 0.0;
        int      pkt_sz  = 0;
        int      dur_sec = 0;
        /* Parse fields after the prefix: "SPDCHK_UDP_REQ %.3f %d %d\n" */
        if (sscanf(line + 14, " %lf %d %d", &bw_mbps, &pkt_sz, &dur_sec) >= 2
                && bw_mbps > 0.0 && pkt_sz >= 16) {
            uint32_t peer_ip = (uint32_t)peer.sin_addr.s_addr;
            if (g_udp_fd < 0) {
                /* UDP socket not ready — refuse gracefully */
                send(fd, "ERR UDP_UNAVAILABLE\n", 20, MSG_NOSIGNAL);
                log_error("SERVER",
                          "UDP_REQ from %s: UDP socket not available",
                          addr_str);
            } else if (sess_udp_authorize(peer_ip) < 0) {
                send(fd, "ERR SESSION_FULL\n", 17, MSG_NOSIGNAL);
                log_error("SERVER",
                          "UDP_REQ from %s: session table full", addr_str);
            } else {
                send(fd, "OK\n", 3, MSG_NOSIGNAL);
                log_info("SERVER",
                         "UDP test authorised for %s: "
                         "bw=%.1f Mbps pkt=%d B dur=%d s",
                         addr_str, bw_mbps, pkt_sz, dur_sec);
            }
        } else {
            send(fd, "ERR BAD_PARAMS\n", 15, MSG_NOSIGNAL);
            log_error("SERVER", "UDP_REQ from %s: bad parameters", addr_str);
        }
        sock_close(fd);
        pthread_mutex_lock(&g_conn_mtx);
        g_conn_count--;
        pthread_mutex_unlock(&g_conn_mtx);
        return NULL;
    }

    /* -------------------------------------------------------------- */
    /* UDP done / report request — client sends                         */
    /*   "SPDCHK_UDP_DONE <packets_sent>\n"                            */
    /* Server replies:                                                  */
    /*   "SPDCHK_UDP_REPORT <rx> <ooo> <jitter_us> <peak_us>\n"       */
    /* -------------------------------------------------------------- */
    if (pn >= 15 && memcmp(peek, SPDCHK_UDP_DONE_PREFIX, 15) == 0) {
        char line[64];
        int  li = 0;
        char ch;
        while (li < (int)sizeof(line) - 1) {
            if (recv(fd, &ch, 1, 0) <= 0) break;
            line[li++] = ch;
            if (ch == '\n') break;
        }
        line[li] = '\0';

        unsigned sent = 0;
        sscanf(line + 15, " %u", &sent);   /* may be 0 if parse fails */

        uint32_t peer_ip = (uint32_t)peer.sin_addr.s_addr;
        uint32_t udp_rx  = 0, udp_ooo = 0;
        double   jit_us  = 0.0, peak_us = 0.0;

        /* Security: only return stats accumulated for the requesting IP. */
        pthread_mutex_lock(&g_sess_mtx);
        for (int i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].ip == peer_ip && g_sessions[i].udp_active) {
                udp_rx  = g_sessions[i].udp_received;
                udp_ooo = g_sessions[i].udp_out_of_order;
                jit_us  = g_sessions[i].udp_jitter_ns      / 1000.0;
                peak_us = g_sessions[i].udp_peak_jitter_ns / 1000.0;
                /* Clear UDP state so the slot can be reused. */
                g_sessions[i].udp_active = 0;
                break;
            }
        }
        pthread_mutex_unlock(&g_sess_mtx);

        char resp[128];
        int rlen = snprintf(resp, sizeof(resp),
                            "SPDCHK_UDP_REPORT %u %u %.3f %.3f\n",
                            udp_rx, udp_ooo, jit_us, peak_us);
        if (rlen > 0 && rlen < (int)sizeof(resp))
            send(fd, resp, (size_t)rlen, MSG_NOSIGNAL);
        log_info("SERVER",
                 "UDP report sent to %s: sent=%u rx=%u ooo=%u jit=%.3f us",
                 addr_str, sent, udp_rx, udp_ooo, jit_us);
        sock_close(fd);
        pthread_mutex_lock(&g_conn_mtx);
        g_conn_count--;
        pthread_mutex_unlock(&g_conn_mtx);
        return NULL;
    }


    /* -------------------------------------------------------------- */
    /* Data drain — bandwidth sink with per-IP session accumulation    */
    /* -------------------------------------------------------------- */
    uint32_t peer_ip  = (uint32_t)peer.sin_addr.s_addr;
    int      sess_idx = sess_acquire(peer_ip);

    char *buf = malloc(DRAIN_BUF_SIZE);
    if (!buf) {
        if (sess_idx >= 0) sess_release(sess_idx);
        sock_close(fd);
        pthread_mutex_lock(&g_conn_mtx);
        g_conn_count--;
        pthread_mutex_unlock(&g_conn_mtx);
        return NULL;
    }

    ssize_t n;
    while ((n = recv(fd, buf, DRAIN_BUF_SIZE, 0)) > 0) {
        if (sess_idx >= 0)
            __atomic_fetch_add(&g_sessions[sess_idx].total_bytes,
                               (long long)n, __ATOMIC_RELAXED);
    }

#ifdef _WIN32
    if (n < 0 && WSAGetLastError() == WSAETIMEDOUT)
#else
    if (n < 0 && errno == EAGAIN)
#endif
        log_info("SERVER", "client %s: max-duration reached, closing", addr_str);

    free(buf);
    if (sess_idx >= 0) sess_release(sess_idx);
    log_info("SERVER", "client %s disconnected", addr_str);
    sock_close(fd);

    pthread_mutex_lock(&g_conn_mtx);
    g_conn_count--;
    pthread_mutex_unlock(&g_conn_mtx);
    return NULL;
}

int run_server(int port, int max_duration)
{
    int                server_fd;
    struct sockaddr_in addr;
    int                opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_error("SERVER", "socket: %s", strerror(errno));
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt)) < 0) {
        log_error("SERVER", "setsockopt SO_REUSEADDR: %s", strerror(errno));
        sock_close(server_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_error("SERVER", "bind: %s", strerror(errno));
        sock_close(server_fd);
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        log_error("SERVER", "listen: %s", strerror(errno));
        sock_close(server_fd);
        return -1;
    }

    if (max_duration > 0)
        log_info("SERVER", "listening on port %d (max-duration: %d s)",
                 port, max_duration);
    else
        log_info("SERVER", "listening on port %d", port);

    /* SIGPIPE does not exist on Windows; on POSIX ignore it so that
     * writing to a disconnected socket returns EPIPE instead of dying. */
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    /* ----------------------------------------------------------------
     * Start the UDP listener on the same port number.
     * The server binds to INADDR_ANY so UDP packets directed to the
     * server's IP will be received regardless of interface.
     * ---------------------------------------------------------------- */
    {
        int      udp_fd  = socket(AF_INET, SOCK_DGRAM, 0);
        int      opt_udp = 1;
        if (udp_fd >= 0) {
            setsockopt(udp_fd, SOL_SOCKET, SO_REUSEADDR,
                       (const char *)&opt_udp, sizeof(opt_udp));

#ifdef _WIN32
            DWORD rcv_ms = 1000;   /* 1 s timeout so the thread can check for exit */
            setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO,
                       (const char *)&rcv_ms, sizeof(rcv_ms));
#else
            struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
            setsockopt(udp_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

            struct sockaddr_in udp_addr;
            memset(&udp_addr, 0, sizeof(udp_addr));
            udp_addr.sin_family      = AF_INET;
            udp_addr.sin_addr.s_addr = INADDR_ANY;
            udp_addr.sin_port        = htons((uint16_t)port);

            if (bind(udp_fd, (struct sockaddr *)&udp_addr,
                     sizeof(udp_addr)) < 0) {
                log_error("SERVER",
                          "UDP bind on port %d failed: %s — "
                          "UDP test unavailable",
                          port, strerror(errno));
                sock_close(udp_fd);
            } else {
                g_udp_fd = udp_fd;
                pthread_t udp_tid;
                if (pthread_create(&udp_tid, NULL,
                                   udp_listener_thread, NULL) == 0) {
                    pthread_detach(udp_tid);
                    log_info("SERVER", "UDP listener ready on port %d", port);
                } else {
                    log_error("SERVER",
                              "UDP listener thread failed: %s",
                              strerror(errno));
                    g_udp_fd = -1;
                    sock_close(udp_fd);
                }
            }
        } else {
            log_error("SERVER", "UDP socket: %s — UDP test unavailable",
                      strerror(errno));
        }
    }

    while (1) {
        struct sockaddr_in peer;
        socklen_t          plen      = sizeof(peer);
        int                client_fd = accept(server_fd,
                                              (struct sockaddr *)&peer, &plen);
        if (client_fd < 0) {
            log_error("SERVER", "accept: %s", strerror(errno));
            continue;
        }

        /* Enforce connection limit before allocating resources. */
        pthread_mutex_lock(&g_conn_mtx);
        if (g_conn_count >= MAX_CONCURRENT_CONNECTIONS) {
            pthread_mutex_unlock(&g_conn_mtx);
            log_info("SERVER",
                     "connection limit (%d) reached — dropping new client",
                     MAX_CONCURRENT_CONNECTIONS);
            sock_close(client_fd);
            continue;
        }
        g_conn_count++;
        pthread_mutex_unlock(&g_conn_mtx);

        struct conn_arg *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            pthread_mutex_lock(&g_conn_mtx);
            g_conn_count--;
            pthread_mutex_unlock(&g_conn_mtx);
            sock_close(client_fd);
            continue;
        }
        ctx->fd           = client_fd;
        ctx->peer         = peer;
        ctx->max_duration = max_duration;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_connection, ctx) != 0) {
            log_error("SERVER", "pthread_create: %s", strerror(errno));
            free(ctx);
            pthread_mutex_lock(&g_conn_mtx);
            g_conn_count--;
            pthread_mutex_unlock(&g_conn_mtx);
            sock_close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    sock_close(server_fd);
    return 0;
}
