# Design (CAD) tab — upstream portability assessment

**Question:** can the SnapOrca *Design tab* (sketch-first parametric CAD: sketch →
constrain → extrude/revolve/fillet/hole/thread/shell, multi-body, undo) be contributed
to **mainline OrcaSlicer** (2.4 dev) rather than living only in this Snapmaker fork?

**Short answer: yes, technically clean — the feature is self-contained and vendor-neutral.
The single real gatekeeper is whether upstream is willing to take on OpenCASCADE (OCCT) as a
build dependency, i.e. whether OrcaSlicer wants to become a CAD-integrated slicer.**

## Why it is portable

- **Self-contained.** The feature is ~28 kLOC of *new* files (kernel
  `CadDocument` / `SketchEngine` / `GeometryEngine` / `Sketch*`; GUI
  `DesignPanel` / `DesignCanvas` / `DesignSketchTool` / `GLGizmoSketch`) plus a
  vendored, self-contained SolveSpace solver (`src/libslic3r/slvs/`, ~10 kLOC, LGPL).
- **Tiny, guarded injection into shared code (~2–3 % surface).** The only edits to
  pre-existing OrcaSlicer files are: a `m_design_sketch_tool` member + a handful of
  null-checked hooks in `GLCanvas3D` (render overlay, mouse routing, Delete/Esc/Ctrl+Z),
  a tab member + construction in `MainFrame`, and a few forward declarations. **No
  changes** to the slicing pipeline (Print/PrintObject/Layer/GCode), Plater, Tab, Model,
  or the printer-profile/config system.
- **Zero Snapmaker coupling.** No "Snapmaker" references in any Design/CAD file; icons
  are generic `resources/images/design_*.svg`; the code is pure wxWidgets/OpenGL/OCCT.
  It would build and run in any OrcaSlicer fork unchanged.
- **Orthogonal git history.** The feature lives on `feature/cad-primitives` as a clean,
  linear series of `Design:` / `C*` / `M*` commits with no interleaved fork-specific
  work, so it cherry-picks onto a clean OrcaSlicer base without slicing-logic conflicts.

## The real blocker: OCCT

`src/libslic3r/CMakeLists.txt` links ~30 OCCT toolkits (`TKBRep TKFillet TKOffset
TKBool TKPrim TKTopAlgo TKMath TKernel …`) via `find_package(OpenCASCADE REQUIRED)`.
OCCT is large (hundreds of MB of binaries, +15–30 min to a clean deps build) and is a
dependency **mainline OrcaSlicer has never carried**. Accepting it is an architectural
decision about the project's scope, not a code problem.

## Top work items to upstream (≈3–5 dev-days)

1. **Make OCCT an *optional* dependency.** Add it to `deps/` like the other externals,
   gate the whole feature behind a CMake option (`-DENABLE_DESIGN_CAD=OFF` by default),
   and document OCCT install per-platform. Builds without OCCT simply omit the tab.
2. **Guard the tab construction** in `MainFrame` (and the `GLCanvas3D` hooks) on that
   same flag so a minimal build links and runs with no Design code at all.
3. **Tests + docs + license hygiene.** Add regression coverage for the kernel
   (sketch-solve, extrude, fillet, undo), ship the LGPL notice for the vendored
   `slvs/`, and add user docs. (libslvs itself is self-contained, no external deps.)

## Verdict

Portability is **high (≈7/10): "needs moderate adaptation," not "deeply entangled."**
The engineering to upstream is modest and mechanical (optional-dependency plumbing +
tests/docs). The decision is strategic: **does OrcaSlicer want OCCT and a CAD tab?** If
yes, this feature is a near-drop-in starting point.

---
*Generated 2026-06-21 from a read-only analysis of the `feature/cad-primitives` branch.
Tracking issue: bd `snaporca-frp`.*
