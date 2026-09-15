# Does the connector pair let two hosts sit COPLANAR, or does it hold them apart?
#
# The male's flat back is the plane Y=0 and all its relief rises to +Y. So Y=0 is the natural
# mating datum: everything the male adds lives on one side of it. The test below builds two dummy
# host plates that meet on that plane -- one with the male FUSED on, one with the cavity CUT in --
# and measures whether they touch, interfere, or stand apart.
#
# It also emits the artifact that makes this work in practice: a CUTTER solid (the male grown by
# the clearance) that you subtract from any host. A standalone female block cannot keep two hosts
# coplanar, because its own floor material stands between them; a cavity can.
#
# Run: /snap/bin/freecad.cmd coplanar_test.py

import os
import FreeCAD as App
import Part
from FreeCAD import Vector

HERE  = os.path.dirname(os.path.abspath(__file__))
MALE  = os.path.join(HERE, "bear.step")
CLEAR = 0.20

male = Part.Shape(); male.read(MALE); male = male.Solids[0]
bb = male.BoundBox
print(f"male relief: Y {bb.YMin:.3f} .. {bb.YMax:.3f}   -> datum plane Y=0, all relief on +Y")

# the flat back face, and proof it is the whole silhouette sitting on Y=0
back = max((f for f in male.Faces
            if abs(f.CenterOfMass.y) < 1e-6 and abs(abs(f.normalAt(0, 0).y) - 1) < 1e-6),
           key=lambda f: f.Area)
print(f"back face  : {back.Area:.1f} mm2 on Y=0 -- this is the contact surface")

# ---- the cutter: the male grown by the clearance, poking 0.2 mm proud so the boolean is clean
cutter = male.makeOffsetShape(CLEAR, 1e-6, False, False, 0, 2, False).Solids[0]
cb = cutter.BoundBox
print(f"cutter     : Y {cb.YMin:.3f} .. {cb.YMax:.3f}, {cutter.Volume/1000:.2f} cm3")

# ---- two dummy hosts meeting on Y = 0
W, H = 120.0, 100.0
hostA = Part.makeBox(W, 10.0, H, Vector(-W/2, -10.0, -15.0))   # occupies Y -10..0
hostB = Part.makeBox(W, 30.0, H, Vector(-W/2,   0.0, -15.0))   # occupies Y   0..30

partA = hostA.fuse(male)          # male stands proud of A's face
partB = hostB.cut(cutter)         # cavity sunk into B from its face

print(f"\npart A (host + male)   : {partA.Volume/1000:.2f} cm3")
print(f"part B (host - cutter)  : {partB.Volume/1000:.2f} cm3")

# ---- the question ------------------------------------------------------------------
inter = partA.common(partB)
iv = inter.Volume if inter.Solids else 0.0
gap = partA.distToShape(partB)[0]
print(f"\nRESULT interference A vs B : {iv:.6f} mm3   (0 = they do not collide)")
print(f"RESULT closest approach    : {gap:.4f} mm   (0 = the host faces are touching)")

# are the two host faces actually on the same plane?
fa = [f for f in partA.Faces if abs(f.CenterOfMass.y) < 1e-9 and abs(abs(f.normalAt(0,0).y)-1) < 1e-6]
fb = [f for f in partB.Faces if abs(f.CenterOfMass.y) < 1e-9 and abs(abs(f.normalAt(0,0).y)-1) < 1e-6]
print(f"RESULT A has {len(fa)} face(s) lying exactly on Y=0, total {sum(f.Area for f in fa):.1f} mm2")
print(f"RESULT B has {len(fb)} face(s) lying exactly on Y=0, total {sum(f.Area for f in fb):.1f} mm2")
print("RESULT -> the hosts meet on Y=0: COPLANAR" if fa and fb and iv < 1e-3
      else "RESULT -> NOT coplanar")

doc = App.newDocument("Cutter")
o = doc.addObject("Part::Feature", "BearConnector_Cutter"); o.Shape = cutter
doc.recompute()
Part.export([o], os.path.join(HERE, "BearConnector_Cutter.step"))
print(f"\nwrote BearConnector_Cutter.step -- subtract this from any host to get the socket")
