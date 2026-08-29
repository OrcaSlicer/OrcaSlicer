# Mate-connector glyph probe — built as REAL solids on REAL mechanical geometry,
# so the shape can be judged in a 3D viewport instead of in a browser mock.
#
# Four polarity treatments, side by side on one bracket:
#   A  Onshape baseline ...... ring + roll quadrant + three short axis arms
#   B  solid cone ............ ring + quadrant + one-sided Z arrow, filled head   (driven)
#   C  hollow collar ......... ring + quadrant + one-sided Z arrow, shell head    (fixed)
#   D  pin / cup ............. polarity by RELIEF: a raised pin vs a sunk cup
#
# D is the one that only a 3D test can settle: in a shaded viewport, solid-vs-hollow is a
# weak cue that depends on angle and lighting, while convex-vs-concave is a strong one --
# and male/female is the mechanical language for polarity anyway.
#
# Scale note: in the real viewport gizmos are screen-constant (~15-40 px via upp = 1/zoom).
# At a zoom where a 60 mm part fills ~600 px, 40 px is about 4 mm, so R = 4.5 mm here.

import FreeCAD as App
import FreeCADGui as Gui
import Part
from FreeCAD import Vector

DOC = "GlyphProbe"
for d in list(App.listDocuments()):
    App.closeDocument(d)
doc = App.newDocument(DOC)

R = 4.5          # disc radius, the module everything scales from
GOLD  = (0.93, 0.66, 0.09)
BLUE  = (0.18, 0.44, 0.93)
GREY  = (0.42, 0.46, 0.52)
RED   = (0.85, 0.29, 0.24)
GREEN = (0.23, 0.65, 0.35)

def add(name, shape, color, transparency=0):
    o = doc.addObject("Part::Feature", name)
    o.Shape = shape
    o.ViewObject.ShapeColor = color
    o.ViewObject.LineColor  = color
    o.ViewObject.PointColor = color
    o.ViewObject.Transparency = transparency
    return o

def frame(origin, zdir, xdir):
    """Right-handed placement matrix from origin + Z + X (X orthonormalised against Z)."""
    z = Vector(*zdir); z.normalize()
    xr = Vector(*xdir)
    x = xr.sub(Vector(z).multiply(z.dot(xr))); x.normalize()
    y = z.cross(x)
    return App.Matrix(x.x, y.x, z.x, origin[0],
                      x.y, y.y, z.y, origin[1],
                      x.z, y.z, z.z, origin[2],
                      0, 0, 0, 1)

# ---------------------------------------------------------------- the bracket
plate = Part.makeBox(120, 46, 8)
bore  = Part.makeCylinder(7, 40, Vector(96, 23, -6))          # a real bore, curved face
boss  = Part.makeCylinder(11, 7, Vector(96, 23, 8))
part  = plate.fuse(boss).cut(bore)
add("Bracket", part, (0.60, 0.63, 0.66))

# ---------------------------------------------------------------- glyph pieces
def ring(t=None):
    t = t or R * 0.10
    return Part.makeCylinder(R, t).cut(Part.makeCylinder(R * 0.84, t))

def quadrant(t=None):
    t = t or R * 0.10
    return Part.makeCylinder(R * 0.84, t, Vector(0, 0, 0), Vector(0, 0, 1), 90)

def stem(L=None, r=None):
    return Part.makeCylinder(r or R * 0.09, L or R * 2.3)

def solid_head():
    return Part.makeCone(R * 0.32, 0, R * 0.80, Vector(0, 0, R * 2.3))

def shell_head():
    outer = Part.makeCone(R * 0.32, 0, R * 0.80, Vector(0, 0, R * 2.3))
    inner = Part.makeCone(R * 0.22, 0, R * 0.62, Vector(0, 0, R * 2.3))
    return outer.cut(inner)

def short_axis(direction, L=None):
    L = L or R * 1.15
    return Part.makeCylinder(R * 0.07, L, Vector(0, 0, 0), Vector(*direction))

def place(shape, m):
    s = shape.copy()
    s.transformShape(m)
    return s

# ---------------------------------------------------------------- the variants
def variant_A(tag, origin):                       # Onshape baseline
    m = frame(origin, (0, 0, 1), (1, 0, 0))
    add(tag + "_ring", place(ring(), m), GREY)
    add(tag + "_quad", place(quadrant(), m), GOLD)
    add(tag + "_x", place(short_axis((1, 0, 0)), m), RED)
    add(tag + "_y", place(short_axis((0, 1, 0)), m), GREEN)
    add(tag + "_z", place(short_axis((0, 0, 1), R * 1.6), m), BLUE)

def variant_B(tag, origin, zdir=(0, 0, 1)):       # solid cone = driven
    m = frame(origin, zdir, (1, 0, 0))
    add(tag + "_ring", place(ring(), m), BLUE)
    add(tag + "_quad", place(quadrant(), m), GOLD)
    add(tag + "_body", place(stem().fuse(solid_head()), m), BLUE)

def variant_C(tag, origin, zdir=(0, 0, 1)):       # hollow collar = fixed
    m = frame(origin, zdir, (1, 0, 0))
    add(tag + "_ring", place(ring(), m), GREY)
    add(tag + "_quad", place(quadrant(), m), GOLD)
    add(tag + "_body", place(stem().fuse(shell_head()), m), GREY)

def variant_D_pin(tag, origin, zdir=(0, 0, 1)):   # polarity by relief: raised PIN
    m = frame(origin, zdir, (1, 0, 0))
    pin = Part.makeCylinder(R * 0.30, R * 1.5).fuse(
          Part.makeCone(R * 0.30, 0, R * 0.55, Vector(0, 0, R * 1.5)))
    add(tag + "_ring", place(ring(), m), BLUE)
    add(tag + "_quad", place(quadrant(), m), GOLD)
    add(tag + "_pin", place(pin, m), BLUE)

def variant_D_cup(tag, origin, zdir=(0, 0, 1)):   # polarity by relief: sunk CUP
    m = frame(origin, zdir, (1, 0, 0))
    cup = Part.makeCylinder(R * 0.62, R * 0.9).cut(
          Part.makeCylinder(R * 0.40, R * 0.9, Vector(0, 0, -0.01)))
    add(tag + "_ring", place(ring(), m), GREY)
    add(tag + "_quad", place(quadrant(), m), GOLD)
    add(tag + "_cup", place(cup, m), GREY)

# four treatments across the plate, all on the same flat face, same Z
variant_A("A", (14, 30, 8))
variant_B("B", (40, 30, 8))
variant_C("C", (64, 30, 8))
variant_D_pin("Dpin", (14, 10, 8))
variant_D_cup("Dcup", (40, 10, 8))

# the hard cases, which is the whole reason for doing this in 3D:
variant_B("Bore", (96, 23, 15))                    # on the boss above a bore
variant_B("Edge", (64, 0, 8), (0, -0.7071, 0.7071))  # tilted, on an edge, oblique Z

doc.recompute()

v = Gui.activeDocument().activeView()
v.viewIsometric()
Gui.SendMsgToActiveView("ViewFit")
App.Console.PrintMessage("glyph probe built: %d objects\n" % len(doc.Objects))
