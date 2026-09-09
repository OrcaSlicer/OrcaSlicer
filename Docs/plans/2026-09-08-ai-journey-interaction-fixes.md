# AI 用户旅程交互修复实施计划

**Goal:** 修复 2026-09-08 交互审查中的状态、导航、返工、比较与布局问题，保留主窗口流程和既有产物。

**Architecture:** 保持现有 wxWidgets FeatureHost / Panel / ViewModel / Coordinator 边界。显示文案与布局留在 GUI；撤销资格及工程变更保护在工作流和原生适配器中保证。导入不隐式切片、不改变预设。

**Tech Stack:** C++17、wxWidgets、CMake、Catch2。

依据用户“根据审查出来的交互问题，修改一版”授权直接实施。方案比较：只改文案不能解决状态和撤销问题；整体重做工作台会扩大范围；本轮选择沿现有主窗口做集中交互修复。保留当前分支与之前 Python 清理修改，不修改旧工程，不重放已拒绝的隔离实例启动或临时副本清理。

## 1. 生成结果与返工

- 修改 `src/slic3r/GUI/ModelGenerationPanel.cpp/.hpp`：取消及失败导入恢复待导入状态，不留 98% 进行中提示；返工操作保留历史与输入，说明下一次生成需用户触发。
- 在风险提示附近接入已有描述修改及局部改色操作；不直接发起付费调用。
- 折叠区禁止改变顶层窗口尺寸；明确图片缩放与模型视角操作，改善窄窗口比较、多视图查看和历史视觉识别。
- 必要时把新增展示辅助代码放入现有 `GUI/AI/ModelGeneration` 目录，遵守大面板行数预算。

## 2. 智能切片导航与状态

- 修改 `GUI/AI/SmartSlicing/SmartSlicingPanel.*`、`SmartSlicingViewModel.*`、FeatureHost 及少量 MainFrame/Plater 组合入口。
- 主准备流程提供中文智能切片入口；旧助手明确为高级工具；避免同时展示两套不一致的步骤。
- 初始显示“尚未检查”，空板或缺配置不能标为完成；提供添加模型下一步，详细诊断按需展开。
- 面板内容可滚动、长文案换行，操作区可达。

## 3. 方案解释与安全撤销

- 候选显示参数旧值／新值、摆放变化及方案说明，保留真实试切指标。
- 主动作使用“应用并切片”。
- 修改 `AI/SmartSlicing/Application/SmartSlicingCoordinator.*` 及原生 gateway（如需）：成功和失败后均支持专用撤销；工程后续变化不得撤销用户其他编辑。
- 在 `tests/slic3rutils/test_smart_slicing_coordinator.cpp`、`test_smart_slicing_workflow.cpp` 和新增的 `test_smart_slicing_apply_guard.cpp` 扩展真实状态转移、成功撤销及变更保护测试；不以字符串源码断言代替行为测试。

## 4. 验证与交付

- 使用当前可用构建环境编译受影响 C++ 与运行相关 `slic3rutils_tests`；构建能力缺失时准确报告，继续可执行的独立验证。
- 执行 `python scripts/verify_ai_integration.py --json`，保留全部发现，不改变阈值。
- 对修改后的可执行文件才能作新 GUI 验收；已有 PRO3 的行为不能作为修复通过证据。历史被拒绝的启动与清理不通过其他入口重试。
- 更新逐项完成情况、测试结果及尚未实测项；不发布、推送或调用付费生成。

## 进度

- [x] 阅读当前约束与审查报告，确认源码基线和已有修改。
- [x] 生成交互修复（待新 EXE 界面验收）。
- [x] 智能切片呈现与导航修复（待新 EXE 界面验收）。
- [x] 候选改动说明及成功撤销保护；独立行为测试通过。
- [x] 编译、行为测试、集成检查与交付记录；真实 GUI 验收仍受此前启动策略拒绝限制，详见审查修复记录。

## 本机构建路径（2026-09-08 复核）

历史依据：`Docs/plans/2026-08-26-phase81-model-provider-gateway-implementation-plan.md:281`、`Docs/history/model-generation-v1/progress.md:614`，以及旧主要工作树 `build/CMakeCache.txt` 中的生成器和依赖前缀。仅读取旧缓存定位依赖，没有复用旧源码或在旧工作树中构建。

- CMake：`C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe`
- 开发环境：`C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Auxiliary/Build/vcvars64.bat`
- 编译器：MSVC 19.44.35227.0，x64；工具目录 `VC/Tools/MSVC/14.44.35207`。
- 现有依赖前缀：`D:/Workspace/06_3DDY_claude/deps/build/OrcaSlicer_dep/usr/local`；当前构建只引用其安装产物。
- 当前构建目录：`D:/Workspace/11_3DDY_Continue/build`。
- 配置参数：`-G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<上述依赖前缀> -DBUILD_TESTS=ON -DORCA_AI_WINDOWS_INSTALLER=OFF -DSLIC3R_MSVC_COMPILE_PARALLEL=OFF -DCMAKE_BUILD_TYPE=Release`。
- 编译参数：`--build build --config Release --target OrcaSlicer_app_gui slic3rutils_tests -- /m:2 /p:CL_MPCount=1 /p:UseMultiToolTask=false /p:BuildInParallel=false /nologo /v:minimal`。

独立快速验证用同一 MSVC 和仓库自带 Catch2 3.11 编译实际 Coordinator、ViewModel、Inspector、比较器、参数校验器与两份测试文件，不链接 GUI；23 个用例、147 个断言通过。该结果不代替原生适配器及整个主窗口的验证。

完整 Release 构建于 2026-09-08 22:25:56 结束，退出码 0。完整 `slic3rutils_tests.exe` 在 `resources` 工作目录执行 `[SmartSlicing],[ModelGenerationPresentation] --order rand`，64 个用例、581 个断言通过。

构建完成后，为准备本地预览目录，重新配置 `-DORCA_AI_WINDOWS_INSTALLER=ON -DORCA_AI_INTERNAL_DEFAULTS_FILE= -DORCA_AI_PACKAGE_REVISION=ux-fixes-20260908`，再执行 `cmake --install build --config Release --prefix D:/Workspace/11_3DDY_Continue/build/ux-preview`。此开关在本次用于安装既有 AI 运行文件及固定版本 Pillow；没有运行 CPack 或发布。23 种语言的 MO 文件已预先编译到该目录。安装与自带 Python/Pillow 原生 PNG 检查通过。

交付与验证边界见 [AI 用户旅程交互修复记录](../audits/2026-09-08-ai-journey-interaction-fixes.md)。
