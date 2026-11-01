# Water ripple series (`drop1_*`)

Interactive water ripple experiments. Touch the screen to drop water; drag to draw a
trail of drops. Waves travel, reflect on the circular edge and fade.

The pool experiments (`drop1_16M_pool_*`) show the floor of a swimming pool: a tiled
grid or an image seen through the water surface, bent by the waves (refraction).

Back to the [main README](../README.md).

## Experiments

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

## Buttons

- BOOT (GPIO0) cycles the color palettes.
- PWR (GPIO17): toggles the microphone in `drop1_16M_pool_listening`; long press powers off in `drop1_16M_pool_tiles`; in `drop1_16M_pool_image` / `drop1_16M_pool_crystal` a short press toggles the microphone and a long press powers off (same in `empty2`, where BOOT has no action yet).

## Previews

Not available yet.

## Microphone

In `drop1_16M_pool_listening`, `drop1_16M_pool_image` and `drop1_16M_pool_crystal` the
ES8311 records 16 kHz mono with 30 dB gain; the RMS level of each 16 ms block is compared
with an estimate of the background noise. The estimate is clamped to -40..-20 dBFS:
in a quiet room the first drop comes at about -33 dBFS, and in loud places continuous
noise raises the reference only up to -20 dBFS, so sounds still make drops.
The serial log prints peak, background and reference level every 2 s.

## How the simulation works

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

## Pool floor images

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
