#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "sf/aggregator.h"
#include "sf/backoff.h"
#include "sf/clock.h"
#include "sf/fusion.h"
#include "sf/sensor.h"
#include "sf/spsc_queue.h"
#include "test_util.h"

static void push_all(sf_spsc_queue *q, uint32_t id, const uint64_t *ts, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const sf_sample s = { .timestamp_us = ts[i], .value = (double)id, .sensor_id = id };
        CHECK(sf_spsc_try_push(q, &s));
    }
    sf_spsc_close(q);
}

static void test_queue_capacity_and_order(void)
{
    sf_spsc_queue q;
    CHECK(sf_spsc_init(&q, 5) == 0);
    CHECK(q.capacity == 8);

    for (uint64_t i = 0; i < 8; ++i) {
        const sf_sample s = { .timestamp_us = i };
        CHECK(sf_spsc_try_push(&q, &s));
    }
    const sf_sample extra = { .timestamp_us = 99 };
    CHECK(!sf_spsc_try_push(&q, &extra));

    for (uint64_t i = 0; i < 8; ++i) {
        const sf_sample *s = sf_spsc_front(&q);
        CHECK(s && s->timestamp_us == i);
        sf_spsc_pop(&q);
    }
    CHECK(sf_spsc_front(&q) == NULL);
    CHECK(!sf_spsc_drained(&q));
    sf_spsc_close(&q);
    CHECK(sf_spsc_drained(&q));
    sf_spsc_destroy(&q);
}

static void test_queue_wraparound_bulk(void)
{
    sf_spsc_queue q;
    CHECK(sf_spsc_init(&q, 16) == 0);
    uint64_t next_in = 0, next_out = 0;
    sf_sample buf[7];
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 11; ++i) {
            const sf_sample s = { .timestamp_us = next_in };
            if (sf_spsc_try_push(&q, &s))
                ++next_in;
        }
        const size_t n = sf_spsc_pop_bulk(&q, buf, 7);
        for (size_t i = 0; i < n; ++i)
            CHECK(buf[i].timestamp_us == next_out++);
    }
    size_t n;
    while ((n = sf_spsc_pop_bulk(&q, buf, 7)) > 0)
        for (size_t i = 0; i < n; ++i)
            CHECK(buf[i].timestamp_us == next_out++);
    CHECK(next_in == next_out);
    sf_spsc_destroy(&q);
}

enum { THREADED_ITEMS = 2000000 };

static void *queue_producer(void *arg)
{
    sf_spsc_queue *q = arg;
    sf_backoff b;
    for (uint64_t i = 0; i < THREADED_ITEMS; ++i) {
        const sf_sample s = { .timestamp_us = i, .value = (double)i };
        sf_backoff_reset(&b);
        while (!sf_spsc_try_push(q, &s))
            sf_backoff_pause(&b);
    }
    sf_spsc_close(q);
    return NULL;
}

static void test_queue_threaded_fifo(void)
{
    sf_spsc_queue q;
    CHECK(sf_spsc_init(&q, 1024) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, queue_producer, &q) == 0);

    uint64_t expected = 0;
    int errors = 0;
    sf_sample buf[256];
    sf_backoff b;
    sf_backoff_reset(&b);
    for (;;) {
        const size_t n = sf_spsc_pop_bulk(&q, buf, 256);
        if (n == 0) {
            if (sf_spsc_drained(&q))
                break;
            sf_backoff_pause(&b);
            continue;
        }
        for (size_t i = 0; i < n; ++i, ++expected)
            errors += buf[i].timestamp_us != expected || buf[i].value != (double)expected;
    }
    pthread_join(t, NULL);
    CHECK(errors == 0);
    CHECK(expected == THREADED_ITEMS);
    sf_spsc_destroy(&q);
}

static void test_sensor_timestamps(void)
{
    CHECK(sf_sensor_timestamp_us(96000000ULL, 0) == 0);
    CHECK(sf_sensor_timestamp_us(96000000ULL, 1) == 10);
    CHECK(sf_sensor_timestamp_us(96000000ULL, 3) == 31);
    CHECK(sf_sensor_timestamp_us(96000000ULL, 96000) == 1000000);
    CHECK(sf_sensor_timestamp_us(69900000ULL, 1) == 14);
    CHECK(sf_sensor_timestamp_us(69900000ULL, 699) == 10000);
    CHECK(sf_sensor_timestamp_us(23000000ULL, 1) == 43);

    CHECK(sf_sensor_expected_samples(96000000ULL, 5000000ULL) == 480000);
    CHECK(sf_sensor_expected_samples(71000000ULL, 5000000ULL) == 355000);
    CHECK(sf_sensor_expected_samples(69900000ULL, 5000000ULL) == 349500);
    CHECK(sf_sensor_expected_samples(23000000ULL, 5000000ULL) == 115000);
    CHECK(sf_sensor_expected_samples(23000000ULL, 1) == 1);
    CHECK(sf_sensor_expected_samples(23000000ULL, 0) == 0);
}

static void test_aggregator_merge_and_conflicts(void)
{
    sf_spsc_queue a, b, c, sink;
    CHECK(sf_spsc_init(&a, 16) == 0);
    CHECK(sf_spsc_init(&b, 16) == 0);
    CHECK(sf_spsc_init(&c, 16) == 0);
    CHECK(sf_spsc_init(&sink, 64) == 0);

    const uint64_t ts_a[] = { 0, 10, 20, 70 };
    const uint64_t ts_b[] = { 0, 43 };
    const uint64_t ts_c[] = { 0, 14, 70 };
    push_all(&a, 1, ts_a, 4);
    push_all(&b, 4, ts_b, 2);
    push_all(&c, 2, ts_c, 3);

    sf_aggregator agg;
    sf_aggregator_init(&agg, &sink);
    CHECK(sf_aggregator_add_source(&agg, &a, 96000000ULL) == 0);
    CHECK(sf_aggregator_add_source(&agg, &c, 71000000ULL) == 1);
    CHECK(sf_aggregator_add_source(&agg, &b, 23000000ULL) == 2);
    sf_aggregator_run(&agg);

    const uint64_t want_ts[] = { 0, 10, 14, 20, 43, 70 };
    const uint32_t want_id[] = { 4, 1, 2, 1, 4, 2 };
    for (size_t i = 0; i < 6; ++i) {
        const sf_sample *s = sf_spsc_front(&sink);
        CHECK(s != NULL);
        if (!s)
            break;
        CHECK(s->timestamp_us == want_ts[i]);
        CHECK(s->sensor_id == want_id[i]);
        sf_spsc_pop(&sink);
    }
    CHECK(sf_spsc_drained(&sink));
    CHECK(agg.emitted == 6);
    CHECK(agg.discarded[0] == 2);
    CHECK(agg.discarded[1] == 1);
    CHECK(agg.discarded[2] == 0);
    CHECK(agg.forwarded[0] + agg.forwarded[1] + agg.forwarded[2] == 6);

    sf_spsc_destroy(&a);
    sf_spsc_destroy(&b);
    sf_spsc_destroy(&c);
    sf_spsc_destroy(&sink);
}

static void test_aggregator_empty_sources(void)
{
    sf_spsc_queue a, sink;
    CHECK(sf_spsc_init(&a, 4) == 0);
    CHECK(sf_spsc_init(&sink, 4) == 0);
    sf_spsc_close(&a);
    sf_aggregator agg;
    sf_aggregator_init(&agg, &sink);
    sf_aggregator_add_source(&agg, &a, 1000ULL);
    sf_aggregator_run(&agg);
    CHECK(agg.emitted == 0);
    CHECK(sf_spsc_drained(&sink));
    sf_spsc_destroy(&a);
    sf_spsc_destroy(&sink);
}

static uint64_t distinct_timestamps(const uint64_t *freq, size_t n, uint64_t duration_us)
{
    uint64_t idx[SF_MAX_SOURCES] = { 0 };
    uint64_t total[SF_MAX_SOURCES];
    for (size_t i = 0; i < n; ++i)
        total[i] = sf_sensor_expected_samples(freq[i], duration_us);

    uint64_t distinct = 0;
    for (;;) {
        uint64_t min = UINT64_MAX;
        for (size_t i = 0; i < n; ++i)
            if (idx[i] < total[i] && sf_sensor_timestamp_us(freq[i], idx[i]) < min)
                min = sf_sensor_timestamp_us(freq[i], idx[i]);
        if (min == UINT64_MAX)
            return distinct;
        ++distinct;
        for (size_t i = 0; i < n; ++i)
            if (idx[i] < total[i] && sf_sensor_timestamp_us(freq[i], idx[i]) == min)
                ++idx[i];
    }
}

static void test_end_to_end_pipeline(void)
{
    enum { N_SENSORS = 4 };
    const uint64_t freq[N_SENSORS] = { 96000000ULL, 71000000ULL, 69900000ULL, 23000000ULL };
    const uint64_t duration_us = 300000;

    sf_spsc_queue queues[N_SENSORS], sink;
    sf_sensor sensors[N_SENSORS];
    sf_aggregator agg;
    sf_fusion fusion;

    for (size_t i = 0; i < N_SENSORS; ++i)
        CHECK(sf_spsc_init(&queues[i], 4096) == 0);
    CHECK(sf_spsc_init(&sink, 8192) == 0);
    CHECK(sf_fusion_init(&fusion, 4097) == 0);

    sf_aggregator_init(&agg, &sink);
    for (size_t i = 0; i < N_SENSORS; ++i)
        sf_aggregator_add_source(&agg, &queues[i], freq[i]);
    CHECK(sf_aggregator_start(&agg) == 0);

    const uint64_t origin = sf_monotonic_ns();
    for (size_t i = 0; i < N_SENSORS; ++i) {
        const sf_sensor_config cfg = {
            .id = (uint32_t)(i + 1),
            .frequency_mhz = freq[i],
            .duration_us = duration_us,
            .batch_interval_us = 100,
            .fault_rate = 0.01,
            .seed = 2024,
        };
        sf_sensor_init(&sensors[i], &cfg, &queues[i], origin);
        CHECK(sf_sensor_start(&sensors[i]) == 0);
    }

    uint64_t received = 0, last_ts = 0, order_errors = 0, range_errors = 0;
    sf_sample buf[512];
    sf_backoff b;
    sf_backoff_reset(&b);
    for (;;) {
        const size_t n = sf_spsc_pop_bulk(&sink, buf, 512);
        if (n == 0) {
            if (sf_spsc_drained(&sink))
                break;
            sf_backoff_pause(&b);
            continue;
        }
        for (size_t i = 0; i < n; ++i) {
            if (received > 0 && buf[i].timestamp_us <= last_ts)
                ++order_errors;
            last_ts = buf[i].timestamp_us;
            ++received;
            if (sf_fusion_push(&fusion, buf[i].value) == SF_PUSH_ACCEPTED &&
                sf_fusion_full(&fusion)) {
                const double f1 = sf_fusion_geometric_mean(&fusion);
                const double f2 = sf_fusion_pairwise_strength(&fusion, SF_F2_NORM_PAIR_MEAN);
                range_errors += !(f1 >= 0.0 && f1 <= 100.0) || !(f2 >= 0.0 && f2 <= 100.0);
            }
        }
    }

    uint64_t generated = 0, discarded = 0, faults = 0;
    for (size_t i = 0; i < N_SENSORS; ++i) {
        sf_sensor_join(&sensors[i]);
        CHECK(sensors[i].generated == sf_sensor_expected_samples(freq[i], duration_us));
        generated += sensors[i].generated;
        discarded += agg.discarded[i];
        faults += sensors[i].faults_injected;
    }
    sf_aggregator_join(&agg);

    CHECK(order_errors == 0);
    CHECK(range_errors == 0);
    CHECK(received == agg.emitted);
    CHECK(received + discarded == generated);
    CHECK(received == distinct_timestamps(freq, N_SENSORS, duration_us));
    CHECK(agg.discarded[3] == 0);
    CHECK(faults > 0);
    CHECK(fusion.rejected > 0 && fusion.rejected <= faults);
    CHECK(fusion.accepted + fusion.rejected == received);

    sf_fusion_destroy(&fusion);
    for (size_t i = 0; i < N_SENSORS; ++i)
        sf_spsc_destroy(&queues[i]);
    sf_spsc_destroy(&sink);
}

int main(void)
{
    RUN_TEST(test_queue_capacity_and_order);
    RUN_TEST(test_queue_wraparound_bulk);
    RUN_TEST(test_queue_threaded_fifo);
    RUN_TEST(test_sensor_timestamps);
    RUN_TEST(test_aggregator_merge_and_conflicts);
    RUN_TEST(test_aggregator_empty_sources);
    RUN_TEST(test_end_to_end_pipeline);
    return TEST_REPORT();
}
