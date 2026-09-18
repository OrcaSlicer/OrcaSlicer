# COL-009: Four-color portrait materials and project-default count

- Status: built candidate; user visual acceptance pending, ear-red report still open.
- Feedback: 2026-09-17 screenshot of a four-color portrait shows a white T-shirt incorrectly mapped to pale skin/beige, mottled clothing, and red on the ear. User clarified the garment is white, not beige. The initial trial count should follow current project filaments.
- Baseline: `83ef55a320989f13c04721457f4ae9723893a6d4` plus the complete working-tree checkpoint `.tmp/model-coloring-checkpoints/COL-009-20260917-213528-823449-before-four-color-fix`.

## Before changes

- Confirmed in code: automatic trial count currently comes from the model's source-color histogram, even when the project has a different number of usable filament slots. Four-color semantic matching uses generic color distance; unlike the six-role portrait card it does not distinguish warm skin from a nearby lip-red target or pale neutral clothing from skin-colored filament.
- Hypothesis, not proven root cause of every screenshot pixel: for reliable low-chroma clothing, choose an available neutral candidate rather than a skin-like or lip-like candidate. On the same continuous cloth surface, a short path from reliable neutral clothing may reclaim uncertain neutral clothing faces with compatible source color. For reliable warm skin, avoid a much redder lip-like candidate when a compatible skin candidate exists. Preserve actual red clothing/lips and supported colored or striped garments. Unrelated Unknown regions remain unclaimed.
- Scope: trial count initialization/reset, four-or-fewer-color semantic target choice, and local uncertain-clothing repair. No recognizer or cache-format change.
- Acceptance: default count follows usable project filaments; manual count and restored trials retain their selection; synthetic four-color shirt and ear cases improve without suppressing actual lipstick, red clothing, gray stripes or six-color card behavior. Real-model same-camera comparisons and user visual acceptance remain separate.

## Candidate

| Candidate | Source snapshot | Checks and comparisons | Result |
| --- | --- | --- | --- |
| a | `COL-009-20260917-222504-987503-four-color-neutral-candidate` | 117 focused cases / 2,458 assertions; complete Orca target built. A same-model white T-shirt comparison left 10,159 skin-colored pixels inside clothing labels. | Superseded by local clothing-support candidate. |
| b | final complete snapshot recorded at delivery | 148 focused cases / 2,566 assertions; full Orca target and AI integration check passed. Same-model white T-shirt clothing-label skin pixels 10,636 -> 468; white pixels 65,516 -> 75,674 at 750 x 820. Five other views changed only inside clothing labels. | Built; user screenshot/original project visual acceptance pending. |

## Delivery

- Build/runtime: `build-semantic/runtime-col-009/orca-slicer.exe` SHA-256 `DEC1058697BEFFB1861111EFF6390EA6CF9E91C1BD9B8CB66C9869489025D2D4`; `OrcaSlicer.dll` SHA-256 `ADEEE1504C947B274BF7AABBC00A47332A1D6590FEDEE79C22A33F30A41B9BDF`. The installed native `libmediapipe.dll` SHA-256 is `A8970C645C8C87C25EC9965CB5C898E803C6C42F7192B7DE9A0541C62AE48CEF`. Complete source identity is the final checkpoint cited at delivery, not the base commit alone.
- Same-model comparison: local historical white T-shirt GLB `e096646f-1161-4b5e-902d-15a39ae36fc9`, SHA-256 `434fbfb10fee0db2b75451ce153ea1e2c49f14b432f9985d4642fcff004fa1b4`, 946,280 faces; canonical diagnostic source SHA-256 `30d0a142a73d0d79d04ee8daf5600d47dfbf727f8151cc636b500f074ea412b8`. Reused one native analysis and a fixed four-filament card `#F7E2DA,#282629,#F6F7F9,#EA9A92` at the same cameras. Before: `.tmp/semantic-validation/canonical-gui-validation/col009-shirt-fixed/results/`; candidate: `col009-shirt-supported/results/`. Across six views, every changed pixel was labeled clothing; front-view red pixels 2,214 -> 2,210, all four changes in clothing. The earlier historical Liu Yifei model at `col009-four/results/` is a different model, and cannot establish the screenshot result.
- Automatic four-color suggestions on this historical model were `#262625,#A65C54,#BE9D8C,#DDDBDD`: there is no white filament candidate. Semantic matching never invents a new material; a white project filament/card must be supplied to obtain white output. Initial count now follows non-mixed project slots even when a slot color is unconfigured; invalid project colors still block selecting the project card. No project falls back to the model count, and reset uses that original suggestion rather than the last manual count. Restored manual trials retain their count.
- Tests: `[SemanticMaterialRegions],[SemanticColoring],[ModelPreviewPalette],[ModelSemanticColoring],[ColorTrialState]` passed 148 cases / 2,566 assertions; `python scripts/verify_ai_integration.py --json` returned `ok: true`, no errors. The linker retained its existing `LNK4098` CRT warning. Full Release Orca build and fresh install passed. Main-window count/state flow and screenshot-original ear red were not run or reproduced in this candidate; user visual acceptance and print fidelity remain open. Do not call the ear report fixed based on synthetic skin tests.
- Rollback: restore the baseline complete snapshot in a separate worktree.
