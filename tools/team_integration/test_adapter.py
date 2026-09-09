"""SDK boundary and CLI tests use fake SDK builders and forbid network access."""
import contextlib
import io
import json
from pathlib import Path
import socket
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

from tools.team_integration.__main__ import instance_lock, main
from tools.team_integration.feishu import Feishu
from tools.team_integration.service import Service, Store
from tools.team_integration.test_service import FakeGitHub, FakeNotifier
from tools.team_integration.test_service import configuration


class Builder:
    def __init__(self, result=None):
        self.fields, self.result = {}, result

    def __getattr__(self, name):
        def set_field(value):
            self.fields[name] = value
            return self
        return set_field

    def build(self):
        return self.result if self.result is not None else SimpleNamespace(**self.fields)


class AdapterTests(unittest.TestCase):
    def setUp(self):
        self.network = patch.object(socket.socket, "connect", side_effect=AssertionError("network forbidden"))
        self.network.start()
        self.addCleanup(self.network.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.config = configuration(Path(self.temp.name) / "state.db")
        self.response = SimpleNamespace(success=lambda: True, data=SimpleNamespace(message_id="om_card"))
        self.messages = SimpleNamespace(create=Mock(return_value=self.response), patch=Mock(return_value=self.response))
        client = SimpleNamespace(im=SimpleNamespace(v1=SimpleNamespace(message=self.messages)))
        self.handler = Builder()
        self.ws = Mock()
        lark = SimpleNamespace(Client=SimpleNamespace(builder=lambda: Builder(client)),
                               EventDispatcherHandler=SimpleNamespace(builder=lambda *args: self.handler),
                               LogLevel=SimpleNamespace(ERROR=40), ws=SimpleNamespace(Client=self.ws))
        request_class = SimpleNamespace(builder=Builder)
        models = SimpleNamespace(**{name: request_class for name in
                                    ("CreateMessageRequest", "CreateMessageRequestBody", "PatchMessageRequest", "PatchMessageRequestBody")})
        with patch.dict("sys.modules", {"lark_oapi": lark, "lark_oapi.api.im.v1": models}):
            self.adapter = Feishu(self.config, "offline-app-id", "offline-app-secret")

    def test_create_uses_chat_interactive_uuid_and_patch_uses_same_id(self):
        self.assertEqual("om_card", self.adapter.upsert(None, "dedup-id", {"title": "卡片"}))
        request = self.messages.create.call_args.args[0]
        self.assertEqual("chat_id", request.receive_id_type)
        self.assertEqual("oc_team", request.request_body.receive_id)
        self.assertEqual("interactive", request.request_body.msg_type)
        self.assertEqual("dedup-id", request.request_body.uuid)
        self.assertEqual("om_card", self.adapter.upsert("om_card", "dedup-id", {"title": "updated"}))
        self.assertEqual("om_card", self.messages.patch.call_args.args[0].message_id)

    def test_long_connection_callback_uses_sdk_sender_id_and_persists_command(self):
        store = Store(self.config["database_path"], self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.addCleanup(store.close)
        self.adapter.listen(store)
        callback = self.handler.fields["register_p2_im_message_receive_v1"]
        sender = SimpleNamespace(sender_type="user", sender_id=SimpleNamespace(open_id="ou_model"))
        message = SimpleNamespace(chat_id="oc_team", message_type="text", message_id="om_event",
                                  content=json.dumps({"text": "@_user_1 提交 PR #42"}),
                                  mentions=[SimpleNamespace(key="@_user_1", id=SimpleNamespace(open_id="ou_bot"))])
        callback(SimpleNamespace(event=SimpleNamespace(sender=sender, message=message)))
        row = store.db.execute("SELECT actor,pr FROM inbox").fetchone()
        self.assertEqual(("ou_model", 42), tuple(row))
        self.ws.return_value.start.assert_called_once()

    def test_validate_is_offline_and_does_not_create_state(self):
        config_path = Path(self.temp.name) / "config.json"
        config_path.write_text(json.dumps(self.config), encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, main(["--config", str(config_path), "validate"]))
        self.assertFalse(Path(self.config["database_path"]).exists())

    def test_instance_lock_prevents_a_second_worker_and_recovers(self):
        path = Path(self.temp.name) / "state.db"
        with instance_lock(path):
            with self.assertRaises(RuntimeError):
                with instance_lock(path):
                    self.fail("second worker acquired an owned lock")
        with instance_lock(path):
            pass

    def test_confirmed_absent_card_can_recover_permanent_create_failure(self):
        config_path = Path(self.temp.name) / "config.json"
        config_path.write_text(json.dumps(self.config), encoding="utf-8")
        store = Store(self.config["database_path"], self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.addCleanup(store.close)
        store.update(42, title="Permission failure", state="pending")
        with store.db:
            store.db.execute("UPDATE jobs SET create_started=1,failures=20,next_send=9999999999 WHERE pr=42")
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            main(["--config", str(config_path), "reset-card-create", "42"])
        self.assertEqual(1, store.job(42)["create_started"])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(0, main(["--config", str(config_path), "reset-card-create", "42", "--confirmed-not-created"]))
        self.assertEqual(0, store.job(42)["create_started"])
        notifier = FakeNotifier()
        service = Service(self.config, store, FakeGitHub(self.config), notifier)
        service.flush_notifications()
        self.assertEqual("om_card", store.job(42)["card_id"])
        with self.assertRaisesRegex(ValueError, "no bound Feishu card"):
            main(["--config", str(config_path), "reset-card-create", "42", "--confirmed-not-created"])

    def test_card_creation_reset_cannot_race_a_running_worker(self):
        config_path = Path(self.temp.name) / "config.json"
        config_path.write_text(json.dumps(self.config), encoding="utf-8")
        store = Store(self.config["database_path"], self.config["repository"], self.config["chat_id"], self.config["bot_open_id"])
        self.addCleanup(store.close)
        store.update(42, title="An attempted card")
        with instance_lock(self.config["database_path"]):
            with self.assertRaises(RuntimeError):
                main(["--config", str(config_path), "reset-card-create", "42", "--confirmed-not-created"])


if __name__ == "__main__":
    unittest.main()
