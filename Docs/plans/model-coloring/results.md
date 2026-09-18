# R0–R7 结果与交付索引

计划执行中；以下待验收项不是通过结果。完整门槛见 [execution.md](execution.md)。

最近核对：2026-09-17。R1-g 用户视觉验收未通过；固定衣料数据改善保留。R3-c2c 已将固定方飞耳鼻和刘亦菲两侧耳核自动反例清零，动物零变化。R2-h3 恢复眉毛；R4-c8f 将刘亦菲耳区暗 `FaceSkin` 自动残留清零。R4-c9f 在 c9d 基础上只新增两个有直接识别证据的眼白叶，八眼缺口 `26 -> 24`。R4-c9l 新增一次性三维耳肤桥接，将用户指出的刘亦菲左耳上缘暖肤色误发色像素从 39 降至 15，大框从 67 降至 27；固定区域、八眼和动物没有新增变化，等待用户六视角判断。八眼最终核心和轮廓门槛仍未全部达到。后续严格按“耳发 → 眼白／虹膜 → 眉毛 → R4 材料边界 → R5 光照表面”的顺序执行，见 [c9f 后收敛计划](convergence-after-r4-c9f.md)。R6 尚未产出无统计运行库。总体尚未完成。

用户负责实际视觉判断，后续只接收四模型固定六视角图并返回效果描述；Codex 维护问题、指标和回溯。R1-g 用户反馈和 11 张图片已登记在 `.tmp/model-coloring-feedback/2026-09-16-r1-g-user/USER-FEEDBACK.md`。R6 编译失败已有独立最小补丁语法复现，尚未集成重构建，见 `.tmp/semantic-runtime-offline/r6-protobuf-20260916/DIAGNOSIS.md`。

| 轮次 | 当前状态 | 起点与已知问题 | 本轮结果／证据 | 实际程序 | 最终通过 |
| --- | --- | --- | --- | --- | --- |
| R0 | 标注／原生性能已冻结；补程序实际输入对照 | 31 ROI、13 原生 4× 区域；已补方飞双眼／侧背 | [R0 清单](../../../.tmp/semantic-validation/r0-baseline/R0-验收清单.md)、[标注冻结](../../../.tmp/semantic-validation/r0-baseline/annotation-freeze.json)、[原生性能](../../../.tmp/semantic-validation/r0-performance/R0-原生性能结果.md)；143 标注／mask 哈希核对通过 | 双人原始历史资产重算、开关、人工改色、版本撤销重做、导入及另存已演示；无在线配置的历史入口缺陷保留 | 未验收 |
| R1 | g 实现／固定区域回归通过，待实际程序验收；a–f 保留 | 内衬 8,426 像素中新增误白 1,240；棕发风险 24＋8 | [实际 GLB 同源结果](../../../.tmp/semantic-validation/canonical-gui-validation/r1-g/summary.json)；清除全部 1,240 新增误白，保住 32 棕发风险点及 10 白衣阴影点；仅撤回 f 肩部 17 面的无充分依据豁免 | g 完整构建、136 项／2,649 断言及集成检查通过；独立运行目录校验通过。f 已演示双人正面／斜面及原色切换；g 尚未演示 | 未验收 |
| R2 | h3 眉毛候选通过固定审计；八眼转 R4 | 眼白核心遗漏、眼线误白、双人女性眉毛缺失 | [h3 结果](../../../.tmp/semantic-validation/canonical-gui-validation/r2-h3-brow-view/)仅恢复最佳视角眉毛；三个人像新增深色眉毛材料 290／342／270 面，八眼与 c2c 相同，动物零变化。h1／h2 的眼白放宽分别新增 212／197 个刘亦菲右眼误白像素，失败保留 | 独立 `runtime-r2-h3` 已安装，DLL `c7d69bcf…705727`；[双击入口](../../../.tmp/semantic-validation/gui-rounds/r2-h3-user/Start-R2-h3-test.cmd)。眼白受整面边界限制，必须由 R4 子面材料处理 | 未验收 |
| R3 | c2c 自动反例通过，待用户视觉验收；a、c2、c2b 失败／部分候选保留 | 发束杂色与侧背、耳鼻误红、刘亦菲耳发混色；白衣真缝及耳颈皮肤必须保留 | [c2c 四模型证据](../../../.tmp/semantic-validation/canonical-gui-validation/r3-c2c-isolated-skin-hole/)；方飞耳部 31 个误红和刘亦菲两侧耳核 16 个深发色像素均清零，c2c 相对 c2b 仅改 25／10／38 个人像面，动物零变化；旧衣料、发衣边界及发梢反例不回退 | 完整构建及独立 `runtime-r3-c2c` 已完成，DLL `d8d29ad…f95aa`；四份真实历史资产恢复检查通过，六视角待用户测试 | 未验收 |
| R4 | c9l 耳发候选已构建，待用户六视角检查；c9g／c9h／c9i／c9j／c9k 失败并保留证据 | 刘亦菲左耳上缘跨断缝，极侧脸没有 Face Landmarker 结果；c9f 后眼白核心仍缺 24 像素 | c9l 从直接耳肤叶出发，只做一次非级联三维桥接；目标耳缘 39→15，大框 67→27。31 个历史 ROI、13 个原生 4× ROI、9 个侧背区、八眼及动物无新增变化。[c9l 证据](../../../.tmp/semantic-validation/canonical-gui-validation/r4-c9l-ear-spatial/) | 独立 `runtime-r4-c9l` 已安装；[双击入口](../../../.tmp/semantic-validation/gui-rounds/r4-c9l-user/Start-R4-c9l-test.cmd)。75 个语义配色用例／610 条断言、49 个相关用例／469 条断言、完整 Release、AI 集成和四份历史任务恢复通过。眼白、眉毛强度和材料边缘锯齿仍开放 | 未验收 |
| R5 | a 候选已编译，待用户视觉检查 | 原色与语义预览逐面法线，受光时五官三角块 | [法线实现](../../../src/slic3r/GUI/AI/ModelGeneration/ModelPreviewNormals.hpp)仅跨同向双面流形边平滑，保留折角／薄片／非流形边；121 用例／2,273 断言及集成检查通过 | 独立 `runtime-r5-a` 资源检查通过；[双击入口](../../../.tmp/semantic-validation/gui-rounds/r5-a-normals/Start-R5-a-test.cmd)，用户六视角及光照开关待验 | 未验收 |
| R6 | 源码构建失败，兼容问题排查中 | 官方 DLL 有 WinInet 统计路径 | 固定提交 6d31f1ebc3284db74d211d62bdc4f0a0c29ea120；最新失败在 Protobuf JSON 的未完整类型与 MSVC variant 实例化，保留原始日志 | 未产出新 DLL；正式程序无外连未验证 | 未验收 |
| R7 | 等待前置轮次 | GUI／持久化／性能及包体待统一验证 | 最终程序、效果包、测试工程尚未冻结 | 待执行 | 未验收 |

COL-002 历史对照保留；COL-003／004 candidate-b 完整失败样本保留。当前没有“上一轮已验收版本”，对照必须明确这一点。

R4-c9f 完整源码检查点为 `.tmp/model-coloring-checkpoints/COL-005-20260917-040245-129228-r4-c9f-direct-sclera-built`；独立运行目录 `build-semantic/runtime-r4-c9f`。构建输出与运行目录 DLL 的 SHA-256 均为 `73ace577ed9c44c4a9ee60bd39a001dd5a180d2b6ccaa906316ce41a6cafc375`，四份真实历史任务只读恢复检查通过。该程序仍使用官方 MediaPipe DLL，不能作为 R6 完全无外连交付。

R4-c9l 完整源码检查点为 `.tmp/model-coloring-checkpoints/COL-005-20260917-045503-926733-r4-c9l-ear-spatial-built`；独立运行目录 `build-semantic/runtime-r4-c9l`。构建输出与运行目录 DLL 的 SHA-256 均为 `6076020f332982d0d0247d8cd3d80954b644d56f478e6013dcce7877cc6b0ed3`，四份真实历史任务只读恢复检查通过。该程序仍使用官方 MediaPipe DLL，不能作为 R6 完全无外连交付。

R5-a `OrcaSlicer.dll` SHA-256 `A7B6AB6FE90EE9F62EE546CC188609770C32EE63223C67A8659D110005C6972E`，EXE SHA-256 `DEC1058697BEFFB1861111EFF6390EA6CF9E91C1BD9B8CB66C9869489025D2D4`；DLL 与本轮编译输出相同。四份真实历史资产已复制到独立 `r5-a-normals` 测试数据目录，回环历史只用于打开旧模型，新生成禁用。官方 MediaPipe DLL 未更换，故 R6 离线无外连仍未完成。用户负责四模型六视角与效果描述；本轮程序未由用户视觉验收。关灯材料分区按本轮代码保持不变，实际程序逐像素截图对照待用户检查；导入／保存路径未修改，但本轮尚未重验该流程。

当前工作树未提交，以基线提交＋完整检查点标识；算法在 `OrcaSlicer.dll`，不可只依据相同 EXE 哈希判断新旧版本。

R2-h3 完整源码检查点为 `.tmp/model-coloring-checkpoints/COL-005-20260916-191203-419318-r2-h3-brow-view-built`，独立运行目录 `build-semantic/runtime-r2-h3`；DLL SHA-256 `c7d69bcf004dba1b63624c9986c3400ebdf6acfd5f77f61b3255c63725705727`，EXE SHA-256 `dec1058697beffb1861111eff6390ea6cf9e91c1bd9b8cb66c9869489025d2d4`。安装文件与构建输出一致，四个历史任务只读恢复检查通过。当前仍使用官方 MediaPipe DLL，不属于 R6 完全无外连交付。

R1-g 完整源码检查点为 `.tmp/model-coloring-checkpoints/COL-004-20260915-230755-803728-r1-g-before-full-build`，构建前后源码一致。独立运行目录 `build-semantic/runtime-r1-g`，DLL SHA-256 `f0e020ae5f57f707b273e4c41164a0fa54a95ebbdffae04ac9c2ed7a4312def8`；[运行目录校验](../../../.tmp/semantic-validation/installed-runtime-r1-g-status-20260916.json)。识别资源仍为原官方 DLL，不能作为完全无网络交付。原生及实际 GLB 同源四模型回归一致，动物自动覆盖为 0；眼部整框无变化，仅说明本轮没有新增变化，既有眼部失败仍待修复。

R1-f 完整源码检查点为 `.tmp/model-coloring-checkpoints/COL-004-20260915-224719-557491-r1-f-before-full-build`；独立运行目录 `build-semantic/runtime-r1-f`，DLL SHA-256 `931a2bcf330e71b002a04462d62e6ded6f98f5099fc041a85ea74cbdd6078e18`。该程序仍含原官方 MediaPipe DLL，不能作为 R6 完全无外连的交付。构建／测试／安装记录分别为 `.tmp/semantic-validation/build-r1-f-receipt.json`、`catch-r1-f.log`、`installed-runtime-r1-f.json`。

肩部组件 94117 的 17 个新增白面，在六视角内只见 10 像素：−90° 的 4 点未落入已有确定核心；原 R0 折线的整数坐标与像素中心距离约定不足以证明全部位于原 2 像素带内。−45° 的 6 点没有同视角的原肩部标注。不追加有利免责带，不把这些点记为已证实正确或已证实真发误白。g 的单一假设是：暖阴影强边界豁免需要独立检查邻近发衣交界风险，风险查询只能否决豁免，不能借用发色。143 个原标注文件保持不变。[逐点核查](../../../.tmp/semantic-validation/canonical-gui-validation/source-review/couple-94117-frozen-boundary-membership.json)。

原生性能的 96 次顺序进程测量（12 次预热＋84 次正式）已完成，冷识别／缓存映射／单槽换色分别保存 7 次值。它不包含 GUI 几何合成和界面刷新，不能代替程序性能门槛。程序实际读取 GLB；现有原生诊断读取其 OBJ 副本。双人面索引完全一致，但浮点几何／颜色不同，最初 GUI 标签与原生差 62 面。已复现实际 GLB 的几何及原色指纹，使用同一输入的原生冷识别与最初 GUI 缓存全部标签／置信度逐位一致。后续追加四模型实际输入审计，不改写旧基准或冻结标注。[来源证据](../../../.tmp/semantic-validation/r2-eye-diagnosis/canonical-load-evidence.json)。

失败起点的 [真实窗口操作记录](../../../.tmp/semantic-validation/r0-gui/replay-observation.json) 和 [自动＋人工改色 3MF](../../../.tmp/semantic-validation/r0-gui/replay/R0-couple-auto-manual-778.3mf) 保留。该工程含原有方飞／刘亦菲对照对象以及新导入的双人对象；双人 956,658 面中的人工蓝色为 778 面。它是 R0 流程证据，不是视觉通过或 R1 新程序验收。

## 本轮发现的产品缺陷

R7-HISTORY-001：R0 实际程序已从独立目录启动并载入测试工程副本，回环历史服务通过认证且 `/health` 返回 200。由于 `AIDesktopFeatureHost::apply_availability` 将“服务兼容”与“在线生成可用”合并，未配置在线生成能力时，生成页的历史恢复也被禁用。必须分离本地历史／编辑与新生成可用条件，并在正式程序断网／无凭据情况下验收。[窗口证据](../../../.tmp/semantic-validation/r0-gui/evidence/history-unavailable.png)。这属于真实产品缺陷，不是服务未启动、平台拒绝或 R6 DLL 构建失败。

本次失败起点运行目录 `build-semantic/runtime-semantic-eye-material`，启动回执在 `.tmp/semantic-eye-material-gui/runtime/logs/launch-receipt.json`。此前准备脚本在 Windows PowerShell 旧宿主遇到脚本执行策略限制；改由本任务当前配置的 PowerShell 宿主运行后 CheckOnly 和启动均成功，未修改系统策略。
