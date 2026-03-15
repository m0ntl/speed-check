#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "spdchk.h"
#include "server.h"
#include "client.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "spdchk — network diagnostic utility\n\n"
            "Usage:\n"
            "  Server : sudo %s -s [-p <port>]\n"
            "  Client : sudo %s -c <IP> [-p <port>] [-i <count>]\n\n"
            "Options:\n"
            "  -s          Start in server mode (passive listener)\n"
            "  -c <IP>     Start in client mode, target IPv4 address\n"
            "  -p <port>   Port number           (default: %d)\n"
            "  -i <count>  Packets to send       (default: %d)\n",
            prog, prog, DEFAULT_PORT, DEFAULT_COUNT);
}

int main(int argc, char *argv[])
{
    int   mode_server = 0;
    char *target_ip   = NULL;
    int   port        = DEFAULT_PORT;
    int   count       = DEFAULT_COUNT;
    int   opt;

    while ((opt = getopt(argc, argv, "sc:p:i:")) != -1) {
        switch (opt) {
        case 's':
            mode_server = 1;
            break;
        case 'c':
            target_ip = optarg;
            break;
        case 'p':
            port = atoi(optarg);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Error: invalid port '%s' (must be 1-65535).\n",
                        optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'i':
            count = atoi(optarg);
            if (count <= 0) {
                fprintf(stderr, "Error: invalid packet count '%s'.\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (mode_server && target_ip) {
        fprintf(stderr, "Error: -s and -c are mutually exclusive.\n\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!mode_server && !target_ip) {
        fprintf(stderr, "Error: specify either -s (server) or -c <IP> (client).\n\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (mode_server)
        return run_server(port, 0) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    return run_client(target_ip, port, count) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
