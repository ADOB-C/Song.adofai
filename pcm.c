/* PCM I/O (WAV reader, ffmpeg fallback, ffprobe tags, WAV writer) - see README.md */
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

#include "pcm.h"
#include "util.h"

Pcm pcm_read_wav(const char *path)
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

Pcm pcm_ffmpeg(const char *path)
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

Pcm pcm_load(const char *path)
{
    size_t l = strlen(path);
    if (l >= 4 && strcasecmp(path + l - 4, ".wav") == 0) {
        Pcm p = pcm_read_wav(path);
        if (p.s) return p;
    }
    return pcm_ffmpeg(path);
}

char *ffprobe_tag(const char *path, const char *key)
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

void wav_write(const char *path, const int16_t *s, size_t n, int rate)
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

