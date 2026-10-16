cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Round panel: the default bottom-right corner of the perf monitor is outside the visible area
idf_build_set_property(COMPILE_OPTIONS "-DLV_USE_PERF_MONITOR_POS=LV_ALIGN_BOTTOM_MID" APPEND)
project(lvgl_touch)
