# Trim the boxy frame off the female so its outer shape is the bear face itself.
#
# Method: take the male's flat back face (the plane Y=0 -- that face IS the bear silhouette),
# offset its OUTER wire outward in 2D, extrude the result along the insertion axis, and keep only
# the part of the female inside it. Everything outside is the block frame and goes away.
#
# NOTE ON THE NUMBER. The pocket's side walls stand at +0.20 mm from the male outline, because that
# is the clearance. A trim boundary at +0.10 mm therefore falls INSIDE them by 0.10 mm and removes
# the side wall entirely rather than leaving a thin one. The script runs the requested value and
# then measures what is actually left, so the outcome is a number rather than an opinion; it also
# emits a second variant at an offset that leaves a printable wall, for comparison.
#
# Run: /snap/bin/freecad.cmd trim_female.py

import os, sys
import FreeCAD as App
import Part
from FreeCAD import Vector

HERE   = os.path.dirname(os.path.abspath(__file__))
MALE   = os.path.join(HERE, "bear.step")
FEMALE = os.path.join(HERE, "BearConnector_Female.step")

REQUESTED = 0.10      # as asked
CLEARANCE = 0.20      # what the pocket was built with
SAFE_WALL = 1.60      # a wall that survives an FDM nozzle: clearance + ~1.4 mm

male = Part.Shape(); male.read(MALE);   male = male.Solids[0]
fem  = Part.Shape(); fem.read(FEMALE);  fem  = fem.Solids[0]
print(f"female in : {fem.Volume/1000:.2f} cm3, {len(fem.Faces)} faces")

# --- the bear silhouette: the male's flat back face at Y = 0
back = None
for f in male.Faces:
    n = f.normalAt(0, 0)
    if abs(f.CenterOfMass.y) < 1e-6 and abs(abs(n.y) - 1.0) < 1e-6:
        if back is None or f.Area > back.Area:
            back = f
if back is None:
    print("FAIL: could not find the flat back face at Y=0"); sys.exit(1)
print(f"silhouette: back face area {back.Area:.1f} mm2, {len(back.Wires)} wires "
      f"(outer + {len(back.Wires)-1} holes: eyes and mouth)")

fb = fem.BoundBox
y0, y1 = fb.YMin - 5.0, fb.YMax + 5.0

def trimmed(offset):
    """keep only the part of the female inside the silhouette grown by `offset`"""
    wire = back.OuterWire
    grown = wire.makeOffset2D(offset, join=2, fill=False, openResult=False, intersection=True)
    face  = Part.Face(Part.Wire(grown.Edges))
    prism = face.extrude(Vector(0, y1 - y0, 0))
    prism.translate(Vector(0, y0 - face.CenterOfMass.y, 0))
    return fem.common(prism)

for tag, off, out in (("requested", REQUESTED, "BearConnector_Female_Trimmed.step"),
                      ("safe wall", SAFE_WALL, "BearConnector_Female_Trimmed_wall.step")):
    r = trimmed(off)
    if not r.Solids:
        print(f"\n{tag} (+{off:.2f} mm): NOTHING LEFT"); continue
    wall = off - CLEARANCE
    # is there any material left at the level of the pocket's side wall?
    sec = r.section(Part.makePlane(400, 400, Vector(-200, 1.5, -200), Vector(0, 1, 0)))
    perim = sum(e.Length for e in sec.Edges)
    print(f"\n{tag} (+{off:.2f} mm)  wall = {wall:+.2f} mm")
    print(f"   volume {r.Volume/1000:.2f} cm3, {len(r.Solids)} solid(s), {len(r.Faces)} faces")
    print(f"   section through the pocket wall at Y=1.5: {perim:.1f} mm of edge")
    if wall <= 0:
        print(f"   -> the trim cuts {abs(wall):.2f} mm INSIDE the pocket wall: no side wall remains")
    doc = App.newDocument(tag.replace(" ", "_"))
    o = doc.addObject("Part::Feature", "Female")
    o.Shape = r; doc.recompute()
    Part.export([o], os.path.join(HERE, out))
    print(f"   wrote {out}")
