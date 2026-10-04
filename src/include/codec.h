#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "util.h"
#include "chart.h"
#include "pcm.h"

/* decoded audio: either int16 (s) or float32 (f) */
typedef struct { int16_t *s; float *f; size_t total; int rate; } Audio;

/* encode_core output layout flags */
#define ENC_PRETTY  1u   /* indentation + CRLF (readable, larger) */
#define ENC_MINIMAL 2u   /* compact + omit gameSound/hitsound (defaults match) */

Audio audio_decode(const char *path);
Audio audio_decode_map(const Map *m);
/* sample format comes from p (p->f != NULL -> float32 chart, lossless) */
size_t encode_core(FILE *f, const Pcm *p, const char *title, const char *artist,
                   size_t *events_out, unsigned flags);
