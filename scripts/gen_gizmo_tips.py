#!/usr/bin/env python3
"""Draws the pictures the gizmo tooltips show: the toolbar's, and those of each gizmo's panel.

Each module in scripts/gizmo_tips/ other than render.py is one set of pictures. It names its output
folder under resources/images in OUT, lists its pictures in FRAMES (name -> frame count, 0 for a
still) and defines a function of that name for each, taking the frame number and the count and
returning the frame as an RGB array. render.py is the ray marcher they are drawn with.

    python3 scripts/gen_gizmo_tips.py                  # every set
    python3 scripts/gen_gizmo_tips.py cut              # one set
    python3 scripts/gen_gizmo_tips.py cut:plug cut:dowel   # single pictures

Run it after changing a scene; nothing in the build depends on it. Needs numpy and pillow.
"""

import importlib
import os
import pkgutil
import sys
from multiprocessing import Pool

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gizmo_tips  # noqa: E402
from gizmo_tips.render import DELAY_MS, vignette  # noqa: E402

IMAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "images")


def _frame(job):
    module, name, k, n = job
    return getattr(importlib.import_module("gizmo_tips." + module), name)(k, n)


def _sets():
    return [m.name for m in pkgutil.iter_modules(gizmo_tips.__path__) if m.name != "render"]


def main():
    wanted = []  # (module, picture)
    for arg in sys.argv[1:] or _sets():
        module, _, picture = arg.partition(":")
        frames = importlib.import_module("gizmo_tips." + module).FRAMES
        wanted += [(module, picture)] if picture else [(module, p) for p in frames]
    jobs = []
    for module, picture in wanted:
        count = importlib.import_module("gizmo_tips." + module).FRAMES[picture]
        jobs += [(module, picture, k, count) for k in range(max(count, 1))]
    # A quarter of the machine: several sets are often drawn at once.
    with Pool(max(1, (os.cpu_count() or 4) // 4)) as pool:
        images = pool.map(_frame, jobs)

    at = 0
    for module, picture in wanted:
        count = importlib.import_module("gizmo_tips." + module).FRAMES[picture]
        frames = [vignette(f) for f in images[at:at + max(count, 1)]]
        at += max(count, 1)
        out_dir = os.path.join(IMAGES, importlib.import_module("gizmo_tips." + module).OUT)
        os.makedirs(out_dir, exist_ok=True)
        if count == 0:
            path = os.path.join(out_dir, picture + ".png")
            # 256 colours with dithering: indistinguishable from full colour at this size, and less than
            # half the bytes, which counts with several hundred pictures shipped in resources/.
            Image.fromarray(frames[0]).quantize(colors=256, method=Image.Quantize.MEDIANCUT,
                                                dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)
        else:
            path = os.path.join(out_dir, picture + ".gif")
            # One palette for the whole loop, so a colour does not shift from one frame to the next.
            strip = Image.fromarray(np.concatenate(frames, axis=0)).quantize(colors=255, method=Image.MEDIANCUT,
                                                                             dither=Image.Dither.NONE)
            pal = [Image.fromarray(f).quantize(palette=strip, dither=Image.Dither.NONE) for f in frames]
            pal[0].save(path, save_all=True, append_images=pal[1:], duration=DELAY_MS, loop=0, optimize=True, disposal=1)
        print("wrote", os.path.normpath(path), os.path.getsize(path) // 1024, "KB")


if __name__ == "__main__":
    main()
