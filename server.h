#ifndef SERVER_H
#define SERVER_H

/*
 * run_server — bind to port, accept parallel client streams, and drain
 * all incoming data (bandwidth-sink mode).
 *
 * Each accepted connection is handled in a dedicated thread so that
 * multiple parallel streams from a single client can run concurrently.
 *
 * If max_duration > 0 every connection is force-closed after that many
 * seconds, preventing rogue tests from monopolising the server.
 *
 * Runs until killed (SIGINT / SIGTERM).
 * Returns -1 on fatal socket error.
 */
int run_server(int port, int max_duration);

#endif /* SERVER_H */
