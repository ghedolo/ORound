#pragma once

#include <stdint.h>

// Doom fire (PSX Doom title screen, as described by Fabien Sanglard).
// A grid of intensities 0..36 mapped to the 37 color Doom fire palette.
// The bottom row is the source (36). Each step every cell is copied one row up,
// shifted by -2..+1 columns and decreased by 0 or 1 (random).
// One grid cell = scale x scale display pixels.

// Reads the scale from NVS and builds the grid
void flame_init(void);

// Next / previous scale (saved in NVS); the fire restarts
void flame_next_scale(void);
void flame_prev_scale(void);

// Source on (bottom row = 36) or off (bottom row dies down, as at the end of the PSX intro)
void flame_toggle_source(void);

// Runs the steps due at now_us (fixed step rate)
void flame_update(int64_t now_us);

// Draws one frame, band by band
void flame_render(void);

// Current scale in display pixels per grid cell
int flame_scale(void);
