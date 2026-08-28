# Design-tab scripts

Everything here supports the parametric Design tab (`src/libslic3r/CAD/`,
`src/slic3r/GUI/CAD/`). Nothing here is needed to build or run OrcaSlicer — these
are the development and verification tools for that one feature.

The verb in the name is the role:

| | |
|---|---|
| `build-…`  | produce a binary |
| `start-…`  | bring something up and leave it running |
| `run-…`    | run a suite and report pass/fail |
| `check-…`  | one specific assertion, usually driving a live app |

## Verification

| Script | What it proves | Needs |
|---|---|---|
| `run-kernel-tests.sh` | The CAD kernel builds and the Catch2 `[CadDocument]` tags pass — every case builds a document, recomputes it and asserts on real geometry. **Exit 0 is the verification contract.** | Docker only. No display. |
| `run-all-checks.sh` | Every check below, in one command. The gate before pushing a Design-tab change. | Docker + the GUI container |
| `check-sketch-engine.py` | A ladder of 2D sketches of increasing complexity, judged on loop count, closure and void attribution rather than on area. | Kernel only |
| `check-sketch-engine-corpus.py` | The same ladder graded against a systematic sample of real drawings instead of shapes we chose. | Kernel + corpus |
| `check-gui-sketching.py` | The same profiles drawn the way a person draws them — synthetic mouse gestures and typed values. | Headless GUI |
| `check-gui-context-menu.py` | That right-click is the pivot of the design gesture, and adapts to what was clicked. | Headless GUI |
| `check-mcp-sketch.py` | The sketch layer driven over the MCP socket, asserting what decides whether a profile is buildable. | Headless GUI + `SNAPORCA_MCP` |

**`run-kernel-tests.sh` is the only one CI can run.** The rest need a live
application with an OpenGL canvas and synthetic input, which hosted runners do not
have. The kernel suite itself is already in CI by an ordinary route: the cases are
registered in `tests/libslic3r/CMakeLists.txt` under `if (SLIC3R_CAD)`, so they are
part of `libslic3r_tests` and run under `ctest` on every platform like any other
unit test. This script exists for the local loop, where it is a two-minute round
trip instead of a full application build.

## Build and run

| Script | Purpose |
|---|---|
| `build-gui.sh` | Build the GUI binary in a throwaway container, writing into the build-cache volume the long-lived GUI container reads. |
| `build-gui-incremental.sh` | Incremental build against the deps-baked image, for a fast edit/compile loop. |
| `start-headless-gui.sh` | Bring the app up on a headless X display (Xvfb + a window manager), ready to drive or attach to over VNC. |

Two constraints that are not obvious and have each cost a session:

- **Never build inside the GUI container.** Its baked source tree silently
  reconfigures the shared build directory and this fork's targets vanish.
- **A window manager is required.** Without one, windows are never focused, and an
  unfocused GTK app ignores synthetic keys — which looks exactly like a code bug.

`docs/rig_build_traps.md` documents these and three more, with symptoms and exact
recovery commands. Read it before debugging a configure or link failure one of
these scripts reports.
