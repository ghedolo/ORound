idf_component_register(
    SRCS "main.c" "display.c" "touch_cst820.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer
)
