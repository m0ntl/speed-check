/*
 * test_udp.c — Unit tests for UDP metrics output (print_udp_metrics).
 *
 * No socket mocking is required because print_udp_metrics() is a pure
 * formatting function that takes a pre-populated udp_result struct and
 * writes to a FILE stream.  fmemopen() captures the output into a fixed
 * buffer for inspection.
 */

#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "../udp.h"
#include "../metrics.h"

/* ------------------------------------------------------------------ */
/* Output-capture helper                                               */
/* ------------------------------------------------------------------ */

static const char *capture_udp(const struct udp_result *r)
{
    static char buf[512];
    memset(buf, 0, sizeof(buf));
    FILE *f = fmemopen(buf, sizeof(buf), "w");
    if (!f) return buf;
    print_udp_metrics(f, r);
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* 4.1 Packet counts and loss                                          */
/* ------------------------------------------------------------------ */

static void test_udp_no_loss(void)
{
    struct udp_result r = {
        .packets_sent     = 1000,
        .packets_received = 1000,
        .lost_packets     = 0,
        .out_of_order     = 0,
        .jitter_us        = 50.0,
        .peak_jitter_us   = 120.0,
        .target_bw_mbps   = 100.0,
        .achieved_bw_mbps = 98.5,
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "1000 sent");
    ASSERT_CONTAINS(out, "1000 received");
    ASSERT_CONTAINS(out, "0 (0.0%)");
}

static void test_udp_with_loss(void)
{
    struct udp_result r = {
        .packets_sent     = 500,
        .packets_received = 450,
        .lost_packets     = 50,
        .out_of_order     = 3,
        .jitter_us        = 200.0,
        .peak_jitter_us   = 800.0,
        .target_bw_mbps   = 50.0,
        .achieved_bw_mbps = 49.9,
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "500 sent");
    ASSERT_CONTAINS(out, "450 received");
    ASSERT_CONTAINS(out, "50 (10.0%)");
}

/* ------------------------------------------------------------------ */
/* 4.2 Jitter display (microseconds → milliseconds conversion)         */
/* ------------------------------------------------------------------ */

static void test_udp_jitter_display(void)
{
    struct udp_result r = {
        .packets_sent     = 100,
        .packets_received = 100,
        .lost_packets     = 0,
        .jitter_us        = 1500.0,   /* 1.500 ms */
        .peak_jitter_us   = 3250.0,   /* 3.250 ms */
        .target_bw_mbps   = 10.0,
        .achieved_bw_mbps = 9.8,
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "1.500 ms");   /* avg jitter */
    ASSERT_CONTAINS(out, "3.250 ms");   /* peak jitter */
}

/* ------------------------------------------------------------------ */
/* 4.3 Capacity: Mbps display                                          */
/* ------------------------------------------------------------------ */

static void test_udp_capacity_mbps(void)
{
    struct udp_result r = {
        .packets_sent     = 200,
        .packets_received = 200,
        .target_bw_mbps   = 100.0,
        .achieved_bw_mbps = 97.3,
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "97.3 Mbps");
    ASSERT_CONTAINS(out, "100.0 Mbps");
    ASSERT_NOT_CONTAINS(out, "Gbps");
}

/* ------------------------------------------------------------------ */
/* 4.3 Capacity: Gbps display (target >= 1000 Mbps)                    */
/* ------------------------------------------------------------------ */

static void test_udp_capacity_gbps(void)
{
    struct udp_result r = {
        .packets_sent     = 10000,
        .packets_received = 9998,
        .lost_packets     = 2,
        .target_bw_mbps   = 10000.0,   /* 10 Gbps */
        .achieved_bw_mbps = 9750.0,    /* 9.75 Gbps */
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "Gbps");
    ASSERT_NOT_CONTAINS(out, "Mbps achieved");
}

/* ------------------------------------------------------------------ */
/* 4.4 Out-of-order line only shown when non-zero                       */
/* ------------------------------------------------------------------ */

static void test_udp_no_ooo_line_when_zero(void)
{
    struct udp_result r = {
        .packets_sent     = 100,
        .packets_received = 100,
        .out_of_order     = 0,
        .target_bw_mbps   = 10.0,
        .achieved_bw_mbps = 9.9,
    };
    const char *out = capture_udp(&r);
    ASSERT_NOT_CONTAINS(out, "Out-order");
}

static void test_udp_ooo_line_shown_when_nonzero(void)
{
    struct udp_result r = {
        .packets_sent     = 100,
        .packets_received = 95,
        .out_of_order     = 7,
        .target_bw_mbps   = 10.0,
        .achieved_bw_mbps = 9.5,
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "Out-order");
    ASSERT_CONTAINS(out, "7");
}

/* ------------------------------------------------------------------ */
/* 4.5 Zero packets sent — no divide-by-zero crash                     */
/* ------------------------------------------------------------------ */

static void test_udp_zero_sent(void)
{
    struct udp_result r = {0};   /* all zeros */
    /* Must not crash — loss % should not be computed from zero denominator */
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "0 sent");
}

/* ------------------------------------------------------------------ */
/* Suite runner (called from test_main.c)                              */
/* ------------------------------------------------------------------ */

void run_udp_tests(void)
{
    run_test("udp: no packet loss",           test_udp_no_loss);
    run_test("udp: packet loss display",      test_udp_with_loss);
    run_test("udp: jitter µs→ms conversion",  test_udp_jitter_display);
    run_test("udp: capacity Mbps display",    test_udp_capacity_mbps);
    run_test("udp: capacity Gbps display",    test_udp_capacity_gbps);
    run_test("udp: out-of-order line absent", test_udp_no_ooo_line_when_zero);
    run_test("udp: out-of-order line present",test_udp_ooo_line_shown_when_nonzero);
    run_test("udp: zero sent no crash",       test_udp_zero_sent);
}
