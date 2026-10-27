#include "tilings.h"

#include <stddef.h>

#define DECLARE(ns)                         \
    void ns##_life_init(void);              \
    void ns##_life_deinit(void);            \
    void ns##_life_next_size(void);         \
    void ns##_life_prev_size(void);         \
    int ns##_life_edge_px(void);            \
    int ns##_life_size_idx(void);           \
    void ns##_life_set_size_idx(int idx);   \
    void ns##_life_randomize(void);         \
    void ns##_life_touch(int x, int y);     \
    void ns##_life_update(int64_t now_us);  \
    void ns##_life_render(void);

#define ENTRY(ns, label)                                                                        \
    {label, ns##_life_init, ns##_life_deinit, ns##_life_next_size, ns##_life_prev_size,         \
     ns##_life_edge_px, ns##_life_size_idx, ns##_life_set_size_idx, ns##_life_touch, ns##_life_update, ns##_life_render}

DECLARE(conway)
DECLARE(penrose)
DECLARE(ts1)
DECLARE(ts2)
DECLARE(ts3)
DECLARE(ts4)
DECLARE(ts5)
DECLARE(ts6)
DECLARE(ts7)
DECLARE(ts8)
DECLARE(ts9)
DECLARE(ts10)
DECLARE(ts11)
DECLARE(ts12)

// Conway has a fixed grid: BOOT makes a new seed, as in life1_conway
const tiling_t TILINGS[] = {
    {"life1_conway", conway_life_init, conway_life_deinit, conway_life_randomize, conway_life_randomize, NULL, NULL, NULL,
     conway_life_touch, conway_life_update, conway_life_render},
    ENTRY(penrose, "life1_penrose"),
    ENTRY(ts1, "life1_ts1"),
    ENTRY(ts2, "life1_ts2"),
    ENTRY(ts3, "life1_ts3"),
    ENTRY(ts4, "life1_ts4"),
    ENTRY(ts5, "life1_ts5"),
    ENTRY(ts6, "life1_ts6"),
    ENTRY(ts7, "life1_ts7"),
    ENTRY(ts8, "life1_ts8"),
    ENTRY(ts9, "life1_ts9"),
    ENTRY(ts10, "life1_ts10"),
    ENTRY(ts11, "life1_ts11"),
    ENTRY(ts12, "life1_ts12"),
};
const int TILING_COUNT = sizeof(TILINGS) / sizeof(TILINGS[0]);
