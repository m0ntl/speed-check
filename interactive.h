#ifndef INTERACTIVE_H
#define INTERACTIVE_H

/*
 * interactive_main — sole entry point for spdchk.
 *
 * All parameters (mode, target IP, port, streams, etc.) are configured
 * inside the interactive TUI.  The terminal is placed in raw
 * (non-canonical, no-echo) mode for the duration of the session and
 * restored on exit or on SIGINT/SIGTERM.
 *
 * Returns  0 on clean exit.
 * Returns -1 if stdin is not a tty or memory allocation fails.
 */
int interactive_main(void);

#endif /* INTERACTIVE_H */
