"""Reduce the bear face to the fewest marks that still read at glyph size — snaporca-wi3z.

Geometry comes from bear_outline.json, which extract_outline.py pulled off the supplied male
B-rep's back plate: the outer wire IS the silhouette, the inner wires are the two eyes and the
muzzle opening. Nothing here is traced by eye.

The glyph is drawn IN the connector's plane, so a grazing view foreshortens it along one axis by
sin(elevation) — exactly what collapsed the disc's roll quadrant to 3 pixels at 10 deg. Every
candidate is therefore rendered at three elevations as well as three pixel sizes.
"""
import json, math, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
D = json.load(open(os.path.join(HERE, "bear_outline.json")))

def norm(pts):
    """Part frame (X right, Z down-negative) -> glyph frame (x right, y up), centred, unit height."""
    p = [(x, -z) for x, z in pts]
    return p

outer = norm(D["outer"])
holes = [norm(h["pts"]) for h in D["holes"]]
# the two Ø9.8 wires are the eyes; the wide one is the muzzle
eyes   = [h for h, meta in zip(holes, D["holes"]) if meta["d"] < 20]
muzzle = [h for h, meta in zip(holes, D["holes"]) if meta["d"] >= 20]

ALL = outer + [p for h in holes for p in h]
xs = [p[0] for p in ALL]; ys = [p[1] for p in ALL]
CX, CY = (min(xs)+max(xs))/2, (min(ys)+max(ys))/2
SPAN = max(max(xs)-min(xs), max(ys)-min(ys))
def to_unit(pts): return [((x-CX)/SPAN, (y-CY)/SPAN) for x, y in pts]

def rdp(pts, eps):
    """Douglas-Peucker. Vertex count is the honest measure of 'how simplified'."""
    if len(pts) < 3: return pts
    ax, ay = pts[0]; bx, by = pts[-1]
    dx, dy = bx-ax, by-ay
    n = math.hypot(dx, dy)
    best, bi = -1.0, 0
    for i in range(1, len(pts)-1):
        px, py = pts[i]
        d = abs(dx*(ay-py) - (ax-px)*dy)/n if n > 1e-12 else math.hypot(px-ax, py-ay)
        if d > best: best, bi = d, i
    if best <= eps:
        return [pts[0], pts[-1]]
    return rdp(pts[:bi+1], eps)[:-1] + rdp(pts[bi:], eps)

def simp_closed(pts, eps):
    r = rdp(pts + [pts[0]], eps)
    return r[:-1]

def centroid(pts):
    return (sum(p[0] for p in pts)/len(pts), sum(p[1] for p in pts)/len(pts))

U_OUT = to_unit(outer)
U_EYE = [to_unit(e) for e in eyes]
U_MUZ = [to_unit(m) for m in muzzle]

def eye_dots(scale=1.0):
    out = []
    for e in U_EYE:
        cx, cy = centroid(e)
        r = max(max(p[0] for p in e)-min(p[0] for p in e),
                max(p[1] for p in e)-min(p[1] for p in e))/2*scale
        out.append((cx, cy, r))
    return out

def muzzle_tri():
    """The muzzle reduced to one filled triangle: its two lower corners and its apex."""
    m = U_MUZ[0]
    lo = min(p[1] for p in m); hi = max(p[1] for p in m)
    bottom = [p for p in m if p[1] < lo + 0.06*(hi-lo)]
    apex   = max(m, key=lambda p: p[1])
    return [min(bottom), max(bottom), apex]

CANDIDATES = {
 "C0 full":        dict(out=U_OUT,                     eyes=eye_dots(), muz=U_MUZ[0]),
 "C1 eps .004":    dict(out=simp_closed(U_OUT, .004),  eyes=eye_dots(), muz=simp_closed(U_MUZ[0], .004)),
 "C2 eps .012":    dict(out=simp_closed(U_OUT, .012),  eyes=eye_dots(), muz=muzzle_tri()),
 "C3 eps .030":    dict(out=simp_closed(U_OUT, .030),  eyes=eye_dots(1.15), muz=muzzle_tri()),
 "C4 no eyes":     dict(out=simp_closed(U_OUT, .012),  eyes=[], muz=muzzle_tri()),
}

def sym_report(pts, tol=0.02):
    """Trivial symmetry group is the property doing the work. If a simplification restores a
    mirror or a 180 deg rotation, that simplification is wrong."""
    def match(tf):
        t = [tf(p) for p in pts]
        hit = 0
        for q in t:
            if min(math.hypot(q[0]-p[0], q[1]-p[1]) for p in pts) <= tol: hit += 1
        return hit, len(pts)
    return {
      "mirror-x": match(lambda p: (-p[0],  p[1])),
      "mirror-y": match(lambda p: ( p[0], -p[1])),
      "rot-180":  match(lambda p: (-p[0], -p[1])),
    }

def render(c, px, elev_deg, supersample=8):
    S = px*supersample
    img = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(img)
    k = math.sin(math.radians(elev_deg))
    def m(p):
        return (S/2 + p[0]*S*0.92, S/2 - p[1]*S*0.92*k)
    d.polygon([m(p) for p in c["out"]], fill=255)
    if c["muz"]: d.polygon([m(p) for p in c["muz"]], fill=0)
    for cx, cy, r in c["eyes"]:
        a = m((cx-r, cy+r)); b = m((cx+r, cy-r))
        d.ellipse([a[0], a[1], b[0], b[1]], fill=0)
    return img.resize((px, px), Image.LANCZOS)

print(f"{'candidate':14} {'verts':>6} {'marks':>6}   symmetry (matched/total, lower is better)")
print("-"*78)
for name, c in CANDIDATES.items():
    s = sym_report(c["out"])
    marks = 1 + (1 if c["muz"] else 0) + len(c["eyes"])
    sym = "  ".join(f"{k} {v[0]}/{v[1]}" for k, v in s.items())
    print(f"{name:14} {len(c['out']):6} {marks:6}   {sym}")

SIZES = [22, 32, 48]
ELEVS = [(90, "flat on"), (47, "47 deg"), (16, "16 deg"), (6, "6 deg")]
pad, cell = 8, 56
W = pad + len(SIZES)*len(ELEVS)*cell + pad
H = pad + len(CANDIDATES)*cell + pad
sheet = Image.new("RGB", (W, H), (24, 27, 32))
for r, (name, c) in enumerate(CANDIDATES.items()):
    for ci, (elev, _) in enumerate(ELEVS):
        for si, px in enumerate(SIZES):
            g = render(c, px, elev)
            tile = Image.new("RGB", (px, px), (24, 27, 32))
            gold = Image.new("RGB", (px, px), (237, 168, 23))
            tile.paste(gold, (0, 0), g)
            x = pad + (ci*len(SIZES)+si)*cell + (cell-px)//2
            y = pad + r*cell + (cell-px)//2
            sheet.paste(tile, (x, y))
sheet = sheet.resize((W*2, H*2), Image.NEAREST)
sheet.save(os.path.join(HERE, "simplify-sheet.png"))
print("\ncolumns: " + " | ".join(f"{e[1]} @ 22/32/48px" for e in ELEVS))
print("rows:    " + ", ".join(CANDIDATES))
print("WROTE simplify-sheet.png")
