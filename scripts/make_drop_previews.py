#!/usr/bin/env python3
"""Preview images (466x466 PNG) of the drop1_16M_pool_* experiments, same simulation and render as the firmware.

Usage: python scripts/make_drop_previews.py [out_dir] [--crystal=FLOOR_BIN]   (default assets/previews)

Wave equation on the 236x236 grid (1 cell = 2x2 px) in float instead of the firmware Q15
fixed point; refraction shift, floor and colors as in ripple.c. Each experiment gets a
few drops (taps with rebounds, drags, microphone drops) and is drawn after a fixed
number of steps. Fixed RNG seed.

- drop1_16M_pool_tiles: azure palette, tiles in 8 slightly different shades
- drop1_16M_pool_listening: azure palette, small random drops as from the microphone
- drop1_16M_pool_image: placeholder floor assets/floor_placeholder.bin (the build default)
- drop1_16M_pool_crystal: only with --crystal=<FLA2 floor.bin> (third-party animation, not in the repository)
"""
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

RES = 466
SCALE = 2
GRID_OFF = 2
GRID_N = 236
GRID_C = (RES / 2 + GRID_OFF) / SCALE
GRID_R = RES / 2 / SCALE - 0.5
WAVE_C2 = 0.45
DAMPING = 1 - 1 / 64
VISCOSITY = 1 / 32
FIELD_SCALE = 2.0
SUB = 16
REFRACT_PX = 0.020
MAX_SHIFT_PX = 24
GAMMA = 2.2
EDGE_POWER = 2.0
MASK_MARGIN = 24
MASK_SIZE = RES + 2 * MASK_MARGIN

AZURE = dict(base=(40, 175, 235), edge=(15, 120, 190), line=(0, 0, 0))

# Drop pulses of a tap: (step after impact, gain, radius)
PULSES = [(0, 1.00, 1.0), (20, -0.20, 3.0 / 3.5), (38, 0.10, 3.0 / 3.5)]


class Water:
    def __init__(self):
        y, x = np.mgrid[0:GRID_N, 0:GRID_N]
        inside = (x - GRID_C) ** 2 + (y - GRID_C) ** 2 < GRID_R ** 2
        inside[[0, -1], :] = False
        inside[:, [0, -1]] = False
        self.inside = inside
        self.cur = np.zeros((GRID_N, GRID_N))
        self.prev = np.zeros((GRID_N, GRID_N))
        k = DAMPING * (1 - VISCOSITY)
        self.cc, self.cp, self.cs = k * (2 - 4 * WAVE_C2), -k, k * WAVE_C2 + DAMPING * VISCOSITY / 4
        self.splashes = []

    def bump(self, px, py, radius_px, strength):
        gx, gy, r = (px + GRID_OFF) / SCALE, (py + GRID_OFF) / SCALE, radius_px / SCALE
        y, x = np.mgrid[0:GRID_N, 0:GRID_N]
        d = np.sqrt((x - gx) ** 2 + (y - gy) ** 2) / r
        w = np.where(d <= 1, 0.5 * (1 + np.cos(d * math.pi)), 0) * self.inside
        self.cur -= strength * w * FIELD_SCALE

    def tap(self, px, py, strength, radius_px):
        self.splashes.append([px, py, strength, radius_px, 0])

    def step(self):
        for s in self.splashes:
            for when, gain, rk in PULSES:
                if s[4] == when:
                    self.bump(s[0], s[1], s[3] * rk, s[2] * gain)
            s[4] += 1
        c = self.cur
        nb = np.zeros_like(c)
        nb[1:-1, 1:-1] = c[1:-1, :-2] + c[1:-1, 2:] + c[:-2, 1:-1] + c[2:, 1:-1]
        new = (self.cc * c + self.cp * self.prev + self.cs * nb) * self.inside
        self.prev, self.cur = self.cur, np.clip(np.trunc(new), -32767, 32767)

    def shift(self):
        """Per pixel floor shift (dx, dy) in 1/SUB px, bilinear from the cell slopes."""
        h = self.cur
        k = REFRACT_PX * SUB
        lim = MAX_SHIFT_PX * SUB
        sx = np.zeros_like(h)
        sy = np.zeros_like(h)
        sx[1:-1, 1:-1] = np.trunc((h[1:-1, :-2] - h[1:-1, 2:]) * k)
        sy[1:-1, 1:-1] = np.trunc((h[:-2, 1:-1] - h[2:, 1:-1]) * k)
        sx, sy = sx.clip(-lim, lim), sy.clip(-lim, lim)
        p = np.arange(RES) + GRID_OFF
        j, f = p // SCALE, (p % SCALE) / SCALE
        def interp(a):
            a = a[j] * (1 - f)[:, None] + a[j + 1] * f[:, None]          # rows
            return a[:, j] * (1 - f)[None, :] + a[:, j + 1] * f[None, :]  # columns
        return np.trunc(interp(sx)), np.trunc(interp(sy))


def ring_e():
    y, x = np.mgrid[0:RES, 0:RES]
    c = RES / 2 - 0.5
    r = np.sqrt((x - c) ** 2 + (y - c) ** 2)
    return np.minimum(r / (RES / 2), 1) ** EDGE_POWER


def mix_color(pal, cover, bright=None, green=None):
    """Floor color (center -> edge) blended toward the line / figure color by cover 0..1, in emitted light."""
    e = ring_e()
    out = np.zeros((RES, RES, 3))
    for k in range(3):
        base = pal["base"][k] + (pal["edge"][k] - pal["base"][k]) * e
        if bright is not None:
            base = base * bright
            if k == 1:
                base = base * green
            base = np.minimum(base, 255)
        lb = (base / 255) ** GAMMA
        ll = (pal["line"][k] / 255) ** GAMMA
        out[..., k] = 255 * (lb + (ll - lb) * cover) ** (1 / GAMMA)
    return out


def circle(img):
    y, x = np.mgrid[0:RES, 0:RES]
    c = RES / 2 - 0.5
    img[(x - c) ** 2 + (y - c) ** 2 > (RES / 2) ** 2] = 0
    return np.clip(np.rint(img), 0, 255).astype(np.uint8)


def tile_variants():
    var = np.zeros((8, 8), int)
    seed = 12345
    for ty in range(8):
        for tx in range(8):
            while True:
                seed = (seed * 1103515245 + 12345) & 0xFFFFFFFF
                v = (seed >> 16) % 8
                if not ((tx > 0 and v == var[ty, tx - 1]) or (ty > 0 and v == var[ty - 1, tx])):
                    break
            var[ty, tx] = v
    return var


VARIANT_SHIFT = [(0.0, 0.0), (0.6, -0.3), (-0.5, 0.4), (0.9, 0.5),
                 (-0.9, -0.2), (0.3, 0.9), (-0.3, -0.8), (0.5, -0.9)]


def render_tiles(dx, dy, vary):
    """Grid 80x48 px, line core 5 px + 2 px fade, a line through the panel center."""
    def cover(pos_sub, tile):
        d = np.mod(pos_sub, tile * SUB)
        idx = np.floor_divide(pos_sub, tile * SUB)
        d = np.minimum(d, tile * SUB - d) / SUB
        return np.clip(1 - (d - 2.5) / 2.0, 0, 1), idx
    px = np.arange(RES)
    u = (px[None, :] - RES // 2) * SUB + dx
    v = (px[:, None] - RES // 2) * SUB + dy
    cu, tu = cover(u, 80)
    cv, tv = cover(v, 48)
    c = np.maximum(cu, cv)
    if vary:
        var = tile_variants()[np.mod(tv, 8).astype(int), np.mod(tu, 8).astype(int)]
        sh = np.array(VARIANT_SHIFT)
        bright = 1 + vary * sh[var, 0]
        green = 1 + 0.6 * vary * sh[var, 1]
        return mix_color(AZURE, c, bright, green)
    return mix_color(AZURE, c)


def render_mask(dx, dy, mask):
    px = np.arange(RES)
    su = (px[None, :] + MASK_MARGIN) * SUB + dx + SUB // 2
    sv = (px[:, None] + MASK_MARGIN) * SUB + dy + SUB // 2
    mu = np.clip(su // SUB, 0, MASK_SIZE - 1).astype(int)
    mv = np.clip(sv // SUB, 0, MASK_SIZE - 1).astype(int)
    return mix_color(AZURE, mask[mv, mu] / SUB)


def load_fla2(path, frame=0):
    b = open(path, "rb").read()
    assert b[:4] == b"FLA2"
    w, h, n, x0, y0 = struct.unpack("<5H", b[4:14])
    bg, colors = b[14], b[15] or 256
    pal = np.frombuffer(b[16:16 + 3 * colors], np.uint8).reshape(-1, 3)
    frames_at = 16 + 3 * colors + 2 * n
    f = np.frombuffer(b[frames_at + frame * w * h: frames_at + (frame + 1) * w * h], np.uint8).reshape(h, w)
    return f, pal, bg, x0, y0


def render_crystal(dx, dy, anim):
    f, pal, bg, x0, y0 = anim
    h, w = f.shape
    px = np.arange(RES)
    su = (px[None, :] + MASK_MARGIN) * SUB + dx + SUB // 2
    sv = (px[:, None] + MASK_MARGIN) * SUB + dy + SUB // 2
    au = ((su + SUB // 2) // SUB - x0).astype(int)
    av = ((sv + SUB // 2) // SUB - y0).astype(int)
    ok = (au >= 0) & (au < w) & (av >= 0) & (av < h)
    idx = np.where(ok, f[av.clip(0, h - 1), au.clip(0, w - 1)], bg)
    c = pal[idx].astype(float)
    # Crests facing the light (top left) toward the highlight, opposite slopes darker
    l = np.clip(-(dx + dy) * 58 / 16, -158, 158)[..., None]
    hi = np.array([235, 255, 235], float)
    c = np.where(l > 0, c + (hi - c) * l / 256, c * (256 + l / 2) / 256)
    return c


def run(water, steps, events, rng=None, mic=0.0, mic_strength=3000):
    for s in range(steps):
        for when, fn in events:
            if when == s:
                fn()
        if mic > 0 and rng.random() < 0.3 + 0.7 * mic:
            r = RES // 2 - 8
            while True:
                x, y = rng.integers(-r, r + 1, 2)
                if x * x + y * y <= r * r:
                    break
            water.bump(RES // 2 + x, RES // 2 + y, 3.5, mic_strength * mic)
        water.step()
    return water.shift()


def drag(water, points, strength=4000, radius=3.5):
    for x, y in points:
        water.bump(x, y, radius, strength)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    crystal = next((a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--crystal=")), None)
    out_dir = args[0] if args else "assets/previews"
    os.makedirs(out_dir, exist_ok=True)
    rng = np.random.default_rng(3)

    def save(img, name):
        path = os.path.join(out_dir, name + ".png")
        Image.fromarray(circle(img)).save(path)
        print("wrote", path)

    # Tiles: three taps at different times
    w = Water()
    dx, dy = run(w, 110, [(0, lambda: w.tap(170, 190, 9000, 3.5)),
                         (25, lambda: w.tap(300, 270, 9000, 3.5)),
                         (45, lambda: w.tap(210, 330, 9000, 3.5))])
    save(render_tiles(dx, dy, 0.10), "drop1_16M_pool_tiles")

    # Listening: microphone drops at level 0.4 for 80 steps
    w = Water()
    dx, dy = run(w, 80, [], rng=rng, mic=0.4)
    save(render_tiles(dx, dy, 0), "drop1_16M_pool_listening")

    # Image: placeholder floor, one tap and a drag trail
    mask = np.frombuffer(open("assets/floor_placeholder.bin", "rb").read(), np.uint8).reshape(MASK_SIZE, MASK_SIZE)
    w = Water()
    trail = [(140 + 12 * i, 300 - 6 * i) for i in range(12)]
    dx, dy = run(w, 100, [(0, lambda: w.tap(250, 200, 9000, 3.5)), (30, lambda: drag(w, trail))])
    save(render_mask(dx, dy, mask), "drop1_16M_pool_image")

    if crystal:
        anim = load_fla2(crystal)
        w = Water()
        dx, dy = run(w, 90, [(0, lambda: w.tap(180, 180, 9000, 7.0)),
                             (25, lambda: w.tap(290, 290, 9000, 7.0))])
        save(render_crystal(dx, dy, anim), "drop1_16M_pool_crystal")


if __name__ == "__main__":
    main()
