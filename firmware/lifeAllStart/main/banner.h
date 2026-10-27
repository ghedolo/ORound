#pragma once

// Full screen title on black: one line per '_' separated word of name, large orange letters along a random wavy
// baseline. Draws one frame and returns; the display keeps it until the next frame.
void banner_show(const char *name);
