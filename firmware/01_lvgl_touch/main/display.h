#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Initializes panel, touch and LVGL, then starts the LVGL task.
esp_err_t display_start(void);

// LVGL is not thread safe: wrap every LVGL call from other tasks.
bool display_lock(uint32_t timeout_ms);
void display_unlock(void);

// 0..255, AMOLED brightness register 0x51
void display_set_brightness(uint8_t level);
