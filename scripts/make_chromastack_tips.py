#!/usr/bin/env python3
"""Draw the ChromaStack tooltip illustrations.

The panel's other illustrations are renders of a model. These cannot be: what they show is what a
stack of translucent filament reaches, which is a property of the colour model rather than of any
particular print. So they are computed from that model - the same Beer-Lambert absorption
chroma_palette() uses, with the same constants - and this script is kept so they can be drawn again
if those constants ever change. A picture that disagrees with the code is worse than no picture.

    python3 scripts/make_chromastack_tips.py

Writes into resources/images/texture_displacement/, matching the house format: 256x256 PNGs and
192x192 GIFs on the same near-black ground. A frame that is a step of a smooth sweep holds for the
house 100 ms; one that is a state to be read holds for as long as reading it takes.
"""

import itertools
import math
import os
from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "images", "texture_displacement")

BG = (7, 7, 7)
DIM = (120, 124, 128)
PNG = 256
GIF = 192
FRAMES = 16

# CHROMA_TD_OPAQUE_FRACTION / OPAQUE_EFOLDS, from libslic3r/ChromaStack.{hpp,cpp}.
TD_OPAQUE_FRACTION = 0.1
OPAQUE_EFOLDS = 3.0
# CHROMA_DEFAULT_RELIEF_LAYERS, so a stack drawn here is as tall as the one the panel starts with.
RELIEF_LAYERS = 24


def s2l(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def l2s(v):
    v = max(0.0, min(1.0, v))
    return round(255 * (v * 12.92 if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055))


def lab(rgb):
    """CIELAB, D65. The space chroma_suggest_colors() clusters in, because a distance in it is about
    as far apart as two colours look."""
    r, g, b = (s2l(c) for c in rgb)
    x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.95047
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.08883

    def f(t):
        return t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116

    fx, fy, fz = f(x), f(y), f(z)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


# A spool rack to choose from: the colours filament is actually sold in, with a transmission
# distance apiece. A pale filament lets more light through, which is roughly what these say.
RACK = [((236, 236, 230), 5.0), ((247, 199, 60), 5.0), ((224, 110, 44), 4.5), ((190, 48, 44), 4.0),
        ((170, 60, 130), 4.0), ((60, 140, 90), 4.0), ((60, 160, 190), 4.5), ((40, 90, 160), 4.0),
        ((120, 122, 126), 3.5), ((20, 20, 24), 3.0)]


def pick_filaments(img, k):
    """The k filaments off the rack that print this picture best, in the order they stack in.

    The shape of chroma_pick_colors(): try the rack, keep what comes closest. Greedy - the best pair,
    then whichever single filament helps most, and again - so every frame of the animation contains
    the one before it, and the picture can only get closer as the count rises. The panel searches
    orderings too; here the order is fixed, palest at the bottom, which is where the search lands
    anyway because every band darkens what is under it.
    """
    small = img.resize((48, 36))
    want = [lab(c) for c in small.getdata()]

    def error(chosen):
        bands = [(rgb, td, max(2, RELIEF_LAYERS // len(chosen))) for rgb, td in order(chosen)]
        reach = [lab(c) for c in palette(bands, 0.08)]
        return sum(min(sum((w[t] - r[t]) ** 2 for t in range(3)) for r in reach) for w in want)

    def order(chosen):
        return sorted(chosen, key=lambda f: -lab(f[0])[0])

    best = min(((a, b) for n, a in enumerate(RACK) for b in RACK[n + 1:]), key=lambda pair: error(list(pair)))
    chosen = list(best)
    while len(chosen) < k:
        chosen.append(min((f for f in RACK if f not in chosen), key=lambda f: error(chosen + [f])))
    return order(chosen)


def transmittance(td, layer_height):
    return math.exp(-layer_height / (TD_OPAQUE_FRACTION * td / OPAQUE_EFOLDS))


def palette(bands, layer_height):
    """The colour a column of k layers shows, for every k. bands: [(rgb, td, layers), ...] bottom-up."""
    per = []
    for rgb, td, n in bands:
        per += [(rgb, td)] * n
    cur = [s2l(c) for c in per[0][0]]
    out = [tuple(l2s(v) for v in cur)]
    for rgb, td in per[1:]:
        t = transmittance(td, layer_height)
        lin = [s2l(c) for c in rgb]
        cur = [(1 - t) * lin[i] + t * cur[i] for i in range(3)]
        out.append(tuple(l2s(v) for v in cur))
    return out


def nearest(color, choices):
    return min(choices, key=lambda c: sum((c[i] - color[i]) ** 2 for i in range(3)))


def ramp_strip(draw, colors, x0, y0, w, h):
    for i, c in enumerate(colors):
        draw.rectangle([x0 + w * i // len(colors), y0, x0 + w * (i + 1) // len(colors) - 1, y0 + h], fill=c)


def test_image(w, h):
    """A smooth two-way ramp: warm to cool across, light to dark down. A stack has to reach all of it."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            u, v = x / (w - 1), y / (h - 1)
            px[x, y] = (int(255 * (0.95 - 0.55 * v) * (0.35 + 0.65 * (1 - u))),
                        int(255 * (0.85 - 0.6 * v) * (0.45 + 0.3 * u)),
                        int(255 * (0.8 - 0.65 * v) * (0.25 + 0.75 * u)))
    return img


def remap(src, choices):
    """src with every pixel taken to the nearest colour the stack reaches."""
    out = Image.new("RGB", src.size)
    sp, op = src.load(), out.load()
    seen = {}
    for y in range(src.size[1]):
        for x in range(src.size[0]):
            c = sp[x, y]
            got = seen.get(c)
            if got is None:
                got = seen[c] = nearest(c, choices)
            op[x, y] = got
    return out


def save_gif(name, frames, hold=100):
    frames = [f.convert("P", palette=Image.ADAPTIVE, colors=32) for f in frames]
    frames[0].save(os.path.join(OUT, name), save_all=True, append_images=frames[1:], duration=hold, loop=0,
                   optimize=True)


def dither_image(w, h, on):
    """A grey ramp taken to five levels, with and without the error carried into the next pixel."""
    src = test_image(w, h).convert("L").convert("RGB")
    levels = [(v, v, v) for v in (18, 70, 125, 180, 240)]
    out = Image.new("RGB", src.size)
    sp, op = src.load(), out.load()
    err = [[0.0] * w for _ in range(h + 1)]
    for y in range(h):
        for x in range(w):
            want = sp[x, y][0] + (err[y][x] if on else 0.0)
            got = min(levels, key=lambda c: abs(c[0] - want))
            op[x, y] = got
            if on:
                e = want - got[0]
                if x + 1 < w:
                    err[y][x + 1] += e * 7 / 16
                if y + 1 <= h:
                    if x:
                        err[y + 1][x - 1] += e * 3 / 16
                    err[y + 1][x] += e * 5 / 16
                    if x + 1 < w:
                        err[y + 1][x + 1] += e * 1 / 16
    return out


def colors_gif(name):
    """The picture as 2 filaments can print it, then 3, and so on up to the 8 the slider offers.

    The same picture every frame, and the best n colours for it each time: what changes is only how
    much of it the stack can reach. Each filament after the second buys a direction the ones under it
    could not go, which is what the slider is choosing between.
    """
    src = test_image(136, 102)
    frames = []
    for n in range(2, 9):
        each = max(2, RELIEF_LAYERS // n)
        chosen = pick_filaments(src, n)
        bands = [(rgb, td, each) for rgb, td in chosen]
        pal = palette(bands, 0.08)
        img = Image.new("RGB", (GIF, GIF), BG)
        draw = ImageDraw.Draw(img)
        img.paste(remap(src, pal), (28, 22))
        draw.rectangle([27, 21, 28 + 136, 22 + 102], outline=(48, 50, 54))
        for i, (rgb, _td) in enumerate(chosen):
            x = 28 + i * 17
            draw.rectangle([x, 136, x + 12, 148], fill=rgb, outline=(60, 62, 66))
        draw.text((28, 164), "%d filaments" % n, fill=DIM)
        frames.append(img)
    save_gif(name, frames, 350)


def order_gif(name):
    """The same three filaments in each of the six orders they can be stacked in.

    Left is the stack, bottom-up, in the filament each layer prints in; right is what a column that
    tall shows from above. Nothing changes between frames but the order, and every colour on the
    right changes with it - which is why the picker searches orderings rather than just colours.
    """
    trio = [((232, 232, 226), 6.0), ((240, 170, 40), 5.0), ((48, 96, 190), 4.0)]
    each = 5
    rows = 3 * each
    frames = []
    for o in itertools.permutations(range(3)):
        bands = [(trio[i][0], trio[i][1], each) for i in o]
        pal = palette(bands, 0.08)
        per = []
        for rgb, _td, n in bands:
            per += [rgb] * n
        img = Image.new("RGB", (GIF, GIF), BG)
        draw = ImageDraw.Draw(img)
        draw.text((34, 20), "stack", fill=DIM)
        draw.text((102, 20), "shows", fill=DIM)
        for i in range(rows):
            y = 154 - i * 8
            draw.rectangle([34, y, 94, y + 6], fill=per[i])
            draw.rectangle([102, y, 162, y + 6], fill=pal[i])
        frames.append(img)
    save_gif(name, frames, 400)


def dither_gif(name):
    """The same ramp with dithering off, then on. A flip rather than a pair, because what is worth
    seeing is the change: the bands move and the stipple does not."""
    frames = []
    for on in (False, True):
        img = Image.new("RGB", (GIF, GIF), BG)
        draw = ImageDraw.Draw(img)
        img.paste(dither_image(152, 132, on), (20, 16))
        draw.rectangle([19, 15, 20 + 152, 16 + 132], outline=(48, 50, 54))
        draw.text((20, 162), "dither on" if on else "dither off", fill=DIM)
        frames.append(img)
    save_gif(name, frames, 900)


def trim_png(name):
    img = Image.new("RGB", (PNG, PNG), BG)
    draw = ImageDraw.Draw(img)
    # A pale band twelve layers long over a dark one. Light over dark is the case trimming is for: it
    # saturates within about five layers and every layer after that shows what the one below showed.
    # (Dark over light goes on paying for far longer, which is why Trim measures the palette rather
    # than the filament's TD - see chroma_trim_bands().)
    bands = [((20, 20, 24), 4.0, 1), ((236, 236, 230), 4.0, 12)]
    pal = palette(bands, 0.08)[1:]
    y = 28
    for i, c in enumerate(pal):
        draw.rectangle([56, y, 200, y + 14], fill=c)
        same = i > 0 and max(abs(c[k] - pal[i - 1][k]) for k in range(3)) <= 2
        draw.text((24, y + 1), str(i + 1), fill=DIM)
        if same:
            draw.line([210, y + 7, 232, y + 7], fill=(150, 110, 50), width=2)
        y += 17
    img.save(os.path.join(OUT, name))


def td_gif(name):
    """The same two filaments as their transmission distance rises: steps become a fade."""
    frames = []
    for f in range(FRAMES):
        td = 1.0 + 7.0 * (f / (FRAMES - 1))
        img = Image.new("RGB", (GIF, GIF), BG)
        draw = ImageDraw.Draw(img)
        pal = palette([((236, 236, 230), 1.0, 1), ((20, 20, 24), td, 14)], 0.08)[1:]  # above the base
        y = 24
        for c in pal:
            draw.rectangle([44, y, 148, y + 8], fill=c)
            y += 10
        draw.text((44, 170), "TD %.1f" % td, fill=DIM)
        frames.append(img)
    save_gif(name, frames)


def layers_gif(name):
    """A column built up one layer at a time, and the colour it shows from above as it grows."""
    # Chosen to stay visible against the ground: a band the colour of the background reads as a column
    # that has stopped growing rather than as a layer of dark filament.
    bands = [((232, 232, 226), 6.0, 5), ((240, 170, 40), 5.0, 5), ((48, 96, 190), 4.0, 5)]
    pal = palette(bands, 0.08)
    per = []
    for rgb, _td, n in bands:
        per += [rgb] * n
    frames = []
    for f in range(FRAMES):
        k = min(len(per), 1 + int(f * len(per) / (FRAMES - 1)))
        img = Image.new("RGB", (GIF, GIF), BG)
        draw = ImageDraw.Draw(img)
        # The column's full extent, so a growing stack reads as growing rather than as moving.
        draw.rectangle([35, 150 - len(per) * 9 + 8, 109, 158], outline=(40, 42, 46))
        for i in range(k):
            y = 150 - i * 9
            draw.rectangle([36, y, 108, y + 7], fill=per[i])
        # What the top of that column shows: the whole of it, seen from above.
        draw.rectangle([128, 44, 164, 80], fill=pal[k - 1])
        draw.rectangle([127, 43, 165, 81], outline=(60, 62, 66))
        draw.text([120, 28], "from above", fill=DIM)
        draw.text([36, 162], "%d layers" % k, fill=DIM)
        frames.append(img)
    save_gif(name, frames)


def main():
    trim_png("chroma_trim.png")
    colors_gif("chroma_colors.gif")
    order_gif("chroma_order.gif")
    dither_gif("chroma_dither.gif")
    td_gif("chroma_td.gif")
    layers_gif("chroma_layers.gif")
    print("wrote 6 illustrations into", os.path.normpath(OUT))


if __name__ == "__main__":
    main()
