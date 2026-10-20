// Game of Life on an isometric tiling with a 3D look (ts9) and slow color fades. Touch adds live cells,
// BOOT switches to the next tile size (to choose the smallest readable one).
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel.h"
#include "life.h"
#include "touch_cst820.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PWR_HOLD_US       1500000   // PWR held this long: power off
#define KEY_DEBOUNCE_US   150000
#define BRIGHT_HOLD_US    800000    // finger held still this long: brightness mode until release
#define BRIGHT_STILL_D2   (15 * 15) // max movement while holding still
#define BRIGHT_RANGE_PX   300       // vertical drag for the full brightness range
#define BRIGHT_MIN        0.15f     // lowest position (0..1), keeps the display readable

static const char *TAG = "life1_ts9";

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

// Brightness as a position 0..1; register value = 255 * pos^2 (closer to perceived brightness)
static float s_bright_pos = 1.0f;

static void brightness_apply(void)
{
    int level = (int)(255.0f * s_bright_pos * s_bright_pos + 0.5f);
    panel_set_brightness((uint8_t)level);
}

static void brightness_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open("display", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "bright", &v) == ESP_OK) {
            s_bright_pos = v / 255.0f;
            if (s_bright_pos < BRIGHT_MIN) s_bright_pos = BRIGHT_MIN;
        }
        nvs_close(nvs);
    }
    brightness_apply();
}

static void brightness_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("display", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "bright", (uint8_t)(s_bright_pos * 255.0f + 0.5f));
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

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

// Power off. On battery, BAT_EN (latch) low cuts the supply. On USB the board stays
// powered, so it goes to deep sleep instead and PWR wakes it (the chip restarts).
static void power_off(void)
{
    ESP_LOGI(TAG, "power off");
    panel_off();
    gpio_set_level(PWR_PIN_LATCH, 0);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Still running: USB power. Wait for the button release, then sleep until the next press.
    ESP_LOGI(TAG, "USB power: deep sleep, press PWR to wake");
    while (gpio_get_level(PWR_PIN_KEY) == 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_sleep_enable_ext0_wakeup(PWR_PIN_KEY, 0);
    esp_deep_sleep_start();
}

// Finger touches the display (not called in brightness mode)
static void on_touch_down(int x, int y)
{
    life_touch(x, y);
}

// Finger moves while touching (not called in brightness mode)
static void on_touch_move(int x, int y)
{
    life_touch(x, y);
}

// BOOT pressed
static void on_boot_key(void)
{
    life_next_size();
}

static void main_task(void *arg)
{
    // PWR is active low. It may still be held from the power-on press,
    // so a hold counts only after the button has been released once.
    bool pwr_armed = false;
    int64_t pwr_down_us = 0;
    bool was_pressed = false;
    int down_x = 0, down_y = 0;     // touch-down point
    int64_t down_us = 0;
    bool still = false;             // finger has not moved since touch-down
    bool bright_mode = false;
    int bright_y0 = 0;              // finger y when brightness mode started
    float bright_pos0 = 0;
    int frames = 0;
    int64_t render_us = 0;
    int64_t fps_t0 = esp_timer_get_time();

    while (1) {
        uint16_t x, y;
        bool pressed = touch_read(&x, &y);
        int64_t now = esp_timer_get_time();
        if (pressed && !was_pressed) {
            down_x = x;
            down_y = y;
            down_us = now;
            still = true;
        }
        if (pressed && still && !bright_mode) {
            int sx = x - down_x;
            int sy = y - down_y;
            if (sx * sx + sy * sy > BRIGHT_STILL_D2) {
                still = false;
            } else if (now - down_us > BRIGHT_HOLD_US) {
                bright_mode = true;
                bright_y0 = y;
                bright_pos0 = s_bright_pos;
                ESP_LOGI(TAG, "brightness mode");
            }
        }
        if (!pressed && bright_mode) {
            bright_mode = false;
            brightness_save();
            ESP_LOGI(TAG, "brightness %d%%", (int)(s_bright_pos * 100));
        }
        if (bright_mode) {
            // Drag up: brighter, drag down: darker
            float pos = bright_pos0 + (float)(bright_y0 - (int)y) / BRIGHT_RANGE_PX;
            if (pos < BRIGHT_MIN) pos = BRIGHT_MIN;
            if (pos > 1.0f) pos = 1.0f;
            if (pos != s_bright_pos) {
                s_bright_pos = pos;
                brightness_apply();
            }
        } else if (pressed) {
            if (!was_pressed) on_touch_down(x, y);
            else on_touch_move(x, y);
        }
        was_pressed = pressed;

        // PWR: long press powers off
        bool pwr_down = gpio_get_level(PWR_PIN_KEY) == 0;
        if (!pwr_down) {
            pwr_armed = true;
            pwr_down_us = 0;
        } else if (pwr_armed) {
            if (pwr_down_us == 0) pwr_down_us = now;
            else if (now - pwr_down_us > PWR_HOLD_US) power_off();
        }

        if (key_pressed(&s_boot_key, now)) {
            on_boot_key();
        }

        int64_t t_render = esp_timer_get_time();
        life_update(now);
        life_render();
        render_us += esp_timer_get_time() - t_render;

        frames++;
        if (now - fps_t0 >= 2000000) {
            ESP_LOGI(TAG, "rhombus %d px, %.1f fps, render %d us", life_edge_px(), frames * 1e6f / (now - fps_t0),
                     (int)(render_us / frames));
            frames = 0;
            render_us = 0;
            fps_t0 = now;
        }
        vTaskDelay(1);
    }
}

void app_main(void)
{
    power_latch_on();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(panel_init());
    brightness_load();
    ESP_ERROR_CHECK(touch_init());
    key_init(&s_boot_key, true);
    gpio_config_t pwr_cfg = {.pin_bit_mask = 1ULL << PWR_PIN_KEY, .mode = GPIO_MODE_INPUT};
    gpio_config(&pwr_cfg);
    life_init();
    xTaskCreatePinnedToCore(main_task, "main", 6 * 1024, NULL, 5, NULL, 1);
}
