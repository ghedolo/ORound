#pragma once

#include <stdint.h>

// Game of Life (B3/S23) on a Penrose kite and dart tiling (P2, data/tassel/darts_and_kites_ii.png) covering the round display.
// Neighbors of a tile are the tiles sharing at least one vertex with it.
// Black lines between tiles as in the image; displayed colors move slowly toward the cell state.

// Builds the tiling with the default tile size and seeds it
void life_init(void);

// Rebuilds the tiling with the next tile size from the list and seeds it
void life_next_size(void);

// Current long edge of the kite in px
int life_edge_px(void);

// New random population (the old one fades out)
void life_randomize(void);

// Brings to life the tile under (x, y) and some of its neighbors
void life_touch(int x, int y);

// Computes the next generation when due and moves the displayed colors toward the states
void life_update(int64_t now_us);

// Draws one frame, band by band
void life_render(void);
