#pragma once
#include <stdint.h>
#include <stddef.h>

/* sample format: auto (native / ffprobe), a forced value, or a reported one */
#define SAMPLE_AUTO 0
#define SAMPLE_S16  1
#define SAMPLE_F32  2
#define SAMPLE_S24  3
#define SAMPLE_U8   4
#define SAMPLE_S32  5
#define SAMPLE_F64  6

/* what the source audio was, for provenance in the chart */
typedef struct {
    char src[128];      /* basename of the source file */
    char codec[24];     /* container/codec name (wav / flac / mp3 / aac ...) */
    int fmt;            /* SAMPLE_* as it came in */
    int bits;           /* bits per raw sample, 0 = unknown */
    int channels;       /* source channels, 0 = unknown */
    uint64_t crc64;     /* CRC64 of the stored mono samples */
} SrcInfo;

typedef struct {
    int16_t *s;         /* mono samples, either s (int16) or f (float32) */
    float *f;
    size_t n;
    int rate;
    SrcInfo info;
} Pcm;

Pcm pcm_load(const char *path, int want);
char *ffprobe_tag(const char *path, const char *key);
const char *pcm_fmt_name(int fmt);          /* "s16" / "s24" / "f32" ... */
int16_t pcm_s16_from_f32(float x);          /* round + clip */
void wav_write(const char *path, const int16_t *s, size_t n, int rate, double gain);
void wav_write_f32(const char *path, const float *f, size_t n, int rate, double gain);
