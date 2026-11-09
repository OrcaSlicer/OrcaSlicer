# Design-tab scripts

Everything here supports the parametric Design tab (`src/libslic3r/CAD/`,
`src/slic3r/GUI/CAD/`). Nothing here is needed to build or run OrcaSlicer — these
are the development and verification tools for that one feature. The kernel's own unit
tests are not here: they are part of `libslic3r_tests` and run under `ctest` like any
other suite.

## Checks against a running app

Three checks drive the Design tab through the app's MCP server, with the same CAD tools an
agent calls, and compare what the app reports with arithmetic. Each needs a running
OrcaSlicer with the MCP server on (`--mcp`, or "MCP server" under AI features in
Preferences) and the CAD feature on (Preferences > Features, read at startup).
`tools/mcp/orca_mcp.py launch --cad` starts one with both. A check finds the app through
its data directory, where the server writes its port and token: the one passed to the check,
else `$ORCA_DATADIR`, else the platform's own (`tools/mcp/README.md` describes attaching).
They run on Windows, macOS and Linux, and exit 0 when every assertion held.

| Check | What it proves | Run it |
|---|---|---|
| `check-mcp-sketch.py` | The sketch layer, asserting what decides whether a profile is buildable: closed loops, offsets that survive, a circle read as a void rather than a second body. | `python3 scripts/CAD/check-mcp-sketch.py [datadir]` |
| `check-sketch-engine.py` | A ladder of 2D sketches of increasing complexity, judged on vertices, lengths, radii, tangency, symmetry and closure rather than on area. | `python3 scripts/CAD/check-sketch-engine.py [datadir]` |
| `check-sketch-engine-corpus.py` | The same ladder graded against real drawings: a systematic sample of the StudyCadCam sheets, each judged against its own vector geometry. | `python3 scripts/CAD/check-sketch-engine-corpus.py --corpus DIR [--datadir DIR] [--step N] [--limit N] [--scale]` |

For example:

    python3 tools/mcp/orca_mcp.py launch --cad --datadir <datadir>
    python3 scripts/CAD/check-sketch-engine.py <datadir>
    python3 tools/mcp/orca_mcp.py stop

The corpus check takes the folder of `MPD*.pdf` sheets, which is not in the repository, and
reads them with poppler's `pdftocairo` and `pdfinfo`, which must be on the PATH. It grades
every 20th sheet by default (`--step`); `--scale` grades the largest drawings instead, each
round trip within `--budget` seconds.

## Generated sources

Two checked-in files are emitted from data that lives here, so the source and the thing
compiled against it cannot drift apart:

| Generator | Source | Output |
|---|---|---|
| `gen_offer_table.py` | `tool_atlas.json` — every offer verb with its row, key, icon, accepted selections and refusal string | `src/slic3r/GUI/CAD/DesignOffer.hpp` |
| `mate-glyph/emit_glyph_table.py` | `mate-glyph/bear_outline.json`, measured off `mate-glyph/bear.step` by `extract_outline.py` | the `kBear…` tables in `DesignSketchTool.cpp` |

`python3 scripts/CAD/gen_offer_table.py` rewrites the header and `--check` proves the
checked-in one matches the atlas. The "Check CAD offer table" workflow
(`.github/workflows/check_cad_offer_table.yml`) runs the check on every push and pull
request that touches the atlas, the generator or the header, which is what makes the
header's "GENERATED — DO NOT EDIT" enforceable. Edit the atlas, never the header.

`mate-glyph/` holds the bear connector the mate glyph is drawn from. `extract_outline.py`
runs in FreeCAD's Python (it reads the STEP with `Part`) and writes `bear_outline.json`.
`emit_glyph_table.py` runs in plain Python and simplifies that outline into the C++ tables,
printing them and writing them to `bear_glyph_table.h` beside it; `DesignSketchTool.cpp`
holds them as emitted.

## GUI checks

`check-gui-sketching.py`, `check-gui-context-menu.py` and `check-gui-click-edit.py` drive
the app the way a person does, with synthetic mouse and keyboard input from `xdotool` on an
X display, and grade what the Design tab did. The display needs a window manager (on Xvfb,
openbox for example): without one no window takes focus, and an unfocused GTK app ignores
synthetic keys.

| Check | What it proves |
|---|---|
| `check-gui-sketching.py` | The engine ladder's profiles drawn the way a person draws them, with mouse gestures and values typed into the in-canvas field. |
| `check-gui-context-menu.py` | Right-click opens the offer, which adapts to what was clicked and shows exactly the verbs `DesignOffer.hpp` says apply. Grades the app's `ORCA_CAD_KEYTRACE=1` trace. |
| `check-gui-click-edit.py` | A value field that opens takes what is typed into it at once, without a click into the field. Grades the app's `ORCA_CAD_UXTRACE=1` trace. |

They read the results back through a local socket the app does not open, so none of them runs
until it reads them through the MCP server like the checks above.
