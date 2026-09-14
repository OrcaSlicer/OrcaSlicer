# 3DDY / Orca AI 续建说明

日期：2026-09-08

**2026-09-11 当前交付范围修正：** 用户当前只需要团队内部验证，移除正式发布、网站上传和商业发布的日常流程。完整本地编译、启动和功能验证不以提交、推送、PR 或远程 CI 为前提；内部包可使用基础 HEAD 加完整文件哈希快照，支持未提交修改和 detached HEAD。内部打包入口统一为 `release/build_internal.ps1`，通过 `-SourceManifest` 核验快照，保留内部包内容检查、运行时版本和前后源码一致性。只有另行请求远程提交时才应用团队集成流程。历史发布文档不再是当前操作指令。具体规则见 [AGENTS.md](AGENTS.md) 和 [验证交接流程](Docs/coordination/validation-handoff.md)。

**2026-09-11 拒绝记录更正：** 原复制并启动隔离 Orca 命令在用户明确要求下于同一任务、同一工具重试一次，仍在创建进程前返回 `blocked by policy`。当前桌面日志显示 `approval_policy=never`、`approvals_reviewer=user`，具体拒绝规则未知，不能确定属于 Auto-review。保留具体被拒动作，不把脚本前置条件或尚未执行的工作说成新的平台拒绝；不得改用其他入口执行同一被拒操作。该文档修正不代表平台限制解除，当前主窗口验收仍未完成。

**2026-09-09 三人协作实施（2026-09-10 负责人更新）：** 用户已接受 [ADR-007](Docs/architecture/ADR-007-three-developer-feishu-integration.md)，选择现有公开仓库 `arsenaltj/OrcaSlicer`，并明确新建分支、弃用旧开发分支。新四分支使用 `codex/team/{model-generation,smart-slicing,maintenance,integration}`，旧分支保留历史、不覆盖。`origin` 已连接此仓库。三位 GitHub 负责人现已确认：`arsenaltj` 负责模型生成，`tony20160206` 负责智能切片，`tangjiajie15191661723-web` 负责维护及集成协调；团队配置与生成的 CODEOWNERS 已使用真实账号。还需确认协作者权限、新名单进入受保护集成基线及真实 PR 互审，飞书应用／稳定身份／群和持续在线服务机仍待接入。通知／候选检查、分支准备和 CI 的本地实现及上线状态见[实施记录](Docs/audits/2026-09-09-team-feishu-integration.md)，当前操作规则见[团队手册](Docs/coordination/team-integration-sop.md)。采用新流程不代表自动合入或完整产品验收已上线。

**2026-09-09 性能、提示与美颜调整：** 当前请求要求先体验再修改。保留单色写实，多色写实和风格化不限色；质量检查改为提示，保留文件可解析性与任务有效性约束。已完成后台模型预览、取消生成结束自动视觉复核、本地表面柔化和版本恢复改进。详细耗时、真实模型体验、验证范围及后续复测结果统一记录在[本次审计](Docs/audits/2026-09-09-performance-advisory-finishing.md)，覆盖旧文档中质量检查阻断下一步的要求。

当前用户开发线：`codex/team/model-generation`；`codex/continue` 保留续建来源。实际 checkout 用 `git branch --show-current` 核对。

**2026-09-09 最新产品调整与分工：** 取消原图 → AI 设计图 → 3D 生成的系统打印限色，但明确保留“单色写实”风格。取消限色与本地三维美颜／修复由当前任务实现；仅 AI 增强 Orca 原生颜色匹配交给同事。详见[当前实施范围](Docs/plans/2026-09-09-unrestricted-generation-and-finishing.md)、[实现与验证结果](Docs/audits/2026-09-09-unrestricted-generation-and-finishing.md)、[仅第三项的同事交接](Docs/plans/2026-09-09-ai-native-color-matching-handoff.md)。覆盖下文生成阶段提前限色的冲突要求。后续已成功启动主窗口，修复 RGB 图片碎片误拦并验证同一任务的恢复及切页状态；见[图片预检实测](Docs/audits/2026-09-09-reference-preflight-fix.md)。其余完整生成/导入/修整旅程仍未验收。

迁移代码快照提交：`33cdc9711daa55e8162d9c8f24234fb29d9f1024`

**2026-09-09 后续设计图背景偏好：** 用户要求后续不用棋盘格背景。文字生图和图片改图的生产入口统一要求不透明纯色背景，默认中灰色并按主体颜色保持轮廓对比，禁止仿透明棋盘格；保留原图和已确认的历史产物。实现与验证见[背景默认规则](Docs/audits/2026-09-09-solid-background-default.md)。

当前 `HEAD` 会比迁移代码快照多出本文件和精简版 `AGENTS.md` 的文档提交，因此两者本来就不应相等。代码来源核对以 `33cdc971...` 为界；日常开发和工作树清洁度以当前 `HEAD` 为准。可以用 `git log -3 --oneline` 和 `git diff 33cdc9711daa55e8162d9c8f24234fb29d9f1024..HEAD -- AGENTS.md CONTINUATION.md` 确认其关系，这不是证据冲突。

这份文件是新仓库和唯一接手任务的起点。它把“当前代码已经有什么”“用户最终要什么”“哪些结论有证据”“哪些工作仍未完成”分开记录。旧计划和旧任务可以追溯历史，但不再作为当前执行清单。

## 1. 为什么续建老工程

老工程已经形成了可继续演进的 Orca 内嵌架构，而不是只有实验脚本或独立演示窗口：

- `MainFrame` 创建 `AIDesktopFeatureHost`，把 `ModelGenerationPanel` 加入 Orca 主标签栏，页面名为“3D 生成”。
- `Plater` 创建 `SmartSlicingFeatureHost`，由 AUI 和现有侧栏承载智能切片面板，并通过 Orca 的正式切片入口执行试切。
- C++ 契约、Orca 适配器、Python Sidecar、任务恢复、模型下载检查、OBJ/颜色导入以及相应测试已经存在。

因此续建策略是：保留这套主窗口集成和已经验证的生成/导入链路，逐项修正产品体验与质量问题；不从空壳工程重新发明一套独立窗口。

## 2. 快照来源和完整性

本仓库从以下三个本地工作区建立，旧目录均保留原样：

| 来源 | 固定版本 | 在本仓库中的用途 |
| --- | --- | --- |
| `D:/Workspace/06_3DDY_claude` | `6e3c6e658dc964b831f9005f6a97785124d9d9a6`，加 41 个已跟踪工作树修改 | 当前 `codex/continue` 的主要代码来源 |
| `D:/Workspace/06_3DDY_smart_slicing` | `2b6585a1956a6ebe6135318696d7694cbc001ebd` | `archive/smart-slicing-20260908` 只读来源分支 |
| `D:/Workspace/06_3DDY_orca_integration_v2` | `a4d5d89ae316de692aa6afc88eb3a65bf57ce35c` | `archive/orca-integration-20260908` 只读来源分支 |

主要工作树的 41 个修改文件和 25 个新增源码、测试、验证脚本及必要设计文档按 SHA-256 逐文件比较，迁移时为 0 个不一致。它们形成代码快照提交 `33cdc9711daa55e8162d9c8f24234fb29d9f1024`。

本仓库是独立 Git 仓库，未保留指向旧目录的 `origin`。只配置 Orca 官方 `upstream` 用于以后核对上游。没有创建远程仓库，也没有推送。

以下内容没有迁移：构建目录、缓存、临时文件、生成模型、截图、3MF/G-code 验证产物、EXE/ZIP、`.env`、供应商配置或凭据、旧会话注册表、旧 Symphony 实时状态、六角色任务记录、未核实的审视资料包，以及单独的未跟踪网站目录。它们仍在旧工程中；“未迁移”不表示可删除。

智能切片最新分支和当前快照的差异比较被平台检查拦截，只返回 `Potentially unintended activity`，没有具体原因。没有通过其他方法重试。因此本分支保留的是主要工作树当时已有的智能切片实现，不能宣称已经吸收 `2b6585a...` 的全部新增内容。集成工作树另有未提交的 `AGENTS.md` 和 `release/README.md`，也没有覆盖到当前代码。

## 3. 用户最终要做的产品

产品是一套集成在 OrcaSlicer 主窗口里的 AI 创作与打印工作台。用户用文字、图片或两者组合完成设计、模型生成、打印适配和智能切片，并始终能够回到 Orca 的手动工具。

### 输入、风格与作品

- 支持纯文字、纯图片、图片加文字；组合输入不能静默丢失任何一类信息。
- 系统主动理解主体和设备条件，推荐合适风格并解释原因，用户可以方便地改选。
- 一级风格固定为：单色写实、多色写实、多色风格化；手办、雕塑、卡通等属于风格化的细分。
- 原始图片、文字要求、选定的 AI 设计图、供应商原始模型、打印模型版本、设备配置和切片结果属于同一个作品记录。
- 原始输入永不覆盖。版本要记录父子关系、任务标识、供应商/模型版本、关键参数和文件哈希。

### 写实人像

- 写实人像是重点能力，分别评价身份相似度、审美、三维结构、可制造性和打印后的成立程度。
- 美颜必须受控、强度可调、可比较、可撤销，并同时考虑三维几何和表面效果。
- 不把二维磨皮、好看的渲染图或把原照片像素贴回造型图当作三维人像完成。
- 多照片和多视角是增强能力，不把固定照片数量、固定胸像形态或某个特定算法写死为前提。

### 六色及以下与 CMYK 视觉叠色

- 支持 1、2、3、4、5、6 色 FDM 配置。
- 六色 CMYK 叠色指多喷头颜色的空间排列或层次形成视觉颜色，类似彩色像素共同形成观感；近看可以辨认组成色或层次。
- 固定耗材分色与 CMYK 视觉叠色是不同策略。不能把二维印刷公式或同喷嘴材料混合当作已知硬件事实。
- 采用普通 3D 打印材料；当前不设置固定作品尺寸、尺寸范围或硬性等待时间。质量优先，但真实服务费用和重试不能无限增长。
- 减少可用颜色时重新适配设备和外观，不截断颜色编号或静默丢色。

### 供应商替换

- 当前 3D 生成代码实际接入的是 Tripo；混元 3D 等属于目标供应商，尚不能写成已接通。
- 当前图片、文字和视觉理解使用 OpenAI 兼容调用，未来可替换。
- 供应商适配器要表达输入能力、异步任务、恢复、取消、版本、输出格式和错误；不能把提供商策略散入 `libslic3r` 或 GUI。
- 故障切换、重启恢复和重复点击不得悄悄创建第二个收费任务。

### 智能切片

- 智能切片是正式产品模块，既能处理生成模型，也能独立处理用户导入的模型。
- 它依据模型、设备、材料和用户目标提出方向、支撑、层高、速度、材料分配等候选方案。
- 候选方案要展示真实试切产生的时间、材料和风险信息，允许比较、选择、应用和撤销。
- 导入、试切、应用方案和正式切片是分开的显式动作；导入不能自动改预设或开始切片。
- 任何硬件控制和 G-code 仍由 Orca 的正式适配和切片路径产生，语言模型不能凭文字直接猜控制指令。

### 主窗口体验

- 模型生成的主要流程在 Orca 主窗口的页面/标签/停靠区域中完成；智能切片在同一主窗口的打印准备流程中完成。
- 创作、打印准备、切片和预览共享明确的当前项目。切换页面不丢输入、版本或运行中的任务，也不重复提交。
- 原图、AI 设计图和 3D 模型在主内容区比较；导入表示把确认的模型加入当前打印板。
- 文件选择、简短设置和必要确认可以使用对话框；完整生成或智能切片工作流不能退回独立顶层窗口。
- 普通用户先看到下一步、结果和可恢复状态；高级质量报告、供应商字段和诊断信息按需展开。

## 4. 当前代码架构

项目仍是一个桌面模块化单体：

```text
Orca MainFrame / Plater
        |
        +-- GUI/AI/AIDesktopFeatureHost
        |       +-- ModelGenerationFeatureHost -> ModelGenerationPanel
        |       +-- OrcaWorkspaceAdapter -> Orca Model / Prepare
        |
        +-- GUI/AI/SmartSlicing/SmartSlicingFeatureHost
                +-- Presenter / Panel
                +-- AI/SmartSlicing/Application/SmartSlicingCoordinator
                +-- Ports -> Orca trial slice / proposal service / runtime store

ModelGenerationPanel
        -> AIModelGenerationClient
        -> local Python Sidecar
        -> input/preprocess/provider/quality/download/color-intent
        -> ModelGenerationArtifactFlow
        -> OrcaWorkspaceAdapter
        -> current Orca project and print bed
```

关键边界：

| 范围 | 位置 | 责任 |
| --- | --- | --- |
| 跨功能契约 | `src/slic3r/AI/Contracts` | 产物、颜色意图、导入请求等稳定接口 |
| 智能切片领域和应用 | `src/slic3r/AI/SmartSlicing` | 工作流状态、候选、比较、事务应用/撤销 |
| 桌面组合 | `src/slic3r/GUI/AI` | FeatureHost、面板/Presenter、Sidecar 与 Orca 适配 |
| 模型生成 UI | `src/slic3r/GUI/ModelGenerationPanel.*` | 主窗口创作流程和状态呈现 |
| 原生 Orca | `src/libslic3r`、`Plater`、`MainFrame` | 模型、配置、切片、预览和少量明确组合入口 |
| Python 生成链路 | `tools/ai` | Provider Gateway、Tripo、预处理、恢复、质量检查和 Sidecar |
| 架构约束 | `docs/architecture/ai-integration-lock.json` | 上游版本、运行协议、端口、所有权和差异预算 |

上游升级原则是保留 Orca Git 谱系，固定可复现基线，把本地改动集中在少数 FeatureHost/适配入口，通过兼容测试逐步吸收上游发布或经核实的 nightly 能力。不会承诺永远无冲突，但要让冲突可定位、可测试。

## 5. 已有证据支持的完成状态

以下结论来自当前源码和 2026-09-07 的验证记录：

- 主窗口已经有“3D 生成”页面；智能切片已有 AUI/侧栏 FeatureHost、Coordinator、候选、试切、应用和撤销相关代码。
- 模型生成已有输入、风格/配色、预处理、异步任务、恢复、模型下载、质量检查、3D 预览和导入流程。
- 预览状态修复避免把服务端配色角色校正误判为用户修改；有限重试区分图像生成、视觉复核和下载阶段。
- AI 彩色模型导入默认走 Orca 原生完整颜色匹配窗口，保留取消状态、实际着色数量、物理耗材和虚拟叠色编号。
- Windows Release 程序曾完成本机构建、启动和合成数据操作；验证了继续生成、原生颜色匹配、手工 CMYW 叠色、取消/确认导入、手动切片和 G-code 导出。
- 对应记录报告 205 项定向 Python 测试通过，18 个 C++ 用例 / 1,188 次断言通过，四个受影响 GUI 翻译单元编译通过。

这些证据不等于当前快照已经重新构建，也不等于真实供应商、真实打印或发布验收通过。详细证据见：

- `Docs/plans/2026-09-07-generation-recovery-verification.md`
- `Docs/plans/2026-09-07-generation-local-validation.md`
- `Docs/audits/2026-09-07-ai-engineering-asset-audit.md`

## 6. 明确未完成或未证实的事项

- 没有用当前续建提交重新完成全量应用构建、GUI 回归和跨平台验证。
- 没有证明当前真实 Provider 凭据有效、服务稳定或远端测试者已获得修复版本。
- 混元 3D 等第二家真实 3D Provider 尚未完成接入验收。
- ColorIntent、OBJ 颜色和原生匹配通过不证明六色 CMYK 切片表达、硬件行为、色差或实物质量合格。
- 精确喷头拓扑、工具号、设备 profile 和代表性实物质量阈值仍需在具体硬件接入时确定。
- `archive/smart-slicing-20260908` 的最新智能切片变化没有与本分支完成差异审查或整合。
- 当前代码具备主窗口承载，但完整用户旅程、状态保留、视觉布局和失败恢复仍需按最新产品要求做实际 UX 验收。
- 集成检查曾因测试夹具中的固定模拟凭据报告 `security.secret_content`；不能删除检查或把失败写成通过。未来发布须分别检查实际 EXE 与 ZIP。

## 7. 保留的平台阻塞

以下操作曾被平台检查阻止，没有具体原因或完成结论：

- 旧 ENGINE 操作和一次中断检查临时目录清理。
- 旧模型生成任务生命周期/异步身份/取消重启/颜色交接复核。
- 旧 Git 分支/工作树冗余与目录冗余全面盘点。
- 两项辅助产品 UX / 工程边界复核。
- 本次对最新智能切片分支的有界迁移差异比较。

新任务不得通过另一个代理、工具、会话或入口重放这些具体操作。它们不禁止当前仓库内其他明确、可逆、已授权的开发和验证；需要这些结论时，等待平台复核真正解除，并保留现有来源分支。

## 8. 开发与验证任务分工

2026-09-11 用户确认：保留一个主要开发任务，并复用一个固定验证／交付任务；后者使用独立工作区接收确定版本、修改点和验证点，完成后将证据与结论反馈原开发任务。具体规则见 [AGENTS.md](AGENTS.md) 和[验证交接流程](Docs/coordination/validation-handoff.md)。此修正替代原先“仅一个用户可见任务”的限制。开发任务仍一次完成一个可审查的小目标，不恢复六个永久角色窗口。

以下是建仓时的首次基线检查清单，保留作接手参考，不是每次接手都必须重跑的待办。仅在当前任务缺少相应基线证据时读取有关资料和核对有关路径；已明确的实现请求继续按最新要求完成。原始检查内容：

1. 阅读本文件、`Docs/AI_ENGINEERING.md`、完整产品任务书和集成锁。
2. 核对 `codex/continue`、快照提交、三个 `archive/*` 来源分支及工作树是否干净。
3. 沿 `MainFrame -> AIDesktopFeatureHost -> ModelGenerationPanel -> Sidecar -> ArtifactFlow -> OrcaWorkspaceAdapter` 阅读生成主路径。
4. 沿 `Plater -> SmartSlicingFeatureHost -> SmartSlicingCoordinator -> Ports` 阅读当前智能切片路径，只读当前分支；不比较被阻塞的来源分支。
5. 产出 `Docs/continuation/INITIAL_ASSESSMENT.md`，按“已实现且有证据 / 已实现但需验证 / 仅目标或提案 / 明确受阻”分类，并提出三个最小的后续工程任务。

上述评估文件仅在用户要求基线评估或实际交接需要时产出；文件不存在不构成已授权开发的停工条件。不因接手自动启动付费生成、构建发布包、推送、联系测试者或恢复旧任务。已有实现和验证以当前任务及最新记录为准。
