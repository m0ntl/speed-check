#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"
#include "spdchk.h"

#define DRAIN_BUF_SIZE (64 * 1024)

struct conn_arg {
    int                fd;
    struct sockaddr_in peer;
    int                max_duration;
};

static void *handle_connection(void *arg)
{
    struct conn_arg *ctx = arg;
    int                fd           = ctx->fd;
    int                max_duration = ctx->max_duration;
    struct sockaddr_in peer         = ctx->peer;
    free(ctx);

    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer.sin_addr, addr_str, sizeof(addr_str));
    printf("[SERVER] Client connected from %s:%d\n",
           addr_str, ntohs(peer.sin_port));

    if (max_duration > 0) {
        struct timeval tv = { .tv_sec = max_duration, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* -------------------------------------------------------------- */
    /* Version handshake — detect "SPDCHK_VER <ver>\n" greeting.      */
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
                printf("[SERVER] Client %s: Dynamic Stream Scaling enabled.\n",
                       addr_str);
        } else {
            char resp[64];
            int  rlen = snprintf(resp, sizeof(resp),
                                 "ERR VERSION_MISMATCH %s\n", SPDCHK_VERSION);
            send(fd, resp, (size_t)rlen, MSG_NOSIGNAL);
            printf("[SERVER] Rejected %s: version mismatch "
                   "(client=%s server=%s).\n",
                   addr_str, ver_buf, SPDCHK_VERSION);
        }
        close(fd);
        return NULL;
    }

    char *buf = malloc(DRAIN_BUF_SIZE);
    if (!buf) {
        close(fd);
        return NULL;
    }

    ssize_t n;
    while ((n = recv(fd, buf, DRAIN_BUF_SIZE, 0)) > 0)
        ; /* drain — this is the bandwidth sink */

    if (n < 0 && errno == EAGAIN)
        printf("[SERVER] Client %s: max-duration reached, closing.\n", addr_str);

    free(buf);
    printf("[SERVER] Client %s disconnected.\n", addr_str);
    close(fd);
    return NULL;
}

int run_server(int port, int max_duration)
{
    int                server_fd;
    struct sockaddr_in addr;
    int                opt = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("server: socket");
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("server: setsockopt SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("server: bind");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("server: listen");
        close(server_fd);
        return -1;
    }

    if (max_duration > 0)
        printf("[SERVER] Listening on port %d (max-duration: %d s)...\n",
               port, max_duration);
    else
        printf("[SERVER] Listening on port %d...\n", port);

    signal(SIGPIPE, SIG_IGN);

    while (1) {
        struct sockaddr_in peer;
        socklen_t          plen      = sizeof(peer);
        int                client_fd = accept(server_fd,
                                              (struct sockaddr *)&peer, &plen);
        if (client_fd < 0) {
            perror("server: accept");
            continue;
        }

        struct conn_arg *ctx = malloc(sizeof(*ctx));
        if (!ctx) {
            close(client_fd);
            continue;
        }
        ctx->fd           = client_fd;
        ctx->peer         = peer;
        ctx->max_duration = max_duration;

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_connection, ctx) != 0) {
            perror("server: pthread_create");
            free(ctx);
            close(client_fd);
            continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    return 0;
}
