/*
 * mock_sockets.h — Socket-level mock layer for TEST_MODE builds.
 *
 * Force-included via -include tests/mock_sockets.h when compiling source
 * files under test (icmp_t.o, client_t.o).  Real system headers are pulled
 * in first so their declarations are unaffected by the function-like macros
 * defined below (include guards prevent re-processing on subsequent includes).
 */

#ifndef MOCK_SOCKETS_H
#define MOCK_SOCKETS_H

/*
 * Pull in the real system headers FIRST so their declarations are processed
 * before our function-like macros become active.  Include guards on these
 * headers prevent them from being re-processed if client.c / icmp.c later
 * include them as well, meaning the real declarations stay intact.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stddef.h>

#ifdef TEST_MODE

/* ------------------------------------------------------------------ */
/* Control variables (defined in mock_sockets.c)                       */
/* ------------------------------------------------------------------ */

/* socket() control */
extern int mock_socket_return;   /* fd returned by mock_socket(); -1 = error */
extern int mock_socket_errno;    /* errno set when mock_socket_return < 0    */

/* connect() control */
extern int mock_connect_return;  /* 0 = immediate success, -1 = error        */
extern int mock_connect_errno;   /* errno set when mock_connect_return < 0   */

/* recv() staged response: fill mock_recv_buf then set mock_recv_len */
extern char mock_recv_buf[256];
extern int  mock_recv_len;       /* total bytes loaded into mock_recv_buf    */
extern int  mock_recv_pos;       /* current read position (advanced by recv) */

/* send() capture: inspect after the function under test calls send() */
extern char mock_send_buf[256];
extern int  mock_send_len;       /* bytes captured from the most recent send */

/* ------------------------------------------------------------------ */
/* Mock function declarations                                          */
/* ------------------------------------------------------------------ */
int     mock_socket(int domain, int type, int protocol);
int     mock_fcntl_impl(int fd, int cmd, ...);
int     mock_connect(int fd, const struct sockaddr *addr, socklen_t addrlen);
int     mock_setsockopt(int fd, int level, int optname,
                        const void *optval, socklen_t optlen);
ssize_t mock_send(int fd, const void *buf, size_t len, int flags);
ssize_t mock_recv(int fd, void *buf, size_t len, int flags);
ssize_t mock_sendto(int fd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen);
ssize_t mock_recvfrom(int fd, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, socklen_t *addrlen);
int     mock_close(int fd);

/* Reset all mock state to defaults (success paths, empty buffers). */
void mock_reset(void);

/* ------------------------------------------------------------------ */
/* Macro overrides — redirect real calls to mock implementations       */
/* ------------------------------------------------------------------ */
#define socket(d,t,p)             mock_socket(d,t,p)
#define fcntl(fd, ...)            mock_fcntl_impl(fd, __VA_ARGS__)
#define connect(s,a,l)            mock_connect(s,a,l)
#define setsockopt(fd,l,o,v,sl)   mock_setsockopt(fd,l,o,v,sl)
#define send(s,b,n,f)             mock_send(s,b,n,f)
#define recv(s,b,n,f)             mock_recv(s,b,n,f)
#define sendto(s,b,n,f,d,dl)      mock_sendto(s,b,n,f,d,dl)
#define recvfrom(s,b,n,f,sa,sl)   mock_recvfrom(s,b,n,f,sa,sl)
#define close(fd)                 mock_close(fd)

#endif /* TEST_MODE */
#endif /* MOCK_SOCKETS_H */
