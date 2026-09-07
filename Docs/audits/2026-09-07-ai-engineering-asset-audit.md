# AI Engineering Asset Audit

日期：2026-09-07。范围：当前工作树的 AI 工程资产、导航和可复用验证流程；不修改业务代码或核心架构。

审计基点：分支 `codex/model-generation-v2`，HEAD `6e3c6e658dc964b831f9005f6a97785124d9d9a6`。工作树已有大量未提交修改，因此下述“当前实现”包含工作树状态，不能等同于受审提交、发布版本或线上状态。审计前保存了 40 个既有文件的哈希用于检查改动边界。

## 1. 项目事实模型

### 产品目标

基于 OrcaSlicer，将文字/图片转为可检查、可导入、可配色的打印模型，并提供独立的智能切片工作台。六色固定耗材和六通道过程色是需要区分的产品能力。真人高相似度、美颜、制造母版等方向仍有提案，不能把目标设计写成已交付功能。依据：[简化生成设计](../plans/2026-09-04-simplified-design-flow-design.md)、[ADR-006](../architecture/ADR-006-six-channel-model-color-intent.md)、[人像路线提案](../architecture/realistic-beautified-portrait-printing.md)。根 README 主要介绍上游 Orca，尚不能单独解释本项目 AI 增量。

### 主要架构

- C++17/wxWidgets/CMake 桌面模块化单体，原有 `Model`、配置、切片与预览继续由 Orca 管理。
- `src/slic3r/AI/Contracts` 定义跨功能契约；`AI/SmartSlicing` 有 Domain/Application/Ports 和独立 CMake 库；GUI FeatureHost 与 `GUI/AI/Orca` 适配器负责桌面组合。
- Python Sidecar 承担图像预处理、模型生成、产物转换与质量报告。当前锁定契约是 Sidecar v9、protocol v2、产品端口 18764；开发端口只能显式覆盖。
- Provider Gateway 已有抽象，但当前实际 3D 后端是 Tripo。OpenAI 兼容图像/文本/视觉调用用于前后处理，并非另一条已接通的 3D Provider。

依据：[集成锁](../../docs/architecture/ai-integration-lock.json)、[FeatureHost](../../src/slic3r/GUI/AI/AIDesktopFeatureHost.cpp)、[CMake 边界](../../src/slic3r/AI/CMakeLists.txt)、[Gateway](../../tools/ai/model_provider_gateway.py)、[预处理](../../tools/ai/openai_preprocessor.py)。锁是可执行契约，ADR 解释决策；二者不一致时应报告差异，不能自动改锁来配合文档。

### 模型生成数据流

`ModelGenerationPanel → AIModelGenerationClient → 本地 Sidecar → 输入检查/参考图预处理 → Provider Gateway/Tripo → 下载和 OBJ 转换 → 网格、颜色、视觉质量检查 → job/产物及 color-intent → 客户端下载校验 → ArtifactFlow → OrcaWorkspaceAdapter → Orca Model/Prepare`。

输入预览、Provider 原始结果、处理后模型和最终导入状态是不同证据。核对入口：[面板](../../src/slic3r/GUI/ModelGenerationPanel.cpp)、[客户端](../../src/slic3r/GUI/AIModelGenerationClient.cpp)、[Sidecar](../../tools/ai/orca_ai_sidecar.py)、[产物流](../../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationArtifactFlow.cpp)、[适配器](../../src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp)。

### 生成与智能切片的实际边界

生成导入请求已不含自动切片开关，导入不应隐式改打印预设或开始切片。智能切片由 `SmartSlicingCoordinator` 管理候选、revision、试切和应用，通过 Ports 调用 Orca；不是模型生成面板的内部步骤。[ADR-002](../../docs/architecture/ADR-002-smart-slicing-transactional-workbench.md)、[导入契约](../../src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp)、[协调器](../../src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp)。

当前 Sidecar 产生颜色意图，客户端校验其 schema/hash/关联模型；但本次检索和适配器阅读未发现 SmartSlicing 或 OrcaWorkspaceAdapter 消费 `color_intent`/ProcessMix 配方的完整链路。ADR-006 的“保存交接、后续求解”是边界设计，不能当作六通道配方、G-code、硬件打印已闭环的证据。此项需要后续有明确验收条件的功能任务，本次只记录。

### Orca 继承能力与新增 AI 能力

| 类别 | 代码及核实范围 |
|---|---|
| 锁定基线已有能力 | `src/libslic3r/Print.cpp`、`ColorDecomposeRecipe.cpp`、`GCode/ToolOrdering.cpp` 在锁定 upstream 对象中存在，本次与工作树逐文件 diff 为空；切片、配方分解和工具排序不是本轮新写的 AI 功能。 |
| 基线已有但本地也有增量 | `ObjColorUtils.cpp` 在基线存在，当前工作树另有颜色处理修改；应按 diff 区分继承与本地修复，不能整文件归类为原生或新增。 |
| 新增 AI | Sidecar、Provider Gateway、预处理/质量门控、生成客户端/面板、AI Contracts、SmartSlicing 协调器与 GUI 适配集成。 |

这里“原生”仅指项目锁定的 upstream Git 对象，未据此断言任何其他 Orca 官方版本支持相同能力。检查方式：读取锁中 SHA 后执行 `git cat-file -e SHA:path`、`git diff --numstat SHA -- path`，不拉取或改写历史。

### 最重要的架构风险

| 优先级 | 风险 | 证据与影响 |
|---|---|---|
| P0 | 将颜色契约或视觉质量误认为打印闭环 | DTO/客户端已有支持，但完整消费和实物校准证据不足；会导致错误验收和越界开发。 |
| P1 | 两个责任集中点接近上限 | 审计时 Sidecar 9426/9450 行，面板 5139/5200 行。预算防止继续堆积，但不是架构质量证明。参见 ADR-005 和集成锁。 |
| P1 | “当前”文档与代码漂移 | ADR-003 仍称 v8，锁/当前实现是 v9；锁定 upstream 的“当前”文字停留在旧日期。 |
| P1 | 证据分散且部分未提交 | `.agents`、协调/审计资料等有未跟踪内容；同机可读不代表从 Git 能完整恢复。固定 SHA 的结果和工作树结果不能互换。 |
| P1 | 验证层次被混淆 | 结构分析、视觉评分、试切、正式切片和真实打印不是同一门槛；采样壁厚也不能证明所有位置合格。 |

## 2. AGENTS.md 审查

扫描自有源码和文档中的 `AGENTS.md`、`agent.md`、`AGENT.md`、`CLAUDE.md`、`SKILL.md`；排除依赖、构建、缓存、生成输出。找到根、tests、website 三份 AGENTS，以及根/tests 的 CLAUDE 转发文件。没有单独项目级 prompts/ 或 skills/ 工作流目录；项目 Skill 位于 `.agents/skills`。本审计不修改用户目录中的通用 Skill。

| 资产 | 结论与建议 | 优先级 |
|---|---|---|
| 根 AGENTS，审计前 127 行、约 13 KB | 行数尚可，但翻译细则、发布历史占据大量默认上下文；标题还是 CLAUDE.md，缺少 AI 产品目标、模块/测试/Skill 导航。补导航，翻译细则原文迁移到 localization/AGENTS.md。 | P1 |
| 根兼容性/回归/代码风格 | 有少量语义重复，但默认不变、3MF/profile 兼容和 GUI 布局规则仍有决策价值；保留。 | P1 |
| 根发布与团队规则 | 历史授权与后续澄清必须按具体对象和最新适用范围阅读。存在“本轮”指代成本，但不能推测删除历史规则。本次保留整段原文；将来单独审查是否能抽取稳定规则。 | P0 |
| tests/AGENTS.md | 约 60 行，清楚且可执行；`slic3rutils` 描述仅提 Python，漏掉实际注册的 AI 契约、面板、SmartSlicing 测试。补一行导航；保留测试规范。 | P1 |
| website/AGENTS.md | 规则仅适用于网站；“This repository contains only…” 对当前父仓库容易误读。建议后续网站维护时改为该目录/子项目；本次不改网站。其安全/迁移/部署规则保留。 | P2 |
| CLAUDE 转发文件 | 单一入口的兼容转发有价值，无需删掉或复制规则。 | P2 |

CI 已覆盖集成锁、版本常量、目录依赖、规模预算、Git receipts 和 Python 单测。根 AGENTS 应链接工具，不重复展开锁的所有字段。兼容性、功能关闭行为、真实打印和用户授权没有被这些静态检查充分证明，不能以“已有 CI”为由删掉核心规则。CODEOWNERS 只列出所有者，未核实远端分支保护，不能说独立审查已被强制执行。

只新增 localization 的局部 AGENTS：它承载从根迁出的原有规则，确有作用域收益。AI 代码分布在 C++/Python 多处，当前用单个入口路由即可；不为每个目录复制一份规则。

## 3. 当前 Skill 清单与处置

| Skill | 状态 | 当前用途/触发 | 问题与建议 |
|---|---|---|---|
| symphony | Keep | 用户要求记录工程指令、恢复任务证据、协调已有项目任务时，读取 state/合同/结果并作只读 Git 核对。 | 唯一项目内现有 Skill；无项目内重复项。含明确 bootstrap、记录回读、交付/检查点 SOP，不是知识文档伪装。已说明 helper 未实现，不能执行旧示例或自动恢复被暂停工作。保留全部原文。 |

Symphony 跨越任务记录与协作，但受本项目和用户任务约束，并非通用“自动执行一切”。目前拆成多个 Skill 只会复制状态规则。其改进重点是未来实现并验证既定 helpers 和恢复场景，而不是增加文字。本轮不解除其既有阻断、不运行其待实现工程。

用户环境中有 architecture-designer、skill-creator、Code、通用研究/绘图等资产，它们不在当前仓库的维护边界；不能把全局目录清单当成本项目 Skill 数量，也不进行全局安装、合并或删除。

## 4. 缺什么 Skill：从重复工作反推

重复证据：phase64–80 系列质量评审记录、多套 printable/model 测试、冻结批次质量报告、近期人像和颜色交接审查。模型结果的“拿哪份产物、用哪些指标、哪些结论不能推出”被反复重建，最适合固化。

| 候选 | 决策 | 收益/成本与原因 |
|---|---|---|
| model-generation-evaluation | P1，新增一个窄 Skill | 高收益/低成本：仅评估已有本地产物，复用结构分析和冻结批次报告；明确输入、命令、证据和限制。 |
| mesh-quality-check、printability-check、color-print-pipeline-check | P1，合并到上述 SOP 的检查维度 | 同一份模型、同一报告和相同授权边界；单独建三个会重复和漏掉跨阶段判断。 |
| architecture-review、orcaslicer-change-review、regression-review | P1，先用导航+现有 guardrails/测试矩阵 | 已有 ADR、锁、所有权和 tests 规则；没有必要再造通用审查 Skill。未来出现稳定特有遗漏再提炼。 |
| release-validation | P1，暂不新增 | 已有 release runbook、包检查脚本和 CI；发布分支及例外仍需单独验证，另建 Skill 容易成为第二授权来源。 |
| AI-provider-integration | P2，暂不新增 | Gateway/Tripo 有接口及测试，但第二家真实 3D Provider 的稳定集成 SOP 证据不足；先维护现有契约。 |

## 5. Skill 与知识分离

增加一份精简的 `Docs/domain/printing-color-boundaries.md`，只记录本项目确有歧义的知识：物理通道/目标色/虚拟配方、离散耗材与过程色、纹理与顶点色、模型和项目格式、结构预检与打印资格。链接代码和既有 ADR，不复制完整 FDM/Mesh/CMYK 教材。

不建立 FDM Knowledge Skill、Mesh Knowledge Skill 或 CMYK Knowledge Skill；也不拆成五份短小而重叠的领域文档。喷头拓扑、实测材料光学参数、六通道具体组合与实物合格阈值仍未知；不得用常识补成硬件事实。

## 6. 可复用的 Prompt / Script / Test

| 工作 | 现有资产 | 能证明什么/不能证明什么 |
|---|---|---|
| 结构预检 | `printable_model_quality.analyze_printable_obj`、`sampled_local_thickness.py` | OBJ 解析、边界/非流形边、连通/悬浮、接地、薄区采样、悬垂区域、顶点颜色覆盖；不是完整自交检测、机械强度或全表面壁厚证明。 |
| 结构回归 | `test_printable_model_quality.py` | 有四面体、盒、旋转薄件、连接薄颈等程序化样例；可直接复用，不需先下载真实人像。 |
| 冻结批次复评 | `run_tripo_quality_report.py --manifest … --tripo-root … --output …` | 读取已有 manifest/validation-state/已下载 OBJ，输出结构 JSON 和五视图；会写报告及 review 文件，需独立输出/副本，不重新生成。 |
| 图像/视觉质量 | `model_input_image_quality.py`、`printable_reference_visual_quality.py`、`printable_visual_quality.py`、对应测试 | 区分本地指标与远程视觉判断；不能把存在函数名当作已完成实物盲测。 |
| 颜色与交接 | `test_color_intent.py`、`test_printable_palette.py`、`test_ai_contracts.cpp`、`test_orca_palette_snapshot_builder.cpp`、`test_model_vertex_colors.cpp` | schema/颜色/模型数据回归；不证明硬件色域和配方准确度。 |
| 智能切片 | `tests/slic3rutils/test_smart_slicing_*.cpp`、`test_ai_sidecar_client.cpp` | 状态、proposal、revision、候选与运行契约；仍需集成 GUI 与正式切片验收。 |
| 架构回归 | `python scripts/verify_ai_integration.py --json`；`.github/workflows/ai-integration-guardrails.yml` | pins/receipts、边界、预算、版本及 Python 测试入口；不能推导全平台发布合格。 |
| 分发检查 | `release/verify_package_contents.py`、其测试、commercial candidate workflow | 按实际包扫描并绑定身份；不在本审计中运行发布、重新解包指定敏感产物或改变授权规则。 |

Prompt 主要嵌在预处理和参考图工作流代码中，没有必要为“统一 Prompt”复制到新目录。修改 prompt 时应关联输入质量/参考图/模型结果回归。`quality_benchmark.py`、`printable_palette_benchmark.py` 和 `run_paid_*` 可能调用真实服务，不能因名称含 benchmark 就当作离线检查。现有 palette manifest、manual approval 和图像哈希可作冻结样例入口，真实私有图片和付费结果不复制进 Skill。

## 7. 最小上下文体系

保留现有 `Docs/architecture`、历史 `Docs/history`、coordination 与 release 入口；不平移成新架构目录或新 decisions 树。当前 ADR 已是决策记录，增加一套会分裂来源。

建议新增且本轮计划落地：

1. `Docs/AI_ENGINEERING.md`：产品、架构导航、当前/历史来源和按改动选验证的单一索引。
2. `Docs/domain/printing-color-boundaries.md`：一个有代码依据的领域边界文档。
3. `.agents/skills/model-generation-evaluation/SKILL.md`：一个复用现有工具的 SOP。

已有文档同时使用 `Docs/` 和 Git 跟踪的 `docs/` 路径，Windows 大小写不敏感掩盖问题。后续需要一次独立、跨平台验证的路径归一化；本轮链接按目标的 Git 路径拼写，不大规模移动历史文件或修改 CI。

## 8. 上下文加载策略

| 层级 | 内容 | 加载原则 |
|---|---|---|
| Always Load | 根 AGENTS 与任务涉及目录的局部 AGENTS；用户当前指令 | 目标、禁区和导航。不会要求每次阅读整个仓库或所有 ADR。 |
| Load On Demand | AI 导航及对应 ADR/锁片段、tests 入口、release、团队 registry、Symphony state | AI 任务才读 AI 导航；集成/版本任务才读锁和 receipts；协作任务才读任务状态；发布任务才展开授权档案。 |
| Skill 自动定位 | 已有产物质量评估、Symphony 证据恢复 | 根据任务匹配 SOP，再定位需要的代码/样例/工具，不预加载所有测试和报告。 |
| 历史证据 | phase 报告、plans、旧质量波次、提案 | 仅在解释某决策、回归或复现实验时读取；不当作当前可执行命令或验收状态。 |

避免将整个 Sidecar、面板或 Plater 首次全读入上下文：先按函数/契约检索，再沿当前调用链阅读；涉及变更时查看实际 diff 和关联测试。上下文成本的收益来自路由和事实分层，不是再加一层通用 Prompt。

## 9. 成熟度与 Top 10

定性评价：已有较强的工程护栏和测试积累，属于“可维护但证据导航尚不完整”的阶段。强项是契约/依赖/预算 CI、可重用质量分析和固定 SHA 集成约束；弱项是知识入口、历史与当前状态区分、从模型到实物的验收闭环。没有依据给一个看似精确的总分，也不能称为全自动工程平台。

P0 表示应先防止错误决策/验收；P1 是近期高价值维护；P2 是按需求推进。成本是相对工程量，不是报价。

| # | 优先级 | 改进项 | 收益 | 成本 | 本轮处理 |
|---|---|---|---|---|---|
| 1 | P0 | 明示生成、切片、硬件执行和验收边界 | 防止把颜色 DTO/视觉效果当完整交付 | 低；闭环实现高 | 文档落地，功能后续立项 |
| 2 | P0 | 保留精确授权及平台复核，发布判断链接唯一证据 | 避免历史事实、当前授权、执行许可混淆 | 低；发布实现另算 | 保持原文，不执行线上操作 |
| 3 | P1 | 根导航与局部翻译规则 | 降低无关上下文，减少找入口成本 | 低 | 本轮落地 |
| 4 | P1 | 用锁指向当前运行契约，历史 ADR 留日期 | 消除 v8/v9 与 upstream 误读 | 低 | 本轮修正 ADR-003 |
| 5 | P1 | 单一模型质量评估 Skill | 复用已有工具、样例、证据格式 | 低 | 本轮新增并做离线验证 |
| 6 | P1 | 完善 AI 测试导航与门槛区分 | 避免跑错 suite 或用 Python 通过替代 C++/GUI | 低 | 本轮落地 |
| 7 | P1 | Sidecar/面板按责任继续渐进拆分 | 降低变更耦合与预算压力 | 中至高 | 仅建议，业务代码不动 |
| 8 | P1 | 冻结模型/颜色/打印验收集并绑定版本 | 支持模型质量和实物色差的可靠比较 | 中至高，需要硬件数据 | 复用现有样例，实物标准待决策 |
| 9 | P1 | 受审的 Harness 文件纳入 Git，未来验证 Symphony 恢复 helpers | 让跨会话/机器恢复可核实 | 中 | 本轮仅留下可审 diff，不混提既有修改或恢复旧操作 |
| 10 | P2 | 文档路径大小写、website 局部措辞及链接维护 | 提升 Linux/CI 和子项目可理解性 | 低至中 | 局部链接核对；大范围归一化留后续 |

仍需用户决策的是实际产品取舍：六色固定耗材/过程色的交付先后、硬件通道与标定资料、实物验收门槛及代表样例；以及何时安排颜色消费链和两处大文件的工程改造。这些不阻止本轮低风险整理，也不在本轮要求重新确认已有权限。

## 10. 整理与验证记录

审计结论先于资产整理形成。本次整理共涉及 8 个文档/Skill 文件：根 AGENTS、tests/AGENTS、ADR-003，以及新增的本报告、AI 导航、领域边界文档、模型评估 Skill 和 localization/AGENTS。

- 根 AGENTS 从 127 行/13036 UTF-8 字节变为 106 行/10884 字节，字节减少约 16.5%；补充产品/模块/测试/Skill 入口，改正标题，将翻译规则原文下移到适用目录。
- tests 仅补齐实际存在的 AI 测试归属；ADR-003 保留旧 SHA/版本作为历史，将当前值指向机器可读锁。
- 没有删除任何文件或现有 Skill。翻译规则和根发布段落逐段对比一致；Symphony Skill/state、授权档案和其他既有文件未改。
- 验证快照中除上述 3 个有意编辑的既有文件外，其余 37 个既有文件哈希不变；暂存区未变化。没有修改业务代码、运行发布或付费生成。

检查结果：

| 检查 | 结果 |
|---|---|
| 新旧两份 Skill 的 quick_validate | 使用 `python -X utf8` 均通过。首次 Symphony 检查遇到 Windows 默认 GBK 解码失败，显式 UTF-8 后通过，未改校验器或旧 Skill。 |
| 结构、颜色契约、冻结批次报告的既有离线测试 | `python -m unittest tools.ai.test_printable_model_quality tools.ai.test_color_intent tools.ai.test_run_tripo_quality_report -q`：43 项通过。 |
| 新 SOP 单 OBJ 命令实际运行 | 合成闭合四面体返回 pass；非法索引返回 reject/invalid_vertex_index；已有报告被拒绝覆盖且内容不变。证据在本地 `output/ai-engineering-audit-20260907/`，未提交生成文件。 |
| 文档本地链接与 Git 路径大小写 | 8 文件、63 个本地链接通过。 |
| 相关 tracked diff whitespace | `git diff --check` 通过；未把既有工作树 diff 当成本轮净改动。 |
| 完整 AI integration guardrail | **未通过**：`security.secret_content`，定位 `tools/ai/test_diagnostic_failure_flow.py:106`。该文件在本轮前已修改，本轮哈希未变。保留疑似字面凭据命中，不输出命中内容；未据此判断凭据有效性，也未修改扫描规则或业务测试来使其通过。 |

没有执行全量 C++ 构建、真实模型生成、真实批次渲染、GUI 或实物打印；本轮文档/SOP 验证不构成产品验收。整体集成检查的既有命中仍需单独核实，不能报告仓库全绿。

**随后用户转向的独立工作：** 用户再次遇到平台弹窗，提供官方说明并要求改为“公开安装包不含供应商凭据，测试配置通过独立受控渠道提供”。这发生在上述审计验证之后；后续发布规则/实现变更应单独记录，不能将其混入上述 8 文件审计范围，也不把历史授权或告警改写为从未发生。
