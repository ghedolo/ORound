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
    # flame1_doom_xong: 25 colors of the Xong logo, sorted by CIELAB L* (blended) and in image order
    "xong_lab": [(0, 0x07, 0x07, 0x07), (1, 0x19, 0x2C, 0x71), (2, 0x3B, 0x3A, 0x34), (4, 0x54, 0x47, 0x3F),
        (5, 0x01, 0x4B, 0x94), (7, 0x51, 0x48, 0x75), (8, 0xB0, 0x16, 0x22), (9, 0xC8, 0x32, 0x62),
        (11, 0xD6, 0x32, 0x19), (12, 0x01, 0x86, 0x35), (14, 0x67, 0x79, 0x85), (15, 0x01, 0x89, 0x49),
        (17, 0x01, 0x86, 0x95), (18, 0xCC, 0x66, 0x4D), (19, 0x94, 0x82, 0x78), (21, 0x00, 0x9A, 0xD6),
        (22, 0xAA, 0x99, 0x91), (24, 0xC0, 0x99, 0x30), (25, 0xC5, 0x9A, 0x31), (27, 0xD2, 0xA5, 0x92),
        (28, 0xC0, 0xB1, 0xAA), (29, 0xB0, 0xC3, 0x1C), (31, 0xBC, 0xBE, 0x8C), (32, 0xEC, 0xC9, 0x63),
        (34, 0xCD, 0xD5, 0xD8), (35, 0xD8, 0xDF, 0xAC), (36, 0xFF, 0xFF, 0xFF)],
    "xong_image": [(0, 0x07, 0x07, 0x07), (1, 0xC8, 0x32, 0x62), (2, 0xC8, 0x32, 0x62),
        (3, 0xC0, 0x99, 0x30), (4, 0x01, 0x86, 0x95), (5, 0x01, 0x86, 0x95), (6, 0x01, 0x89, 0x49),
        (7, 0x19, 0x2C, 0x71), (8, 0x01, 0x4B, 0x94), (9, 0x01, 0x4B, 0x94), (10, 0xCC, 0x66, 0x4D),
        (11, 0xD6, 0x32, 0x19), (12, 0xD6, 0x32, 0x19), (13, 0xD8, 0xDF, 0xAC), (14, 0xB0, 0xC3, 0x1C),
        (15, 0x00, 0x9A, 0xD6), (16, 0x00, 0x9A, 0xD6), (17, 0xB0, 0x16, 0x22), (18, 0xC5, 0x9A, 0x31),
        (19, 0xC5, 0x9A, 0x31), (20, 0x01, 0x86, 0x35), (21, 0xD2, 0xA5, 0x92), (22, 0x67, 0x79, 0x85),
        (23, 0x67, 0x79, 0x85), (24, 0xEC, 0xC9, 0x63), (25, 0xCD, 0xD5, 0xD8), (26, 0xCD, 0xD5, 0xD8),
        (27, 0xBC, 0xBE, 0x8C), (28, 0xC0, 0xB1, 0xAA), (29, 0xAA, 0x99, 0x91), (30, 0xAA, 0x99, 0x91),
        (31, 0x94, 0x82, 0x78), (32, 0x51, 0x48, 0x75), (33, 0x51, 0x48, 0x75), (34, 0x54, 0x47, 0x3F),
        (35, 0x3B, 0x3A, 0x34), (36, 0xFF, 0xFF, 0xFF)],
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


def radial_fire(rng, scale, steps, source_on=True, off_steps=30):
    w, h = polar_grid(scale)
    f = np.zeros((h, w), np.uint8)
    f[-1] = LEVELS - 1
    k, p = decay_params((LEVELS - 1) / (0.70 * h))
    fire_steps(f, rng, steps, [-1, 0, 0, 1], None, k, p)
    if not source_on:
        fire_steps(f, rng, off_steps, [-1, 0, 0, 1], None, k, p, source_on=False)
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
    """One preview per palette: the dying fire on one side, the new fire (just started) on the other."""
    spots = {"doom": ((150, 150), (330, 320)), "green": ((320, 140), (150, 320)),
             "purple": ((140, 300), (320, 160)), "turquoise": ((310, 320), (150, 140))}
    for name, (old_c, new_c) in spots.items():
        old = radial_fire(rng, scale, STEPS, source_on=False, off_steps=12)
        new = radial_fire(rng, scale, 25)
        img = render_polar([(old, *old_c), (new, *new_c)], scale, palette(name))
        save(img, os.path.join(out_dir, f"flame1_doom_rad_mv_{name}.png"))


def doom_xong(out_dir, rng, scale=3):
    """One preview per flame1_doom_xong palette, fire at the center."""
    for name in ("xong_lab", "xong_image"):
        f = radial_fire(rng, scale, STEPS)
        save(render_polar([(f, RADIUS, RADIUS)], scale, palette(name)),
             os.path.join(out_dir, f"flame1_doom_{name}.png"))


def doom_spark(out_dir, rng, scale=3):
    """flame1_doom_spark: black and white fire (white DD), flickering source, welding sparks
    (white DD) and one explosion off center (white FF), same constants as spark.c."""
    w, h = polar_grid(scale)
    f = np.zeros((h, w), np.uint8)
    k, p = decay_params((LEVELS - 1) / (0.70 * h))
    for _ in range(STEPS):
        # Source flicker: 36 - (common 0..8 + per cell 0..6 + 1/6 chance of 0..24)
        loss = rng.integers(0, 9) + rng.integers(0, 7, w) + (rng.integers(0, 6, w) == 0) * rng.integers(0, 25, w)
        f[-1] = np.maximum(LEVELS - 1 - loss, 0).astype(np.uint8)
        fire_steps(f, rng, 1, [-1, 0, 0, 1], None, k, p)
    luma = np.array(DOOM_RGB, float) @ np.array([0.299, 0.587, 0.114])
    pal = np.where((luma + 0.5 >= 128)[:, None], 0xDD, 0).repeat(3, 1).astype(np.uint8)
    img = render_polar([(f, RADIUS, RADIUS)], scale, pal)

    # Sparks: 60 Hz simulation for 1.5 s, explosion 0.35 s before the end
    dt, t_end, t_boom = 1 / 60, 1.5, 1.15
    sp = []                                     # [x, y, vx, vy, life, child, boom]

    def spawn(x, y, speed, life, child, boom):
        a = rng.random() * 2 * math.pi
        sp.append([x, y, math.cos(a) * speed, math.sin(a) * speed, life, child, boom])

    acc, t = 0.0, 0.0
    damp = math.exp(-1.2 * dt)
    bx = by = None
    while t < t_end:
        nxt = []
        for s_ in sp:
            s_[4] -= dt
            if s_[4] <= 0:
                if not s_[5] and rng.random() < 0.30:
                    for _ in range(3):
                        nxt.append((s_[0], s_[1], 560 * 0.45 * (0.3 + 0.7 * rng.random()),
                                    0.30 * (0.4 + 0.6 * rng.random()), True, s_[6]))
                s_[4] = None
                continue
            s_[2] *= damp
            s_[3] = s_[3] * damp + 520 * dt
            s_[0] += s_[2] * dt
            s_[1] += s_[3] * dt
        sp = [s_ for s_ in sp if s_[4] is not None]
        for args in nxt:
            spawn(*args)
        if bx is None and t >= t_boom:
            a = rng.random() * 2 * math.pi
            r = RADIUS * (0.35 + 0.40 * rng.random())
            bx, by = RADIUS + math.cos(a) * r, RADIUS + math.sin(a) * r
            for _ in range(260):
                u = rng.random()
                spawn(bx, by, 150 + 600 * u * u, 0.25 + 0.85 * rng.random(), False, True)
        acc += 260 * dt
        while acc >= 1:
            acc -= 1
            u = rng.random()
            spawn(RADIUS + (rng.random() * 2 - 1) * 5, RADIUS + (rng.random() * 2 - 1) * 5,
                  120 + 440 * u * u, 0.25 + 0.85 * rng.random(), False, False)
        t += dt

    def plot(x, y, g):
        if 0 <= x < RES and 0 <= y < RES:
            img[y, x] = g
    for boom in (False, True):                  # explosion sparks drawn last (FF over DD)
        g = 0xFF if boom else 0xDD
        for x, y, vx, vy, *_rest in (s_ for s_ in sp if s_[6] == boom):
            x0, y0 = x - vx * 0.035, y - vy * 0.035
            n = int(max(abs(x - x0), abs(y - y0)))
            for i in range(n + 1):
                tt = i / n if n else 1
                plot(int(x0 + (x - x0) * tt), int(y0 + (y - y0) * tt), g)
    save(img, os.path.join(out_dir, "flame1_doom_spark.png"))


def main():
    out_dir = sys.argv[1] if len(sys.argv) > 1 else "assets/previews"
    os.makedirs(out_dir, exist_ok=True)
    rng = np.random.default_rng(7)
    doom_linear(out_dir, rng)
    doom_radial(out_dir, rng)
    doom_radial_mv(out_dir, rng)
    doom_xong(out_dir, rng)
    doom_spark(out_dir, rng)


if __name__ == "__main__":
    main()
