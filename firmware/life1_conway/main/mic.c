#include "mic.h"

#include <math.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MIC_RATE        16000
#define MIC_BLOCK       256         // samples per measurement (16 ms)
#define MIC_GAIN_DB     30.0f
#define FLOOR_RISE      0.002f      // background noise estimate: slow rise per block
#define LEVEL_START_DB  6.0f        // dB above the background where the level starts (first drop ~+7 dB)
#define LEVEL_FULL_DB   28.0f       // dB above the background for level 1
#define FLOOR_MIN_DB    -40.0f      // background estimate never below this: first drop at ~-33 dBFS
#define FLOOR_MAX_DB    -20.0f      // background estimate never above this: loud places still give drops
#define LEVEL_RELEASE   0.92f       // per block decay of the level after a sound
#define STARTUP_BLOCKS  32          // blocks ignored after start (codec power-up click), ~0.5 s

static const char *TAG = "mic";

static esp_codec_dev_handle_t s_dev;
static volatile float s_level;

static void mic_task(void *arg)
{
    static int16_t buf[MIC_BLOCK];
    float floor_db = -60.0f;
    float level = 0;
    float peak_db = -120.0f;
    int blocks = 0;
    while (1) {
        if (esp_codec_dev_read(s_dev, buf, sizeof(buf)) != ESP_CODEC_DEV_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // RMS without the DC offset
        int64_t sum = 0;
        for (int i = 0; i < MIC_BLOCK; i++) sum += buf[i];
        int mean = (int)(sum / MIC_BLOCK);
        int64_t sq = 0;
        for (int i = 0; i < MIC_BLOCK; i++) {
            int v = buf[i] - mean;
            sq += (int64_t)v * v;
        }
        static int startup = STARTUP_BLOCKS;
        if (startup > 0) {
            startup--;
            continue;
        }
        float rms = sqrtf((float)sq / MIC_BLOCK) + 1.0f;
        float db = 20.0f * log10f(rms / 32768.0f);

        // Background noise: follows quiet levels immediately, rises slowly
        if (db < floor_db) floor_db = db;
        else floor_db += (db - floor_db) * FLOOR_RISE;
        float ref_db = floor_db < FLOOR_MIN_DB ? FLOOR_MIN_DB : floor_db > FLOOR_MAX_DB ? FLOOR_MAX_DB : floor_db;

        float x = (db - ref_db - LEVEL_START_DB) / (LEVEL_FULL_DB - LEVEL_START_DB);
        x = x < 0 ? 0 : x > 1 ? 1 : x;
        level = x > level ? x : level * LEVEL_RELEASE;
        s_level = level;

        if (db > peak_db) peak_db = db;
        if (++blocks * MIC_BLOCK >= MIC_RATE * 2) {
            ESP_LOGI(TAG, "peak %.1f dB, background %.1f dB (ref %.1f), level %.2f", peak_db, floor_db, ref_db, level);
            peak_db = -120.0f;
            blocks = 0;
        }
    }
}

float mic_level(void)
{
    return s_level;
}

esp_err_t mic_init(i2c_master_bus_handle_t bus)
{
    // Speaker amplifier off
    gpio_config_t pa = {.pin_bit_mask = 1ULL << AUDIO_PIN_PA, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&pa);
    gpio_set_level(AUDIO_PIN_PA, 0);

    // I2S master, receive only; MCLK = 256 x sample rate for the codec
    i2s_chan_handle_t rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx), TAG, "i2s channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_PIN_MCLK,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_PIN_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "i2s std");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG, "i2s enable");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = AUDIO_I2S_PORT, .rx_handle = rx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    audio_codec_i2c_cfg_t i2c_cfg = {.port = TOUCH_I2C_PORT, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "codec interfaces");

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .use_mclk = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES8311 not found");

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_dev, ESP_FAIL, TAG, "codec device");
    esp_codec_dev_sample_info_t fs = {.sample_rate = MIC_RATE, .channel = 1, .bits_per_sample = 16};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_dev, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "codec open");
    esp_codec_dev_set_in_gain(s_dev, MIC_GAIN_DB);

    BaseType_t ok = xTaskCreatePinnedToCore(mic_task, "mic", 4096, NULL, 6, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "mic task");
    ESP_LOGI(TAG, "microphone ready, %d Hz, gain %.0f dB", MIC_RATE, MIC_GAIN_DB);
    return ESP_OK;
}
