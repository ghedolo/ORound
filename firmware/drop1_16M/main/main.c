#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel.h"
#include "ripple.h"
#include "touch_cst820.h"

#define TAP_STRENGTH      9000
#define DRAG_STRENGTH     4000
#define DRAG_INTERVAL_US  40000
#define DRAG_MIN_DIST2    (12 * 12)
#define STEPS_PER_FRAME   1

static const char *TAG = "drop1_16M";

static void power_latch_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PWR_PIN_LATCH,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PWR_PIN_LATCH, 1);
}

static void water_task(void *arg)
{
    bool was_pressed = false;
    int last_x = 0, last_y = 0;
    int64_t last_drop_us = 0;
    int frames = 0;
    int64_t fps_t0 = esp_timer_get_time();

    while (1) {
        uint16_t x, y;
        bool pressed = touch_read(&x, &y);
        int64_t now = esp_timer_get_time();
        if (pressed) {
            int dx = x - last_x;
            int dy = y - last_y;
            if (!was_pressed) {
                ripple_drop(x, y, TAP_STRENGTH, true);
                last_x = x;
                last_y = y;
                last_drop_us = now;
            } else if (now - last_drop_us > DRAG_INTERVAL_US && dx * dx + dy * dy > DRAG_MIN_DIST2) {
                ripple_drop(x, y, DRAG_STRENGTH, false);
                last_x = x;
                last_y = y;
                last_drop_us = now;
            }
        }
        was_pressed = pressed;

        for (int i = 0; i < STEPS_PER_FRAME; i++) {
            ripple_step();
        }
        ripple_render();

        frames++;
        if (now - fps_t0 >= 2000000) {
            ESP_LOGI(TAG, "%.1f fps", frames * 1e6f / (now - fps_t0));
            frames = 0;
            fps_t0 = now;
        }
        vTaskDelay(1);
    }
}

void app_main(void)
{
    power_latch_on();
    ESP_ERROR_CHECK(panel_init());
    ESP_ERROR_CHECK(touch_init());
    ESP_ERROR_CHECK(ripple_init());
    xTaskCreatePinnedToCore(water_task, "water", 6 * 1024, NULL, 5, NULL, 1);
}
