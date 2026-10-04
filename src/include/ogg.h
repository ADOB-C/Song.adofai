#pragma once
#include "pcm.h"

/* native Ogg Vorbis decoding (vendored stb_vorbis) */
int ogg_sniff(const char *path);
/* empty Pcm (n == 0, s == f == NULL) means "not Vorbis / failed" -> ffmpeg */
Pcm ogg_load(const char *path, int want);
