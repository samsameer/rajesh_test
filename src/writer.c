#include "sf/writer.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SF_WRITER_BUFFER_BYTES = 1 << 20,
    SF_PREFIX_LEN = 16,
    SF_HMS_LEN = 10,
};

static const char k_f1_label[] = "fusion function 1 = ";
static const char k_f2_label[] = "fusion function 2 = ";

int sf_writer_open(sf_writer *w, const char *path)
{
    memset(w, 0, sizeof *w);
    w->fp = fopen(path, "w");
    if (!w->fp)
        return errno ? errno : EIO;
    w->buffer = malloc(SF_WRITER_BUFFER_BYTES);
    if (w->buffer)
        setvbuf(w->fp, w->buffer, _IOFBF, SF_WRITER_BUFFER_BYTES);
    w->cached_second = -1;
    return 0;
}

int sf_writer_close(sf_writer *w)
{
    int rc = 0;
    if (w->fp) {
        if (fflush(w->fp) != 0 || ferror(w->fp))
            rc = EIO;
        if (fclose(w->fp) != 0)
            rc = EIO;
    }
    free(w->buffer);
    memset(w, 0, sizeof *w);
    return rc;
}

static size_t writer_prefix(sf_writer *w, char *dst)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    if ((int64_t)ts.tv_sec != w->cached_second) {
        struct tm tm;
        const time_t secs = ts.tv_sec;
        localtime_r(&secs, &tm);
        snprintf(w->cached_prefix, sizeof w->cached_prefix, "[%02d:%02d:%02d.",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
        w->cached_second = (int64_t)ts.tv_sec;
    }

    const unsigned ms = (unsigned)(ts.tv_nsec / 1000000L);
    memcpy(dst, w->cached_prefix, SF_HMS_LEN);
    dst[10] = (char)('0' + ms / 100);
    dst[11] = (char)('0' + (ms / 10) % 10);
    dst[12] = (char)('0' + ms % 10);
    dst[13] = ']';
    dst[14] = ':';
    dst[15] = ' ';
    return SF_PREFIX_LEN;
}

void sf_writer_event(sf_writer *w, const char *fmt, ...)
{
    char line[512];
    size_t n = writer_prefix(w, line);

    va_list ap;
    va_start(ap, fmt);
    const int m = vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);

    if (m > 0)
        n += (size_t)m < sizeof line - n - 1 ? (size_t)m : sizeof line - n - 2;
    line[n++] = '\n';
    fwrite(line, 1, n, w->fp);
}

void sf_writer_text(sf_writer *w, const char *text)
{
    fputs(text, w->fp);
    fputc('\n', w->fp);
}

size_t sf_format_fixed6(char *dst, size_t size, double v)
{
    if (!isfinite(v) || v < 0.0 || v >= 1e12) {
        const int r = snprintf(dst, size, isfinite(v) ? "%.6e" : "%f", v);
        if (r < 0)
            return 0;
        return (size_t)r < size ? (size_t)r : size - 1;
    }

    uint64_t scaled = (uint64_t)(v * 1e6 + 0.5);
    uint64_t integral = scaled / 1000000ULL;
    uint64_t fraction = scaled % 1000000ULL;

    char digits[24];
    size_t nd = 0;
    do {
        digits[nd++] = (char)('0' + integral % 10);
        integral /= 10;
    } while (integral);

    size_t len = 0;
    while (nd)
        dst[len++] = digits[--nd];
    dst[len++] = '.';
    for (int d = 5; d >= 0; --d) {
        dst[len + (size_t)d] = (char)('0' + fraction % 10);
        fraction /= 10;
    }
    len += 6;
    dst[len] = '\0';
    return len;
}

void sf_writer_fusion(sf_writer *w, double f1, double f2)
{
    char line[192];
    size_t n = writer_prefix(w, line);

    memcpy(line + n, k_f1_label, sizeof k_f1_label - 1);
    n += sizeof k_f1_label - 1;
    n += sf_format_fixed6(line + n, 32, f1);
    line[n++] = '\n';

    memcpy(line + n, line, SF_PREFIX_LEN);
    n += SF_PREFIX_LEN;
    memcpy(line + n, k_f2_label, sizeof k_f2_label - 1);
    n += sizeof k_f2_label - 1;
    n += sf_format_fixed6(line + n, 32, f2);
    line[n++] = '\n';

    fwrite(line, 1, n, w->fp);
}
