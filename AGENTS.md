# AGENTS.md

This is the single-task continuation repository for the OrcaSlicer AI project. It preserves the latest useful model-generation working tree while keeping the former workspaces intact.

## Start here

Use the current request to choose the necessary context:

- Read `CONTINUATION.md` when taking over the project or when current scope, provenance or completion status is unclear. Its dated corrections supersede older product descriptions.
- Use `Docs/AI_ENGINEERING.md` to locate an affected module and its verification commands.
- Consult the relevant sections of `Docs/plans/2026-09-07-ai-product-rebuild-execution-prompt.md` for product or main-window UX changes, including its latest dated corrections.
- Read `docs/architecture/ai-integration-lock.json` when changing integration boundaries, contracts, runtime pins, shared touchpoints or architecture budgets. Historical role names in this lock are provenance, not instructions to recreate old tasks.

Do not reread the whole document stack for a small, understood edit. Follow nested `AGENTS.md` when working in its directory.

Root `task_plan.md`, `findings.md`, `progress.md`, dated reports, and `archive/*` branches are retained evidence. They are not the current task list and do not authorize old work.

## Work model

- Keep one user-visible Codex task for this repository. Do not recreate the former six permanent role tasks.
- Complete the requested bounded change through implementation, relevant checks and corrections before handing it back. Local builds, offline tests with verified mocks/fixtures, and isolated local inspection are part of an implementation request; reuse existing authorization.
- Work on one bounded change at a time. Use short-lived internal subagents only when a concrete independent check materially helps; they do not create new sidebar tasks.
- ADR-007 is accepted. The new team branches are `codex/team/model-generation`, `codex/team/smart-slicing`, `codex/team/maintenance`, and `codex/team/integration`. The current continuation checkout remains `codex/continue` until the source snapshot is prepared. New work uses the new team branches; old branches remain historical references and must not be force-updated or deleted during migration.
- Developers synchronize from `codex/team/integration` with ordinary merge commits. Target feature PRs at this integration branch; retain shared history and require current-version CI and non-author review. The initial automation phase is notification and candidate checking with manual merging; automatic merge and release are not enabled by adopting this ADR.
- `archive/model-generation-head-20260908`, `archive/smart-slicing-20260908`, and `archive/orca-integration-20260908` remain provenance references, not merge instructions.
- A platform check blocked comparison of the latest smart-slicing source with this snapshot. Do not retry that comparison through another task, agent, tool, or entry point. Keep the archive ref until the platform review is actually resolved.
- Never reset, clean, delete, or modify the former workspaces as part of work here.

## Product and architecture boundaries

- Model generation and smart slicing are logical modules inside one Orca desktop product. Their principal workflows belong in the Orca main window.
- Orca owns projects, models, profiles, slicing, G-code, and preview. `src/slic3r/AI/Contracts` owns cross-feature contracts; `src/slic3r/AI/SmartSlicing` owns smart-slicing decisions; `src/slic3r/GUI/AI` owns desktop feature hosts and Orca adapters; `tools/ai` owns the Python generation sidecar and provider adapters.
- Keep provider policy out of `libslic3r`. Provider changes must not rewrite the user workflow or Orca core.
- Model import must not silently slice or change presets. Smart-slicing proposals must be comparable, explicitly applied, and undoable.
- Preserve ordinary Orca behavior when AI is disabled, offline, or unavailable.

## Compatibility and delivery

- C++17, wxWidgets, and CMake; keep Windows, macOS, and Linux compatibility.
- Preserve `.3mf`, printer profile, material profile, and manual Orca behavior. Format/profile changes require migration handling.
- Public EXE and portable ZIP packages contain no provider credentials. Private tester configuration remains outside public artifacts, source control, and ordinary logs.
- Do not publish, deploy, push, call paid generation services, or contact testers unless the user explicitly requests that concrete action.

## Verification

- Use `python scripts/verify_ai_integration.py --json` for architecture and integration checks, and report all findings rather than weakening checks. Instruction-only or prose-only edits need document/skill validation; they do not require a full application build or unrelated suites.
- Verify that a selected offline test actually mocks provider access; do not infer isolation from its filename or from available credentials. Paid benchmarks and real provider calls remain separately authorized operations.
- Run checks appropriate to the changed behavior. Once required checks pass, repeat or broaden them only for new changes, failures or unresolved concerns.
- Python AI tests are under `tools/ai/test_*.py`. C++ test placement and commands are in `tests/AGENTS.md`.
- Match verification to the affected module. Compilation or unit tests alone do not prove the main-window journey, provider stability, color fidelity, real slicing behavior, or print quality.
- For GUI work, inspect the affected actual main-window route. When navigation, task state, import or recovery behavior changes, verify state retention, import handoff, failure recovery and relevant ordinary Orca regressions. A copy-only change needs the affected visible state checked, not an unrelated full journey. Full UX acceptance still requires the product journey evidence.
