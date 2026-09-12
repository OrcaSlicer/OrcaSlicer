# AI 工程导航

本项目在 OrcaSlicer 上增加模型生成和智能切片。目标是将文字/图片转为可检查、可导入的彩色打印模型，并由 Orca 管理打印配置、切片和预览。先按任务选择下表入口，不必通读所有历史报告或整个 Orca 仓库。

## 事实与决策入口

- 三人开发、集成与飞书协作：[ADR-007](architecture/ADR-007-three-developer-feishu-integration.md)、[日常同步、合入与版本存档](coordination/team-integration-sop.md)、[落地和接入状态](audits/2026-09-09-team-feishu-integration.md)、[分支准备工具](../scripts/team_collaboration/README.md)、[飞书服务](../tools/team_integration/README.md)。采用四条新的 `codex/team/*` 分支，旧角色分支停止用于新工作；先通知和候选检查，人工合入。

- 当前实现：工作树代码和对应测试；报告须说明使用 HEAD 还是含未提交修改的工作树。
- 最新体验与修改：[性能、质量提示和 3D 美颜](audits/2026-09-09-performance-advisory-finishing.md)，含实际模型加载、美颜保存回退与原生导入取消验证；质量判断不再拦截下一步。
- 当前产品修正：[不限色生成与本地三维修整](audits/2026-09-09-unrestricted-generation-and-finishing.md)；保留单色写实，仅[AI 原生配色交接](plans/2026-09-09-ai-native-color-matching-handoff.md)由同事实施。[RGB 预检误拦修复](audits/2026-09-09-reference-preflight-fix.md)已验证实际任务恢复、图片确认和切页；其他 GUI 主路径仍需验收，不能用编译结果代替。
- 本机续建编译路径：[2026-09-08 工具链与构建记录](plans/2026-09-08-ai-journey-interaction-fixes.md#本机构建路径2026-09-08-复核)，记录已验证的 MSVC、CMake 和依赖前缀；产物写入当前续建仓库。
- 集成基线、运行版本、端口、所有权和预算：[ai-integration-lock.json](../docs/architecture/ai-integration-lock.json)。不要在导航中复制会漂移的版本值。
- 架构决策：[模块边界与 lineage](../docs/architecture/ADR-003-upstream-lineage-ai-integration.md)、[渐进拆分](../docs/architecture/ADR-005-guarded-incremental-ai-decomposition.md)、[颜色交接](architecture/ADR-006-six-channel-model-color-intent.md)、[智能切片事务](../docs/architecture/ADR-002-smart-slicing-transactional-workbench.md)。Accepted 表示接受的设计，实际完成度仍需代码和验收证据。
- 硬件/颜色术语及检验限度：[打印与颜色边界](domain/printing-color-boundaries.md)。
- 本次资产盘点和未解决问题：[2026-09-07 审计](audits/2026-09-07-ai-engineering-asset-audit.md)。这是日期快照，不是新的实时任务表。
- 开发方式与交接：[GPT-6 复评](coordination/symphony/AUDIT.md)、[日常用法](coordination/symphony/README.md)、[可选交接模板](coordination/symphony/HANDOFF.md)。默认在 Codex 内完成工程任务；Chat/ChatGPT Work 只在具体问题需要时加入。
- 工作区与会话选择、已执行归档及剩余阻塞：[2026-09-07 整理记录](coordination/symphony/runs/TASK-20260907-ORGANIZE/RESULT.md)。Codex 的“3D打印 · 当前”集中主入口，“3D打印 · 专项”保留专项任务；这是核对快照，不替代实时任务状态，未核实的分支和目录不能据此删除。

## 文件收纳与工作区选择

| 类别 | 现有位置 | 使用原则 |
|---|---|---|
| 产品源码和资源 | `src/`、`resources/`、`tools/ai/` | 按下表的功能边界查找；名称相似的契约、适配器和实现不是当然重复。 |
| 验证与交付工具 | `tests/`、`scripts/`、`release/`、`.github/workflows/` | 分别维护测试、构建/检查、打包与 CI；清理前确认调用者和历史产物依赖。 |
| 稳定规则与当前导航 | 根/局部 AGENTS、本文、`docs/architecture/`、`.agents/skills/` | 保留稳定约束；具体数值从集成锁读取。 |
| 任务与历史证据 | `Docs/coordination/`、`Docs/history/`、`Docs/plans/`、`Docs/audits/` | state 是实时任务记录；报告/计划按日期理解，不复制成第二份当前状态。 |
| 网站 | `website/` | 单独职责和局部 AGENTS；不能因主桌面构建不使用就认定无用。 |
| 依赖、构建和运行产物 | `deps/`、`build/` 及各 worktree 的构建目录 | 可重建性、运行占用和具体保留版本核实后，才能进入清理批次。 |
| 本机实验与生成资料 | `.planning/`、`output/`、`generated_models/`、`.tmp/`、`tmp/`、`projects/` | 可能含唯一源文件、模型或验收证据；忽略/未跟踪不等于可删除，不整目录清空。 |

当前续建工作区是 `D:/Workspace/11_3DDY_Continue`，团队远程为 `arsenaltj/OrcaSlicer`。原 `06_3DDY_claude`、`06_3DDY_smart_slicing`、`06_3DDY_orca_integration_v2` 工作区仅保留为历史；不能从旧导航自动启动任务或修改那些目录。新成员从确认后的共同基线建立自己的 checkout，具体准备状态见 ADR-007 实施记录。

Codex 项目名、任务记录的 cwd、registry 提示与实际 Git worktree 可能不一致。开工时核对路径、分支和 HEAD；相同路径的两个项目入口共享文件，不是两个隔离仓库。`Docs/` 与 `docs/` 的 Git 路径大小写混用要作为路径迁移问题处理，不能删除其中一个名字来解决。

## 按任务定位

| 任务 | 先读 | 再沿调用链阅读 |
|---|---|---|
| 生成界面/结果交互 | [ModelGeneration FeatureHost](../src/slic3r/GUI/AI/ModelGeneration/ModelGenerationFeatureHost.hpp) | `ModelGenerationPanel.cpp`、`ModelGenerationPresentation*`、`ModelGenerationArtifactFlow.cpp`、`AIModelGenerationClient.cpp` |
| Provider/预处理/质量 | [Gateway](../tools/ai/model_provider_gateway.py) | `tripo_client.py`、`openai_preprocessor.py`、Sidecar 中对应 job/下载/质量函数 |
| 导入与颜色保真 | [导入契约](../src/slic3r/AI/Contracts/IModelArtifactConsumer.hpp)、[ColorIntent](../src/slic3r/AI/Contracts/ColorIntent.hpp) | [OrcaWorkspaceAdapter](../src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp)、OBJ/Model/ObjColorUtils；查消费者是否实际使用元数据 |
| 智能切片 | [SmartSlicingCoordinator](../src/slic3r/AI/SmartSlicing/Application/SmartSlicingCoordinator.cpp) | `AI/SmartSlicing/Domain`、`Ports`、`GUI/AI/SmartSlicing`、`GUI/AI/Orca` |
| 原生尺寸/底座与 AI 连续流程 | [原生 UX 审查、修改前后与回归表](audits/2026-09-08-native-ai-workflow-ux.md) | `GUI/AI/Orca/OrcaModelPreparation*`；几何计算先于原生快照提交，既有涂色和原件保留；实际 GUI 验收与离线引擎测试分开记录 |
| 桌面组合/运行时 | [AIDesktopFeatureHost](../src/slic3r/GUI/AI/AIDesktopFeatureHost.cpp)、集成锁的 ownership | MainFrame/Plater、AIServiceManager/AISidecarClient、network_policy；共享入口需遵守既有集成所有权 |
| 原生切片/配方 | [Print](../src/libslic3r/Print.cpp)、[ColorDecomposeRecipe](../src/libslic3r/ColorDecomposeRecipe.hpp) | PrintConfig、GCode、ToolOrdering、Format；先与锁定基线对照，区分继承与本地增量 |
| 已有模型质量复评 | [model-generation-evaluation Skill](../.agents/skills/model-generation-evaluation/SKILL.md) | 按 SOP 读取已有模型、报告和对应测试 |
| 工程任务记录/恢复 | [Symphony Skill](../.agents/skills/symphony/SKILL.md)、[当前能力](coordination/symphony/README.md) | 有关任务的 state/合同/结果；计划中的 helper 不等于可执行命令 |
| 模型生成团队协作 | [团队入口](coordination/model-generation/README.md) | 以协调者工作树 registry 为准；只在团队任务中加载 |
| 打包/发布 | [release runbook](../release/README.md)、根 AGENTS 发布条款 | 当前操作涉及的准确产物、授权记录、检查脚本和 CI；导航不是授权来源 |
| 网站/翻译 | [website 规则](../website/AGENTS.md)、[localization 规则](../localization/AGENTS.md) | 仅加载涉及的子项目资料 |

## 选择验证范围

命令从仓库根执行，使用已有可用 Python 和构建环境。按实际改动选择，不要求每个文档任务运行全量构建。

| 改动 | 验证入口 |
|---|---|
| AI 架构/契约/运行版本 | `python scripts/verify_ai_integration.py --json`，保留 Git 检查；缺历史对象应报告，不能把 skip-git 的结果称为完整验证 |
| 单个 Python 模块 | `python -m unittest discover -s tools/ai -p 'test_<对应模块>.py' -q` |
| AI 集成 Python 回归 | CI 的 `python -m unittest discover -s tools/ai -p 'test_*.py' -q`；真实生成脚本不属于该命令的替代品 |
| C++ AI DTO/面板/智能切片 | `slic3rutils_tests`；按 [tests/AGENTS.md](../tests/AGENTS.md) 配置、构建和运行，Windows 需 `-C Release` |
| 网格/格式/颜色数据 | `libslic3r_tests` 中对应测试；产生切片/G-code 的行为用 `fff_print_tests` |
| GUI/导入/组合流程 | 使用确定的可执行文件、datadir、端口和模型 SHA；核实导入无隐式切片/配置改变，以及 AI 关闭/离线时普通 Orca 流程 |
| 团队内部包与实际验收 | `release/build_internal.ps1 -SourceManifest <handoff/manifest.json>`；完整主程序、EXE/ZIP、3MF/profile 和真实主窗口证据。无需先推送或提交；当前不做正式发布验收。 |

## 维护导航

新架构决策继续放已有 architecture/ADR 体系；通用知识不写成 Skill。只有反复发生且可明确输入、步骤、输出的一类任务才新增 SOP。历史 phase/plans/proposal 按需读取，不当作当前完成状态。

已有 Git 路径混用 `Docs/` 与 `docs/`；新链接按目标的 Git 拼写定位。跨平台归一化应另立可验证改动，不顺手批量重命名。不要将用户照片、生成大文件、包或供应商配置复制进导航或 Skill。
