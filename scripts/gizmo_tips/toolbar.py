"""The gizmo toolbar's tooltips: one picture per tool (see GLGizmosManager::render_toolbar_tooltip())."""

import math

import numpy as np

from .render import *  # noqa: F401,F403 - the scenes are written against the renderer's whole vocabulary
from .render import Camera, Overlay, render

OUT = "gizmo_toolbar"  # resources/images/<OUT>

# --- the scenes ----------------------------------------------------------------------------------
#
# Each takes a frame number and returns that frame as an RGB array. FRAMES says how many there are; a
# scene listed with 0 frames is a still.


def move(k, n):
    x = -0.55 + 1.1 * ease(k / n)
    c = np.array([x, 0.0, 0.42])
    scene = lambda p: union(bed(p), (sd_box(p, c, (0.42, 0.42, 0.42), 0.03), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=3.0)
    o = Overlay(render(scene, cam), cam)
    o.arrow(c, c + (0.0, 0.0, 0.95), AXIS_Z, width=2.6)
    o.arrow(c, c + (0.0, 0.95, 0.0), AXIS_Y, width=2.6)
    o.arrow(c, c + (1.0, 0.0, 0.0), (250, 150, 120), width=3.2, head=11.0)  # the one being dragged
    o.pointer(c + (0.88, 0.0, 0.0))
    return o.finish(GIF_SIZE)


def rotate(k, n):
    a = math.radians(80.0) * ease(k / n)
    c = np.array([0.0, 0.0, 0.3])
    R = rot_z(a)
    scene = lambda p: union(bed(p), (sd_box((p - c) @ R, (0, 0, 0), (0.72, 0.38, 0.3), 0.03), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.35), extent=2.9)
    o = Overlay(render(scene, cam), cam)
    ring = lambda a0, a1: [c + 1.05 * np.array([math.cos(t), math.sin(t), 0.0]) for t in np.linspace(a0, a1, 64)]
    o.polyline(ring(0.0, 2.0 * math.pi), AXIS_Z, width=1.6)
    a0 = math.radians(-60.0)
    if a > 1e-3:
        o.polyline(ring(a0, a0 + a), (150, 190, 255), width=3.4)
    o.pointer(ring(a0 + a, a0 + a)[0])
    return o.finish(GIF_SIZE)


def scale(k, n):
    h = 0.30 + 0.20 * ease(k / n)
    c = np.array([0.0, 0.0, h])
    scene = lambda p: union(bed(p), (sd_box(p, c, (h, h, h), 0.03), M_PART))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=2.6)
    o = Overlay(render(scene, cam), cam)
    corners = [c + h * np.array([sx, sy, sz]) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    for i, a in enumerate(corners):
        for j, b in enumerate(corners):
            if i < j and np.sum(np.abs(a - b) > 1e-6) == 1:
                o.line(a, b, (220, 226, 226, 150), width=1.0)
    for p in corners:
        o.square(p, 3.2, (210, 214, 214))
    grab = c + h * np.array([1.0, -1.0, 1.0])
    o.square(grab, 4.2, (250, 150, 120))
    o.pointer(grab)
    return o.finish(GIF_SIZE)


def lay_on_face(k, n):
    half = np.array([0.55, 0.36, 0.26])
    R0 = rot_z(math.radians(25.0)) @ rot_y(math.radians(-35.0)) @ rot_x(math.radians(30.0))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.45), extent=2.7)
    # The face that goes down: the most downward-facing one the camera can still see.
    faces = [s * e for e in np.eye(3) for s in (1.0, -1.0)]
    visible = [f for f in faces if (R0 @ f) @ cam.c > 0.15]
    face = min(visible, key=lambda f: (R0 @ f)[2])
    m = R0 @ face
    axis = np.cross(m, [0.0, 0.0, -1.0])
    angle = math.acos(np.clip(m @ np.array([0.0, 0.0, -1.0]), -1.0, 1.0))
    s = smooth(phase(k, 5, 9))
    R = rot_axis(axis, angle * s) @ R0
    verts = np.array([[sx, sy, sz] for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]) * half
    c = np.array([0.0, 0.0, -(verts @ R.T)[:, 2].min()])
    extent_along = np.abs(face) @ half

    def scene(p):
        q = (p - c) @ R
        mat = np.where((q @ face > extent_along - 0.012) & (k >= 2), M_ACCENT, M_PART)
        return union(bed(p), (sd_box(q, (0, 0, 0), half, 0.02), mat))

    o = Overlay(render(scene, cam), cam)
    if k < 5:
        o.pointer(c + R @ (face * extent_along))
    return o.finish(GIF_SIZE)


def cut(k, n):
    r, z1, zc = 0.55, 1.3, 0.62
    split = smooth(phase(k, 6, 7))
    off = np.array([0.0, 0.0, 0.5]) * split

    def scene(p):
        if split <= 0.0:
            return union(bed(p), (sd_cylinder(p, (0, 0), r, 0.0, z1, 0.04), M_PART))
        bottom = np.maximum(sd_cylinder(p, (0, 0), r, 0.0, z1, 0.04), p[:, 2] - zc)
        q = p - off
        top = np.maximum(sd_cylinder(q, (0, 0), r, 0.0, z1, 0.04), zc - q[:, 2])
        return union(bed(p), (bottom, np.where(p[:, 2] > zc - 0.01, M_ACCENT, M_PART)),
                     (top, np.where(q[:, 2] < zc + 0.01, M_ACCENT, M_PART)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.8), extent=2.8)
    o = Overlay(render(scene, cam), cam)
    alpha = smooth(phase(k, 0, 4)) * (1.0 - split)
    if alpha > 0.0:
        sq = [np.array([x, y, zc]) for x, y in ((-0.95, -0.95), (0.95, -0.95), (0.95, 0.95), (-0.95, 0.95))]
        o.draw.polygon([o.pt(p) for p in sq], fill=(26, 153, 138, int(90 * alpha)))
        o.polyline(sq, (60, 200, 180, int(230 * alpha)), width=1.4, closed=True)
    return o.finish(GIF_SIZE)


def _boolean(kind):
    cube = lambda p: sd_box(p, (0, 0, 0.5), (0.5, 0.5, 0.5), 0.02)
    ball = lambda p: sd_sphere(p, (0.45, -0.45, 0.95), 0.55)

    def scene(p):
        a, b = cube(p), ball(p)
        if kind == "union":
            body = union((a, M_PART), (b, M_ACCENT))
        elif kind == "difference":
            body = (np.maximum(a, -b), np.where(-b > a, M_ACCENT, M_PART))
        else:
            body = (np.maximum(a, b), np.where(b > a, M_ACCENT, M_PART))
        return union(bed(p), body)

    cam = Camera(SIZE, target=(0.1, -0.1, 0.55), extent=2.5)
    return Overlay(render(scene, cam), cam).finish(SIZE)


def boolean_union(k, n):
    return _boolean("union")


def boolean_difference(k, n):
    return _boolean("difference")


def boolean_intersection(k, n):
    return _boolean("intersection")


def support_painting(k, n):
    # A block whose front leans out over the bed at 45 degrees: an overhang the camera can see.
    stroke_r, zc, yc = 0.2, 0.36, -0.51
    x_end = -0.5 + 1.0 * phase(k, 0, 10)
    grow = smooth(phase(k, 11, 6))

    def overhang(p):
        return np.maximum(sd_box(p, (0, -0.2, 0.6), (0.7, 0.7, 0.6), 0.02), -(p[:, 1] + 0.15 + p[:, 2]) / math.sqrt(2.0))

    def painted(p):
        a, b = np.array([-0.5, yc, zc]), np.array([x_end, yc, zc])
        ab = b - a
        h = np.clip(((p - a) @ ab) / max(ab @ ab, 1e-9), 0.0, 1.0)
        return np.linalg.norm(p - (a + h[:, None] * ab), axis=1) < stroke_r

    def scene(p):
        items = [bed(p), (overhang(p), np.where(painted(p), M_ACCENT, M_PART))]
        if grow > 0.0:
            s, w = 0.13, 0.03
            lx = np.abs((p[:, 0] + 0.5 * s) % s - 0.5 * s) - w
            ly = np.abs((p[:, 1] + 0.5 * s) % s - 0.5 * s) - w
            pillars = np.minimum(np.maximum(lx, ly), 0.0) + np.hypot(np.maximum(lx, 0.0), np.maximum(ly, 0.0))
            region = sd_box(p, (0.0, yc, 0.5), (0.55, 0.15, 0.5))
            under = p[:, 2] - (-p[:, 1] - 0.15) + 0.01
            height = p[:, 2] - grow * 0.9
            items.append((np.maximum.reduce([pillars, region, under, height]), M_SUPPORT))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.0, -0.25, 0.5), extent=2.6, el=16.0)
    o = Overlay(render(scene, cam), cam)
    if k < 11:
        centre = np.array([x_end, yc, zc])
        o.circle_on(centre, surface_normal(lambda q: (overhang(q), 0), centre), stroke_r, CURSOR)
        o.pointer(centre)
    return o.finish(GIF_SIZE)


def _layered_cylinder(p, r=0.55, z1=1.3, layer=0.1):
    return sd_cylinder(p, (0, 0), r, 0.0, z1, 0.03) + 0.008 * (0.5 + 0.5 * np.cos(2.0 * math.pi * p[:, 2] / layer))


def seam_painting(k, n):
    r, theta = 0.55, math.radians(-30.0)
    z_brush = 0.12 + 1.06 * phase(k, 0, 10)
    seams = int(round(13 * phase(k, 11, 6)))
    u = np.array([math.cos(theta), math.sin(theta), 0.0])

    def scene(p):
        ang = np.arctan2(p[:, 1], p[:, 0])
        along = np.abs(((ang - theta + math.pi) % (2.0 * math.pi)) - math.pi) * r
        painted = (along < 0.13) & (p[:, 2] < z_brush + 0.1)
        return union(bed(p), (_layered_cylinder(p), np.where(painted, M_ACCENT, M_PART)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.65), extent=2.4)
    o = Overlay(render(scene, cam), cam)
    for i in range(seams):
        o.dot(u * (r + 0.01) + np.array([0.0, 0.0, 0.05 + 0.1 * i]), 2.6, (245, 248, 248), outline=(30, 32, 34))
    if k < 11:
        centre = u * r + np.array([0.0, 0.0, z_brush])
        o.circle_on(centre, u, 0.16, CURSOR)
        o.pointer(centre)
    return o.finish(GIF_SIZE)


def fuzzy_skin_painting(k, n):
    r, zc = 0.55, 0.65
    a_end = math.radians(-150.0 + 140.0 * phase(k, 0, 10))
    fuzz = smooth(phase(k, 11, 5))
    path = [np.array([r * math.cos(a), r * math.sin(a), zc]) for a in np.linspace(math.radians(-150.0), a_end, 24)]

    def mask(p):
        d = np.min(np.stack([np.linalg.norm(p - q, axis=1) for q in path]), axis=0)
        return np.clip((0.27 - d) / 0.05, 0.0, 1.0)

    def scene(p):
        m = mask(p)
        base = sd_cylinder(p, (0, 0), r, 0.0, 1.3, 0.03)
        if fuzz > 0.0:
            ang = np.arctan2(p[:, 1], p[:, 0])
            base = base - 0.05 * fuzz * m * value_noise(ang * r / 0.045, p[:, 2] / 0.045)
        return union(bed(p), (base * 0.7, np.where(m > 0.5, M_ACCENT, M_PART)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.65), extent=2.4)
    o = Overlay(render(scene, cam), cam)
    if k < 11:
        centre = path[-1]
        o.circle_on(centre, centre * np.array([1, 1, 0]), 0.2, CURSOR)
        o.pointer(centre)
    return o.finish(GIF_SIZE)


def color_painting(k, n):
    c, h, rr = np.array([0.0, 0.0, 0.5]), 0.5, 0.1
    x_end = -0.32 + 0.64 * phase(k, 0, 9)
    fill = smooth(phase(k, 11, 4))
    click = np.array([0.1, 0.05, 1.0])

    def scene(p):
        a, b = np.array([-0.32, -0.5, 0.42]), np.array([x_end, -0.5, 0.42])
        ab = b - a
        t = np.clip(((p - a) @ ab) / max(ab @ ab, 1e-9), 0.0, 1.0)
        stroke = np.linalg.norm(p - (a + t[:, None] * ab), axis=1) < 0.15
        top = (p[:, 2] > 1.0 - 0.012) & (np.linalg.norm(p[:, :2] - click[:2], axis=1) < 0.75 * fill)
        return union(bed(p), (sd_box(p, c, (h, h, h), rr), np.where(stroke | top, M_ACCENT, M_PART)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.3)
    o = Overlay(render(scene, cam), cam)
    if k < 10:
        centre = np.array([x_end, -0.5, 0.42])
        o.circle_on(centre, (0.0, -1.0, 0.0), 0.15, CURSOR)
        o.pointer(centre)
    elif k < 15:
        o.pointer(click)
    return o.finish(GIF_SIZE)


def _motif(u, v):
    r = np.hypot(u, v)
    ring = np.clip(1.0 - np.abs(r - 0.26) / 0.085, 0.0, 1.0)
    frame = np.clip(1.0 - np.abs(np.maximum(np.abs(u), np.abs(v)) - 0.42) / 0.055, 0.0, 1.0)
    return np.clip(np.maximum(ring, frame) ** 0.7, 0.0, 1.0)


def texture_displacement(k, n):
    depth = 0.10 * ease(k / n)
    top = 0.32
    cell = 0.5

    def scene(p):
        plate = sd_box(p, (0, 0, top / 2), (0.75, 0.75, top / 2), 0.02)
        u = (p[:, 0] / cell) % 1.0 - 0.5
        v = (p[:, 1] / cell) % 1.0 - 0.5
        relief = (p[:, 2] - (top + depth * _motif(u, v))) * 0.4
        relief = np.maximum.reduce([relief, sd_box(p, (0, 0, top), (0.74, 0.74, 0.3)), top - 0.05 - p[:, 2]])
        return union(bed(p), (np.minimum(plate, relief), M_PART))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.25), extent=2.3, el=40.0)
    return Overlay(render(scene, cam, step=0.8), cam).finish(GIF_SIZE)


def _surface_shape(k, n, shape2):
    top = 0.24
    height = 0.12 * math.cos(2.0 * math.pi * (k + 0.5) / n)  # raised, then cut in, then raised again

    def scene(p):
        plate = sd_box(p, (0, 0, top / 2), (0.95, 0.6, top / 2), 0.02)
        d2 = shape2(p[:, 0], p[:, 1])
        if height > 0.0:
            body = union((plate, M_PART), (extrude(d2, top - 0.02, top + height)(p), M_ACCENT))
        else:
            hole = extrude(d2, top + height, top + 0.5)(p)
            body = (np.maximum(plate, -hole), np.where(-hole > plate, M_ACCENT, M_PART))
        return union(bed(p), body)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.2), extent=2.4, el=42.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def text(k, n):
    return _surface_shape(k, n, letters2)


def svg(k, n):
    return _surface_shape(k, n, star2)


def measure(k, n):
    def scene(p):
        low = sd_box(p, (-0.4, 0, 0.2), (0.4, 0.42, 0.2), 0.015)
        high = sd_box(p, (0.35, 0, 0.5), (0.35, 0.42, 0.5), 0.015)
        sel_low = (k >= 3) & (p[:, 2] > 0.4 - 0.01) & (p[:, 0] < 0.0)
        sel_high = (k >= 7) & (p[:, 2] > 1.0 - 0.01)
        mat = np.where(sel_low | sel_high, M_ACCENT, M_PART)
        return union(bed(p), (np.minimum(low, high), mat))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.5)
    o = Overlay(render(scene, cam), cam)
    grow = smooth(phase(k, 9, 4))
    if grow > 0.3:
        x, y = -0.25, -0.42
        z0, z1 = 0.4, 0.4 + 0.6 * grow
        o.line((0.0, y, 1.0), (x - 0.08, y, 1.0), (235, 240, 240, 200), width=1.0)
        o.arrow((x, y, 0.5 * (z0 + z1)), (x, y, z1), (245, 248, 248), width=1.6, head=7.0)
        o.arrow((x, y, 0.5 * (z0 + z1)), (x, y, z0), (245, 248, 248), width=1.6, head=7.0)
    if k < 4:
        o.pointer((-0.45, 0.05, 0.4))
    elif k < 9:
        o.pointer((0.35, 0.05, 1.0))
    return o.finish(GIF_SIZE)


def assembly(k, n):
    base_c, base_h = np.array([-0.25, 0.0, 0.25]), np.array([0.6, 0.45, 0.25])
    part_h = np.array([0.32, 0.26, 0.18])
    R0 = rot_z(math.radians(30.0)) @ rot_x(math.radians(-30.0)) @ rot_y(math.radians(25.0))
    c0 = np.array([0.75, -0.35, 0.95])
    cam = Camera(GIF_SIZE, target=(0.15, 0.0, 0.55), extent=2.7)
    faces = [s * e for e in np.eye(3) for s in (1.0, -1.0)]
    # The face that goes down: one the camera sees and the light falls on, so its highlight shows.
    face = max(faces, key=lambda f: (R0 @ f) @ cam.c + 0.6 * (R0 @ f) @ LIGHT)
    m = R0 @ face
    axis = np.cross(m, [0.0, 0.0, -1.0])
    angle = math.acos(np.clip(m @ np.array([0.0, 0.0, -1.0]), -1.0, 1.0))
    s = smooth(phase(k, 8, 9))
    R = rot_axis(axis, angle * s) @ R0
    along = np.abs(face) @ part_h
    c1 = np.array([base_c[0], base_c[1], 2.0 * base_h[2] + along])
    c = c0 + (c1 - c0) * s

    def scene(p):
        a = sd_box(p, base_c, base_h, 0.015)
        q = (p - c) @ R
        b = sd_box(q, (0, 0, 0), part_h, 0.015)
        sel_a = (k >= 2) & (p[:, 2] > 2.0 * base_h[2] - 0.01)
        sel_b = (k >= 5) & (q @ face > along - 0.01)
        return union(bed(p), (a, np.where(sel_a, M_ACCENT, M_PART)), (b, np.where(sel_b, M_ACCENT, M_PART2)))

    o = Overlay(render(scene, cam), cam)
    if k < 4:
        o.pointer(base_c + np.array([-0.25, 0.1, base_h[2]]))
    elif k < 8:
        o.pointer(c + R @ (face * along))
    return o.finish(GIF_SIZE)


def _fibonacci(count):
    i = np.arange(count) + 0.5
    phi = np.arccos(1.0 - 2.0 * i / count)
    theta = math.pi * (1.0 + 5.0 ** 0.5) * i
    return np.stack([np.cos(theta) * np.sin(phi), np.sin(theta) * np.sin(phi), np.cos(phi)], axis=1)


def simplify(k, n):
    count = int(round(math.exp(math.log(260.0) + (math.log(22.0) - math.log(260.0)) * ease(k / n))))
    normals = _fibonacci(count)
    c = np.array([0.0, 0.0, 0.62])

    def scene(p):
        body = ((p - c) @ normals.T).max(axis=1) - 0.6
        return union(bed(p), (body, M_PART))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.62), extent=2.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def brim_ears(k, n):
    corners = [np.array([0.8 * math.cos(math.radians(a)), 0.8 * math.sin(math.radians(a)), 0.0]) for a in (90.0, 210.0, 330.0)]

    def scene(p):
        tri = sd_polygon2(p[:, 0], p[:, 1], [tuple(c[:2]) for c in corners])
        body = [bed(p), (extrude(tri, 0.0, 0.35)(p), M_PART)]
        for i, c in enumerate(corners):
            grow = smooth(phase(k, 4 * i + 2, 3))
            if grow > 0.0:
                disc = np.hypot(p[:, 0] - c[0], p[:, 1] - c[1]) - 0.3 * grow
                body.append((extrude(disc, 0.0, 0.035)(p), M_ACCENT))
        return union(*body)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.1), extent=2.6, el=52.0)
    o = Overlay(render(scene, cam), cam)
    if k < 12:
        o.pointer(corners[min(k // 4, 2)] + np.array([0.0, 0.0, 0.04]))
    return o.finish(GIF_SIZE)


# --- the main toolbar, left of the gizmos (GLCanvas3D::_render_toolbar_tooltip()) ------------------


def add(k, n):
    # A model arrives on the plate from above and settles.
    s = smooth(phase(k, 3, 10))
    c = np.array([0.0, 0.0, 0.42 + 1.6 * (1.0 - s)])
    scene = lambda p: union(bed(p), (sd_box(p, c, (0.38, 0.38, 0.42), 0.04), M_ACCENT))
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.7), extent=3.0)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def _plate(p, cx):
    """A plate as the plater draws it: a slab a little above the floor, centred at x = cx."""
    return sd_box(p, (cx, 0.0, 0.02), (0.92, 0.92, 0.03), 0.02)


def add_plate(k, n):
    # A second, empty plate slides in beside the first.
    x2 = 1.0 + 2.6 * (1.0 - smooth(phase(k, 4, 10)))

    def scene(p):
        return union(bed(p), (_plate(p, -1.0), M_PART2), (sd_box(p, (-1.0, 0.0, 0.35), (0.3, 0.3, 0.3), 0.03), M_PART),
                     (_plate(p, x2), M_ACCENT))

    cam = Camera(GIF_SIZE, target=(0.1, 0.0, 0.1), extent=4.7)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def _tee(q):
    """A T standing on its stem, in its own frame: stem from z -0.62 to 0.22, bar from 0.22 to 0.42."""
    stem = sd_box(q, (0.0, 0.0, -0.2), (0.14, 0.3, 0.42), 0.02)
    bar = sd_box(q, (0.0, 0.0, 0.32), (0.62, 0.3, 0.1), 0.02)
    return np.minimum(stem, bar)


def orient(k, n):
    # Standing on its stem the T needs support under both arms; auto orient turns it onto its bar, and
    # the support is gone.
    s = smooth(phase(k, 7, 9))
    R = rot_y(math.pi * s)
    corners = np.array([[x, y, z] for x in (-0.62, 0.62) for y in (-0.3, 0.3) for z in (-0.62, 0.42)])
    c = np.array([0.0, 0.0, -(corners @ R.T)[:, 2].min()])

    def scene(p):
        items = [bed(p), (_tee((p - c) @ R), M_PART)]
        if k < 6:
            for x in (-0.4, 0.4):
                items.append((sd_box(p, (x, 0.0, 0.415), (0.18, 0.24, 0.415), 0.0), M_SUPPORT))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.5), extent=2.7)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def arrange(k, n):
    # Objects piled up where they were added spread out into a packed layout with gaps between them.
    s = smooth(phase(k, 4, 11))
    shapes = [  # (start xy, end xy, kind, size)
        ((0.05, 0.1), (-0.95, -0.55), "box", (0.32, 0.32, 0.3)),
        ((0.25, -0.15), (-0.05, -0.55), "cyl", (0.3, 0.45)),
        ((-0.2, -0.1), (0.9, -0.55), "box", (0.4, 0.28, 0.2)),
        ((0.1, 0.3), (-0.65, 0.5), "cyl", (0.36, 0.3)),
        ((-0.15, 0.2), (0.45, 0.5), "box", (0.5, 0.3, 0.25)),
    ]

    def scene(p):
        items = [bed(p)]
        for (x0, y0), (x1, y1), kind, size in shapes:
            x, y = x0 + (x1 - x0) * s, y0 + (y1 - y0) * s
            if kind == "box":
                d = sd_box(p, (x, y, size[2]), size, 0.03)
            else:
                d = sd_cylinder(p, (x, y), size[0], 0.0, size[1], 0.03)
            items.append((d, M_PART))
        return union(*items)

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.2), extent=3.2)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def _copies(grow):
    """Three copies of one object in a row; `grow` scales each, 0 for one that is not there."""
    def scene(p):
        items = [bed(p)]
        for i, g in enumerate(grow):
            if g > 0.0:
                h = 0.3 * g
                items.append((sd_cylinder(p, (-0.85 + 0.85 * i, 0.0), h, 0.0, 2.0 * h, 0.03), M_PART if i == 0 else M_ACCENT))
        return union(*items)
    return scene


def instance_add(k, n):
    scene = _copies([1.0, smooth(phase(k, 4, 4)), smooth(phase(k, 10, 4))])
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.3), extent=2.8)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def instance_remove(k, n):
    scene = _copies([1.0, 1.0 - smooth(phase(k, 10, 4)), 1.0 - smooth(phase(k, 4, 4))])
    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.3), extent=2.8)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def _two_pieces(p, off):
    """One model made of two pieces that do not touch: a block, and a ring around a peg beside it."""
    block = sd_box(p, (-0.45, 0.0, 0.3), (0.3, 0.3, 0.3), 0.03)
    q = p - np.array([off, 0.0, 0.0])
    ring = np.maximum(sd_cylinder(q, (0.45, 0.0), 0.36, 0.0, 0.3, 0.02), -sd_cylinder(q, (0.45, 0.0), 0.2, -1.0, 1.0))
    return block, ring


def split_objects(k, n):
    # The pieces become objects of their own, each placed and set up on its own.
    s = smooth(phase(k, 6, 8))

    def scene(p):
        block, ring = _two_pieces(p - np.array([-0.45 * s, -0.3 * s, 0.0]), 1.0 * s)
        return union(bed(p), (block, M_PART), (ring, M_ACCENT if k >= 6 else M_PART))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.25), extent=3.2)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def split_parts(k, n):
    # The pieces stay where they are, as parts of the same object that can take their own settings.
    def scene(p):
        block, ring = _two_pieces(p, 0.0)
        return union(bed(p), (block, M_PART), (ring, M_ACCENT if k >= 6 else M_PART))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.25), extent=2.8)
    o = Overlay(render(scene, cam), cam)
    if 6 <= k < 12:
        o.circle_on((0.45, 0.0, 0.31), (0.0, 0.0, 1.0), 0.42, (60, 200, 180), width=1.6)
    return o.finish(GIF_SIZE)


def variable_layer_height(k, n):
    # Layers thin out where the dome curves over and stay thick on the straight sides.
    s = ease(k / n)
    r, zc = 0.7, 0.45
    z = np.linspace(0.0, zc + r, 400)
    nz = np.clip((z - zc) / r, 0.0, 1.0)
    h = (1.0 - s) * 0.12 + s * 0.17 * (1.0 - 0.8 * nz)  # layer height at each z
    bounds = np.concatenate([[0.0], np.cumsum(np.diff(z) / h[1:])])  # layers so far at each z

    def scene(p):
        d = np.minimum(sd_cylinder(p, (0.0, 0.0), r, 0.0, zc), sd_sphere(p, (0.0, 0.0, zc), r))
        layer = np.interp(p[:, 2], z, bounds)
        return union(bed(p), (d, np.where(np.floor(layer) % 2 == 0, M_PART, M_PART2)))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.55), extent=2.2)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


def assembly_view(k, n):
    # The parts of an object drawn apart, then back together.
    s = ease(k / n)

    def scene(p):
        base = sd_box(p, (0.0, 0.0, 0.15), (0.6, 0.45, 0.15), 0.03)
        mid = sd_cylinder(p - np.array([0.0, 0.0, 0.45 * s]), (0.0, 0.0), 0.32, 0.3, 0.75, 0.03)
        top = sd_box(p - np.array([0.0, 0.0, 0.9 * s]), (0.0, 0.0, 0.85), (0.45, 0.35, 0.1), 0.03)
        return union(bed(p), (base, M_PART), (mid, M_PART2), (top, M_ACCENT))

    cam = Camera(GIF_SIZE, target=(0.0, 0.0, 0.85), extent=2.8)
    return Overlay(render(scene, cam), cam).finish(GIF_SIZE)


# name -> frame count (0 = a still, written as PNG)
FRAMES = {
    "move": 16,
    "rotate": 16,
    "scale": 16,
    "lay_on_face": 20,
    "cut": 19,
    "boolean_union": 0,
    "boolean_difference": 0,
    "boolean_intersection": 0,
    "support_painting": 22,
    "seam_painting": 22,
    "fuzzy_skin_painting": 22,
    "color_painting": 20,
    "texture_displacement": 16,
    "text": 16,
    "svg": 16,
    "measure": 19,
    "assembly": 22,
    "simplify": 16,
    "brim_ears": 18,
    "add": 18,
    "add_plate": 18,
    "orient": 20,
    "arrange": 18,
    "instance_add": 18,
    "instance_remove": 18,
    "split_objects": 18,
    "split_parts": 18,
    "variable_layer_height": 20,
    "assembly_view": 20,
}


