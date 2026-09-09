# AI 用户旅程交互修复记录

日期：2026-09-08。基线：`codex/continue`，`ad4f74b97ae89d3cd9371d0c3214cd31f3b6269f` 加本次工作树修改。依据：[用户旅程审查](2026-09-08-ai-user-journey-review.md)。本轮修改交互与工作流恢复；没有运行付费生成、发布或推送。

## 修改与审查对应

| 审查问题 | 本轮实现 | 验证边界 |
|---|---|---|
| 1. 取消导入仍显示 98% | 已完成且空闲的结果恢复“模型已生成 · 待导入”，隐藏进行中的进度条和百分比；开始下一项工作时恢复进度显示；导入取消／失败说明不再被历史加载文案覆盖。 | 待真实导入取消／失败走查。 |
| 2. 空板预检误报导入和颜色已完成 | 智能切片停止驱动旧导入步骤；空板和缺配置显示需处理；尚无有效报告时不显示检查通过。提供添加模型、修正后重新检查及中文处理提示。 | ViewModel 空板行为测试通过；添加模型及原生配置场景待 GUI 验证。 |
| 3. 入口隐蔽且与旧助手混淆 | 准备页左侧新增“智能切片：检查与优化…”入口；视图菜单采用“智能切片”“高级参数助手”。旧状态栏明确为“最近模型导入”，由实际导入流程更新。 | 入口布局及切换保留状态待 GUI 验证。 |
| 4. 看不清方案究竟改变什么 | 候选展示参数作用范围、旧值→新值；裙边方案解释附着收益；摆放方案说明涉及实例数量；原有试切指标及差值保留。主动作明确为“应用并切片”；原始说明／失败诊断可展开。 | 参数映射行为测试通过。摆放目前提供实例数量与说明，未新增几何叠加对比。 |
| 5. 成功后没有专用撤销 | 成功与失败后均可撤销本次应用；检查应用后工程版本和原生快照位置，拒绝越过后续编辑或活动编辑工具；先从预览返回准备视图，再调用原生撤销。拒绝时给出查看原生撤销历史／重新检查的路径。 | Gateway 和状态映射行为测试通过；原生快照、预览返回路径待真实界面验证。 |
| 6. 展开检查区改变整个窗口尺寸 | 生成页、历史详情、智能切片诊断折叠区采用 `wxCP_NO_TLW_RESIZE`；智能切片内容可纵向滚动，候选动作竖排；长文本换行。 | 待窗口尺寸、DPI 和键盘操作验收。 |
| 7. 图片缩放与模型视角含义不清 | 改为“图片适应”“完整显示模型”；辅助说明区分图片按钮和模型滚轮；窄窗口下模型／图片改为上下排列。 | 编译验证后仍需实际相机和布局确认。 |
| 8. 风险提示附近缺少返工操作 | 结果判断卡直接提供“修改描述，重新设计”“检查模型／局部改色”；返工保留历史模型、输入图和配色，由用户再次启动生成。 | 沿用既有保留历史的重置逻辑；未发起生成请求。 |
| 9. 历史列表缺少视觉识别、任务编号过于突出 | 增加关联设计图／原图缩略图；缺失或损坏时显示占位；任务编号收进可展开详情并保留复制功能。 | 缩略图是关联图片，不是重新渲染的 3D 视图，工具提示已说明。 |
| 10. 多视图太小且说明仍停留在生成前 | 增加“展开图片／返回模型对照”，多视图使用完整图片区域，可继续缩放；已生成状态的说明改为对照模型和返工。模型新生成／历史加载／进入改色时会恢复模型对照，避免图片展开状态遮住新结果。 | 没有新增独立弹窗；待真实多视图图片走查。 |

## 模块和兼容边界

- GUI 展示留在 `GUI/AI`；工作流恢复留在 Coordinator 和 Orca gateway。未修改 `libslic3r`、模型格式、打印／耗材预设格式或 provider 策略。
- 历史列表提取到 `ModelGenerationLibraryView.cpp`，由现有面板成员函数管理相同数据与事件。没有新增服务或依赖。
- 主准备页入口由 FeatureHost 插入已有侧栏；`Plater.cpp` 只修改已有标题，保持核心改动预算。
- 导入仍不自动切片；方案应用按钮明确触发正式切片；取消和返工仍保留完成的模型历史。

## 验证记录

- 独立 C++17 行为测试：同一 MSVC 19.44、仓库自带 Catch2 3.11，编译实际 Coordinator、Inspector、ViewModel、候选比较器、参数校验器和两份测试文件。**23 个用例、147 个断言通过**，随机顺序。包含空板、旧／新参数值、成功和失败撤销资格，以及后续编辑保护。既有 CandidateComparison 的整数到 double 转换警告保留。
- MSVC 定向 GUI 语法检查：通过，退出码 0。覆盖生成面板、历史列表、模型产物加载、智能切片面板和 FeatureHost；后续修改的文件也已重新检查。
- 当前仓库 Release 完整构建及 `slic3rutils_tests`：**通过，退出码 0**，2026-09-08 22:25:56 完成。已生成当前源码的 `libslic3r.lib`、`libslic3r_gui.lib`、`OrcaSlicer.dll`、`orca-slicer.exe` 和测试程序。保留链接器 `LNK4098` 运行库冲突警告；没有把警告关闭或计为已修复。首次未完成的构建不作为通过证据，结论依据恢复构建的最终退出码。
- 完整测试程序执行 `[SmartSlicing],[ModelGenerationPresentation]`：**64 个用例、581 个断言通过**，随机顺序，含原生 Orca 试切及新增撤销保护测试。第一次从仓库根运行有喷嘴资源查找提示；改在 `resources` 目录运行后再次全部通过，控制台无该提示。测试不发起供应商请求。
- 本地预览目录 `build/ux-preview`：CMake 安装通过，退出码 0；包含当前 EXE、DLL、资源与 AI sidecar。自带 Python **3.12.13**、Pillow **12.2.0** 及原生 PNG 编解码检查通过。仅准备本地目录，没有制作或发布安装包、ZIP。
- 本地安装内容检查：复用 `release/verify_package_contents.py` 的 `Inspection` 规则，扫描 **17,331** 个成员、**616,554,306** 字节，`findings=[]`、`gaps=[]`，未检出规则范围内的凭据内容。记录为 `preview-content-inspection.json`。这是本地目录的启发式检查，不是安装包发布验证，也不证明不存在所有形式的秘密。
- 本地预览目录的 23 种语言资源：使用仓库 `tools/msgfmt.exe --check-format` 编译现有 PO 文件通过，包括 `zh_CN` 和 `zh_TW`；输出位于 `build/ux-preview/resources/i18n`。
- `git diff --check`：通过。
- `python scripts/verify_ai_integration.py --json`：**未通过，保留 4 项修改前已存在的问题**；本轮未新增预算／架构错误。具体为 `release/test_verify_package_contents.py:39`、`tools/ai/test_diagnostic_failure_flow.py:106` 的 credential literal 检查，以及缺失 `codex/model-generation`、`codex/smart-slicing` 来源 ref。没有修改检查阈值、忽略项或重试被禁止的分支比较。

本机已核实的 CMake、MSVC、依赖路径及构建命令见[实施计划](../plans/2026-09-08-ai-journey-interaction-fixes.md)。构建日志位于当前仓库 `.tmp/ux-journey-20260908/`，其中 `domain-tests.log` 为独立行为测试，`gui-syntax-final.log`、`gui-syntax-followup.log`、`gui-syntax-model-final.log`、`gui-syntax-native-final.log` 为 GUI 定向检查，`build-fixes-resume.log` 和 `build-fixes-result.json` 为完整构建证据，`integration-cpp-tests-resources.log` 为 64 个用例结果，`integration-fixes-final.json` 保留全部集成发现，`preview-install.log`、`preview-runtime.json` 为本地运行时准备证据。

本地预览入口：`D:/Workspace/11_3DDY_Continue/build/ux-preview/orca-slicer.exe`。需保留同目录 DLL、`resources`、`python` 等依赖。当前版本来自本报告开头所列基线加未提交修改；生成的 build info 中 commit 是基线 SHA，不能单凭该 SHA 复现本轮修改。

产物 SHA-256：`orca-slicer.exe` 为 `90e0e72095de807391efa2fcfe098008f06bc2281c6851f3be38e14d58b30519`；`OrcaSlicer.dll` 为 `dfbe5ec0848e248274382565d9d8c070591609e0ff4e242a5d559f439c666431`。

## 尚不能声称通过的项目

修改后的主窗口流程还没有实际走查。此前隔离测试实例启动被工具策略拒绝，本轮没有通过另一个入口重试这项操作。旧 PRO3 的界面观察是问题依据，不能证明新代码已通过 GUI 验收。

仍须用本轮 EXE 验证：窄窗口／高 DPI，折叠和键盘导航，结果／历史／准备页往返，取消及失败导入保留结果，空板和缺配置恢复，候选试切—应用—预览—撤销，以及用户随后编辑时的撤销保护。编译或模拟 gateway 测试不能代替真实切片、供应商质量、颜色保真或实物打印。
