# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Markforged (Eiger cloud)"
# description = "Shows your Markforged printers from Eiger in the Device tab (status, progress, queue) and sends the plate to Eiger, which slices it and queues it on the printer you pick."
# author = "Unified Slicer"
# version = "0.1.0"
# ///
"""Markforged printer agent, through the Eiger cloud API (https://www.eiger.io/api/v3).

Markforged printers only run jobs sliced by Eiger, Markforged's closed cloud slicer. Their firmware
does not take G-code, and the print file format is not published. So this agent does what Eiger's
own public API allows, and nothing else:

  * Device tab: lists the organization's printers (GET /devices) and polls each one's state,
    active job, progress, layers and remaining time (GET /devices/{id}).
  * Print: turns the plate into one binary STL, uploads it (POST /parts/upload_stl) with an
    Eiger settings preset, waits for Eiger to slice it (GET /parts/slice_status/{id}), then looks
    for the build Eiger made for it and starts it (POST /devices/{id}) or queues it
    (POST /devices/{id}/queue) when the printer is busy.
  * Send to storage: uploads and slices the part only, so it waits in the Eiger library.

What the API cannot do, so this agent cannot either: pause, resume or stop a print, set
temperatures, run G-code, place continuous fiber, or create a build from a part. If Eiger did not
make a build for the part, the part is left in the Eiger library and Eiger opens in the browser
so you can add it to a build there. Orca's own G-code is only a preview for these printers.

Set up:
  1. In Eiger, an organization admin opens Settings > API Keys and creates an access key and
     secret key. API access depends on your Eiger plan.
  2. Printer settings > Basic information > Printer agent = "Markforged (Eiger cloud)" (the
     Markforged printer profiles already select it). In the printer connection dialog put the
     access key as the user name and the secret key as the password (or "ACCESS:SECRET" as the
     API key).
  3. Optional, in orca_plugins/markforged_eiger_data/settings.json:
       {"access_key": "...", "secret_key": "...", "settings_preset": "<Eiger preset title or id>"}
     The keys there are used when no Markforged printer preset is selected (the Device tab's
     cloud list). settings_preset slices with that preset instead of the organization default;
     "python markforged_eiger.py presets" lists them.
"""
from __future__ import annotations

import base64
import json
import os
import struct
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
import uuid
import webbrowser
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

try:
    import orca  # type: ignore
except Exception:
    orca = None

AGENT_ID = "markforged_eiger"
AGENT_NAME = "Markforged (Eiger cloud)"
AGENT_VERSION = "0.1.0"
CLOUD_PROVIDER = "markforged"
API_BASE = "https://www.eiger.io/api/v3"
EIGER_WEB = "https://www.eiger.io/"
DATA_DIR = Path(__file__).resolve().parent / "markforged_eiger_data"
LOG_FILE = DATA_DIR / "agent.log"
POLL_SECONDS = 10.0
SLICE_TIMEOUT = 20 * 60
BUILD_WAIT = 60

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


def log(msg: str) -> None:
    try:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        with LOG_FILE.open("a", encoding="utf-8") as fh:
            fh.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    except Exception:
        pass


def load_settings() -> dict:
    try:
        return json.loads((DATA_DIR / "settings.json").read_text(encoding="utf-8"))
    except Exception:
        return {}


# ------------------------------------------------------------------------------------------------
# Eiger REST client
# ------------------------------------------------------------------------------------------------

class EigerError(RuntimeError):
    def __init__(self, status: int, body: str):
        super().__init__("Eiger API %s: %s" % (status, body[:300]))
        self.status = status


class Eiger:
    def __init__(self, access_key: str, secret_key: str, base: str = API_BASE):
        if not access_key or not secret_key:
            raise ValueError("Eiger access key and secret key are both needed")
        token = base64.b64encode(("%s:%s" % (access_key, secret_key)).encode()).decode()
        self.auth = "Basic " + token
        self.base = base.rstrip("/")

    def _request(self, method: str, path: str, query: dict | None = None, body: bytes | None = None,
                 content_type: str | None = None, timeout: float = 30.0):
        url = self.base + path
        if query:
            url += "?" + urllib.parse.urlencode(query)
        req = urllib.request.Request(url, data=body, method=method)
        req.add_header("Authorization", self.auth)
        req.add_header("Accept", "application/json")
        req.add_header("User-Agent", "UnifiedSlicer-Markforged/" + AGENT_VERSION)
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                raw = resp.read()
        except urllib.error.HTTPError as e:
            raise EigerError(e.code, e.read().decode("utf-8", "replace")) from None
        return json.loads(raw) if raw else None

    def get(self, path: str, **query):
        return self._request("GET", path, query or None)

    def post_json(self, path: str, payload: dict):
        return self._request("POST", path, body=json.dumps(payload).encode(), content_type="application/json")

    # endpoints ---------------------------------------------------------------------------------
    def devices(self) -> list[dict]:
        out, page = [], 1
        while True:
            res = self.get("/devices", **{"page[number]": page, "page[size]": 100}) or {}
            items = res.get("items", []) if isinstance(res, dict) else list(res)
            out += items
            if not (isinstance(res, dict) and res.get("has_more_items")):
                return out
            page += 1

    def device(self, device_id: str) -> dict:
        return self.get("/devices/" + device_id) or {}

    def settings_presets(self) -> list[dict]:
        res = self.get("/parts/settings_presets") or []
        return res.get("items", []) if isinstance(res, dict) else res

    def upload_stl(self, stl: bytes, filename: str, title: str, preset_id: str = "") -> dict:
        boundary = "----unified" + uuid.uuid4().hex
        parts = []

        def field(name: str, value: str):
            parts.append(("--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n%s\r\n"
                          % (boundary, name, value)).encode())
        field("title", title)
        if preset_id:
            field("settingsPresetId", preset_id)
        safe = filename.replace('"', "_")
        parts.append(("--%s\r\nContent-Disposition: form-data; name=\"stlFile\"; filename=\"%s\"\r\n"
                      "Content-Type: model/stl\r\n\r\n" % (boundary, safe)).encode() + stl + b"\r\n")
        parts.append(("--%s--\r\n" % boundary).encode())
        return self._request("POST", "/parts/upload_stl", body=b"".join(parts),
                             content_type="multipart/form-data; boundary=" + boundary, timeout=300) or {}

    def slice_status(self, slice_job_id: str) -> str:
        return str((self.get("/parts/slice_status/" + slice_job_id) or {}).get("status", ""))

    def builds_titled(self, title: str) -> list[dict]:
        res = self.get("/builds", **{"filter[title][eq]": title, "sort[by]": "created_at",
                                     "sort[order]": "desc", "page[size]": 20}) or {}
        return res.get("items", []) if isinstance(res, dict) else res

    def print_now(self, device_id: str, build_id: str):
        return self.post_json("/devices/" + device_id, {"build": build_id})

    def queue(self, device_id: str, build_id: str):
        return self.post_json("/devices/%s/queue" % device_id, {"build": build_id})


def resolve_preset_id(api: Eiger, wanted: str) -> str:
    """Eiger settings preset id from an id or a title; empty means the organization default."""
    wanted = (wanted or "").strip()
    if not wanted:
        return ""
    for p in api.settings_presets():
        if wanted in (p.get("id"), p.get("title")):
            return str(p["id"])
    log("settings preset %r not found in Eiger, using the default" % wanted)
    return ""


# ------------------------------------------------------------------------------------------------
# 3MF -> binary STL (Eiger takes STL only)
# ------------------------------------------------------------------------------------------------

NS_CORE = "{http://schemas.microsoft.com/3dmanufacturing/core/2015/02}"
NS_PROD = "{http://schemas.microsoft.com/3dmanufacturing/production/2015/06}"
IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0)


def _matrix(text: str | None) -> tuple:
    if not text:
        return IDENTITY
    vals = tuple(float(v) for v in text.split())
    return vals if len(vals) == 12 else IDENTITY


def _compose(a: tuple, b: tuple) -> tuple:
    """a applied first, then b (3MF row-vector convention: p' = p * M)."""
    out = []
    for r in range(4):
        row = a[r * 3:r * 3 + 3] if r < 3 else a[9:12]
        for c in range(3):
            v = row[0] * b[c] + row[1] * b[3 + c] + row[2] * b[6 + c]
            if r == 3:
                v += b[9 + c]
            out.append(v)
    return tuple(out)


def _apply(m: tuple, x: float, y: float, z: float) -> tuple:
    return (x * m[0] + y * m[3] + z * m[6] + m[9],
            x * m[1] + y * m[4] + z * m[7] + m[10],
            x * m[2] + y * m[5] + z * m[8] + m[11])


def _plate_object_ids(zf: zipfile.ZipFile, plate_index: int) -> set[str] | None:
    """Build-item object ids on one plate (Orca/Bambu Metadata/model_settings.config), or None."""
    try:
        root = ET.fromstring(zf.read("Metadata/model_settings.config"))
    except KeyError:
        return None
    for plate in root.iter("plate"):
        meta = {m.get("key"): m.get("value") for m in plate.findall("metadata")}
        if str(meta.get("plater_id")) != str(plate_index + 1):
            continue
        ids = set()
        for inst in plate.iter("model_instance"):
            for m in inst.findall("metadata"):
                if m.get("key") == "object_id":
                    ids.add(str(m.get("value")))
        return ids
    return None


def threemf_to_stl(path: Path, plate_index: int = -1) -> tuple[bytes, int]:
    """All printable build items (of one plate when plate_index >= 0) as one binary STL."""
    with zipfile.ZipFile(path) as zf:
        roots: dict[str, ET.Element] = {}

        def model(part: str) -> ET.Element:
            part = part.lstrip("/")
            if part not in roots:
                roots[part] = ET.fromstring(zf.read(part))
            return roots[part]

        main = "3D/3dmodel.model"
        on_plate = _plate_object_ids(zf, plate_index) if plate_index >= 0 else None
        tris: list[tuple] = []

        def emit(part: str, object_id: str, m: tuple, depth: int = 0):
            if depth > 16:
                return
            for obj in model(part).iter(NS_CORE + "object"):
                if obj.get("id") != object_id:
                    continue
                mesh = obj.find(NS_CORE + "mesh")
                if mesh is not None:
                    vs = [_apply(m, float(v.get("x")), float(v.get("y")), float(v.get("z")))
                          for v in mesh.find(NS_CORE + "vertices")]
                    for t in mesh.find(NS_CORE + "triangles"):
                        tris.append((vs[int(t.get("v1"))], vs[int(t.get("v2"))], vs[int(t.get("v3"))]))
                comps = obj.find(NS_CORE + "components")
                if comps is not None:
                    for c in comps:
                        sub = c.get(NS_PROD + "path") or c.get("path") or part
                        emit(sub, c.get("objectid"), _compose(_matrix(c.get("transform")), m), depth + 1)
                return

        build = model(main).find(NS_CORE + "build")
        for item in (build if build is not None else []):
            if item.get("printable") == "0":
                continue
            oid = item.get("objectid")
            if on_plate is not None and oid not in on_plate:
                continue
            emit(item.get(NS_PROD + "path") or main, oid, _matrix(item.get("transform")))

    if not tris:
        raise ValueError("no printable objects found in " + path.name)
    out = bytearray(b"Unified Slicer -> Markforged Eiger".ljust(80, b" "))
    out += struct.pack("<I", len(tris))
    for a, b, c in tris:
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = (n[0] ** 2 + n[1] ** 2 + n[2] ** 2) ** 0.5 or 1.0
        out += struct.pack("<12fH", n[0] / ln, n[1] / ln, n[2] / ln, *a, *b, *c, 0)
    return bytes(out), len(tris)


# ------------------------------------------------------------------------------------------------
# Eiger device -> Bambu-style "print" JSON the Device tab understands
# ------------------------------------------------------------------------------------------------

def gcode_state(device: dict) -> str:
    state = str(device.get("state", "")).lower()
    if state == "printing":
        printing = str((device.get("active_job") or {}).get("printing_state", "")).lower()
        return "PREPARE" if printing in ("heating", "scanning", "calibrating") else "RUNNING"
    if state == "print paused":
        return "PAUSE"
    if state == "print finished":
        return "FINISH"
    if state in ("material jam", "fiber jam", "out of material", "user action required"):
        return "FAILED"
    return "IDLE"


def build_payload(device: dict) -> dict:
    p: dict = {"command": "push_status", "msg": 0, "support_mqtt_alive": True,
               "mc_print_error_code": 0, "print_error": 0, "sdcard": False, "home_flag": 0,
               "support_send_to_sd": True}
    state = gcode_state(device)
    p["gcode_state"] = state
    p["mc_print_stage"] = {"RUNNING": 1, "PAUSE": 2, "FINISH": 3, "FAILED": 4}.get(state, 0)
    job = device.get("active_job") or {}
    if job:
        p["mc_percent"] = int(job.get("progress", 0) or 0)
        p["mc_remaining_time"] = int((job.get("estimated_seconds_remaining", 0) or 0) / 60)
        p["layer_num"] = int(job.get("current_layer", 0) or 0)
        p["total_layer_num"] = int(job.get("layer_count", 0) or 0)
        title = (job.get("build") or {}).get("title")
        if title:
            p["subtask_name"] = title
            p["gcode_file"] = title
    # Eiger reports no temperatures; the loaded material shows where the AMS tray would.
    p["eiger"] = {k: device.get(k) for k in ("state", "device_type", "loaded_primary_material",
                                               "loaded_secondary_material", "ccs_primary_remaining",
                                               "queue_length", "queue_estimated_time_seconds")}
    return {"print": p, "t_utc": int(time.time() * 1000)}


def _device_id(dev_id: str) -> str:
    return dev_id[3:] if dev_id.startswith("mf-") else dev_id


def _read_preset_keys() -> tuple[str, str]:
    """(access key, secret key) from the selected printer preset."""
    try:
        cfg = orca.host.preset_bundle().printers.get_edited_preset()
        user = str(cfg.config_value("printhost_user") or "").strip('"')
        pwd = str(cfg.config_value("printhost_password") or "").strip('"')
        if user and pwd:
            return user, pwd
        key = str(cfg.config_value("printhost_apikey") or "").strip('"')
        if ":" in key:
            a, s = key.split(":", 1)
            return a.strip(), s.strip()
    except Exception:
        pass
    # The Device tab's provider rail lists printers without a Markforged preset selected.
    saved = load_settings()
    return (saved.get("access_key") or os.environ.get("EIGER_ACCESS_KEY", ""),
            saved.get("secret_key") or os.environ.get("EIGER_SECRET_KEY", ""))


# ------------------------------------------------------------------------------------------------
# The whole send: plate -> STL -> Eiger part -> sliced -> build -> printer
# ------------------------------------------------------------------------------------------------

def send_to_eiger(api: Eiger, src: Path, title: str, device_id: str, start: bool, plate_index: int = -1,
                  status=lambda pc, msg: None, cancelled=lambda: False) -> str:
    """Returns a short result message. Raises on failure, InterruptedError on cancel."""
    status(5, "Converting the plate for Eiger...")
    stl, n = threemf_to_stl(src, plate_index)
    log("plate %s -> STL with %d triangles (%d bytes)" % (src.name, n, len(stl)))
    preset = resolve_preset_id(api, load_settings().get("settings_preset", ""))
    if cancelled():
        raise InterruptedError()
    status(15, "Uploading to Eiger...")
    started = time.time()
    up = api.upload_stl(stl, title + ".stl", title, preset)
    job, part_version = up.get("slice_job_id"), up.get("part_version_id")
    log("uploaded part version %s, slice job %s" % (part_version, job))
    deadline = time.time() + SLICE_TIMEOUT
    while job:
        st = api.slice_status(job).upper()
        if st == "COMPLETED":
            break
        if st in ("ERROR", "CANCELED"):
            raise RuntimeError("Eiger could not slice the part (%s)" % st.lower())
        if time.time() > deadline:
            raise RuntimeError("Eiger is still slicing after %d minutes" % (SLICE_TIMEOUT // 60))
        if cancelled():
            raise InterruptedError()
        status(40, "Eiger is slicing (%s)..." % st.lower().replace("_", " "))
        time.sleep(3)
    if not start:
        return "Sliced in Eiger; the part is in your Eiger library"

    # Eiger's API cannot make a build; use the one Eiger made for this part, if it made one.
    status(80, "Looking for the Eiger build...")
    build = None
    wait_until = time.time() + BUILD_WAIT
    while build is None and time.time() < wait_until:
        for b in api.builds_titled(title):
            created = b.get("created_at", "")
            if b.get("sliced") and created and _iso_ts(created) >= started - 120:
                build = b
                break
        if build is None:
            if cancelled():
                raise InterruptedError()
            time.sleep(5)
    if build is None:
        log("no build named %r after upload; opening Eiger" % title)
        try:
            webbrowser.open(EIGER_WEB)
        except Exception:
            pass
        return "Sliced in Eiger. Add the part to a build in Eiger to print it"

    status(90, "Sending the build to the printer...")
    state = str(api.device(device_id).get("state", ""))
    if state == "Ready":
        api.print_now(device_id, build["id"])
        return "Print started"
    api.queue(device_id, build["id"])
    return "Added to the printer's queue (printer is %s)" % (state.lower() or "busy")


def _iso_ts(text: str) -> float:
    from datetime import datetime
    try:
        return datetime.fromisoformat(text.replace("Z", "+00:00")).timestamp()
    except ValueError:
        return 0.0


# ------------------------------------------------------------------------------------------------
# Printer agent capability
# ------------------------------------------------------------------------------------------------

if orca is not None:
    PA = orca.printer_agent

    class MarkforgedEigerAgent(PA.PrinterAgentBase):
        def __init__(self):
            super().__init__()
            self.api: Eiger | None = None
            self.dev_id = ""
            self.selected = ""
            self.cb: dict = {}
            self.lock = threading.RLock()
            self.stop_evt = threading.Event()
            self.poller: threading.Thread | None = None
            self.devices: dict[str, dict] = {}

        # identity --------------------------------------------------------------------------
        def get_name(self): return AGENT_NAME
        def name(self): return AGENT_NAME

        def get_agent_info(self):
            return PA.AgentInfo(AGENT_ID, AGENT_NAME, AGENT_VERSION,
                                "Markforged printers through the Eiger cloud API (status, queue, send).")

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
            q(fn) if q else fn()

        def _dispatch(self, dev: str, payload: dict):
            text = json.dumps(payload)
            fn = self.cb.get("local_message") or self.cb.get("message")
            if fn:
                self._on_main(lambda: fn(dev, text))

        def _local_connect(self, state: int, dev: str, msg: str):
            fn = self.cb.get("local_connect")
            if fn:
                self._on_main(lambda: fn(state, dev, msg))

        def _client(self) -> Eiger:
            if self.api is None:
                self.api = Eiger(*_read_preset_keys())
            return self.api

        # discovery: every printer in the Eiger organization --------------------------------
        def start_discovery(self, start, sending):
            if not start:
                return True

            def run():
                try:
                    devs = self._client().devices()
                except Exception as e:
                    log("device list failed: %s" % e)
                    return
                fn = self.cb.get("ssdp")
                for d in devs:
                    dev = "mf-" + str(d.get("id"))
                    self.devices[dev] = d
                    if fn:
                        info = json.dumps({"dev_name": d.get("name") or d.get("device_type") or "Markforged",
                                           "dev_id": dev, "dev_ip": "", "dev_type": d.get("device_type", ""),
                                           "dev_signal": "0", "connect_type": "cloud", "bind_state": "occupied",
                                           "sec_link": "secure", "ssdp_version": "v1",
                                           "online": str(d.get("state", "")) not in ("", "Offline", "Disabled", "Unknown"),
                                           "status": str(d.get("state", "")),
                                           "progress": int((d.get("active_job") or {}).get("progress", 0) or 0)})
                        self._on_main(lambda info=info: fn(info))
                    if not self.selected:
                        self.selected = dev
            threading.Thread(target=run, name="eiger-discovery", daemon=True).start()
            return True

        def bind_detect(self, dev_ip, sec_link, detect):
            d = self.devices.get(self.selected, {})
            detect.dev_id = self.selected
            detect.model_id = str(d.get("device_type", ""))
            detect.dev_name = str(d.get("name", ""))
            detect.connect_type = "cloud"
            detect.bind_state = "occupied"
            return OK

        def get_user_selected_machine(self): return self.selected

        def set_user_selected_machine(self, dev_id):
            self.selected = dev_id
            return OK

        def check_cert(self): return OK
        def ping_bind(self, ping_code): return OK
        def bind(self, dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn): return OK
        def unbind(self, dev_id): return OK

        # connection = poll the device ------------------------------------------------------
        def connect_printer(self, params):
            self.disconnect_printer()
            if params.username and params.password:
                self.api = Eiger(params.username, params.password)
            dev = params.dev_id or self.selected
            if not dev:
                return ERR_INVALID_HANDLE
            self.dev_id = dev
            stop = threading.Event()
            self.stop_evt = stop

            def run():
                first = True
                while not stop.is_set():
                    try:
                        d = self._client().device(_device_id(dev))
                        if first:
                            first = False
                            self._local_connect(CONNECT_OK, dev, "0")
                            fn = self.cb.get("connected")
                            if fn:
                                self._on_main(lambda: fn(dev))
                        self._dispatch(dev, build_payload(d))
                    except Exception as e:
                        log("poll %s failed: %s" % (dev, e))
                        self._local_connect(CONNECT_FAILED if first else CONNECT_LOST, dev, str(e))
                        if first:
                            return
                    stop.wait(POLL_SECONDS)
            self.poller = threading.Thread(target=run, name="eiger-poll", daemon=True)
            self.poller.start()
            return OK

        def disconnect_printer(self):
            self.stop_evt.set()
            return OK

        # commands from the Device tab ------------------------------------------------------
        def send_message(self, dev_id, json_str, qos, flag):
            return self.send_message_to_printer(dev_id, json_str, qos, flag)

        def send_message_to_printer(self, dev_id, json_str, qos, flag):
            try:
                msg = json.loads(json_str)
            except ValueError:
                return ERR_PUBLISH_FAILED
            if (msg.get("info") or {}).get("command") == "get_version":
                self._dispatch(dev_id, {"info": {"command": "get_version", "result": "success", "module": []}})
                return OK
            pr = msg.get("print") or {}
            if pr.get("command") == "pushall":
                try:
                    self._dispatch(dev_id, build_payload(self._client().device(_device_id(dev_id))))
                    return OK
                except Exception as e:
                    log("pushall failed: %s" % e)
                    return ERR_CONNECT_FAILED
            # Eiger's API has no pause, resume, stop, temperature or G-code commands.
            log("command %r is not available through Eiger" % pr.get("command"))
            return ERR_PUBLISH_FAILED

        # printing --------------------------------------------------------------------------
        def _project_3mf(self, params) -> Path:
            for cand in (params.filename, params.dst_file):
                p = Path(cand or "")
                if p.is_file() and zipfile.is_zipfile(p):
                    try:
                        with zipfile.ZipFile(p) as zf:
                            if "3D/3dmodel.model" in zf.namelist():
                                return p
                    except zipfile.BadZipFile:
                        pass
            # A plain G-code file has no model in it: export the project from the main thread.
            out = DATA_DIR / "upload" / "project.3mf"
            out.parent.mkdir(parents=True, exist_ok=True)
            done, err = threading.Event(), []

            def export():
                try:
                    orca.host.plater().export_3mf_copy(str(out))
                except Exception as e:
                    err.append(e)
                finally:
                    done.set()
            self._on_main(export)
            if not done.wait(120) or err:
                raise RuntimeError("could not export the project: %s" % (err[0] if err else "timed out"))
            return out

        def _send(self, params, update_fn, cancel_fn, start: bool):
            def status(pc, msg):
                if update_fn:
                    update_fn(STAGE_UPLOAD if pc < 90 else STAGE_SENDING, pc, msg)
            try:
                src = self._project_3mf(params)
                title = (params.project_name or params.task_name or Path(src).stem).strip() or "Unified Slicer part"
                title = title.replace("/", "_").replace("\\", "_")
                if title.lower().endswith(".3mf"):
                    title = title[:-4]
                plate = int(params.plate_index or 0) - 1  # PrintParams counts plates from 1
                result = send_to_eiger(self._client(), src, title, _device_id(params.dev_id or self.dev_id),
                                       start, plate, status, (lambda: bool(cancel_fn and cancel_fn())))
            except InterruptedError:
                return ERR_CANCELED
            except FileNotFoundError:
                return ERR_FILE_NOT_EXIST
            except Exception as e:
                log("send failed: %s\n%s" % (e, traceback.format_exc()))
                if update_fn:
                    update_fn(STAGE_FINISHED, 0, str(e))
                return ERR_UPLOAD_FAILED
            log(result)
            if update_fn:
                update_fn(STAGE_FINISHED, 100, result)
            return OK

        def start_print(self, params, update_fn, cancel_fn, wait_fn):
            return self._send(params, update_fn, cancel_fn, True)

        def start_local_print(self, params, update_fn, cancel_fn):
            return self._send(params, update_fn, cancel_fn, True)

        def start_local_print_with_record(self, params, update_fn, cancel_fn, wait_fn):
            return self._send(params, update_fn, cancel_fn, True)

        def start_send_gcode_to_sdcard(self, params, update_fn, cancel_fn, wait_fn):
            return self._send(params, update_fn, cancel_fn, False)

        def start_sdcard_print(self, params, update_fn, cancel_fn):
            return ERR_PUBLISH_FAILED

    @orca.plugin
    class MarkforgedEigerPlugin(orca.base):
        def register_capabilities(self):
            orca.register_capability(MarkforgedEigerAgent)


# ------------------------------------------------------------------------------------------------
# Command line (set EIGER_ACCESS_KEY and EIGER_SECRET_KEY):
#   python markforged_eiger.py devices
#   python markforged_eiger.py presets
#   python markforged_eiger.py stl PROJECT.3mf OUT.stl [plate]
#   python markforged_eiger.py send PROJECT.3mf DEVICE_ID [--upload-only]
# ------------------------------------------------------------------------------------------------

if __name__ == "__main__" and orca is None:
    import sys
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(2)
    cmd = args[0]
    if cmd == "stl":
        data, count = threemf_to_stl(Path(args[1]), int(args[3]) - 1 if len(args) > 3 else -1)
        Path(args[2]).write_bytes(data)
        print("%d triangles -> %s" % (count, args[2]))
        sys.exit(0)
    client = Eiger(os.environ.get("EIGER_ACCESS_KEY", ""), os.environ.get("EIGER_SECRET_KEY", ""))
    if cmd == "devices":
        for d in client.devices():
            print("%s  %-10s %-20s %s" % (d.get("id"), d.get("device_type"), d.get("state"), d.get("name")))
    elif cmd == "presets":
        for p in client.settings_presets():
            print("%s  %s" % (p.get("id"), p.get("title")))
    elif cmd == "send":
        print(send_to_eiger(client, Path(args[1]), Path(args[1]).stem, args[2], "--upload-only" not in args,
                            status=lambda pc, msg: print("%3d%% %s" % (pc, msg))))
    else:
        print("unknown command " + cmd)
        sys.exit(2)
