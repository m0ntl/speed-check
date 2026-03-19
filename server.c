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
} sess_t;

static sess_t          g_sessions[MAX_SESSIONS];
static pthread_mutex_t g_sess_mtx = PTHREAD_MUTEX_INITIALIZER;

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

static void *handle_connection(void *arg)
{
    struct conn_arg *ctx = arg;
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
