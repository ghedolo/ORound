#pragma once

#include <stdint.h>

// Game of Life (B2/S23) on an isometric tiling from data/tassel/fake_3d2.jpg: L pieces, cubes and parallelograms.
// Neighbors of a tile are the tiles sharing at least one vertex with it.
// Tiles have no border; displayed colors move slowly toward the cell state.

// Builds the tiling with the default tile size and seeds it
void life_init(void);

// Rebuilds the tiling with the next tile size from the list and seeds it
void life_next_size(void);

// Current side of the base rhombus in px
int life_edge_px(void);

// New random population (the old one fades out)
void life_randomize(void);

// Brings to life the tile under (x, y) and some of its neighbors
void life_touch(int x, int y);

// Computes the next generation when due and moves the displayed colors toward the states
void life_update(int64_t now_us);

// Draws one frame, band by band
void life_render(void);
