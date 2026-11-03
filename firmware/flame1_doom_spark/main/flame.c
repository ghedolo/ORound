#include "flame.h"

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "panel.h"
#include "spark.h"

#define DOOM_LEVELS    37          // Doom palette entries, 0 = black, 36 = white
#define LEVELS         DOOM_LEVELS // fire intensities 0..36
// Source off: the center row loses 0..SOURCE_OFF_DIE-1 levels per step
// (0..3 with 37 levels in the original; max loss scaled to LEVELS)
#define SOURCE_OFF_DIE (3 * (LEVELS - 1) / (DOOM_LEVELS - 1) + 1)
#define MAX_STEPS      3           // steps per frame at most (after a slow frame)
#define DEFAULT_SCALE  3
#define HEIGHT_FRAC    0.70f       // average flame length as a fraction of the display radius
#define RADIUS         (LCD_H_RES / 2)   // display radius in px
#define LUT_SIZE       (2 * RADIUS)      // LUT side: offsets -RADIUS..RADIUS-1 from a fire center

static const char *TAG = "flame_spark";

static const int SCALES[] = {2, 3, 4, 5, 6, 8, 10};
// Outward speed in display px/s for each scale (same speed at every scale)
static const int SPEEDS[] = {81, 81, 81, 81, 81, 81, 81};
#define N_SCALES ((int)(sizeof(SCALES) / sizeof(SCALES[0])))

// Doom fire palette (PSX Doom), RGB
static const uint8_t PALETTE_RGB[DOOM_LEVELS][3] = {
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

// Black and white only (no gray): fire level i -> Doom palette color at position
// i * 36 / (LEVELS - 1) (linear between the 37 entries) -> luma (Rec. 601 weights,
// 0..255); white when luma >= BW_LUMA, black otherwise.
#define BW_LUMA        128
#define FIRE_WHITE     0xDD        // fire and fire sparks
#define BOOM_WHITE     0xFF        // explosions
static pixel_t s_palette[LEVELS];

static void palette_build(void)
{
    for (int i = 0; i < LEVELS; i++) {
        float pos = (float)i * (DOOM_LEVELS - 1) / (LEVELS - 1);
        int k = (int)pos;
        if (k > DOOM_LEVELS - 2) k = DOOM_LEVELS - 2;
        float t = pos - k;
        float rgb[3];
        for (int c = 0; c < 3; c++) {
            rgb[c] = PALETTE_RGB[k][c] + (PALETTE_RGB[k + 1][c] - PALETTE_RGB[k][c]) * t;
        }
        float luma = 0.299f * rgb[0] + 0.587f * rgb[1] + 0.114f * rgb[2];
        uint8_t g = luma + 0.5f >= BW_LUMA ? FIRE_WHITE : 0x00;
        s_palette[i] = (pixel_t){g, g, g};
    }
}

// Between two steps the grid is drawn shifted outward by m / MICRO of a cell,
// m = 0..MICRO-1 from the time since the last step: the content moves in
// sub-cell increments instead of jumping by a whole cell once per step.
#define MICRO_SHIFT    2
#define MICRO          (1 << MICRO_SHIFT)
#define LUT_OUTSIDE    0xFFFFu     // radius field of offsets outside the grid
// The shift grows from 0 at the center to full at RAMP_CELLS cells, so the source
// disk at the center keeps its size (a full shift there makes it grow and shrink every step).
#define RAMP_CELLS     4
#define RAMP           (RAMP_CELLS * MICRO)
static int s_micro;                // current sub-cell shift 0..MICRO-1
static int s_scale_idx;
static int s_scale;
static int64_t s_step_us;          // step period: one cell (s_scale px) per step at SPEEDS[]

// Polar grid: column = angle, row = radius. Row s_h - 1 is the center (source),
// row 0 the outer edge. "One row up" in the Doom fire = one cell outward.
// Two fires: the current one and the previous one, which dies down after a move.
typedef struct {
    uint8_t *f;                    // s_w * s_h intensities
    int cx, cy;                    // center in display pixels
    bool source_on;
    bool alive;                    // false once all cells are 0 with the source off
} fire_t;

static int s_w, s_h;               // grid size: s_w angles, s_h radii
static fire_t s_fires[2];
static int s_cur;                  // index of the current fire
// Per offset (dx, dy) from a fire center, dx, dy in -RADIUS..RADIUS-1:
// angle column << 16 | radius in 1/MICRO cells (LUT_OUTSIDE beyond the grid)
static uint32_t *s_lut;
static int64_t s_next_step_us;
static uint32_t s_rng;
static uint32_t s_decay_thr;       // decay when a 16-bit random value is below this
static int s_decay_step;           // levels lost per decay
static float s_m_base;             // mean loss per row without wind (set per scale)
static int s_lat = 1;              // lateral shift range: -s_lat..+s_lat columns

// Wind from the microphone level (0..1, smoothed). Three effects, by level:
//   agitate: wider lateral shift and more irregular decay (same mean)
//   lower:   larger mean decay, so shorter flames
//   blow out: source off after BLOW_OUT_US above BLOW_OUT_LEVEL; it relights
//             after RELIGHT_QUIET_US below WIND_QUIET
#define WIND_RELEASE_S     0.6f    // time constant of the fall after the sound stops
#define WIND_QUIET         0.08f
#define AGIT_START         0.10f
#define AGIT_FULL          0.40f
#define AGIT_LAT_MAX       3       // lateral range at full agitation
#define LOW_START          0.30f
#define LOW_FULL           0.80f
#define LOW_FACTOR         3.0f    // mean decay x (1 + LOW_FACTOR) at full: flames 1/4 long
#define BLOW_OUT_LEVEL     0.85f
#define BLOW_OUT_US        500000
#define RELIGHT_QUIET_US   3000000
static float s_wind;
static float s_agit, s_low;        // 0..1
static int64_t s_wind_us;          // time of the last flame_set_wind call
static int64_t s_strong_since;     // wind above BLOW_OUT_LEVEL since (0 = not)
static int64_t s_quiet_since;      // wind below WIND_QUIET since (0 = not)
static bool s_blown_out;

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

// Index of DEFAULT_SCALE in SCALES (the scale is not saved: every boot starts here)
static int scale_default_idx(void)
{
    int idx;
    for (idx = 0; idx < N_SCALES - 1 && SCALES[idx] != DEFAULT_SCALE; idx++) {
    }
    return idx;
}

// Offset from a fire center -> polar cell. Radial cell size = scale px, angular
// cell size = scale px at radius RADIUS.
static void lut_build(void)
{
    const float c = RADIUS - 0.5f;
    const float ang_k = s_w / (2.0f * (float)M_PI);
    for (int y = 0; y < LUT_SIZE; y++) {
        float dy = y - c;
        for (int x = 0; x < LUT_SIZE; x++) {
            float dx = x - c;
            float r = sqrtf(dx * dx + dy * dy);
            int rq = (int)(r * MICRO / s_scale);
            uint32_t e;
            if ((rq >> MICRO_SHIFT) >= s_h) {
                e = LUT_OUTSIDE;
            } else {
                int ai = (int)((atan2f(dy, dx) + (float)M_PI) * ang_k);
                if (ai >= s_w) ai = s_w - 1;
                if (ai < 0) ai = 0;
                e = (uint32_t)ai << 16 | (uint32_t)rq;
            }
            s_lut[y * LUT_SIZE + x] = e;
        }
    }
}

// Mean loss per row m = s_m_base, larger when the wind lowers the flame. The loss is
// k levels with probability m / k, k = smallest integer >= 2m (at least 1), so the
// probability stays <= 0.5 and the fire keeps its random texture even when m >= 1.
// Agitation doubles k at most (same mean, more irregular) and widens the lateral shift.
static void decay_apply(void)
{
    float m = s_m_base * (1.0f + LOW_FACTOR * s_low);
    int k = (int)ceilf(2.0f * m * (1.0f + s_agit));
    if (k < 1) k = 1;
    float p = m / k;
    if (p > 1.0f) p = 1.0f;
    s_decay_step = k;
    s_decay_thr = (uint32_t)(p * 65535.0f);
    s_lat = 1 + (int)(s_agit * (AGIT_LAT_MAX - 1) + 0.5f);
}

static inline void source_fill(fire_t *fr)
{
    memset(fr->f + (size_t)(s_h - 1) * s_w, LEVELS - 1, s_w);
}

// Starts fire i at (cx, cy): all black, source on
static void fire_start(int i, int cx, int cy)
{
    fire_t *fr = &s_fires[i];
    memset(fr->f, 0, (size_t)s_w * s_h);
    fr->cx = cx;
    fr->cy = cy;
    fr->source_on = true;
    fr->alive = true;
    source_fill(fr);
}

// (Re)allocates the grids for the current scale; the current fire restarts at its center
static void grid_build(void)
{
    s_scale = SCALES[s_scale_idx];
    s_w = (int)(2.0f * (float)M_PI * RADIUS / s_scale + 0.5f);
    s_h = RADIUS / s_scale + 1;
    size_t n = (size_t)s_w * s_h;
    for (int i = 0; i < 2; i++) {
        free(s_fires[i].f);
        s_fires[i].f = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_fires[i].f) s_fires[i].f = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        assert(s_fires[i].f);
        memset(s_fires[i].f, 0, n);
        s_fires[i].alive = false;
    }
    if (!s_lut) {
        s_lut = heap_caps_malloc(sizeof(uint32_t) * LUT_SIZE * LUT_SIZE, MALLOC_CAP_SPIRAM);
        assert(s_lut);
    }
    int64_t t0 = esp_timer_get_time();
    lut_build();
    ESP_LOGI(TAG, "lut %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    fire_t *cur = &s_fires[s_cur];
    bool on = !s_blown_out && cur->source_on;
    fire_start(s_cur, cur->cx, cur->cy);
    if (!on) cur->source_on = false;
    // Decay per row so the flame reaches HEIGHT_FRAC of the radius at every scale.
    s_m_base = (LEVELS - 1) / (HEIGHT_FRAC * s_h);
    decay_apply();
    s_step_us = 1000000LL * s_scale / SPEEDS[s_scale_idx];
    ESP_LOGI(TAG, "decay %d levels with p %.3f", s_decay_step, s_decay_thr / 65536.0f);
    ESP_LOGI(TAG, "==== SCALE %d px ==== grid %d angles x %d radii", s_scale, s_w, s_h);
}

void flame_init(void)
{
    palette_build();
    spark_init();
    s_rng = esp_random() | 1;
    s_scale_idx = scale_default_idx();
    s_cur = 0;
    s_fires[0].cx = RADIUS;
    s_fires[0].cy = RADIUS;
    s_fires[0].source_on = true;
    grid_build();
}

static void scale_set(int idx)
{
    s_scale_idx = (idx + N_SCALES) % N_SCALES;
    grid_build();
}

void flame_set_scale_idx(int idx)
{
    if (idx >= 0 && idx < N_SCALES) scale_set(idx);
}
void flame_next_scale(void) { scale_set(s_scale_idx + 1); }
void flame_prev_scale(void) { scale_set(s_scale_idx - 1); }
int flame_scale(void) { return s_scale; }

void flame_toggle_source(void)
{
    fire_t *cur = &s_fires[s_cur];
    cur->source_on = !cur->source_on;
    s_blown_out = false;
    if (cur->source_on) {
        cur->alive = true;
        source_fill(cur);
    }
    ESP_LOGI(TAG, "source %s", cur->source_on ? "on" : "off");
}

void flame_move_to(int x, int y)
{
    // The current fire dies down where it is; a new one starts at (x, y).
    // A previous fire still dying is dropped.
    s_fires[s_cur].source_on = false;
    s_cur ^= 1;
    fire_start(s_cur, x, y);
    s_blown_out = false;
    ESP_LOGI(TAG, "fire moved to %d,%d", x, y);
}

// One step of the Doom fire on one grid. For every cell (x, y), y >= 1, x outer
// loop, y from top to bottom as in the original: the cell one row up (outward),
// shifted laterally, gets the value minus the decay. Black cells blacken the cell
// straight above. Columns wrap around (angle).
// Returns false when the whole grid is black.
// Arc instability (source on): every step each source cell gets 36 minus a random
// loss 0..SRC_FLICKER, minus a common loss 0..SRC_PULSE for the whole source, and
// with probability 1/SRC_DROP_P a deep drop of 0..SRC_DROP. The center stays still;
// the white core changes shape and size every step.
#define SRC_FLICKER    6
#define SRC_PULSE      8
#define SRC_DROP       24
#define SRC_DROP_P     6
static void source_flicker(fire_t *fr)
{
    uint8_t *row = fr->f + (size_t)(s_h - 1) * s_w;
    int pulse = (int)(rng_next() % (SRC_PULSE + 1));
    for (int x = 0; x < s_w; x++) {
        uint32_t rnd = rng_next();
        int loss = pulse + (int)(rnd % (SRC_FLICKER + 1));
        if (((rnd >> 8) % SRC_DROP_P) == 0) loss += (int)((rnd >> 16) % (SRC_DROP + 1));
        int v = (LEVELS - 1) - loss;
        row[x] = (uint8_t)(v > 0 ? v : 0);
    }
}

static bool fire_step(fire_t *fr)
{
    if (fr->source_on) source_flicker(fr);
    const int w = s_w;
    uint8_t *f = fr->f;
    uint32_t any = 0;
    // Columns are processed in order, so a cell shifted to a column not yet processed
    // can move again in the same step. Where the order wraps (last column -> first)
    // this gives a visible seam on the angle axis. Starting from a random column each
    // step moves the seam around instead of keeping it on one angle.
    int x = (int)(rng_next() % (uint32_t)w);
    for (int i = 0; i < w; i++, x = (x + 1 == w) ? 0 : x + 1) {
        for (int y = 1; y < s_h; y++) {
            uint8_t v = f[y * w + x];
            if (v == 0) {
                f[(y - 1) * w + x] = 0;
                continue;
            }
            uint32_t rnd = rng_next();
            // Lateral shift -1, 0, 0, +1 (symmetric: no swirl; the original Doom fire uses -2..+1);
            // with wind uniform in -s_lat..+s_lat
            static const int8_t SHIFT[4] = {-1, 0, 0, 1};
            int dx = (s_lat == 1) ? x + SHIFT[rnd & 3]
                                  : x + (int)((((rnd >> 2) & 0xFF) * (uint32_t)(2 * s_lat + 1)) >> 8) - s_lat;
            if (dx < 0) dx += w;
            else if (dx >= w) dx -= w;
            int nv = v;
            if ((rnd >> 16) < s_decay_thr) {
                nv -= s_decay_step;
                if (nv < 0) nv = 0;
            }
            f[(y - 1) * w + dx] = (uint8_t)nv;
            any |= (uint32_t)nv;
        }
    }
    // Source off: the center row dies down by 0..SOURCE_OFF_DIE-1 per step
    uint8_t *row = f + (size_t)(s_h - 1) * w;
    if (!fr->source_on) {
        for (int x = 0; x < w; x++) {
            int v = row[x] - (int)(rng_next() % SOURCE_OFF_DIE);
            row[x] = v > 0 ? v : 0;
            any |= (uint32_t)row[x];
        }
    } else {
        any = 1;
    }
    return any != 0;
}

static float ramp(float v, float a, float b)
{
    float x = (v - a) / (b - a);
    return x < 0 ? 0 : (x > 1 ? 1 : x);
}

void flame_set_wind(float level, int64_t now_us)
{
    float dt = s_wind_us ? (now_us - s_wind_us) * 1e-6f : 0;
    s_wind_us = now_us;
    if (level > s_wind) s_wind = level;
    else s_wind += (level - s_wind) * fminf(1.0f, dt / WIND_RELEASE_S);

    float agit = ramp(s_wind, AGIT_START, AGIT_FULL);
    float low = ramp(s_wind, LOW_START, LOW_FULL);
    if (agit != s_agit || low != s_low) {
        s_agit = agit;
        s_low = low;
        decay_apply();
    }

    fire_t *cur = &s_fires[s_cur];
    if (s_wind >= BLOW_OUT_LEVEL) {
        if (!s_strong_since) s_strong_since = now_us;
        if (!s_blown_out && cur->source_on && now_us - s_strong_since >= BLOW_OUT_US) {
            s_blown_out = true;
            cur->source_on = false;
            ESP_LOGI(TAG, "blown out");
        }
    } else {
        s_strong_since = 0;
    }
    if (s_wind < WIND_QUIET) {
        if (!s_quiet_since) s_quiet_since = now_us;
        if (s_blown_out && now_us - s_quiet_since >= RELIGHT_QUIET_US) {
            s_blown_out = false;
            if (!cur->source_on) flame_toggle_source();
            ESP_LOGI(TAG, "relit");
        }
    } else {
        s_quiet_since = 0;
    }
}

float flame_wind(void) { return s_wind; }

// The fire turns around its center: the angle column read at render time is
// shifted by s_rot_turn (fraction of a turn). Angular speed ROT_RPS * sin(2 pi t / ROT_PERIOD_S)
// turns per second: starts still, speeds up to ROT_RPS, slows down to a stop, turns the
// other way, and so on.
#define ROT_RPS        0.0326f
#define ROT_PERIOD_S   15.0f
static float s_rot_turn;
static int64_t s_rot_us;
static float s_rot_t;              // s since the start of the rotation cycle

void flame_update(int64_t now_us)
{
    if (s_rot_us) {
        float dt = (now_us - s_rot_us) * 1e-6f;
        s_rot_t += dt;
        if (s_rot_t >= ROT_PERIOD_S) s_rot_t -= ROT_PERIOD_S;
        s_rot_turn += ROT_RPS * sinf(2.0f * (float)M_PI * s_rot_t / ROT_PERIOD_S) * dt;
        s_rot_turn -= floorf(s_rot_turn);
    }
    s_rot_us = now_us;
    if (s_next_step_us == 0) s_next_step_us = now_us;
    int steps = 0;
    while (now_us >= s_next_step_us && steps < MAX_STEPS) {
        for (int i = 0; i < 2; i++) {
            if (s_fires[i].alive) s_fires[i].alive = fire_step(&s_fires[i]);
        }
        s_next_step_us += s_step_us;
        steps++;
    }
    if (now_us >= s_next_step_us) s_next_step_us = now_us + s_step_us;
    // Time since the last step (last step was at s_next_step_us - s_step_us)
    int64_t since = now_us - (s_next_step_us - s_step_us);
    int m = (int)(since * MICRO / s_step_us);
    s_micro = m < 0 ? 0 : (m > MICRO - 1 ? MICRO - 1 : m);
    const fire_t *cur = &s_fires[s_cur];
    spark_update(now_us, cur->cx, cur->cy, cur->source_on, s_wind);
}

// Adds fire fr to the intensity row val (max), display row y
static void render_fire_row(const fire_t *fr, int y, uint8_t *val)
{
    const int cx = fr->cx, cy = fr->cy;
    int oy = y - cy + RADIUS;
    if (oy < 0 || oy >= LUT_SIZE) return;
    int x0 = cx - RADIUS, x1 = cx + RADIUS;     // display x range covered by the LUT
    if (x0 < 0) x0 = 0;
    if (x1 > LCD_H_RES) x1 = LCD_H_RES;
    const uint32_t *lut = s_lut + (size_t)oy * LUT_SIZE + (RADIUS - cx);
    const uint8_t *f = fr->f;
    const int w = s_w;
    const int last = s_h - 1;
    const int m = s_micro;
    int rot = (int)(s_rot_turn * w);
    if (rot >= w) rot = 0;
    for (int x = x0; x < x1; x++) {
        uint32_t e = lut[x];
        int rq = (int)(e & 0xFFFF);
        if (rq == LUT_OUTSIDE) continue;
        // Sample up to m / MICRO of a cell further in: the content appears moved outward
        rq -= (rq >= RAMP) ? m : (m * rq) / RAMP;
        if (rq < 0) rq = 0;
        int ri = rq >> MICRO_SHIFT;
        if (ri > last) continue;
        int col = (int)(e >> 16) + rot;
        if (col >= w) col -= w;
        uint8_t v = f[(last - ri) * w + col];
        if (v > val[x]) val[x] = v;
    }
}

// Sets color c on row p where the mask row bits are set
static void mask_draw(pixel_t *p, const uint8_t *row, pixel_t c)
{
    for (int b = 0; b < SPARK_STRIDE; b++) {
        uint8_t bits = row[b];
        while (bits) {
            int x = (b << 3) + __builtin_ctz(bits);
            bits &= bits - 1;
            if (x < LCD_H_RES) p[x] = c;
        }
    }
}

void flame_render(void)
{
    static uint8_t val[LCD_H_RES];
    spark_draw();
    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();
        for (int y = y0; y <= y1; y++) {
            memset(val, 0, sizeof(val));
            for (int i = 0; i < 2; i++) {
                if (s_fires[i].alive) render_fire_row(&s_fires[i], y, val);
            }
            pixel_t *p = band + (y - y0) * LCD_H_RES;
            for (int x = 0; x < LCD_H_RES; x++) p[x] = s_palette[val[x]];
            // Sparks over the fire: fire sparks FIRE_WHITE, then explosions BOOM_WHITE
            mask_draw(p, spark_row(y), (pixel_t){FIRE_WHITE, FIRE_WHITE, FIRE_WHITE});
            mask_draw(p, spark_boom_row(y), (pixel_t){BOOM_WHITE, BOOM_WHITE, BOOM_WHITE});
        }
        panel_band_send(band, y0, y1);
    }
}
