#pragma once

#include <stdbool.h>
#include <stdint.h>

// Sparks as in metal welding: white streaks thrown from the fire source in all
// directions, curved down by gravity, slowed by air drag; some split into smaller
// sparks at the end of their life. Every 8..20 s (while emitting) an explosion: a
// burst of sparks and a short white flash at a random point away from the center.
// Drawn into a 1 bit per pixel mask.

void spark_init(void);

// Moves the sparks to now_us and emits new ones at (cx, cy) while emit is true.
// wind 0..1 raises the emission rate.
void spark_update(int64_t now_us, int cx, int cy, bool emit, float wind);

// Rasterizes all sparks into the mask (call once per frame, after spark_update)
void spark_draw(void);

// Mask row y: SPARK_STRIDE bytes, bit (x & 7) of byte x >> 3 = pixel x
#define SPARK_STRIDE 59     // (LCD_H_RES + 7) / 8 for 466 px
// Mask rows: fire sparks (spark_row) and explosion sparks + flash (spark_boom_row)
const uint8_t *spark_row(int y);
const uint8_t *spark_boom_row(int y);
