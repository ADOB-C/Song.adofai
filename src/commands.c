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
#include "zst.h"

int decode_cmd(const char *chart, const char *out, double gain)
{
    double t0 = now_s();
    Audio a = audio_decode(chart);
    if (gain != 1.0) {
        for (size_t i = 0; i < a.total; i++) {
            int64_t c = llround(a.s[i] * gain);
            if (c < -32768) c = -32768;
            if (c > 32767) c = 32767;
            a.s[i] = (int16_t)c;
        }
    }
    wav_write(out, a.s, a.total, a.rate);
    printf("decoded %zu samples (%.1f kHz, %.1fs) -> %s in %.1fs\n",
           a.total, a.rate / 1000.0, (double)a.total / a.rate, out, now_s() - t0);
    free(a.s);
    return 0;
}

int verify_cmd(const char *chart, const char *ref)
{
    double t0 = now_s();
    Audio a = audio_decode(chart);
    Pcm r = pcm_load(ref);
    if (a.total != r.n) {
        fprintf(stderr, "length mismatch: chart %zu vs ref %zu\n", a.total, r.n);
        free(a.s);
        free(r.s);
        return 1;
    }
    size_t mism = 0;
    for (size_t i = 0; i < a.total; i++)
        if (a.s[i] != r.s[i]) mism++;
    free(a.s);
    free(r.s);
    if (mism == 0) {
        printf("VERIFY OK: %zu/%zu samples identical (%.1fs)\n",
               a.total, r.n, now_s() - t0);
        return 0;
    }
    printf("VERIFY FAILED: %zu/%zu mismatches\n", mism, r.n);
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
    int64_t last_floor = ev.n ? ev.floor[ev.n - 1] : 0;
    size_t base = (meta.has_vol && fabs(meta.vol) <= 50.0 && entries > 0)
                      ? (size_t)entries
                      : (entries > 0 ? (size_t)(entries - 1) : 0);
    size_t total = base > (size_t)last_floor ? base : (size_t)last_floor;
    double rate = meta.has_bpm && meta.bpm > 0 ? meta.bpm / 60.0 : 44100.0;
    printf("artist: %s\n", meta.artist[0] ? meta.artist : "(none)");
    printf("song:   %s\n", meta.song[0] ? meta.song : "(none)");
    printf("author: %s\n", meta.author[0] ? meta.author : "(none)");
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

void sanitize_name(const char *in, char *out, size_t outsz)
{
    size_t i = 0, o = 0;
    int prev_space = 0;
    for (; in[i] && o + 1 < outsz; i++) {
        char c = in[i];
        if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 0x20) c = '-';
        if (c == ' ') {
            if (prev_space) continue;
            prev_space = 1;
        } else {
            prev_space = 0;
        }
        out[o++] = c;
    }
    while (o && (out[o - 1] == ' ' || out[o - 1] == '.')) o--;
    out[o] = '\0';
}

int encode_cmd(const char *input, const char *outpath, const char *outdir,
                      const char *title_opt, const char *artist_opt,
                      const char *xzopt, const char *zstdopt)
{
    double t0 = now_s();
    Pcm p = pcm_load(input);

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
    if (outpath) {
        snprintf(out, sizeof out, "%s", outpath);
    } else {
        char name[512];
        if (title[0]) {
            char t[256], a[256];
            sanitize_name(title, t, sizeof t);
            if (artist[0]) {
                sanitize_name(artist, a, sizeof a);
                snprintf(name, sizeof name, "%s - %s.adofai", a, t);
            } else {
                snprintf(name, sizeof name, "%s.adofai", t);
            }
        } else {
            const char *base = strrchr(input, '/');
            base = base ? base + 1 : input;
            const char *dot = strrchr(base, '.');
            size_t bl = dot ? (size_t)(dot - base) : strlen(base);
            char stem[256];
            if (bl >= sizeof stem) bl = sizeof stem - 1;
            memcpy(stem, base, bl);
            stem[bl] = '\0';
            sanitize_name(stem, name, sizeof name);
            snprintf(name, sizeof name, "%s.adofai", name);
        }
        if (outdir)
            snprintf(out, sizeof out, "%s/%s", outdir, name);
        else
            snprintf(out, sizeof out, "%s", name);
    }

    size_t l = strlen(out);
    int is_xz = l >= 3 && strcasecmp(out + l - 3, ".xz") == 0;
    int is_zst = !is_xz && ((l >= 4 && strcasecmp(out + l - 4, ".zst") == 0) ||
                            (l >= 5 && strcasecmp(out + l - 5, ".zstd") == 0));
    int compressed = is_xz || is_zst;
    FILE *f = is_xz ? xz_open(out, xz_parse_preset(xzopt))
             : is_zst ? zstd_open(out, zstd_parse_level(zstdopt))
                      : fopen(out, "wb");
    if (!f) die("cannot write %s: %s", out, strerror(errno));
    size_t ne = 0;
    size_t bytes = encode_core(f, p.s, p.n, p.rate, title, artist, &ne);
    if (fclose(f) != 0) die("write error on %s", out);

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
    return 0;
}

int selftest_cmd(void)
{
    /* fully in-memory: no temp files, no disk writes */
    const int rate = 44100, n = rate;
    int16_t *tone = malloc((size_t)n * 2);
    if (!tone) die("out of memory");
    for (int i = 0; i < n; i++) {
        double v = 32767.0 * 0.6 * sin(2.0 * 3.14159265358979323846 * 440.0 * i / n);
        if (i % 997 == 0) v = -v;
        tone[i] = (int16_t)lround(v);
    }

    char *cbuf = NULL;
    size_t csize = 0;
    FILE *mf = open_memstream(&cbuf, &csize);
    if (!mf) die("open_memstream failed");
    size_t ne = 0;
    encode_core(mf, tone, (size_t)n, rate, "Tone Test", "Artist", &ne);
    fclose(mf);

    Map m = { (const unsigned char *)cbuf, csize, 0, 0, 0 };
    Audio a = audio_decode_map(&m);
    int rc;
    if (a.total != (size_t)n) {
        printf("self-test: length %zu != %d\n", a.total, n);
        rc = 1;
    } else {
        size_t mism = 0;
        for (size_t i = 0; i < a.total; i++)
            if (a.s[i] != tone[i]) mism++;
        printf(mism == 0 ? "SELF-TEST PASSED (in-memory, %zu samples)\n"
                         : "SELF-TEST FAILED: %zu mismatches\n",
               mism == 0 ? a.total : mism);
        rc = mism == 0 ? 0 : 1;
    }
    free(a.s);
    free(cbuf);
    free(tone);
    (void)ne;
    return rc;
}

