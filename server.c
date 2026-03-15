#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "server.h"
#include "spdchk.h"

/* Echo all received spdchk_payload frames back to the client. */
static void handle_client(int fd, const struct sockaddr_in *peer)
{
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &peer->sin_addr, addr_str, sizeof(addr_str));
    printf("[SERVER] Client connected from %s:%d\n",
           addr_str, ntohs(peer->sin_port));

    struct spdchk_payload pkt;
    ssize_t               n;

    while ((n = recv(fd, &pkt, sizeof(pkt), MSG_WAITALL)) > 0) {
        if (n != (ssize_t)sizeof(pkt)) {
            fprintf(stderr, "[SERVER] Short read (%zd bytes), dropping frame.\n", n);
            continue;
        }
        if (send(fd, &pkt, sizeof(pkt), 0) < 0) {
            if (errno != EPIPE && errno != ECONNRESET)
                perror("[SERVER] send");
            break;
        }
    }

    printf("[SERVER] Client %s disconnected.\n", addr_str);
    close(fd);
}

int run_server(int port)
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

    if (listen(server_fd, 5) < 0) {
        perror("server: listen");
        close(server_fd);
        return -1;
    }

    printf("[SERVER] Listening on port %d...\n", port);

    /* Ignore SIGPIPE so a broken client pipe doesn't kill the server */
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
        handle_client(client_fd, &peer);
    }

    close(server_fd);
    return 0;
}
