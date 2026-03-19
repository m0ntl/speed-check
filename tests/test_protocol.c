/*
 * test_protocol.c — Unit tests for client_check_server_version() (§3.2).
 *
 * socket(), connect(), fcntl(), setsockopt(), send(), recv(), and close()
 * are all intercepted via mock_sockets.h.  Each test loads a canned server
 * response into mock_recv_buf / mock_recv_len before calling the function
 * under test, then inspects the return code.
 *
 * Return-code reference (from client.h):
 *    0   version matched
 *   -1   connection / timeout / unexpected-response error
 *   -2   server explicitly replied ERR VERSION_MISMATCH
 */

#include <string.h>
#include "harness.h"
#include "mock_sockets.h"
#include "../client.h"

/* ------------------------------------------------------------------ */
/* 3.2 Version match: server replies "OK\n"                           */
/* ------------------------------------------------------------------ */

static void test_version_match(void)
{
    mock_reset();
    const char *resp = "OK\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    int rc = client_check_server_version("127.0.0.1", 9999, 0);
    ASSERT_EQ_INT(rc, 0);
}

/* ------------------------------------------------------------------ */
/* 3.2 Version mismatch: server replies ERR VERSION_MISMATCH          */
/* ------------------------------------------------------------------ */

static void test_version_mismatch(void)
{
    mock_reset();
    const char *resp = "ERR VERSION_MISMATCH 0.0.1\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    /* client_check_server_version() returns -2 on explicit mismatch;
     * run_client_ex() maps that to -3 for the interactive layer. */
    int rc = client_check_server_version("127.0.0.1", 9999, 0);
    ASSERT_EQ_INT(rc, -2);
}

/* ------------------------------------------------------------------ */
/* 3.2 Corrupt payload: server sends unrecognised data                */
/* ------------------------------------------------------------------ */

static void test_corrupt_payload(void)
{
    mock_reset();
    const char *resp = "XYZZY_JUNK_DATA_!!!\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    int rc = client_check_server_version("127.0.0.1", 9999, 0);
    ASSERT_EQ_INT(rc, -1);
}

/* ------------------------------------------------------------------ */
/* Connection path errors                                              */
/* ------------------------------------------------------------------ */

static void test_socket_failure(void)
{
    mock_reset();
    mock_socket_return = -1;
    mock_socket_errno  = ECONNREFUSED;

    int rc = client_check_server_version("127.0.0.1", 9999, 0);
    ASSERT_EQ_INT(rc, -1);
}

static void test_connect_failure(void)
{
    mock_reset();
    mock_connect_return = -1;
    mock_connect_errno  = ECONNREFUSED;

    int rc = client_check_server_version("127.0.0.1", 9999, 0);
    ASSERT_EQ_INT(rc, -1);
}

/* Server closes the connection before sending any data (recv returns 0). */
static void test_empty_response(void)
{
    mock_reset();
    /* mock_recv_len = 0: mock_recv() returns 0 immediately → ri stays 0 */

    int rc = client_check_server_version("127.0.0.1", 9999, 0);
    ASSERT_EQ_INT(rc, -1);
}

/* ------------------------------------------------------------------ */
/* DSS mode: greeting must include the "DSS" capability token         */
/* ------------------------------------------------------------------ */

static void test_dss_greeting(void)
{
    mock_reset();
    const char *resp = "OK\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    int rc = client_check_server_version("127.0.0.1", 9999, 1 /* dss_mode=1 */);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_CONTAINS(mock_send_buf, "DSS");
}

/* Non-DSS mode: greeting must NOT contain the "DSS" token. */
static void test_nodss_greeting(void)
{
    mock_reset();
    const char *resp = "OK\n";
    memcpy(mock_recv_buf, resp, strlen(resp));
    mock_recv_len = (int)strlen(resp);

    client_check_server_version("127.0.0.1", 9999, 0 /* dss_mode=0 */);
    ASSERT_NOT_CONTAINS(mock_send_buf, "DSS");
}

/* ------------------------------------------------------------------ */
/* Suite runner                                                         */
/* ------------------------------------------------------------------ */

void run_protocol_tests(void)
{
    run_test("version_match",       test_version_match);
    run_test("version_mismatch",    test_version_mismatch);
    run_test("corrupt_payload",     test_corrupt_payload);
    run_test("socket_failure",      test_socket_failure);
    run_test("connect_failure",     test_connect_failure);
    run_test("empty_response",      test_empty_response);
    run_test("dss_greeting_format", test_dss_greeting);
    run_test("nodss_greeting_format", test_nodss_greeting);
}
