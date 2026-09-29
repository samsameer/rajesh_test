#ifndef SF_AGGREGATOR_H
#define SF_AGGREGATOR_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sf/spsc_queue.h"

#define SF_MAX_SOURCES 8

typedef struct sf_aggregator {
    sf_spsc_queue *sources[SF_MAX_SOURCES];
    uint64_t frequency_mhz[SF_MAX_SOURCES];
    size_t priority[SF_MAX_SOURCES];
    size_t source_count;
    sf_spsc_queue *sink;
    uint64_t forwarded[SF_MAX_SOURCES];
    uint64_t discarded[SF_MAX_SOURCES];
    uint64_t emitted;
    pthread_t thread;
    bool running;
} sf_aggregator;

void sf_aggregator_init(sf_aggregator *a, sf_spsc_queue *sink);
int sf_aggregator_add_source(sf_aggregator *a, sf_spsc_queue *source, uint64_t frequency_mhz);
void sf_aggregator_run(sf_aggregator *a);
int sf_aggregator_start(sf_aggregator *a);
void sf_aggregator_join(sf_aggregator *a);

#endif
