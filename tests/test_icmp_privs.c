/*
 * test_icmp_privs.c — Unit tests for ICMP privilege-error handling (§3.3).
 *
 * icmp_ping() opens a raw socket (SOCK_RAW / IPPROTO_ICMP).  When that
 * socket() call fails due to insufficient privileges the function must
 * map the error to return code -2.  All other socket errors must map to -1.
 *
 * socket() is intercepted via mock_sockets.h so the tests work without
 * root privileges.
 *
 * Return-code reference (from icmp.h):
 *    0   at least one reply received
 *   -1   all pings lost or non-permission socket error
 *   -2   raw socket could not be created: EPERM or EACCES
 */

#include "harness.h"
#include "mock_sockets.h"
#include "../icmp.h"

/* ------------------------------------------------------------------ */
/* 3.3 Failure mapping                                                 */
/* ------------------------------------------------------------------ */

/* EPERM (most common on Linux without CAP_NET_RAW) must return -2. */
static void test_eperm_returns_minus2(void)
{
    mock_reset();
    mock_socket_return = -1;
    mock_socket_errno  = EPERM;

    struct icmp_stats stats;
    int rc = icmp_ping("127.0.0.1", 4, &stats);
    ASSERT_EQ_INT(rc, -2);
}

/* EACCES (seen on some kernels / setuid scenarios) must also return -2. */
static void test_eacces_returns_minus2(void)
{
    mock_reset();
    mock_socket_return = -1;
    mock_socket_errno  = EACCES;

    struct icmp_stats stats;
    int rc = icmp_ping("127.0.0.1", 4, &stats);
    ASSERT_EQ_INT(rc, -2);
}

/* Any other socket error (e.g. ENOMEM) must return -1, not -2. */
static void test_other_error_returns_minus1(void)
{
    mock_reset();
    mock_socket_return = -1;
    mock_socket_errno  = ENOMEM;

    struct icmp_stats stats;
    int rc = icmp_ping("127.0.0.1", 4, &stats);
    ASSERT_EQ_INT(rc, -1);
}

/* ------------------------------------------------------------------ */
/* Suite runner                                                         */
/* ------------------------------------------------------------------ */

void run_icmp_priv_tests(void)
{
    run_test("eperm_maps_to_minus2",        test_eperm_returns_minus2);
    run_test("eacces_maps_to_minus2",       test_eacces_returns_minus2);
    run_test("other_error_maps_to_minus1",  test_other_error_returns_minus1);
}
