#include "sf/aggregator.h"

#include <string.h>

#include "sf/backoff.h"

void sf_aggregator_init(sf_aggregator *a, sf_spsc_queue *sink)
{
    memset(a, 0, sizeof *a);
    a->sink = sink;
}

int sf_aggregator_add_source(sf_aggregator *a, sf_spsc_queue *source, uint64_t frequency_mhz)
{
    if (a->source_count == SF_MAX_SOURCES || frequency_mhz == 0)
        return -1;

    const size_t index = a->source_count++;
    a->sources[index] = source;
    a->frequency_mhz[index] = frequency_mhz;

    size_t pos = index;
    while (pos > 0 && a->frequency_mhz[a->priority[pos - 1]] > frequency_mhz) {
        a->priority[pos] = a->priority[pos - 1];
        --pos;
    }
    a->priority[pos] = index;
    return (int)index;
}

void sf_aggregator_run(sf_aggregator *a)
{
    const sf_sample *heads[SF_MAX_SOURCES];
    sf_backoff backoff;
    sf_backoff_reset(&backoff);

    for (;;) {
        size_t best = SIZE_MAX;
        bool blocked = false;

        for (size_t r = 0; r < a->source_count; ++r) {
            const size_t i = a->priority[r];
            heads[i] = sf_spsc_front(a->sources[i]);
            if (!heads[i]) {
                if (!sf_spsc_drained(a->sources[i])) {
                    blocked = true;
                    break;
                }
                continue;
            }
            if (best == SIZE_MAX || heads[i]->timestamp_us < heads[best]->timestamp_us)
                best = i;
        }

        if (blocked) {
            sf_backoff_pause(&backoff);
            continue;
        }
        if (best == SIZE_MAX)
            break;
        sf_backoff_reset(&backoff);

        const sf_sample winner = *heads[best];
        sf_spsc_pop(a->sources[best]);
        ++a->forwarded[best];

        for (size_t i = 0; i < a->source_count; ++i) {
            if (i != best && heads[i] && heads[i]->timestamp_us == winner.timestamp_us) {
                sf_spsc_pop(a->sources[i]);
                ++a->discarded[i];
            }
        }

        while (!sf_spsc_try_push(a->sink, &winner))
            sf_backoff_pause(&backoff);
        sf_backoff_reset(&backoff);
        ++a->emitted;
    }

    sf_spsc_close(a->sink);
}

static void *aggregator_thread_main(void *arg)
{
    sf_aggregator_run(arg);
    return NULL;
}

int sf_aggregator_start(sf_aggregator *a)
{
    const int rc = pthread_create(&a->thread, NULL, aggregator_thread_main, a);
    a->running = rc == 0;
    return rc;
}

void sf_aggregator_join(sf_aggregator *a)
{
    if (a->running) {
        pthread_join(a->thread, NULL);
        a->running = false;
    }
}
