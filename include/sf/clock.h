#ifndef SF_CLOCK_H
#define SF_CLOCK_H

#include <stdint.h>

uint64_t sf_monotonic_ns(void);
void sf_sleep_ns(uint64_t ns);

#endif
