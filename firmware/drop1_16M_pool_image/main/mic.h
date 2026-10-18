#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

// Starts the ES8311 microphone input and a task on core 0 that measures the sound level.
esp_err_t mic_init(i2c_master_bus_handle_t bus);

// Sound level above the background noise, 0 (quiet) .. 1 (loud).
float mic_level(void);
