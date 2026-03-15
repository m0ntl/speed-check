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
            "spdchk — network speed diagnostic utility\n\n"
            "Usage:\n"
            "  Server : sudo %s -s [-p <port>] [-m <max-duration>]\n"
            "  Client : sudo %s -c <IP> [-p <port>] [-i <pings>]\n"
            "                         [-d <seconds>] [-n <streams>] [-j]\n\n"
            "Options:\n"
            "  -s             Start in server mode (passive listener)\n"
            "  -c <IP>        Start in client mode, target IPv4 address\n"
            "  -p <port>      Port number              (default: %d)\n"
            "  -i <pings>     ICMP pings to send       (default: %d)\n"
            "  -d <seconds>   Bandwidth test duration  (default: %d)\n"
            "  -n <streams>   Parallel TCP streams     (default: %d)\n"
            "  -m <seconds>   Server max-duration per test (0 = unlimited)\n"
            "  -j             Emit JSON output\n",
            prog, prog,
            DEFAULT_PORT, DEFAULT_COUNT, DEFAULT_DURATION, DEFAULT_STREAMS);
}

int main(int argc, char *argv[])
{
    int   mode_server = 0;
    char *target_ip   = NULL;
    int   port        = DEFAULT_PORT;
    int   ping_count  = DEFAULT_COUNT;
    int   duration    = DEFAULT_DURATION;
    int   streams     = DEFAULT_STREAMS;
    int   max_dur     = 0;
    int   json_output = 0;
    int   opt;

    while ((opt = getopt(argc, argv, "sc:p:i:d:n:m:j")) != -1) {
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
            ping_count = atoi(optarg);
            if (ping_count <= 0) {
                fprintf(stderr, "Error: invalid ping count '%s'.\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'd':
            duration = atoi(optarg);
            if (duration <= 0) {
                fprintf(stderr, "Error: invalid duration '%s'.\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'n':
            streams = atoi(optarg);
            if (streams <= 0) {
                fprintf(stderr, "Error: invalid stream count '%s'.\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'm':
            max_dur = atoi(optarg);
            if (max_dur < 0) {
                fprintf(stderr, "Error: invalid max-duration '%s'.\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'j':
            json_output = 1;
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
        return run_server(port, max_dur) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    struct client_args args = {
        .target_ip   = target_ip,
        .port        = port,
        .ping_count  = ping_count,
        .duration    = duration,
        .streams     = streams,
        .json_output = json_output,
    };
    return run_client(&args) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
