#include "spark.h"

#include <math.h>
#include <string.h>
#include "board_pins.h"
#include "esp_random.h"

#define SPARK_MAX       1100
#define RATE_BASE       260.0f      // sparks per second without wind
#define RATE_WIND       3.0f        // rate x (1 + RATE_WIND * wind)
#define SPEED_MIN       120.0f      // px/s
#define SPEED_MAX       560.0f
#define GRAVITY         520.0f      // px/s^2, towards +y
#define DRAG            1.2f        // 1/s: v *= exp(-DRAG * dt)
#define LIFE_MIN        0.25f       // s
#define LIFE_MAX        1.10f
#define STREAK_S        0.035f      // streak length = speed * STREAK_S
#define JITTER_PX       5.0f        // emission point spread around the source
#define SPLIT_PROB      0.30f       // chance that a spark splits at the end of its life
#define SPLIT_CHILDREN  3
#define SPLIT_SPEED     0.45f       // child speed as a fraction of SPEED_MAX (max)
#define SPLIT_LIFE      0.30f       // child life max, s
#define DT_MAX          0.1f
// Explosions: now and then a burst of sparks at a random point away from the center
#define BOOM_GAP_MIN    8.0f        // s between two explosions, uniform in MIN..MAX
#define BOOM_GAP_MAX    20.0f
#define BOOM_R_MIN      0.35f       // distance from the display center, fraction of the radius
#define BOOM_R_MAX      0.75f
#define BOOM_SPARKS     260
#define BOOM_SPEED_MIN  150.0f      // px/s
#define BOOM_SPEED_MAX  750.0f
#define BOOM_FLASH_S    0.08f       // white disk at the explosion point for this long
#define BOOM_FLASH_PX   14          // disk radius

_Static_assert(SPARK_STRIDE == (LCD_H_RES + 7) / 8, "SPARK_STRIDE");

typedef struct {
    float x, y, vx, vy;
    float life;                     // remaining, s
    bool child;                     // children do not split again
    bool boom;                      // from an explosion (children inherit it)
} spark_t;

static spark_t s_sp[SPARK_MAX];
static int s_n;
static float s_emit_acc;            // fractional sparks carried to the next update
static int64_t s_last_us;
static uint32_t s_rng;
static uint8_t s_mask[LCD_V_RES * SPARK_STRIDE];       // fire sparks
static uint8_t s_mask_boom[LCD_V_RES * SPARK_STRIDE];  // explosion sparks and flash
static float s_boom_in;             // s to the next explosion
static float s_flash;               // remaining flash time, s
static int s_flash_x, s_flash_y;

static inline uint32_t rng_next(void)
{
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}

// Uniform in [0, 1)
static inline float frand(void)
{
    return (rng_next() >> 8) * (1.0f / 16777216.0f);
}

void spark_init(void)
{
    s_rng = esp_random() | 1;
    s_n = 0;
    s_emit_acc = 0;
    s_last_us = 0;
    s_boom_in = BOOM_GAP_MIN + (BOOM_GAP_MAX - BOOM_GAP_MIN) * frand();
    s_flash = 0;
}

static void spawn(float x, float y, float speed, float life, bool child, bool boom)
{
    if (s_n >= SPARK_MAX) return;
    float a = frand() * 2.0f * (float)M_PI;
    spark_t *s = &s_sp[s_n++];
    s->x = x;
    s->y = y;
    s->vx = cosf(a) * speed;
    s->vy = sinf(a) * speed;
    s->life = life;
    s->child = child;
    s->boom = boom;
}

static void boom(void)
{
    const float r0 = LCD_H_RES / 2.0f;
    float a = frand() * 2.0f * (float)M_PI;
    float r = r0 * (BOOM_R_MIN + (BOOM_R_MAX - BOOM_R_MIN) * frand());
    float x = r0 + cosf(a) * r, y = r0 + sinf(a) * r;
    for (int k = 0; k < BOOM_SPARKS; k++) {
        float u = frand();
        float speed = BOOM_SPEED_MIN + (BOOM_SPEED_MAX - BOOM_SPEED_MIN) * u * u;
        spawn(x, y, speed, LIFE_MIN + (LIFE_MAX - LIFE_MIN) * frand(), false, true);
    }
    s_flash = BOOM_FLASH_S;
    s_flash_x = (int)x;
    s_flash_y = (int)y;
}

void spark_update(int64_t now_us, int cx, int cy, bool emit, float wind)
{
    float dt = s_last_us ? (now_us - s_last_us) * 1e-6f : 0;
    s_last_us = now_us;
    if (dt > DT_MAX) dt = DT_MAX;
    if (dt <= 0) return;

    float damp = expf(-DRAG * dt);
    for (int i = 0; i < s_n;) {
        spark_t *s = &s_sp[i];
        s->life -= dt;
        if (s->life <= 0) {
            float x = s->x, y = s->y;
            bool split = !s->child && frand() < SPLIT_PROB;
            bool from_boom = s->boom;
            *s = s_sp[--s_n];       // remove: last spark takes its place
            if (split) {
                for (int k = 0; k < SPLIT_CHILDREN; k++) {
                    spawn(x, y, SPEED_MAX * SPLIT_SPEED * (0.3f + 0.7f * frand()), SPLIT_LIFE * (0.4f + 0.6f * frand()), true, from_boom);
                }
            }
            continue;
        }
        s->vx *= damp;
        s->vy = s->vy * damp + GRAVITY * dt;
        s->x += s->vx * dt;
        s->y += s->vy * dt;
        i++;
    }

    if (s_flash > 0) s_flash -= dt;
    if (!emit) {
        s_emit_acc = 0;
        return;
    }
    s_boom_in -= dt;
    if (s_boom_in <= 0) {
        boom();
        s_boom_in = BOOM_GAP_MIN + (BOOM_GAP_MAX - BOOM_GAP_MIN) * frand();
    }
    s_emit_acc += RATE_BASE * (1.0f + RATE_WIND * wind) * dt;
    while (s_emit_acc >= 1.0f) {
        s_emit_acc -= 1.0f;
        // Speed biased towards low values: few fast sparks, many slow ones
        float u = frand();
        float speed = SPEED_MIN + (SPEED_MAX - SPEED_MIN) * u * u;
        float jx = (frand() * 2.0f - 1.0f) * JITTER_PX, jy = (frand() * 2.0f - 1.0f) * JITTER_PX;
        spawn(cx + jx, cy + jy, speed, LIFE_MIN + (LIFE_MAX - LIFE_MIN) * frand(), false, false);
    }
}

static inline void plot(uint8_t *mask, int x, int y)
{
    if ((unsigned)x >= LCD_H_RES || (unsigned)y >= LCD_V_RES) return;
    mask[y * SPARK_STRIDE + (x >> 3)] |= (uint8_t)(1u << (x & 7));
}

// Segment from the tail (x0, y0) to the head (x1, y1), one pixel per step
static void segment(uint8_t *mask, float x0, float y0, float x1, float y1)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = fmaxf(fabsf(dx), fabsf(dy));
    int n = (int)len;
    if (n < 1) {
        plot(mask, (int)x1, (int)y1);
        return;
    }
    float sx = dx / n, sy = dy / n;
    for (int i = 0; i <= n; i++) {
        plot(mask, (int)(x0 + sx * i), (int)(y0 + sy * i));
    }
}

void spark_draw(void)
{
    memset(s_mask, 0, sizeof(s_mask));
    memset(s_mask_boom, 0, sizeof(s_mask_boom));
    for (int i = 0; i < s_n; i++) {
        const spark_t *s = &s_sp[i];
        segment(s->boom ? s_mask_boom : s_mask, s->x - s->vx * STREAK_S, s->y - s->vy * STREAK_S, s->x, s->y);
    }
    if (s_flash > 0) {
        for (int dy = -BOOM_FLASH_PX; dy <= BOOM_FLASH_PX; dy++) {
            for (int dx = -BOOM_FLASH_PX; dx <= BOOM_FLASH_PX; dx++) {
                if (dx * dx + dy * dy <= BOOM_FLASH_PX * BOOM_FLASH_PX) plot(s_mask_boom, s_flash_x + dx, s_flash_y + dy);
            }
        }
    }
}

const uint8_t *spark_row(int y)
{
    return s_mask + y * SPARK_STRIDE;
}

const uint8_t *spark_boom_row(int y)
{
    return s_mask_boom + y * SPARK_STRIDE;
}
