# 模型生成修复：本机运行验证

日期：2026-09-07。对应用户要求“你在本机验证一下”。

## 结论

修复后的 Windows Release 程序已实际编译、启动和操作。离线合成数据验证了生成按钮、默认完整颜色匹配、手动选择 CMYW 叠色、取消导入、确认导入、手动切片及 G-code 导出。真实供应商图片/模型生成未提交；本次结果不代表远端服务稳定性或实物打印资格。

## 环境和构建

- 工作目录：`D:/Workspace/06_3DDY_claude`。
- CMake 目标：`OrcaSlicer_app_gui COPY_DLLS`，Release，MSVC 2022；最终构建退出码 0。
- 构建后的 `build/src/Release/OrcaSlicer.dll`：82,646,016 字节，SHA256 `0f2bd4a190f6172b9c64b8a97584acd109b51d7eefcc3f966a8153ca56d1d1ae`。
- 原启动器与本次使用的 `orca-generation-validation.exe` 字节相同。单独命名是为了准确定位验证窗口，避免与另一个项目正在运行的 OrcaSlicer 混淆；两者均加载同目录的新 DLL。
- 使用 `.tmp/local-validation-20260907/data_dir` 独立配置，未使用用户账号或供应商凭据。本地 HTTP 测试服务仅返回合成状态、图片与四色 OBJ。
- 源码哈希、构建日志及二进制身份记录位于 `.tmp/local-validation-20260907/`。本轮没有修改修复源码。

## 实际界面结果

| 检查 | 结果 |
| --- | --- |
| 服务端纠正颜色角色后，图片确认页面的“生成 3D” | 按钮显示且可点击；实际打开生成确认框，随后选择取消。没有提交生成请求。 |
| 视觉复核不可用时的提示 | 页面显示复核不可用提示，与图片预览已生成的状态区分。 |
| 模型导入默认选项 | 显示“完整颜色匹配（支持叠色，推荐）”，点击导入后实际打开原生 Import Model 窗口。 |
| 手动叠色 | 先手动将原生 Color Count 设为 4 并应用，再主动选择 CMYW；窗口显示组成耗材和比例。打开窗口本身没有自动计算叠色。 |
| 取消导入 | 显示“已取消导入”；准备页打印板为空，仍为初始 4 个物理耗材，没有遗留混合耗材。 |
| 确认叠色导入 | 48 面封闭模型成功导入并放到打印板；生成混合耗材 7～10。 |
| 手动切片及导出 | 调整测试擦料塔位置后显示“切片完成”，成功导出 G-code；80 层，界面估时 59 分 6 秒。 |

原生匹配器初始聚类数量为 2，本次手动改成 4。CMYW 操作根据其现有算法补充了 5、6 号物理耗材；最终为 6 个物理槽、4 个混合槽。此结果验证导入链路和叠色数据保留，不代表匹配色差已达标。

首次试切片提示 G-code 超出热床边界，并在准备页明确提示主塔越界。保存的项目显示擦料塔原位置为 `(165, 250)`，热床范围为 `300 × 270 mm`。仅在测试项目内手动将塔移到 `(224.99, 109.172)` 后重新切片，边界错误消失；没有关闭边界检查或修改打印机预设。首次失败截图也保留。

## 文件证据

本地证据根目录：`D:/Workspace/06_3DDY_claude/.tmp/local-validation-20260907/`。

- `screenshots/01-preview-ready.png`、`02-generation-confirmation.png`：按钮和确认框。
- `screenshots/03-default-native-import.png`、`04-native-matcher-open.png`：默认入口和原生窗口。
- `screenshots/07-import-cancelled.jpg`、`08-cancelled-empty-bed.jpg`：取消结果。
- `screenshots/09-selected-cmyw-before-confirm.jpg`、`10-imported-mixed-filaments.jpg`：选择与导入。
- `screenshots/11-slice-bed-boundary-warning.jpg`、`12-wipe-tower-outside.jpg`：首次试切片边界问题。
- `screenshots/13-mixed-slicing-completed.jpg`、`14-gcode-exported.jpg`：调整后切片、导出成功。
- `verified-mixed-import.3mf`、`verified-mixed-import.gcode`：合成测试工程和对应输出，仅用于本地验证。
- `mixed-import-evidence.json`：3MF 中混合标志、组成、比例、涂色三角面计数及输出文件哈希。4 个颜色各对应 12 面；混合比例为 `20/80`、`50/30/20`、`80/20`、`50/50`。

## 其他验证与边界

- 本轮重新运行 `tools/ai/test_openai_preprocessor.py`：59 项通过；前一轮修复的完整定向记录见 `2026-09-07-generation-recovery-verification.md`（205 项 Python、18 个 C++ 用例）。
- 对 `https://laotie.dev/v1/models` 的无凭据 GET 完成 TLS 连接，返回预期的 HTTP 401，耗时 0.88 秒。它只证明当时本机能够建立 TLS 连接，不证明凭据有效或生成服务稳定。
- 本地测试服务日志没有图片或模型生成提交；没有真实付费生成，没有联网打印。
- 没有制作或发布 EXE 安装包/ZIP，也没有更新测试人员电脑上的旧程序。原有集成检查中固定测试凭据触发的检查结果仍按前一轮报告保留。
