#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_random.h"
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

static const char *TAG = "drop2_16M";

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
    float tint = 0;                 // crest tint of the current gesture
    int frames = 0;
    int64_t step_us = 0, render_us = 0;
    int64_t fps_t0 = esp_timer_get_time();

    while (1) {
        uint16_t x, y;
        bool pressed = touch_read(&x, &y);
        int64_t now = esp_timer_get_time();
        if (pressed) {
            int dx = x - last_x;
            int dy = y - last_y;
            if (!was_pressed) {
                tint = (float)esp_random() / (float)UINT32_MAX;
                ripple_drop(x, y, TAP_STRENGTH, true, tint);
                last_x = x;
                last_y = y;
                last_drop_us = now;
            } else if (now - last_drop_us > DRAG_INTERVAL_US && dx * dx + dy * dy > DRAG_MIN_DIST2) {
                ripple_drop(x, y, DRAG_STRENGTH, false, tint);
                last_x = x;
                last_y = y;
                last_drop_us = now;
            }
        }
        was_pressed = pressed;

        int64_t t_step = esp_timer_get_time();
        for (int i = 0; i < STEPS_PER_FRAME; i++) {
            ripple_step();
        }
        int64_t t_render = esp_timer_get_time();
        ripple_render();
        step_us += t_render - t_step;
        render_us += esp_timer_get_time() - t_render;

        frames++;
        if (now - fps_t0 >= 2000000) {
            ESP_LOGI(TAG, "%.1f fps, step %d us, render %d us", frames * 1e6f / (now - fps_t0),
                     (int)(step_us / frames), (int)(render_us / frames));
            frames = 0;
            step_us = 0;
            render_us = 0;
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
