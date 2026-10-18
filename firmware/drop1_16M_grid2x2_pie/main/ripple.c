#include "ripple.h"

#include <math.h>
#include <string.h>

#include "board_pins.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel.h"

// Simulation grid in float: one cell = 2x2 display pixels, the image is rebuilt
// with bilinear interpolation between cell centers.
// Pixel p maps to grid coordinate (p + GRID_OFF) / SCALE; index 0 and GRID_N - 1 are border cells.
#define SCALE          2
#define GRID_OFF       2
#define GRID_N         236
#define GRID_S         240        // row stride in cells: 480 bytes, a multiple of 16 for PIE loads
#define GRID_C         ((LCD_H_RES / 2.0f + GRID_OFF) / SCALE)   // circle center in grid units
#define GRID_R         (LCD_H_RES / 2.0f / SCALE - 0.5f)         // water radius in grid units

#define WAVE_C2        0.45f      // (wave speed)^2 in cells/step, stability limit 0.5; ~1.34 px/step (scale 3 had 1.41)
#define DAMPING        (1.0f - 1.0f / 64.0f)   // per step energy loss, closer to 1 = longer ripples
#define VISCOSITY      (1.0f / 32.0f)          // pull toward local average, damps short waves more
#define DROP_RADIUS_PX 3.5f   // min ~3 (1.5 cells), smaller bumps miss the grid
#define REBOUND_RADIUS_PX 3.0f
#define MAX_SPLASHES   8

#define SHADE_GAIN     (3.0f / 16.0f)  // slope to shade units; x1.5 vs scale 3 (slope is per cell)
#define SHADE_DEADBAND 1.5f            // |shade| below this shows the flat color
#define SHADE_SOFTNESS 140.0f          // shade value that reaches ~76% of the crest color
#define LUT_HALF       256             // shade range is -LUT_HALF..LUT_HALF-1
#define INTERP         (SCALE * SCALE) // bilinear weights sum
#define LUT_OFFSET     (LUT_HALF * INTERP)
#define LUT_DIV        4               // interpolated shade quantization for the LUT
#define LUT_LEVELS     (2 * LUT_OFFSET / LUT_DIV)
#define LUT_RINGS      32              // radial background colors, ~1 level apart, dithered in between
#define EDGE_POWER     2.0f            // higher = brightening closer to the edge only
#define PALETTE_START  0               // index into s_palettes used at boot

// Each tap gets a random crest tint between the palette crest color (tint 0)
// and the palette alt color (tint 1). The tint travels with the waves through a second wave
// field driven by strength * tint: the wave equation is linear, so that field is
// the tint-weighted sum of all drops, and slope2 / slope1 gives the local tint.

static const char *TAG = "ripple";

// Wave fields are stored as int16 (height * FIELD_SCALE) to halve memory traffic;
// the math is float. Stores truncate toward zero: symmetric, and small waves still decay.
#define FIELD_SCALE    2.0f
typedef int16_t cell_t;
static cell_t *s_cur;
static cell_t *s_prev;
static cell_t *s_tint_cur;                 // tint-weighted wave field
static cell_t *s_tint_prev;
static uint8_t s_alpha[LUT_LEVELS];        // crest blend amount per shade level (0 for troughs)
static int s_tint_delta[3];                // crest_alt - crest, per channel
static int16_t s_span_x0[GRID_N];          // first grid cell inside the circle, per grid row
static int16_t s_span_x1[GRID_N];          // last grid cell inside the circle, per grid row
static int16_t s_px_x0[LCD_V_RES];         // first visible pixel, per pixel row
static int16_t s_px_x1[LCD_V_RES];         // last visible pixel, per pixel row
// [ring][shade] -> RGB888; one small block per ring because internal RAM is fragmented
static pixel_t *s_lut[LUT_RINGS];
#define RING_TABLE_SIZE ((4 * (LCD_H_RES / 2) * (LCD_H_RES / 2) * 2 >> 8) + 1)
static uint16_t s_ring_of_r2[RING_TABLE_SIZE];   // (4 * r^2) >> 8 -> ring position * 16
// Dithered ring index per pixel, top half only (the bottom half mirrors it)
#define RING_MAP_ROWS  ((LCD_V_RES + 1) / 2)
static uint8_t *s_ring_map;

// 4x4 ordered dither thresholds (0..15): mixes two adjacent rings so the radial
// gradient has no visible steps despite the coarse RGB565 levels
static const uint8_t s_bayer[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

// Render state of one core: two cached rows of per-cell shade and tint,
// indexed by grid row parity, plus interpolation scratch rows
typedef struct {
    int16_t shade_rows[2][GRID_N];
    uint8_t tint_rows[2][GRID_N];          // per-cell tint 0..255
    int row_id[2];
    int32_t vrow[GRID_N];
    int32_t trow[GRID_N];
} render_ctx_t;

static render_ctx_t s_ctx[2];              // [0] = worker on core 0, [1] = caller

// Worker task on core 0: runs one job at a time for the caller on core 1
typedef void (*job_fn_t)(void *arg);
static TaskHandle_t s_worker;
static TaskHandle_t s_caller;
static job_fn_t s_job_fn;
static void *s_job_arg;

// Color palettes: flat water at center and at edge, crest (positive slope), trough (negative slope),
// alternate crest color for the per-tap tint
typedef struct {
    float base[3];
    float edge[3];
    float light[3];
    float deep[3];
    float alt[3];
} palette_t;

static const palette_t s_palettes[] = {
    {{255, 255, 255}, {215, 215, 215}, {40, 40, 40}, {150, 150, 150}, {100, 100, 100}}, // white, grey waves
    {{22, 22, 22}, {0, 0, 0}, {210, 210, 210}, {80, 80, 80}, {130, 130, 130}},          // black, grey waves
    {{0, 14, 60}, {0, 14, 60}, {20, 130, 85}, {0, 2, 20}, {170, 175, 40}},       // dark blue, emerald crests
    {{70, 24, 0}, {70, 24, 0}, {20, 130, 85}, {18, 5, 0}, {170, 175, 40}},       // dark orange, emerald crests
    {{55, 40, 80}, {55, 40, 80}, {20, 130, 85}, {14, 8, 24}, {170, 175, 40}},    // dark lavender, emerald crests
    {{28, 20, 40}, {10, 7, 16}, {30, 135, 40}, {5, 3, 9}, {170, 175, 40}},       // lavender, lighter center, darker edge, green crests, lemon alt
    {{255, 248, 170}, {250, 215, 0}, {90, 20, 140}, {0, 90, 30}, {30, 20, 120}}, // light yellow center, intense yellow edge, violet crests, green troughs, indigo alt
};
#define PALETTE_COUNT ((int)(sizeof(s_palettes) / sizeof(s_palettes[0])))

static int s_palette = PALETTE_START;

static inline uint8_t clamp_u8(float v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)lrintf(v);
}

static void build_lut(void)
{
    // Shade is compressed with tanh so strong waves never clip:
    // positive slope blends toward the crest color, negative toward the trough color.
    // Each ring has its own flat color, blended from center to edge.
    const palette_t *pal = &s_palettes[s_palette];
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
            if (ring == 0) {
                s_alpha[i] = t > 0 ? (uint8_t)lrintf(a * 255.0f) : 0;
            }
            s_lut[ring][i] = (pixel_t){
                .r = clamp_u8(base[0] + (target[0] - base[0]) * a),
                .g = clamp_u8(base[1] + (target[1] - base[1]) * a),
                .b = clamp_u8(base[2] + (target[2] - base[2]) * a),
            };
        }
    }

    for (int k = 0; k < 3; k++) {
        s_tint_delta[k] = (int)lrintf(pal->alt[k] - pal->light[k]);
    }
}

static void build_ring_table(void)
{
    // Ring position (x16, fractional part used by the dither) from squared distance;
    // which follows (r / R)^EDGE_POWER.
    const float R = LCD_H_RES / 2.0f;
    for (int i = 0; i < RING_TABLE_SIZE; i++) {
        float r = sqrtf((float)(i << 8) / 4.0f);
        float e = powf(fminf(r / R, 1.0f), EDGE_POWER);
        s_ring_of_r2[i] = (uint16_t)lrintf(e * (LUT_RINGS - 1) * 16);
    }
    for (int py = 0; py < RING_MAP_ROWS; py++) {
        int dy2 = 2 * py + 1 - LCD_V_RES;
        for (int px = 0; px < LCD_H_RES; px++) {
            int dx2 = 2 * px + 1 - LCD_H_RES;
            int ring = (s_ring_of_r2[(dx2 * dx2 + dy2 * dy2) >> 8] + s_bayer[py & 3][px & 3]) >> 4;
            s_ring_map[py * LCD_H_RES + px] = (uint8_t)(ring < LUT_RINGS ? ring : LUT_RINGS - 1);
        }
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

static void worker_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        s_job_fn(s_job_arg);
        xTaskNotifyGive(s_caller);
    }
}

// Starts fn(arg) on core 0; pair with job_wait()
static void job_start(job_fn_t fn, void *arg)
{
    s_caller = xTaskGetCurrentTaskHandle();
    s_job_fn = fn;
    s_job_arg = arg;
    xTaskNotifyGive(s_worker);
}

static void job_wait(void)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void build_coefs(void);
static bool pie_self_test(void);

esp_err_t ripple_init(void)
{
    size_t bytes = GRID_N * GRID_S * sizeof(cell_t);
    ESP_LOGI(TAG, "largest internal block %u bytes, grid needs %u",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)bytes);
    // Each buffer falls back to PSRAM on its own when internal RAM is too small
    s_cur = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_cur) s_cur = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM);
    s_prev = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_prev) s_prev = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "height grids: cur %s, prev %s",
             esp_ptr_internal(s_cur) ? "internal" : "PSRAM", esp_ptr_internal(s_prev) ? "internal" : "PSRAM");
    ESP_RETURN_ON_FALSE(s_cur && s_prev, ESP_ERR_NO_MEM, TAG, "grid buffers");
    s_tint_cur = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_tint_cur) s_tint_cur = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM);
    s_tint_prev = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_tint_prev) s_tint_prev = heap_caps_aligned_calloc(16, 1, bytes, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "tint grids: cur %s, prev %s",
             esp_ptr_internal(s_tint_cur) ? "internal" : "PSRAM", esp_ptr_internal(s_tint_prev) ? "internal" : "PSRAM");
    ESP_RETURN_ON_FALSE(s_tint_cur && s_tint_prev, ESP_ERR_NO_MEM, TAG, "tint buffers");
    for (int i = 0; i < LUT_RINGS; i++) {
        s_lut[i] = heap_caps_malloc(sizeof(pixel_t) * LUT_LEVELS, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_lut[i], ESP_ERR_NO_MEM, TAG, "lut");
    }
    s_ring_map = heap_caps_malloc(RING_MAP_ROWS * LCD_H_RES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ring_map) s_ring_map = heap_caps_malloc(RING_MAP_ROWS * LCD_H_RES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_ring_map, ESP_ERR_NO_MEM, TAG, "ring map");
    build_spans();
    build_ring_table();
    build_lut();
    build_coefs();
    ESP_RETURN_ON_FALSE(pie_self_test(), ESP_FAIL, TAG, "PIE self test");
    BaseType_t ok = xTaskCreatePinnedToCore(worker_task, "ripple_w", 4096, NULL, 5, &s_worker, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "worker task");
    ESP_LOGI(TAG, "int16 grid %dx%d ready, %u bytes free internal", GRID_N, GRID_N,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return ESP_OK;
}

static inline cell_t to_cell(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32767.0f) return -32767;
    return (cell_t)v;   // truncates toward zero
}

static inline bool cell_inside(int gx, int gy)
{
    return gy >= 1 && gy <= GRID_N - 2 && gx >= s_span_x0[gy] && gx <= s_span_x1[gy];
}

// Pushes a cosine bump into the surface (positive strength = down).
// Center and radius are in pixels.
static void apply_bump(float px, float py, float radius_px, float strength, float tint)
{
    float gx = (px + GRID_OFF) / SCALE;
    float gy = (py + GRID_OFF) / SCALE;
    float r = radius_px / SCALE;
    for (int y = (int)floorf(gy - r); y <= (int)ceilf(gy + r); y++) {
        for (int x = (int)floorf(gx - r); x <= (int)ceilf(gx + r); x++) {
            if (!cell_inside(x, y)) continue;
            float dx = x - gx;
            float dy = y - gy;
            float d = sqrtf(dx * dx + dy * dy) / r;
            if (d > 1.0f) continue;
            float w = 0.5f * (1.0f + cosf(d * (float)M_PI));
            int i = y * GRID_S + x;
            s_cur[i] = to_cell(s_cur[i] - strength * w * FIELD_SCALE);
            s_tint_cur[i] = to_cell(s_tint_cur[i] - strength * tint * w * FIELD_SCALE);
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
    {20, -0.20f, REBOUND_RADIUS_PX},
    {38, 0.10f, REBOUND_RADIUS_PX},
};
#define PULSE_COUNT (sizeof(s_pulses) / sizeof(s_pulses[0]))

typedef struct {
    bool active;
    float x, y;       // pixels
    float strength;
    float tint;
    int age;          // sim steps since impact
    int next;         // index of the next pulse
} splash_t;

static splash_t s_splashes[MAX_SPLASHES];

void ripple_set_palette(int step)
{
    // Called between frames: the LUT is rewritten in place (no extra RAM)
    s_palette = ((s_palette + step) % PALETTE_COUNT + PALETTE_COUNT) % PALETTE_COUNT;
    int64_t t0 = esp_timer_get_time();
    build_lut();
    ESP_LOGI(TAG, "palette %d (%d us)", s_palette, (int)(esp_timer_get_time() - t0));
}

void ripple_drop(int x, int y, int strength, bool splash, float tint)
{
    if (!splash) {
        apply_bump(x, y, DROP_RADIUS_PX, strength, tint);
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
    *slot = (splash_t){.active = true, .x = x, .y = y, .strength = strength, .tint = tint};
}

static void update_splashes(void)
{
    for (int i = 0; i < MAX_SPLASHES; i++) {
        splash_t *s = &s_splashes[i];
        if (!s->active) continue;
        while (s->next < PULSE_COUNT && s_pulses[s->next].step <= s->age) {
            const pulse_t *p = &s_pulses[s->next];
            apply_bump(s->x, s->y, p->radius_px, s->strength * p->gain, s->tint);
            s->next++;
        }
        s->age++;
        if (s->next >= PULSE_COUNT) s->active = false;
    }
}

// Fixed-point wave update, the float formula expanded into one weighted sum:
//   v = 2c - p + C2 * (sum - 4c);  v += VISC * (sum / 4 - v);  v *= DAMPING
//   -> v = cc * c + cp * p + cs * sum, with k = DAMPING * (1 - VISC):
//      cc = k * (2 - 4 C2), cp = -k, cs = k * C2 + DAMPING * VISC / 4   (Q15)
void pie_wave_row(int16_t *prev, const int16_t *cur, int blocks, const int16_t *coef, int stride);
static int16_t s_coef[3][8] __attribute__((aligned(16)));

static void build_coefs(void)
{
    float k = DAMPING * (1.0f - VISCOSITY);
    float c[3] = {k * (2.0f - 4.0f * WAVE_C2), -k, k * WAVE_C2 + DAMPING * VISCOSITY / 4.0f};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 8; j++) {
            s_coef[i][j] = (int16_t)lrintf(c[i] * 32768.0f);
        }
    }
}

// Scalar reference of pie_wave_row for one cell
static inline cell_t wave_cell_ref(const cell_t *c, const cell_t *p, int x)
{
    int32_t acc = s_coef[0][0] * c[x] + s_coef[1][0] * p[x] +
                  s_coef[2][0] * (c[x - 1] + c[x + 1] + c[x - GRID_S] + c[x + GRID_S]);
    int32_t v = acc >> 15;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    if (v < 0) v++;
    return (cell_t)v;
}

// Runs the vector kernel and the scalar reference on random rows and compares them
static bool pie_self_test(void)
{
    const int rows = 3;
    cell_t *cur = heap_caps_aligned_calloc(16, rows * GRID_S, sizeof(cell_t), MALLOC_CAP_INTERNAL);
    cell_t *prev = heap_caps_aligned_calloc(16, rows * GRID_S, sizeof(cell_t), MALLOC_CAP_INTERNAL);
    cell_t *ref = heap_caps_calloc(GRID_S, sizeof(cell_t), MALLOC_CAP_INTERNAL);
    if (!cur || !prev || !ref) return false;
    for (int i = 0; i < rows * GRID_S; i++) {
        cur[i] = (cell_t)(esp_random() % 40000 - 20000);
        prev[i] = (cell_t)(esp_random() % 40000 - 20000);
    }
    // Small values too, where the rounding matters
    for (int i = GRID_S + 64; i < GRID_S + 128; i++) {
        cur[i] = (cell_t)(esp_random() % 9 - 4);
        prev[i] = (cell_t)(esp_random() % 9 - 4);
    }
    cell_t *c = cur + GRID_S;
    cell_t *p = prev + GRID_S;
    for (int x = 8; x < GRID_S - 8; x++) {
        ref[x] = wave_cell_ref(c, p, x);
    }
    pie_wave_row(p + 8, c + 8, (GRID_S - 16) / 8, &s_coef[0][0], GRID_S * sizeof(cell_t));
    int bad = 0;
    for (int x = 8; x < GRID_S - 8; x++) {
        if (p[x] != ref[x]) {
            if (bad < 5) ESP_LOGE(TAG, "PIE x=%d got %d want %d", x, p[x], ref[x]);
            bad++;
        }
    }
    ESP_LOGI(TAG, "PIE self test: %d mismatches of %d (coef %d %d %d)", bad, GRID_S - 16,
             s_coef[0][0], s_coef[1][0], s_coef[2][0]);
    heap_caps_free(cur);
    heap_caps_free(prev);
    heap_caps_free(ref);
    return bad == 0;
}

// One step of the wave equation on a field pair, then swaps the pair.
// Rows are processed in aligned 8-cell blocks; cells of a block outside the circle
// are reset to 0 afterwards (reflecting edge).
static IRAM_ATTR void wave_step(cell_t **cur, cell_t **prev)
{
    for (int y = 1; y < GRID_N - 1; y++) {
        int x0 = s_span_x0[y];
        int x1 = s_span_x1[y];
        if (x1 < x0) continue;
        int xs = x0 & ~7;
        int xe = x1 | 7;
        const cell_t *c = *cur + y * GRID_S;
        cell_t *p = *prev + y * GRID_S;
        pie_wave_row(p + xs, c + xs, (xe - xs + 1) / 8, &s_coef[0][0], GRID_S * sizeof(cell_t));
        for (int x = xs; x < x0; x++) p[x] = 0;
        for (int x = x1 + 1; x <= xe; x++) p[x] = 0;
    }
    cell_t *t = *cur;
    *cur = *prev;
    *prev = t;
}

static void tint_step_job(void *arg)
{
    wave_step(&s_tint_cur, &s_tint_prev);
}

void ripple_step(void)
{
    update_splashes();

    // Two-buffer wave equation: next = 2*cur - prev + c^2 * laplacian(cur),
    // plus viscosity and damping. Cells outside the circle stay 0 (reflecting edge).
    // The tint field runs on core 0 in parallel with the height field.
    job_start(tint_step_job, NULL);
    wave_step(&s_cur, &s_prev);
    job_wait();
}

// Per-cell shade (light from the top-left) and tint, cached for two grid rows at a time
static IRAM_ATTR const int16_t *shade_row(render_ctx_t *ctx, int gy)
{
    int slot = gy & 1;
    if (ctx->row_id[slot] == gy) {
        return ctx->shade_rows[slot];
    }
    int16_t *out = ctx->shade_rows[slot];
    uint8_t *tint_out = ctx->tint_rows[slot];
    memset(out, 0, sizeof(ctx->shade_rows[0]));
    memset(tint_out, 0, sizeof(ctx->tint_rows[0]));
    if (gy >= 1 && gy <= GRID_N - 2) {
        const cell_t *h = s_cur + gy * GRID_S;
        const cell_t *h2 = s_tint_cur + gy * GRID_S;
        // Cells outside the circle are 0, so only the span plus one cell per side can be nonzero
        int xa = s_span_x0[gy] - 1;
        int xb = s_span_x1[gy] + 1;
        if (xa < 1) xa = 1;
        if (xb > GRID_N - 2) xb = GRID_N - 2;
        for (int x = xa; x <= xb; x++) {
            int slope_i = (h[x - 1] - h[x + 1]) + (h[x - GRID_S] - h[x + GRID_S]);
            float slope = (float)slope_i * (1.0f / FIELD_SCALE);
            // Tint is only used on crests (positive shade)
            if (slope > SHADE_DEADBAND / SHADE_GAIN) {
                int slope2 = (h2[x - 1] - h2[x + 1]) + (h2[x - GRID_S] - h2[x + GRID_S]);
                float tint = (float)slope2 / (float)slope_i;
                tint_out[x] = tint <= 0 ? 0 : tint >= 1 ? 255 : (uint8_t)(tint * 255.0f);
            }
            float s = slope * SHADE_GAIN;
            // Soft threshold: tiny slopes show the flat color
            if (s > SHADE_DEADBAND) s -= SHADE_DEADBAND;
            else if (s < -SHADE_DEADBAND) s += SHADE_DEADBAND;
            else s = 0;
            if (s > LUT_HALF - 1) s = LUT_HALF - 1;
            if (s < -(LUT_HALF - 1)) s = -(LUT_HALF - 1);
            out[x] = (int16_t)lrintf(s);
        }
    }
    ctx->row_id[slot] = gy;
    return out;
}

// Renders display rows y_from..y_to into band (whose first row is band_y0)
static IRAM_ATTR void render_rows(render_ctx_t *ctx, pixel_t *band, int band_y0, int y_from, int y_to)
{
    int32_t *vrow = ctx->vrow;
    int32_t *trow = ctx->trow;
    for (int py = y_from; py <= y_to; py++) {
        pixel_t *row = band + (py - band_y0) * LCD_H_RES;
        int x0 = s_px_x0[py];
        int x1 = s_px_x1[py];

        // Vertical interpolation between grid rows i0 and i0 + 1
        int g = py + GRID_OFF;
        int i0 = g / SCALE;
        int fy = g % SCALE;
        const int16_t *r0 = shade_row(ctx, i0);
        const int16_t *r1 = shade_row(ctx, i0 + 1);
        const uint8_t *t0 = ctx->tint_rows[i0 & 1];
        const uint8_t *t1 = ctx->tint_rows[(i0 + 1) & 1];
        int j_first = (x0 + GRID_OFF) / SCALE;
        int j_last = (x1 + GRID_OFF) / SCALE + 1;
        for (int j = j_first; j <= j_last; j++) {
            vrow[j] = r0[j] * (SCALE - fy) + r1[j] * fy;
            trow[j] = t0[j] * (SCALE - fy) + t1[j] * fy;
        }

        memset(row, 0, x0 * sizeof(pixel_t));
        int map_y = py < RING_MAP_ROWS ? py : LCD_V_RES - 1 - py;
        const uint8_t *ring_row = s_ring_map + map_y * LCD_H_RES;
        // Horizontal interpolation, weights sum to SCALE * SCALE
        for (int px = x0; px <= x1; px++) {
            int gx = px + GRID_OFF;
            int j = gx / SCALE;
            int fx = gx % SCALE;
            int v = vrow[j] * (SCALE - fx) + vrow[j + 1] * fx;
            int level = (v + LUT_OFFSET) / LUT_DIV;
            pixel_t c = s_lut[ring_row[px]][level];
            // Shift the crest color toward crest_alt by the local tint
            int alpha = s_alpha[level];
            if (alpha) {
                int tint = (trow[j] * (SCALE - fx) + trow[j + 1] * fx) / INTERP;
                int k = alpha * tint;   // 0..65025
                c.r = (uint8_t)(c.r + ((k * s_tint_delta[0]) >> 16));
                c.g = (uint8_t)(c.g + ((k * s_tint_delta[1]) >> 16));
                c.b = (uint8_t)(c.b + ((k * s_tint_delta[2]) >> 16));
            }
            row[px] = c;
        }
        memset(row + x1 + 1, 0, (LCD_H_RES - 1 - x1) * sizeof(pixel_t));
    }
}

typedef struct {
    pixel_t *band;
    int band_y0;
    int y_from;
    int y_to;
} render_job_t;

static void render_job(void *arg)
{
    render_job_t *job = arg;
    render_rows(&s_ctx[0], job->band, job->band_y0, job->y_from, job->y_to);
}

void ripple_render(void)
{
    for (int i = 0; i < 2; i++) {
        s_ctx[i].row_id[0] = -1;
        s_ctx[i].row_id[1] = -1;
    }

    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();

        // Top half of the band on core 0, bottom half here
        int mid = (y0 + y1) / 2;
        render_job_t job = {.band = band, .band_y0 = y0, .y_from = y0, .y_to = mid};
        job_start(render_job, &job);
        render_rows(&s_ctx[1], band, y0, mid + 1, y1);
        job_wait();

        panel_band_send(band, y0, y1);
    }
}
