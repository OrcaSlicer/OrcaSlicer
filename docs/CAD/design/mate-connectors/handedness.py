"""Give the bear a handedness mark that survives rasterisation — snaporca-wi3z, Tommaso's call 2.

The study showed the left/right cue lives in sub-millimetre corner radii and is therefore invisible
at glyph size: one pixel is 2.6 mm at 32 px. Roll and verse are safe; handedness is not.

THE MEASURE IS THE QUESTION ITSELF. Render the glyph, render its mirror image, and count how many
pixels differ. If a human is to tell left from right, the two must differ on screen; a candidate
that scores near zero is invisible however elegant it looks in CAD. Reported as a percentage of the
glyph's own lit area, so the sizes are comparable.
"""
import json, math, os
from PIL import Image, ImageDraw, ImageChops

HERE = os.path.dirname(os.path.abspath(__file__))
D = json.load(open(os.path.join(HERE, "bear_outline.json")))
def unit(pts):
    p = [(x, -z) for x, z in pts]
    return p
outer = unit(D["outer"]); holes = [unit(h["pts"]) for h in D["holes"]]
ALL = outer + [p for h in holes for p in h]
xs=[p[0] for p in ALL]; ys=[p[1] for p in ALL]
CX,CY = (min(xs)+max(xs))/2,(min(ys)+max(ys))/2
SPAN  = max(max(xs)-min(xs), max(ys)-min(ys))
U = lambda pts: [((x-CX)/SPAN,(y-CY)/SPAN) for x,y in pts]
OUT = U(outer)
EYES = [U(h) for h,m in zip(holes, D["holes"]) if m["d"] < 20]
MUZ  = U([h for h,m in zip(holes, D["holes"]) if m["d"] >= 20][0])

def rdp(pts, eps):
    if len(pts) < 3: return pts
    ax,ay=pts[0]; bx,by=pts[-1]; dx,dy=bx-ax,by-ay
    n=math.hypot(dx,dy); best,bi=-1.0,0
    for i in range(1,len(pts)-1):
        px,py=pts[i]
        d=abs(dx*(ay-py)-(ax-px)*dy)/n if n>1e-12 else math.hypot(px-ax,py-ay)
        if d>best: best,bi=d,i
    if best<=eps: return [pts[0],pts[-1]]
    return rdp(pts[:bi+1],eps)[:-1]+rdp(pts[bi:],eps)
def simp(pts,eps):
    r=rdp(pts+[pts[0]],eps); return r[:-1]

BASE = simp(OUT, .030)                     # the 22-vertex outline the study settled on
def centroid(p): return (sum(q[0] for q in p)/len(p), sum(q[1] for q in p)/len(p))
def circ(cx,cy,r,n=16): return [(cx+r*math.cos(2*math.pi*i/n), cy+r*math.sin(2*math.pi*i/n)) for i in range(n)]
EYE_D = []
for e in EYES:
    c=centroid(e); r=(max(p[0] for p in e)-min(p[0] for p in e))/2
    EYE_D.append((c[0],c[1],r))
EYE_D.sort()                                # [0] = left (x<0), [1] = right

TOP = max(p[1] for p in BASE)
H   = TOP - min(p[1] for p in BASE)
def ear_tip(sign):
    cands=[p for p in BASE if p[1] > TOP-0.18*H and (p[0]*sign) > 0]
    return max(cands, key=lambda p: p[0]*sign) if cands else None
LT, RT = ear_tip(-1), ear_tip(+1)

def notch(tip, sign, k=0.085):
    """A wedge bitten out of one ear — background-filled, exactly how the eyes are already drawn."""
    x,y = tip
    return [(x, y+0.02), (x - sign*k, y - k*0.55), (x + sign*k*0.15, y - k*1.05)]

CANDS = {
 "H0 none":        dict(cuts=[],                       eyes=EYE_D),
 "H1 notch R ear": dict(cuts=[notch(RT, +1)],          eyes=EYE_D),
 "H2 notch both":  dict(cuts=[notch(RT, +1), notch(LT, -1, 0.045)], eyes=EYE_D),
 "H3 cheek dot":   dict(cuts=[circ(EYE_D[1][0]+0.085, EYE_D[1][1]-0.10, 0.038)], eyes=EYE_D),
 "H4 uneven eyes": dict(cuts=[],  eyes=[EYE_D[0], (EYE_D[1][0], EYE_D[1][1], EYE_D[1][2]*1.55)]),
}

def render(c, px, ss=8, mirror=False):
    S=px*ss; img=Image.new("L",(S,S),0); d=ImageDraw.Draw(img)
    m = lambda p: (S/2 + (-p[0] if mirror else p[0])*S*0.92, S/2 - p[1]*S*0.92)
    d.polygon([m(p) for p in BASE], fill=255)
    d.polygon([m(p) for p in MUZ], fill=0)
    for cx,cy,r in c["eyes"]:
        a=m((cx-r,cy+r)); b=m((cx+r,cy-r))
        d.ellipse([min(a[0],b[0]), min(a[1],b[1]), max(a[0],b[0]), max(a[1],b[1])], fill=0)
    for cut in c["cuts"]:
        d.polygon([m(p) for p in cut], fill=0)
    return img.resize((px,px), Image.LANCZOS)

SIZES=[22,32,48]
print(f"{'candidate':16} " + "  ".join(f"{s}px" for s in SIZES) + "     (pixels differing from own mirror, % of lit area)")
print("-"*84)
scores={}
for name,c in CANDS.items():
    row=[]
    for px in SIZES:
        a=render(c,px); b=render(c,px,mirror=True)
        diff=ImageChops.difference(a,b)
        nd=sum(1 for v in diff.getdata() if v>40)
        lit=sum(1 for v in a.getdata() if v>40) or 1
        row.append(100.0*nd/lit)
    scores[name]=row
    print(f"{name:16} " + "  ".join(f"{v:5.1f}" for v in row))

pad,cell=8,58
W=pad+len(SIZES)*2*cell+pad; Hh=pad+len(CANDS)*cell+pad
sheet=Image.new("RGB",(W,Hh),(24,27,32))
for r,(name,c) in enumerate(CANDS.items()):
    for mi,mir in enumerate((False,True)):
        for si,px in enumerate(SIZES):
            g=render(c,px,mirror=mir)
            tile=Image.new("RGB",(px,px),(24,27,32))
            tile.paste(Image.new("RGB",(px,px),(237,168,23)),(0,0),g)
            x=pad+(mi*len(SIZES)+si)*cell+(cell-px)//2
            y=pad+r*cell+(cell-px)//2
            sheet.paste(tile,(x,y))
sheet.resize((W*2,Hh*2), Image.NEAREST).save(os.path.join(HERE,"handedness-sheet.png"))
print("\nleft block = as drawn, right block = mirrored.  rows: " + ", ".join(CANDS))
print("WROTE handedness-sheet.png")
