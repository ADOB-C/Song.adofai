/* core codec (dyadic formatting, decode rebuild, encode core) - see README.md */
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

#include "codec.h"

void volstr(int32_t x, char *out, size_t outsz)
{
    if (x == 0) { snprintf(out, outsz, "0"); return; }
    int neg = x < 0;
    uint32_t num = (uint32_t)(neg ? -(int64_t)x : (int64_t)x) * 25u; /* /16384 */
    uint32_t ip = num / 16384u, rem = num % 16384u;
    size_t i = 0;
    if (neg && i + 1 < outsz) out[i++] = '-';
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%u", ip);
    size_t tl = strlen(tmp);
    if (i + tl + 1 > outsz) tl = outsz - i - 1;
    memcpy(out + i, tmp, tl);
    i += tl;
    if (rem == 0) { out[i] = '\0'; return; }
    if (i + 1 < outsz) out[i++] = '.';
    while (rem && i + 1 < outsz) {
        rem *= 10;
        out[i++] = (char)('0' + rem / 16384u);
        rem %= 16384u;
    }
    out[i] = '\0';
}

int16_t vol_to_sample(double v)
{
    double x = v * 655.36;
    int64_t s = llround(x);
    if (s < -32768) s = -32768;
    if (s > 32767) s = 32767;
    return (int16_t)s;
}

Audio audio_decode_map(const Map *m)
{
    Events ev = {0};
    scan_events(m, &ev);
    Meta meta;
    meta_parse(m, &meta);
    long entries = angle_entries(m);

    size_t base = 0;
    if (meta.has_vol && fabs(meta.vol) <= 50.0 && entries > 0)
        base = (size_t)entries;                          /* v2: one per sample */
    else if (entries > 0)
        base = (size_t)(entries - 1);                    /* legacy: samples+1 */
    int64_t last_floor = ev.n ? ev.floor[ev.n - 1] : 0;
    size_t total = base > (size_t)last_floor ? base : (size_t)last_floor;
    int rate = meta.has_bpm && meta.bpm > 0 ? (int)(meta.bpm / 60.0 + 0.5) : 44100;

    int16_t *out = calloc(total ? total : 1, 2);
    if (!out) die("out of memory (%zu samples)", total);

    int32_t cur = meta.has_vol ? vol_to_sample(meta.vol) : 0;
    size_t pos = 0;
    for (size_t i = 0; i < ev.n; i++) {
        int64_t f = ev.floor[i];
        if (f < 1) continue;
        size_t want = (size_t)f - 1;
        if (want >= total) break;
        if (want > pos) {
            for (size_t k = pos; k < want; k++) out[k] = (int16_t)cur;
        }
        cur = vol_to_sample(ev.vol[i]);
        out[want] = (int16_t)cur;
        pos = want + 1;
    }
    for (; pos < total; pos++) out[pos] = (int16_t)cur;

    log_info("read %zu events, %zu samples\n", ev.n, total);
    free(ev.floor);
    free(ev.vol);

    Audio a = { out, total, rate };
    return a;
}

Audio audio_decode(const char *path)
{
    Map m = map_open(path);
    Audio a = audio_decode_map(&m);
    map_close(&m);
    return a;
}

/* counted writers: keep FILE* API, no ftello (xz cookie streams are not seekable) */
static size_t wput(FILE *f, const void *b, size_t n)
{
    if (n) fwrite(b, 1, n, f);
    return n;
}
static size_t wstr(FILE *f, const char *s) { return wput(f, s, strlen(s)); }
static size_t wch(FILE *f, int c) { fputc(c, f); return 1; }

size_t encode_core(FILE *f, const int16_t *s, size_t n, int rate,
                           const char *title, const char *artist,
                           size_t *events_out)
{
    size_t max_events = n > 1 ? n - 1 : 0;
    int64_t *efloor = malloc((max_events ? max_events : 1) * sizeof *efloor);
    int16_t *esamp = malloc((max_events ? max_events : 1) * 2);
    if (!efloor || !esamp) die("out of memory");
    size_t ne = 0;
    int16_t prev = s[0];
    for (size_t i = 1; i < n; i++) {
        if (s[i] != prev) {
            efloor[ne] = (int64_t)(i + 1);
            esamp[ne] = s[i];
            ne++;
            prev = s[i];
        }
    }

    char etitle[600], eartist[600], v0[32];
    esc_json(title, etitle, sizeof etitle);
    esc_json(artist, eartist, sizeof eartist);
    volstr(s[0], v0, sizeof v0);

    size_t nw = 0;                       /* bytes written (== plaintext size) */

    /* angleData: one single dense line of n zeros */
    nw += wstr(f, "{\r\n\t\"angleData\": [");
    {
        const size_t K = 4096;
        char *seg = malloc(K * 2);
        if (!seg) die("out of memory");
        for (size_t i = 0; i < K; i++) {
            seg[i * 2] = '0';
            seg[i * 2 + 1] = (i + 1 < K) ? ',' : '\0';
        }
        size_t full = n / K, first = 1;
        for (size_t i = 0; i < full; i++) {
            if (!first) nw += wch(f, ',');
            nw += wput(f, seg, K * 2 - 1);
            first = 0;
        }
        size_t rem = n % K;
        if (rem) {
            if (!first) nw += wch(f, ',');
            for (size_t i = 0; i < rem; i++) {
                nw += wch(f, '0');
                if (i + 1 < rem) nw += wch(f, ',');
            }
        }
        free(seg);
    }

    char setb[4096];
    int sl = snprintf(setb, sizeof setb,
            "],\r\n"
            "\t\"settings\": {\r\n"
            "\t\t\"version\": 13,\r\n"
            "\t\t\"bpm\": %d,\r\n"
            "\t\t\"artist\": \"%s\",\r\n"
            "\t\t\"song\": \"%s\",\r\n"
            "\t\t\"author\": %s,\r\n"
            "\t\t\"hitsound\": \"Kick\",\r\n"
            "\t\t\"hitsoundVolume\": %s,\r\n"
            "\t\t\"offset\": 0\r\n"
            "\t},\r\n"
            "\t\"actions\": [\r\n",
            rate * 60, eartist, etitle, AUTHOR_JSON, v0);
    if (sl < 0 || (size_t)sl >= sizeof setb) die("settings block overflow");
    nw += wput(f, setb, (size_t)sl);

    {
        char *buf = malloc(1 << 20);
        if (!buf) die("out of memory");
        size_t bl = 0;
        char line[512];
        for (size_t i = 0; i < ne; i++) {
            char vs[32];
            volstr(esamp[i], vs, sizeof vs);
            int ln = snprintf(line, sizeof line,
                              "\t\t{ \"floor\": %lld, \"eventType\": \"SetHitsound\", "
                              "\"gameSound\": \"Hitsound\", \"hitsound\": \"Kick\", "
                              "\"hitsoundVolume\": %s },\r\n",
                              (long long)efloor[i], vs);
            if (bl + (size_t)ln + 1 > (1 << 20)) {
                nw += wput(f, buf, bl);
                bl = 0;
            }
            memcpy(buf + bl, line, (size_t)ln);
            bl += (size_t)ln;
        }
        nw += wput(f, buf, bl);
        free(buf);
    }
    nw += wstr(f, "\t]\r\n}\r\n");
    free(efloor);
    free(esamp);
    if (events_out) *events_out = ne;
    return nw;
}

