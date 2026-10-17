idf_component_register(
    SRCS "main.c" "panel.c" "touch_cst820.c" "ripple.c" "wave_pie.S" "mic.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer esp_driver_i2s
)
