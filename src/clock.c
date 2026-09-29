#include "sf/clock.h"

#include <errno.h>
#include <time.h>

uint64_t sf_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void sf_sleep_ns(uint64_t ns)
{
    struct timespec req = {
        .tv_sec = (time_t)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL),
    };
    struct timespec rem;
    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;
}
