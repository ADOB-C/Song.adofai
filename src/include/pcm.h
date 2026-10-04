#pragma once
#include <stdint.h>
#include <stddef.h>

/* sample format: auto (native / ffprobe), or forced */
#define SAMPLE_AUTO 0
#define SAMPLE_S16  1
#define SAMPLE_F32  2

typedef struct { int16_t *s; float *f; size_t n; int rate; } Pcm;

Pcm pcm_load(const char *path, int want);
char *ffprobe_tag(const char *path, const char *key);
int16_t pcm_s16_from_f32(float x);          /* round + clip */
void wav_write(const char *path, const int16_t *s, size_t n, int rate, double gain);
void wav_write_f32(const char *path, const float *f, size_t n, int rate, double gain);
