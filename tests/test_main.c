/*
 * test_main.c — Entry point for the spdchk unit-test binary.
 *
 * Initialises the logger (suppressing all but errors so log noise does not
 * pollute test output), runs all test suites, and exits with a non-zero
 * status if any assertion failed.
 */

#include <stdio.h>
#include "harness.h"
#include "../logger.h"

/* ------------------------------------------------------------------ */
/* Harness state (used by run_test / test_fail)                        */
/* ------------------------------------------------------------------ */

static int g_tests_run    = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static int g_test_failed  = 0;  /* set to 1 by ASSERT_* on failure */

void test_fail(const char *func, int line, const char *msg)
{
    printf("        FAIL  %s:%d  %s\n", func, line, msg);
    g_test_failed = 1;
}

void run_test(const char *name, void (*fn)(void))
{
    g_tests_run++;
    g_test_failed = 0;
    fn();
    if (g_test_failed) {
        printf("  [ FAIL ]  %s\n", name);
        g_tests_failed++;
    } else {
        printf("  [ PASS ]  %s\n", name);
        g_tests_passed++;
    }
}

/* ------------------------------------------------------------------ */
/* Test suite forward declarations                                      */
/* ------------------------------------------------------------------ */

void run_metrics_tests(void);
void run_protocol_tests(void);
void run_icmp_priv_tests(void);
void run_udp_tests(void);

/* ------------------------------------------------------------------ */
/* Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Suppress INFO/DEBUG/TRACE noise; only ERROR messages reach stdout. */
    logger_init(LOG_LEVEL_ERROR);

    printf("=== spdchk Unit Tests ===\n\n");

    printf("-- Metrics --\n");
    run_metrics_tests();

    printf("\n-- Protocol Handshake --\n");
    run_protocol_tests();

    printf("\n-- ICMP Privilege Handling --\n");
    run_icmp_priv_tests();

    printf("\n-- UDP Metrics --\n");
    run_udp_tests();

    printf("\n=== Results: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed)
        printf(" — %d FAILURE(S) DETECTED", g_tests_failed);
    printf(" ===\n");

    logger_close();
    return (g_tests_failed > 0) ? 1 : 0;
}
