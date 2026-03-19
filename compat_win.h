/*
 * compat_win.h — Platform Abstraction Layer (PAL) shims for the Windows port.
 *
 * Include this header in any source file that performs socket I/O.  On Linux
 * it is a near-no-op: it only defines sock_close() as close() so the socket
 * files compile unchanged.  On Windows (MinGW-w64 + Winsock2) it supplies
 * the full Winsock2 / Win32 header set plus a set of thin compatibility macros
 * that let client.c, server.c, and the logger continue to compile without
 * wholesale duplication (SDD §2.3).
 *
 * Design rules:
 *   - Never redefine standard C library functions that might also be used for
 *     regular file I/O (e.g., do NOT globally redefine close()).
 *   - Provide sock_close(s) as the safe, portable way to close a socket.
 *   - Keep this header free of executable code where possible; helpers that
 *     must be inline are marked static inline to avoid multiply-defined symbol
 *     warnings when the header is included in multiple translation units.
 */

#ifndef COMPAT_WIN_H
#define COMPAT_WIN_H

#ifdef _WIN32

/* winsock2.h MUST precede windows.h to avoid winsock.h re-inclusion
 * conflicts that cause dozens of redefinition errors.               */
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>   /* socket/connect/send/recv and family        */
#include <ws2tcpip.h>   /* inet_pton, inet_ntop, getaddrinfo          */
#include <windows.h>    /* HANDLE, DWORD, QueryPerformanceCounter, …  */
#include <io.h>         /* _isatty, _read, _write                     */
#include <process.h>    /* _getpid (getpid shim)                      */

/* ------------------------------------------------------------------ */
/* Type shims                                                           */
/* ------------------------------------------------------------------ */
#ifndef ssize_t
typedef SSIZE_T ssize_t;
#endif

/* ------------------------------------------------------------------ */
/* String compatibility                                                 */
/* ------------------------------------------------------------------ */
#ifndef strncasecmp
#  define strncasecmp(a, b, n)  _strnicmp((a), (b), (n))
#endif

/* ------------------------------------------------------------------ */
/* Sleep                                                               */
/* ------------------------------------------------------------------ */
/*
 * usleep(microseconds) — Windows Sleep() has ~1 ms resolution; that is
 * acceptable for the DSS window / rate-limiter use-cases in this tool.
 */
static inline void usleep_compat(unsigned long us)
{
    DWORD ms = (DWORD)(us / 1000u);
    if (ms == 0 && us > 0) ms = 1;   /* always sleep at least 1 ms */
    Sleep(ms);
}
#define usleep(us)  usleep_compat(us)

/* ------------------------------------------------------------------ */
/* Socket close                                                         */
/* ------------------------------------------------------------------ */
/* Use sock_close() everywhere instead of close() for socket fds so   */
/* we never accidentally shadow the C-runtime close(fd) for files.    */
#define sock_close(s)   closesocket(s)

/* ------------------------------------------------------------------ */
/* MSG_NOSIGNAL — Windows has no SIGPIPE; this flag is a no-op        */
/* ------------------------------------------------------------------ */
#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL  0
#endif

/* ------------------------------------------------------------------ */
/* POSIX fd constants (unistd.h not always pulled in on Windows)       */
/* ------------------------------------------------------------------ */
#ifndef STDIN_FILENO
#  define STDIN_FILENO   0
#  define STDOUT_FILENO  1
#  define STDERR_FILENO  2
#endif

/* ------------------------------------------------------------------ */
/* isatty shim (unistd.h may not be available in all build configs)    */
/* ------------------------------------------------------------------ */
#ifndef isatty
#  define isatty(fd)     _isatty(fd)
#endif

/* ------------------------------------------------------------------ */
/* Last-socket-error helper                                             */
/* ------------------------------------------------------------------ */
/*
 * wsa_strerror() — format the last Winsock error code into a static
 * buffer.  Use this after socket calls instead of strerror(errno).
 */
static inline const char *wsa_strerror(void)
{
    static char buf[128];
    DWORD err = (DWORD)WSAGetLastError();
    DWORD n = FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, 0, buf, (DWORD)sizeof(buf), NULL);
    /* strip trailing CR/LF/period added by FormatMessage */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                     buf[n-1] == '.'  || buf[n-1] == ' '))
        buf[--n] = '\0';
    if (n == 0)
        snprintf(buf, sizeof(buf), "WSA error %lu", (unsigned long)err);
    return buf;
}

#else /* !_WIN32 ---------------------------------------------------- */

/*
 * On POSIX, sock_close() is plain close().  Files that include this
 * header must also include <unistd.h> for close() to be visible; all
 * existing network source files already do that.
 */
#define sock_close(s)    close(s)

#endif /* _WIN32 */
#endif /* COMPAT_WIN_H */
