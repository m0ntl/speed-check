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
