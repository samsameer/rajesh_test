#ifndef SF_SAMPLE_H
#define SF_SAMPLE_H

#include <stdint.h>

typedef struct sf_sample {
    uint64_t timestamp_us;
    double value;
    uint32_t sensor_id;
} sf_sample;

#endif
