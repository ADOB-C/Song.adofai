#pragma once
#include "codec.h"
#include "pcm.h"
#include "chart.h"

int encode_cmd(const char *input, const char *outpath, const char *outdir,
              const char *title_opt, const char *artist_opt, const char *xzopt);
int decode_cmd(const char *chart, const char *out, double gain);
int play_cmd(const char *chart, double gain);
int verify_cmd(const char *chart, const char *ref);
int info_cmd(const char *chart);
int selftest_cmd(void);
