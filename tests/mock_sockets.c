/*
 * mock_sockets.c — Controllable stub implementations for socket-level calls.
 *
 * Compiled as part of the spdchk_test binary under TEST_MODE.  Tests set
 * the global control variables before calling the function under test, then
 * inspect the capture variables afterwards.
 */

#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "mock_sockets.h"

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */

int  mock_socket_return  = 10;   /* default: return a plausible fake fd */
int  mock_socket_errno   = 0;
int  mock_connect_return = 0;    /* default: immediate success          */
int  mock_connect_errno  = 0;
char mock_recv_buf[256]  = {0};
int  mock_recv_len       = 0;
int  mock_recv_pos       = 0;
char mock_send_buf[256]  = {0};
int  mock_send_len       = 0;

/* ------------------------------------------------------------------ */
/* Implementations                                                      */
/* ------------------------------------------------------------------ */

int mock_socket(int domain, int type, int protocol)
{
    (void)domain; (void)type; (void)protocol;
    if (mock_socket_return < 0) {
        errno = mock_socket_errno;
        return -1;
    }
    return mock_socket_return;
}

/* Covers all fcntl(fd, cmd) and fcntl(fd, cmd, arg) call patterns. */
int mock_fcntl_impl(int fd, int cmd, ...)
{
    (void)fd; (void)cmd;
    return 0;
}

int mock_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    (void)fd; (void)addr; (void)addrlen;
    if (mock_connect_return < 0) {
        errno = mock_connect_errno;
        return -1;
    }
    return 0;
}

int mock_setsockopt(int fd, int level, int optname,
                    const void *optval, socklen_t optlen)
{
    (void)fd; (void)level; (void)optname; (void)optval; (void)optlen;
    return 0;
}

ssize_t mock_send(int fd, const void *buf, size_t len, int flags)
{
    (void)fd; (void)flags;
    int n = (int)len;
    if (n > (int)(sizeof(mock_send_buf) - 1))
        n = (int)(sizeof(mock_send_buf) - 1);
    memcpy(mock_send_buf, buf, (size_t)n);
    mock_send_buf[n] = '\0';
    mock_send_len = n;
    return (ssize_t)len;
}

/* Returns bytes one-by-one from the pre-loaded response buffer.
 * Returns 0 (EOF) once the buffer is exhausted. */
ssize_t mock_recv(int fd, void *buf, size_t len, int flags)
{
    (void)fd; (void)flags;
    if (mock_recv_pos >= mock_recv_len)
        return 0;
    int n = (int)len;
    if (n > mock_recv_len - mock_recv_pos)
        n = mock_recv_len - mock_recv_pos;
    memcpy(buf, mock_recv_buf + mock_recv_pos, (size_t)n);
    mock_recv_pos += n;
    return (ssize_t)n;
}

/* sendto: pretend all bytes were sent (no data captured, icmp tests only). */
ssize_t mock_sendto(int fd, const void *buf, size_t len, int flags,
                    const struct sockaddr *dest_addr, socklen_t addrlen)
{
    (void)fd; (void)buf; (void)flags; (void)dest_addr; (void)addrlen;
    return (ssize_t)len;
}

/* recvfrom: simulate a timeout (no ICMP reply received). */
ssize_t mock_recvfrom(int fd, void *buf, size_t len, int flags,
                      struct sockaddr *src_addr, socklen_t *addrlen)
{
    (void)fd; (void)buf; (void)len; (void)flags; (void)src_addr; (void)addrlen;
    errno = EAGAIN;
    return -1;
}

int mock_close(int fd)
{
    (void)fd;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Reset helper                                                         */
/* ------------------------------------------------------------------ */

void mock_reset(void)
{
    mock_socket_return  = 10;
    mock_socket_errno   = 0;
    mock_connect_return = 0;
    mock_connect_errno  = 0;
    mock_recv_len       = 0;
    mock_recv_pos       = 0;
    mock_send_len       = 0;
    memset(mock_recv_buf, 0, sizeof(mock_recv_buf));
    memset(mock_send_buf, 0, sizeof(mock_send_buf));
}
