// Capability rows the active preset uses, as PluginConfig::capabilities_payload emits them:
// {plugin_key, name, type, type_key, has_config_ui}.
let capabilities = [];

// The selected row's identity; plugin_key is part of it because this list spans plugins.
let selectedPluginKey = "";
let selectedCapabilityName = "";
let selectedCapabilityType = "";
let selectedHasPresetOverride = false;
let selectedReadOnly = false;

function SafeJsonParse(text) {
  try {
    return JSON.parse(text);
  } catch (err) {
    return null;
  }
}

function SendWXMessage(message) {
  if (window.wx && typeof window.wx.postMessage === "function")
    window.wx.postMessage(message);
}

function SendMessage(command, payload = {}) {
  const message = {
    sequence_id: Math.round(Date.now() / 1000),
    command: command
  };
  Object.keys(payload).forEach((key) => {
    message[key] = payload[key];
  });
  SendWXMessage(JSON.stringify(message));
}

function HandleStudio(value) {
  const payload = (typeof value === "string") ? SafeJsonParse(value) : value;
  if (!payload || typeof payload !== "object")
    return;

  if (payload.command === "list_capabilities") {
    ApplyCapabilities(payload);
  } else if (payload.command === "status_message") {
    ShowStatusMessage(String(payload.message || ""), String(payload.level || "info"));
  } else if (payload.command === "capability_config") {
    ApplyCapabilityConfig(payload);
  } else if (payload.command === "capability_config_saved") {
    ApplyCapabilityConfigSaved(payload);
  }
}

function ShowStatusMessage(message, level) {
  const bar = document.getElementById("statusBar");
  const text = document.getElementById("statusText");
  if (!bar || !text)
    return;

  const normalizedLevel = ["success", "warn", "error", "info"].includes(level) ? level : "info";
  text.textContent = message;
  text.title = message;
  bar.classList.remove("is-empty", "level-success", "level-warn", "level-error", "level-info");
  bar.classList.add(`level-${normalizedLevel}`);
}

function ApplyCapabilities(payload) {
  capabilities = Array.isArray(payload.data) ? payload.data : [];

  const presetName = document.getElementById("pagePresetName");
  if (presetName)
    presetName.textContent = String(payload.preset_name || "");

  RenderCapabilities();
}

function IsSameCapability(capability) {
  return String(capability.plugin_key || "") === selectedPluginKey
    && String(capability.name || "") === selectedCapabilityName
    && String(capability.type_key || "") === selectedCapabilityType;
}

function RenderCapabilities() {
  const empty = document.getElementById("configEmpty");
  const layout = document.getElementById("configLayout");
  const sidebar = document.getElementById("configSidebar");
  if (!empty || !layout || !sidebar)
    return;

  if (capabilities.length === 0) {
    empty.hidden = false;
    layout.hidden = true;
    sidebar.replaceChildren();
    ClearCapabilityConfigView();
    selectedPluginKey = "";
    selectedCapabilityName = "";
    selectedCapabilityType = "";
    return;
  }

  empty.hidden = true;
  layout.hidden = false;

  // Keep the selection across a refresh if the capability is still there, else select the first.
  if (!capabilities.some(IsSameCapability)) {
    selectedPluginKey = String(capabilities[0].plugin_key || "");
    selectedCapabilityName = String(capabilities[0].name || "");
    selectedCapabilityType = String(capabilities[0].type_key || "");
    ClearCapabilityConfigView();
    RequestCapabilityConfig();
  }

  sidebar.replaceChildren();
  for (const capability of capabilities) {
    const item = document.createElement("button");
    item.type = "button";
    item.className = "config-cap";
    item.dataset.pluginKey = String(capability.plugin_key || "");
    item.dataset.capabilityName = String(capability.name || "");
    item.dataset.capabilityType = String(capability.type_key || "");
    item.setAttribute("role", "option");

    const isSelected = IsSameCapability(capability);
    item.classList.toggle("selected", isSelected);
    item.setAttribute("aria-selected", isSelected ? "true" : "false");

    const label = document.createElement("span");
    label.className = "config-cap-name";
    label.textContent = String(capability.name || "");
    item.appendChild(label);

    const type = document.createElement("span");
    type.className = "config-cap-type";
    type.textContent = String(capability.type || "");
    item.appendChild(type);

    sidebar.appendChild(item);
  }
}

function OnConfigSidebarClick(event) {
  const item = event.target.closest(".config-cap");
  if (!item)
    return;

  const pluginKey = String(item.dataset.pluginKey || "");
  const name = String(item.dataset.capabilityName || "");
  const typeKey = String(item.dataset.capabilityType || "");
  if (!name || (pluginKey === selectedPluginKey && name === selectedCapabilityName && typeKey === selectedCapabilityType))
    return;

  selectedPluginKey = pluginKey;
  selectedCapabilityName = name;
  selectedCapabilityType = typeKey;

  // The native reply is async: clear now so the old config cannot appear under the new selection.
  ClearCapabilityConfigView();
  RequestCapabilityConfig();
  RenderCapabilities();
}

// Config editor: the host's JSON editor, or the capability's own HTML UI in a sandboxed frame.
// Both edit the same stored config; the page renders what the native side sends.

// Replies are async: apply one only if it still matches the selected row (plugin_key included,
// since this list spans plugins), so a stale reply never lands under another capability.
function IsCurrentCapability(payload) {
  return String(payload?.plugin_key || "") === selectedPluginKey
    && String(payload?.capability_name || "") === selectedCapabilityName
    && String(payload?.capability_type || "") === selectedCapabilityType;
}

function RequestCapabilityConfig() {
  if (!selectedPluginKey || !selectedCapabilityName)
    return;

  SendMessage("get_capability_config", {
    plugin_key: selectedPluginKey,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType
  });
}

// Empties both editors and the footer, so nothing from the previous capability lingers while the
// next one is in flight.
function ClearCapabilityConfigView() {
  const editor = document.getElementById("configEditor");
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");
  const error = document.getElementById("configError");
  const footer = document.getElementById("configFooter");

  if (editor)
    editor.hidden = true;
  if (custom) {
    custom.hidden = true;
    custom.removeAttribute("srcdoc");
  }
  if (text)
    text.value = "";
  if (error) {
    error.hidden = true;
    error.textContent = "";
  }
  if (footer)
    footer.hidden = true;
  selectedHasPresetOverride = false;
  selectedReadOnly = false;
  SetConfigValidation("");
}

// A read-only capability cannot be saved, and there is nothing to restore until the preset overrides
// the global configuration.
function UpdateConfigActions(payload) {
  selectedHasPresetOverride = payload?.has_preset_override === true;
  selectedReadOnly = payload?.read_only === true;

  const save = document.getElementById("configSaveBtn");
  const restore = document.getElementById("configRestoreBtn");
  if (save)
    save.disabled = selectedReadOnly;
  if (restore)
    restore.disabled = selectedReadOnly || !selectedHasPresetOverride;
}

function ApplyCapabilityConfig(payload) {
  if (!IsCurrentCapability(payload))
    return;

  const editor = document.getElementById("configEditor");
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");
  const error = document.getElementById("configError");

  const message = String(payload?.error || "");
  if (error) {
    error.textContent = message;
    error.hidden = message === "";
  }

  const config = payload && Object.prototype.hasOwnProperty.call(payload, "config") ? payload.config : {};
  const html = String(payload?.custom_html || "");
  UpdateConfigActions(payload);

  // The footer belongs to the JSON editor. A custom UI owns its whole surface, including whatever
  // save/restore controls it wants, and reaches the host through the window.orca bridge.
  const footer = document.getElementById("configFooter");
  if (footer)
    footer.hidden = html !== "";

  if (html) {
    if (custom) {
      custom.hidden = false;
      custom.srcdoc = BuildCustomConfigDocument(html, config);
    }
    if (editor)
      editor.hidden = true;
    return;
  }

  // Default editor: any reason a custom UI is unavailable already arrived in payload.error.
  if (custom) {
    custom.hidden = true;
    custom.removeAttribute("srcdoc");
  }
  if (editor)
    editor.hidden = false;
  if (text)
    text.value = JSON.stringify(config, null, 2);
  SetConfigValidation("");
}

function SetConfigValidation(message) {
  const node = document.getElementById("configValidation");
  const save = document.getElementById("configSaveBtn");
  if (node) {
    node.textContent = message;
    node.classList.toggle("invalid", message !== "");
  }
  // Invalid JSON is never saved: Save is the only way to persist. The native side re-validates.
  if (save)
    save.disabled = selectedReadOnly || message !== "";
}

function ValidateConfigText() {
  const text = document.getElementById("configText");
  if (!text)
    return false;

  try {
    JSON.parse(text.value);
    SetConfigValidation("");
    return true;
  } catch (err) {
    SetConfigValidation(String(err?.message || "Invalid JSON"));
    return false;
  }
}

function SaveCapabilityConfig() {
  if (!selectedPluginKey || !selectedCapabilityName)
    return;

  const text = document.getElementById("configText");
  if (!text)
    return;

  if (!ValidateConfigText())
    return;

  // Sent as text on purpose: the native side is the authority on validity and parses it itself.
  SendMessage("save_capability_config", {
    plugin_key: selectedPluginKey,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType,
    config: text.value
  });
}

// "Restore defaults" here drops the preset's override, so the capability falls back to the global
// configuration. The native side confirms, then re-sends the config that is now effective.
function RestoreCapabilityConfig() {
  if (!selectedPluginKey || !selectedCapabilityName || !selectedHasPresetOverride)
    return;

  SendMessage("remove_preset_override", {
    plugin_key: selectedPluginKey,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType
  });
}

function ApplyCapabilityConfigSaved(payload) {
  if (!IsCurrentCapability(payload))
    return;

  const error = document.getElementById("configError");
  const message = String(payload?.error || "");
  if (error) {
    error.textContent = message;
    error.hidden = message === "";
  }
  if (payload?.ok !== true)
    return;

  // Reload from what was persisted, not from what was typed.
  const config = payload && Object.prototype.hasOwnProperty.call(payload, "config") ? payload.config : {};
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");

  if (custom && !custom.hidden && custom.contentWindow)
    custom.contentWindow.postMessage({ __orca: "config", config: config }, "*");
  else if (text)
    text.value = JSON.stringify(config, null, 2);

  SetConfigValidation("");
}

// The whole host surface a custom config UI gets: read the config, save one, drop the preset's
// override, and be told when either lands. The frame is sandboxed into an opaque origin, so this
// bridge is its only channel.
function BuildCustomConfigDocument(html, config) {
  // Inlined into a <script>: a stored "</script>" would close the tag early, so escape "<" — the
  // literal stays valid JSON.
  const seed = JSON.stringify(config).replace(/</g, "\\u003c");
  const bridge = `<script>
(function () {
  var handlers = [];
  var current = ${seed};
  window.orca = {
    getConfig: function () { return current; },
    saveConfig: function (cfg) { parent.postMessage({ __orca: "save", config: cfg }, "*"); },
    restoreDefaults: function () { parent.postMessage({ __orca: "restore" }, "*"); },
    onConfig: function (cb) {
      if (typeof cb !== "function") return;
      handlers.push(cb);
      try { cb(current); } catch (e) {}
    }
  };
  window.addEventListener("message", function (event) {
    if (!event.data || event.data.__orca !== "config") return;
    current = event.data.config || {};
    handlers.forEach(function (handler) {
      try { handler(current); } catch (e) {}
    });
  });
})();
<\/script>`;
  return bridge + html;
}

function OnCustomConfigMessage(event) {
  const custom = document.getElementById("configCustom");
  // Only the frame we created, and only while it is actually showing.
  if (!custom || custom.hidden || !custom.contentWindow || event.source !== custom.contentWindow)
    return;

  const data = event.data;
  if (!data || !selectedPluginKey || !selectedCapabilityName)
    return;

  if (data.__orca === "save") {
    SendMessage("save_capability_config", {
      plugin_key: selectedPluginKey,
      capability_name: selectedCapabilityName,
      capability_type: selectedCapabilityType,
      config: data.config === undefined ? {} : data.config
    });
    return;
  }

  if (data.__orca === "restore")
    RestoreCapabilityConfig();
}

document.addEventListener("DOMContentLoaded", () => {
  const sidebar = document.getElementById("configSidebar");
  if (sidebar)
    sidebar.addEventListener("click", OnConfigSidebarClick);

  const saveBtn = document.getElementById("configSaveBtn");
  if (saveBtn)
    saveBtn.addEventListener("click", SaveCapabilityConfig);

  const restoreBtn = document.getElementById("configRestoreBtn");
  if (restoreBtn)
    restoreBtn.addEventListener("click", RestoreCapabilityConfig);

  const text = document.getElementById("configText");
  if (text)
    text.addEventListener("input", ValidateConfigText);

  // The custom UI is sandboxed into an opaque origin, so postMessage is its only channel.
  // OnCustomConfigMessage matches on the frame's contentWindow, not the origin ("null" when
  // sandboxed), and ignores anything else.
  window.addEventListener("message", OnCustomConfigMessage);

  SendMessage("request_capabilities");
});
