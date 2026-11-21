#!/usr/bin/env bash
# End-to-end check that --scale keeps an object where it is on the plate.
#
# --scale scales about the object's origin, which for a model loaded from the command line is
# nowhere near the object, so a bare scale used to lift a shrunk object off the bed (the first layer
# is then empty and the slice fails) and sink a grown one into it. Arrange is off and --ensure-on-bed
# is not passed, so nothing but the scale itself can put the object back: its bounding box must keep
# its XY centre and its lowest point (z 0) and have the expected size.
#
# The observable is the 3MF the CLI exports, with its bounding box computed here from the build item
# transform and the object meshes. G-code could not show a lowest point that moved, since the slicer
# always prints from the object's own bottom, and a skirt or brim would shift its XY extents.
#
# usage: test_cli_scale_in_place.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-scale-in-place.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# Fixtures are generated: a 20 mm box in file coordinates x 100-120, y 50-70, z 0-20 (ASCII STL), and
# a generic 3MF (core spec, no slicer metadata) with one object of two components: the same body plus
# a 5 x 5 x 2 mm inlay on top of it, which reaches z 22.
"$PY" - "$WORK" <<'PYEOF' || exit 1
import sys, zipfile

def mesh(x0, y0, z0, sx, sy, sz):
    v = [(x0 + x, y0 + y, z0 + z) for z in (0, sz) for y in (0, sy) for x in (0, sx)]
    faces = ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5))
    tris = [t for a, b, c, d in faces for t in ((a, b, c), (a, c, d))]
    return v, tris

work = sys.argv[1]
v, tris = mesh(100, 50, 0, 20, 20, 20)
with open(work + "/box.stl", "w") as f:
    f.write("solid box\n")
    for t in tris:
        f.write("facet normal 0 0 0\nouter loop\n")
        for i in t:
            f.write("vertex %g %g %g\n" % v[i])
        f.write("endloop\nendfacet\n")
    f.write("endsolid box\n")

def xml_mesh(origin, size):
    v, tris = mesh(*origin, *size)
    return ("<mesh><vertices>" + "".join('<vertex x="%g" y="%g" z="%g"/>' % p for p in v) + "</vertices><triangles>"
            + "".join('<triangle v1="%d" v2="%d" v3="%d"/>' % t for t in tris) + "</triangles></mesh>")

model = ('<?xml version="1.0" encoding="UTF-8"?><model unit="millimeter" '
         'xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"><resources>'
         '<object id="1" type="model">%s</object><object id="2" type="model">%s</object>'
         '<object id="3" type="model"><components><component objectid="1"/><component objectid="2"/></components></object>'
         '</resources><build><item objectid="3"/></build></model>'
         % (xml_mesh((100, 50, 0), (20, 20, 20)), xml_mesh((105, 55, 20), (5, 5, 2))))
with zipfile.ZipFile(work + "/inlay.3mf", "w") as z:
    z.writestr("[Content_Types].xml", '<?xml version="1.0"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
               '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
               '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
    z.writestr("_rels/.rels", '<?xml version="1.0"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
               '<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
    z.writestr("3D/3dmodel.model", model)
PYEOF

# bbox <3mf>: prints "minx miny minz maxx maxy maxz" of everything on the build plate, in mm.
cat > "$WORK/bbox.py" <<'PYEOF'
import sys, zipfile
import xml.etree.ElementTree as ET

PATH = "{http://schemas.microsoft.com/3dmanufacturing/production/2015/06}path"
zf = zipfile.ZipFile(sys.argv[1])
tag = lambda e: e.tag.rsplit("}", 1)[-1]

def apply(transform):  # 3MF transforms are 12 numbers, row-vector convention: 3x3 rows, then the translation
    m = [float(x) for x in transform.split()] if transform else [1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0]
    return lambda p: tuple(sum(p[i] * m[3 * i + j] for i in range(3)) + m[9 + j] for j in range(3))

models = {}
def points(path, oid):
    if path not in models:
        models[path] = {o.get("id"): o for o in ET.fromstring(zf.read(path.lstrip("/"))).iter() if tag(o) == "object"}
    for child in models[path][oid]:
        if tag(child) == "mesh":
            yield from (tuple(float(v.get(a)) for a in "xyz") for v in child.iter() if tag(v) == "vertex")
        elif tag(child) == "components":
            for c in child:
                move = apply(c.get("transform"))
                yield from (move(p) for p in points(c.get(PATH) or path, c.get("objectid")))

main = "3D/3dmodel.model"
root = ET.fromstring(zf.read(main))
pts = []
for item in (e for e in root.iter() if tag(e) == "item"):
    move = apply(item.get("transform"))
    pts += [move(p) for p in points(main, item.get("objectid"))]
print(*[min(p[i] for p in pts) for i in range(3)], *[max(p[i] for p in pts) for i in range(3)])
PYEOF

# run <tag> <input> [option...]: arrange off, no --ensure-on-bed, slice and export the 3MF.
run() {
    local tag="$1" out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" \
        --load-settings "$PROFILES/machine/Bambu Lab X1 Carbon 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
        --load-defaultfila --arrange 0 "$@" --slice 0 --outputdir "$out" --export-3mf out.3mf "$input" \
        > "$out/log" 2>&1
    local rc=$?
    [ "$rc" = 0 ] || { echo "FAIL: $tag: exit code $rc; $(tail -n 3 "$out/log")"; exit 1; }
}

# check <tag> <size x> <size y> <size z>: centre XY is (110, 60), lowest point is 0, and the size is as given.
check() {
    # shellcheck disable=SC2046 # bbox.py prints six numbers that must arrive as separate arguments
    "$PY" - "$@" $("$PY" "$WORK/bbox.py" "$WORK/$1/out.3mf") <<'PYEOF' || exit 1
import sys
tag, want, got = sys.argv[1], [float(x) for x in sys.argv[2:5]], [float(x) for x in sys.argv[5:11]]
lo, hi = got[:3], got[3:]
have = {"size": [hi[i] - lo[i] for i in range(3)], "centre XY": [(lo[i] + hi[i]) / 2 for i in range(2)], "min Z": [lo[2]]}
expect = {"size": want, "centre XY": [110, 60], "min Z": [0]}
for name in expect:
    if any(abs(a - b) > 0.01 for a, b in zip(have[name], expect[name])):
        sys.exit("FAIL: %s: %s is %s, expected %s" % (tag, name, have[name], expect[name]))
PYEOF
}

# 1. A single box, shrunk and grown.
run shrink "$WORK/box.stl" --scale 0.6;                     check shrink 12 12 12
run grow   "$WORK/box.stl" --scale 1.5;                     check grow 30 30 30

# 2. One object of two parts: the parts must scale together, so the inlay keeps its place on the body.
run multi_shrink "$WORK/inlay.3mf" --scale 0.6;             check multi_shrink 12 12 13.2
run multi_grow   "$WORK/inlay.3mf" --scale 1.5;             check multi_grow 30 30 33

echo "PASS: --scale keeps the object's place on the plate, for one part and for several"
