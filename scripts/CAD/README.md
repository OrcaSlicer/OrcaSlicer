# Design-tab scripts

Everything here supports the parametric Design tab (`src/libslic3r/CAD/`,
`src/slic3r/GUI/CAD/`). Nothing here is needed to build or run OrcaSlicer — these
are the development and verification tools for that one feature. The kernel's own unit
tests are not here: they are part of `libslic3r_tests` and run under `ctest` like any
other suite.

## Checks against a running app

Three checks drive the Design tab through the app's MCP server, with the same Design commands an
agent calls, and compare what the app reports with arithmetic. Each needs a running
OrcaSlicer with the MCP server on (`--mcp`, or "MCP server" under AI features in
Preferences) and the CAD feature on (Preferences > Features, which takes effect after a restart).
`tools/mcp/orca_mcp.py launch --cad` starts one with both. A check finds the app through
its data directory, where the server writes its port and token: the one passed to the check,
else `$ORCA_DATADIR`, else the platform's own (`tools/mcp/README.md` describes attaching).
They run on Windows, macOS and Linux, and exit 0 when every assertion held.

| Check | What it proves | Run it |
|---|---|---|
| `check-mcp-sketch.py` | The sketch layer, asserting what decides whether a profile is buildable: closed loops, offsets that survive, a circle read as a void rather than a second body. | `python3 scripts/CAD/check-mcp-sketch.py [datadir]` |
| `check-sketch-engine.py` | A ladder of 2D sketches of increasing complexity, judged on vertices, lengths, radii, tangency, symmetry and closure rather than on area. | `python3 scripts/CAD/check-sketch-engine.py [datadir]` |
| `check-sketch-engine-corpus.py` | The same ladder graded against real drawings: a systematic sample of the StudyCadCam sheets, each judged against its own vector geometry. | `python3 scripts/CAD/check-sketch-engine-corpus.py --corpus DIR [--datadir DIR] [--step N] [--limit N] [--scale] [--budget S]` |

For example:

    python3 tools/mcp/orca_mcp.py launch --cad --datadir <datadir>
    python3 scripts/CAD/check-sketch-engine.py <datadir>
    python3 tools/mcp/orca_mcp.py stop

The corpus check takes the folder of `MPD*.pdf` sheets, which is not in the repository, and
reads them with poppler's `pdftocairo` and `pdfinfo`, which must be on the PATH. It grades
every 20th sheet by default (`--step`); `--scale` grades the largest drawings instead, each
round trip within `--budget` seconds.

The Design tab's GUI tests, which draw on the canvas with the mouse and keyboard as a person
does, are MCP harness tests in `tools/mcp/` (`test_cad_sketching.py` and
`test_cad_context_menu.py`); `tools/mcp/test-entries/design.md` says
what they check, and `tools/mcp/README.md` how to run them.

## Generated sources

Two checked-in files are emitted from data that lives here, so the source and the thing
compiled against it cannot drift apart:

| Generator | Source | Output |
|---|---|---|
| `gen_offer_table.py` | `tool_atlas.json` — the Design tab's table: every offer verb with its row, key, icon, accepted selections and refusal string, and every Design command's silent form (a `tool` block) | `src/slic3r/GUI/CAD/DesignOffer.hpp` |
| `mate-glyph/emit_glyph_table.py` | `mate-glyph/bear_outline.json`, measured off `mate-glyph/bear.step` by `extract_outline.py` | the `kBear…` tables in `DesignSketchTool.cpp` |

The atlas's `verbs` are one table for three things: the right-click menu (a verb with a `slot` has an
item there), the Speed Dial's Design tab actions and the Design commands. Every verb but a folded or a
greyed item (`fold`; `pat_curve`, `align_to`) is a command whose key is its `id`. A verb with a handler
(`action`) does what its menu item does when called
with no arguments, in the mode the item belongs to, and has an action of its own in the Speed Dial; one
with a `tool` block also runs silently
with arguments, and the block holds what that form takes and does: its `access` (`read`, `change` or
`sketch`, which decides what a busy Design tab lets through), `ok_false` (it reports a rejected rebuild
in its reply), a `summary` of one sentence,
`details`, its `params` (each with `name`, `type`, `unit`, `default`, limits `min`, `exclusive_min` and
`max`, `enum` and `description`; one with no default is required, and one whose default is null also takes
null, unless `nullable` is false, which marks a param whose function reads a null as given), one to three
`examples` and `ids`
(it takes or gives face and edge ids). Entries with no `slot` and no handler are commands only, and
`modal` marks a verb whose handler opens a dialog. `src/slic3r/GUI/Mcp/Tools/Design*.cpp` bind each block
to the function that runs it (`silent_forms()`), and a unit test fails when a block has no function or a
function no block. The generator refuses an entry with no slot that has neither a handler nor a block.

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
