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
#define SLOW_FROM_PX   64                   // larger tiles: generation time and fade grow with the rosette spacing
#define SLOW_PER_PX    0.01f                // extra time per px above SLOW_FROM_PX (x1.3 at 96 px, x3.7 at 330 px)
#define SLOTS          10                   // changing cells start fading in one of these time slots
#define SLOT_US        (s_gen_len_us / SLOTS)
#define SEED_DENSITY   0.30f                // live cells after a random seed
#define AGE_FULL       12                   // generations from newborn color to the last AGE_COLORS entry
#define STALE_GENS     20                   // repeating states this long: new seed
#define EMPTY_GENS     3                    // no live cells this long: new seed
#define MAX_NB         16                   // touching tiles per tile (triangles 6, quads and trapezoids 8, small hexagons 9, big hexagon 12)
#define NO_TILE        0xFFFF
#define RADIUS         (LCD_H_RES / 2.0f)
#define TILE_MAX       6                    // polygon vertices, at most
#define TOUCH_PX       0.05f                // vertices closer than this are the same point

// Rosette spacing (lattice period) in px, BOOT cycles through them
static const int EDGE_PX[] = {48, 64, 80, 96, 112, 128, 160, 200, 260, 330};
#define EDGE_COUNT     (int)(sizeof(EDGE_PX) / sizeof(EDGE_PX[0]))
#define EDGE_DEFAULT   3
#define BORDER_DIV     56.0f                // black line between tiles = 2 * spacing / BORDER_DIV, as in the image
#define BORDER_MIN     0.55f                // smallest inset per tile in px

static const char *TAG = "life";

// Dead cell color by tile kind, blue and gray tones
enum { KIND_BIG_HEX, KIND_SMALL_HEX, KIND_TRIANGLE, KIND_QUAD, KIND_TRAPEZOID, KIND_COUNT };
static const float COLOR_DEAD[KIND_COUNT][3] = {
    {22, 22, 38},
    {28, 32, 42},
    {12, 12, 22},
    {16, 20, 34},
    {24, 26, 32},
};
// Live cell color by age, equally spaced from age 0 to AGE_FULL: orange, yellow, bright green
static const float AGE_COLORS[][3] = {
    {255, 140, 30},
    {255, 230, 40},
    {60, 255, 70},
};
#define AGE_STOPS      (int)(sizeof(AGE_COLORS) / sizeof(AGE_COLORS[0]))

// Tiling from data/tassel/arabesque-arabic.jpg. Rosettes sit on a hexagonal lattice of spacing D (V1 = (1, 0),
// V2 = (1/2, sqrt(3)/2) in units of D). All lines run at 30, 90 and 150 degrees, at distances r, 2r and D/2 from the
// nearest rosette center, with r = 0.18106 D (measured). Each period holds 21 tiles: the central hexagon, six
// triangles and six quads of the star, six trapezoids (one extra vertex where a line ends on their long side) and two
// small hexagons between the rosettes. Vertices in units of D around the rosette center, from the line intersections.
typedef struct {
    int kind;
    int n;
    float v[TILE_MAX][2];
} piece_t;

static const piece_t PIECES[] = {
    {KIND_BIG_HEX, 6, {{-0.181059f, -0.104534f}, {-0.000000f, -0.209069f}, {0.181059f, -0.104534f}, {0.181059f, 0.104534f}, {0.000000f, 0.209069f}, {-0.181059f, 0.104534f}}},
    {KIND_SMALL_HEX, 6, {{-0.637885f, -0.209067f}, {-0.637885f, -0.368283f}, {-0.500000f, -0.447891f}, {-0.362115f, -0.368283f}, {-0.362115f, -0.209067f}, {-0.500000f, -0.129459f}}},
    {KIND_SMALL_HEX, 6, {{0.362115f, 0.368283f}, {0.362115f, 0.209067f}, {0.500000f, 0.129459f}, {0.637885f, 0.209067f}, {0.637885f, 0.368283f}, {0.500000f, 0.447891f}}},
    {KIND_TRIANGLE, 3, {{-0.181059f, -0.104534f}, {-0.181058f, -0.313601f}, {-0.000000f, -0.209069f}}},
    {KIND_TRIANGLE, 3, {{-0.000000f, -0.209069f}, {0.181058f, -0.313601f}, {0.181059f, -0.104534f}}},
    {KIND_TRIANGLE, 3, {{0.181059f, -0.104534f}, {0.362116f, -0.000000f}, {0.181059f, 0.104534f}}},
    {KIND_TRIANGLE, 3, {{0.181059f, 0.104534f}, {0.181058f, 0.313601f}, {0.000000f, 0.209069f}}},
    {KIND_TRIANGLE, 3, {{0.000000f, 0.209069f}, {-0.181058f, 0.313601f}, {-0.181059f, 0.104534f}}},
    {KIND_TRIANGLE, 3, {{-0.181059f, 0.104534f}, {-0.362116f, 0.000000f}, {-0.181059f, -0.104534f}}},
    {KIND_QUAD, 4, {{-0.181059f, -0.104534f}, {-0.362116f, 0.000000f}, {-0.362115f, -0.209067f}, {-0.181058f, -0.313601f}}},
    {KIND_QUAD, 4, {{-0.181058f, -0.313601f}, {0.000000f, -0.418134f}, {0.181058f, -0.313601f}, {-0.000000f, -0.209069f}}},
    {KIND_QUAD, 4, {{0.181058f, -0.313601f}, {0.362115f, -0.209067f}, {0.362116f, -0.000000f}, {0.181059f, -0.104534f}}},
    {KIND_QUAD, 4, {{0.181059f, 0.104534f}, {0.362116f, -0.000000f}, {0.362115f, 0.209067f}, {0.181058f, 0.313601f}}},
    {KIND_QUAD, 4, {{0.000000f, 0.209069f}, {0.181058f, 0.313601f}, {-0.000000f, 0.418134f}, {-0.181058f, 0.313601f}}},
    {KIND_QUAD, 4, {{-0.181058f, 0.313601f}, {-0.362115f, 0.209067f}, {-0.362116f, 0.000000f}, {-0.181059f, 0.104534f}}},
    {KIND_TRAPEZOID, 5, {{-0.362116f, 0.000000f}, {-0.362115f, 0.209067f}, {-0.500000f, 0.129459f}, {-0.500000f, -0.129459f}, {-0.362115f, -0.209067f}}},
    {KIND_TRAPEZOID, 5, {{-0.362115f, -0.368283f}, {-0.137885f, -0.497742f}, {0.000000f, -0.418134f}, {-0.181058f, -0.313601f}, {-0.362115f, -0.209067f}}},
    {KIND_TRAPEZOID, 5, {{0.137885f, -0.497742f}, {0.362115f, -0.368283f}, {0.362115f, -0.209067f}, {0.181058f, -0.313601f}, {0.000000f, -0.418134f}}},
    {KIND_TRAPEZOID, 5, {{0.500000f, -0.129459f}, {0.500000f, 0.129459f}, {0.362115f, 0.209067f}, {0.362116f, -0.000000f}, {0.362115f, -0.209067f}}},
    {KIND_TRAPEZOID, 5, {{0.362115f, 0.368283f}, {0.137885f, 0.497742f}, {-0.000000f, 0.418134f}, {0.181058f, 0.313601f}, {0.362115f, 0.209067f}}},
    {KIND_TRAPEZOID, 5, {{-0.181058f, 0.313601f}, {-0.000000f, 0.418134f}, {-0.137885f, 0.497742f}, {-0.362115f, 0.368283f}, {-0.362115f, 0.209067f}}},
};
#define PIECE_COUNT    (int)(sizeof(PIECES) / sizeof(PIECES[0]))

typedef struct {
    float x[TILE_MAX], y[TILE_MAX];
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

// Piece p of the period at lattice point (a, b), scaled to px
static shape_t polygon(const piece_t *p, int a, int b, float d)
{
    float cx = a + 0.5f * b, cy = b * sqrtf(3) / 2;
    shape_t s;
    for (int k = 0; k < p->n; k++) {
        s.x[k] = (cx + p->v[k][0]) * d;
        s.y[k] = (cy + p->v[k][1]) * d;
    }
    s.n = p->n;
    s.kind = p->kind;
    return s;
}

// Keeps the tile when its vertex mean is inside the display
static int shape_add(shape_t *shapes, int n, shape_t s)
{
    float gx = 0, gy = 0;
    for (int k = 0; k < s.n; k++) {
        gx += s.x[k];
        gy += s.y[k];
    }
    if (hypotf(gx / s.n, gy / s.n) > RADIUS || n >= NO_TILE) return n;
    shapes[n] = s;
    return n + 1;
}

// 21 pieces per lattice period; edge = rosette spacing in px
static int make_shapes(int edge, shape_t *shapes)
{
    int g = lattice_half(edge);
    int n = 0;
    for (int b = -g; b <= g; b++) {
        for (int a = -g; a <= g; a++) {
            for (int p = 0; p < PIECE_COUNT; p++) n = shape_add(shapes, n, polygon(&PIECES[p], a, b, edge));
        }
    }
    return n;
}

// Tiles with a vertex closer than TOUCH_PX to a vertex of another tile are neighbors
// (lattice translations are not exact in float, so vertices are compared with a tolerance)
static void make_neighbors(const shape_t *shapes, int count, float edge)
{
    float far2 = 0.49f * edge * edge;   // touching tiles have centers less than 0.7 spacings apart
    for (int t = 0; t < count; t++) {
        for (int u = t + 1; u < count; u++) {
            float dx = 0, dy = 0;
            for (int k = 0; k < shapes[u].n; k++) {
                dx += shapes[u].x[k] / shapes[u].n;
                dy += shapes[u].y[k] / shapes[u].n;
            }
            for (int k = 0; k < shapes[t].n; k++) {
                dx -= shapes[t].x[k] / shapes[t].n;
                dy -= shapes[t].y[k] / shapes[t].n;
            }
            if (dx * dx + dy * dy > far2) continue;
            bool touch = false;
            for (int i = 0; i < shapes[t].n && !touch; i++) {
                for (int j = 0; j < shapes[u].n; j++) {
                    if (fabsf(shapes[t].x[i] - shapes[u].x[j]) < TOUCH_PX && fabsf(shapes[t].y[i] - shapes[u].y[j]) < TOUCH_PX) {
                        touch = true;
                        break;
                    }
                }
            }
            if (!touch || s_nn[t] >= MAX_NB || s_nn[u] >= MAX_NB) continue;
            s_nb[t][s_nn[t]++] = u;
            s_nb[u][s_nn[u]++] = t;
        }
    }
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
    shape_t *shapes = heap_caps_malloc(sizeof(shape_t) * PIECE_COUNT * (2 * g + 1) * (2 * g + 1), MALLOC_CAP_SPIRAM);
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
    make_neighbors(shapes, n, edge);
    float slow = 1.0f + SLOW_PER_PX * fmaxf(0, edge - SLOW_FROM_PX);
    s_gen_len_us = (int64_t)(GEN_US * slow);
    s_tau_s = FADE_TAU_S * slow;

    float inset = fmaxf(BORDER_MIN, edge / BORDER_DIV);     // black lines as in the image
    rasterize(shapes, n, inset);
    heap_caps_free(shapes);

    int nb_min = MAX_NB, nb_max = 0;
    for (int i = 0; i < n; i++) {
        if (s_nn[i] < nb_min) nb_min = s_nn[i];
        if (s_nn[i] > nb_max) nb_max = s_nn[i];
    }
    ESP_LOGI(TAG, "period %d px: %d tiles, neighbors %d..%d, border %.1f px, generation %.1f s, fade tau %.1f s, %d ms",
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
    ESP_LOGI(TAG, "==== PERIOD %d px ====", EDGE_PX[s_edge_idx]);
    life_randomize();
}

void life_prev_size(void)
{
    s_edge_idx = (s_edge_idx + EDGE_COUNT - 1) % EDGE_COUNT;
    build(EDGE_PX[s_edge_idx]);
    ESP_LOGI(TAG, "==== PERIOD %d px ====", EDGE_PX[s_edge_idx]);
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
