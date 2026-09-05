#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define VERSION "2.1.0"
#define AUTHOR "Music.adofai (https://github.com/CHT-1192/Music.adofai)"
#define AUTHOR_JSON "\"Music.adofai (https://github.com/CHT-1192/Music.adofai)\""
#define MAX_PATH_LEN 4096

typedef struct { const unsigned char *p; size_t n; } Map;

void die(const char *fmt, ...);
double now_s(void);
Map map_file(const char *path);
const unsigned char *findb(const unsigned char *hay, size_t n,
                           const char *needle, size_t nn);
const unsigned char *skip_ws(const unsigned char *p, const unsigned char *end);
void esc_json(const char *in, char *out, size_t outsz);
