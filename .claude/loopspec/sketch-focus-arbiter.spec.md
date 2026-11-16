# DELEGATION SPECIFICATION: HARNESS-DRIVEN VALIDATION LOOP
slug: sketch-focus-arbiter · repo: /home/tommaso/projects/apps/orca_cad · branch: cad-mainline

## 1. TARGET GOAL

**Functional Objective.** Keyboard input in the Design tab is routed by WHAT THE KEY IS, not by
which widget the window manager decided to focus. Adopted from FreeCAD's
`DrawSketchKeyboardManager::detectKeyboardEventHandlingMode`
(src/Mod/Sketcher/Gui/DrawSketchKeyboardManager.cpp), which never queries focus at all:

  - digit, `-`, `.`, `,`  -> the open value field
  - Backspace / Delete    -> the open value field (when one is open)
  - Enter / Return / Tab  -> commit the field, control returns to the view
  - a letter              -> the sketch-tool shortcut map, as today
  - Esc                   -> the existing CadLevel LIFO (DesignInteraction.hpp), unchanged
  - anything else         -> sticky: whoever had it keeps it

Observable postcondition: for EVERY sketch tool that opens a value field, a value typed
immediately after the field appears — with NO click into the field — is the value committed.
Today the prefill is committed instead whenever the WM withholds focus.

**Target Files / Scope (writable).**
  src/slic3r/GUI/CAD/DesignPanel.cpp        (the arbiter lives in the existing wxEVT_CHAR_HOOK)
  src/slic3r/GUI/CAD/DesignCanvas.cpp/.hpp  (forwarding entry points only)
  src/slic3r/GUI/CAD/SketchInlineEditor.cpp/.hpp (accept a programmatically delivered character)
  scripts/CAD/check-gui-click-edit.py       (F2P oracle — authoring exception, see §4)
Everything else read-only. No dependency additions, no reformatting.

**Open Bindings.**
  - The in-canvas ImGui field on wip/in-canvas-value-field is NOT in scope. Default: the arbiter
    is implemented against the CURRENT wxFrame field on cad-mainline, because content-based
    routing makes the window's focus irrelevant either way. If it later moves in-canvas the
    arbiter is unchanged.
  - Tools whose field is opened by a toolbar button rather than a gesture (Constrain path) are
    covered by the same arbiter but are not in the F2P tool list. Default: assert them in P2P only.

## 2. HARNESS ENVIRONMENT & GROUND TRUTH

The rig container `orcacad-gui` on nativedev IS the harness. Xvfb `:11` + openbox, the app under
test, `xdotool` for synthetic input, and an MCP socket at `/tmp/mcp.sock` that reports sketch
state as JSON. It is a closed loop: drive input, read geometry back, assert. No window manager
politics, no human.

  Harness interface (ordered; each slot one invocation, one exit code):
    S1 sync    docker cp <file> orcacad-gui:/OrcaSlicer/<path>
    S2 build   docker exec orcacad-gui ninja -C /OrcaSlicer/build orca-slicer
    S3 restart docker exec orcacad-gui /OrcaSlicer/scripts/CAD/start-headless-gui.sh
    S4 F2P     docker exec -e DISPLAY=:11 orcacad-gui python3 /tmp/check-gui-click-edit.py --attach
    S5 P2P     docker exec -e DISPLAY=:11 orcacad-gui python3 /tmp/check-gui-sketching.py

**F2P.** `scripts/CAD/check-gui-click-edit.py`. For each of Line, Rectangle, Circle, Slot,
Polygon, Ellipse and Rounded rectangle: arm the tool, draw it, and type a value that differs
from the prefill WITHOUT clicking the field. Assert the committed value equals the typed value.
The ladder must FAIL against unmodified cad-mainline — that is what proves it asserts something.

**P2P.** `scripts/CAD/check-gui-sketching.py`, the existing gesture ladder, minus anything red at
baseline. NOTE: it calls `focus_field()` — one click into the field before typing — which is the
workaround this whole task removes. It stays green as a regression guard; it is NOT evidence.

**Test Integrity Constraint.** `focus_field()` in check-gui-sketching.py must NOT be deleted to
make things pass, and check-gui-click-edit.py must NOT be weakened. Either invalidates the run.

## 3. VERIFICATION COMMANDS
1. Static: `docker exec orcacad-gui ninja -C /OrcaSlicer/build orca-slicer` (warnings delta only;
   this repo configures no linter — the compiler is the static gate. Absolute-zero is NOT the gate.)
2. Harness: `docker exec -e DISPLAY=:11 orcacad-gui python3 /tmp/check-gui-click-edit.py --attach`
3. Regression: `docker exec -e DISPLAY=:11 orcacad-gui python3 /tmp/check-gui-sketching.py`

## 4. CONVERGENCE LOOP — ceiling 8 iterations
EDIT (scoped) -> EXECUTE S1..S5 -> PARSE the ladder's per-tool assertions and the [UX]/[KEYTRACE]
lines -> PATCH from the parsed cause. On ceiling without convergence: stop, report the last diff
and the unresolved failure set. Do not report success.

F2P authoring exception: check-gui-click-edit.py is writable, and must be shown RED against
unmodified source before any source edit counts.

## 5. TERMINATION CRITERIA
- [ ] S2 exits 0, and introduces no compiler warning absent from the baseline.
- [ ] S4 ALL_PASSED — every tool commits the typed value, no click into the field.
- [ ] S5 shows zero regressions against its recorded baseline pass count.
- [ ] F2P proven red without the fix (source stashed, ladder re-run, must FAIL).

## 6. GUARDRAILS
Zero-assumption: no completion claim without captured stdout and exit codes. Oracle supremacy:
the ladder's verdict overrides my judgement. Blast radius: §1 files only. Baseline obligation:
run §3 once before the first edit and record it.
