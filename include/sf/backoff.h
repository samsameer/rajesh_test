#ifndef SF_BACKOFF_H
#define SF_BACKOFF_H

typedef struct sf_backoff {
    unsigned attempts;
} sf_backoff;

static inline void sf_backoff_reset(sf_backoff *b)
{
    b->attempts = 0;
}

void sf_backoff_pause(sf_backoff *b);

#endif
