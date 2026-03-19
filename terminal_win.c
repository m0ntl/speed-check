/*
 * terminal_win.c — Win32 console raw-mode and geometry implementation.
 *
 * Replaces the termios(3) raw-mode calls used by interactive.c on Linux
 * with their Win32 equivalents (SDD §2.2):
 *
 *   tcgetattr / tcsetattr  →  GetConsoleMode / SetConsoleMode
 *   ioctl TIOCGWINSZ       →  GetConsoleScreenBufferInfo
 *
 * VT sequence output is enabled via ENABLE_VIRTUAL_TERMINAL_PROCESSING so
 * the ANSI escape codes embedded in interactive.c and telemetry.c render
 * correctly on Windows 10+ consoles without any changes to the TUI logic.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "terminal_win.h"

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static DWORD g_saved_in_mode   = 0;
static int   g_raw_mode_active = 0;

/* ------------------------------------------------------------------ */
/* win_init_console                                                     */
/* ------------------------------------------------------------------ */

void win_init_console(void)
{
    /* --- Output handle: enable VT/ANSI rendering ------------------- */
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hout != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hout, &mode)) {
            SetConsoleMode(hout,
                mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING
                     | ENABLE_PROCESSED_OUTPUT);
        }
    }

    /* --- Input handle: enable VT input + extended key events ------- */
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hin, &mode)) {
            SetConsoleMode(hin,
                mode | ENABLE_VIRTUAL_TERMINAL_INPUT
                     | ENABLE_EXTENDED_FLAGS);
        }
    }
}

/* ------------------------------------------------------------------ */
/* win_set_raw_mode                                                     */
/* ------------------------------------------------------------------ */

void win_set_raw_mode(void)
{
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin == INVALID_HANDLE_VALUE)
        return;

    /* Capture the original mode only on the first activation. */
    if (!g_raw_mode_active)
        GetConsoleMode(hin, &g_saved_in_mode);

    /*
     * Disable line-buffered input (ENABLE_LINE_INPUT) and local echo
     * (ENABLE_ECHO_INPUT).  Keep ENABLE_PROCESSED_INPUT so that
     * Ctrl-C still generates SIGINT via the CRT's signal mechanism.
     */
    DWORD raw = g_saved_in_mode
              & ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    SetConsoleMode(hin, raw);
    g_raw_mode_active = 1;
}

/* ------------------------------------------------------------------ */
/* win_restore_mode                                                     */
/* ------------------------------------------------------------------ */

void win_restore_mode(void)
{
    if (!g_raw_mode_active)
        return;
    HANDLE hin = GetStdHandle(STD_INPUT_HANDLE);
    if (hin != INVALID_HANDLE_VALUE)
        SetConsoleMode(hin, g_saved_in_mode);
    g_raw_mode_active = 0;
}

/* ------------------------------------------------------------------ */
/* win_raw_mode_active / win_get_terminal_width                        */
/* ------------------------------------------------------------------ */

int win_raw_mode_active(void)
{
    return g_raw_mode_active;
}

int win_get_terminal_width(void)
{
    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hout == INVALID_HANDLE_VALUE)
        return 80;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(hout, &csbi))
        return 80;
    int width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    return (width > 0) ? width : 80;
}
