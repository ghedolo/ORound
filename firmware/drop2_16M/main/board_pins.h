#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.32 pin map

// AMOLED 466x466, CO5300 controller, QSPI
#define LCD_HOST          SPI2_HOST
#define LCD_H_RES         466
#define LCD_V_RES         466
#define LCD_X_OFFSET      6
#define LCD_PIN_CS        10
#define LCD_PIN_PCLK      11
#define LCD_PIN_D0        12
#define LCD_PIN_D1        13
#define LCD_PIN_D2        14
#define LCD_PIN_D3        15
#define LCD_PIN_RST       8
#define LCD_PIN_TE        9
#define LCD_PCLK_HZ       (40 * 1000 * 1000)

// Touch CST820, I2C
#define TOUCH_I2C_PORT    I2C_NUM_0
#define TOUCH_PIN_SDA     47
#define TOUCH_PIN_SCL     48
#define TOUCH_PIN_RST     7
#define TOUCH_PIN_INT     6
#define TOUCH_I2C_ADDR    0x15
#define TOUCH_I2C_HZ      300000

// Power latch: keeps the board on when running from battery
#define PWR_PIN_LATCH     18
#define PWR_PIN_KEY       17

// BOOT button, active low (external pull-up)
#define BOOT_PIN_KEY      0
