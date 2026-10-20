#!/usr/bin/env python3
"""Preview images (466x466 PNG) of the life1_* experiments, same geometry and colors as the firmware.

Usage: python scripts/make_life_previews.py [out_dir]   (default assets/previews)
Each image shows the state after a few generations from a random seed (fixed RNG seed).
"""
import math
import os
import random
import sys
from collections import defaultdict

from PIL import Image, ImageDraw

RES = 466
R = RES / 2
SS = 3                      # supersampling factor
GENS = 9                    # generations before the snapshot
AGE_FULL = 12

PENROSE_AGE = [(255, 140, 30), (255, 230, 40), (60, 255, 70)]


def age_color(stops, age):
    a = min(age, AGE_FULL) / AGE_FULL * (len(stops) - 1)
    j = min(int(a), len(stops) - 2)
    f = a - j
    return tuple(int(stops[j][i] + (stops[j + 1][i] - stops[j][i]) * f + 0.5) for i in range(3))


def run_life(nb, rng, density=0.30, birth=(3,), survive=(2, 3)):
    """Life (default B3/S23) on a neighbor list; returns alive flags and ages."""
    n = len(nb)
    alive = [rng.random() < density for _ in range(n)]
    age = [0] * n
    for _ in range(GENS):
        nxt = []
        for i in range(n):
            c = sum(alive[j] for j in nb[i])
            nxt.append(c in survive if alive[i] else c in birth)
        for i in range(n):
            if nxt[i]:
                age[i] = age[i] + 1 if alive[i] else 0
        alive = nxt
    return alive, age


def neighbors_by_vertex(polys):
    vmap = defaultdict(set)
    for t, p in enumerate(polys):
        for x, y in p:
            vmap[(round(x * 4), round(y * 4))].add(t)
    nb = [set() for _ in polys]
    for ts in vmap.values():
        for t in ts:
            nb[t] |= ts - {t}
    return [list(s) for s in nb]


def border_line(size, from_px, div):
    """Black line width between tiles as in the firmware: 2 * max(0.55, size / div) from from_px on."""
    return 2 * max(0.55, size / div) if size >= from_px else 0


def draw_polys(polys, colors, border):
    im = Image.new("RGB", (RES * SS, RES * SS), (0, 0, 0))
    d = ImageDraw.Draw(im)
    for p, c in zip(polys, colors):
        pts = [((x + R) * SS, (y + R) * SS) for x, y in p]
        d.polygon(pts, fill=c)
        if border > 0:
            d.line(pts + [pts[0]], fill=(0, 0, 0), width=max(1, round(border * SS)))
    return finish(im)


def finish(im):
    mask = Image.new("L", im.size, 0)
    ImageDraw.Draw(mask).ellipse((0, 0, im.size[0] - 1, im.size[1] - 1), fill=255)
    out = Image.new("RGB", im.size, (0, 0, 0))
    out.paste(im, (0, 0), mask)
    return out.resize((RES, RES), Image.LANCZOS)


def centroid_in(p):
    gx = sum(x for x, _ in p) / len(p)
    gy = sum(y for _, y in p) / len(p)
    return math.hypot(gx, gy) <= R


# ---- life1_conway: 20 px cells, 2 px gap, rounded corners ----

def conway(rng):
    cell, gap, corner = 20, 2.0, 6.0
    grid = RES // cell
    origin = (RES - grid * cell) / 2
    cells = []
    for gy in range(grid):
        for gx in range(grid):
            corners = [(origin + (gx + (k & 1)) * cell - R, origin + (gy + (k >> 1)) * cell - R) for k in range(4)]
            if all(math.hypot(x, y) <= R for x, y in corners):
                cells.append((gx, gy))
    index = {c: i for i, c in enumerate(cells)}
    nb = []
    for gx, gy in cells:
        nb.append([index[(gx + dx, gy + dy)] for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                   if (dx or dy) and (gx + dx, gy + dy) in index])
    alive, age = run_life(nb, rng)
    im = Image.new("RGB", (RES * SS, RES * SS), (0, 0, 0))
    d = ImageDraw.Draw(im)
    for i, (gx, gy) in enumerate(cells):
        c = age_color([(255, 190, 70), (50, 140, 255)], age[i]) if alive[i] else (14, 14, 24)
        x0 = (origin + gx * cell + gap) * SS
        y0 = (origin + gy * cell + gap) * SS
        x1 = (origin + (gx + 1) * cell - gap) * SS
        y1 = (origin + (gy + 1) * cell - gap) * SS
        d.rounded_rectangle((x0, y0, x1, y1), radius=corner * SS, fill=c)
    return finish(im)


# ---- life1_penrose: P3 rhombi by deflation, edge 24 px ----

PHI = (1 + 5 ** 0.5) / 2


def penrose_polys(edge=24):
    """Rhombi (or unpaired halves) with the center inside the display, and thick flags."""
    r0 = R / math.cos(math.pi / 10) + 2 * edge
    steps = math.ceil(math.log(r0 / edge) / math.log(PHI))
    r = edge * PHI ** steps
    tris = []
    for i in range(10):
        b = complex(r * math.cos((2 * i - 1) * math.pi / 10), r * math.sin((2 * i - 1) * math.pi / 10))
        c = complex(r * math.cos((2 * i + 1) * math.pi / 10), r * math.sin((2 * i + 1) * math.pi / 10))
        tris.append((0, 0j, c, b) if i % 2 == 0 else (0, 0j, b, c))
    for _ in range(steps):
        out = []
        for color, a, b, c in tris:
            if color == 0:
                p = a + (b - a) / PHI
                out += [(0, c, p, b), (1, p, c, a)]
            else:
                q = b + (a - b) / PHI
                rr = b + (c - b) / PHI
                out += [(1, rr, c, a), (1, q, rr, b), (0, rr, q, a)]
        tris = [t for t in out if abs((t[1] + t[2] + t[3]) / 3) - max(abs(v - (t[1] + t[2] + t[3]) / 3) for v in t[1:]) <= R + 1]
    halves = defaultdict(list)
    for t in tris:
        key = tuple(sorted([(round(t[2].real * 8), round(t[2].imag * 8)), (round(t[3].real * 8), round(t[3].imag * 8))]))
        halves[(key, t[0])].append(t)
    polys, thick = [], []
    for (_, color), ts in halves.items():
        a = ts[0]
        if len(ts) == 2:
            p = [a[1], a[2], ts[1][1], a[3]]
        else:
            p = [a[1], a[2], a[3]]
        p = [(v.real, v.imag) for v in p]
        if centroid_in(p):
            polys.append(p)
            thick.append(color)
    return polys, thick


def penrose(rng, edge=24):
    polys, thick = penrose_polys(edge)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else ((22, 22, 38) if thick[i] else (12, 12, 22))
              for i in range(len(polys))]
    return draw_polys(polys, colors, border_line(edge, 18, 32))


# ---- life1_ts1: Alhambra arrows on a square grid, arrow 28 px ----

QX, QY = 629 / 1024, 772 / 1024
ARROW = [(0, 0), (-(1 - QY), QX), (-0.5, 0.5), (-QX, QY), (0, 1), (QX, QY), (0.5, 0.5), (1 - QY, QX)]


def arrow(ti, tj, rot, size):
    pts = []
    for x, y in ARROW:
        for _ in range(rot):
            x, y = -y, x
        pts.append(((ti + x) * size, (tj + y) * size))
    return pts


def ts1(rng, size=28):
    g = math.ceil(R / size) + 1
    polys, vert = [], []
    for j in range(-g, g + 1):
        for i in range(-g, g + 1):
            even = (i + j) % 2 == 0
            for p, v in ((arrow(i, j, 0, size) if even else arrow(i, j + 1, 2, size), 1),
                         (arrow(i + 1, j, 1, size) if even else arrow(i, j, 3, size), 0)):
                if centroid_in(p):
                    polys.append(p)
                    vert.append(v)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else ((22, 22, 38) if vert[i] else (12, 12, 22))
              for i in range(len(polys))]
    return draw_polys(polys, colors, border_line(size, 24, 40))


# ---- life1_ts2: Cairo pentagons, short edge 14 px ----

PENTA = [(0, -1), (-2, -2), (-3, 0), (-2, 2), (0, 1)]


def penta(cx, cy, vert, side, unit):
    pts = []
    for x, y in PENTA:
        x *= side
        if not vert:
            x, y = y, x
        pts.append(((cx + x) * unit, (cy + y) * unit))
    return pts


def ts2(rng, edge=14):
    unit = edge / 2
    g = math.ceil(R / (2 * edge)) + 1
    polys, vert = [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            v = (a + b) % 2 == 0
            for side in (-1, 1):
                p = penta(4 * a, 4 * b, v, side, unit)
                if centroid_in(p):
                    polys.append(p)
                    vert.append(v)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else ((22, 22, 38) if vert[i] else (12, 12, 22))
              for i in range(len(polys))]
    return draw_polys(polys, colors, border_line(edge, 16, 32))


# ---- life1_ts3: hexagons in columns and 60 degree rhombi, hexagon side 16 px ----

HEX = [(-2, 0), (-1, -1), (1, -1), (2, 0), (1, 1), (-1, 1)]
RHOMB = [(0, -1), (1, 0), (0, 1), (-1, 0)]


def ts3(rng, side=16):
    ux, uy = side / 2, side * 3 ** 0.5 / 2
    g = math.ceil(R / side) + 1
    polys, kind = [], []
    for j in range(-g, g + 1):
        for i in range(-g, g + 1):
            for (cx, cy), v, k in (((4 * i, 2 * j), HEX, (i + j) % 2), ((4 * i + 2, 2 * j + 1), RHOMB, 2)):
                p = [((cx + x) * ux, (cy + y) * uy) for x, y in v]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    dead = [(12, 12, 22), (22, 22, 38), (28, 32, 42)]
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else dead[kind[i]] for i in range(len(polys))]
    return draw_polys(polys, colors, border_line(side, 12, 24))


# ---- life1_ts4: pentaflake level 3 (pentagon side 15.3 px) plus the rhombi between pentagons, rule B2/S23 ----

def flake(level, cx, cy, r, th, down, out):
    if level == 0:
        out.append(([(cx + r * math.cos(th + k * 2 * math.pi / 5), cy + r * math.sin(th + k * 2 * math.pi / 5))
                     for k in range(5)], down))
        return
    rc = r / (1 + PHI)
    flake(level - 1, cx, cy, rc, th + math.pi / 5, not down, out)
    for k in range(5):
        a = th + k * 2 * math.pi / 5
        flake(level - 1, cx + PHI * rc * math.cos(a), cy + PHI * rc * math.sin(a), rc, th, down, out)


def flake_rhombi(polys):
    """Thin rhombi closed by four pentagon edges (same search as the firmware)."""
    mids = {(round((a[0] + b[0]) * 2), round((a[1] + b[1]) * 2)) for p in polys for a, b in zip(p, p[1:] + p[:1])}

    def has(a, b):
        kx, ky = round((a[0] + b[0]) * 2), round((a[1] + b[1]) * 2)
        return any((kx + dx, ky + dy) in mids for dx in (-1, 0, 1) for dy in (-1, 0, 1))

    found = {}
    for p in polys:
        cx, cy = sum(x for x, _ in p) / 5, sum(y for _, y in p) / 5
        for a, b in zip(p, p[1:] + p[:1]):
            mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
            for ang in (36, -36, 144, -144):
                c, sn = math.cos(math.radians(ang)), math.sin(math.radians(ang))
                vx, vy = (b[0] - a[0]) * c - (b[1] - a[1]) * sn, (b[0] - a[0]) * sn + (b[1] - a[1]) * c
                q, d = (b[0] + vx, b[1] + vy), (a[0] + vx, a[1] + vy)
                rx, ry = (a[0] + q[0]) / 2, (a[1] + q[1]) / 2
                if (rx - mx) * (mx - cx) + (ry - my) * (my - cy) <= 0:
                    continue
                if has(b, q) and has(q, d) and has(d, a):
                    found[(round(rx * 4), round(ry * 4))] = [a, b, q, d]
    return list(found.values())


def ts4(rng, level=3):
    out = []
    flake(level, 0, 0, R, -math.pi / 2, False, out)
    polys = [p for p, _ in out]
    kind = [1 if down else 0 for _, down in out]
    rh = flake_rhombi(polys)
    polys += rh
    kind += [2] * len(rh)
    vmap = defaultdict(set)
    for t, p in enumerate(polys):
        for x, y in p:
            vmap[(round(x * 20), round(y * 20))].add(t)
    nb = [set() for _ in polys]
    for ts in vmap.values():
        for t in ts:
            nb[t] |= ts - {t}
    alive, age = run_life([list(n) for n in nb], rng, birth=(2,), survive=(2, 3))
    dead = [(12, 12, 22), (22, 22, 38), (28, 32, 42)]
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else dead[kind[i]] for i in range(len(polys))]
    side = math.dist(polys[0][0], polys[0][1])
    return draw_polys(polys, colors, border_line(side, 12, 28))


# ---- life1_ts5: pointy-top hexagons with striped fills, side 12 px ----

HEX_P = [(0, -2), (1, -1), (1, 1), (0, 2), (-1, 1), (-1, -1)]
SHADE = [0.12, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90, 1.0]


def ts5(rng, side=12):
    import numpy as np
    ux, uy = side * 3 ** 0.5 / 2, side / 2
    g = math.ceil(R / (1.5 * side)) + 1
    polys, centers, angles = [], [], []
    for r in range(-g, g + 1):
        for k in range(-g, g + 1):
            hx, hy = 2 * k + (r & 1), 3 * r
            cx, cy = hx * ux, hy * uy
            if math.hypot(cx, cy) > R:
                continue
            polys.append([((hx + x) * ux, (hy + y) * uy) for x, y in HEX_P])
            centers.append((cx, cy))
            angles.append((hx - (2 if r & 1 else 0)) * math.pi / 12)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else (34, 36, 46) for i in range(len(polys))]

    # tile index map (1 px, no supersampling: stripes stay sharp), border line 1.2 px as on the device
    idx = Image.new("I", (RES, RES), -1)
    d = ImageDraw.Draw(idx)
    for t, p in enumerate(polys):
        d.polygon([(x + R, y + R) for x, y in p], fill=t, outline=-1)
    m = np.array(idx)
    ys, xs = np.mgrid[0:RES, 0:RES]
    x, y = xs + 0.5 - R, ys + 0.5 - R
    ok = m >= 0
    t = np.where(ok, m, 0)
    cx = np.array([c[0] for c in centers])[t]
    cy = np.array([c[1] for c in centers])[t]
    an = np.array(angles)[t]
    period = max(2.5, side * 3 ** 0.5 / 7.5)
    ph = (-(x - cx) * np.sin(an) + (y - cy) * np.cos(an)) / period + 0.5 * 0.33
    ph -= np.floor(ph)
    v = (y - cy + side) / (2 * side)
    level = np.clip(1 + (v * 7).astype(int), 1, 7)
    level[ph < 0.33] = 0
    f = np.array(SHADE)[level]
    rgb = np.array(colors, float)[t] * f[..., None]
    rgb[~ok] = 0
    rgb[np.hypot(x, y) > R] = 0
    return Image.fromarray(rgb.round().astype(np.uint8), "RGB")


# ---- life1_ts6: isometric L pieces and parallelograms (data/tassel/fake_3d1.jpg), rhombus side 12 px, B3/S234 ----

TS6_PIECES = [
    ("R", [(-3, -1), (-2, 0), (0, -2), (0, 0), (1, 1), (1, -3), (-1, -5), (-1, -3)]),
    ("W", [(-1, 1), (1, 3), (3, 1), (2, 0), (2, -4), (1, -3), (1, 1), (0, 0)]),
    ("G", [(-3, -3), (-1, -5), (-1, -3), (-3, -1)]),
    ("B", [(-2, 0), (0, -2), (0, 0), (-1, 1), (1, 3), (1, 5), (-1, 3), (-2, 4)]),
    ("G", [(2, -4), (2, 0), (3, 1), (3, -3)]),
    ("G", [(-1, -5), (1, -3), (2, -4), (0, -6)]),
]
TS6_DEAD = {"R": (32, 36, 48), "W": (44, 44, 48), "B": (12, 30, 50), "G": (24, 24, 28)}
TS6_SHADE = {"R": 0.85, "W": 1.0, "B": 0.70, "G": 0.55}


def ts6_polys(side=12):
    ux, uy = side * 3 ** 0.5 / 2, side / 2
    g = math.ceil(R / (21 ** 0.5 * side)) + 3
    polys, kind = [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            dx, dy = -5 * a + 4 * b, 3 * a + 6 * b
            for k, v in TS6_PIECES:
                p = [((dx + x) * ux, (dy + y) * uy) for x, y in v]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
    return polys, kind


def ts6(rng, side=12):
    polys, kind = ts6_polys(side)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng, birth=(3,), survive=(2, 3, 4))
    colors = []
    for i in range(len(polys)):
        if alive[i]:
            c = age_color(PENROSE_AGE, age[i])
            colors.append(tuple(int(v * TS6_SHADE[kind[i]] + 0.5) for v in c))
        else:
            colors.append(TS6_DEAD[kind[i]])
    return draw_polys(polys, colors, 0)


# ---- life1_ts7: L pieces, cubes and parallelograms (designed from data/tassel/fake_3d2.jpg), side 12 px, B2/S23 ----

TS7_PIECES = [
    ("R", [(-3, -1), (-2, 0), (0, -2), (0, 0), (1, 1), (1, -3), (-1, -5), (-1, -3)]),
    ("W", [(-1, 1), (1, 3), (3, 1), (2, 0), (2, -4), (1, -3), (1, 1), (0, 0)]),
    ("B", [(-2, 0), (0, -2), (0, 0), (-1, 1), (1, 3), (1, 5), (-1, 3), (-2, 4)]),
    ("W", [(-7, 11), (-9, 9), (-7, 7), (-5, 9)]),
    ("B", [(-7, 11), (-9, 9), (-9, 13), (-7, 15)]),
    ("R", [(-7, 11), (-7, 15), (-5, 13), (-5, 9)]),
    ("G", [(-4, -8), (-2, -6), (-1, -7), (-3, -9)]),
    ("G", [(-6, 14), (-6, 18), (-5, 19), (-5, 15)]),
    ("G", [(-6, -4), (-6, 0), (-7, 1), (-7, -3)]),
]
TS7_DEAD = {"R": (32, 36, 48), "W": (44, 44, 48), "B": (12, 30, 50), "G": (24, 24, 28)}


def ts7_polys(side=12):
    ux, uy = side * 3 ** 0.5 / 2, side / 2
    g = math.ceil(R / (21 ** 0.5 * side)) + 3
    polys, kind = [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            dx, dy = -5 * a - 2 * b, 3 * a - 12 * b
            for k, v in TS7_PIECES:
                p = [((dx + x) * ux, (dy + y) * uy) for x, y in v]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
    return polys, kind


def ts7(rng, side=12):
    polys, kind = ts7_polys(side)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng, birth=(2,), survive=(2, 3))
    colors = []
    for i in range(len(polys)):
        if alive[i]:
            c = age_color(PENROSE_AGE, age[i])
            colors.append(tuple(int(v * TS6_SHADE[kind[i]] + 0.5) for v in c))
        else:
            colors.append(TS7_DEAD[kind[i]])
    return draw_polys(polys, colors, 0)


# ---- life1_ts8: arabesque (data/tassel/arabesque-arabic.jpg), rosette spacing 96 px, B3/S23 ----

# kinds: H central hexagon, h small hexagon, T triangle, Q quad, P trapezoid; vertices in units of the spacing
TS8_PIECES = [
    ("H", [(-0.181059, -0.104534), (-0.000000, -0.209069), (0.181059, -0.104534), (0.181059, 0.104534), (0.000000, 0.209069), (-0.181059, 0.104534)]),
    ("h", [(-0.637885, -0.209067), (-0.637885, -0.368283), (-0.500000, -0.447891), (-0.362115, -0.368283), (-0.362115, -0.209067), (-0.500000, -0.129459)]),
    ("h", [(0.362115, 0.368283), (0.362115, 0.209067), (0.500000, 0.129459), (0.637885, 0.209067), (0.637885, 0.368283), (0.500000, 0.447891)]),
    ("T", [(-0.181059, -0.104534), (-0.181058, -0.313601), (-0.000000, -0.209069)]),
    ("T", [(-0.000000, -0.209069), (0.181058, -0.313601), (0.181059, -0.104534)]),
    ("T", [(0.181059, -0.104534), (0.362116, -0.000000), (0.181059, 0.104534)]),
    ("T", [(0.181059, 0.104534), (0.181058, 0.313601), (0.000000, 0.209069)]),
    ("T", [(0.000000, 0.209069), (-0.181058, 0.313601), (-0.181059, 0.104534)]),
    ("T", [(-0.181059, 0.104534), (-0.362116, 0.000000), (-0.181059, -0.104534)]),
    ("Q", [(-0.181059, -0.104534), (-0.362116, 0.000000), (-0.362115, -0.209067), (-0.181058, -0.313601)]),
    ("Q", [(-0.181058, -0.313601), (0.000000, -0.418134), (0.181058, -0.313601), (-0.000000, -0.209069)]),
    ("Q", [(0.181058, -0.313601), (0.362115, -0.209067), (0.362116, -0.000000), (0.181059, -0.104534)]),
    ("Q", [(0.181059, 0.104534), (0.362116, -0.000000), (0.362115, 0.209067), (0.181058, 0.313601)]),
    ("Q", [(0.000000, 0.209069), (0.181058, 0.313601), (-0.000000, 0.418134), (-0.181058, 0.313601)]),
    ("Q", [(-0.181058, 0.313601), (-0.362115, 0.209067), (-0.362116, 0.000000), (-0.181059, 0.104534)]),
    ("P", [(-0.362116, 0.000000), (-0.362115, 0.209067), (-0.500000, 0.129459), (-0.500000, -0.129459), (-0.362115, -0.209067)]),
    ("P", [(-0.362115, -0.368283), (-0.137885, -0.497742), (0.000000, -0.418134), (-0.181058, -0.313601), (-0.362115, -0.209067)]),
    ("P", [(0.137885, -0.497742), (0.362115, -0.368283), (0.362115, -0.209067), (0.181058, -0.313601), (0.000000, -0.418134)]),
    ("P", [(0.500000, -0.129459), (0.500000, 0.129459), (0.362115, 0.209067), (0.362116, -0.000000), (0.362115, -0.209067)]),
    ("P", [(0.362115, 0.368283), (0.137885, 0.497742), (-0.000000, 0.418134), (0.181058, 0.313601), (0.362115, 0.209067)]),
    ("P", [(-0.181058, 0.313601), (-0.000000, 0.418134), (-0.137885, 0.497742), (-0.362115, 0.368283), (-0.362115, 0.209067)]),
]
TS8_DEAD = {"H": (22, 22, 38), "h": (28, 32, 42), "T": (12, 12, 22), "Q": (16, 20, 34), "P": (24, 26, 32)}


def ts8_polys(spacing=96):
    g = math.ceil(R / (0.8 * spacing)) + 2
    polys, kind = [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            cx, cy = a + 0.5 * b, b * 3 ** 0.5 / 2
            for k, v in TS8_PIECES:
                p = [((cx + x) * spacing, (cy + y) * spacing) for x, y in v]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
    return polys, kind


def touch_neighbors_fast(polys, tol=0.05):
    vmap = defaultdict(set)
    for t, p in enumerate(polys):
        for x, y in p:
            vmap[(round(x / tol), round(y / tol))].add(t)
    nb = [set() for _ in polys]
    for (kx, ky), ts in list(vmap.items()):
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for t in ts:
                    nb[t] |= vmap.get((kx + dx, ky + dy), set()) - {t}
    return [list(n) for n in nb]


def ts8(rng, spacing=96):
    polys, kind = ts8_polys(spacing)
    nb = touch_neighbors_fast(polys)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else TS8_DEAD[kind[i]] for i in range(len(polys))]
    return draw_polys(polys, colors, 2 * max(0.55, spacing / 56))


# ---- life1_ts9: isometric L pieces and small rhombi (data/tassel/fake-3d4.jpg), side 14 px, B3/S23 ----

TS9_PIECES = [
    ("W", [(-1, -3), (0, -2), (1, -3), (3, -1), (5, -3), (4, -4), (3, -3), (1, -5)]),
    ("L", [(-4, -6), (-4, -4), (-3, -3), (-3, 1), (-1, 3), (-1, 1), (-2, 0), (-2, -4)]),
    ("D", [(-2, -4), (0, -6), (0, -8), (1, -9), (1, -5), (-1, -3), (-1, -1), (-2, 0)]),
    ("W", [(0, 0), (1, 1), (2, 0), (1, -1)]),
    ("W", [(-2, 0), (-1, 1), (0, 0), (-1, -1)]),
    ("L", [(-1, -3), (-1, -1), (0, 0), (0, -2)]),
    ("L", [(0, 0), (0, 2), (1, 3), (1, 1)]),
    ("D", [(0, -2), (1, -3), (1, -1), (0, 0)]),
    ("D", [(3, -5), (4, -6), (4, -4), (3, -3)]),
]
TS9_DEAD = {"W": (44, 44, 48), "L": (28, 32, 42), "D": (14, 24, 42)}
TS9_SHADE = {"W": 1.0, "L": 0.80, "D": 0.60}


def ts9_polys(side=14):
    ux, uy = side * 3 ** 0.5 / 2, side / 2
    g = math.ceil(R / (21 ** 0.5 * side)) + 3
    polys, kind = [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            dx, dy = 5 * a - 4 * b, 3 * a + 6 * b
            for k, v in TS9_PIECES:
                p = [((dx + x) * ux, (dy + y) * uy) for x, y in v]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
    return polys, kind


def ts9(rng, side=14):
    polys, kind = ts9_polys(side)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    colors = []
    for i in range(len(polys)):
        if alive[i]:
            c = age_color(PENROSE_AGE, age[i])
            colors.append(tuple(int(v * TS9_SHADE[kind[i]] + 0.5) for v in c))
        else:
            colors.append(TS9_DEAD[kind[i]])
    return draw_polys(polys, colors, 0)


# ---- life1_ts10: curved tiles (data/tassel/hexTongue.jpg), lattice spacing 37 px, B3/S23 ----

# outline (60 points, units of the lattice spacing, around the tile centroid) and centroids of the three orientations
TS10_TILE = [
    (0.19202, -0.42659),
    (0.14238, -0.41732),
    (0.09414, -0.40254),
    (0.04718, -0.38172),
    (0.00170, -0.35693),
    (-0.04238, -0.32980),
    (-0.08425, -0.29964),
    (-0.12443, -0.26855),
    (-0.13992, -0.22590),
    (-0.13209, -0.17525),
    (-0.12718, -0.12417),
    (-0.12609, -0.07262),
    (-0.12621, -0.02137),
    (-0.13173, 0.02969),
    (-0.14358, 0.07923),
    (-0.16089, 0.12602),
    (-0.18303, 0.17109),
    (-0.20961, 0.21458),
    (-0.24362, 0.25194),
    (-0.28355, 0.28367),
    (-0.33052, 0.30387),
    (-0.38080, 0.31197),
    (-0.42137, 0.28599),
    (-0.45704, 0.27710),
    (-0.46213, 0.32463),
    (-0.43226, 0.36528),
    (-0.38890, 0.39361),
    (-0.34242, 0.41507),
    (-0.29387, 0.42790),
    (-0.24243, 0.43111),
    (-0.19202, 0.42659),
    (-0.14238, 0.41732),
    (-0.09414, 0.40254),
    (-0.04718, 0.38172),
    (-0.00170, 0.35693),
    (0.04238, 0.32980),
    (0.08425, 0.29964),
    (0.12443, 0.26855),
    (0.13992, 0.22590),
    (0.13209, 0.17525),
    (0.12718, 0.12417),
    (0.12609, 0.07262),
    (0.12621, 0.02137),
    (0.13173, -0.02969),
    (0.14358, -0.07923),
    (0.16089, -0.12602),
    (0.18303, -0.17109),
    (0.20961, -0.21458),
    (0.24362, -0.25194),
    (0.28355, -0.28367),
    (0.33052, -0.30387),
    (0.38080, -0.31197),
    (0.42137, -0.28599),
    (0.45704, -0.27710),
    (0.46213, -0.32463),
    (0.43226, -0.36528),
    (0.38890, -0.39361),
    (0.34242, -0.41507),
    (0.29387, -0.42790),
    (0.24243, -0.43111),
]
TS10_POS = [(-0.43254, -0.75074), (0.00040, -0.49922), (-0.43389, -0.25004)]
TS10_DEAD = [(12, 12, 22), (22, 22, 38), (28, 32, 42)]


def ts10_polys(spacing=37):
    g = math.ceil(R / (0.8 * spacing)) + 2
    polys, kind = [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            lx, ly = b * 3 ** 0.5 / 2, a + 0.5 * b
            for k in range(3):
                an = k * 2 * math.pi / 3
                ca, sa = math.cos(an), math.sin(an)
                p = [((lx + TS10_POS[k][0] + x * ca - y * sa) * spacing, (ly + TS10_POS[k][1] + x * sa + y * ca) * spacing)
                     for x, y in TS10_TILE]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
    return polys, kind


def ts10_neighbors(polys, spacing):
    """Tiles whose outlines come closer than spacing / 10 (as in the firmware)."""
    import numpy as np
    P = [np.array(p) for p in polys]
    cen = np.array([p.mean(0) for p in P])
    nb = [[] for _ in P]
    for i in range(len(P)):
        for j in range(i + 1, len(P)):
            if np.hypot(*(cen[i] - cen[j])) > 1.6 * spacing:
                continue
            d = np.min(np.hypot(*(P[i][:, None, :] - P[j][None, :, :]).transpose(2, 0, 1)))
            if d < spacing / 10:
                nb[i].append(j)
                nb[j].append(i)
    return nb


def ts10(rng, spacing=37):
    import numpy as np
    polys, kind = ts10_polys(spacing)
    nb = ts10_neighbors(polys, spacing)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else TS10_DEAD[kind[i]] for i in range(len(polys))]
    # pixel map as in the firmware: tiles, holes filled from the 4 neighbors (2 passes), no line between tiles
    idx = Image.new("I", (RES, RES), -1)
    d = ImageDraw.Draw(idx)
    for t, p in enumerate(polys):
        d.polygon([(x + R, y + R) for x, y in p], fill=t)
    m = np.array(idx)
    ys, xs = np.mgrid[0:RES, 0:RES]
    inc = np.hypot(xs + 0.5 - R, ys + 0.5 - R) <= R
    for _ in range(2):
        new = m.copy()
        for dy, dx in ((0, -1), (0, 1), (-1, 0), (1, 0)):
            sh = np.roll(np.roll(m, dy, 0), dx, 1)
            sel = (new < 0) & inc & (sh >= 0)
            new[sel] = sh[sel]
        m = new
    rgb = np.array(colors, float)[np.where(m >= 0, m, 0)]
    rgb[m < 0] = 0
    rgb[~inc] = 0
    return Image.fromarray(rgb.astype(np.uint8), "RGB")


# ---- life1_ts11: rhombic frames, small pieces and triangles (data/tassel/fake_3d5.jpg), side 12 px, B2/S23 ----

# (kind, triangles, vertices); x in half sides, y in triangle heights; frames: outer and inner outline joined by a cut
TS11_PIECES = [
    ("L", 32, [(-5, 5), (-15, 5), (-10, 0), (0, 0), (-5, 5), (-6, 4), (-5, 3), (-4, 2), (-3, 1), (-9, 1), (-12, 4), (-10, 4), (-8, 4), (-6, 4)]),
    ("M", 32, [(-5, -5), (0, -10), (5, -5), (0, 0), (-5, -5), (-3, -5), (-2, -4), (-1, -3), (0, -2), (3, -5), (0, -8), (-1, -7), (-2, -6), (-3, -5)]),
    ("D", 32, [(-10, 0), (-15, -5), (-5, -5), (0, 0), (-10, 0), (-9, -1), (-3, -1), (-4, -2), (-5, -3), (-6, -4), (-8, -4), (-10, -4), (-12, -4), (-9, -1)]),
    ("L", 5, [(-9, -1), (-8, -2), (-6, -2), (-4, -2), (-3, -1)]),
    ("L", 5, [(0, -2), (-1, -3), (0, -4), (1, -5), (3, -5)]),
    ("M", 5, [(-8, -2), (-9, -1), (-12, -4), (-10, -4), (-9, -3)]),
    ("M", 5, [(-9, 1), (-8, 2), (-9, 3), (-10, 4), (-12, 4)]),
    ("D", 5, [(-3, 1), (-4, 2), (-6, 2), (-8, 2), (-9, 1)]),
    ("D", 5, [(0, 2), (3, 5), (1, 5), (0, 4), (-1, 3)]),
    ("L", 3, [(-2, 4), (0, 4), (1, 5), (-3, 5)]),
    ("L", 3, [(-8, -2), (-9, -3), (-8, -4), (-6, -4)]),
    ("M", 3, [(-6, -4), (-5, -3), (-6, -2), (-8, -2)]),
    ("M", 3, [(-5, 3), (-6, 4), (-8, 2), (-6, 2)]),
    ("D", 3, [(1, -5), (0, -4), (-2, -4), (-3, -5)]),
    ("D", 3, [(-8, 2), (-6, 4), (-8, 4), (-9, 3)]),
    ("W", 1, [(-6, 2), (-4, 2), (-5, 3)]),
    ("W", 1, [(-5, -3), (-4, -2), (-6, -2)]),
    ("W", 1, [(0, -4), (-1, -3), (-2, -4)]),
    ("W", 1, [(0, 4), (-2, 4), (-1, 3)]),
    ("W", 1, [(-8, -4), (-9, -3), (-10, -4)]),
    ("W", 1, [(-8, 4), (-10, 4), (-9, 3)]),
]
TS11_DEAD = {"W": (40, 42, 50), "L": (30, 32, 40), "M": (20, 24, 34), "D": (12, 16, 28)}
TS11_SHADE = {"W": 1.0, "L": 0.85, "M": 0.70, "D": 0.55}


def ts11_polys(side=12):
    ux, uy = side / 2, side * 3 ** 0.5 / 2
    g = math.ceil(R / (7.5 * side)) + 2
    polys, kind, ntri = [], [], []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            dx, dy = 15 * b, 10 * a + 5 * b
            for k, n, v in TS11_PIECES:
                p = [((dx + x) * ux, (dy + y) * uy) for x, y in v]
                if centroid_in(p):
                    polys.append(p)
                    kind.append(k)
                    ntri.append(n)
    return polys, kind, ntri


def ts11(rng, side=12):
    polys, kind, _ = ts11_polys(side)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng, birth=(2,), survive=(2, 3))
    colors = []
    for i in range(len(polys)):
        if alive[i]:
            c = age_color(PENROSE_AGE, age[i])
            colors.append(tuple(int(v * TS11_SHADE[kind[i]] + 0.5) for v in c))
        else:
            colors.append(TS11_DEAD[kind[i]])
    return draw_polys(polys, colors, 0)


# ---- life1_ts12: Penrose kites and darts (P2) by deflation, long edge 28 px, B3/S23 ----

def p2_polys(edge=28):
    """Kites and darts: halves deflated from a sun of 5 kites, paired on their axis AC (as in the firmware)."""
    def visible(t):
        g = (t[1] + t[2] + t[3]) / 3
        return abs(g) - max(abs(v - g) for v in t[1:]) <= R + 1

    def hd(A, B, C):
        P = A + (B - A) / PHI
        return [(0, A, P, C), (1, B, C, P)]

    def hk(A, B, C):
        P = A + (C - A) / PHI
        return hd(B, A, P) + [(0, B, C, P)]

    r0 = R / math.cos(math.pi / 10) + 2 * edge
    steps = math.ceil(math.log(r0 / edge) / math.log(PHI))
    r = edge * PHI ** steps
    tris = []
    for i in range(10):
        z0 = complex(r * math.cos(i * math.pi / 5), r * math.sin(i * math.pi / 5))
        z1 = complex(r * math.cos((i + 1) * math.pi / 5), r * math.sin((i + 1) * math.pi / 5))
        tris.append((0, 0j, z0, z1) if i % 2 == 0 else (0, 0j, z1, z0))
    for _ in range(steps):
        out = []
        for t in tris:
            out += hk(*t[1:]) if t[0] == 0 else hd(*t[1:])
        tris = [t for t in out if visible(t)]
    halves = defaultdict(list)
    for t in tris:
        k = tuple(sorted([(round(t[1].real * 8), round(t[1].imag * 8)), (round(t[3].real * 8), round(t[3].imag * 8))]))
        halves[(k, t[0])].append(t)
    polys, kite = [], []
    for (_, color), ts in halves.items():
        a = ts[0]
        q = [a[2], a[1], ts[1][2], a[3]] if len(ts) == 2 else [a[1], a[2], a[3]]
        p = [(v.real, v.imag) for v in q]
        if centroid_in(p):
            polys.append(p)
            kite.append(color == 0)
    return polys, kite


def ts12(rng, edge=28):
    polys, kite = p2_polys(edge)
    nb = neighbors_by_vertex(polys)
    alive, age = run_life(nb, rng)
    colors = [age_color(PENROSE_AGE, age[i]) if alive[i] else ((22, 22, 38) if kite[i] else (12, 12, 22))
              for i in range(len(polys))]
    return draw_polys(polys, colors, border_line(edge, 18, 32))


# RNG seed per experiment for the snapshot (default 7); ts9 with seed 7 has only 8 live tiles left after 9 generations
SEEDS = {"life1_ts9": 9}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "assets/previews"
    os.makedirs(out, exist_ok=True)
    for name, fn in (("life1_conway", conway), ("life1_penrose", penrose), ("life1_ts1", ts1), ("life1_ts2", ts2), ("life1_ts3", ts3), ("life1_ts4", ts4), ("life1_ts5", ts5), ("life1_ts6", ts6), ("life1_ts7", ts7), ("life1_ts8", ts8), ("life1_ts9", ts9), ("life1_ts10", ts10), ("life1_ts11", ts11), ("life1_ts12", ts12)):
        path = os.path.join(out, name + ".png")
        fn(random.Random(SEEDS.get(name, 7))).save(path)
        print(path)


if __name__ == "__main__":
    main()
