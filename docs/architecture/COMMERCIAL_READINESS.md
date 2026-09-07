# Commercial readiness for the Orca AI product line

**Assessment date:** 2026-08-28

**Decision basis:** current `codex/orca-integration-v2` source and architecture documents

**Status:** Internal beta is feasible; public commercial release is blocked by the P0 gates below

## Executive conclusion

The product direction is viable. Orca already supplies the mature local workspace and slicing engine; model generation and smart slicing can remain optional modules; the Sidecar provides a practical boundary for Python, preprocessing and provider integration. A desktop modular monolith avoids unnecessary runtime complexity and retains a workable upstream path.

The current implementation should continue as an internal beta. Under the latest 2026-09-07 direction (INS-20260907-006), public EXE/ZIP packages contain no provider credentials; test configuration is delivered separately through an access-controlled channel. The earlier anonymous-download credential exception is historical and superseded for subsequent work; see the [dated record](../../Docs/coordination/model-generation/internal-test-authorization-20260905.md). This does not qualify a commercial release. Commercial viability still depends on the server-side trust boundary, local authentication and paid-job recovery qualification, repeatable CI/release evidence, and privacy, licensing and printer-quality gates.

The current AI commercial target is **Windows only**. Shared native code should remain cross-platform, but macOS/Linux AI packages are outside the supported commercial matrix until their Sidecar runtime, installer/signing and printer qualification are explicitly funded and passed.

No test result is claimed by this document. A release gate is passed only when its evidence is attached to the exact candidate SHA and installer hash.

## Controls implemented in the 2026-08-27 review

These controls reduce immediate integration and support risk but do not by themselves approve a public release:

- fixed-SHA merge of the reviewed Orca upstream snapshot, with one shared-file conflict resolved semantically;
- machine-readable upstream/feature/runtime lock, dependency-boundary validator, CODEOWNERS, AI-specific PR checklist and required-check workflow;
- distinct product/development port policy and full application/package/Sidecar identity in the installed runtime health/log contract;
- exact source identity is isolated in one generated translation unit, avoiding a whole-tree rebuild when only the release commit changes;
- protocol-v2 per-launch challenge/proof authentication for the GUI-managed loopback Sidecar, including rejection of a stale or foreign protected service; negative, replay and process-lifecycle qualification remains open;
- one network policy for OpenAI, Tripo and artifact downloads, including the Windows `NO_PROXY`/system-proxy case and value-free route diagnostics;
- explicit `internal` versus `commercial` packaging channels; commercial configuration fails if a package-only provider credential payload is supplied;
- internal fast-package checks for clean/current source, matching build identity, localization, configuration schema, installer checksum and release manifest;
- release publication checks for the exact successful workflow, branch and full source SHA.
- general pull-request builds are forced onto ephemeral GitHub-hosted runners and no longer inherit repository secrets; signing credentials belong only in a separately approved release environment;
- the installed Sidecar rechecks and retains the owning-process identity before restoring remote work, supports authenticated graceful shutdown, and applies nested structured-log redaction.

The remaining P0 items below—especially Gateway-held provider credentials, server-side entitlement/idempotency, privacy/compliance, signed supply chain and printer qualification—remain release blockers.

## Product requirements

### Functional requirements

| Area | Commercial requirement |
|---|---|
| Printer onboarding | Preserve the normal Orca configuration wizard, language selection and official profile behavior; make supported product printers easy to select without hiding upstream capabilities. |
| Model generation | Accept supported images/text, show price/time/remote-processing consent before paid work, survive restart, validate artifacts and import only after explicit user action. |
| Smart slicing | Inspect the current workspace, propose bounded changes, compare trial results, apply atomically through Orca adapters and allow the user to reject or undo. |
| Account and entitlement | Sign in or bind an entitled device/account, display remaining entitlement and distinguish authorization, quota, provider and network failures. |
| Offline/degraded use | Keep normal project opening, model loading, manual settings, slicing and export usable when AI, Sidecar, Gateway or a provider is unavailable. |
| Updates and rollback | Upgrade without losing user data, retain profile/3MF compatibility, identify every component version and provide a supported rollback path. |
| Support | Export a redacted diagnostic bundle, correlate a failed job end to end and provide stable error codes with actionable user guidance. |

### Non-functional requirements

| Quality | Requirement |
|---|---|
| Security | No provider secret in public artifacts or public repositories. Test configuration is delivered separately to identified recipients through an access-controlled channel. Authenticated local IPC, short-lived authorization, least privilege, signed release and dependency provenance remain requirements. |
| Privacy | Explicit consent for remote processing; documented data classes, regions and retention; deletion/export behavior; user content excluded from logs by default. |
| Reliability | No duplicate paid work under retries/restarts; durable job reconciliation; bounded retries; non-AI fallback; tested disk-full and process-restart behavior. |
| Compatibility | Representative old 3MF/profile projects, target printer profiles and feature-off behavior remain compatible across upgrade and rollback. |
| Performance | AI orchestration must not block the GUI or normal slicing; local preprocessing has bounded CPU, memory, disk and time budgets. |
| Scalability | Concurrent customers are handled by Gateway admission control, quota, rate limits and provider isolation, not by adding concurrency to a desktop instance. |
| Maintainability | Feature modules have shared Git ancestry, explicit ownership and stable Ports; shared Orca touchpoints stay thin; protocol changes are versioned. |
| Observability | Structured redacted logs, correlation IDs, component/build identity, connectivity diagnostics, server metrics and alertable failure categories. |
| Supply chain | Reproducible candidate, locked dependencies, SBOM, malware/signature checks, installer hash, provenance and retained rollback artifact. |
| Compliance | AGPLv3 distribution obligations, third-party licenses, provider commercial terms, privacy notices and customer support terms receive formal review. |

## Feasibility by capability

| Capability | Current feasibility | Commercial conclusion |
|---|---|---|
| Orca desktop and printer workflow | High | Reuse upstream behavior; qualify target-printer profiles and guard shared-core changes. |
| Smart slicing | High for assisted local workflow | Keep proposals bounded and locally validated; do not let remote advice bypass Orca validation. |
| Model generation | High for internal beta | Commercial release requires Gateway authorization, idempotency, policy and provider abstraction. |
| Local Sidecar | High as one packaged helper process | Per-launch authentication is implemented as a baseline; qualify negative, replay, parent/child lifecycle and hostile-local-process behavior as a supported product contract. |
| Multi-developer delivery | Medium today | Move new feature work to shared ancestry, CODEOWNERS and required checks; freeze no-merge-base histories. |
| Fast upstream sync | Medium today | Preserve explicit upstream merge commits and reduce changes in `Plater`, `MainFrame` and `libslic3r`. |
| Public commercial operations | Low today | Blocked until security, privacy, account/billing, release-signing, support and production-Gateway gates pass. |

## Prioritized gaps and actions

### P0 — required before any public commercial release

| Gap/risk | Required action | Acceptance evidence | Primary owner |
|---|---|---|---|
| Shared provider credentials can be extracted from a packaged client | Implement the commercial Provider Gateway; public package contains no provider key; rotate/revoke internal beta credentials | Secret scan of source and unpacked installer; Gateway architecture/security review; rotation exercise | Backend/Platform + Security |
| The three product branches and commercial release environment are not protected remotely | Configure GitHub rulesets for integration/model/smart branches: no delete/force-push, PR required, CODEOWNER review, required checks and admin enforcement; create `ai-commercial-release` with required reviewers and integration-only deployment policy | Repository ruleset export/API evidence plus a rejected bypass/force-push test | Repository Admin + Release |
| Loopback Sidecar authentication is implemented but not commercially qualified | Keep strict loopback binding and protocol-v2 per-launch challenge/proof; validate method, content type, body size, replay resistance and parent/child lifecycle assumptions | Negative tests from another local process; token lifecycle, replay and stale-process tests | Desktop Integration |
| Commercial runtime can still honor a developer Sidecar URL override | Bind override policy to signed build identity; commercial packages reject or authenticate every override instead of silently disabling session startup | Commercial-package tests for hostile environment overrides and stale/foreign listeners | Desktop Integration + Security |
| Paid provider request can be ambiguous during persistence/network failure | Persist product idempotency before external submission; Gateway deduplicates and reconciles provider task IDs; never silently continue after durable-state failure | Restart, timeout, retry and disk-full fault-injection suite proving one billed task | Model Generation + Backend |
| Account, entitlement, quota and price authority are client-side/incomplete | Make Gateway authoritative; define account/device binding, confirmation receipt, quota and refund/cancel semantics | Contract tests and approved product/billing journey | Product + Backend |
| Privacy and retention policy is undefined | Inventory every outbound/local data class; implement consent, deletion, TTL/quota and regional retention; complete provider/legal review | Approved data-flow record, UI copy and deletion/retention tests | Product + Legal/Security |
| Release artifacts lack complete commercial supply-chain evidence | Produce a clean signed installer with pinned dependencies, SBOM, provenance, installer hash, vulnerability/license review and retained rollback artifact | Release manifest and signature validation on a clean machine | Release + Security |
| Regression qualification is not tied to exact commercial candidate | Gate target printers, old 3MF/profile corpus, official feature-off workflow, upgrade/uninstall, failure recovery and combined AI GUI journeys | Archived CI/QA report linked to source SHA and installer hash | QA + Integration |
| Production incidents cannot be contained centrally | Add feature/provider kill switches, staged rollout, health dashboards and rollback procedure while preserving non-AI Orca | Game-day evidence for provider disable and desktop rollback | Backend + Release/Support |
| Distribution/licensing obligations need approval | Review AGPLv3 source-offer obligations and all bundled/runtime/provider licenses and terms | Signed compliance checklist for the release | Legal + Release |

### P1 — complete before broad rollout

| Gap/risk | Required action | Relevant implementation area |
|---|---|---|
| Large GUI and Sidecar files concentrate unrelated responsibilities | Extract panel state/presentation, job/application services, storage and provider adapters behind current behavior; avoid a big-bang rewrite | `src/slic3r/GUI/ModelGenerationPanel.*`, `tools/ai/orca_ai_sidecar.py` |
| The model-owned Sidecar file also contains shared HTTP/auth/lifecycle policy | Extract shared `sidecar_server`, authentication and lifecycle modules with integration ownership; keep model routes/jobs/provider adapters model-owned | Sidecar characterization tests and ownership lock/CODEOWNERS update |
| Historical feature branches do not provide a normal merge base | Freeze them; create new `model-generation-v2` and `smart-slicing-v2` work from the current integration baseline; enforce exact-SHA intake | Git workflow and baseline manifest |
| Shared Orca touchpoints can accumulate product logic | Define CODEOWNERS and line-budget/review rules; move policy into module/application services and adapters | `MainFrame.*`, `Plater.*`, CMake/install composition |
| Sidecar protocol v2 is explicit but its supported compatibility window is not published | Publish the capability schema and minimum/maximum supported protocol window; test fail-closed upgrade/downgrade behavior | `AIModelGenerationClient.*`, `AIServiceManager.*`, Sidecar routes |
| Network/provider errors are too coarse for field diagnosis | Normalize DNS, TLS, proxy, auth, quota, content-policy, rate-limit, timeout and provider-outage errors; retain safe upstream request IDs | `tools/ai/ai_diagnostics.py`, provider adapters, GUI error mapping |
| Local jobs/artifacts/logs can grow without policy | Add configurable TTL, disk quota, rotation, safe deletion and low-disk preflight | Sidecar persistence/output and application data directory |
| Support bundle is not a stable product artifact | Implement the redacted, previewable bundle specified below and test it against seeded secrets/user content | Desktop diagnostics and Sidecar log export |
| Upstream changes are not continuously rehearsed | Schedule merge-preview CI against a pinned/fetched upstream commit and report shared-file conflicts without auto-publishing | CI and integration workflow |
| macOS/Linux AI packaging is not qualified | Keep the current commercial matrix Windows-only; fund and pass platform-specific Sidecar packaging, signing and printer qualification before advertising wider support | Product/release matrix |

### P2 — scale and optimization after launch readiness

- Provider routing/fallback based on policy, region, capability and measured quality.
- Opt-in telemetry and product-quality dashboards with privacy budgets.
- Remote configuration and controlled experiments with signed configuration and safe defaults.
- Incremental upload/download, cache policy and preprocessing performance tuning.
- Automated upstream conflict forecasts and module-boundary conformance checks.
- Self-service job history/export and support status, subject to approved retention policy.

## Target module ownership

| Boundary | Owns | Must not own | Source area |
|---|---|---|---|
| Model Generation | Generation UX/application state, provider-neutral job DTOs, artifact validation/import request | Smart-slicing implementation, direct Orca workspace mutation, public provider secrets | `src/slic3r/GUI/ModelGeneration*`, `AIModelGenerationClient.*`, model-generation parts of `tools/ai` |
| Smart Slicing | Inspection, proposals, trial orchestration, comparison and apply transaction | Provider jobs, model-generation GUI, direct wxWidgets in Domain/Application | `src/slic3r/AI/SmartSlicing`, `src/slic3r/GUI/AI/SmartSlicing` |
| Orca adapters/composition | Workspace snapshots, official slice gateway, artifact consumption, palette, navigation and registration | Feature policy or provider-specific behavior | `src/slic3r/GUI/AI/Orca`, thin `MainFrame`/`Plater`/CMake changes |
| Local Sidecar | Pre/post-processing, local job state, downloads, diagnostics and provider-neutral transport adapters | Orca Model/Config mutation, account authority, permanent provider keys | `tools/ai` and installed bootstrap/runtime |
| Provider Gateway | Identity, entitlement, quota/billing, idempotency, routing, secrets, rate limits, audit and kill switch | Local file/workspace access or slicer UI state | Separate deployable service; not present in this repository today |
| Orca core | General-purpose geometry, model, config and slicing capabilities | AI workflow/provider/product policy | `src/libslic3r` |

`tools/ai/model_provider_gateway.py` is the local Sidecar's provider router/facade. Despite the similar name, it is not the separate commercial Provider Gateway and is not an authority for identity, entitlement, billing or distributable provider credentials.

## Release gates

All gates are required unless the approved product scope explicitly marks one not applicable. “Passed” means evidence belongs to the same source SHA, dependency lock and installer hash.

The CMake/package distribution channel value `commercial` is candidate metadata and enables credential-exclusion policy. It never means that these gates were approved, and automation must not infer publish authorization from that value alone.

| Gate | Minimum evidence | Current assessment |
|---|---|---|
| Source lineage | Exact upstream, model, smart, Gateway API and integration identities in release lock; clean reviewed tree | Upstream/model/smart lock and validator are present; final candidate/Gateway identity and release evidence remain pending |
| Build and unit tests | Clean Release build; Python/C++ suites; protocol/contract tests; no excluded product-critical failure | Candidate evidence required |
| Core compatibility | Feature-off Orca regression; old 3MF/profile open/slice/save; no unintended default/profile changes | Candidate evidence required |
| Printer qualification | Golden projects on every supported printer/material/firmware class, output review and bounded performance | Not yet evidenced here |
| AI failure recovery | Sidecar absent/crash/restart, Gateway unavailable, provider 4xx/429/5xx, proxy/TLS, timeout, cancellation, disk-full and app restart | Not yet evidenced here |
| Paid-job integrity | Fault injection proves idempotency and reconciliation with no duplicate billing | Blocked on Gateway design |
| Security/privacy | Threat review, IPC negative tests, secret/redaction scan, dependency/license review, consent/retention/deletion evidence | Not yet evidenced here |
| Installer lifecycle | Signed installer; clean install/upgrade/downgrade/uninstall; language/config wizard retained; user data policy verified | Candidate evidence required |
| Operations | Staged rollout, dashboards/alerts, provider kill switch, incident runbook, support bundle and rollback rehearsal | Not yet evidenced here |
| Approval | Product, Engineering, QA, Security/Legal, Release and Support sign-off | Pending all P0 gates |

## Failure handling and service objectives

The following are proposed launch targets, not measured current performance. Product and Operations must approve them and define measurement windows before release.

### Hard correctness invariants

- A logical paid request creates at most one billable provider task across retries and restarts.
- A failed AI operation never silently mutates or corrupts the current Orca model, config, 3MF or profile.
- Failure of Sidecar, Gateway, provider or telemetry never disables ordinary local slicing.
- Logs, support bundles and metrics contain no secrets or raw user content by default.

### Proposed service/support targets

| Objective | Proposed target | Measurement/status |
|---|---:|---|
| Gateway monthly availability | 99.9% excluding announced maintenance | Not yet measured |
| Gateway submit/authorization latency | p95 under 2 seconds, excluding provider generation time and input upload | Not yet measured |
| Sidecar discovery after normal desktop launch | p95 under 10 seconds on supported hardware | Not yet measured |
| Job-state recovery after desktop/Sidecar restart | Visible reconciled state within 30 seconds when Gateway is reachable | Not yet measured |
| P0 security/billing incident containment | Kill switch or rollout halt within 30 minutes of confirmed incident | Runbook/game day required |
| Customer P0 support acknowledgement | Within 4 support hours | Staffing and support hours must be approved |
| Supported-release rollback decision | Within 2 hours of confirmed release-wide regression | Rehearsal required |

Provider generation duration is displayed as an estimate and is not counted as product control-plane latency. The UI must show a recoverable job identity rather than pretending a long provider operation is synchronous.

### Failure-mode behavior

| Failure | Product behavior | Automatic action | Required diagnostic evidence |
|---|---|---|---|
| Sidecar missing or failed to start | AI unavailable message; normal Orca remains usable | Bounded restart/probe, then stop retrying | Manager/Sidecar version, launch phase, exit code, sanitized path |
| Port occupied or stale Sidecar | Do not connect solely because `/health` answers | Authenticate and compare instance/protocol/build; use product default 18764, or only the explicit development mapping model=18765/smart=18766/integration=18767 | Endpoint, process ownership where permitted, instance/build IDs |
| Proxy/DNS/TLS/connectivity failure | Distinguish configuration from provider outage; never expose credentials | Safe connectivity checks and bounded retry only for retryable classes | Proxy decision without values, DNS/TLS category, correlation ID |
| Gateway auth/quota/policy rejection | Explain sign-in, entitlement, quota or policy action | Refresh short-lived token once where valid; no blind paid retry | Stable product code, HTTP class, safe request ID |
| Provider throttling/outage | Preserve job, show retry state and estimate | Gateway backoff/circuit breaker or policy-approved failover | Provider alias, status class, retry-after, correlation ID |
| Ambiguous create timeout | Show “reconciling”, not “failed—retry” | Query by idempotency key before any resubmit | Product idempotency key hash, Gateway/provider task mapping |
| Disk full or persistence failure | Block new paid submission before provider call | Cleanup only expired unreferenced data; otherwise ask user to free space | Free space, quota, failed store phase; no local filenames unless user opts in |
| Job stuck across restart | Restore known phase and allow safe cancel/reconcile | Gateway status reconciliation; bounded stale-job policy | Job timeline, persisted version, last successful phase |
| Smart-slicing trial/apply failure | Keep original workspace and explain rollback | Abort transaction and restore validated revision | Workspace revision, proposal ID, validation/error category |
| Upstream regression | Disable affected AI composition or roll back product build | Feature kill switch or signed installer rollback | Release lock, crash/error fingerprint, reproduction project with consent |

## Future support bundle contract

The exporter is not implemented yet. When implemented, the user explicitly creates the bundle, can preview its categories, and can choose whether to attach reproduction content separately. The default bundle shall include:

- Orca product version, full integration SHA, release channel and installer hash;
- Sidecar build/protocol/schema/instance identity and enabled capabilities;
- request/job/correlation IDs and a phase/timing timeline;
- sanitized provider alias, HTTP/error class and provider request ID when safe;
- OS version/architecture and bundled runtime/OpenSSL versions;
- proxy routing decision and DNS/TLS category without proxy URL credentials or environment values;
- rotating recent desktop/Sidecar logs after deterministic redaction;
- data/output/log directory category, free-space/quota figures and retention configuration;
- whether AI was disabled, mocked, internal-locked or commercial-Gateway mode.

It excludes provider/API keys, bearer tokens, cookies, authorization headers, full environment dumps, proxy credentials, raw prompts, images, models, 3MF/profile contents, account PII and arbitrary absolute user paths. Secret-seeded tests must prove exclusion before release.

## RACI and ownership

R = responsible, A = accountable, C = consulted, I = informed. One person may fill multiple roles in a small team, but the accountability must still be explicit per release.

| Workstream | Product | Integration | Model Gen | Smart Slicing | Backend/Platform | QA/Printer | Security/Legal | Release/Support |
|---|---|---|---|---|---|---|---|---|
| Product scope, price/consent journey | A/R | C | C | C | C | I | C | I |
| Upstream intake and shared Orca composition | I | A/R | C | C | I | C | I | C |
| Model-generation desktop/Sidecar module | C | C | A/R | I | C | C | C | I |
| Smart-slicing workflow and apply safety | C | C | I | A/R | I | C | C | I |
| Gateway, auth, quota, billing and idempotency | C | C | C | I | A/R | C | C | I |
| Printer/profile and compatibility qualification | C | C | C | C | I | A/R | I | C |
| Threat, privacy, license and provider-term review | C | C | C | I | C | I | A/R | C |
| Signed build, rollout, monitoring and rollback | I | C | I | I | C | C | C | A/R |
| Field triage and support-bundle handling | C | C | C | C | C | C | C | A/R |

## Delivery sequence

1. **Contain the beta:** label internal packages, rotate/revoke beta credentials, freeze release identities and add a support-bundle/redaction baseline.
2. **Close commercial P0:** Gateway/auth/idempotency, local IPC authentication qualification, privacy/retention, signed supply chain and printer qualification.
3. **Make parallel delivery routine:** shared-ancestry feature branches, CODEOWNERS, required checks, release lock and upstream merge-preview CI.
4. **Reduce hotspots safely:** extract responsibilities from the model-generation panel and Sidecar behind characterization/contract tests.
5. **Pilot and decide:** staged employee/customer pilot, operational game day, rollback rehearsal and evidence-based production approval.

The production launch date should be set only after P0 owners and evidence are assigned; calendar pressure must not silently convert an internal locked package into a commercial candidate.
