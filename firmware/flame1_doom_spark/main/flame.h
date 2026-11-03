#pragma once

#include <stdint.h>

// Doom fire (PSX Doom title screen, as described by Fabien Sanglard).
// A grid of intensities 0..36, drawn in black and white only: white where the
// Doom fire palette color has luma >= 128, black elsewhere.
// Radial variant: the grid is polar (columns = angles, rows = radii). The center
// row is the source (36). Each step every cell is copied one cell outward,
// shifted by -1..+1 angle columns and decreased by 1 with probability p.
// Radial cell size = scale px; angular cell size = scale px at the display edge.

// Builds the grid at DEFAULT_SCALE
void flame_init(void);

// Next / previous scale; the fire restarts
void flame_next_scale(void);
void flame_prev_scale(void);

// Scale by index into the scale list (ignored if out of range)
void flame_set_scale_idx(int idx);

// Source on (center = 36) or off (center dies down, as at the end of the PSX intro)
void flame_toggle_source(void);

// The current fire dies down where it is and a new one starts at (x, y) (display px)
void flame_move_to(int x, int y);

// Microphone level 0..1. Low: the flames move more; medium: shorter flames;
// loud for 0.5 s: the source goes off (relights after 3 s of quiet).
void flame_set_wind(float level, int64_t now_us);

// Smoothed wind level 0..1 (for the log)
float flame_wind(void);

// Runs the steps due at now_us (fixed step rate)
void flame_update(int64_t now_us);

// Draws one frame, band by band
void flame_render(void);

// Current scale in display pixels per grid cell
int flame_scale(void);
