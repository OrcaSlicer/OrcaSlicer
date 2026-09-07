# ADR-003: Keep AI integration on the official Orca Git lineage

**Status:** Accepted
**Date:** 2026-08-21
**Upstream baseline:** `6ef02a67dbb22ae1a019d9f485f46bfc3e1b44aa`

**Historical locked upstream (2026-08-29):** `db29f570bd2f77742ab04e0bb8f0aa55237bd70a`; the original baseline above remains the branch-creation record. The current pinned SHA is maintained in [ai-integration-lock.json](ai-integration-lock.json), under `upstream.sha`.

## Context

Model generation and smart slicing are developed and accepted independently. The earlier development repository was created from an aggregate source import and has no Git merge base with the official Orca `upstream/main` history. Directly merging or rebasing that history cannot provide a reliable long-term upstream update path.

The product must preserve Orca's native Model, Config, 3MF/profile, slicing and Preview behavior. AI features must remain optional, cross-platform and independently testable. Changes to high-churn Orca GUI composition files must stay small and auditable.

## Decision

1. `codex/orca-integration-v2` is the long-lived AI product integration line and is created directly from the pinned official upstream commit above.
2. The official `upstream/main` reference remains read-only. Shared integration history receives upstream changes through explicit merge commits; published product history is not rebased.
3. Model generation and smart slicing continue on independent branches. Integration consumes an exact user-accepted commit SHA, never a moving branch head.
4. Feature-owned code is ported before shared Orca touchpoints. Shared `MainFrame`, `Plater`, GUI build registration and navigation changes are owned by the integration line.
5. The two feature modules do not depend on each other. Cross-feature handoff uses stable DTOs and Ports through a thin composition layer.
6. `libslic3r` does not contain provider or AI workflow policy. Any necessary core change must be a generally useful Orca capability with focused tests.
7. Feature-off behavior, old 3MF/profile compatibility and normal manual slicing are release gates.
8. Shared runtime and loopback-security files (`AIServiceManager`, `AISidecarClient`, local HTTP policy, network diagnostics and installed bootstrap/build identity) require integration ownership even when a feature team proposes the change.

## Branch and commit policy

```text
upstream/main
      |
      v
codex/orca-integration-v2
      ^                    ^
      |                    |
accepted model commit   accepted slicing commit
```

Each integration cycle records:

- pinned upstream SHA;
- accepted model-generation SHA and its verification evidence;
- accepted smart-slicing SHA and its verification evidence;
- a machine-verifiable receipt tying unchanged feature-owned Git objects to the accepted source SHA and the reviewed integration commit when history was snapshot-ported;
- one commit per ported module or architectural boundary;
- full build, test, compatibility and GUI results.

## Runtime isolation

The installed product uses **18764** as its single default Sidecar port. A branch name or developer role must never silently change the product endpoint.

The current AI runtime contract is maintained in [ai-integration-lock.json](ai-integration-lock.json), under `runtime_contract`, and checked against native/Python source constants. The Sidecar v8 / protocol v2 value previously recorded here is historical; a source contract does not by itself prove the identity of an installed package. Protocol changes update the machine-readable lock and both native/Python contract tests in the same reviewed change.

Concurrent development instances use separate build directories, `--datadir` values, output roots and explicit endpoint overrides. Ports 18765, 18766 and 18767 are development examples only; they are not alternate product defaults and must not appear as implicit release behavior.

| Runtime | Sidecar port | Selection rule | Runtime/output identity |
|---|---:|---|---|
| Installed product / release candidate | 18764 | Product default | Product data directory |
| Model-generation development instance | 18765 | Explicit developer override only | `model-generation` |
| Smart-slicing development instance | 18766 | Explicit developer override only | `smart-slicing` |
| Integration development instance | 18767 | Explicit developer override only | `orca-integration-v2` |

Development overrides must set the supported Sidecar URL/port configuration explicitly and use an isolated `--datadir` and output directory. A smart-slicing development instance uses 18766 only when it launches a Sidecar; the number remains reserved for that development identity so concurrent automation is deterministic.

The current commercial AI support target is Windows only. Cross-platform source compatibility remains required, but macOS/Linux AI distribution is not claimed until platform-specific Sidecar packaging, signing and qualification pass.

GUI automation must select the executable path, endpoint, data directory and concrete window identity. Combined acceptance runs only against the integration build. Release verification first confirms that no development override changes the 18764 product default.

## Consequences

- The first migration requires controlled porting rather than a history merge.
- Later Orca updates have a normal merge base and produce reviewable conflicts.
- Upstream, model-generation, smart-slicing and composition changes remain attributable.
- Conflicts cannot be eliminated when Orca changes the same public integration point, but they are constrained to a small, tested boundary.

## Release gates

- Windows Release build and available Catch2 suites pass.
- Model-generation Python/C++ and smart-slicing C++ tests pass.
- Independent and combined GUI journeys pass with isolated runtime state.
- AI-disabled and Sidecar-offline paths preserve normal Orca behavior.
- Representative old 3MF projects and profiles open, slice and save without migration regressions.
- No credentials, generated models, build products or machine-specific paths are committed.
