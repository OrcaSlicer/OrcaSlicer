# /// script
# requires-python = ">=3.10"
# dependencies = ["paho-mqtt"]
#
# [tool.orcaslicer.plugin]
# name = "BBL Printer Agent"
# description = "Barebones Bambu Lab printer-agent plugin."
# author = "OrcaSlicer"
# version = "0.0.1"
# type = "printer-connection"
# ///
import json
import ftplib
import queue
import select
import socket
import ssl
import sys
import threading
import uuid
from pathlib import Path

import orca

from typing import Any, Callable

try:
    import paho.mqtt.client as mqtt
except ModuleNotFoundError:
    mqtt = None


def _log(msg):
    print(f"[BBLPrinterAgentPlugin] {msg}", file=sys.stderr, flush=True)


MQTT_CONNECT_TIMEOUT_SECONDS = 10
MQTT_KEEPALIVE_SECONDS = 60
MQTT_PORT = 1883
MQTT_SSL_PORT = 8883
FTP_SSL_PORT = 990
SSDP_GROUP = "239.255.255.250"
SSDP_PORTS = (1990, 2021)
SSDP_RECV_SIZE = 8192
BAMBU_DEFAULT_USERNAME = "bblp"
MQTT_CONNACK_MESSAGES = {
    0: "accepted",
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "bad username or password",
    5: "not authorized",
}
CONNECT_STATUS_OK = 0
CONNECT_STATUS_FAILED = 1
CONNECT_STATUS_LOST = 2
BAMBU_NETWORK_ERR_INVALID_HANDLE = -1
BAMBU_NETWORK_ERR_SEND_MSG_FAILED = -4
BAMBU_NETWORK_ERR_BIND_FAILED = -5
BAMBU_NETWORK_ERR_UNBIND_FAILED = -6
BAMBU_NETWORK_ERR_FILE_NOT_EXIST = -14
BAMBU_NETWORK_ERR_CANCELED = -18
BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED = -2130
BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED = -4020
BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED = -4030
BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED = -5010
PRINTING_STAGE_CREATE = 0
PRINTING_STAGE_UPLOAD = 1
PRINTING_STAGE_SENDING = 3
PRINTING_STAGE_FINISHED = 6
PRINTING_STAGE_ERROR = 7
INITIAL_PUSH_PAYLOAD = {
    "pushing": {"command": "pushall"},
    "info": {"command": "get_version"},
    "upgrade": {"command": "get_history"},
}


class _ImplicitFTP_TLS(ftplib.FTP_TLS):
    def __init__(self, *args, unwrap: bool = False, **kwargs):
        super().__init__(*args, **kwargs)
        self._sock = None
        self.unwrap = unwrap

    def ntransfercmd(self, cmd, rest=None):
        conn, size = ftplib.FTP.ntransfercmd(self, cmd, rest)
        if self._prot_p:
            conn = self.context.wrap_socket(conn, server_hostname=self.host, session=self.sock.session)
        return conn, size

    @property
    def sock(self):
        return self._sock

    @sock.setter
    def sock(self, value):
        if value is not None and not isinstance(value, ssl.SSLSocket):
            value = self.context.wrap_socket(value)
        self._sock = value

    def storbinary(self, cmd, fp, blocksize=8192, callback=None, rest=None):
        self.voidcmd("TYPE I")
        conn = self.transfercmd(cmd, rest)
        try:
            while True:
                buf = fp.read(blocksize)
                if not buf:
                    break
                conn.sendall(buf)
                if callback:
                    callback(buf)
            if isinstance(conn, ssl.SSLSocket) and self.unwrap:
                conn.unwrap()
        finally:
            conn.close()

        # Bambu's FTPS server may leave the control channel without the usual
        # final 226 response after the data socket has been closed. At this
        # point transfercmd/sendall has already succeeded, so accept the known
        # non-standard completion forms instead of reporting a false upload
        # failure to the UI.
        old_timeout = self.sock.gettimeout() if self.sock else None
        try:
            if self.sock:
                self.sock.settimeout(2)
            return self.voidresp()
        except TimeoutError:
            _log(f"FTP upload completed for {cmd}; timed out waiting for final response")
            return "226 Transfer complete"
        except ftplib.error_reply as exc:
            if str(exc).startswith("200"):
                _log(f"FTP upload completed for {cmd}; accepted non-standard response: {exc}")
                return str(exc)
            raise
        finally:
            if self.sock and old_timeout is not None:
                self.sock.settimeout(old_timeout)


class BBLPrinterAgentPlugin(orca.printer_agent.PrinterAgentBase):
    def on_load(self):
        self.on_server_err_fn: Callable[..., None] | None = None
        self.on_printer_connected_fn: Callable[..., None] | None = None
        self.on_subscribe_failure_fn: Callable[..., None] | None = None
        self.on_message_fn: Callable[..., None] | None = None
        self.on_user_message_fn: Callable[..., None] | None = None
        self.on_local_connect_fn: Callable[..., None] | None = None
        self.on_local_message_fn: Callable[..., None] | None = None
        self.on_ssdp_msg_fn: Callable[..., None] | None = None
        self.queue_on_main_fn: Callable[..., None] | None = None
        self._selected_machine = ""
        self._mqtt_lock = threading.RLock()
        self._mqtt_client = None
        self._mqtt_connected = threading.Event()
        self._mqtt_dev_id = ""
        self._mqtt_host = ""
        self._mqtt_username = BAMBU_DEFAULT_USERNAME
        self._mqtt_password = ""
        self._mqtt_command_topic = ""
        self._mqtt_manual_disconnect = False
        self._printer_data: dict[str, Any] = {}
        self._last_error = ""
        self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
        self._sequence_id = 10000000
        self._mqtt_command_queue = queue.Queue()
        self._mqtt_command_worker_stop = threading.Event()
        self._mqtt_command_worker = threading.Thread(target=self._mqtt_command_loop, daemon=True)
        self._mqtt_command_worker.start()
        self._ssdp_stop = threading.Event()
        self._ssdp_thread = None
        self._ssdp_sockets = []
        self._ssdp_seen = {}

    def get_name(self):
        return "BBL Printer Agent"

    def set_server_callback(self, on_server_err_fn):
        self.on_server_err_fn = on_server_err_fn
        return 0

    def set_on_printer_connected_fn(self, on_printer_connected_fn):
        self.on_printer_connected_fn = on_printer_connected_fn
        return 0

    def set_on_subscribe_failure_fn(self, on_subscribe_failure_fn):
        self.on_subscribe_failure_fn = on_subscribe_failure_fn
        return 0

    def set_on_message_fn(self, on_message_fn):
        self.on_message_fn = on_message_fn
        return 0

    def set_on_user_message_fn(self, on_user_message_fn):
        self.on_user_message_fn = on_user_message_fn
        return 0

    def set_on_local_connect_fn(self, on_connect_fn):
        self.on_local_connect_fn = on_connect_fn
        return 0

    def set_on_local_message_fn(self, on_message_fn):
        self.on_local_message_fn = on_message_fn
        return 0

    def set_on_ssdp_msg_fn(self, on_ssdp_msg_fn):
        self.on_ssdp_msg_fn = on_ssdp_msg_fn
        return 0

    def set_queue_on_main_fn(self, queue_on_main_fn):
        self.queue_on_main_fn = queue_on_main_fn
        return 0

    def connect_printer(self, dev_id, dev_ip, username, password, use_ssl) -> int:
        self._ensure_state()
        dev_id = str(dev_id or "").strip()
        dev_ip = str(dev_ip or "").strip()
        username = str(username or BAMBU_DEFAULT_USERNAME).strip()
        password = str(password or "").strip()
        _log(
            "connect_printer requested "
            f"dev_id={dev_id or '<missing>'} host={dev_ip or '<missing>'} "
            f"username={username or BAMBU_DEFAULT_USERNAME} ssl={bool(use_ssl)} "
            f"password_length={len(password)}"
        )

        if not dev_ip:
            return self._connection_failed(dev_id, "Missing printer IP address")
        if mqtt is None:
            return self._connection_failed(dev_id, "paho-mqtt is required for MQTT printer connections")
        if not dev_id:
            return self._connection_failed(dev_id, "Missing printer serial/device id")
        if not password:
            return self._connection_failed(dev_id, "Missing printer access code/password")

        port = MQTT_SSL_PORT if use_ssl else MQTT_PORT
        connected = threading.Event()
        connect_result = {"rc": None}
        client = self._create_mqtt_client(dev_id)
        command_topic = f"device/{dev_id}/request"
        report_topic = f"device/{dev_id}/report"

        def on_connect(client, userdata, flags, rc, *extra):
            connect_result["rc"] = self._mqtt_result_code(rc)
            try:
                _log(
                    f"MQTT on_connect dev_id={dev_id} rc={connect_result['rc']} "
                    f"message={MQTT_CONNACK_MESSAGES.get(connect_result['rc'], 'unknown')}"
                )
                if connect_result["rc"] == 0:
                    self._mqtt_connected.set()
                    client.subscribe(report_topic)
                    client.publish(command_topic, json.dumps(INITIAL_PUSH_PAYLOAD))
                    _log(f"MQTT subscribed dev_id={dev_id} topic={report_topic}; requested initial push")
            finally:
                connected.set()

        def on_disconnect(client, userdata, *args):
            reason = self._disconnect_reason(args)
            with self._mqtt_lock:
                is_current_client = client is self._mqtt_client
                if is_current_client:
                    self._mqtt_connected.clear()
                    current_dev_id = self._mqtt_dev_id
                    manual_disconnect = self._mqtt_manual_disconnect
                else:
                    current_dev_id = ""
                    manual_disconnect = True
            _log(f"MQTT on_disconnect dev_id={current_dev_id or dev_id} reason={reason} manual={manual_disconnect}")
            if self.on_local_connect_fn and current_dev_id and not manual_disconnect:
                self.on_local_connect_fn(CONNECT_STATUS_LOST, current_dev_id, f"MQTT disconnected: {reason}")

        def on_message(client, userdata, message):
            with self._mqtt_lock:
                if client is not self._mqtt_client:
                    return
            payload = message.payload.decode("utf-8", errors="replace")
            self._handle_message_payload(payload)
            if self.on_local_message_fn:
                self.on_local_message_fn(dev_id, payload)

        def on_log(client, userdata, level, buf):
            text = str(buf)
            if any(token in text.lower() for token in ("connect", "ssl", "tls", "fail", "error", "refused")):
                _log(f"MQTT paho log dev_id={dev_id} level={level}: {text}")

        client.on_connect = on_connect
        client.on_disconnect = on_disconnect
        client.on_message = on_message
        client.on_log = on_log

        username = username or BAMBU_DEFAULT_USERNAME
        client.username_pw_set(username, password)
        if use_ssl:
            client.tls_set(tls_version=ssl.PROTOCOL_TLS, cert_reqs=ssl.CERT_NONE)
            client.tls_insecure_set(True)

        with self._mqtt_lock:
            old_client = self._detach_mqtt_locked()
        if old_client is not None:
            _log("connect_printer replacing existing MQTT client")
        self._stop_mqtt_client(old_client)

        with self._mqtt_lock:
            self._mqtt_client = client
            self._mqtt_dev_id = dev_id
            self._mqtt_host = dev_ip
            self._mqtt_username = username
            self._mqtt_password = password
            self._mqtt_command_topic = command_topic
            self._mqtt_manual_disconnect = False
            self._printer_data = {}
            self._last_error = ""
            self._selected_machine = dev_id

        try:
            _log(f"MQTT connecting dev_id={dev_id} host={dev_ip} port={port}")
            client.connect_async(dev_ip, port, keepalive=MQTT_KEEPALIVE_SECONDS)
            client.loop_start()
            _log(f"MQTT loop started dev_id={dev_id}; waiting up to {MQTT_CONNECT_TIMEOUT_SECONDS}s for CONNACK")
        except Exception as exc:
            client_to_stop = None
            with self._mqtt_lock:
                if self._mqtt_client is client:
                    client_to_stop = self._detach_mqtt_locked()
            self._stop_mqtt_client(client_to_stop)
            return self._connection_failed(dev_id, str(exc))

        if not connected.wait(MQTT_CONNECT_TIMEOUT_SECONDS):
            client_to_stop = None
            with self._mqtt_lock:
                if self._mqtt_client is client:
                    client_to_stop = self._detach_mqtt_locked()
            self._stop_mqtt_client(client_to_stop)
            return self._connection_failed(dev_id, f"Timed out connecting to MQTT broker at {dev_ip}:{port}")

        with self._mqtt_lock:
            is_current_client = client is self._mqtt_client
        if not is_current_client:
            return self._connection_failed(dev_id, "MQTT client was replaced before connection completed")

        rc = connect_result.get("rc")
        if rc == 0:
            if self.on_local_connect_fn:
                self.on_local_connect_fn(CONNECT_STATUS_OK, dev_id, "0")
            if self.on_printer_connected_fn:
                self.on_printer_connected_fn(report_topic)
            _log(f"connect_printer succeeded dev_id={dev_id} host={dev_ip} port={port}")
            return 0

        reason = MQTT_CONNACK_MESSAGES.get(rc, "unknown")
        message = (
            f"MQTT broker rejected connection with code {rc} ({reason}); "
            f"check LAN access code, username={username}, ssl={bool(use_ssl)}"
        )
        client_to_stop = None
        with self._mqtt_lock:
            if self._mqtt_client is client:
                client_to_stop = self._detach_mqtt_locked()
        self._stop_mqtt_client(client_to_stop)
        return self._connection_failed(dev_id, message, callback_message=str(rc))

    def disconnect_printer(self) -> int:
        self._ensure_state()
        with self._mqtt_lock:
            client = self._detach_mqtt_locked()
        self._stop_mqtt_client(client)
        return 0

    def start_discovery(self, start=True, sending=False) -> bool:
        self._ensure_state()
        if start:
            if self._ssdp_thread and self._ssdp_thread.is_alive():
                _log("SSDP discovery already running")
                return True
            self._ssdp_stop.clear()
            self._ssdp_seen = {}
            _log(f"Starting SSDP discovery sending={bool(sending)} ports={SSDP_PORTS}")
            self._ssdp_thread = threading.Thread(
                target=self._ssdp_loop,
                args=(bool(sending),),
                daemon=True,
            )
            self._ssdp_thread.start()
            return True

        if not self._ssdp_sockets and not (self._ssdp_thread and self._ssdp_thread.is_alive()):
            return True

        _log("Stopping SSDP discovery")
        self._ssdp_stop.set()
        for sock in list(self._ssdp_sockets):
            try:
                sock.close()
            except OSError:
                pass
        self._ssdp_sockets = []
        return True

    def install_device_cert(self, dev_id, lan_only=True):
        return

    def get_agent_info(self):
        return orca.printer_agent.AgentInfo(
            "pybbl",
            "Bambu Lab",
            "0.0.1",
            "Barebones Bambu Lab printer-agent plugin",
        )

    def get_user_selected_machine(self):
        self._ensure_state()
        return self._selected_machine

    def set_user_selected_machine(self, dev_id):
        self._ensure_state()
        self._selected_machine = str(dev_id or "")
        return 0

    def send_message(self, dev_id, json_str, qos=0, flag=0):
        return self.send_message_to_printer(dev_id, json_str, qos, flag)

    def send_message_to_printer(self, dev_id, json_str, qos=0, flag=0):
        self._ensure_state()
        del dev_id, flag
        if self._publish_payload(json_str, qos=max(0, min(2, self._int_or_default(qos, 0)))):
            return 0
        self._last_code = BAMBU_NETWORK_ERR_SEND_MSG_FAILED
        return self._last_code

    def check_cert(self):
        return 0

    def ping_bind(self, ping_code):
        self._last_error = "Printer binding is not supported by this plugin"
        self._last_code = BAMBU_NETWORK_ERR_BIND_FAILED
        return self._last_code

    def bind_detect(self, dev_ip, sec_link, detect):
        detect.result_msg = "Printer binding is not supported by this plugin"
        detect.command = "bind_detect"
        detect.dev_id = ""
        detect.model_id = ""
        detect.dev_name = ""
        detect.version = ""
        detect.bind_state = "unsupported"
        detect.connect_type = "lan"
        self._last_error = detect.result_msg
        self._last_code = BAMBU_NETWORK_ERR_BIND_FAILED
        return self._last_code

    def bind(self, dev_ip, dev_id, sec_link, timezone, improved, update_fn):
        del dev_ip, dev_id, sec_link, timezone, improved
        self._last_error = "Printer binding is not supported by this plugin"
        self._last_code = BAMBU_NETWORK_ERR_BIND_FAILED
        self._update_progress(update_fn, PRINTING_STAGE_ERROR, self._last_code, self._last_error)
        return self._last_code

    def unbind(self, dev_id):
        del dev_id
        self._last_error = "Printer unbinding is not supported by this plugin"
        self._last_code = BAMBU_NETWORK_ERR_UNBIND_FAILED
        return self._last_code

    def request_bind_ticket(self):
        return (BAMBU_NETWORK_ERR_BIND_FAILED, "")

    def start_print(self, params=None, update_fn=None, cancel_fn=None, wait_fn=None):
        del wait_fn
        return self._start_local_print(params, BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED, update_fn, cancel_fn)

    def start_local_print(self, params=None, update_fn=None, cancel_fn=None):
        return self._start_local_print(params, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, update_fn, cancel_fn)

    def start_local_print_with_record(self, params=None, update_fn=None, cancel_fn=None, wait_fn=None):
        del wait_fn
        return self._start_local_print(params, BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED, update_fn, cancel_fn)

    def start_sdcard_print(self, params=None, update_fn=None, cancel_fn=None):
        self._ensure_state()
        del update_fn
        if self._cancel_requested(cancel_fn):
            self._last_error = "Cancelled"
            self._last_code = BAMBU_NETWORK_ERR_CANCELED
            return self._last_code
        if params is None:
            self._last_error = "Missing print params"
            self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
            return self._last_code
        try:
            payload = self._build_sdcard_project_file_payload(params)
        except Exception as exc:
            self._last_error = str(exc)
            self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
            return self._last_code
        if self._queue_payload(payload):
            return 0
        self._last_code = BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED
        return self._last_code

    def start_send_gcode_to_sdcard(self, params=None, update_fn=None, cancel_fn=None, wait_fn=None):
        self._ensure_state()
        del wait_fn
        if params is None:
            self._last_error = "Missing print params"
            self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
            return self._last_code
        if self._upload_print_file(params, update_fn, cancel_fn):
            return 0
        return self._last_code or BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED

    def get_filament_sync_mode(self):
        return orca.printer_agent.FilamentSyncMode.Subscription

    def fetch_filament_info(self, dev_id):
        del dev_id
        return self._publish_payload({"pushing": {"command": "pushall"}}, require_connected=False)

    def send_gcode(self, gcode, sequence_id=None):
        self._ensure_state()
        gcode = str(gcode or "").strip()
        if not gcode:
            self._last_error = "Missing G-code command"
            self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
            return self._last_code
        payload = {
            "print": {
                "command": "gcode_line",
                "param": gcode,
                "sequence_id": str(sequence_id or self._next_sequence_id()),
            }
        }
        if self._queue_payload(payload):
            return 0
        self._last_code = BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED
        return self._last_code

    def send_command(self, request_json):
        return self._send_command_impl(request_json, None, None)

    def send_command_with_progress(self, request_json, update_fn, cancel_fn):
        return self._send_command_impl(request_json, update_fn, cancel_fn)

    def _send_command_impl(self, request_json, update_fn=None, cancel_fn=None):
        try:
            request = json.loads(request_json)
        except json.JSONDecodeError:
            return json.dumps({
                "status": "error",
                "message": "Invalid printer-agent request",
            })

        command = request.get("command")
        payload = request.get("payload") or {}
        if not isinstance(payload, dict):
            return json.dumps({
                "status": "error",
                "message": "Invalid printer-agent payload",
            })

        if command == "connect_printer":
            dev_id = request.get("dev_id") or payload.get("dev_id") or ""
            result = self.connect_printer(
                dev_id,
                payload.get("dev_ip") or "",
                payload.get("username") or "",
                payload.get("password") or "",
                self._as_bool(payload.get("use_ssl")),
            )
            if result == 0:
                return json.dumps({"status": "ok"})
            return json.dumps({"status": "error", "message": self._last_error or "MQTT connection failed"})

        if command == "disconnect_printer":
            self.disconnect_printer()
            return json.dumps({"status": "ok"})

        if command == "get_filament_info":
            self._queue_payload({"pushing": {"command": "pushall"}}, require_connected=False)
            return json.dumps(self._build_filament_info())

        if command in ("send_message", "send_message_to_printer"):
            outbound = payload.get("message") if "message" in payload else payload
            qos = self._int_or_default(payload.get("qos"), 0)
            qos = max(0, min(2, qos))
            return self._status_response(self._queue_payload(outbound, qos=qos))

        if command == "start_local_print_with_record":
            return self._status_response(
                self._start_local_print(payload, BAMBU_NETWORK_ERR_PRINT_WR_UPLOAD_FTP_FAILED, update_fn, cancel_fn)
            )

        if command == "start_local_print":
            return self._status_response(
                self._start_local_print(payload, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, update_fn, cancel_fn)
            )

        if command == "start_send_gcode_to_sdcard":
            result = 0 if self._upload_print_file(payload, update_fn, cancel_fn) else self._last_code
            if result is None:
                result = BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED
            return self._status_response(result)

        if command == "start_sdcard_print":
            return self._status_response(self.start_sdcard_print(payload))

        if command == "send_gcode":
            gcode = payload.get("gcode") or payload.get("param") or payload.get("message")
            return self._status_response(self.send_gcode(gcode, payload.get("sequence_id")))

        if self._looks_like_bambu_payload(request):
            return self._status_response(self._queue_payload(request, qos=0))

        return json.dumps({
            "status": "error",
            "message": f"Unsupported printer-agent command: {command or '<missing>'}",
        })

    def on_unload(self):
        self.start_discovery(False, False)
        self._mqtt_command_worker_stop.set()
        self.disconnect_printer()
        if self._mqtt_command_worker and self._mqtt_command_worker.is_alive():
            self._mqtt_command_worker.join(timeout=1.0)
        self.on_server_err_fn: Callable[..., None] | None = None
        self.on_printer_connected_fn: Callable[..., None] | None = None
        self.on_subscribe_failure_fn: Callable[..., None] | None = None
        self.on_message_fn: Callable[..., None] | None = None
        self.on_user_message_fn: Callable[..., None] | None = None
        self.on_local_connect_fn: Callable[..., None] | None = None
        self.on_local_message_fn: Callable[..., None] | None = None
        self.on_ssdp_msg_fn: Callable[..., None] | None = None
        self.queue_on_main_fn: Callable[..., None] | None = None

    def _ensure_state(self):
        if hasattr(self, "_mqtt_lock"):
            return
        self.on_load()

    def _create_mqtt_client(self, dev_id):
        client_id = f"OrcaSlicer-{uuid.uuid4().hex[:12]}"
        _log(f"MQTT creating client dev_id={dev_id or '<missing>'} client_id={client_id}")
        kwargs = {
            "client_id": client_id,
            "clean_session": True,
            "protocol": mqtt.MQTTv311,
        }
        callback_api_version = getattr(mqtt, "CallbackAPIVersion", None)
        if callback_api_version is not None:
            kwargs["callback_api_version"] = callback_api_version.VERSION1
        return mqtt.Client(**kwargs)

    def _detach_mqtt_locked(self):
        client = self._mqtt_client
        self._mqtt_manual_disconnect = True
        self._mqtt_client = None
        self._mqtt_dev_id = ""
        self._mqtt_host = ""
        self._mqtt_command_topic = ""
        self._mqtt_connected.clear()
        return client

    def _stop_mqtt_client(self, client):
        if client is None:
            return
        try:
            client.disconnect()
        except Exception:
            pass
        try:
            client.loop_stop()
        except Exception:
            pass

    def _stop_mqtt_client_async(self, client):
        if client is None:
            return
        threading.Thread(target=self._stop_mqtt_client, args=(client,), daemon=True).start()

    def _ssdp_loop(self, sending=False):
        sockets = []
        try:
            for port in SSDP_PORTS:
                try:
                    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                    if hasattr(socket, "SO_REUSEPORT"):
                        try:
                            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
                        except OSError:
                            pass
                    sock.bind(("", port))
                    membership = socket.inet_aton(SSDP_GROUP) + socket.inet_aton("0.0.0.0")
                    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
                    sockets.append(sock)
                    _log(f"SSDP listening on {SSDP_GROUP}:{port}")
                except OSError as exc:
                    self._last_error = f"SSDP listen failed on port {port}: {exc}"
                    _log(self._last_error)

            self._ssdp_sockets = sockets
            if not sockets:
                _log("SSDP discovery has no listening sockets")
                return

            if sending:
                _log("Sending SSDP M-SEARCH")
                for port in SSDP_PORTS:
                    request = (
                        "M-SEARCH * HTTP/1.1\r\n"
                        f"HOST: {SSDP_GROUP}:{port}\r\n"
                        'MAN: "ssdp:discover"\r\n'
                        "MX: 1\r\n"
                        "ST: ssdp:all\r\n"
                        "\r\n"
                    ).encode("utf-8")
                    for sock in sockets:
                        try:
                            sock.sendto(request, (SSDP_GROUP, port))
                        except OSError as exc:
                            _log(f"SSDP M-SEARCH send failed port={port}: {exc}")

            while not self._ssdp_stop.is_set():
                try:
                    readable, _, _ = select.select(sockets, [], [], 1.0)
                except OSError as exc:
                    _log(f"SSDP select failed: {exc}")
                    break

                for sock in readable:
                    try:
                        packet, addr = sock.recvfrom(SSDP_RECV_SIZE)
                    except OSError as exc:
                        _log(f"SSDP recv failed: {exc}")
                        continue

                    text = packet.decode("utf-8", errors="replace")
                    headers = {}
                    for line in text.replace("\r\n", "\n").split("\n"):
                        if ":" not in line:
                            continue
                        key, value = line.split(":", 1)
                        key = "".join(ch for ch in key.lower() if ch.isalnum())
                        value = value.strip()
                        headers[key] = value
                        for suffix in ("bambucom", "bambulabcom"):
                            if key.endswith(suffix):
                                headers.setdefault(key[:-len(suffix)], value)

                    def header(*names, default=""):
                        for name in names:
                            key = "".join(ch for ch in name.lower() if ch.isalnum())
                            value = headers.get(key)
                            if value:
                                return value
                        return default

                    dev_id = header("dev_id", "devid", "serial", "serial_number", "usn")
                    if dev_id.startswith("uuid:"):
                        dev_id = dev_id[5:]
                    if "::" in dev_id:
                        dev_id = dev_id.split("::", 1)[0]
                    if not dev_id:
                        continue

                    dev_name = header("dev_name", "devname", "friendly_name", "name", default=dev_id)
                    device = {
                        "dev_name": dev_name,
                        "dev_id": dev_id,
                        "dev_ip": addr[0],
                        "dev_type": header("dev_type", "dev_model", "devmodel", "model", "printer_type", default=""),
                        "dev_signal": header("dev_signal", "devsignal", "signal", default="0"),
                        "connect_type": header("connect_type", "connection_type", "dev_connect", "devconnect", default="lan"),
                        "bind_state": header("bind_state", "dev_bind", "devbind", "bind", default="free"),
                        "sec_link": header("sec_link", "dev_sec_link", "devseclink", "secure_link", default=""),
                        "ssdp_version": header("ssdp_version", "dev_version", "devversion", "version", default=""),
                        "connection_name": header("connection_name", default=dev_name),
                    }
                    seen_key = device["dev_id"]
                    seen_value = (
                        device["dev_ip"],
                        device["dev_name"],
                        device["dev_type"],
                        device["connect_type"],
                        device["bind_state"],
                        device["sec_link"],
                        device["ssdp_version"],
                    )
                    if self._ssdp_seen.get(seen_key) != seen_value:
                        self._ssdp_seen[seen_key] = seen_value
                        _log(f"SSDP discovered printer {json.dumps(device, sort_keys=True)}")
                    if self.on_ssdp_msg_fn:
                        self.on_ssdp_msg_fn(json.dumps(device))
        finally:
            for sock in sockets:
                try:
                    sock.close()
                except OSError:
                    pass
            self._ssdp_sockets = []
            _log("SSDP discovery loop exited")

    def _connection_failed(self, dev_id, message, callback_message=None) -> int:
        self._last_error = message
        _log(f"connect_printer failed dev_id={dev_id or '<missing>'}: {message}")
        if self.on_local_connect_fn:
            self.on_local_connect_fn(CONNECT_STATUS_FAILED, dev_id, callback_message or message)
        return -1

    def _queue_payload(self, payload, qos=1, require_connected=True) -> bool:
        try:
            self._mqtt_command_queue.put_nowait((payload, qos, require_connected))
            return True
        except Exception as exc:
            self._last_error = str(exc)
            return False

    def _mqtt_command_loop(self):
        while not self._mqtt_command_worker_stop.is_set():
            try:
                payload, qos, require_connected = self._mqtt_command_queue.get(timeout=0.25)
            except queue.Empty:
                continue

            try:
                if not self._publish_payload(payload, qos=qos, require_connected=require_connected):
                    _log(f"MQTT queued command publish failed: {self._last_error}")
            finally:
                self._mqtt_command_queue.task_done()

    def _publish_payload(self, payload, qos=1, require_connected=True) -> bool:
        if isinstance(payload, str):
            try:
                payload = json.loads(payload)
            except json.JSONDecodeError:
                pass

        with self._mqtt_lock:
            client = self._mqtt_client
            topic = self._mqtt_command_topic
            connected = self._mqtt_connected.is_set()

        if client is None or not topic:
            self._last_error = "MQTT client is not connected"
            return False
        if require_connected and not connected:
            self._last_error = "MQTT client is not connected"
            return False

        try:
            message = payload if isinstance(payload, str) else json.dumps(payload)
            info = client.publish(topic, message, qos=qos)
            if getattr(info, "rc", 0) != 0:
                self._last_error = f"MQTT publish failed with code {info.rc}"
                return False
            return True
        except Exception as exc:
            self._last_error = str(exc)
            return False

    def _handle_message_payload(self, payload: str):
        try:
            doc = json.loads(payload)
        except json.JSONDecodeError:
            return
        if not isinstance(doc, dict):
            return
        with self._mqtt_lock:
            self._merge_printer_data(doc)

    def _merge_printer_data(self, doc: dict[str, Any]):
        for key, value in doc.items():
            if isinstance(value, dict) and isinstance(self._printer_data.get(key), dict):
                self._printer_data[key].update(value)
            else:
                self._printer_data[key] = value

    def _build_filament_info(self) -> dict[str, Any]:
        with self._mqtt_lock:
            data = json.loads(json.dumps(self._printer_data))

        print_data = data.get("print", {})
        if not isinstance(print_data, dict):
            return {"units": [], "external": []}

        return {
            "units": self._build_ams_units(print_data.get("ams", {})),
            "external": self._build_external_filaments(print_data.get("vt_tray")),
        }

    def _build_ams_units(self, ams_data) -> list[dict[str, Any]]:
        if not isinstance(ams_data, dict):
            return []
        units = []
        for unit_index, unit in enumerate(ams_data.get("ams", []) or []):
            if not isinstance(unit, dict):
                continue
            unit_id = str(unit.get("id", unit_index))
            slots = []
            for slot_index, tray in enumerate(unit.get("tray", []) or []):
                if not isinstance(tray, dict):
                    continue
                slots.append(self._convert_filament_tray(tray, slot_index))
            units.append({
                "id": unit_id,
                "type": self._ams_type_name(unit),
                "extruder": self._int_or_default(unit.get("ext_id"), 0),
                "temperature": self._float_or_default(unit.get("temp"), 0.0),
                "humidity_percent": self._int_or_default(unit.get("humidity_raw"), -1),
                "dry_time_min": self._int_or_default(unit.get("dry_time"), 0),
                "slots": slots,
            })
        return units

    def _build_external_filaments(self, vt_tray) -> list[dict[str, Any]]:
        if not isinstance(vt_tray, dict):
            return []
        return [self._convert_filament_tray(vt_tray, 0, extruder=0)]

    def _convert_filament_tray(self, tray, index, extruder=None) -> dict[str, Any]:
        color = self._normalize_color(tray.get("tray_color") or tray.get("color") or "00000000")
        material = str(tray.get("tray_type") or tray.get("type") or "")
        preset_id = str(tray.get("tray_info_idx") or tray.get("preset_id") or "")
        converted = {
            "index": self._int_or_default(tray.get("id"), index),
            "loaded": bool(material or preset_id or tray.get("n")),
            "material": material,
            "preset_id": preset_id,
            "color": color,
            "nozzle_temp_min": self._int_or_default(tray.get("nozzle_temp_min"), 0),
            "nozzle_temp_max": self._int_or_default(tray.get("nozzle_temp_max"), 0),
            "remain_percent": self._int_or_default(tray.get("remain"), -1),
            "k": self._float_or_default(tray.get("k"), 0.0),
        }
        if extruder is not None:
            converted["extruder"] = extruder
        diameter = self._float_or_none(tray.get("tray_diameter") or tray.get("diameter"))
        if diameter is not None:
            converted["diameter_mm"] = diameter
        weight = self._int_or_none(tray.get("tray_weight") or tray.get("weight"))
        if weight is not None:
            converted["weight_g"] = weight
        return converted

    def _start_local_print(self, params, upload_error_code, update_fn=None, cancel_fn=None):
        self._ensure_state()
        if params is None:
            self._last_error = "Missing print params"
            self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
            return self._last_code
        if self._cancel_requested(cancel_fn):
            self._last_error = "Cancelled"
            self._last_code = BAMBU_NETWORK_ERR_CANCELED
            return self._last_code

        self._update_progress(update_fn, PRINTING_STAGE_CREATE, 0, "Preparing...")
        if not self._upload_print_file(params, update_fn, cancel_fn, upload_error_code):
            self._update_progress(update_fn, PRINTING_STAGE_ERROR, self._last_code, self._last_error)
            return self._last_code
        if self._cancel_requested(cancel_fn):
            self._last_error = "Cancelled"
            self._last_code = BAMBU_NETWORK_ERR_CANCELED
            return self._last_code

        try:
            payload = self._build_project_file_payload(params)
        except Exception as exc:
            self._last_error = str(exc)
            self._last_code = BAMBU_NETWORK_ERR_INVALID_HANDLE
            self._update_progress(update_fn, PRINTING_STAGE_ERROR, self._last_code, self._last_error)
            return self._last_code

        self._update_progress(update_fn, PRINTING_STAGE_SENDING, 0, "Starting print...")
        if not self._queue_payload(payload):
            self._last_code = BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED
            self._update_progress(update_fn, PRINTING_STAGE_ERROR, self._last_code, self._last_error)
            return self._last_code

        self._update_progress(update_fn, PRINTING_STAGE_FINISHED, 100, "1")
        return 0

    def _build_project_file_payload(self, params) -> dict[str, Any]:
        filename = self._remote_print_name(params)
        if not filename:
            raise ValueError("Missing print filename")

        plate = self._param(params, "plate_index", 1)
        try:
            plate = int(plate)
        except (TypeError, ValueError):
            plate = 1

        ams_mapping = self._json_or_default(self._param(params, "ams_mapping"), [0])
        if not isinstance(ams_mapping, list):
            ams_mapping = [ams_mapping]

        return {
            "print": {
                "command": "project_file",
                "param": self._param(params, "plate_location") or f"Metadata/plate_{plate}.gcode",
                "file": filename,
                "bed_leveling": self._as_bool(self._param(params, "task_bed_leveling", True)),
                "bed_type": self._param(params, "task_bed_type", "textured_plate"),
                "flow_cali": self._as_bool(self._param(params, "task_flow_cali", True)),
                "vibration_cali": self._as_bool(self._param(params, "task_vibration_cali", True)),
                "url": self._param(params, "url") or f"ftp:///{filename}",
                "layer_inspect": self._as_bool(self._param(params, "task_layer_inspect", False)),
                "sequence_id": str(self._param(params, "sequence_id") or self._next_sequence_id()),
                "timelapse": self._as_bool(self._param(params, "task_record_timelapse", False)),
                "use_ams": self._as_bool(self._param(params, "task_use_ams", True)),
                "ams_mapping": ams_mapping,
                "skip_objects": self._json_or_default(self._param(params, "skip_objects"), None),
            }
        }

    def _build_sdcard_project_file_payload(self, params) -> dict[str, Any]:
        remote = (
            self._param(params, "dst_file")
            or self._param(params, "ftp_file")
            or self._param(params, "file")
            or self._param(params, "filename")
        )
        if not remote:
            raise ValueError("Missing printer storage file")
        remote = str(remote)
        if remote.startswith("file://"):
            file_url = remote
        elif remote.startswith("/"):
            file_url = f"file://{remote}"
        else:
            file_url = f"file:///{remote}"
        payload_params = dict(params) if isinstance(params, dict) else {}
        payload_params["ftp_file"] = Path(remote).name
        payload_params["url"] = self._param(params, "url") or file_url
        return self._build_project_file_payload(payload_params)

    def _upload_print_file(self, params, update_fn=None, cancel_fn=None, error_code=BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED) -> bool:
        source = (
            self._param(params, "local_path")
            or self._param(params, "source")
            or self._param(params, "filename")
            or self._param(params, "dst_file")
        )
        if not source:
            self._last_error = "Missing local file path"
            self._last_code = BAMBU_NETWORK_ERR_FILE_NOT_EXIST
            return False
        path = Path(source)
        if not path.is_file():
            self._last_error = f"Local file does not exist: {source}"
            self._last_code = BAMBU_NETWORK_ERR_FILE_NOT_EXIST
            return False

        remote = self._remote_print_name(params, path)
        host = self._param(params, "dev_ip") or self._mqtt_host
        username = self._param(params, "username") or self._mqtt_username or BAMBU_DEFAULT_USERNAME
        password = self._param(params, "password") or self._mqtt_password
        use_ssl = self._as_bool(self._param(params, "use_ssl_for_ftp", True))
        port = FTP_SSL_PORT if use_ssl else 21
        if not host or not password:
            self._last_error = "Missing FTP host or password"
            self._last_code = error_code
            return False

        try:
            self._update_progress(update_fn, PRINTING_STAGE_UPLOAD, 0, "Uploading...")
            ftp = _ImplicitFTP_TLS() if use_ssl else ftplib.FTP()
            _log(
                "FTP upload starting "
                f"host={host} port={port} ssl={use_ssl} source={path} remote={remote} "
                f"size={path.stat().st_size}"
            )
            ftp.connect(host=host, port=port, timeout=30)
            ftp.login(username, password)
            if use_ssl:
                ftp.prot_p()
            total = path.stat().st_size
            uploaded = 0

            def on_upload(block):
                nonlocal uploaded
                if self._cancel_requested(cancel_fn):
                    raise InterruptedError("Cancelled")
                uploaded += len(block)
                if total > 0:
                    self._update_progress(update_fn, PRINTING_STAGE_UPLOAD, min(100, int(uploaded * 100 / total)), "Uploading...")

            with path.open("rb") as fh:
                ftp.storbinary(f"STOR {remote}", fh, blocksize=32768, callback=on_upload)
            ftp.close()
            self._update_progress(update_fn, PRINTING_STAGE_UPLOAD, 100, "Uploading...")
            _log(f"FTP upload succeeded remote={remote} bytes={uploaded}")
            return True
        except InterruptedError as exc:
            self._last_error = str(exc)
            self._last_code = BAMBU_NETWORK_ERR_CANCELED
            _log(f"FTP upload cancelled remote={remote}: {exc}")
            return False
        except Exception as exc:
            self._last_error = str(exc)
            self._last_code = error_code
            _log(f"FTP upload failed remote={remote} code={error_code}: {type(exc).__name__}: {exc}")
            return False

    def _remote_print_name(self, params, source_path: Path | None = None) -> str:
        remote = self._param(params, "ftp_file") or self._param(params, "file")
        if remote:
            return Path(str(remote)).name
        candidate = self._param(params, "filename") or self._param(params, "dst_file") or self._param(params, "source") or self._param(params, "local_path")
        if candidate:
            return Path(str(candidate)).name
        project_name = self._param(params, "project_name")
        if project_name:
            return Path(str(project_name)).name
        return source_path.name if source_path else ""

    def _status_response(self, result):
        if isinstance(result, bool):
            result = 0 if result else (self._last_code or BAMBU_NETWORK_ERR_INVALID_HANDLE)
        if result == 0:
            return json.dumps({"status": "ok"})
        status = "cancelled" if result == BAMBU_NETWORK_ERR_CANCELED else "error"
        return json.dumps({
            "status": status,
            "code": result,
            "message": self._last_error or "operation failed",
        })

    @staticmethod
    def _looks_like_bambu_payload(request) -> bool:
        return isinstance(request, dict) and any(key in request for key in ("print", "system", "pushing", "info", "upgrade", "xcam", "camera"))

    @staticmethod
    def _disconnect_reason(args) -> int:
        if not args:
            return 0
        if len(args) >= 2:
            return BBLPrinterAgentPlugin._mqtt_result_code(args[1])
        return BBLPrinterAgentPlugin._mqtt_result_code(args[0])

    @staticmethod
    def _ams_type_name(unit) -> str:
        ams_type = str(unit.get("ams_type", unit.get("type", "ams"))).lower()
        if ams_type in {"2", "ams_lite", "ams-lite"}:
            return "ams_lite"
        if ams_type in {"3", "n3f"}:
            return "n3f"
        if ams_type in {"4", "n3s"}:
            return "n3s"
        return "ams"

    @staticmethod
    def _normalize_color(value) -> str:
        color = str(value or "").strip().lstrip("#")
        if not color:
            return "00000000"
        if len(color) == 6:
            color += "FF"
        return color.upper()

    @staticmethod
    def _param(params, key, default=None):
        if isinstance(params, dict):
            return params.get(key, default)
        return getattr(params, key, default)

    def _next_sequence_id(self):
        with self._mqtt_lock:
            self._sequence_id += 1
            return self._sequence_id

    @staticmethod
    def _update_progress(update_fn, stage, code, info):
        if not update_fn:
            return
        try:
            update_fn(stage, code, info)
        except Exception as exc:
            _log(f"progress callback failed: {exc}")

    @staticmethod
    def _cancel_requested(cancel_fn) -> bool:
        if not cancel_fn:
            return False
        try:
            return bool(cancel_fn())
        except Exception as exc:
            _log(f"cancel callback failed: {exc}")
            return False

    @staticmethod
    def _json_or_default(value, default):
        if value in (None, ""):
            return default
        if isinstance(value, str):
            try:
                return json.loads(value)
            except json.JSONDecodeError:
                return default
        return value

    @staticmethod
    def _int_or_default(value, default):
        result = BBLPrinterAgentPlugin._int_or_none(value)
        return default if result is None else result

    @staticmethod
    def _int_or_none(value):
        try:
            return int(value)
        except (TypeError, ValueError):
            return None

    @staticmethod
    def _float_or_default(value, default):
        result = BBLPrinterAgentPlugin._float_or_none(value)
        return default if result is None else result

    @staticmethod
    def _float_or_none(value):
        try:
            return float(value)
        except (TypeError, ValueError):
            return None

    @staticmethod
    def _mqtt_result_code(rc) -> int:
        try:
            return int(rc)
        except (TypeError, ValueError):
            value = getattr(rc, "value", None)
            return int(value) if value is not None else -1

    @staticmethod
    def _as_bool(value) -> bool:
        if isinstance(value, str):
            return value.strip().lower() in {"1", "true", "yes", "on"}
        return bool(value)


@orca.plugin
class BBLPrinterAgentPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(BBLPrinterAgentPlugin)
