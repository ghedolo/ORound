#pragma once

// Forced include (-include) for the life.c of each life1_* experiment: the public functions get the prefix
// LIFE_NS (set per source file in CMakeLists.txt), so all tilings link into one firmware.
// Example with LIFE_NS=ts11: life_init -> ts11_life_init.

#define LIFE_CAT2(a, b) a##_##b
#define LIFE_CAT(a, b)  LIFE_CAT2(a, b)

#define life_init       LIFE_CAT(LIFE_NS, life_init)
#define life_deinit     LIFE_CAT(LIFE_NS, life_deinit)
#define life_next_size  LIFE_CAT(LIFE_NS, life_next_size)
#define life_prev_size  LIFE_CAT(LIFE_NS, life_prev_size)
#define life_edge_px    LIFE_CAT(LIFE_NS, life_edge_px)
#define life_size_idx   LIFE_CAT(LIFE_NS, life_size_idx)
#define life_set_size_idx LIFE_CAT(LIFE_NS, life_set_size_idx)
#define life_randomize  LIFE_CAT(LIFE_NS, life_randomize)
#define life_touch      LIFE_CAT(LIFE_NS, life_touch)
#define life_update     LIFE_CAT(LIFE_NS, life_update)
#define life_render     LIFE_CAT(LIFE_NS, life_render)
