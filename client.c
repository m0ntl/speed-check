#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>

/* Platform-specific network headers (SDD §2.3). */
#include "compat_win.h"   /* sock_close, MSG_NOSIGNAL, winsock2 on Windows */
#ifndef _WIN32
#  include <unistd.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#endif

#include "client.h"
#include "logger.h"
#include "metrics.h"
#include "telemetry.h"

static int connect_timed(int fd, const struct sockaddr *addr, socklen_t len, int timeout_ms);
#include "spdchk.h"

#define SEND_BUF_SIZE       (64 * 1024)  /* 64 KiB per send call       */
#define CONNECT_TIMEOUT_SEC 10           /* abort if server unreachable */

/* Shared stop signal — set to 1 by main thread after the test duration. */
static volatile int g_stop = 0;

/* Active telemetry session; NULL when not in a bandwidth test. */
static spdchk_telemetry_t *g_telemetry = NULL;

/* One-shot TCP connection that exchanges version strings with the server.
 * When dss_mode is non-zero the greeting includes a DSS capability flag. */
int client_check_server_version(const char *target_ip, int port, int dss_mode)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_error("CLIENT", "version-check socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, target_ip, &addr.sin_addr);

    if (connect_timed(sock, (struct sockaddr *)&addr,
                      sizeof(addr), CONNECT_TIMEOUT_SEC) < 0) {
        log_error("CLIENT", "version check: cannot reach server: %s", strerror(errno));
        sock_close(sock);
        return -1;
    }

    /* 5-second receive timeout so an old server does not block forever.
     * Winsock SO_RCVTIMEO takes DWORD milliseconds; POSIX takes timeval. */
#ifdef _WIN32
    DWORD rcv_ms = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv_ms, sizeof(rcv_ms));
#else
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    /* Send greeting. */
    char greeting[64];
    if (dss_mode)
        snprintf(greeting, sizeof(greeting),
                 "SPDCHK_VER " SPDCHK_VERSION " DSS\n");
    else
        snprintf(greeting, sizeof(greeting),
                 "SPDCHK_VER " SPDCHK_VERSION "\n");
    if (send(sock, greeting, strlen(greeting), MSG_NOSIGNAL) < 0) {
        log_error("CLIENT", "version-check send: %s", strerror(errno));
        sock_close(sock);
        return -1;
    }

    /* Read server response line. */
    char resp[64];
    int  ri = 0;
    char c;
    while (ri < (int)sizeof(resp) - 1) {
        ssize_t r = recv(sock, &c, 1, 0);
        if (r <= 0) break;
        resp[ri++] = c;
        if (c == '\n') break;
    }
    resp[ri] = '\0';
    sock_close(sock);

    if (strncmp(resp, "OK", 2) == 0)
        return 0;

    if (strncmp(resp, "ERR VERSION_MISMATCH ", 21) == 0) {
        char *sv = resp + 21;
        char *nl = strchr(sv, '\n');
        if (nl) *nl = '\0';
        log_error("CLIENT",
                  "version mismatch: client=%s, server=%s. "
                  "Please upgrade both sides to the same version.",
                  SPDCHK_VERSION, sv);
        return -2;
    }

    if (ri == 0) {
        log_error("CLIENT",
                  "version check timed out — server may not support the handshake. "
                  "Please upgrade the server to %s.",
                  SPDCHK_VERSION);
        return -1;
    }

    log_error("CLIENT", "unexpected version-check response: %s", resp);
    return -1;
}

/* Per-stream worker context. */
struct stream_arg {
    const char *target_ip;
    int         port;
    int         stream_id;
    long long   bytes_sent;  /* output: -1 on connect failure */
};

/*
 * connect_timed — non-blocking connect with a timeout.
 *
 * On POSIX: fcntl O_NONBLOCK + poll().
 * On Windows: ioctlsocket FIONBIO + WSAPoll() (SDD §2.3).
 */
static int connect_timed(int fd, const struct sockaddr *addr,
                          socklen_t addrlen, int timeout_sec)
{
#ifdef _WIN32
    u_long nb = 1;
    if (ioctlsocket((SOCKET)fd, FIONBIO, &nb) == SOCKET_ERROR)
        return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        nb = 0;
        ioctlsocket((SOCKET)fd, FIONBIO, &nb);
        return 0;
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        nb = 0;
        ioctlsocket((SOCKET)fd, FIONBIO, &nb);
        return -1;
    }

    WSAPOLLFD pfd = { (SOCKET)fd, POLLOUT, 0 };
    int r = WSAPoll(&pfd, 1, timeout_sec * 1000);
    if (r <= 0) {
        nb = 0;
        ioctlsocket((SOCKET)fd, FIONBIO, &nb);
        return -1;
    }

    int err    = 0;
    int errlen = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen);
    nb = 0;
    ioctlsocket((SOCKET)fd, FIONBIO, &nb);
    return (err == 0) ? 0 : -1;

#else  /* POSIX */
    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;

    int rc = connect(fd, addr, addrlen);
    if (rc == 0) {
        fcntl(fd, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS)
        return -1;

    struct pollfd pfd = { .fd = fd, .events = POLLOUT };
    int r = poll(&pfd, 1, timeout_sec * 1000);
    if (r <= 0)
        return -1; /* timeout (0) or poll error (<0) */

    int       err = 0;
    socklen_t len = sizeof(err);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0) {
        errno = err;
        return -1;
    }

    fcntl(fd, F_SETFL, flags); /* restore blocking mode */
    return 0;
#endif
}

static void *stream_worker(void *arg)
{
    struct stream_arg *ctx = arg;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_error("CLIENT", "stream socket: %s", strerror(errno));
        __atomic_store_n(&ctx->bytes_sent, (long long)-1, __ATOMIC_RELAXED);
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)ctx->port);
    inet_pton(AF_INET, ctx->target_ip, &addr.sin_addr);

    if (connect_timed(sock, (struct sockaddr *)&addr,
                      sizeof(addr), CONNECT_TIMEOUT_SEC) < 0) {
        log_error("CLIENT", "stream %d: connect failed: %s",
                  ctx->stream_id, strerror(errno));
        sock_close(sock);
        __atomic_store_n(&ctx->bytes_sent, (long long)-1, __ATOMIC_RELAXED);
        return NULL;
    }

    char *buf = malloc(SEND_BUF_SIZE);
    if (!buf) {
        sock_close(sock);
        __atomic_store_n(&ctx->bytes_sent, (long long)-1, __ATOMIC_RELAXED);
        return NULL;
    }
    memset(buf, 0xAB, SEND_BUF_SIZE);

    while (!g_stop) {
        ssize_t n = send(sock, buf, SEND_BUF_SIZE, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        __atomic_fetch_add(&ctx->bytes_sent, (long long)n, __ATOMIC_RELAXED);
        /* Report bytes to the live telemetry display (SDD §4.2). */
        if (g_telemetry)
            atomic_fetch_add_explicit(&g_telemetry->total_bytes,
                                      (int64_t)n, memory_order_relaxed);
    }

    free(buf);
    sock_close(sock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Phase 3 — request server-verified byte count                        */
/* ------------------------------------------------------------------ */

/*
 * request_server_report — open a short control connection after the data
 * phase, send SPDCHK_REPORT_REQ, and wait up to 2 seconds for the server's
 * tally.  Returns the server-confirmed byte count and sets *is_verified = 1
 * on success.  Falls back to bytes_sent (local estimate) and *is_verified = 0
 * on any connection or timeout error.
 */
static uint64_t request_server_report(const struct client_args *args,
                                       uint64_t bytes_sent, int *is_verified)
{
    *is_verified = 0;

    int ctrl = socket(AF_INET, SOCK_STREAM, 0);
    if (ctrl < 0) {
        log_info("CLIENT", "Phase 3: socket failed — using local estimate");
        return bytes_sent;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)args->port);
    inet_pton(AF_INET, args->target_ip, &addr.sin_addr);

    if (connect_timed(ctrl, (struct sockaddr *)&addr, sizeof(addr), 2) < 0) {
        log_info("CLIENT", "Phase 3: control connection failed — using local estimate");
        sock_close(ctrl);
        return bytes_sent;
    }

    /* 2-second receive timeout */
#ifdef _WIN32
    DWORD rcv_ms = 2000;
    setsockopt(ctrl, SOL_SOCKET, SO_RCVTIMEO, (char *)&rcv_ms, sizeof(rcv_ms));
#else
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(ctrl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    if (send(ctrl, SPDCHK_REPORT_REQ, strlen(SPDCHK_REPORT_REQ),
             MSG_NOSIGNAL) < 0) {
        log_info("CLIENT", "Phase 3: send failed — using local estimate");
        sock_close(ctrl);
        return bytes_sent;
    }

    /* Read response line: "SPDCHK_REPORT <bytes> <duration_ms>\n" */
    char resp[128] = {0};
    int  ri        = 0;
    char c;
    while (ri < (int)sizeof(resp) - 1) {
        ssize_t r = recv(ctrl, &c, 1, 0);
        if (r <= 0) break;
        resp[ri++] = c;
        if (c == '\n') break;
    }
    resp[ri] = '\0';
    sock_close(ctrl);

    unsigned long long srv_bytes = 0;
    unsigned int       dur_ms    = 0;
    if (ri > 0 && sscanf(resp, "SPDCHK_REPORT %llu %u", &srv_bytes, &dur_ms) >= 1) {
        *is_verified = 1;
        log_info("CLIENT", "Phase 3: server report — %llu bytes in %u ms",
                 srv_bytes, dur_ms);
        return (uint64_t)srv_bytes;
    }

    log_info("CLIENT", "Phase 3: no valid report received — using local estimate");
    return bytes_sent;
}

/* ------------------------------------------------------------------ */
/* Stream Manager helpers                                              */
/* ------------------------------------------------------------------ */

static int spawn_stream(struct stream_arg *ctx, pthread_t *tid,
                        int idx, const char *ip, int port)
{
    ctx->target_ip  = ip;
    ctx->port       = port;
    ctx->stream_id  = idx + 1;
    ctx->bytes_sent = 0;
    return pthread_create(tid, NULL, stream_worker, ctx);
}

/* ------------------------------------------------------------------ */
/* Throughput Monitor + Scaling Engine (DSS)                           */
/* ------------------------------------------------------------------ */

static int run_bandwidth_dss(const struct client_args *args,
                             struct bandwidth_result  *out)
{
    struct stream_arg ctxs[DSS_MAX_STREAMS];
    pthread_t         tids[DSS_MAX_STREAMS];
    int               n = 0;

    memset(ctxs, 0, sizeof(ctxs));
    g_stop = 0;

    /* Absolute test end time */
    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    t_end.tv_sec += args->duration;

    long window_ns = (long)args->dss_window_ms * 1000000L;

    /* Launch the first stream */
    if (spawn_stream(&ctxs[n], &tids[n], n, args->target_ip, args->port) != 0) {
        log_error("CLIENT", "DSS: pthread_create: %s", strerror(errno));
        g_stop = 1;
        return -1;
    }
    n++;
    if (g_telemetry)
        g_telemetry->parallel_streams = n;
    log_info("CLIENT", "DSS: started stream 1 (window=%d ms, threshold=%.0f%%, cap=%d)",
             args->dss_window_ms, DSS_THRESHOLD * 100.0, DSS_MAX_STREAMS);

    long long prev_total = 0;
    double    prev_bw    = 0.0;   /* bytes/s in last window              */
    int       scaling    = 1;     /* 1 while still probing for more      */
    int       optimal_n  = 1;     /* last stream count that beat the bar */

    while (1) {
        /* Sleep one sampling window */
        struct timespec req = { .tv_sec  = 0, .tv_nsec = window_ns };
        nanosleep(&req, NULL);

        /* Has the test duration elapsed? */
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int expired = (now.tv_sec  > t_end.tv_sec) ||
                      (now.tv_sec == t_end.tv_sec &&
                       now.tv_nsec >= t_end.tv_nsec);

        /* Aggregate bytes across all active streams (Throughput Monitor) */
        long long total = 0;
        for (int i = 0; i < n; i++)
            total += __atomic_load_n(&ctxs[i].bytes_sent, __ATOMIC_RELAXED);

        double bw = (double)(total - prev_total)
                  / ((double)args->dss_window_ms / 1000.0);
        prev_total = total;

        if (expired)
            break;

        if (scaling) {
            /* Scaling Engine: gradient-ascent decision */
            if (prev_bw == 0.0 || bw > prev_bw * (1.0 + DSS_THRESHOLD)) {
                /* Improvement above threshold — current N is good */
                optimal_n = n;
                if (n < DSS_MAX_STREAMS) {
                    if (spawn_stream(&ctxs[n], &tids[n], n,
                                     args->target_ip, args->port) == 0) {
                        n++;
                        if (g_telemetry)
                            g_telemetry->parallel_streams = n;
                        log_debug("CLIENT", "DSS: %.1f Mbps → adding stream %d",
                                  bw / 1.0e6, n);
                    } else {
                        log_error("CLIENT", "DSS: pthread_create: %s", strerror(errno));
                        scaling = 0;
                    }
                } else {
                    log_debug("CLIENT", "DSS: safety cap (%d streams) reached",
                              DSS_MAX_STREAMS);
                    scaling = 0;
                }
            } else {
                /* Plateau detected — stop scaling */
                double gain_pct = prev_bw > 0.0
                                ? (bw - prev_bw) / prev_bw * 100.0
                                : 0.0;
                log_debug("CLIENT",
                          "DSS: plateau at %d stream(s)  %.1f Mbps  gain %.1f%% < %.0f%% threshold",
                          n, bw / 1.0e6, gain_pct, DSS_THRESHOLD * 100.0);
                scaling = 0;
            }
            prev_bw = bw;
        }
    }

    /* Signal all streams to stop and collect results */
    g_stop = 1;

    /*
     * Throughput is computed from the first `optimal_n` streams only.
     * Any extra probe stream(s) that caused plateau detection kept running
     * until now but must not inflate the reported result — the summary
     * must be consistent with the advertised optimal stream count.
     */
    long long total_bytes = 0;
    int       ok          = 0;
    for (int i = 0; i < n; i++) {
        pthread_join(tids[i], NULL);
        long long bs = __atomic_load_n(&ctxs[i].bytes_sent, __ATOMIC_RELAXED);
        if (i < optimal_n && bs >= 0) {
            total_bytes += bs;
            ok++;
        }
    }

    if (ok == 0) {
        log_error("CLIENT", "DSS: all streams failed to connect");
        return -1;
    }

    log_info("CLIENT", "DSS: steady state — %d optimal stream(s) of %d probed",
             optimal_n, n);

    out->duration_sec     = args->duration;
    out->parallel_streams = optimal_n;          /* effective count (throughput basis) */
    out->optimal_streams  = (n > optimal_n) ? n : 0; /* probed count; 0 = no extra probe */
    out->bytes_sent       = (uint64_t)total_bytes;   /* raw count; throughput computed after Phase 3 */
    return 0;
}

int run_client_ex(const struct client_args *args, struct run_client_result *result)
{
    /* ------------------------------------------------------------------ */
    /* Phase 0: Version check (skipped when caller has already done it)   */
    /* ------------------------------------------------------------------ */
    if (!args->skip_version_check) {
        log_info("CLIENT", "Phase 0 — version check (client %s)%s...",
                 SPDCHK_VERSION, args->dss_mode ? " [DSS]" : "");
        int ver_rc = client_check_server_version(args->target_ip, args->port,
                                                  args->dss_mode);
        if (ver_rc == -2)
            return -3;  /* version mismatch — distinguishable by caller */
        if (ver_rc != 0)
            return -1;
        log_info("CLIENT", "version OK");
    }

    /* ------------------------------------------------------------------ */
    /* Phase 1 (ICMP reachability) removed — TCP test runs directly.       */
    /* ------------------------------------------------------------------ */

    /* ------------------------------------------------------------------ */
    /* Phase 2: Parallel TCP bandwidth measurement                         */
    /* ------------------------------------------------------------------ */
    struct bandwidth_result bw = { 0 };

    /* Start live telemetry display (SDD §4.1).  Silent no-op when stdout
     * is not a TTY; always call telemetry_stop() in every code path. */
    spdchk_telemetry_t tel = {
        .total_duration   = args->duration,
        .parallel_streams = args->dss_mode ? 1 : args->streams,
        .avg_latency_ms   = 0.0,
    };
    pthread_t tel_tid;
    g_telemetry = &tel;
    telemetry_start(&tel, &tel_tid);

    if (args->dss_mode) {
        log_info("CLIENT",
                 "Phase 2 — bandwidth test: Dynamic Stream Scaling, %d s → %s:%d",
                 args->duration, args->target_ip, args->port);

        if (run_bandwidth_dss(args, &bw) != 0) {
            g_telemetry = NULL;
            telemetry_stop(&tel, &tel_tid);
            return -1;
        }
    } else {
        log_info("CLIENT", "Phase 2 — bandwidth test: %d stream(s), %d s → %s:%d",
                 args->streams, args->duration, args->target_ip, args->port);

        struct stream_arg *ctxs = calloc((size_t)args->streams, sizeof(*ctxs));
        pthread_t         *tids = calloc((size_t)args->streams, sizeof(*tids));
        if (!ctxs || !tids) {
            log_error("CLIENT", "calloc: %s", strerror(errno));
            free(ctxs);
            free(tids);
            g_telemetry = NULL;
            telemetry_stop(&tel, &tel_tid);
            return -1;
        }

        g_stop = 0;

        for (int i = 0; i < args->streams; i++) {
            ctxs[i].target_ip  = args->target_ip;
            ctxs[i].port       = args->port;
            ctxs[i].stream_id  = i + 1;
            ctxs[i].bytes_sent = 0;

            if (pthread_create(&tids[i], NULL, stream_worker, &ctxs[i]) != 0) {
                log_error("CLIENT", "pthread_create: %s", strerror(errno));
                g_stop = 1;
                for (int j = 0; j < i; j++)
                    pthread_join(tids[j], NULL);
                free(ctxs);
                free(tids);
                g_telemetry = NULL;
                telemetry_stop(&tel, &tel_tid);
                return -1;
            }
        }

        sleep((unsigned)args->duration);
        g_stop = 1;

        long long total_bytes = 0;
        int       ok_streams  = 0;
        for (int i = 0; i < args->streams; i++) {
            pthread_join(tids[i], NULL);
            long long bs = __atomic_load_n(&ctxs[i].bytes_sent, __ATOMIC_RELAXED);
            if (bs > 0) {
                total_bytes += bs;
                ok_streams++;
            }
        }

        free(ctxs);
        free(tids);

        if (ok_streams == 0) {
            log_error("CLIENT", "all streams failed to connect");
            g_telemetry = NULL;
            telemetry_stop(&tel, &tel_tid);
            return -1;
        }

        bw.duration_sec     = args->duration;
        bw.parallel_streams = args->streams;
        bw.optimal_streams  = 0;
        bw.bytes_sent       = (uint64_t)total_bytes; /* throughput computed after Phase 3 */
    }

    /* Stop the live display before printing the final statistics block. */
    g_telemetry = NULL;
    telemetry_stop(&tel, &tel_tid);

    /* ------------------------------------------------------------------ */
    /* Phase 3: Request server-verified byte count                         */
    /* The client waits up to 2 s for the server report; falls back to     */
    /* the local byte count and marks the result as "Estimated" on timeout.*/
    /* ------------------------------------------------------------------ */
    {
        int      is_verified    = 0;
        uint64_t bytes_received = request_server_report(args, bw.bytes_sent,
                                                        &is_verified);
        bw.bytes_received    = bytes_received;
        bw.is_verified       = is_verified;
        bw.reliability_score = (bw.bytes_sent > 0)
                             ? ((double)bytes_received / (double)bw.bytes_sent) * 100.0
                             : 100.0;
        bw.throughput_gbps   = (bw.duration_sec > 0)
                             ? ((double)bytes_received * 8.0)
                               / (double)bw.duration_sec / 1.0e9
                             : 0.0;
    }

    struct ping_result ping = {
        .avg_latency_ms  = 0.0,
        .packet_loss_pct = 0.0,
    };

    if (result) {
        result->throughput_gbps   = bw.throughput_gbps;
        result->avg_latency_ms    = 0.0;
        result->packet_loss_pct   = 0.0;
        result->reliability_score = bw.reliability_score;
        result->is_verified       = bw.is_verified;
    }

    FILE *out = stdout;
    if (args->output_path) {
        /* Reject paths containing ".." components to prevent directory
         * traversal writes, which are especially dangerous when this
         * process runs with elevated privileges (e.g. sudo for ICMP). */
        const char *op = args->output_path;
        int has_traversal = (strncmp(op, "..", 2) == 0 &&
                             (op[2] == '\0' || op[2] == '/' || op[2] == '\\'));
        if (!has_traversal) {
            for (const char *p = op; *p && !has_traversal; p++) {
                if ((*p == '/' || *p == '\\') &&
                        p[1] == '.' && p[2] == '.' &&
                        (p[3] == '\0' || p[3] == '/' || p[3] == '\\'))
                    has_traversal = 1;
            }
        }
        if (has_traversal) {
            log_error("CLIENT",
                      "output path '%s' contains '..' — directory traversal"
                      " is not permitted", args->output_path);
            return -1;
        }
        out = fopen(args->output_path, "w");
        if (!out) {
            log_error("CLIENT", "cannot open output file '%s': %s",
                      args->output_path, strerror(errno));
            return -1;
        }
    }

    if (args->json_output)
        print_results_json(out, &ping, &bw);
    else
        print_bandwidth(out, &bw);

    if (args->output_path)
        fclose(out);

    return 0;
}

int run_client(const struct client_args *args)
{
    return run_client_ex(args, NULL);
}
