#include "sf/spsc_queue.h"

#include <errno.h>
#include <stdlib.h>

static size_t round_up_pow2(size_t v)
{
    size_t p = 1;
    while (p < v)
        p <<= 1;
    return p;
}

int sf_spsc_init(sf_spsc_queue *q, size_t min_capacity)
{
    if (min_capacity < 2 || min_capacity > ((size_t)1 << 30))
        return EINVAL;

    const size_t capacity = round_up_pow2(min_capacity);
    sf_sample *slots = calloc(capacity, sizeof *slots);
    if (!slots)
        return ENOMEM;

    atomic_init(&q->tail, 0);
    atomic_init(&q->head, 0);
    atomic_init(&q->closed, false);
    q->cached_head = 0;
    q->cached_tail = 0;
    q->slots = slots;
    q->capacity = capacity;
    q->mask = capacity - 1;
    return 0;
}

void sf_spsc_destroy(sf_spsc_queue *q)
{
    free(q->slots);
    q->slots = NULL;
    q->capacity = 0;
    q->mask = 0;
}
