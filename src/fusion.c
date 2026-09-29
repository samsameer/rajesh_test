#include "sf/fusion.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static inline void ksum_reset(sf_ksum *k)
{
    k->sum = 0.0;
    k->comp = 0.0;
}

static inline void ksum_add(sf_ksum *k, double x)
{
    const double t = k->sum + x;
    if (fabs(k->sum) >= fabs(x))
        k->comp += (k->sum - t) + x;
    else
        k->comp += (x - t) + k->sum;
    k->sum = t;
}

static inline double ksum_value(const sf_ksum *k)
{
    return k->sum + k->comp;
}

static void fusion_rebase(sf_fusion *f)
{
    ksum_reset(&f->log_sum);
    ksum_reset(&f->sum);
    ksum_reset(&f->sum_sq);
    f->zero_count = 0;
    for (size_t i = 0; i < f->count; ++i) {
        const sf_fusion_slot *s = &f->slots[i];
        if (s->value == 0.0)
            ++f->zero_count;
        else
            ksum_add(&f->log_sum, s->log_value);
        ksum_add(&f->sum, s->value);
        ksum_add(&f->sum_sq, s->value * s->value);
    }
    f->since_rebase = 0;
}

int sf_fusion_init(sf_fusion *f, size_t window)
{
    memset(f, 0, sizeof *f);
    if (window == 0 || window > SIZE_MAX / sizeof(sf_fusion_slot))
        return EINVAL;
    f->slots = calloc(window, sizeof *f->slots);
    if (!f->slots)
        return ENOMEM;
    f->capacity = window;
    return 0;
}

void sf_fusion_destroy(sf_fusion *f)
{
    free(f->slots);
    memset(f, 0, sizeof *f);
}

bool sf_fusion_is_valid_input(double x)
{
    return isfinite(x) && x >= SF_FUSION_MIN_VALUE && x <= SF_FUSION_MAX_VALUE;
}

sf_push_result sf_fusion_push(sf_fusion *f, double x)
{
    if (!sf_fusion_is_valid_input(x)) {
        ++f->rejected;
        return SF_PUSH_REJECTED;
    }

    if (f->count == f->capacity) {
        const sf_fusion_slot *old = &f->slots[f->head];
        if (old->value == 0.0)
            --f->zero_count;
        else
            ksum_add(&f->log_sum, -old->log_value);
        ksum_add(&f->sum, -old->value);
        ksum_add(&f->sum_sq, -(old->value * old->value));
    } else {
        ++f->count;
    }

    const double lx = x > 0.0 ? log(x) : 0.0;
    f->slots[f->head] = (sf_fusion_slot){ .value = x, .log_value = lx };
    f->head = (f->head + 1 == f->capacity) ? 0 : f->head + 1;

    if (x == 0.0)
        ++f->zero_count;
    else
        ksum_add(&f->log_sum, lx);
    ksum_add(&f->sum, x);
    ksum_add(&f->sum_sq, x * x);

    ++f->accepted;
    if (++f->since_rebase >= f->capacity)
        fusion_rebase(f);
    return SF_PUSH_ACCEPTED;
}

double sf_fusion_geometric_mean(const sf_fusion *f)
{
    if (f->count == 0 || f->zero_count > 0)
        return 0.0;
    const double gm = exp(ksum_value(&f->log_sum) / (double)f->count);
    if (gm > SF_FUSION_MAX_VALUE)
        return SF_FUSION_MAX_VALUE;
    return gm;
}

double sf_fusion_pairwise_strength(const sf_fusion *f, sf_f2_norm norm)
{
    if (f->count < 2)
        return 0.0;

    const double n = (double)f->count;
    const double s = ksum_value(&f->sum);
    const double q = ksum_value(&f->sum_sq);
    const double pairs = s * s - q;
    if (!(pairs > 0.0))
        return 0.0;

    if (norm == SF_F2_NORM_PAIR_MEAN) {
        const double r = sqrt(pairs / (n * (n - 1.0)));
        return r > SF_FUSION_MAX_VALUE ? SF_FUSION_MAX_VALUE : r;
    }
    return sqrt(pairs / n);
}

const char *sf_f2_norm_name(sf_f2_norm norm)
{
    return norm == SF_F2_NORM_PAIR_MEAN ? "pair-mean" : "formula";
}

bool sf_f2_norm_parse(const char *text, sf_f2_norm *out)
{
    if (strcmp(text, "formula") == 0) {
        *out = SF_F2_NORM_FORMULA;
        return true;
    }
    if (strcmp(text, "pair-mean") == 0) {
        *out = SF_F2_NORM_PAIR_MEAN;
        return true;
    }
    return false;
}
