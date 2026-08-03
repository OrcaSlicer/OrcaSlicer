#!/usr/bin/env python3
"""Belt temperature-tower asset generator (discrete-provini design).

A vertical temperature tower cannot be sliced on a belt printer, so lay a row of
DISCRETE provini (one per temperature) along the belt (designed Y) with a fixed
surface gap. Each provino is the chevron+arc unit (belt_temp_provino_unit.stl,
keel-first); its temperature is ENGRAVED upright into the 50 mm face — a raised
number would be an unsupported overhang on the belt. The C++ calib_temp belt branch
(Plater.cpp) injects one M104 per zone 70 layers INTO provino i:
  print_z[i] = i * PITCH * cos(theta) + 70 * layer_height   (theta = 45)
inside the body, not in the empty inter-provino gap (which has no sliced layers for
the event to attach to). PITCH below is the shared geometry contract with that code —
keep them in sync.

Generates one STL per filament temp range used by Temp_Calibration_Dlg.
"""
import numpy as np, trimesh, os
from matplotlib.textpath import TextPath
from matplotlib.font_manager import FontProperties
from shapely.geometry import Polygon as ShPoly
from shapely.ops import unary_union

HERE = os.path.dirname(os.path.abspath(__file__))
UNIT = os.path.join(HERE, 'belt_temp_provino_unit.stl')   # single provino, keel-first
SURF_GAP = 25.0            # surface-to-surface gap between provini (mm) — user spec
TEXT_H = 9.0
TEXT_DEPTH = 0.8           # engraving depth (numbers are CUT into the face, not raised:
                          # a raised number is an unsupported Y-overhang on the belt)
TEXT_OVERSHOOT = 0.6       # extra height poking out of the face for a clean boolean cut

# Temperature ranges (start, end) per filament family, 5 C step. File name encodes them.
RANGES = [(230,190),(270,230),(250,230),(280,240),(240,210),(320,280)]

unit = trimesh.load(UNIT)
dY = unit.bounds[1,1] - unit.bounds[0,1]
PITCH = dY + SURF_GAP                       # designed-Y pitch == C++ contract constant
print(f"unit dY={dY:.2f}  PITCH={PITCH:.3f}  (C++ contract: print_z[i]=i*{PITCH:.3f}*cos45)")

# 50 mm face normal (0,-1,1)/sqrt2 ; UPRIGHT basis u=+X det(+1) (verified non-mirrored)
n = np.array([0,-1,1.])/np.sqrt(2)
u = np.array([1,0,0.]); v = np.array([0,1,1.])/np.sqrt(2)
R = np.column_stack([u,v,n])
fn = unit.face_normals; fc = unit.triangles_center; fa = unit.area_faces
sel = (fn@n) > 0.9
face_c = (fc[sel]*fa[sel,None]).sum(0)/fa[sel].sum()

def text_mesh(s):
    tp = TextPath((0,0), s, size=TEXT_H, prop=FontProperties(family='DejaVu Sans'))
    rings = [ShPoly(p) for p in tp.to_polygons() if len(p)>=3]
    rings.sort(key=lambda r:r.area, reverse=True)
    used=[False]*len(rings); parts=[]
    for i,o in enumerate(rings):
        if used[i]: continue
        holes=[]
        for j in range(i+1,len(rings)):
            if not used[j] and o.contains(rings[j]): holes.append(rings[j].exterior.coords); used[j]=True
        parts.append(ShPoly(o.exterior.coords,holes)); used[i]=True
    poly = unary_union(parts)
    geoms = list(poly.geoms) if poly.geom_type=='MultiPolygon' else [poly]
    m = trimesh.util.concatenate([trimesh.creation.extrude_polygon(g,height=TEXT_DEPTH+TEXT_OVERSHOOT) for g in geoms])
    c = m.bounds.mean(axis=0); m.apply_translation([-c[0],-c[1],0]); return m

for t_start, t_end in RANGES:
    temps = list(range(t_start, t_end-1, -5))
    parts=[]
    for i,T in enumerate(temps):
        c = unit.copy(); c.apply_translation([0, i*PITCH, 0])
        t = text_mesh(str(T)); M=np.eye(4); M[:3,:3]=R; t.apply_transform(M)
        # place the text spanning from TEXT_DEPTH inside the face to TEXT_OVERSHOOT outside,
        # then CUT it out of the provino (engrave) — no raised material, no Y-overhang.
        t.apply_translation(face_c - n*TEXT_DEPTH + np.array([0,i*PITCH,0]))
        c = trimesh.boolean.difference([c, t], engine='manifold')
        parts.append(c)
    asset = trimesh.util.concatenate(parts)
    out = os.path.join(HERE, f"belt_temp_tower_{t_start}_{t_end}.stl")
    asset.export(out)
    dims = np.round(asset.bounds[1]-asset.bounds[0],1)
    wt = all(p.is_watertight for p in parts)
    print(f"  {t_start}->{t_end}: {len(temps)} zones  bbox={dims}  watertight={wt}  -> {os.path.basename(out)}")
