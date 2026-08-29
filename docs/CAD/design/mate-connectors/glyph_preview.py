"""Render the SIMPLIFIED glyph exactly as render_mate_face() draws it — snaporca-x0kd.

This is the panel the study was missing. simplify_study.py measured a FLAT outline and
relief_sheet.py measured the FULL 1508-facet part; neither showed the simplified glyph WITH its
relief, which is what the code actually draws and the only thing that answers "is the snout still
protruding". Same facet list, same painter order, same camera-fixed lambert as the C++.
"""
import math, os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
T = open(os.path.join(HERE, "bear_glyph_table.h")).read()
def grab(name, n):
    body = T.split(name + "[] = {")[1].split("};")[0]
    body = "\n".join(l.split("//")[0] for l in body.splitlines())
    out = []
    for tok in body.replace("\n", " ").split("},"):
        tok = tok.strip().lstrip("{").strip()
        if not tok: continue
        v = [float(x) for x in tok.replace("{", "").split(",")[:n]]
        if len(v) == n: out.append(tuple(v))
    return out
OUT   = grab("kBearOutline", 2)
CHIN  = grab("kBearChin", 2)          # NB: this table entry is the CHIN BAR, not the snout
MARKS = grab("kBearMarks", 3)
CREST = grab("kBearCrest", 3)
SBASE = grab("kBearSnoutBase", 2)
PLATE = float(T.split("kBearPlateZ = ")[1].split(";")[0])


def facets():
    F = []
    n = len(OUT)
    for i in range(n):                                   # plate sides -> the grazing silhouette
        a, b = OUT[i], OUT[(i+1) % n]
        F.append(([(a[0],a[1],0.0),(b[0],b[1],0.0),(b[0],b[1],PLATE),(a[0],a[1],PLATE)], "body", True))
    F.append(([(x,y,PLATE) for x,y in OUT], "body", True))          # plate top
    zm = PLATE + 0.004
    for cx,cy,r in MARKS:                                            # eyes + cheek dot
        F.append(([(cx+r*math.cos(2*math.pi*i/12), cy+r*math.sin(2*math.pi*i/12), zm) for i in range(12)], "mark", False))
    F.append(([(x,y,zm) for x,y in CHIN], "mark", False))            # chin bar
    A, B = CREST                                                     # THE MUZZLE: base quad + crest
    nl=(SBASE[0][0],SBASE[0][1],PLATE); nr=(SBASE[1][0],SBASE[1][1],PLATE)
    tr=(SBASE[2][0],SBASE[2][1],PLATE); tl=(SBASE[3][0],SBASE[3][1],PLATE)
    F += [([nl,tl,B,A],"body",True),   # left flank
          ([nr,A,B,tr],"body",True),   # right flank
          ([nl,A,nr],"body",True),     # nose cap, sloping because the base overhangs the crest
          ([tr,B,tl],"body",True)]     # tail cap
    return F
FACETS = facets()

BODY=(0.42,0.46,0.52); MARK=(0.126,0.138,0.156)
def render(px, elev_deg, ss=8):
    S=px*ss; a=math.radians(elev_deg); ca,sa=math.cos(a),math.sin(a)
    # camera orbits down; the connector's +Z (relief) tips toward the horizon
    xf=lambda p:(p[0], p[1]*sa + p[2]*ca, -p[1]*ca + p[2]*sa)
    light=(-0.70,0.30,0.45)
    img=Image.new("RGB",(S,S),(24,27,32)); d=ImageDraw.Draw(img)
    tris=[]
    for pts,kind,shade in FACETS:
        q=[xf(p) for p in pts]
        tris.append((sum(v[2] for v in q)/len(q), q, kind, shade))
    tris.sort(key=lambda t:t[0])                       # far first
    for _,q,kind,shade in tris:
        (x0,y0,z0),(x1,y1,z1),(x2,y2,z2)=q[0],q[1],q[2]
        ux,uy,uz=x1-x0,y1-y0,z1-z0; vx,vy,vz=x2-x0,y2-y0,z2-z0
        nx,ny,nz=uy*vz-uz*vy, uz*vx-ux*vz, ux*vy-uy*vx
        nn=math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0
        nx,ny,nz=nx/nn,ny/nn,nz/nn
        if nz<0: nx,ny,nz=-nx,-ny,-nz
        base=BODY if kind=="body" else MARK
        k=(0.42+0.58*max(0.0,nx*light[0]+ny*light[1]+nz*light[2])) if shade else 1.0
        col=tuple(min(255,int(255*c*k)) for c in base)
        d.polygon([(S/2+p[0]*S*0.92, S/2-p[1]*S*0.92) for p in q], fill=col)
    return img.resize((px,px), Image.LANCZOS)

SIZES=[22,32,48]; ELEVS=[(90,"flat on"),(47,"47"),(16,"16"),(6,"6")]
pad,cell=8,58
W=pad+len(SIZES)*len(ELEVS)*cell+pad; H=pad+cell+pad
sheet=Image.new("RGB",(W,H),(24,27,32))
for ci,(e,_) in enumerate(ELEVS):
    for si,px in enumerate(SIZES):
        g=render(px,e)
        sheet.paste(g, (pad+(ci*len(SIZES)+si)*cell+(cell-px)//2, pad+(cell-px)//2))
sheet.resize((W*2,H*2), Image.NEAREST).save(os.path.join(HERE,"glyph-preview.png"))

# how much of the glyph is the snout: render with and without the tent and diff
def render_no_tent(px, elev):
    global FACETS
    keep=FACETS; FACETS=FACETS[:-4]
    try: return render(px, elev)
    finally: FACETS=keep
print(f"{'elev':>8} {'lit px@32':>10} {'snout px':>9} {'snout share':>12}")
for e,_ in ELEVS:
    a=render(32,e); b=render_no_tent(32,e)
    la=sum(1 for p in a.get_flattened_data() if p!=(24,27,32))
    diff=sum(1 for p,q in zip(a.get_flattened_data(), b.get_flattened_data()) if p!=q)
    print(f"{e:>8} {la:>10} {diff:>9} {100.0*diff/max(1,la):>11.1f}%")
print("WROTE glyph-preview.png")
