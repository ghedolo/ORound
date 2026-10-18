# ORound

Interactive water ripple experiments on the round 1.32" AMOLED of the
**Waveshare ESP32-S3-Touch-AMOLED-1.32**. Touch the screen to drop water;
drag to draw a trail of drops. Waves travel, reflect on the circular edge and fade.

The pool experiments (`drop1_16M_pool_*`) show the floor of a swimming pool: a tiled
grid or an image seen through the water surface, bent by the waves (refraction).

## Hardware

- Board: Waveshare ESP32-S3-Touch-AMOLED-1.32 (ESP32-S3-PICO-1-N8R8, 8 MB flash, 8 MB octal PSRAM)
- Display: 466x466 AMOLED, CO5300 controller, QSPI at 40 MHz, driven in RGB888 (24 bit)
- Touch: CST820 on I2C (address 0x15)
- Brightness (all experiments): hold a finger still for 0.8 s, then drag up (brighter) or down (darker) without lifting; the value is saved in NVS and restored at boot
- Buttons: BOOT (GPIO0) cycles the color palettes; PWR (GPIO17) toggles the microphone in `drop1_16M_pool_listening`, long press powers off in `drop1_16M_pool_tiles`; in `drop1_16M_pool_image` / `drop1_16M_pool_crystal` a short press toggles the microphone and a long press powers off (same in `empty2`, where BOOT has no action yet); in `life1_*` BOOT is a new seed (`life1_conway`) or the next tile size (`life1_penrose`), PWR long press powers off
- Docs: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-1.32>

## Firmware experiments

Each experiment is a standalone ESP-IDF project under `firmware/`.

| Folder | Description |
|---|---|
| `drop1_65k` | Direct panel driver (no LVGL), 2D wave equation, slope shading, RGB565 |
| `drop1_16M` | Same in RGB888 (16.7M colors), radial background with ordered dither |
| `drop1_16M_multipalette` | Each tap gets a random crest tint carried by a second wave field; 7 palettes on BOOT/PWR |
| `drop1_16M_grid2x2` | Finer simulation grid (1 cell = 2x2 px), int16 fields, precomputed ring map |
| `drop1_16M_grid2x2_pie` | Wave step in fixed point with ESP32-S3 PIE vector instructions (8 x int16 per instruction) |
| `drop1_16M_pool_tiles` | Pool floor grid deformed by refraction, with tiles in slightly different azure shades (azure palette); long press PWR powers off (battery) or deep sleeps (USB) |
| `drop1_16M_pool_image` | Pool floor from an image: coverage mask drawn with the palette, full color image, or animated GIF (frames switched in time); microphone ripples |
| `drop1_16M_pool_crystal` | Frozen copy of `drop1_16M_pool_image` with a rotating crystal animation on a dark green background (`--anim --step=2 --bg=198919`, 466 px); wider drops and lit crests so waves show on the uniform background |
| `drop1_16M_pool_image_bufalini` | Frozen copy of `drop1_16M_pool_image` with a full color photo as floor (`--rgb`, 514 px; third-party artwork by Paolo Bufalini, `floor.bin` not in the repository); microphone with fixed reference (-55 dBFS, first drop at ~-52 dBFS, background adaptation commented out) |
| `drop1_16M_pool_listening` | Pool floor grid deformed by refraction (4 palettes, ~20 fps) that listens: microphone (ES8311) sound level ripples the water with small random drops; PWR toggles the microphone |
| `empty2` | Empty skeleton for new experiments: solid background, touch, microphone, brightness control; PWR short press toggles the microphone, long press powers off; BOOT free for a new use |
| `life1_conway` | Conway's Game of Life, 24 px cells (19x19 grid) inside the circle, one generation every 3 s; each changing cell fades its color (two-stage filter, slow start and end, ~3 s) starting in a random one of 10 time slots (300 ms apart); newborn orange turning blue with age; touch adds cells, BOOT new seed, automatic new seed on still or empty states; no microphone, PWR long press powers off |
| `life1_penrose` | Game of Life (B3/S23) on a Penrose rhombus tiling (P3) built on the device by deflation; neighbors = tiles sharing a vertex (4..11); tiles without gap, two dark shades for thin/thick rhombi, black border from 18 px; BOOT cycles the tile edge (8, 11, 14, 18, 24, 32, 40, 48, 56, 64 px, shown on the serial log); generation 4 s and fade time constant 1.1 s up to 14 px, longer for larger tiles (x2.4 at 48 px); live colors by age orange, yellow, bright green; no microphone |

### Microphone

In `drop1_16M_pool_listening`, `drop1_16M_pool_image` and `drop1_16M_pool_crystal` the
ES8311 records 16 kHz mono with 30 dB gain; the RMS level of each 16 ms block is compared
with an estimate of the background noise. The estimate is clamped to -40..-20 dBFS:
in a quiet room the first drop comes at about -33 dBFS, and in loud places continuous
noise raises the reference only up to -20 dBFS, so sounds still make drops.
The serial log prints peak, background and reference level every 2 s.

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
- In the image and animation floors the slope also shades the water: slopes facing the
  light (top left) mix toward a light green, the opposite slopes darken.
- The frame is sent in bands of 16 lines with two DMA buffers; both cores render half of each band.

## Build and flash

Requires ESP-IDF v5.5. Adjust the paths in `scripts/idf_env.sh` to your installation.

```
scripts/build.sh firmware/drop1_16M_pool_listening
scripts/flash.sh firmware/drop1_16M_pool_listening            # first /dev/cu.usbmodem* port
scripts/flash.sh firmware/drop1_16M_pool_listening <port> --monitor
```

The serial log prints frames per second and the time of the simulation step and of the render every 2 s.

### Pool floor images

`drop1_16M_pool_image` and `drop1_16M_pool_crystal` embed `main/floor.bin`, produced by
`scripts/make_mask.py` (Pillow, run in the project `.venv`). The file is not in the
repository (third-party images). Without it the build uses the placeholder
`assets/floor_placeholder.bin` (coverage mask made from `assets/PH.png`, 480 px); the
check runs again when `floor.bin` is added or removed. Generate your own, for example:

```
python scripts/make_mask.py assets/PH.png assets/floor_placeholder.bin 480   # the placeholder
python scripts/make_mask.py figure.png firmware/drop1_16M_pool_image/main/floor.bin 400
python scripts/make_mask.py photo.jpg  firmware/drop1_16M_pool_image/main/floor.bin 514 --rgb --crop=x0,y0,x1,y1
python scripts/make_mask.py anim.gif   firmware/drop1_16M_pool_image/main/floor.bin 466 --anim --step=2 --bg=23c423
```

The firmware detects the mode from the file: coverage mask (514x514 bytes), RGB888 image
(514x514x3 bytes) or animation (`FLA2` header, indexed frames, shared palette).

## License

- Code: GPL-3.0-or-later
- Image (including `assets/PH.png` and `assets/floor_placeholder.bin`): CC BY-SA 4.0

## Author

ghedo (luca.ghedini@gmail.com) — 2026

Built with Claude Code by Anthropic.

## Development effort

Figures extracted from the local Claude Code session transcripts of this project.

| Metric | Value |
|---|---|
| First message (UTC) | 2026-10-16 19:12 |
| Last message (UTC) | 2026-10-18 16:36 |
| Calendar span | 3 days |
| Commits | 24 |
| Sessions | 4 |
| Messages (user + assistant) | 764 (150 + 614) |
| Active time | ~6.04 h |

Active time is the sum of the gaps between consecutive messages (user and assistant,
sorted by timestamp) that are at most 5 minutes long. Longer gaps are treated as pauses
and not counted, so the value approximates the time spent actively working, including
build and flash waits shorter than 5 minutes.

| Tokens | Count |
|---|---|
| Input (non-cache) | 1,228 |
| Output | 436,553 |
| Cache write | 1,429,756 |
| Cache read | 106,590,379 |
| Total | 108,457,916 |

Cache read tokens are the conversation context re-sent to the model at every turn and
served from the prompt cache; they are by far the largest share but are billed at a
fraction of the normal input price.

**Caveman mode.** All four sessions ran with Caveman mode active (terse responses).
Average output was 711 tokens per assistant message (436,553 output tokens in total).
No session without Caveman mode exists in this project, so no comparison with
standard mode is available.
