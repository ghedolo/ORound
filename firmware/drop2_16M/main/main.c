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
#define KEY_DEBOUNCE_US   150000

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

// Push button with debounce. The idle level is sampled at boot, so the
// polarity does not need to be known (BOOT is active low, PWR level depends on the board).
typedef struct {
    int pin;
    int idle_level;
    bool was_down;
    int64_t change_us;
} key_t;

static key_t s_boot_key = {.pin = BOOT_PIN_KEY};
static key_t s_pwr_key = {.pin = PWR_PIN_KEY};

static void key_init(key_t *key, bool pull_up)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << key->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    gpio_config(&cfg);
    key->idle_level = gpio_get_level(key->pin);
    ESP_LOGI(TAG, "key GPIO%d idle level %d", key->pin, key->idle_level);
}

// Returns true once per press
static bool key_pressed(key_t *key, int64_t now)
{
    bool down = gpio_get_level(key->pin) != key->idle_level;
    if (down == key->was_down || now - key->change_us < KEY_DEBOUNCE_US) {
        return false;
    }
    key->was_down = down;
    key->change_us = now;
    return down;
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

        // BOOT: next palette, PWR: previous palette
        if (key_pressed(&s_boot_key, now)) {
            ripple_set_palette(+1);
        }
        if (key_pressed(&s_pwr_key, now)) {
            ripple_set_palette(-1);
        }

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
    key_init(&s_boot_key, true);
    key_init(&s_pwr_key, false);
    ESP_ERROR_CHECK(ripple_init());
    xTaskCreatePinnedToCore(water_task, "water", 6 * 1024, NULL, 5, NULL, 1);
}
