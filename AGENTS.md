# AGENTS.md

This is the continuation repository for the OrcaSlicer AI project. It preserves the latest useful model-generation working tree while keeping the former workspaces intact.

## Current scope: internal team validation (2026-09-11)

- The current deliverable is a Windows internal test build and its actual Orca main-window journey. Public releases, commercial release promotion, website uploads, signing/publication qualification and deployment are outside the current workflow. Do not restore those steps from historical plans.
- Local full application builds, startup and functional validation do not require a Git commit, push, PR, remote CI, team notification or a clean worktree first. "Full build" means building the required application and runtime targets; it does not require deleting an existing incremental build.
- Internal test packages may use a verified base commit plus a complete file-hashed snapshot, including uncommitted changes and a detached validation HEAD. Record the source identity, build configuration, artifact hashes and actual verification limits. Do not label a snapshot as a clean integrated commit.
- Use `release/build_internal.ps1 -SourceManifest <handoff/manifest.json>` for internal snapshot packaging. Its `-ValidateOnly` checks prerequisites without building, copying the runtime or starting Orca. Package/configuration failures are technical conditions to resolve within existing authorization, not evidence of a platform denial.
- The team submission section below applies only when the user requests remote submission or integration. Internal packaging does not require pushing or fetching; an unavailable integration baseline is recorded as unknown.

## Start here

Use the current request to choose the necessary context:

- Read `CONTINUATION.md` when taking over the project or when current scope, provenance or completion status is unclear. Its dated corrections supersede older product descriptions.
- Use `Docs/AI_ENGINEERING.md` to locate an affected module and its verification commands.
- Consult the relevant sections of `Docs/plans/2026-09-07-ai-product-rebuild-execution-prompt.md` for product or main-window UX changes, including its latest dated corrections.
- Read `docs/architecture/ai-integration-lock.json` when changing integration boundaries, contracts, runtime pins, shared touchpoints or architecture budgets. Historical role names in this lock are provenance, not instructions to recreate old tasks.

Do not reread the whole document stack for a small, understood edit. Follow nested `AGENTS.md` when working in its directory.

Root `task_plan.md`, `findings.md`, `progress.md`, dated reports, and `archive/*` branches are retained evidence. They are not the current task list and do not authorize old work.

## Work model

- Use one principal development task and one reusable validation/delivery task, as authorized on 2026-09-11. Follow [the validation handoff workflow](Docs/coordination/validation-handoff.md). Do not recreate the former six permanent role tasks or create a new validation task for each request.
- Complete the requested bounded change through implementation, relevant checks and corrections before handing it back. Local builds, offline tests with verified mocks/fixtures, and isolated local inspection are part of an implementation request; reuse existing authorization.
- Work on one bounded change at a time. Use short-lived internal subagents only when a concrete independent check materially helps; they do not create new sidebar tasks.
- ADR-007 is accepted. The new team branches are `codex/team/model-generation`, `codex/team/smart-slicing`, `codex/team/maintenance`, and `codex/team/integration`. The common source snapshot has been prepared; `codex/continue` remains a provenance reference. New work uses the new team branches; old branches must not be force-updated or deleted during migration.
- Developers synchronize from `codex/team/integration` with ordinary merge commits. Target feature PRs at this integration branch; retain shared history and require current-version CI and non-author review. The initial automation phase is notification and candidate checking with manual merging; automatic merge and release are not enabled by adopting this ADR.
- `archive/model-generation-head-20260908`, `archive/smart-slicing-20260908`, and `archive/orca-integration-20260908` remain provenance references, not merge instructions.
- A platform check blocked comparison of the latest smart-slicing source with this snapshot. Do not retry that comparison through another task, agent, tool, or entry point. Keep the archive ref until the platform review is actually resolved.
- Never reset, clean, delete, or modify the former workspaces as part of work here.

## Team submission and integration (only for requested remote submission)

Follow [the team SOP](Docs/coordination/team-integration-sop.md) for commands, notification templates, build records and rollout status. These repository instructions guide developers and agents; branch protection, CI and the service enforce the remote workflow.

The confirmed GitHub owners are `arsenaltj` for model generation, `tony20160206` for smart slicing, and `tangjiajie15191661723-web` for maintenance and integration coordination. Keep `.github/team-collaboration.json` and generated `.github/CODEOWNERS` aligned with these roles. Account assignment does not by itself grant repository access; effective ownership requires collaborator write access and the CODEOWNERS version on the protected PR base branch.

- Before submitting work for integration, announce the task, branch and shared files in the agreed team channel. Fetch `origin`, incorporate updates to your own remote branch if any, then merge `origin/codex/team/integration` into your own branch. Preserve unfinished work before merging; never resolve conflicts by blindly replacing another developer's changes.
- Report the sync outcome, including conflicts or failure, and record the fetched integration SHA. Resolve conflicts, inspect the combined behavior, compile affected code and run applicable checks from `Docs/AI_ENGINEERING.md`. Documentation-only edits need documentation checks. Record the exact tested source SHA and any remaining verification limits.
- Push your own branch and open/update a PR targeting `codex/team/integration`; do not push directly to integration. Incomplete work may be saved as a clearly marked draft but must not enter the ready queue. Keep one independently reviewable change per PR and list dependencies as `Depends-On: #12, #13` when present.
- Require current-version CI, resolved discussions and non-author review, including the relevant owner for shared changes. A source or integration HEAD change invalidates the previous candidate; rebuild/recheck against the new pair before merging. Chat announcements are coordination notices, not a global lock or evidence that checks passed.
- Only the designated maintainer merges approved PRs during the current manual phase. The future bot must serialize merges, obey branch protection and recheck both HEADs immediately before merging. Automatic merging requires its separate implementation and real PR acceptance; the current service cannot perform it.
- Record CI/team-package attempts and retain successful internal packages with source snapshot identities, platform/toolchain, verification results and artifact hashes as specified in the SOP. Keep snapshot and PR candidates distinct from builds of the actual merged integration SHA. Binary packages and private configuration do not belong in Git source history; pending remote archive automation does not block local validation.
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
- Internal EXE and portable ZIP packages contain no provider credentials. Private tester configuration remains outside shared artifacts, source control, and ordinary logs.
- Do not publish, deploy, push, call paid generation services, or contact testers unless the user explicitly requests that concrete action.

## Verification

### 固定验证任务交接（2026-09-11 用户确认）

- 用户要求验证程序功能、实际体验、打包或推送时，开发任务完成必要自检，按[交接流程](Docs/coordination/validation-handoff.md)准备确定版本、修改点、验证点和已有授权，发送给本机登记的固定验证任务。纯文档、文案或单项单元测试检查按实际范围处理，不必每次跨任务。
- 用户已授权开发与验证任务之间发送交接、缺陷和结果消息。实际调用任务消息工具并检查回执；写入文档或说“已交接”不能代替发送。原开发任务继续负责修复、复测闭环和向用户汇总。
- 通过 `git rev-parse --path-format=absolute --git-common-dir` 定位本机登记 `<git-common-dir>/codex/validation/registry.json`。根据任务 ID 区分角色：固定验证任务自己收到验证请求时直接执行，不再次派给自己；开发任务收到结果回执时消费该结果，不把回执当新验证请求。
- 验证使用独立源码 worktree、构建目录、运行目录和测试配置。接收后固定源提交或完整补丁快照；开发目录后续变化不进入正在验证的版本。验证结果、打包文件和推送提交必须能对应到同一版本。
- “验证”默认授权相关本地构建、离线检查和真实主窗口操作；不自动扩展为收费生成、打包、推送、发布或联系测试者。明确要求的打包／推送及已有具体授权随交接传递，不重复索要相同许可。远程提交继续遵守团队 SOP、CI、非作者审核和人工合入规则。
- 平台拒绝的具体操作及原始理由必须随交接传递，不得换任务、工具或入口重试绕过。任务分工不改变下方真实主窗口验收要求，未执行项目必须保留为待验证。
- 拒绝记录写明时间、工具、具体命令/动作及原始输出；区分平台明确拒绝、脚本前置条件失败和代理尚未执行。仅有 `blocked by policy` 时不得自行归因为 Auto-review、永久账户限制或某个风险规则；也不得把一个组合命令的拒绝直接扩大成所有编译均被禁止。不依赖被拒操作的工作继续完成；必要步骤确实涉及同一被拒操作时保留阻塞，不拆分或换入口规避。

- Use `python scripts/verify_ai_integration.py --json` for architecture and integration checks, and report all findings rather than weakening checks. Instruction-only or prose-only edits need document/skill validation; they do not require a full application build or unrelated suites.
- Verify that a selected offline test actually mocks provider access; do not infer isolation from its filename or from available credentials. Paid benchmarks and real provider calls remain separately authorized operations.
- Run checks appropriate to the changed behavior. Once required checks pass, repeat or broaden them only for new changes, failures or unresolved concerns.
- Python AI tests are under `tools/ai/test_*.py`. C++ test placement and commands are in `tests/AGENTS.md`.
- Match verification to the affected module. Compilation or unit tests alone do not prove the main-window journey, provider stability, color fidelity, real slicing behavior, or print quality.
- For GUI work, inspect the affected actual main-window route. When navigation, task state, import or recovery behavior changes, verify state retention, import handoff, failure recovery and relevant ordinary Orca regressions. A copy-only change needs the affected visible state checked, not an unrelated full journey. Full UX acceptance still requires the product journey evidence.

### Windows 本地执行与审批（2026-09-11）

- AGENTS.md 说明项目授权和工作方式，不覆盖工具执行策略。用户通过客户端支持的方式修改审批设置后，遇到权限问题时核对当前会话的有效设置；配置文件已保存不等于运行中的任务已经加载。不把日常已允许的操作变成额外权限排查或确认流程。
- 命令应清楚呈现目标路径、修改范围、启动程序及网络用途，复用可审查的现有项目脚本并传入明确参数。不得通过编码、隐藏字符串、间接执行或改变工具入口掩盖实际动作；不以规避检测为目的调整命令写法。回环 HTTP 地址也应说明用途，不默认免于审核。
- 新的测试环境准备优先保持完整、版本相符的独立运行目录；启动前只读核对 EXE、DLL、基础资源、Python 和安装版 sidecar 启动文件。不要把资源不完整的开发 EXE 当作已修复测试包交给用户，也不要为了方便修改已有运行实例或共享开发目录的资源联接。此原则不授权替代执行已经被拒绝的运行准备或启动操作。
- 复杂 PowerShell 命令可先做静态语法与路径检查，用于定位技术错误；只解析不等于执行成功或策略已批准。拒绝诊断须区分实际命中证据、公开实现推断和仍未知的原因，不把语法正确或离线规则复现写成平台放行。
- 已有明确拒绝只能按平台提供的正式审批或复核路径处理。用户修改审批模式、再次表达任务授权或编辑项目指令，不自动等于该具体动作已获平台批准；取得适用于该动作的正式批准且当前工具允许后，才按批准范围继续。未解除前不拆分、换脚本、换任务或换入口执行同一被拒操作；不依赖它的已授权工作继续完成，准确保留验收缺口。

### 真实 3D 生成与历史资产（2026-09-10 用户要求）

- 后续真实 3D 生成须从软件自身主窗口的完整用户旅程发起，经过输入、设计图确认、生成及结果查看；不要用独立脚本或直接提供商／sidecar API 提交新生成任务来替代软件旅程。隔离且确实 mock 提供商的离线测试不受此限制，但不能作为主窗口验收证据。
- 生成的输入、设计图、模型及版本记录应保留在软件实际使用的历史资产中。验证时检查历史列表可见、切页后可重新打开，以及重启后仍能恢复；仅在 `outputs` 或临时测试目录保留文件不算已进入历史资产。保留原文件和真实任务标识，不覆盖已有资产或伪造生成记录。
- 继续遵守当前已授权的调用预算，不能因改用主窗口而重置计数。已有模型的查看、导入和恢复不应隐式提交新的收费任务。

### 用户要求验证时，必须实际启动程序

- 用户要求验证程序功能、实际体验或“看看有没有问题”时，必须实际启动包含待验收修改的程序，并从真实主窗口操作相关流程。不能只做代码阅读、编译、单元测试或模拟接口测试就宣称验证完成；仅启动成功也不等于功能验证通过。
- 此类请求已经授权必要的本地构建、准备测试运行目录、启动程序和相关界面操作。不得以尚未再次确认、一般技能建议、预计耗时或假设性风险为由自行拒绝、跳过实际验证或重复索要启动许可。使用独立测试配置和模型副本，保留用户已有工程与运行实例；付费服务、发布和其他外部动作仍按各自授权执行。
- 核对实际运行的可执行文件、DLL、资源和 sidecar 对应当前待验收修改，记录版本或文件哈希。旧实例只能作为问题复现或对照依据，不能替代新修正版的实际验收。
- 按本次修改范围走查成功路径、必要的取消／失败恢复和相关普通 Orca 行为；涉及切页、编辑、版本恢复或导入时，实际检查状态保留、撤销／重做及准备页交接。发现问题应修正、重建并重新启动修正版复测，直至相关问题解决或遇到真实阻塞。
- 保存足以复核的界面截图、日志或结果文件，明确哪些流程实际通过、哪些仍未执行。未完成实际程序验证时，不得写“没有问题”“全流程通过”或把该验收项标记完成。
- 本要求不改变平台工具权限。若平台明确拒绝启动或必要操作，必须说明被拒绝的具体动作及原始理由，继续完成不依赖该动作的准备工作，并保留待验证项；不得把代理自身的判断说成平台拒绝，也不得绕过平台拦截或虚报通过。
