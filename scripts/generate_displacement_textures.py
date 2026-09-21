#!/usr/bin/env python3
"""Generates the displacement textures shipped in resources/textures/displacement/.

Every texture is procedural - periodic functions and wrap-around noise on a unit tile - so the images
tile seamlessly in both directions and carry no third-party material. Grey PNGs are height maps
(white = raised). The colour PNGs are RGB where the luminance is the height, which is what
decode_height_texture() reads, so they displace and colour at the same time.

    python3 scripts/generate_displacement_textures.py [--size 1024] [--out resources/textures/displacement]

Needs numpy and Pillow. Regenerating overwrites the files this script owns and nothing else.
"""
import argparse, os
import numpy as np
from PIL import Image

SS = 2  # supersampling factor; the final image is the box-filtered average

# ---------------------------------------------------------------------------------------------
# helpers: everything works on u, v in [0, 1) with period 1 on the tile
# ---------------------------------------------------------------------------------------------
def grid(n):
    s = n * SS
    v, u = np.meshgrid((np.arange(s) + 0.5) / s, (np.arange(s) + 0.5) / s, indexing='ij')
    return u, v

def down(img):
    s = img.shape[0] // SS
    return img.reshape(s, SS, s, SS).mean(axis=(1, 3))

def norm(h):
    h = h - h.min(); m = h.max()
    return h / m if m > 0 else h

def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)

def value_noise(u, v, freq, seed):
    """Tileable value noise: a random lattice of `freq` cells with wrap-around, smooth interpolation."""
    rng = np.random.default_rng(seed)
    lat = rng.random((freq, freq))
    x = u * freq; y = v * freq
    x0 = np.floor(x).astype(int); y0 = np.floor(y).astype(int)
    fx = x - x0; fy = y - y0
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy)
    x1 = (x0 + 1) % freq; y1 = (y0 + 1) % freq; x0 %= freq; y0 %= freq
    a = lat[y0, x0]; b = lat[y0, x1]; c = lat[y1, x0]; d = lat[y1, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy

def fbm(u, v, freq, seed, octaves=4, gain=0.5):
    out = np.zeros_like(u); amp = 1.0; total = 0.0
    for o in range(octaves):
        out += amp * value_noise(u, v, freq * 2 ** o, seed + o); total += amp; amp *= gain
    return out / total

def wrapped_points(n, seed, jitter=0.35):
    """n x n jittered lattice points on the unit tile (periodic)."""
    rng = np.random.default_rng(seed)
    gx, gy = np.meshgrid((np.arange(n) + 0.5) / n, (np.arange(n) + 0.5) / n)
    pts = np.stack([gx.ravel(), gy.ravel()], 1) + (rng.random((n * n, 2)) - 0.5) * (jitter / n)
    return pts % 1.0

def voronoi(u, v, pts):
    """Distance to the nearest and second-nearest point on the torus, and the nearest point's index."""
    d1 = np.full(u.shape, 9.0); d2 = np.full(u.shape, 9.0); idx = np.zeros(u.shape, int)
    for i, (px, py) in enumerate(pts):
        dx = u - px; dx -= np.round(dx); dy = v - py; dy -= np.round(dy)
        d = np.sqrt(dx * dx + dy * dy)
        closer = d < d1
        d2 = np.where(closer, d1, np.minimum(d2, d)); d1 = np.where(closer, d, d1); idx = np.where(closer, i, idx)
    return d1, d2, idx

def tri_wave(x):
    return np.abs(2 * (x - np.floor(x + 0.5)))  # 0..1 triangle wave, period 1

# ---------------------------------------------------------------------------------------------
# the textures
# ---------------------------------------------------------------------------------------------
def diamond_plate(u, v):
    # Raised diamonds in two offset rows, bevelled; the classic tread plate.
    n = 4
    h = np.zeros_like(u)
    for ox, oy in ((0.0, 0.0), (0.5, 0.5)):
        x = (u * n + ox) % 1 - 0.5; y = (v * n + oy) % 1 - 0.5
        d = np.abs(x) / 0.36 + np.abs(y) / 0.18  # a long diamond, 2:1
        h = np.maximum(h, smoothstep(1.0, 0.72, d))
    h += 0.06 * fbm(u, v, 32, 11)
    return norm(h)

def hex_cells(u, v, nx):
    """Distance to the nearest hexagon centre, normalised so the hexagon's edge is at 1, plus a cell id.
    nx hexagons across; the rows are stretched by at most ~2 % so a whole number fit the tile."""
    W = 1.0 / nx                                # hexagon width (flat-to-flat, pointy-top layout)
    H = 2 * W / np.sqrt(3)                       # ideal corner-to-corner height
    ny = 2 * max(1, int(round(1.0 / (1.5 * H))))  # rows per tile, even: the two staggered lattices
    H = 1.0 / (0.75 * ny)                          # stretched so the rows tile exactly
    best = np.full(u.shape, 9.0); cid = np.zeros(u.shape, int)
    for k, (ox, oy) in enumerate(((0.0, 0.0), (0.5, 0.5))):
        cx = (np.round(u / W - ox) + ox) * W; cy = (np.round(v / (1.5 * H) - oy) + oy) * 1.5 * H
        dx = u - cx
        dy = (v - cy) * (2 * W / np.sqrt(3)) / H          # undo the row stretch: regular-hex units
        ax = np.abs(dx) / (W / 2); ay = np.abs(dy) / (W / 2)
        d = np.maximum(ax, (ax + ay * np.sqrt(3)) / 2)    # hexagon SDF, edge at 1
        ix = np.round(u / W - ox).astype(int) % nx; iy = np.round(v / (1.5 * H) - oy).astype(int) % (ny // 2)
        this = (ix * 7 + iy * 13 + k * 3) % 1009   # periodic, so a cell's colour matches across the seam
        cid = np.where(d < best, this, cid); best = np.minimum(best, d)
    return best, cid

def honeycomb(u, v):
    d, _ = hex_cells(u, v, 7)
    return norm(smoothstep(1.0, 0.86, d))

def knurl(u, v):
    n = 24
    a = tri_wave((u + v) * n); b = tri_wave((u - v) * n)
    return norm(1 - np.maximum(a, b))  # pyramids

def scales(u, v):
    n = 8
    h = np.zeros_like(u)
    # rows of circles, each row offset by half a scale and drawn over the row below
    for row in range(-1, 2 * n + 1):
        cy = row / (2 * n); ox = 0.5 if row % 2 else 0.0
        x = (u * n + ox) % 1 - 0.5; y = v - cy
        y -= np.round(y)
        r = np.sqrt((x / 1.0) ** 2 + (y * n) ** 2)
        inside = r < 0.5
        dome = np.sqrt(np.clip(0.25 - r * r, 0, None)) * 2  # spherical cap
        ramp = 0.4 + 0.6 * np.clip((0.5 * n * -y) / 0.5 + 0.5, 0, 1)  # thicker at the exposed edge
        cand = np.where(inside, 0.35 + 0.65 * dome * ramp, 0)
        h = np.where(inside & (y * n <= 0.02), cand, h)
    return norm(h)

def herringbone(u, v):
    # 2:1 bricks in the domino herringbone, turned 45 degrees. In cell coordinates (x, y) the brick a
    # cell belongs to follows from (i - j) mod 4: 0/1 pair horizontally, 2/3 pair vertically. An even
    # cell count per tile edge keeps that rule periodic.
    n = 4
    x = (u + v) * n; y = (u - v) * n
    i = np.floor(x); j = np.floor(y); fx = x - i; fy = y - j
    k = ((i - j) % 4 + 4) % 4
    bx = np.where(k == 0, fx / 2, np.where(k == 1, 0.5 + fx / 2, fx))
    by = np.where(k == 2, 0.5 + fy / 2, np.where(k == 3, fy / 2, fy))
    # local coordinates on a 2x1 brick: the long axis is x for k in {0,1}, y for k in {2,3}
    long_ = np.where(k < 2, bx, by); short = np.where(k < 2, by, bx)
    gap_l = 0.03; gap_s = 0.06
    m = smoothstep(0, gap_l, long_) * smoothstep(0, gap_l, 1 - long_) * smoothstep(0, gap_s, short) * smoothstep(0, gap_s, 1 - short)
    return norm(0.85 * m + 0.15 * m * fbm(u, v, 16, 5))

def cobblestone(u, v):
    pts = wrapped_points(6, 21, 0.55)
    d1, d2, idx = voronoi(u, v, pts)
    edge = d2 - d1
    rng = np.random.default_rng(22); tops = rng.random(len(pts)) * 0.25 + 0.75
    h = smoothstep(0.0, 0.05, edge) * (0.55 + 0.45 * np.sqrt(np.clip(1 - (d1 / 0.11) ** 2, 0, None)))
    h *= tops[idx]
    h += 0.08 * fbm(u, v, 48, 23)
    return norm(h)

def leather(u, v):
    pts = wrapped_points(22, 31, 0.9)
    d1, d2, idx = voronoi(u, v, pts)
    grooves = 1 - smoothstep(0.0, 0.012, d2 - d1)
    h = 1 - 0.55 * grooves - 0.25 * fbm(u, v, 12, 33) - 0.1 * fbm(u, v, 96, 34)
    return norm(h)

def carbon_fibre(u, v):
    n = 8
    x = u * n; y = v * n
    cx = np.floor(x); cy = np.floor(y)
    over = ((cx + cy) % 4) < 2  # 2x2 twill
    fx = x % 1; fy = y % 1
    warp = 1 - 0.5 * (2 * np.abs(fx - 0.5)) ** 2 + 0.06 * np.sin(fy * 2 * np.pi * 14)
    weft = 1 - 0.5 * (2 * np.abs(fy - 0.5)) ** 2 + 0.06 * np.sin(fx * 2 * np.pi * 14)
    h = np.where(over, warp, weft * 0.92)
    return norm(h)

def chevron(u, v):
    n = 6
    h = tri_wave(v * n * 2 + tri_wave(u * n) * 1.0)
    return norm(1 - smoothstep(0.35, 0.65, h))

def ripples(u, v):
    # Rings spreading from a few points on the torus, fading with distance - rain on water.
    pts = wrapped_points(3, 81, 1.0)
    h = np.zeros_like(u)
    rng = np.random.default_rng(82)
    for (px, py), f, ph in zip(pts, rng.random(len(pts)) * 6 + 10, rng.random(len(pts)) * 6.28):
        dx = u - px; dx -= np.round(dx); dy = v - py; dy -= np.round(dy)
        r = np.sqrt(dx * dx + dy * dy)
        h += np.exp(-r / 0.22) * np.cos(2 * np.pi * r * f + ph)
    return norm(h)

def hammered(u, v):
    pts = wrapped_points(9, 41, 0.8)
    d1, d2, idx = voronoi(u, v, pts)
    rng = np.random.default_rng(42); rad = rng.random(len(pts)) * 0.04 + 0.07
    dent = np.clip(1 - (d1 / rad[idx]) ** 2, 0, None)
    return norm(1 - 0.8 * dent + 0.05 * fbm(u, v, 64, 43))

def perforated(u, v):
    n = 8
    x = (u * n) % 1 - 0.5; y = (v * n) % 1 - 0.5
    r = np.sqrt(x * x + y * y)
    return norm(smoothstep(0.30, 0.34, r))

def rope(u, v):
    n = 6  # ropes per tile, running along v
    x = (u * n) % 1 - 0.5
    body = np.sqrt(np.clip(0.25 - x * x, 0, None)) * 2
    twist = 0.5 + 0.5 * np.sin(2 * np.pi * (v * 12 + x * 1.6))
    return norm(body * (0.65 + 0.35 * twist))

def stone_wall(u, v):
    rows = 5
    y = v * rows; row = np.floor(y); fy = y % 1
    rng = np.random.default_rng(51)
    h = np.zeros_like(u)
    for r in range(rows):
        # stones of varying width along the row, periodic in u
        widths = rng.random(6) * 0.6 + 0.7; widths *= 1.0 / widths.sum()
        edges = np.concatenate([[0], np.cumsum(widths)]) + rng.random() * 0.3
        xu = (u + 0.0) % 1
        in_row = row == r
        for i in range(len(widths)):
            a = edges[i] % 1; w = widths[i]
            dx = (xu - a) % 1
            inside = dx < w
            gap = 0.035
            m = smoothstep(0, gap, dx) * smoothstep(0, gap, w - dx) * smoothstep(0, 0.12, fy) * smoothstep(0, 0.12, 1 - fy)
            top = 0.7 + 0.3 * rng.random()
            h = np.where(in_row & inside, m * top, h)
    h = h * (0.85 + 0.15 * fbm(u, v, 24, 52)) + 0.04 * fbm(u, v, 96, 53)
    return norm(h)

# ---- colour textures: (rgb in 0..1, height = luminance by construction)
def lum(rgb):
    return 0.299 * rgb[..., 0] + 0.587 * rgb[..., 1] + 0.114 * rgb[..., 2]

def colour_bricks(u, v):
    n = 6
    y = v * n; row = np.floor(y); x = u * n * 2 + 0.5 * (row % 2)
    bx = x % 1; by = y % 1
    gap = 0.07
    m = smoothstep(0, gap, bx) * smoothstep(0, gap, 1 - bx) * smoothstep(0, gap * 2, by) * smoothstep(0, gap * 2, 1 - by)
    rng = np.random.default_rng(61)
    cell = (np.floor(x).astype(int) * 7 + row.astype(int) * 13) % 97
    tone = rng.random(97)[cell]
    # brick reds of varying warmth over a dark grey mortar
    brick = np.stack([0.70 + 0.2 * tone, 0.30 + 0.12 * tone, 0.22 + 0.06 * tone], -1)
    brick *= (0.85 + 0.15 * fbm(u, v, 48, 62))[..., None]
    mortar = np.array([0.30, 0.29, 0.27])
    rgb = brick * m[..., None] + mortar * (1 - m[..., None])
    return rgb

def mosaic(u, v):
    n = 8
    x = (u * n) % 1; y = (v * n) % 1
    gap = 0.08
    m = smoothstep(0, gap, x) * smoothstep(0, gap, 1 - x) * smoothstep(0, gap, y) * smoothstep(0, gap, 1 - y)
    rng = np.random.default_rng(71)
    cell = (np.floor(u * n).astype(int) * 31 + np.floor(v * n).astype(int) * 17) % 64
    pal = np.array([[0.90, 0.85, 0.70], [0.20, 0.45, 0.75], [0.85, 0.35, 0.25], [0.35, 0.65, 0.40]])
    tile = pal[rng.integers(0, 4, 64)[cell]]
    grout = np.array([0.18, 0.18, 0.18])
    return tile * m[..., None] + grout * (1 - m[..., None])

def hex_tiles(u, v):
    d, cid = hex_cells(u, v, 7)
    m = smoothstep(1.0, 0.9, d)
    pal = np.array([[0.95, 0.93, 0.88], [0.25, 0.55, 0.60], [0.80, 0.55, 0.20]])
    rng = np.random.default_rng(91)
    tile = pal[rng.integers(0, 3, 1009)[cid]]
    grout = np.array([0.15, 0.15, 0.16])
    return tile * m[..., None] + grout * (1 - m[..., None])


def wood_planks(u, v):
    n = 4  # planks across, running along v; staggered ends
    x = u * n; col = np.floor(x); fx = x - col
    rng = np.random.default_rng(101)
    y = v * 2 + rng.random(n)[col.astype(int) % n]  # each plank column has its own end offset
    fy = y % 1
    gap = 0.04
    m = smoothstep(0, gap, fx) * smoothstep(0, gap, 1 - fx) * smoothstep(0, gap * 1.5, fy) * smoothstep(0, gap * 1.5, 1 - fy)
    # grain: stretched noise along the plank, per plank phase
    grain = fbm((u * 1.0 + rng.random(n)[col.astype(int) % n]) % 1, v, 6, 102, octaves=5)
    rings = 0.5 + 0.5 * np.sin(2 * np.pi * (fx * 3 + grain * 2.5))
    return norm(m * (0.75 + 0.25 * rings))

def basket_weave(u, v):
    n = 4
    x = u * n; y = v * n
    cx = np.floor(x); cy = np.floor(y); fx = x % 1; fy = y % 1
    horiz = (cx + cy) % 2 == 0
    strips = 3  # strips per cell
    along = np.where(horiz, fx, fy); across = np.where(horiz, fy, fx)
    strip = (across * strips) % 1
    body = np.sqrt(np.clip(1 - (2 * strip - 1) ** 2, 0, None))  # rounded strip
    ends = smoothstep(0, 0.06, along) * smoothstep(0, 0.06, 1 - along)
    return norm(0.35 + 0.65 * body * ends)

def chainmail(u, v):
    n = 6
    h = np.zeros_like(u)
    for ox, oy in ((0.0, 0.0), (0.5, 0.5)):
        x = (u * n + ox) % 1 - 0.5; y = (v * n + oy) % 1 - 0.5
        r = np.sqrt(x * x + y * y)
        ring = np.exp(-((r - 0.36) / 0.09) ** 2)
        h = np.maximum(h, ring)
    return norm(h)

def pyramids(u, v):
    n = 8
    x = np.abs((u * n) % 1 - 0.5); y = np.abs((v * n) % 1 - 0.5)
    return norm(0.5 - np.maximum(x, y))

def waffle(u, v):
    n = 6
    x = (u * n) % 1; y = (v * n) % 1
    gap = 0.12
    m = smoothstep(0, gap, x) * smoothstep(0, gap, 1 - x) * smoothstep(0, gap, y) * smoothstep(0, gap, 1 - y)
    return norm(m)

def bubbles(u, v):
    rng = np.random.default_rng(111)
    h = np.zeros_like(u)
    for (px, py), r in zip(wrapped_points(7, 112, 0.9), rng.random(49) * 0.05 + 0.03):
        dx = u - px; dx -= np.round(dx); dy = v - py; dy -= np.round(dy)
        d2 = dx * dx + dy * dy
        h = np.maximum(h, np.sqrt(np.clip(r * r - d2, 0, None)) / 0.08)
    return norm(h)

def cracked_earth(u, v):
    pts = wrapped_points(7, 121, 0.7)
    d1, d2, idx = voronoi(u, v, pts)
    crack = 1 - smoothstep(0.0, 0.03, d2 - d1)
    plates = 0.8 + 0.2 * fbm(u, v, 24, 122)
    curl = 1 - 0.35 * np.clip(1 - d1 / 0.12, 0, 1)  # plates curl up at the edges
    return norm(plates * (2 - curl) * (1 - 0.9 * crack))

def sand_ripples(u, v):
    n = 8
    wob = 0.06 * np.sin(2 * np.pi * u * 2) + 0.03 * fbm(u, v, 4, 131)
    h = 0.5 + 0.5 * np.sin(2 * np.pi * (v * n + wob))
    h = h ** 1.6  # sharp crests, soft troughs
    return norm(h + 0.05 * fbm(u, v, 64, 132))

def bark(u, v):
    grooves = fbm((u * 3) % 1, v, 4, 141, octaves=4)  # the ridges below stretch it along v
    ridges = np.abs(np.sin(2 * np.pi * (u * 9 + grooves * 1.5)))
    return norm(ridges ** 0.7 * (0.7 + 0.3 * fbm(u, v, 12, 142)) + 0.1 * fbm(u, v, 48, 143))

def slate(u, v):
    h = fbm(u, v, 3, 151, octaves=6, gain=0.55)
    steps = np.floor(h * 6) / 6 + 0.4 * (h * 6 - np.floor(h * 6)) / 6  # cleaved layers
    return norm(steps + 0.05 * fbm(u, v, 48, 152))

def triangles(u, v):
    n = 6
    x = u * n; y = v * n * np.sqrt(3) / 1.5  # rows of equilateral triangles
    row = np.floor(y); fy = y - row
    xs = x + 0.5 * (row % 2)
    fx = xs % 1
    up = fx < 1 - fy  # which triangle of the rhombus
    # distance to the nearest edge of the triangle, in either orientation
    d_up = np.minimum(np.minimum(fy, fx - 0 * fy), (1 - fy - fx)) 
    d_dn = np.minimum(np.minimum(1 - fy, 1 - fx), (fx + fy - 1))
    d = np.where(up, d_up, d_dn)
    return norm(smoothstep(0.0, 0.08, d))

def roof_tiles(u, v):
    n = 6
    h = np.zeros_like(u)
    for row in range(-1, 2 * n + 1):
        cy = row / (2 * n); ox = 0.5 if row % 2 else 0.0
        x = (u * n + ox) % 1 - 0.5; y = v - cy; y -= np.round(y)
        yy = y * n * 2  # 0 at the row's exposed edge, rising toward the covered end
        inside = (yy >= -0.05) & (yy < 1.0)
        arch = np.cos(x * np.pi) * 0.6 + 0.4
        cand = np.where(inside, 0.3 + 0.7 * arch * (1 - 0.35 * yy), 0)
        h = np.where(inside & (cand > 0), cand, h)
    return norm(h)

def star_tiles(u, v):
    # 8-point stars and crosses (the classic Islamic star-and-cross tiling)
    n = 4
    x = (u * n) % 1 - 0.5; y = (v * n) % 1 - 0.5
    a = np.abs(x); b = np.abs(y)
    star = np.maximum(np.maximum(a, b), (a + b) / np.sqrt(2) * 1.15)
    m = smoothstep(0.42, 0.36, star)
    # the crosses between the stars sit on the half-offset lattice
    x2 = (u * n + 0.5) % 1 - 0.5; y2 = (v * n + 0.5) % 1 - 0.5
    a2 = np.abs(x2); b2 = np.abs(y2)
    cross = np.minimum(np.maximum(a2 / 0.12, b2 / 0.30), np.maximum(a2 / 0.30, b2 / 0.12))
    m2 = smoothstep(1.0, 0.85, cross) * 0.8
    return norm(np.maximum(m, m2))

def terrazzo(u, v):
    base = np.array([0.82, 0.80, 0.76])
    rgb = np.broadcast_to(base, u.shape + (3,)).copy() * (0.95 + 0.05 * fbm(u, v, 32, 161))[..., None]
    rng = np.random.default_rng(162)
    pal = np.array([[0.85, 0.30, 0.25], [0.20, 0.35, 0.55], [0.25, 0.25, 0.25], [0.95, 0.90, 0.80], [0.80, 0.60, 0.20]])
    for (px, py), r, c, ang in zip(wrapped_points(12, 163, 1.0), rng.random(144) * 0.02 + 0.012, rng.integers(0, 5, 144), rng.random(144) * 3.14):
        dx = u - px; dx -= np.round(dx); dy = v - py; dy -= np.round(dy)
        ca, sa = np.cos(ang), np.sin(ang)
        ex = (dx * ca - dy * sa) / (r * 1.4); ey = (dx * sa + dy * ca) / r
        inside = (np.abs(ex) + np.abs(ey) * 0.7 + np.maximum(np.abs(ex), np.abs(ey)) * 0.5) < 1.0
        rgb[inside] = pal[c] * 0.92  # chips sit a touch below the matrix: darker = lower
    return rgb

def camouflage(u, v):
    pal = np.array([[0.36, 0.42, 0.24], [0.55, 0.50, 0.32], [0.22, 0.26, 0.17], [0.60, 0.58, 0.45]])
    a = fbm(u, v, 3, 171, octaves=4); b = fbm(u, v, 3, 172, octaves=4); c = fbm(u, v, 5, 173, octaves=3)
    idx = (a > 0.55).astype(int) + 2 * (b > 0.5).astype(int)
    idx = np.where(c > 0.72, 3, idx)
    rgb = pal[idx].astype(float)
    return rgb * (0.94 + 0.06 * fbm(u, v, 48, 174))[..., None]

def tartan(u, v):
    n = 2
    def stripes(t):
        t = (t * n) % 1
        band = np.zeros_like(t)
        for a, w, val in ((0.0, 0.32, 1), (0.32, 0.06, 2), (0.38, 0.24, 0), (0.62, 0.06, 2), (0.68, 0.32, 1)):
            band = np.where((t >= a) & (t < a + w), val, band)
        return band
    pal = np.array([[0.12, 0.25, 0.20], [0.55, 0.12, 0.14], [0.90, 0.80, 0.30]])
    su = stripes(u).astype(int); sv = stripes(v).astype(int)
    weave = ((np.floor(u * 400) + np.floor(v * 400)) % 2) == 0  # the two thread directions alternate
    rgb = np.where(weave[..., None], pal[su], pal[sv]).astype(float)
    return rgb * (0.9 + 0.1 * (0.5 + 0.5 * np.sin(2 * np.pi * (u + v) * 200)))[..., None]

GREY = {
    'Diamond Plate': diamond_plate, 'Honeycomb': honeycomb, 'Fine Knurl': knurl, 'Scales': scales,
    'Herringbone': herringbone, 'Cobblestone': cobblestone, 'Leather': leather, 'Carbon Fibre': carbon_fibre,
    'Chevron': chevron, 'Ripples': ripples, 'Hammered': hammered, 'Perforated': perforated, 'Rope': rope,
    'Stone Wall': stone_wall, 'Wood Planks': wood_planks, 'Basket Weave': basket_weave, 'Chainmail': chainmail,
    'Pyramids': pyramids, 'Waffle': waffle, 'Bubbles': bubbles, 'Cracked Earth': cracked_earth,
    'Sand Ripples': sand_ripples, 'Bark': bark, 'Slate': slate, 'Triangles': triangles, 'Roof Tiles': roof_tiles,
    'Star Tiles': star_tiles,
}
COLOUR = {'Colour Bricks': colour_bricks, 'Mosaic Tiles': mosaic, 'Hex Tiles': hex_tiles, 'Terrazzo': terrazzo,
          'Camouflage': camouflage, 'Tartan': tartan}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--size', type=int, default=1024)
    ap.add_argument('--out', default='resources/textures/displacement')
    ap.add_argument('--sheet', default='')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    u, v = grid(args.size)
    thumbs = []
    for name, fn in GREY.items():
        img = (np.clip(down(fn(u, v)), 0, 1) * 255 + 0.5).astype(np.uint8)
        Image.fromarray(img, 'L').save(os.path.join(args.out, name + '.png'), optimize=True)
        thumbs.append((name, np.stack([img] * 3, -1)))
        print('wrote', name)
    for name, fn in COLOUR.items():
        rgb = fn(u, v)
        rgb = np.stack([down(rgb[..., c]) for c in range(3)], -1)
        img = (np.clip(rgb, 0, 1) * 255 + 0.5).astype(np.uint8)
        Image.fromarray(img, 'RGB').save(os.path.join(args.out, name + '.png'), optimize=True)
        thumbs.append((name, img))
        print('wrote', name, '(colour)')
    if args.sheet:
        t = 256; cols = 7; rows = (len(thumbs) + cols - 1) // cols
        sheet = Image.new('RGB', (cols * t, rows * t), (40, 40, 40))
        for i, (name, img) in enumerate(thumbs):
            im = Image.fromarray(img).resize((t, t), Image.LANCZOS)
            sheet.paste(im, ((i % cols) * t, (i // cols) * t))
        sheet.save(args.sheet); print('sheet', args.sheet)

if __name__ == '__main__':
    main()
