/* .adofai parsing (settings meta / angleData / SetHitsound scan) - see README.md */
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

#include "chart.h"

void meta_parse(const Map *m, Meta *meta)
{
    memset(meta, 0, sizeof *meta);
    size_t win = m->n < (1u << 29) ? m->n : (1u << 29);
    const unsigned char *s = findb(m->p, win, "\"settings\"", 10);
    if (!s) return;
    const unsigned char *ob = findb(s + 10, win - (size_t)((s + 10) - m->p), "{", 1);
    if (!ob) return;
    /* find closing brace of the settings object (quote-aware) */
    const unsigned char *e = ob + 1, *end = m->p + win;
    int depth = 1, instr = 0, esc = 0;
    for (; e < end && depth > 0; e++) {
        unsigned char c = *e;
        if (instr) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') instr = 1;
        else if (c == '{') depth++;
        else if (c == '}') depth--;
    }
    if (depth > 0) return;
    /* parse the five fields of interest inside the object slice */
    struct { const char *key; int num; } want[] = {
        { "\"bpm\"", 1 }, { "\"hitsoundVolume\"", 1 },
        { "\"artist\"", 0 }, { "\"song\"", 0 }, { "\"author\"", 0 },
    };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        const unsigned char *q = ob;
        while (q < e) {
            const unsigned char *f = findb(q, (size_t)(e - q), want[k].key, strlen(want[k].key));
            if (!f) break;
            q = f + strlen(want[k].key);
            const unsigned char *vp = skip_ws(q, e);
            if (!(vp < e && *vp == ':')) continue;
            vp = skip_ws(vp + 1, e);
            if (vp >= e) break;
            if (want[k].num) {
                char *ep = NULL;
                double v = strtod((const char *)vp, &ep);
                if (ep == (const char *)vp) break;
                if (strcmp(want[k].key, "\"bpm\"") == 0) { meta->bpm = v; meta->has_bpm = 1; }
                else { meta->vol = v; meta->has_vol = 1; }
            } else if (*vp == '"') {
                char *dst = strcmp(want[k].key, "\"artist\"") == 0 ? meta->artist
                          : strcmp(want[k].key, "\"song\"") == 0 ? meta->song : meta->author;
                const unsigned char *r = vp + 1;
                size_t i = 0;
                while (r < e && *r != '"' && i + 1 < 256) {
                    if (*r == '\\' && r + 1 < e) r++;
                    dst[i++] = (char)*r++;
                }
                dst[i] = '\0';
            }
            break; /* matched the first occurrence of this key inside settings */
        }
    }
}

long angle_entries(const Map *m)
{
    const unsigned char *s = findb(m->p, m->n, "\"angleData\"", 11);
    if (!s) return 0;
    const unsigned char *b = findb(s, m->n - (size_t)(s - m->p), "[", 1);
    if (!b) return 0;
    long entries = 0;
    const unsigned char *p = b + 1, *end = m->p + m->n;
    int instr = 0, esc = 0;
    for (; p < end; p++) {
        unsigned char c = *p;
        if (instr) {
            if (esc) esc = 0;
            else if (c == '\\') esc = 1;
            else if (c == '"') instr = 0;
            continue;
        }
        if (c == '"') { instr = 1; continue; }
        if (c == ',') entries++;
        else if (c == ']') break;
        else if (c == '{' || c == '[') return 0; /* nested: unknown -> 0 */
    }
    return entries + 1;
}

void ev_push(Events *e, int64_t floor, double vol)
{
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 1 << 20;
        int64_t *nf = realloc(e->floor, e->cap * sizeof *e->floor);
        double *nv = realloc(e->vol, e->cap * sizeof *e->vol);
        if (!nf || !nv) die("out of memory");
        e->floor = nf;
        e->vol = nv;
    }
    e->floor[e->n] = floor;
    e->vol[e->n] = vol;
    e->n++;
}

void scan_events(const Map *m, Events *ev)
{
    const unsigned char *end = m->p + m->n;
    static const char ANCHOR[] = "\"SetHitsound\"";
    const unsigned char *p = m->p;
    for (;;) {
        const unsigned char *hit = findb(p, (size_t)(end - p), ANCHOR, sizeof ANCHOR - 1);
        if (!hit) break;
        /* walk back to the enclosing '{' (bounded) */
        const unsigned char *s = hit;
        size_t back = (size_t)(hit - m->p);
        if (back > 16384) back = 16384;
        const unsigned char *lim = hit - back;
        while (s > lim && *s != '{') s--;
        if (*s != '{') { p = hit + sizeof ANCHOR - 1; continue; }
        /* walk forward to matching '}' (quote-aware) */
        const unsigned char *e = s + 1;
        int depth = 1, instr = 0, esc = 0;
        for (; e < end && depth > 0; e++) {
            unsigned char c = *e;
            if (instr) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') instr = 0;
                continue;
            }
            if (c == '"') instr = 1;
            else if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        if (depth > 0) { p = hit + sizeof ANCHOR - 1; continue; }
        /* parse floor and hitsoundVolume inside [s, e) */
        int64_t floor = -1;
        double vol = 0;
        int have_floor = 0, have_vol = 0;
        const unsigned char *qf = findb(s, (size_t)(e - s), "\"floor\"", sizeof "\"floor\"" - 1);
        if (qf) {
            const unsigned char *vp = skip_ws(qf + 7, e);   /* "floor" = 7 bytes */
            if (vp < e && *vp == ':') {
                char *ep = NULL;
                vp = skip_ws(vp + 1, e);
                floor = strtoll((const char *)vp, &ep, 10);
                have_floor = ep != (const char *)vp;
            }
        }
        const unsigned char *qv = findb(s, (size_t)(e - s), "\"hitsoundVolume\"",
                                        sizeof "\"hitsoundVolume\"" - 1);
        if (qv) {
            const unsigned char *vp = skip_ws(qv + 16, e);  /* 14 chars + 2 quotes */
            if (vp < e && *vp == ':') {
                char *ep = NULL;
                vp = skip_ws(vp + 1, e);
                vol = strtod((const char *)vp, &ep);
                have_vol = ep != (const char *)vp;
            }
        }
        if (have_floor && have_vol)
            ev_push(ev, floor, vol);
        p = e; /* continue after this object */
    }
}

int chart_codec_f32(const Map *m)
{
    size_t win = m->n < (1u << 29) ? m->n : (1u << 29);
    const unsigned char *a = findb(m->p, win, "\"actions\"", 9);
    if (!a) return -1;
    size_t rest = m->n - (size_t)(a - m->p);
    size_t lim = rest < 4096 ? rest : 4096;   /* marker is the first action */
    if (findb(a, lim, "\"adofai-music:f32\"", 18)) return 1;
    if (findb(a, lim, "\"adofai-music:s16\"", 18)) return 0;
    return -1;
}

void chart_stream_info(const Map *m, size_t *samples, double *rate)
{
    Meta meta;
    meta_parse(m, &meta);
    long entries = angle_entries(m);
    Events ev = {0};
    scan_events(m, &ev);
    int64_t last_floor = ev.n ? ev.floor[ev.n - 1] : 0;
    int f32 = chart_codec_f32(m) == 1;
    size_t base = (f32 && entries > 0)
                      ? (size_t)entries
                      : (meta.has_vol && fabs(meta.vol) <= 50.0 && entries > 0)
                            ? (size_t)entries
                            : (entries > 0 ? (size_t)(entries - 1) : 0);
    *samples = base > (size_t)last_floor ? base : (size_t)last_floor;
    *rate = meta.has_bpm && meta.bpm > 0 ? meta.bpm / 60.0 : 44100.0;
    free(ev.floor);
    free(ev.vol);
}

