#include "touch_cst820.h"

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CST820_REG_FINGER_NUM 0x02
#define I2C_TIMEOUT_MS        50

static const char *TAG = "touch";

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

static void touch_reset(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << TOUCH_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(TOUCH_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(TOUCH_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = TOUCH_PIN_SDA,
        .scl_io_num = TOUCH_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    touch_reset();

    esp_err_t err = i2c_master_probe(s_bus, TOUCH_I2C_ADDR, I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST820 not answering at 0x%02x (%s)", TOUCH_I2C_ADDR, esp_err_to_name(err));
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TOUCH_I2C_ADDR,
        .scl_speed_hz = TOUCH_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "i2c dev");
    ESP_LOGI(TAG, "CST820 ready");
    return ESP_OK;
}

i2c_master_bus_handle_t touch_bus(void)
{
    return s_bus;
}

bool touch_read(uint16_t *x, uint16_t *y)
{
    // 0x02: finger count, 0x03..0x06: X high/low, Y high/low
    uint8_t reg = CST820_REG_FINGER_NUM;
    uint8_t buf[5] = {0};
    if (i2c_master_transmit_receive(s_dev, &reg, 1, buf, sizeof(buf), I2C_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    uint8_t fingers = buf[0];
    uint8_t event = buf[1] >> 6;  // 0 = down, 1 = up, 2 = contact
    if (fingers == 0 || event == 1) {
        return false;
    }
    uint16_t raw_x = ((buf[1] & 0x0F) << 8) | buf[2];
    uint16_t raw_y = ((buf[3] & 0x0F) << 8) | buf[4];

    // Panel is mounted rotated by 180 degrees relative to the display
    int px = LCD_H_RES - raw_x;
    int py = LCD_V_RES - raw_y;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px > LCD_H_RES - 1) px = LCD_H_RES - 1;
    if (py > LCD_V_RES - 1) py = LCD_V_RES - 1;
    *x = px;
    *y = py;
    return true;
}
