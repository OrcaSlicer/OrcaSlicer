# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# name = "Elegoo Cloud for Centauri Carbon 2"
# description = "Send G-code from the slicer to an Elegoo Centauri Carbon 2 through the Elegoo cloud (works away from the printer's network)."
# author = "Unified Slicer"
# version = "0.2.1"
# ///
"""Elegoo Cloud for Centauri Carbon 2.

Sends prints to a Centauri Carbon 2 the same way ElegooSlicer does in cloud mode:

  1. HTTPS to the Elegoo IoT API (https://matrix.elegoo.com) with your account's bearer token:
       - device list, Agora (RTM) token, file detail
       - an upload slot in Elegoo's object storage ("biz-entrypoint"), then a PUT of the G-code there
  2. Agora RTM (real-time messaging) to the printer's own RTM user ("<agoraUserId><serial>"):
       - method 1058 cancel any old download task
       - method 1057 "download this URL" (the printer pulls the file from cloud storage)
       - progress arrives back as method 6006 messages
       - method 1020 start print

Agora RTM has no Python SDK, so this plugin drives Agora's native RTM 2.x library
(agora_rtm_sdk.dll + libaosl.dll, the same files ElegooSlicer installs) through ctypes.

Inside the slicer:
  * Plugins > "Elegoo Cloud (CC2)" opens a panel beside the 3D view.
  * Slice, then press "Upload & print" in the panel. It sends the current slice (no export needed);
    a newer "Export G-code" is used instead when there is one, and auto-send can fire on each export.

Outside the slicer the same file works as a command-line tool, useful for testing:
  python elegoo_cloud_cc2.py login            import your ElegooSlicer sign-in
  python elegoo_cloud_cc2.py printers         list printers bound to your account
  python elegoo_cloud_cc2.py send FILE.gcode  upload and print (add --upload-only, --printer SERIAL)
"""
from __future__ import annotations

import base64
import ctypes
import hashlib
import json
import os
import shutil
import sys
import threading
import time
import traceback
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

try:  # Inside the slicer
    import orca  # type: ignore
except Exception:  # Standalone command-line use
    orca = None

PLUGIN_VERSION = "0.3.0"
API_GLOBAL = "https://matrix.elegoo.com"
API_CHINA = "https://matrix.elegoo.com.cn"
AGORA_APP_ID = "d035320941e34cd5bc4ec106eff05580"  # Elegoo's Agora project (from the open-source elegoo-link SDK)
ELEGOO_LINK_RAW = "https://raw.githubusercontent.com/ELEGOO-3D/elegoo-link/main/thirdparty/agora"

DATA_DIR = Path(__file__).resolve().parent / "elegoo_cloud_data"
STATE_FILE = DATA_DIR / "state.json"
LAST_EXPORT = DATA_DIR / "last_export.gcode"
LAST_EXPORT_META = DATA_DIR / "last_export.json"
LOG_FILE = DATA_DIR / "plugin.log"

# Printer method codes (Centauri Carbon 2 protocol)
M_START_PRINT = 1020
M_DOWNLOAD_FILE = 1057
M_CANCEL_DOWNLOAD = 1058
M_DOWNLOAD_PROGRESS = 6006

PRINTER_ERRORS = {
    1000: "printer rejected the token",
    1003: "invalid parameter",
    1009: "printer is busy",
    1021: "file not found on printer",
    1026: "bed leveling data missing",
    109: "filament runout",
}


class CloudError(Exception):
    pass


def _now_ms() -> int:
    return int(time.time() * 1000)


def log(msg: str) -> None:
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + msg
    try:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass
    if orca is None:
        print(line, file=sys.stderr)


def _mask(s: str) -> str:
    s = str(s or "")
    return s if len(s) <= 6 else s[:3] + "…" + s[-3:]


# ---------------------------------------------------------------------------------------------
# Persistent state (credentials, chosen printer, options)
# ---------------------------------------------------------------------------------------------

DEFAULT_STATE = {
    "region": "global",            # "global" or "CN"
    "access_token": "",
    "refresh_token": "",
    "access_expire_ms": 0,
    "user_id": "",                 # Elegoo account id
    "printer_serial": "",
    "auto_bed_leveling": True,
    "timelapse": False,
    "plate_side": "A",            # print_layout: "A" or "B"
    "auto_send_on_export": False,
    "agora_dir": "",              # optional folder holding agora_rtm_sdk.dll/libaosl.dll
    "imported_from_mtime": 0,
}

_state_lock = threading.RLock()


def load_state() -> dict:
    with _state_lock:
        st = dict(DEFAULT_STATE)
        try:
            st.update(json.loads(STATE_FILE.read_text(encoding="utf-8")))
        except Exception:
            pass
        return st


def save_state(st: dict) -> None:
    with _state_lock:
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        tmp = STATE_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(st, indent=2), encoding="utf-8")
        os.replace(tmp, STATE_FILE)


# ---------------------------------------------------------------------------------------------
# Import the sign-in that ElegooSlicer saved (<data_dir>/user/user_info)
#   Windows: DPAPI (CryptProtectData, current user).  macOS/Linux: AES-256-CBC, iv|key|ciphertext.
# ---------------------------------------------------------------------------------------------

def elegooslicer_user_file() -> Path | None:
    candidates = []
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA")
        if appdata:
            candidates.append(Path(appdata) / "ElegooSlicer" / "user" / "user_info")
    elif sys.platform == "darwin":
        candidates.append(Path.home() / "Library" / "Application Support" / "ElegooSlicer" / "user" / "user_info")
    else:
        candidates.append(Path.home() / ".config" / "ElegooSlicer" / "user" / "user_info")
    for c in candidates:
        if c.exists():
            return c
        js = c.with_name("user_info.json")
        if js.exists():
            return js
    return None


def _dpapi_decrypt(blob: bytes) -> bytes:
    from ctypes import wintypes

    class DATA_BLOB(ctypes.Structure):
        _fields_ = [("cbData", wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    buf = ctypes.create_string_buffer(blob, len(blob))
    inp = DATA_BLOB(len(blob), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    out = DATA_BLOB()
    crypt32 = ctypes.windll.crypt32
    kernel32 = ctypes.windll.kernel32
    if not crypt32.CryptUnprotectData(ctypes.byref(inp), None, None, None, None, 0, ctypes.byref(out)):
        raise CloudError("Windows could not decrypt ElegooSlicer's saved sign-in (DPAPI error %d)" % kernel32.GetLastError())
    try:
        return ctypes.string_at(out.pbData, out.cbData)
    finally:
        kernel32.LocalFree(out.pbData)


def _aes_cbc_decrypt(blob: bytes) -> bytes:
    iv, key, ct = blob[:16], blob[16:48], blob[48:]
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes  # type: ignore
        d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
        pt = d.update(ct) + d.finalize()
    except ImportError:
        import subprocess
        pt = subprocess.run(["openssl", "enc", "-d", "-aes-256-cbc", "-nopad", "-K", key.hex(), "-iv", iv.hex()],
                            input=ct, capture_output=True, check=True).stdout
    pad = pt[-1]
    return pt[:-pad] if 1 <= pad <= 16 else pt


def _find_key(obj, names):
    """Depth-first search for the first non-empty value under any of names (case-insensitive)."""
    lowered = {n.lower() for n in names}
    if isinstance(obj, dict):
        for k, v in obj.items():
            if k.lower() in lowered and v not in (None, "", 0):
                return v
        for v in obj.values():
            r = _find_key(v, names)
            if r not in (None, "", 0):
                return r
    elif isinstance(obj, list):
        for v in obj:
            r = _find_key(v, names)
            if r not in (None, "", 0):
                return r
    return None


def import_elegooslicer_login(st: dict, force: bool = False) -> bool:
    """Copy tokens from ElegooSlicer when its file is newer than what we imported. Returns True if updated."""
    path = elegooslicer_user_file()
    if path is None:
        if force:
            raise CloudError("ElegooSlicer sign-in not found. Sign in to Elegoo in ElegooSlicer once, or paste tokens.")
        return False
    mtime = int(path.stat().st_mtime)
    if not force and mtime <= int(st.get("imported_from_mtime") or 0):
        return False
    raw = path.read_bytes()
    if path.suffix == ".json":
        text = raw.decode("utf-8")
    elif sys.platform == "win32":
        text = _dpapi_decrypt(raw).decode("utf-8", "replace")
    else:
        text = _aes_cbc_decrypt(raw).decode("utf-8", "replace")
    data = json.loads(text)
    access = _find_key(data, ["token", "accessToken", "access_token"])
    refresh = _find_key(data, ["refreshToken", "refresh_token"])
    user_id = _find_key(data, ["userId", "user_id", "accountId"])
    expire = _find_key(data, ["accessTokenExpireTime", "access_token_expire_time"]) or 0
    region = _find_key(data, ["region"]) or ""
    if not access:
        raise CloudError("ElegooSlicer is not signed in to an Elegoo account (no token in its saved sign-in).")
    st.update({
        "access_token": str(access),
        "refresh_token": str(refresh or ""),
        "user_id": str(user_id or ""),
        "access_expire_ms": int(expire or 0),
        "imported_from_mtime": mtime,
    })
    if str(region).upper() in ("CN", "CHINA"):
        st["region"] = "CN"
    save_state(st)
    log("Imported Elegoo sign-in from ElegooSlicer (user %s)" % _mask(st["user_id"]))
    return True


# ---------------------------------------------------------------------------------------------
# Elegoo IoT HTTP API
# ---------------------------------------------------------------------------------------------

class ElegooHttp:
    def __init__(self, st: dict):
        self.st = st
        self.base = API_CHINA if st.get("region") == "CN" else API_GLOBAL
        self.user_agent = "ElegooCloudPlugin/%s (%s)" % (PLUGIN_VERSION, sys.platform)

    # -- low level -----------------------------------------------------------------------------
    def _request(self, method: str, path: str, body=None, auth=True, timeout=30) -> dict:
        url = self.base + path
        headers = {"User-Agent": self.user_agent, "Accept": "application/json"}
        data = None
        if body is not None:
            data = json.dumps(body).encode("utf-8")
            headers["Content-Type"] = "application/json"
        if auth:
            headers["Authorization"] = "Bearer " + self.st.get("access_token", "")
        req = urllib.request.Request(url, data=data, method=method, headers=headers)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                payload = r.read()
        except urllib.error.HTTPError as e:
            text = e.read()[:400].decode("utf-8", "replace")
            if e.code in (401, 403):
                raise CloudError("Elegoo rejected the sign-in (HTTP %d). Re-import or paste a fresh token." % e.code)
            raise CloudError("Elegoo API %s %s failed: HTTP %d %s" % (method, path.split("?")[0], e.code, text))
        except urllib.error.URLError as e:
            raise CloudError("Cannot reach %s: %s" % (self.base, e.reason))
        try:
            j = json.loads(payload.decode("utf-8"))
        except ValueError:
            raise CloudError("Unexpected reply from Elegoo API for %s" % path.split("?")[0])
        code = j.get("code", -1)
        if code != 0:
            if code in (401, 403):
                raise CloudError("Elegoo rejected the sign-in (code %s). Re-import or paste a fresh token." % code)
            raise CloudError("Elegoo API %s: code %s %s" % (path.split("?")[0], code, j.get("message", "")))
        return j

    def call(self, method: str, path: str, body=None) -> dict:
        self.ensure_token()
        try:
            return self._request(method, path, body)
        except CloudError as e:
            if "rejected the sign-in" in str(e) and self.st.get("refresh_token"):
                self.refresh()
                return self._request(method, path, body)
            raise

    # -- auth ----------------------------------------------------------------------------------
    def ensure_token(self) -> None:
        # Prefer a newer sign-in from ElegooSlicer (it may have rotated the refresh token).
        try:
            import_elegooslicer_login(self.st)
        except Exception as e:
            log("Import from ElegooSlicer skipped: %s" % e)
        if not self.st.get("access_token"):
            raise CloudError("Not signed in. Press 'Import from ElegooSlicer' or paste a token.")
        exp = int(self.st.get("access_expire_ms") or 0)
        if exp < 1_000_000_000:  # not an absolute time (e.g. "expires in"): rely on a 401 instead
            exp = 0
        elif exp < 10_000_000_000:  # seconds, not ms
            exp *= 1000
        if exp and exp - 60_000 < _now_ms() and self.st.get("refresh_token"):
            self.refresh()

    def refresh(self) -> None:
        log("Refreshing Elegoo access token")
        j = self._request("POST", "/api/v1/account-center-server/account-auth/token/refresh",
                          {"refreshToken": self.st.get("refresh_token", ""), "clientId": "Slicer"})
        d = j.get("data") or {}
        if not d.get("accessToken"):
            raise CloudError("Token refresh returned no access token; sign in again in ElegooSlicer and re-import.")
        self.st.update({
            "access_token": d["accessToken"],
            "refresh_token": d.get("refreshToken") or self.st.get("refresh_token", ""),
            "access_expire_ms": int(d.get("expiresTime") or 0),
            "user_id": str(d.get("accountId") or self.st.get("user_id", "")),
        })
        save_state(self.st)

    # -- endpoints -----------------------------------------------------------------------------
    def printers(self) -> list[dict]:
        j = self.call("GET", "/api/v1/device-management-server/device/list")
        out = []
        for it in j.get("data") or []:
            out.append({
                "serial": it.get("serialNo", ""),
                "model": it.get("pcode", ""),
                "name": it.get("deviceName") or it.get("pcode") or it.get("serialNo", ""),
                "online": it.get("onlineStatus", it.get("online")),
            })
        return out

    def agora_credential(self) -> dict:
        j = self.call("GET", "/api/v1/device-management-server/device/list/agora-token?source=slicer")
        t = (j.get("data") or {}).get("agoraToken") or {}
        cred = {
            "user_id": str(t.get("userId", "")),
            "rtm_user_id": str(t.get("rtmUserId", "")),
            "rtm_token": t.get("rtmToken", ""),
        }
        if not cred["rtm_token"] or not cred["rtm_user_id"]:
            raise CloudError("Elegoo did not return an RTM token for this account.")
        return cred

    def file_detail(self, serial: str, filename: str) -> dict | None:
        try:
            j = self.call("GET", "/api/v1/device-management-server/local-file/filename?serialNo=%s&filename=%s"
                          % (urllib.parse.quote(serial, safe=""), urllib.parse.quote(filename, safe="")))
            return j.get("data") or None
        except CloudError:
            return None

    def printer_report(self, serial: str) -> dict:
        """The printer's last status report as stored by the cloud, as {group: {field: value}}.

        Groups follow the printer's own status message: machine_status, print_status, extruder,
        heater_bed, ztemperature_sensor, fans, ...
        """
        j = self.call("GET", "/api/v1/device-management-server/device/report-data/list?deviceCode="
                      + urllib.parse.quote(serial, safe=""))
        out: dict = {}
        for group, items in (j.get("data") or {}).items():
            if not isinstance(items, list):
                continue
            fields = out.setdefault(group, {})
            for item in items:
                key = item.get("reportLinkKey")
                if not key:
                    continue
                raw = item.get("reportValue")
                try:
                    fields[key] = json.loads(raw) if isinstance(raw, str) else raw
                except ValueError:
                    fields[key] = raw
                fields.setdefault("_updated", 0)
                fields["_updated"] = max(fields["_updated"], int(item.get("updateTime") or 0))
        return out

    def printer_state(self, serial: str) -> int | None:
        """machine_status.status from the cloud's last report: 1 idle, 2 printing, None unknown."""
        try:
            v = self.printer_report(serial).get("machine_status", {}).get("status")
            return int(v) if v is not None else None
        except (CloudError, TypeError, ValueError):
            return None

    def upload_to_storage(self, server_name: str, path: Path, progress=None, cancelled=None) -> str:
        md5_b64 = base64.b64encode(_file_md5(path).digest()).decode()
        j = self.call("GET", "/api/v1/device-management-server/oss/biz-entrypoint?filename=%s&bucketAlias=iot-private"
                      "&module=gcode&fileMd5=%s" % (urllib.parse.quote(server_name, safe=""),
                                                     urllib.parse.quote(md5_b64, safe="")))
        d = j.get("data") or {}
        entry, access = d.get("entrypoint"), d.get("accessUrl")
        if not entry or not access:
            raise CloudError("Elegoo did not provide an upload slot.")
        size = path.stat().st_size
        if size >= 500 * 1024 * 1024:
            raise CloudError("Files of 500 MB or more need multipart upload, which this plugin does not do yet.")

        class _Reader:
            def __init__(self, f):
                self.f, self.sent = f, 0

            def read(self, n=-1):
                if cancelled and cancelled():
                    raise CloudError("Cancelled")
                chunk = self.f.read(256 * 1024 if n is None or n < 0 else min(n, 256 * 1024))
                self.sent += len(chunk)
                if progress:
                    progress(self.sent, size)
                return chunk

        with open(path, "rb") as f:
            req = urllib.request.Request(entry, data=_Reader(f), method="PUT", headers={
                "Content-Type": "application/octet-stream",
                "Content-MD5": md5_b64,
                "Content-Length": str(size),
                # Cloudflare in front of the storage bucket rejects Python's default "Python-urllib"
                # identity with "error code: 1010".
                "User-Agent": self.user_agent,
            })
            try:
                with urllib.request.urlopen(req, timeout=300) as r:
                    if not (200 <= r.status < 300):
                        raise CloudError("Cloud storage upload failed: HTTP %d" % r.status)
            except urllib.error.HTTPError as e:
                raise CloudError("Cloud storage upload failed: HTTP %d %s" % (e.code, e.read()[:200]))
        return access


def _file_md5(path: Path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h


# ---------------------------------------------------------------------------------------------
# Agora RTM 2.x through ctypes (no Python SDK exists)
#   Struct layouts and vtable order from IAgoraRtmClient.h / IAgoraRtmEventHandler.h / AgoraRtmBase.h, v2.2.8.
# ---------------------------------------------------------------------------------------------

c = ctypes


class _RtmLogConfig(c.Structure):
    _fields_ = [("filePath", c.c_char_p), ("fileSizeInKB", c.c_uint32), ("level", c.c_int32)]


class _RtmProxyConfig(c.Structure):
    _fields_ = [("proxyType", c.c_int32), ("server", c.c_char_p), ("port", c.c_uint16),
                ("account", c.c_char_p), ("password", c.c_char_p)]


class _RtmEncryptionConfig(c.Structure):
    _fields_ = [("encryptionMode", c.c_int32), ("encryptionKey", c.c_char_p), ("encryptionSalt", c.c_uint8 * 32)]


class _RtmPrivateConfig(c.Structure):
    _fields_ = [("serviceType", c.c_int32), ("accessPointHosts", c.c_void_p), ("accessPointHostsCount", c.c_size_t)]


class _RtmConfig(c.Structure):
    _fields_ = [
        ("appId", c.c_char_p), ("userId", c.c_char_p),
        ("areaCode", c.c_uint32), ("protocolType", c.c_int32),
        ("presenceTimeout", c.c_uint32), ("heartbeatInterval", c.c_uint32),
        ("context", c.c_void_p), ("reconnectTimeout", c.c_uint32),
        ("useStringUserId", c.c_bool), ("multipath", c.c_bool), ("ispPolicyEnabled", c.c_bool),
        ("eventHandler", c.c_void_p),
        ("logConfig", _RtmLogConfig), ("proxyConfig", _RtmProxyConfig),
        ("encryptionConfig", _RtmEncryptionConfig), ("privateConfig", _RtmPrivateConfig),
    ]


class _PublishOptions(c.Structure):
    _fields_ = [("channelType", c.c_int32), ("messageType", c.c_int32), ("customType", c.c_char_p),
                ("storeInHistory", c.c_bool)]


class _SubscribeOptions(c.Structure):
    _fields_ = [("withMessage", c.c_bool), ("withMetadata", c.c_bool), ("withPresence", c.c_bool),
                ("withLock", c.c_bool), ("beQuiet", c.c_bool)]


class _LinkStateEvent(c.Structure):
    _fields_ = [("currentState", c.c_int32), ("previousState", c.c_int32), ("serviceType", c.c_int32),
                ("operation", c.c_int32), ("reasonCode", c.c_int32), ("reason", c.c_char_p),
                ("affectedChannels", c.c_void_p), ("affectedChannelCount", c.c_size_t),
                ("unrestoredChannels", c.c_void_p), ("unrestoredChannelCount", c.c_size_t),
                ("isResumed", c.c_bool), ("timestamp", c.c_uint64)]


class _MessageEvent(c.Structure):
    _fields_ = [("channelType", c.c_int32), ("messageType", c.c_int32), ("channelName", c.c_char_p),
                ("channelTopic", c.c_char_p), ("message", c.c_void_p), ("messageLength", c.c_size_t),
                ("publisher", c.c_char_p), ("customType", c.c_char_p), ("timestamp", c.c_uint64)]


RTM_AREA_CODE_GLOB = 0xFFFFFFFF
RTM_CHANNEL_TYPE_USER = 3
RTM_MESSAGE_TYPE_STRING = 1
RTM_LINK_STATE_CONNECTED = 2
RTM_LINK_STATE_FAILED = 5
RTM_LOG_LEVEL_WARN = 2

# IRtmClient vtable (its destructor is declared last, so the order is the same on MSVC and Itanium)
_VT_RELEASE, _VT_LOGIN, _VT_LOGOUT = 0, 1, 2
_VT_RENEW_TOKEN, _VT_PUBLISH, _VT_SUBSCRIBE = 7, 8, 9

# IRtmEventHandler: virtual destructor first (1 slot on MSVC, 2 on Itanium), then 47 methods.
_EH_DTOR_SLOTS = 1 if sys.platform == "win32" else 2
_EH_METHODS = 47
_EH_LINK_STATE, _EH_MESSAGE, _EH_SUBSCRIBE_RESULT, _EH_PUBLISH_RESULT, _EH_LOGIN_RESULT = 0, 1, 17, 19, 20

_FN_EVENT = c.CFUNCTYPE(None, c.c_void_p, c.c_void_p)
_FN_REQ_ERR = c.CFUNCTYPE(None, c.c_void_p, c.c_uint64, c.c_int32)
_FN_REQ_STR_ERR = c.CFUNCTYPE(None, c.c_void_p, c.c_uint64, c.c_char_p, c.c_int32)
_FN_NOOP = c.CFUNCTYPE(None, c.c_void_p)


def _agora_candidate_dirs(st: dict) -> list[Path]:
    dirs = []
    if st.get("agora_dir"):
        dirs.append(Path(st["agora_dir"]))
    dirs.append(DATA_DIR / "agora")
    if sys.platform == "win32":
        for env in ("ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"):
            root = os.environ.get(env)
            if not root:
                continue
            for sub in ("ElegooSlicer", "ELEGOO\\ElegooSlicer", "Programs\\ElegooSlicer", "Elegoo\\ElegooSlicer"):
                dirs.append(Path(root) / sub)
    elif sys.platform == "darwin":
        dirs.append(Path("/Applications/ElegooSlicer.app/Contents/Frameworks"))
    return dirs


def _agora_lib_names() -> tuple[str, str]:
    if sys.platform == "win32":
        return "agora_rtm_sdk.dll", "libaosl.dll"
    if sys.platform == "darwin":
        return "AgoraRtmKit.framework/AgoraRtmKit", "aosl.framework/aosl"
    return "libagora_rtm_sdk.so", "libaosl.so"


def find_agora_dir(st: dict) -> Path | None:
    main, _ = _agora_lib_names()
    for d in _agora_candidate_dirs(st):
        if (d / main).exists():
            return d
        try:  # one level deep, e.g. ...\ElegooSlicer\bin
            for sub in d.iterdir() if d.is_dir() else []:
                if (sub / main).exists():
                    return sub
        except OSError:
            pass
    return None


def download_agora_libs() -> Path:
    """Fetch the Agora RTM 2.2.8 runtime that Elegoo publishes in its open-source elegoo-link repository."""
    target = DATA_DIR / "agora"
    target.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        files = {"agora_rtm_sdk.dll": "windows/lib/x86_64/agora_rtm_sdk.dll",
                 "libaosl.dll": "windows/lib/x86_64/libaosl.dll"}
    elif sys.platform.startswith("linux"):
        files = {"libagora_rtm_sdk.so": "linux/libagora_rtm_sdk.so", "libaosl.so": "linux/libaosl.so"}
    else:
        raise CloudError("Automatic download is only set up for Windows and Linux; install ElegooSlicer instead.")
    for name, rel in files.items():
        log("Downloading %s" % name)
        req = urllib.request.Request(ELEGOO_LINK_RAW + "/" + rel, headers={"User-Agent": "ElegooCloudPlugin"})
        with urllib.request.urlopen(req, timeout=120) as r, open(target / (name + ".part"), "wb") as f:
            shutil.copyfileobj(r, f)
        os.replace(target / (name + ".part"), target / name)
    return target


class AgoraRtm:
    """Minimal RTM 2.x client: login, publish to a user channel, receive messages."""

    def __init__(self, lib_dir: Path, user_id: str, on_message):
        self._on_message = on_message
        self._cond = threading.Condition()
        self._results: dict[int, int] = {}
        self._link_state = 0
        self._client = None
        main, aosl = _agora_lib_names()
        if sys.platform == "win32":
            self._dll_dir_handle = os.add_dll_directory(str(lib_dir))
            c.CDLL(str(lib_dir / aosl))
        else:
            c.CDLL(str(lib_dir / aosl), mode=c.RTLD_GLOBAL)
        self._lib = c.CDLL(str(lib_dir / main))
        self._lib.createAgoraRtmClient.restype = c.c_void_p
        self._lib.createAgoraRtmClient.argtypes = [c.POINTER(_RtmConfig), c.POINTER(c.c_int)]
        self._lib.getVersion.restype = c.c_char_p
        log("Agora RTM library %s loaded from %s" % (self._lib.getVersion().decode(), lib_dir))
        self._build_handler()

        cfg = _RtmConfig()
        self._keep = [AGORA_APP_ID.encode(), user_id.encode(), str(DATA_DIR / "agora_rtm.log").encode()]
        cfg.appId, cfg.userId = self._keep[0], self._keep[1]
        cfg.areaCode = RTM_AREA_CODE_GLOB
        cfg.protocolType = 0
        cfg.presenceTimeout = 300
        cfg.heartbeatInterval = 30
        cfg.reconnectTimeout = 15
        cfg.useStringUserId = True
        cfg.eventHandler = c.addressof(self._handler)
        cfg.logConfig.filePath = self._keep[2]
        cfg.logConfig.fileSizeInKB = 1024
        cfg.logConfig.level = RTM_LOG_LEVEL_WARN
        err = c.c_int(0)
        ptr = self._lib.createAgoraRtmClient(c.byref(cfg), c.byref(err))
        if not ptr or err.value != 0:
            raise CloudError("Agora RTM client could not be created (error %d)" % err.value)
        self._client = ptr

    # vtable helpers ---------------------------------------------------------------------------
    def _vcall(self, index, restype, argtypes, *args):
        vtbl = c.cast(self._client, c.POINTER(c.POINTER(c.c_void_p)))[0]
        fn = c.CFUNCTYPE(restype, c.c_void_p, *argtypes)(vtbl[index])
        return fn(self._client, *args)

    def _build_handler(self):
        total = _EH_DTOR_SLOTS + _EH_METHODS + 8  # spare slots in case a newer runtime appends methods
        noop = _FN_NOOP(lambda this: None)
        slots = [c.cast(noop, c.c_void_p).value] * total

        def link_state(this, ev_ptr):
            try:
                ev = c.cast(ev_ptr, c.POINTER(_LinkStateEvent)).contents
                with self._cond:
                    self._link_state = ev.currentState
                    self._cond.notify_all()
                log("RTM link state %d -> %d (%s)" % (ev.previousState, ev.currentState,
                                                     (ev.reason or b"").decode("utf-8", "replace")))
            except Exception:
                log(traceback.format_exc())

        def message(this, ev_ptr):
            try:
                ev = c.cast(ev_ptr, c.POINTER(_MessageEvent)).contents
                body = c.string_at(ev.message, ev.messageLength).decode("utf-8", "replace") if ev.message else ""
                pub = (ev.publisher or b"").decode("utf-8", "replace")
                self._on_message(pub, body)
            except Exception:
                log(traceback.format_exc())

        def req_result(this, request_id, error):
            with self._cond:
                self._results[int(request_id)] = int(error)
                self._cond.notify_all()

        def sub_result(this, request_id, channel, error):
            req_result(this, request_id, error)

        self._cb = {
            "noop": noop,
            "link": _FN_EVENT(link_state),
            "msg": _FN_EVENT(message),
            "login": _FN_REQ_ERR(req_result),
            "publish": _FN_REQ_ERR(req_result),
            "subscribe": _FN_REQ_STR_ERR(sub_result),
        }
        base = _EH_DTOR_SLOTS
        slots[base + _EH_LINK_STATE] = c.cast(self._cb["link"], c.c_void_p).value
        slots[base + _EH_MESSAGE] = c.cast(self._cb["msg"], c.c_void_p).value
        slots[base + _EH_LOGIN_RESULT] = c.cast(self._cb["login"], c.c_void_p).value
        slots[base + _EH_PUBLISH_RESULT] = c.cast(self._cb["publish"], c.c_void_p).value
        slots[base + _EH_SUBSCRIBE_RESULT] = c.cast(self._cb["subscribe"], c.c_void_p).value
        self._vtable = (c.c_void_p * total)(*slots)

        class _Handler(c.Structure):
            _fields_ = [("vtbl", c.c_void_p)]

        self._handler = _Handler(c.addressof(self._vtable))

    def _wait_result(self, request_id: int, timeout: float, what: str) -> None:
        deadline = time.monotonic() + timeout
        with self._cond:
            while request_id not in self._results:
                left = deadline - time.monotonic()
                if left <= 0:
                    raise CloudError("Agora RTM %s timed out" % what)
                self._cond.wait(left)
            err = self._results.pop(request_id)
        if err != 0:
            reason = ""
            try:
                self._lib.getErrorReason.restype = c.c_char_p
                reason = (self._lib.getErrorReason(err) or b"").decode()
            except Exception:
                pass
            raise CloudError("Agora RTM %s failed: error %d %s" % (what, err, reason))

    # public API -------------------------------------------------------------------------------
    def login(self, token: str, timeout=20.0) -> None:
        rid = c.c_uint64(0)
        self._tok = token.encode()
        self._vcall(_VT_LOGIN, None, [c.c_char_p, c.POINTER(c.c_uint64)], self._tok, c.byref(rid))
        self._wait_result(rid.value, timeout, "login")

    def subscribe(self, channel: str, timeout=10.0) -> None:
        opts = _SubscribeOptions(True, False, True, False, False)
        rid = c.c_uint64(0)
        self._vcall(_VT_SUBSCRIBE, None, [c.c_char_p, c.POINTER(_SubscribeOptions), c.POINTER(c.c_uint64)],
                    channel.encode(), c.byref(opts), c.byref(rid))
        self._wait_result(rid.value, timeout, "subscribe")

    def publish_to_user(self, user: str, message: str, timeout=10.0) -> None:
        opts = _PublishOptions(RTM_CHANNEL_TYPE_USER, RTM_MESSAGE_TYPE_STRING, b"PlainText", False)
        data = message.encode("utf-8")
        rid = c.c_uint64(0)
        self._vcall(_VT_PUBLISH, None,
                    [c.c_char_p, c.c_char_p, c.c_size_t, c.POINTER(_PublishOptions), c.POINTER(c.c_uint64)],
                    user.encode(), data, len(data), c.byref(opts), c.byref(rid))
        self._wait_result(rid.value, timeout, "publish")

    def close(self) -> None:
        if self._client:
            try:
                rid = c.c_uint64(0)
                self._vcall(_VT_LOGOUT, None, [c.POINTER(c.c_uint64)], c.byref(rid))
                time.sleep(0.3)
                self._vcall(_VT_RELEASE, c.c_int, [])
            except Exception:
                log(traceback.format_exc())
            self._client = None


# ---------------------------------------------------------------------------------------------
# Printer session over RTM: request/response by integer id, plus download progress
# ---------------------------------------------------------------------------------------------

class PrinterChannel:
    def __init__(self, st: dict, http: ElegooHttp, serial: str, status=None):
        self.st, self.http, self.serial = st, http, serial
        self.status = status or (lambda msg: None)
        self._cond = threading.Condition()
        self._responses: dict[int, dict] = {}
        self._download = None  # last 6006 result
        self._next_id = int(time.time()) % 100000 * 10
        self.rtm = None

    def __enter__(self):
        lib_dir = find_agora_dir(self.st)
        if lib_dir is None:
            self.status("Agora messaging library not found; downloading it from Elegoo's elegoo-link repository…")
            lib_dir = download_agora_libs()
        cred = self.http.agora_credential()
        self.cred = cred
        self.printer_user = cred["user_id"] + self.serial
        self.rtm = AgoraRtm(lib_dir, cred["rtm_user_id"], self._on_message)
        self.status("Connecting to Elegoo messaging…")
        self.rtm.login(cred["rtm_token"])
        try:
            self.rtm.subscribe(cred["user_id"])
        except CloudError as e:  # replies come as user-channel messages anyway
            log("Subscribe skipped: %s" % e)
        return self

    def __exit__(self, *exc):
        if self.rtm:
            self.rtm.close()
            self.rtm = None

    def _on_message(self, publisher: str, body: str):
        log("RTM <- %s: %s" % (_mask(publisher), body[:300]))
        if publisher and publisher != self.printer_user:
            return
        try:
            j = json.loads(body)
        except ValueError:
            return
        with self._cond:
            if j.get("method") == M_DOWNLOAD_PROGRESS:
                self._download = dict(j.get("result") or {}, _t=time.monotonic())
            elif "id" in j:
                self._responses[int(j["id"])] = j
            self._cond.notify_all()

    def request(self, method: int, params: dict, timeout=10.0, need_reply=True) -> dict:
        with self._cond:
            self._next_id += 1
            rid = self._next_id
        msg = json.dumps({"id": rid, "method": method, "params": params}, separators=(",", ":"))
        log("RTM -> printer: %s" % msg[:300])
        self.rtm.publish_to_user(self.printer_user, msg)
        if not need_reply:
            return {}
        deadline = time.monotonic() + timeout
        with self._cond:
            while rid not in self._responses:
                left = deadline - time.monotonic()
                if left <= 0:
                    raise CloudError("Printer did not answer method %d within %.0f s (is it online?)" % (method, timeout))
                self._cond.wait(left)
            reply = self._responses.pop(rid)
        code = (reply.get("result") or {}).get("error_code", 0)
        if code:
            raise CloudError("Printer refused method %d: %s (error %s)" % (method, PRINTER_ERRORS.get(code, "error"), code))
        return reply

    def download_state(self):
        with self._cond:
            return dict(self._download) if self._download else None

    def reset_download(self):
        with self._cond:
            self._download = None


# ---------------------------------------------------------------------------------------------
# The whole job: upload to cloud storage, printer downloads it, then start the print
# ---------------------------------------------------------------------------------------------

def send_to_printer(path: Path, display_name: str, st: dict, start_print=True,
                    progress=None, status=None, cancelled=None) -> str:
    progress = progress or (lambda pct: None)
    status = status or (lambda msg: None)
    cancelled = cancelled or (lambda: False)
    path = Path(path)
    if not path.exists():
        raise CloudError("File not found: %s" % path)
    name = Path(display_name or path.name).name
    if not name.lower().endswith((".gcode", ".3mf")):
        name += ".gcode"

    http = ElegooHttp(st)
    status("Checking sign-in…")
    http.ensure_token()
    serial = st.get("printer_serial")
    if not serial:
        printers = http.printers()
        if len(printers) != 1:
            raise CloudError("Choose a printer first (%d bound to this account)." % len(printers))
        serial = printers[0]["serial"]
    if http.printer_state(serial) == 2 and start_print:
        raise CloudError("The printer reports it is printing. Wait for it to finish, or use 'Upload only'.")

    hex_md5 = _file_md5(path).hexdigest()
    uid = str(st.get("user_id") or "")[-6:]
    server_name = "%s_%s_%s%s" % (uid, serial[-6:], hex_md5, Path(name).suffix.lower())
    before = (http.file_detail(serial, name) or {}).get("createTime")

    status("Uploading %s to Elegoo cloud…" % name)
    url = http.upload_to_storage(server_name, path,
                                 progress=lambda sent, total: progress(int(sent * 50 / max(total, 1))),
                                 cancelled=cancelled)
    log("Uploaded %s (%d bytes) to cloud storage" % (name, path.stat().st_size))

    with PrinterChannel(st, http, serial, status) as ch:
        task_id = serial
        try:
            ch.request(M_CANCEL_DOWNLOAD, {"taskID": task_id}, timeout=5)
        except CloudError:
            pass  # nothing to cancel
        time.sleep(1)
        ch.reset_download()
        status("Printer is downloading the file…")
        ch.request(M_DOWNLOAD_FILE, {"filename": name, "url": url, "md5": hex_md5, "taskID": task_id}, timeout=15)

        last_change = time.monotonic()
        last_pct = -1
        done = False
        while not done:
            if cancelled():
                ch.request(M_CANCEL_DOWNLOAD, {"taskID": task_id}, need_reply=False)
                raise CloudError("Cancelled")
            time.sleep(0.5)
            d = ch.download_state()
            if d:
                pct = int(d.get("progress", 0))
                if pct != last_pct:
                    last_pct, last_change = pct, time.monotonic()
                    progress(50 + pct // 2)
                s = int(d.get("status", 0))
                if s == 1:
                    done = True
                elif s == 2:
                    raise CloudError("The printer cancelled the download.")
                elif s == 3:
                    raise CloudError("The printer failed to download the file.")
            if not done and time.monotonic() - last_change > 60:
                # No RTM progress for a minute: check whether the file landed anyway.
                detail = http.file_detail(serial, name)
                if detail and detail.get("createTime") != before:
                    done = True
                else:
                    raise CloudError("No download progress from the printer for 60 s.")
        progress(100)
        if not start_print:
            return "Uploaded %s to the printer." % name

        status("Starting print…")
        params = {
            "storage_media": "local",
            "filename": name,
            "config": {
                "delay_video": bool(st.get("timelapse")),
                "printer_check": bool(st.get("auto_bed_leveling", True)),
                "print_layout": "B" if st.get("plate_side") == "B" else "A",
                "bedlevel_force": False,
                "slot_map": [],
            },
        }
        try:
            ch.request(M_START_PRINT, params, timeout=10)
        except CloudError as e:
            if "error 1026" in str(e) and params["config"]["printer_check"]:
                params["config"]["bedlevel_force"] = True
                ch.request(M_START_PRINT, params, timeout=10)
            else:
                raise
    return "Printing %s on %s." % (name, serial)


# ---------------------------------------------------------------------------------------------
# Slicer integration
# ---------------------------------------------------------------------------------------------

PAGE = r"""
<style>
  body { margin:0; padding:12px; font-size:13px; }
  h3 { margin:0 0 6px; font-size:14px; }
  section { margin-bottom:14px; }
  .muted { color: var(--orca-muted); font-size:12px; }
  .row { display:flex; gap:6px; align-items:center; flex-wrap:wrap; margin:4px 0; }
  select, input[type=text] { flex:1; min-width:0; }
  button.quiet { background:transparent; color:var(--orca-fg); border-color:var(--orca-border); }
  progress { width:100%; }
  #log { white-space:pre-wrap; font-size:11px; color:var(--orca-muted); max-height:140px; overflow:auto; }
  details { margin-top:4px; }
</style>
<section>
  <h3>Elegoo account</h3>
  <div id="acct" class="muted">…</div>
  <div class="row">
    <button type="button" id="import">Import from ElegooSlicer</button>
  </div>
  <details><summary class="muted">Paste a token instead</summary>
    <div class="row"><input type="text" id="tok" placeholder="access token"></div>
    <div class="row"><input type="text" id="rtok" placeholder="refresh token (optional)"></div>
    <div class="row"><input type="text" id="uid" placeholder="account id (optional)"></div>
    <div class="row"><button type="button" id="savetok" class="quiet">Save token</button>
      <label><input type="checkbox" id="cn"> China region</label></div>
  </details>
</section>
<section>
  <h3>Printer</h3>
  <div class="row"><select id="printer"></select><button type="button" id="refresh" class="quiet">Refresh</button></div>
</section>
<section>
  <h3>Print</h3>
  <div id="file" class="muted">Sends the current slice. Slice the plate first.</div>
  <div class="row"><label><input type="checkbox" id="abl"> Auto bed leveling</label>
    <label><input type="checkbox" id="tl"> Timelapse</label></div>
  <div class="row">Plate side <select id="side" style="flex:0"><option>A</option><option>B</option></select></div>
  <div class="row"><label><input type="checkbox" id="auto"> Send automatically after each Export G-code</label></div>
  <div class="row"><button type="button" id="print">Upload &amp; print</button>
    <button type="button" id="upload" class="quiet">Upload only</button>
    <button type="button" id="cancel" class="quiet">Cancel</button></div>
  <progress id="prog" max="100" value="0"></progress>
  <div id="status" class="muted"></div>
</section>
<section>
  <h3>Printer status</h3>
  <div id="pstate" style="font-weight:600">…</div>
  <div id="pfile" class="muted"></div>
  <progress id="pprog" max="100" value="0"></progress>
  <table style="width:100%;font-size:12px">
    <tr><td class="muted">Progress</td><td id="ppct"></td><td class="muted">Layer</td><td id="player"></td></tr>
    <tr><td class="muted">Time left</td><td id="pleft"></td><td class="muted">Nozzle</td><td id="pnoz"></td></tr>
    <tr><td class="muted">Bed</td><td id="pbed"></td><td class="muted">Chamber</td><td id="pch"></td></tr>
  </table>
  <div class="muted" id="pupd"></div>
</section>
<details><summary class="muted">Log</summary><div id="log"></div></details>
<script>
(function () {
  var $ = function (id) { return document.getElementById(id); };
  function send(cmd, extra) { var m = extra || {}; m.command = cmd; orca.postMessage(m); }
  function opts() {
    return { auto_bed_leveling: $("abl").checked, timelapse: $("tl").checked, plate_side: $("side").value,
             auto_send_on_export: $("auto").checked, printer_serial: $("printer").value };
  }
  orca.onMessage(function (m) {
    if (!m) return;
    if (m.command === "state") {
      $("acct").textContent = m.account;
      var sel = $("printer"); sel.innerHTML = "";
      (m.printers || []).forEach(function (p) {
        var o = document.createElement("option"); o.value = p.serial;
        o.textContent = p.name + " (" + p.serial + ")" + (p.online === false || p.online === 0 ? " offline" : "");
        sel.appendChild(o);
      });
      if (m.printer_serial) sel.value = m.printer_serial;
      $("abl").checked = !!m.auto_bed_leveling; $("tl").checked = !!m.timelapse;
      $("side").value = m.plate_side || "A"; $("auto").checked = !!m.auto_send_on_export;
      $("cn").checked = m.region === "CN";
      $("file").textContent = m.file || "Sends the current slice. Slice the plate first.";
    } else if (m.command === "printer_status") {
      var s = m.status || {};
      $("pstate").textContent = m.error ? m.error : s.state;
      $("pfile").textContent = s.file || "";
      $("pprog").value = s.progress || 0;
      $("ppct").textContent = (s.progress || 0) + "%";
      $("player").textContent = s.layer || "–";
      $("pleft").textContent = s.time_left || "–";
      $("pnoz").textContent = s.nozzle || "–";
      $("pbed").textContent = s.bed || "–";
      $("pch").textContent = s.chamber || "–";
      $("pupd").textContent = s.updated ? "Printer report from " + s.updated + " (refreshes every 10 s)" : "";
    } else if (m.command === "progress") {
      $("prog").value = m.value;
    } else if (m.command === "status") {
      $("status").textContent = m.text;
      if (m.log) { $("log").textContent = m.log; }
    }
  });
  ["abl", "tl", "side", "auto", "printer"].forEach(function (id) {
    $(id).addEventListener("change", function () { send("options", opts()); });
  });
  $("import").onclick = function () { send("import"); };
  $("refresh").onclick = function () { send("refresh"); };
  $("savetok").onclick = function () {
    send("token", { access_token: $("tok").value.trim(), refresh_token: $("rtok").value.trim(),
                    user_id: $("uid").value.trim(), region: $("cn").checked ? "CN" : "global" });
  };
  $("print").onclick = function () { send("send", { start: true }); };
  $("upload").onclick = function () { send("send", { start: false }); };
  $("cancel").onclick = function () { send("cancel"); };
  send("hello");
})();
</script>
"""


SUB_STATUS_TEXT = {
    1081: "Downloading file", 1082: "Downloading file", 1086: "Downloading file",
    1045: "Heating nozzle", 1096: "Heating nozzle", 1405: "Heating bed", 1906: "Heating bed",
    2801: "Homing", 2802: "Homing", 2901: "Auto bed leveling", 2902: "Auto bed leveling",
    2501: "Pausing", 2502: "Paused", 2505: "Paused", 2401: "Resuming", 2402: "Resuming",
    2075: "Printing", 2077: "Print finished", 2503: "Stopping", 2504: "Stopped",
}


def _num(v, default=0.0):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def summarize_report(rep: dict) -> dict:
    ms, ps = rep.get("machine_status", {}), rep.get("print_status", {})
    status, sub = ms.get("status"), ms.get("sub_status")
    try:
        sub = int(sub)
    except (TypeError, ValueError):
        sub = None
    if status in (2, "2"):
        state = SUB_STATUS_TEXT.get(sub, "Printing")
    elif status in (1, "1"):
        state = "Print finished" if sub == 2077 else "Idle"
    elif status in (0, "0"):
        state = "Starting up"
    else:
        state = "Unknown" if status is None else "Busy (status %s)" % status
    if ms.get("exception_status"):
        state += " (alert %s)" % ms.get("exception_status")
    left = int(_num(ps.get("remaining_time_sec")))
    updated = max([g.get("_updated", 0) for g in rep.values() if isinstance(g, dict)] or [0])

    def temp(group):
        g = rep.get(group, {})
        if "temperature" not in g:
            return ""
        t = "%.0f°C" % _num(g.get("temperature"))
        if _num(g.get("target")) > 0:
            t += " / %.0f°C" % _num(g.get("target"))
        return t

    return {
        "state": state,
        "file": ps.get("filename") or "",
        "progress": int(_num(ms.get("progress"))),
        "layer": "%d / %d" % (int(_num(ps.get("current_layer"))), int(_num(ps.get("total_layer"))))
                 if ps.get("total_layer") else "",
        "time_left": ("%dh %02dm" % (left // 3600, left % 3600 // 60)) if left > 0 else "",
        "nozzle": temp("extruder"),
        "bed": temp("heater_bed"),
        "chamber": temp("ztemperature_sensor"),
        "updated": time.strftime("%H:%M:%S", time.localtime(updated / 1000)) if updated > 1e12 else "",
    }


def find_current_slice() -> Path | None:
    """The G-code the slicer wrote for the current slice, before any export.

    The slicer writes it to
      <temp>/orcaslicer_<user>/orcaslicer_model/<date>/<time>#<pid>#<model>/Metadata/.<pid>.<plate>.gcode
    and the plugin runs inside that same process, so os.getpid() picks this window's slice.
    """
    import glob
    import tempfile
    pid = os.getpid()
    roots = {tempfile.gettempdir()}
    for env in ("TEMP", "TMP"):
        if os.environ.get(env):
            roots.add(os.environ[env])
    found = []
    for root in roots:
        for pat in ("orcaslicer_*/orcaslicer_model/*/*#%d#*/Metadata/.%d.*.gcode",
                    "orcaslicer_model/*/*#%d#*/Metadata/.%d.*.gcode"):
            found += glob.glob(os.path.join(root, pat % (pid, pid)))
    found = [Path(f) for f in found if os.path.getsize(f) > 0]
    if not found:
        return None
    newest = max(found, key=lambda f: f.stat().st_mtime)
    if time.time() - newest.stat().st_mtime < 2:  # still being written
        time.sleep(2)
    return newest


def _plate_name() -> str:
    """A file name for the printer's screen, from the project or the first object. UI thread only."""
    try:
        proj = orca.host.plater().project_path()
        if proj:
            return Path(proj).stem + ".gcode"
    except Exception:
        pass
    try:
        objs = list(orca.host.model().objects())
        if objs and objs[0].name:
            return Path(objs[0].name).stem + ".gcode"
    except Exception:
        pass
    return "print.gcode"


def _last_export_info():
    try:
        meta = json.loads(LAST_EXPORT_META.read_text(encoding="utf-8"))
        if LAST_EXPORT.exists():
            return meta
    except Exception:
        pass
    return None


class _Job:
    """One background send at a time, reporting to whichever panel is open."""
    lock = threading.Lock()
    running = False
    cancel = False
    panel_post = None
    printers: list = []
    log_lines: list = []

    @classmethod
    def post(cls, msg):
        fn = cls.panel_post
        if fn:
            try:
                fn(msg)
            except Exception:
                pass

    @classmethod
    def status(cls, text):
        log(text)
        cls.log_lines = (cls.log_lines + [time.strftime("%H:%M:%S ") + text])[-40:]
        cls.post({"command": "status", "text": text, "log": "\n".join(cls.log_lines)})

    @classmethod
    def push_state(cls):
        st = load_state()
        if st.get("access_token"):
            acct = "Signed in" + (" (account %s)" % _mask(st["user_id"]) if st.get("user_id") else "")
            acct += ", %s region." % ("China" if st.get("region") == "CN" else "global")
        else:
            acct = "Not signed in."
        info = _last_export_info()
        file_text = ("Sends the current slice. Last export: %s (%s)" % (info["name"], info["time"])) if info \
            else "Sends the current slice. Slice the plate first."
        cls.post(dict({"command": "state", "account": acct, "printers": cls.printers, "file": file_text},
                      **{k: st.get(k) for k in ("printer_serial", "auto_bed_leveling", "timelapse", "plate_side",
                                                 "auto_send_on_export", "region")}))

    @classmethod
    def run_async(cls, target, *args):
        t = threading.Thread(target=cls._guard, args=(target,) + args, daemon=True)
        t.start()

    @classmethod
    def _guard(cls, target, *args):
        try:
            target(*args)
        except CloudError as e:
            cls.status("Error: %s" % e)
        except Exception as e:
            log(traceback.format_exc())
            cls.status("Error: %s" % e)
        finally:
            cls.push_state()

    poll_stop = None

    @classmethod
    def start_polling(cls):
        if cls.poll_stop is not None:
            return
        stop = threading.Event()
        cls.poll_stop = stop

        def loop():
            while not stop.is_set():
                st = load_state()
                serial = st.get("printer_serial")
                if st.get("access_token") and serial:
                    try:
                        cls.post({"command": "printer_status",
                                  "status": summarize_report(ElegooHttp(st).printer_report(serial))})
                    except Exception as e:
                        cls.post({"command": "printer_status", "error": "Status unavailable: %s" % e})
                stop.wait(10)

        threading.Thread(target=loop, daemon=True).start()

    @classmethod
    def stop_polling(cls):
        if cls.poll_stop is not None:
            cls.poll_stop.set()
            cls.poll_stop = None

    @classmethod
    def refresh_printers(cls):
        st = load_state()
        http = ElegooHttp(st)
        cls.printers = http.printers()
        if cls.printers and st.get("printer_serial") not in [p["serial"] for p in cls.printers]:
            st["printer_serial"] = cls.printers[0]["serial"]
            save_state(st)
        cls.status("Found %d printer(s) on the account." % len(cls.printers))

    @classmethod
    def send(cls, start: bool, name_hint: str = ""):
        with cls.lock:
            if cls.running:
                cls.status("A send is already in progress.")
                return
            cls.running, cls.cancel = True, False
        try:
            info = _last_export_info()
            source, name = None, ""
            try:
                source = find_current_slice()
            except Exception as e:
                log("Looking for the current slice failed: %s" % e)
            # An export newer than the slice wins: it may include post-processing scripts.
            if info and (source is None or LAST_EXPORT.stat().st_mtime >= source.stat().st_mtime):
                source, name = LAST_EXPORT, info["name"]
            if source is None:
                raise CloudError("Nothing to send yet. Slice the plate first.")
            name = name or name_hint or "print.gcode"
            cls.status("Sending %s (%s)" % (name, "exported file" if source == LAST_EXPORT else "current slice"))
            DATA_DIR.mkdir(parents=True, exist_ok=True)
            work = DATA_DIR / ("sending_" + str(os.getpid()) + ".gcode")
            shutil.copyfile(source, work)
            try:
                msg = send_to_printer(work, name, load_state(), start_print=start,
                                      progress=lambda p: cls.post({"command": "progress", "value": p}),
                                      status=cls.status, cancelled=lambda: cls.cancel)
            finally:
                try:
                    work.unlink()
                except OSError:
                    pass
            cls.status(msg)
            try:
                orca.host.ui.push_notification(text="Elegoo Cloud: " + msg)
            except Exception:
                pass
        finally:
            cls.running = False


if orca is not None:

    class ElegooCloudPanel(orca.script.ScriptPluginCapabilityBase):
        panel = None

        def get_name(self):
            return "Elegoo Cloud (CC2)"

        def execute(self):
            if self.panel is not None and self.panel.is_open():
                self.panel.show()
                return orca.ExecutionResult.success("Elegoo Cloud panel is already open.")
            self.panel = orca.host.ui.create_dock_panel(
                html=PAGE, title="Elegoo Cloud (CC2)", width=340, height=620,
                on_message=self.on_message, on_close=self.on_close, dock="right")
            _Job.panel_post = self.panel.post
            return orca.ExecutionResult.success("Elegoo Cloud panel opened.")

        # UI thread: never block here.
        def on_message(self, message):
            m = message or {}
            cmd = m.get("command")
            if cmd == "hello":
                _Job.push_state()
                _Job.start_polling()
                if load_state().get("access_token") and not _Job.printers:
                    _Job.run_async(_Job.refresh_printers)
            elif cmd == "import":
                def _imp():
                    st = load_state()
                    import_elegooslicer_login(st, force=True)
                    _Job.status("Imported your Elegoo sign-in from ElegooSlicer.")
                    _Job.refresh_printers()
                _Job.run_async(_imp)
            elif cmd == "token":
                st = load_state()
                if m.get("access_token"):
                    st.update({"access_token": m["access_token"], "refresh_token": m.get("refresh_token", ""),
                               "user_id": m.get("user_id", ""), "access_expire_ms": 0,
                               "imported_from_mtime": 2 ** 40})  # stop auto-import overriding a pasted token
                st["region"] = m.get("region", "global")
                save_state(st)
                _Job.run_async(_Job.refresh_printers)
            elif cmd == "refresh":
                _Job.run_async(_Job.refresh_printers)
            elif cmd == "options":
                st = load_state()
                for k in ("auto_bed_leveling", "timelapse", "plate_side", "auto_send_on_export", "printer_serial"):
                    if k in m:
                        st[k] = m[k]
                save_state(st)
            elif cmd == "send":
                _Job.run_async(_Job.send, bool(m.get("start", True)), _plate_name())
            elif cmd == "cancel":
                _Job.cancel = True
                _Job.status("Cancelling…")

        def on_close(self):
            self.panel = None
            _Job.panel_post = None
            _Job.stop_polling()

    class ElegooCloudCapture(orca.slicing.SlicingPipelineCapabilityBase):
        """Keeps a copy of every exported G-code so the panel can send it."""

        def get_name(self):
            return "Elegoo Cloud: capture exported G-code"

        def execute(self, ctx):
            if ctx.step != orca.slicing.Step.psGCodePostProcess or not ctx.gcode_path:
                return orca.ExecutionResult.success()
            try:
                DATA_DIR.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(ctx.gcode_path, LAST_EXPORT)
                name = Path(ctx.output_name or ctx.gcode_path).name
                if not name.lower().endswith(".gcode"):
                    name = Path(name).stem + ".gcode"
                LAST_EXPORT_META.write_text(json.dumps({"name": name, "host": ctx.host or "",
                                                        "time": time.strftime("%H:%M:%S")}), encoding="utf-8")
                _Job.push_state()
                if load_state().get("auto_send_on_export") and (ctx.host or "File") == "File":
                    _Job.run_async(_Job.send, True)
            except Exception as e:
                log(traceback.format_exc())
                return orca.ExecutionResult.success("Elegoo Cloud: could not keep a copy of the G-code: %s" % e)
            return orca.ExecutionResult.success()

    @orca.plugin
    class ElegooCloudPlugin(orca.base):
        def register_capabilities(self):
            orca.register_capability(ElegooCloudPanel)
            orca.register_capability(ElegooCloudCapture)


# ---------------------------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------------------------

def _cli(argv: list[str]) -> int:
    import argparse
    p = argparse.ArgumentParser(description="Elegoo cloud printing for Centauri Carbon 2")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("login", help="import the Elegoo sign-in saved by ElegooSlicer")
    tk = sub.add_parser("token", help="store a token you copied yourself")
    tk.add_argument("access_token")
    tk.add_argument("--refresh-token", default="")
    tk.add_argument("--user-id", default="")
    tk.add_argument("--china", action="store_true")
    sub.add_parser("printers", help="list printers bound to the account")
    sub.add_parser("status", help="show the selected printer's status")
    sub.add_parser("rtm-test", help="log in to Agora RTM only (checks the messaging path)")
    s = sub.add_parser("send", help="upload a G-code file and (by default) start printing")
    s.add_argument("file")
    s.add_argument("--printer", help="printer serial number")
    s.add_argument("--upload-only", action="store_true")
    s.add_argument("--no-leveling", action="store_true")
    s.add_argument("--timelapse", action="store_true")
    s.add_argument("--plate-side", choices=["A", "B"])
    a = p.parse_args(argv)
    st = load_state()
    try:
        if a.cmd == "login":
            import_elegooslicer_login(st, force=True)
            print("Signed in as account %s" % _mask(st.get("user_id")))
        elif a.cmd == "token":
            st.update({"access_token": a.access_token, "refresh_token": a.refresh_token, "user_id": a.user_id,
                       "access_expire_ms": 0, "imported_from_mtime": 2 ** 40, "region": "CN" if a.china else "global"})
            save_state(st)
            print("Token saved.")
        elif a.cmd == "printers":
            for pr in ElegooHttp(st).printers():
                print("%s  %s  %s  online=%s" % (pr["serial"], pr["model"], pr["name"], pr["online"]))
        elif a.cmd == "status":
            http = ElegooHttp(st)
            serial = st.get("printer_serial") or (http.printers() or [{}])[0].get("serial")
            if not serial:
                raise CloudError("No printer on this account.")
            for k, v in summarize_report(http.printer_report(serial)).items():
                print("%-10s %s" % (k, v))
        elif a.cmd == "rtm-test":
            http = ElegooHttp(st)
            printers = http.printers()
            serial = st.get("printer_serial") or (printers[0]["serial"] if printers else "none")
            with PrinterChannel(st, http, serial, print):
                print("Agora RTM login OK")
        elif a.cmd == "send":
            if a.printer:
                st["printer_serial"] = a.printer
            if a.no_leveling:
                st["auto_bed_leveling"] = False
            if a.timelapse:
                st["timelapse"] = True
            if a.plate_side:
                st["plate_side"] = a.plate_side
            msg = send_to_printer(Path(a.file), Path(a.file).name, st, start_print=not a.upload_only,
                                  progress=lambda pc: print("\r%3d%%" % pc, end="", flush=True), status=print)
            print("\n" + msg)
        return 0
    except CloudError as e:
        print("Error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__" and orca is None:
    sys.exit(_cli(sys.argv[1:]))
