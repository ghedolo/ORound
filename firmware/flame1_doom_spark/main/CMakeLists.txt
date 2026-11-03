idf_component_register(
    SRCS "main.c" "flame.c" "spark.c" "panel.c" "touch_cst820.c" "mic.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_driver_i2s esp_driver_usb_serial_jtag esp_lcd esp_timer nvs_flash
)
