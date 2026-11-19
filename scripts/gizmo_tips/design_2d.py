"""The Design tab's sketch tooltips: one picture per sketch tool and per constraint (DesignPanel.cpp).

A sketch is flat, so these are drawn straight onto the sketch plane with PIL, seen from above, in the
palette of the ray-marched pictures: the dark plane, grey geometry, and ACCENT for what the tool draws
or what the constraint fixes. A drawing tool is an animation of its clicks - the pointer goes to each
point the tool asks for, the shape follows it, and the finished shape holds. A constraint is a still:
the geometry before it dashed and dim, after it in the accent colour.

The file names are the toolbar icons' (design_arc3pt.svg -> tool_arc3pt.gif), so the canvas, which
names the same tools, finds the same pictures.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

from .render import ACCENT, BED, CURSOR, GIF_SIZE, GRID, SIZE, SS, letters2, smooth, star2

OUT = "design_tips"  # resources/images/<OUT>

BG = tuple(int(c * 255 * 0.9) for c in BED)
GRID_C = tuple(int(c * 255) for c in GRID)
GEOM = (168, 176, 179)
DIM = (110, 116, 119)
ACC = (34, 178, 160)
ACC_SOFT = (34, 178, 160, 150)
POINT = (232, 238, 238)
INK = (30, 32, 34)
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

    def w(self, width):
        return max(1, int(round(width * SS * self.size / 192.0)))

    def poly(self, pts, col=GEOM, width=2.0, closed=False, dash=False):
        pts = [tuple(p) for p in pts]
        if closed:
            pts = pts + [pts[0]]
        if len(pts) < 2:
            return
        if not dash:
            self.d.line([self.xy(p) for p in pts], fill=col, width=self.w(width), joint="curve")
            return
        # Dashes of a fixed length along the whole polyline.
        on, off, acc, draw_on = 0.07, 0.05, 0.0, True
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
        self.arc(c, r, 0.0, 360.0, col, width, dash)

    def arc(self, c, r, a0, a1, col=GEOM, width=2.0, dash=False):
        """Counter-clockwise from a0 to a1, in degrees."""
        n = max(8, int(abs(a1 - a0) / 4))
        pts = [(c[0] + r * math.cos(math.radians(a)), c[1] + r * math.sin(math.radians(a))) for a in np.linspace(a0, a1, n)]
        self.poly(pts, col, width, dash=dash)

    def dot(self, p, r=3.0, col=POINT):
        x, y = self.xy(p)
        r = r * SS * self.size / 192.0
        self.d.ellipse([x - r, y - r, x + r, y + r], fill=col, outline=INK, width=max(1, SS))

    def ring(self, p, r=7.0, col=ACC):
        x, y = self.xy(p)
        r = r * SS * self.size / 192.0
        self.d.ellipse([x - r, y - r, x + r, y + r], outline=col, width=self.w(1.5))

    def fill(self, pts, col):
        self.d.polygon([self.xy(p) for p in pts], fill=col)

    def arrow(self, a, b, col=ACC, width=1.6, head=0.07):
        a, b = np.array(a, float), np.array(b, float)
        v = b - a
        n = np.linalg.norm(v)
        if n < 1e-9:
            return
        v /= n
        w = np.array([-v[1], v[0]])
        base = b - v * head
        self.line(a, base, col, width)
        self.fill([tuple(b), tuple(base + w * head * 0.45), tuple(base - w * head * 0.45)], col)

    def box(self, c, half=0.08, col=GEOM):
        """The empty value box a dimension shows its number in: no digits, they are not translated."""
        pts = [(c[0] - half, c[1] - half * 0.55), (c[0] + half, c[1] - half * 0.55),
               (c[0] + half, c[1] + half * 0.55), (c[0] - half, c[1] + half * 0.55)]
        self.fill(pts, (52, 56, 58))
        self.poly(pts, col, 1.2, closed=True)

    def pointer(self, p):
        x, y = self.xy(p)
        k = 10.5 * SS * self.size / 192.0
        self.d.polygon([(x, y), (x, y + 1.45 * k), (x + 0.38 * k, y + 1.05 * k), (x + 0.62 * k, y + 1.55 * k),
                        (x + 0.90 * k, y + 1.40 * k), (x + 0.66 * k, y + 0.92 * k), (x + 1.05 * k, y + 0.84 * k)],
                       fill=CURSOR, outline=INK, width=max(1, SS))

    def field(self, f, col, inside=None, width=0.012):
        """Draws a 2D distance field: its outline, and its inside when `inside` is a colour."""
        a = ((np.arange(self.px) + 0.5) / self.px * 2.0 - 1.0) / SCALE
        x, y = np.meshgrid(a, -a)
        d = f(x, y)
        img = np.asarray(self.img).astype(float)
        if inside is not None:
            m = (d < 0)[..., None]
            img = np.where(m, img * 0.35 + np.array(inside) * 0.65, img)
        edge = np.clip(1.0 - (np.abs(d) - width) / 0.006, 0.0, 1.0)[..., None]
        img = img * (1.0 - edge) + np.array(col) * edge
        self.img = Image.fromarray(img.astype(np.uint8))
        self.d = ImageDraw.Draw(self.img, "RGBA")

    def finish(self):
        return np.asarray(self.img.resize((self.size, self.size), Image.LANCZOS))


def lerp(a, b, t):
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def rot(p, a, c=(0.0, 0.0)):
    s, co = math.sin(a), math.cos(a)
    x, y = p[0] - c[0], p[1] - c[1]
    return (c[0] + x * co - y * s, c[1] + x * s + y * co)


# --- drawing tools: the pointer clicks each point the tool asks for --------------------------------

MOVE = 5  # frames from one click to the next
HOLD = 6  # frames the finished shape holds before the loop starts over


def click_frames(m, move=MOVE):
    return 1 + m * move + HOLD


def clicks(k, n, pts, draw, start=(0.85, -0.85), move=MOVE):
    """Frame k of the clicks on `pts`: draw(sk, clicked, cursor, done) draws the shape so far."""
    sk = Sketch(GIF_SIZE)
    m = len(pts)
    path = [start] + list(pts)
    if k == 0:
        cur, done, just = start, 0, False
    elif k >= 1 + m * move:
        cur, done, just = pts[-1], m, False
    else:
        seg, f = divmod(k - 1, move)
        t = (f + 1) / float(move)
        cur = lerp(path[seg], path[seg + 1], smooth(t))
        done, just = seg + (1 if f + 1 == move else 0), f + 1 == move
    draw(sk, list(pts[:done]), cur, done == m)
    for p in pts[:done]:
        sk.dot(p, 2.6)
    if just:
        sk.ring(cur)
    if done < m or k < 1 + m * move + 2:
        sk.pointer(cur)
    return sk.finish()


def col(final):
    return (ACC, 2.6) if final else (ACC_SOFT, 1.8)


def _rect(a, b):
    return [a, (b[0], a[1]), b, (a[0], b[1])]


# Each tool lists its clicks; the function draws the shape for the clicks made so far.

def _line(sk, pts, cur, final):
    if pts:
        c, w = col(final)
        sk.line(pts[0], pts[1] if len(pts) > 1 else cur, c, w)


L_PTS = [(-0.6, -0.4), (0.55, 0.45)]


def tool_line(k, n):
    return clicks(k, n, L_PTS, _line)


PL_PTS = [(-0.65, -0.5), (-0.2, 0.45), (0.3, -0.15), (0.65, 0.5)]


def tool_polyline(k, n):
    def draw(sk, pts, cur, final):
        if pts:
            c, w = col(final)
            sk.poly(pts + ([] if final else [cur]), c, w)
    return clicks(k, n, PL_PTS, draw)


R_PTS = [(-0.6, -0.45), (0.55, 0.4)]


def tool_rect(k, n):
    def draw(sk, pts, cur, final):
        if pts:
            c, w = col(final)
            sk.poly(_rect(pts[0], pts[1] if final else cur), c, w, closed=True)
    return clicks(k, n, R_PTS, draw)


CR_PTS = [(0.0, 0.0), (0.6, 0.42)]


def tool_crect(k, n):
    def draw(sk, pts, cur, final):
        if pts:
            c, w = col(final)
            b = pts[1] if final else cur
            a = (2 * pts[0][0] - b[0], 2 * pts[0][1] - b[1])
            sk.poly(_rect(a, b), c, w, closed=True)
            sk.line((pts[0][0] - 0.06, pts[0][1]), (pts[0][0] + 0.06, pts[0][1]), DIM, 1.2)
            sk.line((pts[0][0], pts[0][1] - 0.06), (pts[0][0], pts[0][1] + 0.06), DIM, 1.2)
    return clicks(k, n, CR_PTS, draw)


OR_PTS = [(-0.6, -0.3), (0.35, 0.35), (0.55, 0.05)]


def _oblique(a, b, p):
    e = np.array(b) - np.array(a)
    nrm = np.array([-e[1], e[0]]) / np.linalg.norm(e)
    h = float(np.dot(np.array(p) - np.array(a), nrm))
    off = nrm * h
    return [a, b, tuple(np.array(b) + off), tuple(np.array(a) + off)]


def tool_rect_oblique(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, c, w)
        elif len(pts) >= 2:
            sk.poly(_oblique(pts[0], pts[1], pts[2] if final else cur), c, w, closed=True)
    return clicks(k, n, OR_PTS, draw)


RR_PTS = [(-0.6, -0.42), (0.6, 0.42), (0.36, 0.42)]


def _rounded(a, b, r):
    x0, x1 = sorted((a[0], b[0]))
    y0, y1 = sorted((a[1], b[1]))
    r = max(0.0, min(r, 0.5 * (x1 - x0), 0.5 * (y1 - y0)))
    pts = []
    for cx, cy, a0 in ((x1 - r, y1 - r, 0), (x0 + r, y1 - r, 90), (x0 + r, y0 + r, 180), (x1 - r, y0 + r, 270)):
        for t in np.linspace(a0, a0 + 90, 10):
            pts.append((cx + r * math.cos(math.radians(t)), cy + r * math.sin(math.radians(t))))
    return pts


def tool_rect_rounded(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.poly(_rect(pts[0], cur), c, w, closed=True)
        elif len(pts) >= 2:
            p = pts[2] if final else cur
            r = abs(pts[1][0] - p[0]) + 0.02
            sk.poly(_rounded(pts[0], pts[1], r), c, w, closed=True)
    return clicks(k, n, RR_PTS, draw, start=(0.85, 0.85))


C_PTS = [(0.0, 0.0), (0.55, 0.3)]


def tool_circle(k, n):
    def draw(sk, pts, cur, final):
        if pts:
            c, w = col(final)
            b = pts[1] if final else cur
            r = math.dist(pts[0], b)
            sk.circle(pts[0], r, c, w)
            if not final:
                sk.line(pts[0], b, DIM, 1.0, dash=True)
    return clicks(k, n, C_PTS, draw)


C2_PTS = [(-0.55, -0.25), (0.5, 0.3)]


def tool_circle2pt(k, n):
    def draw(sk, pts, cur, final):
        if pts:
            c, w = col(final)
            b = pts[1] if final else cur
            m = lerp(pts[0], b, 0.5)
            sk.circle(m, math.dist(pts[0], b) / 2, c, w)
            sk.line(pts[0], b, DIM, 1.0, dash=True)
    return clicks(k, n, C2_PTS, draw)


def _circle3(a, b, c):
    ax, ay = a
    bx, by = b
    cx, cy = c
    d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by))
    if abs(d) < 1e-6:
        return None
    ux = ((ax * ax + ay * ay) * (by - cy) + (bx * bx + by * by) * (cy - ay) + (cx * cx + cy * cy) * (ay - by)) / d
    uy = ((ax * ax + ay * ay) * (cx - bx) + (bx * bx + by * by) * (ax - cx) + (cx * cx + cy * cy) * (bx - ax)) / d
    return (ux, uy), math.dist((ux, uy), a)


C3_PTS = [(-0.6, 0.0), (0.1, 0.6), (0.5, -0.35)]


def tool_circle3pt(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
        elif len(pts) >= 2:
            cc = _circle3(pts[0], pts[1], pts[2] if final else cur)
            if cc:
                sk.circle(cc[0], cc[1], c, w)
    return clicks(k, n, C3_PTS, draw)


def _arc3(a, b, m):
    """The arc from a to b through m, as (centre, radius, start, end) counter-clockwise."""
    cc = _circle3(a, m, b)
    if cc is None:
        return None
    (cx, cy), r = cc
    ang = lambda p: math.degrees(math.atan2(p[1] - cy, p[0] - cx)) % 360
    sa, sb, sm = ang(a), ang(b), ang(m)
    if (sm - sa) % 360 <= (sb - sa) % 360:
        return (cx, cy), r, sa, sa + (sb - sa) % 360
    return (cx, cy), r, sb, sb + (sa - sb) % 360


A3_PTS = [(-0.6, -0.3), (0.6, -0.3), (0.1, 0.45)]


def tool_arc3pt(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
        elif len(pts) >= 2:
            a = _arc3(pts[0], pts[1], pts[2] if final else cur)
            if a:
                sk.arc(a[0], a[1], a[2], a[3], c, w)
    return clicks(k, n, A3_PTS, draw)


def _tangent_arc(e, d, p):
    """The arc leaving point e in direction d and ending at p."""
    nrm = np.array([-d[1], d[0]])
    v = np.array(p) - np.array(e)
    den = 2 * np.dot(v, nrm)
    if abs(den) < 1e-6:
        return None
    t = np.dot(v, v) / den
    c = np.array(e) + nrm * t
    r = abs(t)
    a0 = math.degrees(math.atan2(e[1] - c[1], e[0] - c[0]))
    a1 = math.degrees(math.atan2(p[1] - c[1], p[0] - c[0]))
    if t > 0:  # turning left: counter-clockwise from e
        return tuple(c), r, a0, a0 + (a1 - a0) % 360
    return tuple(c), r, a1, a1 + (a0 - a1) % 360


TA_START = (0.0, -0.35)
TA_PTS = [TA_START, (0.55, 0.35)]


def tool_tangentarc(k, n):
    def draw(sk, pts, cur, final):
        sk.line((-0.7, -0.35), TA_START, GEOM, 2.2)
        if pts:
            c, w = col(final)
            a = _tangent_arc(TA_START, (1.0, 0.0), pts[1] if final else cur)
            if a:
                sk.arc(a[0], a[1], a[2], a[3], c, w)
    return clicks(k, n, TA_PTS, draw)


AC_PTS = [(0.0, -0.2), (0.6, -0.2), (-0.35, 0.3)]


def tool_arc_center(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
        elif len(pts) >= 2:
            r = math.dist(pts[0], pts[1])
            e = pts[2] if final else cur
            a1 = math.degrees(math.atan2(e[1] - pts[0][1], e[0] - pts[0][0])) % 360
            sk.arc(pts[0], r, 0.0, a1, c, w)
            sk.line(pts[0], pts[1], DIM, 1.0, dash=True)
            if not final:
                sk.line(pts[0], e, DIM, 1.0, dash=True)
    return clicks(k, n, AC_PTS, draw)


def _slot(a, b, r):
    a, b = np.array(a), np.array(b)
    ang = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
    pts = []
    for t in np.linspace(ang - 90, ang + 90, 16):
        pts.append(tuple(b + r * np.array([math.cos(math.radians(t)), math.sin(math.radians(t))])))
    for t in np.linspace(ang + 90, ang + 270, 16):
        pts.append(tuple(a + r * np.array([math.cos(math.radians(t)), math.sin(math.radians(t))])))
    return pts


SL_PTS = [(-0.45, -0.15), (0.45, 0.15), (0.4, 0.42)]


def tool_slot(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
        elif len(pts) >= 2:
            a, b = np.array(pts[0]), np.array(pts[1])
            e = (b - a) / np.linalg.norm(b - a)
            p = np.array(pts[2] if final else cur)
            r = max(0.04, abs(e[0] * (p - a)[1] - e[1] * (p - a)[0]))
            sk.poly(_slot(pts[0], pts[1], r), c, w, closed=True)
            sk.line(pts[0], pts[1], DIM, 1.0, dash=True)
    return clicks(k, n, SL_PTS, draw)


SA_PTS = [(0.0, -0.5), (0.6, -0.1), (-0.6, -0.1), (-0.45, 0.25)]


def tool_slot_arc(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
            return
        if len(pts) < 2:
            return
        ctr = pts[0]
        R = math.dist(ctr, pts[1])
        a0 = math.degrees(math.atan2(pts[1][1] - ctr[1], pts[1][0] - ctr[0]))
        e = pts[2] if len(pts) >= 3 else cur
        a1 = math.degrees(math.atan2(e[1] - ctr[1], e[0] - ctr[0]))
        a1 = a0 + (a1 - a0) % 360
        if len(pts) == 2:
            sk.arc(ctr, R, a0, a1, DIM, 1.2, dash=True)
            return
        p = pts[3] if final else cur
        h = max(0.04, abs(math.dist(ctr, p) - R))
        pts_o = [(ctr[0] + (R + h) * math.cos(math.radians(t)), ctr[1] + (R + h) * math.sin(math.radians(t))) for t in np.linspace(a0, a1, 30)]
        pts_i = [(ctr[0] + (R - h) * math.cos(math.radians(t)), ctr[1] + (R - h) * math.sin(math.radians(t))) for t in np.linspace(a1, a0, 30)]
        cap1 = [(ctr[0] + R * math.cos(math.radians(a1)) + h * math.cos(math.radians(t)),
                 ctr[1] + R * math.sin(math.radians(a1)) + h * math.sin(math.radians(t))) for t in np.linspace(a1, a1 + 180, 12)]
        cap0 = [(ctr[0] + R * math.cos(math.radians(a0)) + h * math.cos(math.radians(t)),
                 ctr[1] + R * math.sin(math.radians(a0)) + h * math.sin(math.radians(t))) for t in np.linspace(a0 + 180, a0 + 360, 12)]
        sk.poly(pts_o + cap1 + pts_i + cap0, c, w, closed=True)
        sk.arc(ctr, R, a0, a1, DIM, 1.0, dash=True)
    return clicks(k, n, SA_PTS, draw, move=4)


def _ellipse(c, a_end, b_pt, t0=0.0, t1=360.0):
    c, a_end, b_pt = np.array(c), np.array(a_end), np.array(b_pt)
    u = a_end - c
    A = np.linalg.norm(u)
    u /= A
    v = np.array([-u[1], u[0]])
    B = max(0.05, abs(np.dot(b_pt - c, v)))
    return [tuple(c + A * math.cos(math.radians(t)) * u + B * math.sin(math.radians(t)) * v) for t in np.linspace(t0, t1, 64)]


E_PTS = [(0.0, 0.0), (0.65, 0.2), (0.0, 0.4)]


def tool_ellipse(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
        elif len(pts) >= 2:
            sk.poly(_ellipse(pts[0], pts[1], pts[2] if final else cur), c, w, closed=True)
            sk.line(pts[0], pts[1], DIM, 1.0, dash=True)
    return clicks(k, n, E_PTS, draw)


EA_PTS = [(0.0, -0.1), (0.65, 0.1), (0.0, 0.32), (0.55, 0.3), (-0.6, -0.25)]


def tool_ellipse_arc(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        if len(pts) == 1:
            sk.line(pts[0], cur, DIM, 1.0, dash=True)
        elif len(pts) == 2:
            sk.line(pts[0], pts[1], DIM, 1.0, dash=True)
            sk.poly(_ellipse(pts[0], pts[1], cur), DIM, 1.0, closed=True, dash=True)
        elif len(pts) >= 3:
            sk.poly(_ellipse(pts[0], pts[1], pts[2]), DIM, 1.0, closed=True, dash=True)
            if len(pts) >= 4:
                c0 = np.array(pts[0])
                u = np.array(pts[1]) - c0
                u /= np.linalg.norm(u)
                v = np.array([-u[1], u[0]])
                ang = lambda p: math.degrees(math.atan2(np.dot(np.array(p) - c0, v), np.dot(np.array(p) - c0, u)))
                t0 = ang(pts[3])
                t1 = ang(pts[4] if final else cur)
                sk.poly(_ellipse(pts[0], pts[1], pts[2], t0, t0 + (t1 - t0) % 360), c, w)
    return clicks(k, n, EA_PTS, draw, move=4)


def _chaikin(pts, it=4):
    pts = [np.array(p, float) for p in pts]
    for _ in range(it):
        out = [pts[0]]
        for a, b in zip(pts[:-1], pts[1:]):
            out += [0.75 * a + 0.25 * b, 0.25 * a + 0.75 * b]
        out.append(pts[-1])
        pts = out
    return [tuple(p) for p in pts]


BS_PTS = [(-0.7, -0.4), (-0.35, 0.45), (0.15, -0.45), (0.45, 0.4), (0.7, -0.1)]


def tool_bspline(k, n):
    def draw(sk, pts, cur, final):
        c, w = col(final)
        ctrl = pts + ([] if final else [cur])
        if len(ctrl) >= 2:
            sk.poly(ctrl, DIM, 1.0, dash=True)
            sk.poly(_chaikin(ctrl), c, w)
    return clicks(k, n, BS_PTS, draw, move=4)


PT_PTS = [(-0.5, 0.3), (0.2, -0.35), (0.55, 0.4)]


def tool_point(k, n):
    def draw(sk, pts, cur, final):
        for p in pts:
            sk.dot(p, 4.5, ACC)
    return clicks(k, n, PT_PTS, draw)


PG_PTS = [(0.0, 0.0), (0.55, 0.25)]


def tool_polygon(k, n):
    def draw(sk, pts, cur, final):
        if pts:
            c, w = col(final)
            b = pts[1] if final else cur
            r = math.dist(pts[0], b)
            a0 = math.atan2(b[1] - pts[0][1], b[0] - pts[0][0])
            sk.circle(pts[0], r, DIM, 1.0, dash=True)
            sk.poly([(pts[0][0] + r * math.cos(a0 + i * math.pi / 3), pts[0][1] + r * math.sin(a0 + i * math.pi / 3)) for i in range(6)],
                    c, w, closed=True)
    return clicks(k, n, PG_PTS, draw)


def _dim_h(sk, a, b, y, c=ACC):
    """A horizontal dimension between a and b, its line at height y, with an empty value box."""
    sk.line(a, (a[0], y + 0.04 * np.sign(y - a[1])), c, 1.0)
    sk.line(b, (b[0], y + 0.04 * np.sign(y - b[1])), c, 1.0)
    m = ((a[0] + b[0]) / 2, y)
    sk.arrow(m, (a[0], y), c, 1.4, 0.06)
    sk.arrow(m, (b[0], y), c, 1.4, 0.06)
    sk.box(m, 0.11, c)


def _dim_v(sk, a, b, x, c=ACC):
    sk.line(a, (x + 0.04 * np.sign(x - a[0]), a[1]), c, 1.0)
    sk.line(b, (x + 0.04 * np.sign(x - b[0]), b[1]), c, 1.0)
    m = (x, (a[1] + b[1]) / 2)
    sk.arrow(m, (x, a[1]), c, 1.4, 0.06)
    sk.arrow(m, (x, b[1]), c, 1.4, 0.06)
    sk.box(m, 0.11, c)


DM_A, DM_B = (-0.55, -0.25), (0.55, -0.25)
DM_PTS = [DM_A, DM_B, (0.1, 0.25)]


def tool_dimension(k, n):
    def draw(sk, pts, cur, final):
        sk.poly(_rect((-0.55, -0.25), (0.55, -0.6)), GEOM, 2.2, closed=True)
        if len(pts) >= 2:
            _dim_h(sk, DM_A, DM_B, (pts[2] if final else cur)[1], ACC if final else (34, 178, 160, 170))
    return clicks(k, n, DM_PTS, draw)


# --- tools that act on what is there: pick, then the change happens ---------------------------------

def _edit(k, n, picks, base, change, steps=6):
    """Clicks on `picks`, then `change(sk, t)` plays t from 0 to 1 and holds."""
    m = len(picks)
    pick_end = 1 + m * MOVE
    sk = Sketch(GIF_SIZE)
    path = [(0.85, -0.85)] + list(picks)
    if k == 0:
        cur, done, just = path[0], 0, False
    elif k < pick_end:
        seg, f = divmod(k - 1, MOVE)
        cur = lerp(path[seg], path[seg + 1], smooth((f + 1) / float(MOVE)))
        done, just = seg + (1 if f + 1 == MOVE else 0), f + 1 == MOVE
    else:
        cur, done, just = picks[-1], m, False
    t = smooth(min(1.0, max(0.0, (k - pick_end + 1) / float(steps)))) if k >= pick_end else 0.0
    base(sk, done)
    if done == m:
        change(sk, t)
    if just:
        sk.ring(cur)
    if k < pick_end + 1:
        sk.pointer(cur)
    return sk.finish()


def edit_frames(m, steps=6):
    return 1 + m * MOVE + steps + HOLD


SEL_PTS = [(-0.35, 0.42), (0.6, 0.0)]


def tool_select(k, n):
    def base(sk, done):
        segs = [((-0.6, 0.42), (0.6, 0.42)), ((0.6, 0.42), (0.6, -0.42)), ((0.6, -0.42), (-0.6, -0.42)), ((-0.6, -0.42), (-0.6, 0.42))]
        chosen = [0, 1][:done]
        for i, (a, b) in enumerate(segs):
            sk.line(a, b, ACC if i in chosen else GEOM, 2.8 if i in chosen else 2.0)
        sk.circle((0.0, 0.0), 0.2, GEOM, 2.0)
    return _edit(k, n, SEL_PTS, base, lambda sk, t: None, steps=1)


CORNER = (0.45, 0.4)
FL_PTS = [(-0.2, 0.4), (0.45, -0.1)]


def _corner_tool(k, n, chamfer):
    r = 0.32

    def draw_all(sk, done, t):
        a_col = ACC if done >= 1 else GEOM
        b_col = ACC if done >= 2 else GEOM
        cut = r * t
        sk.line((-0.6, 0.4), (CORNER[0] - cut, 0.4), a_col if t == 0 else GEOM, 2.2)
        sk.line((CORNER[0], -0.6), (CORNER[0], 0.4 - cut), b_col if t == 0 else GEOM, 2.2)
        if t > 0:
            if chamfer:
                sk.line((CORNER[0] - cut, 0.4), (CORNER[0], 0.4 - cut), ACC, 2.8)
            else:
                sk.arc((CORNER[0] - cut, 0.4 - cut), cut, 0.0, 90.0, ACC, 2.8)
            sk.line((CORNER[0] - cut, 0.4), CORNER, DIM, 1.0, dash=True)
            sk.line(CORNER, (CORNER[0], 0.4 - cut), DIM, 1.0, dash=True)

    def base(sk, done):
        if done < 2:
            draw_all(sk, done, 0.0)

    return _edit(k, n, FL_PTS, base, lambda sk, t: draw_all(sk, 2, t))


def tool_filletedge(k, n):
    return _corner_tool(k, n, False)


def tool_chamfer(k, n):
    return _corner_tool(k, n, True)


OFF_SHAPE = [(-0.35, -0.3), (0.35, -0.3), (0.35, 0.0), (0.0, 0.0), (0.0, 0.3), (-0.35, 0.3)]


def _offset_poly(pts, d):
    """Offsets a closed counter-clockwise polygon outward by d (mitred corners)."""
    out = []
    n = len(pts)
    for i in range(n):
        p0, p1, p2 = np.array(pts[i - 1]), np.array(pts[i]), np.array(pts[(i + 1) % n])
        e0 = (p1 - p0) / np.linalg.norm(p1 - p0)
        e1 = (p2 - p1) / np.linalg.norm(p2 - p1)
        n0, n1 = np.array([e0[1], -e0[0]]), np.array([e1[1], -e1[0]])
        m = n0 + n1
        m /= np.linalg.norm(m)
        out.append(tuple(p1 + m * d / max(0.3, np.dot(m, n0))))
    return out


def tool_offset(k, n):
    def base(sk, done):
        sk.poly(OFF_SHAPE, ACC if done else GEOM, 2.2, closed=True)

    def change(sk, t):
        d = 0.22 * t
        if d > 0.005:
            sk.poly(_offset_poly(OFF_SHAPE, d), ACC, 2.6, closed=True)
            sk.arrow((0.35, -0.15), (0.35 + d, -0.15), POINT, 1.2, 0.05)
    return _edit(k, n, [(0.35, -0.15)], base, change)


MIR_SHAPE = [(-0.65, -0.35), (-0.15, -0.35), (-0.15, -0.05), (-0.4, 0.35), (-0.65, 0.35)]


def tool_mirror(k, n):
    def base(sk, done):
        sk.line((0.0, -0.75), (0.0, 0.75), ACC if done >= 1 else DIM, 1.6 if done >= 1 else 1.2, dash=True)
        sk.poly(MIR_SHAPE, ACC if done >= 2 else GEOM, 2.2, closed=True)

    def change(sk, t):
        pts = [(x * (1.0 - 2.0 * t), y) for x, y in MIR_SHAPE]
        sk.poly(pts, ACC, 2.6, closed=True)
    return _edit(k, n, [(0.0, 0.55), (-0.4, -0.35)], base, change)


def tool_trim(k, n):
    def base(sk, done):
        sk.line((-0.2, -0.7), (-0.2, 0.7), GEOM, 2.2)
        sk.line((0.3, -0.7), (0.3, 0.7), GEOM, 2.2)
        sk.line((-0.7, 0.0), (-0.2, 0.0), GEOM, 2.2)
        sk.line((0.3, 0.0), (0.7, 0.0), GEOM, 2.2)
        sk.line((-0.2, 0.0), (0.3, 0.0), ACC if done else GEOM, 2.2)

    def change(sk, t):
        if t < 1.0:
            sk.line((-0.2, 0.0), (0.3, 0.0), BG, 3.2)
            c = tuple(int(DIM[i] * (1 - t) + BG[i] * t) for i in range(3))
            sk.line((-0.2, 0.0), (0.3, 0.0), c, 1.4, dash=True)
        else:
            sk.line((-0.2, 0.0), (0.3, 0.0), BG, 3.2)
        sk.dot((-0.2, 0.0), 3.0, ACC)
        sk.dot((0.3, 0.0), 3.0, ACC)
    return _edit(k, n, [(0.05, 0.0)], base, change)


def tool_extend(k, n):
    def base(sk, done):
        sk.line((0.5, -0.7), (0.5, 0.7), GEOM, 2.2)
        sk.line((-0.6, -0.1), (0.0, 0.2), ACC if done else GEOM, 2.2)

    def change(sk, t):
        # Out along its own direction to the line it meets.
        end = (0.5, 0.2 + 0.5 * 0.5)
        sk.line((0.0, 0.2), lerp((0.0, 0.2), end, t), ACC, 2.8)
        if t >= 1.0:
            sk.dot(end, 3.0, ACC)
    return _edit(k, n, [(-0.2, 0.0)], base, change)


SHAPE = [(-0.25, -0.2), (0.25, -0.2), (0.25, 0.05), (0.0, 0.25), (-0.25, 0.05)]


def _moved(pts, dx=0.0, dy=0.0, a=0.0, s=1.0, c=(0.0, 0.0)):
    out = []
    for p in pts:
        q = ((p[0] - c[0]) * s, (p[1] - c[1]) * s)
        q = rot(q, a)
        out.append((q[0] + c[0] + dx, q[1] + c[1] + dy))
    return out


SH0 = _moved(SHAPE, -0.35, -0.2)


def tool_move(k, n):
    def base(sk, done):
        if not done:
            sk.poly(SH0, GEOM, 2.2, closed=True)

    def change(sk, t):
        sk.poly(SH0, DIM, 1.0, closed=True, dash=True)
        sk.poly(_moved(SH0, 0.65 * t, 0.45 * t), ACC, 2.6, closed=True)
        sk.arrow((-0.35, -0.2), (-0.35 + 0.65 * t + 0.001, -0.2 + 0.45 * t), POINT, 1.2, 0.05)
    return _edit(k, n, [(-0.35, -0.2)], base, change)


def tool_rotate(k, n):
    ctr = (0.0, 0.0)
    sh = _moved(SHAPE, 0.0, 0.0, 0.0, 1.6)

    def base(sk, done):
        if not done:
            sk.poly(sh, GEOM, 2.2, closed=True)

    def change(sk, t):
        a = math.radians(70) * t
        sk.poly(sh, DIM, 1.0, closed=True, dash=True)
        sk.poly(_moved(sh, 0, 0, a, 1.0, ctr), ACC, 2.6, closed=True)
        sk.arc(ctr, 0.62, -20, -20 + 70 * t + 0.01, POINT, 1.2)
        sk.dot(ctr, 2.5, POINT)
    return _edit(k, n, [(0.2, -0.2)], base, change)


def tool_scale(k, n):
    sh = _moved(SHAPE, 0.0, 0.0, 0.0, 1.0)

    def base(sk, done):
        if not done:
            sk.poly(sh, GEOM, 2.2, closed=True)

    def change(sk, t):
        sk.poly(sh, DIM, 1.0, closed=True, dash=True)
        sk.poly(_moved(sh, 0, 0, 0, 1.0 + 1.0 * t), ACC, 2.6, closed=True)
        sk.dot((0.0, 0.0), 2.5, POINT)
    return _edit(k, n, [(0.1, 0.0)], base, change)


def tool_array(k, n):
    sh = _moved(SHAPE, -0.55, 0.0, 0.0, 0.55)

    def base(sk, done):
        sk.poly(sh, ACC if done else GEOM, 2.2, closed=True)

    def change(sk, t):
        count = 1 + int(round(3 * t))
        for i in range(1, count):
            sk.poly(_moved(sh, 0.37 * i, 0.0), ACC, 2.6, closed=True)
        sk.arrow((-0.55, -0.3), (-0.55 + 0.37 * max(1, count - 1) * min(1.0, 3 * t), -0.3), POINT, 1.2, 0.05)
    return _edit(k, n, [(-0.55, 0.0)], base, change, steps=8)


def tool_polararray(k, n):
    sh = _moved(SHAPE, 0.0, 0.5, 0.0, 0.45)

    def base(sk, done):
        sk.poly(sh, ACC if done else GEOM, 2.2, closed=True)
        sk.dot((0.0, 0.0), 2.5, POINT)

    def change(sk, t):
        count = 1 + int(round(5 * t))
        for i in range(1, count):
            sk.poly(_moved(sh, 0, 0, i * math.pi / 3), ACC, 2.6, closed=True)
        sk.arc((0.0, 0.0), 0.5, 90, 90 + 300 * t + 0.01, DIM, 1.0, dash=True)
    return _edit(k, n, [(0.0, 0.5)], base, change, steps=8)


def _art(k, n, field):
    """Text and SVG: drag a box, and the art fills it as a profile."""
    def draw(sk, pts, cur, final):
        if pts:
            b = pts[1] if final else cur
            sk.poly(_rect(pts[0], b), DIM, 1.0, closed=True, dash=True)
            if final:
                cx, cy = (pts[0][0] + b[0]) / 2, (pts[0][1] + b[1]) / 2
                s = abs(b[0] - pts[0][0]) / 1.4
                sk.field(lambda x, y: field((x - cx) / s, (y - cy) / s) * s, ACC, inside=ACC)
    return clicks(k, n, [(-0.7, -0.4), (0.7, 0.4)], draw)


def tool_text(k, n):
    return _art(k, n, lambda x, y: letters2(x, y, 1.0))


def tool_svg(k, n):
    return _art(k, n, lambda x, y: star2(x, y * 1.4, 0.55, 0.24))


def tool_construction(k, n):
    sk = Sketch(SIZE)
    sk.poly(_rect((-0.6, -0.45), (0.6, 0.45)), GEOM, 2.2, closed=True)
    sk.line((-0.6, -0.45), (0.6, 0.45), ACC, 1.6, dash=True)
    sk.line((-0.6, 0.45), (0.6, -0.45), ACC, 1.6, dash=True)
    sk.circle((0.0, 0.0), 0.2, GEOM, 2.2)
    sk.dot((0.0, 0.0), 2.6, ACC)
    return sk.finish()


def _glyph(sk, c, kind):
    """A constraint badge as the sketch draws it: a small dark square with a symbol."""
    h = 0.075
    sk.fill([(c[0] - h, c[1] - h), (c[0] + h, c[1] - h), (c[0] + h, c[1] + h), (c[0] - h, c[1] + h)], (24, 26, 28))
    sk.poly([(c[0] - h, c[1] - h), (c[0] + h, c[1] - h), (c[0] + h, c[1] + h), (c[0] - h, c[1] + h)], ACC, 1.0, closed=True)
    g = h * 0.55
    if kind == "h":
        sk.line((c[0] - g, c[1]), (c[0] + g, c[1]), POINT, 1.4)
    elif kind == "v":
        sk.line((c[0], c[1] - g), (c[0], c[1] + g), POINT, 1.4)
    else:
        sk.line((c[0] - g, c[1] - g), (c[0] - g, c[1] + g), POINT, 1.4)
        sk.line((c[0] - g, c[1] - g), (c[0] + g, c[1] - g), POINT, 1.4)


def tool_constrain(k, n):
    sk = Sketch(SIZE)
    sk.poly(_rect((-0.55, -0.4), (0.55, 0.4)), ACC, 2.4, closed=True)
    _glyph(sk, (0.0, 0.52), "h")
    _glyph(sk, (0.0, -0.52), "h")
    _glyph(sk, (0.67, 0.0), "v")
    _glyph(sk, (-0.67, 0.0), "v")
    _glyph(sk, (-0.42, 0.27), "p")
    return sk.finish()


# --- constraints: before dashed and dim, after in the accent colour ------------------------------


def _still():
    return Sketch(SIZE)


def tool_c_horizontal(k, n):
    sk = _still()
    sk.line((-0.55, -0.3), (0.55, 0.3), DIM, 1.4, dash=True)
    sk.line((-0.62, 0.0), (0.62, 0.0), ACC, 2.8)
    for p in ((-0.62, 0.0), (0.62, 0.0)):
        sk.dot(p, 3.0)
    return sk.finish()


def tool_c_vertical(k, n):
    sk = _still()
    sk.line((-0.3, -0.55), (0.3, 0.55), DIM, 1.4, dash=True)
    sk.line((0.0, -0.62), (0.0, 0.62), ACC, 2.8)
    for p in ((0.0, -0.62), (0.0, 0.62)):
        sk.dot(p, 3.0)
    return sk.finish()


def tool_c_parallel(k, n):
    sk = _still()
    sk.line((-0.7, -0.45), (0.5, 0.0), GEOM, 2.2)
    sk.line((-0.5, 0.05), (0.6, 0.6), DIM, 1.4, dash=True)
    sk.line((-0.55, 0.2), (0.65, 0.65), ACC, 2.8)
    return sk.finish()


def tool_c_perpendicular(k, n):
    sk = _still()
    sk.line((-0.65, -0.35), (0.65, -0.35), GEOM, 2.2)
    sk.line((0.0, -0.35), (0.45, 0.5), DIM, 1.4, dash=True)
    sk.line((0.0, -0.35), (0.0, 0.6), ACC, 2.8)
    sk.poly([(0.0, -0.23), (0.12, -0.23), (0.12, -0.35)], POINT, 1.2)
    return sk.finish()


def tool_c_coincident(k, n):
    sk = _still()
    sk.line((-0.65, -0.4), (0.0, 0.1), GEOM, 2.2)
    sk.line((0.2, 0.25), (0.65, -0.4), DIM, 1.4, dash=True)
    sk.line((0.0, 0.1), (0.65, -0.4), ACC, 2.8)
    sk.dot((0.2, 0.25), 2.5, DIM)
    sk.dot((0.0, 0.1), 4.0, ACC)
    return sk.finish()


def _ticks(sk, a, b, col):
    a, b = np.array(a), np.array(b)
    m = (a + b) / 2
    e = (b - a) / np.linalg.norm(b - a)
    nrm = np.array([-e[1], e[0]])
    for o in (-0.025, 0.025):
        c = m + e * o
        sk.line(tuple(c - nrm * 0.05), tuple(c + nrm * 0.05), col, 1.4)


def tool_c_equal(k, n):
    sk = _still()
    sk.line((-0.65, -0.35), (0.45, -0.35), GEOM, 2.2)
    _ticks(sk, (-0.65, -0.35), (0.45, -0.35), GEOM)
    sk.line((-0.55, 0.3), (-0.05, 0.3), DIM, 1.4, dash=True)
    sk.line((-0.55, 0.3), (0.55, 0.3), ACC, 2.8)
    _ticks(sk, (-0.55, 0.3), (0.55, 0.3), ACC)
    return sk.finish()


def tool_c_equal_radius(k, n):
    sk = _still()
    sk.circle((-0.35, 0.0), 0.3, GEOM, 2.2)
    sk.circle((0.4, 0.0), 0.14, DIM, 1.4, dash=True)
    sk.circle((0.4, 0.0), 0.3, ACC, 2.8)
    return sk.finish()


def tool_c_collinear(k, n):
    sk = _still()
    sk.line((-0.7, -0.4), (-0.1, -0.1), GEOM, 2.2)
    sk.line((0.1, 0.25), (0.65, 0.55), DIM, 1.4, dash=True)
    sk.line((-0.1, -0.1), (0.1, 0.0), DIM, 1.0, dash=True)
    sk.line((0.1, 0.0), (0.7, 0.3), ACC, 2.8)
    return sk.finish()


def tool_c_concentric(k, n):
    sk = _still()
    sk.circle((0.0, 0.0), 0.55, GEOM, 2.2)
    sk.dot((0.0, 0.0), 2.6)
    sk.circle((0.25, 0.2), 0.25, DIM, 1.4, dash=True)
    sk.circle((0.0, 0.0), 0.25, ACC, 2.8)
    return sk.finish()


def tool_c_tangent(k, n):
    sk = _still()
    sk.circle((0.0, -0.1), 0.38, GEOM, 2.2)
    sk.line((-0.7, 0.05), (0.7, 0.4), DIM, 1.4, dash=True)
    sk.line((-0.7, 0.28), (0.7, 0.28), ACC, 2.8)
    sk.dot((0.0, 0.28), 3.4, ACC)
    return sk.finish()


def tool_c_midpoint(k, n):
    sk = _still()
    sk.line((-0.65, -0.35), (0.65, -0.35), GEOM, 2.2)
    sk.line((0.3, -0.35), (0.3, 0.5), DIM, 1.4, dash=True)
    sk.line((0.0, -0.35), (0.0, 0.5), ACC, 2.8)
    sk.dot((0.0, -0.35), 4.0, ACC)
    _ticks(sk, (-0.65, -0.35), (0.0, -0.35), GEOM)
    _ticks(sk, (0.0, -0.35), (0.65, -0.35), GEOM)
    return sk.finish()


def tool_c_symmetric(k, n):
    sk = _still()
    sk.line((-0.5, -0.65), (0.5, 0.65), DIM, 1.2, dash=True)
    sk.dot((-0.45, 0.3), 4.0, GEOM)
    sk.dot((0.2, -0.55), 3.0, DIM)
    sk.dot((0.3, -0.45), 4.0, ACC)
    sk.line((-0.45, 0.3), (0.3, -0.45), ACC, 1.0, dash=True)
    return sk.finish()


def _tri(x0, s=1.0):
    return [(x0, -0.35), (x0 + 0.45 * s, -0.35), (x0 + 0.1 * s, 0.4)]


def tool_c_sym_v(k, n):
    sk = _still()
    sk.line((0.0, -0.75), (0.0, 0.75), DIM, 1.2, dash=True)
    sk.poly(_tri(-0.1, -1.0), GEOM, 2.2, closed=True)
    sk.poly([(x + 0.12, y + 0.08) for x, y in _tri(0.1)], DIM, 1.4, closed=True, dash=True)
    sk.poly(_tri(0.1), ACC, 2.8, closed=True)
    return sk.finish()


def tool_c_sym_h(k, n):
    sk = _still()
    sk.line((-0.75, 0.0), (0.75, 0.0), DIM, 1.2, dash=True)
    tri = [(y, x) for x, y in _tri(-0.1, -1.0)]
    sk.poly([(-x, y) for x, y in tri], GEOM, 2.2, closed=True)
    low = [(-x, -y) for x, y in tri]
    sk.poly([(x + 0.12, y - 0.06) for x, y in low], DIM, 1.4, closed=True, dash=True)
    sk.poly(low, ACC, 2.8, closed=True)
    return sk.finish()


def tool_c_angle(k, n):
    sk = _still()
    o = (-0.55, -0.4)
    sk.line(o, (0.7, -0.4), GEOM, 2.2)
    sk.line(o, (0.6, 0.2), DIM, 1.4, dash=True)
    e = (o[0] + 1.25 * math.cos(math.radians(40)), o[1] + 1.25 * math.sin(math.radians(40)))
    sk.line(o, e, ACC, 2.8)
    sk.arc(o, 0.5, 0.0, 40.0, ACC, 1.4)
    sk.box((o[0] + 0.68, o[1] + 0.16), 0.1, ACC)
    return sk.finish()


def tool_c_radius(k, n):
    sk = _still()
    sk.circle((0.0, 0.0), 0.55, ACC, 2.8)
    sk.dot((0.0, 0.0), 2.6)
    e = (0.55 * math.cos(math.radians(35)), 0.55 * math.sin(math.radians(35)))
    sk.arrow((0.0, 0.0), e, ACC, 1.4, 0.07)
    sk.box((0.2, 0.08), 0.1, ACC)
    return sk.finish()


def tool_c_diameter(k, n):
    sk = _still()
    sk.circle((0.0, 0.0), 0.55, ACC, 2.8)
    e = (0.55 * math.cos(math.radians(30)), 0.55 * math.sin(math.radians(30)))
    sk.arrow((0.0, 0.0), e, ACC, 1.4, 0.07)
    sk.arrow((0.0, 0.0), (-e[0], -e[1]), ACC, 1.4, 0.07)
    sk.box((0.0, 0.0), 0.1, ACC)
    return sk.finish()


def tool_c_fix(k, n):
    sk = _still()
    p = (-0.1, 0.05)
    sk.line(p, (0.6, 0.45), GEOM, 2.2)
    sk.line(p, (0.55, -0.4), GEOM, 2.2)
    sk.fill([(p[0], p[1] - 0.03), (p[0] - 0.13, p[1] - 0.24), (p[0] + 0.13, p[1] - 0.24)], ACC)
    sk.line((p[0] - 0.2, p[1] - 0.24), (p[0] + 0.2, p[1] - 0.24), ACC, 2.0)
    for i in range(5):
        x = p[0] - 0.18 + i * 0.09
        sk.line((x, p[1] - 0.24), (x - 0.06, p[1] - 0.32), ACC, 1.2)
    sk.dot(p, 4.5, ACC)
    return sk.finish()


def tool_c_dist_x(k, n):
    sk = _still()
    a, b = (-0.5, -0.35), (0.45, 0.2)
    sk.line(a, b, GEOM, 2.2)
    sk.dot(a, 3.0)
    sk.dot(b, 3.0)
    _dim_h(sk, a, b, 0.5)
    return sk.finish()


def tool_c_dist_y(k, n):
    sk = _still()
    a, b = (-0.45, -0.5), (0.2, 0.45)
    sk.line(a, b, GEOM, 2.2)
    sk.dot(a, 3.0)
    sk.dot(b, 3.0)
    _dim_v(sk, a, b, 0.55)
    return sk.finish()


# --- the list --------------------------------------------------------------------------------------

FRAMES = {
    "tool_line": click_frames(len(L_PTS)),
    "tool_polyline": click_frames(len(PL_PTS)),
    "tool_rect": click_frames(len(R_PTS)),
    "tool_crect": click_frames(len(CR_PTS)),
    "tool_rect_oblique": click_frames(len(OR_PTS)),
    "tool_rect_rounded": click_frames(len(RR_PTS)),
    "tool_circle": click_frames(len(C_PTS)),
    "tool_circle2pt": click_frames(len(C2_PTS)),
    "tool_circle3pt": click_frames(len(C3_PTS)),
    "tool_arc3pt": click_frames(len(A3_PTS)),
    "tool_tangentarc": click_frames(len(TA_PTS)),
    "tool_arc_center": click_frames(len(AC_PTS)),
    "tool_slot": click_frames(len(SL_PTS)),
    "tool_slot_arc": click_frames(len(SA_PTS), 4),
    "tool_ellipse": click_frames(len(E_PTS)),
    "tool_ellipse_arc": click_frames(len(EA_PTS), 4),
    "tool_bspline": click_frames(len(BS_PTS), 4),
    "tool_point": click_frames(len(PT_PTS)),
    "tool_polygon": click_frames(len(PG_PTS)),
    "tool_dimension": click_frames(len(DM_PTS)),
    "tool_select": edit_frames(len(SEL_PTS), 1),
    "tool_filletedge": edit_frames(len(FL_PTS)),
    "tool_chamfer": edit_frames(len(FL_PTS)),
    "tool_offset": edit_frames(1),
    "tool_mirror": edit_frames(2),
    "tool_trim": edit_frames(1),
    "tool_extend": edit_frames(1),
    "tool_move": edit_frames(1),
    "tool_rotate": edit_frames(1),
    "tool_scale": edit_frames(1),
    "tool_array": edit_frames(1, 8),
    "tool_polararray": edit_frames(1, 8),
    "tool_text": click_frames(2),
    "tool_svg": click_frames(2),
    "tool_construction": 0,
    "tool_constrain": 0,
    "tool_c_horizontal": 0,
    "tool_c_vertical": 0,
    "tool_c_parallel": 0,
    "tool_c_perpendicular": 0,
    "tool_c_coincident": 0,
    "tool_c_equal": 0,
    "tool_c_equal_radius": 0,
    "tool_c_collinear": 0,
    "tool_c_concentric": 0,
    "tool_c_tangent": 0,
    "tool_c_midpoint": 0,
    "tool_c_symmetric": 0,
    "tool_c_sym_v": 0,
    "tool_c_sym_h": 0,
    "tool_c_angle": 0,
    "tool_c_radius": 0,
    "tool_c_diameter": 0,
    "tool_c_fix": 0,
    "tool_c_dist_x": 0,
    "tool_c_dist_y": 0,
}
