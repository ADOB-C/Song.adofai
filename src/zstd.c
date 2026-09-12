/* libzstd glue: chart <-> .zst — see README.md */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE            /* _SC_NPROCESSORS_ONLN */
#endif
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zstd.h>

#include "zst.h"

static const unsigned char ZSTD_MAGIC[4] = { 0x28, 0xB5, 0x2F, 0xFD };

int zstd_sniff(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    unsigned char h[4];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    return got == sizeof h && memcmp(h, ZSTD_MAGIC, sizeof h) == 0;
}

Map zstd_map(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    size_t src = 0;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0) src = (size_t)st.st_size;

    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) die("zstd decoder init failed");
    size_t zr = ZSTD_initDStream(ds);
    if (ZSTD_isError(zr)) die("zstd decoder init failed: %s", ZSTD_getErrorName(zr));

    unsigned char in[1 << 16], out[1 << 16];
    size_t cap = 1 << 20, len = 0, last = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) die("out of memory");

    for (;;) {
        size_t got = fread(in, 1, sizeof in, f);
        if (got == 0) {
            if (last != 0) die("zstd decode failed: %s is truncated", path);
            break;
        }
        ZSTD_inBuffer ib = { in, got, 0 };
        while (ib.pos < ib.size) {
            ZSTD_outBuffer ob = { out, sizeof out, 0 };
            last = ZSTD_decompressStream(ds, &ob, &ib);
            if (ZSTD_isError(last))
                die("zstd decode failed (%s): %s", ZSTD_getErrorName(last), path);
            if (ob.pos) {
                if (len + ob.pos > cap) {
                    while (cap < len + ob.pos) cap *= 2;
                    unsigned char *nb = realloc(buf, cap);
                    if (!nb) die("out of memory (%zu bytes)", cap);
                    buf = nb;
                }
                memcpy(buf + len, out, ob.pos);
                len += ob.pos;
            }
        }
    }
    fclose(f);
    ZSTD_freeDStream(ds);
    unsigned char *nb = realloc(buf, len ? len : 1); /* trim */
    if (nb) buf = nb;
    Map m = { buf, len, src, 1, MAP_ZSTD };
    return m;
}

/* ---- write side: cookie FILE* compressing through libzstd ---- */

typedef struct {
    ZSTD_CCtx *c;
    unsigned char obuf[1 << 15];
} ZstdC;

static int zw(void *ctx, const void *b, size_t n, FILE *u)
{
    ZstdC *z = ctx;
    ZSTD_inBuffer ib = { b, n, 0 };
    while (ib.pos < ib.size) {
        ZSTD_outBuffer ob = { z->obuf, sizeof z->obuf, 0 };
        size_t r = ZSTD_compressStream2(z->c, &ob, &ib, ZSTD_e_continue);
        if (ZSTD_isError(r)) return -1;
        if (ob.pos && fwrite(z->obuf, 1, ob.pos, u) != ob.pos) return -1;
    }
    return 0;
}

static int zf(void *ctx, FILE *u)
{
    ZstdC *z = ctx;
    ZSTD_inBuffer empty = { NULL, 0, 0 };
    size_t r;
    do {
        ZSTD_outBuffer ob = { z->obuf, sizeof z->obuf, 0 };
        r = ZSTD_compressStream2(z->c, &ob, &empty, ZSTD_e_end);
        if (ZSTD_isError(r)) break;
        if (ob.pos && fwrite(z->obuf, 1, ob.pos, u) != ob.pos) { r = (size_t)-1; break; }
    } while (r != 0);
    int rc = (r == 0) ? 0 : -1;
    ZSTD_freeCCtx(z->c);
    free(z);
    return rc;
}

FILE *zstd_open(const char *path, int level)
{
    ZstdC *z = calloc(1, sizeof *z);
    if (!z) die("out of memory");
    z->c = ZSTD_createCCtx();
    if (!z->c) die("zstd encoder init failed");
    size_t r = ZSTD_CCtx_setParameter(z->c, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(r)) r = ZSTD_CCtx_setParameter(z->c, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(r)) die("zstd encoder setup failed: %s", ZSTD_getErrorName(r));
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu > 1)   /* ignore failure: single-thread-only builds stay valid */
        (void)ZSTD_CCtx_setParameter(z->c, ZSTD_c_nbWorkers, (int)ncpu);
    return cookie_wopen(path, z, zw, zf);
}

int zstd_parse_level(const char *s)
{
    if (!s) return ZSTD_LEVEL_DEFAULT;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (s[0] == '\0' || end == s || *end != '\0' || v < 1 || v > 22)
        die("bad --zstd-level '%s' (use 1-22, e.g. 19)", s);
    return (int)v;
}
