# Design (CAD) tab — upstream pull request

## What this adds

A sketch-first parametric CAD tab inside the slicer. The workflow is direct:
sketch → constrain → solid features → commit to plate. The whole feature recipe is
persisted inside the 3MF, so reopening restores an editable model rather than a frozen mesh.

- Kernel: OCCT, which upstream already links for STEP import — see
  [cad_dependency_weight.md](docs/cad_dependency_weight.md)
- Constraint solver: vendored SolveSpace `libslvs` subset
- Interaction model: object-driven — point at geometry, the geometry offers the verbs that
  apply to it; see [cad_ux_guidelines.md](docs/cad_ux_guidelines.md)
- Full user-facing documentation: [design_tab.md](docs/design_tab.md)

## Why it belongs in the slicer

Every round trip through an external CAD tool costs a file export, a re-import, and the
design intent that both steps discard. A part modified after slicing should return to its
feature history, not to a mesh. Keeping the CAD model inside the slicer preserves that
loop — the nozzle diameter, the build volume and the material are known at design time.

For the integration case in full: [design_tab_upstream_portability.md](docs/design_tab_upstream_portability.md).

## How it is built

The `SLIC3R_CAD` CMake flag (default ON) gates the entire tab. With it OFF the tab is not
compiled and the deps prefix matches upstream exactly — the dependency diff is one line in
OCCT's CMake: `BUILD_MODULE_ModelingAlgorithms=OFF → ON`.

Measured cost table: [cad_dependency_weight.md](docs/cad_dependency_weight.md).

## Diff shape

<!-- fork-specific: measured against this fork's upstream base; re-run the commands above after mirroring -->

Against merge-base `d6cb667b894f`:

 306 files changed, 83032 insertions(+), 777 deletions(-)

350 commits, of which 284 are new files and 37 modify upstream files. 99.3 % of the diff
is new code. The negotiable surface is the 37 modified files.

## Tests

205 `TEST_CASE` blocks across 6 new test source files. This counts assertions written, not
assertions passed — a run needs a build.

`scripts/kernel-test.sh` is the headless verification contract: it builds only
`libslic3r_tests` (not the GUI app), needs no display, and exit 0 means the CAD suite
passed. It now runs with **no exclusions** — both cases that used to be quarantined (the
circle-line tangency solver abort and the internal-thread reference) are fixed.

## Licensing

The vendored solver in `src/libslic3r/slvs/` is **GPL-3.0** (see `src/libslic3r/slvs/LICENSE`),
not LGPL. The combined work is distributable under AGPL-3.0. See the Licensing section of
[design_tab_upstream_portability.md](docs/design_tab_upstream_portability.md) for the
AGPLv3/GPLv3 compatibility argument; this point should be confirmed with upstream explicitly.

## Not verified

- Card wiring for 9 of the 16 late-wired tools was never click-tested.
- There is no automated GUI test in CI. A green kernel run says nothing about the GUI —
  synthetic clicks never drift, so the test suite and the viewport are two separate realities.
- The click-test defect rate has **not converged**: a second pass found no new defects, but
  four further days of work found five more. The earlier pass is not evidence of stability.

## Reviewer's map

See the [Where the code lives](docs/design_tab.md#where-the-code-lives) table in the user
doc for the file-to-role mapping, and [docs/ux/tool_atlas.json](docs/ux/tool_atlas.json) as
the generated-from source of `src/slic3r/GUI/DesignOffer.hpp`.
