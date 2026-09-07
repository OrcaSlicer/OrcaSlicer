# 本项目的打印与颜色边界

这份知识说明用于评审模型产物和颜色交接。执行步骤见 [模型评估 Skill](../../.agents/skills/model-generation-evaluation/SKILL.md)，产品导航见 [AI 工程入口](../AI_ENGINEERING.md)。这里的实现事实应随契约变更核对，硬件未知项不能由术语推断。

## 物理通道、目标色和配方

| 概念 | 本项目含义 | 不可据此推断 |
|---|---|---|
| 物理耗材通道 | 真实 filament input 对应的零基 slot；当前契约支持 1–6 个 | 六个界面色块不等于六个可用硬件通道 |
| 目标色 | 模型希望呈现的颜色和区域 | RGB 显示色不是实际材料光学特性或已校准打印色 |
| 虚拟混色配方 | 从物理池中选择组件及比例，供后续工艺表达 | 当前契约每个配方最多 3 个组件，不是任意六组分混合 |
| DiscreteFilament | 离散耗材选择/分区与兼容 fallback | 不是连续色域复现 |
| ProcessMix | 过程色意图及配方表达 | 不指定喷头拓扑，也不证明 CMYK 硬件执行已实现 |

依据：[ColorIntent.hpp](../../src/slic3r/AI/Contracts/ColorIntent.hpp)、[ADR-006](../architecture/ADR-006-six-channel-model-color-intent.md)、[Orca 配方能力](../../src/libslic3r/ColorDecomposeRecipe.hpp)。通道上限、单配方组件上限和 target palette 上限是不同约束。

六通道不能自动解释为 C/M/Y/K/W/透明或其他固定组合；具体材料、通道映射、逐层/空间/同喷嘴混色方式和标定数据需要设备证据。[人像路线提案](../architecture/realistic-beautified-portrait-printing.md) 已明确这些未知项，它不是硬件规格。

## 几何、颜色和文件交接

- 图像、纹理 UV/材质、顶点颜色和打印区域是不同表示；屏幕上有彩色纹理，不保证导入后有正确可切片颜色区域。
- 当前生成交接使用 OBJ 和关联的 `color-intent.v1.json`。客户端 schema/hash 校验只证明检查覆盖的文件关联，不证明 SmartSlicing 已使用意图生成配方。
- 生成模型不等于 Orca 项目 3MF。项目 3MF 包含项目/配置语义，生成模块不应自行拥有这些状态；GLB/制造母版方案仍需区分提案与实际实现。
- 生成阶段结束于产物交付；导入、试切、正式切片/G-code 和机器执行分别需要证据。导入成功不应隐式改变预设或触发切片。

依据：[生成产物](../../src/slic3r/AI/Contracts/GeneratedModelArtifact.hpp)、[客户端校验](../../src/slic3r/GUI/AIModelGenerationClient.cpp)、[Orca 适配器](../../src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp)。

## 质量检查能证明什么

[结构分析器](../../tools/ai/printable_model_quality.py) 输入为已归一化的 Z-up OBJ；长度阈值按毫米使用。来源单位、打印尺寸或朝向不明时，先报告未知，不能把默认阈值的结果解释为制造合格。

分析器覆盖顶点/面合法性、边界和非流形边、连通分量/悬浮、接地、薄部件、[局部壁厚采样](../../tools/ai/sampled_local_thickness.py)、悬垂区域，以及顶点颜色与目标 palette 的覆盖。检查原始 status、errors、warnings、thresholds 和 available 标志；缺失测量不能按零风险处理。

边界边为零且未检出非流形边，不等于已经穷尽顶点非流形、自交或所有内部几何问题。有限壁厚采样不保证每个位置达标；悬垂阈值不能代替指定打印方向、材料和支持配置的验证。OBJ 分析也不自动验证所有 MTL/纹理文件的视觉效果。

| 证据层 | 验收问题 |
|---|---|
| 输入与参考图 | 图像是否可用；多视图是否一致；身份/造型目标是否保留 |
| 几何与颜色预检 | 本次阈值下发现了什么风险，哪些测量不可用 |
| 导入与切片 | 导入后区域/颜色/尺寸是否正确；实际配置下是否可切片；预览与 G-code 是否符合预期 |
| 真实打印 | 身份/造型、表面缺陷、色差、强度和重复性是否符合约定标准 |

前三层不能替代最后一层。当前仓库有结构/视觉/切片测试与报告，不据此宣称已有经硬件标定的全链路打印资格。
