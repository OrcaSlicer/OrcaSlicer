# ADR-007 实施与接入记录

日期：2026-09-09。这是三人协作基础设施的实施记录，产品功能和真实打印验收仍使用各自报告。

**2026-09-10 成员及源码更新：** 三位 GitHub 账号已确认，同事 A 已有 write 权限，同事 B 的 write 邀请已发出；生成三人 CODEOWNERS，并保存本地最新源码及两处跨平台字符串修正。详见[成员接入与源码快照](2026-09-10-team-members-and-source-snapshot.md)。下面的缺账号／旧构建状态属于更早记录，新配置进入集成仍需真实成员参与的首次迁移 PR。

**2026-09-10 后续核查：** [候选及原生构建 run 34362293686](https://github.com/arsenaltj/OrcaSlicer/actions/runs/34362293686) 已结束，整体失败。Windows 构建和 C++ 测试成功；Linux/macOS 在该提交 `ModelGenerationPanel.cpp` 第 2818、3315 行的三元表达式出现 `wxString`／`const wxChar *` 类型歧义，相关测试未执行。下文“仍在运行”是前一日记录，不能作为当前通过证据。本轮细化了根 AGENTS 和[团队操作手册](../coordination/team-integration-sop.md)，覆盖同步前后通知、PR 串行合入及逐构建版本档案；未修改产品源码、启用自动合并或部署飞书。完整版本归档仍待实现，修复云端基线是上线前置工作。

## 已确定的工作方式

用户已接受 ADR-007，使用公开仓库 `arsenaltj/OrcaSlicer`；重新创建四条 `codex/team/*` 分支，旧开发分支停止用于新工作并保留历史。当前用户 GitHub 登录名已通过 API 确认是 `arsenaltj`，负责模型生成。两位同事的账号暂缺，没有填写虚假身份或邀请未知账号。

四条新分支分别为 `codex/team/model-generation`、`codex/team/smart-slicing`、`codex/team/maintenance`、`codex/team/integration`。它们的共同起点应是本次完整源码快照，不能只使用遗漏未提交美颜／配色成果的旧 HEAD。`origin` 已连接上述仓库；官方 `upstream` 保留。首个完整源码快照为 `914e3219012ab8e12ea2f725ac7d43d677bb70e2`，四条新分支已推送。当前本地 checkout 已切换模型生成分支；后续基础设施修正在新分支正常快进，不改写历史。新起点保留目前固定 Orca 上游 SHA，上游同步另开独立 PR 验证，不表示已经合入最新 main 或旧智能切片新增内容。

## 本地实现

- 集成锁迁移为 v4，声明四条新分支、开发线从集成普通 merge、禁止强推、共享区域复核和两个必需检查。历史 source SHA／receipt、运行时版本、端口及架构预算保持不变。
- 历史校验按固定 SHA 和对象收据执行，不再依赖旧角色分支当前 ref。CI 缺对象时仅按锁中完整 SHA 拉取；本地 `--check-only` 不联网、不触碰旧工作区或被禁止比较的 archive。
- 修复两个精确测试假凭据误报，保留其他字面凭据扫描；没有关闭检查。PR 模板同步新开发流程，ModelFinishing 及协作脚本补充 CODEOWNERS 范围。最终三人 CODEOWNERS 由真实账号生成；当前仅是现有账号的启动配置。
- `AI integration checks` 和 `Team integration candidate` 始终响应目标 PR／merge_group；不使用会让必需检查一直 pending 的路径过滤。角色 push 运行 AI 检查，集成 push 另外运行候选及适用构建。
- 候选报告绑定仓库、PR、源 HEAD、集成基点、合并提交、Actions run／attempt。检查源码删除／重命名和构建入口变化；适用任务失败、取消、跳过或版本变化均不能报成功。仅当未改变的基线已有成功候选检查时才按路径缩小构建；未构建或失败的初始基线不能被后续文档提交掩盖。原生改动复用 Windows 构建和 C++ 测试；依赖／构建入口／上游同步扩大到 Linux/macOS。使用 GitHub 托管机器，后续独立 Windows 构建机须先隔离并验收。
- 独立飞书服务采用官方 SDK 可选长连接、GitHub GET 和 SQLite。限定群、用户和命令，持久消息去重、队列、单卡片通知和失败重试；当前集成 CI 缺失阻止准入，失败持久暂停。保留本机暂停／恢复及卡片恢复工具。服务不检出 PR 代码，不含 GitHub merge／dispatch 写 API。
- 初始阶段由维护人人工合入；自动合入是第三阶段，需真实 PR 演练后另行实施。飞书应用发布、通知服务启动、付费模型生成和正式产品发布均未因本地代码完成而自动执行。

## 验证入口

```powershell
python scripts/verify_ai_integration.py --json
python scripts/fetch_ai_provenance.py --check-only
python scripts/run_ai_offline_tests.py
python -m unittest discover -s tools/team_integration -p 'test_*.py' -q
python -m unittest discover -s scripts/team_collaboration -p 'test_*.py' -q
python -m unittest discover -s scripts -p 'test_team_ci_candidate.py' -q
python -m unittest discover -s scripts -p 'test_fetch_ai_provenance.py' -q
```

完整集成校验已通过，未跳过 Git 检查；原有四项问题均按真实原因解决。协作服务 52 项、分支准备 14 项、候选 8 项、来源对象 10 项离线测试通过。测试中的 GitHub／飞书请求为显式替身；AI suite 的 Python 外网访问受拦截，诊断子进程只使用本机端口，打包子进程单独拒绝网络。付费验证脚本的测试只使用模拟 provider，不代表调用真实付费服务。

全量 AI 测试暴露了四份旧测试仍要求限色／透明背景及旧入口参数的情况；已根据 CONTINUATION 中当前产品规则修正，保留用户明确颜色、单次调用、失败恢复和不重复提交覆盖。没有为了使 CI 通过而恢复已取消的产品行为。最终全量 AI 测试 671 项全部通过，耗时 114.884 秒。运行输出保留在本地 `.tmp/adr007-ai-tests-final.log`；远程构建结果以 GitHub 实际 run 为准，本地 Python 通过不等于真实 GUI、全平台构建或产品验收通过。

## 团队接入剩余条件

1. 两位同事提供 GitHub 登录名后，确认角色、仓库访问权限，并用 `scripts/team_collaboration/bootstrap.py plan` 生成真实 CODEOWNERS 和复核规则。当前不能宣称已完成三人互审。
2. 提供飞书租户、群、三位成员的应用内稳定 open ID、机器人 open ID；创建／授权企业自建应用。App Secret 和 GitHub token 只在仓库外的服务环境保存。
3. 确定持续在线服务机，填写 `tools/team_integration/config.example.json` 对应的私有运行配置，核实真实 GitHub Actions app_id、分支保护及 CI 权限，再安装 SDK、部署后台服务并做真实群联调。
4. 用无害 PR 演练正常通过、失败、冲突、双 PR 排队、HEAD／基点变化、重复消息、通知失败、重启和暂停恢复，保留真实版本／CI／卡片证据。规则未验证前保持人工合入。

公开团队配置：`.github/team-collaboration.json`。操作说明：[分支准备](../../scripts/team_collaboration/README.md)、[飞书服务](../../tools/team_integration/README.md)。仓库已有旧分支不删除；GitHub Actions 应用 ID 已通过实际 check run 确认为 `15368`。新分支及远程规则以实际 GitHub 状态为准，不把生成的部署 JSON 当作已经应用。

## 远程设置实测（2026-09-09）

四条新分支已经从完整源码及协作修正提交 `7b6b270349a48d5b85a3c74a934f89c84da1c8ad` 建立并推送；旧分支未改写或删除。本机当前工作分支是 `codex/team/model-generation`。GitHub 默认分支已切到 `codex/team/integration`。

已通过 API 回读验证：四条新分支均受保护、禁止强推和删除；集成分支要求两个绑定 GitHub Actions 应用 `15368` 的检查、strict 最新基线、过期审批失效、CODEOWNER 审批、一次非作者审批及最后一次 push 审批、讨论解决，管理员同样受约束。飞书服务的保护校验器对实际返回数据判定通过。个人仓库不发送仅组织可用的 bypass allowances 字段；保护请求使用 checks，不同时发送 contexts。普通开发分支允许日常 push。仓库自动合并和合并后自动删除分支关闭，保留 merge commit，rebase merge 关闭。

原上游的定期工单分配／提醒、重复工单自动关闭两个 workflow 已在此团队仓库停用，避免把它们当作新团队机器人运行；没有删除其上游源码。Orca 上游预检仍需显式开启 `TEAM_UPSTREAM_PREVIEW_ENABLED`。

云端验证：集成分支 [AI 检查](https://github.com/arsenaltj/OrcaSlicer/actions/runs/34362292520) 已成功；[候选检查与原生构建](https://github.com/arsenaltj/OrcaSlicer/actions/runs/34362293686) 的精确版本及协作测试部分已成功，Windows／Linux／macOS 构建仍在运行。本记录不把运行中构建写成通过，也不代表飞书或真实 PR 旅程已验收。更晚状态以链接中的实际结果为准。

工作区仅剩 `.tmp/`、`graphify-out/`、`resources/generated_models/` 三类本地产物未跟踪；没有把它们或私有凭据推送。远程 API 回执保留在 `.tmp/adr007-remote-setup.json` 和 `.tmp/adr007-live-integration-protection.json`。
