/*
 * harness.h — Minimal unit-test harness for spdchk.
 *
 * Usage:
 *   - Declare test suites with void run_*_tests(void) and call them from
 *     test_main.c.
 *   - Inside each test function use the ASSERT_* macros below. On failure
 *     the macro prints a diagnostic and returns from the test function.
 *   - run_test() records pass/fail and prints a one-line summary per test.
 */

#ifndef HARNESS_H
#define HARNESS_H

#include <stdio.h>
#include <string.h>

/* Provided by test_main.c */
void run_test(const char *name, void (*fn)(void));
void test_fail(const char *func, int line, const char *msg);

/* Assert that two integer expressions are equal. */
#define ASSERT_EQ_INT(actual, expected)                                    \
    do {                                                                   \
        int _a = (int)(actual), _b = (int)(expected);                     \
        if (_a != _b) {                                                    \
            char _m[128];                                                  \
            snprintf(_m, sizeof(_m), "expected %d, got %d", _b, _a);      \
            test_fail(__func__, __LINE__, _m);                             \
            return;                                                        \
        }                                                                  \
    } while (0)

/* Assert that the string haystack contains needle as a substring. */
#define ASSERT_CONTAINS(haystack, needle)                                  \
    do {                                                                   \
        if (!strstr((haystack), (needle))) {                               \
            char _m[256];                                                  \
            snprintf(_m, sizeof(_m),                                       \
                     "expected substring \"%s\" in: %s",                   \
                     (needle), (haystack));                                \
            test_fail(__func__, __LINE__, _m);                             \
            return;                                                        \
        }                                                                  \
    } while (0)

/* Assert that the string haystack does NOT contain needle. */
#define ASSERT_NOT_CONTAINS(haystack, needle)                              \
    do {                                                                   \
        if (strstr((haystack), (needle))) {                                \
            char _m[256];                                                  \
            snprintf(_m, sizeof(_m),                                       \
                     "unexpected substring \"%s\" in: %s",                 \
                     (needle), (haystack));                                \
            test_fail(__func__, __LINE__, _m);                             \
            return;                                                        \
        }                                                                  \
    } while (0)

#endif /* HARNESS_H */
