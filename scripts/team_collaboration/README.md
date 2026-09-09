# ADR-007 本地准备工具

使用 Python 标准库和本机 Git，默认读取仓库根目录的 `.github/team-collaboration.json`。此工具不访问网络、不 push、不创建 GitHub 仓库、不修改远程保护规则，也不发送飞书消息。

## 检查当前准备状态

在仓库根目录运行：

```powershell
python scripts/team_collaboration/bootstrap.py check
```

输出 JSON 包括缺失的仓库名／三人账号／基线 SHA、工作区是否干净、`origin` 是否匹配配置，以及四条**本地**分支的准备状态。`check` 成功读取时返回 0；`blockers` 为空才代表本地准备条件齐全。`remote_protection_verified` 始终为 false：本地检查不能证明 GitHub 已部署保护或远程分支已经对齐。

工作区检查包括未跟踪文件，不忽略生成目录，也不自动提交、清理或收纳现有工作。配置包含公开的团队标识；凭证和应用密钥不得写入配置。示例见 `team-collaboration.example.json`，未知账号保持 `null`。`feishu.users` 为稳定 `ou_...` open ID 到已配置 GitHub 登录名的映射。

## 生成可审阅的部署文件

补齐 `repository`（如 `owner/repository`）和三个不同的 GitHub 登录名（不含 `@`）后运行：

```powershell
python scripts/team_collaboration/bootstrap.py plan --output-dir D:/TeamSetup/adr007-plan
```

`--output-dir` 必须显式提供。工具拒绝覆盖已有同名文件，建议使用新的目录，且放在仓库外，避免生成物使基线工作区变脏。

生成内容：

- `readiness.json`：本地状态和待办条件。
- `CODEOWNERS`：根据架构锁归属和真实账号生成，补齐 ModelFinishing、共享运行时和团队脚本。
- `integration-branch-protection.json`：`codex/team/integration` 的 GitHub REST 请求体。
- `developer-branch-protection.json`：三条开发分支禁强推、禁删除的请求体；允许普通开发 push。
- `deployment-plan.json`：具体 API 路径、仓库合并设置、部署顺序及验证要求，`applied` 为 false。

把 CODEOWNERS 和团队 CI 文件纳入经过验证的基线后，再在 GitHub 实施计划。保护请求要求严格最新基线检查、两个固定的必需检查、PR、至少一次审批、CODEOWNER 审批、提交后过期审批失效及最后一次 push 的非推送者审批，并对管理员生效。保留 merge commit，不要求线性历史。应用请求前先读取并保存现有保护设置；PUT 请求会替换部分设置，不能盲目覆盖更严格的现有规则。

GitHub 的 CODEOWNERS 对一行列出的多个账号只要求**其中一个**批准，不保证每人批准。模块路径列模块负责人和维护人；共享路径及默认路径列三人，避免维护人作为作者时无法获得另一位 owner 审批。维护人仍须安排相关非作者复核和必要验证证据。真实账号未知时不会生成占位 CODEOWNERS；生成器也不能验证远程账号存在或已获得 write 权限，上线时需确认。

## 准备四条本地分支

先审阅、验证并保存现有工作，确定共同基线；不会由此工具代替团队选择基线。按 2026-09-09 用户确认的新建分支决定，四条新长期分支为 `codex/team/model-generation`、`codex/team/smart-slicing`、`codex/team/maintenance`、`codex/team/integration`。旧 `codex/model-generation`／`codex/smart-slicing` 等分支停止用于新团队开发，原样保留为历史，不比较、不覆盖、不删除。新分支从已经完整保存、经团队验收的当前源码基线建立。

在**干净且 HEAD 就是已验收提交**的工作区执行：

```powershell
python scripts/team_collaboration/bootstrap.py prepare-branches --baseline <完整40位提交SHA>
```

该命令只把缺失的四条本地分支一次性创建在指定提交上，不 checkout，不改动已有分支。任一同名本地分支已位于不同提交、HEAD 不一致、存在暂存／未暂存／未跟踪改动，均拒绝操作。重复运行不会重复创建分支。

不要为把“当前提交自己的 SHA”写入受 Git 管理的配置而再提交一次，否则 HEAD 会变化。受管理的配置可保留 `bootstrap.baseline_sha: null`，使用 `--baseline` 提供验收 SHA；如需保存该 SHA，使用仓库外的部署配置或生成记录。若配置已经明确设置 `baseline_sha`，则它必须与命令参数一致。

自定义配置与仓库位置参数放在子命令之前：

```powershell
python scripts/team_collaboration/bootstrap.py --repo D:/Workspace/11_3DDY_Continue --config D:/TeamSetup/team.json check
```

后续 GitHub 保护验证、隔离构建机配置、飞书应用配置与真实 PR 验收按 ADR-007 和部署清单进行。第一阶段固定 `notify_and_preview`、`auto_merge: false`；生成计划和创建本地分支均不启用自动合并。

## 离线验证

```powershell
python -m unittest discover -s scripts/team_collaboration -p 'test_*.py' -v
```

测试只在系统临时目录创建合成 Git 仓库，禁用用户 Git 配置，不 fetch，也不调用 GitHub、飞书或任何模型 provider。覆盖基线分支事务、幂等、脏工作区、分支冲突、身份／路径校验、凭据脱敏和生成的保护约束。

GitHub 字段参考：[保护分支 REST API](https://docs.github.com/en/rest/branches/branch-protection#update-branch-protection)、[CODEOWNERS 语义](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/about-code-owners)。线上保护仍需在实际仓库确认；离线单元测试不能替代真实 PR 验收。
