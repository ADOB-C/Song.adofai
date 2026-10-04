/* native Ogg Vorbis decoding via vendored stb_vorbis - see README.md */
#define _POSIX_C_SOURCE 200809L
#define STB_VORBIS_NO_STDIO            /* we decode from memory (mmap) */
#define STB_VORBIS_NO_PUSHDATA_API
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"                      /* Map / map_file / map_close / die */

#if defined(__GNUC__) || defined(__clang__)
#define alloca(x) __builtin_alloca(x)  /* stb_vorbis needs alloca under -std=c11 */
#endif

#include "stb_vorbis.c"                /* vendored, third_party/ */

#include "ogg.h"

int ogg_sniff(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[4];
    size_t got = fread(h, 1, sizeof h, f);
    fclose(f);
    return got == sizeof h && memcmp(h, "OggS", 4) == 0;
}

char *ogg_tag(const char *path, const char *key)
{
    Map m = map_file(path);
    char *out = NULL;
    if (m.n == 0 || m.n > (size_t)INT_MAX) {
        map_close(&m);
        return NULL;
    }
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(m.p, (int)m.n, &err, NULL);
    if (v) {
        stb_vorbis_comment vc = stb_vorbis_get_comment(v);
        size_t kl = strlen(key);
        for (int i = 0; i < vc.comment_list_length && !out; i++) {
            const char *c = vc.comment_list[i];
            if (strncasecmp(c, key, kl) == 0 && c[kl] == '=') out = strdup(c + kl + 1);
        }
        stb_vorbis_close(v);
    }
    map_close(&m);
    return out;
}

Pcm ogg_load(const char *path, int want)
{
    Map m = map_file(path);
    Pcm p = {0};
    if (m.n == 0 || m.n > (size_t)INT_MAX) {
        map_close(&m);
        return p;                       /* too big / empty -> ffmpeg */
    }
    int err = 0;
    stb_vorbis *v = stb_vorbis_open_memory(m.p, (int)m.n, &err, NULL);
    if (!v) {
        map_close(&m);
        return p;                       /* not Vorbis (e.g. Ogg Opus) -> ffmpeg */
    }
    stb_vorbis_info vi = stb_vorbis_get_info(v);
    int ch = vi.channels > 0 ? vi.channels : 1;
    int rate = (int)vi.sample_rate;
    size_t cap = 1 << 20, n = 0;
    float *buf = malloc(cap * sizeof *buf);
    if (!buf) die("out of memory");
    for (;;) {
        if ((n + 4096) * (size_t)ch > cap) {
            cap *= 2;
            float *nb = realloc(buf, cap * sizeof *buf);
            if (!nb) die("out of memory");
            buf = nb;
        }
        int got = stb_vorbis_get_samples_float_interleaved(
            v, ch, buf + n * (size_t)ch, (int)(4096 * ch));
        if (got <= 0) break;
        n += (size_t)got;
    }
    stb_vorbis_close(v);
    map_close(&m);
    if (n == 0 || rate <= 0) {
        free(buf);
        return p;
    }

    /* downmix to mono with 1/sqrt(n) per channel: this is ffmpeg's default
     * matrix (energy preserving), so .ogg and .opus land at the same level */
    double g = 1.0 / sqrt((double)ch);
    p.n = n;
    p.rate = rate;
    if (want == SAMPLE_S16) {
        p.s = malloc(n * 2);
        if (!p.s) die("out of memory");
        for (size_t i = 0; i < n; i++) {
            double acc = 0;
            for (int c = 0; c < ch; c++) acc += buf[i * (size_t)ch + c];
            p.s[i] = pcm_s16_from_f32((float)(acc * g));
        }
    } else {
        p.f = malloc(n * sizeof *p.f);
        if (!p.f) die("out of memory");
        for (size_t i = 0; i < n; i++) {
            double acc = 0;
            for (int c = 0; c < ch; c++) acc += buf[i * (size_t)ch + c];
            p.f[i] = (float)(acc * g);
        }
    }
    free(buf);
    snprintf(p.info.codec, sizeof p.info.codec, "vorbis");
    p.info.fmt = SAMPLE_F32;
    p.info.bits = 0;
    p.info.channels = ch;
    return p;
}
