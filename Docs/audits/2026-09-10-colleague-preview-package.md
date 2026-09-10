# 2026-09-10 同事试用包

用户要求打包当前版本并附修改说明，供其自行发送给同事。本次只产生本地 Windows x64 ZIP，没有提交、推送、发布、上传或发送私密配置。

- 产物：`build/colleague-test-20260910/OrcaAI_20260910_x64_portable.zip`，235,239,447 字节。
- SHA-256：`f5d04a4bb63fede49350a0a6e175a2c48ffb743a1047e5d162ab6db221595264`。
- 来源：`codex/team/model-generation`，基础提交 `8d14b41dd07ea3a37f949a218d8d2012878ee218` 加未提交工作树；包内 `package-identity.json` 记录源码文件哈希及实际 EXE/DLL。不是仅凭基础提交可复建的正式发布。
- DLL 与本轮主窗口验收版本一致：`F46427C2CE8352DC6BD3516B68B33A161F502C1D9520037905A4ACCF1823C19E`。保留原编译身份 JSON，并在清单和说明中明确其为旧 CMake 配置元数据，不能把旧版本字符串当作本次来源。
- 使用现有 CMake Release 安装规则暂存，无修改正式发布脚本的分支/干净工作树限制。首次暂存因缺少 `.mo` 失败，使用仓库 msgfmt 对 23 份已有 PO 做 `--check-format` 编译，暂存重跑退出 0；未修改翻译源文件。
- 使用标准 ZIP 收纳暂存文件，排除 `resources/generated_models`、Python 缓存和 PDB；未加入用户照片、模型库、配置目录或服务密钥。`Start-OrcaAI.cmd` 固定工作目录，显式使用包内独立 `user-data` 和 `generated_models`。
- ZIP 完整性检查通过；打包前后源码哈希一致。最终扫描报告 `build/colleague-test-20260910/OrcaAI_20260910_x64_portable.contents.json`：`NOT_DETECTED_WITHIN_SCOPE`、17,130 文件、findings/gaps 均空，与最终 ZIP 哈希绑定。扫描范围与限制保留在报告中。
- 从最终 ZIP 解压至 `.tmp/post-generation-20260910/package smoke/`，随包 Python 3.12.13 / Pillow 12.2.0 原生 PNG 往返通过；由包内启动器实际打开中文首次配置向导。截图 `package-first-launch.jpg`，运行时结果 `package-extracted-runtime.json` 均位于 `.tmp/post-generation-20260910/`。
- 新配置首次向导曾显示加载约 3 分钟后正常进入欢迎页；这是观察到的遗留体验问题，已加入外部修改说明，不能宣称首次启动即时可用。没有操作隐私设置或登录，也没有在新包完整重跑上一轮已验收的美颜旅程。
- 本包未签名，无安装器安装/卸载、另一台电脑、真机打印或真实生成服务验收。外部 `修改说明.txt` 比包内基础说明多一段最终 ZIP 检查和首次启动的实测补充。

打包与验证脚本/日志：`.tmp/post-generation-20260910/package_colleague.py`、`package-install-final.log`、`package-create.json`、`package-scan.log`；交付清单在输出目录 `package-handoff.json`。
