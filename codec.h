#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "util.h"
#include "chart.h"

typedef struct { int16_t *s; size_t total; int rate; } Audio;

Audio audio_decode(const char *path);
Audio audio_decode_map(const Map *m);
size_t encode_core(FILE *f, const int16_t *s, size_t n, int rate,
                   const char *title, const char *artist, size_t *events_out);
