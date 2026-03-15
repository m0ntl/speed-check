# spdchk — Project Guidelines

## Project Overview

**spdchk** is a high-precision network diagnostic utility written in C. It operates in a client-server model and executes a two-stage test sequence:

1. **Reachability phase (ICMP):** The client sends ICMP echo requests to confirm the Layer-3 path is up before committing to a heavier test. If no reply is received within the timeout the tool exits with "Target Unreachable".
2. **Bandwidth phase (TCP):** Upon successful ICMP reply, the client opens multiple parallel TCP streams to the server and pushes data for a configurable duration. Results (throughput, jitter, latency, packet loss) are reported as structured JSON.

Both modes are compiled into a single binary toggled by CLI flags (`-s` server, `-c <IP>` client). The binary requires `CAP_NET_RAW` (or `sudo`) for the raw ICMP socket.

### Key Files

| File | Purpose |
|---|---|
| `spdchk.h` | Shared constants, version macro, packed wire struct |
| `main.c` | Entry point — argument parsing, mode dispatch |
| `client.c / client.h` | ICMP probe + TCP bandwidth initiator |
| `server.c / server.h` | Passive listener — echos payloads, streams data |
| `icmp.c / icmp.h` | Raw socket helpers, checksum, send/receive |
| `metrics.c / metrics.h` | RTT, jitter, packet-loss, throughput calculations |

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

### 3. Finish by Pushing — Do Not Build

- Do **not** run `make`, `gcc`, or any build/compile command.
- After all commits are ready, finish the session by running:
  ```
  git push
  ```
