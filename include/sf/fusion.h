#ifndef SF_FUSION_H
#define SF_FUSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SF_FUSION_MIN_VALUE 0.0
#define SF_FUSION_MAX_VALUE 100.0

typedef enum sf_f2_norm {
    SF_F2_NORM_FORMULA,
    SF_F2_NORM_PAIR_MEAN,
} sf_f2_norm;

typedef enum sf_push_result {
    SF_PUSH_ACCEPTED,
    SF_PUSH_REJECTED,
} sf_push_result;

typedef struct sf_ksum {
    double sum;
    double comp;
} sf_ksum;

typedef struct sf_fusion_slot {
    double value;
    double log_value;
} sf_fusion_slot;

typedef struct sf_fusion {
    sf_fusion_slot *slots;
    size_t capacity;
    size_t count;
    size_t head;
    size_t zero_count;
    size_t since_rebase;
    sf_ksum log_sum;
    sf_ksum sum;
    sf_ksum sum_sq;
    uint64_t accepted;
    uint64_t rejected;
} sf_fusion;

int sf_fusion_init(sf_fusion *f, size_t window);
void sf_fusion_destroy(sf_fusion *f);

bool sf_fusion_is_valid_input(double x);
sf_push_result sf_fusion_push(sf_fusion *f, double x);

double sf_fusion_geometric_mean(const sf_fusion *f);
double sf_fusion_pairwise_strength(const sf_fusion *f, sf_f2_norm norm);

static inline size_t sf_fusion_size(const sf_fusion *f)
{
    return f->count;
}

static inline bool sf_fusion_full(const sf_fusion *f)
{
    return f->count == f->capacity;
}

const char *sf_f2_norm_name(sf_f2_norm norm);
bool sf_f2_norm_parse(const char *text, sf_f2_norm *out);

#endif
