# GLB / OBJ 生成路径适配（2026-09-10）

代码适配已完成，并在已有运行实例中实测了 GLB / OBJ 预览、局部改色、撤销、颜色匹配取消和损坏文件恢复。实测发现的修正及版本差异见文末追加记录；主窗口全流程验收仍未完成。自动审批拦截了复制并启动隔离测试版 Orca 的操作，仅返回 `blocked by policy`，未给出细分原因。新二进制的界面复测受此限制，不能报告全流程通过。

## 已实现行为

- 新生成请求使用标准几何和标准纹理。高质量档目标为 100 万面，高性能档为 30 万面；保留 PBR 和 UV。旧 200 万面请求仍可读取，适配器将新提交的几何请求限制在标准档范围内。
- 新任务直接下载生成结果中的 GLB，不再创建远端格式转换任务。旧任务已经提交的 OBJ 转换仍按原任务编号恢复；已完整下载的 GLB 可在恢复时直接复用。
- `provider-model.glb` 保留原始下载字节。交付的 `model.glb` 只增加场景根变换，统一到最长边 100 mm、底部落地；嵌入几何缓冲、贴图和材质数据保留。
- AI 预览、历史模型、局部改色、修整和准备页导入接受 OBJ / GLB。扩展名由实际文件决定，历史 OBJ 不需要迁移。
- GLB 在编辑边界转换为 Z 轴向上、毫米单位和 sRGB 顶点色，合并基础色贴图、材质基础色和 `COLOR_0`。原始 GLB 保留；改色或修整结果另存为带顶点色的新 GLB。编辑副本不保留原始贴图采样方式和 PBR 材质效果。
- GLB 导入准备页时使用本地派生 OBJ，沿用原生颜色匹配、耗材分配及撤销事务。派生文件放在 `.orca-import/<源文件SHA256>/` 中，与原模型及普通历史列表分开。
- 结构检查与外观检查使用本地分析 OBJ，公开下载仍返回 GLB，MIME 为 `model/gltf-binary`。没有为获得 OBJ 而增加付费转换，也没有调用真实生成服务。

## 代码位置

| 范围 | 入口 |
| --- | --- |
| GLB 解析、保留贴图的尺寸归一化、分析 OBJ | `tools/ai/glb_artifact.py` |
| 下载、历史任务恢复、检查接口 | `tools/ai/orca_ai_sidecar.py` |
| 标准参数和兼容旧请求 | `tools/ai/tripo_client.py`、`tools/ai/model_provider_gateway.py` |
| 桌面统一读取与编辑文件写出 | `src/slic3r/GUI/AI/Model/ModelArtifact.*` |
| 预览、历史、修整与改色 | `ModelPreview3D.hpp`、`ModelGenerationArtifactFlow.cpp`、`ModelGenerationFinishingView.cpp`、`ModelGenerationPanel.cpp` |
| 准备页交接 | `src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp` |

## 验证证据

| 检查 | 结果 |
| --- | --- |
| Windows Release：`OrcaSlicer_app_gui`、`slic3rutils_tests` | 编译通过 |
| C++：ModelArtifact、ModelFinishing、VertexColorRegion、ModelPreviewPalette、OrcaModelPreparation、ModelGenerationPresentation | 64 个测试，5565 个断言通过 |
| Python：生成参数、GLB 文件、供应商网关、HTTP 契约、打包依赖 | 103 个测试通过 |
| 旧任务生成、恢复、OBJ 下载和重贴图 | 22 个测试通过，运行时阻断 socket 外连 |
| Sidecar readiness | 6 个测试通过 |
| `python scripts/verify_ai_integration.py --json` | `ok: true`，无 findings |
| `git diff --check` | 通过 |

测试使用自建的嵌入 PNG、交错访问器、归一化顶点色、节点实例、镜像及 UV 变换样本。覆盖原始字节保留、坐标和颜色对应、编辑保存、取消、源文件保持、无新增转换任务、GLB 下载 MIME、复查以及旧任务恢复。打包测试在隔离 Python 中导入所有发布模块并禁止外连。

本次修正了 OBJ 读取复用颜色容器时累积旧颜色、GLB 与 OBJ 负体积朝向处理不一致等问题。一次编译因另一个本地编译进程同时占用 `ModelGenerationPanel.obj` 失败；该编译结束后重建成功。编译器已有的 `LNK4098` 运行库警告仍存在。

源码基础 HEAD：`2f593724908aaf2303de49cb0aa5c9cb44927ea9`，分支 `codex/team/model-generation`。本次为包含原有修改的未提交工作树，HEAD 本身不代表这些改动。

- 源码文件清单 SHA256：`4d1410e5179795303aa3679983207ca6b61c9cd6b0d5e64a27ec17b764b7c103`
- `build/src/Release/OrcaSlicer.dll` SHA256：`b5cd948c467b3d10874b0314d214aec0caed2fe11ad88878fb3e5fc0c02c5bb0`
- 详细文件哈希：`.tmp/glb-verification-manifest.json`
- 构建、测试与架构检查日志：`.tmp/glb-build-final.log`、`.tmp/glb-cpp-regression.log`、`.tmp/glb-python-final.log`、`.tmp/glb-legacy-tests.log`、`.tmp/glb-integration-final.json`

## 尚未建立的证据

- 新二进制的主窗口完整旅程仍待复测；已有运行实例中的部分旅程和发现见下方追加记录。
- 未调用真实 Tripo，也未使用真实 Tripo GLB 样本验证颜色保真、耗时和最终打印质量。
- 当前范围是静态、嵌入资源的 GLB 2.0 三角网格及 `TEXCOORD_0` 基础色贴图。稀疏访问器、蒙皮、变形目标、Draco 等未支持的必需扩展会明确报错；不自动创建付费转换作为替代。
- 桌面预览与编辑采样基础色到顶点；纹理内部细节、法线、金属度和粗糙度效果不属于这个编辑副本的保真承诺。复杂多材质资产仍需补充实际样本验收。
- 尚未编译 macOS / Linux，未运行实际切片或打印。以上源码、哈希和测试数字属于首次适配时的工作树快照；后续验证及打包以追加记录和包清单为准。

## 追加实测与修正（2026-09-10）

用户要求实际验证、移除精细几何与 8K 纹理，并在没有问题后推送和打包。当前产品新生成请求明确使用 `geometry_quality=standard`、`texture_quality=standard`；重新贴图的默认参数也已改为 `standard`。高质量 / 高性能分别为 100 万 / 30 万目标面数。参数测试通过 mock 检查实际请求体，未调用付费供应商；显式高级重贴图 API 的历史参数兼容性保留。

已有运行实例的 DLL SHA256 为 `b5cd948c467b3d10874b0314d214aec0caed2fe11ad88878fb3e5fc0c02c5bb0`。它包含首次 GLB 适配，但不包含本节修正。使用仓库自建 GLB / OBJ 四面体和已有本地 OBJ 在实际 Orca 主窗口执行：

- GLB 历史加载显示 4 面、4 原始色，尺寸 60 × 40 × 100 mm；相应 OBJ 对照仍可读取。
- GLB 局部选面改色保存为独立 GLB，4 面和所有坐标保持；三个选中顶点变色，未选顶点保持。原 GLB SHA256 未变化。整体美颜的四面体样本因锐角保护没有几何变动；已有大型 OBJ 的美颜另存成功。
- 返回上个版本恢复原模型；发现颜色建议仍残留编辑版本的颜色，已修正撤销 / 重做及源模型上下文对颜色试配状态的保存恢复。
- 发现 GLB 历史缩略图占位错误标成 OBJ、切换历史后残留上一模型处理提示，均已修正。
- 原 GLB 进入原生颜色匹配，4 目标色映射到 3 个已有物理耗材，新增耗材为 0。取消后另存工程，与原 3MF 逐条目哈希比对：模型、项目配置、耗材及其余非图片条目完全相同，仅 4 张预览图改变。
- 损坏 GLB 给出文件无效错误，并保留当前模型与预览；发现统计文字停留在“正在解析模型”，已修正失败回调恢复原统计。随后 OBJ 对照加载成功，4 面、4 色、相同尺寸，本机读取 0.03 秒。
- 确认彩色导入时，封闭四面体被颜色分割产生的内部 T 接点破坏，触发自动网格修复失败。取消修复后原工程保留；此失败不得登记为导入通过。已修正 `TextureToColor.cpp` 三色三角面的分割，同时分割中心切线相邻的角三角形并保持颜色区域。真实公开入口 `face_colors_to_painting` → `apply_painted_mesh_to_volume` 的回归覆盖一至四色、三种面循环顺序，验证闭合、体积、无退化、颜色区域及三耗材映射。`[ModelVertexColors]` 的 10 个测试、2483 个断言通过。

界面与文件证据在本机 `outputs/glb-obj-verification-20260910/`，包含截图、可访问性文本、`recolor-evidence.json`、`cancel-archive-comparison.json` 和独立的取消工程。这些包含本地模型资料的证据不进入公开安装包。

本轮 Python 回归 104 个测试通过。修正后的应用及两个 C++ 测试目标编译通过；第一次新增测试编译因浮点容差类型重载歧义失败，统一为 float 后通过，两个尝试日志均保留。模型生成、修整、颜色预览、准备交接、同模型更新和耗材卡共 71 个 C++ 测试、5601 个断言通过；原生顶点色测试另有 10 个、2483 个断言通过。架构检查 `ok: true`、`errors: []`。新二进制启动仍被自动审批拒绝，必须保留这一界面验收缺口。未以其他启动入口绕过，也未访问真实 Tripo。

本次继续使用 `codex/team/model-generation`。提交准备时已 fetch origin，自己的远程仍为 `2f593724908aaf2303de49cb0aa5c9cb44927ea9`，集成基线仍为 `7b6b270349a48d5b85a3c74a934f89c84da1c8ad`，两者均已在本地历史中。共享改动涉及 CMake、Assimp 导入、颜色网格分割、Plater 与原生颜色匹配，需相关 owner 与非作者复核。飞书未配置，此任务和既有 Draft PR 作为同步工作项；不会把开发包登记为集成验收或正式发布。包旁清单记录最终源码 SHA、构建 ID、文件哈希和验证限制。
