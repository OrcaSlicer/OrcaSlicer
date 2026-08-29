# Build the complementary FEMALE for BearConnector.step.
#
# Method: take the supplied male B-rep as-is, grow it by a uniform clearance, and subtract that
# from a block. Working on the real solid rather than re-modelling the bear is the whole point —
# the pocket is then exactly complementary by construction, including every deliberate asymmetry.
#
# The offset uses join=2 (Intersection), which extends the adjacent planes and meets them at a
# sharp corner. For a faceted part that is the correct join: the arc join would round every convex
# edge and blunt the very cues the design depends on.
#
# THE MALE'S NATIVE FRAME: the flat back is the plane Y=0 and the relief rises to Y=+17.27.
# X and Z carry the face (83.34 x 66.69). The frame is kept exactly as supplied so that male and
# female drop into the same assembly without anyone having to re-orient one of them.
# Insertion is therefore along +Y, and the pocket must OPEN on the Y=0 plane.
#
# A first version of this script assumed the relief ran along +Z, built the block around the wrong
# axis, and produced a sealed cavity with no way in. It passed a "male does not intersect female"
# check, because that only tests the seated position and says nothing about whether the part can
# get there. The straight-pull test below is what catches it.
#
# Run:  /snap/bin/freecad.cmd make_female.py

import os, sys, math
import FreeCAD as App
import Part

HERE     = os.path.dirname(os.path.abspath(__file__))
MALE     = os.path.join(HERE, "bear.step")
OUT_STEP = os.path.join(HERE, "BearConnector_Female.step")

CLEAR = 0.20     # per-face clearance, mm
WALL  = 4.0      # material around the pocket, mm
FLOOR = 3.0      # material behind the deepest point of the pocket, mm

male = Part.Shape(); male.read(MALE)
if len(male.Solids) != 1:
    print(f"FAIL: expected 1 solid in the male, found {len(male.Solids)}"); sys.exit(1)
male = male.Solids[0]
bb = male.BoundBox
print(f"male  : {bb.XLength:.2f} (X) x {bb.YLength:.2f} (Y) x {bb.ZLength:.2f} (Z) mm, "
      f"{len(male.Faces)} faces, {male.Volume/1000:.2f} cm3")
print(f"        relief runs Y {bb.YMin:.2f} .. {bb.YMax:.2f}  -> insertion along +Y, mouth at Y={bb.YMin:.2f}")

# ---- 1. can the male even be withdrawn along the insertion axis? ----------------------
# Ray-cast a grid along +Y through the tessellated male and count crossings. A straight pull is
# possible only if no ray enters the solid more than once; a second entry is an undercut.
verts, facets = male.tessellate(0.15)
V = [(v.x, v.y, v.z) for v in verts]
worst, undercut_pts = 0, 0
NX = NZ = 90
for i in range(NX):
    x = bb.XMin + (i + 0.5) * bb.XLength / NX
    for j in range(NZ):
        z = bb.ZMin + (j + 0.5) * bb.ZLength / NZ
        hits = 0
        for (ia, ib, ic) in facets:            # ray (x, *, z) along +Y vs triangle
            ax, ay, az = V[ia]; bx, by, bz = V[ib]; cx, cy, cz = V[ic]
            # 2D point-in-triangle in the XZ plane
            d = (bz - cz) * (ax - cx) + (cx - bx) * (az - cz)
            if abs(d) < 1e-12: continue
            u = ((bz - cz) * (x - cx) + (cx - bx) * (z - cz)) / d
            v = ((cz - az) * (x - cx) + (ax - cx) * (z - cz)) / d
            if u < 0 or v < 0 or u + v > 1: continue
            hits += 1
        worst = max(worst, hits)
        if hits > 2: undercut_pts += 1
print(f"pull  : max crossings along +Y = {worst}, undercut samples = {undercut_pts}/{NX*NZ}")
if undercut_pts:
    print("FAIL: the male has an undercut along +Y; a straight pocket cannot release it")
    sys.exit(1)
print("        no undercut -> a straight-pull pocket works")

# ---- 2. grow the male by the clearance -----------------------------------------------
grown = None
for join, name in ((2, "Intersection"), (1, "Tangent"), (0, "Arc")):
    try:
        g = male.makeOffsetShape(CLEAR, 1e-6, False, False, 0, join, False)
        if g.isValid() and g.Solids:
            grown = g.Solids[0]; print(f"offset: join={name}, {grown.Volume/1000:.2f} cm3"); break
    except Exception as e:
        print(f"offset: join={name} failed -- {e}")
if grown is None:
    print("FAIL: could not offset the male; refusing to emit a zero-clearance pocket"); sys.exit(1)

# ---- 3. the block: walls in X and Z, depth in +Y, OPEN at the Y=0 mouth ---------------
gb = grown.BoundBox
y_mouth = bb.YMin                       # the male's flat back plane
depth   = gb.YMax - y_mouth
block = Part.makeBox(gb.XLength + 2*WALL, depth + FLOOR, gb.ZLength + 2*WALL,
                     App.Vector(gb.XMin - WALL, y_mouth, gb.ZMin - WALL))
print(f"block : {gb.XLength + 2*WALL:.2f} x {depth + FLOOR:.2f} x {gb.ZLength + 2*WALL:.2f} mm, "
      f"mouth on the Y={y_mouth:.2f} plane")

female = block.cut(grown)

# ---- 4. verify --------------------------------------------------------------------------
ok = True
if not female.isValid():              print("FAIL: invalid shape"); ok = False
if len(female.Solids) != 1:           print(f"FAIL: {len(female.Solids)} solids"); ok = False

clash = male.common(female)
cv = clash.Volume if clash.Solids else 0.0
print(f"check : male ∩ female = {cv:.6f} mm3 (seated fit, must be ~0)")
if cv > 1e-3: print("FAIL: male collides with female"); ok = False

# the mouth must actually be open: the pocket has to reach the Y=y_mouth face of the block
mouth_face_area = 0.0
for f in female.Faces:
    c = f.CenterOfMass
    if abs(c.y - y_mouth) < 1e-6:
        mouth_face_area += f.Area
solid_mouth = (gb.XLength + 2*WALL) * (gb.ZLength + 2*WALL)
open_area = solid_mouth - mouth_face_area
print(f"check : mouth plane -- material {mouth_face_area:.1f} mm2, opening {open_area:.1f} mm2 "
      f"({100*open_area/solid_mouth:.1f}% of the face)")
if open_area < 100:
    print("FAIL: the pocket is sealed -- the male cannot be inserted"); ok = False

cavity = block.Volume - female.Volume
print(f"check : cavity {cavity/1000:.2f} cm3 vs male {male.Volume/1000:.2f} cm3 "
      f"-> clearance shell {(cavity-male.Volume)/1000:.2f} cm3")
if cavity < male.Volume: print("FAIL: cavity smaller than the male"); ok = False

if not ok:
    print("\nREFUSING to write the STEP"); sys.exit(1)

doc = App.newDocument("Female")
obj = doc.addObject("Part::Feature", "BearConnector_Female")
obj.Shape = female
doc.recompute()
Part.export([obj], OUT_STEP)
fb = female.BoundBox
print(f"\nwrote {OUT_STEP}")
print(f"female: {fb.XLength:.2f} x {fb.YLength:.2f} x {fb.ZLength:.2f} mm, "
      f"{len(female.Faces)} faces, {female.Volume/1000:.2f} cm3")
