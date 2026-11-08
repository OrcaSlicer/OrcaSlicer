#!/usr/bin/env bash
# End-to-end check that the CLI judges a project against this build's version on the line that wrote it.
#
# A project records two versions: the OrcaSlicer version this build is based on in the "OrcaSlicer"
# metadata tag, and this fork's release in the "UnlayeredSlicer" tag. Releases from before that second
# tag wrote their own 0.x release into the first. A project is exported from the shipped Bambu Lab P1S
# presets, which must carry each tag once, and its tags are rewritten to make:
# - an OrcaSlicer project at the minor version this build is based on, which loads;
# - an OrcaSlicer project one minor version newer, which is refused, and loads with --allow-newer-file;
# - a project this fork saved before the UnlayeredSlicer tag, which loads without OrcaSlicer's migrations
#   of old files, and is saved again with this build's two tags;
# - a project from a newer release of this fork, which is refused.
#
# usage: test_cli_project_version.sh <orca-slicer binary> <python3> <resources/profiles/BBL>
set -u

BIN="${1:-}"
PY="${2:-python3}"
PROFILES="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -d "$PROFILES" ] || { echo "FAIL: profiles directory not found: $PROFILES"; exit 1; }

# CLI_FILE_VERSION_NOT_SUPPORTED (-24) as a process exit status.
REFUSED=232

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-project-version.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

"$PY" - "$WORK/cube.stl" <<'EOF'
import sys

v = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
with open(sys.argv[1], "w") as f:
    f.write("solid cube\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")
EOF

# slice <tag> <expected exit status> <input> [option...]: slice into $WORK/<tag>/out.3mf with a fresh
# data directory, logging at info level to $WORK/<tag>/cli.log.
slice() {
    local out="$WORK/$1" want="$2" input="$3"; shift 3
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$out/datadir" --debug 3 --logfile "$out/cli.log" "$@" --slice 0 --outputdir "$out" \
        --export-3mf out.3mf "$input" > "$out/log" 2>&1
    local rc=$?
    if [ "$rc" -ne "$want" ]; then
        echo "FAIL: $1: orca-slicer exited $rc, want $want"
        tail -n 40 "$out/log"
        exit 1
    fi
}

# tags <3mf>: the file's OrcaSlicer and UnlayeredSlicer tag values, one "name=value" line each, in file order.
tags() {
    "$PY" - "$1" <<'EOF'
import re, sys, zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    model = z.read("3D/3dmodel.model").decode("utf-8")
for name, value in re.findall(r'<metadata name="(OrcaSlicer|UnlayeredSlicer)">([^<]*)</metadata>', model):
    print("%s=%s" % (name, value))
EOF
}

slice base 0 "$WORK/cube.stl" \
    --load-settings "$PROFILES/machine/Bambu Lab P1S 0.4 nozzle.json;$PROFILES/process/0.20mm Standard @BBL X1C.json" \
    --load-filaments "$PROFILES/filament/Bambu PLA Basic @BBL P1S 0.4 nozzle.json"

BASE_TAGS="$(tags "$WORK/base/out.3mf")"
ORCA="$(printf '%s\n' "$BASE_TAGS" | sed -n 's/^OrcaSlicer=//p')"
UNLAYERED="$(printf '%s\n' "$BASE_TAGS" | sed -n 's/^UnlayeredSlicer=//p')"
if [ "$(printf '%s\n' "$BASE_TAGS" | wc -l)" -ne 2 ] || [ -z "$ORCA" ] || [ -z "$UNLAYERED" ]; then
    echo "FAIL: a saved project carries each version tag once; got:"
    printf '%s\n' "$BASE_TAGS"
    exit 1
fi

# variant <name> <OrcaSlicer value> [UnlayeredSlicer value]: $WORK/<name>.3mf, the base project with its
# tags replaced; without the third argument it has no UnlayeredSlicer tag.
variant() {
    "$PY" - "$WORK/base/out.3mf" "$WORK/$1.3mf" "$2" "${3:-}" <<'EOF' || exit $?
import re, sys, zipfile

src, dst, orca, unlayered = sys.argv[1:5]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for item in zin.infolist():
        data = zin.read(item.filename)
        if item.filename == "3D/3dmodel.model":
            model = data.decode("utf-8")
            model = re.sub(r'<metadata name="OrcaSlicer">[^<]*</metadata>', '<metadata name="OrcaSlicer">%s</metadata>' % orca, model)
            model = re.sub(r'<metadata name="UnlayeredSlicer">[^<]*</metadata>',
                           '<metadata name="UnlayeredSlicer">%s</metadata>' % unlayered if unlayered else "", model)
            data = model.encode("utf-8")
        zout.writestr(item, data)
EOF
}

# bump <version>: the next minor version.
bump() { "$PY" -c 'import sys; v = sys.argv[1].split("."); print("%s.%d.0" % (v[0], int(v[1]) + 1))' "$1"; }

ORCA_MAJOR_MINOR="$(printf '%s' "$ORCA" | cut -d. -f1,2)"
variant orca_same "$ORCA_MAJOR_MINOR.0"
variant orca_newer "$(bump "$ORCA")"
variant unlayered_old "0.0.9-dev"
variant unlayered_newer "$ORCA" "$(bump "$UNLAYERED")"

slice orca_same 0 "$WORK/orca_same.3mf"
slice orca_newer "$REFUSED" "$WORK/orca_newer.3mf"
slice orca_newer_allowed 0 "$WORK/orca_newer.3mf" --allow-newer-file
slice unlayered_newer "$REFUSED" "$WORK/unlayered_newer.3mf"

slice unlayered_old 0 "$WORK/unlayered_old.3mf"
LOG="$WORK/unlayered_old/cli.log"
# The version line proves the log is being written at the level the migration lines use.
if ! grep -q "the first file is a 3mf, version" "$LOG"; then
    echo "FAIL: unlayered_old: the CLI log has no 3MF version line, so it cannot show the migrations"
    exit 1
fi
if grep -E "old 3mf version .*, need to" "$LOG"; then
    echo "FAIL: unlayered_old: OrcaSlicer's migrations of old files ran on a project this fork saved"
    exit 1
fi
RESAVED="$(tags "$WORK/unlayered_old/out.3mf")"
if [ "$RESAVED" != "$BASE_TAGS" ]; then
    echo "FAIL: unlayered_old: saved again with tags"
    printf '%s\n' "$RESAVED"
    echo "want"
    printf '%s\n' "$BASE_TAGS"
    exit 1
fi
echo "PASS"
