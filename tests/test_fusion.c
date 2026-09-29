#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sf/fusion.h"
#include "sf/rng.h"
#include "sf/writer.h"
#include "test_util.h"

#define TOL 1e-12

static double naive_geometric_mean(const double *x, size_t n)
{
    long double acc = 0.0L;
    for (size_t i = 0; i < n; ++i) {
        if (x[i] == 0.0)
            return 0.0;
        acc += logl((long double)x[i]);
    }
    return (double)expl(acc / (long double)n);
}

static double naive_pairwise(const double *x, size_t n, sf_f2_norm norm)
{
    if (n < 2)
        return 0.0;
    long double acc = 0.0L;
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < n; ++j)
            if (i != j)
                acc += (long double)x[i] * (long double)x[j];
    const long double denom = norm == SF_F2_NORM_PAIR_MEAN
                                  ? (long double)n * (long double)(n - 1)
                                  : (long double)n;
    return (double)sqrtl(acc / denom);
}

static sf_fusion make_fusion(size_t window)
{
    sf_fusion f;
    CHECK(sf_fusion_init(&f, window) == 0);
    return f;
}

static void test_init_rejects_zero_window(void)
{
    sf_fusion f;
    CHECK(sf_fusion_init(&f, 0) != 0);
}

static void test_empty_window_is_zero(void)
{
    sf_fusion f = make_fusion(8);
    CHECK(sf_fusion_geometric_mean(&f) == 0.0);
    CHECK(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA) == 0.0);
    CHECK(sf_fusion_pairwise_strength(&f, SF_F2_NORM_PAIR_MEAN) == 0.0);
    sf_fusion_destroy(&f);
}

static void test_single_value(void)
{
    sf_fusion f = make_fusion(4);
    sf_fusion_push(&f, 42.0);
    CHECK_NEAR(sf_fusion_geometric_mean(&f), 42.0, TOL);
    CHECK(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA) == 0.0);
    CHECK(sf_fusion_pairwise_strength(&f, SF_F2_NORM_PAIR_MEAN) == 0.0);
    sf_fusion_destroy(&f);
}

static void test_known_values(void)
{
    sf_fusion f = make_fusion(3);
    sf_fusion_push(&f, 1.0);
    sf_fusion_push(&f, 2.0);
    sf_fusion_push(&f, 4.0);
    CHECK(sf_fusion_full(&f));
    CHECK_NEAR(sf_fusion_geometric_mean(&f), 2.0, TOL);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA), sqrt(28.0 / 3.0), TOL);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_PAIR_MEAN), sqrt(28.0 / 6.0), TOL);
    sf_fusion_destroy(&f);
}

static void test_window_eviction(void)
{
    sf_fusion f = make_fusion(3);
    const double seq[] = { 1.0, 2.0, 4.0, 8.0, 16.0 };
    for (size_t i = 0; i < 5; ++i)
        sf_fusion_push(&f, seq[i]);
    CHECK(sf_fusion_size(&f) == 3);
    CHECK_NEAR(sf_fusion_geometric_mean(&f), 8.0, TOL);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA),
               naive_pairwise(seq + 2, 3, SF_F2_NORM_FORMULA), TOL);
    sf_fusion_destroy(&f);
}

static void test_zero_handling(void)
{
    sf_fusion f = make_fusion(3);
    sf_fusion_push(&f, 0.0);
    sf_fusion_push(&f, 50.0);
    sf_fusion_push(&f, 50.0);
    CHECK(sf_fusion_geometric_mean(&f) == 0.0);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA), sqrt(5000.0 / 3.0), TOL);
    sf_fusion_push(&f, 50.0);
    CHECK_NEAR(sf_fusion_geometric_mean(&f), 50.0, TOL);
    CHECK(f.zero_count == 0);
    sf_fusion_destroy(&f);
}

static void test_invalid_inputs_rejected(void)
{
    sf_fusion f = make_fusion(4);
    sf_fusion_push(&f, 10.0);
    sf_fusion_push(&f, 20.0);
    const double before_f1 = sf_fusion_geometric_mean(&f);
    const double before_f2 = sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA);

    const double bad[] = { (double)NAN, (double)INFINITY, -(double)INFINITY, -0.001, -50.0, 100.0000001, 1e308 };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; ++i)
        CHECK(sf_fusion_push(&f, bad[i]) == SF_PUSH_REJECTED);

    CHECK(f.rejected == sizeof bad / sizeof bad[0]);
    CHECK(f.accepted == 2);
    CHECK(sf_fusion_size(&f) == 2);
    CHECK(sf_fusion_geometric_mean(&f) == before_f1);
    CHECK(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA) == before_f2);
    sf_fusion_destroy(&f);
}

static void test_range_boundaries_accepted(void)
{
    CHECK(sf_fusion_is_valid_input(0.0));
    CHECK(sf_fusion_is_valid_input(-0.0));
    CHECK(sf_fusion_is_valid_input(100.0));
    CHECK(sf_fusion_is_valid_input(5e-324));
    CHECK(!sf_fusion_is_valid_input(nextafter(100.0, 200.0)));
    CHECK(!sf_fusion_is_valid_input(nextafter(0.0, -1.0)));
}

static void test_extreme_small_values(void)
{
    sf_fusion f = make_fusion(2);
    sf_fusion_push(&f, 1e-300);
    sf_fusion_push(&f, 100.0);
    CHECK_NEAR(sf_fusion_geometric_mean(&f) / 1e-149, 1.0, 1e-9);
    CHECK(isfinite(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA)));
    sf_fusion_destroy(&f);
}

static void test_constant_stream(void)
{
    const size_t n = 5000;
    sf_fusion f = make_fusion(n);
    for (size_t i = 0; i < 3 * n; ++i)
        sf_fusion_push(&f, 37.5);
    CHECK_NEAR(sf_fusion_geometric_mean(&f), 37.5, TOL);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_PAIR_MEAN), 37.5, TOL);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA), 37.5 * sqrt((double)(n - 1)),
               TOL);
    sf_fusion_destroy(&f);
}

static void test_outputs_stay_in_range(void)
{
    sf_fusion f = make_fusion(4097);
    for (size_t i = 0; i < 20000; ++i)
        sf_fusion_push(&f, 100.0);
    CHECK(sf_fusion_geometric_mean(&f) <= 100.0);
    CHECK(sf_fusion_pairwise_strength(&f, SF_F2_NORM_PAIR_MEAN) <= 100.0);
    sf_fusion_destroy(&f);
}

static void test_matches_bruteforce(void)
{
    const size_t window = 300;
    double *hist = malloc(sizeof(double) * 5000);
    CHECK(hist != NULL);
    if (!hist)
        return;

    sf_rng rng = { 12345 };
    sf_fusion f = make_fusion(window);
    for (size_t i = 0; i < 5000; ++i) {
        hist[i] = 100.0 * sf_rng_unit(&rng);
        sf_fusion_push(&f, hist[i]);
        if (i + 1 >= window && (i % 97 == 0 || i == 4999)) {
            const double *w = hist + i + 1 - window;
            CHECK_NEAR(sf_fusion_geometric_mean(&f), naive_geometric_mean(w, window), 1e-11);
            CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA),
                       naive_pairwise(w, window, SF_F2_NORM_FORMULA), 1e-11);
            CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_PAIR_MEAN),
                       naive_pairwise(w, window, SF_F2_NORM_PAIR_MEAN), 1e-11);
        }
    }
    sf_fusion_destroy(&f);
    free(hist);
}

static void test_long_run_has_no_drift(void)
{
    const size_t window = 4097;
    const size_t total = 3000000;
    double *ring = malloc(sizeof(double) * window);
    CHECK(ring != NULL);
    if (!ring)
        return;

    sf_rng rng = { 99 };
    sf_fusion f = make_fusion(window);
    for (size_t i = 0; i < total; ++i) {
        const double scale = (i / 50000) % 2 ? 100.0 : 1e-3;
        const double v = scale * sf_rng_unit(&rng) + 1e-9;
        ring[i % window] = v;
        sf_fusion_push(&f, v);
    }

    long double ls = 0.0L, s = 0.0L, q = 0.0L;
    for (size_t i = 0; i < window; ++i) {
        ls += logl((long double)ring[i]);
        s += (long double)ring[i];
        q += (long double)ring[i] * (long double)ring[i];
    }
    const double ref_f1 = (double)expl(ls / window);
    const double ref_f2 = (double)sqrtl((s * s - q) / window);
    CHECK_NEAR(sf_fusion_geometric_mean(&f), ref_f1, 1e-11);
    CHECK_NEAR(sf_fusion_pairwise_strength(&f, SF_F2_NORM_FORMULA), ref_f2, 1e-11);

    sf_fusion_destroy(&f);
    free(ring);
}

static void test_norm_parse(void)
{
    sf_f2_norm n;
    CHECK(sf_f2_norm_parse("formula", &n) && n == SF_F2_NORM_FORMULA);
    CHECK(sf_f2_norm_parse("pair-mean", &n) && n == SF_F2_NORM_PAIR_MEAN);
    CHECK(!sf_f2_norm_parse("average", &n));
}

static void test_fixed_formatter(void)
{
    char buf[64];
    CHECK(sf_format_fixed6(buf, sizeof buf, 0.0) == 8 && strcmp(buf, "0.000000") == 0);
    sf_format_fixed6(buf, sizeof buf, 48.1640494);
    CHECK(strcmp(buf, "48.164049") == 0);
    sf_format_fixed6(buf, sizeof buf, 99.9999996);
    CHECK(strcmp(buf, "100.000000") == 0);
    sf_format_fixed6(buf, sizeof buf, 4575.1054295);
    CHECK(strcmp(buf, "4575.105430") == 0);
    sf_format_fixed6(buf, sizeof buf, (double)NAN);
    CHECK(strcmp(buf, "nan") == 0);
}

int main(void)
{
    RUN_TEST(test_init_rejects_zero_window);
    RUN_TEST(test_empty_window_is_zero);
    RUN_TEST(test_single_value);
    RUN_TEST(test_known_values);
    RUN_TEST(test_window_eviction);
    RUN_TEST(test_zero_handling);
    RUN_TEST(test_invalid_inputs_rejected);
    RUN_TEST(test_range_boundaries_accepted);
    RUN_TEST(test_extreme_small_values);
    RUN_TEST(test_constant_stream);
    RUN_TEST(test_outputs_stay_in_range);
    RUN_TEST(test_matches_bruteforce);
    RUN_TEST(test_long_run_has_no_drift);
    RUN_TEST(test_norm_parse);
    RUN_TEST(test_fixed_formatter);
    return TEST_REPORT();
}
