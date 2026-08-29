"""Flat glyph vs 3D relief, at the elevations that killed the disc — snaporca-wi3z.

The flat study collapsed at 16 deg because anything drawn IN the connector's plane foreshortens by
sin(elevation). This renders the SAME bear as its real relief (1508 facets off the supplied male)
with a simple lambert shade, so the silhouette does the work at a grazing angle. Two rows, same
sizes, same elevations, so the comparison is direct.
"""
import json, math, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
M = json.load(open(os.path.join(HERE, "bear_mesh.json")))
V, F = M["v"], M["f"]

# Part frame: face carried by X (right) and Z (down-negative), relief along +Y.
P = [(v[0], -v[2], v[1]) for v in V]           # -> (x right, y up, z out of the face)
xs=[p[0] for p in P]; ys=[p[1] for p in P]; zs=[p[2] for p in P]
CX,CY,CZ = (min(xs)+max(xs))/2, (min(ys)+max(ys))/2, (min(zs)+max(zs))/2
SPAN = max(max(xs)-min(xs), max(ys)-min(ys))
P = [((x-CX)/SPAN, (y-CY)/SPAN, (z-CZ)/SPAN) for x,y,z in P]

def shade(px, elev_deg, supersample=8):
    """Camera orbits down from straight-on (90) to grazing (small). Rotate about the screen x-axis."""
    S = px*supersample
    a = math.radians(elev_deg)
    ca, sa = math.cos(a), math.sin(a)
    # view: rotate the model so the face normal tips away from the camera
    def xf(p):
        x,y,z = p
        return (x, y*sa + z*ca, -y*ca + z*sa)     # third component = depth toward camera
    Q = [xf(p) for p in P]
    img = Image.new("L", (S,S), 0)
    d = ImageDraw.Draw(img)
    order = []
    for tri in F:
        a3 = [Q[i] for i in tri]
        order.append((sum(v[2] for v in a3)/3.0, tri, a3))
    order.sort(key=lambda t: t[0])                 # painter: far first
    light = (-0.35, 0.55, 0.76)
    for _, tri, a3 in order:
        (x0,y0,z0),(x1,y1,z1),(x2,y2,z2) = a3
        ux,uy,uz = x1-x0, y1-y0, z1-z0
        vx,vy,vz = x2-x0, y2-y0, z2-z0
        nx,ny,nz = uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
        n = math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0
        nx,ny,nz = nx/n, ny/n, nz/n
        if nz < 0: nx,ny,nz = -nx,-ny,-nz          # face the camera
        lam = max(0.0, nx*light[0] + ny*light[1] + nz*light[2])
        val = int(70 + 185*lam)
        pts = [(S/2 + x*S*0.92, S/2 - y*S*0.92) for x,y,_ in a3]
        d.polygon(pts, fill=val)
    return img.resize((px,px), Image.LANCZOS)

# flat outline, for the side-by-side
D = json.load(open(os.path.join(HERE, "bear_outline.json")))
def unit(pts):
    p=[(x,-z) for x,z in pts]
    return [((x-CX)/SPAN,(y-CY)/SPAN) for x,y in p]
OUT = unit(D["outer"])
HOLES = [unit(h["pts"]) for h in D["holes"]]

def flat(px, elev_deg, supersample=8):
    S=px*supersample
    img=Image.new("L",(S,S),0); d=ImageDraw.Draw(img)
    k=math.sin(math.radians(elev_deg))
    m=lambda p:(S/2+p[0]*S*0.92, S/2-p[1]*S*0.92*k)
    d.polygon([m(p) for p in OUT], fill=255)
    for h in HOLES: d.polygon([m(p) for p in h], fill=0)
    return img.resize((px,px), Image.LANCZOS)

SIZES=[22,32,48]; ELEVS=[(90,"flat on"),(47,"47"),(16,"16"),(6,"6")]
pad,cell=8,58
W=pad+len(SIZES)*len(ELEVS)*cell+pad; H=pad+2*cell+pad
sheet=Image.new("RGB",(W,H),(24,27,32))
for r,fn in enumerate((flat, shade)):
    for ci,(elev,_) in enumerate(ELEVS):
        for si,px in enumerate(SIZES):
            g=fn(px,elev)
            tile=Image.new("RGB",(px,px),(24,27,32))
            if fn is flat:
                tile.paste(Image.new("RGB",(px,px),(237,168,23)),(0,0),g)
            else:
                gg=g.convert("L")
                tile=Image.merge("RGB",(gg.point(lambda v:min(255,int(v*1.00))),
                                        gg.point(lambda v:int(v*0.71)),
                                        gg.point(lambda v:int(v*0.16))))
            x=pad+(ci*len(SIZES)+si)*cell+(cell-px)//2
            y=pad+r*cell+(cell-px)//2
            sheet.paste(tile,(x,y))
sheet.resize((W*2,H*2), Image.NEAREST).save(os.path.join(HERE,"relief-sheet.png"))

# how much ink survives — the same measure used on the disc glyph
print(f"{'elev':>6} {'flat px@32':>11} {'relief px@32':>13}")
for elev,_ in ELEVS:
    f32=flat(32,elev); s32=shade(32,elev)
    fi=sum(1 for v in f32.getdata() if v>40)
    si=sum(1 for v in s32.getdata() if v>40)
    print(f"{elev:>6} {fi:>11} {si:>13}")
print("WROTE relief-sheet.png")
