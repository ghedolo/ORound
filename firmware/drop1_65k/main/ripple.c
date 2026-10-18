#include "ripple.h"

#include <math.h>
#include <string.h>

#include "board_pins.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "panel.h"

// Simulation grid in float: one cell = 3x3 display pixels, the image is rebuilt
// with bilinear interpolation between cell centers.
// Pixel p maps to grid coordinate (p + 2) / 3; index 0 and GRID_N - 1 are border cells.
#define SCALE          3
#define GRID_N         158
#define GRID_C         ((LCD_H_RES / 2.0f + 2.0f) / SCALE)       // circle center in grid units
#define GRID_R         (LCD_H_RES / 2.0f / SCALE - 0.5f)         // water radius in grid units

#define WAVE_C2        0.22f      // (wave speed)^2 in cells/step; 0.22 at scale 3 = old speed at scale 2
#define DAMPING        (1.0f - 1.0f / 64.0f)   // per step energy loss, closer to 1 = longer ripples
#define VISCOSITY      (1.0f / 32.0f)          // pull toward local average, damps short waves more
#define DROP_RADIUS_PX 5.0f   // min ~4.5 (1.5 cells), smaller bumps miss the grid
#define MAX_SPLASHES   8

#define SHADE_GAIN     (1.0f / 8.0f)   // slope to shade units
#define SHADE_DEADBAND 1.5f            // |shade| below this shows the flat color
#define SHADE_SOFTNESS 140.0f          // shade value that reaches ~76% of the crest color
#define LUT_HALF       256             // shade range is -LUT_HALF..LUT_HALF-1
#define INTERP         (SCALE * SCALE) // bilinear weights sum
#define LUT_OFFSET     (LUT_HALF * INTERP)
#define LUT_DIV        4               // interpolated shade quantization for the LUT
#define LUT_LEVELS     (2 * LUT_OFFSET / LUT_DIV)
#define LUT_RINGS      9               // radial background colors, ~1 RGB565 step apart
#define EDGE_POWER     2.5f            // higher = brightening closer to the edge only
#define PALETTE        2               // index into s_palettes

static const char *TAG = "ripple";

static float *s_cur;
static float *s_prev;
static int16_t s_span_x0[GRID_N];          // first grid cell inside the circle, per grid row
static int16_t s_span_x1[GRID_N];          // last grid cell inside the circle, per grid row
static int16_t s_px_x0[LCD_V_RES];         // first visible pixel, per pixel row
static int16_t s_px_x1[LCD_V_RES];         // last visible pixel, per pixel row
static uint16_t s_lut[LUT_RINGS][LUT_LEVELS];  // [ring][shade] -> RGB565 big endian
#define RING_TABLE_SIZE ((4 * (LCD_H_RES / 2) * (LCD_H_RES / 2) * 2 >> 8) + 1)
static uint8_t s_ring_of_r2[RING_TABLE_SIZE];   // (4 * r^2) >> 8 -> ring position * 16

// 4x4 ordered dither thresholds (0..15): mixes two adjacent rings so the radial
// gradient has no visible steps despite the coarse RGB565 levels
static const uint8_t s_bayer[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Two cached rows of per-cell shade, indexed by grid row parity
static int16_t s_shade_rows[2][GRID_N];
static int s_shade_row_id[2];

// Color palettes: flat water at center and at edge, crest (positive slope), trough (negative slope)
typedef struct {
    float base[3];
    float edge[3];
    float light[3];
    float deep[3];
} palette_t;

static const palette_t s_palettes[] = {
    {{0, 14, 60}, {0, 14, 60}, {20, 130, 85}, {0, 2, 20}},      // 0: dark blue, emerald crests
    {{70, 24, 0}, {70, 24, 0}, {20, 130, 85}, {18, 5, 0}},      // 1: dark orange, emerald crests
    {{55, 40, 80}, {55, 40, 80}, {20, 130, 85}, {14, 8, 24}},   // 2: dark lavender, emerald crests
    {{30, 21, 46}, {70, 52, 102}, {20, 130, 85}, {10, 6, 18}},  // 3: lavender, darker center, lighter edge
};

static inline uint16_t rgb565_be(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static void build_lut(void)
{
    // Shade is compressed with tanh so strong waves never clip:
    // positive slope blends toward the crest color, negative toward the trough color.
    // Each ring has its own flat color, blended from center to edge.
    const palette_t *pal = &s_palettes[PALETTE];
    for (int ring = 0; ring < LUT_RINGS; ring++) {
        float e = (float)ring / (LUT_RINGS - 1);
        float base[3];
        for (int k = 0; k < 3; k++) {
            base[k] = pal->base[k] + (pal->edge[k] - pal->base[k]) * e;
        }
        for (int i = 0; i < LUT_LEVELS; i++) {
            float shade = (float)(i * LUT_DIV - LUT_OFFSET) / INTERP;
            float t = tanhf(shade / SHADE_SOFTNESS);
            const float *target = t >= 0 ? pal->light : pal->deep;
            float a = fabsf(t);
            int r = (int)(base[0] + (target[0] - base[0]) * a);
            int g = (int)(base[1] + (target[1] - base[1]) * a);
            int b = (int)(base[2] + (target[2] - base[2]) * a);
            s_lut[ring][i] = rgb565_be(r, g, b);
        }
    }

    // Ring position (x16, fractional part used by the dither) from squared distance;
    // which follows (r / R)^EDGE_POWER.
    const float R = LCD_H_RES / 2.0f;
    for (int i = 0; i < RING_TABLE_SIZE; i++) {
        float r = sqrtf((float)(i << 8) / 4.0f);
        float e = powf(fminf(r / R, 1.0f), EDGE_POWER);
        s_ring_of_r2[i] = (uint8_t)lrintf(e * (LUT_RINGS - 1) * 16);
    }
}

static void build_spans(void)
{
    for (int y = 0; y < GRID_N; y++) {
        float dy = y - GRID_C;
        float d = GRID_R * GRID_R - dy * dy;
        if (d <= 0 || y < 1 || y > GRID_N - 2) {
            s_span_x0[y] = 1;
            s_span_x1[y] = 0;   // empty
            continue;
        }
        float half = sqrtf(d);
        int x0 = (int)ceilf(GRID_C - half);
        int x1 = (int)floorf(GRID_C + half);
        if (x0 < 1) x0 = 1;
        if (x1 > GRID_N - 2) x1 = GRID_N - 2;
        s_span_x0[y] = x0;
        s_span_x1[y] = x1;
    }

    const float r = LCD_H_RES / 2.0f;
    for (int y = 0; y < LCD_V_RES; y++) {
        float dy = y + 0.5f - r;
        float half = sqrtf(fmaxf(r * r - dy * dy, 0));
        int x0 = (int)(r - half + 0.5f);
        int x1 = (int)(r + half - 0.5f);
        if (x0 < 0) x0 = 0;
        if (x1 > LCD_H_RES - 1) x1 = LCD_H_RES - 1;
        s_px_x0[y] = x0;
        s_px_x1[y] = x1;
    }
}

esp_err_t ripple_init(void)
{
    size_t bytes = GRID_N * GRID_N * sizeof(float);
    s_cur = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_prev = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_cur || !s_prev) {
        ESP_LOGW(TAG, "internal RAM too small, using PSRAM");
        heap_caps_free(s_cur);
        heap_caps_free(s_prev);
        s_cur = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
        s_prev = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    }
    ESP_RETURN_ON_FALSE(s_cur && s_prev, ESP_ERR_NO_MEM, TAG, "grid buffers");
    build_spans();
    build_lut();
    ESP_LOGI(TAG, "float grid %dx%d ready, %u bytes free internal", GRID_N, GRID_N,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

static inline bool cell_inside(int gx, int gy)
{
    return gy >= 1 && gy <= GRID_N - 2 && gx >= s_span_x0[gy] && gx <= s_span_x1[gy];
}

// Pushes a cosine bump into the surface (positive strength = down).
// Center and radius are in pixels.
static void apply_bump(float px, float py, float radius_px, float strength)
{
    float gx = (px + 2.0f) / SCALE;
    float gy = (py + 2.0f) / SCALE;
    float r = radius_px / SCALE;
    for (int y = (int)floorf(gy - r); y <= (int)ceilf(gy + r); y++) {
        for (int x = (int)floorf(gx - r); x <= (int)ceilf(gx + r); x++) {
            if (!cell_inside(x, y)) continue;
            float dx = x - gx;
            float dy = y - gy;
            float d = sqrtf(dx * dx + dy * dy) / r;
            if (d > 1.0f) continue;
            float w = 0.5f * (1.0f + cosf(d * (float)M_PI));
            s_cur[y * GRID_N + x] -= strength * w;
        }
    }
}

// A heavy drop: the main impact, then the water rebounds at the impact point
// with alternating sign and decreasing size, emitting smaller trailing rings.
typedef struct {
    int step;         // sim steps after impact
    float gain;       // fraction of the drop strength, sign = direction
    float radius_px;
} pulse_t;

static const pulse_t s_pulses[] = {
    {0, 1.00f, DROP_RADIUS_PX},
    {20, -0.20f, 4.5f},
    {38, 0.10f, 4.5f},
};
#define PULSE_COUNT (sizeof(s_pulses) / sizeof(s_pulses[0]))

typedef struct {
    bool active;
    float x, y;       // pixels
    float strength;
    int age;          // sim steps since impact
    int next;         // index of the next pulse
} splash_t;

static splash_t s_splashes[MAX_SPLASHES];

void ripple_drop(int x, int y, int strength, bool splash)
{
    if (!splash) {
        apply_bump(x, y, DROP_RADIUS_PX, strength);
        return;
    }
    // Reuse a free slot, or the oldest one
    splash_t *slot = &s_splashes[0];
    for (int i = 0; i < MAX_SPLASHES; i++) {
        if (!s_splashes[i].active) {
            slot = &s_splashes[i];
            break;
        }
        if (s_splashes[i].age > slot->age) slot = &s_splashes[i];
    }
    *slot = (splash_t){.active = true, .x = x, .y = y, .strength = strength};
}

static void update_splashes(void)
{
    for (int i = 0; i < MAX_SPLASHES; i++) {
        splash_t *s = &s_splashes[i];
        if (!s->active) continue;
        while (s->next < PULSE_COUNT && s_pulses[s->next].step <= s->age) {
            const pulse_t *p = &s_pulses[s->next];
            apply_bump(s->x, s->y, p->radius_px, s->strength * p->gain);
            s->next++;
        }
        s->age++;
        if (s->next >= PULSE_COUNT) s->active = false;
    }
}

void ripple_step(void)
{
    update_splashes();

    // Two-buffer wave equation: next = 2*cur - prev + c^2 * laplacian(cur),
    // plus viscosity and damping. Cells outside the circle stay 0 (reflecting edge).
    for (int y = 1; y < GRID_N - 1; y++) {
        int x0 = s_span_x0[y];
        int x1 = s_span_x1[y];
        const float *c = s_cur + y * GRID_N;
        float *p = s_prev + y * GRID_N;
        for (int x = x0; x <= x1; x++) {
            float sum = c[x - 1] + c[x + 1] + c[x - GRID_N] + c[x + GRID_N];
            float v = 2.0f * c[x] - p[x] + WAVE_C2 * (sum - 4.0f * c[x]);
            v += VISCOSITY * (0.25f * sum - v);
            v *= DAMPING;
            // Avoid denormal floats, which are slow
            if (fabsf(v) < 1e-3f) v = 0.0f;
            p[x] = v;
        }
    }
    float *t = s_cur;
    s_cur = s_prev;
    s_prev = t;
}

// Per-cell shade (light from the top-left), cached for two grid rows at a time
static const int16_t *shade_row(int gy)
{
    int slot = gy & 1;
    if (s_shade_row_id[slot] == gy) {
        return s_shade_rows[slot];
    }
    int16_t *out = s_shade_rows[slot];
    memset(out, 0, sizeof(s_shade_rows[0]));
    if (gy >= 1 && gy <= GRID_N - 2) {
        const float *h = s_cur + gy * GRID_N;
        for (int x = 1; x < GRID_N - 1; x++) {
            float s = ((h[x - 1] - h[x + 1]) + (h[x - GRID_N] - h[x + GRID_N])) * SHADE_GAIN;
            // Soft threshold: tiny slopes show the flat color
            if (s > SHADE_DEADBAND) s -= SHADE_DEADBAND;
            else if (s < -SHADE_DEADBAND) s += SHADE_DEADBAND;
            else s = 0;
            if (s > LUT_HALF - 1) s = LUT_HALF - 1;
            if (s < -(LUT_HALF - 1)) s = -(LUT_HALF - 1);
            out[x] = (int16_t)lrintf(s);
        }
    }
    s_shade_row_id[slot] = gy;
    return out;
}

void ripple_render(void)
{
    s_shade_row_id[0] = -1;
    s_shade_row_id[1] = -1;
    int32_t vrow[GRID_N];

    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        uint16_t *band = panel_band_acquire();

        for (int py = y0; py <= y1; py++) {
            uint16_t *row = band + (py - y0) * LCD_H_RES;
            int x0 = s_px_x0[py];
            int x1 = s_px_x1[py];

            // Vertical interpolation between grid rows i0 and i0 + 1
            int g = py + 2;
            int i0 = g / SCALE;
            int fy = g % SCALE;
            const int16_t *r0 = shade_row(i0);
            const int16_t *r1 = shade_row(i0 + 1);
            int j_first = (x0 + 2) / SCALE;
            int j_last = (x1 + 2) / SCALE + 1;
            for (int j = j_first; j <= j_last; j++) {
                vrow[j] = r0[j] * (SCALE - fy) + r1[j] * fy;
            }

            memset(row, 0, x0 * sizeof(uint16_t));
            // Distances in half pixels from the panel center: d = 2 * p + 1 - LCD_H_RES
            int dy2 = 2 * py + 1 - LCD_V_RES;
            int dy2_sq = dy2 * dy2;
            const uint8_t *bayer_row = s_bayer[py & 3];
            // Horizontal interpolation, weights sum to SCALE * SCALE
            for (int px = x0; px <= x1; px++) {
                int gx = px + 2;
                int j = gx / SCALE;
                int fx = gx % SCALE;
                int v = vrow[j] * (SCALE - fx) + vrow[j + 1] * fx;
                int dx2 = 2 * px + 1 - LCD_H_RES;
                int ring = (s_ring_of_r2[(dx2 * dx2 + dy2_sq) >> 8] + bayer_row[px & 3]) >> 4;
                row[px] = s_lut[ring][(v + LUT_OFFSET) / LUT_DIV];
            }
            memset(row + x1 + 1, 0, (LCD_H_RES - 1 - x1) * sizeof(uint16_t));
        }
        panel_band_send(band, y0, y1);
    }
}
