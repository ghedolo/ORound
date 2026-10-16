#include "display.h"

#include "board_pins.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "touch_cst820.h"

#define DRAW_BUF_LINES     40
#define DRAW_BUF_SIZE      (LCD_H_RES * DRAW_BUF_LINES * 2)
#define LVGL_TASK_STACK    (8 * 1024)
#define LVGL_TASK_PRIO     2
#define LVGL_TASK_CORE     1
#define LVGL_MAX_DELAY_MS  100
#define LVGL_MIN_DELAY_MS  2

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_lvgl_mutex;

// CO5300 init sequence (from Waveshare demo)
static const sh8601_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},                    // RGB565
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

void display_set_brightness(uint8_t level)
{
    panel_write_reg(0x51, level);
}

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1 + LCD_X_OFFSET, area->y1,
                              area->x2 + LCD_X_OFFSET + 1, area->y2 + 1,
                              px_map);
}

// CO5300 requires even start and odd end coordinates
static void rounder_cb(lv_event_t *e)
{
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static uint32_t tick_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t delay_ms = LVGL_MAX_DELAY_MS;
        if (display_lock(0)) {
            delay_ms = lv_timer_handler();
            display_unlock();
        }
        if (delay_ms > LVGL_MAX_DELAY_MS) delay_ms = LVGL_MAX_DELAY_MS;
        if (delay_ms < LVGL_MIN_DELAY_MS) delay_ms = LVGL_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

bool display_lock(uint32_t timeout_ms)
{
    TickType_t ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, ticks) == pdTRUE;
}

void display_unlock(void)
{
    xSemaphoreGiveRecursive(s_lvgl_mutex);
}

static esp_err_t panel_init(void)
{
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_PIN_PCLK,
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .max_transfer_sz = DRAW_BUF_SIZE,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
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
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh8601(s_io, &panel_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    panel_write_reg(0x36, 0xC0);  // MADCTL: mirror X and Y to match board orientation
    return ESP_OK;
}

esp_err_t display_start(void)
{
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_lvgl_mutex, ESP_ERR_NO_MEM, TAG, "mutex");

    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel_init");
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch_init");

    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    void *buf1 = heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf2 = heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(buf1 && buf2, ESP_ERR_NO_MEM, TAG, "draw buffers");
    lv_display_set_buffers(disp, buf1, buf2, DRAW_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);

    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = on_color_trans_done};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, disp), TAG, "io callbacks");

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, touch_read_cb);

    BaseType_t ok = xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL, LVGL_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "lvgl task");

    ESP_LOGI(TAG, "display %dx%d ready", LCD_H_RES, LCD_V_RES);
    return ESP_OK;
}
