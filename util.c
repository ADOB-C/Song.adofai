/* common utilities (die/now/mmap scanning/text escape) - see README.md */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "util.h"

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

Map map_file(const char *path)
{
    Map m = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat %s: %s", path, strerror(errno));
    m.n = (size_t)st.st_size;
    m.p = mmap(NULL, m.n, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m.p == MAP_FAILED) die("mmap %s: %s", path, strerror(errno));
    close(fd);
    return m;
}

const unsigned char *findb(const unsigned char *hay, size_t n,
                                  const char *needle, size_t nn)
{
    if (nn == 0 || n < nn) return NULL;
    const unsigned char *end = hay + n - nn;
    for (const unsigned char *p = hay; p <= end; p++)
        if (*p == (unsigned char)needle[0] && memcmp(p, needle, nn) == 0)
            return p;
    return NULL;
}

const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end)
{
    while (p < end && isspace(*p)) p++;
    return p;
}

void esc_json(const char *in, char *out, size_t outsz)
{
    size_t i = 0, o = 0;
    for (; in[i] && o + 6 < outsz; i++) {
        if (in[i] == '\\' || in[i] == '"') {
            out[o++] = '\\';
            out[o++] = in[i];
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

