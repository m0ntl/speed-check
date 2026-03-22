CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

# --------------------------------------------------------------------
# Platform detection — the Windows build uses separate source files for
# the modules that rely on OS-specific APIs (SDD §6):
#   icmp_win.c     replaces icmp.c     (iphlpapi IcmpSendEcho, no raw socket)
#   logger_win.c   replaces logger.c   (file-based log, no syslog)
#   terminal_win.c new file            (SetConsoleMode / TIOCGWINSZ)
#   win_main.c     replaces main.c     (WSAStartup, no elevation required)
#   spdchk.rc      Windows resource    (version info + application manifest)
# --------------------------------------------------------------------

# Sources shared by all platforms
BASE_SRCS = server.c client.c metrics.c interactive.c telemetry.c udp.c

ifeq ($(OS),Windows_NT)
    CFLAGS  += -D_DEFAULT_SOURCE
    LDFLAGS  = -lm -Wl,-Bstatic -lpthread -Wl,-Bdynamic -lws2_32 -liphlpapi
    TARGET   = spdchk.exe
    SRCS     = $(BASE_SRCS) icmp_win.c logger_win.c terminal_win.c win_main.c
    WIN_RES  = spdchk_res.o
else
    CFLAGS  += -D_DEFAULT_SOURCE
    LDFLAGS  = -lm -lpthread
    TARGET   = spdchk
    SRCS     = $(BASE_SRCS) icmp.c logger.c main.c
    WIN_RES  =
endif

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS) $(WIN_RES)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

spdchk_res.o: spdchk.rc spdchk.manifest
	windres spdchk.rc -o spdchk_res.o

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(WIN_RES) $(TARGET) spdchk spdchk.exe tests/*_t.o spdchk_test

# -----------------------------------------------------------------------
# Unit tests (Linux only — requires fmemopen / POSIX sockets / syslog)
#
# Each production source that needs socket mocking is recompiled in a
# separate tests/ object with -DTEST_MODE and -include tests/mock_sockets.h
# so the mock macros redirect socket calls to the stub implementations in
# tests/mock_sockets.c without touching the release objects.
# -----------------------------------------------------------------------
ifneq ($(OS),Windows_NT)

TEST_CFLAGS  = $(CFLAGS) -DTEST_MODE -I./tests
TEST_OBJ_DIR = tests

TEST_OBJS = \
	$(TEST_OBJ_DIR)/metrics_t.o   \
	$(TEST_OBJ_DIR)/icmp_t.o      \
	$(TEST_OBJ_DIR)/client_t.o    \
	$(TEST_OBJ_DIR)/logger_t.o    \
	$(TEST_OBJ_DIR)/telemetry_t.o \
	$(TEST_OBJ_DIR)/udp_t.o

$(TEST_OBJ_DIR):
	mkdir -p $@

$(TEST_OBJ_DIR)/metrics_t.o: metrics.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/icmp_t.o: icmp.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -include tests/mock_sockets.h -c -o $@ $<

$(TEST_OBJ_DIR)/client_t.o: client.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -include tests/mock_sockets.h -c -o $@ $<

$(TEST_OBJ_DIR)/logger_t.o: logger.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/telemetry_t.o: telemetry.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -c -o $@ $<

$(TEST_OBJ_DIR)/udp_t.o: udp.c | $(TEST_OBJ_DIR)
	$(CC) $(TEST_CFLAGS) -include tests/mock_sockets.h -c -o $@ $<

.PHONY: test
test: $(TEST_OBJS)
	$(CC) $(TEST_CFLAGS) \
	    tests/test_main.c   tests/test_metrics.c \
	    tests/test_protocol.c tests/test_icmp_privs.c \
	    tests/test_udp.c \
	    tests/mock_sockets.c \
	    $(TEST_OBJS) -o spdchk_test -lm -lpthread
	./spdchk_test

endif
