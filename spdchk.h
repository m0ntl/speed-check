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
#define SPDCHK_VERSION "0.8.2"

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

#endif /* SPDCHK_H */
