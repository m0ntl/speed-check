CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

# --------------------------------------------------------------------
# Platform detection — the Windows build uses separate source files for
# the modules that rely on OS-specific APIs (SDD §6):
#   icmp_win.c     replaces icmp.c     (Winsock2 raw sockets)
#   logger_win.c   replaces logger.c   (file-based log, no syslog)
#   terminal_win.c new file            (SetConsoleMode / TIOCGWINSZ)
#   win_main.c     replaces main.c     (WSAStartup + privilege check)
# --------------------------------------------------------------------

# Sources shared by all platforms
BASE_SRCS = server.c client.c metrics.c interactive.c telemetry.c

ifeq ($(OS),Windows_NT)
    CFLAGS  += -D_DEFAULT_SOURCE
    LDFLAGS  = -lm -Wl,-Bstatic -lpthread -Wl,-Bdynamic -lws2_32 -liphlpapi
    TARGET   = spdchk.exe
    SRCS     = $(BASE_SRCS) icmp_win.c logger_win.c terminal_win.c win_main.c
else
    CFLAGS  += -D_DEFAULT_SOURCE
    LDFLAGS  = -lm -lpthread
    TARGET   = spdchk
    SRCS     = $(BASE_SRCS) icmp.c logger.c main.c
endif

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) spdchk spdchk.exe
