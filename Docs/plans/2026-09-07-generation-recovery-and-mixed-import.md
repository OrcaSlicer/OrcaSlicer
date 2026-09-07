# Generation recovery and mixed import implementation plan

**Goal:** Keep completed AI previews usable after server palette normalization, recover transient image/vision failures without silently regenerating images, and default generated colored models to the native nightly matcher.

**Architecture:** Update presentation state only when the user has not edited the submitted palette. Keep provider recovery in Python; generated-image POSTs are not blindly replayed. Reuse the Plater texture import transaction and recipe engine, preserving explicit manual/physical-only/single-color choices and ordinary Orca imports.

**Tech Stack:** C++17 / wxWidgets / Catch2; Python unittest with mocked HTTP.

**Baseline:** isolated worktree `codex/generation-repair-20260907`, snapshot `65b867ecf7`, includes original working-tree changes. No generation, release, or remote tester state is changed by tests.

## 1. Preview state and color count

- Files: ModelGenerationPanel.cpp, ModelGenerationPresentation.hpp/Core.cpp, test_model_generation_presentation.cpp.
- Test server role updates, repeated polling, genuine user role/color changes, and restoration count consistency.
- Synchronize accepted roles before updating job snapshot; do not reset genuine user changes or quality checks.
- Make changing recommendation count trigger recommendation rather than reusing the previous count's palette, and expose the existing recommendation action.

## 2. Provider and image recovery

- Files: openai_preprocessor.py, printable_reference_visual_quality.py, affected Python tests and GUI status text if required.
- Reproduce TLS/502 vision failures and partial/truncated result downloads with mocks. Bound retries for text/vision only; keep provider selection, TLS checks and image POST ambiguity semantics.
- Bound review-image payload dimensions while preserving source files. Preserve successful images on review failure and report its stage accurately.
- Run affected preprocessor, visual review, diagnostic and pipeline tests; no paid calls.

## 3. Native nightly color matching

- Files: import contract, OrcaWorkspaceAdapter, ModelGenerationPanel, Plater/TextureImportDialog only where necessary; model import / recipe tests.
- Default AI imports use native textured mesh handling for vertex, face and UV colors. Keep the previous import modes selectable.
- Account for cancellation, actual painted filament IDs and native mixed recipe persistence. Per the user's confirmed preference, open the complete native matcher and let the user select mixed colors manually; do not automatically calculate mixtures.
- Preserve normal manual Orca import defaults, slicing activation, physical slots and existing 3MF/profile schema.

## 4. Verification and handoff

- Run targeted Python and C++ tests and compile affected GUI translation units with available Windows dependencies.
- Review the delta from the isolated snapshot, then apply only this task's validated changes to the original workspace after checking that affected files did not drift.
- Record command results and runtime limits. The affected tester is on another computer; local compilation does not prove that remote EXE was updated.

## Completion

- Implemented preview-role synchronization and stable button visibility, bounded analysis/download recovery, and the native matcher default.
- Targeted Python tests: 205 passed. C++ tests: 18 cases / 1,188 assertions passed.
- Four affected Windows GUI translation units compiled successfully; the final panel cleanup is checked separately before handoff.
- See the adjacent verification report for evidence, baseline integration findings and remote tester acceptance limits.
