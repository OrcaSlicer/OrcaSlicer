"""Emit the simplified bear as a C++ table for the viewport glyph — snaporca-wi3z.

Everything is normalised to the part's own bounding span and centred, so the renderer scales by
one radius R in screen pixels and nothing here carries millimetres. Emitting rather than
hand-authoring keeps the glyph and the printed part from drifting apart: rerun this and the table
follows the STEP.
"""
import json, math, os
HERE = os.path.dirname(os.path.abspath(__file__))
D = json.load(open(os.path.join(HERE, "bear_outline.json")))

def unit_frame(pts_sets):
    allp=[p for s in pts_sets for p in s]
    xs=[p[0] for p in allp]; ys=[p[1] for p in allp]
    cx,cy=(min(xs)+max(xs))/2,(min(ys)+max(ys))/2
    span=max(max(xs)-min(xs), max(ys)-min(ys))
    return cx,cy,span

outer=[(x,-z) for x,z in D["outer"]]
holes=[[(x,-z) for x,z in h["pts"]] for h in D["holes"]]
CX,CY,SPAN = unit_frame([outer]+holes)
U=lambda pts:[((x-CX)/SPAN,(y-CY)/SPAN) for x,y in pts]
OUT=U(outer)
EYES=[U(h) for h,m in zip(holes,D["holes"]) if m["d"]<20]
MUZ =U([h for h,m in zip(holes,D["holes"]) if m["d"]>=20][0])

def rdp(p,eps):
    if len(p)<3: return p
    ax,ay=p[0]; bx,by=p[-1]; dx,dy=bx-ax,by-ay; n=math.hypot(dx,dy)
    best,bi=-1.0,0
    for i in range(1,len(p)-1):
        px,py=p[i]
        d=abs(dx*(ay-py)-(ax-px)*dy)/n if n>1e-12 else math.hypot(px-ax,py-ay)
        if d>best: best,bi=d,i
    if best<=eps: return [p[0],p[-1]]
    return rdp(p[:bi+1],eps)[:-1]+rdp(p[bi:],eps)
def simp(p,eps):
    r=rdp(p+[p[0]],eps); return r[:-1]

OUT_S = simp(OUT,.030)                      # 22 verts, the size the study settled on
# wind counter-clockwise so the renderer's normals come out facing +Z
def area2(p): return sum(p[i][0]*p[(i+1)%len(p)][1]-p[(i+1)%len(p)][0]*p[i][1] for i in range(len(p)))
if area2(OUT_S) < 0: OUT_S = OUT_S[::-1]

def centroid(p): return (sum(q[0] for q in p)/len(p), sum(q[1] for q in p)/len(p))
E=[]
for e in EYES:
    c=centroid(e); r=(max(p[0] for p in e)-min(p[0] for p in e))/2
    E.append((c[0],c[1],r))
E.sort()

lo=min(p[1] for p in MUZ); hi=max(p[1] for p in MUZ)
bottom=[p for p in MUZ if p[1] < lo+0.06*(hi-lo)]
apex=max(MUZ,key=lambda p:p[1])
TRI=[min(bottom),max(bottom),apex]
if area2(TRI)<0: TRI=TRI[::-1]

# the cheek dot: the handedness mark adopted after the mirror-difference study
DOT=(E[1][0]+0.085, E[1][1]-0.10, 0.038)

# THE MUZZLE. Six facets lifted straight off the mesh -- every facet touching anything above the
# 3 mm plate. Do NOT recompute the base from height*tan(draft): the first version did and produced
# a needle, because the real base OVERHANGS the crest at both ends (0.062 at the nose, 0.034 at the
# tail) and it is that overhang that makes it a tapered wedge instead of a blade.
PLATE = 0.036                      # 3.00 / 83.34
SNOUT_BASE = ((-0.0727, -0.2417), (+0.0630, -0.2417),      # nose end, 0.136 wide
              (+0.0259, +0.1939), (-0.0356, +0.1939))      # tail end, 0.062 wide
CREST = ((-0.0048, -0.1793, 0.2073), (-0.0048, +0.1605, 0.1279))

def fmt(v): return f"{v:+.4f}"
L=[]
L.append(f"// Emitted by doc/design/mate-connectors/emit_glyph_table.py from bear.step — do not hand-edit.")
L.append(f"// Normalised to the part's bounding span and centred: the renderer scales by one radius.")
L.append(f"static const Vec2d kBearOutline[] = {{        // {len(OUT_S)} verts, RDP eps 0.030, CCW")
for i in range(0,len(OUT_S),3):
    row=", ".join(f"{{{fmt(x)}, {fmt(y)}}}" for x,y in OUT_S[i:i+3])
    L.append("    "+row+",")
L.append("};")
L.append(f"static const Vec2d kBearChin[] = {{            // the CHIN BAR, flat. The muzzle is relief — see kBearCrest.")
L.append("    "+", ".join(f"{{{fmt(x)}, {fmt(y)}}}" for x,y in TRI)+",")
L.append("};")
L.append("// {cx, cy, r}: two eyes, then the cheek dot that carries handedness (snaporca-wi3z).")
L.append("static const Vec3d kBearMarks[] = {")
for cx,cy,r in E: L.append(f"    {{{fmt(cx)}, {fmt(cy)}, {fmt(r)}}},")
L.append(f"    {{{fmt(DOT[0])}, {fmt(DOT[1])}, {fmt(DOT[2])}}},")
L.append("};")
L.append("// THE MUZZLE, lifted off the mesh: a tapered wedge, base quad + crest edge, 6 facets.")
L.append("// This is the only feature standing along +Z and the only one still legible edge-on.")
L.append(f"static const double kBearPlateZ = {PLATE:+.4f};")
L.append("static const Vec2d kBearSnoutBase[] = {   // CCW from the nose end")
for x,y in SNOUT_BASE: L.append(f"    {{{fmt(x)}, {fmt(y)}}},")
L.append("};")
L.append("static const Vec3d kBearCrest[] = {       // nose (tall) -> tail (short)")
for x,y,z in CREST: L.append(f"    {{{fmt(x)}, {fmt(y)}, {fmt(z)}}},")
L.append("};")
open(os.path.join(HERE,"bear_glyph_table.h"),"w").write("\n".join(L)+"\n")
print("\n".join(L))
