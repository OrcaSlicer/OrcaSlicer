# AGENTS.md

OrcaSlicer — open-source C++17 3D slicer. wxWidgets GUI, CMake build system.

## Project scope and AI navigation

This fork adds model generation and a separate smart-slicing workbench to OrcaSlicer. For AI work, start with [Docs/AI_ENGINEERING.md](Docs/AI_ENGINEERING.md), then load only the relevant module, decision and verification entry.

- Model generation delivers artifacts; smart slicing owns proposals/trial slicing/application through Orca adapters. Keep provider policy out of `libslic3r` and preserve manual Orca behavior.
- C++ contracts: `src/slic3r/AI/Contracts`; smart slicing: `src/slic3r/AI/SmartSlicing`; desktop adapters: `src/slic3r/GUI/AI`; Python generation: `tools/ai`.
- Runtime versions, integration ownership and architecture budgets: [AI integration lock](docs/architecture/ai-integration-lock.json). Verify with `python scripts/verify_ai_integration.py --json`; do not copy changing values into instructions.
- Project workflows: [model evaluation](.agents/skills/model-generation-evaluation/SKILL.md) for existing artifacts; [Symphony](.agents/skills/symphony/SKILL.md) for task evidence and instruction records. Use them when the task applies.
- Product/color boundaries: [printing and color](Docs/domain/printing-color-boundaries.md). Designs and historical reports do not prove current implementation or print qualification.

## Build Commands

```bash
# macOS
cmake --build build/arm64 --config RelWithDebInfo --target all --

# Linux
cmake --build build --config RelWithDebInfo --target all --

# Windows (replace %build_type% with Debug/Release/RelWithDebInfo)
cmake --build . --config %build_type% --target ALL_BUILD -- -m
```

## Testing

Catch2 framework. Tests in `tests/`; see [tests/AGENTS.md](tests/AGENTS.md) for where a new test belongs and the conventions to follow.

Tests must be enabled in the build. Use the platform-specific commands in `tests/AGENTS.md`; multi-configuration generators need `-C Release`. AI Python tests live in `tools/ai/test_*.py`; choose the affected module or the integration suite via the AI navigation above.

## Code Style

- C++17, selective C++20. PascalCase classes, snake_case functions/variables
- `#pragma once` for headers. Smart pointers and RAII preferred
- Parallelization via TBB — be mindful of shared state
- Always use `SetSizerAndFit(sizer)` instead of `SetSizer(sizer)` on top level window. Unless `SetSizer` must be called before the full layout is built, call `sizer->SetSizeHints(window)` afterwards in this case.

## Key Entry Points

- App startup: `src/OrcaSlicer.cpp`
- Slicing pipeline: `src/libslic3r/Print.cpp`
- All print/printer/material settings: `src/libslic3r/PrintConfig.cpp`
- GUI: `src/slic3r/GUI/`
- Core algorithms: `src/libslic3r/` (GCode/, Fill/, Support/, Geometry/, Format/, Arachne/)
- Printer profiles: `resources/profiles/[manufacturer].json`

## Critical Constraints

- **Backward compatibility required** for .3mf project files and printer profiles
- **Cross-platform** — all changes must work on Windows, macOS, and Linux
- Profile/format changes need version migration handling
- Dependencies built separately in `deps/build/`, then linked to main app

## Code review focus areas

- Changes must not cause regressions in existing functionality, defaults, profiles, or project compatibility.
- Features gated by options must not affect existing behavior when those options are disabled.
- Changes should follow the existing code style and architecture. Architectural changes should be justified in code comments and the PR description.
- Add helper functions or utilities only when existing code cannot reasonably be reused. Avoid duplication.
- Keep code concise and clear. Manually simplify AI generated bloated codes before review.
- Include targeted tests or documented verification for behavior changes, especially in slicing logic, profiles, formats, and GUI defaults.
- For profile changes (`resources/profiles/<Vendor>/**`), check that `version` in the sibling `resources/profiles/<Vendor>.json` was bumped.
- For translation changes (`localization/i18n/**/*.po`), check that recurring terms match the [Localization glossary](https://github.com/OrcaSlicer/OrcaSlicer_WIKI/blob/main/guides/localization_glossary.md) for that language.

## Localization & translations

For translation work, read [localization/AGENTS.md](localization/AGENTS.md) before editing catalogs or regenerating translations.

## Model-generation task coordination

For the user-established model-generation team, read `Docs/coordination/model-generation/README.md` before accepting work. The coordinator maintains the live task registry at `D:/Workspace/06_3DDY_claude/Docs/coordination/model-generation/registry.json`; copied registries in worker worktrees may be older snapshots. This applies only to the model-generation team, not automatically to unrelated projects.

- The coordinator receives requirements, dispatches to the registered owner, reviews results, and hands accepted commits to the existing cross-project integration task.
- Workers operate only in their assigned worktree and role scope. Shared entry points and cross-feature contracts require coordination; a role title alone does not authorize changes outside that scope.
- Initial team setup authorizes environment checks and readiness reports, not open-ended optimization, paid generation, release, or deployment. Follow subsequent concrete assignments and the user's existing authorization.
- Preserve compatibility, existing user work, and the project ownership rules above. Record role-specific progress separately instead of merging concurrent edits to the root planning files.

## Public packages and private test configuration (2026-09-07)

- The user's latest direction is authoritative: public EXE/portable ZIP packages, including anonymous downloads on `3dprint.beer`, contain no provider credentials. Do not use or implement the earlier credential-bearing public-distribution exception for subsequent work.
- Test configuration is provided separately through an access-controlled channel to identified testers. Do not place configuration bundles in public downloads, the website release payload, public source control or ordinary logs/messages.
- Reuse the existing credential-excluding build/package path and inspect the actual EXE and ZIP separately with `release/verify_package_contents.py`. Findings or incomplete inspection block publication; never suppress results or bypass checks.
- Current procedure and available tools: [release/README.md](release/README.md). The [dated authorization record](Docs/coordination/model-generation/internal-test-authorization-20260905.md) retains earlier instructions as history and records the latest supersession; it is not permission to continue the old path.
- Historical published files are not made credential-free by this policy change. Replacement, withdrawal, access changes and credential rotation must identify their actual objects and covered actions; do not claim they occurred without evidence.
- Preserve historical findings and specific unresolved platform-review blocks. Do not disable monitoring, auto-confirm dialogs or replay blocked work. This boundary does not stop unrelated authorized development, tests or documentation; do not ask the user again to accept the withdrawn public-credential arrangement.
