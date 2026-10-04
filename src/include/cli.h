#pragma once
#include "codec.h"
#include "pcm.h"
#include "chart.h"

/* encode output format: OUT_AUTO = infer from the output extension */
#define OUT_AUTO  0
#define OUT_PLAIN 1
#define OUT_XZ    2
#define OUT_ZSTD  3

int encode_cmd(const char *input, const char *outpath,
              const char *title_opt, const char *artist_opt,
              const char *xzopt, const char *zstdopt, int fmt, unsigned encflags,
              int want);
int decode_cmd(const char *chart, const char *out, double gain, int want);
int play_cmd(const char *chart, double gain);
int verify_cmd(const char *chart, const char *ref);
int info_cmd(const char *chart);
int bench_cmd(const char *chart, long slice_mb, int full);
int selftest_cmd(void);
