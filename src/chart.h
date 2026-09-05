#pragma once
#include "util.h"

typedef struct {
    char artist[256], song[256], author[256];
    double bpm, vol;
    int has_bpm, has_vol;
} Meta;

typedef struct {
    int64_t *floor;
    double *vol;
    size_t n, cap;
} Events;

void meta_parse(const Map *m, Meta *meta);
long angle_entries(const Map *m);
void scan_events(const Map *m, Events *ev);
