# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Elegoo Centauri Carbon 2 (Device tab)"
# description = "Shows an Elegoo Centauri Carbon 2 in the Device tab over your network: live status, temperatures, progress, pause/stop, and printing straight from the slicer."
# author = "Unified Slicer"
# version = "0.1.0"
# ///
"""Elegoo Centauri Carbon 2 printer agent (LAN).

Talks to the printer the same way Elegoo's open-source elegoo-link SDK does:

  * HTTP  GET  http://<ip>/system/info?X-Token=<access code>   -> serial number
  * MQTT  tcp://<ip>:1883, user "elegoo", password = access code (default 123456)
      register:  elegoo/<sn>/api_register            {"client_id", "request_id"}
                 reply on elegoo/<sn>/<request_id>/register_response  {"error": "ok"}
      requests:  elegoo/<sn>/<client_id>/api_request {"id", "method", "params"}
      replies:   elegoo/<sn>/<client_id>/api_response
      pushes:    elegoo/<sn>/api_status               method 6000 = status delta
  * HTTP  PUT  http://<ip>/upload  in 1 MB chunks (Content-Range, X-File-Name, X-File-MD5, X-Token)

Status is merged and translated to the Bambu-style "print" JSON the Device tab understands.

Set up: Printer settings > Basic information > Printer agent = "Elegoo CC2 (LAN)". Set the printer's
IP address as the host and its access code (Settings > Network on the printer) as the API key in the
printer connection dialog.
"""
from __future__ import annotations

import hashlib
import json
import random
import socket
import struct
import threading
import time
import traceback
import urllib.parse
import urllib.request
from pathlib import Path

try:
    import orca  # type: ignore
except Exception:
    orca = None

AGENT_ID = "elegoo_cc2"
AGENT_VERSION = "0.1.0"
DEFAULT_ACCESS_CODE = "123456"
LOG_FILE = Path(__file__).resolve().parent / "elegoo_cc2_lan_data" / "agent.log"

# Bambu network status codes the host understands (slic3r/Utils/bambu_networking.hpp)
OK = 0
ERR_INVALID_HANDLE = -2
ERR_CONNECT_FAILED = -11
ERR_FILE_NOT_EXIST = -14
ERR_CANCELED = -18
ERR_UPLOAD_FAILED = -4020
ERR_PUBLISH_FAILED = -4040
STAGE_CREATE, STAGE_UPLOAD, STAGE_WAITING, STAGE_SENDING, STAGE_RECORD, _, STAGE_FINISHED = range(7)
CONNECT_OK, CONNECT_FAILED, CONNECT_LOST = 0, 1, 2

M_ATTRIBUTES, M_STATUS, M_START, M_PAUSE, M_STOP, M_RESUME = 1001, 1002, 1020, 1021, 1022, 1023
M_HOME, M_TEMPERATURE, M_STATUS_PUSH, M_ATTR_PUSH = 1026, 1028, 6000, 6008


def log(msg: str) -> None:
    try:
        LOG_FILE.parent.mkdir(parents=True, exist_ok=True)
        with LOG_FILE.open("a", encoding="utf-8") as fh:
            fh.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except Exception:
        pass


# ------------------------------------------------------------------------------------------------
# Minimal MQTT 3.1.1 client (QoS 0 only, which is all the printer needs)
# ------------------------------------------------------------------------------------------------

def _enc_str(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


def _enc_len(n: int) -> bytes:
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        if n:
            byte |= 0x80
        out.append(byte)
        if not n:
            return bytes(out)


class MiniMqtt:
    def __init__(self, host: str, port: int, client_id: str, username: str, password: str, on_message, on_lost):
        self.host, self.port, self.client_id = host, port, client_id
        self.username, self.password = username, password
        self.on_message, self.on_lost = on_message, on_lost
        self.sock: socket.socket | None = None
        self.lock = threading.Lock()
        self.alive = False
        self.keepalive = 30
        self._pid = 1

    def connect(self, timeout=8.0) -> None:
        s = socket.create_connection((self.host, self.port), timeout=timeout)
        flags = 0x02 | 0x80 | 0x40  # clean session, username, password
        var = _enc_str("MQTT") + bytes([4, flags]) + struct.pack("!H", self.keepalive)
        payload = _enc_str(self.client_id) + _enc_str(self.username) + _enc_str(self.password)
        body = var + payload
        s.sendall(bytes([0x10]) + _enc_len(len(body)) + body)
        hdr, data = self._read_packet(s)
        if hdr >> 4 != 2 or len(data) < 2 or data[1] != 0:
            s.close()
            code = data[1] if len(data) > 1 else -1
            raise ConnectionError("MQTT connection refused (code %s). Check the access code." % code)
        s.settimeout(None)
        self.sock = s
        self.alive = True
        threading.Thread(target=self._reader, name="cc2-mqtt-read", daemon=True).start()
        threading.Thread(target=self._pinger, name="cc2-mqtt-ping", daemon=True).start()

    @staticmethod
    def _recv_exact(s: socket.socket, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("connection closed")
            buf += chunk
        return bytes(buf)

    def _read_packet(self, s: socket.socket):
        hdr = self._recv_exact(s, 1)[0]
        mult, length = 1, 0
        while True:
            b = self._recv_exact(s, 1)[0]
            length += (b & 0x7F) * mult
            if not b & 0x80:
                break
            mult *= 128
        return hdr, self._recv_exact(s, length) if length else b""

    def _send(self, packet: bytes) -> bool:
        with self.lock:
            if not self.sock:
                return False
            try:
                self.sock.sendall(packet)
                return True
            except OSError:
                return False

    def subscribe(self, topics: list[str]) -> bool:
        self._pid = (self._pid % 65535) + 1
        body = struct.pack("!H", self._pid) + b"".join(_enc_str(t) + b"\x00" for t in topics)
        return self._send(bytes([0x82]) + _enc_len(len(body)) + body)

    def publish(self, topic: str, payload: str) -> bool:
        body = _enc_str(topic) + payload.encode("utf-8")
        return self._send(bytes([0x30]) + _enc_len(len(body)) + body)

    def _reader(self):
        try:
            while self.alive and self.sock:
                hdr, data = self._read_packet(self.sock)
                kind = hdr >> 4
                if kind == 3:  # PUBLISH
                    tlen = struct.unpack("!H", data[:2])[0]
                    topic = data[2:2 + tlen].decode("utf-8", "replace")
                    pos = 2 + tlen
                    if (hdr >> 1) & 0x03:
                        pos += 2  # packet id for QoS > 0
                    try:
                        self.on_message(topic, data[pos:].decode("utf-8", "replace"))
                    except Exception:
                        log("message handler: " + traceback.format_exc())
        except Exception as e:
            if self.alive:
                log("MQTT reader stopped: %s" % e)
        finally:
            was_alive = self.alive
            self.close()
            if was_alive:
                self.on_lost()

    def _pinger(self):
        while self.alive:
            time.sleep(self.keepalive / 2)
            if self.alive and not self._send(b"\xc0\x00"):
                break

    def close(self):
        self.alive = False
        with self.lock:
            if self.sock:
                try:
                    self.sock.sendall(b"\xe0\x00")
                except OSError:
                    pass
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None


# ------------------------------------------------------------------------------------------------
# Printer session
# ------------------------------------------------------------------------------------------------

def _deep_merge(dst: dict, src: dict) -> dict:
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _deep_merge(dst[k], v)
        else:
            dst[k] = v
    return dst


def _host_only(host: str) -> str:
    host = (host or "").strip()
    if "://" in host:
        host = urllib.parse.urlparse(host).hostname or host
    return host.split("/")[0].split(":")[0]


class CC2Session:
    def __init__(self, host: str, access_code: str, on_status, on_lost):
        self.host = _host_only(host)
        self.code = access_code or DEFAULT_ACCESS_CODE
        self.on_status, self.on_lost = on_status, on_lost
        self.client_id = "1_PC_%d" % random.randint(1000, 9999)
        self.request_id = self.client_id + "_req"
        self.sn = ""
        self.status: dict = {}
        self.attributes: dict = {}
        self.mqtt: MiniMqtt | None = None
        self._next_id = 1
        self._pending: dict[int, list] = {}
        self._registered = threading.Event()
        self._register_error = ""
        self._lock = threading.Lock()

    # HTTP ---------------------------------------------------------------------------------
    def fetch_serial(self, timeout=6.0) -> str:
        url = "http://%s/system/info?X-Token=%s" % (self.host, urllib.parse.quote(self.code))
        req = urllib.request.Request(url, headers={"X-Token": self.code, "Accept": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                data = json.loads(resp.read().decode("utf-8", "replace"))
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise ConnectionError("The printer rejected the access code.")
            raise
        if data.get("error_code", 0) not in (0, None):
            raise ConnectionError("Printer error %s" % data.get("error_code"))
        sn = (data.get("system_info") or {}).get("sn", "")
        if not sn:
            raise ConnectionError("The printer did not report a serial number.")
        self.sn = sn
        return sn

    def upload(self, path: Path, name: str, progress=None, cancelled=None) -> None:
        size = path.stat().st_size
        md5 = hashlib.md5()
        with path.open("rb") as fh:
            for block in iter(lambda: fh.read(1 << 20), b""):
                md5.update(block)
        digest = md5.hexdigest()
        offset = 0
        with path.open("rb") as fh:
            while offset < size:
                if cancelled and cancelled():
                    raise InterruptedError()
                chunk = fh.read(1 << 20)
                end = offset + len(chunk) - 1
                req = urllib.request.Request(
                    "http://%s/upload" % self.host, data=chunk, method="PUT",
                    headers={"Content-Type": "application/octet-stream", "Content-Length": str(len(chunk)),
                             "Content-Range": "bytes %d-%d/%d" % (offset, end, size), "X-File-Name": name,
                             "X-File-MD5": digest, "X-Token": self.code})
                with urllib.request.urlopen(req, timeout=180) as resp:
                    body = resp.read().decode("utf-8", "replace") or "{}"
                code = json.loads(body).get("error_code", 0)
                if code not in (0, None):
                    raise IOError("Upload refused by printer (error %s)." % code)
                offset = end + 1
                if progress:
                    progress(int(offset * 100 / max(size, 1)))

    # MQTT ---------------------------------------------------------------------------------
    def connect(self) -> None:
        if not self.sn:
            self.fetch_serial()
        self.mqtt = MiniMqtt(self.host, 1883, self.client_id, "elegoo", self.code, self._on_message, self._on_lost)
        self.mqtt.connect()
        base = "elegoo/%s/" % self.sn
        self.mqtt.subscribe([base + self.client_id + "/api_response", base + "api_status",
                             base + self.request_id + "/register_response"])
        self._registered.clear()
        self.mqtt.publish(base + "api_register", json.dumps({"client_id": self.client_id, "request_id": self.request_id}))
        if not self._registered.wait(8.0):
            self.close()
            raise ConnectionError(self._register_error or "The printer did not answer the registration.")
        self.request(M_ATTRIBUTES, {}, wait=5.0)
        full = self.request(M_STATUS, {}, wait=5.0)
        if isinstance(full, dict):
            with self._lock:
                self.status = full
            self.on_status(self.status)

    def close(self):
        if self.mqtt:
            self.mqtt.close()
            self.mqtt = None

    def request(self, method: int, params: dict, wait: float | None = None):
        if not self.mqtt:
            return None
        with self._lock:
            rid = self._next_id
            self._next_id += 1
            slot = [threading.Event(), None]
            self._pending[rid] = slot
        topic = "elegoo/%s/%s/api_request" % (self.sn, self.client_id)
        if not self.mqtt.publish(topic, json.dumps({"id": rid, "method": method, "params": params})):
            return None
        if wait is None:
            return True
        slot[0].wait(wait)
        with self._lock:
            self._pending.pop(rid, None)
        return slot[1]

    def _on_lost(self):
        self.on_lost()

    def _on_message(self, topic: str, payload: str):
        try:
            msg = json.loads(payload)
        except ValueError:
            return
        if topic.endswith("/register_response"):
            if msg.get("client_id") == self.client_id and msg.get("error") == "ok":
                self._registered.set()
            else:
                self._register_error = "Printer refused the connection: %s" % msg.get("error", "unknown")
                if "too many" in str(msg.get("error", "")):
                    self._register_error = "The printer already has too many clients connected."
            return
        method = msg.get("method")
        result = msg.get("result")
        if method == M_STATUS_PUSH and isinstance(result, dict):
            with self._lock:
                _deep_merge(self.status, result)
                snapshot = json.loads(json.dumps(self.status))
            self.on_status(snapshot)
            return
        if method == M_ATTR_PUSH and isinstance(result, dict):
            _deep_merge(self.attributes, result)
            return
        rid = msg.get("id")
        if isinstance(rid, int):
            with self._lock:
                slot = self._pending.get(rid)
            if method == M_ATTRIBUTES and isinstance(result, dict):
                self.attributes = result
            if slot is not None:
                slot[1] = result if result is not None else msg
                slot[0].set()


# ------------------------------------------------------------------------------------------------
# Translation to the Device tab's status JSON
# ------------------------------------------------------------------------------------------------

def gcode_state(status: dict) -> str:
    ms = status.get("machine_status") or {}
    st, sub = ms.get("status", -1), ms.get("sub_status", 0)
    if st == 2:
        if sub in (2501, 2502, 2505):
            return "PAUSE"
        if sub == 2077:
            return "FINISH"
        if sub in (2503, 2504):
            return "FAILED"
        if sub in (1045, 1096, 1405, 1906, 1041, 2801, 2802, 2901, 2902):
            return "PREPARE"
        return "RUNNING"
    if st == 14:
        return "FAILED"
    if st == 0:
        return "PREPARE"
    return "IDLE"


def build_payload(status: dict, attributes: dict) -> dict:
    p: dict = {"command": "push_status", "msg": 0, "support_mqtt_alive": True,
               "mc_print_error_code": 0, "print_error": 0, "sdcard": True,
               "home_flag": (1 << 8), "support_send_to_sd": True, "support_bed_leveling": 1,
               "nozzle_temp_range": [100, 350], "bed_temp_range": [0, 110]}
    state = gcode_state(status)
    p["gcode_state"] = state
    p["mc_print_stage"] = {"RUNNING": 1, "PAUSE": 2, "FINISH": 3, "FAILED": 4}.get(state, 0)
    ms = status.get("machine_status") or {}
    ps = status.get("print_status") or {}
    if state in ("RUNNING", "PAUSE", "PREPARE", "FINISH"):
        p["mc_percent"] = int(ms.get("progress", 0) or 0)
        p["mc_remaining_time"] = int((ps.get("remaining_time_sec", 0) or 0) / 60)
        p["layer_num"] = int(ps.get("current_layer", 0) or 0)
        p["total_layer_num"] = int(ps.get("total_layer", 0) or 0)
        if ps.get("filename"):
            p["subtask_name"] = ps["filename"]
            p["gcode_file"] = ps["filename"]
    ext = status.get("extruder") or {}
    bed = status.get("heater_bed") or {}
    p["nozzle_temper"] = float(ext.get("temperature", 0) or 0)
    p["nozzle_target_temper"] = float(ext.get("target", 0) or 0)
    p["bed_temper"] = float(bed.get("temperature", 0) or 0)
    p["bed_target_temper"] = float(bed.get("target", 0) or 0)
    chamber = status.get("ztemperature_sensor") or {}
    if "temperature" in chamber:
        p["chamber_temper"] = float(chamber.get("temperature") or 0)
    fan = (status.get("fans") or {}).get("fan") or {}
    if "speed" in fan:
        speed = float(fan.get("speed") or 0)
        p["fan_gear"] = max(0, min(255, int(speed * 255 / 100 + 0.5) if speed <= 100 else int(speed)))
    homed = ((status.get("toolhead") or {}).get("homed_axes") or "").lower()
    p["home_flag"] |= (1 if "x" in homed else 0) | (2 if "y" in homed else 0) | (4 if "z" in homed else 0)
    led = status.get("led") or {}
    if "status" in led:
        p["lights_report"] = [{"node": "chamber_light", "mode": "on" if led.get("status") else "off"}]
    return {"print": p, "t_utc": int(time.time() * 1000)}


def _read_preset_host() -> tuple[str, str, str]:
    """(host, access code, model name) from the selected printer preset."""
    try:
        preset = orca.host.preset_bundle().printers.get_edited_preset()
        host = str(preset.config_value("print_host") or "")
        code = str(preset.config_value("printhost_apikey") or "")
        model = str(preset.config_value("printer_model") or "Centauri Carbon 2")
        return host.strip('"'), code.strip('"'), model.strip('"')
    except Exception:
        return "", "", "Centauri Carbon 2"


def _dev_id_for(host: str) -> str:
    return "cc2-" + _host_only(host)


# ------------------------------------------------------------------------------------------------
# Printer agent capability
# ------------------------------------------------------------------------------------------------

if orca is not None:
    PA = orca.printer_agent

    class ElegooCC2Agent(PA.PrinterAgentBase):
        def __init__(self):
            super().__init__()
            self.session: CC2Session | None = None
            self.dev_id = ""
            self.selected = ""
            self.cb: dict = {}
            self.lock = threading.RLock()
            self.last_emit = 0.0
            self.last_state = ""
            self.version = ""

        # identity --------------------------------------------------------------------------
        def get_name(self):
            return "Elegoo CC2 (LAN)"

        def name(self):
            return "Elegoo CC2 (LAN)"

        def get_agent_info(self):
            return PA.AgentInfo(AGENT_ID, "Elegoo CC2 (LAN)", AGENT_VERSION,
                                "Elegoo Centauri Carbon 2 over the local network (MQTT + HTTP).")

        # callbacks -------------------------------------------------------------------------
        def _set(self, key, fn):
            with self.lock:
                self.cb[key] = fn
            return OK

        def set_on_ssdp_msg_fn(self, fn): return self._set("ssdp", fn)
        def set_on_printer_connected_fn(self, fn): return self._set("connected", fn)
        def set_on_subscribe_failure_fn(self, fn): return self._set("subscribe_failure", fn)
        def set_on_message_fn(self, fn): return self._set("message", fn)
        def set_on_user_message_fn(self, fn): return self._set("user_message", fn)
        def set_on_local_connect_fn(self, fn): return self._set("local_connect", fn)
        def set_on_local_message_fn(self, fn): return self._set("local_message", fn)
        def set_queue_on_main_fn(self, fn): return self._set("queue", fn)
        def set_server_callback(self, fn): return self._set("server_err", fn)

        def _on_main(self, fn):
            q = self.cb.get("queue")
            if q:
                q(fn)
            else:
                fn()

        def _dispatch(self, payload: dict):
            text = json.dumps(payload)
            dev = self.dev_id
            fn = self.cb.get("local_message") or self.cb.get("message")
            if fn:
                self._on_main(lambda: fn(dev, text))

        def _local_connect(self, state: int, msg: str):
            fn = self.cb.get("local_connect")
            dev = self.dev_id
            if fn:
                self._on_main(lambda: fn(state, dev, msg))

        # discovery and binding -------------------------------------------------------------
        def start_discovery(self, start, sending):
            if not start:
                return True
            host, _code, model = _read_preset_host()
            fn = self.cb.get("ssdp")
            if fn and host:
                dev = _dev_id_for(host)
                fn(json.dumps({"dev_name": model or "Centauri Carbon 2", "dev_id": dev, "dev_ip": _host_only(host),
                               "dev_type": model or "Centauri Carbon 2", "dev_signal": "0", "connect_type": "lan",
                               "bind_state": "free", "sec_link": "secure", "ssdp_version": "v1"}))
                if not self.selected:
                    self.selected = dev
            return True

        def bind_detect(self, dev_ip, sec_link, detect):
            detect.dev_id = self.dev_id or _dev_id_for(dev_ip)
            detect.model_id = "Centauri Carbon 2"
            detect.dev_name = "Centauri Carbon 2"
            detect.version = self.version
            detect.connect_type = "lan"
            detect.bind_state = "free"
            return OK

        def get_user_selected_machine(self): return self.selected

        def set_user_selected_machine(self, dev_id):
            self.selected = dev_id
            return OK

        def check_cert(self): return OK
        def ping_bind(self, ping_code): return OK
        def bind(self, dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn): return OK
        def unbind(self, dev_id): return OK

        # connection ------------------------------------------------------------------------
        def connect_printer(self, params):
            host = params.host
            code = params.password
            if not host:
                host, code2, _ = _read_preset_host()
                code = code or code2
            if not host:
                return ERR_INVALID_HANDLE
            self.disconnect_printer()
            self.dev_id = params.dev_id or _dev_id_for(host)
            session = CC2Session(host, code, self._on_status, self._on_lost)
            with self.lock:
                self.session = session

            def run():
                try:
                    session.connect()
                    self.version = str((session.attributes.get("software_version") or {}).get("ota_version", "")) \
                        if isinstance(session.attributes.get("software_version"), dict) else ""
                    if self.session is not session:
                        session.close()
                        return
                    self._local_connect(CONNECT_OK, "0")
                    fn = self.cb.get("connected")
                    dev = self.dev_id
                    if fn:
                        self._on_main(lambda: fn(dev))
                    log("connected to %s (sn %s)" % (session.host, session.sn[-4:]))
                except Exception as e:
                    log("connect failed: %s" % e)
                    self._local_connect(CONNECT_FAILED, str(e))
            threading.Thread(target=run, name="cc2-connect", daemon=True).start()
            return OK

        def disconnect_printer(self):
            with self.lock:
                s, self.session = self.session, None
            if s:
                s.close()
            return OK

        def _on_lost(self):
            self._local_connect(CONNECT_LOST, "lost")

        def _on_status(self, status: dict):
            s = self.session
            payload = build_payload(status, s.attributes if s else {})
            state = payload["print"]["gcode_state"]
            now = time.time()
            if state != self.last_state or now - self.last_emit >= 1.0:
                self.last_state, self.last_emit = state, now
                self._dispatch(payload)

        # commands from the Device tab ------------------------------------------------------
        def send_message(self, dev_id, json_str, qos, flag):
            return self.send_message_to_printer(dev_id, json_str, qos, flag)

        def send_message_to_printer(self, dev_id, json_str, qos, flag):
            s = self.session
            try:
                msg = json.loads(json_str)
            except ValueError:
                return ERR_PUBLISH_FAILED
            info = msg.get("info") or {}
            if info.get("command") == "get_version":
                self._dispatch({"info": {"command": "get_version", "result": "success",
                                         "module": [{"name": "ota", "sw_ver": self.version, "product_name": "Centauri Carbon 2"}]}})
                return OK
            system = msg.get("system") or {}
            if system.get("command") == "get_access_code":
                self._dispatch({"system": {"command": "get_access_code", "access_code": s.code if s else ""}})
                return OK
            pr = msg.get("print") or {}
            cmd = pr.get("command")
            if s is None:
                return ERR_CONNECT_FAILED
            if cmd == "pause":
                s.request(M_PAUSE, {})
            elif cmd == "resume":
                s.request(M_RESUME, {})
            elif cmd == "stop":
                s.request(M_STOP, {})
            elif cmd == "set_bed_temp" and isinstance(pr.get("temp"), (int, float)):
                s.request(M_TEMPERATURE, {"heater_bed": pr["temp"]})
            elif cmd == "set_nozzle_temp" and isinstance(pr.get("target_temp"), (int, float)):
                s.request(M_TEMPERATURE, {"extruder": pr["target_temp"]})
            elif cmd == "home":
                s.request(M_HOME, {"homed_axes": "xyz"})
            elif cmd == "pushall":
                full = s.request(M_STATUS, {}, wait=5.0)
                if isinstance(full, dict):
                    s.status = full
                    self._dispatch(build_payload(full, s.attributes))
            elif cmd == "gcode_line":
                resp = {"print": {"command": "gcode_line", "param": pr.get("param", ""), "result": "failed",
                                  "sequence_id": pr.get("sequence_id", "")}}
                self._dispatch(resp)  # the CC2 LAN API has no raw G-code command
            return OK

        def command_go_home(self):
            s = self.session
            return OK if s and s.request(M_HOME, {"homed_axes": "xyz"}) else ERR_PUBLISH_FAILED

        def command_set_bed(self, temp):
            s = self.session
            return OK if s and s.request(M_TEMPERATURE, {"heater_bed": temp}) else ERR_PUBLISH_FAILED

        # printing --------------------------------------------------------------------------
        def _upload_and_maybe_print(self, params, update_fn, cancel_fn, start: bool):
            s = self.session
            if s is None:
                return ERR_CONNECT_FAILED
            src = Path(params.dst_file or params.filename)
            if not src.exists():
                return ERR_FILE_NOT_EXIST
            name = (params.project_name or src.stem).replace("/", "_").replace("\\", "_")
            if not name.lower().endswith(".gcode"):
                name = Path(name).stem + ".gcode"
            try:
                if update_fn:
                    update_fn(STAGE_UPLOAD, 0, "Uploading G-code...")
                s.upload(src, name, progress=(lambda pc: update_fn(STAGE_UPLOAD, pc, "Uploading G-code...")) if update_fn else None,
                         cancelled=cancel_fn)
            except InterruptedError:
                return ERR_CANCELED
            except Exception as e:
                log("upload failed: %s" % e)
                return ERR_UPLOAD_FAILED
            if not start:
                if update_fn:
                    update_fn(STAGE_FINISHED, 100, "File uploaded")
                return OK
            if update_fn:
                update_fn(STAGE_SENDING, 0, "Starting print...")
            layout = "B" if "B" in str(getattr(params, "task_bed_type", "") or "") else "A"
            res = s.request(M_START, {"storage_media": "local", "filename": name,
                                      "config": {"delay_video": bool(params.task_record_timelapse),
                                                 "printer_check": bool(params.task_bed_leveling),
                                                 "print_layout": layout, "bedlevel_force": False, "slot_map": []}},
                            wait=10.0)
            code = res.get("error_code", 0) if isinstance(res, dict) else (0 if res else -1)
            if code not in (0, None):
                log("start print refused: %s" % res)
                return ERR_PUBLISH_FAILED
            if update_fn:
                update_fn(STAGE_FINISHED, 100, "Print started")
            return OK

        def start_local_print(self, params, update_fn, cancel_fn):
            return self._upload_and_maybe_print(params, update_fn, cancel_fn, True)

        def start_print(self, params, update_fn, cancel_fn, wait_fn):
            return self._upload_and_maybe_print(params, update_fn, cancel_fn, True)

        def start_local_print_with_record(self, params, update_fn, cancel_fn, wait_fn):
            return self._upload_and_maybe_print(params, update_fn, cancel_fn, True)

        def start_send_gcode_to_sdcard(self, params, update_fn, cancel_fn, wait_fn):
            return self._upload_and_maybe_print(params, update_fn, cancel_fn, False)

        def start_sdcard_print(self, params, update_fn, cancel_fn):
            s = self.session
            if s is None:
                return ERR_CONNECT_FAILED
            name = Path(params.filename or params.dst_file).name
            res = s.request(M_START, {"storage_media": "local", "filename": name, "config": {"slot_map": []}}, wait=10.0)
            return OK if res else ERR_PUBLISH_FAILED

    @orca.plugin
    class ElegooCC2Plugin(orca.base):
        def register_capabilities(self):
            orca.register_capability(ElegooCC2Agent)


# ------------------------------------------------------------------------------------------------
# Command line test:  python elegoo_cc2_lan.py <printer-ip> [access-code]
# ------------------------------------------------------------------------------------------------

if __name__ == "__main__" and orca is None:
    import sys
    if len(sys.argv) < 2:
        print("usage: python elegoo_cc2_lan.py <printer-ip> [access-code]")
        sys.exit(2)
    done = threading.Event()

    def show(st):
        print(json.dumps(build_payload(st, {})["print"], indent=1))
        done.set()
    sess = CC2Session(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "", show, lambda: print("connection lost"))
    sess.connect()
    done.wait(10)
    sess.close()
