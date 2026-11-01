#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t touch_init(void);

// Returns true while a finger is on the panel; x, y in display coordinates.
bool touch_read(uint16_t *x, uint16_t *y);

// I2C bus shared with the audio codec
i2c_master_bus_handle_t touch_bus(void);
