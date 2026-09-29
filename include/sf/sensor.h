#ifndef SF_SENSOR_H
#define SF_SENSOR_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "sf/spsc_queue.h"

typedef struct sf_sensor_config {
    uint32_t id;
    uint64_t frequency_mhz;
    uint64_t duration_us;
    uint64_t batch_interval_us;
    double fault_rate;
    uint64_t seed;
} sf_sensor_config;

typedef struct sf_sensor {
    sf_sensor_config cfg;
    sf_spsc_queue *out;
    uint64_t origin_ns;
    uint64_t generated;
    uint64_t faults_injected;
    pthread_t thread;
    bool running;
} sf_sensor;

static inline uint64_t sf_sensor_timestamp_us(uint64_t frequency_mhz, uint64_t index)
{
    return index * 1000000000ULL / frequency_mhz;
}

static inline uint64_t sf_sensor_expected_samples(uint64_t frequency_mhz, uint64_t duration_us)
{
    return (duration_us * frequency_mhz + 999999999ULL) / 1000000000ULL;
}

void sf_sensor_init(sf_sensor *s, const sf_sensor_config *cfg, sf_spsc_queue *out,
                    uint64_t origin_ns);
void sf_sensor_run(sf_sensor *s);
int sf_sensor_start(sf_sensor *s);
void sf_sensor_join(sf_sensor *s);

#endif
