# Pool floor: main/floor.bin when present (made with scripts/make_mask.py, not in git),
# otherwise the placeholder from assets/ copied to the build folder with the same name.
set(FLOOR_BIN "${CMAKE_CURRENT_LIST_DIR}/floor.bin")
# Folder date changes when floor.bin is added or removed: the build re-runs this check
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}")
if(NOT EXISTS "${FLOOR_BIN}")
    set(FLOOR_BIN "${CMAKE_CURRENT_BINARY_DIR}/floor.bin")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/../../../assets/floor_placeholder.bin" "${FLOOR_BIN}" COPYONLY)
    message(STATUS "floor.bin not found, using assets/floor_placeholder.bin")
endif()

idf_component_register(
    SRCS "main.c" "panel.c" "touch_cst820.c" "ripple.c" "wave_pie.S" "mic.c"
    INCLUDE_DIRS "."
    EMBED_FILES "${FLOOR_BIN}"
    REQUIRES driver esp_lcd esp_timer esp_driver_i2s nvs_flash
)
