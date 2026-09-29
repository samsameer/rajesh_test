#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sf/aggregator.h"
#include "sf/backoff.h"
#include "sf/clock.h"
#include "sf/fusion.h"
#include "sf/sensor.h"
#include "sf/spsc_queue.h"
#include "sf/writer.h"

#define SENSOR_COUNT 4
#define MIN_WINDOW 4097u
#define DEFAULT_WINDOW 8192u
#define SENSOR_QUEUE_CAPACITY (1u << 15)
#define AGGREGATE_QUEUE_CAPACITY (1u << 17)
#define CONSUMER_BATCH 1024u

typedef struct sensor_spec {
    uint32_t id;
    uint64_t frequency_mhz;
} sensor_spec;

static const sensor_spec k_sensors[SENSOR_COUNT] = {
    { 1, 96000000ULL },
    { 2, 71000000ULL },
    { 3, 69900000ULL },
    { 4, 23000000ULL },
};

typedef struct app_options {
    size_t window;
    uint64_t duration_ms;
    uint64_t batch_interval_us;
    uint64_t stride;
    uint64_t seed;
    double fault_rate;
    sf_f2_norm norm;
    const char *output_path;
} app_options;

typedef struct app_stats {
    uint64_t consumed;
    uint64_t results;
    uint64_t written;
} app_stats;

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "\n"
            "  -n, --window N         number of most recent readings (N > 4096, default %u)\n"
            "  -d, --duration MS      acquisition time in milliseconds (default 5000)\n"
            "  -o, --output PATH      output file (default fusion_output.txt)\n"
            "  -m, --f2-norm MODE     fusion function 2 normalisation: formula | pair-mean\n"
            "                         (default formula)\n"
            "  -f, --fault-rate P     probability of an injected invalid reading, 0..1\n"
            "                         (default 0.0005)\n"
            "  -s, --seed S           random seed (default derived from the clock)\n"
            "  -k, --stride K         write every K-th fusion result (default 1)\n"
            "  -b, --batch-us US      sensor wake-up interval in microseconds (default 100)\n"
            "  -h, --help             show this help\n",
            prog, DEFAULT_WINDOW);
}

static bool parse_u64(const char *text, uint64_t min, uint64_t max, uint64_t *out)
{
    if (!text || !*text || *text == '-')
        return false;
    char *end = NULL;
    errno = 0;
    const unsigned long long v = strtoull(text, &end, 0);
    if (errno || *end != '\0' || v < min || v > max)
        return false;
    *out = (uint64_t)v;
    return true;
}

static bool parse_probability(const char *text, double *out)
{
    char *end = NULL;
    errno = 0;
    const double v = strtod(text, &end);
    if (errno || end == text || *end != '\0' || !(v >= 0.0 && v <= 1.0))
        return false;
    *out = v;
    return true;
}

static int parse_options(int argc, char **argv, app_options *opt)
{
    *opt = (app_options){
        .window = DEFAULT_WINDOW,
        .duration_ms = 5000,
        .batch_interval_us = 100,
        .stride = 1,
        .seed = sf_monotonic_ns(),
        .fault_rate = 0.0005,
        .norm = SF_F2_NORM_FORMULA,
        .output_path = "fusion_output.txt",
    };

    static const struct option long_opts[] = {
        { "window", required_argument, NULL, 'n' },
        { "duration", required_argument, NULL, 'd' },
        { "output", required_argument, NULL, 'o' },
        { "f2-norm", required_argument, NULL, 'm' },
        { "fault-rate", required_argument, NULL, 'f' },
        { "seed", required_argument, NULL, 's' },
        { "stride", required_argument, NULL, 'k' },
        { "batch-us", required_argument, NULL, 'b' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    int c;
    uint64_t v;
    while ((c = getopt_long(argc, argv, "n:d:o:m:f:s:k:b:h", long_opts, NULL)) != -1) {
        switch (c) {
        case 'n':
            if (!parse_u64(optarg, MIN_WINDOW, (uint64_t)1 << 28, &v)) {
                fprintf(stderr, "error: window must be an integer in [%u, %u]\n",
                        MIN_WINDOW, 1u << 28);
                return -1;
            }
            opt->window = (size_t)v;
            break;
        case 'd':
            if (!parse_u64(optarg, 1, 3600000, &opt->duration_ms)) {
                fprintf(stderr, "error: duration must be in [1, 3600000] ms\n");
                return -1;
            }
            break;
        case 'o':
            opt->output_path = optarg;
            break;
        case 'm':
            if (!sf_f2_norm_parse(optarg, &opt->norm)) {
                fprintf(stderr, "error: f2-norm must be 'formula' or 'pair-mean'\n");
                return -1;
            }
            break;
        case 'f':
            if (!parse_probability(optarg, &opt->fault_rate)) {
                fprintf(stderr, "error: fault-rate must be in [0, 1]\n");
                return -1;
            }
            break;
        case 's':
            if (!parse_u64(optarg, 0, UINT64_MAX, &opt->seed)) {
                fprintf(stderr, "error: invalid seed\n");
                return -1;
            }
            break;
        case 'k':
            if (!parse_u64(optarg, 1, UINT64_MAX, &opt->stride)) {
                fprintf(stderr, "error: stride must be >= 1\n");
                return -1;
            }
            break;
        case 'b':
            if (!parse_u64(optarg, 1, 1000000, &opt->batch_interval_us)) {
                fprintf(stderr, "error: batch-us must be in [1, 1000000]\n");
                return -1;
            }
            break;
        case 'h':
            print_usage(stdout, argv[0]);
            return 1;
        default:
            print_usage(stderr, argv[0]);
            return -1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "error: unexpected argument '%s'\n", argv[optind]);
        return -1;
    }
    return 0;
}

static void consume(sf_spsc_queue *aggregate, sf_fusion *fusion, sf_writer *writer,
                    const app_options *opt, app_stats *stats)
{
    sf_sample batch[CONSUMER_BATCH];
    sf_backoff backoff;
    sf_backoff_reset(&backoff);

    for (;;) {
        const size_t n = sf_spsc_pop_bulk(aggregate, batch, CONSUMER_BATCH);
        if (n == 0) {
            if (sf_spsc_drained(aggregate))
                break;
            sf_backoff_pause(&backoff);
            continue;
        }
        sf_backoff_reset(&backoff);
        stats->consumed += n;

        for (size_t i = 0; i < n; ++i) {
            if (sf_fusion_push(fusion, batch[i].value) != SF_PUSH_ACCEPTED ||
                !sf_fusion_full(fusion))
                continue;
            if (stats->results++ % opt->stride != 0)
                continue;
            sf_writer_fusion(writer, sf_fusion_geometric_mean(fusion),
                             sf_fusion_pairwise_strength(fusion, opt->norm));
            ++stats->written;
        }
    }
}

typedef struct pipeline {
    sf_spsc_queue sensor_queues[SENSOR_COUNT];
    sf_spsc_queue aggregate_queue;
    sf_sensor sensors[SENSOR_COUNT];
    sf_aggregator aggregator;
    sf_fusion fusion;
    size_t queues_ready;
    bool aggregate_ready;
    bool fusion_ready;
} pipeline;

static void pipeline_release(pipeline *p)
{
    if (p->fusion_ready)
        sf_fusion_destroy(&p->fusion);
    if (p->aggregate_ready)
        sf_spsc_destroy(&p->aggregate_queue);
    for (size_t i = 0; i < p->queues_ready; ++i)
        sf_spsc_destroy(&p->sensor_queues[i]);
}

static int pipeline_allocate(pipeline *p, size_t window)
{
    memset(p, 0, sizeof *p);
    for (size_t i = 0; i < SENSOR_COUNT; ++i) {
        if (sf_spsc_init(&p->sensor_queues[i], SENSOR_QUEUE_CAPACITY) != 0)
            return ENOMEM;
        p->queues_ready = i + 1;
    }
    if (sf_spsc_init(&p->aggregate_queue, AGGREGATE_QUEUE_CAPACITY) != 0)
        return ENOMEM;
    p->aggregate_ready = true;
    if (sf_fusion_init(&p->fusion, window) != 0)
        return ENOMEM;
    p->fusion_ready = true;
    return 0;
}

static void print_summary(const app_options *opt, const pipeline *p, const app_stats *stats,
                          double elapsed_s)
{
    fprintf(stderr, "window=%zu f2-norm=%s duration=%" PRIu64 "ms fault-rate=%g seed=%" PRIu64 "\n",
            opt->window, sf_f2_norm_name(opt->norm), opt->duration_ms, opt->fault_rate, opt->seed);
    for (size_t i = 0; i < SENSOR_COUNT; ++i) {
        fprintf(stderr,
                "  sensor %" PRIu32 " (%7.3f kHz): generated=%" PRIu64 " forwarded=%" PRIu64
                " discarded(conflict)=%" PRIu64 " injected-invalid=%" PRIu64 "\n",
                p->sensors[i].cfg.id, (double)p->sensors[i].cfg.frequency_mhz / 1e6,
                p->sensors[i].generated, p->aggregator.forwarded[i], p->aggregator.discarded[i],
                p->sensors[i].faults_injected);
    }
    fprintf(stderr,
            "  aggregated=%" PRIu64 " accepted=%" PRIu64 " rejected(invalid)=%" PRIu64
            " fusion-results=%" PRIu64 " written=%" PRIu64 " elapsed=%.3fs\n",
            stats->consumed, p->fusion.accepted, p->fusion.rejected, stats->results,
            stats->written, elapsed_s);
}

int main(int argc, char **argv)
{
    app_options opt;
    const int parsed = parse_options(argc, argv, &opt);
    if (parsed != 0)
        return parsed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    sf_writer writer;
    if (sf_writer_open(&writer, opt.output_path) != 0) {
        fprintf(stderr, "error: cannot open '%s': %s\n", opt.output_path, strerror(errno));
        return EXIT_FAILURE;
    }
    sf_writer_event(&writer, "Program started");

    static pipeline p;
    if (pipeline_allocate(&p, opt.window) != 0) {
        fprintf(stderr, "error: out of memory\n");
        pipeline_release(&p);
        sf_writer_close(&writer);
        return EXIT_FAILURE;
    }

    sf_aggregator_init(&p.aggregator, &p.aggregate_queue);
    for (size_t i = 0; i < SENSOR_COUNT; ++i)
        sf_aggregator_add_source(&p.aggregator, &p.sensor_queues[i], k_sensors[i].frequency_mhz);

    if (sf_aggregator_start(&p.aggregator) != 0) {
        fprintf(stderr, "error: failed to start aggregator thread\n");
        pipeline_release(&p);
        sf_writer_close(&writer);
        return EXIT_FAILURE;
    }

    bool failed = false;
    const uint64_t origin_ns = sf_monotonic_ns();
    for (size_t i = 0; i < SENSOR_COUNT; ++i) {
        const sf_sensor_config cfg = {
            .id = k_sensors[i].id,
            .frequency_mhz = k_sensors[i].frequency_mhz,
            .duration_us = opt.duration_ms * 1000ULL,
            .batch_interval_us = opt.batch_interval_us,
            .fault_rate = opt.fault_rate,
            .seed = opt.seed,
        };
        sf_sensor_init(&p.sensors[i], &cfg, &p.sensor_queues[i], origin_ns);
        if (sf_sensor_start(&p.sensors[i]) != 0) {
            fprintf(stderr, "error: failed to start sensor %" PRIu32 "\n", cfg.id);
            sf_spsc_close(&p.sensor_queues[i]);
            failed = true;
        }
    }

    app_stats stats = { 0 };
    consume(&p.aggregate_queue, &p.fusion, &writer, &opt, &stats);

    for (size_t i = 0; i < SENSOR_COUNT; ++i)
        sf_sensor_join(&p.sensors[i]);
    sf_aggregator_join(&p.aggregator);

    sf_writer_text(&writer, "Count of generated values for each sensor:");
    for (size_t i = 0; i < SENSOR_COUNT; ++i)
        sf_writer_event(&writer, "Sensor %" PRIu32 " = %" PRIu64, p.sensors[i].cfg.id,
                        p.sensors[i].generated);
    sf_writer_event(&writer, "Program finished");

    const double elapsed_s = (double)(sf_monotonic_ns() - origin_ns) / 1e9;
    if (sf_writer_close(&writer) != 0) {
        fprintf(stderr, "error: failed to write '%s'\n", opt.output_path);
        failed = true;
    }

    print_summary(&opt, &p, &stats, elapsed_s);
    pipeline_release(&p);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
