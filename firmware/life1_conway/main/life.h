#pragma once

#include <stdint.h>

// Conway's Game of Life on a square grid of cells inside the round display.
// Cells outside the circle are always dead. Displayed colors move slowly toward
// the cell state, so births and deaths fade in and out.

void life_init(void);

// New random population (the old one fades out)
void life_randomize(void);

// Brings to life the cell under (x, y) and some of its neighbors
void life_touch(int x, int y);

// Computes the next generation when due and moves the displayed colors toward the states
void life_update(int64_t now_us);

// Draws one frame, band by band
void life_render(void);

// Frees the buffers and resets the timing; life_init builds everything again
void life_deinit(void);
