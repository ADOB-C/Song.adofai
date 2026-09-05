#pragma once
#include <stdint.h>
#include <stdio.h>
#include "util.h"

/* .xz = container magic FD 37 7A 58 5A 00 */
int xz_sniff(const char *path);              /* 1 if file starts with xz magic */
Map xz_map(const char *path);                /* decompress whole stream to heap (owned=1) */

#define XZ_LEVEL_DEFAULT 6                   /* measured: 4-min worst case ~63 MB */
uint32_t xz_parse_preset(const char *s);     /* "6" / "9e"; NULL -> default; die on junk */

/* write-cookie FILE*: fputs/fwrite/... compress through liblzma to PATH.
 * fclose() finalizes the stream (do not fflush-and-reuse). */
FILE *xz_open(const char *path, uint32_t preset);
