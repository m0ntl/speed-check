# spdchk — Project Guidelines

## Project Overview

**spdchk** is a high-precision network diagnostic utility written in C. It operates in a client-server model and executes a two-stage test sequence:

1. **Reachability phase (ICMP):** The client sends ICMP echo requests to confirm the Layer-3 path is up before committing to a heavier test. If no reply is received within the timeout the tool exits with "Target Unreachable".
2. **Bandwidth phase (TCP):** Upon successful ICMP reply, the client opens multiple parallel TCP streams to the server and pushes data for a configurable duration. Results (throughput, jitter, latency, packet loss) are reported as structured JSON or plain text.

Both modes are compiled into a single binary toggled by CLI flags (`-s` server, `-c <IP>` client). The binary requires `CAP_NET_RAW` (or `sudo`) for the raw ICMP socket.

Current version: **0.5.0** (defined in `spdchk.h`).

### CLI Flags

| Flag | Description |
|---|---|
| `-s` | Start in server mode (passive listener) |
| `-c <IP>` | Start in client mode targeting the given IPv4 address |
| `-p <port>` | Port number (default: 2200) |
| `-i <pings>` | Number of ICMP pings to send (default: 4) |
| `-d <seconds>` | Bandwidth test duration (default: 10) |
| `-n <streams>` | Number of parallel TCP streams (default: 4) |
| `-m <seconds>` | Server max-duration per test; 0 = unlimited |
| `-D` | Enable Dynamic Stream Scaling (DSS) |
| `-w <ms>` | DSS sampling window in milliseconds (default: 500) |
| `-j` | Emit JSON output |
| `-o <file>` | Write statistics to a file instead of stdout |
| `-v` | Increase log verbosity (cumulative: `-v`=INFO, `-vv`=DEBUG, `-vvv`=TRACE) |
| `--log-level N` | Set log verbosity directly (0=ERROR 1=INFO 2=DEBUG 3=TRACE) |

### Key Files

| File | Purpose |
|---|---|
| `spdchk.h` | Shared constants (`DEFAULT_PORT`, `DEFAULT_COUNT`, `DEFAULT_DURATION`, `DEFAULT_STREAMS`, DSS parameters), `SPDCHK_VERSION` macro, and the packed `spdchk_payload` wire struct (seq number, departure timestamp, optional padding) |
| `main.c` | Entry point — full CLI argument parsing (`getopt_long`), input validation, logger initialisation, and dispatch to `run_server()` or `run_client()` |
| `client.h` | Declares `struct client_args` (all test parameters) and `run_client()` |
| `client.c` | Implements the two-phase test: ICMP multi-ping via `icmp_ping()`, version handshake with the server (including DSS capability flag), parallel TCP stream workers (`stream_worker` threads), Dynamic Stream Scaling loop, and final metrics output (plain text or JSON) |
| `server.h` | Declares `run_server(port, max_duration)` |
| `server.c` | Passive bandwidth sink — binds a TCP socket, accepts connections in per-thread handlers, performs the version handshake (accepts or rejects mismatched clients), drains incoming data, and enforces the optional `max_duration` receive timeout |
| `icmp.h` | Declares `struct icmp_stats` (avg latency, packet-loss %) and `icmp_ping()` |
| `icmp.c` | Raw-socket ICMP implementation: builds echo-request packets, computes the Internet checksum, sends `count` pings, receives replies with per-packet RTT timing, and populates `icmp_stats` |
| `metrics.h` | Declares `struct ping_result`, `struct bandwidth_result`, `struct metrics_result`, and the output functions |
| `metrics.c` | Computes and formats results: `print_metrics()` (loss %, min/avg/max RTT, jitter), `print_bandwidth()` (throughput in Gbps), and `print_results_json()` (full JSON report with timestamp, ping stats, and bandwidth stats) |
| `logger.h` | Declares the four-level log system (`ERROR`/`INFO`/`DEBUG`/`TRACE`), `logger_init()`, `logger_close()`, and the `log_error` / `log_info` / `log_debug` / `log_trace` convenience macros |
| `logger.c` | Thread-safe dual-output logger: writes to both syslog (`LOG_DAEMON` facility) and stdout/stderr; TRACE messages are rate-limited to 1 000 calls/s to prevent disk exhaustion |
| `Makefile` | Build rules for the `spdchk` binary |

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

### 5. Finish by Pushing — Do Not Build

- Do **not** run `make`, `gcc`, or any build/compile command.
- After all commits are ready, finish the session by running:
  ```
  git push
  ```
