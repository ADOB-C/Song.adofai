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
    const int16_t *s;      /* one of these is set */
    const float *f;
    double gain;
    size_t total, pos;
    volatile int done;
} PlayCtx;

static PlayCtx g_pc;
static ma_device g_dev;

static void data_cb(ma_device *d, void *out, const void *in, ma_uint32 frames)
{
    (void)d;
    (void)in;
    float *o = out;
    size_t want = g_pc.total - g_pc.pos;
    size_t copy = (size_t)frames > want ? want : (size_t)frames;
    for (size_t i = 0; i < copy; i++) {
        double v = g_pc.f ? (double)g_pc.f[g_pc.pos + i]
                          : g_pc.s[g_pc.pos + i] / 32768.0;
        o[i] = (float)(v * g_pc.gain);
    }
    for (size_t i = copy; i < (size_t)frames; i++) o[i] = 0.0f;
    g_pc.pos += copy;
    if (copy < (size_t)frames) g_pc.done = 1;
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
    if (a.total == 0) die("nothing to play in %s", chart);

    g_pc.s = a.s;
    g_pc.f = a.f;
    g_pc.gain = gain;
    g_pc.total = a.total;
    g_pc.pos = 0;
    g_pc.done = 0;
    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
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

    printf("playing %zu samples (%.1f kHz, %.1fs, %s) from %s - Ctrl+C to stop\n",
           a.total, a.rate / 1000.0, (double)a.total / a.rate,
           a.f ? "float32" : "int16", chart);
    fflush(stdout);
    while (!g_pc.done) ma_sleep(20);
    ma_device_uninit(&g_dev);
    printf("done in %.1fs\n", now_s() - t0);
    free(a.s);
    free(a.f);
    return 0;
}
