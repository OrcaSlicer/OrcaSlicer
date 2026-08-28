#!/usr/bin/env bash
# Every ladder, in one command, as the gate before a push that touched the Design tab.
#
# WHY A SCRIPT AND NOT CI. Three of the four rungs need a running application with an OpenGL
# canvas and synthetic input; GitHub's runners have neither. So the gate is local and explicit:
# run this, read the last line, and do not push a red one. The kernel suite is the only part CI
# can carry, and it already does.
#
#   scripts/ladder-all.sh                 # kernel + engine + corpus (every 20th) + gestures + offer
#   FULL=1 scripts/ladder-all.sh          # corpus over ALL 997 sheets (~25 min)
#   SKIP_GUI=1 scripts/ladder-all.sh      # kernel only, for a machine with no rig
#
# The rig container is expected to be up with the app running and SNAPORCA_MCP set; bring it up
# with scripts/gui-session.sh inside it. The corpus lives at /corpus in that container.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 1

C="${C:-snaporca-gui}"
CORPUS="${CORPUS:-/corpus}"
STEP="${STEP:-20}"
[ -n "${FULL:-}" ] && STEP=1
fail=0

step() {
    local name="$1"; shift
    echo
    echo "=== $name ==="
    if "$@"; then echo "--- $name OK"; else echo "--- $name FAILED"; fail=1; fi
}

# SC2329: every call goes through step(), which invokes it via "$@", so shellcheck
# cannot see the callers below.
# shellcheck disable=SC2329
run_in_rig() {                      # copy the script in fresh, then run it there
    docker cp "$1" "$C:/tmp/$(basename "$1")" >/dev/null || return 1
    shift
    docker exec "$C" python3 "$@"
}

# FIRST, and it needs no rig: the offer table the menu is compiled from must be what the atlas
# says. The header calls itself GENERATED and had been hand-edited anyway — which cost four rows
# that existed only in the header, one row wired to the wrong action, and a count of 91 for a
# 92-row array, so the last verb was unreachable (snaporca-z8rs, snaporca-ziam).
step "offer table matches the atlas" python3 docs/ux/mockups/gen_offer_table.py --check

step "kernel suite" scripts/kernel-test.sh --vol "${KVOL:-snaporca_kerneltest}"

if [ -z "${SKIP_GUI:-}" ]; then
    step "engine ladder (rungs 1-8, scripted geometry)" \
        run_in_rig scripts/sketch-ladder.py /tmp/sketch-ladder.py
    step "corpus rung (real drawings, every ${STEP}th)" \
        run_in_rig scripts/ladder-corpus.py /tmp/ladder-corpus.py --corpus "$CORPUS" --step "$STEP"
    step "corpus scale rung (the heaviest sheets)" \
        run_in_rig scripts/ladder-corpus.py /tmp/ladder-corpus.py --corpus "$CORPUS" --scale
    step "gesture ladder (mouse and keyboard)" \
        run_in_rig scripts/gui-ladder.py /tmp/gui-ladder.py
    # The offer ladder needs TWO extra things the others do not: the app must have been launched
    # with SNAPORCA_KEYTRACE=1 (its [OFFER] lines are the whole instrument), and it reads the
    # generated offer table to predict what each selection should show — which is not in the
    # container's own baked source tree, so it is copied in beside the script — /tmp, where
    # run_in_rig puts the script, is one of the paths the ladder looks in.
    docker cp src/slic3r/GUI/CAD/DesignOffer.hpp "$C:/tmp/DesignOffer.hpp" >/dev/null
    step "offer ladder (right-click, the menu, the verbs behind it)" \
        run_in_rig scripts/offer-ladder.py /tmp/offer-ladder.py
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL LADDERS HELD"; else echo "AT LEAST ONE LADDER FAILED"; fi
exit "$fail"
