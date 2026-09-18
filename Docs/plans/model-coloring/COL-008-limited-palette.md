# COL-008: Enable portrait-region optimization for one to five colors

- Status: built; user visual acceptance pending.
- Feedback: 2026-09-17, one-to-five-color portrait trials are not usable as expected.
- Baseline: `83ef55a320989f13c04721457f4ae9723893a6d4` plus the complete working-tree snapshot in `.tmp/model-coloring-checkpoints/COL-008-20260917-211851-975633-before-1-to-5-colors`.

## Before changes

- Observed in code: semantic mapping accepts one through six candidates. The six-color portrait card has specialized roles; other palettes use the generic matcher. When a trial exceeds six colors, the UI forcibly unchecks optimization, so returning to one through five colors leaves it off.
- Hypothesis: preserve the user's checkbox intent while optimization is temporarily unavailable above six colors, and explicitly test the one-through-five-color mapping path. Do not fabricate missing materials or increase the requested color budget.
- Scope: trial controls and focused regression tests. Preserve manual edits, six-color behavior, unknown-region fallback, and the six-candidate runtime limit.
- Acceptance: each count from one through five reaches the semantic coordinator, maps only to supplied colors, reuses recognition across palette changes, and one color never creates a second material. Visual/model-region quality remains for user acceptance.

## Candidate

| Candidate | Source snapshot | Checks and comparison | Status |
| --- | --- | --- | --- |
| a | complete working tree, checkpoint below | 41 focused cases / 1,864 assertions; 78 semantic-domain cases / 648 assertions; integration check passed | built, visual acceptance pending |

## Delivery

- Build: `cmake --build build-semantic --config Release --target slic3rutils_tests --parallel 4` and `--target OrcaSlicer` passed. Existing linker emitted `LNK4098` CRT warning. Installed to `build-semantic/runtime-col-008` with `cmake --install`.
- Installed `OrcaSlicer.dll` SHA-256: `4642060dd8775616ef04b11cbdcea2b9727aefa0732664b97b58795432daaf8b`. Launcher: `build-semantic/runtime-col-008/orca-slicer.exe`. Native runtime and pinned Python sidecar files are present.
- Automated checks: `[ModelSemanticColoring],[ColorTrialState],[ModelPreviewPalette]` passed 41 cases / 1,864 assertions; `[SemanticColoring]~[ModelSemanticColoring]` passed 78 cases / 648 assertions; `python scripts/verify_ai_integration.py --json` returned `ok: true` and no errors. An initial test-only `DYNAMIC_SECTION` mistake failed the cache assertion because it recreated the coordinator; the loop was corrected and rerun green.
- Model and camera comparison: no fixed real-model same-view visual capture in this round. Six-color algorithm unchanged; one-to-five candidate counts exercised with mocked recognizers, including one-color reuse and recognition cache across count changes.
- Actual main-window flow and print fidelity: not executed; user acceptance pending. This is not a claim of visible color quality improvement.
- Final complete source snapshot: generated after this record; see delivery note for the exact checkpoint path.
- Rollback: restore the above complete source snapshot in a separate worktree.
