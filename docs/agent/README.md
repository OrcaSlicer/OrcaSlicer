# OrcaSlicer Agent 开发加速包

本目录提供面向 AI Agent 的实战资料。总架构请先阅读仓库根目录的 [`ARCHITECTURE_FOR_AGENTS.md`](../../ARCHITECTURE_FOR_AGENTS.md)，然后根据任务选择本文档集中的一到两份材料，不需要每次全部通读。

## 文档选择

| 当前任务 | 优先阅读 |
| --- | --- |
| 不知道代码在哪 | [`TASK_ROUTING.md`](TASK_ROUTING.md) |
| 要新增参数或完整功能 | [`FEATURE_PLAYBOOKS.md`](FEATURE_PLAYBOOKS.md) |
| 开发多色共挤与 C 轴控制 | [`C_AXIS_COEXTRUSION_DEVELOPMENT_PLAN.md`](C_AXIS_COEXTRUSION_DEVELOPMENT_PLAN.md) |
| 继续当前多色共挤实现 | [`C_AXIS_COEXTRUSION_IMPLEMENTATION_STATUS.md`](C_AXIS_COEXTRUSION_IMPLEMENTATION_STATUS.md) |
| 配置或验证多色共挤输出 | [`COEXTRUSION_CONFIGURATION_AND_VERIFICATION.md`](COEXTRUSION_CONFIGURATION_AND_VERIFICATION.md) |
| 涉及缓存、线程、3MF、多盘 | [`INVARIANTS_AND_RISKS.md`](INVARIANTS_AND_RISKS.md) |
| 需要快速检索调用链 | [`SEARCH_RECIPES.md`](SEARCH_RECIPES.md) |
| 工作需要交给下一个 Agent | [`HANDOFF_TEMPLATE.md`](HANDOFF_TEMPLATE.md) |

## 推荐的最短开发流程

```text
读取 AGENTS.md
  -> 在 TASK_ROUTING 中定位状态所有者和入口
  -> 用 SEARCH_RECIPES 验证实际调用链
  -> 按 FEATURE_PLAYBOOKS 做最小闭环修改
  -> 用 INVARIANTS_AND_RISKS 检查隐性联动
  -> 用 HANDOFF_TEMPLATE 留下可继续开发的上下文
```

## 开始修改前必须能回答的五个问题

1. 用户看到的入口事件或命令在哪里？
2. 数据的唯一所有者是 `GUI_App`、`Plater`、`Model`、`PresetBundle`、`PartPlate` 还是 `Print`？
3. 代码运行在 UI 线程、后台切片线程、TBB 任务还是网络线程？
4. 修改是否需要持久化到 AppConfig、profile、`.3mf` 或 G-code？
5. 修改会让哪个 `PrintStep`/`PrintObjectStep` 失效？

如果其中任何一项不明确，先继续搜索代码，不要先创建新的平行状态或辅助体系。

## 文档维护规则

- 只记录跨任务稳定的架构事实，不记录某次临时实现细节。
- 文件和符号比行号更可靠，因此正文尽量不绑定具体行号。
- 新增重要模块、状态所有者或兼容边界时同步更新本目录。
- 发现文档与代码不一致时，以代码为准并修正文档。
