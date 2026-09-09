"""Durable single-worker scheduling and notifications; all merges remain manual."""
import hashlib
import json
import re
import sqlite3
import threading
import time

from .github import GitHubHTTPError, number, sha

STATES = {"waiting": "等待依赖", "pending": "待检查", "checking": "检查中", "conflict": "冲突",
          "failed": "失败", "ready": "可合入（人工）", "merged": "已合入", "closed": "已关闭",
          "cancelled": "已取消排队", "draft": "草稿"}
COMMAND = re.compile(r"(提交|查状态|重试|取消排队|查看冲突)\s+PR\s*#([1-9][0-9]{0,8})", re.IGNORECASE)


class Store:
    def __init__(self, path, repository, chat_id, bot_open_id):
        self.identity = {"repository": repository, "chat_id": chat_id, "bot_open_id": bot_open_id}
        self.lock = threading.RLock()
        self.db = sqlite3.connect(path, timeout=30, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        self.db.executescript("""
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS inbox (id TEXT PRIMARY KEY, actor TEXT NOT NULL, action TEXT NOT NULL,
                pr INTEGER NOT NULL, done INTEGER NOT NULL DEFAULT 0);
            CREATE TABLE IF NOT EXISTS jobs (pr INTEGER PRIMARY KEY, head TEXT NOT NULL DEFAULT '',
                base TEXT NOT NULL DEFAULT '', title TEXT NOT NULL DEFAULT '', author TEXT NOT NULL DEFAULT '',
                state TEXT NOT NULL DEFAULT 'pending', detail TEXT NOT NULL DEFAULT '', queued INTEGER NOT NULL DEFAULT 1,
                sequence INTEGER NOT NULL, card_id TEXT, revision INTEGER NOT NULL DEFAULT 0,
                delivered INTEGER NOT NULL DEFAULT 0, failures INTEGER NOT NULL DEFAULT 0,
                next_send REAL NOT NULL DEFAULT 0, create_started REAL NOT NULL DEFAULT 0);
        """)
        try:
            with self.db:
                saved = dict(self.db.execute("SELECT key,value FROM settings WHERE key IN ('repository','chat_id','bot_open_id')"))
                if any(saved[key] != value for key, value in self.identity.items() if key in saved):
                    raise ValueError("state database belongs to another repository, Feishu chat, or bot")
                has_work = (self.db.execute("SELECT EXISTS(SELECT 1 FROM jobs)").fetchone()[0] or
                            self.db.execute("SELECT EXISTS(SELECT 1 FROM inbox)").fetchone()[0])
                if set(saved) != set(self.identity) and has_work:
                    raise ValueError("populated state database lacks notification identity; explicit offline migration required")
                for key, value in self.identity.items():
                    self.db.execute("INSERT OR IGNORE INTO settings VALUES (?,?)", (key, value))
                self.db.execute("INSERT OR IGNORE INTO settings VALUES ('paused', '0')")
                self.db.execute("INSERT OR IGNORE INTO settings VALUES ('pause_reason', '')")
        except Exception:
            self.db.close()
            raise

    def close(self):
        self.db.close()

    def jobs(self):
        with self.lock:
            return [dict(r) for r in self.db.execute("SELECT * FROM jobs ORDER BY sequence, pr")]

    def job(self, pr):
        with self.lock:
            row = self.db.execute("SELECT * FROM jobs WHERE pr=?", (pr,)).fetchone()
            return dict(row) if row else None

    def update(self, pr, **values):
        allowed = {"head", "base", "title", "author", "state", "detail", "queued"}
        if not values.keys() <= allowed:
            raise ValueError("unknown state field")
        with self.lock, self.db:
            old = self.job(pr)
            if old is None:
                seq = self.db.execute("SELECT COALESCE(MAX(sequence),0)+1 FROM jobs").fetchone()[0]
                self.db.execute("INSERT INTO jobs(pr,sequence) VALUES (?,?)", (pr, seq))
                old = self.job(pr)
            if all(old[k] == v for k, v in values.items()):
                return
            fields = ",".join(k + "=?" for k in values)
            self.db.execute("UPDATE jobs SET " + fields + ",revision=revision+1 WHERE pr=?", (*values.values(), pr))

    def pause(self, paused, reason=""):
        with self.lock, self.db:
            self.db.execute("UPDATE settings SET value=? WHERE key='paused'", ("1" if paused else "0",))
            self.db.execute("UPDATE settings SET value=? WHERE key='pause_reason'", (reason if paused else "",))

    def paused(self):
        with self.lock:
            return self.db.execute("SELECT value FROM settings WHERE key='paused'").fetchone()[0] == "1"

    def pause_reason(self):
        with self.lock:
            return self.db.execute("SELECT value FROM settings WHERE key='pause_reason'").fetchone()[0]


def accept_message(config, store, event):
    """Called only with an official SDK event; no text can supply the actor ID."""
    if event.get("chat_id") != config["chat_id"] or event.get("actor") not in config["users"]:
        return False
    if event.get("sender_type") != "user" or event.get("message_type") != "text":
        return False
    text = event.get("text", "")
    mentions = [m for m in event.get("mentions", []) if m.get("open_id") == config["bot_open_id"]]
    if len(mentions) != 1 or not mentions[0].get("key") or not text.startswith(mentions[0]["key"]):
        return False
    match = COMMAND.fullmatch(text[len(mentions[0]["key"]):].strip())
    if not match or not isinstance(event.get("message_id"), str) or not event["message_id"]:
        return False
    with store.lock, store.db:
        result = store.db.execute("INSERT OR IGNORE INTO inbox(id,actor,action,pr) VALUES (?,?,?,?)",
                                  (event["message_id"], event["actor"], match[1], int(match[2])))
        return result.rowcount == 1


def dependencies(body, own_pr):
    result = set()
    for line in (body or "").splitlines():
        if not line.lower().startswith("depends-on:"):
            continue
        value = line.split(":", 1)[1].strip()
        if not re.fullmatch(r"#[1-9][0-9]*(?:\s*,\s*#[1-9][0-9]*)*", value):
            raise ValueError("依赖格式应为 Depends-On: #12, #13")
        result.update(number(int(n)) for n in re.findall(r"#([0-9]+)", value))
    if own_pr in result or len(result) > 20:
        raise ValueError("依赖不能包含自身且最多 20 项")
    return sorted(result)


class Service:
    def __init__(self, config, store, github, notifier=None):
        if store.identity != {key: config[key] for key in store.identity}:
            raise ValueError("service configuration differs from state database identity")
        self.config, self.store, self.github, self.notifier = config, store, github, notifier

    def allowed(self, pr):
        c = self.config
        branch = pr["head"]["ref"]
        return (pr["base"]["repo"]["full_name"].lower() == c["repository"].lower() and
                (pr["head"].get("repo") or {}).get("full_name", "").lower() == c["repository"].lower() and
                pr["base"]["ref"] == c["target_branch"] and
                (branch in c["source_branches"] or any(branch.startswith(p) for p in c["task_branch_prefixes"])) and
                pr["user"]["login"].lower() in {u.lower() for u in c["users"].values()})

    def reconcile(self, pr, base):
        n, head = number(pr["number"]), sha(pr["head"]["sha"])
        old = self.store.job(n)
        if not self.allowed(pr):
            if old:
                self.store.update(n, state="failed", detail="仓库、分支或作者不符合配置", queued=0)
            return
        values = {"head": head, "base": base, "title": pr.get("title", "")[:200], "author": pr["user"]["login"]}
        if pr.get("merged"):
            values.update(state="merged", detail="GitHub 确认已由人工合入", queued=0)
        elif pr["state"] != "open":
            values.update(state="closed", detail="PR 已关闭", queued=0)
        elif pr.get("draft"):
            values.update(state="draft", detail="标记为 ready 后进入检查", queued=0)
        elif not old or head != old["head"] or base != old["base"] or old["state"] in {"draft", "closed"}:
            # A cancellation survives integration-base changes, but a new author commit is a new work version.
            if old and old["state"] == "cancelled" and head == old["head"]:
                pass
            else:
                values.update(state="pending", detail="版本更新，重新核对准入", queued=1)
        self.store.update(n, **values)

    def process_inbox(self):
        with self.store.lock:
            events = list(self.store.db.execute("SELECT * FROM inbox WHERE done=0 ORDER BY rowid"))
        for event in events:
            try:
                pr = self.github.pull(event["pr"])
                actor = self.config["users"].get(event["actor"], "")
                permitted = self.allowed(pr) and (event["action"] in {"查状态", "查看冲突"} or
                                                   actor.lower() == pr["user"]["login"].lower() or
                                                   event["actor"] in self.config["maintainer_ids"])
                if permitted:
                    self.reconcile(pr, self.github.base_sha())
                    if event["action"] == "取消排队" and pr["state"] == "open":
                        self.store.update(pr["number"], state="cancelled", detail="操作者已取消；同一 HEAD 不会自动重排", queued=0)
                    elif event["action"] in {"提交", "重试"} and pr["state"] == "open" and not pr.get("draft"):
                        self.store.update(pr["number"], state="pending", detail="已接收，等待后台检查", queued=1)
                    else:
                        with self.store.lock, self.store.db:
                            self.store.db.execute("UPDATE jobs SET revision=revision+1 WHERE pr=?", (pr["number"],))
                with self.store.lock, self.store.db:
                    self.store.db.execute("UPDATE inbox SET done=1 WHERE id=?", (event["id"],))
            except GitHubHTTPError as error:
                if error.status not in {400, 404, 422}:
                    break
                with self.store.lock, self.store.db:
                    self.store.db.execute("UPDATE inbox SET done=1 WHERE id=?", (event["id"],))
            except (RuntimeError, OSError):
                # Keep an unprocessed durable command for a transient API outage.
                break
            except (ValueError, KeyError, TypeError):
                with self.store.lock, self.store.db:
                    self.store.db.execute("UPDATE inbox SET done=1 WHERE id=?", (event["id"],))

    def approvals(self, pr):
        latest = {}
        for review in sorted(self.github.reviews(pr["number"]), key=lambda r: r["id"]):
            state = review.get("state")
            if state in {"APPROVED", "CHANGES_REQUESTED", "DISMISSED"}:
                latest[review["user"]["login"].lower()] = review
        if any(r.get("state") == "CHANGES_REQUESTED" for r in latest.values()):
            return False
        team = {u.lower() for u in self.config["users"].values()} - {pr["user"]["login"].lower()}
        count = sum(r.get("state") == "APPROVED" and r.get("commit_id") == pr["head"]["sha"]
                    for user, r in latest.items() if user in team)
        return count >= self.config["required_approvals"]

    def baseline(self, base):
        checks = self.github.checks(base)
        waiting = []
        for required in self.config["required_checks"]:
            matches = [check for check in checks if check.get("name") == required["name"] and
                       check.get("app", {}).get("id") == required["app_id"] and check.get("head_sha") == base]
            if not matches:
                waiting.append(required["name"])
                continue
            check = max(matches, key=lambda item: item["id"])
            if check.get("status") != "completed":
                waiting.append(required["name"])
            elif check.get("conclusion") in {"failure", "timed_out", "cancelled", "startup_failure", "action_required", "stale"}:
                return "failed", f"集成 {base[:12]} 检查失败：{required['name']}。队列已暂停；维护人检查 CI，用独立 revert PR 恢复，禁止 reset 共享历史。"
            elif check.get("conclusion") != "success":
                waiting.append(required["name"])
        if waiting:
            return "pending", f"等待集成 {base[:12]} 的成功 CI：" + ", ".join(waiting)
        return "ready", "集成基线必需检查成功"

    def evaluate(self, pr, base):
        n, head = pr["number"], pr["head"]["sha"]
        for dependency in dependencies(pr.get("body"), n):
            dep = self.github.pull(dependency)
            if dep["base"]["repo"]["full_name"].lower() != self.config["repository"].lower() or dep["base"]["ref"] != self.config["target_branch"]:
                return "failed", "依赖 PR 目标不符合配置"
            if not dep.get("merged"):
                return "waiting", f"等待依赖 PR #{dependency} 合入；关闭但未合入也不满足依赖"
        if pr.get("mergeable") is False:
            return "conflict", "GitHub 报告文本冲突；作者在个人分支同步集成并解决后重试"
        if pr.get("mergeable") is not True or not pr.get("merge_commit_sha"):
            return "pending", "GitHub 正在计算最新合并候选"
        if not self.approvals(pr):
            return "pending", "等待当前 HEAD 的非作者团队审批，或处理 changes requested"
        candidate = sha(pr["merge_commit_sha"])
        checks = self.github.checks(head)
        selected_checks = []
        for required in self.config["required_checks"]:
            matches = [check for check in checks if check.get("name") == required["name"] and
                       check.get("app", {}).get("id") == required["app_id"]]
            if not matches:
                return "pending", "缺少必需检查：" + required["name"]
            check = max(matches, key=lambda item: item["id"])
            selected_checks.append(check)
            if check.get("head_sha") != head or check.get("status") != "completed":
                return "checking", "检查中：" + required["name"]
            if check.get("conclusion") != "success":
                return "failed", "必需检查未成功：" + required["name"]
        if pr.get("mergeable_state") != "clean" or not self.github.protection_ready():
            return "pending", "等待 GitHub 分支保护、CODEOWNERS、讨论和必需检查满足"
        evidence = self.github.candidate_evidence(n, base, head, candidate)
        if not evidence:
            return "pending", "缺少绑定最新 base/head 的成功候选 CI 报告；需重新运行候选检查"
        for check in selected_checks:
            if check.get("name") == "Team integration candidate" and check.get("check_suite", {}).get("id") != evidence["check_suite_id"]:
                return "pending", "候选检查与证据不属于同一次 CI"
        current = self.github.pull(n)
        if (self.github.base_sha() != base or current["head"]["sha"] != head or current.get("merge_commit_sha") != candidate or
                not self.allowed(current) or current["state"] != "open" or current.get("draft") or
                current.get("mergeable_state") != "clean" or current.get("body") != pr.get("body")):
            return "pending", "验证期间 PR 或集成基线变化，旧结果失效"
        if not self.approvals(current):
            return "pending", "验证期间审批变化，重新核对"
        baseline_state, baseline_detail = self.baseline(base)
        if baseline_state != "ready":
            if baseline_state == "failed":
                self.store.pause(True, baseline_detail)
            return "pending", baseline_detail
        return "ready", "只读模拟通过；维护人须核对产品验收证据并按分支保护人工合入。CI：" + evidence["url"]

    def tick(self):
        self.process_inbox()
        try:
            base = self.github.base_sha()
            discovered = self.github.open_pulls()
        except (ValueError, KeyError, TypeError, RuntimeError, OSError):
            for job in self.store.jobs():
                if job["state"] in {"ready", "checking"}:
                    self.store.update(job["pr"], state="pending", detail="GitHub 当前不可核实；之前的准入结果暂时失效")
            self.flush_notifications()
            raise
        numbers = {number(p["number"]) for p in discovered}
        numbers.update(j["pr"] for j in self.store.jobs() if j["state"] not in {"closed", "merged"})
        for n in sorted(numbers):
            try:
                pr = self.github.pull(n)
                self.reconcile(pr, base)
            except (ValueError, KeyError, TypeError, RuntimeError, OSError):
                if self.store.job(n):
                    self.store.update(n, state="pending", detail="无法核实 GitHub 最新状态；暂停该项准入")
        try:
            baseline_state, baseline_detail = self.baseline(base)
        except (ValueError, KeyError, TypeError, RuntimeError, OSError):
            baseline_state, baseline_detail = "pending", "无法核实当前集成基线 CI，等待 API 恢复"
        if baseline_state == "failed":
            self.store.pause(True, baseline_detail)
        if baseline_state != "ready":
            for job in self.store.jobs():
                if job["queued"]:
                    self.store.update(job["pr"], state="pending", detail=baseline_detail)
                elif baseline_state == "failed" and job["state"] == "merged" and job["base"] == base:
                    self.store.update(job["pr"], detail=baseline_detail)
        if baseline_state == "ready" and not self.store.paused():
            for job in self.store.jobs():
                if self.store.paused():
                    break
                if not job["queued"] or job["state"] in {"failed", "conflict", "closed", "merged"}:
                    continue
                try:
                    pr = self.github.pull(job["pr"])
                    current_base = self.github.base_sha()
                    if current_base != base:
                        self.reconcile(pr, current_base)
                        self.store.update(job["pr"], state="pending", detail="集成基线刚更新，等待下一轮核对新基线 CI")
                        continue
                    self.reconcile(pr, base)
                    current = self.store.job(job["pr"])
                    if not current["queued"]:
                        continue
                    state, detail = self.evaluate(pr, base)
                    self.store.update(job["pr"], state=state, detail=detail)
                except (ValueError, KeyError, TypeError, RuntimeError, OSError):
                    self.store.update(job["pr"], state="pending", detail="API 或候选证据不可核实；保留队列等待恢复")
        self.flush_notifications()

    def flush_notifications(self):
        if self.notifier is None:
            return
        for job in self.store.jobs():
            if job["delivered"] == job["revision"] or time.time() < job["next_send"]:
                continue
            # Feishu's create uuid dedup window is finite (one hour). Beyond that,
            # an ambiguous send requires manual card-id recovery, never a duplicate card.
            if not job["card_id"] and job["create_started"] and time.time() - job["create_started"] > 3500:
                continue
            token = hashlib.sha256((self.config["repository"] + ":" + str(job["pr"])).encode()).hexdigest()[:32]
            try:
                if not job["card_id"] and not job["create_started"]:
                    with self.store.lock, self.store.db:
                        self.store.db.execute("UPDATE jobs SET create_started=? WHERE pr=?", (time.time(), job["pr"]))
                card_id = self.notifier.upsert(job["card_id"], token, card(self.config, job))
                if not isinstance(card_id, str) or not card_id:
                    raise ValueError("missing notification message id")
                with self.store.lock, self.store.db:
                    self.store.db.execute("UPDATE jobs SET card_id=?,delivered=?,failures=0,next_send=0 WHERE pr=?",
                                          (card_id, job["revision"], job["pr"]))
            except Exception:
                # Notification failure changes only the outbox, never queue state.
                with self.store.lock, self.store.db:
                    self.store.db.execute("UPDATE jobs SET failures=failures+1,next_send=? WHERE pr=?",
                                          (time.time() + min(300, 2 ** min(job["failures"] + 1, 8)), job["pr"]))


def card(config, job):
    content = (f"{job['title']}\n作者：{job['author']}\nPR HEAD：{job['head'][:12]} / 集成：{job['base'][:12]}\n"
               f"状态：{STATES[job['state']]}\n{job['detail']}")
    # Plain text keeps PR titles/body from injecting card markup, mentions, or links.
    return {"config": {"wide_screen_mode": True},
            "header": {"title": {"tag": "plain_text", "content": f"PR #{job['pr']} · {STATES[job['state']]}"}},
            "elements": [{"tag": "div", "text": {"tag": "plain_text", "content": content}},
                         {"tag": "action", "actions": [{"tag": "button", "text": {"tag": "plain_text", "content": "查看 PR"},
                              "url": f"https://github.com/{config['repository']}/pull/{job['pr']}"}]}]}
