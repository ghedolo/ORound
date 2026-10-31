#include "flame.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "panel.h"

#define LEVELS         37          // palette entries, 0 = black, 36 = white
#define STEP_US        (1000000 / 27)   // fire steps per second
#define MAX_STEPS      3           // steps per frame at most (after a slow frame)
#define DEFAULT_SCALE  3

static const char *TAG = "flame";

static const int SCALES[] = {1, 2, 3, 4, 5, 6, 8};
#define N_SCALES ((int)(sizeof(SCALES) / sizeof(SCALES[0])))

// Doom fire palette (PSX Doom), RGB
static const uint8_t PALETTE_RGB[LEVELS][3] = {
    {0x07, 0x07, 0x07}, {0x1F, 0x07, 0x07}, {0x2F, 0x0F, 0x07}, {0x47, 0x0F, 0x07},
    {0x57, 0x17, 0x07}, {0x67, 0x1F, 0x07}, {0x77, 0x1F, 0x07}, {0x8F, 0x27, 0x07},
    {0x9F, 0x2F, 0x07}, {0xAF, 0x3F, 0x07}, {0xBF, 0x47, 0x07}, {0xC7, 0x47, 0x07},
    {0xDF, 0x4F, 0x07}, {0xDF, 0x57, 0x07}, {0xDF, 0x57, 0x07}, {0xD7, 0x5F, 0x07},
    {0xD7, 0x5F, 0x07}, {0xD7, 0x67, 0x0F}, {0xCF, 0x6F, 0x0F}, {0xCF, 0x77, 0x0F},
    {0xCF, 0x7F, 0x0F}, {0xCF, 0x87, 0x17}, {0xC7, 0x87, 0x17}, {0xC7, 0x8F, 0x17},
    {0xC7, 0x97, 0x1F}, {0xBF, 0x9F, 0x1F}, {0xBF, 0x9F, 0x1F}, {0xBF, 0xA7, 0x27},
    {0xBF, 0xA7, 0x27}, {0xBF, 0xAF, 0x2F}, {0xB7, 0xAF, 0x2F}, {0xB7, 0xB7, 0x2F},
    {0xB7, 0xB7, 0x37}, {0xCF, 0xCF, 0x6F}, {0xDF, 0xDF, 0x9F}, {0xEF, 0xEF, 0xC7},
    {0xFF, 0xFF, 0xFF},
};

static pixel_t s_palette[LEVELS];
static int s_scale_idx;
static int s_scale;
static int s_w, s_h;               // grid size in cells
static uint8_t *s_fire;            // s_w * s_h intensities, row 0 at the top
static bool s_source_on = true;
static int64_t s_next_step_us;
static uint32_t s_rng;

// xorshift32, one call per cell per step
static inline uint32_t rng_next(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

static void scale_save(void)
{
    nvs_handle_t nvs;
    if (nvs_open("flame", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "scale", (uint8_t)s_scale_idx);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

static int scale_load(void)
{
    int idx = -1;
    nvs_handle_t nvs;
    if (nvs_open("flame", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "scale", &v) == ESP_OK && v < N_SCALES) idx = v;
        nvs_close(nvs);
    }
    if (idx < 0) {
        for (idx = 0; idx < N_SCALES - 1 && SCALES[idx] != DEFAULT_SCALE; idx++) {
        }
    }
    return idx;
}

// (Re)allocates the grid for the current scale: all black, source row set if on
static void grid_build(void)
{
    free(s_fire);
    s_scale = SCALES[s_scale_idx];
    s_w = (LCD_H_RES + s_scale - 1) / s_scale;
    s_h = (LCD_V_RES + s_scale - 1) / s_scale;
    size_t n = (size_t)s_w * s_h;
    s_fire = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fire) s_fire = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_fire);
    memset(s_fire, 0, n);
    if (s_source_on) memset(s_fire + (size_t)(s_h - 1) * s_w, LEVELS - 1, s_w);
    ESP_LOGI(TAG, "==== SCALE %d px ==== grid %dx%d", s_scale, s_w, s_h);
}

void flame_init(void)
{
    for (int i = 0; i < LEVELS; i++) {
        s_palette[i] = (pixel_t){PALETTE_RGB[i][0], PALETTE_RGB[i][1], PALETTE_RGB[i][2]};
    }
    s_rng = esp_random() | 1;
    s_scale_idx = scale_load();
    grid_build();
}

static void scale_set(int idx)
{
    s_scale_idx = (idx + N_SCALES) % N_SCALES;
    grid_build();
    scale_save();
}

void flame_next_scale(void) { scale_set(s_scale_idx + 1); }
void flame_prev_scale(void) { scale_set(s_scale_idx - 1); }
int flame_scale(void) { return s_scale; }

void flame_toggle_source(void)
{
    s_source_on = !s_source_on;
    if (s_source_on) memset(s_fire + (size_t)(s_h - 1) * s_w, LEVELS - 1, s_w);
    ESP_LOGI(TAG, "source %s", s_source_on ? "on" : "off");
}

// One step of the Doom fire. For every cell (x, y), y >= 1, x outer loop, y from top
// to bottom as in the original: r = random 0..3, the cell above shifted by 1 - r
// columns gets the value minus (r & 1). Black cells blacken the cell straight above.
// Columns wrap around.
static void fire_step(void)
{
    const int w = s_w;
    uint8_t *f = s_fire;
    for (int x = 0; x < w; x++) {
        for (int y = 1; y < s_h; y++) {
            uint8_t v = f[y * w + x];
            if (v == 0) {
                f[(y - 1) * w + x] = 0;
                continue;
            }
            uint32_t r = rng_next() & 3;
            int dx = x - (int)r + 1;
            if (dx < 0) dx += w;
            else if (dx >= w) dx -= w;
            f[(y - 1) * w + dx] = v - (r & 1);
        }
    }
    // Source off: the bottom row dies down by 0..3 per step
    if (!s_source_on) {
        uint8_t *row = f + (size_t)(s_h - 1) * w;
        for (int x = 0; x < w; x++) {
            int v = row[x] - (int)(rng_next() & 3);
            row[x] = v > 0 ? v : 0;
        }
    }
}

void flame_update(int64_t now_us)
{
    if (s_next_step_us == 0) s_next_step_us = now_us;
    int steps = 0;
    while (now_us >= s_next_step_us && steps < MAX_STEPS) {
        fire_step();
        s_next_step_us += STEP_US;
        steps++;
    }
    if (now_us >= s_next_step_us) s_next_step_us = now_us + STEP_US;
}

void flame_render(void)
{
    const int sc = s_scale;
    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();
        for (int y = y0; y <= y1; y++) {
            pixel_t *p = band + (y - y0) * LCD_H_RES;
            if (y > y0 && y / sc == (y - 1) / sc) {
                memcpy(p, p - LCD_H_RES, sizeof(pixel_t) * LCD_H_RES);
                continue;
            }
            const uint8_t *row = s_fire + (size_t)(y / sc) * s_w;
            int x = 0;
            for (int gx = 0; gx < s_w; gx++) {
                pixel_t c = s_palette[row[gx]];
                for (int i = 0; i < sc && x < LCD_H_RES; i++, x++) p[x] = c;
            }
        }
        panel_band_send(band, y0, y1);
    }
}
