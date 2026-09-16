# Test suite rules

Rules for writing tests under `tests/`. [CATCH2.md](CATCH2.md) is the Catch2 reference. Building and running the suites is covered on the wiki, at <https://www.orcaslicer.com/wiki/developer_reference/how_to_test.html>.

## The suites

- `libslic3r`: the core library. Geometry, meshes, file formats, config and presets, Clipper, algorithms, data structures.
- `fff_print`: the FFF slicing pipeline, from a `Model` plus config through `Print` and `PrintObject` to emitted G-code.
- `sla_print`: SLA support-tree and pad geometry, support-point generation, raycast.
- `libnest2d`: 2D nesting and packing.
- `slic3rutils`: the Python plugin system and its slicing-pipeline bindings.
- `filament_group`: filament-to-extruder grouping, checked against golden files.

## Building and running

Tests are off by default, so the build has to be told to include them.

- Windows: `build_release_vs.bat tests`, then `ctest --test-dir build/tests -C Release`
- macOS: `./build_release_macos.sh -s -a arm64 -T`, which builds and runs them
- Linux: `./build_linux.sh -t`, then `ctest --test-dir build/tests`

Rebuild a single suite with `cmake --build build --config Release --target <suite>_tests`. Visual Studio and Xcode are multi-configuration generators, so `ctest` needs `-C` there; on Linux it does not.

## Where a test goes

- Pick the suite by the production code the test exercises, not by how the test is written.
- A property of a class that holds with no `Print` involved belongs in `libslic3r`. Behavior that depends on print settings, or produces or consumes G-code or slicing state, belongs in `fff_print`.
- One file per subsystem, named `test_<subsystem>.cpp`. It owns every test for that subsystem, whether the test reads in-memory state or generated output.
- When you add a file, list it in that suite's `CMakeLists.txt` in the same change.

## Use the existing helpers

Check these before writing your own setup or output-parsing code.

- `tests/test_utils.hpp` is shared by every suite. `load_model()` loads a mesh from `tests/data/`, and `ScopedTemporaryFile` gives a temp path that removes itself.
- `fff_print/test_helpers.hpp` builds and slices a `Print` and parses the emitted G-code. Read it before writing an fff_print test rather than assembling a `Print` by hand.
- The other suites have their own: `sla_print/sla_test_utils.hpp`, `libnest2d/libnest2d_test_utils.hpp`, `slic3rutils/plugin_test_utils.hpp`, `filament_group/fg_test_utils.hpp`. `libslic3r` has none and uses the shared header.
- Test data lives in `tests/data/` and is reached through the `TEST_DATA_DIR` define. Wrap it in `std::string(...)` before joining a path onto it.

## Writing the test

- Name the test case as a plain behavioral sentence in the present tense. No `Subsystem:` prefix.
- Tag it with the subsystem it covers, matching the file, in PascalCase. That tag is what people filter on, so every test needs one.
- Add further tags where they help: a narrower one to slice a large file (`[Rotcalip]`, `[Placer]`), a shared one for something spanning files (`[Python]`, `[H2C]`, `[Regression]`), or `[NotWorking]` / `[.]` to disable or hide a test. Say why in a comment if you disable or hide.
- Prefer a flat `TEST_CASE` per behavior, with `GENERATE` for parameterized cases. Reserve `SCENARIO` / `GIVEN` / `WHEN` / `THEN` for genuine shared setup that branches into a few close variations.
- Set the config keys your test depends on, and derive the expected values from what you set. A 20mm cube sliced at `layer_height` 2 is 10 layers, and the test should state both parts. If a number in your assertion comes from a key you never set, the test is also testing that default.
- Assert the defining property, not an incidental value. "Skirt present" or "at least 2 brim loops" survives a refactor; exact coordinates and byte counts do not.
- Name a regression test for the behavior it protects, never for an issue or PR number.
- When asserting on G-code, match the meaningful token such as `; skirt` rather than whole lines, whitespace or comment wording. Depend on ordering only when ordering is the contract.

## Catch2 rules that cause real breakage

- Never reuse a `SECTION` name inside a loop. Use `DYNAMIC_SECTION` so each iteration is unique.
- Never assert from a spawned thread. Catch2 assertions are not thread-safe. Collect results in the thread and assert on the main thread.
- Never combine conditions with `&&` or `||` inside one assertion. Split them so Catch2 can print both operands on failure.
- Compare floats with `WithinAbs` or `WithinRel`, never `==`. Prefer these over `Approx` in new tests.
- Keep tests self-contained: no shared state, green under `--order rand`.
