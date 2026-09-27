# Export the real male's relief as a triangle mesh, so the grazing test uses the actual geometry.
import os, json
import Part
HERE = os.path.dirname(os.path.abspath(__file__))
s = Part.Shape(); s.read(os.path.join(HERE, "bear.step"))
verts, facets = s.Solids[0].tessellate(0.25)
V = [[round(p.x,4), round(p.y,4), round(p.z,4)] for p in verts]
json.dump({"v": V, "f": facets}, open(os.path.join(HERE, "bear_mesh.json"), "w"))
print(f"verts {len(V)} facets {len(facets)}")
