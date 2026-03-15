#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "spdchk.h"
#include "logger.h"
#include "server.h"
#include "client.h"
#include "interactive.h"

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
            "  -j             Emit JSON output\n"
            "  -o <file>      Write statistics to <file> instead of stdout\n"
            "  -I, --interactive  Interactive client mode (requires -c)\n"
            "  -v             Increase log verbosity (cumulative; -v=INFO -vv=DEBUG -vvv=TRACE)\n"
            "  --log-level N  Set log verbosity directly (0=ERROR 1=INFO 2=DEBUG 3=TRACE)\n",
            prog, prog,
            DEFAULT_PORT, DEFAULT_COUNT, DEFAULT_DURATION, DEFAULT_STREAMS);
}

int main(int argc, char *argv[])
{
    int   mode_server  = 0;
    char *target_ip    = NULL;
    int   port         = DEFAULT_PORT;
    int   ping_count   = DEFAULT_COUNT;
    int   duration     = DEFAULT_DURATION;
    int   streams      = DEFAULT_STREAMS;
    int   max_dur      = 0;
    int   json_output  = 0;
    char *output_path  = NULL;
    int   dss_mode     = 1;
    int   dss_window   = DSS_WINDOW_MS;
    int   verbose_cnt  = 0;
    int   explicit_log = -1;
    int   interactive  = 0;
    int   opt;

    static const struct option long_opts[] = {
        { "log-level",   required_argument, NULL, 'L' },
        { "interactive", no_argument,       NULL, 'I' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "sc:p:i:d:n:m:jDw:o:vI",
                              long_opts, NULL)) != -1) {
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
        case 'D':
            dss_mode = 1;
            break;
        case 'w':
            dss_window = atoi(optarg);
            if (dss_window <= 0) {
                fprintf(stderr, "Error: invalid DSS window '%s' (must be > 0 ms).\n",
                        optarg);
                return EXIT_FAILURE;
            }
            break;
        case 'o':
            output_path = optarg;
            break;
        case 'I':
            interactive = 1;
            break;
        case 'v':
            verbose_cnt++;
            break;
        case 'L':
            explicit_log = atoi(optarg);
            if (explicit_log < 0 || explicit_log > 3) {
                fprintf(stderr, "Error: --log-level must be 0-3.\n");
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

    if (interactive && mode_server) {
        fprintf(stderr, "Error: --interactive is a client-only flag; cannot be used with -s.\n\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!mode_server && !target_ip) {
        fprintf(stderr, "Error: specify either -s (server) or -c <IP> (client).\n\n");
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    /* Resolve log level: --log-level beats -v count; -v count beats default. */
    log_level_t log_level = LOG_LEVEL_INFO;
    if (explicit_log >= 0)
        log_level = (log_level_t)explicit_log;
    else if (verbose_cnt > 0)
        log_level = (log_level_t)(verbose_cnt > 3 ? 3 : verbose_cnt);
    logger_init(log_level);

    int ret;
    if (mode_server) {
        ret = run_server(port, max_dur) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    } else if (interactive) {
        ret = interactive_main(target_ip, port, ping_count) == 0
              ? EXIT_SUCCESS : EXIT_FAILURE;
    } else {
        struct client_args args = {
            .target_ip    = target_ip,
            .port         = port,
            .ping_count   = ping_count,
            .duration     = duration,
            .streams      = streams,
            .json_output  = json_output,
            .output_path  = output_path,
            .dss_mode     = dss_mode,
            .dss_window_ms= dss_window,
        };
        ret = run_client(&args) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    logger_close();
    return ret;
}
