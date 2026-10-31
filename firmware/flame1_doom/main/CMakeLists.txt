idf_component_register(
    SRCS "main.c" "flame.c" "panel.c" "touch_cst820.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer nvs_flash
)
