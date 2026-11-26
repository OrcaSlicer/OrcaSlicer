#!/usr/bin/env bash
# End-to-end check that the CLI keeps the per-part extruders of a PrusaSlicer 3MF.
#
# PrusaSlicer stores each part's extruder in Metadata/Slic3r_PE_model.config. The CLI used to load every
# .3mf with the Bambu loader, which ignores that file, so a multi-material PrusaSlicer project printed
# entirely with the first filament. A one-object, two-part 3MF written the way PrusaSlicer writes it (left
# part on extruder 1, right part on extruder 2, apart in X so both are printed) is sliced with two
# filaments: both must be used, which the "; filament used [mm]" G-code line shows as two non-zero
# entries, and the T0 and T1 tool selections both appear. A part on extruder 3 with only two filaments
# loaded must slice without error and select no tool beyond the two loaded. The same object placed by
# two build items must print both copies, told apart by their "unique label id".
#
# usage: test_cli_prusa_3mf_part_extruders.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-prusa-3mf-part-extruders.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# prusa_3mf <file> <left extruder> <right extruder> <build items>: a PrusaSlicer-style 3MF, so the test
# needs no fixture. One object with two 10 mm cube parts 20 mm apart, placed by that many build items.
prusa_3mf() {
    "$PY" - "$@" <<'EOF'
import sys, zipfile

out, left_extruder, right_extruder, items = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])


def cube(x0):
    v = [(x0 + x * 10, y * 10, z * 10) for z in (0, 1) for y in (0, 1) for x in (0, 1)]
    t = []
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        t += [(a, b, c), (a, c, d)]
    return v, t


left_v, left_t = cube(0)
right_v, right_t = cube(20)
vertices = left_v + right_v
triangles = left_t + [(a + 8, b + 8, c + 8) for a, b, c in right_t]

model = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"'
    ' xmlns:slic3rpe="http://schemas.slic3r.org/3mf/2017/06">\n'
    ' <metadata name="slic3rpe:Version3mf">1</metadata>\n'
    ' <metadata name="Application">PrusaSlicer-2.6.0</metadata>\n'
    ' <resources>\n  <object id="1" type="model">\n   <mesh>\n    <vertices>\n'
    + "".join('     <vertex x="%g" y="%g" z="%g"/>\n' % p for p in vertices)
    + '    </vertices>\n    <triangles>\n'
    + "".join('     <triangle v1="%d" v2="%d" v3="%d"/>\n' % t for t in triangles)
    + '    </triangles>\n   </mesh>\n  </object>\n </resources>\n <build>\n'
    + "".join('  <item objectid="1" transform="1 0 0 0 1 0 0 0 1 %d 100 0" printable="1"/>\n' % (60 + 80 * i) for i in range(items))
    + ' </build>\n</model>\n'
)

config = (
    '<?xml version="1.0" encoding="UTF-8"?>\n<config>\n'
    ' <object id="1" instances_count="%d">\n  <metadata type="object" key="name" value="two parts"/>\n' % items
)
for name, first, last, extruder in (("left", 0, 11, left_extruder), ("right", 12, 23, right_extruder)):
    config += (
        '  <volume firstid="%d" lastid="%d">\n'
        '   <metadata type="volume" key="name" value="%s"/>\n'
        '   <metadata type="volume" key="volume_type" value="ModelPart"/>\n'
        '   <metadata type="volume" key="matrix" value="1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1"/>\n'
        '   <metadata type="volume" key="extruder" value="%d"/>\n'
        '   <mesh edges_fixed="0" degenerate_facets="0" facets_removed="0" facets_reversed="0" backwards_edges="0"/>\n'
        '  </volume>\n' % (first, last, name, extruder)
    )
config += ' </object>\n</config>\n'

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", '<?xml version="1.0" encoding="UTF-8"?><Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types"><Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/><Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
    z.writestr("_rels/.rels", '<?xml version="1.0" encoding="UTF-8"?><Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
    z.writestr("3D/3dmodel.model", model)
    z.writestr("Metadata/Slic3r_PE_model.config", config)
EOF
}

FILAMENT="$PROFILES/filament/Bambu PLA Basic @BBL X1C.json"

# slice <tag> <input> [option...] -> $WORK/<tag>/{rc,stdout,stderr,plate_1.gcode}, two filaments loaded.
slice() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" \
        --load-settings "$PROFILES/machine/Bambu Lab X1 Carbon 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
        --load-filaments "$FILAMENT;$FILAMENT" "$@" --slice 0 --outputdir "$out" "$input" \
        > "$out/stdout" 2> "$out/stderr"
    echo "$?" > "$out/rc"
}

fail() { echo "FAIL: $*"; exit 1; }

# succeeded <tag>: the run exited 0 and wrote g-code.
succeeded() {
    [ "$(cat "$WORK/$1/rc")" = 0 ] || fail "$1: orca-slicer exited $(cat "$WORK/$1/rc"); $(tail -n 5 "$WORK/$1/stdout")"
    [ -s "$WORK/$1/plate_1.gcode" ] || fail "$1: no g-code written"
}

# filament_used <tag> -> the per-filament lengths of "; filament used [mm] = a, b", one per line.
filament_used() {
    grep -m1 '^; filament used \[mm\]' "$WORK/$1/plate_1.gcode" | sed 's/^[^=]*= *//; s/, */\n/g'
}

# both_filaments_used <tag>: two filament entries, each with extrusion, and a tool change between them.
both_filaments_used() {
    local lengths count positive
    lengths="$(filament_used "$1")"
    count="$(printf '%s\n' "$lengths" | wc -l)"
    positive="$(printf '%s\n' "$lengths" | awk '$1 > 0' | wc -l)"
    [ "$count" = 2 ] || fail "$1: expected 2 filament entries, got: $lengths"
    [ "$positive" = 2 ] || fail "$1: expected both filaments to extrude, got: $(printf '%s' "$lengths" | tr '\n' ' ')"
    grep -q '^T0$' "$WORK/$1/plate_1.gcode" || fail "$1: no T0 tool selection"
    grep -q '^T1$' "$WORK/$1/plate_1.gcode" || fail "$1: no T1 tool selection"
}

# 1. Extruders 1 and 2 on the two parts of one object: both filaments print.
prusa_3mf "$WORK/two_parts.3mf" 1 2 1
slice two_parts "$WORK/two_parts.3mf"
succeeded two_parts
both_filaments_used two_parts

# 2. A part on extruder 3 with two filaments loaded: the run succeeds and selects no third tool.
prusa_3mf "$WORK/extruder_three.3mf" 1 3 1
slice extruder_three "$WORK/extruder_three.3mf"
succeeded extruder_three
grep -q '^T2$' "$WORK/extruder_three/plate_1.gcode" && fail "extruder_three: selected a tool beyond the two filaments"

# 3. The object placed twice: arrange keeps both copies, and both filaments still print.
prusa_3mf "$WORK/two_items.3mf" 1 2 2
slice two_items "$WORK/two_items.3mf" --arrange 1
succeeded two_items
both_filaments_used two_items
labels="$(grep '^; start printing object' "$WORK/two_items/plate_1.gcode" | sort -u | wc -l)"
[ "$labels" = 2 ] || fail "two_items: expected 2 printed objects, got $labels"

echo "PASS: PrusaSlicer 3MF part extruders, extruder beyond the loaded filaments, and repeated build items"
