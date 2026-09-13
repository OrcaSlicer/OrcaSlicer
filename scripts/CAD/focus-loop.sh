#!/usr/bin/env bash
# One turn of the keyboard-focus convergence loop, start to verdict, with no human in it.
#
#   scripts/CAD/focus-loop.sh              # full turn: sync -> build -> restart -> assert
#   SKIP_BUILD=1 scripts/CAD/focus-loop.sh # re-assert against the binary already on the host
#
# Exit 0 only when every gate holds. Any other exit is a failing gate and names which.
#
# WHY THIS EXISTS. The focus defects in the Design tab were chased for days by hand: build, launch
# the GUI, drive it with xdotool, read a screenshot, guess, repeat. That needs a person at every
# step and it is where the days went. This does not: behemoth carries an agent-owned Xvfb :10 with
# openbox, the app, xdotool and an MCP socket that reports sketch state as JSON, so a turn is
# sync -> build -> restart -> assert, and the ASSERTION is the verdict, not my reading of a picture.
#
# WHY BEHEMOTH AND NOT THE orcacad-gui RIG CONTAINER. The rig was the obvious host and it does not
# work for this: its image pins a dependency set 216 non-CAD source files behind cad-mainline
# (assimp among them), so today's CAD sources call GUI_App::is_auto_close_sketch_loops and
# MainFrame::ensure_design_panel, which that tree has never heard of. Syncing all of src/ to fix
# that needs a deps rebuild measured in hours. behemoth already builds this exact tree, already
# runs a WM on :10, and is the machine the user actually runs the product on — so the loop asserts
# against the shipping artefact rather than a stale twin. Reviving the rig means rebuilding its
# deps image first; until then it cannot adjudicate anything about this code.
set -uo pipefail

HOST="${HOST:-tommaso@100.103.234.2}"
DISP="${DISP:-:10}"
SRC="${SRC:-\$HOME/projects/orca/orcacad-native/src}"
TRACE="${TRACE:-/tmp/ux-focus-loop.log}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN="build/src/Release/orca-slicer"

say() { printf '\n=== %s\n' "$*"; }
die() { printf 'GATE FAILED: %s\n' "$*" >&2; exit 1; }

ssh -o ConnectTimeout=10 "$HOST" true || die "cannot reach $HOST"

# ---------------------------------------------------------------- S1 sync
# Only the CAD paths and the ladders. behemoth's tree is a full cad-mainline checkout kept in step
# by its own realign; pushing unrelated files from here would make the build host disagree with
# git for reasons no later session could reconstruct.
say "S1 sync"
rsync -q "$REPO"/src/slic3r/GUI/CAD/*.{cpp,hpp} "$HOST:$SRC/src/slic3r/GUI/CAD/" || die "sync GUI/CAD"
rsync -q "$REPO"/src/libslic3r/CAD/*.{cpp,hpp}  "$HOST:$SRC/src/libslic3r/CAD/"  || die "sync libslic3r/CAD"
rsync -q "$REPO"/scripts/CAD/check-gui-click-edit.py "$REPO"/scripts/CAD/check-gui-sketching.py \
        "$HOST:/tmp/" || die "sync ladders"
echo "  sources + ladders in place"

# ---------------------------------------------------------------- S2 build
# flock: two concurrent Orca builds once OOM'd this machine for 2h28m. Every build script on the
# fleet takes this same lock.
#
# Grade the BINARY'S TIMESTAMP, never the build command's exit code. This is a Ninja Multi-Config
# tree whose default rules are Debug while the artefact under test is Release, so a wrong-config
# invocation returns success in seconds having touched nothing — it cost a wasted cycle here
# before anyone thought to look at the file.
if [ -z "${SKIP_BUILD:-}" ]; then
    say "S2 build"
    before=$(ssh "$HOST" "stat -c %Y $SRC/$BIN 2>/dev/null || echo 0")
    ssh "$HOST" "flock /tmp/orca-rig-build.lock \$HOME/projects/orca/orcacad-native/rebuild.sh > /tmp/focus-build.log 2>&1"
    rc=$?
    after=$(ssh "$HOST" "stat -c %Y $SRC/$BIN 2>/dev/null || echo 0")
    if [ "$rc" != 0 ] || [ "$after" = "$before" ]; then
        ssh "$HOST" "grep -m5 -B2 'error:' /tmp/focus-build.log; tail -5 /tmp/focus-build.log"
        die "S2 build (exit $rc, binary $( [ "$after" = "$before" ] && echo unchanged || echo rebuilt ))"
    fi
    echo "  built"
fi

# ---------------------------------------------------------------- S3 F2P
# The ladder launches and tears down the app itself, in its own datadir, so nothing here has to
# manage a process. It types WITHOUT clicking the field first, which is the whole contract.
say "S3 fail-to-pass: type without clicking the field"
ssh "$HOST" "cd $SRC && DISPLAY=$DISP python3 /tmp/check-gui-click-edit.py \
             --display $DISP --bin $BIN --trace $TRACE"
f2p=$?

# ---------------------------------------------------------------- S4 P2P
say "S4 pass-to-pass: the existing gesture ladder"
ssh "$HOST" "cd $SRC && DISPLAY=$DISP python3 /tmp/check-gui-sketching.py 2>&1 | tail -3"
p2p=$?

say "VERDICT"
[ "$f2p" = 0 ] || die "F2P: a tool did not take the typed value (exit $f2p)"
[ "$p2p" = 0 ] || die "P2P: the gesture ladder regressed (exit $p2p)"
echo "ALL GATES HELD"
