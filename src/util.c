/* common utilities (die/now/mmap scanning/text escape/writer cookie) - see README.md */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE            /* funopen lives behind full visibility */
#endif
#if defined(__linux__)
#define _GNU_SOURCE                 /* fopencookie */
#endif
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
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "util.h"
#include "xz.h"
#include "zstd.h"

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

int log_level = 1;

void log_msg(int level, const char *fmt, ...)
{
    if (level > log_level) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

int thread_count = 0;

unsigned codec_threads(void)
{
    if (thread_count > 0) return (unsigned)thread_count;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
}

unsigned long long free_bytes(const char *path)
{
    char dir[MAX_PATH_LEN];
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(dir, sizeof dir, ".");
    } else if (slash == path) {
        snprintf(dir, sizeof dir, "/");
    } else {
        size_t l = (size_t)(slash - path);
        if (l >= sizeof dir) l = sizeof dir - 1;
        memcpy(dir, path, l);
        dir[l] = '\0';
    }
    struct statvfs st;
    if (statvfs(dir, &st) != 0) return 0;
    return (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
}

Map map_file(const char *path)
{
    Map m = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat %s: %s", path, strerror(errno));
    m.n = (size_t)st.st_size;
    m.src = m.n;
    m.p = mmap(NULL, m.n, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m.p == MAP_FAILED) die("mmap %s: %s", path, strerror(errno));
    close(fd);
    return m;
}

Map map_open(const char *path)
{
    if (xz_sniff(path)) return xz_map(path);
    if (zstd_sniff(path)) return zstd_map(path);
    return map_file(path);
}

void map_close(Map *m)
{
    if (!m || !m->p) return;
    if (m->owned) free((void *)m->p);
    else munmap((void *)m->p, m->n);
    m->p = NULL;
    m->n = m->src = 0;
    m->owned = 0;
    m->fmt = MAP_PLAIN;
}

/* ---- compressed-writer cookie FILE* (funopen / fopencookie) ---- */

typedef struct {
    void *ctx;
    CookieWrite w;
    CookieFinish f;
    FILE *u;
} Cookie;

static int ck_write(void *p, const char *b, int n)      /* BSD writefn */
{
    Cookie *c = p;
    return c->w(c->ctx, b, (size_t)n, c->u) < 0 ? -1 : n;
}

static int ck_close(void *p)                            /* both platforms */
{
    Cookie *c = p;
    int rc = c->f(c->ctx, c->u);                        /* finish + free ctx */
    if (fclose(c->u) != 0) rc = -1;
    free(c);
    return rc;
}

#if defined(__APPLE__)
FILE *cookie_wopen(const char *path, void *ctx, CookieWrite w, CookieFinish f)
{
    FILE *u = fopen(path, "wb");
    if (!u) die("cannot write %s: %s", path, strerror(errno));
    Cookie *c = calloc(1, sizeof *c);
    if (!c) die("out of memory");
    c->ctx = ctx; c->w = w; c->f = f; c->u = u;
    FILE *fp = funopen(c, NULL, ck_write, NULL, ck_close);
    if (!fp) die("cannot wrap compressed stream for %s", path);
    return fp;
}
#elif defined(__linux__)
static ssize_t ck_write2(void *p, const char *b, size_t n)
{
    Cookie *c = p;
    return c->w(c->ctx, b, n, c->u) < 0 ? (ssize_t)-1 : (ssize_t)n;
}
FILE *cookie_wopen(const char *path, void *ctx, CookieWrite w, CookieFinish f)
{
    FILE *u = fopen(path, "wb");
    if (!u) die("cannot write %s: %s", path, strerror(errno));
    Cookie *c = calloc(1, sizeof *c);
    if (!c) die("out of memory");
    c->ctx = ctx; c->w = w; c->f = f; c->u = u;
    cookie_io_functions_t io = { NULL, ck_write2, NULL, ck_close };
    FILE *fp = fopencookie(c, "w", io);
    if (!fp) die("cannot wrap compressed stream for %s", path);
    return fp;
}
#else
#error "compressed writing needs funopen (macOS) or fopencookie (glibc)"
#endif

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

