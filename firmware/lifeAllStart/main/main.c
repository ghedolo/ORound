// All the life1_* Game of Life experiments in one firmware. PWR: single click next tiling, double click previous
// tiling, long press power off. BOOT: single click next tile size, double click previous tile size, long press free.
// Touch adds live cells; hold still and drag for brightness. The tiling and the tile size of each tiling are saved
// in NVS and restored at boot.
#include <stdio.h>
#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "banner.h"
#include "panel.h"
#include "tilings.h"
#include "touch_cst820.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PWR_HOLD_US       1500000   // PWR held this long: power off
#define BOOT_HOLD_US      800000    // BOOT held this long: long press (no action), not a click
#define KEY_DEBOUNCE_US   30000     // level stable this long counts as a change
#define DOUBLE_GAP_US     300000    // second press within this time after a release: double click
#define BANNER_US         1500000   // tiling name shown at least this long (the tiling is built meanwhile)
#define BRIGHT_HOLD_US    800000    // finger held still this long: brightness mode until release
#define BRIGHT_STILL_D2   (15 * 15) // max movement while holding still
#define BRIGHT_RANGE_PX   300       // vertical drag for the full brightness range
#define BRIGHT_MIN        0.15f     // lowest position (0..1), keeps the display readable

static const char *TAG = "lifeAllStart";

static void power_latch_on(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PWR_PIN_LATCH,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(PWR_PIN_LATCH, 1);
}

// Push button with debounce and single/double/long click detection. A key held at boot (PWR from the
// power-on press) is ignored until it has been released once.
typedef enum { CLICK_NONE, CLICK_SINGLE, CLICK_DOUBLE, CLICK_LONG } click_t;

typedef struct {
    int pin;
    int idle_level;
    int64_t hold_us;                    // press longer than this: long press
    bool armed;
    bool raw;                           // last read level (true = pressed)
    bool down;                          // debounced level
    int64_t raw_us;                     // time of the last raw change
    int64_t down_us;                    // time of the last debounced press
    int64_t up_us;                      // time of the last debounced release
    int clicks;                         // short presses waiting for the double click gap
    bool long_fired;
} key_t;

static key_t s_boot_key = {.pin = BOOT_PIN_KEY, .hold_us = BOOT_HOLD_US};
static key_t s_pwr_key = {.pin = PWR_PIN_KEY, .hold_us = PWR_HOLD_US};

static int s_tiling;
static const tiling_t *s_cur;

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

static int tiling_load(void)
{
    int idx = 0;
    nvs_handle_t nvs;
    if (nvs_open("life", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "tiling", &v) == ESP_OK && v < TILING_COUNT) idx = v;
        nvs_close(nvs);
    }
    return idx;
}

static void tiling_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("life", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "tiling", (uint8_t)s_tiling);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// Tile size index of each tiling, NVS key "sz<tiling index>"; -1 when never saved
static int size_load(int tiling)
{
    int idx = -1;
    char key[8];
    snprintf(key, sizeof(key), "sz%d", tiling);
    nvs_handle_t nvs;
    if (nvs_open("life", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, key, &v) == ESP_OK) idx = v;
        nvs_close(nvs);
    }
    return idx;
}

static void size_save(void)
{
    if (!s_cur->size_idx) return;
    char key[8];
    snprintf(key, sizeof(key), "sz%d", s_tiling);
    nvs_handle_t nvs;
    if (nvs_open("life", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, key, (uint8_t)s_cur->size_idx());
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

// Frees the current tiling, shows its name and builds the one at idx (wraps around) with its saved tile size
static void tiling_select(int idx)
{
    if (s_cur) s_cur->deinit();
    s_tiling = (idx % TILING_COUNT + TILING_COUNT) % TILING_COUNT;
    s_cur = &TILINGS[s_tiling];
    ESP_LOGI(TAG, "==== %s (%d/%d) ====", s_cur->name, s_tiling + 1, TILING_COUNT);
    int64_t t0 = esp_timer_get_time();
    banner_show(s_cur->name);
    if (s_cur->set_size_idx) s_cur->set_size_idx(size_load(s_tiling));
    s_cur->init();
    int64_t left = BANNER_US - (esp_timer_get_time() - t0);
    if (left > 0) vTaskDelay(pdMS_TO_TICKS(left / 1000));
}

static void key_init(key_t *key, bool pull_up)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << key->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
    };
    gpio_config(&cfg);
    ESP_LOGI(TAG, "key GPIO%d level %d", key->pin, gpio_get_level(key->pin));
}

static click_t key_poll(key_t *key, int64_t now)
{
    bool raw = gpio_get_level(key->pin) != key->idle_level;
    if (raw != key->raw) {
        key->raw = raw;
        key->raw_us = now;
    }
    if (!key->armed) {
        if (!raw) key->armed = true;
        return CLICK_NONE;
    }

    if (raw != key->down && now - key->raw_us >= KEY_DEBOUNCE_US) {
        key->down = raw;
        if (raw) {
            key->down_us = now;
        } else if (key->long_fired) {
            key->long_fired = false;
        } else {
            key->up_us = now;
            if (++key->clicks == 2) {
                key->clicks = 0;
                return CLICK_DOUBLE;
            }
        }
    }
    if (key->down && !key->long_fired && now - key->down_us > key->hold_us) {
        key->long_fired = true;
        key->clicks = 0;
        return CLICK_LONG;
    }
    if (!key->down && key->clicks == 1 && now - key->up_us > DOUBLE_GAP_US) {
        key->clicks = 0;
        return CLICK_SINGLE;
    }
    return CLICK_NONE;
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

static void on_pwr_key(click_t c)
{
    if (c == CLICK_LONG) {
        power_off();
    } else if (c == CLICK_SINGLE || c == CLICK_DOUBLE) {
        tiling_select(s_tiling + (c == CLICK_SINGLE ? 1 : -1));
        tiling_save();
    }
}

static void on_boot_key(click_t c)
{
    if (c == CLICK_SINGLE) s_cur->next_size();
    else if (c == CLICK_DOUBLE) s_cur->prev_size();
    else return;
    size_save();
}

static void main_task(void *arg)
{
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
            s_cur->touch(x, y);
        }
        was_pressed = pressed;

        on_pwr_key(key_poll(&s_pwr_key, now));
        on_boot_key(key_poll(&s_boot_key, now));

        int64_t t_render = esp_timer_get_time();
        s_cur->update(now);
        s_cur->render();
        render_us += esp_timer_get_time() - t_render;

        frames++;
        if (now - fps_t0 >= 2000000) {
            if (s_cur->edge_px) {
                ESP_LOGI(TAG, "%s, size %d px, %.1f fps, render %d us", s_cur->name, s_cur->edge_px(),
                         frames * 1e6f / (now - fps_t0), (int)(render_us / frames));
            } else {
                ESP_LOGI(TAG, "%s, %.1f fps, render %d us", s_cur->name, frames * 1e6f / (now - fps_t0),
                         (int)(render_us / frames));
            }
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
    s_boot_key.idle_level = 1;      // active low, external pull-up
    s_pwr_key.idle_level = 1;       // active low
    key_init(&s_boot_key, true);
    key_init(&s_pwr_key, false);
    tiling_select(tiling_load());
    xTaskCreatePinnedToCore(main_task, "main", 6 * 1024, NULL, 5, NULL, 1);
}
