# 当前代码与旧 v2、Orca 主线的合并评估

评估日期：2026-09-09。用户确认 v2 指 `D:/Workspace/06_3DDY_orca_integration_v2`。仅评估，不实施合并、切换分支、提交或推送；未修改旧工作区，也未比较被阻止的智能切片来源。

## 结论

建议先整理并保存当前完整源码，再从当前代码建立 `codex/upstream-sync-20260909` 同步分支，合入固定的 Orca 官方 main 提交。保留旧 v2 作为历史参考，逐项核对它尚未吸收的有效修复；不建议把本轮全部成果先并回旧 v2，也不建议从空白上游重新搬运整套 AI 功能。

当前代码仍在 `codex/continue`，尚未合入旧 v2。当前 HEAD 为 `ad4f74b97ae89d3cd9371d0c3214cd31f3b6269f`。工作区相对 HEAD 的实际源码及文档变动为 73 个文件、6018 行增加、1573 行删除，其中 36 个新增文件。该数量不包含 `.tmp`、生成模型、graphify 输出及构建产物。仅合并 HEAD 会遗漏美颜工作台等未提交成果。

## 冻结输入与结果

| 目标 | 提交 | 共同基点 | 文本冲突 |
| --- | --- | --- | --- |
| 旧 v2 | `a4d5d89ae316de692aa6afc88eb3a65bf57ce35c` | `808efe44018e050b64c57b8cca7a82eafb00597c` | 32 个文件，97 处 |
| Orca 官方 main | `8af92214d0d5f604a541b3e5813359f294c2129a` | `b6ef6cf1be428b931d669dcc31b7f43ae320be85` | 9 个文件，12 处 |

官方 main 通过 GitHub upstream 实际查询并抓取，提交说明为 `Extract and Unify Wipe Tower Estimation (#15532)`，时间 2026-09-09 17:46:20 +0800。本次结果固定针对该提交，后续主线变化需重新评估。

旧 v2 本地 HEAD 与当前仓库的 `archive/orca-integration-20260908` 完全一致；旧目录另有 `AGENTS.md` 和 `release/README.md` 未提交改动，仅记录其存在，没有纳入目标模拟。当前 HEAD 与 v2 分别有 14 / 29 个单侧提交；其中 v2 的 11 个提交与当前 HEAD 存在等价补丁，另 18 个不等价。这不表示 18 个都缺失：当前未提交实现可能已经覆盖或替代其中的行为，仍需逐项核对。

当前 HEAD 与官方 main 分别有 52 / 130 个单侧提交。自共同基点，本地完整快照改变 440 个文件，上游改变 4742 个文件，双方重叠 23 个文件。因此文本冲突少不等于兼容性已通过。

## 冲突集中位置

旧 v2：`ModelGenerationPanel.cpp` 23 处、`orca_ai_sidecar.py` 12 处、`openai_preprocessor.py` 7 处、`ModelPreview3D.hpp` 6 处、`ModelGenerationArtifactFlow.cpp` 6 处；另涉及跨模块导入接口、版本恢复、测试和打包。这些区域与今天的不限色、美颜、后台预览、提示化流程重叠，不适合整文件取某一侧。

官方 main 的 9 个文件：

- `.gitignore`
- `AGENTS.md`
- `CMakeLists.txt`
- `src/dev-utils/BaseException.cpp`
- `src/slic3r/GUI/AboutDialog.cpp`
- `src/slic3r/GUI/GUI_App.cpp`
- `src/slic3r/GUI/Plater.cpp`
- `src/slic3r/GUI/TroubleshootDialog.cpp`
- `tests/libslic3r/test_preset_bundle_loading.cpp`

其中构建标识有上游 `BuildCommit.hpp` 与本地 `BuildInfo.hpp` 的重复实现，应统一以免长期分叉。`Plater.cpp` 两处涉及 OBJ 导入／重新加载回调和本地原生纹理配色路径，需要同时保留普通 Orca 和 AI 模型的正确行为，不能机械选边。美颜核心与生成侧文件在本次主线模拟中没有文本冲突，但仍需编译及运行验证。

## 建议执行顺序

1. 整理当前源码与文档，排除临时文件、生成素材和配置，保留可回退基线；今天新增源码必须纳入。
2. 从该完整基线建立同步分支，合入上述固定 main 提交，解决 9 个文件的冲突。
3. 核对 v2 独有修复的实际价值，按需吸收；旧的限色、强制检查等行为按最新产品要求处理。
4. 构建、定向测试和架构检查；实际验证历史加载、美颜对比／保存／撤销／重做、原色及六色预览、普通 OBJ 导入／重载、原生耗材配色、准备页交接和手动切片。同步版本通过后再作为后续开发基线。

## 方法与证据范围

使用独立临时 Git index 保存当前工作文件为 tree `9aa7ffd642f55ee384b4062ae5afcbc790a40a74`，没有创建 commit 或分支。以实际唯一共同基点调用 `git merge-tree --write-tree --name-only --merge-base=...`。该工具使用三方内容合并及重命名检测，但不修改真实工作区／index，参见 [Git 官方文档](https://git-scm.com/docs/git-merge-tree)。真实 index 前后 SHA256 相同。

原始数据和完整冲突清单：`.tmp/ux-performance-20260909/branch-merge-assessment.json`；模拟输出：`merge-v2-working-snapshot.txt`、`merge-upstream-working-snapshot.txt`。文件数由 Git 的冲突文件段统计，文本处数由合并 tree 中的冲突标记块统计。未编译或执行模拟合并结果，以上不能证明合并后的程序可运行。
