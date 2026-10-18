#pragma once

#include <stdint.h>
#include "esp_err.h"

#define PANEL_BAND_LINES 16
#define PANEL_BAND_COUNT 2

// One pixel in the panel byte order (RGB888, 16.7M colors)
typedef struct __attribute__((packed)) {
    uint8_t r, g, b;
} pixel_t;

esp_err_t panel_init(void);

// Returns a free band buffer (LCD_H_RES * PANEL_BAND_LINES pixels).
// Blocks until a previous transfer using it has finished.
pixel_t *panel_band_acquire(void);

// Sends rows y1..y2 (inclusive, y1 even, y2 odd) of full width from the band buffer.
void panel_band_send(pixel_t *band, int y1, int y2);

// 0..255, AMOLED brightness register 0x51
void panel_set_brightness(uint8_t level);
