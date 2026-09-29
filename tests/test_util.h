#ifndef SF_TEST_UTIL_H
#define SF_TEST_UTIL_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int g_test_failures;
static int g_test_checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_test_checks;                                                                \
        if (!(cond)) {                                                                  \
            ++g_test_failures;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                               \
    } while (0)

#define CHECK_NEAR(actual, expected, rel_tol)                                           \
    do {                                                                                \
        ++g_test_checks;                                                                \
        const double a_ = (actual);                                                     \
        const double e_ = (expected);                                                   \
        const double scale_ = fmax(1.0, fmax(fabs(a_), fabs(e_)));                      \
        if (!(fabs(a_ - e_) <= (rel_tol) * scale_)) {                                   \
            ++g_test_failures;                                                          \
            fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.17g, expected %.17g\n",   \
                    __FILE__, __LINE__, #actual, a_, e_);                               \
        }                                                                               \
    } while (0)

#define RUN_TEST(fn)                                                                    \
    do {                                                                                \
        const int before_ = g_test_failures;                                            \
        fn();                                                                           \
        printf("%s %s\n", g_test_failures == before_ ? "[  OK  ]" : "[ FAIL ]", #fn);   \
    } while (0)

#define TEST_REPORT()                                                                   \
    (printf("%d checks, %d failures\n", g_test_checks, g_test_failures),                \
     g_test_failures ? EXIT_FAILURE : EXIT_SUCCESS)

#endif
