#include "life.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "board_pins.h"
#include "esp_log.h"
#include "esp_random.h"
#include "panel.h"

#define CELL           20                   // cell size in px
#define GRID           (LCD_H_RES / CELL)   // cells per side
#define ORIGIN         ((LCD_H_RES - GRID * CELL) / 2)
#define GEN_US         3000000              // time between generations
#define FADE_TAU_S     0.8f                 // time constant of each of the two color filter stages
#define SLOTS          10                   // changing cells start fading in one of these time slots
#define SLOT_US        (GEN_US / SLOTS)
#define SEED_DENSITY   0.30f                // live cells after a random seed
#define AGE_FULL       12                   // generations from newborn to old color
#define STALE_GENS     20                   // repeating states this long: new seed
#define EMPTY_GENS     3                    // no live cells this long: new seed
#define CORNER_R       6.0f                 // rounded corner radius of a cell in px
#define CELL_GAP       2.0f                 // empty border on each side of a cell in px

static const char *TAG = "life";

static const float COLOR_DEAD[3]    = {14, 14, 24};
static const float COLOR_NEWBORN[3] = {255, 190, 70};
static const float COLOR_OLD[3]     = {50, 140, 255};

static bool s_inside[GRID][GRID];       // cell fully inside the circle
static uint8_t s_alive[GRID][GRID];
static uint8_t s_next[GRID][GRID];
static uint8_t s_prev[GRID][GRID];      // state before the last generation, shown until the slot starts
static uint8_t s_slot[GRID][GRID];      // time slot of the change, 0..SLOTS-1
static uint8_t s_age[GRID][GRID];       // generations alive, saturated at AGE_FULL (kept after death)
static float s_mid[GRID][GRID][3];      // first filter stage
static float s_color[GRID][GRID][3];    // displayed color (second stage): slow start, slow end
static pixel_t s_rgb[GRID][GRID];       // displayed color for the renderer
static uint8_t s_mask[CELL][CELL];      // cell shape coverage 0..16

static int64_t s_gen_us;
static int64_t s_last_us;
static uint32_t s_hash[3];              // last generations, to detect still lifes and short cycles
static int s_stale;
static int s_empty;

static float frand(void)
{
    return (float)esp_random() / (float)UINT32_MAX;
}

// Rounded square with a CELL_GAP border, coverage from 4x4 samples per pixel
static void mask_init(void)
{
    const float lo = CELL_GAP, hi = CELL - CELL_GAP;
    for (int y = 0; y < CELL; y++) {
        for (int x = 0; x < CELL; x++) {
            int n = 0;
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    float px = x + (sx + 0.5f) / 4;
                    float py = y + (sy + 0.5f) / 4;
                    if (px < lo || px > hi || py < lo || py > hi) continue;
                    float cx = fminf(fmaxf(px, lo + CORNER_R), hi - CORNER_R);
                    float cy = fminf(fmaxf(py, lo + CORNER_R), hi - CORNER_R);
                    float dx = px - cx, dy = py - cy;
                    if (dx * dx + dy * dy <= CORNER_R * CORNER_R) n++;
                }
            }
            s_mask[y][x] = n;
        }
    }
}

void life_init(void)
{
    mask_init();
    const float r = LCD_H_RES / 2.0f - 1.0f;
    const float c = LCD_H_RES / 2.0f;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            bool in = true;
            for (int k = 0; k < 4; k++) {
                float x = ORIGIN + (gx + (k & 1)) * CELL - c;
                float y = ORIGIN + (gy + (k >> 1)) * CELL - c;
                if (x * x + y * y > r * r) in = false;
            }
            s_inside[gy][gx] = in;
            for (int i = 0; i < 3; i++) s_mid[gy][gx][i] = s_color[gy][gx][i] = 0;
        }
    }
    life_randomize();
}

void life_randomize(void)
{
    int n = 0;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            s_prev[gy][gx] = s_alive[gy][gx];
            s_slot[gy][gx] = esp_random() % SLOTS;
            s_alive[gy][gx] = s_inside[gy][gx] && frand() < SEED_DENSITY;
            if (s_alive[gy][gx] && !s_prev[gy][gx]) s_age[gy][gx] = 0;
            n += s_alive[gy][gx];
        }
    }
    s_gen_us = s_last_us;
    s_stale = 0;
    s_empty = 0;
    ESP_LOGI(TAG, "new seed, %d cells", n);
}

void life_touch(int x, int y)
{
    int gx = (x - ORIGIN) / CELL;
    int gy = (y - ORIGIN) / CELL;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            int cx = gx + dx, cy = gy + dy;
            if (cx < 0 || cx >= GRID || cy < 0 || cy >= GRID || !s_inside[cy][cx]) continue;
            if ((dx == 0 && dy == 0) || frand() < 0.5f) {
                if (!s_alive[cy][cx]) s_age[cy][cx] = 0;
                s_alive[cy][cx] = 1;
                s_prev[cy][cx] = 1;     // shown at once
            }
        }
    }
    s_stale = 0;
}

static void step(void)
{
    uint32_t hash = 2166136261u;
    int n = 0;
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            int nb = 0;
            for (int dy = -1; dy <= 1; dy++) {
                int y = gy + dy;
                if (y < 0 || y >= GRID) continue;
                for (int dx = -1; dx <= 1; dx++) {
                    int x = gx + dx;
                    if (x < 0 || x >= GRID || (dx == 0 && dy == 0)) continue;
                    nb += s_alive[y][x];
                }
            }
            bool live = s_inside[gy][gx] && (nb == 3 || (nb == 2 && s_alive[gy][gx]));
            s_next[gy][gx] = live;
            hash = (hash ^ live) * 16777619u;
            n += live;
        }
    }
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            if (s_next[gy][gx]) {
                if (!s_alive[gy][gx]) s_age[gy][gx] = 0;
                else if (s_age[gy][gx] < AGE_FULL) s_age[gy][gx]++;
            }
            s_slot[gy][gx] = esp_random() % SLOTS;
        }
    }
    memcpy(s_prev, s_alive, sizeof(s_prev));
    memcpy(s_alive, s_next, sizeof(s_alive));

    // Still life or cycle of period 1..3: count; nothing alive: count separately
    if (hash == s_hash[0] || hash == s_hash[1] || hash == s_hash[2]) s_stale++;
    else s_stale = 0;
    s_hash[2] = s_hash[1];
    s_hash[1] = s_hash[0];
    s_hash[0] = hash;
    s_empty = n ? 0 : s_empty + 1;
    if (s_stale > STALE_GENS || s_empty > EMPTY_GENS) life_randomize();
}

void life_update(int64_t now_us)
{
    if (s_last_us == 0) {
        s_last_us = now_us;
        s_gen_us = now_us;
    }
    if (now_us - s_gen_us >= GEN_US) {
        s_gen_us = now_us;
        step();
    }

    float dt = (now_us - s_last_us) * 1e-6f;
    s_last_us = now_us;
    float k = 1.0f - expf(-dt / FADE_TAU_S);
    for (int gy = 0; gy < GRID; gy++) {
        for (int gx = 0; gx < GRID; gx++) {
            if (!s_inside[gy][gx]) continue;
            float target[3];
            bool started = now_us - s_gen_us >= (int64_t)s_slot[gy][gx] * SLOT_US;
            if (started ? s_alive[gy][gx] : s_prev[gy][gx]) {
                float t = (float)s_age[gy][gx] / AGE_FULL;
                for (int i = 0; i < 3; i++) target[i] = COLOR_NEWBORN[i] + (COLOR_OLD[i] - COLOR_NEWBORN[i]) * t;
            } else {
                for (int i = 0; i < 3; i++) target[i] = COLOR_DEAD[i];
            }
            float *m = s_mid[gy][gx];
            float *c = s_color[gy][gx];
            for (int i = 0; i < 3; i++) {
                m[i] += (target[i] - m[i]) * k;
                c[i] += (m[i] - c[i]) * k;
            }
            s_rgb[gy][gx] = (pixel_t){(uint8_t)(c[0] + 0.5f), (uint8_t)(c[1] + 0.5f), (uint8_t)(c[2] + 0.5f)};
        }
    }
}

void life_render(void)
{
    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();
        memset(band, 0, sizeof(pixel_t) * LCD_H_RES * (y1 - y0 + 1));
        for (int y = y0; y <= y1; y++) {
            int gy = (y - ORIGIN) / CELL;
            if (y < ORIGIN || gy >= GRID) continue;
            const uint8_t *m = s_mask[(y - ORIGIN) % CELL];
            pixel_t *p = band + (y - y0) * LCD_H_RES + ORIGIN;
            for (int gx = 0; gx < GRID; gx++, p += CELL) {
                if (!s_inside[gy][gx]) continue;
                pixel_t c = s_rgb[gy][gx];
                for (int i = 0; i < CELL; i++) {
                    p[i].r = (c.r * m[i]) >> 4;
                    p[i].g = (c.g * m[i]) >> 4;
                    p[i].b = (c.b * m[i]) >> 4;
                }
            }
        }
        panel_band_send(band, y0, y1);
    }
}
