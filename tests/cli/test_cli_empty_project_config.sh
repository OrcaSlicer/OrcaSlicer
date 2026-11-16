#!/usr/bin/env bash
# Regression check: a 3mf whose Metadata/project_settings.config carries no settings must load.
#
# The CLI reads printable_height out of the project config with opt_float(), which dereferences
# what option<>() returns. With create = false that is null when the key is absent, so a project
# saved without settings used to take the CLI down with a segfault. Both models in
# resources/handy_models are such files.
#
# usage: test_cli_empty_project_config.sh <orca-slicer binary> <python3> <source dir>
set -u

BIN="${1:-}"
PY="${2:-python3}"
SRC="${3:-}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }
[ -f "$SRC/resources/handy_models/OrcaBadge.3mf" ] || { echo "SKIP: handy model not found"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-emptycfg.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Rewrite the project settings to an empty object, so the test holds no matter what the shipped
# models carry later on.
cp "$SRC/resources/handy_models/OrcaBadge.3mf" "$WORK/empty_config.3mf"
"$PY" - "$WORK/empty_config.3mf" <<'PYEOF'
import shutil, sys, zipfile

path = sys.argv[1]
entry = "Metadata/project_settings.config"
with zipfile.ZipFile(path) as src:
    items = [(i, src.read(i.filename)) for i in src.infolist()]
with zipfile.ZipFile(path + ".new", "w", zipfile.ZIP_DEFLATED) as dst:
    seen = False
    for info, data in items:
        if info.filename == entry:
            data, seen = b"{\n}\n", True
        dst.writestr(info, data)
    if not seen:
        dst.writestr(entry, b"{\n}\n")
shutil.move(path + ".new", path)
PYEOF

"$BIN" --datadir "$WORK/datadir" --info "$WORK/empty_config.3mf" > "$WORK/info.txt" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
    echo "FAIL: --info on a project with empty settings exited $rc"
    tail -20 "$WORK/info.txt"
    exit 1
fi
grep -q "size_x" "$WORK/info.txt" || { echo "FAIL: --info printed no geometry"; cat "$WORK/info.txt"; exit 1; }
echo "PASS: a project with empty settings loads"
