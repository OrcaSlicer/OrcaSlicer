# 同事验证包记录

本次按用户要求交付刚完成本机验证的 Windows x64 工作树构建，用于同事测试。不是官方集成分支发布，不执行上传、网站更新或私密配置分发。

## 构建来源

- 基础提交：`6e3c6e658dc964b831f9005f6a97785124d9d9a6`，包含当前未提交修改和本次生成修复；不能仅凭基础提交重建相同内容。
- 复用已实际构建和操作验证的 Release EXE/DLL。验证见 `2026-09-07-generation-local-validation.md`。
- 使用现有 CMake 安装规则和 CPack NSIS/ZIP 生成本地测试包。没有修改正式集成发布脚本的分支、干净工作树、来源或检查约束；没有把本次产物冒充官方集成发布。
- `ORCA_AI_INTERNAL_DEFAULTS_FILE` 为空，安装规则包含指定的 AI 运行文件和 Python/Pillow。实际 ZIP 内程序、修复后的 Sidecar 模块均与已验证文件逐字节/哈希匹配。
- `build/colleague-test-20260907/source-and-input-manifest.json` 记录基础提交、工作树属性及 16,066 个资源/模块输入的 SHA256。打包后输入无变化。

## 产物与验证

目录：`D:/Workspace/06_3DDY_claude/build/colleague-test-20260907/`。

- `OrcaAI_Test_20260907_x64.exe`：192,606,947 字节。
- `OrcaAI_Test_20260907_x64_portable.zip`：237,445,183 字节。
- 同目录提供 `.sha256`、各自的 `.contents.json`、`同事验证说明.txt`。
- 安装器签名状态为 `NotSigned`。
- 打包运行时检查：Python 3.12.13、Pillow 12.2.0、原生 PNG 往返、独立 Sidecar 导入通过；没有使用开发机供应商配置或网络生成请求。
- 打包模块测试 2 项、内容检查工具测试 8 项通过。最终实际产物检查结果以对应 SHA256 绑定的 `.contents.json` 为准。
- 最终 EXE 扫描 17,339 个文件，ZIP 扫描 17,330 个文件；两者均为 `NOT_DETECTED_WITHIN_SCOPE`，无未处理发现、无检查缺口。
- EXE SHA256：`e9b969321944abd7f868625269784efb32329035bb57bcd032b419ab215da51e`。
- ZIP SHA256：`16f339a49651954a6ce9308534420fe57a0ffc4899e043a0bf2fa7276bf878c0`。
- 汇总交付身份：同目录 `package-handoff.json`。复检前后的两包哈希一致。

## 首次内容检查误报及处理

初次 EXE 和 ZIP 都只发现 `resources/tooltip/main.js` 的 `secret` 字段，无检查缺口。它是公开 emoji 名称表中的 U+3299 U+FE0F；相邻项为 `ideograph_advantage`、`congratulations` 等 emoji 名称，没有供应商配置语义。

保留两份 `*.initial-findings.json` 原始报告。没有删改资源以躲避检查，也没有修改既有报告的结果。检查器仅为字段名 `secret`、该固定 Unicode 值、固定资源路径以及完整文件 SHA256 `a4040f542802a7c939c6823986b239a334fffd4eadbb41961f051052e7ccdfdf` 的组合添加 `PUBLIC_EMOJI_NAME_MAPPING` 分类，报告中继续保留候选记录。

回归验证包括：追加凭据、改变 emoji 值、换路径、仅仿造该字段、其他文件包含凭据均继续阻塞。任何资源字节变化都使该已核实分类失效。对原产物重新完整检查，文件哈希不变；检查器不进入程序包，所以此校正不改变已验证程序。

## 保留边界

此前集成检查中固定测试凭据触发的源码检查项继续保留，没有把总体集成门禁称为通过。实际包的检查不能推断凭据有效性、所有编码形式的秘密不存在、实物打印资格或历史公开包已经整改。本次不导出私密配置、不提交真实生成、不更新他人电脑。
