#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "client.h"
#include "icmp.h"
#include "logger.h"
#include "metrics.h"

static int connect_timed(int fd, const struct sockaddr *addr, socklen_t len, int timeout_ms);
#include "spdchk.h"

#define SEND_BUF_SIZE       (64 * 1024)  /* 64 KiB per send call       */
#define CONNECT_TIMEOUT_SEC 10           /* abort if server unreachable */

/* Shared stop signal — set to 1 by main thread after the test duration. */
static volatile int g_stop = 0;

/* One-shot TCP connection that exchanges version strings with the server.
 * When dss_mode is non-zero the greeting includes a DSS capability flag. */
static int check_server_version(const char *target_ip, int port, int dss_mode)
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
        close(sock);
        return -1;
    }

    /* 5-second receive timeout so an old server does not block forever. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

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
        close(sock);
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
    close(sock);

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
        return -1;
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

/* Non-blocking connect with a poll()-based timeout. */
static int connect_timed(int fd, const struct sockaddr *addr,
                          socklen_t addrlen, int timeout_sec)
{
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
        close(sock);
        __atomic_store_n(&ctx->bytes_sent, (long long)-1, __ATOMIC_RELAXED);
        return NULL;
    }

    char *buf = malloc(SEND_BUF_SIZE);
    if (!buf) {
        close(sock);
        __atomic_store_n(&ctx->bytes_sent, (long long)-1, __ATOMIC_RELAXED);
        return NULL;
    }
    memset(buf, 0xAB, SEND_BUF_SIZE);

    while (!g_stop) {
        ssize_t n = send(sock, buf, SEND_BUF_SIZE, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        __atomic_fetch_add(&ctx->bytes_sent, (long long)n, __ATOMIC_RELAXED);
    }

    free(buf);
    close(sock);
    return NULL;
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

    long long total_bytes = 0;
    int       ok          = 0;
    for (int i = 0; i < n; i++) {
        pthread_join(tids[i], NULL);
        long long bs = __atomic_load_n(&ctxs[i].bytes_sent, __ATOMIC_RELAXED);
        if (bs >= 0) {
            total_bytes += bs;
            ok++;
        }
    }

    if (ok == 0) {
        log_error("CLIENT", "DSS: all streams failed to connect");
        return -1;
    }

    log_info("CLIENT", "DSS: steady state — %d optimal stream(s), %d total active",
             optimal_n, n);

    out->throughput_gbps  = ((double)total_bytes * 8.0)
                          / (double)args->duration / 1.0e9;
    out->duration_sec     = args->duration;
    out->parallel_streams = n;
    out->optimal_streams  = optimal_n;
    return 0;
}

int run_client_ex(const struct client_args *args, struct run_client_result *result)
{
    /* ------------------------------------------------------------------ */
    /* Phase 0: Version check                                              */
    /* ------------------------------------------------------------------ */
    log_info("CLIENT", "Phase 0 — version check (client %s)%s...",
             SPDCHK_VERSION, args->dss_mode ? " [DSS]" : "");
    if (check_server_version(args->target_ip, args->port, args->dss_mode) != 0)
        return -1;
    log_info("CLIENT", "version OK");

    /* ------------------------------------------------------------------ */
    /* Phase 1: ICMP reachability                                          */
    /* ------------------------------------------------------------------ */
    log_info("CLIENT", "Phase 1 — ICMP ping (%d packets) → %s",
             args->ping_count, args->target_ip);

    struct icmp_stats icmp_result;
    if (icmp_ping(args->target_ip, args->ping_count, &icmp_result) != 0) {
        log_error("CLIENT", "aborting: target unreachable");
        return -1;
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2: Parallel TCP bandwidth measurement                         */
    /* ------------------------------------------------------------------ */
    struct bandwidth_result bw = { 0 };

    if (args->dss_mode) {
        log_info("CLIENT",
                 "Phase 2 — bandwidth test: Dynamic Stream Scaling, %d s → %s:%d",
                 args->duration, args->target_ip, args->port);

        if (run_bandwidth_dss(args, &bw) != 0)
            return -1;
    } else {
        log_info("CLIENT", "Phase 2 — bandwidth test: %d stream(s), %d s → %s:%d",
                 args->streams, args->duration, args->target_ip, args->port);

        struct stream_arg *ctxs = calloc((size_t)args->streams, sizeof(*ctxs));
        pthread_t         *tids = calloc((size_t)args->streams, sizeof(*tids));
        if (!ctxs || !tids) {
            log_error("CLIENT", "calloc: %s", strerror(errno));
            free(ctxs);
            free(tids);
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
            return -1;
        }

        bw.throughput_gbps  = ((double)total_bytes * 8.0)
                            / (double)args->duration / 1.0e9;
        bw.duration_sec     = args->duration;
        bw.parallel_streams = args->streams;
        bw.optimal_streams  = 0;
    }

    struct ping_result ping = {
        .avg_latency_ms  = icmp_result.avg_latency_ms,
        .packet_loss_pct = icmp_result.packet_loss_pct,
    };

    if (result) {
        result->throughput_gbps = bw.throughput_gbps;
        result->avg_latency_ms  = icmp_result.avg_latency_ms;
        result->packet_loss_pct = icmp_result.packet_loss_pct;
    }

    FILE *out = stdout;
    if (args->output_path) {
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
