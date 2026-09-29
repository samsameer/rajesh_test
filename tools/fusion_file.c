#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sf/fusion.h"

typedef enum norm_selection {
    SELECT_FORMULA,
    SELECT_PAIR_MEAN,
    SELECT_BOTH,
} norm_selection;

typedef struct value_list {
    double *data;
    size_t size;
    size_t capacity;
} value_list;

typedef struct file_stats {
    size_t lines;
    size_t blank;
    size_t malformed;
} file_stats;

static void print_usage(FILE *out, const char *prog)
{
    fprintf(out,
            "Usage: %s [options] FILE...\n"
            "\n"
            "Loads one reading per line and prints both fusion functions.\n"
            "Lines with several comma/semicolon/whitespace separated fields use the last field.\n"
            "\n"
            "  -n, --window N       use only the N most recent valid readings\n"
            "                       (default: every valid reading in the file)\n"
            "  -m, --f2-norm MODE   formula | pair-mean | both (default formula)\n"
            "  -p, --precision P    digits after the decimal point (default 10)\n"
            "  -h, --help           show this help\n",
            prog);
}

static bool list_push(value_list *l, double v)
{
    if (l->size == l->capacity) {
        const size_t cap = l->capacity ? l->capacity * 2 : 4096;
        double *grown = realloc(l->data, cap * sizeof *grown);
        if (!grown)
            return false;
        l->data = grown;
        l->capacity = cap;
    }
    l->data[l->size++] = v;
    return true;
}

static bool is_separator(char c)
{
    return c == ',' || c == ';' || isspace((unsigned char)c);
}

static int parse_line(char *line, double *out)
{
    size_t len = strlen(line);
    while (len > 0 && is_separator(line[len - 1]))
        line[--len] = '\0';
    if (len == 0)
        return 0;

    size_t start = len;
    while (start > 0 && !is_separator(line[start - 1]))
        --start;

    const char *token = line + start;
    char *end = NULL;
    errno = 0;
    const double v = strtod(token, &end);
    if (end == token || *end != '\0' || errno == ERANGE)
        return -1;
    *out = v;
    return 1;
}

static bool read_line(FILE *fp, char **line, size_t *cap)
{
    if (!*line) {
        *cap = 256;
        *line = malloc(*cap);
        if (!*line)
            return false;
    }
    size_t len = 0;
    for (;;) {
        if (!fgets(*line + len, (int)(*cap - len), fp))
            return len > 0;
        len += strlen(*line + len);
        if (len > 0 && (*line)[len - 1] == '\n')
            return true;
        if (len + 1 < *cap)
            return true;
        char *grown = realloc(*line, *cap * 2);
        if (!grown)
            return false;
        *line = grown;
        *cap *= 2;
    }
}

static int load_file(const char *path, value_list *values, file_stats *stats)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char *line = NULL;
    size_t cap = 0;
    int rc = 0;
    while (read_line(fp, &line, &cap)) {
        ++stats->lines;
        double v;
        const int r = parse_line(line, &v);
        if (r == 0) {
            ++stats->blank;
        } else if (r < 0) {
            ++stats->malformed;
        } else if (!list_push(values, v)) {
            fprintf(stderr, "error: out of memory reading '%s'\n", path);
            rc = -1;
            break;
        }
    }
    if (rc == 0 && ferror(fp)) {
        fprintf(stderr, "error: read failure on '%s'\n", path);
        rc = -1;
    }
    free(line);
    fclose(fp);
    return rc;
}

static int process_file(const char *path, size_t window_opt, norm_selection sel, int precision)
{
    value_list values = { 0 };
    file_stats stats = { 0 };
    if (load_file(path, &values, &stats) != 0) {
        free(values.data);
        return -1;
    }

    size_t valid = 0;
    for (size_t i = 0; i < values.size; ++i)
        valid += sf_fusion_is_valid_input(values.data[i]);

    if (valid == 0) {
        fprintf(stderr, "error: '%s' contains no valid readings\n", path);
        free(values.data);
        return -1;
    }

    const size_t window = window_opt ? window_opt : valid;
    sf_fusion fusion;
    if (sf_fusion_init(&fusion, window) != 0) {
        fprintf(stderr, "error: cannot allocate window of %zu\n", window);
        free(values.data);
        return -1;
    }
    for (size_t i = 0; i < values.size; ++i)
        sf_fusion_push(&fusion, values.data[i]);

    printf("file: %s\n", path);
    printf("lines: total=%zu numeric=%zu blank=%zu malformed=%zu\n", stats.lines, values.size,
           stats.blank, stats.malformed);
    printf("readings: accepted=%" PRIu64 " rejected=%" PRIu64 " window=%zu used=%zu\n",
           fusion.accepted, fusion.rejected, window, sf_fusion_size(&fusion));
    printf("fusion function 1 = %.*f\n", precision, sf_fusion_geometric_mean(&fusion));
    if (sel == SELECT_BOTH) {
        printf("fusion function 2 [formula] = %.*f\n", precision,
               sf_fusion_pairwise_strength(&fusion, SF_F2_NORM_FORMULA));
        printf("fusion function 2 [pair-mean] = %.*f\n", precision,
               sf_fusion_pairwise_strength(&fusion, SF_F2_NORM_PAIR_MEAN));
    } else {
        const sf_f2_norm norm = sel == SELECT_PAIR_MEAN ? SF_F2_NORM_PAIR_MEAN : SF_F2_NORM_FORMULA;
        printf("fusion function 2 = %.*f\n", precision, sf_fusion_pairwise_strength(&fusion, norm));
    }

    sf_fusion_destroy(&fusion);
    free(values.data);
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option long_opts[] = {
        { "window", required_argument, NULL, 'n' },
        { "f2-norm", required_argument, NULL, 'm' },
        { "precision", required_argument, NULL, 'p' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 },
    };

    size_t window = 0;
    norm_selection sel = SELECT_FORMULA;
    int precision = 10;
    int c;
    while ((c = getopt_long(argc, argv, "n:m:p:h", long_opts, NULL)) != -1) {
        char *end = NULL;
        switch (c) {
        case 'n': {
            errno = 0;
            const unsigned long long v = strtoull(optarg, &end, 10);
            if (errno || *end != '\0' || v == 0 || *optarg == '-') {
                fprintf(stderr, "error: window must be a positive integer\n");
                return EXIT_FAILURE;
            }
            window = (size_t)v;
            break;
        }
        case 'm':
            if (strcmp(optarg, "formula") == 0) {
                sel = SELECT_FORMULA;
            } else if (strcmp(optarg, "pair-mean") == 0) {
                sel = SELECT_PAIR_MEAN;
            } else if (strcmp(optarg, "both") == 0) {
                sel = SELECT_BOTH;
            } else {
                fprintf(stderr, "error: f2-norm must be formula, pair-mean or both\n");
                return EXIT_FAILURE;
            }
            break;
        case 'p': {
            const long v = strtol(optarg, &end, 10);
            if (*end != '\0' || v < 0 || v > 17) {
                fprintf(stderr, "error: precision must be in [0, 17]\n");
                return EXIT_FAILURE;
            }
            precision = (int)v;
            break;
        }
        case 'h':
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        default:
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        print_usage(stderr, argv[0]);
        return EXIT_FAILURE;
    }

    int status = EXIT_SUCCESS;
    for (int i = optind; i < argc; ++i) {
        if (i > optind)
            putchar('\n');
        if (process_file(argv[i], window, sel, precision) != 0)
            status = EXIT_FAILURE;
    }
    return status;
}
