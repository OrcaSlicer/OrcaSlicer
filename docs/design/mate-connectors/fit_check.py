# Measure the assembled fit between the supplied male and the generated female.
# This is the number that matters: the minimum gap in the seated position.
#   Run: /snap/bin/freecad.cmd fit_check.py
import os
import Part

HERE = os.path.dirname(os.path.abspath(__file__))
male = Part.Shape(); male.read(os.path.join(HERE, "bear.step"))
fem  = Part.Shape(); fem.read(os.path.join(HERE, "BearConnector_Female.step"))
male, fem = male.Solids[0], fem.Solids[0]

d = male.distToShape(fem)
print(f"RESULT minimum gap male<->female, seated: {d[0]:.4f} mm   (design clearance 0.20)")

c = male.common(fem)
print(f"RESULT interference volume: {(c.Volume if c.Solids else 0.0):.6f} mm3")

p = d[1][0][0]
print(f"RESULT tightest point on the male: ({p.x:.2f}, {p.y:.2f}, {p.z:.2f})")
print(f"RESULT male {male.Volume/1000:.2f} cm3 / female {fem.Volume/1000:.2f} cm3")
