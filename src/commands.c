/* CLI subcommands (encode/decode/verify/info/self-test) - see README.md */
/* CLI commands and entry point - see README.md */
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

#include "cli.h"
#include "chart.h"
#include "pcm.h"
#include "xz.h"
#include "zstd.h"

int decode_cmd(const char *chart, const char *out, double gain, int want)
{
    double t0 = now_s();
    Audio a = audio_decode(chart);
    int native = a.f ? SAMPLE_F32 : SAMPLE_S16;
    int fmt = want == SAMPLE_AUTO ? native : want;
    unsigned long long need = (unsigned long long)a.total * (fmt == SAMPLE_F32 ? 4 : 2)
                              + 44 + (1u << 20);
    unsigned long long avail = free_bytes(out);
    if (avail && need > avail)
        die("not enough free space for %s: need ~%.1f MB, have %.1f MB",
            out, need / 1e6, avail / 1e6);
    if (fmt == SAMPLE_F32) {
        if (a.f) {
            wav_write_f32(out, a.f, a.total, a.rate, gain);
        } else {
            float *t = malloc(a.total * sizeof *t);
            if (!t) die("out of memory");
            for (size_t i = 0; i < a.total; i++) t[i] = (float)a.s[i];
            wav_write_f32(out, t, a.total, a.rate, gain);
            free(t);
        }
    } else {
        if (a.s) {
            wav_write(out, a.s, a.total, a.rate, gain);
        } else {
            int16_t *t = malloc(a.total * 2);
            if (!t) die("out of memory");
            for (size_t i = 0; i < a.total; i++)
                t[i] = pcm_s16_from_f32((float)(a.f[i] * gain));
            wav_write(out, t, a.total, a.rate, 1.0);
            free(t);
        }
    }
    printf("decoded %zu samples (%.1f kHz, %.1fs, %s) -> %s in %.1fs\n",
           a.total, a.rate / 1000.0, (double)a.total / a.rate,
           fmt == SAMPLE_F32 ? "float32" : "int16", out, now_s() - t0);
    free(a.s);
    free(a.f);
    return 0;
}

int verify_cmd(const char *chart, const char *ref)
{
    double t0 = now_s();
    Audio a = audio_decode(chart);
    Pcm r = pcm_load(ref, SAMPLE_AUTO);
    if (a.total != r.n) {
        fprintf(stderr, "length mismatch: chart %zu vs ref %zu\n", a.total, r.n);
        free(a.s);
        free(r.s);
        return 1;
    }
    size_t mism = 0;
    int mixed = (a.f == NULL) != (r.f == NULL);
    for (size_t i = 0; i < a.total; i++) {
        if (a.f && r.f) {
            if (memcmp(&a.f[i], &r.f[i], sizeof(float)) != 0) mism++;
        } else if (!a.f && !r.f) {
            if (a.s[i] != r.s[i]) mism++;
        } else if (a.f) {
            if (pcm_s16_from_f32(a.f[i]) != r.s[i]) mism++;
        } else {
            if (a.s[i] != pcm_s16_from_f32(r.f[i])) mism++;
        }
    }
    free(a.s);
    free(a.f);
    free(r.s);
    free(r.f);
    if (mism == 0) {
        printf("VERIFY OK: %zu/%zu samples identical%s (%.1fs)\n",
               a.total, r.n, mixed ? " after int16 quantization" : "", now_s() - t0);
        return 0;
    }
    printf("VERIFY FAILED: %zu/%zu mismatches%s\n", mism, r.n,
           mixed ? " after int16 quantization" : "");
    return 1;
}

int info_cmd(const char *chart)
{
    Map m = map_open(chart);
    Meta meta;
    meta_parse(&m, &meta);
    long entries = angle_entries(&m);
    Events ev = {0};
    scan_events(&m, &ev);
    size_t total = 0;
    double rate = 44100.0;
    chart_stream_info(&m, &total, &rate);
    printf("artist: %s\n", meta.artist[0] ? meta.artist : "(none)");
    printf("song:   %s\n", meta.song[0] ? meta.song : "(none)");
    printf("author: %s\n", meta.author[0] ? meta.author : "(none)");
    int cm = chart_codec_f32(&m);
    printf("codec: %s\n", cm == 1 ? "float32 (lossless)"
                          : cm == 0 ? "int16" : "int16 (no codec marker)");
    printf("events: %zu  angle entries: %ld\n", ev.n, entries);
    printf("sample rate (bpm/60): %.1f Hz  duration: %.3fs\n", rate, total / rate);
    if (m.owned)
        printf("size: %.3f GB text (%.1f MB .%s on disk)\n",
               m.n / 1e9, (double)m.src / 1e6, m.fmt == MAP_ZSTD ? "zst" : "xz");
    else
        printf("size: %.3f GB\n", m.n / 1e9);
    free(ev.floor);
    free(ev.vol);
    map_close(&m);
    return 0;
}

static int has_ext(const char *s, const char *ext)
{
    size_t ls = strlen(s), le = strlen(ext);
    return ls >= le && strcasecmp(s + ls - le, ext) == 0;
}

int encode_cmd(const char *input, const char *outpath,
                      const char *title_opt, const char *artist_opt,
                      const char *xzopt, const char *zstdopt, int fmt,
                      unsigned encflags, int want)
{
    double t0 = now_s();
    Pcm p = pcm_load(input, want);
    int f32 = p.f != NULL;
    if (f32) {
        float peak = 0.0f;
        for (size_t i = 0; i < p.n; i++) {
            if (!isfinite(p.f[i])) die("non-finite sample at index %zu", i);
            float a = fabsf(p.f[i]);
            if (a > peak) peak = a;
        }
        if (peak > 1.0f)
            log_info("note: peak %.4f exceeds 1.0; ADOFAI may clamp volumes >50\n", peak);
    }

    char title[512] = {0}, artist[512] = {0};
    if (title_opt) snprintf(title, sizeof title, "%s", title_opt);
    if (artist_opt) snprintf(artist, sizeof artist, "%s", artist_opt);
    if (!title[0]) {
        char *t = ffprobe_tag(input, "title");
        if (t) { snprintf(title, sizeof title, "%s", t); free(t); }
    }
    if (!artist[0]) {
        char *a = ffprobe_tag(input, "artist");
        if (a) { snprintf(artist, sizeof artist, "%s", a); free(a); }
    }

    char out[MAX_PATH_LEN];
    snprintf(out, sizeof out, "%s", outpath);

    int is_xz = fmt == OUT_XZ || (fmt == OUT_AUTO && has_ext(out, ".xz"));
    int is_zst = fmt == OUT_ZSTD ||
                 (fmt == OUT_AUTO && !is_xz &&
                  (has_ext(out, ".zst") || has_ext(out, ".zstd")));
    int compressed = is_xz || is_zst;

    /* disk-space guard: estimate the plaintext size from the sample stream
     * (event line ~<=140 B, angleData 2 B/sample) and refuse up front. */
    unsigned long long ne_est = 0;
    for (size_t i = 1; i < p.n; i++) {
        if (f32) {
            if (memcmp(&p.f[i], &p.f[i - 1], sizeof(float)) != 0) ne_est++;
        } else if (p.s[i] != p.s[i - 1]) {
            ne_est++;
        }
    }
    unsigned long long per_event = f32 ? 90 : 140;   /* conservative upper bounds */
    unsigned long long est = (unsigned long long)p.n * 2 + ne_est * per_event + 4096;
    unsigned long long need = compressed ? est / 16 + (4u << 20) : est + (1u << 20);
    unsigned long long avail = free_bytes(out);
    struct stat ost;
    if (stat(out, &ost) == 0 && S_ISREG(ost.st_mode))   /* will be truncated */
        avail += (unsigned long long)ost.st_size;
    if (avail && need > avail)
        die("not enough free space for %s: need ~%.2f GB, have %.2f GB%s", out,
            need / 1e9, avail / 1e9,
            compressed ? "" : " (or use -f xz / -f zst)");
    if (!compressed && est > (256ull << 20))
        log_info("note: plain chart will be ~%.2f GB; -f xz gives ~%.0f MB "
                 "(keep plain only if ADOFAI needs it)\n",
                 est / 1e9, (double)est / 1048576.0 / 16.0);
    log_verbose("estimated plaintext %.2f GB, free %.2f GB\n", est / 1e9, avail / 1e9);

    FILE *f = is_xz ? xz_open(out, xz_parse_preset(xzopt))
             : is_zst ? zstd_open(out, zstd_parse_level(zstdopt))
                      : fopen(out, "wb");
    if (!f) die("cannot write %s: %s", out, strerror(errno));
    size_t ne = 0;
    size_t bytes = encode_core(f, &p, title, artist, &ne, encflags);
    if (fclose(f) != 0) {
        remove(out);   /* don't leave a half-written chart behind */
        die("write error on %s (partial file removed; disk full?)", out);
    }

    double tel = now_s() - t0;
    if (compressed) {
        long long disk = -1;
        struct stat st;
        if (stat(out, &st) == 0) disk = (long long)st.st_size;
        printf("encoded %zu samples (%.1f kHz, %.1fs) -> %s\n"
               "  text %.2f GB -> %s %.2f MB on disk (%.0fx) in %.1fs\n",
               p.n, p.rate / 1000.0, (double)p.n / p.rate, out,
               (double)bytes / 1e9, is_zst ? "zstd" : "xz",
               disk < 0 ? 0.0 : (double)disk / 1e6,
               disk > 0 ? (double)bytes / (double)disk : 0.0, tel);
    } else {
        printf("encoded %zu samples (%.1f kHz, %.1fs) -> %s (%.2f GB) in %.1fs\n",
               p.n, p.rate / 1000.0, (double)p.n / p.rate, out,
               (double)bytes / 1e9, tel);
    }
    size_t max_events = p.n > 1 ? p.n - 1 : 0;
    printf("  events: %zu (max %zu; saved %zu, %.1f%%)\n",
           ne, max_events, max_events - ne,
           max_events ? 100.0 * (double)(max_events - ne) / (double)max_events : 0.0);
    if (artist[0] || title[0])
        printf("  artist=%s  song=%s  author=%s\n", artist, title, AUTHOR);
    free(p.s);
    free(p.f);
    return 0;
}

int selftest_cmd(void)
{
    /* fully in-memory: no temp files, no disk writes */
    const int rate = 44100, n = rate;
    int16_t *tone = malloc((size_t)n * 2);
    float *ftone = malloc((size_t)n * sizeof *ftone);
    if (!tone || !ftone) die("out of memory");
    for (int i = 0; i < n; i++) {
        double v = 32767.0 * 0.6 * sin(2.0 * 3.14159265358979323846 * 440.0 * i / n);
        if (i % 997 == 0) v = -v;
        tone[i] = (int16_t)lround(v);
        ftone[i] = (float)(tone[i] / 32768.0);
    }
    if (n > 1) ftone[1] = -0.0f;          /* sign of zero must survive */

    static const unsigned layouts[] = { 0, ENC_PRETTY, ENC_MINIMAL };
    static const char *lname[] = { "compact", "pretty", "minimal" };
    int rc = 0;
    for (size_t L = 0; L < sizeof layouts / sizeof layouts[0]; L++) {
        for (int f32 = 0; f32 < 2; f32++) {
            Pcm src = { f32 ? NULL : tone, f32 ? ftone : NULL, (size_t)n, rate };
            char *cbuf = NULL;
            size_t csize = 0;
            FILE *mf = open_memstream(&cbuf, &csize);
            if (!mf) die("open_memstream failed");
            size_t ne = 0;
            encode_core(mf, &src, "Tone Test", "Artist", &ne, layouts[L]);
            fclose(mf);

            Map m = { (const unsigned char *)cbuf, csize, 0, 0, 0 };
            Audio a = audio_decode_map(&m);
            size_t mism = 0;
            if (a.total != (size_t)n) {
                mism = a.total > (size_t)n ? a.total - (size_t)n : (size_t)n - a.total;
            } else if (f32) {
                for (size_t i = 0; i < a.total; i++)
                    if (memcmp(&a.f[i], &ftone[i], sizeof(float)) != 0) mism++;
            } else {
                for (size_t i = 0; i < a.total; i++)
                    if (a.s[i] != tone[i]) mism++;
            }
            printf("SELF-TEST %s: %s/%s (%zu samples, %zu B)\n",
                   mism ? "FAILED" : "PASSED", lname[L], f32 ? "f32" : "s16",
                   a.total, csize);
            if (mism) rc = 1;
            free(a.s);
            free(a.f);
            free(cbuf);
            (void)ne;
        }
    }
    free(tone);
    free(ftone);
    return rc;
}
