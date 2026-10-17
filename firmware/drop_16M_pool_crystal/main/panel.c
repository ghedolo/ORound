#include "panel.h"

#include "board_pins.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define BAND_BYTES (LCD_H_RES * PANEL_BAND_LINES * sizeof(pixel_t))

static const char *TAG = "panel";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_free_bands;
static pixel_t *s_bands[PANEL_BAND_COUNT];
static int s_next_band;

// CO5300 init sequence (from Waveshare demo)
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x77}, 1, 0},                    // RGB888
    {0x35, (uint8_t[]){0x00}, 1, 0},                    // tearing effect on
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},                    // brightness
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},  // columns 6..471
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},  // rows 0..465
    {0x11, (uint8_t[]){0x00}, 0, 100},                  // sleep out
    {0x29, (uint8_t[]){0x00}, 0, 0},                    // display on
};

// QSPI command word: opcode 0x02 (single-line write), then register
static void panel_write_reg(uint8_t reg, uint8_t value)
{
    int cmd = (0x02 << 24) | (reg << 8);
    esp_lcd_panel_io_tx_param(s_io, cmd, &value, 1);
}

void panel_set_brightness(uint8_t level)
{
    panel_write_reg(0x51, level);
}

void panel_off(void)
{
    // Wait until no band transfer is in flight
    for (int i = 0; i < PANEL_BAND_COUNT; i++) {
        panel_band_acquire();
    }
    for (int level = 255; level >= 0; level -= 15) {
        panel_set_brightness((uint8_t)level);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_disp_sleep(s_panel, true);
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_free_bands, &woken);
    return woken == pdTRUE;
}

pixel_t *panel_band_acquire(void)
{
    xSemaphoreTake(s_free_bands, portMAX_DELAY);
    pixel_t *band = s_bands[s_next_band];
    s_next_band = (s_next_band + 1) % PANEL_BAND_COUNT;
    return band;
}

void panel_band_send(pixel_t *band, int y1, int y2)
{
    esp_lcd_panel_draw_bitmap(s_panel, LCD_X_OFFSET, y1, LCD_X_OFFSET + LCD_H_RES, y2 + 1, band);
}

esp_err_t panel_init(void)
{
    s_free_bands = xSemaphoreCreateCounting(PANEL_BAND_COUNT, PANEL_BAND_COUNT);
    ESP_RETURN_ON_FALSE(s_free_bands, ESP_ERR_NO_MEM, TAG, "semaphore");
    for (int i = 0; i < PANEL_BAND_COUNT; i++) {
        s_bands[i] = heap_caps_malloc(BAND_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        ESP_RETURN_ON_FALSE(s_bands[i], ESP_ERR_NO_MEM, TAG, "band buffer");
    }

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_PCLK,
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .max_transfer_sz = BAND_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io), TAG, "panel io");

    sh8601_vendor_config_t vendor_cfg = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 24,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    panel_write_reg(0x36, 0xC0);  // MADCTL: mirror X and Y to match board orientation
    ESP_LOGI(TAG, "panel %dx%d ready", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
