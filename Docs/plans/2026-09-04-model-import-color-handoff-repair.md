# Model Import Color Handoff Repair Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** 修复模型生成导入时二次减色和颜色匹配状态错报，并保持普通导入、3MF/profile 和手动切片边界不变。

**Architecture:** 恢复 `Model::read_from_file` 对显式 OBJ 颜色回调的调用；未提供回调时仍使用现有纹理导入。模型生成手动匹配默认保留已确认的离散输入颜色，使用现有 OBJ 上色函数返回真实结果；取消不导入，单色不弹配色窗口。只在适配边界处理颜色，不扩展智能切片。

**Tech Stack:** C++17、wxWidgets、Catch2、MSVC/CMake Release、捆绑 Python 架构守卫。

## 方案与边界

- 采用显式回调路径：已有 API 足以承载专用颜色策略，改动小，保留普通纹理导入默认行为。
- 不选择在通用 TextureImportDialog 中加入模型生成业务：会把颜色意图、手动/自动模式和状态协议扩散到通用窗口。
- 不选择只固定默认色数或只改状态文案：仍会重算 RGB，且不能处理真实失败/取消。
- 用户已批准修复上一轮两个问题，沿用 `codex/model-generation-v2` 和当前工作树；保留既有补包改动。暂不提交、推送或合并；不调用付费 API。

### Task 1: 先补真实导入与保色红测

**Files:** `tests/libslic3r/test_model_vertex_colors.cpp`、`tests/libslic3r/CMakeLists.txt`（若需新增独立测试文件）、`tools/ai/test_integration_guardrails.py`。

1. 使用 ScopedTemporaryFile 写小型 OBJ，覆盖实际 `Model::read_from_file` 对显式回调的调用、1～6 色、单色空回调和未指定回调时保留纹理路径。
2. 对保留离散输入颜色补回归：相近 RGB 也不能自动合并，标签完整；普通模式行为不变。
3. 增加取消不导入与面色 OBJ 分支覆盖；记录修复前失败输出。
4. 使用现有主构建树启用原生测试，构建 `libslic3r_tests`，按相关标签运行红测。不以只测试映射函数代替实际解析入口。

### Task 2: 修复专用导入衔接

**Files:** `src/libslic3r/Model.cpp`、`src/libslic3r/Format/OBJ.hpp`、`src/libslic3r/ObjColorUtils.cpp/.hpp`、`src/slic3r/GUI/ObjColorDialog.cpp`、`src/slic3r/GUI/Plater.cpp`、`src/slic3r/GUI/AI/Orca/OrcaWorkspaceAdapter.cpp`。

1. 显式回调优先处理顶点色/面色 OBJ；保留默认无回调纹理路径和 UV 纹理行为。
2. 删除 Plater 普通导入中实际已失效的默认旧回调注入，确保普通导入仍进入 TextureImportDialog。
3. 增加默认关闭的保留输入颜色选项；仅模型生成手动匹配开启，直接分组离散颜色，用户主动减少颜色时才允许重新聚类。
4. 显式处理取消；依据真实上色函数结果填写 applied/count，不再以两色哨兵代替实际源色数。
5. 重跑定向原生与架构守卫；检查单色、自动映射、手动确认和普通导入四条路径。

### Task 3: 完整构建与真实 GUI 复验

**Files:** `Docs/architecture/2026-09-04-model-generation-v2-windows-release-acceptance.md` 及根目录跟踪文件。

1. `cmake --build build --config Release --target ALL_BUILD -- /m:2 /verbosity:minimal`，通过现有 CMake install 组装完整目录，不手抄 DLL 或运行文件。
2. 使用本地样例验证模型生成四色、六色导入默认不再减色，真实匹配状态成功；取消不增加对象，单色不弹配色窗口，导入无自动切片。
3. 普通文件导入仍使用现有原生纹理窗口。现有用户未保存工程不强行关闭或覆盖；需要更新运行目录时先保留验收场景或使用新的完整目录。
4. 记录准确测试数量、GUI 证据、产物路径与未验项；执行 `git diff --check` 后交接。

## 状态

- Task 1: complete（真实入口红测复现；绿色回归 854 断言 / 8 用例全部通过）
- Task 2: complete（634 项 Python + 45 项最终边界复检通过）
- Task 3: complete（完整 Release / install 返回 0；真实 GUI 四色、六色、单色、自动映射、取消和普通导入通过；测试工程另存，证据与未验矩阵写入验收报告）
