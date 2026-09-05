/*
 * adofai-audio — lossless audio <-> .adofai "audio-as-chart" codec (C).
 *
 * Every audio sample becomes one ADOFAI tile; settings.bpm = sampleRate*60;
 * each sample is stored as hitsoundVolume = sample/655.36 (exact dyadic).
 * v2 layout: initial volume lives in settings.hitsoundVolume and SetHitsound
 * events are only emitted at sample-to-sample changes (ADOFAI volume
 * persists forward); angleData (all zeros) is one dense single line.
 * Older full-event charts decode fine too: layout detection via
 * settings.hitsoundVolume (|v|<=50 => v2, 100 => legacy).
 *
 * POSIX (mmap). ffmpeg/ffprobe optional: needed for non-WAV input and for
 * auto-naming from container tags.
 */
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

#define VERSION "2.1.0"
#define AUTHOR "Music.adofai (https://github.com/CHT-1192/Music.adofai)"
#define AUTHOR_JSON "\"Music.adofai (https://github.com/CHT-1192/Music.adofai)\""
#define MAX_PATH_LEN 4096

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "error: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* mmap                                                                */
/* ------------------------------------------------------------------ */

typedef struct { const unsigned char *p; size_t n; } Map;

static Map map_file(const char *path)
{
    Map m = {0};
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("cannot open %s: %s", path, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) die("fstat %s: %s", path, strerror(errno));
    m.n = (size_t)st.st_size;
    m.p = mmap(NULL, m.n, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m.p == MAP_FAILED) die("mmap %s: %s", path, strerror(errno));
    close(fd);
    return m;
}

static const unsigned char *findb(const unsigned char *hay, size_t n,
                                  const char *needle, size_t nn)
{
    if (nn == 0 || n < nn) return NULL;
    const unsigned char *end = hay + n - nn;
    for (const unsigned char *p = hay; p <= end; p++)
        if (*p == (unsigned char)needle[0] && memcmp(p, needle, nn) == 0)
            return p;
    return NULL;
}

static const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end)
{
    while (p < end && isspace(*p)) p++;
    return p;
}

/* ------------------------------------------------------------------ */
/* settings metadata                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    char artist[256], song[256], author[256];
    double bpm, vol;
    int has_bpm, has_vol;
} Meta;

static void meta_parse(const Map *m, Meta *meta)
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

/* count angleData entries (commas at depth 1 until closing bracket) */
static long angle_entries(const Map *m)
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

/* ------------------------------------------------------------------ */
/* event scan                                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    int64_t *floor;
    double *vol;
    size_t n, cap;
} Events;

static void ev_push(Events *e, int64_t floor, double vol)
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

/* anchor scan: every "SetHitsound" occurrence -> enclosing object -> keys */
static void scan_events(const Map *m, Events *ev)
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

/* ------------------------------------------------------------------ */
/* PCM input: WAV (stdlib) or ffmpeg                                   */
/* ------------------------------------------------------------------ */

typedef struct { int16_t *s; size_t n; int rate; } Pcm;

static Pcm pcm_read_wav(const char *path)
{
    Map m = map_file(path);
    Pcm p = {0};
    if (m.n < 44 || memcmp(m.p, "RIFF", 4) || memcmp(m.p + 8, "WAVE", 4))
        die("%s: not a RIFF/WAVE file", path);
    int rate = 0, ch = 0, bits = 0, is_pcm = 0;
    size_t pos = 12;
    const unsigned char *data = NULL;
    size_t dlen = 0;
    while (pos + 8 <= m.n) {
        uint32_t sz;
        memcpy(&sz, m.p + pos + 4, 4);
        if (memcmp(m.p + pos, "fmt ", 4) == 0) {
            uint16_t fmt, c, b;
            memcpy(&fmt, m.p + pos + 8, 2);
            memcpy(&c, m.p + pos + 10, 2);
            memcpy(&b, m.p + pos + 22, 2);
            is_pcm = (fmt == 1);
            ch = c;
            bits = b;
            memcpy(&rate, m.p + pos + 12, 4);
        } else if (memcmp(m.p + pos, "data", 4) == 0) {
            data = m.p + pos + 8;
            dlen = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (is_pcm && ch == 1 && bits == 16 && data && dlen && rate > 0) {
        p.n = dlen / 2;
        p.s = malloc(p.n * 2);
        if (!p.s) die("out of memory");
        memcpy(p.s, data, p.n * 2);
        p.rate = rate;
    }
    munmap((void *)m.p, m.n);
    return p; /* empty -> caller falls back to ffmpeg */
}

static Pcm pcm_ffmpeg(const char *path)
{
    char cmd[9000];
    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -i \"%s\" -f s16le -ac 1 pipe:1", path);
    FILE *fp = popen(cmd, "r");
    if (!fp) die("cannot run ffmpeg for %s", path);
    size_t cap = 1 << 20, len = 0;
    int16_t *buf = malloc(cap);
    if (!buf) die("out of memory");
    for (;;) {
        if (len + 65536 > cap) {
            cap *= 2;
            int16_t *nb = realloc(buf, cap);
            if (!nb) die("out of memory");
            buf = nb;
        }
        size_t r = fread((char *)buf + len, 1, 65536, fp);
        len += r;
        if (r < 65536) break;
    }
    if (pclose(fp) != 0) die("ffmpeg failed on %s", path);
    Pcm p = {0};
    p.s = buf;
    p.n = len / 2;

    char cmd2[9000];
    snprintf(cmd2, sizeof cmd2,
             "ffprobe -v error -show_entries stream=sample_rate "
             "-of default=noprint_wrappers=1:nokey=1 \"%s\"", path);
    fp = popen(cmd2, "r");
    if (!fp) die("cannot run ffprobe for %s", path);
    char line[64] = {0};
    if (fgets(line, sizeof line, fp)) p.rate = atoi(line);
    pclose(fp);
    if (p.rate <= 0) die("cannot determine sample rate of %s", path);
    return p;
}

static Pcm pcm_load(const char *path)
{
    size_t l = strlen(path);
    if (l >= 4 && strcasecmp(path + l - 4, ".wav") == 0) {
        Pcm p = pcm_read_wav(path);
        if (p.s) return p;
    }
    return pcm_ffmpeg(path);
}

static char *ffprobe_tag(const char *path, const char *key)
{
    char cmd[9000];
    snprintf(cmd, sizeof cmd,
             "ffprobe -v error -show_entries format_tags=%s "
             "-of default=noprint_wrappers=1 \"%s\"", key, path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char line[4096];
    char *out = NULL;
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        if (strcmp(line, key) == 0) {
            size_t l2 = strlen(eq + 1);
            while (l2 && (eq[1 + l2 - 1] == '\n' || eq[1 + l2 - 1] == '\r'))
                l2--;
            out = strndup(eq + 1, l2);
            break;
        }
    }
    pclose(fp);
    return out;
}

/* ------------------------------------------------------------------ */
/* exact dyadic formatting: sample/655.36                              */
/* ------------------------------------------------------------------ */

static void volstr(int32_t x, char *out, size_t outsz)
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

static int16_t vol_to_sample(double v)
{
    double x = v * 655.36;
    int64_t s = llround(x);
    if (s < -32768) s = -32768;
    if (s > 32767) s = 32767;
    return (int16_t)s;
}

/* ------------------------------------------------------------------ */
/* decode                                                              */
/* ------------------------------------------------------------------ */

typedef struct { int16_t *s; size_t total; int rate; } Audio;

static Audio audio_decode_map(const Map *m)
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

    fprintf(stderr, "read %zu events, %zu samples\n", ev.n, total);
    free(ev.floor);
    free(ev.vol);

    Audio a = { out, total, rate };
    return a;
}

static Audio audio_decode(const char *path)
{
    Map m = map_file(path);
    Audio a = audio_decode_map(&m);
    munmap((void *)m.p, m.n);
    return a;
}

static void wav_write(const char *path, const int16_t *s, size_t n, int rate)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    uint32_t dlen = (uint32_t)(n * 2);
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t fsz = 36 + dlen;
    memcpy(hdr + 4, &fsz, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t c16 = 16;
    memcpy(hdr + 16, &c16, 4);
    uint16_t w = 1;
    memcpy(hdr + 20, &w, 2);
    memcpy(hdr + 22, &w, 2);                    /* mono */
    uint32_t dw = (uint32_t)rate;
    memcpy(hdr + 24, &dw, 4);
    dw = (uint32_t)rate * 2;
    memcpy(hdr + 28, &dw, 4);
    w = 2;
    memcpy(hdr + 32, &w, 2);
    w = 16;
    memcpy(hdr + 34, &w, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dlen, 4);
    fwrite(hdr, 1, 44, f);
    fwrite(s, 2, n, f);
    fclose(f);
}

static int decode_cmd(const char *chart, const char *out, double gain)
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

static int verify_cmd(const char *chart, const char *ref)
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

static int info_cmd(const char *chart)
{
    Map m = map_file(chart);
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
    printf("size: %.3f GB\n", m.n / 1e9);
    free(ev.floor);
    free(ev.vol);
    munmap((void *)m.p, m.n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* encode                                                              */
/* ------------------------------------------------------------------ */

static void esc_json(const char *in, char *out, size_t outsz)
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

static void sanitize_name(const char *in, char *out, size_t outsz)
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

static size_t encode_core(FILE *f, const int16_t *s, size_t n, int rate,
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

    /* angleData: one single dense line of n zeros */
    fputs("{\r\n\t\"angleData\": [", f);
    {
        const size_t K = 4096;
        char *seg = malloc(K * 2);
        for (size_t i = 0; i < K; i++) {
            seg[i * 2] = '0';
            seg[i * 2 + 1] = (i + 1 < K) ? ',' : '\0';
        }
        size_t full = n / K, first = 1;
        for (size_t i = 0; i < full; i++) {
            if (!first) fputc(',', f);
            fwrite(seg, 1, K * 2 - 1, f);
            first = 0;
        }
        size_t rem = n % K;
        if (rem) {
            if (!first) fputc(',', f);
            for (size_t i = 0; i < rem; i++) {
                fputc('0', f);
                if (i + 1 < rem) fputc(',', f);
            }
        }
        free(seg);
    }
    fprintf(f,
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
                fwrite(buf, 1, bl, f);
                bl = 0;
            }
            memcpy(buf + bl, line, (size_t)ln);
            bl += (size_t)ln;
        }
        fwrite(buf, 1, bl, f);
        free(buf);
    }
    fputs("\t]\r\n}\r\n", f);
    size_t bytes = (size_t)ftello(f);
    free(efloor);
    free(esamp);
    if (events_out) *events_out = ne;
    return bytes;
}

static int encode_cmd(const char *input, const char *outpath, const char *outdir,
                      const char *title_opt, const char *artist_opt)
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

    FILE *f = fopen(out, "wb");
    if (!f) die("cannot write %s: %s", out, strerror(errno));
    size_t ne = 0;
    size_t bytes = encode_core(f, p.s, p.n, p.rate, title, artist, &ne);
    fclose(f);

    printf("encoded %zu samples (%.1f kHz, %.1fs) -> %s (%.2f GB) in %.1fs\n",
           p.n, p.rate / 1000.0, (double)p.n / p.rate, out,
           (double)bytes / 1e9, now_s() - t0);
    size_t max_events = p.n > 1 ? p.n - 1 : 0;
    printf("  events: %zu (max %zu; saved %zu, %.1f%%)\n",
           ne, max_events, max_events - ne,
           max_events ? 100.0 * (double)(max_events - ne) / (double)max_events : 0.0);
    if (artist[0] || title[0])
        printf("  artist=%s  song=%s  author=%s\n", artist, title, AUTHOR);
    free(p.s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* self-test                                                           */
/* ------------------------------------------------------------------ */

static int selftest_cmd(void)
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

    Map m = { (const unsigned char *)cbuf, csize };
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

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static void usage(FILE *f)
{
    fprintf(f,
            "adofai-audio %s - lossless audio <-> .adofai \"audio-as-chart\" codec\n"
            "\n"
            "usage: adofai-audio <command> [options]\n"
            "\n"
            "commands:\n"
            "  encode INPUT [OUT]   audio -> .adofai (v2 change-event layout)\n"
            "      --artist NAME    override artist (default: ffprobe tags)\n"
            "      --title NAME     override song title\n"
            "      --out-dir DIR    directory for auto-named output\n"
            "  decode CHART OUT     .adofai -> mono s16 WAV\n"
            "      --gain F         output gain (1.0 = bit-exact)\n"
            "  verify CHART REF     bit-exact diff against reference audio\n"
            "  info CHART           print artist/song/author + audio info\n"
            "  self-test            roundtrip a synthetic tone\n"
            "\n"
            "options:\n"
            "  -h, --help           show this help\n"
            "      --version        print version\n"
            "\n"
            "format: bpm = sampleRate*60; hitsoundVolume = int16/655.36;\n"
            "events only at sample changes; angleData is one dense zero line.\n"
            "ffmpeg/ffprobe are needed only for non-WAV input or tags.\n",
            VERSION);
}

int main(int argc, char **argv)
{
    const char *cmd = NULL;
    const char *pos[4] = {0};
    int npos = 0;
    const char *artist = NULL, *title = NULL, *outdir = NULL;
    double gain = 1.0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("adofai-audio %s\n", VERSION);
            return 0;
        } else if (strcmp(a, "--artist") == 0 && i + 1 < argc) {
            artist = argv[++i];
        } else if (strcmp(a, "--title") == 0 && i + 1 < argc) {
            title = argv[++i];
        } else if (strcmp(a, "--out-dir") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strcmp(a, "--gain") == 0 && i + 1 < argc) {
            gain = atof(argv[++i]);
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "unknown option: %s\n", a);
            usage(stderr);
            return 2;
        } else if (!cmd) {
            cmd = a;
        } else if (npos < 4) {
            pos[npos++] = a;
        } else {
            fprintf(stderr, "too many arguments\n");
            return 2;
        }
    }
    if (!cmd) {
        usage(stderr);
        return 2;
    }
    if (strcmp(cmd, "encode") == 0) {
        if (npos < 1 || npos > 2) { usage(stderr); return 2; }
        return encode_cmd(pos[0], npos == 2 ? pos[1] : NULL, outdir, title, artist);
    }
    if (strcmp(cmd, "decode") == 0) {
        if (npos != 2) { usage(stderr); return 2; }
        return decode_cmd(pos[0], pos[1], gain);
    }
    if (strcmp(cmd, "verify") == 0) {
        if (npos != 2) { usage(stderr); return 2; }
        return verify_cmd(pos[0], pos[1]);
    }
    if (strcmp(cmd, "info") == 0) {
        if (npos != 1) { usage(stderr); return 2; }
        return info_cmd(pos[0]);
    }
    if (strcmp(cmd, "self-test") == 0) {
        return selftest_cmd();
    }
    fprintf(stderr, "unknown command: %s\n", cmd);
    usage(stderr);
    return 2;
}
