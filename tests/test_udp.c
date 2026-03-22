/*
 * test_udp.c — Unit tests for UDP metrics output (print_udp_metrics)
 *              and UDP preflight query response parsing.
 *
 * print_udp_metrics() tests use fmemopen() to capture output.
 * udp_preflight_query() tests use mock_sockets to stage canned responses.
 */

#include <stdio.h>
#include <string.h>
#include "harness.h"
#include "mock_sockets.h"
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
/* 4.6 Server report unavailable — lost_packets must equal packets_sent */
/*                                                                      */
/* When udp_collect_report() fails, run_udp_client() now sets          */
/* lost_packets = packets_sent so the display is internally consistent: */
/* the count shown on the "Pkt loss" line must match the percentage.   */
/* Previously lost_packets stayed 0 from memset, giving "0 (100.0%)". */
/* ------------------------------------------------------------------ */

static void test_udp_collect_fail_consistent_display(void)
{
    /* Populate the result as run_udp_client does when collect fails:
     * packets_sent and achieved_bw are set; packets_received stays 0;
     * lost_packets is now set to packets_sent instead of being left at 0. */
    struct udp_result r = {
        .packets_sent     = 500,
        .packets_received = 0,
        .lost_packets     = 500,   /* was 0 (inconsistent) before the fix */
        .target_bw_mbps   = 100.0,
        .achieved_bw_mbps = 98.0,
    };
    const char *out = capture_udp(&r);
    ASSERT_CONTAINS(out, "500 sent");
    ASSERT_CONTAINS(out, "0 received");
    /* The loss count and the loss percentage must be consistent. */
    ASSERT_CONTAINS(out, "500 (100.0%)");
}

/* ------------------------------------------------------------------ */
/* 4.7 Preflight query: server confirms probes arrived                 */
/* ------------------------------------------------------------------ */

static void test_udp_preflight_ok(void)
{
    mock_reset();
    const char *resp = "SPDCHK_UDP_CHECK 3\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    int rx = udp_preflight_query("127.0.0.1", 9999);
    ASSERT_EQ_INT(rx, 3);
}

/* ------------------------------------------------------------------ */
/* 4.8 Preflight query: server received zero probes (port blocked)     */
/* ------------------------------------------------------------------ */

static void test_udp_preflight_zero(void)
{
    mock_reset();
    const char *resp = "SPDCHK_UDP_CHECK 0\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    int rx = udp_preflight_query("127.0.0.1", 9999);
    ASSERT_EQ_INT(rx, 0);
}

/* ------------------------------------------------------------------ */
/* 4.9 Preflight query: unrecognised response (old server)             */
/* ------------------------------------------------------------------ */

static void test_udp_preflight_old_server(void)
{
    mock_reset();
    /* An older server without SPDCHK_UDP_CHECK support would fall through
     * to the data drain handler and eventually close the connection.
     * The response would be empty or unrecognised. */
    mock_recv_len = 0;   /* EOF immediately */

    int rx = udp_preflight_query("127.0.0.1", 9999);
    ASSERT_EQ_INT(rx, -1);
}

/* ------------------------------------------------------------------ */
/* 4.10 Preflight query: TCP connect failure                           */
/* ------------------------------------------------------------------ */

static void test_udp_preflight_connect_fail(void)
{
    mock_reset();
    mock_socket_return = -1;
    mock_socket_errno  = ECONNREFUSED;

    int rx = udp_preflight_query("127.0.0.1", 9999);
    ASSERT_EQ_INT(rx, -1);
}

/* ------------------------------------------------------------------ */
/* Suite runner (called from test_main.c)                              */
/* ------------------------------------------------------------------ */

void run_udp_tests(void)
{
    run_test("udp: no packet loss",              test_udp_no_loss);
    run_test("udp: packet loss display",         test_udp_with_loss);
    run_test("udp: jitter µs→ms conversion",     test_udp_jitter_display);
    run_test("udp: capacity Mbps display",       test_udp_capacity_mbps);
    run_test("udp: capacity Gbps display",       test_udp_capacity_gbps);
    run_test("udp: out-of-order line absent",    test_udp_no_ooo_line_when_zero);
    run_test("udp: out-of-order line present",   test_udp_ooo_line_shown_when_nonzero);
    run_test("udp: zero sent no crash",          test_udp_zero_sent);
    run_test("udp: collect-fail consistent display",
             test_udp_collect_fail_consistent_display);
    run_test("udp: preflight OK (server confirms probes)",
             test_udp_preflight_ok);
    run_test("udp: preflight zero (port blocked)",
             test_udp_preflight_zero);
    run_test("udp: preflight old server (unrecognised)",
             test_udp_preflight_old_server);
    run_test("udp: preflight connect failure",
             test_udp_preflight_connect_fail);
}
