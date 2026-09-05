/* liblzma (xz) glue: chart <-> .xz — see README.md */
/* feature-test macros must precede every include */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE            /* funopen lives behind full visibility */
#endif
#if defined(__linux__)
#define _GNU_SOURCE                 /* fopencookie */
#endif
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <lzma.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "xz.h"

static const unsigned char XZ_MAGIC[6] = { 0xFD, '7', 'z', 'X', 'Z', 0x00 };

int xz_sniff(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    unsigned char h[6];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    return got == sizeof h && memcmp(h, XZ_MAGIC, sizeof h) == 0;
}

Map xz_map(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    size_t src = 0;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0) src = (size_t)st.st_size;

    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r = lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED);
    if (r != LZMA_OK) die("xz decoder init failed (%d)", (int)r);

    unsigned char in[1 << 16], out[1 << 16];
    size_t cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) die("out of memory");

    int eof = 0;
    while (!eof) {
        size_t got = fread(in, 1, sizeof in, f);
        eof = got < sizeof in;
        s.next_in = in;
        s.avail_in = got;
        do {
            s.next_out = out;
            s.avail_out = sizeof out;
            r = lzma_code(&s, eof ? LZMA_FINISH : LZMA_RUN);
            size_t have = sizeof out - s.avail_out;
            if (have) {
                if (len + have > cap) {
                    while (cap < len + have) cap *= 2;
                    unsigned char *nb = realloc(buf, cap);
                    if (!nb) die("out of memory (%zu bytes)", cap);
                    buf = nb;
                }
                memcpy(buf + len, out, have);
                len += have;
            }
            if (r == LZMA_STREAM_END) { eof = 1; break; }
            if (r != LZMA_OK) die("xz decode failed (%d): %s is corrupt", (int)r, path);
        } while (s.avail_out == 0);
    }
    fclose(f);
    lzma_end(&s);
    unsigned char *nb = realloc(buf, len ? len : 1); /* trim */
    if (nb) buf = nb;
    Map m = { buf, len, src, 1 };
    return m;
}

/* ---- write side: cookie FILE* that routes bytes through liblzma ---- */

typedef struct {
    FILE *u;                 /* underlying output file */
    lzma_stream s;
    unsigned char obuf[1 << 15];
    int bad;
} XzC;

static int feed(XzC *x, const void *data, size_t len)
{
    x->s.next_in = data;
    x->s.avail_in = len;
    while (x->s.avail_in > 0) {
        x->s.next_out = x->obuf;
        x->s.avail_out = sizeof x->obuf;
        lzma_ret r = lzma_code(&x->s, LZMA_RUN);
        size_t have = sizeof x->obuf - x->s.avail_out;
        if (have && fwrite(x->obuf, 1, have, x->u) != have) { x->bad = 1; return -1; }
        if (r != LZMA_OK) { x->bad = 1; return -1; }
    }
    return (int)len;
}

#if defined(__APPLE__)
static int xz_cw(void *c, const char *b, int n) { return feed((XzC *)c, b, (size_t)n); }
static int xz_cc(void *c)
{
    XzC *x = c;
    lzma_ret r;
    do {
        x->s.next_out = x->obuf;
        x->s.avail_out = sizeof x->obuf;
        r = lzma_code(&x->s, LZMA_FINISH);
        size_t have = sizeof x->obuf - x->s.avail_out;
        if (have && fwrite(x->obuf, 1, have, x->u) != have) x->bad = 1;
    } while (r == LZMA_OK);
    if (r != LZMA_STREAM_END) x->bad = 1;
    int rc = x->bad ? -1 : 0;
    if (fclose(x->u) != 0) rc = -1;
    lzma_end(&x->s);
    free(x);
    return rc;
}
FILE *xz_open(const char *path, uint32_t preset)
{
    FILE *u = fopen(path, "wb");
    if (!u) die("cannot write %s: %s", path, strerror(errno));
    XzC *x = calloc(1, sizeof *x);
    if (!x) die("out of memory");
    x->u = u;
    if (lzma_easy_encoder(&x->s, preset, LZMA_CHECK_CRC64) != LZMA_OK)
        die("lzma encoder init failed");
    FILE *f = funopen(x, NULL, xz_cw, NULL, xz_cc);
    if (!f) die("cannot wrap xz stream for %s", path);
    return f;
}
#elif defined(__linux__)
static ssize_t xz_cw2(void *c, const char *b, size_t n)
{
    return (ssize_t)feed((XzC *)c, b, n);
}
static int xz_cc2(void *c)
{
    XzC *x = c;
    lzma_ret r;
    do {
        x->s.next_out = x->obuf;
        x->s.avail_out = sizeof x->obuf;
        r = lzma_code(&x->s, LZMA_FINISH);
        size_t have = sizeof x->obuf - x->s.avail_out;
        if (have && fwrite(x->obuf, 1, have, x->u) != have) x->bad = 1;
    } while (r == LZMA_OK);
    if (r != LZMA_STREAM_END) x->bad = 1;
    int rc = x->bad ? -1 : 0;
    if (fclose(x->u) != 0) rc = -1;
    lzma_end(&x->s);
    free(x);
    return rc;
}
FILE *xz_open(const char *path, uint32_t preset)
{
    FILE *u = fopen(path, "wb");
    if (!u) die("cannot write %s: %s", path, strerror(errno));
    XzC *x = calloc(1, sizeof *x);
    if (!x) die("out of memory");
    x->u = u;
    if (lzma_easy_encoder(&x->s, preset, LZMA_CHECK_CRC64) != LZMA_OK)
        die("lzma encoder init failed");
    cookie_io_functions_t io = { NULL, xz_cw2, NULL, xz_cc2 };
    FILE *f = fopencookie(x, "w", io);
    if (!f) die("cannot wrap xz stream for %s", path);
    return f;
}
#else
#error "xz write support needs funopen (macOS) or fopencookie (glibc)"
#endif

uint32_t xz_parse_preset(const char *s)
{
    if (!s) return (uint32_t)XZ_LEVEL_DEFAULT;
    if (s[0] < '0' || s[0] > '9')
        die("bad --xz-level '%s' (use 0-9, optionally 'e' e.g. 6, 9e)", s);
    uint32_t p = (uint32_t)(s[0] - '0');
    const char *r = s + 1;
    if (*r == 'e') { p |= LZMA_PRESET_EXTREME; r++; }
    if (*r != '\0')
        die("bad --xz-level '%s' (use 0-9, optionally 'e' e.g. 6, 9e)", s);
    return p;
}
