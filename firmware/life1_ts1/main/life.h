#pragma once

#include <stdint.h>

// Game of Life (B3/S23) on the Alhambra arrow tiling (data/tassel/Alhambra.jpg) covering the round display.
// Neighbors of a tile are the tiles sharing at least one vertex with it.
// Tiles have no border; displayed colors move slowly toward the cell state.

// Builds the tiling with the default tile size and seeds it
void life_init(void);

// Rebuilds the tiling with the next tile size from the list and seeds it
void life_next_size(void);

// Rebuilds the tiling with the previous tile size from the list and seeds it
void life_prev_size(void);

// Index of the current tile size in the list
int life_size_idx(void);

// Selects the tile size by index without rebuilding (call before life_init); out of range: ignored
void life_set_size_idx(int idx);

// Current arrow length in px
int life_edge_px(void);

// New random population (the old one fades out)
void life_randomize(void);

// Brings to life the tile under (x, y) and some of its neighbors
void life_touch(int x, int y);

// Computes the next generation when due and moves the displayed colors toward the states
void life_update(int64_t now_us);

// Draws one frame, band by band
void life_render(void);

// Frees the buffers and resets the timing; life_init builds everything again
void life_deinit(void);
