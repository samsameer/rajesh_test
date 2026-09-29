#ifndef SF_WRITER_H
#define SF_WRITER_H

#include <stdint.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#define SF_PRINTF_LIKE(fmt, args) __attribute__((format(printf, fmt, args)))
#else
#define SF_PRINTF_LIKE(fmt, args)
#endif

typedef struct sf_writer {
    FILE *fp;
    char *buffer;
    int64_t cached_second;
    char cached_prefix[48];
} sf_writer;

int sf_writer_open(sf_writer *w, const char *path);
int sf_writer_close(sf_writer *w);
void sf_writer_event(sf_writer *w, const char *fmt, ...) SF_PRINTF_LIKE(2, 3);
void sf_writer_text(sf_writer *w, const char *text);
void sf_writer_fusion(sf_writer *w, double f1, double f2);

size_t sf_format_fixed6(char *dst, size_t size, double v);

#endif
