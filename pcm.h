#pragma once
#include <stdint.h>
#include <stddef.h>

typedef struct { int16_t *s; size_t n; int rate; } Pcm;

Pcm pcm_load(const char *path);
char *ffprobe_tag(const char *path, const char *key);
void wav_write(const char *path, const int16_t *s, size_t n, int rate);
