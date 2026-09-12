/* compression benchmark: per-preset ratio/speed + roundtrip, in memory only */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE            /* _SC_NPROCESSORS_ONLN */
#endif
#if defined(__linux__)
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#include <lzma.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <zstd.h>

#include "cli.h"
#include "chart.h"

#define XZ_BLOCK   (64u << 20)      /* same block shape as the encoder */
#define BUDGET_MB  100.0            /* size budget for a 3-minute chart */
#define BUDGET_SEC 180.0

typedef struct { unsigned char *p; size_t n, cap; } Buf;

static void buf_put(Buf *b, const void *src, size_t n)
{
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap : (1 << 16);
        while (cap < b->n + n) cap *= 2;
        unsigned char *np = realloc(b->p, cap);
        if (!np) die("out of memory");
        b->p = np;
        b->cap = cap;
    }
    memcpy(b->p + b->n, src, n);
    b->n += n;
}

static unsigned cpus(void)
{
#if LZMA_VERSION >= 50040000
    unsigned n = lzma_cputhreads();
    return n ? n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (unsigned)n : 1;
#endif
}

static int xz_enc(const unsigned char *in, size_t n, uint32_t preset, Buf *out)
{
    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r;
#if LZMA_VERSION >= 50040000
    lzma_mt mt;
    memset(&mt, 0, sizeof mt);
    mt.threads = cpus();
    mt.block_size = XZ_BLOCK;
    mt.preset = preset;
    mt.check = LZMA_CHECK_CRC64;
    r = lzma_stream_encoder_mt(&s, &mt);
#else
    r = lzma_easy_encoder(&s, preset, LZMA_CHECK_CRC64);
#endif
    if (r != LZMA_OK) return -1;
    unsigned char ob[1 << 16];
    s.next_in = in;
    s.avail_in = n;
    int action = n ? LZMA_RUN : LZMA_FINISH;
    for (;;) {
        s.next_out = ob;
        s.avail_out = sizeof ob;
        r = lzma_code(&s, action);
        buf_put(out, ob, sizeof ob - s.avail_out);
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) { lzma_end(&s); return -1; }
        if (s.avail_in == 0) action = LZMA_FINISH;
    }
    lzma_end(&s);
    return 0;
}

static int zstd_enc(const unsigned char *in, size_t n, int level, Buf *out)
{
    ZSTD_CCtx *c = ZSTD_createCCtx();
    if (!c) return -1;
    size_t r = ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level);
    if (!ZSTD_isError(r)) r = ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(r)) { ZSTD_freeCCtx(c); return -1; }
    if (cpus() > 1) (void)ZSTD_CCtx_setParameter(c, ZSTD_c_nbWorkers, (int)cpus());
    unsigned char ob[1 << 16];
    ZSTD_inBuffer ib = { in, n, 0 };
    for (;;) {
        int last = ib.pos == ib.size;
        ZSTD_outBuffer obb = { ob, sizeof ob, 0 };
        size_t r2 = ZSTD_compressStream2(c, &obb, &ib, last ? ZSTD_e_end : ZSTD_e_continue);
        if (ZSTD_isError(r2)) { ZSTD_freeCCtx(c); return -1; }
        buf_put(out, ob, obb.pos);
        if (last && r2 == 0) break;
    }
    ZSTD_freeCCtx(c);
    return 0;
}

static int xz_dec(const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r;
#if LZMA_VERSION >= 50040000
    lzma_mt mt;
    memset(&mt, 0, sizeof mt);
    mt.flags = LZMA_CONCATENATED;
    mt.threads = cpus();
#if LZMA_VERSION >= 50060000
    mt.memlimit_threading = UINT64_MAX;
    mt.memlimit_stop = UINT64_MAX;
#else
    mt.memlimit = UINT64_MAX;
#endif
    r = lzma_stream_decoder_mt(&s, &mt);
#else
    r = lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED);
#endif
    if (r != LZMA_OK) return -1;
    s.next_in = in;
    s.avail_in = n;
    s.next_out = out;
    s.avail_out = cap;
    for (;;) {
        r = lzma_code(&s, LZMA_FINISH);
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) { lzma_end(&s); return -1; }
    }
    size_t got = cap - s.avail_out;
    lzma_end(&s);
    return (int)got;
}

static int zstd_dec(const unsigned char *in, size_t n, unsigned char *out, size_t cap)
{
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds) return -1;
    size_t r = ZSTD_initDStream(ds);
    if (ZSTD_isError(r)) { ZSTD_freeDStream(ds); return -1; }
    ZSTD_inBuffer ib = { in, n, 0 };
    ZSTD_outBuffer ob = { out, cap, 0 };
    while (ib.pos < ib.size) {
        size_t before = ib.pos;
        r = ZSTD_decompressStream(ds, &ob, &ib);
        if (ZSTD_isError(r)) { ZSTD_freeDStream(ds); return -1; }
        if (r == 0 && ib.pos == ib.size) break;
        if (ib.pos == before && ob.pos == ob.size) break; /* no room, no progress */
    }
    size_t got = ob.pos;
    ZSTD_freeDStream(ds);
    return (int)got;
}

typedef struct {
    const char *codec;
    const char *level;
    int kind;          /* 0 = xz, 1 = zstd */
    uint32_t preset;
    int zlevel;
} Cand;

int bench_cmd(const char *chart, long slice_mb, int full)
{
    double t0 = now_s();
    Map m = map_open(chart);
    size_t samples = 0;
    double rate = 44100.0;
    chart_stream_info(&m, &samples, &rate);
    double dur = rate > 0 ? (double)samples / rate : 0.0;

    size_t off = 0, n = m.n;
    size_t slice = slice_mb > 0 ? (size_t)slice_mb * 1024u * 1024u : 0;
    if (!full && slice && slice < m.n) {
        n = slice;
        off = (m.n - n) / 2;        /* middle slice: skip zeros head + sparse tail */
    }
    const unsigned char *text = m.p + off;
    double bps = dur > 0 ? (double)m.n / dur : 0.0;   /* chart text bytes per audio sec */

    printf("bench: %s\n", chart);
    printf("  text %.3f GB, %.1f s @ %.1f Hz, %u threads\n",
           m.n / 1e9, dur, rate, cpus());
    if (off || n != m.n)
        printf("  slice %.0f MiB (%u x %u MiB xz blocks) at %.0f%% of text\n",
               n / 1048576.0, (unsigned)((n + XZ_BLOCK - 1) / XZ_BLOCK),
               (unsigned)(XZ_BLOCK / 1048576), m.n ? 100.0 * (double)off / (double)m.n : 0.0);
    if (dur > 0)
        printf("  budget %.0f MB / %.0f s  ->  text %.2f MB per audio sec\n",
               BUDGET_MB, BUDGET_SEC, bps / 1e6);
    printf("\n%-5s %-4s %10s %8s %10s %10s %9s %7s %s\n",
           "codec", "lvl", "size", "ratio", "comp MB/s", "dec MB/s",
           "est 3min", "budget", "roundtrip");

    /* matrix derived from measurements: xz 9e = smallest, xz 6 = knee (default),
     * zstd 19 ~= xz 6 size, zstd 3/12 = fast tiers; zstd 22 dominated (drop) */
    static const Cand cands[] = {
        { "xz", "3", 0, 3, 0 },
        { "xz", "6", 0, 6, 0 },
        { "xz", "9e", 0, 9 | LZMA_PRESET_EXTREME, 0 },
        { "zstd", "3", 1, 0, 3 },
        { "zstd", "12", 1, 0, 12 },
        { "zstd", "19", 1, 0, 19 },
    };
    unsigned char *dec = malloc(n + 64);
    if (!dec) die("out of memory");
    for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
        const Cand *c = &cands[i];
        Buf z = {0};
        double tc = now_s();
        int rc = c->kind ? zstd_enc(text, n, c->zlevel, &z)
                         : xz_enc(text, n, c->preset, &z);
        tc = now_s() - tc;
        double td = 0;
        int ok = 0;
        if (rc == 0) {
            double td0 = now_s();
            int got = c->kind ? zstd_dec(z.p, z.n, dec, n + 64)
                              : xz_dec(z.p, z.n, dec, n + 64);
            ok = (got == (int)n && memcmp(dec, text, n) == 0);
            td = now_s() - td0;
        }
        double est = (dur > 0 && n) ? (double)z.n / (double)n * bps * BUDGET_SEC : 0.0;
        if (rc != 0) {
            printf("%-5s %-4s %10s %8s %10s %10s %9s %7s %s\n",
                   c->codec, c->level, "-", "-", "-", "-", "-", "-", "COMPRESS ERROR");
        } else {
            printf("%-5s %-4s %8.2f MB %7.1fx %10.1f %10.1f %7.1f MB %7s %s\n",
                   c->codec, c->level, z.n / 1e6, (double)n / (double)z.n,
                   tc > 0 ? n / tc / 1e6 : 0.0, td > 0 ? n / td / 1e6 : 0.0,
                   est / 1e6, dur > 0 ? (est <= BUDGET_MB * 1e6 ? "PASS" : "FAIL") : "?",
                   ok ? "OK" : "FAILED");
        }
        fflush(stdout);
        free(z.p);
    }
    free(dec);
    map_close(&m);
    printf("\ntotal %.1fs, no disk writes\n", now_s() - t0);
    return 0;
}
