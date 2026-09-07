# ADR-004: Commercial AI product-line architecture

**Status:** Accepted for implementation

**Date:** 2026-08-27

**Scope:** Orca desktop integration, local AI Sidecar, commercial provider access and release operations

## Context

The product will ship with commercial 3D printers while model generation, smart slicing and upstream Orca continue to evolve in parallel. It therefore needs all of the following at the same time:

- preserve Orca's non-AI behavior, 3MF/profile compatibility and upstream merge path;
- let feature teams work independently without copying each other's implementation;
- keep local preprocessing, model import and slicing close to the desktop workspace;
- protect provider credentials and enforce account, entitlement, quota and billing policy;
- make failures diagnosable on customer machines without collecting prompts, images, models or credentials by default;
- support signed, reproducible releases, staged rollout and rollback.

The current locked installer is useful for a small, controlled internal beta. It is not a public commercial architecture: packaged provider credentials can be extracted and clients cannot authoritatively enforce quota or billing. A protocol-v2 per-launch challenge/proof baseline now protects the GUI-managed loopback Sidecar, but replay, hostile-local-process and lifecycle qualification is still required before treating it as a commercial trust boundary.

## Decision

Adopt a **desktop modular monolith + authenticated local Sidecar + commercial Provider Gateway** architecture.

```text
┌──────────────────────── Orca desktop modular monolith ────────────────────────┐
│ Native Orca workspace and slicing                                              │
│                                                                                │
│ Model generation UI/application ─┐                                             │
│                                 ├─ typed Ports/DTOs ─ Orca adapters/composition│
│ Smart slicing domain/application ┘                                             │
└───────────────────────────────┬────────────────────────────────────────────────┘
                                │ loopback only, per-launch authentication
                                ▼
┌──────────────────────── local AI Sidecar ──────────────────────────────────────┐
│ preprocessing · local job state · artifact validation · diagnostics · adapters │
└───────────────────────────────┬────────────────────────────────────────────────┘
                                │ TLS, short-lived product token, idempotency key
                                ▼
┌──────────────────────── commercial Provider Gateway ───────────────────────────┐
│ identity/entitlement · quota/billing · policy · provider credentials            │
│ idempotency · rate limits · kill switches · audit/metrics · provider routing    │
└───────────────────────────────┬────────────────────────────────────────────────┘
                                ▼
                         external AI providers
```

This is not a decision to split the desktop into independently deployed microservices. Model generation and smart slicing are modules inside one Orca executable; the Sidecar is one local process; only the trust and commercial control plane becomes a server-side service.

## Boundary rules

### Orca desktop

- Owns user interaction, the live Orca workspace, printable palette selection, model import and the final decision to apply slicing changes.
- Model generation remains behind `ModelGeneration`, `ModelGenerationPanel`, `AIModelGenerationClient` and related module boundaries.
- Smart slicing keeps Domain/Application/Ports under `src/slic3r/AI/SmartSlicing`; wxWidgets and concrete Orca calls remain in `src/slic3r/GUI/AI` adapters.
- Cross-feature handoff uses `IModelArtifactConsumer`, `IPrintablePaletteProvider`, `IOrcaWorkspace` and small versioned DTOs. Neither feature includes the other's GUI or application implementation.
- `MainFrame`, `Plater`, top-level CMake and installer files are composition roots only. They may register, navigate or adapt; they must not contain provider workflows or smart-slicing policy.
- AI-disabled, unauthorized and Sidecar-offline states must leave official Orca workflows usable and unchanged.

### Local Sidecar

- Owns local image/model preprocessing, bounded job orchestration, artifact download and validation, local persistence, provider-neutral error normalization and support diagnostics.
- Binds to loopback only and requires an unguessable, per-launch credential or an OS-authenticated IPC mechanism. Merely using `127.0.0.1` is not authentication.
- Receives the minimum-lived product access token needed to call the Gateway; it never contains a distributable provider master key.
- Does not mutate `Model`, print configuration, 3MF or profiles. It returns artifacts, diagnostics and proposals for the desktop to validate and apply.
- Uses an explicit protocol version and capability response. Unknown or incompatible versions fail closed for AI while leaving non-AI Orca available.

### Commercial Provider Gateway

The local Python `ModelProviderGateway` in this repository is a provider-neutral router/facade inside the Sidecar. It improves adapter isolation and error normalization, but it is **not** the server-side commercial Provider Gateway described here and it cannot own customer identity, authoritative entitlement, provider master credentials or billing policy.

- Owns user/device identity, entitlement, quota, billing authorization, idempotency, provider routing, rate limits, regional policy, kill switches and provider credentials.
- Returns a stable product error code plus a correlation identifier. Provider wording is diagnostic detail, not the desktop contract.
- Persists the mapping between the product idempotency key and provider task identifier before returning success. Retrying an ambiguous request must not create a second billed task.
- Cannot access a customer's Orca workspace, 3MF/profile files or arbitrary local paths. Inputs are explicit request payloads covered by product consent and retention policy.
- Is independently deployable and backward compatible with at least the currently supported desktop protocol window.

## Security and commercial controls

Public installers contain no OpenAI, Tripo or other provider credentials; public repositories also exclude them. The 2026-09-07 direction (INS-20260907-006) supersedes the earlier anonymous `3dprint.beer` credential-bearing exception for subsequent work. Provide test configuration separately through a recipient-authenticated, access-controlled channel. Preserve the [historical authorization record](../../Docs/coordination/model-generation/internal-test-authorization-20260905.md); do not use it to continue the old distribution path. This policy change does not itself replace old public artifacts or approve a commercial release.

Commercial release requires:

- authenticated local IPC and strict loopback binding;
- TLS validation to the Gateway and short-lived, revocable product tokens;
- server-side entitlement, quota and price confirmation before paid work;
- request idempotency across desktop retry, Sidecar restart and Gateway retry;
- secret scanning, dependency/SBOM review, code signing and provenance for released artifacts;
- redaction tests covering authorization headers, query strings, proxy credentials, environment values and provider payloads;
- a remotely enforceable provider/feature kill switch that does not disable normal slicing.

Credentials are never accepted through logs or support bundles. Development overrides remain explicit and are not enabled by a production installer.

## Data ownership and retention

Local inputs, generated artifacts and detailed job state belong to the user. The application must provide a visible storage location, deletion action and bounded retention policy. Automatic cleanup must be safe around artifacts already imported into an Orca project.

The Gateway stores only the data required for authorization, delivery, abuse prevention, billing reconciliation and support. Before commercial release, Product, Security and Legal must approve:

- which prompts, images or models leave the device;
- retention duration by data class and region;
- user-facing consent and deletion/export behavior;
- whether provider policies permit the intended commercial use;
- telemetry defaults and opt-out behavior.

Operational logs use identifiers, timings, sizes, error categories and versions. They do not record raw prompts, images, models, tokens or authorization headers by default.

## Multi-team development and upstream policy

- `codex/orca-integration-v2` remains the product integration line based on official Orca history.
- New model-generation and smart-slicing development branches start from the current shared product baseline so ordinary Git review and conflict detection work. Historical branches without a merge base are frozen as evidence, not reused as templates.
- Integration consumes exact, user-accepted 40-character SHAs. It never follows a moving feature branch head.
- The product endpoint remains 18764. Concurrent development may explicitly override model generation to 18765, smart slicing to 18766 and integration to 18767, together with isolated data/output roots; these are never alternate release defaults.
- Upstream sync, model-generation intake, smart-slicing intake, composition cleanup and release metadata are separate commits or pull requests.
- CODEOWNERS and required checks define the shared-touchpoint policy; branch rulesets must enforce it. Feature owners approve their modules; the integration owner approves `MainFrame`, `Plater`, shared adapters, CMake, packaging and upstream conflict resolution.
- Pull-request code runs only on ephemeral hosted runners with no inherited repository secrets. Signing, notarization and publication credentials are available only to exact-SHA jobs behind a protected release environment.
- Repository rulesets, not convention alone, must prohibit deletion/force-push and require PR, CODEOWNER approval and checks on integration, model-generation and smart-slicing branches.
- Each release records upstream SHA, both feature SHAs, Sidecar protocol/build identity, Gateway API compatibility and the final integration SHA in a machine-readable lock manifest.
- Source identity is generated into one integration-owned translation unit instead of a target-wide compiler definition. A commit-only identity change therefore rebuilds that object and relinks, rather than invalidating Orca's libraries and precompiled headers.
- Changes in `libslic3r` must be provider-neutral Orca capabilities with focused regression tests. Product policy stays outside the slicing core.

## Observability and supportability

Every workflow carries one correlation chain across UI, Sidecar and Gateway:

```text
session/build id → request id → product job id → provider task id
```

The desktop and Sidecar write structured, rotating, redacted logs with timestamps, component version, phase, duration, retry decision and stable error code. The Gateway exposes metrics and alerts for availability, latency, provider rejection, throttling, duplicate suppression and cost anomalies.

The product exposes a user-invoked connectivity doctor. It shall add the support-bundle exporter defined in `COMMERCIAL_READINESS.md`; that exporter is not implemented yet. The future bundle must be previewable before export and omit secrets and user content by default.

## Release channels

| Channel | Purpose | Credential model | Required assurance |
|---|---|---|---|
| Developer | Local development and deterministic mocks | Explicit local overrides; no shared production key | Focused tests and isolated data/port configuration |
| Internal fast beta | Test installer may use the authorized public download channel | Public EXE/ZIP contain no provider credentials; test configuration is separate | Configuration delivery requires an identified recipient and access-controlled channel. Keep artifact identity, test marking and withdrawal records. |
| Commercial candidate | Printer release candidate and pilot | Gateway-issued short-lived product token; no provider key in package | All P0 gates, signed installer, SBOM, compatibility and printer-matrix evidence |
| Commercial production | Staged public rollout | Same as candidate | Release approval, monitoring, kill switch, rollback artifact and support readiness |

The current AI commercial distribution scope is Windows only. macOS and Linux remain source-compatibility targets for shared native modules, but no AI commercial support is advertised until their Sidecar packaging, signing and qualification are funded and complete.

The internal fast package is not a promotable build. The build setting/channel value `commercial` is candidate metadata and activates credential-exclusion checks; it is **not** release approval. A commercial candidate is produced by a separate, reproducible pipeline from a reviewed source commit and becomes publishable only after the release gates below are approved.

## Public-commercial readiness gate

The existing integrated product is suitable for continued internal beta and architecture hardening. **Public commercial distribution is blocked** until at least these P0 items have objective evidence:

1. Provider Gateway, identity/entitlement and no-provider-key public installer.
2. Commercial qualification of the authenticated local Sidecar channel, plus tested log and future support-bundle redaction.
3. End-to-end idempotency and durable paid-job reconciliation.
4. Approved privacy, retention, provider terms and AGPL/commercial distribution obligations.
5. Signed reproducible installer, SBOM/provenance, rollback and incident kill switch.
6. Golden-printer, feature-off, old 3MF/profile, upgrade/uninstall and failure-recovery qualification.

Passing an internal GUI journey or a local Release build does not satisfy these commercial gates.

## Alternatives considered

### Call providers directly from the Orca GUI

Rejected. It embeds credentials and provider policy in a high-churn desktop surface, makes billing controls bypassable and couples releases to provider changes.

### Package shared provider keys in a locked installer

Not used for public packages under INS-20260907-006. Separate private test configuration remains extractable by recipients; obfuscation or a locked settings page cannot enforce authoritative quota or revocation.

### Move the complete model/slicing workflow to a cloud service

Rejected for the current product. It would expose more workspace data, weaken offline behavior and duplicate Orca's trusted local model/config/slicing state.

### Split each desktop feature into its own local service

Rejected. Multiple ports, processes, installers and compatibility matrices add operational cost without solving the commercial trust boundary. One modular Sidecar is sufficient.

### Continue snapshot-porting unrelated feature histories

Rejected for new work. It hides conflicts and makes attribution, review and automated upstream intake unnecessarily expensive. Future branches require shared ancestry.

## Consequences

- A Gateway and identity service add backend operating cost, but move secrets and commercial policy to an enforceable boundary.
- The local Sidecar remains useful for Python/provider isolation and diagnostics, but its protocol and lifecycle become a supported product surface.
- Desktop modules need incremental decomposition; a risky full rewrite is not required.
- Releases take more automated evidence than an internal fast package, while routine feature integration becomes faster through shared ancestry and protected boundaries.
- Provider outages degrade only AI capabilities. Manual Orca loading, configuration and slicing remain available.

## Initial implementation sequence

1. Freeze the current internal channel and label its limitations; introduce the release lock manifest.
2. Qualify and harden the implemented local IPC authentication/protocol/build identity; add replay, lifecycle, redaction and paid-job idempotency tests.
3. Implement the Gateway and replace packaged provider credentials with short-lived product authorization.
4. Establish required CI, signing, SBOM, compatibility and golden-printer gates.
5. Decompose the largest panel and Sidecar files behind existing contracts while preserving behavior.
6. Run a controlled pilot, validate support and rollback, then make a separate production-release decision.
