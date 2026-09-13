# Check both trimmed females still fit the male, and export STLs for a visual comparison.
# Run: /snap/bin/freecad.cmd verify_trimmed.py
import os
import Mesh, Part

HERE = os.path.dirname(os.path.abspath(__file__))
male = Part.Shape(); male.read(os.path.join(HERE, "bear.step")); male = male.Solids[0]

for name in ("BearConnector_Female_Trimmed", "BearConnector_Female_Trimmed_wall"):
    p = os.path.join(HERE, name + ".step")
    s = Part.Shape(); s.read(p); s = s.Solids[0]
    d = male.distToShape(s)[0]
    c = male.common(s)
    cv = c.Volume if c.Solids else 0.0
    bb = s.BoundBox
    print(f"{name}")
    print(f"   {bb.XLength:.2f} x {bb.YLength:.2f} x {bb.ZLength:.2f} mm, {s.Volume/1000:.2f} cm3, "
          f"{len(s.Faces)} faces, valid={s.isValid()}")
    print(f"   gap to male {d:.4f} mm, interference {cv:.6f} mm3")
    m = Mesh.Mesh(); m.addFacets([tuple(t) for t in s.tessellate(0.12)[1]] and
                                 [(s.tessellate(0.12)[0][a], s.tessellate(0.12)[0][b],
                                   s.tessellate(0.12)[0][c2])
                                  for a, b, c2 in s.tessellate(0.12)[1]])
    m.write(os.path.join(HERE, name + ".stl"))
    print(f"   wrote {name}.stl ({m.CountFacets} facets)")
