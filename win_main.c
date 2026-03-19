/*
 * win_main.c — Windows entry point for spdchk (SDD §2, §5).
 *
 * Responsibilities (SDD §5 execution flow):
 *   1. Initialize Winsock 2.2 via WSAStartup() before any socket call.
 *   2. Warn if the process is not running as Administrator (raw ICMP
 *      sockets require elevation; without it icmp_win.c returns -2).
 *   3. Enable ANSI/VT console output so the existing escape codes render.
 *   4. Parse the same CLI flags as main.c: -v / --log-level N.
 *   5. Invoke the shared interactive_main() entry point.
 *   6. Call WSACleanup() before exit.
 *
 * This file replaces main.c in the Windows build (see the Windows branch
 * of the Makefile).  All TUI and test logic remains in the shared files.
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
/* Administrator privilege check                                        */
/* ------------------------------------------------------------------ */

/*
 * is_elevated — return 1 when the current process token carries the
 * elevated Administrator SID (UAC-raised or full-admin session).
 *
 * Uses TOKEN_ELEVATION / TokenElevation rather than IsUserAnAdmin()
 * (shell32) so no extra DLL is required at start-up.
 */
static int is_elevated(void)
{
    BOOL   elevated = FALSE;
    HANDLE tok      = NULL;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok))
        return 0;

    TOKEN_ELEVATION elev;
    DWORD           len = 0;
    if (GetTokenInformation(tok, TokenElevation,
                             &elev, sizeof(elev), &len))
        elevated = elev.TokenIsElevated;

    CloseHandle(tok);
    return elevated ? 1 : 0;
}

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

    /* ---- Step 1: Administrator privilege warning ---- */
    if (!is_elevated()) {
        fprintf(stderr,
                "spdchk: WARNING — not running as Administrator.\n"
                "  ICMP raw sockets require elevation; reachability\n"
                "  tests will report \"Insufficient privileges\".\n\n");
    }

    /* ---- Step 2: Enable ANSI/VT console output ---- */
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
