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
#define SPDCHK_VERSION "0.15.4"

#define DEFAULT_PORT       2200
#define DEFAULT_COUNT      4    /* ICMP pings per test (spec §3.2)       */
#define DEFAULT_DURATION   10   /* bandwidth-test duration in seconds     */
#define DEFAULT_STREAMS    4    /* parallel TCP streams for bandwidth test */

#define DEFAULT_UDP_BW     100.0 /* UDP target bit-rate in Mbps            */
#define DEFAULT_PKT_SIZE   1472  /* UDP payload bytes (Ethernet MTU safe)  */

/* test_mode values for client_args / run_client_ex() */
#define TEST_MODE_TCP      0
#define TEST_MODE_UDP      1

/*
 * SPDCHK_UDP_MAGIC — embedded in every UDP datagram to filter rogue
 * traffic arriving on the same port from unrelated sources.
 */
#define SPDCHK_UDP_MAGIC   0x5350434Bu  /* "SPCK" in ASCII                */

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
 *
 * TCP tests: server transmits "SPDCHK_REPORT <bytes> <duration_ms>\n"
 * UDP fields (lost_packets, out_of_order, jitter_us) are zero for TCP
 * tests; they are populated from the server's per-session UDP counters
 * when a UDP test is performed.
 */
typedef struct {
    uint64_t total_bytes_received;  /* bytes the server actually received  */
    uint32_t test_duration_ms;      /* server-observed session duration    */
    uint32_t server_version;        /* packed: (major<<16)|(minor<<8)|patch */
    /* UDP test additions (zero for TCP tests) */
    uint32_t lost_packets;          /* packets_sent - packets_received     */
    uint32_t out_of_order;          /* datagrams arriving below seq max    */
    uint32_t jitter_us;             /* RFC 3550 smoothed jitter, µs        */
} spdchk_report;

/* Greeting sent by the client to request a Phase 3 TCP report from the server. */
#define SPDCHK_REPORT_REQ "SPDCHK_REPORT_REQ\n"

/*
 * UDP control-channel greetings (sent over TCP before/after UDP packets).
 *
 *   SPDCHK_UDP_REQ  <bw_mbps> <pkt_size> <duration_sec>\n
 *     → server replies "OK\n" and marks the source IP as UDP-authorised.
 *
 *   SPDCHK_UDP_DONE <packets_sent>\n
 *     → server replies "SPDCHK_UDP_REPORT <rx> <ooo> <jitter_us> <peak_us>\n"
 */
#define SPDCHK_UDP_REQ_PREFIX  "SPDCHK_UDP_REQ"
#define SPDCHK_UDP_DONE_PREFIX "SPDCHK_UDP_DONE"

/*
 * spdchk_udp_payload — packed UDP measurement frame.
 *
 * The `padding` flexible array member extends the datagram to the
 * configured packet size (default DEFAULT_PKT_SIZE bytes total).
 * Minimum total size is sizeof(spdchk_udp_payload) = 16 bytes.
 */
struct spdchk_udp_payload {
    uint32_t seq_number;    /* monotonic counter — loss detection          */
    uint64_t timestamp_ns;  /* sender CLOCK_MONOTONIC / QPC in nanoseconds */
    uint32_t magic_id;      /* must equal SPDCHK_UDP_MAGIC                 */
    char     padding[];     /* zero-filled — extends frame to pkt_size     */
} __attribute__((packed));

#endif /* SPDCHK_H */
