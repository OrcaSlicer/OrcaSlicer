#!/usr/bin/env bash
# The MCP + GUI checks of the SolidCraft epic, run against a RUNNING app on the behemoth rig (agent Xvfb :10, MCP socket
# /tmp/orca-test.sock). run-all-checks.sh is the older docker gate; this one needs no container: copy the scripts across, run
# them in order, print one line each and exit non-zero if any failed.
#
#   scripts/CAD/run-rig-checks.sh                 # every check
#   ONLY="undo sheet" scripts/CAD/run-rig-checks.sh   # a subset, by name
#   (the app needs ORCA_CAD_MCP_UI=1 for the ui.* drivers)
#   HOST=tommaso@100.103.234.2 SOCK=/tmp/orca-test.sock DISPLAY_RIG=:10
#
# The app must be up (see the orca-cad-rig skill). Each check starts from new_document and leaves the tab clean; between them the
# harness answers any modal dialog the app raised.
# The remote commands are meant to expand HOST, SOCK and DSP here, on the client.
# shellcheck disable=SC2029
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 1
HOST="${HOST:-tommaso@100.103.234.2}"
SOCK="${SOCK:-/tmp/orca-test.sock}"
DSP="${DISPLAY_RIG:-:10}"
fail=0

# name -> script (one check per line: oracle first, it is the kernel's measured truth)
CHECKS=(
    "oracle:oracle_runner.py"
    "hostile:check-mcp-hostile.py"
    "commands:check-mcp-commands.py"
    "api-gaps:check-mcp-api-gaps.py"
    "undo:check-undo.py"
    "cards:check-ui-card-features.py"
    "sheet:check-ui-sheet.py"
    "tree-multi:check-ui-tree-multi.py"
    "timeline:check-ui-timeline.py"
    "constraint-verbs:check-ui-constraint-verbs.py"
    "sketch-draw:check-ui-sketch-draw.py"
    "drag-snap:check-ui-drag-snap.py"
    "dim-origin:check-ui-dim-origin.py"
    "sketch-undo:check-ui-sketch-undo.py"
    "sketch-include:check-ui-sketch-include.py"
    "sketch-split:check-ui-sketch-split.py"
    "guide-tags:check-ui-guide-tags.py"
    "select-panel:check-ui-select-panel.py"
    "scenarios:check-ui-scenarios.py"
)

ssh "$HOST" 'mkdir -p /tmp/orc/scripts/CAD' || exit 1
scp -q scripts/CAD/*.py "$HOST:/tmp/orc/scripts/CAD/" || exit 1
ssh "$HOST" 'mkdir -p /tmp/orc/tests/cad_scenarios' && scp -q tests/cad_scenarios/*.json "$HOST:/tmp/orc/tests/cad_scenarios/" || exit 1
ssh "$HOST" "test -S $SOCK" || { echo "no MCP socket $SOCK on $HOST: start the app first (orca-cad-rig skill)"; exit 2; }

for entry in "${CHECKS[@]}"; do
    name="${entry%%:*}"; script="${entry#*:}"
    if [ -n "${ONLY:-}" ] && ! [[ " $ONLY " == *" $name "* ]]; then continue; fi
    [ -f "scripts/CAD/$script" ] || { echo "SKIP $name ($script missing)"; continue; }
    ssh "$HOST" "DISPLAY=$DSP bash /tmp/dis.sh" >/dev/null 2>&1
    if [ "$script" = oracle_runner.py ]; then
        out=$(ssh "$HOST" "cd /tmp/orc && ORCA_CAD_MCP=$SOCK python3 scripts/CAD/$script 2>&1" | tail -1)
        case "$out" in *"FAIL 0"*"BLOCKED 0"*) rc=0 ;; *) rc=1 ;; esac
    else
        out=$(ssh "$HOST" "cd /tmp/orc && DISPLAY=$DSP timeout 600 python3 scripts/CAD/$script $SOCK 2>&1" | tail -1)
        rc=$?
        # the last line of every check is its verdict: "... PASS" / "... FAIL (n)"
        case "$out" in *PASS*) rc=0 ;; *) rc=1 ;; esac
    fi
    if [ "$rc" -eq 0 ]; then echo "ok    $name: $out"; else echo "FAIL  $name: $out"; fail=1; fi
done
echo
[ "$fail" -eq 0 ] && echo "ALL RIG CHECKS HELD" || echo "AT LEAST ONE RIG CHECK FAILED"
exit "$fail"
