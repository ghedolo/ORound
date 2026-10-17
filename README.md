# ORound

Interactive water ripple experiments on the round 1.32" AMOLED of the
**Waveshare ESP32-S3-Touch-AMOLED-1.32**. Touch the screen to drop water;
drag to draw a trail of drops. Waves travel, reflect on the circular edge and fade.

The latest experiment, `drop_16M_pool`, shows the floor of a swimming pool: a tiled
grid seen through the water surface, bent by the waves (refraction).

## Hardware

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.32 (ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB octal PSRAM)
- Display: 466x466 AMOLED, CO5300 controller, QSPI at 40 MHz, driven in RGB888 (24 bit)
- Touch: CST820 on I2C (address 0x15)
- Buttons: BOOT (GPIO0) cycles the color palettes; PWR (GPIO17) toggles the microphone in `drop_16M_pool_listening`, long press powers off in `drop_16M_pool_tiles`; in `drop_16M_pool_image` / `drop_16M_pool_crystal` a short press toggles the microphone and a long press powers off
- Docs: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.32>

## Firmware experiments

Each experiment is a standalone ESP-IDF project under `firmware/`.

| Folder | Description |
|---|---|
| `01_lvgl_touch` | Baseline: LVGL 9.3 demo, display and touch bring-up |
| `drop1` | Direct panel driver (no LVGL), 2D wave equation, slope shading, RGB565 |
| `drop1_16M` | Same in RGB888 (16.7M colors), radial background with ordered dither |
| `drop2_16M` | Each tap gets a random crest tint carried by a second wave field; 7 palettes on BOOT/PWR |
| `drop_16M_grid2x2` | Finer simulation grid (1 cell = 2x2 px), int16 fields, precomputed ring map |
| `drop_16M_grid2x2_pie` | Wave step in fixed point with ESP32-S3 PIE vector instructions (8 x int16 per instruction) |
| `drop_16M_pool` | Pool floor grid deformed by refraction, 4 palettes, ~20 fps |
| `drop_16M_pool_tiles` | Pool with tiles in slightly different azure shades (azure palette); long press PWR powers off (battery) or deep sleeps (USB) |
| `drop_16M_pool_image` | Pool floor from an image: coverage mask drawn with the palette, full color image, or animated GIF (frames switched in time); microphone ripples |
| `drop_16M_pool_crystal` | Frozen copy of `drop_16M_pool_image` with a rotating crystal animation on a dark green background (`--anim --step=2 --bg=198919`, 466 px) |
| `drop_16M_pool_listening` | Pool that listens: microphone (ES8311) sound level ripples the water with small random drops; PWR toggles the microphone |

### How the simulation works

- Height field on a 236x236 grid (1 cell = 2x2 pixels), two buffers (current, previous).
- Wave equation with viscosity and damping, written as one weighted sum per cell:
  `new = cc*c + cp*p + cs*(left + right + up + down)` with Q15 coefficients.
- The step runs in `wave_pie.S` with PIE instructions on both cores (rows split in two halves);
  a boot self test compares it with a scalar C reference.
- Cells outside the circle stay 0, so waves reflect on the round edge.
- Rendering: bilinear interpolation of the per-cell slope to every pixel. In the pool
  version each pixel samples the grid at its position shifted by the local slope
  (x and y separately, 1/16 px resolution). Grid lines have a full core plus soft edges
  blended in emitted light (gamma 2.2).
- The frame is sent in bands of 16 lines with two DMA buffers; both cores render half of each band.

## Build and flash

Requires ESP-IDF v5.5. Adjust the paths in `scripts/idf_env.sh` to your installation.

```
scripts/build.sh firmware/drop_16M_pool
scripts/flash.sh firmware/drop_16M_pool            # first /dev/cu.usbmodem* port
scripts/flash.sh firmware/drop_16M_pool <port> --monitor
```

The serial log prints frames per second and the time of the simulation step and of the render every 2 s.

### Pool floor images

`drop_16M_pool_image` and `drop_16M_pool_crystal` embed `main/floor.bin`, produced by
`scripts/make_mask.py` (Pillow, run in the project `.venv`). The file is not in the
repository (third-party images); generate it before building, for example:

```
python scripts/make_mask.py figure.png firmware/drop_16M_pool_image/main/floor.bin 400
python scripts/make_mask.py photo.jpg  firmware/drop_16M_pool_image/main/floor.bin 514 --rgb --crop=x0,y0,x1,y1
python scripts/make_mask.py anim.gif   firmware/drop_16M_pool_image/main/floor.bin 466 --anim --step=2 --bg=23c423
```

The firmware detects the mode from the file: coverage mask (514x514 bytes), RGB888 image
(514x514x3 bytes) or animation (`FLA2` header, indexed frames, shared palette).

## License

- Code: GPL-3.0-or-later
- Image: CC BY-SA 4.0

## Author

ghedo (luca.ghedini@gmail.com) — 2026

Built with Claude Code by Anthropic.

## Development effort

Figures extracted from the local Claude Code session transcripts of this project.

| Metric | Value |
|---|---|
| First message (UTC) | 2026-10-16 19:12 |
| Last message (UTC) | 2026-10-17 21:58 |
| Calendar span | 2 days |
| Commits | 15 |
| Sessions | 2 |
| Messages (user + assistant) | 515 (102 + 413) |
| Active time | ~3.98 h |

Active time is the sum of the gaps between consecutive messages (user and assistant,
sorted by timestamp) that are at most 5 minutes long. Longer gaps are treated as pauses
and not counted, so the value approximates the time spent actively working, including
build and flash waits shorter than 5 minutes.

| Tokens | Count |
|---|---|
| Input (non-cache) | 826 |
| Output | 321,006 |
| Cache write | 1,124,172 |
| Cache read | 80,084,173 |
| Total | 81,530,177 |

Cache read tokens are the conversation context re-sent to the model at every turn and
served from the prompt cache; they are by far the largest share but are billed at a
fraction of the normal input price.

**Caveman mode.** Both sessions ran with Caveman mode active (terse responses).
Average output was 777 tokens per assistant message (321,006 output tokens in total).
No session without Caveman mode exists in this project, so no comparison with
standard mode is available.
