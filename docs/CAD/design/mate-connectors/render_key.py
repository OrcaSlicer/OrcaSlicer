#!/usr/bin/env python3
"""Flat-shade the faceted ridge key from several camera directions.

The point is not a pretty picture. It is one question: does a low-poly solid, flat-shaded,
let a human read its orientation from an arbitrary viewpoint -- and specifically, is the
view ALONG the ridge ambiguous between front and back, as the geometry suggests it must be
in silhouette?

Flat shading (one normal per facet, no smoothing) is deliberate: it is what the concept
claims to rely on, and it is what a CAD viewport with hard normals actually produces.
"""
import numpy as np
from PIL import Image, ImageDraw

# ---- the key, same numbers as faceted_ridge_key.scad
L, W, tf, H, pr, pf, hf = 12.0, 4.0, 0.45, 4.5, 0.22, 0.62, 0.35
Wf, xr0, xr1, Hf = W * tf, -L / 2 + L * pr, -L / 2 + L * pf, H * hf

V = np.array([(-L/2, -W, 0), (-L/2, W, 0), (L/2, Wf, 0), (L/2, -Wf, 0),
              (xr0, 0, H), (xr1, 0, Hf)], dtype=float)
F = [[0, 1, 2, 3], [0, 4, 1], [0, 3, 5], [0, 5, 4], [1, 4, 5], [1, 5, 2], [3, 2, 5]]

LIGHT = np.array([0.35, -0.5, 0.78])           # a headlight-ish key light
LIGHT /= np.linalg.norm(LIGHT)


def look_at(eye, target, up=(0, 0, 1)):
    f = np.array(target, float) - np.array(eye, float)
    f /= np.linalg.norm(f)
    up = np.array(up, float)
    if abs(np.dot(f, up)) > 0.999:
        up = np.array([0, 1, 0], float)
    r = np.cross(f, up); r /= np.linalg.norm(r)
    u = np.cross(r, f)
    return r, u, f


def render(eye, target, path, size=(620, 460), scale=26.0, label=""):
    r, u, f = look_at(eye, target)
    eye = np.array(eye, float)
    cam = np.stack([r, u, f])                   # world -> camera rows
    P = (V - eye) @ cam.T                       # orthographic: x,y screen, z depth

    w, h = size
    img = Image.new("RGB", size, (238, 240, 243))
    d = ImageDraw.Draw(img)

    def to_px(p):
        return (w / 2 + p[0] * scale, h / 2 - p[1] * scale)

    faces = []
    for face in F:
        pts = V[face]
        n = np.cross(pts[1] - pts[0], pts[2] - pts[0])
        n /= np.linalg.norm(n)
        centre = pts.mean(axis=0)
        if np.dot(n, centre - eye) > 0:          # back-face cull
            continue
        depth = P[face][:, 2].mean()
        lam = max(0.0, float(np.dot(n, LIGHT)))
        shade = 0.22 + 0.78 * lam                # flat: ONE value for the whole facet
        col = tuple(int(255 * shade * c) for c in (0.86, 0.72, 0.35))
        faces.append((depth, [to_px(P[i]) for i in face], col))

    for _, poly, col in sorted(faces, key=lambda t: -t[0]):   # painter's algorithm
        d.polygon(poly, fill=col)

    if label:
        d.rectangle([8, 8, 8 + 9 * len(label), 30], fill=(255, 255, 255))
        d.text((14, 14), label, fill=(20, 20, 20))
    img.save(path)
    return path


if __name__ == "__main__":
    t = (0, 0, H * 0.35)
    views = [
        ((26, -22, 20), "iso: the reference view"),
        ((30,   0,  6), "ALONG +X  (from the FRONT, low end)"),
        ((-30,  0,  6), "ALONG -X  (from the BACK, tall end)"),
        ((0,    0, 34), "ALONG +Z  (straight down the mating axis)"),
        ((2,  -32,  5), "ALONG -Y  (broadside, grazing)"),
    ]
    for i, (eye, lab) in enumerate(views):
        print(render(eye, t, f"rk-{i}.png", label=lab))
