#ifndef SPDCHK_H
#define SPDCHK_H

#include <stdint.h>
#include <time.h>

#define DEFAULT_PORT     2200
#define DEFAULT_COUNT    4   /* ICMP pings per test (spec §3.2)       */
#define DEFAULT_DURATION 10  /* bandwidth-test duration in seconds     */
#define DEFAULT_STREAMS  4   /* parallel TCP streams for bandwidth test */

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
