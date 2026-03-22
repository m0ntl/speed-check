# spdchk — Project Guidelines

## Project Overview

**spdchk** is a high-precision network diagnostic utility written in C. It operates in a client-server model and executes a configurable test sequence:

1. **Reachability phase (ICMP):** The client sends ICMP echo requests to confirm the Layer-3 path is up before committing to a heavier test. If no reply is received within the timeout the tool exits with "Target Unreachable".
2. **Bandwidth phase (TCP):** The client opens multiple parallel TCP streams to the server and pushes data for a configurable duration.
3. **UDP Jitter/Loss phase (UDP, optional):** An alternative to the TCP phase; the client sends a constant-bit-rate UDP stream and the server measures packet loss, out-of-order delivery, and RFC 3550 jitter.

Results (throughput, jitter, latency, packet loss) are reported as structured JSON or plain text.

The binary is a single interactive TUI application. All parameters (mode, target IP, port, streams, etc.) are configured inside the TUI. The only CLI flags accepted at launch are `-v` / `--log-level` for log verbosity. On Linux the binary requires `CAP_NET_RAW` (or `sudo`) for the raw ICMP socket; on Windows no elevated privileges are needed (ICMP uses the `IcmpSendEcho` API).

Current version: **0.14.0** (defined in `spdchk.h`).

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
| Test Type | Toggle: TCP (Bandwidth) / UDP (Jitter & Loss) (default: TCP) |
| UDP Target BW | Desired UDP throughput in Mbps (default: 100) |
| UDP Pkt Size | UDP datagram size in bytes (default: 1472) |

**Server mode settings:**

| Setting | Description |
|---|---|
| Mode | Toggle between Server and Client |
| Port | TCP port to listen on (default: 2200) |
| Max Duration | Per-test receive timeout in seconds; 0 = unlimited |

### Key Files

| File | Purpose |
|---|---|
| `udp.h` | Declares `struct udp_result` (`packets_sent`, `packets_received`, `lost_packets`, `out_of_order`, `jitter_us`, `peak_jitter_us`, `target_bw_mbps`, `achieved_bw_mbps`) and `run_udp_client()` |
| `udp.c` | UDP test client: Phase A TCP negotiation (`SPDCHK_UDP_REQ`), Phase B CBR datagram send loop using `clock_nanosleep`/QPC for sub-millisecond precision, 200 ms pause, Phase C TCP report collection (`SPDCHK_UDP_DONE`); `udp_tcp_connect()` mirrors `connect_timed()` from `client.c` |
| `spdchk.h` | Shared constants (`DEFAULT_PORT`, `DEFAULT_COUNT`, `DEFAULT_DURATION`, `DEFAULT_STREAMS`, `DEFAULT_UDP_BW`, `DEFAULT_PKT_SIZE`, `TEST_MODE_TCP`, `TEST_MODE_UDP`, `SPDCHK_UDP_MAGIC`, DSS parameters), `SPDCHK_VERSION` macro, the packed `spdchk_payload` wire struct (seq number, departure timestamp, optional padding), `spdchk_udp_payload` (seq_number, timestamp_ns, magic_id, flexible padding[]), the `spdchk_report` struct (now includes UDP fields: `lost_packets`, `out_of_order`, `jitter_us`), `SPDCHK_REPORT_REQ`, and the `SPDCHK_UDP_REQ_PREFIX` / `SPDCHK_UDP_DONE_PREFIX` control-channel greeting prefixes |
| `main.c` | Entry point — minimal CLI parsing (only `-v` / `--log-level`), logger initialisation, then unconditional dispatch to `interactive_main()` |
| `client.h` | Declares `struct client_args` (all test parameters, including `skip_version_check`, `test_mode` (`TEST_MODE_TCP`/`TEST_MODE_UDP`), `udp_target_bw`, and `udp_pkt_size`), `struct run_client_result` (programmatic result data including `throughput_gbps`, `reliability_score`, `is_verified`, and `struct udp_result udp`), `client_check_server_version()`, `run_client()`, and `run_client_ex()`; includes `udp.h` |
| `client.c` | Implements the two-phase test plus post-test Phase 3 report: version handshake with the server via `client_check_server_version()` (including DSS capability flag; skipped when `args->skip_version_check` is set), parallel TCP stream workers (`stream_worker` threads), Dynamic Stream Scaling loop (enabled by default; disabled when `dss_mode = 0`), and final metrics output (plain text or JSON); both the DSS path and the static path now set `bw.bytes_sent` (raw sent bytes) and defer `throughput_gbps` computation to after Phase 3; **Phase 3**: after all stream threads are joined, `request_server_report()` opens a short TCP control connection, sends `SPDCHK_REPORT_REQ` (`spdchk.h`), and parses the server's `"SPDCHK_REPORT <bytes> <duration_ms>\n"` response within a 2-second timeout; on success `is_verified=1` and `throughput_gbps` is computed from server-confirmed bytes; on timeout/error the client falls back to local `bytes_sent` with `is_verified=0` ("Estimated"); `bytes_sent` includes bytes from **all** streams (optimal + probe) so its denominator matches what the server accumulated — `reliability_score = (bytes_received/bytes_sent)*100` is therefore always ≤ 100%; throughput is still derived from `bytes_received` (server-confirmed) so the displayed Gbps remains consistent with `optimal_n` stream count; DSS throughput is computed from exactly `optimal_n` streams — extra probe streams that triggered plateau detection are joined but excluded from the byte total so the reported metric is consistent with the displayed stream count; `run_client_ex()` additionally populates a `run_client_result` struct (including `reliability_score` and `is_verified`) for callers that need the data programmatically; `output_path` values are validated before `fopen`: any path containing a `..` component is rejected with an error to prevent directory-traversal writes when the process runs with elevated privileges; uses `compat_win.h` and `#ifdef _WIN32` guards for Winsock2 socket I/O, `ioctlsocket`+`WSAPoll` non-blocking connect, DWORD `SO_RCVTIMEO`, and `sock_close()` |
| `server.h` | Declares `run_server(port, max_duration)` |
| `server.c` | Passive bandwidth sink — binds a TCP socket, accepts connections in per-thread handlers, performs the version handshake, drains incoming data with per-session byte accumulation, and enforces the optional `max_duration` receive timeout; **UDP support**: `run_server()` additionally opens a `SOCK_DGRAM` socket on the same port and starts a `udp_listener_thread` that validates `magic_id`, checks per-IP authorisation, tracks sequence numbers and computes RFC 3550 jitter; `sess_t` extended with UDP fields (`udp_active`, `udp_seq_max`, `udp_received`, `udp_out_of_order`, `udp_jitter_ns`, `udp_peak_jitter_ns`, timestamps); `sess_udp_authorize()` marks a client IP as UDP-authorised after the TCP handshake; TCP handler detects `SPDCHK_UDP_REQ` (parse parameters, authorise IP, reply `OK\n`) and `SPDCHK_UDP_DONE` (read per-IP UDP stats, reply `SPDCHK_UDP_REPORT <rx> <ooo> <jitter_us> <peak_us>\n`) greetings |
| `icmp.h` | Declares `struct icmp_stats` (avg latency, packet-loss %) and `icmp_ping()` |
| `icmp.c` | Raw-socket ICMP implementation: builds echo-request packets, computes the Internet checksum, sends `count` pings, receives replies with per-packet RTT timing, and populates `icmp_stats`; returns `0` on success, `-1` on all-pings-lost or non-permission socket error, `-2` when the raw socket cannot be created due to insufficient privileges (`EPERM`/`EACCES`) |
| `metrics.h` | Declares `struct ping_result`, `struct bandwidth_result`, `struct metrics_result`, and `print_udp_metrics()`; includes `udp.h` for `struct udp_result` |
| `metrics.c` | Computes and formats results: `print_metrics()`, `print_bandwidth()`, `print_results_json()`, and `print_udp_metrics()` (displays Sent/Received counts, Packet Loss %, Out-of-order count, Jitter avg+peak in ms, and Capacity achieved vs. target; Gbps display when target ≥ 1000 Mbps) |
| `logger.h` | Declares the four-level log system (`ERROR`/`INFO`/`DEBUG`/`TRACE`), `logger_init()`, `logger_close()`, and the `log_error` / `log_info` / `log_debug` / `log_trace` convenience macros; `__attribute__((format))` guarded behind `#ifdef __GNUC__` for MSVC compatibility |
| `logger.c` | Thread-safe dual-output logger: writes to both syslog (`LOG_DAEMON` facility) and stdout/stderr; TRACE messages are rate-limited to 1 000 calls/s to prevent disk exhaustion |
| `interactive.h` | Declares `interactive_main(void)` — the sole entry point for the binary (all implementation types are internal to `interactive.c`) |
| `interactive.c` | The entire spdchk UI. `TestType` enum now has `TEST_UDP = 2`. `AppCtx` gains `udp_test_mode` (0=TCP/1=UDP), `udp_target_bw`, and `udp_pkt_size`. Settings screen has 14 client items (3 new: Test Type toggle, UDP Target BW, UDP Pkt Size). Main menu item 1 shows "Run Bandwidth (TCP)" or "Run UDP (Jitter & Loss)" depending on `udp_test_mode`. History view has a **UDP / Jitter & Loss Results** table alongside ICMP and TCP tables. `render_udp_results()` displays the UDP result screen. `TestResult` carries per-run UDP counters (`udp_sent`, `udp_received`, `udp_lost`, `udp_jitter_us`, `udp_target_bw`, `udp_achieved_bw`). All other behaviour unchanged from v0.13.x. |
| `telemetry.h` | Declares `spdchk_telemetry_t` (shared state: `total_duration`, `parallel_streams`, `avg_latency_ms`, `stop`, atomic `total_bytes`), `TELEMETRY_REFRESH_MS` (200 ms / 5 Hz), `TELEMETRY_BAR_WIDTH` (30 chars), `telemetry_start()`, and `telemetry_stop()` |
| `telemetry.c` | Live progress display for the Bandwidth Phase: a pthread wakes every 200 ms, reads atomic byte counters from `stream_worker` threads, and redraws a 5-line ASCII block in-place using ANSI cursor-up escape codes; terminal width obtained via `ioctl TIOCGWINSZ` on Linux and `GetConsoleScreenBufferInfo` on Windows; `isatty()` guard disables rendering on non-interactive output; installs a SIGINT forwarder that emits clean newlines and delegates to the previously-registered handler |
| `spdchk.manifest` | Windows application manifest embedded in `spdchk.exe` via `spdchk.rc`; declares `requestedExecutionLevel=asInvoker` (no UAC elevation prompt), OS compatibility entries for Windows 7 through 11, and the assembly identity; reduces Windows Defender ML heuristic score |
| `spdchk.rc` | Windows resource script compiled by `windres`; embeds `spdchk.manifest` as `RT_MANIFEST` and a `VERSIONINFO` block (company name, file description, version strings, original filename) so Explorer and Defender see proper binary metadata |
| `Makefile` | Platform-conditional build rules; `udp.c` added to `BASE_SRCS` so it is compiled on all platforms; Linux-only `make test` target includes `tests/udp_t.o` (compiled with `-DTEST_MODE -include tests/mock_sockets.h`) and `tests/test_udp.c` |
| `tests/test_udp.c` | Unit tests for `print_udp_metrics()`: no-loss display, loss % computation, µs→ms jitter conversion, Mbps/Gbps capacity threshold, out-of-order line presence/absence, and zero-sent no-crash guard |
| `.github/workflows/release.yml` | GitHub Actions CI/CD pipeline: `test` job runs `make test` on Ubuntu and uploads the report as an artifact; `build-amd64`, `build-arm64`, `build-windows`, `build-windows-arm64` jobs compile the binary for each target and upload it; `release` job (needs all others) downloads all artifacts, creates a GitHub Release tagged `v{SPDCHK_VERSION}` with a changelog derived from `git log`, and attaches the four binaries plus the test report; a pre-release guard step fails the job if the computed tag already exists in the repo, preventing accidental overwrites of published releases |
| `tests/mock_sockets.h` | Socket-level mock layer for `TEST_MODE` builds; force-included via `-include` when compiling `icmp_t.o` and `client_t.o`; pulls in the real system headers first so their declarations are intact, then—under `#ifdef TEST_MODE`—replaces `socket`, `connect`, `fcntl`, `setsockopt`, `send`, `recv`, `sendto`, `recvfrom`, and `close` with controllable stub macros; also declares the global control variables (`mock_socket_return`, `mock_recv_buf`, etc.) and `mock_reset()` |
| `tests/mock_sockets.c` | Definitions of all mock control variables and stub function implementations; `mock_recv()` replays bytes from `mock_recv_buf` one-by-one to simulate server responses; `mock_recvfrom()` returns `EAGAIN` to simulate ICMP timeouts; `mock_reset()` restores defaults between tests |
| `tests/harness.h` | Minimal test harness: `ASSERT_EQ_INT`, `ASSERT_CONTAINS`, `ASSERT_NOT_CONTAINS` macros that call `test_fail()` and return on failure; `run_test()` and `test_fail()` are implemented in `test_main.c` |
| `tests/test_main.c` | Entry point for `spdchk_test`; calls `logger_init(LOG_LEVEL_ERROR)` to suppress log noise, invokes the three suite runners, and exits with a non-zero code if any assertion failed |
| `tests/test_metrics.c` | Unit tests for `metrics.c`: Gbps/Mbps threshold scaling, all three DSS stream-count format strings, zero-duration no-crash, `print_metrics` edge cases (zero packets, all lost), `print_results_json` field presence and `dss_probed_streams` gating, verified vs. unverified throughput display (`(Verified)` / `(Estimated)` tags), all four reliability rating thresholds (Optimal/Stable/Degraded/Unstable), JSON reliability fields (`verified`, `reliability_score`, `bytes_received`) gated on `is_verified`, and `test_reliability_dss_clamped` (verifies that a verified DSS run with `reliability_score=100.0` renders as "Optimal") |
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
