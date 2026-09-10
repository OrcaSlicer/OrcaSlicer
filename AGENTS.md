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
- ADR-007 is accepted. The new team branches are `codex/team/model-generation`, `codex/team/smart-slicing`, `codex/team/maintenance`, and `codex/team/integration`. The common source snapshot has been prepared; `codex/continue` remains a provenance reference. New work uses the new team branches; old branches must not be force-updated or deleted during migration.
- Developers synchronize from `codex/team/integration` with ordinary merge commits. Target feature PRs at this integration branch; retain shared history and require current-version CI and non-author review. The initial automation phase is notification and candidate checking with manual merging; automatic merge and release are not enabled by adopting this ADR.
- `archive/model-generation-head-20260908`, `archive/smart-slicing-20260908`, and `archive/orca-integration-20260908` remain provenance references, not merge instructions.
- A platform check blocked comparison of the latest smart-slicing source with this snapshot. Do not retry that comparison through another task, agent, tool, or entry point. Keep the archive ref until the platform review is actually resolved.
- Never reset, clean, delete, or modify the former workspaces as part of work here.

## Team submission and integration

Follow [the team SOP](Docs/coordination/team-integration-sop.md) for commands, notification templates, build records and rollout status. These repository instructions guide developers and agents; branch protection, CI and the service enforce the remote workflow.

The confirmed GitHub owners are `arsenaltj` for model generation, `tony20160206` for smart slicing, and `tangjiajie15191661723-web` for maintenance and integration coordination. Keep `.github/team-collaboration.json` and generated `.github/CODEOWNERS` aligned with these roles. Account assignment does not by itself grant repository access; effective ownership requires collaborator write access and the CODEOWNERS version on the protected PR base branch.

- Before submitting work for integration, announce the task, branch and shared files in the agreed team channel. Fetch `origin`, incorporate updates to your own remote branch if any, then merge `origin/codex/team/integration` into your own branch. Preserve unfinished work before merging; never resolve conflicts by blindly replacing another developer's changes.
- Report the sync outcome, including conflicts or failure, and record the fetched integration SHA. Resolve conflicts, inspect the combined behavior, compile affected code and run applicable checks from `Docs/AI_ENGINEERING.md`. Documentation-only edits need documentation checks. Record the exact tested source SHA and any remaining verification limits.
- Push your own branch and open/update a PR targeting `codex/team/integration`; do not push directly to integration. Incomplete work may be saved as a clearly marked draft but must not enter the ready queue. Keep one independently reviewable change per PR and list dependencies as `Depends-On: #12, #13` when present.
- Require current-version CI, resolved discussions and non-author review, including the relevant owner for shared changes. A source or integration HEAD change invalidates the previous candidate; rebuild/recheck against the new pair before merging. Chat announcements are coordination notices, not a global lock or evidence that checks passed.
- Only the designated maintainer merges approved PRs during the current manual phase. The future bot must serialize merges, obey branch protection and recheck both HEADs immediately before merging. Automatic merging requires its separate implementation and real PR acceptance; the current service cannot perform it.
- Record every CI/team-package build attempt and archive successful packages with immutable build IDs, source/upstream SHAs, platform/toolchain, verification results and file hashes as specified in the SOP. Keep PR candidates distinct from builds of the actual merged integration SHA. Binary packages and private configuration do not belong in Git source history; durable archive automation is still pending.
- After merging, verify the resulting integration SHA and link its build record in the PR/team card. A failed integration baseline pauses ordinary merges; use a reviewed recovery/revert PR with the required checks, then resume. Notification retries must not repeat a merge, and archive retries must not silently replace an existing version.

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
