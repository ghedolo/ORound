#pragma once

#include <stdint.h>

// One Game of Life experiment (life1_*). Only one is built at a time: deinit frees its buffers before the next init.
typedef struct {
    const char *name;
    void (*init)(void);
    void (*deinit)(void);
    void (*next_size)(void);            // BOOT single click
    void (*prev_size)(void);            // BOOT double click
    int (*edge_px)(void);               // NULL: fixed size
    int (*size_idx)(void);              // NULL: fixed size
    void (*set_size_idx)(int idx);      // call before init
    void (*touch)(int x, int y);
    void (*update)(int64_t now_us);
    void (*render)(void);
} tiling_t;

extern const tiling_t TILINGS[];
extern const int TILING_COUNT;
