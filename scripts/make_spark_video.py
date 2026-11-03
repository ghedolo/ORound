#!/usr/bin/env python3
"""8 s animated GIF preview of flame1_doom_spark (466x466, 20 fps), one explosion.

Usage: python scripts/make_spark_video.py [out_path]   (default assets/previews/flame1_doom_spark.gif)

Same constants as the firmware (flame.c, spark.c): radial Doom fire at 5 px (firmware default), 16.2 steps/s,
flickering source, black and white with white DD:DD:DD, sinusoidal rotation
(0.0326 x sin(2 pi t / 15 s) turns/s), welding sparks (white DD), one explosion off center
(white FF). Fire and sparks are warmed up before the first frame. Fixed RNG seed.
The sub-cell outward shift is not drawn.
"""
import math
import os
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_flame_previews import (DOOM_RGB, LEVELS, RADIUS, RES, circle_mask, decay_params,  # noqa: E402
                                 fire_steps, polar_grid, polar_lut)

SCALE = 5
FPS = 20
DURATION_S = 8.0
STEP_RATE = 81 / SCALE            # fire steps per second
WARM_STEPS = 200
WARM_SPARK_S = 1.5
BOOM_T = 3.0                      # explosion time in the video, s
SPARK_DT = 1 / 60
ROT_RPS, ROT_PERIOD = 0.0326, 15.0
FIRE_W, BOOM_W = 0xDD, 0xFF


def source_flicker(f, rng):
    w = f.shape[1]
    loss = rng.integers(0, 9) + rng.integers(0, 7, w) + (rng.integers(0, 6, w) == 0) * rng.integers(0, 25, w)
    f[-1] = np.maximum(LEVELS - 1 - loss, 0).astype(np.uint8)


class Sparks:
    """Vectorized copy of spark.c."""

    def __init__(self, rng):
        self.rng = rng
        self.a = np.zeros((0, 7))         # x, y, vx, vy, life, child, boom
        self.acc = 0.0

    def spawn(self, x, y, speed, life, child, boom):
        n = len(speed)
        ang = self.rng.random(n) * 2 * math.pi
        new = np.column_stack([x, y, np.cos(ang) * speed, np.sin(ang) * speed, life,
                               np.full(n, child), np.full(n, boom)])
        self.a = np.vstack([self.a, new])

    def step(self, dt, emit=True):
        rng, a = self.rng, self.a
        a[:, 4] -= dt
        dead = a[:, 4] <= 0
        split = dead & (a[:, 5] == 0) & (rng.random(len(a)) < 0.30)
        parents = a[split]
        a = a[~dead]
        damp = math.exp(-1.2 * dt)
        a[:, 2] *= damp
        a[:, 3] = a[:, 3] * damp + 520 * dt
        a[:, 0] += a[:, 2] * dt
        a[:, 1] += a[:, 3] * dt
        self.a = a
        if len(parents):
            p = np.repeat(parents, 3, axis=0)
            n = len(p)
            self.spawn(p[:, 0], p[:, 1], 560 * 0.45 * (0.3 + 0.7 * rng.random(n)),
                       0.30 * (0.4 + 0.6 * rng.random(n)), 1, p[:, 6])
        if emit:
            self.acc += 260 * dt
            n = int(self.acc)
            self.acc -= n
            if n:
                u = rng.random(n)
                self.spawn(RADIUS + (rng.random(n) * 2 - 1) * 5, RADIUS + (rng.random(n) * 2 - 1) * 5,
                           120 + 440 * u * u, 0.25 + 0.85 * rng.random(n), 0, 0)

    def boom(self):
        rng = self.rng
        ang = rng.random() * 2 * math.pi
        r = RADIUS * (0.35 + 0.40 * rng.random())
        x, y = RADIUS + math.cos(ang) * r, RADIUS + math.sin(ang) * r
        u = rng.random(260)
        self.spawn(np.full(260, x), np.full(260, y), 150 + 600 * u * u, 0.25 + 0.85 * rng.random(260), 0, 1)
        return int(x), int(y)

    def draw(self, img):
        for boom, g in ((0, FIRE_W), (1, BOOM_W)):
            s = self.a[self.a[:, 6] == boom]
            if not len(s):
                continue
            x1, y1 = s[:, 0], s[:, 1]
            x0, y0 = x1 - s[:, 2] * 0.035, y1 - s[:, 3] * 0.035
            n = np.maximum(np.abs(x1 - x0), np.abs(y1 - y0)).astype(int)
            tmax = int(n.max()) + 1
            t = np.arange(tmax + 1)[None, :] / np.maximum(n, 1)[:, None]
            valid = t <= 1.0
            px = (x0[:, None] + (x1 - x0)[:, None] * t).astype(int)[valid]
            py = (y0[:, None] + (y1 - y0)[:, None] * t).astype(int)[valid]
            ok = (px >= 0) & (px < RES) & (py >= 0) & (py < RES)
            img[py[ok], px[ok]] = g


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/previews/flame1_doom_spark.gif"
    rng = np.random.default_rng(11)
    w, h = polar_grid(SCALE)
    row, ai = polar_lut(SCALE, w, h)
    inside = row >= 0
    rr = row.clip(0)
    f = np.zeros((h, w), np.uint8)
    k, p = decay_params((LEVELS - 1) / (0.70 * h))
    luma = np.array(DOOM_RGB, float) @ np.array([0.299, 0.587, 0.114])
    bw = np.where(luma + 0.5 >= 128, FIRE_W, 0).astype(np.uint8)
    mask = circle_mask()

    def fire_step():
        source_flicker(f, rng)
        fire_steps(f, rng, 1, [-1, 0, 0, 1], None, k, p)

    for _ in range(WARM_STEPS):
        fire_step()
    sp = Sparks(rng)
    for _ in range(int(WARM_SPARK_S / SPARK_DT)):
        sp.step(SPARK_DT)

    frames = []
    t = 0.0
    step_acc = spark_acc = 0.0
    rot = 0.0
    boomed = False
    flash = None
    n_frames = int(DURATION_S * FPS)
    for _ in range(n_frames):
        dt = 1 / FPS
        step_acc += dt * STEP_RATE
        while step_acc >= 1:
            step_acc -= 1
            fire_step()
        spark_acc += dt
        while spark_acc >= SPARK_DT:
            spark_acc -= SPARK_DT
            sp.step(SPARK_DT)
        rot += ROT_RPS * math.sin(2 * math.pi * t / ROT_PERIOD) * dt
        if not boomed and t >= BOOM_T:
            boomed = True
            flash = (sp.boom(), t)
        col = (ai + int((rot % 1.0) * w)) % w
        img = np.where(inside, bw[f[rr, col]], 0).astype(np.uint8)
        sp.draw(img)
        if flash and t - flash[1] < 0.08:
            (fx, fy) = flash[0]
            yy, xx = np.ogrid[0:RES, 0:RES]
            img[(xx - fx) ** 2 + (yy - fy) ** 2 <= 14 * 14] = BOOM_W
        img[~mask] = 0
        frames.append(Image.fromarray(img, "L"))
        t += dt

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=int(1000 / FPS), loop=0,
                   optimize=True)
    print("wrote", out, f"{os.path.getsize(out) / 1e6:.1f} MB", len(frames), "frames")


if __name__ == "__main__":
    main()
