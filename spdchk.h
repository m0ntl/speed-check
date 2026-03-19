#ifndef SPDCHK_H
#define SPDCHK_H

#include <stdint.h>
#include <time.h>

/*
 * VERSION — semantic version of this build (https://semver.org).
 *
 * Rules for bumping:
 *   MAJOR (x)  — incompatible change: wire-protocol break, flag removal,
 *                or any change that requires the peer to be updated too.
 *   MINOR (y)  — backward-compatible new feature: new flag, new JSON field,
 *                new server capability that old clients can still ignore.
 *   PATCH (z)  — backward-compatible bug fix, docs, or refactor with no
 *                behaviour change visible to the user or the network peer.
 *
 * The CI/CD pipeline reads this value to name every release automatically.
 * Bump it here before merging the commit that should carry the new version.
 */
#define SPDCHK_VERSION "0.13.3"

#define DEFAULT_PORT       2200
#define DEFAULT_COUNT      4    /* ICMP pings per test (spec §3.2)       */
#define DEFAULT_DURATION   10   /* bandwidth-test duration in seconds     */
#define DEFAULT_STREAMS    4    /* parallel TCP streams for bandwidth test */

/* Dynamic Stream Scaling (DSS) parameters */
#define DSS_WINDOW_MS      500  /* sampling window in milliseconds        */
#define DSS_THRESHOLD      0.05 /* minimum Δ to continue scaling (5 %)   */
#define DSS_MAX_STREAMS    32   /* hard cap — prevents socket exhaustion  */

/*
 * spdchk_payload — packed measurement frame sent over TCP.
 * Packing ensures identical wire layout between hosts.
 */
struct spdchk_payload {
    uint32_t        seq_num;    /* Sequence number for ordering / loss detection */
    struct timespec ts;         /* Departure timestamp (CLOCK_MONOTONIC)         */
    char            padding[32];/* Optional padding for MTU probing              */
} __attribute__((packed));

/*
 * spdchk_report — Phase 3 end-of-test report returned by the server.
 * The server transmits a text line: "SPDCHK_REPORT <bytes> <duration_ms>\n"
 * populated from the session byte accumulator.
 */
typedef struct {
    uint64_t total_bytes_received;  /* bytes the server actually received  */
    uint32_t test_duration_ms;      /* server-observed session duration    */
    uint32_t server_version;        /* packed: (major<<16)|(minor<<8)|patch */
} spdchk_report;

/* Greeting sent by the client to request a Phase 3 report from the server. */
#define SPDCHK_REPORT_REQ "SPDCHK_REPORT_REQ\n"

#endif /* SPDCHK_H */
