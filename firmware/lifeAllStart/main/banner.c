#include "banner.h"

#include <math.h>
#include <string.h>
#include "banner_font.h"
#include "board_pins.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "panel.h"

#define MAX_LINES      3
#define MAX_WORD       16
#define LINE_GAP       30                   // empty space between lines in px
#define CAP_PX         62                   // height of digits and tall letters in px (font size 84)
#define WAVE_AMP_MIN   10.0f                // main wave amplitude in px
#define WAVE_AMP_MAX   22.0f
#define WAVE_LEN_MIN   300.0f               // main wave length in px
#define WAVE_LEN_MAX   520.0f
#define RIPPLE_AMP_MIN 2.0f                 // second, shorter wave
#define RIPPLE_AMP_MAX 6.0f
#define RIPPLE_LEN_MIN 150.0f
#define RIPPLE_LEN_MAX 240.0f
#define DESCENT_PX     18                   // below the baseline (p, y)
#define SPREAD         0.7f                 // advance growth per unit of CAP_PX * curvature in valleys

static const pixel_t COLOR = {255, 140, 30};

typedef struct {
    float base;                             // baseline y at rest
    float a1, k1, p1;                       // main wave: amplitude, 2*pi/length, phase
    float a2, k2, p2;                       // ripple
} wave_t;

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)esp_random() / (float)UINT32_MAX;
}

static float wave_y(const wave_t *w, float x)
{
    return w->base + w->a1 * sinf(w->k1 * x + w->p1) + w->a2 * sinf(w->k2 * x + w->p2);
}

static float wave_slope(const wave_t *w, float x)
{
    return w->a1 * w->k1 * cosf(w->k1 * x + w->p1) + w->a2 * w->k2 * cosf(w->k2 * x + w->p2);
}

static const banner_glyph_t *glyph(char c)
{
    const char *p = strchr(BANNER_FONT_CHARS, c);
    return (c && p) ? &BANNER_GLYPHS[p - BANNER_FONT_CHARS] : NULL;
}

static int advance(char c)
{
    const banner_glyph_t *g = glyph(c);
    return g ? g->advance : BANNER_FONT_SIZE / 3;
}

// Bilinear alpha of a glyph bitmap at (bx, by), 0 outside
static int sample(const banner_glyph_t *g, float bx, float by)
{
    bx -= 0.5f;
    by -= 0.5f;
    int ix = (int)floorf(bx), iy = (int)floorf(by);
    float fx = bx - ix, fy = by - iy;
    const uint8_t *bm = BANNER_FONT_DATA + g->offset;
    float a = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int x = ix + i, y = iy + j;
            if (x < 0 || y < 0 || x >= g->w || y >= g->h) continue;
            a += bm[y * g->w + x] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
        }
    }
    return (int)(a + 0.5f);
}

// Glyph rotated by the baseline slope; (px, py) is the baseline point under the middle of its advance
static void draw_glyph(uint8_t *fb, const banner_glyph_t *g, float px, float py, float angle)
{
    float c = cosf(angle), s = sinf(angle);
    float half = g->advance / 2.0f;
    int r = (int)ceilf(hypotf(half + g->w, BANNER_FONT_SIZE)) + 1;
    int xa = (int)px - r, xb = (int)px + r, ya = (int)py - r, yb = (int)py + r;
    if (xa < 0) xa = 0;
    if (ya < 0) ya = 0;
    if (xb >= LCD_H_RES) xb = LCD_H_RES - 1;
    if (yb >= LCD_V_RES) yb = LCD_V_RES - 1;
    for (int y = ya; y <= yb; y++) {
        for (int x = xa; x <= xb; x++) {
            float dx = x + 0.5f - px, dy = y + 0.5f - py;
            float u = dx * c + dy * s;      // along the baseline
            float v = -dx * s + dy * c;     // across, down positive
            int a = sample(g, u + half - g->x0, v - g->y0);
            uint8_t *d = &fb[y * LCD_H_RES + x];
            if (a > *d) *d = a > 255 ? 255 : a;
        }
    }
}

typedef struct {
    const char *text;
    wave_t w;
    float arc[LCD_H_RES];               // arc length along the baseline from x = 0, one entry per px
    float start;                        // arc length of the first letter
    float x0, x1;                       // horizontal span of the text
} line_t;

static float wave_curve(const wave_t *w, float x)
{
    return -w->a1 * w->k1 * w->k1 * sinf(w->k1 * x + w->p1) - w->a2 * w->k2 * w->k2 * sinf(w->k2 * x + w->p2);
}

static float arc_to_x(const line_t *l, float s)
{
    int x = 0;
    while (x < LCD_H_RES - 2 && l->arc[x + 1] < s) x++;
    return x + (s - l->arc[x]) / (l->arc[x + 1] - l->arc[x]);
}

// Lays out the text from arc length start, draws it when fb is not NULL; returns the arc length at the end.
// Where the baseline bends into a valley the letter tops converge, so the advance grows with the curvature.
static float line_place(line_t *l, float start, uint8_t *fb)
{
    float pen = start;
    l->x0 = arc_to_x(l, start);
    for (const char *p = l->text; *p; p++) {
        float x = arc_to_x(l, pen);
        float m = wave_slope(&l->w, x);
        float k = -wave_curve(&l->w, x) / powf(1 + m * m, 1.5f);
        float adv = advance(*p) * (1 + SPREAD * CAP_PX * fmaxf(0, k));
        float gx = arc_to_x(l, pen + adv / 2);
        pen += adv;
        const banner_glyph_t *g = glyph(*p);
        if (g && fb) draw_glyph(fb, g, gx, wave_y(&l->w, gx), atanf(wave_slope(&l->w, gx)));
    }
    l->x1 = arc_to_x(l, pen);
    return pen;
}

// Random wave, then the start that centers the text horizontally (two passes: the advance depends on the place)
static void line_init(line_t *l, const char *text)
{
    l->text = text;
    l->w = (wave_t){
        .a1 = frand(WAVE_AMP_MIN, WAVE_AMP_MAX) * (esp_random() & 1 ? 1 : -1),
        .k1 = 2 * (float)M_PI / frand(WAVE_LEN_MIN, WAVE_LEN_MAX),
        .p1 = frand(0, 2 * (float)M_PI),
        .a2 = frand(RIPPLE_AMP_MIN, RIPPLE_AMP_MAX),
        .k2 = 2 * (float)M_PI / frand(RIPPLE_LEN_MIN, RIPPLE_LEN_MAX),
        .p2 = frand(0, 2 * (float)M_PI),
    };
    l->arc[0] = 0;
    for (int x = 1; x < LCD_H_RES; x++) {
        float m = wave_slope(&l->w, x - 0.5f);
        l->arc[x] = l->arc[x - 1] + sqrtf(1 + m * m);
    }
    float mid = l->arc[LCD_H_RES / 2];
    int total = 0;
    for (const char *p = text; *p; p++) total += advance(*p);
    float len = line_place(l, mid - total / 2.0f, NULL) - (mid - total / 2.0f);
    l->start = mid - len / 2;
    line_place(l, l->start, NULL);
}

void banner_show(const char *name)
{
    uint8_t *fb = heap_caps_calloc(LCD_H_RES * LCD_V_RES, 1, MALLOC_CAP_SPIRAM);
    line_t *lines = heap_caps_malloc(sizeof(line_t) * MAX_LINES, MALLOC_CAP_SPIRAM);
    if (!fb || !lines) {
        heap_caps_free(fb);
        heap_caps_free(lines);
        return;
    }

    char words[MAX_LINES][MAX_WORD];
    int n = 0;
    for (const char *p = name; *p && n < MAX_LINES;) {
        int len = strcspn(p, "_");
        if (len > MAX_WORD - 1) len = MAX_WORD - 1;
        memcpy(words[n], p, len);
        words[n][len] = 0;
        line_init(&lines[n], words[n]);
        n++;
        p += strcspn(p, "_");
        if (*p) p++;
    }

    // Each line at least CAP_PX + LINE_GAP below the previous one at every x of both texts
    for (int i = 0; i < n; i++) {
        lines[i].w.base = 0;
        if (i == 0) continue;
        line_t *a = &lines[i - 1], *b = &lines[i];
        int xa = (int)fminf(a->x0, b->x0), xb = (int)ceilf(fmaxf(a->x1, b->x1));
        float d = 1e9f;
        for (int x = xa; x <= xb; x++) d = fminf(d, wave_y(&b->w, x) - wave_y(&a->w, x));
        b->w.base = CAP_PX + LINE_GAP - d;
    }

    // Block centered between the highest letter top and the lowest descender
    float top = 1e9f, bottom = -1e9f;
    for (int i = 0; i < n; i++) {
        for (int x = (int)lines[i].x0; x <= (int)ceilf(lines[i].x1); x++) {
            float y = wave_y(&lines[i].w, x);
            top = fminf(top, y - CAP_PX);
            bottom = fmaxf(bottom, y + DESCENT_PX);
        }
    }
    float shift = LCD_V_RES / 2.0f - (top + bottom) / 2;
    for (int i = 0; i < n; i++) {
        lines[i].w.base += shift;
        line_place(&lines[i], lines[i].start, fb);
    }
    heap_caps_free(lines);

    for (int y0 = 0; y0 < LCD_V_RES; y0 += PANEL_BAND_LINES) {
        int y1 = y0 + PANEL_BAND_LINES - 1;
        if (y1 >= LCD_V_RES) y1 = LCD_V_RES - 1;
        pixel_t *band = panel_band_acquire();
        const uint8_t *src = fb + y0 * LCD_H_RES;
        for (int i = 0; i < (y1 - y0 + 1) * LCD_H_RES; i++) {
            int a = src[i];
            band[i] = (pixel_t){COLOR.r * a / 255, COLOR.g * a / 255, COLOR.b * a / 255};
        }
        panel_band_send(band, y0, y1);
    }
    heap_caps_free(fb);
}
