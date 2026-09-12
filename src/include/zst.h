#pragma once
#include <stdint.h>
#include <stdio.h>
#include "util.h"

/* .zst = frame magic 28 B5 2F FD (little-endian) */
int zstd_sniff(const char *path);            /* 1 if file starts with zstd magic */
Map zstd_map(const char *path);              /* decompress whole stream to heap (owned=1) */

#define ZSTD_LEVEL_DEFAULT 19                /* ~= xz -6 size, faster decode */
int zstd_parse_level(const char *s);         /* "1".."22"; NULL -> default; die on junk */

/* write-cookie FILE*: fputs/fwrite/... compress through libzstd to PATH. */
FILE *zstd_open(const char *path, int level);
