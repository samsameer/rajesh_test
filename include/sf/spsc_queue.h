#ifndef SF_SPSC_QUEUE_H
#define SF_SPSC_QUEUE_H

#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>

#include "sf/sample.h"

#define SF_CACHE_LINE 64

typedef struct sf_spsc_queue {
    alignas(SF_CACHE_LINE) atomic_size_t tail;
    size_t cached_head;
    alignas(SF_CACHE_LINE) atomic_size_t head;
    size_t cached_tail;
    alignas(SF_CACHE_LINE) atomic_bool closed;
    alignas(SF_CACHE_LINE) sf_sample *slots;
    size_t capacity;
    size_t mask;
} sf_spsc_queue;

int sf_spsc_init(sf_spsc_queue *q, size_t min_capacity);
void sf_spsc_destroy(sf_spsc_queue *q);

static inline bool sf_spsc_try_push(sf_spsc_queue *q, const sf_sample *s)
{
    const size_t t = atomic_load_explicit(&q->tail, memory_order_relaxed);
    if (t - q->cached_head == q->capacity) {
        q->cached_head = atomic_load_explicit(&q->head, memory_order_acquire);
        if (t - q->cached_head == q->capacity)
            return false;
    }
    q->slots[t & q->mask] = *s;
    atomic_store_explicit(&q->tail, t + 1, memory_order_release);
    return true;
}

static inline const sf_sample *sf_spsc_front(sf_spsc_queue *q)
{
    const size_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    if (h == q->cached_tail) {
        q->cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        if (h == q->cached_tail)
            return NULL;
    }
    return &q->slots[h & q->mask];
}

static inline void sf_spsc_pop(sf_spsc_queue *q)
{
    const size_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    atomic_store_explicit(&q->head, h + 1, memory_order_release);
}

static inline size_t sf_spsc_pop_bulk(sf_spsc_queue *q, sf_sample *out, size_t max)
{
    const size_t h = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t available = q->cached_tail - h;
    if (available < max) {
        q->cached_tail = atomic_load_explicit(&q->tail, memory_order_acquire);
        available = q->cached_tail - h;
    }
    if (available == 0)
        return 0;
    if (available > max)
        available = max;
    for (size_t i = 0; i < available; ++i)
        out[i] = q->slots[(h + i) & q->mask];
    atomic_store_explicit(&q->head, h + available, memory_order_release);
    return available;
}

static inline void sf_spsc_close(sf_spsc_queue *q)
{
    atomic_store_explicit(&q->closed, true, memory_order_release);
}

static inline bool sf_spsc_drained(sf_spsc_queue *q)
{
    if (!atomic_load_explicit(&q->closed, memory_order_acquire))
        return false;
    return sf_spsc_front(q) == NULL;
}

#endif
