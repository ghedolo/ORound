#include "battery.h"

#include <stdio.h>
#include <string.h>
#include "battery_font.h"
#include "board_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "panel.h"

#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_3     // GPIO4
#define BAT_DIVIDER     2.0f              // R37 200K, R38 200K
#define BAT_SAMPLES     32
#define LINE_GAP        26                // px between the percent and the voltage

static const char *TAG = "battery";
static const pixel_t COLOR = {255, 220, 0};

float battery_volts(void)
{
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t unit_cfg = {.unit_id = BAT_ADC_UNIT};
    if (adc_oneshot_new_unit(&unit_cfg, &adc) != ESP_OK) return 0;
    adc_oneshot_chan_cfg_t ch_cfg = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(adc, BAT_ADC_CHANNEL, &ch_cfg);

    adc_cali_handle_t cali = NULL;
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .chan = BAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali) != ESP_OK) cali = NULL;

    int sum = 0, n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw, mv;
        if (adc_oneshot_read(adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        if (cali) {
            adc_cali_raw_to_voltage(cali, raw, &mv);
        } else {
            mv = raw * 3100 / 4095;
        }
        sum += mv;
        n++;
    }
    if (cali) adc_cali_delete_scheme_curve_fitting(cali);
    adc_oneshot_del_unit(adc);
    float v = n ? sum / (float)n / 1000.0f * BAT_DIVIDER : 0;
    ESP_LOGI(TAG, "%.3f V (%s)", v, cali ? "calibrated" : "not calibrated");
    return v;
}

int battery_percent(float volts)
{
    static const float V[] = {3.27f, 3.61f, 3.69f, 3.71f, 3.73f, 3.75f, 3.77f, 3.79f, 3.80f, 3.82f, 3.84f,
                              3.85f, 3.87f, 3.91f, 3.95f, 3.98f, 4.02f, 4.08f, 4.11f, 4.15f, 4.20f};
    int n = sizeof(V) / sizeof(V[0]);   // V[i] = i * 5 %
    if (volts <= V[0]) return 0;
    if (volts >= V[n - 1]) return 100;
    int i = 0;
    while (volts > V[i + 1]) i++;
    return (int)(5 * (i + (volts - V[i]) / (V[i + 1] - V[i])) + 0.5f);
}

static const bfont_glyph_t *glyph(const bfont_t *f, char c)
{
    const char *p = strchr(f->chars, c);
    return (c && p) ? &f->glyphs[p - f->chars] : NULL;
}

static int text_width(const bfont_t *f, const char *s)
{
    int w = 0;
    for (; *s; s++) {
        const bfont_glyph_t *g = glyph(f, *s);
        w += g ? g->advance : f->space;
    }
    return w;
}

// Text centered horizontally, baseline at y, into an 8 bit alpha frame
static void draw_text(uint8_t *fb, const bfont_t *f, const char *s, int y)
{
    int pen = (LCD_H_RES - text_width(f, s)) / 2;
    for (; *s; s++) {
        const bfont_glyph_t *g = glyph(f, *s);
        if (!g) {
            pen += f->space;
            continue;
        }
        const uint8_t *bm = f->data + g->offset;
        for (int j = 0; j < g->h; j++) {
            int py = y + g->y0 + j;
            if (py < 0 || py >= LCD_V_RES) continue;
            for (int i = 0; i < g->w; i++) {
                int px = pen + g->x0 + i;
                if (px < 0 || px >= LCD_H_RES) continue;
                uint8_t a = bm[j * g->w + i];
                uint8_t *d = &fb[py * LCD_H_RES + px];
                if (a > *d) *d = a;
            }
        }
        pen += g->advance;
    }
}

void battery_show(float volts)
{
    uint8_t *fb = heap_caps_calloc(LCD_H_RES * LCD_V_RES, 1, MALLOC_CAP_SPIRAM);
    if (!fb) return;

    char pct[8], volt[12];
    snprintf(pct, sizeof(pct), "%d%%", battery_percent(volts));
    snprintf(volt, sizeof(volt), "%.2f V", volts);

    // Block (percent cap + gap + voltage cap) centered vertically
    int top = (LCD_V_RES - (BFONT_BIG.cap + LINE_GAP + BFONT_SMALL.cap)) / 2;
    int base_big = top + BFONT_BIG.cap;
    int base_small = base_big + LINE_GAP + BFONT_SMALL.cap;
    draw_text(fb, &BFONT_BIG, pct, base_big);
    draw_text(fb, &BFONT_SMALL, volt, base_small);

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
