"""The muzzle has to READ, not just be present — snaporca-wi3z.

Faithfully scaled, the part's ridge is 11.3 mm on an 83 mm face: 13.6 % of the width. At glyph
size that is a scratch. A glyph is a symbol, not a scale model, so the question is how much
emphasis it takes before the only +Z feature actually reads. Variants, all with the same crest
geometry, differing only in width and colour.
"""
import math, os, importlib.util
from PIL import Image, ImageDraw
spec=importlib.util.spec_from_file_location("gp","glyph_preview.py")
gp=importlib.util.module_from_spec(spec); spec.loader.exec_module(gp)

OUT, CHIN, MARKS, CREST, SBASE, PLATE = gp.OUT, gp.CHIN, gp.MARKS, gp.CREST, gp.SBASE, gp.PLATE
BODY=(0.42,0.46,0.52); MARK=(0.126,0.138,0.156); GOLD=(0.93,0.66,0.09)

def facets(widen=1.0, muzzle_gold=False):
    F=[]; n=len(OUT)
    for i in range(n):
        a,b=OUT[i],OUT[(i+1)%n]
        F.append(([(a[0],a[1],0.0),(b[0],b[1],0.0),(b[0],b[1],PLATE),(a[0],a[1],PLATE)],BODY,True))
    F.append(([(x,y,PLATE) for x,y in OUT],BODY,True))
    zm=PLATE+0.004
    for cx,cy,r in MARKS:
        F.append(([(cx+r*math.cos(2*math.pi*i/12),cy+r*math.sin(2*math.pi*i/12),zm) for i in range(12)],MARK,False))
    F.append(([(x,y,zm) for x,y in CHIN],MARK,False))
    A,B=CREST
    w=lambda p:(p[0]*widen,p[1],PLATE)
    nl,nr,tr,tl=(w(SBASE[0]),w(SBASE[1]),w(SBASE[2]),w(SBASE[3]))
    col = GOLD if muzzle_gold else BODY
    F+=[([nl,tl,B,A],col,True),([nr,A,B,tr],col,True),
        ([nl,A,nr],col,True),  ([tr,B,tl],col,True)]
    return F

def render(F, px, elev, ss=8):
    S=px*ss; a=math.radians(elev); ca,sa=math.cos(a),math.sin(a)
    xf=lambda p:(p[0],p[1]*sa+p[2]*ca,-p[1]*ca+p[2]*sa)
    light=(-0.70,0.30,0.45)
    img=Image.new("RGB",(S,S),(24,27,32)); d=ImageDraw.Draw(img)
    tris=sorted(((sum(v[2] for v in [xf(q) for q in pts])/len(pts),[xf(q) for q in pts],c,sh)
                 for pts,c,sh in F), key=lambda t:t[0])
    for _,q,base,shade in tris:
        (x0,y0,z0),(x1,y1,z1),(x2,y2,z2)=q[0],q[1],q[2]
        ux,uy,uz=x1-x0,y1-y0,z1-z0; vx,vy,vz=x2-x0,y2-y0,z2-z0
        nx,ny,nz=uy*vz-uz*vy,uz*vx-ux*vz,ux*vy-uy*vx
        L=math.sqrt(nx*nx+ny*ny+nz*nz) or 1.0; nx,ny,nz=nx/L,ny/L,nz/L
        if nz<0: nx,ny,nz=-nx,-ny,-nz
        k=(0.42+0.58*max(0.0,nx*light[0]+ny*light[1]+nz*light[2])) if shade else 1.0
        d.polygon([(S/2+p[0]*S*0.92,S/2-p[1]*S*0.92) for p in q],
                  fill=tuple(min(255,int(255*c*k)) for c in base))
    return img.resize((px,px),Image.LANCZOS)

VAR=[("V1 faithful",        1.0, False),
     ("V2 gold muzzle",     1.0, True),
     ("V3 gold + 1.8x wide",1.8, True),
     ("V4 body + 1.8x wide",1.8, False)]
big=Image.new("RGB",(4*250+30,4*140+30),(24,27,32))
for r,(name,wd,gold) in enumerate(VAR):
    F=facets(wd,gold)
    for c,e in enumerate((90,47,16,6)):
        big.paste(render(F,120,e),(15+c*250+60,15+r*140+10))
big.save("/tmp/muzzle-variants.png")
for name,wd,gold in VAR:
    F=facets(wd,gold); F0=[f for f in F][:-4]
    row=[]
    for e in (90,16,6):
        a=render(F,32,e); b=render(F0,32,e)
        la=sum(1 for p in a.get_flattened_data() if p!=(24,27,32))
        df=sum(1 for p,q in zip(a.get_flattened_data(),b.get_flattened_data()) if p!=q)
        row.append(f"{100.0*df/max(1,la):5.1f}%")
    print(f"{name:22} muzzle share at 90/16/6 deg: " + "  ".join(row))
print("WROTE /tmp/muzzle-variants.png")
