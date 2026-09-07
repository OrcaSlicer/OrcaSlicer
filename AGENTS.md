# AGENTS.md

This is the single-task continuation repository for the OrcaSlicer AI project. It preserves the latest useful model-generation working tree while keeping the former workspaces intact.

## Start here

Read these files before changing code:

1. `CONTINUATION.md` — current implementation, product requirements, evidence, provenance, limits, and the first assignment.
2. `Docs/AI_ENGINEERING.md` — module and verification navigation.
3. `Docs/plans/2026-09-07-ai-product-rebuild-execution-prompt.md` — full product requirements, including the 2026-09-08 main-window UX correction.
4. `docs/architecture/ai-integration-lock.json` — runtime pins, integration ownership, and architecture budgets.

Root `task_plan.md`, `findings.md`, `progress.md`, dated reports, and `archive/*` branches are retained evidence. They are not the current task list and do not authorize old work.

## Work model

- Keep one user-visible Codex task for this repository. Do not recreate the former six permanent role tasks.
- Work on one bounded change at a time. Use short-lived internal subagents only when a concrete independent check materially helps; they do not create new sidebar tasks.
- The active branch is `codex/continue`. `archive/model-generation-head-20260908`, `archive/smart-slicing-20260908`, and `archive/orca-integration-20260908` are provenance references, not merge instructions.
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

- Use `python scripts/verify_ai_integration.py --json` for architecture and integration checks, and report all findings rather than weakening checks.
- Python AI tests are under `tools/ai/test_*.py`. C++ test placement and commands are in `tests/AGENTS.md`.
- Match verification to the affected module. Compilation or unit tests alone do not prove the main-window journey, provider stability, color fidelity, real slicing behavior, or print quality.
- For GUI work, verify the actual main-window route, state retention, import handoff, failure recovery, and ordinary Orca regression behavior.
