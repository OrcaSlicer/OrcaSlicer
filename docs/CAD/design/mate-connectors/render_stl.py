#!/usr/bin/env python3
"""Flat-shade an ASCII/binary STL from several directions.

Used to answer one question with a picture instead of an argument: does a RECESSED faceted
pocket read as an oriented feature, or does a concave feature collapse into a dark hole?
"""
import struct
import sys
import numpy as np
from PIL import Image, ImageDraw

LIGHT = np.array([0.35, -0.5, 0.78]); LIGHT /= np.linalg.norm(LIGHT)


def load_stl(path):
    data = open(path, "rb").read()
    if data[:5] == b"solid" and b"facet" in data[:2000]:
        tris, cur = [], []
        for line in data.decode("ascii", "ignore").splitlines():
            s = line.split()
            if s and s[0] == "vertex":
                cur.append([float(x) for x in s[1:4]])
                if len(cur) == 3:
                    tris.append(cur); cur = []
        return np.array(tris, dtype=float)
    n = struct.unpack("<I", data[80:84])[0]
    tris = np.empty((n, 3, 3), dtype=float)
    off = 84
    for i in range(n):
        v = struct.unpack("<12f", data[off:off + 48])
        tris[i] = np.array(v[3:12]).reshape(3, 3)
        off += 50
    return tris


def render(tris, eye, target, path, size=(620, 460), scale=14.0, label=""):
    eye = np.array(eye, float); target = np.array(target, float)
    f = target - eye; f /= np.linalg.norm(f)
    up = np.array([0, 0, 1.0])
    if abs(np.dot(f, up)) > 0.999: up = np.array([0, 1.0, 0])
    r = np.cross(f, up); r /= np.linalg.norm(r)
    u = np.cross(r, f)
    cam = np.stack([r, u, f])

    w, h = size
    img = Image.new("RGB", size, (238, 240, 243)); d = ImageDraw.Draw(img)
    faces = []
    for t in tris:
        n = np.cross(t[1] - t[0], t[2] - t[0])
        ln = np.linalg.norm(n)
        if ln < 1e-12: continue
        n /= ln
        c = t.mean(axis=0)
        if np.dot(n, c - eye) > 0: continue                  # cull back faces
        P = (t - eye) @ cam.T
        lam = max(0.0, float(np.dot(n, LIGHT)))
        shade = 0.20 + 0.80 * lam
        col = tuple(int(255 * shade * ch) for ch in (0.86, 0.72, 0.35))
        poly = [(w / 2 + p[0] * scale, h / 2 - p[1] * scale) for p in P]
        faces.append((P[:, 2].mean(), poly, col))
    for _, poly, col in sorted(faces, key=lambda x: -x[0]):
        d.polygon(poly, fill=col)
    if label:
        d.rectangle([8, 8, 8 + 9 * len(label), 30], fill=(255, 255, 255))
        d.text((14, 14), label, fill=(20, 20, 20))
    img.save(path)


if __name__ == "__main__":
    tris = load_stl(sys.argv[1])
    print("triangles:", len(tris))
    views = [((26, -22, 20), "iso"), ((0, 0, 34), "straight down +Z"),
             ((4, -30, 9), "grazing"), ((-28, -10, 12), "from the tall end")]
    for i, (eye, lab) in enumerate(views):
        render(tris, eye, (0, 0, 0), f"fem-{i}.png", label=f"FEMALE POCKET — {lab}")
        print(f"fem-{i}.png")
