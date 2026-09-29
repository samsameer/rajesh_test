#include "sf/backoff.h"

#include <sched.h>

#include "sf/clock.h"

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define SF_CPU_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define SF_CPU_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define SF_CPU_RELAX() ((void)0)
#endif

enum {
    SF_BACKOFF_SPIN_LIMIT = 64,
    SF_BACKOFF_YIELD_LIMIT = 128,
    SF_BACKOFF_SLEEP_NS = 50000,
};

void sf_backoff_pause(sf_backoff *b)
{
    if (b->attempts < SF_BACKOFF_SPIN_LIMIT) {
        SF_CPU_RELAX();
        ++b->attempts;
    } else if (b->attempts < SF_BACKOFF_YIELD_LIMIT) {
        sched_yield();
        ++b->attempts;
    } else {
        sf_sleep_ns(SF_BACKOFF_SLEEP_NS);
    }
}
