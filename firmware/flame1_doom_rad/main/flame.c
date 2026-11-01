#include "flame.h"

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

#define LEVELS         37          // palette entries, 0 = black, 36 = white
#define MAX_STEPS      3           // steps per frame at most (after a slow frame)
#define DEFAULT_SCALE  3
#define HEIGHT_FRAC    0.70f       // average flame length as a fraction of the display radius
#define RADIUS         (LCD_H_RES / 2)   // display radius in px

static const char *TAG = "flame_rad";

static const int SCALES[] = {2, 3, 4, 5, 6, 8, 10};
// Outward speed in display px/s for each scale (same speed at every scale)
static const int SPEEDS[] = {81, 81, 81, 81, 81, 81, 81};
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
// Between two steps the grid is drawn shifted outward by m / MICRO of a cell,
// m = 0..MICRO-1 from the time since the last step: the content moves in
// sub-cell increments instead of jumping by a whole cell once per step.
#define MICRO_SHIFT    2
#define MICRO          (1 << MICRO_SHIFT)
#define LUT_OUTSIDE    0xFFFFu     // radius field of pixels outside the circle
// The shift grows from 0 at the center to full at RAMP_CELLS cells, so the source
// disk at the center keeps its size (a full shift there makes it grow and shrink every step).
#define RAMP_CELLS     4
#define RAMP           (RAMP_CELLS * MICRO)
static int s_micro;                // current sub-cell shift 0..MICRO-1
static int s_scale_idx;
static int s_scale;
static int64_t s_step_us;          // step period: one cell (s_scale px) per step at SPEEDS[]
// Polar grid: column = angle, row = radius. Row s_h - 1 is the center (source),
// row 0 the display edge. "One row up" in the Doom fire = one cell outward.
static int s_w, s_h;               // grid size: s_w angles, s_h radii
static uint8_t *s_fire;            // s_w * s_h intensities + 1 trailing black cell
static uint32_t *s_lut;            // per display pixel: angle column << 16 | radius in 1/MICRO cells
static bool s_source_on = true;
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

// Display pixel -> polar cell. Radial cell size = scale px, angular cell size =
// scale px at the display edge. Pixels outside the circle get LUT_OUTSIDE.
static void lut_build(void)
{
    const float c = RADIUS - 0.5f;
    const float ang_k = s_w / (2.0f * (float)M_PI);
    for (int y = 0; y < LCD_V_RES; y++) {
        float dy = y - c;
        for (int x = 0; x < LCD_H_RES; x++) {
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
            s_lut[y * LCD_H_RES + x] = e;
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

// (Re)allocates the grid for the current scale: all black, source row set if on
static void grid_build(void)
{
    free(s_fire);
    s_scale = SCALES[s_scale_idx];
    s_w = (int)(2.0f * (float)M_PI * RADIUS / s_scale + 0.5f);
    s_h = RADIUS / s_scale + 1;
    size_t n = (size_t)s_w * s_h;
    s_fire = heap_caps_malloc(n + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_fire) s_fire = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(s_fire);
    memset(s_fire, 0, n + 1);
    if (!s_lut) {
        s_lut = heap_caps_malloc(sizeof(uint32_t) * LCD_H_RES * LCD_V_RES, MALLOC_CAP_SPIRAM);
        assert(s_lut);
    }
    int64_t t0 = esp_timer_get_time();
    lut_build();
    ESP_LOGI(TAG, "lut %d ms", (int)((esp_timer_get_time() - t0) / 1000));
    if (s_source_on) memset(s_fire + (size_t)(s_h - 1) * s_w, LEVELS - 1, s_w);
    // Decay per row so the flame reaches HEIGHT_FRAC of the radius at every scale.
    s_m_base = (LEVELS - 1) / (HEIGHT_FRAC * s_h);
    decay_apply();
    s_step_us = 1000000LL * s_scale / SPEEDS[s_scale_idx];
    ESP_LOGI(TAG, "decay %d levels with p %.3f", s_decay_step, s_decay_thr / 65536.0f);
    ESP_LOGI(TAG, "==== SCALE %d px ==== grid %d angles x %d radii", s_scale, s_w, s_h);
}

void flame_init(void)
{
    for (int i = 0; i < LEVELS; i++) {
        s_palette[i] = (pixel_t){PALETTE_RGB[i][0], PALETTE_RGB[i][1], PALETTE_RGB[i][2]};
    }
    s_rng = esp_random() | 1;
    s_scale_idx = scale_default_idx();
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
    s_source_on = !s_source_on;
    s_blown_out = false;
    if (s_source_on) memset(s_fire + (size_t)(s_h - 1) * s_w, LEVELS - 1, s_w);
    ESP_LOGI(TAG, "source %s", s_source_on ? "on" : "off");
}

// One step of the Doom fire. For every cell (x, y), y >= 1, x outer loop, y from top
// to bottom as in the original: r = random 0..3, the cell above shifted by 1 - r
// columns gets the value minus 1 with probability p (set per scale in grid_build). Black cells blacken the cell straight above.
// Columns wrap around.
static void fire_step(void)
{
    const int w = s_w;
    uint8_t *f = s_fire;
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

    if (s_wind >= BLOW_OUT_LEVEL) {
        if (!s_strong_since) s_strong_since = now_us;
        if (!s_blown_out && s_source_on && now_us - s_strong_since >= BLOW_OUT_US) {
            s_blown_out = true;
            s_source_on = false;
            ESP_LOGI(TAG, "blown out");
        }
    } else {
        s_strong_since = 0;
    }
    if (s_wind < WIND_QUIET) {
        if (!s_quiet_since) s_quiet_since = now_us;
        if (s_blown_out && now_us - s_quiet_since >= RELIGHT_QUIET_US) {
            s_blown_out = false;
            if (!s_source_on) flame_toggle_source();
            ESP_LOGI(TAG, "relit");
        }
    } else {
        s_quiet_since = 0;
    }
}

float flame_wind(void) { return s_wind; }

void flame_update(int64_t now_us)
{
    if (s_next_step_us == 0) s_next_step_us = now_us;
    int steps = 0;
    while (now_us >= s_next_step_us && steps < MAX_STEPS) {
        fire_step();
        s_next_step_us += s_step_us;
        steps++;
    }
    if (now_us >= s_next_step_us) s_next_step_us = now_us + s_step_us;
    // Time since the last step as 0..256 (last step was at s_next_step_us - s_step_us)
    int64_t since = now_us - (s_next_step_us - s_step_us);
    int m = (int)(since * MICRO / s_step_us);
    s_micro = m < 0 ? 0 : (m > MICRO - 1 ? MICRO - 1 : m);
}

void flame_render(void)
{
    const int w = s_w;
    const int last = s_h - 1;
    const int m = s_micro;
    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();
        const uint32_t *lut = s_lut + (size_t)y0 * LCD_H_RES;
        const int npx = (y1 - y0 + 1) * LCD_H_RES;
        const uint8_t *f = s_fire;
        for (int i = 0; i < npx; i++) {
            uint32_t e = lut[i];
            int rq = (int)(e & 0xFFFF);
            if (rq == LUT_OUTSIDE) {
                band[i] = s_palette[0];
                continue;
            }
            // Sample up to m / MICRO of a cell further in: the content appears moved outward
            rq -= (rq >= RAMP) ? m : (m * rq) / RAMP;
            if (rq < 0) rq = 0;
            int ri = rq >> MICRO_SHIFT;
            band[i] = ri > last ? s_palette[0] : s_palette[f[(last - ri) * w + (int)(e >> 16)]];
        }
        panel_band_send(band, y0, y1);
    }
}
