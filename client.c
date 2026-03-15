#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "client.h"
#include "icmp.h"
#include "metrics.h"
#include "spdchk.h"

#define SEND_BUF_SIZE       (64 * 1024)  /* 64 KiB per send call       */
#define CONNECT_TIMEOUT_SEC 10           /* abort if server unreachable */

/* Shared stop signal — set to 1 by main thread after the test duration. */
static volatile int g_stop = 0;

/* One-shot TCP connection that exchanges version strings with the server. */
static int check_server_version(const char *target_ip, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("[CLIENT] version-check: socket");
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    inet_pton(AF_INET, target_ip, &addr.sin_addr);

    if (connect_timed(sock, (struct sockaddr *)&addr,
                      sizeof(addr), CONNECT_TIMEOUT_SEC) < 0) {
        fprintf(stderr, "[CLIENT] Version check: cannot reach server: %s\n",
                strerror(errno));
        close(sock);
        return -1;
    }

    /* 5-second receive timeout so an old server does not block forever. */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Send greeting. */
    const char greeting[] = "SPDCHK_VER " SPDCHK_VERSION "\n";
    if (send(sock, greeting, sizeof(greeting) - 1, MSG_NOSIGNAL) < 0) {
        perror("[CLIENT] version-check: send");
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
        fprintf(stderr,
                "[CLIENT] Version mismatch: client=%s, server=%s.\n"
                "         Please upgrade both sides to the same version.\n",
                SPDCHK_VERSION, sv);
        return -1;
    }

    if (ri == 0) {
        fprintf(stderr,
                "[CLIENT] Version check timed out — the server may be an older\n"
                "         build that does not support the handshake.\n"
                "         Please upgrade the server to %s.\n",
                SPDCHK_VERSION);
        return -1;
    }

    fprintf(stderr, "[CLIENT] Unexpected version-check response: %s\n", resp);
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
        perror("client: socket");
        ctx->bytes_sent = -1;
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)ctx->port);
    inet_pton(AF_INET, ctx->target_ip, &addr.sin_addr);

    if (connect_timed(sock, (struct sockaddr *)&addr,
                      sizeof(addr), CONNECT_TIMEOUT_SEC) < 0) {
        fprintf(stderr, "[CLIENT] Stream %d: connect failed: %s\n",
                ctx->stream_id, strerror(errno));
        close(sock);
        ctx->bytes_sent = -1;
        return NULL;
    }

    char *buf = malloc(SEND_BUF_SIZE);
    if (!buf) {
        close(sock);
        ctx->bytes_sent = -1;
        return NULL;
    }
    memset(buf, 0xAB, SEND_BUF_SIZE);

    long long bytes = 0;
    while (!g_stop) {
        ssize_t n = send(sock, buf, SEND_BUF_SIZE, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        bytes += n;
    }

    free(buf);
    close(sock);
    ctx->bytes_sent = bytes;
    return NULL;
}

int run_client(const struct client_args *args)
{
    /* ------------------------------------------------------------------ */
    /* Phase 0: Version check                                              */
    /* ------------------------------------------------------------------ */
    printf("[CLIENT] Phase 0 — version check (client %s)...\n", SPDCHK_VERSION);
    if (check_server_version(args->target_ip, args->port) != 0)
        return -1;
    printf("[CLIENT] Version OK.\n\n");

    /* ------------------------------------------------------------------ */
    /* Phase 1: ICMP reachability                                          */
    /* ------------------------------------------------------------------ */
    printf("[CLIENT] Phase 1 — ICMP ping (%d packets) → %s\n",
           args->ping_count, args->target_ip);

    struct icmp_stats icmp_result;
    if (icmp_ping(args->target_ip, args->ping_count, &icmp_result) != 0) {
        fprintf(stderr, "[CLIENT] Aborting: target unreachable.\n");
        return -1;
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2: Parallel TCP bandwidth measurement                         */
    /* ------------------------------------------------------------------ */
    printf("\n[CLIENT] Phase 2 — bandwidth test: %d stream(s), %d s → %s:%d\n",
           args->streams, args->duration, args->target_ip, args->port);

    struct stream_arg *ctxs = calloc((size_t)args->streams, sizeof(*ctxs));
    pthread_t         *tids = calloc((size_t)args->streams, sizeof(*tids));
    if (!ctxs || !tids) {
        perror("client: calloc");
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
            perror("client: pthread_create");
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
        if (ctxs[i].bytes_sent > 0) {
            total_bytes += ctxs[i].bytes_sent;
            ok_streams++;
        }
    }

    free(ctxs);
    free(tids);

    if (ok_streams == 0) {
        fprintf(stderr, "[CLIENT] All streams failed to connect.\n");
        return -1;
    }

    double throughput_gbps = ((double)total_bytes * 8.0)
                           / (double)args->duration / 1.0e9;

    struct ping_result ping = {
        .avg_latency_ms  = icmp_result.avg_latency_ms,
        .packet_loss_pct = icmp_result.packet_loss_pct,
    };
    struct bandwidth_result bw = {
        .throughput_gbps  = throughput_gbps,
        .duration_sec     = args->duration,
        .parallel_streams = args->streams,
    };

    FILE *out = stdout;
    if (args->output_path) {
        out = fopen(args->output_path, "w");
        if (!out) {
            fprintf(stderr, "[CLIENT] Cannot open output file '%s': %s\n",
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
