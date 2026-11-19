"""The renderer behind the gizmo tooltip pictures: a small ray marcher over signed distance functions,
with the overlays (handles, pointer, brush) drawn on top. Scene modules beside this one import it; see
scripts/gen_gizmo_tips.py for how they are run.

The look follows docs/HLSD/tooltip-illustrations.md: the plater's default view of a grey model on the
bed, one accent colour (ACCENT) for what the tool made or acts on, no text in the picture.
"""

import math

import numpy as np
from PIL import Image, ImageDraw

SIZE = 256  # px, square: stills
GIF_SIZE = 192  # animations are smaller, since every frame of one ships in resources/
SS = 2  # supersampling: render at twice the size and shrink, for antialiased edges
DELAY_MS = 100

# Neutral enough to sit in a tooltip in either theme. ACCENT is the panel's teal and the only colour
# in the set: whatever the tool made or acts on - a painted area, a cut face, the second part.
BED = np.array([0.17, 0.18, 0.19])
GRID = np.array([0.23, 0.24, 0.25])
PART = np.array([0.66, 0.69, 0.70])
PART2 = np.array([0.47, 0.51, 0.54])
ACCENT = np.array([0.10, 0.60, 0.54])
SUPPORT = np.array([0.84, 0.86, 0.86])
# The 3D view's reddish overhang highlight, and the accent on a face turned down. Overhangs face away
# from the light, which leaves them at 40% of their colour, so both start brighter than they read.
OVERHANG = np.array([1.75, 0.95, 0.90])
ACCENT_UNDER = ACCENT * 1.9
MATERIALS = np.array([BED, BED, PART, ACCENT, PART2, SUPPORT, OVERHANG, ACCENT_UNDER])
M_BED, M_PART, M_ACCENT, M_PART2, M_SUPPORT, M_OVERHANG, M_ACCENT_UNDER = 1, 2, 3, 4, 5, 6, 7

CURSOR = (235, 242, 242)
AXIS_X = (214, 92, 82)
AXIS_Y = (110, 190, 96)
AXIS_Z = (90, 140, 228)

LIGHT = np.array([-0.45, -0.65, 0.95])
LIGHT /= np.linalg.norm(LIGHT)


# --- signed distance functions -------------------------------------------------------------------
#
# Each takes points as an (N, 3) array and returns their distances, negative inside.


def sd_box(p, centre, half, r=0.0):
    q = np.abs(p - np.asarray(centre)) - (np.asarray(half) - r)
    return np.linalg.norm(np.maximum(q, 0.0), axis=1) + np.minimum(q.max(axis=1), 0.0) - r


def sd_cylinder(p, centre, radius, z0, z1, r=0.0):
    """Upright, centred on `centre` in x and y, from z0 to z1, edges rounded by r."""
    dxy = np.hypot(p[:, 0] - centre[0], p[:, 1] - centre[1]) - (radius - r)
    dz = np.abs(p[:, 2] - 0.5 * (z0 + z1)) - (0.5 * (z1 - z0) - r)
    return np.minimum(np.maximum(dxy, dz), 0.0) + np.hypot(np.maximum(dxy, 0.0), np.maximum(dz, 0.0)) - r


def sd_sphere(p, centre, radius):
    return np.linalg.norm(p - np.asarray(centre), axis=1) - radius


def bed(p):
    return sd_box(p, (0.0, 0.0, -0.05), (9.0, 9.0, 0.05)), M_BED


def union(*items):
    """Nearest of several (distance, material) pairs; a material is a constant or a per-point array."""
    d = np.stack([i[0] for i in items])
    k = d.argmin(axis=0)
    cols = np.arange(d.shape[1])
    mats = np.stack([np.broadcast_to(np.asarray(i[1]), d.shape[1:]) for i in items])
    return d[k, cols], mats[k, cols]


def extrude(d2, z0, z1):
    """A 2D distance field (per point) extruded between z0 and z1 - needs the points' z as well."""
    def f(p):
        dz = np.abs(p[:, 2] - 0.5 * (z0 + z1)) - 0.5 * (z1 - z0)
        return np.minimum(np.maximum(d2, dz), 0.0) + np.hypot(np.maximum(d2, 0.0), np.maximum(dz, 0.0))
    return f


def rot_z(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])


def rot_x(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[1.0, 0.0, 0.0], [0.0, c, -s], [0.0, s, c]])


def rot_y(a):
    c, s = math.cos(a), math.sin(a)
    return np.array([[c, 0.0, s], [0.0, 1.0, 0.0], [-s, 0.0, c]])


def rot_axis(axis, a):
    axis = np.asarray(axis, float)
    n = np.linalg.norm(axis)
    if n < 1e-9 or abs(a) < 1e-9:
        return np.eye(3)
    x, y, z = axis / n
    c, s, C = math.cos(a), math.sin(a), 1.0 - math.cos(a)
    return np.array([[c + x * x * C, x * y * C - z * s, x * z * C + y * s],
                     [y * x * C + z * s, c + y * y * C, y * z * C - x * s],
                     [z * x * C - y * s, z * y * C + x * s, c + z * z * C]])


def ease(t):
    """0 -> 1 -> 0 over t in 0..1, with no step where the loop joins."""
    return 0.5 - 0.5 * math.cos(2.0 * math.pi * t)


def smooth(t):
    t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)


def phase(k, start, length):
    """How far frame k is through a stretch of `length` frames starting at `start`, 0..1."""
    return min(max((k - start) / float(max(length - 1, 1)), 0.0), 1.0)


# --- 2D shapes, for text and SVG -----------------------------------------------------------------


def sd_segment2(x, y, a, b):
    ax, ay = a
    bx, by = b
    px, py = x - ax, y - ay
    ex, ey = bx - ax, by - ay
    h = np.clip((px * ex + py * ey) / (ex * ex + ey * ey), 0.0, 1.0)
    return np.hypot(px - ex * h, py - ey * h)


def sd_arc2(x, y, centre, radius, a0, a1):
    """Distance to an arc of a circle, from angle a0 to a1 in degrees, counter-clockwise."""
    dx, dy = x - centre[0], y - centre[1]
    ang = np.degrees(np.arctan2(dy, dx)) % 360.0
    span = (a1 - a0) % 360.0
    inside = ((ang - a0) % 360.0) <= span
    on = np.abs(np.hypot(dx, dy) - radius)
    ends = [(centre[0] + radius * math.cos(math.radians(a)), centre[1] + radius * math.sin(math.radians(a))) for a in (a0, a1)]
    off = np.minimum(np.hypot(x - ends[0][0], y - ends[0][1]), np.hypot(x - ends[1][0], y - ends[1][1]))
    return np.where(inside, on, off)


def letters2(x, y, scale=1.0):
    """'ABC' drawn with strokes, as a 2D distance field: negative inside the letters."""
    x, y = x / scale, y / scale
    d = []
    ox = -0.48  # A
    d += [sd_segment2(x, y, (ox - 0.15, -0.22), (ox, 0.22)), sd_segment2(x, y, (ox, 0.22), (ox + 0.15, -0.22)),
          sd_segment2(x, y, (ox - 0.08, -0.05), (ox + 0.08, -0.05))]
    ox = 0.0  # B
    d += [sd_segment2(x, y, (ox - 0.11, -0.22), (ox - 0.11, 0.22)),
          sd_segment2(x, y, (ox - 0.11, 0.22), (ox + 0.0, 0.22)), sd_arc2(x, y, (ox + 0.0, 0.11), 0.11, 270.0, 90.0),
          sd_segment2(x, y, (ox - 0.11, 0.0), (ox + 0.02, 0.0)), sd_arc2(x, y, (ox + 0.02, -0.11), 0.11, 270.0, 90.0),
          sd_segment2(x, y, (ox - 0.11, -0.22), (ox + 0.02, -0.22))]
    ox = 0.50  # C
    d += [sd_arc2(x, y, (ox + 0.03, 0.0), 0.2, 50.0, 310.0)]
    return (np.min(np.stack(d), axis=0) - 0.045) * scale


def sd_polygon2(x, y, verts):
    d = np.full(x.shape, np.inf)
    inside = np.zeros(x.shape, bool)
    n = len(verts)
    for i in range(n):
        a, b = verts[i], verts[(i + 1) % n]
        d = np.minimum(d, sd_segment2(x, y, a, b))
        crosses = ((a[1] > y) != (b[1] > y)) & (x < (b[0] - a[0]) * (y - a[1]) / (b[1] - a[1] + 1e-12) + a[0])
        inside ^= crosses
    return np.where(inside, -d, d)


def star2(x, y, outer=0.40, inner=0.17):
    verts = []
    for i in range(10):
        a = math.radians(90.0 + 36.0 * i)
        r = outer if i % 2 == 0 else inner
        verts.append((r * math.cos(a), r * math.sin(a)))
    return sd_polygon2(x, y, verts)


def value_noise(u, v, seed=3):
    """Smooth noise in 0..1 on a unit lattice, for the fuzzy skin."""
    def h(i, j):
        n = (i * 374761393 + j * 668265263 + seed * 2147483647) & 0xFFFFFFFF
        n = ((n ^ (n >> 13)) * 1274126177) & 0xFFFFFFFF
        return (n & 0xFFFF) / 65535.0
    i, j = np.floor(u).astype(np.int64), np.floor(v).astype(np.int64)
    fu, fv = u - i, v - j
    fu, fv = fu * fu * (3 - 2 * fu), fv * fv * (3 - 2 * fv)
    a, b, c, d = h(i, j), h(i + 1, j), h(i, j + 1), h(i + 1, j + 1)
    return a + (b - a) * fu + (c - a) * fv + (a - b - c + d) * fu * fv


# --- camera and rendering ------------------------------------------------------------------------


class Camera:
    """Orthographic, looking down at the bed from the front right - the default view of the plater."""

    def __init__(self, size, target=(0.0, 0.0, 0.5), extent=3.0, az=35.0, el=30.0):
        az, el = math.radians(az), math.radians(el)
        self.c = np.array([math.sin(az) * math.cos(el), -math.cos(az) * math.cos(el), math.sin(el)])
        self.right = np.cross([0.0, 0.0, 1.0], self.c)
        self.right /= np.linalg.norm(self.right)
        self.up = np.cross(self.c, self.right)
        self.target = np.asarray(target, float)
        self.extent = extent
        self.px = size * SS

    def rays(self):
        a = ((np.arange(self.px) + 0.5) / self.px - 0.5) * self.extent
        sx, sy = np.meshgrid(a, -a)
        o = self.target + sx.reshape(-1, 1) * self.right + sy.reshape(-1, 1) * self.up + self.c * 30.0
        return o, -self.c

    def project(self, p):
        q = np.asarray(p, float) - self.target
        return ((q @ self.right / self.extent + 0.5) * self.px, (0.5 - q @ self.up / self.extent) * self.px)


def render(scene, cam, step=0.9, steps=160):
    """Ray marches `scene` and shades it. Returns the image and the per-pixel buffers the overlays use."""
    o, d = cam.rays()
    n = o.shape[0]
    t = np.zeros(n)
    hit = np.zeros(n, bool)
    active = np.arange(n)
    for _ in range(steps):
        if active.size == 0:
            break
        p = o[active] + d * t[active, None]
        dist, _ = scene(p)
        t[active] += dist * step
        done_hit = dist < 1e-3
        done_miss = t[active] > 60.0
        hit[active[done_hit]] = True
        active = active[~(done_hit | done_miss)]

    idx = np.nonzero(hit)[0]
    p = o[idx] + d * t[idx, None]
    _, mat = scene(p)
    e = 1e-3
    normal = np.zeros_like(p)
    for axis in range(3):
        off = np.zeros(3)
        off[axis] = e
        normal[:, axis] = scene(p + off)[0] - scene(p - off)[0]
    normal /= np.linalg.norm(normal, axis=1, keepdims=True) + 1e-12

    # Soft shadows: a second march from the surface towards the light.
    shadow = np.ones(len(idx))
    ts = np.full(len(idx), 0.02)
    sp = p + normal * 0.004
    act = np.arange(len(idx))
    for _ in range(64):
        if act.size == 0:
            break
        dist, _ = scene(sp[act] + LIGHT * ts[act, None])
        shadow[act] = np.minimum(shadow[act], np.clip(10.0 * dist / ts[act], 0.0, 1.0))
        ts[act] += np.clip(dist, 0.01, 0.2)
        act = act[(dist > 1e-4) & (ts[act] < 6.0)]
    shadow = np.clip(shadow, 0.0, 1.0)

    lam = np.clip(normal @ LIGHT, 0.0, 1.0)
    base = MATERIALS[mat]
    is_bed = mat == M_BED
    # The bed's grid, every half unit.
    gx = np.abs((p[:, 0] / 0.5 + 0.5) % 1.0 - 0.5) * 0.5
    gy = np.abs((p[:, 1] / 0.5 + 0.5) % 1.0 - 0.5) * 0.5
    line = np.clip(1.0 - (np.minimum(gx, gy) - 0.004) / 0.006, 0.0, 1.0) * is_bed
    base = base + (GRID - BED) * line[:, None]
    light = np.where(is_bed, 0.75 + 0.25 * shadow, 0.40 + 0.60 * lam * (0.35 + 0.65 * shadow))
    rgb = np.zeros((n, 3))
    rgb[:] = BED * 0.75
    rgb[idx] = base * light[:, None]

    depth = np.full(n, np.inf)
    depth[idx] = t[idx]
    nbuf = np.zeros((n, 3))
    nbuf[idx] = normal
    side = cam.px
    rgb = rgb.reshape(side, side, 3)
    depth = depth.reshape(side, side)
    nbuf = nbuf.reshape(side, side, 3)

    # Outlines, the way a CAD view draws them: dark where the depth jumps (a silhouette), lighter where
    # the surface turns a corner (a crease). They are what make a grey model read as a shape.
    sil = np.zeros((side, side), bool)
    crease = np.zeros((side, side), bool)
    for dy, dx in ((0, 1), (1, 0)):
        a_d, b_d = depth[: side - dy, : side - dx], depth[dy:, dx:]
        both = np.isfinite(a_d) & np.isfinite(b_d)
        with np.errstate(invalid="ignore"):  # inf - inf where both rays missed; masked out by `both`
            jump = np.where(both, np.abs(np.where(both, a_d - b_d, 0.0)) > 0.06, np.isfinite(a_d) != np.isfinite(b_d))
        turn = (nbuf[: side - dy, : side - dx] * nbuf[dy:, dx:]).sum(axis=2) < 0.8
        sil[: side - dy, : side - dx] |= jump
        sil[dy:, dx:] |= jump
        crease[: side - dy, : side - dx] |= turn
        crease[dy:, dx:] |= turn
    rgb = np.where(sil[..., None], rgb * 0.35, np.where(crease[..., None], rgb * 0.72, rgb))
    return np.clip(rgb, 0.0, 1.0)


# --- overlays: handles, pointer, brush ----------------------------------------------------------


class Overlay:
    def __init__(self, rgb, cam):
        self.cam = cam
        self.image = Image.fromarray((rgb * 255.0 + 0.5).astype(np.uint8)).convert("RGBA")
        self.layer = Image.new("RGBA", self.image.size, (0, 0, 0, 0))
        self.draw = ImageDraw.Draw(self.layer)

    def pt(self, p):
        x, y = self.cam.project(p)
        return (float(x), float(y))

    def line(self, a, b, colour, width=1.5):
        self.draw.line([self.pt(a), self.pt(b)], fill=colour, width=max(1, int(round(width * SS))))

    def polyline(self, pts, colour, width=1.5, closed=False):
        xy = [self.pt(p) for p in pts]
        if closed:
            xy.append(xy[0])
        self.draw.line(xy, fill=colour, width=max(1, int(round(width * SS))), joint="curve")

    def arrow(self, a, b, colour, width=2.2, head=9.0):
        pa, pb = np.array(self.pt(a)), np.array(self.pt(b))
        v = pb - pa
        length = np.linalg.norm(v)
        if length < 1e-6:
            return
        v /= length
        w = np.array([-v[1], v[0]])
        h = head * SS
        base = pb - v * h
        self.draw.line([tuple(pa), tuple(base)], fill=colour, width=max(1, int(round(width * SS))))
        self.draw.polygon([tuple(pb), tuple(base + w * h * 0.42), tuple(base - w * h * 0.42)], fill=colour)

    def dot(self, p, radius, colour, outline=None):
        x, y = self.pt(p)
        r = radius * SS
        self.draw.ellipse([x - r, y - r, x + r, y + r], fill=colour, outline=outline, width=max(1, SS))

    def square(self, p, half, colour, outline=(30, 32, 34)):
        x, y = self.pt(p)
        h = half * SS
        self.draw.rectangle([x - h, y - h, x + h, y + h], fill=colour, outline=outline, width=max(1, SS))

    def circle_on(self, centre, normal, radius, colour, width=1.6, fill=None):
        """A circle lying on a surface, as the brush cursor is drawn on the model."""
        normal = np.asarray(normal, float)
        normal /= np.linalg.norm(normal)
        u = np.cross(normal, [0.0, 0.0, 1.0] if abs(normal[2]) < 0.9 else [1.0, 0.0, 0.0])
        u /= np.linalg.norm(u)
        v = np.cross(normal, u)
        pts = [np.asarray(centre) + radius * (math.cos(a) * u + math.sin(a) * v) for a in np.linspace(0, 2 * math.pi, 48)]
        xy = [self.pt(p) for p in pts]
        if fill is not None:
            self.draw.polygon(xy, fill=fill)
        self.draw.line(xy, fill=colour, width=max(1, int(round(width * SS))), joint="curve")

    def pointer(self, p=None, xy=None):
        """The mouse pointer, its tip at a point of the scene or at a pixel."""
        x, y = self.pt(p) if xy is None else xy
        k = 10.5 * SS
        self.draw.polygon([(x, y), (x, y + 1.45 * k), (x + 0.38 * k, y + 1.05 * k), (x + 0.62 * k, y + 1.55 * k),
                           (x + 0.90 * k, y + 1.40 * k), (x + 0.66 * k, y + 0.92 * k), (x + 1.05 * k, y + 0.84 * k)],
                          fill=CURSOR, outline=(30, 32, 34), width=max(1, SS))

    def finish(self, size):
        out = Image.alpha_composite(self.image, self.layer).convert("RGB")
        return np.asarray(out.resize((size, size), Image.LANCZOS))


def surface_normal(scene, p):
    p = np.asarray(p, float)[None, :]
    n = np.array([scene(p + o)[0][0] - scene(p - o)[0][0] for o in np.eye(3) * 1e-3])
    return n / np.linalg.norm(n)



# --- output ----------------------------------------------------------------------------------------


def vignette(rgb):
    """Rounds the picture off at its border, so it does not end in a hard rectangle."""
    a = (np.arange(rgb.shape[0]) + 0.5) / rgb.shape[0] - 0.5
    x, y = np.meshgrid(a, a)
    edge = np.clip((0.5 - np.maximum(np.abs(x), np.abs(y))) / 0.045, 0.0, 1.0)
    return (rgb * edge[..., None]).astype(np.uint8)
