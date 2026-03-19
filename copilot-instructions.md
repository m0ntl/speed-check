# spdchk — Project Guidelines

## Project Overview

**spdchk** is a high-precision network diagnostic utility written in C. It operates in a client-server model and executes a two-stage test sequence:

1. **Reachability phase (ICMP):** The client sends ICMP echo requests to confirm the Layer-3 path is up before committing to a heavier test. If no reply is received within the timeout the tool exits with "Target Unreachable".
2. **Bandwidth phase (TCP):** Upon successful ICMP reply, the client opens multiple parallel TCP streams to the server and pushes data for a configurable duration. Results (throughput, jitter, latency, packet loss) are reported as structured JSON or plain text.

The binary is a single interactive TUI application. All parameters (mode, target IP, port, streams, etc.) are configured inside the TUI. The only CLI flags accepted at launch are `-v` / `--log-level` for log verbosity. On Linux the binary requires `CAP_NET_RAW` (or `sudo`) for the raw ICMP socket; on Windows no elevated privileges are needed (ICMP uses the `IcmpSendEcho` API).

Current version: **0.12.1** (defined in `spdchk.h`).

### CLI Flags

All parameters are configured inside the interactive TUI. The only flags accepted at invocation are:

| Flag | Description |
|---|---|
| `-v` | Increase log verbosity (cumulative: `-v`=INFO, `-vv`=DEBUG, `-vvv`=TRACE) |
| `--log-level N` | Set log verbosity directly (0=ERROR 1=INFO 2=DEBUG 3=TRACE) |

### Interactive TUI Settings

The Settings screen adapts to the current mode.

**Client mode settings:**

| Setting | Description |
|---|---|
| Mode | Toggle between Client and Server |
| Target IP | IPv4 address of the spdchk server |
| Port | TCP/ICMP port (default: 2200) |
| ICMP Ping Count | Number of pings per reachability test (default: 4) |
| TCP Duration | Bandwidth test duration in seconds (default: 10) |
| TCP Streams | Parallel TCP streams (default: 4) |
| DSS Mode | Enable/disable Dynamic Stream Scaling (default: On) |
| DSS Window | DSS sampling window in milliseconds (default: 500) |
| JSON Output | Emit JSON instead of plain text (default: Off) |
| Output File | Write results to a file instead of stdout (blank = stdout) |

**Server mode settings:**

| Setting | Description |
|---|---|
| Mode | Toggle between Server and Client |
| Port | TCP port to listen on (default: 2200) |
| Max Duration | Per-test receive timeout in seconds; 0 = unlimited |

### Key Files

| File | Purpose |
|---|---|
| `spdchk.h` | Shared constants (`DEFAULT_PORT`, `DEFAULT_COUNT`, `DEFAULT_DURATION`, `DEFAULT_STREAMS`, DSS parameters), `SPDCHK_VERSION` macro, and the packed `spdchk_payload` wire struct (seq number, departure timestamp, optional padding) |
| `main.c` | Entry point — minimal CLI parsing (only `-v` / `--log-level`), logger initialisation, then unconditional dispatch to `interactive_main()` |
| `client.h` | Declares `struct client_args` (all test parameters, including `skip_version_check` to suppress Phase 0 when the handshake has already been performed), `struct run_client_result` (programmatic result data), `client_check_server_version()`, `run_client()`, and `run_client_ex()`; `run_client()` is no longer called directly — all client tests go through `run_client_ex()` from interactive mode; `run_client_ex()` returns `0` on success, `-1` on network/allocation error, `-2` when the ICMP phase fails due to insufficient privileges (`EPERM`/`EACCES`), `-3` when Phase 0 reports a version mismatch between client and server |
| `client.c` | Implements the two-phase test: version handshake with the server via `client_check_server_version()` (including DSS capability flag; skipped when `args->skip_version_check` is set), parallel TCP stream workers (`stream_worker` threads), Dynamic Stream Scaling loop (enabled by default; disabled when `dss_mode = 0`), and final metrics output (plain text or JSON); the ICMP reachability phase has been removed from this path — TCP bandwidth tests run directly after the version check; DSS throughput is computed from exactly `optimal_n` streams — extra probe streams that triggered plateau detection are joined but excluded from the byte total so the reported metric is consistent with the displayed stream count; `run_client_ex()` additionally populates a `run_client_result` struct for callers that need the data programmatically; `client_check_server_version()` returns `0` on success, `-1` on connection/timeout/unexpected-response error, `-2` when the server explicitly sends `ERR VERSION_MISMATCH`; `run_client_ex()` maps that `-2` to `-3` so the three Phase 0 failure modes remain distinguishable at the interactive layer; uses `compat_win.h` and `#ifdef _WIN32` guards for Winsock2 socket I/O, `ioctlsocket`+`WSAPoll` non-blocking connect, DWORD `SO_RCVTIMEO`, and `sock_close()` |
| `server.h` | Declares `run_server(port, max_duration)` |
| `server.c` | Passive bandwidth sink — binds a TCP socket, accepts connections in per-thread handlers, performs the version handshake (accepts or rejects mismatched clients), drains incoming data, and enforces the optional `max_duration` receive timeout; uses `compat_win.h` and `#ifdef _WIN32` guards for Winsock2 socket I/O, DWORD `SO_RCVTIMEO`, `WSAGetLastError` recv-timeout detection, `SIGPIPE` guard, and `sock_close()` |
| `icmp.h` | Declares `struct icmp_stats` (avg latency, packet-loss %) and `icmp_ping()` |
| `icmp.c` | Raw-socket ICMP implementation: builds echo-request packets, computes the Internet checksum, sends `count` pings, receives replies with per-packet RTT timing, and populates `icmp_stats`; returns `0` on success, `-1` on all-pings-lost or non-permission socket error, `-2` when the raw socket cannot be created due to insufficient privileges (`EPERM`/`EACCES`) |
| `metrics.h` | Declares `struct ping_result`, `struct bandwidth_result` (`parallel_streams` = effective stream count used for throughput; `optimal_streams` = total streams DSS probed, or 0 when no extra probe ran / static mode), `struct metrics_result`, and the output functions |
| `metrics.c` | Computes and formats results: `print_metrics()` (loss %, min/avg/max RTT, jitter), `print_bandwidth()` (throughput in Gbps; three stream-count formats: plain `N` for static, `N (DSS)` when no extra probe, `N optimal (of M probed, DSS)` when a plateau probe ran), and `print_results_json()` (full JSON report with timestamp, ping stats, and bandwidth stats; DSS probe count emitted as `dss_probed_streams` when non-zero) |
| `logger.h` | Declares the four-level log system (`ERROR`/`INFO`/`DEBUG`/`TRACE`), `logger_init()`, `logger_close()`, and the `log_error` / `log_info` / `log_debug` / `log_trace` convenience macros; `__attribute__((format))` guarded behind `#ifdef __GNUC__` for MSVC compatibility |
| `logger.c` | Thread-safe dual-output logger: writes to both syslog (`LOG_DAEMON` facility) and stdout/stderr; TRACE messages are rate-limited to 1 000 calls/s to prevent disk exhaustion |
| `interactive.h` | Declares `interactive_main(void)` — the sole entry point for the binary (all implementation types are internal to `interactive.c`) |
| `interactive.c` | The entire spdchk UI — `termios` raw mode on Linux / `SetConsoleMode` on Windows (via `terminal_win.h`) and ANSI escape codes (no ncurses). Implements a state machine (`AppState` enum: MAIN_MENU, RUNNING_TEST, RUNNING_SERVER, VIEW_RESULTS, VIEW_HISTORY, SETTINGS, EXIT). The main menu adapts to the current mode: **Client mode** offers Run Reachability (ICMP), Run Bandwidth (TCP), Start Server, View Session History, Settings, Exit; **Server mode** offers Start Server (blocking `run_server()` call with the terminal restored during the call), Settings, Exit. The Settings screen exposes all parameters for both modes (see Interactive TUI Settings table). The version handshake (`skip_version_check`) is managed per-session: `version_checked` is set to 1 after the first successful TCP run and reset to 0 when Target IP changes. **If a test is launched while Target IP is blank**, the user is prompted to enter the address inline (canonical mode restored temporarily via `read_str_field`); the entered value is saved as the new Target IP and used for all subsequent runs until changed in Settings; launching is silently aborted if the field remains empty. Arrow-key navigation: on Linux via 3-byte CSI escape-sequence parsing (ESC timeout uses `tcsetattr VTIME`); on Windows `capture_input()` delegates to `win_read_key()` which uses `ReadConsoleInputA()` with virtual key codes — no VT parsing required. Inverted highlight on the selected item, `read_int_field` / `read_str_field` temporarily restore canonical mode for editing. `A_DIM` is overridden to `\033[90m` (dark-gray) on Windows because `\033[2m` renders near-invisible on dark console themes. Session history view split into **ICMP Results** and **TCP / Bandwidth Results** tables; changed columns printed in yellow. `last_bw_failed` stores the raw `run_client_ex` return code (`0` = success, `-1` = generic failure, `-2` = privilege error, `-3` = version mismatch); both result screens show a dedicated "Insufficient privileges" message when the code is `-2`; the Bandwidth Results screen shows "Version mismatch — client and server must run the same version (x.y.z)." when the code is `-3`. On Windows, when starting the server (`STATE_RUNNING_SERVER`), a fixed `netsh advfirewall firewall add rule` hint is always printed (no registry reads) so the user knows how to open the inbound TCP port if needed. |
| `telemetry.h` | Declares `spdchk_telemetry_t` (shared state: `total_duration`, `parallel_streams`, `avg_latency_ms`, `stop`, atomic `total_bytes`), `TELEMETRY_REFRESH_MS` (200 ms / 5 Hz), `TELEMETRY_BAR_WIDTH` (30 chars), `telemetry_start()`, and `telemetry_stop()` |
| `telemetry.c` | Live progress display for the Bandwidth Phase: a pthread wakes every 200 ms, reads atomic byte counters from `stream_worker` threads, and redraws a 5-line ASCII block in-place using ANSI cursor-up escape codes; terminal width obtained via `ioctl TIOCGWINSZ` on Linux and `GetConsoleScreenBufferInfo` on Windows; `isatty()` guard disables rendering on non-interactive output; installs a SIGINT forwarder that emits clean newlines and delegates to the previously-registered handler |
| `spdchk.manifest` | Windows application manifest embedded in `spdchk.exe` via `spdchk.rc`; declares `requestedExecutionLevel=asInvoker` (no UAC elevation prompt), OS compatibility entries for Windows 7 through 11, and the assembly identity; reduces Windows Defender ML heuristic score |
| `spdchk.rc` | Windows resource script compiled by `windres`; embeds `spdchk.manifest` as `RT_MANIFEST` and a `VERSIONINFO` block (company name, file description, version strings, original filename) so Explorer and Defender see proper binary metadata |
| `Makefile` | Platform-conditional build rules: Linux selects `icmp.c`, `logger.c`, `main.c` with `-lpthread -lm`; Windows selects `icmp_win.c`, `logger_win.c`, `terminal_win.c`, `win_main.c` with `-lws2_32 -liphlpapi`; on Windows `windres spdchk.rc` compiles `spdchk_res.o` (manifest + version info) and links it into the executable; Linux-only `make test` target compiles each production source under test as a separate `tests/*_t.o` object with `-DTEST_MODE -include tests/mock_sockets.h`, links them with the test sources and `tests/mock_sockets.c`, and runs `./spdchk_test` |
| `tests/mock_sockets.h` | Socket-level mock layer for `TEST_MODE` builds; force-included via `-include` when compiling `icmp_t.o` and `client_t.o`; pulls in the real system headers first so their declarations are intact, then—under `#ifdef TEST_MODE`—replaces `socket`, `connect`, `fcntl`, `setsockopt`, `send`, `recv`, `sendto`, `recvfrom`, and `close` with controllable stub macros; also declares the global control variables (`mock_socket_return`, `mock_recv_buf`, etc.) and `mock_reset()` |
| `tests/mock_sockets.c` | Definitions of all mock control variables and stub function implementations; `mock_recv()` replays bytes from `mock_recv_buf` one-by-one to simulate server responses; `mock_recvfrom()` returns `EAGAIN` to simulate ICMP timeouts; `mock_reset()` restores defaults between tests |
| `tests/harness.h` | Minimal test harness: `ASSERT_EQ_INT`, `ASSERT_CONTAINS`, `ASSERT_NOT_CONTAINS` macros that call `test_fail()` and return on failure; `run_test()` and `test_fail()` are implemented in `test_main.c` |
| `tests/test_main.c` | Entry point for `spdchk_test`; calls `logger_init(LOG_LEVEL_ERROR)` to suppress log noise, invokes the three suite runners, and exits with a non-zero code if any assertion failed |
| `tests/test_metrics.c` | Unit tests for `metrics.c`: Gbps/Mbps threshold scaling, all three DSS stream-count format strings, zero-duration no-crash, `print_metrics` edge cases (zero packets, all lost), `print_results_json` field presence and `dss_probed_streams` gating |
| `tests/test_protocol.c` | Unit tests for `client_check_server_version()`: version match → 0; mismatch → -2; corrupt payload → -1; socket/connect failure → -1; empty response → -1; DSS greeting token present/absent |
| `tests/test_icmp_privs.c` | Unit tests for `icmp_ping()` privilege-error handling: `EPERM` → -2; `EACCES` → -2; any other socket error → -1 |
| `compat_win.h` | Platform Abstraction Layer header — included by `client.c` and `server.c` on all platforms; on Windows provides Winsock2 includes, `sock_close()`, `MSG_NOSIGNAL=0`, `usleep`, `strncasecmp`, `ssize_t`, and `wsa_strerror()`; on Linux provides only the `sock_close()` → `close()` shim |
| `icmp_win.c` | Windows ICMP implementation replacing `icmp.c`; uses the `IcmpCreateFile` / `IcmpSendEcho` / `IcmpCloseHandle` API from `iphlpapi` — no raw socket is opened, so no Administrator privilege is required; RTT timing via `QueryPerformanceCounter`; `ERROR_ACCESS_DENIED` on `IcmpCreateFile` maps to return code `-2` (Insufficient Privileges, practically never fires on a normal Windows install) |
| `terminal_win.h` | Declares `win_init_console()`, `win_set_raw_mode()`, `win_restore_mode()`, `win_raw_mode_active()`, `win_get_terminal_width()`, and `win_read_key()`; also defines the `KEY_UP` / `KEY_DOWN` / `KEY_ENTER` / `KEY_ESC` / `KEY_QUIT` constants shared with `interactive.c` |
| `terminal_win.c` | Win32 console raw-mode implementation: `SetConsoleMode` replaces `tcsetattr`; `ENABLE_VIRTUAL_TERMINAL_PROCESSING` enables ANSI output; `GetConsoleScreenBufferInfo` replaces `ioctl TIOCGWINSZ`; `win_read_key()` uses `ReadConsoleInputA()` to return one `KEY_*` constant per physical key press, filtering all non-key-down `INPUT_RECORD` types (mouse, resize, focus, key-up) so each selection requires exactly one press; `ENABLE_VIRTUAL_TERMINAL_INPUT` is not set because VT sequence parsing is no longer needed |
| `logger_win.c` | Windows logger replacing `logger.c`; same interface; uses `CRITICAL_SECTION` for thread safety; writes timestamped entries to `spdchk.log` instead of syslog; same 1,000/s TRACE rate-limit |
| `win_main.c` | Windows entry point replacing `main.c`; calls `WSAStartup(2.2)`, calls `SetConsoleOutputCP(CP_UTF8)` to ensure UTF-8 box-drawing characters render correctly, enables VT console via `win_init_console()`, then parses CLI flags identically to `main.c` before calling `interactive_main()`; no Administrator privilege check or warning — all functionality (ICMP via `IcmpSendEcho`, TCP) runs as a standard user |

---

## Workflow Instructions

### 1. Atomic Commits

Every commit must cover exactly one logical change. Before committing:
- Stage only the files that belong to that change (`git add <files>`).
- Write a commit message that follows the Conventional Commits format:
  ```
  <type>(<optional scope>): <short imperative summary>

  <body — what changed and why, not how>
  ```
  Valid types: `feat`, `fix`, `refactor`, `perf`, `docs`, `test`, `chore`.
- Do **not** bundle unrelated changes (e.g., a bug fix and a new feature) in a single commit.

### 2. Version Management

The canonical version lives in `spdchk.h`:

```c
#define SPDCHK_VERSION "x.y.z"
```

Bump the version **in the same commit** as the change that warrants it, using the rules documented in `spdchk.h`:

| Change type | Part to bump | Example trigger |
|---|---|---|
| Wire-protocol break, removed flag, peer must upgrade | **MAJOR** (`x`) | Changed payload struct layout |
| New backward-compatible feature | **MINOR** (`y`) | New `-d` duration flag |
| Bug fix, docs, refactor — no visible behaviour change | **PATCH** (`z`) | Fixed jitter calculation off-by-one |

Always bump the version as the **last commit** in a feature branch before pushing.

### 3. Commit and Push Without Asking

- Never ask for permission to commit or push. Execute `git add`, `git commit`, and `git push` autonomously as soon as the changes are ready.

### 4. Keep copilot-instructions.md Current

- After all file changes in a session are committed, review `copilot-instructions.md` and update it to reflect any new files, removed files, changed behaviour, new flags, or version bumps introduced during that session. Commit the update as a standalone `docs` commit before pushing.

### 5. Add Tests for New Code

- Whenever a new function, module, or behaviour is added, include a corresponding test in the appropriate `tests/test_*.c` file (or a new suite file if the scope warrants it).
- Mock any system-call or socket dependency via `tests/mock_sockets.h` rather than relying on a live network.
- If a new production source file is introduced, add a `tests/*_t.o` compile rule for it in the `Makefile` test target so it is built with `-DTEST_MODE`.
- Commit the test in the **same commit** as the production code it covers.

### 6. Finish by Pushing — Do Not Build

- Do **not** run `make`, `gcc`, or any build/compile command.
- Make sure you increase the version number in `spdchk.h` and in the `description` field of `copilot-instructions.md`.
- After all commits are ready, finish the session by running:
  ```
  git push
  ```
