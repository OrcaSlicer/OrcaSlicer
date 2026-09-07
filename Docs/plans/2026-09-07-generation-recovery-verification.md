# 图片预览恢复与默认叠色匹配：修复记录

日期：2026-09-07。基线：`65b867ecf779c5a229efb8d6d59c5870548b1bd3`，独立工作树 `D:/Workspace/06_3DDY_generation_repair_20260907`。该快照包含主工作区原有改动；本次交付只包含快照之后的修复差异。

## 原因与行为变化

1. **预览完成后无法继续生成 3D**：后端会修正肤色、服装等配色角色，界面却只更新任务快照，导致当前输入被误判为已修改。现在仅当用户未编辑提交的颜色及角色时，同步后端校正；真实用户改动仍要求重新确认。生成按钮在等待确认时保持可见，是否可用仍受输入一致性及质量检查约束。修改推荐颜色数量后会重新请求推荐。
2. **图片与视觉复核失败混淆**：提供的日志中，图片编辑请求已返回成功，随后视觉复核出现 TLS EOF 和 HTTP 502。现在文本/视觉分析仅对连接故障或服务暂不可用重试一次，并将复核图片缩放到最长边 2048 像素、编码为 JPEG；原图和模型参考图不变。界面明确提示复核暂不可用，保留已有本地质量检查。
3. **结果下载中断**：捕获 HTTP 不完整读取，检查空响应和 Content-Length 不符，在替换目标文件前验证下载完整性。仅下载 GET 最多尝试两次。图片生成 POST 仍不自动重复，401/403、限流及服务拒绝有独立提示。
4. **默认导入绕过 nightly 完整匹配窗口**：AI 适配器原先传入简易 OBJ 配色回调，绕过原生纹理/颜色导入分支。新增默认 `NativeMatch`，复用原生窗口、叠色配方和导入事务。按用户确认的偏好，打开完整窗口后由用户选择叠色，不自动计算混色。原有物理耗材自动匹配、单色和简易匹配仍可选择。新增导入反馈包括取消状态和实际着色数量，支持超过六个物理槽位的虚拟叠色编号。

## 验证

所有网络相关测试使用模拟响应或本机测试服务，未调用付费生图服务。

| Python 测试模块（`tools/ai/`） | 通过数量 |
| --- | ---: |
| `test_openai_preprocessor.py` | 59 |
| `test_printable_sidecar_pipeline.py` | 8 |
| `test_printable_reference_visual_quality.py` | 3 |
| `test_obj_generation.py` | 125 |
| `test_diagnostic_failure_flow.py` | 2 |
| `test_simplified_design_flow.py` | 8 |
| 合计 | 205 |

使用 `python -m unittest tools.ai.<模块名>` 运行上述定向测试。原始输出保存在修复工作树的 `.tmp/verification/`，其中流水线、视觉复核输出分别为工作树根目录下的 `pipeline-test.log`、`vision-test.log`。

C++ 定向测试共 **18 个用例、1,188 次断言**通过：

- 配色展示状态：4 个用例、170 次断言，覆盖后端角色校正、重复轮询及用户编辑保护。
- AI 导入契约：5 个用例、136 次断言，包含原生匹配默认值。
- OBJ 颜色和原生导入反馈：9 个用例、882 次断言，覆盖 1～6 色原生导入路径、物理耗材与虚拟叠色编号及纯几何导入。

Windows 使用现有 MSVC、SDK 和只读依赖编译了 `ModelGenerationPanel.cpp`、`OrcaWorkspaceAdapter.cpp`、`ModelGenerationStatusText.cpp`、`Plater.cpp` 四个 GUI 翻译单元。独立构建脚本及输出位于修复工作树 `.tmp/verification/compile-gui.ps1` 和 `gui-compile-final.log`；最终面板清理另由 `compile-panel-final.ps1`、`panel-compile-final.log` 检查。C++ 测试链接复用现有依赖，存在既有 `LIBCMT` 与运行时库冲突警告，但测试程序正常执行并通过。

## 集成检查与验收范围

`python scripts/verify_ai_integration.py --json` 的架构检查通过，完整结果仍为 `ok: false`。修复前主工作区已报告 `tools/ai/test_diagnostic_failure_flow.py:106` 的 `security.secret_content`；独立快照因同时跟踪原有新增文件，还报告 `release/test_verify_package_contents.py:39`。两处均是已有测试夹具中的固定模拟凭据，本次未修改或豁免扫描规则。主工作区修复前结果保存在 `.tmp/generation-repair-20260907/baseline-integration.json`。

本次验证证明源码修复及定向测试通过，未完成整套应用链接、GUI 交互验收或测试者电脑上的回归。没有构建或发布新的 EXE/ZIP，也没有更新远端测试者安装包。测试者验收需使用包含本次修改的新构建，检查：预览完成后继续生成按钮可用；视觉复核暂不可用时提示准确；导入彩色模型默认打开完整匹配窗口，手动选用叠色后可在准备页看到结果，取消则不完成导入。后续发布仍需独立检查实际 EXE 和 ZIP，现存检查发现不得忽略。
