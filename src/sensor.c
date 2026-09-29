#include "sf/sensor.h"

#include <math.h>
#include <string.h>

#include "sf/backoff.h"
#include "sf/clock.h"
#include "sf/rng.h"

typedef struct sensor_signal {
    sf_rng rng;
    double level;
    double anchor;
} sensor_signal;

static double signal_next(sensor_signal *sig, double fault_rate, uint64_t *faults)
{
    if (fault_rate > 0.0 && sf_rng_unit(&sig->rng) < fault_rate) {
        ++*faults;
        switch (sf_rng_next(&sig->rng) & 3u) {
        case 0:
            return (double)NAN;
        case 1:
            return (double)INFINITY;
        case 2:
            return -1.0 - 10.0 * sf_rng_unit(&sig->rng);
        default:
            return 101.0 + 50.0 * sf_rng_unit(&sig->rng);
        }
    }

    const double step = (sf_rng_unit(&sig->rng) - 0.5) * 0.4;
    sig->level += 0.001 * (sig->anchor - sig->level) + step;
    if (sig->level < 0.0)
        sig->level = 0.0;
    else if (sig->level > 100.0)
        sig->level = 100.0;
    return sig->level;
}

void sf_sensor_init(sf_sensor *s, const sf_sensor_config *cfg, sf_spsc_queue *out,
                    uint64_t origin_ns)
{
    memset(s, 0, sizeof *s);
    s->cfg = *cfg;
    s->out = out;
    s->origin_ns = origin_ns;
}

void sf_sensor_run(sf_sensor *s)
{
    const uint64_t freq = s->cfg.frequency_mhz;
    const uint64_t total = sf_sensor_expected_samples(freq, s->cfg.duration_us);
    const uint64_t batch_us = s->cfg.batch_interval_us ? s->cfg.batch_interval_us : 1;

    sensor_signal sig = { .rng = { s->cfg.seed ^ (0xD1B54A32D192ED03ULL * (s->cfg.id + 1)) } };
    sig.anchor = 20.0 + 60.0 * sf_rng_unit(&sig.rng);
    sig.level = sig.anchor;

    sf_backoff backoff;
    uint64_t k = 0;
    while (k < total) {
        const uint64_t now_us = (sf_monotonic_ns() - s->origin_ns) / 1000ULL;
        uint64_t ts;
        while (k < total && (ts = sf_sensor_timestamp_us(freq, k)) <= now_us) {
            const sf_sample sample = {
                .timestamp_us = ts,
                .value = signal_next(&sig, s->cfg.fault_rate, &s->faults_injected),
                .sensor_id = s->cfg.id,
            };
            sf_backoff_reset(&backoff);
            while (!sf_spsc_try_push(s->out, &sample))
                sf_backoff_pause(&backoff);
            ++k;
        }
        if (k < total) {
            const uint64_t next_us = sf_sensor_timestamp_us(freq, k);
            uint64_t wait_us = next_us > now_us ? next_us - now_us : 0;
            if (wait_us < batch_us)
                wait_us = batch_us;
            sf_sleep_ns(wait_us * 1000ULL);
        }
    }

    s->generated = k;
    sf_spsc_close(s->out);
}

static void *sensor_thread_main(void *arg)
{
    sf_sensor_run(arg);
    return NULL;
}

int sf_sensor_start(sf_sensor *s)
{
    const int rc = pthread_create(&s->thread, NULL, sensor_thread_main, s);
    s->running = rc == 0;
    return rc;
}

void sf_sensor_join(sf_sensor *s)
{
    if (s->running) {
        pthread_join(s->thread, NULL);
        s->running = false;
    }
}
