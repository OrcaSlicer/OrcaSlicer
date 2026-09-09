"""Optional official lark-oapi SDK adapter; imported only for live Feishu mode."""
import json

from .service import accept_message


class Feishu:
    def __init__(self, config, app_id, app_secret):
        import lark_oapi as lark
        from lark_oapi.api.im.v1 import (CreateMessageRequest, CreateMessageRequestBody,
                                        PatchMessageRequest, PatchMessageRequestBody)
        self.lark, self.config = lark, config
        self.app_id, self.app_secret = app_id, app_secret
        self.create_request, self.create_body = CreateMessageRequest, CreateMessageRequestBody
        self.patch_request, self.patch_body = PatchMessageRequest, PatchMessageRequestBody
        self.client = lark.Client.builder().app_id(app_id).app_secret(app_secret).log_level(lark.LogLevel.ERROR).build()

    def upsert(self, message_id, dedup_id, card):
        content = json.dumps(card, ensure_ascii=False)
        if message_id:
            request = self.patch_request.builder().message_id(message_id).request_body(
                self.patch_body.builder().content(content).build()).build()
            response = self.client.im.v1.message.patch(request)
        else:
            request = self.create_request.builder().receive_id_type("chat_id").request_body(
                self.create_body.builder().receive_id(self.config["chat_id"]).msg_type("interactive")
                .content(content).uuid(dedup_id).build()).build()
            response = self.client.im.v1.message.create(request)
        if not response.success():
            # SDK messages may contain user payloads; log only the numeric code.
            raise RuntimeError("Feishu API code " + str(response.code))
        return message_id or response.data.message_id

    def listen(self, store):
        def on_message(data):
            try:
                sender, message = data.event.sender, data.event.message
                event = {"chat_id": message.chat_id, "actor": sender.sender_id.open_id,
                         "sender_type": sender.sender_type, "message_type": message.message_type,
                         "message_id": message.message_id,
                         "text": json.loads(message.content).get("text", ""),
                         "mentions": [{"key": m.key, "open_id": m.id.open_id} for m in (message.mentions or [])]}
                accept_message(self.config, store, event)
            except (AttributeError, TypeError, ValueError):
                return
        handler = self.lark.EventDispatcherHandler.builder("", "").register_p2_im_message_receive_v1(on_message).build()
        self.lark.ws.Client(self.app_id, self.app_secret, event_handler=handler,
                            log_level=self.lark.LogLevel.ERROR).start()
