/*
 * win_main.c — Windows entry point for spdchk.
 *
 * Responsibilities:
 *   1. Initialize Winsock 2.2 via WSAStartup() before any socket call.
 *   2. Enable ANSI/VT console output so the existing escape codes render.
 *   3. Parse the same CLI flags as main.c: -v / --log-level N.
 *   4. Invoke the shared interactive_main() entry point.
 *   5. Call WSACleanup() before exit.
 *
 * This file replaces main.c in the Windows build (see the Windows branch
 * of the Makefile).  All TUI and test logic remains in the shared files.
 *
 * Note: No Administrator privilege is required.  ICMP uses IcmpSendEcho
 * (iphlpapi) rather than raw sockets, so the binary runs as a standard
 * user for all functionality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>   /* MinGW-w64 provides getopt_long via <getopt.h> */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
/* winsock2.h before windows.h — see compat_win.h commentary */
#include <winsock2.h>
#include <windows.h>

#include "spdchk.h"
#include "logger.h"
#include "interactive.h"
#include "terminal_win.h"

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    /* ---- Step 1: Winsock 2.2 initialisation ---- */
    WSADATA wsa;
    int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        fprintf(stderr, "spdchk: WSAStartup failed (error %d)\n", rc);
        return EXIT_FAILURE;
    }

    /* ---- Step 2: UTF-8 output + enable ANSI/VT console ---- */
    SetConsoleOutputCP(CP_UTF8);
    win_init_console();

    /* ---- CLI flag parsing (mirrors main.c exactly) ---- */
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
                WSACleanup();
                return EXIT_FAILURE;
            }
            break;
        default:
            fprintf(stderr,
                    "spdchk %s — network speed diagnostic utility\n\n"
                    "Usage: spdchk.exe [-v] [--log-level N]\n\n"
                    "All parameters (target IP, mode, port, streams, etc.) are\n"
                    "configured within the interactive TUI.\n\n"
                    "  -v             Increase log verbosity (cumulative;\n"
                    "                 -v=INFO  -vv=DEBUG  -vvv=TRACE)\n"
                    "  --log-level N  Set log verbosity directly\n"
                    "                 (0=ERROR 1=INFO 2=DEBUG 3=TRACE)\n",
                    SPDCHK_VERSION);
            WSACleanup();
            return EXIT_FAILURE;
        }
    }

    /* Resolve log level: --log-level beats -v count; -v beats default. */
    log_level_t log_level = LOG_LEVEL_INFO;
    if (explicit_log >= 0)
        log_level = (log_level_t)explicit_log;
    else if (verbose_cnt > 0)
        log_level = (log_level_t)(verbose_cnt > 3 ? 3 : verbose_cnt);

    logger_init(log_level);

    /* ---- Step 2–5: Run the interactive TUI ---- */
    int ret = interactive_main() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    logger_close();
    WSACleanup();
    return ret;
}
