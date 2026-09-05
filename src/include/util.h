#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define VERSION "0.3.0"
#define AUTHOR "Song.adofai (https://github.com/CHT-1192/Song.adofai)"
#define AUTHOR_JSON "\"Song.adofai (https://github.com/CHT-1192/Song.adofai)\""
#define MAX_PATH_LEN 4096

typedef struct {
    const unsigned char *p;   /* file-backed mmap or heap copy */
    size_t n;                 /* logical (uncompressed) size */
    size_t src;               /* on-disk size (== n unless xz) */
    int owned;                /* 1 = heap (xz), 0 = anonymous mmap */
} Map;

void die(const char *fmt, ...);
double now_s(void);
Map map_file(const char *path);   /* plain mmap only */
Map map_open(const char *path);   /* mmap, or fully decompress if .xz stream */
void map_close(Map *m);           /* munmap or free per owned; zeroes *m */
const unsigned char *findb(const unsigned char *hay, size_t n,
                           const char *needle, size_t nn);
const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end);
void esc_json(const char *in, char *out, size_t outsz);
