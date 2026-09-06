/* playback: decode chart (plain/.xz) and play through miniaudio — see README.md */
#define _POSIX_C_SOURCE 200809L
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_IMPLEMENTATION
#include "miniaudio.h"

#include "cli.h"
#include "codec.h"

typedef struct {
    const int16_t *s;
    size_t total, pos;
    volatile int done;
} PlayCtx;

static PlayCtx g_pc;
static ma_device g_dev;

static void data_cb(ma_device *d, void *out, const void *in, ma_uint32 frames)
{
    (void)d;
    (void)in;
    int16_t *o = out;
    size_t want = g_pc.total - g_pc.pos;
    if ((size_t)frames > want) {
        if (want) memcpy(o, g_pc.s + g_pc.pos, want * 2);
        memset(o + want, 0, (size_t)(frames - want) * 2);   /* pad tail w/ silence */
        g_pc.pos = g_pc.total;
        g_pc.done = 1;
    } else {
        memcpy(o, g_pc.s + g_pc.pos, (size_t)frames * 2);
        g_pc.pos += (size_t)frames;
    }
}

static void on_sigint(int sig)
{
    (void)sig;
    g_pc.done = 1;
    signal(SIGINT, SIG_DFL);
}

int play_cmd(const char *chart, double gain)
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
    if (a.total == 0) die("nothing to play in %s", chart);

    g_pc.s = a.s;
    g_pc.total = a.total;
    g_pc.pos = 0;
    g_pc.done = 0;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_s16;
    cfg.playback.channels = 1;
    cfg.sampleRate = (ma_uint32)(a.rate > 0 ? a.rate : 44100);
    cfg.dataCallback = data_cb;
    if (ma_device_init(NULL, &cfg, &g_dev) != MA_SUCCESS)
        die("cannot open audio output device");
    signal(SIGINT, on_sigint);
    if (ma_device_start(&g_dev) != MA_SUCCESS) {
        ma_device_uninit(&g_dev);
        die("cannot start audio output");
    }

    printf("playing %zu samples (%.1f kHz, %.1fs) from %s - Ctrl+C to stop\n",
           a.total, a.rate / 1000.0, (double)a.total / a.rate, chart);
    fflush(stdout);
    while (!g_pc.done) ma_sleep(20);
    ma_device_uninit(&g_dev);
    printf("done in %.1fs\n", now_s() - t0);
    free(a.s);
    return 0;
}
