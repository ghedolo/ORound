#!/usr/bin/env python3
"""Preview images (466x466 PNG) of the flame1_* experiments, same algorithm and palettes as the firmware.

Usage: python scripts/make_flame_previews.py [out_dir]   (default assets/previews)

The Doom fire step is vectorized per row (the firmware goes column by column, so a
cell shifted to a column not yet processed can move twice in one step; not reproduced).
The sub-cell outward shift of the radial versions is not drawn (it only moves the
image between two steps). Fixed RNG seed.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

RES = 466
RADIUS = RES // 2
LEVELS = 37
STEPS = 300

DOOM_RGB = [
    (0x07, 0x07, 0x07), (0x1F, 0x07, 0x07), (0x2F, 0x0F, 0x07), (0x47, 0x0F, 0x07),
    (0x57, 0x17, 0x07), (0x67, 0x1F, 0x07), (0x77, 0x1F, 0x07), (0x8F, 0x27, 0x07),
    (0x9F, 0x2F, 0x07), (0xAF, 0x3F, 0x07), (0xBF, 0x47, 0x07), (0xC7, 0x47, 0x07),
    (0xDF, 0x4F, 0x07), (0xDF, 0x57, 0x07), (0xDF, 0x57, 0x07), (0xD7, 0x5F, 0x07),
    (0xD7, 0x5F, 0x07), (0xD7, 0x67, 0x0F), (0xCF, 0x6F, 0x0F), (0xCF, 0x77, 0x0F),
    (0xCF, 0x7F, 0x0F), (0xCF, 0x87, 0x17), (0xC7, 0x87, 0x17), (0xC7, 0x8F, 0x17),
    (0xC7, 0x97, 0x1F), (0xBF, 0x9F, 0x1F), (0xBF, 0x9F, 0x1F), (0xBF, 0xA7, 0x27),
    (0xBF, 0xA7, 0x27), (0xBF, 0xAF, 0x2F), (0xB7, 0xAF, 0x2F), (0xB7, 0xB7, 0x2F),
    (0xB7, 0xB7, 0x37), (0xCF, 0xCF, 0x6F), (0xDF, 0xDF, 0x9F), (0xEF, 0xEF, 0xC7),
    (0xFF, 0xFF, 0xFF),
]

# Color stops (level, r, g, b) of the flame1_doom_rad_mv palettes, as in flame.c
PALETTE_STOPS = {
    "green": [(0, 0x07, 0x07, 0x07), (4, 0x07, 0x2F, 0x07), (12, 0x1F, 0xBF, 0x1F), (20, 0x5F, 0xCF, 0x3F),
              (30, 0xFF, 0x8F, 0xCF), (33, 0xFF, 0xBF, 0xE7), (36, 0xFF, 0xFF, 0xFF)],
    "purple": [(0, 0x07, 0x07, 0x07), (4, 0x2F, 0x07, 0x3F), (12, 0x8F, 0x1F, 0xCF), (20, 0x9F, 0x3F, 0xEF),
               (30, 0x5F, 0xBF, 0xFF), (33, 0x9F, 0xDF, 0xFF), (36, 0xFF, 0xFF, 0xFF)],
    "turquoise": [(0, 0x07, 0x07, 0x07), (4, 0x07, 0x2F, 0x2F), (12, 0x0F, 0xAF, 0xAF), (20, 0x1F, 0xCF, 0xBF),
                  (30, 0x7F, 0xEF, 0x4F), (33, 0xBF, 0xFF, 0x9F), (36, 0xFF, 0xFF, 0xFF)],
}


def palette(name):
    if name == "doom":
        return np.array(DOOM_RGB, np.uint8)
    stops = PALETTE_STOPS[name]
    out = []
    for i in range(LEVELS):
        k = 0
        while k < len(stops) - 2 and i > stops[k + 1][0]:
            k += 1
        a, b = stops[k], stops[k + 1]
        span, t = b[0] - a[0], i - a[0]
        out.append(tuple(a[c] + (b[c] - a[c]) * t // span for c in (1, 2, 3)))
    return np.array(out, np.uint8)


def decay_params(m):
    """Loss k levels with probability m / k, k = smallest integer >= 2m (flame1_doom_rad)."""
    k = max(1, math.ceil(2 * m))
    return k, min(1.0, m / k)


def fire_steps(f, rng, steps, shifts, shift_p, k, p, source_on=True, source_row=-1):
    """Doom fire on grid f (rows: 0 = far end, last = source), columns wrap."""
    h, w = f.shape
    cols = np.arange(w)
    for _ in range(steps):
        for y in range(1, h):
            v = f[y].astype(np.int16)
            dx = rng.choice(shifts, size=w, p=shift_p)
            dec = (rng.random(w) < p) * k
            nv = np.maximum(v - dec, 0).astype(np.uint8)
            live = v > 0
            row = f[y - 1]
            row[cols[~live]] = 0
            row[(cols[live] + dx[live]) % w] = nv[live]
        if not source_on:
            r = f[source_row].astype(np.int16) - rng.integers(0, 4, w)
            f[source_row] = np.maximum(r, 0).astype(np.uint8)
    return f


def circle_mask():
    y, x = np.mgrid[0:RES, 0:RES]
    c = RADIUS - 0.5
    return (x - c) ** 2 + (y - c) ** 2 <= RADIUS ** 2


def save(img, path):
    img[~circle_mask()] = 0
    Image.fromarray(img).save(path)
    print("wrote", path)


def doom_linear(out_dir, rng, scale=3):
    """flame1_doom: bottom row source, shift -2..+1, decay p = 36 / (0.7 * h)."""
    w = h = (RES + scale - 1) // scale
    f = np.zeros((h, w), np.uint8)
    f[-1] = LEVELS - 1
    p = min(1.0, (LEVELS - 1) / (0.70 * h))
    fire_steps(f, rng, STEPS, [1, 0, -1, -2], None, 1, p)
    idx = f.repeat(scale, 0).repeat(scale, 1)[:RES, :RES]
    save(palette("doom")[idx], os.path.join(out_dir, "flame1_doom.png"))


def polar_lut(scale, w, h):
    """Offset (dy, dx) -> (row, column) of the polar grid, row -1 outside."""
    y, x = np.mgrid[0:2 * RADIUS, 0:2 * RADIUS]
    c = RADIUS - 0.5
    dx, dy = x - c, y - c
    ri = (np.sqrt(dx * dx + dy * dy) / scale).astype(int)
    ai = ((np.arctan2(dy, dx) + math.pi) * w / (2 * math.pi)).astype(int).clip(0, w - 1)
    row = np.where(ri < h, h - 1 - ri, -1)
    return row, ai


def polar_grid(scale):
    w = int(2 * math.pi * RADIUS / scale + 0.5)
    h = RADIUS // scale + 1
    return w, h


def radial_fire(rng, scale, steps, source_on=True):
    w, h = polar_grid(scale)
    f = np.zeros((h, w), np.uint8)
    f[-1] = LEVELS - 1
    k, p = decay_params((LEVELS - 1) / (0.70 * h))
    fire_steps(f, rng, steps, [-1, 0, 0, 1], None, k, p)
    if not source_on:
        fire_steps(f, rng, 30, [-1, 0, 0, 1], None, k, p, source_on=False)
    return f


def render_polar(fires, scale, pal):
    """fires: list of (grid, cx, cy); value per pixel = max over fires."""
    w, h = polar_grid(scale)
    row, ai = polar_lut(scale, w, h)
    val = np.zeros((RES, RES), np.uint8)
    for f, cx, cy in fires:
        y0, x0 = cy - RADIUS, cx - RADIUS                 # display position of LUT (0, 0)
        ys, xs = max(0, y0), max(0, x0)
        ye, xe = min(RES, y0 + 2 * RADIUS), min(RES, x0 + 2 * RADIUS)
        r = row[ys - y0:ye - y0, xs - x0:xe - x0]
        a = ai[ys - y0:ye - y0, xs - x0:xe - x0]
        v = np.where(r >= 0, f[r.clip(0), a], 0).astype(np.uint8)
        val[ys:ye, xs:xe] = np.maximum(val[ys:ye, xs:xe], v)
    return pal[val]


def doom_radial(out_dir, rng, scale=3):
    f = radial_fire(rng, scale, STEPS)
    save(render_polar([(f, RADIUS, RADIUS)], scale, palette("doom")),
         os.path.join(out_dir, "flame1_doom_rad.png"))


def doom_radial_mv(out_dir, rng, scale=3):
    """One preview per palette; the fire moved off the center, the previous one dying."""
    spots = {"doom": (300, 170), "green": (170, 290), "purple": (290, 300), "turquoise": (180, 170)}
    for name, (cx, cy) in spots.items():
        old = radial_fire(rng, scale, STEPS, source_on=False)
        new = radial_fire(rng, scale, 40)
        img = render_polar([(old, RADIUS, RADIUS), (new, cx, cy)], scale, palette(name))
        save(img, os.path.join(out_dir, f"flame1_doom_rad_mv_{name}.png"))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "assets/previews"
    os.makedirs(out_dir, exist_ok=True)
    rng = np.random.default_rng(7)
    doom_linear(out_dir, rng)
    doom_radial(out_dir, rng)
    doom_radial_mv(out_dir, rng)


if __name__ == "__main__":
    main()
