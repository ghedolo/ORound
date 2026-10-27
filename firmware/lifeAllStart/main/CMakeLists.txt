# The tilings are compiled from the life.c of each life1_* experiment (single source), with the public
# functions renamed by the forced include life_ns.h.
set(TILING_NS conway penrose ts1 ts2 ts3 ts4 ts5 ts6 ts7 ts8 ts9 ts10 ts11 ts12)
set(TILING_SRCS)
foreach(ns ${TILING_NS})
    list(APPEND TILING_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/../../life1_${ns}/main/life.c")
endforeach()

idf_component_register(
    SRCS "main.c" "tilings.c" "banner.c" "panel.c" "touch_cst820.c" ${TILING_SRCS}
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer nvs_flash
)

# Source properties are set after the registration: the requirements pass runs this file in script mode and
# returns from idf_component_register
foreach(ns ${TILING_NS})
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../../life1_${ns}/main/life.c" PROPERTIES
        COMPILE_DEFINITIONS "LIFE_NS=${ns}"
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/life_ns.h")
endforeach()
