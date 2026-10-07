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

/* float32 mode: sample x in [-1,1] <-> volume x*50; %.9g round-trips float32
 * exactly (including the sign of zero, which we deliberately keep). */
static void volstr_f32(float x, char *out, size_t outsz)
{
    snprintf(out, outsz, "%.9g", (double)x * 50.0);
}

Audio audio_decode_map(const Map *m)
{
    Events ev = {0};
    scan_events(m, &ev);
    Meta meta;
    meta_parse(m, &meta);
    long entries = angle_entries(m);
    int f32 = chart_sample_mode(m, &meta, &ev) == 1;

    size_t base = 0;
    if (f32 && entries > 0)
        base = (size_t)entries;                          /* f32: one per sample */
    else if (meta.has_vol && fabs(meta.vol) <= 50.0 && entries > 0)
        base = (size_t)entries;                          /* v2: one per sample */
    else if (entries > 0)
        base = (size_t)(entries - 1);                    /* legacy: samples+1 */
    int64_t last_floor = ev.n ? ev.floor[ev.n - 1] : 0;
    size_t total = base > (size_t)last_floor ? base : (size_t)last_floor;
    int rate = meta.has_bpm && meta.bpm > 0 ? (int)(meta.bpm / 60.0 + 0.5) : 44100;
    if (total == 0) total = 1;

    int16_t *out = f32 ? NULL : calloc(total, 2);
    float *outf = f32 ? calloc(total, sizeof *outf) : NULL;
    if (!out && !outf) die("out of memory (%zu samples)", total);

    double curv = meta.has_vol ? meta.vol : 0.0;
    int32_t cur = meta.has_vol ? vol_to_sample(meta.vol) : 0;
    size_t pos = 0;
    for (size_t i = 0; i < ev.n; i++) {
        int64_t f = ev.floor[i];
        if (f < 1) continue;
        size_t want = (size_t)f - 1;
        if (want >= total) break;
        if (f32) {
            float v = (float)(curv / 50.0);
            for (size_t k = pos; k < want; k++) outf[k] = v;
            curv = ev.vol[i];
            outf[want] = (float)(curv / 50.0);
        } else {
            for (size_t k = pos; k < want; k++) out[k] = (int16_t)cur;
            cur = vol_to_sample(ev.vol[i]);
            out[want] = (int16_t)cur;
        }
        pos = want + 1;
    }
    if (f32) {
        float v = (float)(curv / 50.0);
        for (; pos < total; pos++) outf[pos] = v;
    } else {
        for (; pos < total; pos++) out[pos] = (int16_t)cur;
    }

    log_info("read %zu events, %zu samples (%s)\n", ev.n, total,
             f32 ? "float32" : "int16");
    free(ev.floor);
    free(ev.vol);

    Audio a = { out, outf, total, rate };
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

size_t encode_core(FILE *f, const Pcm *p, const char *title, const char *artist,
                           size_t *events_out, unsigned flags)
{
    int pretty = (flags & ENC_PRETTY) != 0;
    int minimal = (flags & ENC_MINIMAL) != 0;
    int f32 = p->f != NULL;                 /* float32 chart (lossless) */
    const int16_t *s = p->s;
    const float *fs = p->f;
    size_t n = p->n;
    int rate = p->rate;
    size_t max_events = n > 1 ? n - 1 : 0;
    int64_t *efloor = malloc((max_events ? max_events : 1) * sizeof *efloor);
    int16_t *esamp = f32 ? NULL : malloc((max_events ? max_events : 1) * 2);
    float *ef32 = f32 ? malloc((max_events ? max_events : 1) * sizeof *ef32) : NULL;
    if (!efloor || (f32 ? !ef32 : !esamp)) die("out of memory");
    size_t ne = 0;
    if (f32) {
        float prev = fs[0];
        for (size_t i = 1; i < n; i++) {
            /* memcmp: +0.0 and -0.0 are different bit patterns and must both
             * survive the round trip */
            if (memcmp(&fs[i], &prev, sizeof prev) != 0) {
                efloor[ne] = (int64_t)(i + 1);
                ef32[ne] = fs[i];
                ne++;
                prev = fs[i];
            }
        }
    } else {
        int16_t prev = s[0];
        for (size_t i = 1; i < n; i++) {
            if (s[i] != prev) {
                efloor[ne] = (int64_t)(i + 1);
                esamp[ne] = s[i];
                ne++;
                prev = s[i];
            }
        }
    }

    char etitle[600], eartist[600], v0[32];
    esc_json(title, etitle, sizeof etitle);
    esc_json(artist, eartist, sizeof eartist);
    if (f32) volstr_f32(fs[0], v0, sizeof v0);
    else volstr(s[0], v0, sizeof v0);
    const char *codec_tag = f32 ? "f32" : "s16";

    size_t nw = 0;                       /* bytes written (== plaintext size) */

    /* angleData: one single dense line of n zeros */
    nw += wstr(f, pretty ? "{\r\n\t\"angleData\": [" : "{\"angleData\":[");
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
    int sl = pretty
        ? snprintf(setb, sizeof setb,
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
            rate * 60, eartist, etitle, AUTHOR_JSON, v0)
        : snprintf(setb, sizeof setb,
            "],\"settings\":{\"version\":13,\"bpm\":%d,\"artist\":\"%s\",\"song\":\"%s\","
            "\"author\":%s,\"hitsound\":\"Kick\",\"hitsoundVolume\":%s,\"offset\":0},"
            "\"actions\":[",
            rate * 60, eartist, etitle, AUTHOR_JSON, v0);
    if (sl < 0 || (size_t)sl >= sizeof setb) die("settings block overflow");
    nw += wput(f, setb, (size_t)sl);

    /* actions[0]: standard EditorComment event carrying the codec tag
     * actions[1]: same event type, carrying source-audio provenance */
    {
        char cbuf[512], esrc[256];
        esc_json(p->info.src[0] ? p->info.src : "?", esrc, sizeof esrc);
        int cl = pretty
            ? snprintf(cbuf, sizeof cbuf,
                       "\t\t{ \"floor\": 0, \"eventType\": \"EditorComment\", "
                       "\"comment\": \"adofai-music:%s\" }", codec_tag)
            : snprintf(cbuf, sizeof cbuf,
                       "{\"floor\":0,\"eventType\":\"EditorComment\","
                       "\"comment\":\"adofai-music:%s\"}", codec_tag);
        nw += wput(f, cbuf, (size_t)cl);
        nw += wstr(f, pretty ? ",\r\n" : ",");
        cl = pretty
            ? snprintf(cbuf, sizeof cbuf,
                       "\t\t{ \"floor\": 0, \"eventType\": \"EditorComment\", "
                       "\"comment\": \"adofai-music codec=%s fmt=%s bits=%d ch=%d "
                       "frames=%zu crc64=%016llx src=%s\" }",
                       p->info.codec[0] ? p->info.codec : "?", pcm_fmt_name(p->info.fmt),
                       p->info.bits, p->info.channels, n,
                       (unsigned long long)p->info.crc64, esrc)
            : snprintf(cbuf, sizeof cbuf,
                       "{\"floor\":0,\"eventType\":\"EditorComment\",\"comment\":"
                       "\"adofai-music codec=%s fmt=%s bits=%d ch=%d frames=%zu "
                       "crc64=%016llx src=%s\"}",
                       p->info.codec[0] ? p->info.codec : "?", pcm_fmt_name(p->info.fmt),
                       p->info.bits, p->info.channels, n,
                       (unsigned long long)p->info.crc64, esrc);
        nw += wput(f, cbuf, (size_t)cl);
        if (ne) nw += wstr(f, pretty ? ",\r\n" : ",");
        else nw += wstr(f, pretty ? "\r\n" : "");
    }

    {
        char *buf = malloc(1 << 20);
        if (!buf) die("out of memory");
        size_t bl = 0;
        char line[512];
        for (size_t i = 0; i < ne; i++) {
            char vs[32];
            if (f32) volstr_f32(ef32[i], vs, sizeof vs);
            else volstr(esamp[i], vs, sizeof vs);
            const char *fmt;
            if (pretty && minimal)
                fmt = "\t\t{ \"floor\": %lld, \"eventType\": \"SetHitsound\", "
                      "\"hitsoundVolume\": %s }";
            else if (pretty)
                fmt = "\t\t{ \"floor\": %lld, \"eventType\": \"SetHitsound\", "
                      "\"gameSound\": \"Hitsound\", \"hitsound\": \"Kick\", "
                      "\"hitsoundVolume\": %s }";
            else if (minimal)
                fmt = "{\"floor\":%lld,\"eventType\":\"SetHitsound\","
                      "\"hitsoundVolume\":%s}";
            else
                fmt = "{\"floor\":%lld,\"eventType\":\"SetHitsound\","
                      "\"gameSound\":\"Hitsound\",\"hitsound\":\"Kick\","
                      "\"hitsoundVolume\":%s}";
            int last = i + 1 == ne;
            const char *sep = last ? (pretty ? "\r\n" : "")
                                   : (pretty ? ",\r\n" : ",");
            int ln = snprintf(line, sizeof line, fmt, (long long)efloor[i], vs);
            size_t seplen = strlen(sep);
            if (bl + (size_t)ln + seplen + 1 > (1 << 20)) {
                nw += wput(f, buf, bl);
                bl = 0;
            }
            memcpy(buf + bl, line, (size_t)ln);
            bl += (size_t)ln;
            memcpy(buf + bl, sep, seplen);
            bl += seplen;
        }
        nw += wput(f, buf, bl);
        free(buf);
    }
    nw += wstr(f, pretty ? "\t]\r\n}\r\n" : "]}");
    free(efloor);
    free(esamp);
    free(ef32);
    if (events_out) *events_out = ne;
    return nw;
}

