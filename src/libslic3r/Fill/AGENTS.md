# Adding an infill pattern

Everything here is about `src/libslic3r/Fill/`. A new pattern is a `Fill` subclass plus a fixed
set of registration edits. Most of the work is the registration, not the geometry.

## 1. Pick a base class

| Base | Use it when | Examples |
|---|---|---|
| `FillPlanePath` | The pattern is one continuous path generated from the origin outwards, independent of the region shape. You implement `generate()` and get bounding box setup, density scaling, clipping, chaining and rotation for free. | Hilbert, Archimedean Chords, Octagram Spiral, Gosper |
| `Fill` (direct) | The pattern is a field evaluated per layer, or needs the region's geometry. Implement `_fill_surface_single()`. | Gyroid, TPMS, CrossHatch, 3D Honeycomb |
| `FillRectilinear` subclass | The pattern is lines-with-a-twist. Usually just a constructor and a few overridden knobs. | Grid, Triangles, Stars, Zig Zag |

Do not add a new file if an existing one already hosts the family — `FillPlanePath.cpp` holds all
four space-filling-curve patterns. A new `.cpp`/`.hpp` pair must be listed in
`src/libslic3r/CMakeLists.txt` (the `Fill/...` block around line 142).

### The `FillPlanePath` contract

`generate(min_x, min_y, max_x, max_y, resolution, output)` works in **units of the distance between
neighbouring lines**, not in scaled coordinates — the caller multiplies by
`scaled(spacing) * multiline / density` afterwards. So a pattern whose lines sit one unit apart is
automatically correct at every density and line width.

`centered()` decides whether the caller subtracts the bounding box center or its min corner before
calling you. Centered patterns get a box straddling the origin.

`output.add_point()` is **not virtual** — `InfillPolylineClipper` shadows it. That is why every
generator is a template dispatched through `if (output.clips())`. Copy that shape or your points
will silently skip clipping.

## 2. Registration checklist

The pattern will not appear, or will crash at `Fill::new_from_type`, until all of these are done.

| File | Edit |
|---|---|
| `PrintConfig.hpp` | Add `ipYourPattern` to `enum InfillPattern`. **Append before `ipCount`** — see backward compatibility below. |
| `PrintConfig.cpp` | Add `{ "yourpattern", ipYourPattern }` to `s_keys_map_InfillPattern`. |
| `PrintConfig.cpp` | Add `enum_values` + `enum_labels` entries to `sparse_infill_pattern` (~line 3440) and/or `top_surface_pattern` (~line 2286). `bottom_surface_pattern` and `internal_solid_infill_pattern` copy their lists from `def_top_fill_pattern`, so one edit covers all three solid slots. |
| `Fill/FillBase.cpp` | Add a `case` to `Fill::new_from_type`. |
| `Fill/Fill.cpp` | Add a `case` to the exhaustive switch in `Layer::make_fills` (~line 1500). |
| `PrintConfig.hpp` | Add to `is_separable_infill_pattern` if the pattern's origin follows the fill bounding box. Patterns evaluated in absolute coordinates (Gyroid, TPMS, Honeycomb) or shape-relative (Concentric) do **not** belong there. |
| `GUI/ConfigManipulation.cpp` | Add to `have_multiline_infill_pattern` if `params.multiline` is honored. |

**The compiler catches two of these** — `Fill::new_from_type` and the `make_fills` switch both lack a
`default`, so a missing case is a build error. Everything else fails silently at runtime or just
never shows up in the UI. Work the table top to bottom rather than waiting for the build.

`Layer.cpp`'s void-area switch and `PrintObject.cpp`'s bridging-angle switch both have a `default`.
Only add a case there if your pattern genuinely differs from it.

## 3. Get the density constant right

This is the most common silent bug, and a build will not catch it. The pattern must place its lines
**exactly one unit apart** in generator space. Derive it, do not eyeball it:

> filled area / path length = distance between neighbouring passes

For the Gosper curve, each segment owns a hexagonal lattice cell of area `(√3/2)·s²` over a path
length of `s`, giving a spacing of `(√3/2)·s` — so the step had to be `2/√3`, not `1`. A step of `1`
looks perfectly correct on screen and prints ~15% over-dense.

Assert this in a test. It is one line and it is the difference between "the picture looks right" and
"the density slider means something".

## 4. Optional features, and when to opt in

Each of these is a list somewhere that you add your pattern to. Only opt in where the feature is
actually meaningful — a pattern in the wrong list gives the user a control that does nothing.

- **`fill_multiline`** — `ConfigManipulation.cpp`, `have_multiline_infill_pattern`.
- **`center_of_surface_pattern`** — `ConfigManipulation.cpp`, `is_centered_pattern`. For patterns
  whose visual center is meaningful.
- **`top/bottom_surface_fill_order`** — `Fill.cpp` (~line 953) and `ConfigManipulation.cpp`,
  `is_centered_fill`. Only for patterns whose path actually runs radially: Inward/Outward is
  implemented by reordering path fragments from the center out. A centered pattern traversed corner
  to corner (Gosper) must stay out of this list.
- **`sparse_infill_smooth_factor`** — currently Hilbert only; gated in `Fill.cpp` (~line 975).
- **`separated_infills`** — `is_separable_infill_pattern` in `PrintConfig.hpp`.

## 5. Backward compatibility

- **Append the enum value before `ipCount`.** Config and 3mf serialization go through the string
  keys, but renumbering an existing value is not worth the risk for zero gain.
- Changing an existing `L("...")` tooltip invalidates that string in every `.po` catalog. If you
  need to mention your pattern in an existing tooltip, that is a real cost — weigh it.
- Never remove or rename an existing pattern's string key; old projects deserialize by it.

## 6. Verify before you build

A full OrcaSlicer build from cold (deps included) is hours. Pattern geometry is pure math, so check
it outside the build first — a throwaway script in the scratchpad that reimplements the generator
and compares against a reference image, a known point count, or the expected area/length ratio.
That catches a wrong recursion rule or a wrong constant in seconds instead of after a long build.

Beware reference images: SVGs and screenshots round their coordinates, so exact comparisons need a
tolerance well below the pattern's smallest real feature.

Then add tests to `tests/libslic3r/test_fill_plane_path.cpp` (path-based patterns) or
`tests/fff_print/test_fill.cpp` (anything needing a sliced `Print`), tagged `[FillPlanePath]` /
`[Fill]`. See [tests/AGENTS.md](../../../tests/AGENTS.md). Assert the defining property — segment
count, line spacing, coverage of the requested box — not exact coordinates.

```bash
build_release_vs.bat tests
```

```bash
ctest --test-dir build/tests -C Release -R Fill
```

## Worked example

The Gosper curve is the smallest complete example of the whole checklist: `FillGosperCurve` in
[FillPlanePath.hpp](FillPlanePath.hpp) / [FillPlanePath.cpp](FillPlanePath.cpp), plus the
registration edits above and three tests.
