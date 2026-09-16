# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# id = "reload-local-bundle-demo"
# name = "Reload Local Bundle Demo"
# description = "Manages local filament, process, and printer profiles with live bundle reload."
# author = "OrcaSlicer contributors"
# version = "0.1.0"
# ///
"""A small interactive consumer of ``orca.host.reload_local_bundle``."""

import json
import os
import tempfile

import orca


BUNDLE_ID = "reload-local-bundle-demo"
STATE_FILE = ".reload-local-bundle-demo-state.json"
DEMO_PRESETS = (
    {
        "id": "filament",
        "name": "Local Bundle Demo Filament",
        "kind": "Filament",
        "type": "filament",
        "directory": "filament",
        "collection": "filaments",
        "setting_key": "filament_settings_id",
    },
    {
        "id": "process",
        "name": "Local Bundle Demo Process",
        "kind": "Process",
        "type": "process",
        "directory": "process",
        "collection": "prints",
        "setting_key": "print_settings_id",
    },
    {
        "id": "printer",
        "name": "Local Bundle Demo Printer",
        "kind": "Printer",
        "type": "machine",
        "directory": "machine",
        "collection": "printers",
        "setting_key": "printer_settings_id",
    },
)


def _data_dir():
    parts = os.path.abspath(__file__).replace("\\", "/").split("/")
    if "orca_plugins" not in parts:
        raise RuntimeError("Install this file as a local OrcaSlicer plugin")
    return "/".join(parts[: parts.index("orca_plugins")])


def _folder_from_preset_file(path):
    parts = os.path.normpath(path or "").replace("\\", "/").split("/")
    try:
        return parts[parts.index("user") + 1]
    except (ValueError, IndexError):
        return None


def _bundle_dir(bundle):
    for collection in (bundle.filaments, bundle.prints, bundle.printers):
        for index in range(collection.size()):
            folder = _folder_from_preset_file(collection.preset(index).file)
            if folder:
                return os.path.join(_data_dir(), "user", folder, "_local", BUNDLE_ID)
    return os.path.join(_data_dir(), "user", "default", "_local", BUNDLE_ID)


def _source_path(bundle, item):
    return os.path.join(_bundle_dir(bundle), item["directory"], item["name"] + ".json")


def _state_path(bundle):
    return os.path.join(_bundle_dir(bundle), STATE_FILE)


def _write_json_atomic(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix="." + os.path.basename(path) + ".", suffix=".tmp", dir=os.path.dirname(path)
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        os.replace(temporary, path)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _remove_file(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def _read_state(bundle):
    try:
        with open(_state_path(bundle), "r", encoding="utf-8") as stream:
            state = json.load(stream)
    except FileNotFoundError:
        return {"profiles": {}}
    except (OSError, ValueError) as error:
        raise RuntimeError("The demo state file cannot be read: %s" % error)
    if not isinstance(state, dict) or not isinstance(state.get("profiles"), dict):
        raise RuntimeError("The demo state file has an invalid shape")
    return state


def _profile_state(state, item):
    profiles = state["profiles"]
    profile = profiles.get(item["id"])
    if profile is None:
        profile = {}
        profiles[item["id"]] = profile
    if not isinstance(profile, dict):
        raise RuntimeError("The demo state for %s has an invalid shape" % item["name"])
    profile["enabled"] = profile.get("enabled") is True
    profile["deleted"] = profile.get("deleted") is True
    if profile["deleted"]:
        profile["enabled"] = False
    profile["parent_name"] = str(profile.get("parent_name") or "")
    return profile


def _collection(bundle, item):
    return getattr(bundle, item["collection"])


def _target_name(item):
    return "_local/%s/%s" % (BUNDLE_ID, item["name"])


def _parent_name(bundle, item):
    collection = _collection(bundle, item)
    own_names = {
        _target_name(candidate)
        for candidate in DEMO_PRESETS
        if candidate["collection"] == item["collection"]
    }
    candidates = [collection.selected_preset()]
    candidates.extend(collection.preset(index) for index in range(collection.size()))
    for preset in candidates:
        if preset.name not in own_names:
            return preset.name
    raise RuntimeError("A parent %s preset is required for this demo" % item["kind"].lower())


def _parent_is_live(bundle, item, name):
    return bool(name) and name not in {_target_name(item)} and _collection(bundle, item).find_preset(name) is not None


def _profile(item, parent_name):
    return {
        "type": item["type"],
        "name": item["name"],
        "from": "User",
        "instantiation": "true",
        "version": "1.0.0",
        "inherits": parent_name,
        item["setting_key"]: item["name"],
    }


def _reconcile(bundle, state):
    for item in DEMO_PRESETS:
        profile = _profile_state(state, item)
        if profile["enabled"] and not profile["deleted"] and not _parent_is_live(bundle, item, profile["parent_name"]):
            profile["parent_name"] = _parent_name(bundle, item)

    for item in DEMO_PRESETS:
        profile = _profile_state(state, item)
        path = _source_path(bundle, item)
        if profile["enabled"] and not profile["deleted"]:
            _write_json_atomic(path, _profile(item, profile["parent_name"]))
        else:
            _remove_file(path)
    _write_json_atomic(
        os.path.join(_bundle_dir(bundle), "bundle_metadata.json"),
        {"id": BUNDLE_ID, "name": "Reload Local Bundle Demo", "version": "0.1.0"},
    )
    _write_json_atomic(_state_path(bundle), state)
    orca.host.reload_local_bundle(BUNDLE_ID)


def _state_payload(bundle, state, message="", error=""):
    items = []
    for item in DEMO_PRESETS:
        profile = _profile_state(state, item)
        items.append(
            {
                "id": item["id"],
                "name": item["name"],
                "kind": item["kind"],
                "enabled": profile["enabled"],
                "deleted": profile["deleted"],
                "loaded": _collection(bundle, item).find_preset(_target_name(item)) is not None,
            }
        )
    return {"command": "state", "items": items, "message": message, "error": error}


PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8"><style>
:root { color-scheme: light dark; font: 14px system-ui, sans-serif; }
body { margin: 0; padding: 16px; background: Canvas; color: CanvasText; }
.profiles { display: grid; gap: 9px; }
.profile { border: 1px solid rgba(127, 127, 127, .4); border-radius: 8px; padding: 11px; }
.top, .bottom { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.top { margin-bottom: 10px; }
.name { font-weight: 650; }
.kind, .message { color: GrayText; font-size: 12px; }
.status { border-radius: 999px; padding: 3px 8px; font-size: 12px; white-space: nowrap; }
.loaded { background: #0b8f5a; color: white; }
.disabled, .deleted { background: rgba(127, 127, 127, .25); }
.missing { background: #be4b0a; color: white; }
.toggle { display: inline-flex; align-items: center; gap: 7px; cursor: pointer; }
.toggle input { position: absolute; opacity: 0; pointer-events: none; }
.switch { width: 34px; height: 20px; border-radius: 999px; background: rgba(127, 127, 127, .45); position: relative; }
.switch::after { content: ""; position: absolute; top: 3px; left: 3px; width: 14px; height: 14px; border-radius: 50%; background: white; transition: transform .15s ease; }
.toggle input:checked + .switch { background: #008f7a; }
.toggle input:checked + .switch::after { transform: translateX(14px); }
.toggle input:disabled + .switch, button:disabled { opacity: .5; cursor: wait; }
button { border: 0; border-radius: 6px; padding: 6px 10px; font: inherit; cursor: pointer; background: #b64036; color: white; }
button.secondary { background: rgba(127, 127, 127, .25); color: CanvasText; }
.actions { display: flex; justify-content: flex-end; margin-top: 14px; }
.message { min-height: 18px; margin-top: 10px; }
.error { color: #d13c32; }
.help { margin: 0 0 12px; color: GrayText; font-size: 12px; line-height: 1.4; }
.help summary { color: CanvasText; cursor: pointer; font-weight: 650; }
.help p { margin: 7px 0 0; }
</style></head><body>
<details class="help"><summary>How it works</summary><p>Each row controls one profile in this local bundle. A switch adds or removes its source file; Delete removes the demo record and Restore returns it disabled. Every action reloads this bundle across filament, process, and machine.</p></details>
<div class="profiles" id="profiles"></div>
<div class="actions"><button class="secondary" id="close">Close</button></div>
<div class="message" id="message"></div>
<script>
'use strict';
const $ = id => document.getElementById(id);
let items = [];
let busy = false;
function esc(value) {
  return String(value == null ? '' : value).replace(/[&<>"']/g, c =>
    ({'&':'&amp;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
function status(item) {
  if (item.deleted) return ['deleted', 'Deleted'];
  if (item.loaded) return ['loaded', 'Loaded'];
  if (item.enabled) return ['missing', 'Missing after reload'];
  return ['disabled', 'Not in Orca'];
}
function setBusy(value) {
  document.querySelectorAll('[data-toggle], [data-action]').forEach(control => control.disabled = value);
}
function render(message, error) {
  $('profiles').innerHTML = items.map(item => {
    const state = status(item);
    const deleted = item.deleted ? ' disabled' : '';
    const action = item.deleted
      ? '<button class="secondary" data-action="restore" data-id="' + esc(item.id) + '">Restore</button>'
      : '<button data-action="delete" data-id="' + esc(item.id) + '">Delete</button>';
    return '<section class="profile"><div class="top"><div><div class="name">' + esc(item.name) +
      '</div><div class="kind">' + esc(item.kind) + '</div></div><span class="status ' + state[0] +
      '">' + state[1] + '</span></div><div class="bottom"><label class="toggle"><input type="checkbox" data-toggle="' +
      esc(item.id) + '"' + (item.enabled ? ' checked' : '') + deleted + '><span class="switch"></span>Available in Orca</label>' +
      action + '</div></section>';
  }).join('');
  $('message').textContent = error || message || '';
  $('message').className = error ? 'message error' : 'message';
  if (busy) setBusy(true);
}
function post(message) {
  busy = true;
  setBusy(true);
  $('message').textContent = 'Reloading local bundle…';
  $('message').className = 'message';
  orca.postMessage(message);
}
$('close').addEventListener('click', () => orca.close());
$('profiles').addEventListener('change', event => {
  if (!busy && event.target.matches('[data-toggle]')) {
    post({command: 'set_enabled', id: event.target.dataset.toggle, enabled: event.target.checked});
  }
});
$('profiles').addEventListener('click', event => {
  const button = event.target.closest('[data-action]');
  if (!button || busy) return;
  if (button.dataset.action === 'delete' && !confirm('Delete this demo profile from the local bundle?')) return;
  post({command: button.dataset.action, id: button.dataset.id});
});
orca.onMessage(message => {
  if (!message || message.command !== 'state') return;
  items = message.items || [];
  busy = false;
  render(message.message, message.error);
});
orca.postMessage({command: 'state'});
</script></body></html>"""


class ReloadLocalBundleDemo(orca.script.ScriptPluginCapabilityBase):
    window = None

    def get_name(self):
        return "Reload local bundle demo"

    def execute(self):
        try:
            if self.window is not None and self.window.is_open():
                self._post_state()
                return orca.ExecutionResult.success("Reload local bundle demo refreshed.")
            self.window = orca.host.ui.create_window(
                title="Reload local bundle demo", html=PAGE, width=500, height=420,
                on_message=self.on_message, on_close=self.on_close,
            )
            return orca.ExecutionResult.success("Reload local bundle demo opened.")
        except Exception as error:
            message = "Reload local bundle demo failed: %s" % error
            orca.host.ui.message(message, title="Reload local bundle demo", buttons="ok", icon="error")
            return orca.ExecutionResult.failure(orca.PluginResult.RecoverableError, message)

    def on_close(self):
        self.window = None

    def _post_state(self, message="", error=""):
        try:
            bundle = orca.host.preset_bundle()
            payload = _state_payload(bundle, _read_state(bundle), message, error)
        except Exception as state_error:
            payload = {"command": "state", "items": [], "message": message, "error": str(state_error)}
        if self.window is not None and self.window.is_open():
            self.window.post(payload)

    def on_message(self, message):
        try:
            message = message or {}
            bundle = orca.host.preset_bundle()
            state = _read_state(bundle)
            if message.get("command") == "state":
                self._post_state()
                return
            item = next((item for item in DEMO_PRESETS if item["id"] == message.get("id")), None)
            if item is None:
                raise RuntimeError("Unknown demo preset")
            profile = _profile_state(state, item)
            command = message.get("command")
            if command == "set_enabled":
                enabled = message.get("enabled")
                if not isinstance(enabled, bool):
                    raise RuntimeError("Preset availability must be a boolean")
                if profile["deleted"]:
                    raise RuntimeError("Restore the deleted demo preset before enabling it")
                profile["enabled"] = enabled
            elif command == "delete":
                profile.update({"enabled": False, "deleted": True, "parent_name": ""})
            elif command == "restore":
                profile.update({"enabled": False, "deleted": False, "parent_name": ""})
            else:
                raise RuntimeError("Unknown demo action")
            _reconcile(bundle, state)
            self._post_state(message="The change was applied. Check the corresponding preset selector in Orca.")
        except Exception as error:
            self._post_state(error=str(error))


@orca.plugin
class ReloadLocalBundleDemoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(ReloadLocalBundleDemo)
