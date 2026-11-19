"""The Design tab's canvas tooltips (DesignSketchTool.cpp): what the pointer rests on in a sketch.

The sketch tools and the constraints have their pictures in design_2d.py; these are the things only
the canvas has: the snaps and the angle lock of a tool drawing, the handles a drag reshapes, a value
label being typed into, the colours that say how constrained a sketch is, and the box that places
imported art. Drawn flat with PIL, as design_2d.py draws its own, in the same palette: the dark sketch
plane, grey geometry, and ACCENT for what the handle or the snap acts on. The three constraint-state
pictures are the exception: their tooltips explain a colour the sketch is drawn in, so they show it in
that colour (orange, green, red), as the canvas does.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, CURSOR, GIF_SIZE, GRID, SIZE, SS, smooth

OUT = "design_tips"  # resources/images/<OUT>

BG = tuple(int(c * 255 * 0.9) for c in BED)
GRID_C = tuple(int(c * 255) for c in GRID)
GEOM = (168, 176, 179)
DIM = (110, 116, 119)
ACC = tuple(int(min(255, c * 255 * 1.15)) for c in ACCENT)
ACC_SOFT = ACC + (150,)
POINT = (232, 238, 238)
INK = (30, 32, 34)
FIELD = (52, 56, 58)
# The sketch's own state colours (DesignSketchTool::render): under-constrained, fully constrained,
# conflicting.
ORANGE = (255, 140, 26)
GREEN = (77, 217, 107)
RED = (255, 56, 56)
SCALE = 0.80  # the unit square fills this much of the picture


class Sketch:
    """The sketch plane, -1..1 in both directions, y up."""

    def __init__(self, size):
        self.size = size
        self.px = size * SS
        self.img = Image.new("RGB", (self.px, self.px), BG)
        self.d = ImageDraw.Draw(self.img, "RGBA")
        step = self.px / 8.0
        for i in range(1, 8):
            v = i * step
            self.d.line([(v, 0), (v, self.px)], fill=GRID_C, width=SS)
            self.d.line([(0, v), (self.px, v)], fill=GRID_C, width=SS)

    def xy(self, p):
        return ((p[0] * SCALE + 1.0) * 0.5 * self.px, (1.0 - p[1] * SCALE) * 0.5 * self.px)

    def k(self):
        return SS * self.size / 192.0

    def w(self, width):
        return max(1, int(round(width * self.k())))

    def poly(self, pts, col=GEOM, width=2.0, closed=False, dash=False):
        pts = [tuple(p) for p in pts]
        if closed:
            pts = pts + [pts[0]]
        if len(pts) < 2:
            return
        if not dash:
            self.d.line([self.xy(p) for p in pts], fill=col, width=self.w(width), joint="curve")
            return
        on, off, acc, draw_on = 0.06, 0.045, 0.0, True
        for a, b in zip(pts[:-1], pts[1:]):
            a, b = np.array(a, float), np.array(b, float)
            seg = np.linalg.norm(b - a)
            t = 0.0
            while t < seg - 1e-9:
                left = (on if draw_on else off) - acc
                step = min(left, seg - t)
                if draw_on:
                    p0, p1 = a + (b - a) * (t / seg), a + (b - a) * ((t + step) / seg)
                    self.d.line([self.xy(p0), self.xy(p1)], fill=col, width=self.w(width))
                t += step
                acc += step
                if acc >= (on if draw_on else off) - 1e-9:
                    acc, draw_on = 0.0, not draw_on

    def line(self, a, b, col=GEOM, width=2.0, dash=False):
        self.poly([a, b], col, width, dash=dash)

    def circle(self, c, r, col=GEOM, width=2.0, dash=False):
        self.poly(circle_pts(c, r), col, width, dash=dash)

    def dot(self, p, r=3.0, col=POINT):
        x, y = self.xy(p)
        r = r * self.k()
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=col, outline=INK, width=max(1, SS))

    def ring(self, p, r=7.0, col=ACC, width=1.5):
        x, y = self.xy(p)
        r = r * self.k()
        self.d.ellipse([x - r, y - r, x + r, y + r], outline=col, width=self.w(width))

    def square(self, p, half=3.6, col=POINT):
        x, y = self.xy(p)
        h = half * self.k()
        self.d.rectangle([x - h, y - h, x + h, y + h], fill=col, outline=INK, width=max(1, SS))

    def fill(self, pts, col):
        self.d.polygon([self.xy(p) for p in pts], fill=col)

    def arrow_head(self, tip, direction, col, size=0.06):
        v = np.array(direction, float)
        v /= max(np.linalg.norm(v), 1e-9)
        w = np.array([-v[1], v[0]])
        tip = np.array(tip, float)
        base = tip - v * size
        self.fill([tuple(tip), tuple(base + w * size * 0.42), tuple(base - w * size * 0.42)], col)

    def box(self, c, half=0.09, col=GEOM, fill=FIELD, width=1.2):
        """The value box a label shows its number in: empty, digits are not translated."""
        pts = [(c[0] - half, c[1] - half * 0.55), (c[0] + half, c[1] - half * 0.55),
               (c[0] + half, c[1] + half * 0.55), (c[0] - half, c[1] + half * 0.55)]
        self.fill(pts, fill)
        self.poly(pts, col, width, closed=True)

    def badge(self, c, col=ACC):
        """A constraint badge: a small dark square with a mark in it."""
        h = 0.055
        pts = [(c[0] - h, c[1] - h), (c[0] + h, c[1] - h), (c[0] + h, c[1] + h), (c[0] - h, c[1] + h)]
        self.fill(pts, FIELD)
        self.poly(pts, col, 1.0, closed=True)

    def pointer(self, p):
        x, y = self.xy(p)
        k = 10.5 * self.k()
        self.d.polygon([(x, y), (x, y + 1.45 * k), (x + 0.38 * k, y + 1.05 * k), (x + 0.62 * k, y + 1.55 * k),
                        (x + 0.90 * k, y + 1.40 * k), (x + 0.66 * k, y + 0.92 * k), (x + 1.05 * k, y + 0.84 * k)],
                       fill=CURSOR, outline=INK, width=max(1, SS))

    def finish(self):
        return np.asarray(self.img.resize((self.size, self.size), Image.LANCZOS))


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def rot(p, a, c=(0.0, 0.0)):
    s, co = math.sin(a), math.cos(a)
    x, y = p[0] - c[0], p[1] - c[1]
    return (c[0] + x * co - y * s, c[1] + x * s + y * co)


def circle_pts(c, r, a0=0.0, a1=360.0, n=None):
    n = n or max(12, int(abs(a1 - a0) / 4))
    return [(c[0] + r * math.cos(math.radians(a)), c[1] + r * math.sin(math.radians(a))) for a in np.linspace(a0, a1, n)]


def swing(k, n):
    """0 -> 1 -> 0 over the loop, with no step where it starts over."""
    return 0.5 - 0.5 * math.cos(2.0 * math.pi * k / n)


# --- snaps: a line being drawn, its free end pulled onto what is under the pointer --------------

SNAP_FRAMES = 20
ANCHOR = (0.62, -0.62)  # where the line being drawn started


def _snap(k, n, draw_target, target, approach):
    """The pointer comes in from `approach` towards `target`; once close it snaps on and holds."""
    sk = Sketch(GIF_SIZE)
    draw_target(sk)
    move = 11
    t = smooth(min(1.0, k / float(move)))
    cur = lerp(approach, (target[0] + 0.05, target[1] + 0.06), t)
    snapped = k >= move - 3
    end = target if snapped else cur
    sk.line(ANCHOR, end, ACC if snapped else ACC_SOFT, 2.4 if snapped else 1.8)
    sk.dot(ANCHOR, 2.6)
    if snapped:
        sk.dot(target, 3.6, ACC)
        sk.ring(target, 7.5 + (1.5 if k == move - 3 else 0.0))
    sk.pointer(cur)
    return sk.finish()


def snap_endpoint(k, n):
    a, b = (-0.7, -0.25), (0.05, 0.42)
    def draw(sk):
        sk.line(a, b)
        sk.dot(a, 2.4, GEOM)
    return _snap(k, n, draw, b, (0.55, 0.62))


def snap_center(k, n):
    c, r = (-0.2, 0.18), 0.42
    def draw(sk):
        sk.circle(c, r)
        sk.line((c[0] - 0.04, c[1]), (c[0] + 0.04, c[1]), DIM, 1.2)
        sk.line((c[0], c[1] - 0.04), (c[0], c[1] + 0.04), DIM, 1.2)
    return _snap(k, n, draw, c, (0.45, 0.55))


def snap_midpoint(k, n):
    a, b = (-0.75, 0.35), (0.55, 0.35)
    m = lerp(a, b, 0.5)
    def draw(sk):
        sk.line(a, b)
        sk.line((m[0], m[1] - 0.05), (m[0], m[1] + 0.05), DIM, 1.2)
    return _snap(k, n, draw, m, (0.4, 0.75))


def snap_on_edge(k, n):
    c, r = (-0.35, -0.05), 0.62
    arc = circle_pts(c, r, -40.0, 120.0)
    target = (c[0] + r * math.cos(math.radians(35.0)), c[1] + r * math.sin(math.radians(35.0)))
    def draw(sk):
        sk.poly(arc)
    return _snap(k, n, draw, target, (0.7, 0.7))


def snap_origin(k, n):
    o = (-0.25, 0.05)
    def draw(sk):
        sk.line((o[0] - 0.9, o[1]), (o[0] + 1.3, o[1]), DIM, 1.2, dash=True)
        sk.line((o[0], o[1] - 1.1), (o[0], o[1] + 1.1), DIM, 1.2, dash=True)
    return _snap(k, n, draw, o, (0.45, 0.6))


def angle_lock(k, n):
    """The free end swings across the horizontal; near it the line locks level and a guide shows."""
    sk = Sketch(GIF_SIZE)
    a = (-0.62, -0.1)
    length = 1.15
    # Lingers about the horizontal, where the lock holds, on each pass.
    c = math.cos(2.0 * math.pi * k / n)
    ang = math.radians(26.0) * math.copysign(max(0.0, (abs(c) - 0.35) / 0.65), c)
    cur = (a[0] + length * math.cos(ang), a[1] + length * math.sin(ang))
    locked = abs(math.degrees(ang)) < 6.0
    if locked:
        sk.line((-1.2, a[1]), (1.2, a[1]), ACC_SOFT, 1.2, dash=True)
        end = (a[0] + length * math.cos(ang), a[1])
        sk.line(a, end, ACC, 2.6)
        sk.badge((a[0] + 0.5 * length, a[1] + 0.13))
        mid = (a[0] + 0.5 * length, a[1] + 0.13)
        sk.line((mid[0] - 0.035, mid[1]), (mid[0] + 0.035, mid[1]), ACC, 1.4)
    else:
        sk.line(a, cur, ACC_SOFT, 2.2)
    sk.dot(a, 2.6)
    sk.pointer(cur)
    return sk.finish()


# --- handles: a drag reshapes what the handle belongs to --------------------------------------

DRAG_FRAMES = 20


def _grab(sk, p, t):
    """The handle being dragged, and the pointer on it."""
    sk.square(p, 4.0, ACC)
    sk.pointer(p)


def handle_point(k, n):
    sk = Sketch(GIF_SIZE)
    a, b = (-0.65, -0.45), (0.65, -0.45)
    p0 = (-0.05, 0.3)
    t = swing(k, n)
    p = lerp(p0, (0.35, 0.62), t)
    sk.line(a, b)
    sk.poly([a, p0, b], DIM, 1.2, dash=True)
    sk.poly([a, p, b], ACC, 2.6)
    sk.dot(a, 2.6)
    sk.dot(b, 2.6)
    _grab(sk, p, t)
    return sk.finish()


def handle_center(k, n):
    sk = Sketch(GIF_SIZE)
    c0, r = (-0.2, -0.05), 0.4
    t = swing(k, n)
    c = lerp(c0, (0.25, 0.2), t)
    sk.circle(c0, r, DIM, 1.2, dash=True)
    sk.circle(c, r, ACC, 2.6)
    sk.square((c[0] + r, c[1]), 3.0, POINT)
    _grab(sk, c, t)
    return sk.finish()


def handle_radius(k, n):
    sk = Sketch(GIF_SIZE)
    c = (-0.2, 0.0)
    t = swing(k, n)
    r = 0.3 + 0.32 * t
    sk.circle(c, 0.3, DIM, 1.2, dash=True)
    sk.circle(c, r, ACC, 2.6)
    sk.line(c, (c[0] + r, c[1]), DIM, 1.2)
    sk.dot(c, 2.6)
    _grab(sk, (c[0] + r, c[1]), t)
    return sk.finish()


def _ellipse(c, a, b, phi):
    pts = []
    for s in np.linspace(0.0, 2.0 * math.pi, 72):
        x, y = a * math.cos(s), b * math.sin(s)
        pts.append(rot((c[0] + x, c[1] + y), phi, c))
    return pts


def handle_ellipse(k, n):
    sk = Sketch(GIF_SIZE)
    c, b = (-0.1, -0.05), 0.28
    t = swing(k, n)
    a = 0.5 + 0.18 * t
    phi = math.radians(28.0) * t
    sk.poly(_ellipse(c, 0.5, b, 0.0), DIM, 1.2, closed=True, dash=True)
    sk.poly(_ellipse(c, a, b, phi), ACC, 2.6, closed=True)
    major = rot((c[0] + a, c[1]), phi, c)
    minor = rot((c[0], c[1] + b), phi, c)
    sk.line(c, major, DIM, 1.2)
    sk.line(c, minor, DIM, 1.2)
    sk.dot(c, 2.6)
    sk.square(minor, 3.0, POINT)
    _grab(sk, major, t)
    return sk.finish()


def _bezier(poles, m=60):
    p = np.array(poles, float)
    out = []
    for s in np.linspace(0.0, 1.0, m):
        q = p.copy()
        while len(q) > 1:
            q = q[:-1] * (1 - s) + q[1:] * s
        out.append(tuple(q[0]))
    return out


def handle_spline(k, n):
    sk = Sketch(GIF_SIZE)
    t = swing(k, n)
    base = [(-0.75, -0.4), (-0.3, 0.45), (0.3, -0.45), (0.75, 0.35)]
    moved = list(base)
    moved[1] = lerp(base[1], (0.0, 0.8), t)
    sk.poly(_bezier(base), DIM, 1.2, dash=True)
    sk.poly(moved, DIM, 1.0, dash=True)
    sk.poly(_bezier(moved), ACC, 2.6)
    for i, p in enumerate(moved):
        if i != 1:
            sk.square(p, 3.0, POINT)
    _grab(sk, moved[1], t)
    return sk.finish()


def _ngon(c, r, phase, sides=6):
    return [(c[0] + r * math.cos(phase + 2 * math.pi * i / sides), c[1] + r * math.sin(phase + 2 * math.pi * i / sides))
            for i in range(sides)]


def handle_polygon(k, n):
    sk = Sketch(GIF_SIZE)
    c = (-0.1, -0.05)
    t = swing(k, n)
    r = 0.42 + 0.2 * t
    phase = math.radians(25.0) * t
    sk.poly(_ngon(c, 0.42, 0.0), DIM, 1.2, closed=True, dash=True)
    pts = _ngon(c, r, phase)
    sk.poly(pts, ACC, 2.6, closed=True)
    for p in pts[1:]:
        sk.dot(p, 2.4)
    sk.dot(c, 2.0, DIM)
    _grab(sk, pts[0], t)
    return sk.finish()


def handle_corner(k, n):
    sk = Sketch(GIF_SIZE)
    fixed = (-0.65, -0.5)
    t = swing(k, n)
    corner = lerp((0.2, 0.2), (0.65, 0.6), t)
    rect0 = [fixed, (0.2, fixed[1]), (0.2, 0.2), (fixed[0], 0.2)]
    rect = [fixed, (corner[0], fixed[1]), corner, (fixed[0], corner[1])]
    sk.poly(rect0, DIM, 1.2, closed=True, dash=True)
    sk.poly(rect, ACC, 2.6, closed=True)
    sk.dot(rect[1], 2.4)
    sk.dot(rect[3], 2.4)
    sk.square(fixed, 3.4, GEOM)
    _grab(sk, corner, t)
    return sk.finish()


def _slot(c0, c1, r):
    d = np.array(c1, float) - np.array(c0, float)
    a = math.degrees(math.atan2(d[1], d[0]))
    cap1 = circle_pts(c1, r, a - 90.0, a + 90.0, 24)
    cap0 = circle_pts(c0, r, a + 90.0, a + 270.0, 24)
    return cap1 + cap0 + [cap1[0]]


def handle_slot(k, n):
    sk = Sketch(GIF_SIZE)
    c0, r = (-0.5, -0.15), 0.2
    t = swing(k, n)
    c1 = lerp((0.3, -0.15), (0.45, 0.4), t)
    sk.poly(_slot(c0, (0.3, -0.15), r), DIM, 1.2, dash=True)
    sk.poly(_slot(c0, c1, r), ACC, 2.6)
    sk.dot(c0, 2.4)
    _grab(sk, c1, t)
    return sk.finish()


# --- a value label: click it, type, and the shape follows ------------------------------------

def dim_edit(k, n):
    sk = Sketch(GIF_SIZE)
    a = (-0.8, -0.3)
    move, typed = 6, 10
    length = 0.95 + 0.6 * smooth((k - typed) / 4.0) if k >= typed else 0.95
    b = (a[0] + length, a[1])
    y = a[1] + 0.42
    editing = move <= k < typed + 2
    done = k >= typed
    sk.line(a, b, ACC if done else GEOM, 2.8 if done else 2.4)
    sk.dot(a, 3.0)
    sk.dot(b, 3.0)
    sk.line((a[0], a[1] + 0.05), (a[0], y + 0.07), DIM, 1.2)
    sk.line((b[0], b[1] + 0.05), (b[0], y + 0.07), DIM, 1.2)
    sk.line((a[0], y), (b[0], y), DIM, 1.4)
    sk.arrow_head((a[0], y), (-1, 0), DIM, 0.07)
    sk.arrow_head((b[0], y), (1, 0), DIM, 0.07)
    label = ((a[0] + b[0]) * 0.5, y)
    if editing:
        sk.box(label, 0.17, ACC, FIELD, 2.0)
        # the typed digits, as bars: no numbers in the picture
        for i in range(min(3, max(0, k - move))):
            x = label[0] - 0.08 + i * 0.07
            sk.line((x, label[1] - 0.035), (x, label[1] + 0.035), POINT, 2.0)
    else:
        sk.box(label, 0.14, ACC if done else GEOM, FIELD, 1.6)
    cur = lerp((0.6, 0.7), (label[0] + 0.03, label[1] - 0.03), smooth(k / float(move - 1)))
    if k == move:
        sk.ring(label, 12.0)
    if k < typed:
        sk.pointer(cur)
    return sk.finish()


# --- how constrained a sketch is, in the colours the canvas draws it in -----------------------

def _quad(t):
    return [(-0.55, -0.45), (0.5, -0.45), lerp((0.5, 0.35), (0.72, 0.6), t), (-0.55, 0.35)]


def state_under(k, n):
    sk = Sketch(GIF_SIZE)
    t = swing(k, n)
    pts = _quad(t)
    sk.poly(pts, ORANGE, 2.6, closed=True)
    for p in pts[:2] + pts[3:]:
        sk.dot(p, 2.4)
    sk.square(pts[2], 4.0, ORANGE)
    sk.pointer(pts[2])
    return sk.finish()


def _dims(sk, pts, col):
    a, b, c, d = pts
    y = a[1] - 0.2
    sk.line((a[0], a[1] - 0.04), (a[0], y - 0.05), DIM, 1.0)
    sk.line((b[0], b[1] - 0.04), (b[0], y - 0.05), DIM, 1.0)
    sk.line((a[0], y), (b[0], y), DIM, 1.2)
    sk.arrow_head((a[0], y), (-1, 0), DIM, 0.05)
    sk.arrow_head((b[0], y), (1, 0), DIM, 0.05)
    sk.box(((a[0] + b[0]) * 0.5, y), 0.1, col)
    x = a[0] - 0.2
    sk.line((a[0] - 0.04, a[1]), (x - 0.05, a[1]), DIM, 1.0)
    sk.line((d[0] - 0.04, d[1]), (x - 0.05, d[1]), DIM, 1.0)
    sk.line((x, a[1]), (x, d[1]), DIM, 1.2)
    sk.arrow_head((x, a[1]), (0, -1), DIM, 0.05)
    sk.arrow_head((x, d[1]), (0, 1), DIM, 0.05)
    sk.box((x, (a[1] + d[1]) * 0.5), 0.1, col)


def state_fully(k, n):
    sk = Sketch(SIZE)
    pts = [(-0.4, -0.35), (0.6, -0.35), (0.6, 0.45), (-0.4, 0.45)]
    sk.poly(pts, GREEN, 2.6, closed=True)
    for p in pts:
        sk.dot(p, 2.4)
    _dims(sk, pts, GREEN)
    # the corner pinned in place, and a horizontal and a vertical badge
    sk.badge((-0.4 + 0.12, -0.35 + 0.12), GREEN)
    sk.poly([(-0.32, -0.27), (-0.24, -0.27), (-0.24, -0.19), (-0.32, -0.19)], GREEN, 1.0, closed=True)
    hb = (0.1, 0.45 + 0.13)
    sk.badge(hb, GREEN)
    sk.line((hb[0] - 0.03, hb[1]), (hb[0] + 0.03, hb[1]), GREEN, 1.4)
    vb = (0.6 + 0.13, 0.05)
    sk.badge(vb, GREEN)
    sk.line((vb[0], vb[1] - 0.03), (vb[0], vb[1] + 0.03), GREEN, 1.4)
    return sk.finish()


def state_over(k, n):
    sk = Sketch(SIZE)
    pts = [(-0.4, -0.35), (0.6, -0.35), (0.6, 0.45), (-0.4, 0.45)]
    sk.poly([pts[3], pts[0], pts[1]], GEOM, 2.2)
    sk.poly([pts[1], pts[2], pts[3]], RED, 2.6)
    for p in pts:
        sk.dot(p, 2.4)
    _dims(sk, pts, GEOM)
    # a second dimension on the top edge asks for a length the bottom one already fixes
    y = pts[2][1] + 0.2
    sk.line((pts[3][0], pts[3][1] + 0.04), (pts[3][0], y + 0.05), RED, 1.0)
    sk.line((pts[2][0], pts[2][1] + 0.04), (pts[2][0], y + 0.05), RED, 1.0)
    sk.line((pts[3][0], y), (pts[2][0], y), RED, 1.2)
    sk.arrow_head((pts[3][0], y), (-1, 0), RED, 0.05)
    sk.arrow_head((pts[2][0], y), (1, 0), RED, 0.05)
    sk.box(((pts[2][0] + pts[3][0]) * 0.5, y), 0.1, RED, FIELD, 1.8)
    return sk.finish()


# --- the box that places imported text and SVG art --------------------------------------------

def _star(c, sx, sy, outer=0.32, inner=0.14):
    pts = []
    for i in range(10):
        r = outer if i % 2 == 0 else inner
        a = math.pi / 2 + i * math.pi / 5
        pts.append((c[0] + sx * r * math.cos(a), c[1] + sy * r * math.sin(a)))
    return pts


def art_transform(k, n):
    """A corner stretches the art, the opposite corner holding; then the centre moves it."""
    sk = Sketch(GIF_SIZE)
    half = n // 2
    if k < half:
        t = smooth(min(1.0, k / (half * 0.5))) if k < half * 0.5 else smooth(max(0.0, (half - k) / (half * 0.5)))
        sx, sy, off = 1.0 + 0.55 * t, 1.0 + 0.25 * t, (0.0, 0.0)
        grab = "corner"
    else:
        j = k - half
        t = smooth(min(1.0, j / (half * 0.5))) if j < half * 0.5 else smooth(max(0.0, (half - j) / (half * 0.5)))
        sx, sy, off = 1.0, 1.0, (0.3 * t, 0.22 * t)
        grab = "centre"
    lo0 = (-0.55, -0.45)
    w0, h0 = 0.7, 0.68
    lo = (lo0[0] + off[0], lo0[1] + off[1])
    hi = (lo[0] + w0 * sx, lo[1] + h0 * sy)
    c = ((lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5)
    sk.poly([lo0, (lo0[0] + w0, lo0[1]), (lo0[0] + w0, lo0[1] + h0), (lo0[0], lo0[1] + h0)], DIM, 1.0, closed=True, dash=True)
    star = _star(c, w0 * sx / 0.7, h0 * sy / 0.68)
    sk.fill(star, ACC + (90,))
    sk.poly(star, ACC, 2.2, closed=True)
    corners = [lo, (hi[0], lo[1]), hi, (lo[0], hi[1])]
    sk.poly(corners, ACC, 1.2, closed=True, dash=True)
    for p in corners:
        sk.square(p, 3.2, POINT)
    sk.square(c, 3.2, POINT)
    p = hi if grab == "corner" else c
    sk.square(p, 4.0, ACC)
    sk.pointer(p)
    return sk.finish()


FRAMES = {
    "snap_endpoint": SNAP_FRAMES,
    "snap_center": SNAP_FRAMES,
    "snap_midpoint": SNAP_FRAMES,
    "snap_on_edge": SNAP_FRAMES,
    "snap_origin": SNAP_FRAMES,
    "angle_lock": DRAG_FRAMES,
    "handle_point": DRAG_FRAMES,
    "handle_center": DRAG_FRAMES,
    "handle_radius": DRAG_FRAMES,
    "handle_ellipse": DRAG_FRAMES,
    "handle_spline": DRAG_FRAMES,
    "handle_polygon": DRAG_FRAMES,
    "handle_corner": DRAG_FRAMES,
    "handle_slot": DRAG_FRAMES,
    "dim_edit": 22,
    "state_under": DRAG_FRAMES,
    "state_fully": 0,
    "state_over": 0,
    "art_transform": 22,
}
