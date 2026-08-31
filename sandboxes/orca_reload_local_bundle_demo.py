# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# id = "reload-local-bundle-demo"
# name = "Reload Local Bundle Demo"
# description = "Manages two local filament presets to demonstrate live bundle reload."
# author = "OrcaSlicer contributors"
# version = "0.1.0"
# ///
"""A minimal consumer of ``orca.host.reload_local_bundle``.

Install this single file as a local plugin. Its window manages two demo
filament presets. Each user action stages the demo bundle before one host call
and re-observes the live collection afterward.
"""

import json
import os
import tempfile

import orca


BUNDLE_ID = "reload-local-bundle-demo"
STATE_FILE = ".reload-local-bundle-demo-state.json"
LEGACY_PROFILE = "Reload API Demo Filament.json"
DEMO_PRESETS = (
    {"id": "sample-a", "name": "Reload API Demo Filament A"},
    {"id": "sample-b", "name": "Reload API Demo Filament B"},
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


def _active_preset_folder(bundle):
    for collection in (bundle.filaments, bundle.printers, bundle.prints):
        for index in range(collection.size()):
            preset = collection.preset(index)
            folder = _folder_from_preset_file(preset.file)
            if folder:
                return folder
    return "default"


def _bundle_dir(bundle):
    return os.path.join(
        _data_dir(), "user", _active_preset_folder(bundle), "_local", BUNDLE_ID
    )


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


def _target_name(item):
    return "_local/%s/%s" % (BUNDLE_ID, item["name"])


def _profile_path(bundle, item):
    return os.path.join(_bundle_dir(bundle), "filament", item["name"] + ".json")


def _state_path(bundle):
    return os.path.join(_bundle_dir(bundle), STATE_FILE)


def _read_state(bundle):
    path = _state_path(bundle)
    if not os.path.exists(path):
        return {"profiles": {}}
    try:
        with open(path, "r", encoding="utf-8") as stream:
            state = json.load(stream)
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
    try:
        profile["revision"] = max(0, int(profile.get("revision", 0)))
    except (TypeError, ValueError):
        profile["revision"] = 0
    profile["parent_name"] = str(profile.get("parent_name") or "")
    return profile


def _remove_file(path):
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def _parent_filament(bundle):
    target_names = {_target_name(item) for item in DEMO_PRESETS}
    candidates = [bundle.filaments.selected_preset()]
    candidates.extend(bundle.filaments.preset(index) for index in range(bundle.filaments.size()))
    for preset in candidates:
        if preset.name not in target_names:
            return preset.name
    raise RuntimeError("A parent filament preset is required for this demo")


def _parent_is_live(bundle, name):
    return bool(name) and name not in {_target_name(item) for item in DEMO_PRESETS} and bundle.filaments.find_preset(name) is not None


def _demo_profile(item, profile):
    return {
        "type": "filament",
        "name": item["name"],
        "from": "User",
        "instantiation": "true",
        "version": "1.0.0",
        "inherits": profile["parent_name"],
        "filament_settings_id": "%s r%d" % (item["name"], profile["revision"]),
    }


def _reconcile(bundle, state):
    active = []
    for item in DEMO_PRESETS:
        profile = _profile_state(state, item)
        if not profile["deleted"] and profile["enabled"]:
            active.append((item, profile))
    for item, profile in active:
        if not _parent_is_live(bundle, profile["parent_name"]):
            profile["parent_name"] = _parent_filament(bundle)
        if profile["revision"] == 0:
            profile["revision"] = 1

    bundle_dir = _bundle_dir(bundle)
    _remove_file(os.path.join(bundle_dir, "filament", LEGACY_PROFILE))
    for item in DEMO_PRESETS:
        profile = _profile_state(state, item)
        path = _profile_path(bundle, item)
        if profile["deleted"] or not profile["enabled"]:
            _remove_file(path)
        else:
            _write_json_atomic(path, _demo_profile(item, profile))

    _write_json_atomic(
        os.path.join(bundle_dir, "bundle_metadata.json"),
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
                "enabled": profile["enabled"],
                "deleted": profile["deleted"],
                "revision": profile["revision"],
                "loaded": bundle.filaments.find_preset(_target_name(item)) is not None,
            }
        )
    return {"command": "state", "items": items, "message": message, "error": error}


PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8"><style>
:root { color-scheme: light dark; font: 14px system-ui, sans-serif; }
body { margin: 0; padding: 20px; background: Canvas; color: CanvasText; }
p { color: GrayText; margin: 0 0 16px; line-height: 1.45; }
.profiles { display: grid; gap: 10px; }
.profile { border: 1px solid rgba(127, 127, 127, .4); border-radius: 9px; padding: 13px; }
.top, .bottom { display: flex; align-items: center; justify-content: space-between; gap: 12px; }
.top { margin-bottom: 12px; }
.name { font-weight: 650; }
.meta { color: GrayText; font-size: 12px; margin-top: 3px; }
.status { border-radius: 999px; padding: 4px 9px; font-size: 12px; white-space: nowrap; }
.loaded { background: #0b8f5a; color: white; }
.disabled, .deleted { background: rgba(127, 127, 127, .25); }
.missing { background: #be4b0a; color: white; }
.actions { display: flex; justify-content: space-between; align-items: center; margin-top: 16px; gap: 12px; }
button { border: 0; border-radius: 7px; padding: 8px 12px; font: inherit; cursor: pointer; background: #008f7a; color: white; }
button.secondary { background: rgba(127, 127, 127, .25); color: CanvasText; }
button.danger { background: #b64036; }
button:disabled { opacity: .55; cursor: wait; }
.message { min-height: 20px; margin-top: 12px; color: GrayText; }
.error { color: #d13c32; }
.toggle { display: inline-flex; align-items: center; gap: 8px; cursor: pointer; }
.toggle input { position: absolute; opacity: 0; pointer-events: none; }
.switch { width: 38px; height: 22px; border-radius: 999px; background: rgba(127, 127, 127, .45); position: relative; transition: background .15s ease; }
.switch::after { content: ""; position: absolute; top: 3px; left: 3px; width: 16px; height: 16px; border-radius: 50%; background: white; transition: transform .15s ease; }
.toggle input:checked + .switch { background: #008f7a; }
.toggle input:checked + .switch::after { transform: translateX(16px); }
.toggle input:focus-visible + .switch { outline: 2px solid #008f7a; outline-offset: 2px; }
.toggle input:disabled + .switch { opacity: .45; }
.help { margin: 0 0 16px; padding: 11px 13px; border: 1px solid rgba(127, 127, 127, .35); border-radius: 8px; color: GrayText; line-height: 1.45; }
.help summary { color: CanvasText; cursor: pointer; font-weight: 650; }
.help ul { margin: 8px 0 0; padding-left: 20px; }
.help li + li { margin-top: 6px; }
</style></head><body>
<p>Two managed filament presets in one local bundle. Changing a toggle reloads the bundle and then shows the observed live state.</p>
<details class="help">
  <summary>What can this simulate?</summary>
  <ul>
    <li><strong>Available in Orca</strong> keeps a managed profile available. Turning it off removes the profile from the local bundle; this fits a catalog item that is temporarily not needed in Orca.</li>
    <li><strong>Delete</strong> removes the profile from the bundle and marks the demo record deleted; this models removing an item from the managed catalog.</li>
    <li><strong>Restore</strong> returns a deleted demo record as disabled. Enable it to add the profile back to Orca.</li>
  </ul>
</details>
<div class="profiles" id="profiles"></div>
<div class="actions">
  <button class="secondary" id="close">Close</button>
</div>
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
function render(message, error) {
  $('profiles').innerHTML = items.map(item => {
    const s = status(item);
    const deleted = item.deleted ? ' disabled' : '';
    const restore = item.deleted
      ? '<button class="secondary" data-action="restore" data-id="' + esc(item.id) + '">Restore</button>'
      : '<button class="danger" data-action="delete" data-id="' + esc(item.id) + '">Delete</button>';
    return '<section class="profile"><div class="top"><div><div class="name">' + esc(item.name) +
      '</div><div class="meta">revision ' + esc(item.revision) + '</div></div><span class="status ' + s[0] +
      '">' + s[1] + '</span></div><div class="bottom"><label class="toggle"><input type="checkbox" data-toggle="' +
      esc(item.id) + '"' + (item.enabled ? ' checked' : '') + deleted + '><span class="switch"></span>Available in Orca</label>' +
      restore + '</div></section>';
  }).join('');
  $('message').textContent = error || message || '';
  $('message').className = error ? 'message error' : 'message';
  document.querySelectorAll('[data-toggle], [data-action]').forEach(control => {
    control.disabled = busy || control.dataset.action === 'delete' && control.closest('.profile').querySelector('[data-toggle]').disabled;
  });
}
function post(message) {
  busy = true;
  document.querySelectorAll('[data-toggle], [data-action]').forEach(control => control.disabled = true);
  $('message').textContent = 'Applying changes…';
  $('message').className = 'message';
  orca.postMessage(message);
}
$('close').addEventListener('click', () => orca.close());
$('profiles').addEventListener('change', event => {
  if (busy || !event.target.matches('[data-toggle]')) return;
  post({command: 'set_enabled', id: event.target.dataset.toggle, enabled: event.target.checked});
});
$('profiles').addEventListener('click', event => {
  const button = event.target.closest('[data-action]');
  if (!button) return;
  const id = button.dataset.id;
  if (button.dataset.action === 'delete') {
    if (confirm('Delete this demo preset from the local bundle?')) post({command: 'delete', id: id});
  } else {
    post({command: 'restore', id: id});
  }
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
                self._post_state(message="Window refreshed.")
                return orca.ExecutionResult.success("Reload local bundle demo refreshed.")
            self.window = orca.host.ui.create_window(
                title="Reload local bundle demo",
                html=PAGE,
                width=560,
                height=440,
                on_message=self.on_message,
                on_close=self.on_close,
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
            command = message.get("command")
            bundle = orca.host.preset_bundle()
            state = _read_state(bundle)
            if command == "state":
                self._post_state()
                return
            if command == "set_enabled":
                item = next((item for item in DEMO_PRESETS if item["id"] == message.get("id")), None)
                if item is None:
                    raise RuntimeError("Unknown demo preset")
                enabled = message.get("enabled")
                if not isinstance(enabled, bool):
                    raise RuntimeError("Preset availability must be a boolean")
                profile = _profile_state(state, item)
                if profile["deleted"]:
                    raise RuntimeError("Restore the deleted demo preset before enabling it")
                profile["enabled"] = enabled
                _reconcile(bundle, state)
                self._post_state(message="The change was applied. Check the Filament list in Orca.")
                return
            item = next((item for item in DEMO_PRESETS if item["id"] == message.get("id")), None)
            if item is None:
                raise RuntimeError("Unknown demo preset")
            profile = _profile_state(state, item)
            if command == "delete":
                profile.update({"enabled": False, "deleted": True, "revision": 0, "parent_name": ""})
                _reconcile(bundle, state)
                self._post_state(message="%s was deleted." % item["name"])
            elif command == "restore":
                profile.update({"enabled": False, "deleted": False, "revision": 0, "parent_name": ""})
                _reconcile(bundle, state)
                self._post_state(message="%s was restored as disabled." % item["name"])
            else:
                raise RuntimeError("Unknown demo action")
        except Exception as error:
            self._post_state(error=str(error))


@orca.plugin
class ReloadLocalBundleDemoPackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(ReloadLocalBundleDemo)
