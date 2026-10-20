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
#define SLOW_FROM_PX   16                   // larger tiles: generation time and fade grow with the side
#define SLOW_PER_PX    0.05f                // extra time per px above SLOW_FROM_PX (x2.2 at 40 px)
#define SLOTS          10                   // changing cells start fading in one of these time slots
#define SLOT_US        (s_gen_len_us / SLOTS)
#define SEED_DENSITY   0.30f                // live cells after a random seed
#define AGE_FULL       12                   // generations from newborn color to the last AGE_COLORS entry
#define STALE_GENS     20                   // repeating states this long: new seed
#define EMPTY_GENS     3                    // no live cells this long: new seed
#define MAX_NB         16                   // touching tiles per tile (pentagons 3..10, rhombi 6)
#define NO_TILE        0xFFFF
#define RADIUS         (LCD_H_RES / 2.0f)
#define TILE_MAX       5                    // polygon vertices, at most
#define PHI            1.6180339887f
#define TOUCH_PX       0.05f                // vertices closer than this are the same point

// Life rule as bit masks of live neighbor counts. With few neighbors per tile (most 3..7)
// B3/S23 freezes within ~15 generations, B2/S23 keeps changing.
#define RULE_BIRTH     (1u << 2)
#define RULE_SURVIVE   ((1u << 2) | (1u << 3))

// Pentaflake levels (BOOT cycles through them); pentagon side = 466 px flake / (1 + PHI)^level:
// level 2: 40 px, 3: 15.3 px, 4: 5.8 px
static const int LEVELS[] = {3, 4, 2};
#define LEVEL_COUNT    (int)(sizeof(LEVELS) / sizeof(LEVELS[0]))
#define BORDER_FROM_PX 12                   // from this side on, tiles get a black border
#define BORDER_DIV     28.0f                // border inset per tile = side / BORDER_DIV (line = twice that)
#define BORDER_MIN     0.55f                // smallest inset per tile in px

static const char *TAG = "life";

// Dead cell color by tile kind: pentagon vertex up, pentagon vertex down, rhombus (gray blue)
enum { KIND_UP, KIND_DOWN, KIND_RHOMB };
static const float COLOR_DEAD[3][3] = {
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

// Pentaflake from data/tassel/penta1.webp (level 2 there):
// a flake of circumradius R is a central flake of radius R / (1 + PHI) rotated by 36 degrees plus five flakes
// of the same radius and orientation at distance PHI * R / (1 + PHI) toward the five tips. Pentagons touch only
// at vertices. The small gaps closed by four pentagon edges are thin rhombi (36 degrees) and become cells too;
// the larger star shaped gaps and the open gaps stay black.
typedef struct {
    float x[TILE_MAX], y[TILE_MAX];
    uint8_t n;
    uint8_t kind;
} shape_t;

static int s_level_idx;
static float s_side_px;                 // pentagon side of the current level
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

// Adds the pentagons of a flake centered on (cx, cy) with circumradius r and first vertex at angle th
static int flake(shape_t *shapes, int n, int level, float cx, float cy, float r, float th, bool down)
{
    if (level == 0) {
        shape_t *s = &shapes[n];
        for (int k = 0; k < 5; k++) {
            float a = th + k * 2 * (float)M_PI / 5;
            s->x[k] = cx + r * cosf(a);
            s->y[k] = cy + r * sinf(a);
        }
        s->n = 5;
        s->kind = down ? KIND_DOWN : KIND_UP;
        return n + 1;
    }
    float rc = r / (1 + PHI);
    n = flake(shapes, n, level - 1, cx, cy, rc, th + (float)M_PI / 5, !down);
    for (int k = 0; k < 5; k++) {
        float a = th + k * 2 * (float)M_PI / 5;
        n = flake(shapes, n, level - 1, cx + PHI * rc * cosf(a), cy + PHI * rc * sinf(a), rc, th, down);
    }
    return n;
}

// Edge midpoint key, 1/4 px
typedef struct {
    int32_t x, y;
} mid_key_t;

static int cmp_mid(const void *a, const void *b)
{
    const mid_key_t *p = a, *q = b;
    if (p->x != q->x) return p->x < q->x ? -1 : 1;
    if (p->y != q->y) return p->y < q->y ? -1 : 1;
    return 0;
}

// True when some pentagon edge has its midpoint within one key step of the midpoint of (ax, ay)-(bx, by)
static bool is_edge(const mid_key_t *mids, int count, float ax, float ay, float bx, float by)
{
    int32_t kx = lroundf((ax + bx) * 2), ky = lroundf((ay + by) * 2);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
            mid_key_t k = {kx + dx, ky + dy};
            if (bsearch(&k, mids, count, sizeof(mid_key_t), cmp_mid)) return true;
        }
    }
    return false;
}

// Adds the thin rhombi closed by four pentagon edges: for each pentagon edge AB, rhombi ABCD on the outer side
// with angle 36 or 144 degrees at A are kept when BC, CD and DA are pentagon edges too
static int add_rhombi(shape_t *shapes, int n_penta, int max)
{
    int nm = n_penta * 5;
    mid_key_t *mids = heap_caps_malloc(sizeof(mid_key_t) * nm, MALLOC_CAP_SPIRAM);
    for (int t = 0; t < n_penta; t++) {
        for (int k = 0; k < 5; k++) {
            int k1 = (k + 1) % 5;
            mids[t * 5 + k] = (mid_key_t){lroundf((shapes[t].x[k] + shapes[t].x[k1]) * 2),
                                          lroundf((shapes[t].y[k] + shapes[t].y[k1]) * 2)};
        }
    }
    qsort(mids, nm, sizeof(mid_key_t), cmp_mid);

    static const float ANGLES[4] = {36, -36, 144, -144};
    int n = n_penta;
    for (int t = 0; t < n_penta; t++) {
        const shape_t *p = &shapes[t];
        float cx = 0, cy = 0;
        for (int k = 0; k < 5; k++) {
            cx += p->x[k] / 5;
            cy += p->y[k] / 5;
        }
        for (int k = 0; k < 5; k++) {
            int k1 = (k + 1) % 5;
            float ax = p->x[k], ay = p->y[k], bx = p->x[k1], by = p->y[k1];
            float mx = (ax + bx) / 2, my = (ay + by) / 2;
            for (int i = 0; i < 4; i++) {
                float a = ANGLES[i] * (float)M_PI / 180, c = cosf(a), sn = sinf(a);
                float vx = (bx - ax) * c - (by - ay) * sn, vy = (bx - ax) * sn + (by - ay) * c;
                float qx = bx + vx, qy = by + vy, dx = ax + vx, dy = ay + vy;
                float rx = (ax + qx) / 2, ry = (ay + qy) / 2;
                if ((rx - mx) * (mx - cx) + (ry - my) * (my - cy) <= 0) continue;     // inner side
                if (!is_edge(mids, nm, bx, by, qx, qy) || !is_edge(mids, nm, qx, qy, dx, dy) ||
                    !is_edge(mids, nm, dx, dy, ax, ay)) continue;
                bool known = false;     // found again from its other three edges
                for (int r = n_penta; r < n && !known; r++) {
                    float ux = (shapes[r].x[0] + shapes[r].x[2]) / 2, uy = (shapes[r].y[0] + shapes[r].y[2]) / 2;
                    known = fabsf(ux - rx) < TOUCH_PX && fabsf(uy - ry) < TOUCH_PX;
                }
                if (known || n >= max) continue;
                shapes[n++] = (shape_t){{ax, bx, qx, dx}, {ay, by, qy, dy}, 4, KIND_RHOMB};
            }
        }
    }
    heap_caps_free(mids);
    return n;
}

// Tiles with a vertex closer than TOUCH_PX to a vertex of another tile are neighbors
static void make_neighbors(const shape_t *shapes, int count, float side)
{
    float far2 = 4 * side * side;       // touching tiles have centers 1.9 sides apart at most
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

// Pixel centers inside a tile (crossing test) get its index.
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

static void build(int level)
{
    int64_t t0 = esp_timer_get_time();
    cells_free();

    int n_penta = 1;
    for (int i = 0; i < level; i++) n_penta *= 6;
    int max = n_penta + n_penta / 2;    // rhombi: 5, 40, 260 for levels 2, 3, 4
    shape_t *shapes = heap_caps_malloc(sizeof(shape_t) * max, MALLOC_CAP_SPIRAM);
    flake(shapes, 0, level, 0, 0, RADIUS, -(float)M_PI / 2, false);
    s_count = add_rhombi(shapes, n_penta, max);
    s_side_px = hypotf(shapes[0].x[1] - shapes[0].x[0], shapes[0].y[1] - shapes[0].y[0]);
    float side = s_side_px;
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
    make_neighbors(shapes, n, side);
    float slow = 1.0f + SLOW_PER_PX * fmaxf(0, side - SLOW_FROM_PX);
    s_gen_len_us = (int64_t)(GEN_US * slow);
    s_tau_s = FADE_TAU_S * slow;

    float inset = 0;
    if (side >= BORDER_FROM_PX) inset = fmaxf(BORDER_MIN, side / BORDER_DIV);
    rasterize(shapes, n, inset);
    heap_caps_free(shapes);

    int nb_min = MAX_NB, nb_max = 0;
    for (int i = 0; i < n; i++) {
        if (s_nn[i] < nb_min) nb_min = s_nn[i];
        if (s_nn[i] > nb_max) nb_max = s_nn[i];
    }
    ESP_LOGI(TAG, "level %d, side %.1f px: %d tiles (%d rhombi), neighbors %d..%d, border %.1f px, generation %.1f s, "
             "fade tau %.1f s, %d ms",
             level, side, n, n - n_penta, nb_min, nb_max, 2 * inset, s_gen_len_us * 1e-6f, s_tau_s,
             (int)((esp_timer_get_time() - t0) / 1000));
}

// ---- Life ----

void life_init(void)
{
    build(LEVELS[s_level_idx]);
    life_randomize();
}

int life_edge_px(void)
{
    return (int)lroundf(s_side_px);
}

void life_next_size(void)
{
    s_level_idx = (s_level_idx + 1) % LEVEL_COUNT;
    build(LEVELS[s_level_idx]);
    ESP_LOGI(TAG, "==== LEVEL %d, SIDE %.1f px ====", LEVELS[s_level_idx], s_side_px);
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
        bool live = ((s_alive[i] ? RULE_SURVIVE : RULE_BIRTH) >> nb) & 1;
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
