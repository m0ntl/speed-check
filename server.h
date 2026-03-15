#ifndef SERVER_H
#define SERVER_H

/*
 * run_server — bind to the given port, accept clients one at a time,
 * and echo each spdchk_payload back to the sender for RTT measurement.
 *
 * Runs until killed (SIGINT / SIGTERM).
 * Returns -1 on fatal socket error.
 */
int run_server(int port);

#endif /* SERVER_H */
