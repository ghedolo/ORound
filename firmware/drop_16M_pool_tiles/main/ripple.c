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

// Simulation grid: one cell = 2x2 display pixels, the image is rebuilt
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

// Pool floor: a tiled grid seen through the water. Each pixel samples the floor at
// its own position shifted by the surface slope (refraction), so waves bend the lines.
#define TILE_W_PX      80              // tile width
#define TILE_H_PX      48              // tile height
#define LINE_CORE_PX   5.0f            // grid line: full color core width
#define LINE_FADE_PX   2.0f            // grid line: soft edge width on each side
// Even core widths need the line center between two pixels to stay symmetric
#define LINE_CENTER_SUB (((int)LINE_CORE_PX % 2 == 0) ? SUB / 2 : 0)
#define REFRACT_PX     0.020f          // floor shift in px per unit of stored slope
#define MAX_SHIFT_PX   24              // shift clamp
#define SUB            16              // sub-pixel units per pixel for the floor lookup
#define COVER_LEVELS   (SUB + 1)       // line coverage 0..SUB
#define INTERP         (SCALE * SCALE) // bilinear weights sum
#define LUT_RINGS      32              // radial background colors, ~1 level apart, dithered in between
#define EDGE_POWER     2.0f            // higher = darkening closer to the edge only
#define GAMMA          2.2f            // panel response: light ~ (value / 255)^GAMMA
#define PALETTE_START  0               // index into s_palettes used at boot

static const char *TAG = "ripple";

// Wave fields are stored as int16 (height * FIELD_SCALE) to halve memory traffic;
// the math is float. Stores truncate toward zero: symmetric, and small waves still decay.
#define FIELD_SCALE    2.0f
typedef int16_t cell_t;
static cell_t *s_cur;
static cell_t *s_prev;
static int16_t s_span_x0[GRID_N];          // first grid cell inside the circle, per grid row
static int16_t s_span_x1[GRID_N];          // last grid cell inside the circle, per grid row
static int16_t s_px_x0[LCD_V_RES];         // first visible pixel, per pixel row
static int16_t s_px_x1[LCD_V_RES];         // last visible pixel, per pixel row
// Floor u coordinate of each pixel column modulo the tile width (1/SUB px);
// a grid line crosses the panel center
static int16_t s_u_mod[LCD_H_RES];
static uint8_t s_u_tile[LCD_H_RES];        // tile column of each pixel column (mod 8)
// Line coverage 0..SUB by distance from the line center (1/SUB px)
#define COVER_TABLE    128             // >= (LINE_CORE_PX / 2 + LINE_FADE_PX) * SUB + 1
static uint8_t s_cover[COVER_TABLE];
// [ring][line coverage] -> RGB888
// Each tile gets one of TILE_VARIANTS floor shades, picked by its position
#define TILE_VARIANTS  8
static pixel_t s_lut[LUT_RINGS][TILE_VARIANTS][COVER_LEVELS];
static uint8_t s_tile_variant[8][8];       // [tile row mod 8][tile column mod 8]
// Per variant: brightness and green balance offsets (-1..1), scaled by the palette variation
static const float s_variant_shift[TILE_VARIANTS][2] = {
    {0.0f, 0.0f}, {0.6f, -0.3f}, {-0.5f, 0.4f}, {0.9f, 0.5f},
    {-0.9f, -0.2f}, {0.3f, 0.9f}, {-0.3f, -0.8f}, {0.5f, -0.9f},
};
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

// Render state of one core: two cached rows of per-cell floor shift (x and y, in 1/SUB px),
// indexed by grid row parity, plus interpolation scratch rows
typedef struct {
    int16_t sx_rows[2][GRID_N];
    int16_t sy_rows[2][GRID_N];
    int row_id[2];
    int32_t vx[GRID_N];
    int32_t vy[GRID_N];
} render_ctx_t;

static render_ctx_t s_ctx[2];              // [0] = worker on core 0, [1] = caller

// Worker task on core 0: runs one job at a time for the caller on core 1
typedef void (*job_fn_t)(void *arg);
static TaskHandle_t s_worker;
static TaskHandle_t s_caller;
static job_fn_t s_job_fn;
static void *s_job_arg;

// Color palettes: floor color at center and at edge, grid line color,
// tile shade variation (0 = all tiles equal)
typedef struct {
    float base[3];
    float edge[3];
    float line[3];
    float vary;
} palette_t;

static const palette_t s_palettes[] = {
    {{40, 175, 235}, {15, 120, 190}, {0, 0, 0}, 0.10f},          // azure pool, black grid, tiles in slightly different azure
    {{20, 20, 20}, {0, 0, 0}, {200, 200, 200}, 0.0f},            // black, light grey grid
    {{30, 160, 50}, {18, 100, 30}, {170, 20, 20}, 0.0f},         // green, red grid
    {{28, 20, 40}, {10, 7, 16}, {30, 135, 40}, 0.0f},            // dark lavender, bright green grid (drop1_16M crest green)
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
    // Each ring has its own floor color, blended from center to edge, then toward
    // the line color by the line coverage. The coverage blend is done in emitted light
    // (gamma removed, mixed, gamma applied) so the soft edge looks evenly graded.
    // Tile variants change brightness and the green/blue balance a little.
    const palette_t *pal = &s_palettes[s_palette];
    for (int ring = 0; ring < LUT_RINGS; ring++) {
        float e = (float)ring / (LUT_RINGS - 1);
        for (int v = 0; v < TILE_VARIANTS; v++) {
            float bright = 1.0f + pal->vary * s_variant_shift[v][0];
            float green = 1.0f + 0.6f * pal->vary * s_variant_shift[v][1];
            for (int i = 0; i < COVER_LEVELS; i++) {
                float a = (float)i / SUB;
                float c[3];
                for (int k = 0; k < 3; k++) {
                    float base = (pal->base[k] + (pal->edge[k] - pal->base[k]) * e) * bright;
                    if (k == 1) base *= green;
                    if (base > 255.0f) base = 255.0f;
                    float lb = powf(base / 255.0f, GAMMA);
                    float ll = powf(pal->line[k] / 255.0f, GAMMA);
                    c[k] = 255.0f * powf(lb + (ll - lb) * a, 1.0f / GAMMA);
                }
                s_lut[ring][v][i] = (pixel_t){.r = clamp_u8(c[0]), .g = clamp_u8(c[1]), .b = clamp_u8(c[2])};
            }
        }
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

// Line coverage by distance: full core, then a linear fade to 0
static void build_cover(void)
{
    for (int d = 0; d < COVER_TABLE; d++) {
        float t = ((float)d / SUB - LINE_CORE_PX / 2) / LINE_FADE_PX;
        s_cover[d] = (uint8_t)lrintf(SUB * fminf(fmaxf(1.0f - t, 0.0f), 1.0f));
    }
}

static void build_spans(void)
{
    const int u_off = TILE_W_PX * SUB * 64 - (LCD_H_RES / 2) * SUB + LINE_CENTER_SUB;
    for (int px = 0; px < LCD_H_RES; px++) {
        s_u_mod[px] = (int16_t)((px * SUB + u_off) % (TILE_W_PX * SUB));
        s_u_tile[px] = (uint8_t)(((px * SUB + u_off) / (TILE_W_PX * SUB)) & 7);
    }
    // Fixed pseudo-random shade per tile, neighbors never equal
    uint32_t seed = 12345;
    for (int ty = 0; ty < 8; ty++) {
        for (int tx = 0; tx < 8; tx++) {
            int v;
            do {
                seed = seed * 1103515245u + 12345u;
                v = (seed >> 16) % TILE_VARIANTS;
            } while ((tx > 0 && v == s_tile_variant[ty][tx - 1]) || (ty > 0 && v == s_tile_variant[ty - 1][tx]));
            s_tile_variant[ty][tx] = (uint8_t)v;
        }
    }
    build_cover();

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
static void apply_bump(float px, float py, float radius_px, float strength)
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
    (void)tint;   // no crest colors on the pool floor
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

// Wave equation on grid rows y_from..y_to (cur -> prev). Rows are processed in aligned
// 8-cell blocks; cells of a block outside the circle are reset to 0 (reflecting edge).
static IRAM_ATTR void wave_rows(int y_from, int y_to)
{
    for (int y = y_from; y <= y_to; y++) {
        int x0 = s_span_x0[y];
        int x1 = s_span_x1[y];
        if (x1 < x0) continue;
        int xs = x0 & ~7;
        int xe = x1 | 7;
        const cell_t *c = s_cur + y * GRID_S;
        cell_t *p = s_prev + y * GRID_S;
        pie_wave_row(p + xs, c + xs, (xe - xs + 1) / 8, &s_coef[0][0], GRID_S * sizeof(cell_t));
        for (int x = xs; x < x0; x++) p[x] = 0;
        for (int x = x1 + 1; x <= xe; x++) p[x] = 0;
    }
}

static void wave_rows_job(void *arg)
{
    wave_rows(1, GRID_N / 2 - 1);
}

void ripple_step(void)
{
    update_splashes();

    // Top half of the grid on core 0, bottom half here; rows only read s_cur
    job_start(wave_rows_job, NULL);
    wave_rows(GRID_N / 2, GRID_N - 2);
    job_wait();
    cell_t *t = s_cur;
    s_cur = s_prev;
    s_prev = t;
}

// Per-cell floor shift from the surface slope, cached for two grid rows at a time
static IRAM_ATTR int shift_row(render_ctx_t *ctx, int gy)
{
    int slot = gy & 1;
    if (ctx->row_id[slot] == gy) {
        return slot;
    }
    int16_t *ox = ctx->sx_rows[slot];
    int16_t *oy = ctx->sy_rows[slot];
    memset(ox, 0, sizeof(ctx->sx_rows[0]));
    memset(oy, 0, sizeof(ctx->sy_rows[0]));
    if (gy >= 1 && gy <= GRID_N - 2) {
        const cell_t *h = s_cur + gy * GRID_S;
        // Cells outside the circle are 0, so only the span plus one cell per side can be nonzero
        int xa = s_span_x0[gy] - 1;
        int xb = s_span_x1[gy] + 1;
        if (xa < 1) xa = 1;
        if (xb > GRID_N - 2) xb = GRID_N - 2;
        const float k = REFRACT_PX * SUB;
        const int lim = MAX_SHIFT_PX * SUB;
        for (int x = xa; x <= xb; x++) {
            int sx = (int)((float)(h[x - 1] - h[x + 1]) * k);
            int sy = (int)((float)(h[x - GRID_S] - h[x + GRID_S]) * k);
            ox[x] = (int16_t)(sx > lim ? lim : sx < -lim ? -lim : sx);
            oy[x] = (int16_t)(sy > lim ? lim : sy < -lim ? -lim : sy);
        }
    }
    ctx->row_id[slot] = gy;
    return slot;
}

// Line coverage 0..SUB at d = position modulo tile plus a shift smaller than one tile
// (1/SUB px): lines are centered on multiples of tile
// d = position modulo tile plus a shift smaller than one tile; *t is the tile index (mod 8),
// moved by one when the shift crosses a tile boundary
static inline int line_cover(int d, int tile, int *t)
{
    if (d < 0) {
        d += tile;
        *t -= 1;
    } else if (d >= tile) {
        d -= tile;
        *t += 1;
    }
    if (d > tile / 2) d = tile - d;
    return d < COVER_TABLE ? s_cover[d] : 0;
}

// Renders display rows y_from..y_to into band (whose first row is band_y0)
static IRAM_ATTR void render_rows(render_ctx_t *ctx, pixel_t *band, int band_y0, int y_from, int y_to)
{
    int32_t *vx = ctx->vx;
    int32_t *vy = ctx->vy;
    const int v_off = TILE_H_PX * SUB * 64 - (LCD_V_RES / 2) * SUB + LINE_CENTER_SUB;
    for (int py = y_from; py <= y_to; py++) {
        pixel_t *row = band + (py - band_y0) * LCD_H_RES;
        int x0 = s_px_x0[py];
        int x1 = s_px_x1[py];

        // Vertical interpolation between grid rows i0 and i0 + 1
        int g = py + GRID_OFF;
        int i0 = g / SCALE;
        int fy = g % SCALE;
        int s0 = shift_row(ctx, i0);
        int s1 = shift_row(ctx, i0 + 1);
        const int16_t *x0r = ctx->sx_rows[s0], *x1r = ctx->sx_rows[s1];
        const int16_t *y0r = ctx->sy_rows[s0], *y1r = ctx->sy_rows[s1];
        int j_first = (x0 + GRID_OFF) / SCALE;
        int j_last = (x1 + GRID_OFF) / SCALE + 1;
        for (int j = j_first; j <= j_last; j++) {
            vx[j] = x0r[j] * (SCALE - fy) + x1r[j] * fy;
            vy[j] = y0r[j] * (SCALE - fy) + y1r[j] * fy;
        }

        memset(row, 0, x0 * sizeof(pixel_t));
        int map_y = py < RING_MAP_ROWS ? py : LCD_V_RES - 1 - py;
        const uint8_t *ring_row = s_ring_map + map_y * LCD_H_RES;
        int v_row = (py * SUB + v_off) % (TILE_H_PX * SUB);
        int tv_row = ((py * SUB + v_off) / (TILE_H_PX * SUB)) & 7;
        // Horizontal interpolation, weights sum to SCALE * SCALE
        for (int px = x0; px <= x1; px++) {
            int gx = px + GRID_OFF;
            int j = gx / SCALE;
            int fx = gx % SCALE;
            int dx = (vx[j] * (SCALE - fx) + vx[j + 1] * fx) / INTERP;
            int dy = (vy[j] * (SCALE - fx) + vy[j + 1] * fx) / INTERP;
            int tu = s_u_tile[px];
            int tv = tv_row;
            int cu = line_cover(s_u_mod[px] + dx, TILE_W_PX * SUB, &tu);
            int cv = line_cover(v_row + dy, TILE_H_PX * SUB, &tv);
            int var = s_tile_variant[tv & 7][tu & 7];
            row[px] = s_lut[ring_row[px]][var][cu > cv ? cu : cv];
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
