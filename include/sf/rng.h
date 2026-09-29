#ifndef SF_RNG_H
#define SF_RNG_H

#include <stdint.h>

typedef struct sf_rng {
    uint64_t state;
} sf_rng;

static inline uint64_t sf_rng_next(sf_rng *r)
{
    uint64_t z = (r->state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static inline double sf_rng_unit(sf_rng *r)
{
    return (double)(sf_rng_next(r) >> 11) * 0x1.0p-53;
}

#endif
