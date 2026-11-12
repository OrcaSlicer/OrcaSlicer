# Pull the bear's true silhouette and feature positions out of the supplied male B-rep, so the
# simplification study starts from measured geometry instead of a tracing of the flat drawing.
#
# The part's native frame (make_female.py): flat back on Y=0, relief rising to Y=+17.27, the FACE
# carried by X and Z. So the face plane is XZ and the silhouette is the outline projected along Y.
import os, json
import Part

HERE = os.path.dirname(os.path.abspath(__file__))
s = Part.Shape(); s.read(os.path.join(HERE, "bear.step"))
sol = s.Solids[0]
bb = sol.BoundBox
print(f"bbox X {bb.XMin:.2f}..{bb.XMax:.2f}  Y {bb.YMin:.2f}..{bb.YMax:.2f}  Z {bb.ZMin:.2f}..{bb.ZMax:.2f}")

# The back plate face: the planar face whose normal is -Y and which sits at Y=YMin. Its outer wire
# IS the silhouette; its inner wires are the eye holes.
best = None
for f in sol.Faces:
    if f.Surface.__class__.__name__ != "Plane":
        continue
    n = f.Surface.Axis
    if abs(abs(n.y) - 1.0) > 1e-6:
        continue
    c = f.CenterOfMass
    if best is None or c.y < best[0]:
        best = (c.y, f)
y, face = best
print(f"back plate at Y={y:.3f}   wires={len(face.Wires)}   area={face.Area:.1f} mm2")

def wire_pts(w, tol=0.05):
    # ORDER MATTERS and w.Edges does not carry it: OCC hands the edges back in whatever order the
    # face stored them, so concatenating their discretisations gives a scrambled ring. The first
    # version of this script did exactly that and emitted an outline with 7 duplicated points and
    # twice the perimeter it should have. OrderedEdges walks the wire, and each edge is reversed
    # when its own orientation runs against the walk.
    pts = []
    for e in w.OrderedEdges:
        d = e.discretize(Deflection=tol)
        if e.Orientation == "Reversed":
            d = list(reversed(d))
        for p in d:
            pts.append((round(p.x, 3), round(p.z, 3)))
    # drop consecutive duplicates
    out = [pts[0]]
    for p in pts[1:]:
        if abs(p[0]-out[-1][0]) > 1e-4 or abs(p[1]-out[-1][1]) > 1e-4:
            out.append(p)
    return out

data = {"outer": None, "holes": []}
outer = face.OuterWire
data["outer"] = wire_pts(outer)
for w in face.Wires:
    if w.isSame(outer):
        continue
    pts = wire_pts(w)
    xs = [p[0] for p in pts]; zs = [p[1] for p in pts]
    data["holes"].append({"pts": pts,
                          "cx": round(sum(xs)/len(xs), 3), "cz": round(sum(zs)/len(zs), 3),
                          "d": round(max(xs)-min(xs), 3)})
    print(f"  hole: centre ({data['holes'][-1]['cx']}, {data['holes'][-1]['cz']}) dia {data['holes'][-1]['d']}")

print(f"outer wire: {len(data['outer'])} points")
json.dump(data, open(os.path.join(HERE, "bear_outline.json"), "w"))
print("WROTE bear_outline.json")
