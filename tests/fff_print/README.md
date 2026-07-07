# fff_print test suite

Component- and pipeline-level tests for FFF slicing: the path from a `Model` plus config, through `Print` / `PrintObject`, to emitted G-code.

For Catch2 mechanics (assertions, generators, matchers, random ordering, thread-safety), see [../CLAUDE.md](../CLAUDE.md). This document is the organizing contract for the suite: where a test goes, and how it is named.

## Organizing principle

**One file per subsystem. A subsystem is usually a single production class (`Flow`, `PrintObject`), but may be a cohesive feature that spans several (skirt/brim lives in `Brim.cpp`, `Print.cpp`, and `GCode.cpp`). That file owns every test for the subsystem: in-memory-state assertions and emitted-G-code assertions alike.**

A test's home is decided by *what production code it exercises*, never by *how it observes the result*. A skirt test that inspects `print.skirt()` and one that greps the G-code for `; skirt` live in the same file.

If you touched code in a subsystem, its test file is where your test goes. If a subsystem has no file yet, add `test_<subsystem>.cpp` and list it in `CMakeLists.txt`.

## File ownership

### Building blocks (one class, exercised through its API)

| File | Source (`src/libslic3r/`) | Covers |
|---|---|---|
| `test_trianglemesh` | `TriangleMesh.{c,h}pp` | mesh stats, transforms, slicing, split/merge/cut |
| `test_flow` | `Flow.{c,h}pp` | extrusion width / area math |
| `test_extrusion_entity` | `ExtrusionEntity.{c,h}pp` | extrusion-collection geometry |
| `test_gcodewriter` | `GCodeWriter.{c,h}pp`, `GCode.cpp` | low-level G-code emit primitives, origin |
| `test_model` | `Model.{c,h}pp` | object / volume / instance construction |

### Slicing pipeline (build a `Print`, then assert state or G-code)

| File | Source (`src/libslic3r/`) | Covers |
|---|---|---|
| `test_printobject` | `PrintObject.cpp` | layer heights, perimeter generation |
| `test_fill` | `Fill/` | infill patterns and infill G-code |
| `test_skirt_brim` | `Brim.cpp`, `Print.cpp` | skirt/brim loop counts, grouping, brim ears, emission order |
| `test_support_material` | `Support/` | support & raft layers, contact distance |
| `test_cooling` | `GCode/CoolingBuffer.cpp` | fan control, speed-marker consumption |
| `test_multifilament` | `GCode/ToolOrdering.cpp` | per-feature and per-object filament routing |
| `test_print` | `Print.{c,h}pp` | `validate()`, solid-shell behavior, sequential printing, custom G-code & config comments, default-slice smoke |

Paths are under `src/libslic3r/`. A trailing `/` is a directory of related files; otherwise it is a single class. `{c,h}pp` means the `.cpp`/`.hpp` pair.

## Naming and tags

- **File:** `test_<subsystem>.cpp`.
- **Test name:** a plain behavioral sentence, present tense, stating the contract the test pins down. No `Subsystem:` prefix (the tag carries that).
  - Good: `TEST_CASE("Skirt is emitted once per layer it spans", "[SkirtBrim]")`
  - Avoid: `TEST_CASE("Print: Skirt generation", "[Print]")`
- **Tags:**
  - Exactly one **subsystem** tag, PascalCase, matching the file (`[SkirtBrim]`, `[PrintObject]`, `[Fill]`). This is the grouping / filter key.
  - Optional **cross-cutting** tags for a concern that genuinely spans files (`[validate]`, `[Regression]`).
  - **Status** tags: `[NotWorking]` marks a test disabled for a known, documented reason; CI excludes it via `~[NotWorking]` (it does not hide itself). Use `[.]` to hide a test from default runs entirely. Either way, say why in a one-line comment.

## Test style

Prefer a flat `TEST_CASE` per behavior, with `GENERATE` for parameterized cases and shared setup factored into helpers. The test name carries the behavior, so the BDD scaffolding is usually redundant. Reserve `SCENARIO` / `GIVEN` / `WHEN` / `THEN` for a test with genuine shared setup that branches into a few closely related variations, and never let a `SCENARIO` accumulate unrelated `WHEN`s: that grab-bag is what this contract exists to prevent (and it hides failures behind a single coarse test case).

## Robust tests

A test should fail only when the behavior it names breaks, not from unrelated changes (the "change-detector" anti-pattern). Test behavior, not incidentals, and aim for one reason to fail. Concretely:

- Don't depend on or assert defaults: set the config keys the behavior needs, and derive expected values from those inputs (a 20mm cube at 0.2mm = 100 layers), not from a default that may change.
- Assert the defining property, not an incidental value: prefer "skirt present", "at least 2 brim loops", or "ears vs none" over exact coordinates, extrusion amounts, line counts, or byte sizes.
- Compare floats with a tolerance (`WithinAbs` / `WithinRel`), never `==`.
- Match the meaningful G-code token (`; skirt`), not whole lines, whitespace, or comment wording.
- Rely on ordering only when it is the contract (as `role_sequence` does).
- Keep tests self-contained: no shared state, green under `--order rand`.

## Helpers

Reuse these instead of building a `Print` or parsing G-code by hand.

- **Global** (`tests/test_utils.hpp`, available to every suite):
  - `load_model("file.obj")`: load a `TriangleMesh` from `tests/data/`.
  - `ScopedTemporaryFile`: an RAII temp-file path, removed on scope exit.
- **Suite harness** (`fff_print/test_helpers.{hpp,cpp}`):
  - Build and run: `init_print(...)`, `init_and_process_print(...)`, `slice(...)` (returns the G-code string), and `gcode(print)`.
  - Two-cube placement: `slice_two_cubes_arranged(...)` (arranger-positioned), and `place_two_cubes_apart(...)` / `slice_two_cubes_apart(...)` (a fixed gap, not arranged).
  - Meshes: `cube(size)` / `make_cube(...)` for simple shapes; the `TestMesh` enum with `mesh(...)` for named fixtures.
  - G-code analysis: `layers_with_role(gcode, role)`, `max_z(gcode)`, `role_passes(gcode, role)`, `role_sequence(gcode, roles)`. Subsystem-specific checks stay local (for example `brim_count` in `test_skirt_brim`).

Promote a helper into the suite harness when it is a general test primitive (not tied to one subsystem's logic), even if only one file uses it today; keep genuinely subsystem-specific helpers local (file-static). Reuse potential, not current usage count, is the test.

## Adding a test (checklist)

1. Find the subsystem's file in the tables; create `test_<subsystem>.cpp` if missing.
2. Build the print with a harness helper; set only the config keys the behavior needs.
3. Assert the behavior, in-memory or via parsed G-code, whichever is clearest.
4. Name it as a behavioral sentence and tag it `[Subsystem]`.
5. For a bug fix, add the regression test in the owning file. Name it for the behavior it protects; the test must stand on its own without relying on an external issue or PR for meaning.

## Running

    ctest --test-dir build/tests/fff_print
    build/tests/fff_print/<config>/fff_print_tests --order rand "~[NotWorking]"
