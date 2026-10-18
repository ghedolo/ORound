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
#define SLOW_FROM_PX   14                   // larger tiles: generation time and fade grow with the edge
#define SLOW_PER_PX    0.04f                // extra time per px above SLOW_FROM_PX (x2.4 at 48 px, x3 at 64 px)
#define SLOTS          10                   // changing cells start fading in one of these time slots
#define SLOT_US        (s_gen_len_us / SLOTS)
#define SEED_DENSITY   0.30f                // live cells after a random seed
#define AGE_FULL       12                   // generations from newborn color to the last AGE_COLORS entry
#define STALE_GENS     20                   // repeating states this long: new seed
#define EMPTY_GENS     3                    // no live cells this long: new seed
#define MAX_NB         16                   // vertex neighbors per tile (Penrose P3: at most 11)
#define MAX_TRI        60000                // triangles during the subdivision
#define NO_TILE        0xFFFF
#define PHI            1.6180339887f
#define RADIUS         (LCD_H_RES / 2.0f)

// Tile edge lengths in px, BOOT cycles through them
static const int EDGE_PX[] = {8, 11, 14, 18, 24, 32, 40, 48, 56, 64};
#define EDGE_COUNT     (int)(sizeof(EDGE_PX) / sizeof(EDGE_PX[0]))
#define EDGE_DEFAULT   2
#define BORDER_FROM_PX 18                   // from this edge length on, tiles get a black border
#define BORDER_DIV     32.0f                // border inset per tile = edge / BORDER_DIV (line = twice that)
#define BORDER_MIN     0.55f                // smallest inset per tile in px

static const char *TAG = "life";

static const float COLOR_DEAD_THIN[3]  = {12, 12, 22};
static const float COLOR_DEAD_THICK[3] = {22, 22, 38};
// Live cell color by age, equally spaced from age 0 to AGE_FULL: orange, yellow, bright green
static const float AGE_COLORS[][3] = {
    {255, 140, 30},
    {255, 230, 40},
    {60, 255, 70},
};
#define AGE_STOPS      (int)(sizeof(AGE_COLORS) / sizeof(AGE_COLORS[0]))

// Robinson triangle: color 0 = half of a thin rhombus, 1 = half of a thick one.
// A is the apex, BC the base shared with the other half of the rhombus.
typedef struct {
    uint8_t color;
    float ax, ay, bx, by, cx, cy;
} tri_t;

// Tile polygon (rhombus, or triangle when the other half was culled)
typedef struct {
    float x[4], y[4];
    uint8_t n;
    uint8_t thick;
} shape_t;

static int s_edge_idx = EDGE_DEFAULT;
static int s_count;                     // number of tiles
static uint16_t *s_map;                 // tile index per display pixel, NO_TILE outside
static uint8_t *s_thick;
static uint8_t *s_alive, *s_next, *s_prev, *s_slot, *s_age, *s_nn;
static uint16_t (*s_nb)[MAX_NB];
static float (*s_mid)[3];               // first filter stage
static float (*s_color)[3];             // displayed color (second stage): slow start, slow end
static pixel_t *s_rgb;                  // displayed color for the renderer

static int64_t s_gen_len_us = GEN_US;   // time between generations for the current edge
static float s_tau_s = FADE_TAU_S;      // fade time constant for the current edge
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

static bool tri_visible(const tri_t *t)
{
    float gx = (t->ax + t->bx + t->cx) / 3, gy = (t->ay + t->by + t->cy) / 3;
    float r = fmaxf(hypotf(t->ax - gx, t->ay - gy), fmaxf(hypotf(t->bx - gx, t->by - gy), hypotf(t->cx - gx, t->cy - gy)));
    return hypotf(gx, gy) - r <= RADIUS + 1;
}

static int tri_add(tri_t *out, int n, uint8_t color, float ax, float ay, float bx, float by, float cx, float cy)
{
    if (n >= MAX_TRI) return n;
    tri_t t = {color, ax, ay, bx, by, cx, cy};
    if (!tri_visible(&t)) return n;
    out[n] = t;
    return n + 1;
}

// One deflation step; triangles that cannot reach the display are dropped
static int subdivide(const tri_t *in, int count, tri_t *out)
{
    int n = 0;
    for (int i = 0; i < count; i++) {
        const tri_t *t = &in[i];
        if (t->color == 0) {
            float px = t->ax + (t->bx - t->ax) / PHI, py = t->ay + (t->by - t->ay) / PHI;
            n = tri_add(out, n, 0, t->cx, t->cy, px, py, t->bx, t->by);
            n = tri_add(out, n, 1, px, py, t->cx, t->cy, t->ax, t->ay);
        } else {
            float qx = t->bx + (t->ax - t->bx) / PHI, qy = t->by + (t->ay - t->by) / PHI;
            float rx = t->bx + (t->cx - t->bx) / PHI, ry = t->by + (t->cy - t->by) / PHI;
            n = tri_add(out, n, 1, rx, ry, t->cx, t->cy, t->ax, t->ay);
            n = tri_add(out, n, 1, qx, qy, rx, ry, t->bx, t->by);
            n = tri_add(out, n, 0, rx, ry, qx, qy, t->ax, t->ay);
        }
    }
    if (n >= MAX_TRI) ESP_LOGE(TAG, "too many triangles, tiling incomplete");
    return n;
}

// Base edge key: both end points rounded to 1/8 px, smaller point first
typedef struct {
    int32_t x1, y1, x2, y2;
    int32_t color;
    int32_t idx;
} edge_key_t;

static int cmp_edge(const void *a, const void *b)
{
    const edge_key_t *p = a, *q = b;
    if (p->x1 != q->x1) return p->x1 < q->x1 ? -1 : 1;
    if (p->y1 != q->y1) return p->y1 < q->y1 ? -1 : 1;
    if (p->x2 != q->x2) return p->x2 < q->x2 ? -1 : 1;
    if (p->y2 != q->y2) return p->y2 < q->y2 ? -1 : 1;
    return p->color - q->color;
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

// Pairs triangles sharing the base into rhombi; keeps tiles with the center inside the display
static int make_shapes(const tri_t *tri, int count, shape_t *shapes)
{
    edge_key_t *keys = heap_caps_malloc(sizeof(edge_key_t) * count, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < count; i++) {
        int32_t x1 = lroundf(tri[i].bx * 8), y1 = lroundf(tri[i].by * 8);
        int32_t x2 = lroundf(tri[i].cx * 8), y2 = lroundf(tri[i].cy * 8);
        if (x2 < x1 || (x2 == x1 && y2 < y1)) {
            int32_t tx = x1, ty = y1;
            x1 = x2; y1 = y2; x2 = tx; y2 = ty;
        }
        keys[i] = (edge_key_t){x1, y1, x2, y2, tri[i].color, i};
    }
    qsort(keys, count, sizeof(edge_key_t), cmp_edge);

    int n = 0;
    for (int i = 0; i < count; i++) {
        shape_t s;
        const tri_t *a = &tri[keys[i].idx];
        s.thick = a->color;
        if (i + 1 < count && cmp_edge(&keys[i], &keys[i + 1]) == 0) {
            const tri_t *b = &tri[keys[i + 1].idx];
            s.n = 4;
            s.x[0] = a->ax; s.y[0] = a->ay;
            s.x[1] = a->bx; s.y[1] = a->by;
            s.x[2] = b->ax; s.y[2] = b->ay;
            s.x[3] = a->cx; s.y[3] = a->cy;
            i++;
        } else {
            s.n = 3;
            s.x[0] = a->ax; s.y[0] = a->ay;
            s.x[1] = a->bx; s.y[1] = a->by;
            s.x[2] = a->cx; s.y[2] = a->cy;
        }
        float gx = 0, gy = 0;
        for (int k = 0; k < s.n; k++) {
            gx += s.x[k];
            gy += s.y[k];
        }
        if (hypotf(gx / s.n, gy / s.n) > RADIUS || n >= NO_TILE) continue;
        shapes[n++] = s;
    }
    heap_caps_free(keys);
    return n;
}

// Tiles sharing a vertex are neighbors
static void make_neighbors(const shape_t *shapes, int count)
{
    int nv = 0;
    for (int i = 0; i < count; i++) nv += shapes[i].n;
    vert_key_t *keys = heap_caps_malloc(sizeof(vert_key_t) * nv, MALLOC_CAP_SPIRAM);
    int k = 0;
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < shapes[i].n; j++) {
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

// Pixel centers inside a tile get its index (edges included, the last tile wins).
// inset > 0: pixels closer than inset to a tile edge stay black (border).
static void rasterize(const shape_t *shapes, int count, float inset)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) s_map[i] = NO_TILE;
    for (int t = 0; t < count; t++) {
        const shape_t *s = &shapes[t];
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        float area = 0;
        for (int k = 0; k < s->n; k++) {
            int k1 = (k + 1) % s->n;
            x0 = fminf(x0, s->x[k]); x1 = fmaxf(x1, s->x[k]);
            y0 = fminf(y0, s->y[k]); y1 = fmaxf(y1, s->y[k]);
            area += s->x[k] * s->y[k1] - s->x[k1] * s->y[k];
        }
        float sign = area > 0 ? 1.0f : -1.0f;
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
                bool in = true;
                for (int k = 0; k < s->n && in; k++) {
                    int k1 = (k + 1) % s->n;
                    float ex = s->x[k1] - s->x[k], ey = s->y[k1] - s->y[k];
                    float cr = ex * (y - s->y[k]) - ey * (x - s->x[k]);
                    if (cr * sign < inset * hypotf(ex, ey) - 1e-3f) in = false;
                }
                if (in) s_map[py * LCD_H_RES + px] = t;
            }
        }
    }
}

static void cells_free(void)
{
    void *p[] = {s_thick, s_alive, s_next, s_prev, s_slot, s_age, s_nn, s_nb, s_mid, s_color, s_rgb};
    for (int i = 0; i < (int)(sizeof(p) / sizeof(p[0])); i++) heap_caps_free(p[i]);
}

static void build(int edge)
{
    int64_t t0 = esp_timer_get_time();
    cells_free();

    // Wheel of 10 thin halves; radius = edge * PHI^n, the decagon covers the display
    float r0 = RADIUS / cosf((float)M_PI / 10) + 2 * edge;
    int steps = (int)ceilf(logf(r0 / edge) / logf(PHI));
    float r = edge * powf(PHI, steps);
    tri_t *a = heap_caps_malloc(sizeof(tri_t) * MAX_TRI, MALLOC_CAP_SPIRAM);
    tri_t *b = heap_caps_malloc(sizeof(tri_t) * MAX_TRI, MALLOC_CAP_SPIRAM);
    int count = 0;
    for (int i = 0; i < 10; i++) {
        float ab = (2 * i - 1) * (float)M_PI / 10, ac = (2 * i + 1) * (float)M_PI / 10;
        float bx = r * cosf(ab), by = r * sinf(ab), cx = r * cosf(ac), cy = r * sinf(ac);
        if (i % 2 == 0) a[count++] = (tri_t){0, 0, 0, cx, cy, bx, by};
        else a[count++] = (tri_t){0, 0, 0, bx, by, cx, cy};
    }
    for (int s = 0; s < steps; s++) {
        count = subdivide(a, count, b);
        tri_t *t = a;
        a = b;
        b = t;
    }
    heap_caps_free(b);

    shape_t *shapes = heap_caps_malloc(sizeof(shape_t) * count, MALLOC_CAP_SPIRAM);
    s_count = make_shapes(a, count, shapes);
    heap_caps_free(a);

    int n = s_count;
    s_thick = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
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

    int halves = 0;
    for (int i = 0; i < n; i++) {
        s_thick[i] = shapes[i].thick;
        halves += shapes[i].n == 3;
    }
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
    ESP_LOGI(TAG, "edge %d px: %d steps, %d triangles, %d tiles (%d unpaired), neighbors %d..%d, border %.1f px, "
             "generation %.1f s, fade tau %.1f s, %d ms",
             edge, steps, count, n, halves, nb_min, nb_max, 2 * inset, s_gen_len_us * 1e-6f, s_tau_s,
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
    ESP_LOGI(TAG, "==== TILE EDGE %d px ====", EDGE_PX[s_edge_idx]);
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
            const float *d = s_thick[t] ? COLOR_DEAD_THICK : COLOR_DEAD_THIN;
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
