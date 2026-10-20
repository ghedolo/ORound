#include "life.h"

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

#define GEN_US         4000000              // time between generations, up to SLOW_FROM_PX
#define FADE_TAU_S     1.1f                 // time constant of each of the two color filter stages, up to SLOW_FROM_PX
#define SLOW_FROM_PX   20                   // larger tiles: generation time and fade grow with the size
#define SLOW_PER_PX    0.03f                // extra time per px above SLOW_FROM_PX (x2.3 at 64 px, x2.8 at 80 px)
#define SLOTS          10                   // changing cells start fading in one of these time slots
#define SLOT_US        (s_gen_len_us / SLOTS)
#define SEED_DENSITY   0.30f                // live cells after a random seed
#define AGE_FULL       12                   // generations from newborn color to the last AGE_COLORS entry
#define STALE_GENS     20                   // repeating states this long: new seed
#define EMPTY_GENS     3                    // no live cells this long: new seed
#define MAX_NB         16                   // vertex neighbors per tile (arrow tiling: 8)
#define NO_TILE        0xFFFF
#define RADIUS         (LCD_H_RES / 2.0f)
#define ARROW_N        8                    // arrow polygon vertices

// Arrow length in px (tip to tail notch = grid spacing), BOOT cycles through them
static const int EDGE_PX[] = {12, 16, 20, 24, 28, 32, 40, 48, 64, 80};
#define EDGE_COUNT     (int)(sizeof(EDGE_PX) / sizeof(EDGE_PX[0]))
#define EDGE_DEFAULT   4
#define BORDER_FROM_PX 24                   // from this size on, tiles get a black border
#define BORDER_DIV     40.0f                // border inset per tile = size / BORDER_DIV (line = twice that)
#define BORDER_MIN     0.55f                // smallest inset per tile in px

static const char *TAG = "life";

static const float COLOR_DEAD_HORZ[3] = {12, 12, 22};
static const float COLOR_DEAD_VERT[3] = {22, 22, 38};
// Live cell color by age, equally spaced from age 0 to AGE_FULL: orange, yellow, bright green
static const float AGE_COLORS[][3] = {
    {255, 140, 30},
    {255, 230, 40},
    {60, 255, 70},
};
#define AGE_STOPS      (int)(sizeof(AGE_COLORS) / sizeof(AGE_COLORS[0]))

// Arrow tile from data/tassel/Alhambra.jpg, in grid units (y down): tip at (0, 0), tail notch at (0, 1).
// Every grid edge carries one arrow; vertical arrows have the tip on grid points with i + j even,
// horizontal arrows on points with i + j odd. Barb point (ARROW_QX, ARROW_QY) measured on the image;
// the values are multiples of 1/1024 so all vertices are exact in float and shared vertices match bit for bit.
#define ARROW_QX       (629.0f / 1024)
#define ARROW_QY       (772.0f / 1024)
static const float ARROW[ARROW_N][2] = {
    {0, 0},
    {-(1 - ARROW_QY), ARROW_QX},
    {-0.5f, 0.5f},
    {-ARROW_QX, ARROW_QY},
    {0, 1},
    {ARROW_QX, ARROW_QY},
    {0.5f, 0.5f},
    {1 - ARROW_QY, ARROW_QX},
};

typedef struct {
    float x[ARROW_N], y[ARROW_N];
    uint8_t vert;
} shape_t;

static int s_edge_idx = EDGE_DEFAULT;
static int s_count;                     // number of tiles
static uint16_t *s_map;                 // tile index per display pixel, NO_TILE outside
static uint8_t *s_vert;
static uint8_t *s_alive, *s_next, *s_prev, *s_slot, *s_age, *s_nn;
static uint16_t (*s_nb)[MAX_NB];
static float (*s_mid)[3];               // first filter stage
static float (*s_color)[3];             // displayed color (second stage): slow start, slow end
static pixel_t *s_rgb;                  // displayed color for the renderer

static int64_t s_gen_len_us = GEN_US;   // time between generations for the current size
static float s_tau_s = FADE_TAU_S;      // fade time constant for the current size
static int64_t s_gen_us;
static int64_t s_last_us;
static uint32_t s_hash[3];              // last generations, to detect still lifes and short cycles
static int s_stale;
static int s_empty;

static float frand(void)
{
    return (float)esp_random() / (float)UINT32_MAX;
}

// ---- Tiling ----

// Arrow with the tip on grid point (ti, tj), rotated by rot quarter turns (0: body toward +y), scaled to px
static shape_t arrow(int ti, int tj, int rot, int size)
{
    shape_t s;
    for (int k = 0; k < ARROW_N; k++) {
        float x = ARROW[k][0], y = ARROW[k][1];
        for (int r = 0; r < rot; r++) {
            float t = x;
            x = -y;
            y = t;
        }
        s.x[k] = (ti + x) * size;
        s.y[k] = (tj + y) * size;
    }
    s.vert = rot % 2 == 0;
    return s;
}

// Keeps the tile when its vertex mean is inside the display
static int shape_add(shape_t *shapes, int n, shape_t s)
{
    float gx = 0, gy = 0;
    for (int k = 0; k < ARROW_N; k++) {
        gx += s.x[k];
        gy += s.y[k];
    }
    if (hypotf(gx / ARROW_N, gy / ARROW_N) > RADIUS || n >= NO_TILE) return n;
    shapes[n] = s;
    return n + 1;
}

// One vertical and one horizontal arrow per grid point
static int make_shapes(int size, shape_t *shapes)
{
    int g = (int)ceilf(RADIUS / size) + 1;
    int n = 0;
    for (int j = -g; j <= g; j++) {
        for (int i = -g; i <= g; i++) {
            bool even = ((i + j) & 1) == 0;
            // vertical edge (i, j)-(i, j + 1)
            n = shape_add(shapes, n, even ? arrow(i, j, 0, size) : arrow(i, j + 1, 2, size));
            // horizontal edge (i, j)-(i + 1, j)
            n = shape_add(shapes, n, even ? arrow(i + 1, j, 1, size) : arrow(i, j, 3, size));
        }
    }
    return n;
}

typedef struct {
    int32_t x, y;
    int32_t tile;
} vert_key_t;

static int cmp_vert(const void *a, const void *b)
{
    const vert_key_t *p = a, *q = b;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    if (p->y != q->y) return p->y < q->y ? -1 : 1;
    return 0;
}

// Tiles sharing a vertex are neighbors
static void make_neighbors(const shape_t *shapes, int count)
{
    int nv = count * ARROW_N;
    vert_key_t *keys = heap_caps_malloc(sizeof(vert_key_t) * nv, MALLOC_CAP_SPIRAM);
    int k = 0;
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < ARROW_N; j++) {
            keys[k++] = (vert_key_t){lroundf(shapes[i].x[j] * 4), lroundf(shapes[i].y[j] * 4), i};
        }
    }
    qsort(keys, nv, sizeof(vert_key_t), cmp_vert);

    for (int a = 0; a < nv;) {
        int b = a + 1;
        while (b < nv && cmp_vert(&keys[a], &keys[b]) == 0) b++;
        for (int i = a; i < b; i++) {
            for (int j = a; j < b; j++) {
                int t = keys[i].tile, u = keys[j].tile;
                if (t == u) continue;
                bool known = false;
                for (int m = 0; m < s_nn[t]; m++) {
                    if (s_nb[t][m] == u) known = true;
                }
                if (!known && s_nn[t] < MAX_NB) s_nb[t][s_nn[t]++] = u;
            }
        }
        a = b;
    }
    heap_caps_free(keys);
}

// Squared distance from (x, y) to segment (ax, ay)-(bx, by)
static float seg_dist2(float x, float y, float ax, float ay, float bx, float by)
{
    float ex = bx - ax, ey = by - ay;
    float u = ((x - ax) * ex + (y - ay) * ey) / (ex * ex + ey * ey);
    u = fminf(1, fmaxf(0, u));
    float dx = x - ax - u * ex, dy = y - ay - u * ey;
    return dx * dx + dy * dy;
}

// Pixel centers inside a tile (crossing test, the polygon is not convex) get its index.
// inset > 0: pixels closer than inset to a tile edge stay black (border).
static void rasterize(const shape_t *shapes, int count, float inset)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) s_map[i] = NO_TILE;
    float inset2 = inset * inset;
    for (int t = 0; t < count; t++) {
        const shape_t *s = &shapes[t];
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < ARROW_N; k++) {
            x0 = fminf(x0, s->x[k]); x1 = fmaxf(x1, s->x[k]);
            y0 = fminf(y0, s->y[k]); y1 = fmaxf(y1, s->y[k]);
        }
        int px0 = (int)floorf(x0 + RADIUS), px1 = (int)ceilf(x1 + RADIUS);
        int py0 = (int)floorf(y0 + RADIUS), py1 = (int)ceilf(y1 + RADIUS);
        if (px0 < 0) px0 = 0;
        if (py0 < 0) py0 = 0;
        if (px1 > LCD_H_RES - 1) px1 = LCD_H_RES - 1;
        if (py1 > LCD_V_RES - 1) py1 = LCD_V_RES - 1;
        for (int py = py0; py <= py1; py++) {
            float y = py + 0.5f - RADIUS;
            for (int px = px0; px <= px1; px++) {
                float x = px + 0.5f - RADIUS;
                bool in = false;
                for (int k = 0, k1 = ARROW_N - 1; k < ARROW_N; k1 = k++) {
                    if ((s->y[k] > y) != (s->y[k1] > y) &&
                        x < (s->x[k1] - s->x[k]) * (y - s->y[k]) / (s->y[k1] - s->y[k]) + s->x[k]) in = !in;
                }
                for (int k = 0; k < ARROW_N && in && inset > 0; k++) {
                    int k1 = (k + 1) % ARROW_N;
                    if (seg_dist2(x, y, s->x[k], s->y[k], s->x[k1], s->y[k1]) < inset2) in = false;
                }
                if (in) s_map[py * LCD_H_RES + px] = t;
            }
        }
    }
}

static void cells_free(void)
{
    void *p[] = {s_vert, s_alive, s_next, s_prev, s_slot, s_age, s_nn, s_nb, s_mid, s_color, s_rgb};
    for (int i = 0; i < (int)(sizeof(p) / sizeof(p[0])); i++) heap_caps_free(p[i]);
}

static void build(int edge)
{
    int64_t t0 = esp_timer_get_time();
    cells_free();

    int g = (int)ceilf(RADIUS / edge) + 1;
    shape_t *shapes = heap_caps_malloc(sizeof(shape_t) * 2 * (2 * g + 1) * (2 * g + 1), MALLOC_CAP_SPIRAM);
    s_count = make_shapes(edge, shapes);

    int n = s_count;
    s_vert = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_alive = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_next = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_prev = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_slot = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_age = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_nn = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
    s_nb = heap_caps_calloc(n, sizeof(*s_nb), MALLOC_CAP_SPIRAM);
    s_mid = heap_caps_calloc(n, sizeof(*s_mid), MALLOC_CAP_SPIRAM);
    s_color = heap_caps_calloc(n, sizeof(*s_color), MALLOC_CAP_SPIRAM);
    s_rgb = heap_caps_calloc(n, sizeof(pixel_t), MALLOC_CAP_INTERNAL);
    if (!s_rgb) s_rgb = heap_caps_calloc(n, sizeof(pixel_t), MALLOC_CAP_SPIRAM);
    if (!s_map) s_map = heap_caps_malloc(sizeof(uint16_t) * LCD_H_RES * LCD_V_RES, MALLOC_CAP_SPIRAM);

    for (int i = 0; i < n; i++) s_vert[i] = shapes[i].vert;
    make_neighbors(shapes, n);
    float slow = 1.0f + SLOW_PER_PX * fmaxf(0, edge - SLOW_FROM_PX);
    s_gen_len_us = (int64_t)(GEN_US * slow);
    s_tau_s = FADE_TAU_S * slow;

    float inset = 0;
    if (edge >= BORDER_FROM_PX) inset = fmaxf(BORDER_MIN, edge / BORDER_DIV);
    rasterize(shapes, n, inset);
    heap_caps_free(shapes);

    int nb_min = MAX_NB, nb_max = 0;
    for (int i = 0; i < n; i++) {
        if (s_nn[i] < nb_min) nb_min = s_nn[i];
        if (s_nn[i] > nb_max) nb_max = s_nn[i];
    }
    ESP_LOGI(TAG, "arrow %d px: %d tiles, neighbors %d..%d, border %.1f px, generation %.1f s, fade tau %.1f s, %d ms",
             edge, n, nb_min, nb_max, 2 * inset, s_gen_len_us * 1e-6f, s_tau_s,
             (int)((esp_timer_get_time() - t0) / 1000));
}

// ---- Life ----

void life_init(void)
{
    build(EDGE_PX[s_edge_idx]);
    life_randomize();
}

int life_edge_px(void)
{
    return EDGE_PX[s_edge_idx];
}

void life_next_size(void)
{
    s_edge_idx = (s_edge_idx + 1) % EDGE_COUNT;
    build(EDGE_PX[s_edge_idx]);
    ESP_LOGI(TAG, "==== ARROW %d px ====", EDGE_PX[s_edge_idx]);
    life_randomize();
}

void life_randomize(void)
{
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        s_prev[i] = s_alive[i];
        s_slot[i] = esp_random() % SLOTS;
        s_alive[i] = frand() < SEED_DENSITY;
        if (s_alive[i] && !s_prev[i]) s_age[i] = 0;
        n += s_alive[i];
    }
    s_gen_us = s_last_us;
    s_stale = 0;
    s_empty = 0;
    ESP_LOGI(TAG, "new seed, %d cells", n);
}

static void set_alive(int t)
{
    if (!s_alive[t]) s_age[t] = 0;
    s_alive[t] = 1;
    s_prev[t] = 1;      // shown at once
}

void life_touch(int x, int y)
{
    if (x < 0 || x >= LCD_H_RES || y < 0 || y >= LCD_V_RES) return;
    int t = s_map[y * LCD_H_RES + x];
    if (t == NO_TILE) return;
    set_alive(t);
    for (int m = 0; m < s_nn[t]; m++) {
        if (frand() < 0.5f) set_alive(s_nb[t][m]);
    }
    s_stale = 0;
}

static void step(void)
{
    uint32_t hash = 2166136261u;
    int n = 0;
    for (int i = 0; i < s_count; i++) {
        int nb = 0;
        for (int m = 0; m < s_nn[i]; m++) nb += s_alive[s_nb[i][m]];
        bool live = nb == 3 || (nb == 2 && s_alive[i]);
        s_next[i] = live;
        hash = (hash ^ live) * 16777619u;
        n += live;
    }
    for (int i = 0; i < s_count; i++) {
        if (s_next[i]) {
            if (!s_alive[i]) s_age[i] = 0;
            else if (s_age[i] < AGE_FULL) s_age[i]++;
        }
        s_slot[i] = esp_random() % SLOTS;
    }
    memcpy(s_prev, s_alive, s_count);
    memcpy(s_alive, s_next, s_count);

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
    if (now_us - s_gen_us >= s_gen_len_us) {
        s_gen_us = now_us;
        step();
    }

    float dt = (now_us - s_last_us) * 1e-6f;
    s_last_us = now_us;
    float k = 1.0f - expf(-dt / s_tau_s);
    for (int t = 0; t < s_count; t++) {
        float target[3];
        bool started = now_us - s_gen_us >= (int64_t)s_slot[t] * SLOT_US;
        if (started ? s_alive[t] : s_prev[t]) {
            float a = (float)s_age[t] / AGE_FULL * (AGE_STOPS - 1);
            int j = (int)a;
            if (j > AGE_STOPS - 2) j = AGE_STOPS - 2;
            float f = a - j;
            for (int i = 0; i < 3; i++) target[i] = AGE_COLORS[j][i] + (AGE_COLORS[j + 1][i] - AGE_COLORS[j][i]) * f;
        } else {
            const float *d = s_vert[t] ? COLOR_DEAD_VERT : COLOR_DEAD_HORZ;
            for (int i = 0; i < 3; i++) target[i] = d[i];
        }
        float *m = s_mid[t];
        float *c = s_color[t];
        for (int i = 0; i < 3; i++) {
            m[i] += (target[i] - m[i]) * k;
            c[i] += (m[i] - c[i]) * k;
        }
        s_rgb[t] = (pixel_t){(uint8_t)(c[0] + 0.5f), (uint8_t)(c[1] + 0.5f), (uint8_t)(c[2] + 0.5f)};
    }
}

void life_render(void)
{
    static const pixel_t black = {0, 0, 0};
    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 > LCD_V_RES - 1) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();
        const uint16_t *m = s_map + y0 * LCD_H_RES;
        int n = LCD_H_RES * (y1 - y0 + 1);
        for (int i = 0; i < n; i++) {
            uint16_t t = m[i];
            band[i] = t == NO_TILE ? black : s_rgb[t];
        }
        panel_band_send(band, y0, y1);
    }
}
