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
#define SLOW_FROM_PX   30                   // larger tiles: generation time and fade grow with the lattice spacing
#define SLOW_PER_PX    0.02f                // extra time per px above SLOW_FROM_PX (x1.14 at 37 px, x3.2 at 140 px)
#define SLOTS          10                   // changing cells start fading in one of these time slots
#define SLOT_US        (s_gen_len_us / SLOTS)
#define SEED_DENSITY   0.30f                // live cells after a random seed
#define AGE_FULL       12                   // generations from newborn color to the last AGE_COLORS entry
#define STALE_GENS     20                   // repeating states this long: new seed
#define EMPTY_GENS     3                    // no live cells this long: new seed
#define MAX_NB         16                   // neighbors per tile (10 away from the edge)
#define NEAR_DIV       10.0f                // tiles whose outlines come closer than spacing / NEAR_DIV are neighbors
#define NO_TILE        0xFFFF
#define RADIUS         (LCD_H_RES / 2.0f)
#define TILE_N         60                   // points on the tile outline

// Lattice spacing (distance between tiles of the same orientation) in px, BOOT cycles through them
static const int EDGE_PX[] = {24, 30, 37, 44, 52, 60, 74, 90, 110, 140};
#define EDGE_COUNT     (int)(sizeof(EDGE_PX) / sizeof(EDGE_PX[0]))
#define EDGE_DEFAULT   2
#define LINE_PX        0                    // black line between tiles in px (0: none, the colors alone separate the tiles)

static const char *TAG = "life";

// Dead cell color by tile orientation (three classes), blue and gray tones
#define KIND_COUNT     3
static const float COLOR_DEAD[KIND_COUNT][3] = {
    {12, 12, 22},
    {22, 22, 38},
    {28, 32, 42},
};
// Live cell color by age, equally spaced from age 0 to AGE_FULL: orange, yellow, bright green
static const float AGE_COLORS[][3] = {
    {255, 140, 30},
    {255, 230, 40},
    {60, 255, 70},
};
#define AGE_STOPS      (int)(sizeof(AGE_COLORS) / sizeof(AGE_COLORS[0]))

// Tiling from data/tassel/hexTongue.jpg: one curved tile with a 180 degree symmetry, in three orientations 120
// degrees apart, repeated on a hexagonal lattice of spacing D (V1 = (0, 1), V2 = (sqrt(3)/2, 1/2) in units of D).
// The outline was averaged over the 12 complete tiles of the image (rotated back), made symmetric and scaled so
// that the three tiles fill the period area exactly; 60 points in units of D around the tile centroid.
// CLASS_POS: centroids of the three orientations (rotated by 0, 120, 240 degrees about a 3-fold center), relative to
// a swirl point, which is placed at the display center.
static const float TILE[TILE_N][2] = {
    {0.19202f, -0.42659f},
    {0.14238f, -0.41732f},
    {0.09414f, -0.40254f},
    {0.04718f, -0.38172f},
    {0.00170f, -0.35693f},
    {-0.04238f, -0.32980f},
    {-0.08425f, -0.29964f},
    {-0.12443f, -0.26855f},
    {-0.13992f, -0.22590f},
    {-0.13209f, -0.17525f},
    {-0.12718f, -0.12417f},
    {-0.12609f, -0.07262f},
    {-0.12621f, -0.02137f},
    {-0.13173f, 0.02969f},
    {-0.14358f, 0.07923f},
    {-0.16089f, 0.12602f},
    {-0.18303f, 0.17109f},
    {-0.20961f, 0.21458f},
    {-0.24362f, 0.25194f},
    {-0.28355f, 0.28367f},
    {-0.33052f, 0.30387f},
    {-0.38080f, 0.31197f},
    {-0.42137f, 0.28599f},
    {-0.45704f, 0.27710f},
    {-0.46213f, 0.32463f},
    {-0.43226f, 0.36528f},
    {-0.38890f, 0.39361f},
    {-0.34242f, 0.41507f},
    {-0.29387f, 0.42790f},
    {-0.24243f, 0.43111f},
    {-0.19202f, 0.42659f},
    {-0.14238f, 0.41732f},
    {-0.09414f, 0.40254f},
    {-0.04718f, 0.38172f},
    {-0.00170f, 0.35693f},
    {0.04238f, 0.32980f},
    {0.08425f, 0.29964f},
    {0.12443f, 0.26855f},
    {0.13992f, 0.22590f},
    {0.13209f, 0.17525f},
    {0.12718f, 0.12417f},
    {0.12609f, 0.07262f},
    {0.12621f, 0.02137f},
    {0.13173f, -0.02969f},
    {0.14358f, -0.07923f},
    {0.16089f, -0.12602f},
    {0.18303f, -0.17109f},
    {0.20961f, -0.21458f},
    {0.24362f, -0.25194f},
    {0.28355f, -0.28367f},
    {0.33052f, -0.30387f},
    {0.38080f, -0.31197f},
    {0.42137f, -0.28599f},
    {0.45704f, -0.27710f},
    {0.46213f, -0.32463f},
    {0.43226f, -0.36528f},
    {0.38890f, -0.39361f},
    {0.34242f, -0.41507f},
    {0.29387f, -0.42790f},
    {0.24243f, -0.43111f},
};
static const float CLASS_POS[3][2] = {{-0.43254f, -0.75074f}, {0.00040f, -0.49922f}, {-0.43389f, -0.25004f}};

typedef struct {
    float x[TILE_N], y[TILE_N];
    uint8_t n;
    uint8_t kind;
} shape_t;

static int s_edge_idx = EDGE_DEFAULT;
static int s_count;                     // number of tiles
static uint16_t *s_map;                 // tile index per display pixel, NO_TILE outside
static uint8_t *s_kind;
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

// Lattice periods per side from the center needed to cover the display
static int lattice_half(int edge)
{
    return (int)ceilf(RADIUS / (0.8f * edge)) + 2;
}

// Keeps the tile when its vertex mean is inside the display
static int shape_add(shape_t *shapes, int n, const shape_t *s)
{
    float gx = 0, gy = 0;
    for (int k = 0; k < s->n; k++) {
        gx += s->x[k];
        gy += s->y[k];
    }
    if (hypotf(gx / s->n, gy / s->n) > RADIUS || n >= NO_TILE) return n;
    shapes[n] = *s;
    return n + 1;
}

// Three tiles per lattice point (one per orientation); edge = lattice spacing in px
static int make_shapes(int edge, shape_t *shapes)
{
    int g = lattice_half(edge);
    int n = 0;
    for (int b = -g; b <= g; b++) {
        for (int a = -g; a <= g; a++) {
            float lx = b * sqrtf(3) / 2, ly = a + 0.5f * b;
            for (int k = 0; k < 3; k++) {
                float ang = k * 2 * (float)M_PI / 3, ca = cosf(ang), sa = sinf(ang);
                shape_t s;
                for (int i = 0; i < TILE_N; i++) {
                    float x = TILE[i][0] * ca - TILE[i][1] * sa, y = TILE[i][0] * sa + TILE[i][1] * ca;
                    s.x[i] = (lx + CLASS_POS[k][0] + x) * edge;
                    s.y[i] = (ly + CLASS_POS[k][1] + y) * edge;
                }
                s.n = TILE_N;
                s.kind = k;
                n = shape_add(shapes, n, &s);
            }
        }
    }
    return n;
}

// Neighbors: tiles whose outlines come closer than spacing / NEAR_DIV. Each tile touches 4 tiles along its sides;
// 6 more come within 0.04..0.07 spacings around the small holes at the swirl points (as in the image, where every
// complete tile touches 10 tiles across the drawn lines). The next tile is 0.25 spacings away.
static void make_neighbors(const shape_t *shapes, int count, float edge)
{
    float near2 = edge * edge / (NEAR_DIV * NEAR_DIV), far2 = 2.56f * edge * edge;
    float *cx = heap_caps_malloc(sizeof(float) * count * 2, MALLOC_CAP_SPIRAM), *cy = cx + count;
    for (int t = 0; t < count; t++) {
        cx[t] = cy[t] = 0;
        for (int k = 0; k < shapes[t].n; k++) {
            cx[t] += shapes[t].x[k] / shapes[t].n;
            cy[t] += shapes[t].y[k] / shapes[t].n;
        }
    }
    for (int t = 0; t < count; t++) {
        for (int u = t + 1; u < count; u++) {
            float dx = cx[u] - cx[t], dy = cy[u] - cy[t];
            if (dx * dx + dy * dy > far2) continue;
            bool near = false;
            for (int i = 0; i < shapes[t].n && !near; i++) {
                for (int j = 0; j < shapes[u].n; j++) {
                    float ex = shapes[t].x[i] - shapes[u].x[j], ey = shapes[t].y[i] - shapes[u].y[j];
                    if (ex * ex + ey * ey < near2) {
                        near = true;
                        break;
                    }
                }
            }
            if (!near || s_nn[t] >= MAX_NB || s_nn[u] >= MAX_NB) continue;
            s_nb[t][s_nn[t]++] = u;
            s_nb[u][s_nn[u]++] = t;
        }
    }
    heap_caps_free(cx);
}

// Fills uncovered pixels left by the sampled outlines (and the small holes at the swirl points), then draws the
// black line: pixels next to a different tile, within line px, lose their tile
static void map_finish(int line)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 1; y < LCD_V_RES - 1; y++) {
            for (int x = 1; x < LCD_H_RES - 1; x++) {
                uint16_t *m = &s_map[y * LCD_H_RES + x];
                if (*m != NO_TILE || hypotf(x + 0.5f - RADIUS, y + 0.5f - RADIUS) > RADIUS) continue;
                uint16_t c[4] = {m[-1], m[1], m[-LCD_H_RES], m[LCD_H_RES]};
                for (int k = 0; k < 4; k++) {
                    if (c[k] < 0x8000) {            // a tile not filled in this pass
                        *m = c[k] | 0x8000;     // marked as filled in this pass
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
            if (s_map[i] != NO_TILE) s_map[i] &= 0x7FFF;
        }
    }
    if (line <= 0) return;
    uint8_t *edge = heap_caps_calloc(LCD_H_RES * LCD_V_RES, 1, MALLOC_CAP_SPIRAM);
    for (int y = 0; y < LCD_V_RES; y++) {
        for (int x = 0; x < LCD_H_RES; x++) {
            uint16_t t = s_map[y * LCD_H_RES + x];
            if (t == NO_TILE) continue;
            for (int d = 1; d <= line && !edge[y * LCD_H_RES + x]; d++) {
                if (x + d < LCD_H_RES) {
                    uint16_t u = s_map[y * LCD_H_RES + x + d];
                    if (u != NO_TILE && u != t) edge[y * LCD_H_RES + x] = 1;
                }
                if (y + d < LCD_V_RES) {
                    uint16_t u = s_map[(y + d) * LCD_H_RES + x];
                    if (u != NO_TILE && u != t) edge[y * LCD_H_RES + x] = 1;
                }
            }
        }
    }
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) {
        if (edge[i]) s_map[i] = NO_TILE;
    }
    heap_caps_free(edge);
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

// Pixel centers inside a tile (crossing test, the outline is not convex) get its index.
// inset > 0: pixels closer than inset to a tile edge stay black (border).
static void rasterize(const shape_t *shapes, int count, float inset)
{
    for (int i = 0; i < LCD_H_RES * LCD_V_RES; i++) s_map[i] = NO_TILE;
    float inset2 = inset * inset;
    for (int t = 0; t < count; t++) {
        const shape_t *s = &shapes[t];
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int k = 0; k < s->n; k++) {
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
                for (int k = 0, k1 = s->n - 1; k < s->n; k1 = k++) {
                    if ((s->y[k] > y) != (s->y[k1] > y) &&
                        x < (s->x[k1] - s->x[k]) * (y - s->y[k]) / (s->y[k1] - s->y[k]) + s->x[k]) in = !in;
                }
                for (int k = 0; k < s->n && in && inset > 0; k++) {
                    int k1 = (k + 1) % s->n;
                    if (seg_dist2(x, y, s->x[k], s->y[k], s->x[k1], s->y[k1]) < inset2) in = false;
                }
                if (in) s_map[py * LCD_H_RES + px] = t;
            }
        }
    }
}

static void cells_free(void)
{
    void *p[] = {s_kind, s_alive, s_next, s_prev, s_slot, s_age, s_nn, s_nb, s_mid, s_color, s_rgb};
    for (int i = 0; i < (int)(sizeof(p) / sizeof(p[0])); i++) heap_caps_free(p[i]);
}

static void build(int edge)
{
    int64_t t0 = esp_timer_get_time();
    cells_free();

    int g = lattice_half(edge);
    shape_t *shapes = heap_caps_malloc(sizeof(shape_t) * 3 * (2 * g + 1) * (2 * g + 1), MALLOC_CAP_SPIRAM);
    s_count = make_shapes(edge, shapes);

    int n = s_count;
    s_kind = heap_caps_calloc(n, 1, MALLOC_CAP_SPIRAM);
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

    for (int i = 0; i < n; i++) s_kind[i] = shapes[i].kind;
    float slow = 1.0f + SLOW_PER_PX * fmaxf(0, edge - SLOW_FROM_PX);
    s_gen_len_us = (int64_t)(GEN_US * slow);
    s_tau_s = FADE_TAU_S * slow;

    make_neighbors(shapes, n, edge);
    float inset = 0;
    rasterize(shapes, n, inset);
    heap_caps_free(shapes);
    int line = LINE_PX;
    map_finish(line);

    int nb_min = MAX_NB, nb_max = 0;
    for (int i = 0; i < n; i++) {
        if (s_nn[i] < nb_min) nb_min = s_nn[i];
        if (s_nn[i] > nb_max) nb_max = s_nn[i];
    }
    ESP_LOGI(TAG, "spacing %d px: %d tiles, neighbors %d..%d, line %d px, generation %.1f s, fade tau %.1f s, %d ms",
             edge, n, nb_min, nb_max, line, s_gen_len_us * 1e-6f, s_tau_s,
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
    ESP_LOGI(TAG, "==== SPACING %d px ====", EDGE_PX[s_edge_idx]);
    life_randomize();
}

void life_prev_size(void)
{
    s_edge_idx = (s_edge_idx + EDGE_COUNT - 1) % EDGE_COUNT;
    build(EDGE_PX[s_edge_idx]);
    ESP_LOGI(TAG, "==== SPACING %d px ====", EDGE_PX[s_edge_idx]);
    life_randomize();
}

void life_deinit(void)
{
    cells_free();
    s_kind = NULL;
    s_alive = NULL;
    s_next = NULL;
    s_prev = NULL;
    s_slot = NULL;
    s_age = NULL;
    s_nn = NULL;
    s_nb = NULL;
    s_mid = NULL;
    s_color = NULL;
    s_rgb = NULL;
    heap_caps_free(s_map);
    s_map = NULL;
    s_count = 0;
    s_last_us = 0;
}

int life_size_idx(void)
{
    return s_edge_idx;
}

void life_set_size_idx(int idx)
{
    if (idx >= 0 && idx < EDGE_COUNT) s_edge_idx = idx;
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
            const float *d = COLOR_DEAD[s_kind[t]];
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
