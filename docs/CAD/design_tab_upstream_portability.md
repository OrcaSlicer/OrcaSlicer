# Design (CAD) tab — upstream integration brief

**Question:** can the Design tab (sketch-first parametric CAD: sketch → constrain →
extrude/revolve/fillet/hole/thread/shell, multi-body, undo, 3MF persistence) land in
mainline OrcaSlicer?

**Answer: yes, and the ask is far smaller than previously believed.** OCCT is *already*
an OrcaSlicer dependency. We are not asking upstream to adopt a new library; we are
asking it to widen one it already builds, at a measured cost of **3.77 MiB on Windows**.

> ### Corrections to the 2026-06-21 assessment
> That revision was written before the persistence work landed and got two load-bearing
> facts wrong. Both are corrected here from direct measurement of the branch:
>
> 1. **"The real blocker: OCCT … a dependency mainline OrcaSlicer has never carried."**
>    **False.** `deps/OCCT/` exists at the merge-base and upstream links it from
>    `Format/STEP.cpp`, `Format/svg.cpp`, and `Shape/TextShape.cpp`. Our entire
>    dependency diff is **one line**: `BUILD_MODULE_ModelingAlgorithms=OFF → ON`.
> 2. **"vendored SolveSpace solver … LGPL."** **False.** `src/libslic3r/slvs/LICENSE` is
>    **GPL-3.0**, not LGPL. This is fine (see Licensing) but must not be misstated.
>
> It also claimed "no changes to Model" — no longer true; 3MF recipe persistence adds one
> `std::string` to `Model`.

## Measured shape of the change

Against merge-base `449a4cf9fc` (34 commits ahead):

| | files | lines |
|---|---:|---:|
| **New files** | 138 | +61,720 |
| **Modified upstream files** | 23 | +457 / −75 |
| **Deleted upstream files** | 0 | — |

The 62 kLOC headline is inflated by localization. The feature itself:

| area | LOC | files |
|---|---:|---:|
| kernel (`src/libslic3r/`) | 15,828 | 37 |
| GUI (`src/slic3r/`) | 19,544 | 14 |
| tests (Catch2) | 2,567 | 6 |
| i18n (unrelated; strip from the CAD PR) | 23,438 | 77 |

**99.3 % of the diff is new files.** The negotiable surface is 457 added lines across 23
files, and nothing upstream is deleted. The largest single hook is `GLCanvas3D.cpp`
(+110/−2): an `m_design_sketch_tool` member plus render/mouse/key hooks, **every one
already null-guarded** — which is why the compile-time gate below is cheap.

No changes to the slicing pipeline (Print/PrintObject/Layer/GCode), Tab, or the
printer-profile/config system.

## The dependency ask, precisely

Not "adopt OCCT" — **widen the existing OCCT build**:

```diff
-        -DBUILD_MODULE_ModelingAlgorithms=OFF
+        -DBUILD_MODULE_ModelingAlgorithms=ON
```

Cost, measured from the shipped Windows artifact (42 OCCT DLLs, 45.43 MiB total):

| toolkit | size | note |
|---|---:|---|
| `TKFillet.dll` | 2.02 MiB | only exists with the flag ON |
| `TKOffset.dll` | 1.75 MiB | only exists with the flag ON |
| **delta** | **3.77 MiB** | Windows only (OCCT is Shared on Win, Static elsewhere) |

`TKBool` is *not* part of the delta — upstream's `DataExchange` already pulls it in
transitively. On macOS/Linux OCCT links statically, so the cost is only the code actually
referenced, not a 3.77 MiB floor.

**Unmeasured, and we should measure before the call:** clean-deps build-time delta with
the flag ON vs OFF, and the resulting CI runner-minute cost. Do not guess these at him.

## Licensing

- Vendored solver `src/libslic3r/slvs/` — **GPL-3.0**, 9,339 LOC, © Jonathan Westhues,
  a self-contained subset of SolveSpace (`libslvs`). No external dependencies.
- OrcaSlicer — **AGPL-3.0** (`LICENSE.txt`).

GPLv3 §13 expressly permits combining a GPLv3 work with an AGPLv3 work; AGPLv3 §13 grants
the converse. The combined work is distributable under AGPL-3.0 with the solver's GPLv3
terms preserved. This is a favourable direction (GPLv3 → into an AGPLv3 project), but it
is a point to **confirm explicitly with upstream**, not to assert unilaterally.

Open question for SoftFever: keep the solver **vendored** (current: pinned, no submodule,
no external build) or move it to `deps/` as a fetched external? Vendoring costs us
upstream-sync burden; `deps/` costs build complexity.

## The one irreversible decision: the 3MF format

Persistence adds an **optional** archive entry and one field:

```cpp
// Model.hpp
std::string cad_recipe;   // empty for non-CAD projects
```

```
Metadata/orca_cad.bin   // written only when cad_recipe is non-empty
```

Readers that do not know the entry ignore it; writers skip it entirely when empty. So
existing projects are bit-identical and old readers are unaffected. Good.

**But the moment upstream ships this, it owns forward-compatibility forever.** Three
things should be settled *before* the first release, because none can be changed after:

1. **Name.** Renamed to `Metadata/orca_cad.bin`.
2. **Encoding.** The recipe is an opaque **cereal `PortableBinaryArchive`** blob whose
   layout is the field order of `CadFeature::serialize`. Portable across endianness and
   word size — *not* across a field reorder. Append-only is currently a convention held by
   discipline, not by any check.
3. **Embedded BRep.** `Import` features embed OCCT's ASCII BRep for the imported solid,
   which couples saved project files to an OCCT BRep revision. Alternative: re-import from
   the source STEP and store only a reference. Worth deciding deliberately.

**Concrete gap we should close before the call.** `test_caddocument.cpp` covers the
in-memory round-trip and correctly refuses a version-999 blob — but there is **no
checked-in v1 fixture on disk**. A reordered field in `CadFeature::serialize` would pass
the entire suite while silently breaking every previously-saved project. Ship a golden
`.bin` fixture generated today plus a test that loads it; that is the only thing that will
hold the format still once real users have files.

## Proposed PR decomposition

35 kLOC in one PR is not reviewable. Behind the flag, slices 1–4 are behaviour-neutral for
existing users:

1. **Build gate + OCCT flag + Windows packaging guard.** `-DSLIC3R_CAD=ON/OFF`, default
   **OFF**. Flips `ModelingAlgorithms=ON`. Includes the guard that asserts every linked
   OCCT toolkit has a shipped DLL (already on both forks: `546cef5f42`). ← *this is what
   makes SoftFever's "parallel build" a one-line CI matrix entry.*
2. **Vendored `slvs` solver** + its Catch2 tests. No GUI, no OCCT.
3. **CAD kernel** (`CadDocument`, `SketchEngine`, `GeometryEngine`, `Sketch*`) + kernel
   tests. Headless, no GUI.
4. **3MF recipe persistence** + golden-fixture regression test.
5. **GUI Design tab** (`DesignPanel`, `DesignCanvas`, `DesignSketchTool`, `GLGizmoSketch`)
   + the 23 upstream hooks.

## Agenda for the call

Questions only SoftFever can answer:

- Does OrcaSlicer *want* to be a CAD-integrated slicer? (Strategic; everything else is mechanical.)
- Default of `SLIC3R_CAD` at merge time, and when it flips ON.
- Vendored solver vs `deps/` external; and confirmation of the GPLv3/AGPLv3 combination.
- Project-file format: neutral name, encoding, embedded-BRep policy, and who owns v1 forward-compat.
- Undo/redo: the Design tab has its own stack; integrate with Orca's snapshot system or keep separate?
- Does he want the i18n work (Romanian, +23 kLOC) as a wholly separate PR? (Yes, almost certainly.)

## Verdict

Portability **high**. The prior "does upstream want OCCT" framing was wrong — OCCT is
already there. What remains is a 3.77 MiB dependency widening, a compile-time gate that
the existing null-guards make cheap, and one file-format decision that must be made before
the first release rather than after.

---
*Revised 2026-07-10 from direct measurement of `cad-mainline` @ `546cef5f42` vs upstream
merge-base `449a4cf9fc`. Supersedes the 2026-06-21 read-only assessment.*
