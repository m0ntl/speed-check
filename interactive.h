#ifndef INTERACTIVE_H
#define INTERACTIVE_H

/*
 * interactive_main — entry point for the arrow-key-navigated interactive
 * client mode (activated via -I / --interactive).
 *
 *   target_ip  : IPv4 address of the spdchk server (required).
 *   port       : TCP port the server listens on.
 *   ping_count : initial ICMP ping count; adjustable within the UI.
 *
 * Returns  0 on clean exit.
 * Returns -1 if stdin is not a tty or memory allocation fails.
 *
 * The terminal is placed in raw (non-canonical, no-echo) mode for the
 * duration of the session and restored on exit or on SIGINT/SIGTERM.
 */
int interactive_main(const char *target_ip, int port, int ping_count);

#endif /* INTERACTIVE_H */
