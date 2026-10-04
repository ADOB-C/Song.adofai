#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define VERSION "0.4.0"
#define AUTHOR "Song.adofai (https://github.com/CHT-1192/Song.adofai)"
#define AUTHOR_JSON "\"Song.adofai (https://github.com/CHT-1192/Song.adofai)\""
#define MAX_PATH_LEN 4096

#define MAP_PLAIN 0
#define MAP_XZ    1
#define MAP_ZSTD  2

typedef struct {
    const unsigned char *p;   /* file-backed mmap or heap copy */
    size_t n;                 /* logical (uncompressed) size */
    size_t src;               /* on-disk size (== n unless compressed) */
    int owned;                /* 1 = heap, 0 = anonymous mmap */
    int fmt;                  /* MAP_PLAIN / MAP_XZ / MAP_ZSTD */
} Map;

void die(const char *fmt, ...);
double now_s(void);

/* ffmpeg-style log levels: 0 quiet, 1 info (default), 2 verbose */
extern int log_level;
void log_msg(int level, const char *fmt, ...);
#define log_info(...)    log_msg(1, __VA_ARGS__)
#define log_verbose(...) log_msg(2, __VA_ARGS__)

/* codec threading: 0 = auto (all CPUs), N > 0 = cap */
extern int thread_count;
unsigned codec_threads(void);

/* free bytes on the filesystem holding path (parent dir if it doesn't exist);
 * 0 = unknown */
unsigned long long free_bytes(const char *path);

Map map_file(const char *path);   /* plain mmap only */
Map map_open(const char *path);   /* mmap, or decompress .xz/.zst on the fly */
void map_close(Map *m);           /* munmap or free per owned; zeroes *m */
const unsigned char *findb(const unsigned char *hay, size_t n,
                           const char *needle, size_t nn);
const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end);
void esc_json(const char *in, char *out, size_t outsz);

/* generic compressed-writer cookie FILE*: callbacks compress into `out`.
 * write() appends compressed bytes and returns 0 / -1; finish() ends the
 * stream, frees its own state and returns 0 / -1. fclose() triggers finish(). */
typedef int (*CookieWrite)(void *ctx, const void *buf, size_t n, FILE *out);
typedef int (*CookieFinish)(void *ctx, FILE *out);
FILE *cookie_wopen(const char *path, void *ctx, CookieWrite w, CookieFinish f);
