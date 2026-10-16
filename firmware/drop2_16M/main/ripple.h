#pragma once

#include <stdbool.h>
#include "esp_err.h"

esp_err_t ripple_init(void);

// Adds a drop at display coordinates (x, y). With splash = true the impact is
// followed by smaller rebounds that create trailing secondary rings.
// tint 0..1 selects the crest color between the palette crest and the alternate one.
void ripple_drop(int x, int y, int strength, bool splash, float tint);

// Advances the water simulation by one step.
void ripple_step(void);

// Renders the water surface and sends it to the panel.
void ripple_render(void);
