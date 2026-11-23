#!/usr/bin/env python3
"""Draws the illustrations the texture-displacement tooltips show.

The panel's tooltips can carry a picture under their text (see GLGizmoTextureDisplacement's
panel_tooltip()). A good many of those settings - the ones that are a shape rather than a photograph -
are better drawn from the same arithmetic the feature uses than captured from the screen, which is
what this script does: it shades a height field exactly as a relief would catch the light, so
"what Rotation turns" and "what Tile repeats" are visible rather than described.

Settings that involve the model, the mesh or colour are not here: those want a capture of the real
tool. See docs/HLSD/tooltip-illustrations.md.

Output goes to resources/images/texture_displacement/. Run it after changing one of the patterns
below; nothing in the build depends on it.

    python3 scripts/gen_texture_displacement_tips.py

Needs numpy and pillow.
"""

import os

import numpy as np
from PIL import Image, ImageDraw

SIZE = 256  # px, square - the panel draws these at whatever width the tooltip has
GIF_SIZE = 192  # animations are the same picture, smaller: every frame of one ships in resources/
RELIEF = 30.0  # how hard the shading reacts to a slope; purely cosmetic

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "images", "texture_displacement")

# Neutral enough to sit in a tooltip in either theme: a dark ground, a mid grey for the flat surface
# and a cool near-white for the lit slopes.
SHADOW = np.array([0.13, 0.14, 0.15])
MIDTONE = np.array([0.40, 0.42, 0.43])
HIGHLIGHT = np.array([0.78, 0.84, 0.83])
LIGHT = np.array([-0.45, -0.62, 0.65])
LIGHT /= np.linalg.norm(LIGHT)


def grid(size=SIZE):
    """Unit coordinates, -0.5 .. 0.5, y down as in an image."""
    a = (np.arange(size) + 0.5) / size - 0.5
    return np.meshgrid(a, a)


def shade(height):
    """A height field in 0..1, lit as a relief: the picture the tooltip shows."""
    # The slope has to mean the same thing at either size, so the gradient is taken per unit of the
    # picture rather than per pixel - otherwise the smaller animations would come out twice as flat.
    gy, gx = np.gradient(height * RELIEF * (height.shape[0] / float(SIZE)))
    normal = np.dstack([-gx, -gy, np.ones_like(height)])
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    lam = np.clip(normal @ LIGHT, 0.0, 1.0)

    # Two ramps rather than one: the dark half separates the slopes, the lit half picks up the
    # accent, and the flat ground lands in the middle of both.
    low = SHADOW + (MIDTONE - SHADOW) * (lam / 0.5)[..., None]
    high = MIDTONE + (HIGHLIGHT - MIDTONE) * ((lam - 0.5) / 0.5)[..., None]
    rgb = np.where((lam < 0.5)[..., None], low, high)

    # A touch of the height itself, so a tall feature still reads as tall where it is flat on top.
    #
    # Deliberately almost grey: these say what a shape does, and a relief catching the light is grey.
    # A coloured one would read as a third thing the setting does, and the amount of colour would move
    # with the slope - a pattern at 45 degrees would flash where the same pattern upright did not.
    rgb = np.clip(rgb * (0.88 + 0.18 * height[..., None]), 0.0, 1.0)
    return (rgb * 255.0 + 0.5).astype(np.uint8)


def vignette(rgb):
    """Rounds the picture off at its border, so it does not end in a hard rectangle."""
    x, y = grid(rgb.shape[0])
    edge = np.clip((0.5 - np.maximum(np.abs(x), np.abs(y))) / 0.045, 0.0, 1.0)
    return (rgb * edge[..., None]).astype(np.uint8)


def save_png(name, rgb):
    path = os.path.join(OUT_DIR, name)
    Image.fromarray(vignette(rgb)).save(path, optimize=True)
    print("wrote", os.path.normpath(path), os.path.getsize(path) // 1024, "KB")


def save_gif(name, frames, delay_ms):
    path = os.path.join(OUT_DIR, name)
    images = [Image.fromarray(vignette(f)).convert("P", palette=Image.ADAPTIVE, colors=24) for f in frames]
    images[0].save(path, save_all=True, append_images=images[1:], duration=delay_ms, loop=0, optimize=True)
    print("wrote", os.path.normpath(path), os.path.getsize(path) // 1024, "KB")


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def blur(a, radius):
    """Separable box blur, run twice - near enough a gaussian for a picture of a blur."""
    for _ in range(2):
        for axis in (0, 1):
            padded = np.pad(a, [(radius, radius) if i == axis else (0, 0) for i in range(2)], mode="edge")
            cumulative = np.cumsum(padded, axis=axis)
            cumulative = np.concatenate([np.zeros_like(np.take(cumulative, [0], axis=axis)), cumulative], axis=axis)
            n = a.shape[axis]
            hi = np.take(cumulative, np.arange(n) + 2 * radius + 1, axis=axis)
            lo = np.take(cumulative, np.arange(n), axis=axis)
            a = (hi - lo) / (2.0 * radius + 1.0)
    return a


# --- the patterns ------------------------------------------------------------------------------
#
# One motif runs through nearly all of them - a ring inside a square frame. Using the same shape for
# every setting is what lets the pictures be compared: whatever changes between two of them is the
# setting, because nothing else is different.


def _motif_cell(u, v):
    """The motif within one cell, u and v in -0.5 .. 0.5."""
    r = np.hypot(u, v)
    ring = np.clip(1.0 - np.abs(r - 0.26) / 0.085, 0.0, 1.0)
    frame_d = np.maximum(np.abs(u), np.abs(v))
    frame = np.clip(1.0 - np.abs(frame_d - 0.42) / 0.055, 0.0, 1.0)
    return np.clip(np.maximum(ring, frame) ** 0.7, 0.0, 1.0)


def motif(repeats, size=SIZE):
    """The motif laid out `repeats` times across the picture."""
    x, y = grid(size)
    return _motif_cell((x * repeats) % 1.0 - 0.5, (y * repeats) % 1.0 - 0.5)


def decal(motif_scale=0.46):
    """One copy of the motif in the middle of a flat field - what Tile off leaves."""
    x, y = grid()
    u, v = x / motif_scale, y / motif_scale
    inside = (np.maximum(np.abs(u), np.abs(v)) <= 0.5).astype(float)
    return _motif_cell(u, v) * inside


def ridges(angle_deg, period=0.16, size=SIZE):
    """Parallel ridges at an angle: the simplest pattern whose direction is unmistakable."""
    x, y = grid(size)
    a = np.radians(angle_deg)
    t = (x * np.cos(a) + y * np.sin(a)) / period
    return 0.2 + 0.6 * (0.5 + 0.5 * np.sin(2.0 * np.pi * t))


def bosses(repeats, size=SIZE):
    """Rounded domes standing on a flat field - a shape with an unmistakable right way up."""
    x, y = grid(size)
    u = (x * repeats) % 1.0 - 0.5
    v = (y * repeats) % 1.0 - 0.5
    return smoothstep(0.36, 0.08, np.hypot(u, v))


def chevron_tiles(mirrored, repeats=3):
    """Cells of diagonal ridges, laid out repeated or mirrored.

    Repeated, every cell starts the pattern over and the seam between two of them is a step; mirrored,
    each cell is the reflection of its neighbour and the ridges meet across the seam as chevrons. That
    seam is the whole of what the Repeat/Mirror pair does, and a motif symmetric in itself would hide
    it entirely.
    """

    def axis(t):
        if not mirrored:
            return (t * repeats) % 1.0
        s = (t * repeats) % 2.0
        return np.where(s < 1.0, s, 2.0 - s)

    x, y = grid()
    t = (axis(x + 0.5) + axis(y + 0.5)) * 1.5
    return 0.22 + 0.56 * (0.5 + 0.5 * np.cos(2.0 * np.pi * t))


def painted_area(size=SIZE, radius=0.44):
    """A round painted area in the middle of bare surface, with a soft edge so it does not alias."""
    x, y = grid(size)
    return smoothstep(radius, radius - 1.5 / size, np.hypot(x, y))


def painted_patch(fade, size=SIZE):
    """The motif inside a painted area, either stopping at its edge or fading into it."""
    x, y = grid(size)
    r = np.hypot(x, y) / 0.44
    amount = smoothstep(1.0, 0.55, r) if fade else 1.0
    return motif(3, size) * painted_area(size) * amount


def profile_wave(size=SIZE, periods=2.5):
    """A one-dimensional height profile with clear highs, lows and a flat stretch between them."""
    t = (np.arange(size) + 0.5) / size * periods * 2.0 * np.pi
    h = 0.5 + 0.34 * np.cos(t) + 0.16 * np.cos(2.0 * t + 0.9)
    return np.clip((h - h.min()) / (h.max() - h.min()), 0.0, 1.0)


def profile_view(height_1d, midlevel, amplitude=0.26, size=SIZE):
    """A side view of the surface: material below, air above, and the bare surface as a dashed line.

    Shading cannot say how high something is - it reads slopes, and a height field moved up or down as
    a whole has exactly the slopes it had before. Midlevel is entirely about where the texture sits
    relative to the surface it is on, so it is drawn in section, where that is the one thing visible.
    """
    base = 0.52 * size
    surface = base - (height_1d - midlevel) * amplitude * size
    rows = np.arange(size)[:, None]

    covered = np.clip(rows - surface[None, :] + 0.5, 0.0, 1.0)  # 1 where there is material
    air = np.array([0.11, 0.12, 0.13])
    material = np.array([0.44, 0.46, 0.47])
    rgb = air * (1.0 - covered)[..., None] + material * covered[..., None]

    # The lit top face, and the bare surface the texture is measured against.
    edge = np.clip(1.0 - np.abs(rows - surface[None, :]) / 1.6, 0.0, 1.0)
    rgb = rgb * (1.0 - edge[..., None]) + np.array([0.80, 0.86, 0.85]) * edge[..., None]
    dash = ((np.arange(size) // 7) % 2 == 0)[None, :]
    line = (np.abs(rows - base) < 0.9) & dash
    rgb[line] = np.array([0.58, 0.62, 0.62])
    return (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def dome(size=SIZE):
    """One broad swell filling the picture - the layer underneath, for the blend pictures."""
    x, y = grid(size)
    return smoothstep(0.52, 0.02, np.hypot(x, y))


# --- meshes, cursors and sections ---------------------------------------------------------------
#
# The second family of pictures. The settings above are about a pattern, and a shaded relief is what a
# pattern looks like; these are about a tool or about the mesh, where what matters is which triangles
# exist and which of them are painted. So they are drawn flat, the way the panel's own wireframe and
# paint overlays draw them, with one accent colour meaning "painted" and nothing else coloured.

SS = 3  # supersampling: PIL fills polygons without antialiasing, so draw big and shrink

INK = (0.62, 0.67, 0.67)  # mesh edges
BARE = (0.28, 0.30, 0.31)  # unpainted surface
PAINT = (0.20, 0.47, 0.45)  # painted with the active layer, in the panel's own accent
CURSOR = (0.92, 0.95, 0.95)
MARGIN = 0.05  # keeps the mesh off the picture's border, where the vignette darkens it


def _px(p, size):
    """Unit coordinates to pixels, inside the margin."""
    return ((MARGIN + p[0] * (1.0 - 2.0 * MARGIN)) * size, (MARGIN + p[1] * (1.0 - 2.0 * MARGIN)) * size)


def _rgb255(c):
    return tuple(int(round(v * 255.0)) for v in c)


def raster(draw_fn, size=SIZE, background=BARE):
    """Runs draw_fn(draw, size_in_pixels) on a supersampled canvas and hands back the shrunk result.

    `background` is a colour, or a picture already drawn by the relief half of this script - which is
    how a mesh is shown sitting on the surface it refines.
    """
    if isinstance(background, np.ndarray):
        image = Image.fromarray(background).resize((size * SS, size * SS), Image.BILINEAR)
    else:
        image = Image.new("RGB", (size * SS, size * SS), _rgb255(background))
    draw_fn(ImageDraw.Draw(image), size * SS)
    return np.asarray(image.resize((size, size), Image.LANCZOS)).astype(np.uint8)


def tri_mesh(n, jitter=0.0, seed=1):
    """A square patch of triangles, optionally with its inner vertices nudged off the grid.

    Jitter is not decoration: a perfectly regular grid reads as graph paper, and the point of these
    pictures is a mesh over a surface.
    """
    rng = np.random.default_rng(seed)
    v = np.stack(np.meshgrid(np.linspace(0.0, 1.0, n + 1), np.linspace(0.0, 1.0, n + 1)), axis=-1)
    if jitter:
        v[1:n, 1:n] += (rng.random((n - 1, n - 1, 2)) - 0.5) * (jitter / n)
    tris = []
    for i in range(n):
        for j in range(n):
            tris.append((v[i, j], v[i, j + 1], v[i + 1, j + 1]))
            tris.append((v[i, j], v[i + 1, j + 1], v[i + 1, j]))
    return tris


def centroids(tris):
    return np.array([np.mean(t, axis=0) for t in tris])


def neighbours(tris):
    """Which triangles share an edge with which, keyed on the rounded vertex positions."""
    edges = {}
    for i, t in enumerate(tris):
        for a, b in ((0, 1), (1, 2), (2, 0)):
            key = tuple(sorted((tuple(np.round(t[a], 6)), tuple(np.round(t[b], 6)))))
            edges.setdefault(key, []).append(i)
    out = [[] for _ in tris]
    for shared in edges.values():
        if len(shared) == 2:
            out[shared[0]].append(shared[1])
            out[shared[1]].append(shared[0])
    return out


def draw_mesh(draw, size, tris, painted=None, edge=INK, edge_width=0.7):
    fill_bare, fill_paint = _rgb255(BARE), _rgb255(PAINT)
    for i, t in enumerate(tris):
        draw.polygon([_px(p, size) for p in t],
                     fill=fill_paint if painted is not None and painted[i] else fill_bare,
                     outline=_rgb255(edge), width=max(1, int(edge_width * SS)))


def draw_brush(draw, size, centre, radius, filled=False):
    """The brush cursor: a ring, which is how the gizmo draws it on the model."""
    c = _px(centre, size)
    r = radius * (1.0 - 2.0 * MARGIN) * size
    if filled:
        draw.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=(255, 255, 255, 40))
    draw.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], outline=_rgb255(CURSOR), width=max(1, int(1.6 * SS)))


def draw_pointer(draw, size, at):
    """A small arrow, for the tools that are clicked rather than dragged."""
    x, y = _px(at, size)
    k = 0.055 * size
    draw.polygon([(x, y), (x, y + 1.45 * k), (x + 0.38 * k, y + 1.05 * k),
                  (x + 0.62 * k, y + 1.55 * k), (x + 0.90 * k, y + 1.40 * k),
                  (x + 0.66 * k, y + 0.92 * k), (x + 1.05 * k, y + 0.84 * k)],
                 fill=_rgb255(CURSOR), outline=(30, 32, 34), width=max(1, int(0.9 * SS)))


def stroke_path(t):
    """The path a demonstrated brush stroke follows, t in 0..1."""
    return np.array([0.14 + 0.72 * t, 0.46 + 0.22 * np.sin(2.0 * np.pi * t * 1.1)])


def brush_frames(size, erase, steps=14, radius=0.115, hold=3):
    tris = tri_mesh(9, jitter=0.55)
    cents = centroids(tris)
    painted = np.ones(len(tris), bool) if erase else np.zeros(len(tris), bool)
    frames = []
    for k in range(steps):
        centre = stroke_path(k / (steps - 1.0))
        hit = np.linalg.norm(cents - centre, axis=1) < radius
        painted = painted & ~hit if erase else painted | hit
        state = painted.copy()
        frames.append(raster(lambda d, s, st=state, c=centre: (draw_mesh(d, s, tris, st), draw_brush(d, s, c, radius)), size))
    frames += [frames[-1]] * hold
    return frames


def face_frames(size, steps=9, hold=3):
    """The Face tool: one triangle per click, under a pointer."""
    tris = tri_mesh(7, jitter=0.5, seed=4)
    cents = centroids(tris)
    order = np.argsort(np.linalg.norm(cents - np.array([0.5, 0.5]), axis=1))[:steps]
    painted = np.zeros(len(tris), bool)
    frames = []
    for i in order:
        painted[i] = True
        state = painted.copy()
        at = cents[i]
        frames.append(raster(lambda d, s, st=state, a=at: (draw_mesh(d, s, tris, st), draw_pointer(d, s, a)), size))
    frames += [frames[-1]] * hold
    return frames


def flood_frames(size, creases, steps=12, hold=3, seed=7):
    """The Connected area tool: a flood from one click, stopped by the creases it may not cross.

    `creases` is a list of x positions; a triangle pair straddling one cannot pass the fill between
    them, which is what an edge sharper than the angle threshold does on the model.
    """
    tris = tri_mesh(9, jitter=0.5, seed=seed)
    cents = centroids(tris)
    adj = neighbours(tris)
    blocked = lambda i, j: any((cents[i][0] - x) * (cents[j][0] - x) < 0.0 for x in creases)

    seed_tri = int(np.argmin(np.linalg.norm(cents - np.array([0.22, 0.5]), axis=1)))
    painted = np.zeros(len(tris), bool)
    painted[seed_tri] = True
    frontier = [seed_tri]
    frames = []
    for _ in range(steps):
        state = painted.copy()
        frames.append(raster(lambda d, s, st=state: (draw_mesh(d, s, tris, st),
                                                     draw_creases(d, s, creases),
                                                     draw_pointer(d, s, cents[seed_tri])), size))
        nxt = []
        for i in frontier:
            for j in adj[i]:
                if not painted[j] and not blocked(i, j):
                    painted[j] = True
                    nxt.append(j)
        frontier = nxt
    frames += [frames[-1]] * hold
    return frames


def draw_creases(draw, size, creases):
    """A fold too sharp for the fill to cross: drawn dark with a bright crest, like a real one."""
    for x in creases:
        a, b = _px((x, -0.02), size), _px((x, 1.02), size)
        draw.line([a, b], fill=_rgb255((0.08, 0.09, 0.10)), width=max(1, int(4.0 * SS)))
        draw.line([a, b], fill=_rgb255((0.97, 0.99, 0.99)), width=max(1, int(1.4 * SS)))



def split4(tri):
    a, b, c = tri
    ab, bc, ca = (a + b) * 0.5, (b + c) * 0.5, (c + a) * 0.5
    return [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]


def refine(tris, density, max_depth, threshold=0.45):
    """Splits a triangle into four wherever `density` at its middle asks for it, up to max_depth."""

    def go(tri, depth):
        if depth >= max_depth:
            return [tri]
        centre = np.mean(tri, axis=0)
        if density(centre[0], centre[1]) < threshold:
            return [tri]
        return [out for part in split4(tri) for out in go(part, depth + 1)]

    return [out for tri in tris for out in go(tri, 0)]


def sampler(field):
    """Looks a numpy field up at unit coordinates, for use as a density."""
    n = field.shape[0]
    return lambda x, y: float(field[int(np.clip(y * n, 0, n - 1)), int(np.clip(x * n, 0, n - 1))])


def feature_density(size=192):
    """High where the pattern bends, which is where feature-adaptive refinement spends triangles.

    One large copy of the motif rather than a field of them: the point is that the triangles gather
    somewhere and not elsewhere, and a pattern that bends everywhere has no elsewhere to show.
    """
    m = motif(1, size)
    gy, gx = np.gradient(m)
    g = blur(np.hypot(gx, gy), 2)
    return g / max(g.max(), 1e-6)


def irregular_mesh(seed=3):
    """The uneven mesh a model arrives with: coarse here, fine there, nothing of one size."""
    rng = np.random.default_rng(seed)
    tris = tri_mesh(4, jitter=0.9, seed=seed)
    out = []
    for t in tris:
        out.extend(split4(t) if rng.random() < 0.45 else [t])
    final = []
    for t in out:
        final.extend(split4(t) if rng.random() < 0.3 else [t])
    return final


def snap_to_crease(tris, crease, distance=0.07):
    """Pulls every vertex near the crease onto it, so mesh edges run along it instead of across."""
    (x0, y0), (x1, y1) = crease
    d = np.array([x1 - x0, y1 - y0], float)
    d /= np.linalg.norm(d)

    def snap(p):
        t = np.dot(p - np.array([x0, y0]), d)
        foot = np.array([x0, y0]) + t * d
        return foot if np.linalg.norm(p - foot) < distance else p

    return [tuple(snap(np.asarray(v, float)) for v in t) for t in tris]


def sliver_mesh(seed=5):
    """A mesh carrying the thin triangles refinement inherits from a model's own triangulation."""
    rng = np.random.default_rng(seed)
    tris = tri_mesh(7, jitter=0.4, seed=seed)
    out = []
    for t in tris:
        if rng.random() < 0.45:
            a, b, c = (np.asarray(v, float) for v in t)
            mid = (b + c) * 0.5
            a = mid + (a - mid) * 0.08  # all but flattened onto the opposite edge
            out.append((a, b, c))
        else:
            out.append(t)
    return out


def draw_profile(draw, size, height_1d, baseline=0.62, amplitude=0.30):
    """A side view of a surface, as a filled region under its profile."""
    n = len(height_1d)
    top = [(i / (n - 1.0) * size, (baseline - (height_1d[i] - 0.5) * amplitude) * size) for i in range(n)]
    draw.polygon(top + [(size, size * 1.1), (0, size * 1.1)], fill=_rgb255((0.44, 0.46, 0.47)))
    draw.line(top, fill=_rgb255((0.82, 0.88, 0.87)), width=max(1, int(1.8 * SS)))


def bayer(size, n=4):
    """An ordered dither matrix tiled over the picture, for the mixing illustrations."""
    m = np.array([[0]])
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    m = (m + 0.5) / m.size
    return np.tile(m, (size // m.shape[0] + 1, size // m.shape[1] + 1))[:size, :size]


def levels_image(levels, dither=False, speckle=0.0, size=SIZE, seed=11):
    """A smooth ramp reduced to a handful of greys - what a bake does to a photograph's colours.

    Flat rather than lit: this family of settings is about which filament a pixel ends up as, and a
    relief would only add a second thing changing between two pictures that are meant to differ in
    one.
    """
    x, y = grid(size)
    ramp = np.clip(0.5 + 0.5 * np.cos(2.0 * np.pi * (x + 0.25 * y)), 0.0, 1.0)
    noise = bayer(size) - 0.5 if dither else 0.0
    q = np.round(np.clip(ramp + noise / max(levels - 1, 1), 0.0, 1.0) * (levels - 1)) / (levels - 1)
    if speckle > 0.0:
        rng = np.random.default_rng(seed)
        hit = rng.random((size, size)) < speckle
        q = np.where(hit, rng.integers(0, levels, (size, size)) / (levels - 1), q)
    grey = 0.16 + 0.62 * q
    return (np.dstack([grey, grey * 1.03, grey * 1.02]).clip(0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def draw_mesh_lines(draw, size, tris, colour=(0.90, 0.94, 0.94), width=0.8):
    """Only the edges, for drawing a mesh over a picture of the surface it sits on."""
    for t in tris:
        pts = [_px(p, size) for p in t]
        draw.line(pts + [pts[0]], fill=_rgb255(colour), width=max(1, int(width * SS)))


def draw_line(draw, size, segment, colour=(0.97, 0.99, 0.99)):
    a, b = _px(segment[0], size), _px(segment[1], size)
    draw.line([a, b], fill=_rgb255((0.08, 0.09, 0.10)), width=max(1, int(4.0 * SS)))
    draw.line([a, b], fill=_rgb255(colour), width=max(1, int(1.4 * SS)))


def posterize(rgb, levels):
    """The shaded picture reduced to flat bands - what the fast preview's per-fragment quantizing
    does to a surface that the real preview shades smoothly."""
    v = rgb.astype(float) / 255.0
    return (np.round(v * (levels - 1)) / (levels - 1) * 255.0 + 0.5).astype(np.uint8)


def with_checker(rgb, squares=10, strength=0.30, warp=False):
    """A checker laid over the surface, evenly or stretched.

    Even squares are what the Checker view is for - a texture laid out without stretching. The warped
    version is the same squares through a mapping that does stretch, which is what the Distortion view
    is there to find: cells crowded in one place and pulled apart in another.
    """
    size = rgb.shape[0]
    x, y = grid(size)
    u, v = (np.sign(x) * np.abs(2.0 * x) ** 2.4 * 0.5, y * (1.0 + 0.9 * np.abs(x))) if warp else (x, y)
    cells = (np.floor((u + 0.5) * squares) + np.floor((v + 0.5) * squares)) % 2.0
    return (rgb * (1.0 - strength * cells)[..., None]).astype(np.uint8)


def projected_sphere(kind, size=SIZE, squares=9):
    """A ball with a checker laid on it by one of the panel's projections.

    One shape, five mappings: what differs between the pictures is only where the squares go, which is
    the whole of what choosing a projection does. A sphere is the shape that shows it - every one of
    these mappings is well behaved on a flat face and tells itself apart on a curved one.
    """
    a = (np.arange(size) + 0.5) / size * 2.24 - 1.12
    x, y = np.meshgrid(a, a)
    inside = x * x + y * y <= 1.0
    z = np.sqrt(np.clip(1.0 - x * x - y * y, 0.0, 1.0))

    lam = np.clip(x * LIGHT[0] + y * LIGHT[1] + z * LIGHT[2], 0.0, 1.0)
    rgb = SHADOW + (HIGHLIGHT - SHADOW) * (lam ** 0.85)[..., None]

    around = np.arctan2(x, z) / (2.0 * np.pi) + 0.5
    if kind == "sphere":
        u, v = around, np.arccos(np.clip(-y, -1.0, 1.0)) / np.pi
    elif kind == "cylinder":
        u, v = around, (y + 1.0) * 0.5
    elif kind == "view":
        u, v = (x + 1.0) * 0.5, (y + 1.0) * 0.5
    elif kind == "cube":
        axis = np.argmax(np.abs(np.dstack([x, y, z])), axis=2)
        u = np.where(axis == 0, z, x)
        v = np.where(axis == 1, z, y)
        u, v = (u + 1.0) * 0.5, (v + 1.0) * 0.5
    else:
        raise ValueError(kind)

    cells = (np.floor(u * squares * 2.0) + np.floor(v * squares)) % 2.0
    rgb = rgb * (1.0 - 0.34 * cells)[..., None]
    rgb = np.where(inside[..., None], rgb, np.array([0.13, 0.14, 0.15]))
    return (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)


def uv_islands(size=SIZE, squares=9, seed=12):
    """The flat layout an unwrap produces: the surface cut into islands and laid out side by side.

    Not a projection onto the model like the other four, so not drawn on one - an unwrap is the model
    carrying its own coordinates, and this is what those look like.
    """
    rng = np.random.default_rng(seed)
    x, y = grid(size)
    cells = (np.floor((x + 0.5) * squares * 2.0) + np.floor((y + 0.5) * squares)) % 2.0
    checker = (np.array([0.40, 0.43, 0.43]) + np.array([0.22, 0.24, 0.24]) * (1.0 - cells)[..., None])

    def blob(centre, radius, lobes):
        angles = np.linspace(0.0, 2.0 * np.pi, 26, endpoint=False)
        wobble = 1.0 + 0.22 * np.sin(lobes * angles + rng.random() * 6.0)
        return [(centre[0] + np.cos(t) * radius * w, centre[1] + np.sin(t) * radius * w)
                for t, w in zip(angles, wobble)]

    islands = [blob((0.31, 0.32), 0.21, 3), blob((0.72, 0.30), 0.17, 4),
               blob((0.34, 0.74), 0.17, 4), blob((0.72, 0.71), 0.20, 3)]

    mask = np.asarray(raster(lambda d, px: [d.polygon([(p[0] * px, p[1] * px) for p in island], fill=(255, 255, 255))
                                            for island in islands],
                             size, background=(0.0, 0.0, 0.0))).astype(float)[..., 0] / 255.0
    rgb = checker * mask[..., None] + np.array([0.13, 0.14, 0.15]) * (1.0 - mask)[..., None]
    out = (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    return raster(lambda d, px: [d.polygon([(p[0] * px, p[1] * px) for p in island], outline=_rgb255(INK),
                                           width=max(1, int(1.1 * SS)))
                                 for island in islands],
                  size, background=out)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    # Tile: one copy against the pattern filling the painted area.
    save_png("tile_off.png", shade(decal()))
    save_png("tile_on.png", shade(motif(3)))

    # Tile size: how wide one copy is.
    save_png("tile_size_large.png", shade(motif(2)))
    save_png("tile_size_small.png", shade(motif(5)))

    # Repeat against Mirror, where the seam between two copies is the whole point.
    save_png("tile_repeat.png", shade(chevron_tiles(mirrored=False)))
    save_png("tile_mirror.png", shade(chevron_tiles(mirrored=True)))

    # Invert: the negative of the image. Domes rather than the ring motif, which is close enough to
    # its own negative that inverting it would look like nothing had happened.
    save_png("invert_off.png", shade(bosses(3)))
    save_png("invert_on.png", shade(1.0 - bosses(3)))

    # Smoothing: a stepped image against the same one blurred before it is used. Domes again, where
    # the steps show up as contour rings - what a photograph with too few grey levels does.
    stepped = np.floor(bosses(3) * 5.0) / 4.0
    save_png("smoothing_off.png", shade(np.clip(stepped, 0.0, 1.0)))
    save_png("smoothing_on.png", shade(np.clip(blur(stepped, max(2, SIZE // 40)), 0.0, 1.0)))

    # Edge fade: relief that stops at the edge of the painted area against relief that sinks into it.
    save_png("edge_fade_off.png", shade(painted_patch(fade=False)))
    save_png("edge_fade_on.png", shade(painted_patch(fade=True)))

    # Blend: piling relief on top of the layer below against using it as a mask over it. Add keeps
    # both shapes, Multiply keeps the motif only where the layer under it is high.
    base = dome()
    save_png("blend_add.png", shade(np.clip(0.55 * base + 0.42 * motif(3), 0.0, 1.0)))
    save_png("blend_multiply.png", shade(base * motif(3)))

    # Rotation: a half turn brings parallel ridges back onto themselves, so a single sweep loops with
    # no jump and the file stays a fraction of the size a there-and-back one would be.
    steps = 16
    save_gif("rotation.gif", [shade(ridges(180.0 * i / steps, size=GIF_SIZE)) for i in range(steps)], delay_ms=100)

    # Depth and Midlevel are both about where the relief sits relative to the surface it is on, so
    # both are drawn as a painted area in the middle of bare surface, which is that reference. Without
    # it neither would show: shading reads slopes, and a height field that moves up or down as a whole
    # has exactly the same slopes it had before.
    #
    # A cosine over the frame count starts and ends at the same value with no step in between, so a
    # single pass through it loops.
    area = painted_area(GIF_SIZE)
    pattern = motif(3, GIF_SIZE)
    sweep = [0.5 - 0.5 * np.cos(2.0 * np.pi * i / steps) for i in range(steps)]

    # Depth: the relief growing out of the surface and settling back flush with it.
    frames = [shade(np.clip(0.5 + 0.45 * pattern * (0.08 + 0.92 * t) * area, 0.0, 1.0)) for t in sweep]
    save_gif("depth.gif", frames, delay_ms=100)

    # Midlevel: the grey that stays put, sweeping from "the whole texture pushes out" through
    # "mid-grey stays where the surface is" to "the whole of it cuts in". Drawn in section - see
    # profile_view() for why this one is not a relief like the rest.
    wave = profile_wave(GIF_SIZE)
    frames = [profile_view(wave, t, size=GIF_SIZE) for t in sweep]
    save_gif("midlevel.gif", frames, delay_ms=100)

    # The paint tools, each shown doing the one thing that tells it apart from the others.
    save_gif("paint.gif", brush_frames(GIF_SIZE, erase=False), delay_ms=110)
    save_gif("erase.gif", brush_frames(GIF_SIZE, erase=True), delay_ms=110)
    save_gif("tool_face.gif", face_frames(GIF_SIZE), delay_ms=180)
    save_gif("tool_smart_fill.gif", flood_frames(GIF_SIZE, creases=[0.63]), delay_ms=110)

    # Brush size: the ring, and what it covers, over one size and back.
    brush_tris = tri_mesh(9, jitter=0.55, seed=2)
    brush_cents = centroids(brush_tris)
    frames = []
    for i in range(steps):
        r = 0.07 + 0.16 * (0.5 - 0.5 * np.cos(2.0 * np.pi * i / steps))
        under = np.linalg.norm(brush_cents - np.array([0.5, 0.5]), axis=1) < r
        frames.append(raster(lambda d, sz, u=under, rr=r: (draw_mesh(d, sz, brush_tris, u),
                                                           draw_brush(d, sz, (0.5, 0.5), rr)), GIF_SIZE))
    save_gif("cursor_size.gif", frames, delay_ms=90)

    # Angle threshold: the same fill, let past one fold and then past both.
    for name, creases in (("smart_fill_angle_low.png", [0.40, 0.70]), ("smart_fill_angle_high.png", [0.70])):
        done = flood_frames(SIZE, creases=creases, steps=22, hold=0)[-1]
        save_png(name, done)

    # Whole model: every face painted, and every face cleared.
    whole = tri_mesh(9, jitter=0.55, seed=2)
    save_png("select_all.png", raster(lambda d, sz: draw_mesh(d, sz, whole, np.ones(len(whole), bool)), SIZE))
    save_png("erase_all.png", raster(lambda d, sz: draw_mesh(d, sz, whole, np.zeros(len(whole), bool)), SIZE))

    # The views the panel offers over the painted area.
    relief = shade(motif(3))
    save_png("view_fast.png", posterize(relief, 3))
    # Checker and Distortion are about how the texture is laid out, not about the texture, so they go
    # on a plain swell where the checker itself is what there is to look at.
    plain = shade(0.25 + 0.5 * dome())
    save_png("view_checker.png", with_checker(plain, squares=7, strength=0.40))
    save_png("view_distortion.png", with_checker(plain, squares=7, strength=0.40, warp=True))
    save_png("view_wireframe.png", raster(lambda d, sz: draw_mesh_lines(d, sz, tri_mesh(9, jitter=0.55, seed=2)),
                                          SIZE, background=relief))

    # The projections: one ball, the checker put on it five different ways.
    for kind in ("cube", "cylinder", "sphere", "view"):
        save_png("map_%s.png" % kind, projected_sphere(kind))
    save_png("map_unwrap.png", uv_islands())

    # Refinement: where the triangles go, and how many.
    density_feature = sampler(feature_density())
    coarse = tri_mesh(3)
    save_png("adaptive_off.png", raster(lambda d, sz: draw_mesh(d, sz, refine(coarse, lambda x, y: 1.0, 2)), SIZE))
    save_png("adaptive_on.png", raster(lambda d, sz: draw_mesh(d, sz, refine(coarse, density_feature, 3)), SIZE))

    disc = sampler(painted_area(192, radius=0.34))
    painted_mesh = refine(coarse, disc, 3)
    save_png("painted_only_on.png",
             raster(lambda d, sz: draw_mesh(d, sz, painted_mesh,
                                            np.array([disc(*np.mean(t, axis=0)) > 0.5 for t in painted_mesh])), SIZE))
    everywhere = refine(coarse, lambda x, y: 1.0, 3)
    save_png("painted_only_off.png",
             raster(lambda d, sz: draw_mesh(d, sz, everywhere,
                                            np.array([disc(*np.mean(t, axis=0)) > 0.5 for t in everywhere])), SIZE))

    # Subdivide: every triangle into four, a step at a time.
    level = tri_mesh(2)
    frames = []
    for _ in range(4):
        frames.append(raster(lambda d, sz, m=level: draw_mesh(d, sz, m), GIF_SIZE))
        level = [out for t in level for out in split4(t)]
    save_gif("subdivide.gif", frames, delay_ms=650)

    # Remesh: the model's own uneven triangulation, rebuilt at one size.
    save_png("remesh_before.png", raster(lambda d, sz: draw_mesh(d, sz, irregular_mesh()), SIZE))
    save_png("remesh_after.png", raster(lambda d, sz: draw_mesh(d, sz, tri_mesh(8, jitter=0.35, seed=9)), SIZE))

    # Hard edges: a fold the remesh runs across, against one it lays its edges along.
    crease = ((0.06, 0.84), (0.94, 0.20))
    across = tri_mesh(5, jitter=0.45, seed=6)
    save_png("preserve_edges_off.png",
             raster(lambda d, sz: (draw_mesh(d, sz, across), draw_line(d, sz, crease)), SIZE))
    save_png("preserve_edges_on.png",
             raster(lambda d, sz: (draw_mesh(d, sz, snap_to_crease(across, crease, 0.13)),
                                   draw_line(d, sz, crease)), SIZE))

    # Thin triangles, and the same mesh without them.
    save_png("collapse_slivers_off.png", raster(lambda d, sz: draw_mesh(d, sz, sliver_mesh()), SIZE))
    save_png("collapse_slivers_on.png", raster(lambda d, sz: draw_mesh(d, sz, tri_mesh(7, jitter=0.4, seed=5)), SIZE))

    # Smoothing the geometry after the texture is in it: the steps relax out of the surface.
    stepped_profile = np.floor(profile_wave(GIF_SIZE, periods=2.0) * 5.0) / 4.0
    frames = []
    for t in sweep:
        h = stepped_profile.copy()
        for _ in range(int(round(t * 6))):
            h = blur(h[None, :], 2)[0]
        frames.append(raster(lambda d, sz, hh=h: draw_profile(d, sz, hh), GIF_SIZE, background=(0.11, 0.12, 0.13)))
    save_gif("geometry_smoothing.gif", frames, delay_ms=100)

    # Mixing, how many mixes, and denoise - all about which filament a spot ends up as, so all flat.
    save_png("mix_off.png", levels_image(4))
    save_png("mix_on.png", levels_image(4, dither=True))
    save_png("mixed_colors_few.png", levels_image(3, dither=True))
    save_png("mixed_colors_many.png", levels_image(8, dither=True))
    save_png("denoise_off.png", levels_image(5, dither=True, speckle=0.05))
    save_png("denoise_on.png", levels_image(5, dither=True))


if __name__ == "__main__":
    main()
