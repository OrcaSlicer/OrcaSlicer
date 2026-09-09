"""Explicit local CLI; importing this package never connects to any service."""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import sys
import threading

from .config import load_config
from .github import GitHub, HTTPTransport
from .service import Service, Store


@contextmanager
def instance_lock(path):
    """One service worker per state database; OS releases the lock on crashes."""
    with open(str(path) + ".lock", "a+b") as handle:
        if os.fstat(handle.fileno()).st_size == 0:
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            raise RuntimeError("another service owns this database") from None
        try:
            yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle, fcntl.LOCK_UN)


def main(argv=None):
    parser = argparse.ArgumentParser(description="ADR-007 phase1: read-only GitHub checks; merges remain manual")
    parser.add_argument("--config", required=True, help="private JSON config outside the checkout")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("validate", help="validate configuration without network or state writes")
    for command in ("status", "pause", "resume"):
        sub.add_parser(command)
    recovery = sub.add_parser("recover-card", help="bind a confirmed existing Feishu card after an ambiguous create")
    recovery.add_argument("pr", type=int)
    recovery.add_argument("message_id")
    reset = sub.add_parser("reset-card-create", help="retry first card creation only after confirming no card was created")
    reset.add_argument("pr", type=int)
    reset.add_argument("--confirmed-not-created", action="store_true", required=True,
                       help="operator has checked the team chat and confirmed this PR has no existing card")
    run = sub.add_parser("run", help="poll GitHub; use a dedicated service host")
    run.add_argument("--once", action="store_true")
    run.add_argument("--listen-feishu", action="store_true", help="connect the official SDK message event stream")
    run.add_argument("--send-feishu", action="store_true", help="send/update cards in the configured team chat")
    args = parser.parse_args(argv)
    config = load_config(args.config)
    if args.command == "validate":
        print("configuration valid; phase1 only; no network accessed")
        return 0
    path = Path(config["database_path"])
    path.parent.mkdir(parents=True, exist_ok=True)
    store = Store(path, config["repository"], config["chat_id"], config["bot_open_id"])
    try:
        if args.command in {"pause", "resume"}:
            store.pause(args.command == "pause")
            print("queue paused" if store.paused() else "queue resumed")
        elif args.command == "status":
            print(json.dumps({"paused": store.paused(), "pause_reason": store.pause_reason(), "jobs": store.jobs()}, ensure_ascii=False, indent=2))
        elif args.command == "recover-card":
            if not store.job(args.pr) or not re.fullmatch(r"om_[A-Za-z0-9_-]+", args.message_id):
                raise ValueError("provide an existing PR and confirmed Feishu om_ message ID")
            with instance_lock(path), store.lock, store.db:
                store.db.execute("UPDATE jobs SET card_id=?,next_send=0 WHERE pr=?", (args.message_id, args.pr))
        elif args.command == "reset-card-create":
            job = store.job(args.pr)
            if not job or job["card_id"]:
                raise ValueError("reset requires an existing PR with no bound Feishu card")
            # Stop the worker while changing creation recovery state. This prevents
            # an in-flight create from racing the operator's confirmed absence.
            with instance_lock(path), store.lock, store.db:
                store.db.execute("UPDATE jobs SET create_started=0,next_send=0,failures=0 WHERE pr=?", (args.pr,))
            print("card creation retry enabled after operator confirmed no card was created")
        else:
            token = os.environ.get(config["github_token_env"], "")
            if not token:
                raise ValueError("missing environment variable: " + config["github_token_env"])
            feishu = None
            if args.listen_feishu or args.send_feishu:
                load_config(args.config, require_secrets=True)
                from .feishu import Feishu
                feishu = Feishu(config, os.environ[config["feishu_app_id_env"]], os.environ[config["feishu_app_secret_env"]])
            if args.once and args.listen_feishu:
                raise ValueError("--once and --listen-feishu cannot be combined")
            service = Service(config, store, GitHub(config, HTTPTransport(token)), feishu if args.send_feishu else None)
            stop = threading.Event()
            failures = []

            def worker():
                while not stop.is_set():
                    try:
                        service.tick()
                    except Exception as exc:
                        # Never log credentials, request URLs, message text, or SDK payloads.
                        print("poll failed; queue retained: " + type(exc).__name__, file=sys.stderr)
                        failures.append(type(exc).__name__)
                        service.flush_notifications()
                    if args.once or stop.wait(config["poll_seconds"]):
                        return

            with instance_lock(path):
                if args.listen_feishu:
                    thread = threading.Thread(target=worker, name="team-integration-worker", daemon=True)
                    thread.start()
                    try:
                        feishu.listen(store)
                    finally:
                        stop.set()
                        thread.join()
                else:
                    worker()
            if args.once and failures:
                return 1
    finally:
        store.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
    except (ValueError, RuntimeError, OSError, ImportError) as error:
        print("team integration: " + str(error), file=sys.stderr)
        sys.exit(2)
