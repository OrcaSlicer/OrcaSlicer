# 三人协作助手（ADR-007 第一阶段）

这是独立、单机持久化的 Python 服务。当前只读 GitHub、接收限定飞书命令、维护检查队列和一项一张卡；**全部合并由维护人人工完成**。代码没有 GitHub 写请求、workflow dispatch、merge API，也不检出或执行 PR 代码。候选编译和离线测试由仓库 CI 执行。

当前仓库实现已经过完全离线测试；真实 GitHub/飞书接入、Windows 构建机和真实 PR 演练仍须使用团队实际配置验收。没有凭据和已验证 CI，不能宣称已上线。第三阶段自动合入不在当前实现内，不存在修改配置即可开启的自动合并开关。

## 配置与运行

需要 Python 3.10+。服务与配置应放在持续在线的机器上，以独立服务账号运行；不要依赖交互式 Codex 任务。GitHub HTTP、SQLite、离线测试仅用标准库。只有连接飞书时需要官方 SDK：

```powershell
python -m venv C:\TeamIntegration\venv
C:\TeamIntegration\venv\Scripts\python.exe -m pip install -r tools/team_integration/requirements.txt
```

`requirements.txt` 限定 SDK 大版本；接入验收时将实际安装版本记入部署记录后固定该环境。本次没有安装或启动服务。Linux/macOS 用相应的 `python3` 和虚拟环境路径。

将 `config.example.json` 复制为仓库外的私有文件，例如 `C:\TeamIntegration\config.json`，填完所有占位符。项目根的团队 bootstrap 配置用于创建仓库分支和规则；这里的运行配置另含飞书稳定身份、凭据环境变量名和状态目录，两者用途不同。

| 字段 | 要求 |
| --- | --- |
| `repository` | 唯一团队 GitHub `owner/repo`，不支持任意 PR URL 或 fork |
| `target_branch` | `codex/team/integration` |
| `source_branches` | 三条新长期分支 `codex/team/model-generation`、`codex/team/smart-slicing`、`codex/team/maintenance` |
| `task_branch_prefixes` | 明确允许的临时前缀；默认 `codex/task/`、`codex/upstream-sync-` |
| `users` | 恰好三位成员的飞书应用内稳定 `open_id` → GitHub 登录名，不使用显示名 |
| `maintainer_ids` | 上述成员中的维护人，可重试/取消其他成员 PR |
| `chat_id` / `bot_open_id` | 唯一团队群和机器人身份；其他群、无明确 @、其他用户全部忽略 |
| `required_checks` | 至少 `AI integration checks`、`Team integration candidate`，每项必须填 API 验证的 GitHub Actions `app_id`；示例 0 故意不能运行 |
| `required_approvals` | 当前完整 HEAD 的非作者团队审批数量，至少 1 |
| `database_path` | 仓库外持久目录中的绝对 SQLite 路径，仅服务账号可读写；不要放网络共享盘 |
| `*_env` | 环境变量名称；JSON 中不填任何 token、App Secret |

运行时环境使用 `TEAM_GITHUB_TOKEN`、`TEAM_FEISHU_APP_ID`、`TEAM_FEISHU_APP_SECRET`（或配置中指定的名称）。由操作系统服务凭据配置提供，勿写入仓库、命令历史、公开产物或日志。GitHub 凭据仅授予该仓库 `metadata:read`、`contents:read`、`pull_requests:read`、`checks:read`、`actions:read`、`administration:read`，最后一项只用于核对保护规则。安装 token 的刷新应由部署环境提供；服务不会自行签发/续期 GitHub token。

飞书企业自建应用开启机器人，订阅 `im.message.receive_v1`，使用官方 SDK 长连接；启用群内 @ 消息接收和以机器人身份发送/更新消息所需权限，发布应用并加入配置群。权限及真实 `open_id` 在飞书开放平台核实。这里不新增公网 webhook 服务器。

从仓库根执行：

```powershell
# 纯本地校验，不访问网络，不读取凭据，不创建数据库。
python -m tools.team_integration --config C:\TeamIntegration\config.json validate

# 只读 GitHub 一次，不连接飞书、不发送消息。
python -m tools.team_integration --config C:\TeamIntegration\config.json run --once

# 明确启动持续 GitHub 轮询、飞书长连接和团队群卡片后使用。
python -m tools.team_integration --config C:\TeamIntegration\config.json run --listen-feishu --send-feishu

# 本机服务账号可管理持久队列，不通过群聊天授予控制权限。
python -m tools.team_integration --config C:\TeamIntegration\config.json status
python -m tools.team_integration --config C:\TeamIntegration\config.json pause
python -m tools.team_integration --config C:\TeamIntegration\config.json resume
```

运行命令应交给团队持续在线主机的服务管理器；设置仓库工作目录、专用账号、失败重启和私有持久目录。Windows 使用管理员选定的服务管理器/任务计划，隐藏窗口；Linux 可用 systemd。不要在未完成真实配置和群通知授权时启动上面的联网模式。一个数据库只允许一个服务 worker；进程崩溃由系统释放实例锁。SQLite inbox、队列和 outbox 跨重启保留。备份时停止服务，或用 SQLite 在线 backup；不要只复制正在使用的主文件而丢掉 WAL。

数据库同时绑定仓库、`chat_id` 和 `bot_open_id`。修改群或机器人后不能继续使用原库，否则会拒绝启动，避免更新旧群卡片。已有工作项/消息但缺少这些身份元数据的旧数据库也拒绝启动；应停服务、备份并明确核对旧群/机器人与全部卡片 ID 后实施离线迁移，不能自动猜测。切换群或应用时使用另一个全新数据库并保留旧库供审计；这会在新群建立独立通知记录。

## 命令和状态

群内必须明确 @ 已配置机器人，随后是以下完整命令（不能附加其他自然语言）：

```text
@集成助手 提交 PR #42
@集成助手 查状态 PR #42
@集成助手 重试 PR #42
@集成助手 取消排队 PR #42
@集成助手 查看冲突 PR #42
```

三位成员均可查状态/冲突；提交、重试、取消由 PR 作者或维护人执行。消息事件由 SDK 快速确认，命令先落 SQLite，后台在下一次轮询处理并更新该 PR 卡片；普通轮询默认间隔 30 秒，GitHub API 故障时保留未完成命令。消息 `message_id` 持久去重；同一 PR/HEAD 不重复创建工作项或卡片。PR ready 状态也会自动入队；draft 只通知，取消的同一 HEAD 不会被轮询重新排队。

PR 正文可增加唯一仓库内的显式依赖：`Depends-On: #12, #13`。依赖必须已经合入相同集成目标；关闭而未合入不算完成。循环依赖会持续等待，成员需纠正依赖；不自动改写 PR。

状态包括等待依赖、待检查、检查中、冲突、失败、可合入（人工）、已合入。每轮按持久顺序串行核对，冲突/失败项让开其他事项；修正 HEAD/base 或收到重试命令后重验。候选缺失、API 异常、配置或规则不足都停在待检查。HEAD/base 改变会使旧候选失效；每次报告可合入之前再读双方 HEAD 和当前审批。人工合入之后通过 API 对账为已合入，不发送发布或测试者消息。

当前集成 HEAD 自己也必须具备所有配置的成功 CI；初始缺检查或检查进行中时，新的 PR 不能被报告可合入。读取到该 HEAD 的可信必需检查明确失败时，服务自动持久暂停队列，更新已合入项和等待项原有卡片，提示维护人检查失败并通过独立 revert PR 恢复。`status` 显示 `pause_reason`。人工 `resume` 后若同一当前基线仍失败，下一轮立即重新暂停；旧 SHA 的失败检查不会覆盖新 SHA 的成功结果。恢复基线后仍需维护人明确 resume，避免未经处理自动恢复准入。

一张卡显示 PR、作者、标题、双方短 SHA、状态和链接，后续 push/CI/合并更新原卡。通知失败只重试 outbox，不重新排队 PR。首次发卡使用确定性的 `uuid`；飞书去重仅有一小时窗口，若首次发送超时且 3500 秒内仍不能确认消息 ID，服务停止自动重建该卡。维护人从群内确认真实消息 ID 后本地恢复：

```powershell
python -m tools.team_integration --config C:\TeamIntegration\config.json recover-card 42 om_CONFIRMED_MESSAGE_ID
```

卡片恢复操作前必须停止服务，命令会取得实例锁以避免与正在发卡的 worker 竞态。恢复命令不发送消息；重启服务后更新已确认卡片。`status` 中 `create_started` 非零、`card_id` 为空且超过窗口表示需要人工核对。被人工删除的卡不会自动补发以免重复，需要同样的维护处理。

若首次创建因权限等问题一直失败、群内明确从未生成这张卡，维护人先修复权限，停服务并在群内确认不存在该 PR 的卡片，然后使用显式恢复命令：

```powershell
python -m tools.team_integration --config C:\TeamIntegration\config.json reset-card-create 42 --confirmed-not-created
```

该命令仅清除首次创建的失败等待状态，重新启动服务才会尝试发送；没有确认参数、已有绑定卡片或 worker 正在运行时拒绝操作。如果发送结果仍不确定，不要确认不存在，继续核对并使用 `recover-card` 绑定实际 ID。服务不会自动绕过飞书有限去重窗口。

## CI 与保护规则接口

准入需要目标分支开启管理员也受约束、严格最新基线检查、过期审批失效、CODEOWNERS 审批、最后一次 push 由其他人审批、讨论全部解决，以及 `required_checks` 中的名字和指定 App ID。必须禁用强推和删分支，PR 审批绕过的用户/团队/应用名单必须为空。读取不到保护或上述任一规则不足时不报告可合入。共享文件/CI/打印行为的额外复核和主窗口证据仍由维护人按 PR 模板验收；本服务不从 PR 中的自述证明 GUI/打印质量。

候选工作流固定 `.github/workflows/team-integration-candidate.yml`。读取 GitHub API 返回的最新 `pull_request` workflow run：必须路径一致、完整 source HEAD 一致、当前 run attempt 成功。工作流上传唯一 artifact `team-integration-candidate`，ZIP 内仅 `candidate.json`：

```json
{
  "schema_version": 1,
  "repository": "team/orca",
  "pr_number": 42,
  "head_sha": "<40 lowercase hex characters>",
  "base_sha": "<40 lowercase hex characters>",
  "candidate_sha": "<40 lowercase hex characters>",
  "status": "success",
  "run_id": 123456789,
  "run_attempt": 1
}
```

报告只在候选构建、适用离线测试和集成检查真正成功后生成；缺少构建机或运行证据不能生成 success。服务验证候选 Git commit 的父节点顺序恰为 `[base_sha, head_sha]`，比对当前 GitHub PR 的合并候选；读取实际 checks 的可信 App ID、最新 source HEAD 和 success，并核对候选 check 属于该报告的 check suite。仅 source HEAD 的旧绿灯不能替代新集成基点的候选结果。Actions artifacts 由只读 API 下载，签名存储 URL 不携带 GitHub token，ZIP 不解压到文件系统。

候选工作流可复用独立 Windows 构建路径并提供 CI 构建/验证 artifacts；该服务只读这些证据，不自行打包发布。实际构建路径、缓存和产物范围以工作流及主机配置为准，仍需真实 PR 演练确认。

GitHub API 轮询不触发 CI。缺候选或集成基点变化时，维护人应同步个人分支到最新集成并 push 触发新 PR 检查；仅重跑旧事件可能继续使用旧 base，不能当作新基线验证。自动化代码由受保护分支和 CODEOWNERS 控制，PR 正文、群聊天及模型输出不能改变允许仓库、身份或准入策略。

## 验证与上线验收

```powershell
python -m unittest discover -s tools/team_integration -p 'test_*.py' -q
```

测试模块全程禁止 socket 连接，GitHub、飞书均使用显式 mock transport/fixture；不会读取环境中的真实 token、访问 provider、联系群成员或合入。覆盖正常通过、失败/冲突让开其他 PR、第二个 PR 在首个合入后冲突、作者追加提交、基点变化、最终检查竞态、依赖、非作者审批、伪造 App/工作流/版本、重复消息、取消、暂停恢复、通知失败及进程重启保留单卡。

上线前仍需用无害真实 PR 验证 ADR-007 的全部场景，记录 Git SHA、CI URL、Windows 构建/测试结果及受影响主窗口证据。检查通过只代表这些明确检查的范围；本地 mock 测试不证明飞书权限、GitHub 套餐保护可用性、真实主窗口、provider 稳定性或打印质量。通过几轮人工验收后再决定第三阶段范围。

官方接口依据（2026-09-09 核对）：[Python SDK](https://github.com/larksuite/oapi-sdk-python)、[长连接 sample](https://github.com/larksuite/oapi-sdk-python/blob/v2_main/samples/ws/sample.py)、[发送/更新卡片](https://open.feishu.cn/document/server-docs/im-v1/message/create)、[消息接收事件](https://open.feishu.cn/document/server-docs/im-v1/message/events/receive)、[GitHub PR](https://docs.github.com/en/rest/pulls/pulls)、[checks](https://docs.github.com/en/rest/checks/runs)、[reviews](https://docs.github.com/en/rest/pulls/reviews)、[Actions artifacts](https://docs.github.com/en/rest/actions/artifacts)。
