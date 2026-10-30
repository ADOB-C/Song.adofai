/* PCM I/O (WAV reader/writer, ffmpeg fallback, ffprobe tags) - see README.md */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ogg.h"
#include "pcm.h"
#include "util.h"

static int16_t rd_s16(const unsigned char *p) { int16_t v; memcpy(&v, p, 2); return v; }
static float rd_f32(const unsigned char *p) { float v; memcpy(&v, p, 4); return v; }

const char *pcm_fmt_name(int fmt)
{
    switch (fmt) {
        case SAMPLE_S16: return "s16";
        case SAMPLE_S24: return "s24";
        case SAMPLE_S32: return "s32";
        case SAMPLE_F32: return "f32";
        case SAMPLE_F64: return "f64";
        case SAMPLE_U8:  return "u8";
        default:         return "?";
    }
}

int16_t pcm_s16_from_f32(float x)
{
    long r = lround((double)x * 32768.0);
    return (int16_t)(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
}

/* ffmpeg/libswresample default mono-downmix weights by channel count (measured):
 * 1=mono 2=stereo 3=2.1 4=4.0 5=5.0 6=5.1 7=6.1 8=7.1.  LFE is always excluded. */
int pcm_downmix_weights(int ch, double *w)
{
    const double F = 0.7071067811865476;
    static const double t4[] = {0.7071067811865476, 0.7071067811865476, 1.0, 0.5};
    static const double t5[] = {0.7071067811865476, 0.7071067811865476, 1.0, 0.5, 0.5};
    static const double t6[] = {0.7071067811865476, 0.7071067811865476, 1.0, 0.0, 0.5, 0.5};
    static const double t7[] = {0.7071067811865476, 0.7071067811865476, 1.0, 0.0, 0.5, 0.5, 0.5};
    static const double t8[] = {0.7071067811865476, 0.7071067811865476, 1.0, 0.0,
                                0.5, 0.5, 0.5, 0.5};
    const double *src = NULL;
    if (ch == 1) { w[0] = 1.0; return 1; }
    if (ch == 2) { w[0] = F; w[1] = F; return 2; }
    if (ch == 3) { w[0] = F; w[1] = F; w[2] = 0.0; return 3; }
    if (ch == 4) src = t4;
    else if (ch == 5) src = t5;
    else if (ch == 6) src = t6;
    else if (ch == 7) src = t7;
    else if (ch == 8) src = t8;
    if (src) {
        for (int i = 0; i < ch; i++) w[i] = src[i];
        return ch;
    }
    double e = 1.0 / sqrt((double)ch);          /* rare counts: equal weights */
    for (int i = 0; i < ch; i++) w[i] = e;
    return ch;
}

/* read a WAV natively: 16/24-bit PCM or 32-bit IEEE float, any channel count
 * (mono downmix uses pcm_downmix_weights). `want` may force a conversion.
 * An empty result means the caller should fall back to ffmpeg. */
Pcm pcm_read_wav(const char *path, int want)
{
    Map m = map_file(path);
    Pcm p = {0};
    if (m.n < 44 || memcmp(m.p, "RIFF", 4) || memcmp(m.p + 8, "WAVE", 4))
        die("%s: not a RIFF/WAVE file", path);
    int rate = 0, ch = 0, bits = 0, fmt = 0;
    size_t pos = 12;
    const unsigned char *data = NULL;
    size_t dlen = 0;
    while (pos + 8 <= m.n) {
        uint32_t sz;
        memcpy(&sz, m.p + pos + 4, 4);
        if (memcmp(m.p + pos, "fmt ", 4) == 0) {
            uint16_t f2, c, b;
            memcpy(&f2, m.p + pos + 8, 2);
            memcpy(&c, m.p + pos + 10, 2);
            memcpy(&b, m.p + pos + 22, 2);
            fmt = f2;
            ch = c;
            bits = b;
            memcpy(&rate, m.p + pos + 12, 4);
        } else if (memcmp(m.p + pos, "data", 4) == 0) {
            data = m.p + pos + 8;
            dlen = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
    if (data && (size_t)(m.p + m.n - data) < dlen)
        dlen = (size_t)(m.p + m.n - data);          /* trust the file size */
    int src_s16 = fmt == 1 && bits == 16;
    int src_s24 = fmt == 1 && bits == 24;
    int src_f32 = fmt == 3 && bits == 32;
    if (ch >= 1 && ch <= 64 && data && dlen && rate > 0 &&
        (src_s16 || src_s24 || src_f32)) {
        snprintf(p.info.codec, sizeof p.info.codec, "wav");
        p.info.fmt = src_s16 ? SAMPLE_S16 : src_s24 ? SAMPLE_S24 : SAMPLE_F32;
        p.info.bits = bits;
        p.info.channels = ch;
        size_t bps = src_f32 ? 4 : src_s24 ? 3 : 2;
        size_t n = dlen / (bps * (size_t)ch);      /* frames */
        int to_f32 = want == SAMPLE_F32 ||
                     (want == SAMPLE_AUTO && (src_f32 || src_s24));
        double w[64];
        pcm_downmix_weights(ch, w);
        p.n = n;
        p.rate = rate;
        if (to_f32) {
            p.f = malloc(n * sizeof *p.f);
            if (!p.f) die("out of memory");
            for (size_t i = 0; i < n; i++) {
                double acc = 0.0;
                for (int c = 0; c < ch; c++) {
                    const unsigned char *q = data + (i * (size_t)ch + c) * bps;
                    double v;
                    if (src_f32) v = rd_f32(q);
                    else if (src_s24) {
                        int32_t x = q[0] | (q[1] << 8) | (q[2] << 16);
                        if (x & 0x800000) x -= 0x1000000;
                        v = x / 8388608.0;
                    } else {
                        v = rd_s16(q) / 32768.0;
                    }
                    acc += w[c] * v;
                }
                p.f[i] = (float)acc;
            }
        } else {
            p.s = malloc(n * 2);
            if (!p.s) die("out of memory");
            for (size_t i = 0; i < n; i++) {
                double acc = 0.0;
                for (int c = 0; c < ch; c++) {
                    const unsigned char *q = data + (i * (size_t)ch + c) * bps;
                    double v;
                    if (src_f32) v = rd_f32(q);
                    else if (src_s24) {
                        int32_t x = q[0] | (q[1] << 8) | (q[2] << 16);
                        if (x & 0x800000) x -= 0x1000000;
                        v = x / 8388608.0;
                    } else {
                        v = rd_s16(q) / 32768.0;
                    }
                    acc += w[c] * v;
                }
                p.s[i] = pcm_s16_from_f32((float)acc);
            }
        }
    }
    map_close(&m);
    return p;
}

static int ffprobe_stream(const char *path, int *rate, char *sf, size_t sfsz, int *bits,
                          int *channels, char *codec, size_t codecsz)
{
    char cmd[16384], fpexe[MAX_PATH_LEN + 16];
    tool_path(fpexe, sizeof fpexe, "ffprobe");
    snprintf(cmd, sizeof cmd,
             "\"%s\" -v error -show_entries "
             "stream=sample_rate,sample_fmt,bits_per_raw_sample,channels,codec_name "
             "-of default=noprint_wrappers=1 \"%s\"", fpexe, path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    char line[128];
    *rate = 0;
    *bits = 0;
    *channels = 0;
    if (sfsz) sf[0] = '\0';
    if (codecsz) codec[0] = '\0';
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        if (strcmp(line, "sample_rate") == 0) *rate = atoi(val);
        else if (strcmp(line, "bits_per_raw_sample") == 0) *bits = atoi(val);
        else if (strcmp(line, "channels") == 0) *channels = atoi(val);
        else if (codecsz && strcmp(line, "codec_name") == 0) snprintf(codec, codecsz, "%s", val);
        else if (sfsz && strcmp(line, "sample_fmt") == 0) snprintf(sf, sfsz, "%s", val);
    }
    pclose(fp);
    return *rate > 0 ? 0 : -1;
}

Pcm pcm_ffmpeg(const char *path, int want)
{
    int rate = 0, bits = 0, channels = 0;
    char sf[32] = {0}, codec[24] = {0};
    if (ffprobe_stream(path, &rate, sf, sizeof sf, &bits, &channels, codec, sizeof codec) != 0)
        die("cannot determine sample format of %s (need ffmpeg/ffprobe; install it or pass -ffmpeg PATH)", path);
    Pcm info = {0};
    info.info.bits = bits;
    info.info.channels = channels;
    snprintf(info.info.codec, sizeof info.info.codec, "%s", codec);
    info.info.fmt = !strncmp(sf, "u8", 2)     ? SAMPLE_U8
                  : !strncmp(sf, "s16", 3)    ? SAMPLE_S16
                  : !strncmp(sf, "flt", 3)    ? SAMPLE_F32
                  : !strncmp(sf, "dbl", 3)    ? SAMPLE_F64
                  : !strncmp(sf, "s32", 3)    ? (bits == 24 ? SAMPLE_S24 : SAMPLE_S32)
                  : SAMPLE_S16;
    int is_flt = strncmp(sf, "flt", 3) == 0;
    int is_dbl = strncmp(sf, "dbl", 3) == 0;
    int is_s32 = strncmp(sf, "s32", 3) == 0;
    if (want == SAMPLE_AUTO) {
        /* never degrade silently: float32 only carries a 24-bit mantissa */
        if (is_dbl)
            die("%s is 64-bit float; a chart stores at most float32. "
                "pass -sample_fmt f32 to accept 32-bit storage", path);
        if (is_s32 && bits > 24)
            die("%s is %d-bit integer; float32 keeps only 24 bits. "
                "pass -sample_fmt f32 to accept 24-bit storage", path, bits);
    }
    int use_f32 = want == SAMPLE_F32 || (want == SAMPLE_AUTO && (is_flt || is_dbl || is_s32));
    char cmd[16384], ff[MAX_PATH_LEN + 16];
    tool_path(ff, sizeof ff, "ffmpeg");
    snprintf(cmd, sizeof cmd, "\"%s\" -v error -i \"%s\" -f %s -ac 1 pipe:1",
             ff, path, use_f32 ? "f32le" : "s16le");
    FILE *fp = popen(cmd, "r");
    if (!fp) die("cannot run ffmpeg for %s", path);
    size_t bps = use_f32 ? 4 : 2, cap = 1 << 20, len = 0;
    unsigned char *buf = malloc(cap);
    if (!buf) die("out of memory");
    for (;;) {
        if (len + 65536 > cap) {
            cap *= 2;
            unsigned char *nb = realloc(buf, cap);
            if (!nb) die("out of memory");
            buf = nb;
        }
        size_t r = fread(buf + len, 1, 65536, fp);
        len += r;
        if (r < 65536) break;
    }
    if (pclose(fp) != 0) die("ffmpeg failed on %s", path);
    Pcm p = {0};
    p.n = len / bps;
    p.rate = rate;
    p.info = info.info;
    if (use_f32) p.f = (float *)buf;
    else p.s = (int16_t *)buf;
    return p;
}

Pcm pcm_load(const char *path, int want)
{
    size_t l = strlen(path);
    Pcm p = {0};
    if (l >= 4 && strcasecmp(path + l - 4, ".wav") == 0) {
        p = pcm_read_wav(path, want);
        if (!p.s && !p.f) p = pcm_ffmpeg(path, want);
    } else if (ogg_sniff(path)) {
        p = ogg_load(path, want);                 /* native Vorbis */
        if (!p.s && !p.f) p = pcm_ffmpeg(path, want);   /* e.g. Ogg Opus */
    } else {
        p = pcm_ffmpeg(path, want);
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(p.info.src, sizeof p.info.src, "%s", base);
    return p;
}

char *ffprobe_tag(const char *path, const char *key)
{
    char cmd[16384], fpexe[MAX_PATH_LEN + 16];
    tool_path(fpexe, sizeof fpexe, "ffprobe");
    snprintf(cmd, sizeof cmd,
             "\"%s\" -v error -show_entries format_tags=%s "
             "-of default=noprint_wrappers=1 \"%s\"", fpexe, key, path);
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    char line[4096];
    char *out = NULL;
    while (fgets(line, sizeof line, fp)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;                       /* ffprobe prints "TAG:key=value" */
        if (strncasecmp(k, "TAG:", 4) == 0) k += 4;
        if (strcasecmp(k, key) == 0) {
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

static FILE *wav_open(const char *path, size_t n, int rate, int bits)
{
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s: %s", path, strerror(errno));
    uint32_t dlen = (uint32_t)(n * (size_t)(bits / 8));
    uint8_t hdr[44];
    memcpy(hdr, "RIFF", 4);
    uint32_t fsz = 36 + dlen;
    memcpy(hdr + 4, &fsz, 4);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    uint32_t c16 = 16;
    memcpy(hdr + 16, &c16, 4);
    uint16_t w = bits == 32 ? 3 : 1;            /* IEEE float / PCM */
    memcpy(hdr + 20, &w, 2);
    w = 1;                                      /* mono */
    memcpy(hdr + 22, &w, 2);
    uint32_t dw = (uint32_t)rate;
    memcpy(hdr + 24, &dw, 4);
    dw = (uint32_t)rate * (uint32_t)(bits / 8);
    memcpy(hdr + 28, &dw, 4);
    w = (uint16_t)(bits / 8);
    memcpy(hdr + 32, &w, 2);
    w = (uint16_t)bits;
    memcpy(hdr + 34, &w, 2);
    memcpy(hdr + 36, "data", 4);
    memcpy(hdr + 40, &dlen, 4);
    fwrite(hdr, 1, 44, f);
    return f;
}

void wav_write(const char *path, const int16_t *s, size_t n, int rate, double gain)
{
    FILE *f = wav_open(path, n, rate, 16);
    if (gain == 1.0) {
        fwrite(s, 2, n, f);
    } else {
        int16_t buf[4096];
        for (size_t i = 0; i < n; i++) {
            long v = lround(s[i] * gain);
            buf[i % 4096] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
            if (i % 4096 == 4095) fwrite(buf, 1, sizeof buf, f);
        }
        if (n % 4096) fwrite(buf, 1, (n % 4096) * 2, f);
    }
    fclose(f);
}

void wav_write_f32(const char *path, const float *src, size_t n, int rate, double gain)
{
    FILE *f = wav_open(path, n, rate, 32);
    if (gain == 1.0) {
        fwrite(src, 4, n, f);
    } else {
        float buf[4096];
        for (size_t i = 0; i < n; i++) {
            buf[i % 4096] = (float)(src[i] * gain);
            if (i % 4096 == 4095) fwrite(buf, 1, sizeof buf, f);
        }
        if (n % 4096) fwrite(buf, 1, (n % 4096) * 4, f);
    }
    fclose(f);
}
