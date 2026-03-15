#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>

#include "spdchk.h"
#include "logger.h"
#include "interactive.h"

int main(int argc, char *argv[])
{
    int verbose_cnt  = 0;
    int explicit_log = -1;
    int opt;

    static const struct option long_opts[] = {
        { "log-level", required_argument, NULL, 'L' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "v", long_opts, NULL)) != -1) {
        switch (opt) {
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
            fprintf(stderr,
                    "spdchk %s — network speed diagnostic utility\n\n"
                    "Usage: sudo %s [-v] [--log-level N]\n\n"
                    "All parameters (target IP, mode, port, streams, etc.) are\n"
                    "configured within the interactive TUI.\n\n"
                    "  -v             Increase log verbosity (cumulative;\n"
                    "                 -v=INFO  -vv=DEBUG  -vvv=TRACE)\n"
                    "  --log-level N  Set log verbosity directly\n"
                    "                 (0=ERROR 1=INFO 2=DEBUG 3=TRACE)\n",
                    SPDCHK_VERSION, argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Resolve log level: --log-level beats -v count; -v count beats default. */
    log_level_t log_level = LOG_LEVEL_INFO;
    if (explicit_log >= 0)
        log_level = (log_level_t)explicit_log;
    else if (verbose_cnt > 0)
        log_level = (log_level_t)(verbose_cnt > 3 ? 3 : verbose_cnt);
    logger_init(log_level);

    int ret = interactive_main() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    logger_close();
    return ret;
}
