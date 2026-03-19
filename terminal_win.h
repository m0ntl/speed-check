/*
 * terminal_win.h — Win32 console raw-mode and geometry API (SDD §2.2).
 *
 * Included by interactive.c on Windows in place of the termios(3) calls.
 * All persistent state (saved console mode) lives inside terminal_win.c.
 *
 * Functions are safe to call from signal/exception handlers with the
 * exception of win_init_console(), which must be called once at start-up
 * from the main thread before any TUI output is produced.
 */

#ifndef TERMINAL_WIN_H
#define TERMINAL_WIN_H

/*
 * Key codes returned by win_read_key() — identical values to the KEY_*
 * defines in interactive.c so capture_input() can forward them directly.
 */
#define KEY_UP    1000
#define KEY_DOWN  1001
#define KEY_ENTER 1002
#define KEY_ESC   1003
#define KEY_QUIT  1004

/*
 * win_init_console — enable ANSI/Virtual Terminal Processing on the
 * output console handle so that the existing ANSI escape codes produced
 * by interactive.c and telemetry.c render correctly on Windows 10+
 * consoles (SDD §2.2).
 *
 * ENABLE_VIRTUAL_TERMINAL_INPUT is intentionally NOT set; key input is
 * handled via ReadConsoleInput() with virtual key codes in win_read_key()
 * which does not need VT sequence synthesis.
 *
 * On pre-Windows-10 hosts the SetConsoleMode flags may be rejected; the
 * function silently continues — text will contain raw escape sequences
 * but the application remains functional.
 *
 * Call once from win_main.c before interactive_main().
 */
void win_init_console(void);

/*
 * win_read_key — blocking read of one logical keypress using the Win32
 * ReadConsoleInput() API.  All non key-down INPUT_RECORD types (mouse
 * moves, resize, focus, key-up) are discarded so the caller receives
 * exactly one action per physical key press.
 *
 * Returns one of the KEY_* constants above, or the raw character code
 * for printable keys.  Returns -1 on I/O error.
 *
 * This replaces the _read() / WaitForSingleObject approach in
 * capture_input() which could return WAIT_OBJECT_0 for non-character
 * events, causing key presses to require two presses to register.
 */
int win_read_key(void);

/*
 * win_set_raw_mode — disable line-buffering (ENABLE_LINE_INPUT) and
 * character echo (ENABLE_ECHO_INPUT) on standard input.
 *
 * The original console mode is saved on the first call and is restored
 * by win_restore_mode().  Subsequent calls before a matching restore
 * are idempotent.
 */
void win_set_raw_mode(void);

/*
 * win_restore_mode — restore the console input mode saved by the first
 * call to win_set_raw_mode().
 *
 * Safe to call when raw mode is not active (no-op in that case).
 * Safe to call from SIGINT signal handlers.
 */
void win_restore_mode(void);

/*
 * win_raw_mode_active — return 1 if the console is currently in raw
 * mode, 0 otherwise.  Used by sig_cleanup() to decide whether a restore
 * is necessary.
 */
int win_raw_mode_active(void);

/*
 * win_get_terminal_width — return the current console window column
 * count via GetConsoleScreenBufferInfo (SDD §2.2).  Falls back to 80
 * when the output handle is redirected (pipe / file).
 */
int win_get_terminal_width(void);

#endif /* TERMINAL_WIN_H */
