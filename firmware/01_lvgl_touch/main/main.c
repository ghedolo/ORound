#include "board_pins.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "ui.h"

static const char *TAG = "main";

static void power_latch_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PWR_PIN_LATCH,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PWR_PIN_LATCH, 1);
}

void app_main(void)
{
    power_latch_on();
    ESP_ERROR_CHECK(display_start());

    if (display_lock(0)) {
        ui_create();
        display_unlock();
    }
    ESP_LOGI(TAG, "ui running");
}
