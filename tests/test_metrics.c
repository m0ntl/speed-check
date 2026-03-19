/*
 * test_metrics.c — Unit tests for metrics.c (§3.1 of the test SDD).
 *
 * Tests covered:
 *   - Gbps / Mbps threshold scaling in print_bandwidth()
 *   - Three stream-count format strings (static / DSS / DSS with probe)
 *   - Zero-duration does not crash print_bandwidth()
 *   - print_metrics() edge cases: zero packets, all lost
 *   - print_results_json() field presence and dss_probed_streams gating
 *
 * None of these functions use sockets, so no mock layer is needed.
 * fmemopen is used to capture stdio output into a fixed buffer.
 * The buffer is pre-zeroed before each capture so it is always
 * null-terminated even if fmemopen omits the trailing null on close.
 */

#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "../metrics.h"

/* ------------------------------------------------------------------ */
/* Output-capture helpers                                              */
/* ------------------------------------------------------------------ */

static const char *capture_bandwidth(const struct bandwidth_result *bw)
{
    static char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return buf;
    print_bandwidth(f, bw);
    fclose(f);
    return buf;
}

static const char *capture_metrics(const struct metrics_result *r)
{
    static char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return buf;
    print_metrics(f, r);
    fclose(f);
    return buf;
}

static const char *capture_json(const struct ping_result *ping,
                                 const struct bandwidth_result *bw)
{
    static char buf[1024];
    memset(buf, 0, sizeof(buf));
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return buf;
    print_results_json(f, ping, bw);
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* 3.1 Gbps / Mbps scaling                                             */
/* ------------------------------------------------------------------ */

static void test_throughput_gbps(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 9.753,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 0,
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Gbps");
    ASSERT_NOT_CONTAINS(out, "Mbps");
}

static void test_throughput_mbps(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 0.450,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 0,
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Mbps");
    ASSERT_NOT_CONTAINS(out, "Gbps");
}

/* ------------------------------------------------------------------ */
/* 3.1 DSS string logic                                                */
/* ------------------------------------------------------------------ */

static void test_streams_static(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 1.0,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 0,   /* static mode: no DSS */
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Streams    : 4\n");
    ASSERT_NOT_CONTAINS(out, "DSS");
}

static void test_streams_dss_no_extra_probe(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 1.5,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 4,   /* DSS converged without an extra probe */
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "4 (DSS)");
}

static void test_streams_dss_with_extra_probe(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 1.5,
        .duration_sec     = 10,
        .parallel_streams = 3,   /* optimal */
        .optimal_streams  = 5,   /* total probed before plateau */
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "3 optimal (of 5 probed, DSS)");
}

/* ------------------------------------------------------------------ */
/* 3.1 Zero-duration: print_bandwidth must not crash when duration = 0 */
/* ------------------------------------------------------------------ */

static void test_zero_duration_no_crash(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 0.0,
        .duration_sec     = 0,
        .parallel_streams = 4,
        .optimal_streams  = 0,
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Duration   : 0 s");
}

/* ------------------------------------------------------------------ */
/* print_metrics edge cases                                            */
/* ------------------------------------------------------------------ */

static void test_metrics_no_packets(void)
{
    struct metrics_result r = { .count = 0, .received = 0, .rtts = NULL };
    const char *out = capture_metrics(&r);
    ASSERT_CONTAINS(out, "No packets were sent.");
}

static void test_metrics_all_lost(void)
{
    double rtts[] = { -1.0, -1.0, -1.0, -1.0 };
    struct metrics_result r = { .count = 4, .received = 0, .rtts = rtts };
    const char *out = capture_metrics(&r);
    ASSERT_CONTAINS(out, "100.0% loss");
}

/* ------------------------------------------------------------------ */
/* print_results_json                                                  */
/* ------------------------------------------------------------------ */

static void test_json_formatting(void)
{
    struct ping_result ping = { .avg_latency_ms = 12.5, .packet_loss_pct = 0.0 };
    struct bandwidth_result bw = {
        .throughput_gbps  = 1.234,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 0,   /* static mode: no dss_probed_streams field */
        .is_verified      = 0,
    };
    const char *out = capture_json(&ping, &bw);
    ASSERT_CONTAINS(out, "\"timestamp\"");
    ASSERT_CONTAINS(out, "\"ping_stats\"");
    ASSERT_CONTAINS(out, "\"bandwidth_stats\"");
    ASSERT_CONTAINS(out, "\"avg_latency_ms\"");
    ASSERT_CONTAINS(out, "\"packet_loss_pct\"");
    ASSERT_CONTAINS(out, "\"throughput_gbps\"");
    ASSERT_CONTAINS(out, "\"verified\"");
    ASSERT_NOT_CONTAINS(out, "dss_probed_streams");
    ASSERT_NOT_CONTAINS(out, "\"reliability_score\"");
}

static void test_json_dss_probed_streams_field(void)
{
    struct ping_result ping = { .avg_latency_ms = 5.0, .packet_loss_pct = 0.0 };
    struct bandwidth_result bw = {
        .throughput_gbps  = 2.0,
        .duration_sec     = 10,
        .parallel_streams = 3,
        .optimal_streams  = 6,   /* probe ran: field must appear */
        .is_verified      = 0,
    };
    const char *out = capture_json(&ping, &bw);
    ASSERT_CONTAINS(out, "\"dss_probed_streams\"");
}

/* ------------------------------------------------------------------ */
/* Verified / Unverified display                                        */
/* ------------------------------------------------------------------ */

static void test_bandwidth_verified(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 1.0,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 0,
        .bytes_sent       = 1000000000ULL,
        .bytes_received   = 982000000ULL,
        .reliability_score = 98.2,
        .is_verified      = 1,
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Verified");
    ASSERT_CONTAINS(out, "Reliability");
    ASSERT_CONTAINS(out, "98.2%");
    ASSERT_CONTAINS(out, "Stable");
}

static void test_bandwidth_unverified(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps  = 1.0,
        .duration_sec     = 10,
        .parallel_streams = 4,
        .optimal_streams  = 0,
        .is_verified      = 0,
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Estimated");
    ASSERT_NOT_CONTAINS(out, "Reliability");
}

/* ------------------------------------------------------------------ */
/* Reliability rating thresholds                                        */
/* ------------------------------------------------------------------ */

static void test_reliability_optimal(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps   = 1.0,
        .duration_sec      = 10,
        .parallel_streams  = 4,
        .reliability_score = 100.0,
        .is_verified       = 1,
    };
    ASSERT_CONTAINS(capture_bandwidth(&bw), "Optimal");
}

static void test_reliability_stable(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps   = 1.0,
        .duration_sec      = 10,
        .parallel_streams  = 4,
        .reliability_score = 95.0,
        .is_verified       = 1,
    };
    ASSERT_CONTAINS(capture_bandwidth(&bw), "Stable");
}

static void test_reliability_degraded(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps   = 1.0,
        .duration_sec      = 10,
        .parallel_streams  = 4,
        .reliability_score = 92.5,
        .is_verified       = 1,
    };
    ASSERT_CONTAINS(capture_bandwidth(&bw), "Degraded");
}

static void test_reliability_unstable(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps   = 1.0,
        .duration_sec      = 10,
        .parallel_streams  = 4,
        .reliability_score = 85.0,
        .is_verified       = 1,
    };
    ASSERT_CONTAINS(capture_bandwidth(&bw), "Unstable");
}

/* DSS probe-stream inflation: bytes_sent counts only optimal_n streams but
 * the server accumulates all streams, so raw ratio can exceed 100.  The
 * client clamps reliability_score to 100.0 before populating the struct.
 * Verify that a clamped value of 100.0 renders as "Optimal", not ">100". */
static void test_reliability_dss_clamped(void)
{
    struct bandwidth_result bw = {
        .throughput_gbps   = 1.0,
        .duration_sec      = 10,
        .parallel_streams  = 4,
        .optimal_streams   = 5,
        .reliability_score = 100.0,  /* clamped from e.g. 108.5 */
        .is_verified       = 1,
    };
    const char *out = capture_bandwidth(&bw);
    ASSERT_CONTAINS(out, "Optimal");
    ASSERT_NOT_CONTAINS(out, "108");
}

/* JSON verified fields appear only when is_verified = 1 */
static void test_json_verified_fields(void)
{
    struct ping_result ping = { .avg_latency_ms = 5.0, .packet_loss_pct = 0.0 };
    struct bandwidth_result bw = {
        .throughput_gbps   = 1.0,
        .duration_sec      = 10,
        .parallel_streams  = 4,
        .bytes_sent        = 100000000ULL,
        .bytes_received    = 98200000ULL,
        .reliability_score = 98.2,
        .is_verified       = 1,
    };
    const char *out = capture_json(&ping, &bw);
    ASSERT_CONTAINS(out, "\"verified\"");
    ASSERT_CONTAINS(out, "\"reliability_score\"");
    ASSERT_CONTAINS(out, "\"bytes_received\"");
}

/* ------------------------------------------------------------------ */
/* Suite runner                                                         */
/* ------------------------------------------------------------------ */

void run_metrics_tests(void)
{
    run_test("throughput_gbps_display",       test_throughput_gbps);
    run_test("throughput_mbps_display",       test_throughput_mbps);
    run_test("streams_static_format",         test_streams_static);
    run_test("streams_dss_no_extra_probe",    test_streams_dss_no_extra_probe);
    run_test("streams_dss_with_extra_probe",  test_streams_dss_with_extra_probe);
    run_test("zero_duration_no_crash",        test_zero_duration_no_crash);
    run_test("metrics_no_packets",            test_metrics_no_packets);
    run_test("metrics_all_lost",              test_metrics_all_lost);
    run_test("json_formatting",               test_json_formatting);
    run_test("json_dss_probed_streams_field", test_json_dss_probed_streams_field);
    run_test("bandwidth_verified_display",    test_bandwidth_verified);
    run_test("bandwidth_unverified_display",  test_bandwidth_unverified);
    run_test("reliability_optimal",           test_reliability_optimal);
    run_test("reliability_stable",            test_reliability_stable);
    run_test("reliability_degraded",          test_reliability_degraded);
    run_test("reliability_unstable",          test_reliability_unstable);
    run_test("reliability_dss_clamped",       test_reliability_dss_clamped);
    run_test("json_verified_fields",          test_json_verified_fields);
}
