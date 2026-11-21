"""The 3D view's own controls: the view menu, the variable layer height panel and the assembly view's
explosion slider (see GLCanvas3D::view_tooltip()).

The view menu's pictures show one model, a faceted post that widens upwards, each with one option of
the menu on, so that what differs between two of them is the option. The camera is low enough to see
the sloped underside, which is what the overhang highlight colours.
"""

import math

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import BED, GRID, LIGHT, MATERIALS, Camera, Overlay, render

OUT = "viewport"  # resources/images/<OUT>


# --- the model of the view menu's pictures ------------------------------------------------------
#
# An octagonal post: a stem, a slope out at 45 degrees (an overhang), a wider head with a flat top.
# Faceted rather than round, so the wireframe has triangles to draw that are the model's own.

_SIDES = 8
_RINGS = [(0.0, 0.30), (0.36, 0.30), (0.66, 0.60), (1.02, 0.60)]  # (z, apothem)
_TOP = _RINGS[-1][0]
_FACE_ANGLES = [2.0 * math.pi * i / _SIDES for i in range(_SIDES)]
_FACE_N = np.array([[math.cos(a), math.sin(a)] for a in _FACE_ANGLES])


def _apothem(z):
    return np.interp(z, [r[0] for r in _RINGS], [r[1] for r in _RINGS])


def _post(p):
    # The octagon's apothem along each face normal; the slope makes the field steeper than 1, hence k.
    side = (p[:, :2] @ _FACE_N.T).max(axis=1) - _apothem(p[:, 2])
    return np.maximum.reduce([side * 0.68, -p[:, 2], p[:, 2] - _TOP])


def _overhang_band(p):
    return (p[:, 2] > _RINGS[1][0] + 0.006) & (p[:, 2] < _RINGS[2][0] - 0.006)


def _vertex(ring, i):
    z, a = _RINGS[ring]
    r = a / math.cos(math.pi / _SIDES)
    t = 2.0 * math.pi * (i + 0.5) / _SIDES
    return np.array([r * math.cos(t), r * math.sin(t), z])


def _mesh_edges():
    """The edges of the post's triangle mesh: rings, the edges up the sides, a diagonal across each
    side quad and a fan across the top - the triangles a slicer would have loaded."""
    edges = []
    for ring in range(len(_RINGS)):
        for i in range(_SIDES):
            edges.append((_vertex(ring, i), _vertex(ring, (i + 1) % _SIDES)))
    for ring in range(len(_RINGS) - 1):
        for i in range(_SIDES):
            edges.append((_vertex(ring, i), _vertex(ring + 1, i)))
            edges.append((_vertex(ring, i), _vertex(ring + 1, (i + 1) % _SIDES)))
    top = len(_RINGS) - 1
    for i in range(2, _SIDES - 1):
        edges.append((_vertex(top, 0), _vertex(top, i)))
    return edges


def _view_camera(size=SIZE):
    return Camera(size, target=(0.0, 0.0, 0.5), extent=2.2, el=16.0)


def _visible(scene, cam, pts):
    """Which of the points on a surface the camera sees: a march from each towards the camera that
    meets nothing on the way."""
    toward = cam.c
    t = np.full(len(pts), 0.025)
    seen = np.ones(len(pts), bool)
    active = np.arange(len(pts))
    for _ in range(80):
        if active.size == 0:
            break
        d, _ = scene(pts[active] + toward * t[active, None])
        hit = d < 1e-3
        seen[active[hit]] = False
        t[active] += np.maximum(d, 0.004)
        active = active[~hit & (t[active] < 6.0)]
    return seen


def _mask(rgb):
    """The pixels a body covers, from a render of it alone: everything that is not the empty
    background, which the corner pixel is."""
    return np.abs(rgb - rgb[0, 0]).max(axis=2) > 1e-3


def view_overhangs(k, n):
    def scene(p):
        return union(bed(p), (_post(p), np.where(_overhang_band(p), M_OVERHANG, M_PART)))

    cam = _view_camera()
    return Overlay(render(scene, cam), cam).finish(SIZE)


def view_outline(k, n):
    cam = _view_camera()
    rgb = render(lambda p: union(bed(p), (_post(p), M_PART)), cam)
    alone = _mask(render(lambda p: (_post(p), np.full(len(p), M_PART)), cam))
    grown = np.asarray(Image.fromarray((alone * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(2 * 3 * SS + 1))) > 0
    ring = grown & ~alone
    accent = np.array(MATERIALS[M_ACCENT]) * 1.25
    rgb = np.where(ring[..., None], np.clip(accent, 0.0, 1.0), rgb)
    return Overlay(rgb, cam).finish(SIZE)


def view_wireframe(k, n):
    scene = lambda p: union(bed(p), (_post(p), M_PART))
    cam = _view_camera()
    o = Overlay(render(scene, cam), cam)
    colour = (60, 205, 185)
    for a, b in _mesh_edges():
        s = np.linspace(0.0, 1.0, 48)[:, None]
        pts = a + (b - a) * s
        # Off the surface a hair, towards the outside of the post, so an edge does not hide itself.
        outward = np.zeros_like(pts)
        outward[:, :2] = pts[:, :2]
        outward /= np.linalg.norm(outward, axis=1, keepdims=True) + 1e-9
        lifted = pts + 0.004 * outward + np.where(np.isclose(pts[:, 2:3], _TOP), [0.0, 0.0, 0.004], 0.0)
        seen = _visible(scene, cam, lifted)
        for i in range(len(pts) - 1):
            if seen[i] and seen[i + 1]:
                o.line(pts[i], pts[i + 1], colour, width=1.1)
    return o.finish(SIZE)


def view_xray(k, n):
    # A part inside the post, which only X-Ray shows: through the outer surface, at half strength.
    def inner(p):
        return sd_cylinder(p, (0.0, 0.0), 0.17, 0.12, 0.88, 0.03)

    cam = _view_camera()
    outer = render(lambda p: union(bed(p), (_post(p), M_PART)), cam)
    inside = render(lambda p: union(bed(p), (inner(p), M_ACCENT)), cam)
    covered = _mask(render(lambda p: (_post(p), np.full(len(p), M_PART)), cam))
    glass = 0.42 * outer + 0.58 * inside
    rgb = np.where(covered[..., None], glass, outer)
    return Overlay(rgb, cam).finish(SIZE)


# --- perspective --------------------------------------------------------------------------------
#
# render() takes one direction for every ray, which is what an orthographic camera has. A perspective
# camera has one per pixel, so it is drawn here with the same march and shading, ray by ray.


class _PerspectiveCamera(Camera):
    def __init__(self, size, distance, **kw):
        super().__init__(size, **kw)
        self.distance = distance
        self.eye = self.target + self.c * distance

    def rays(self):
        a = ((np.arange(self.px) + 0.5) / self.px - 0.5) * self.extent
        sx, sy = np.meshgrid(a, -a)
        d = self.target + sx.reshape(-1, 1) * self.right + sy.reshape(-1, 1) * self.up - self.eye
        d /= np.linalg.norm(d, axis=1, keepdims=True)
        return np.broadcast_to(self.eye, d.shape).copy(), d

    def project(self, p):
        q = np.asarray(p, float) - self.eye
        depth = -(q @ self.c)
        f = self.distance / depth
        return ((q @ self.right * f / self.extent + 0.5) * self.px, (0.5 - q @ self.up * f / self.extent) * self.px)


def _render_perspective(scene, cam, step=0.9, steps=200):
    o, d = cam.rays()
    n = o.shape[0]
    t = np.zeros(n)
    hit = np.zeros(n, bool)
    active = np.arange(n)
    for _ in range(steps):
        if active.size == 0:
            break
        dist, _ = scene(o[active] + d[active] * t[active, None])
        t[active] += dist * step
        done_hit = dist < 1e-3
        hit[active[done_hit]] = True
        active = active[~(done_hit | (t[active] > 60.0))]
    # From here on, render()'s shading and outlines.
    idx = np.nonzero(hit)[0]
    p = o[idx] + d[idx] * t[idx, None]
    _, mat = scene(p)
    e = 1e-3
    normal = np.zeros_like(p)
    for axis in range(3):
        off = np.zeros(3)
        off[axis] = e
        normal[:, axis] = scene(p + off)[0] - scene(p - off)[0]
    normal /= np.linalg.norm(normal, axis=1, keepdims=True) + 1e-12
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
    lam = np.clip(normal @ LIGHT, 0.0, 1.0)
    base = MATERIALS[mat]
    is_bed = mat == M_BED
    gx = np.abs((p[:, 0] / 0.5 + 0.5) % 1.0 - 0.5) * 0.5
    gy = np.abs((p[:, 1] / 0.5 + 0.5) % 1.0 - 0.5) * 0.5
    # The grid lines keep their width on screen as they recede, as the 3D view draws them.
    width = 0.004 * (t[idx] / cam.distance)
    line = np.clip(1.0 - (np.minimum(gx, gy) - width) / (1.5 * width), 0.0, 1.0) * is_bed
    base = base + (GRID - BED) * line[:, None]
    light = np.where(is_bed, 0.75 + 0.25 * shadow, 0.40 + 0.60 * lam * (0.35 + 0.65 * shadow))
    rgb = np.zeros((n, 3))
    rgb[:] = BED * 0.75
    rgb[idx] = base * light[:, None]
    side = cam.px
    rgb = rgb.reshape(side, side, 3)
    depth = np.full(n, np.inf)
    depth[idx] = t[idx]
    depth = depth.reshape(side, side)
    nbuf = np.zeros((n, 3))
    nbuf[idx] = normal
    nbuf = nbuf.reshape(side, side, 3)
    sil = np.zeros((side, side), bool)
    crease = np.zeros((side, side), bool)
    for dy, dx in ((0, 1), (1, 0)):
        a_d, b_d = depth[: side - dy, : side - dx], depth[dy:, dx:]
        both = np.isfinite(a_d) & np.isfinite(b_d)
        with np.errstate(invalid="ignore"):
            jump = np.where(both, np.abs(np.where(both, a_d - b_d, 0.0)) > 0.06, np.isfinite(a_d) != np.isfinite(b_d))
        turn = (nbuf[: side - dy, : side - dx] * nbuf[dy:, dx:]).sum(axis=2) < 0.8
        sil[: side - dy, : side - dx] |= jump
        sil[dy:, dx:] |= jump
        crease[: side - dy, : side - dx] |= turn
        crease[dy:, dx:] |= turn
    rgb = np.where(sil[..., None] & ~(np.isinf(depth)[..., None]), rgb * 0.35, np.where(crease[..., None], rgb * 0.72, rgb))
    return np.clip(rgb, 0.0, 1.0)


# Three posts in a row running away from the camera: the far ones shrink in perspective and the grid
# lines meet, where in the orthographic view all three are the same size and the lines stay parallel.
_ROW = [np.array([0.0, y, 0.0]) for y in (-1.3, 0.0, 1.3)]


def _row_scene(p):
    items = [bed(p)]
    for c in _ROW:
        items.append((_post((p - c) * 2.0) / 2.0, M_PART))
    return union(*items)


def _row_camera(perspective):
    kw = dict(target=(0.0, 0.0, 0.25), az=24.0, el=24.0)
    if perspective:
        return _PerspectiveCamera(SIZE, 4.6, extent=2.7, **kw)
    return Camera(SIZE, extent=3.3, **kw)


def view_perspective(k, n):
    cam = _row_camera(True)
    return Overlay(_render_perspective(_row_scene, cam), cam).finish(SIZE)


def view_orthographic(k, n):
    cam = _row_camera(False)
    return Overlay(render(_row_scene, cam), cam).finish(SIZE)


# --- variable layer height ----------------------------------------------------------------------
#
# Drawn flat, from the side, the way the panel's own height profile is: an object cut into its layers on
# the left, the layer height up its height on the right. A bottle - a wide base with a flat shoulder,
# a straight neck, a domed top - since adaptive layers are about where the surface is flat and where it
# is steep, and that shape has both.

_BASE_W, _BASE_H, _NECK_W, _NECK_TOP = 0.40, 0.28, 0.24, 0.58
_HEIGHT = _NECK_TOP + _NECK_W
_H_MIN, _H_MAX, _CUSP = 0.016, 0.07, 0.011


def _half_width(z):
    if z <= _BASE_H:
        return _BASE_W
    if z <= _NECK_TOP:
        return _NECK_W
    return math.sqrt(max(_NECK_W * _NECK_W - (z - _NECK_TOP) ** 2, 0.0))


def _flatness(z0, z1):
    """How flat the steepest-to-print surface between two heights is: the normal's z of the flattest
    facet there, 1 for the flat shoulder, 0 for a vertical wall."""
    n = 0.0
    if z0 <= _BASE_H <= z1:
        n = 1.0
    zz = np.linspace(max(z0, _NECK_TOP), z1, 8)
    if z1 > _NECK_TOP:
        n = max(n, float(np.max(np.clip((zz - _NECK_TOP) / _NECK_W, 0.0, 1.0))))
    return n


def _adaptive_layers():
    zs, z = [0.0], 0.0
    while z < _HEIGHT - 1e-6:
        h = _H_MAX
        # The thickest layer whose cusp - the step its edge leaves on a sloped surface - stays small.
        for _ in range(4):
            nz = _flatness(z, z + h)
            h = min(_H_MAX, max(_H_MIN, _CUSP / nz)) if nz > 1e-6 else _H_MAX
        z = min(z + h, _HEIGHT)
        zs.append(z)
    return zs


def _smoothed_layers(zs, radius=5, passes=4):
    """The same profile through a Gaussian blur, as Smooth runs it, and laid out as layers again."""
    mids = np.array([(a + b) * 0.5 for a, b in zip(zs, zs[1:])])
    hs = np.diff(zs)
    sigma = 0.3 * (radius - 1) + 0.8
    kernel = np.exp(-0.5 * (np.arange(-radius, radius + 1) / sigma) ** 2)
    for _ in range(passes):
        padded = np.pad(hs, radius, mode="edge")
        hs = np.convolve(padded, kernel / kernel.sum(), mode="valid")
    out, z = [0.0], 0.0
    while z < _HEIGHT - 1e-6:
        h = float(np.interp(z, mids, hs))
        z = min(z + h, _HEIGHT)
        out.append(z)
    return out


def _vlh_picture(zs):
    px = SIZE * SS
    img = Image.new("RGB", (px, px), tuple(int(c * 255 * 0.75) for c in BED))
    draw = ImageDraw.Draw(img)
    scale = min(px * 0.80 / _HEIGHT, px * 0.56 / (2.0 * _BASE_W))
    bottom = px * 0.90
    cx = px * 0.36
    to_y = lambda z: bottom - z * scale

    # The bed, as a line under the object.
    draw.line([(cx - px * 0.32, bottom + SS), (cx + px * 0.32, bottom + SS)], fill=tuple(int(c * 255) for c in GRID * 1.4), width=2 * SS)
    part = MATERIALS[M_PART]
    light, dark = tuple(int(c * 255) for c in part * 0.95), tuple(int(c * 255) for c in part * 0.72)
    for i, (z0, z1) in enumerate(zip(zs, zs[1:])):
        pts = []
        steps = 32
        for j in range(steps + 1):  # up the right side, then down the left: the outline of this layer
            z = z0 + (z1 - z0) * j / steps
            pts.append((cx + _half_width(min(z, _HEIGHT - 1e-6)) * scale, to_y(z)))
        for j in range(steps, -1, -1):
            z = z0 + (z1 - z0) * j / steps
            pts.append((cx - _half_width(min(z, _HEIGHT - 1e-6)) * scale, to_y(z)))
        draw.polygon(pts, fill=light if i % 2 == 0 else dark)

    # The profile: layer height up the object's height, in its own panel as the 3D view draws it.
    x0, x1 = px * 0.74, px * 0.94
    panel = tuple(int(c * 255) for c in BED * 1.25)
    draw.rectangle([x0, to_y(_HEIGHT) - 2 * SS, x1, bottom], fill=panel)
    for f in (0.0, 0.5, 1.0):
        gx = x0 + (x1 - x0) * (0.12 + 0.76 * f)
        draw.line([(gx, to_y(_HEIGHT) - 2 * SS), (gx, bottom)], fill=tuple(int(c * 255) for c in GRID * 1.5), width=SS)
    curve = []
    for z0, z1 in zip(zs, zs[1:]):
        h = (z1 - z0 - _H_MIN) / (_H_MAX - _H_MIN)
        gx = x0 + (x1 - x0) * (0.12 + 0.76 * h)
        curve += [(gx, to_y(z0)), (gx, to_y(z1))]
    draw.line(curve, fill=tuple(int(c * 255) for c in np.clip(MATERIALS[M_ACCENT] * 1.45, 0, 1)), width=int(2.2 * SS), joint="curve")
    return np.asarray(img.resize((SIZE, SIZE), Image.LANCZOS))


def vlh_adaptive(k, n):
    return _vlh_picture(_adaptive_layers())


def vlh_smooth(k, n):
    return _vlh_picture(_smoothed_layers(_adaptive_layers()))


# --- assembly view ------------------------------------------------------------------------------
#
# A box, its lid and two pins between them. The explosion ratio pushes every part away from the middle
# of the assembly by its distance from it, so the parts spread out along the lines they were joined on.

_PIN_HALF = np.array([0.09, 0.09, 0.10])
_PINS = [np.array([x, 0.0, 0.36]) for x in (-0.3, 0.3)]
_PARTS = [  # (centre, half size, material); the pins sit in holes, half in the box and half in the lid
    (np.array([0.0, 0.0, 0.18]), np.array([0.62, 0.42, 0.18]), M_PART),
    (_PINS[0], _PIN_HALF, M_ACCENT),
    (_PINS[1], _PIN_HALF, M_ACCENT),
    (np.array([0.0, 0.0, 0.44]), np.array([0.62, 0.42, 0.08]), M_PART2),
]
_CENTRE = np.array([0.0, 0.0, 0.30])


def assembly_explode(k, n):
    # Together for a moment, apart, held apart, and back together.
    if k < 12:
        s = smooth(phase(k, 3, 7))
    else:
        s = 1.0 - smooth(phase(k, 15, 6))
    ratio = 1.0 + 2.0 * s  # the slider's whole range, 1 to 3
    placed = []
    for c, half, mat in _PARTS:
        placed.append((c, _CENTRE + (c - _CENTRE) * ratio, half, mat))
    lift = max(0.0, -min(c[2] - h[2] for _, c, h, _ in placed))
    placed = [(c0, c + np.array([0.0, 0.0, lift]), h, m) for c0, c, h, m in placed]

    def body(p, c0, c, half, mat):
        d = sd_box(p, c, half, 0.02)
        if mat != M_ACCENT:  # the holes the pins sit in, moving with the part they are in
            for pin in _PINS:
                d = np.maximum(d, -sd_box(p, c + pin - c0, _PIN_HALF + 0.01))
        return d, mat

    def scene(p):
        return union(bed(p), *[body(p, *part) for part in placed])

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=3.0, el=30.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "view_overhangs": 0,
    "view_outline": 0,
    "view_wireframe": 0,
    "view_xray": 0,
    "view_perspective": 0,
    "view_orthographic": 0,
    "vlh_adaptive": 0,
    "vlh_smooth": 0,
    "assembly_explode": 22,
}
