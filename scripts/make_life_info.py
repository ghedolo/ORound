#!/usr/bin/env python3
"""Info images for the life1_* experiments, written to firmware/<name>/info/.

Usage: python scripts/make_life_info.py <name> [<name> ...]   (e.g. life1_conway)

For each experiment:
  preview.png  display preview with the default tile size (same as assets/previews)
  tiles.png    the tile shapes, separated, drawn in green on black with their sizes
The rules text (regole.md) is written by hand in the same folder.
"""
import math
import os
import random
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_life_previews as prev  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GREEN = (60, 255, 70)
GREEN_DIM = (30, 120, 35)
TEXT = (210, 210, 210)
FONT_PATH = "/System/Library/Fonts/Supplemental/Arial.ttf"


def font(size):
    try:
        return ImageFont.truetype(FONT_PATH, size)
    except OSError:
        return ImageFont.load_default()


def conway_tiles():
    """One tile type: rounded square inside a 20 px cell, shown large and as a 3x3 block."""
    cell, gap, corner = 20, 2, 6
    im = Image.new("RGB", (1120, 620), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)

    # single tile, 20x scale: cell frame (dim) and rounded square (green)
    k = 20
    x0, y0 = 60, 80
    d.rectangle((x0, y0, x0 + cell * k, y0 + cell * k), outline=GREEN_DIM, width=2)
    d.rounded_rectangle((x0 + gap * k, y0 + gap * k, x0 + (cell - gap) * k, y0 + (cell - gap) * k),
                        radius=corner * k, outline=GREEN, width=4)
    d.text((x0, 30), "cella 20 x 20 px (1,44 mm)", fill=TEXT, font=f)
    d.text((x0, y0 + cell * k + 14), f"quadrato {cell - 2 * gap} px, angoli raggio {corner} px,", fill=TEXT, font=fs)
    d.text((x0, y0 + cell * k + 40), f"spazio nero {gap} px per lato", fill=TEXT, font=fs)

    # 3x3 block, 8x scale: neighbors of the center cell
    k = 8
    x0, y0 = 590, 80
    for gy in range(3):
        for gx in range(3):
            cx, cy = x0 + gx * cell * k, y0 + gy * cell * k
            col = GREEN if (gx, gy) == (1, 1) else GREEN_DIM
            d.rounded_rectangle((cx + gap * k, cy + gap * k, cx + (cell - gap) * k, cy + (cell - gap) * k),
                                radius=corner * k, outline=col, width=3)
    d.text((x0, 30), "vicine: le 8 celle intorno", fill=TEXT, font=f)
    d.text((x0, y0 + 3 * cell * k + 14), "griglia 23 x 23, solo celle tutte dentro il cerchio", fill=TEXT, font=fs)
    return im


def rhombus(d, cx, cy, edge, angle_deg, k, col, width):
    """Rhombus with the given acute angle, long diagonal horizontal, centered on (cx, cy), scale k."""
    a = math.radians(angle_deg) / 2
    hx, hy = edge * math.cos(a) * k, edge * math.sin(a) * k
    pts = [(cx - hx, cy), (cx, cy - hy), (cx + hx, cy), (cx, cy + hy)]
    d.polygon(pts, outline=col, width=width)
    return pts


def neighbor_view(d, polys, nb, center, x0, y0, size, k):
    """Draws tile `center` (bright) and its neighbors (dim), scaled by k, in a size x size box."""
    cx = sum(x for x, _ in polys[center]) / len(polys[center])
    cy = sum(y for _, y in polys[center]) / len(polys[center])
    for t in nb[center] + [center]:
        # each tile shrunk by 10% toward its own center, so the tiles appear separated
        tx = sum(x for x, _ in polys[t]) / len(polys[t])
        ty = sum(y for _, y in polys[t]) / len(polys[t])
        pts = [(x0 + size / 2 + (tx + (x - tx) * 0.9 - cx) * k, y0 + size / 2 + (ty + (y - ty) * 0.9 - cy) * k)
               for x, y in polys[t]]
        d.polygon(pts, outline=GREEN if t == center else GREEN_DIM, width=4 if t == center else 3)


def pick_center(polys, nb, want=None):
    """Tile closest to the display center, optionally with a given number of neighbors."""
    best = None
    for i, p in enumerate(polys):
        if want is not None and len(nb[i]) != want:
            continue
        r = math.hypot(sum(x for x, _ in p) / len(p), sum(y for _, y in p) / len(p))
        if best is None or r < best[0]:
            best = (r, i)
    return best[1]


def penrose_tiles():
    """Thin and thick rhombus with their angles, and one tile with its vertex neighbors."""
    edge = 24
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    k = 9

    d.text((60, 30), f"lato {edge} px (1,73 mm), due tipi di rombo", fill=TEXT, font=f)
    rhombus(d, 300, 170, edge, 36, k, GREEN, 4)
    d.text((60, 250), "rombo sottile: angoli 36 e 144 gradi", fill=TEXT, font=fs)
    d.text((60, 276), f"area {edge * edge * math.sin(math.radians(36)):.0f} px2, 140 tessere", fill=TEXT, font=fs)
    rhombus(d, 300, 430, edge, 72, k, GREEN, 4)
    d.text((60, 560), "rombo largo: angoli 72 e 108 gradi", fill=TEXT, font=fs)
    d.text((60, 586), f"area {edge * edge * math.sin(math.radians(72)):.0f} px2, 220 tessere", fill=TEXT, font=fs)

    polys, _ = prev.penrose_polys(edge)
    nb = prev.neighbors_by_vertex(polys)
    c = pick_center(polys, nb, want=10)
    d.text((600, 30), "vicine: tessere con un vertice in comune", fill=TEXT, font=f)
    neighbor_view(d, polys, nb, c, 600, 70, 480, 4.2)
    d.text((600, 586), "da 4 a 11 vicine (qui 10); 360 tessere in tutto", fill=TEXT, font=fs)
    return im


def ts1_polys(size=28):
    g = math.ceil(prev.R / size) + 1
    polys = []
    for j in range(-g, g + 1):
        for i in range(-g, g + 1):
            even = (i + j) % 2 == 0
            for p in (prev.arrow(i, j, 0, size) if even else prev.arrow(i, j + 1, 2, size),
                      prev.arrow(i + 1, j, 1, size) if even else prev.arrow(i, j, 3, size)):
                if prev.centroid_in(p):
                    polys.append(p)
    return polys


def ts2_polys(edge=14):
    unit = edge / 2
    g = math.ceil(prev.R / (2 * edge)) + 1
    polys = []
    for b in range(-g, g + 1):
        for a in range(-g, g + 1):
            for side in (-1, 1):
                p = prev.penta(4 * a, 4 * b, (a + b) % 2 == 0, side, unit)
                if prev.centroid_in(p):
                    polys.append(p)
    return polys


def ts3_polys(side=16):
    ux, uy = side / 2, side * 3 ** 0.5 / 2
    g = math.ceil(prev.R / side) + 1
    polys = []
    for j in range(-g, g + 1):
        for i in range(-g, g + 1):
            for (cx, cy), v in (((4 * i, 2 * j), prev.HEX), ((4 * i + 2, 2 * j + 1), prev.RHOMB)):
                p = [((cx + x) * ux, (cy + y) * uy) for x, y in v]
                if prev.centroid_in(p):
                    polys.append(p)
    return polys


def ts4_polys(level=3):
    out = []
    prev.flake(level, 0, 0, prev.R, -math.pi / 2, False, out)
    polys = [p for p, _ in out]
    return polys + prev.flake_rhombi(polys)


def ts5_polys(side=12):
    ux, uy = side * 3 ** 0.5 / 2, side / 2
    g = math.ceil(prev.R / (1.5 * side)) + 1
    polys = []
    for r in range(-g, g + 1):
        for k in range(-g, g + 1):
            hx, hy = 2 * k + (r & 1), 3 * r
            if math.hypot(hx * ux, hy * uy) <= prev.R:
                polys.append([((hx + x) * ux, (hy + y) * uy) for x, y in prev.HEX_P])
    return polys


def touch_neighbors(polys, tol=0.05):
    """Vertex neighbors with a distance tolerance (for coordinates that are not exact)."""
    nb = [[] for _ in polys]
    cen = [(sum(x for x, _ in p) / len(p), sum(y for _, y in p) / len(p)) for p in polys]
    for i in range(len(polys)):
        for j in range(i + 1, len(polys)):
            if math.dist(cen[i], cen[j]) > 60:
                continue
            if any(abs(a[0] - b[0]) < tol and abs(a[1] - b[1]) < tol for a in polys[i] for b in polys[j]):
                nb[i].append(j)
                nb[j].append(i)
    return nb


def draw_shape(d, pts, cx, cy, k, col=GREEN, width=4):
    """Polygon scaled by k, its vertex mean placed at (cx, cy)."""
    mx = sum(x for x, _ in pts) / len(pts)
    my = sum(y for _, y in pts) / len(pts)
    d.polygon([(cx + (x - mx) * k, cy + (y - my) * k) for x, y in pts], outline=col, width=width)


def area(p):
    return abs(sum(p[i][0] * p[(i + 1) % len(p)][1] - p[(i + 1) % len(p)][0] * p[i][1] for i in range(len(p)))) / 2


def most_neighbors(polys, nb, n_vert=None):
    """Tile near the center with the largest number of neighbors (optionally with n_vert vertices)."""
    cand = [i for i in range(len(polys)) if n_vert is None or len(polys[i]) == n_vert]
    top = max(len(nb[i]) for i in cand)
    return pick_center([polys[i] if i in cand and len(nb[i]) == top else [(1e9, 1e9)] for i in range(len(polys))],
                       nb), top


def ts1_tiles():
    a = 28
    polys = ts1_polys(a)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    d.text((60, 30), f"freccia lunga {a} px (2,02 mm)", fill=TEXT, font=f)
    draw_shape(d, prev.arrow(0, 0, 0, a), 280, 300, 12)
    d.text((60, 530), "8 vertici, non convessa; dalla punta alla tacca della coda = lato della griglia", fill=TEXT, font=fs)
    d.text((60, 556), f"area {a * a / 2:.0f} px2; 220 verticali, 216 orizzontali (stessa forma ruotata)", fill=TEXT, font=fs)
    c, top = most_neighbors(polys, nb)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    neighbor_view(d, polys, nb, c, 620, 80, 480, 4.6)
    d.text((620, 586), f"{top} vicine lontano dal bordo; 436 tessere", fill=TEXT, font=fs)
    return im


def ts2_tiles():
    e = 14
    polys = ts2_polys(e)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    d.text((60, 30), f"pentagono, lato corto {e} px (1,01 mm)", fill=TEXT, font=f)
    u = e / 2
    left = [(x * u, y * u) for x, y in prev.PENTA]
    draw_shape(d, left, 170, 230, 10)
    d.text((60, 430), f"lato corto {e} px, altri 4 lati {e * 5 ** 0.5 / 2:.1f} px".replace(".", ","), fill=TEXT, font=fs)
    d.text((60, 456), "angoli 116,6 / 90 / 126,9 / 90 / 116,6 gradi", fill=TEXT, font=fs)
    d.text((60, 482), f"area {2 * e * e} px2; a destra una coppia verticale", fill=TEXT, font=fs)
    d.text((60, 508), "(le coppie orizzontali sono la stessa coppia ruotata)", fill=TEXT, font=fs)
    k = 6
    for side in (-1, 1):
        p = prev.penta(0, 0, True, side, u)
        tx = sum(x for x, _ in p) / 5
        d.polygon([(440 + (tx + (x - tx) * 0.9) * k, 230 + y * k) for x, y in p], outline=GREEN_DIM, width=3)
    c, top = most_neighbors(polys, nb)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    neighbor_view(d, polys, nb, c, 620, 80, 480, 5.2)
    d.text((620, 586), f"{top} vicine lontano dal bordo; 436 tessere", fill=TEXT, font=fs)
    return im


def ts3_tiles():
    s = 16
    polys = ts3_polys(s)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    ux, uy = s / 2, s * 3 ** 0.5 / 2
    d.text((60, 30), f"lato {s} px (1,15 mm)", fill=TEXT, font=f)
    draw_shape(d, [(x * ux, y * uy) for x, y in prev.HEX], 190, 200, 7)
    d.text((60, 330), f"esagono regolare, area {2.598 * s * s:.0f} px2, 199 tessere", fill=TEXT, font=fs)
    draw_shape(d, [(x * ux, y * uy) for x, y in prev.RHOMB], 190, 470, 7)
    d.text((60, 586), f"rombo 60 / 120 gradi, area {0.866 * s * s:.0f} px2, 192 tessere", fill=TEXT, font=fs)
    d.text((560, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    ch, th = most_neighbors(polys, nb, 6)
    neighbor_view(d, polys, nb, ch, 520, 90, 300, 3.6)
    d.text((570, 400), f"esagono: {th} vicine", fill=TEXT, font=fs)
    cr, tr = most_neighbors(polys, nb, 4)
    neighbor_view(d, polys, nb, cr, 820, 90, 300, 3.6)
    d.text((850, 400), f"rombo: {tr} vicine", fill=TEXT, font=fs)
    d.text((560, 586), "391 tessere", fill=TEXT, font=fs)
    return im


def ts4_tiles():
    polys = ts4_polys(3)
    nb = touch_neighbors(polys)
    side = math.dist(polys[0][0], polys[0][1])
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    d.text((60, 30), f"livello 3, lato {side:.1f} px (1,10 mm)".replace(".", ","), fill=TEXT, font=f)
    pent = [p for p in polys if len(p) == 5]
    rh = [p for p in polys if len(p) == 4]
    draw_shape(d, pent[0], 190, 210, 11)
    d.text((60, 330), f"pentagono regolare, area {area(pent[0]):.0f} px2, 216 tessere", fill=TEXT, font=fs)
    draw_shape(d, rh[0], 190, 470, 11)
    d.text((60, 586), f"rombo 36 / 144 gradi, area {area(rh[0]):.0f} px2, 40 tessere", fill=TEXT, font=fs)
    d.text((560, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    cp, tp = most_neighbors(polys, nb, 5)
    neighbor_view(d, polys, nb, cp, 520, 100, 300, 3.6)
    d.text((540, 430), f"pentagono: {tp} vicine (da 3 a 10)", fill=TEXT, font=fs)
    cr, tr = most_neighbors(polys, nb, 4)
    neighbor_view(d, polys, nb, cr, 820, 100, 300, 3.6)
    d.text((880, 430), f"rombo: {tr} vicine", fill=TEXT, font=fs)
    d.text((560, 586), "i buchi a stella non sono tessere", fill=TEXT, font=fs)
    return im


def ts5_tiles():
    s = 12
    polys = ts5_polys(s)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    ux, uy = s * 3 ** 0.5 / 2, s / 2
    hexagon = [(x * ux, y * uy) for x, y in prev.HEX_P]
    d.text((60, 30), f"esagono, lato {s} px (0,86 mm)", fill=TEXT, font=f)
    draw_shape(d, hexagon, 170, 190, 9)
    d.text((60, 320), f"area {2.598 * s * s:.0f} px2, 451 tessere", fill=TEXT, font=fs)
    # stripe fills: one hexagon per 30 degree step, stripes as green lines
    d.text((60, 380), "righe: +30 gradi per esagono verso destra", fill=TEXT, font=fs)
    k = 3.2
    period = max(2.5, s * 3 ** 0.5 / 7.5) * k
    for n in range(6):
        cx, cy = 90 + n * 82, 500
        a = math.radians(30 * n)
        pts = [(cx + x * k, cy + y * k) for x, y in hexagon]
        mask = Image.new("L", im.size, 0)
        ImageDraw.Draw(mask).polygon(pts, fill=255)
        lines = Image.new("RGB", im.size, (0, 0, 0))
        dl = ImageDraw.Draw(lines)
        for m in range(-8, 9):
            off = m * period
            ox, oy = -math.sin(a) * off, math.cos(a) * off
            dl.line([(cx + ox - math.cos(a) * 60, cy + oy - math.sin(a) * 60),
                     (cx + ox + math.cos(a) * 60, cy + oy + math.sin(a) * 60)], fill=GREEN_DIM, width=2)
        im.paste(lines, (0, 0), mask)
        d.polygon(pts, outline=GREEN, width=2)
        d.text((cx - 14, cy + 50), f"{30 * n}", fill=TEXT, font=fs)
    c, top = most_neighbors(polys, nb)
    d.text((620, 30), "vicine: un lato in comune", fill=TEXT, font=f)
    neighbor_view(d, polys, nb, c, 620, 70, 460, 5.5)
    d.text((620, 586), f"{top} vicine lontano dal bordo", fill=TEXT, font=fs)
    return im


def ts6_tiles():
    a = 12
    polys, kind = prev.ts6_polys(a)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    ux, uy = a * 3 ** 0.5 / 2, a / 2
    d.text((60, 30), f"rombo di base: lato {a} px (0,86 mm)", fill=TEXT, font=f)
    names = {"R": "rosso", "W": "bianco", "B": "blu"}
    for n, (k, v) in enumerate(p for p in prev.TS6_PIECES if p[0] != "G"):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 130 + n * 170, 190, 3.0)
        d.text((80 + n * 170, 320), f"pezzo {names[k]}", fill=TEXT, font=fs)
    d.text((60, 356), f"pezzi a L: 8 vertici, area {10 * 0.433 * a * a:.0f} px2, 65 per colore", fill=TEXT, font=fs)
    for n, (k, v) in enumerate(p for p in prev.TS6_PIECES if p[0] == "G"):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 130 + n * 170, 470, 3.4)
    d.text((60, 560), f"parallelogrammi grigi 60 / 120 gradi, lati {a} e {2 * a} px,", fill=TEXT, font=fs)
    d.text((60, 586), f"area {4 * 0.433 * a * a:.0f} px2, 192 tessere", fill=TEXT, font=fs)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    cl, tl = most_neighbors(polys, nb, 8)
    neighbor_view(d, polys, nb, cl, 600, 80, 260, 2.3)
    d.text((620, 400), f"pezzo a L: {tl} vicine", fill=TEXT, font=fs)
    cg, tg = most_neighbors(polys, nb, 4)
    neighbor_view(d, polys, nb, cg, 860, 80, 260, 2.3)
    d.text((880, 400), f"parallelogramma: {tg} vicine", fill=TEXT, font=fs)
    d.text((620, 586), "387 tessere in tutto", fill=TEXT, font=fs)
    return im


def ts7_tiles():
    a = 12
    polys, kind = prev.ts7_polys(a)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    ux, uy = a * 3 ** 0.5 / 2, a / 2
    d.text((60, 30), f"rombo di base: lato {a} px (0,86 mm)", fill=TEXT, font=f)
    names = {"R": "magenta", "W": "bianco", "B": "blu"}
    for n, (k, v) in enumerate(prev.TS7_PIECES[:3]):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 130 + n * 170, 170, 3.0)
        d.text((80 + n * 170, 270), f"pezzo {names[k]}", fill=TEXT, font=fs)
    d.text((60, 300), f"pezzi a L: area {10 * 0.433 * a * a:.0f} px2", fill=TEXT, font=fs)
    # cube: the three faces separated, in place
    for k, v in prev.TS7_PIECES[3:6]:
        q = [(x + 7, y - 11) for x, y in v]
        neighbor = [(x * ux, y * uy) for x, y in q]
        cx = sum(x for x, _ in neighbor) / 4
        cy = sum(y for _, y in neighbor) / 4
        d.polygon([(150 + (cx + (x - cx) * 0.9) * 3.0, 450 + (cy + (y - cy) * 0.9) * 3.0) for x, y in neighbor],
                  outline=GREEN, width=3)
    d.text((60, 560), f"cubo: 3 rombi di lato {2 * a} px, area {8 * 0.433 * a * a:.0f} px2 ciascuno", fill=TEXT, font=fs)
    for n, (k, v) in enumerate(prev.TS7_PIECES[6:]):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 330 + n * 75, 450, 3.0)
    d.text((300, 520), f"parallelogrammi, area {4 * 0.433 * a * a:.0f} px2", fill=TEXT, font=fs)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    cl, tl = most_neighbors(polys, nb, 8)
    neighbor_view(d, polys, nb, cl, 600, 80, 260, 2.3)
    d.text((620, 400), f"pezzo a L: {tl} vicine", fill=TEXT, font=fs)
    cands = [i for i in range(len(polys)) if len(polys[i]) == 4 and area(polys[i]) > 6 * 0.433 * a * a]
    big = max(len(nb[i]) for i in cands)
    cc = pick_center([p if (i in cands and len(nb[i]) == big) else [(1e9, 1e9)] for i, p in enumerate(polys)], nb)
    neighbor_view(d, polys, nb, cc, 860, 80, 260, 2.3)
    d.text((880, 400), f"faccia del cubo: {big} vicine", fill=TEXT, font=fs)
    d.text((620, 586), "371 tessere; vicine da 1 a 10 (bordo compreso)", fill=TEXT, font=fs)
    return im


def ts8_tiles():
    sp = 96
    polys, kind = prev.ts8_polys(sp)
    nb = prev.touch_neighbors_fast(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    d.text((60, 30), f"passo delle rosette {sp} px (6,9 mm)", fill=TEXT, font=f)
    names = [("H", "esagono centrale"), ("T", "triangolo"), ("Q", "quadrilatero"), ("P", "trapezio"), ("h", "esagono piccolo")]
    for n, (k, label) in enumerate(names):
        v = next(v for kk, v in prev.TS8_PIECES if kk == k)
        p = [(x * sp, y * sp) for x, y in v]
        cx, cy = 130 + (n % 3) * 170, 180 + (n // 3) * 230
        draw_shape(d, p, cx, cy, 4.0)
        d.text((cx - 70, cy + 80), f"{label}", fill=TEXT, font=fs)
        d.text((cx - 70, cy + 104), f"area {area(p):.0f} px2", fill=TEXT, font=fs)
    d.text((60, 586), "per periodo: 1 + 6 + 6 + 6 + 2 = 21 tessere", fill=TEXT, font=fs)
    d.text((620, 30), "rosetta e vicine (un vertice in comune)", fill=TEXT, font=f)
    c = pick_center(polys, nb, want=12)
    neighbor_view(d, polys, nb, c, 600, 70, 260, 2.2)
    d.text((620, 400), f"esagono centrale: {len(nb[c])} vicine", fill=TEXT, font=fs)
    ct = pick_center(polys, nb, want=9)
    neighbor_view(d, polys, nb, ct, 860, 70, 260, 2.2)
    d.text((880, 400), f"esagono piccolo: {len(nb[ct])} vicine", fill=TEXT, font=fs)
    d.text((620, 586), "451 tessere; vicine da 2 a 12 (bordo compreso)", fill=TEXT, font=fs)
    return im


def ts9_tiles():
    a = 14
    polys, kind = prev.ts9_polys(a)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    ux, uy = a * 3 ** 0.5 / 2, a / 2
    d.text((60, 30), f"rombo di base: lato {a} px (1,01 mm)", fill=TEXT, font=f)
    names = {"W": "bianco", "L": "grigio chiaro", "D": "grigio scuro"}
    for n, (k, v) in enumerate(prev.TS9_PIECES[:3]):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 130 + n * 170, 190, 3.0)
        d.text((80 + n * 170, 300), f"pezzo {names[k]}", fill=TEXT, font=fs)
    d.text((60, 336), f"pezzi a L: 8 vertici, area {10 * 0.433 * a * a:.0f} px2, 48 per tono", fill=TEXT, font=fs)
    for n, (k, v) in enumerate(prev.TS9_PIECES[3:]):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 90 + n * 80, 470, 3.0)
    d.text((60, 560), f"rombi 60 / 120 gradi, lato {a} px, area {2 * 0.433 * a * a:.0f} px2,", fill=TEXT, font=fs)
    d.text((60, 586), "6 per periodo (2 per tono), 294 in tutto", fill=TEXT, font=fs)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    cl, tl = most_neighbors(polys, nb, 8)
    neighbor_view(d, polys, nb, cl, 600, 80, 260, 2.6)
    d.text((620, 400), f"pezzo a L: {tl} vicine", fill=TEXT, font=fs)
    cr, tr = most_neighbors(polys, nb, 4)
    neighbor_view(d, polys, nb, cr, 860, 80, 260, 2.6)
    d.text((880, 400), f"rombo: {tr} vicine", fill=TEXT, font=fs)
    d.text((620, 586), "438 tessere; vicine da 3 a 8", fill=TEXT, font=fs)
    return im


def ts10_tiles():
    sp = 37
    polys, kind = prev.ts10_polys(sp)
    nb = prev.ts10_neighbors(polys, sp)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    d.text((60, 30), f"passo del reticolo {sp} px (2,66 mm)", fill=TEXT, font=f)
    tile = [(x * sp, y * sp) for x, y in prev.TS10_TILE]
    draw_shape(d, tile, 260, 230, 6.0)
    d.text((60, 400), f"una sola forma curva, simmetrica a 180 gradi, area {area(tile):.0f} px2", fill=TEXT, font=fs)
    for k in range(3):
        an = k * 2 * math.pi / 3
        q = [(x * math.cos(an) - y * math.sin(an), x * math.sin(an) + y * math.cos(an)) for x, y in tile]
        draw_shape(d, q, 120 + k * 160, 510, 2.4, width=3)
        d.text((95 + k * 160, 586), f"{120 * k} gradi", fill=TEXT, font=fs)
    d.text((620, 30), "vicine: contorni a meno di 1/10 del passo", fill=TEXT, font=f)
    c = pick_center(polys, nb, want=10)
    neighbor_view(d, polys, nb, c, 620, 70, 460, 3.6)
    d.text((620, 556), "4 lungo i lati + 6 attorno ai buchi dei vortici = 10", fill=TEXT, font=fs)
    d.text((620, 586), "432 tessere; vicine da 5 a 10", fill=TEXT, font=fs)
    return im


def ts11_tiles():
    a = 12
    polys, kind, ntri = prev.ts11_polys(a)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    ux, uy = a / 2, a * 3 ** 0.5 / 2
    tri = a * a * 3 ** 0.5 / 4
    d.text((60, 30), f"triangolo di base: lato {a} px (0,86 mm)", fill=TEXT, font=f)
    frames = [v for k, n, v in prev.TS11_PIECES if n == 32]
    for n_, v in enumerate(frames):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 120 + n_ * 170, 170, 1.4)
    d.text((60, 290), f"cornici a rombo con buco: 32 triangoli, area {32 * tri:.0f} px2", fill=TEXT, font=fs)
    small = [(n, v) for k, n, v in prev.TS11_PIECES if n in (3, 5)]
    for n_, (n, v) in enumerate([small[0], small[6], small[2]]):
        draw_shape(d, [(x * ux, y * uy) for x, y in v], 110 + n_ * 120, 410, 3.0)
    white = [v for k, n, v in prev.TS11_PIECES if n == 1][0]
    draw_shape(d, [(x * ux, y * uy) for x, y in white], 470, 410, 3.0)
    d.text((60, 500), f"pezzi da 5 e da 3 triangoli (area {5 * tri:.0f} e {3 * tri:.0f} px2),", fill=TEXT, font=fs)
    d.text((60, 526), f"triangolo bianco (area {tri:.0f} px2)", fill=TEXT, font=fs)
    d.text((60, 586), "per periodo: 3 cornici, 12 pezzi, 6 triangoli", fill=TEXT, font=fs)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    cf = pick_center([p if ntri[i] == 32 else [(1e9, 1e9)] for i, p in enumerate(polys)], nb)
    neighbor_view(d, polys, nb, cf, 600, 70, 300, 1.15)
    d.text((620, 400), f"cornice: {len(nb[cf])} vicine", fill=TEXT, font=fs)
    cs = pick_center([p if ntri[i] == 5 else [(1e9, 1e9)] for i, p in enumerate(polys)], nb)
    neighbor_view(d, polys, nb, cs, 880, 70, 220, 2.6)
    d.text((900, 400), f"pezzo: {len(nb[cs])} vicine", fill=TEXT, font=fs)
    d.text((620, 556), "triangoli bianchi 3 vicine, pezzi 5, cornici 16", fill=TEXT, font=fs)
    d.text((620, 586), "372 tessere", fill=TEXT, font=fs)
    return im


def ts12_tiles():
    e = 28
    polys, kite = prev.p2_polys(e)
    nb = prev.neighbors_by_vertex(polys)
    im = Image.new("RGB", (1120, 640), (0, 0, 0))
    d = ImageDraw.Draw(im)
    f, fs = font(26), font(20)
    phi = (1 + 5 ** 0.5) / 2
    d.text((60, 30), f"lato lungo {e} px (2,02 mm), lato corto {e / phi:.1f} px".replace(".", ","), fill=TEXT, font=f)
    ki = next(p for p, k in zip(polys, kite) if k and len(p) == 4)
    da = next(p for p, k in zip(polys, kite) if not k and len(p) == 4)
    draw_shape(d, ki, 190, 220, 5.0)
    d.text((60, 330), "aquilone: angoli 72, 72, 72, 144 gradi", fill=TEXT, font=fs)
    d.text((60, 356), f"area {area(ki):.0f} px2, {sum(kite)} tessere", fill=TEXT, font=fs)
    draw_shape(d, da, 450, 220, 5.0)
    d.text((60, 410), "dardo: angoli 36, 72, 36, 216 gradi", fill=TEXT, font=fs)
    d.text((60, 436), f"area {area(da):.0f} px2, {len(polys) - sum(kite)} tessere", fill=TEXT, font=fs)
    d.text((60, 586), "Penrose P2: non periodica, simmetria a 5", fill=TEXT, font=fs)
    d.text((620, 30), "vicine: un vertice in comune", fill=TEXT, font=f)
    ck = pick_center([p if kite[i] else [(1e9, 1e9)] for i, p in enumerate(polys)], nb, want=9)
    neighbor_view(d, polys, nb, ck, 600, 70, 260, 3.0)
    d.text((620, 400), f"aquilone: {len(nb[ck])} vicine", fill=TEXT, font=fs)
    cd = pick_center([p if not kite[i] else [(1e9, 1e9)] for i, p in enumerate(polys)], nb, want=10)
    neighbor_view(d, polys, nb, cd, 860, 70, 260, 3.0)
    d.text((880, 400), f"dardo: {len(nb[cd])} vicine", fill=TEXT, font=fs)
    d.text((620, 586), f"{len(polys)} tessere; vicine da 4 a 10", fill=TEXT, font=fs)
    return im


EXPERIMENTS = {
    "life1_conway": (prev.conway, conway_tiles),
    "life1_penrose": (prev.penrose, penrose_tiles),
    "life1_ts1": (prev.ts1, ts1_tiles),
    "life1_ts2": (prev.ts2, ts2_tiles),
    "life1_ts3": (prev.ts3, ts3_tiles),
    "life1_ts4": (prev.ts4, ts4_tiles),
    "life1_ts5": (prev.ts5, ts5_tiles),
    "life1_ts6": (prev.ts6, ts6_tiles),
    "life1_ts7": (prev.ts7, ts7_tiles),
    "life1_ts8": (prev.ts8, ts8_tiles),
    "life1_ts9": (prev.ts9, ts9_tiles),
    "life1_ts10": (prev.ts10, ts10_tiles),
    "life1_ts11": (prev.ts11, ts11_tiles),
    "life1_ts12": (prev.ts12, ts12_tiles),
}


def main():
    names = sys.argv[1:] or list(EXPERIMENTS)
    for name in names:
        preview, tiles = EXPERIMENTS[name]
        out = os.path.join(ROOT, "firmware", name, "info")
        os.makedirs(out, exist_ok=True)
        preview(random.Random(prev.SEEDS.get(name, 7))).save(os.path.join(out, "preview.png"))
        tiles().save(os.path.join(out, "tiles.png"))
        print(out)


if __name__ == "__main__":
    main()
