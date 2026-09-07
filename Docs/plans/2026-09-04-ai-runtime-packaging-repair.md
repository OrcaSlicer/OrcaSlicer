# Windows AI Runtime Packaging Repair Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** 修复本地 AI 服务缺失，交付包含真实 sidecar 的可运行 Windows 目录，并验证自动启动。

**Architecture:** 保持 `ORCA_AI_WINDOWS_INSTALLER` 默认关闭，使用已有 CMake install 规则组装独立开发验收目录。补齐显式运行文件清单，并用该清单执行隔离导入回归；不往源码 resources 联接写临时脚本，不复制供应商凭据，不改智能切片、会话认证或生成行为。

**Tech Stack:** CMake、MSVC Release、捆绑 CPython 3.12.13 / Pillow 12.2.0、unittest、原生 Windows GUI。

**Status (2026-09-04):** 三项运行包修复任务已完成，正常双击完整开发目录后，C++ 自动拉起捆绑 Python 的生产 sidecar，认证健康检查成功。真实 GUI 另发现二次限色与颜色状态回传问题；不将“运行包修复完成”混同于整版功能验收通过，详见 Windows Release 验收报告。

---

## 已确认的设计

用户已批准修复。保留当前分支和可复用构建树，不创建另一套完整依赖环境。

- 选择现有 CMake 安装目录组装：与正式包共用运行清单，便于重复验证。
- 不采用向 `build/src/Release/resources` 手抄脚本：它是源码 resources 的联接，会污染源码资源树。
- 不把普通构建默认改成 AI 包：保持原有禁用选项行为和跨平台边界。
- 本轮产物为本机开发验收目录，不发布安装器、不合并或推送。

### Task 1: 为运行清单补失败回归

**Files:** 创建 `tools/ai/test_packaged_sidecar.py`，读取 `CMakeLists.txt` 中 `ORCA_AI_SIDECAR_RUNTIME_FILES`。

1. 使用 AST 检查清单内每个模块的本地依赖也在清单内。
2. 仅复制清单文件到临时目录，在 `python -I -B` 下导入生产 sidecar，禁止网络连接并清除供应商环境变量。
3. 执行 `python -m unittest tools.ai.test_packaged_sidecar -v`，确认因缺少 `color_intent.py` 失败。

### Task 2: 修复清单并组装可运行目录

**Files:** 修改 `CMakeLists.txt`；生成目录为 `build/model-generation-v2-app/`。

1. 显式添加 `color_intent.py`，不复制测试文件或内部凭据文件。
2. 重跑 Task 1 测试与既有 bootstrap / runtime / integration guardrails 回归。
3. 本机重新配置 `ORCA_AI_WINDOWS_INSTALLER=ON`、internal 开发修订号；复用固定哈希 Pillow。
4. 执行完整增量 Release ALL_BUILD，再执行 `cmake --install build --config Release --prefix <独立开发验收目录>`。
5. 验证安装目录 Python/Pillow、sidecar 全量导入、构建身份和无内嵌凭据；记录真实命令和结果。

### Task 3: 真实启动与交接

**Files:** 更新 `Docs/architecture/2026-09-04-model-generation-v2-windows-release-acceptance.md` 及根目录跟踪文件。

1. 用实际安装目录 bootstrap + 捆绑 Python 执行隔离真实服务启动、认证健康检查和关闭；不调用生成端点。
2. 通过受支持的桌面工具打开开发验收程序，验证 GUI 自动启动服务及模型生成入口可用；任何工具安全拒绝不绕过。
3. 保留证据和明确剩余 GUI 项，提供正确的可运行 EXE 路径。
4. 精确检查 diff；本轮暂不提交，验收后再决定交付 SHA。
