const pluginsById = new Map();
const pluginInstallActions = {
  "explore": {
    label: "Install plugin",
    command: "open_plugin_hub"
  },
  "install-local": {
    label: "Install local plugin",
    command: "install_local_plugin"
  }
};

let expandedPluginIds = new Set();

// why: transient per-search override (id -> bool) on top of expandedPluginIds. A search auto-expands
//      matching rows, so a triangle click during search must not touch the base expand state.
let searchExpandOverride = new Map();
let selectedPluginId = "";
let contextPluginId = "";
let activeDetailTab = "plugin-info";
let selectedInstallAction = "explore";
// Config tab: the capability whose config is shown. Name and type together address it natively.
let selectedCapabilityName = "";
let selectedCapabilityType = "";
// The plugin that selection belongs to. Capability names are only unique within a plugin, so
// without the owner, selecting another plugin could keep the selection and show the wrong config.
let configPluginId = "";

let pluginList = null;
let ctxMenu = null;
let exploreMenu = null;
let exploreMenuButton = null;

// Split pane. The ratio is the list's share of the content height, so it survives a dialog resize.
const SPLIT_STORAGE_KEY = "orca.plugins.split_ratio";
const SPLIT_DEFAULT_RATIO = 0.62;
const SPLIT_MIN_LIST_PX = 180;
const SPLIT_MIN_DETAILS_PX = 160;

let contentPane = null;
let paneSplitter = null;
let splitRatio = SPLIT_DEFAULT_RATIO;

function OnInit() {
  pluginList = document.getElementById("pluginList");
  ctxMenu = document.getElementById("ctxMenu");
  exploreMenu = document.getElementById("exploreMenu");
  exploreMenuButton = document.getElementById("explore_menu_btn");

  if (typeof TranslatePage === "function")
    TranslatePage();

  document.getElementById("refresh_btn")?.addEventListener("click", () => SendMessage("refresh_plugins"));
  document.getElementById("explore_btn")?.addEventListener("click", () => {
    HideExploreMenu();
    RunSelectedInstallAction();
  });
  exploreMenuButton?.addEventListener("click", ToggleExploreMenu);
  exploreMenuButton?.addEventListener("keydown", OnExploreMenuButtonKeyDown);
  exploreMenu?.addEventListener("click", OnExploreMenuClick);
  document.getElementById("open_terminal")?.addEventListener("click", () => SendMessage("open_terminal"));
  document.getElementById("detailUpdateBtn")?.addEventListener("click", UpdateSelectedPlugin);

  document.querySelectorAll("[role='tab']").forEach((tab) => {
    tab.addEventListener("click", () => ActivateDetailTab(String(tab.dataset.tab || "")));
    tab.addEventListener("keydown", OnDetailTabKeyDown);
  });
  document.getElementById("detailThumbnail")?.addEventListener("error", (event) => {
    event.currentTarget.hidden = true;
  });

  pluginList?.addEventListener("click", OnPluginListClick);
  pluginList?.addEventListener("change", OnPluginListChange);
  pluginList?.addEventListener("contextmenu", OnPluginContextMenu);
  ctxMenu?.addEventListener("click", OnContextMenuClick);

  InitPaneSplitter();

  document.getElementById("configSidebar")?.addEventListener("click", OnConfigSidebarClick);
  document.getElementById("configSaveBtn")?.addEventListener("click", SaveCapabilityConfig);
  document.getElementById("configRestoreBtn")?.addEventListener("click", RestoreCapabilityConfig);

  const configText = document.getElementById("configText");
  // why: common.js cancels every key at the document level (returnValue=false) to block webview
  //      shortcuts, which also swallows typing; don't bubble to it, so the textarea stays editable.
  //      Same treatment as the search field (see plugin-search.js).
  configText?.addEventListener("keydown", (event) => event.stopPropagation());
  configText?.addEventListener("input", ValidateConfigText);
  // The custom UI is sandboxed into an opaque origin, so postMessage is its only channel.
  // OnCustomConfigMessage matches on the frame's contentWindow, not the origin ("null" when
  // sandboxed), and ignores anything else.
  window.addEventListener("message", OnCustomConfigMessage);

  document.addEventListener("click", (event) => {
    if (!event.target.closest(".ctx"))
      HideContextMenu();
    if (!event.target.closest(".explore-dropdown"))
      HideExploreMenu();
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") {
      HideContextMenu();
      HideExploreMenu();
    }
  });

  ActivateDetailTab(activeDetailTab);
  SetSelectedInstallAction(selectedInstallAction, false);
  RequestPlugins();
}

function InitPaneSplitter() {
  contentPane = document.querySelector(".content");
  paneSplitter = document.getElementById("paneSplitter");
  if (!contentPane || !paneSplitter)
    return;

  ApplySplitRatio(ReadStoredSplitRatio());

  paneSplitter.addEventListener("pointerdown", OnSplitterPointerDown);
  paneSplitter.addEventListener("dblclick", () => {
    ApplySplitRatio(SPLIT_DEFAULT_RATIO);
    StoreSplitRatio(splitRatio);
  });
  // A resized dialog changes what the ratio is a ratio *of*, so re-clamp it against the new height
  // rather than letting a pane fall below its minimum.
  window.addEventListener("resize", () => ApplySplitRatio(splitRatio));
}

// Clamped so neither pane drops below its minimum. When the dialog is too short to honor both, the
// panes just split what there is.
function ApplySplitRatio(ratio) {
  const available = contentPane.clientHeight;
  const sash = paneSplitter.offsetHeight;
  let next = Number.isFinite(ratio) ? ratio : SPLIT_DEFAULT_RATIO;

  if (available > 0) {
    const min = SPLIT_MIN_LIST_PX / available;
    const max = (available - sash - SPLIT_MIN_DETAILS_PX) / available;
    next = min > max ? 0.5 : Math.min(Math.max(next, min), max);
  }

  splitRatio = next;
  contentPane.style.setProperty("--plugin-list-height", `${(next * 100).toFixed(2)}%`);
}

function OnSplitterPointerDown(event) {
  if (event.button !== 0)
    return;

  const bounds = contentPane.getBoundingClientRect();
  // Offset of the grab point inside the strip, so the splitter does not jump under the cursor.
  const grabOffset = event.clientY - paneSplitter.getBoundingClientRect().top;

  const onMove = (moveEvent) => {
    ApplySplitRatio((moveEvent.clientY - bounds.top - grabOffset) / bounds.height);
  };
  const onUp = (upEvent) => {
    paneSplitter.releasePointerCapture(upEvent.pointerId);
    paneSplitter.removeEventListener("pointermove", onMove);
    paneSplitter.removeEventListener("pointerup", onUp);
    paneSplitter.classList.remove("dragging");
    document.body.classList.remove("pane-resizing");
    StoreSplitRatio(splitRatio);
  };

  // Captured, so the drag keeps tracking once the pointer leaves the strip (which it does at once).
  paneSplitter.setPointerCapture(event.pointerId);
  paneSplitter.addEventListener("pointermove", onMove);
  paneSplitter.addEventListener("pointerup", onUp);
  paneSplitter.classList.add("dragging");
  document.body.classList.add("pane-resizing");
  event.preventDefault();
}

function ReadStoredSplitRatio() {
  try {
    const stored = Number.parseFloat(window.localStorage.getItem(SPLIT_STORAGE_KEY));
    return Number.isFinite(stored) ? stored : SPLIT_DEFAULT_RATIO;
  } catch (err) {
    return SPLIT_DEFAULT_RATIO; // storage can be unavailable in the webview; the default still works
  }
}

function StoreSplitRatio(ratio) {
  try {
    window.localStorage.setItem(SPLIT_STORAGE_KEY, String(ratio));
  } catch (err) {
    // Persisting the position is a nicety, never a reason to break the drag.
  }
}

function NormalizeInstallAction(action) {
  const normalized = String(action || "");
  return pluginInstallActions[normalized] ? normalized : "explore";
}

function SetSelectedInstallAction(action, notifyNative = false) {
  selectedInstallAction = NormalizeInstallAction(action);

  const selected = pluginInstallActions[selectedInstallAction];
  const button = document.getElementById("explore_btn");
  if (button)
    button.textContent = selected.label;

  document.querySelectorAll("[data-install-action]").forEach((item) => {
    const isSelected = NormalizeInstallAction(item.dataset.installAction) === selectedInstallAction;
    item.classList.toggle("selected", isSelected);
  });

  if (notifyNative) {
    SendMessage("set_plugin_install_action", {
      action: selectedInstallAction
    });
  }
}

function RunSelectedInstallAction() {
  SendMessage(pluginInstallActions[selectedInstallAction].command);
}

function ToggleExploreMenu(event) {
  event.preventDefault();
  event.stopPropagation();

  if (!exploreMenu || !exploreMenuButton)
    return;

  if (exploreMenu.hidden)
    ShowExploreMenu();
  else
    HideExploreMenu();
}

function ShowExploreMenu() {
  if (!exploreMenu || !exploreMenuButton)
    return;

  exploreMenu.hidden = false;
  exploreMenuButton.setAttribute("aria-expanded", "true");
}

function HideExploreMenu() {
  if (!exploreMenu || !exploreMenuButton)
    return;

  exploreMenu.hidden = true;
  exploreMenuButton.setAttribute("aria-expanded", "false");
}

function OnExploreMenuButtonKeyDown(event) {
  if (event.key !== "ArrowDown")
    return;

  event.preventDefault();
  ShowExploreMenu();
  exploreMenu?.querySelector(".explore-menu-item")?.focus();
}

function OnExploreMenuClick(event) {
  const item = event.target.closest("[data-install-action]");
  if (!item)
    return;

  event.preventDefault();
  const action = String(item.dataset.installAction || "");
  HideExploreMenu();
  SetSelectedInstallAction(action, true);
}

function ActivateDetailTab(tabId, focusTab = false) {
  const tabs = Array.from(document.querySelectorAll("[role='tab'][data-tab]"));
  if (!tabs.some((tab) => tab.dataset.tab === tabId))
    return;

  activeDetailTab = tabId;
  tabs.forEach((tab) => {
    const isActive = tab.dataset.tab === activeDetailTab;
    tab.classList.toggle("active", isActive);
    tab.setAttribute("aria-selected", isActive ? "true" : "false");
    tab.tabIndex = isActive ? 0 : -1;
    if (isActive && focusTab)
      tab.focus();
  });

  document.querySelectorAll("[role='tabpanel'][data-panel]").forEach((panel) => {
    panel.hidden = panel.dataset.panel !== activeDetailTab;
  });
}

function OnDetailTabKeyDown(event) {
  const tabs = Array.from(document.querySelectorAll("[role='tab'][data-tab]"));
  const currentIndex = tabs.indexOf(event.currentTarget);
  if (currentIndex < 0)
    return;

  let nextIndex = currentIndex;
  if (event.key === "ArrowRight")
    nextIndex = (currentIndex + 1) % tabs.length;
  else if (event.key === "ArrowLeft")
    nextIndex = (currentIndex - 1 + tabs.length) % tabs.length;
  else if (event.key === "Home")
    nextIndex = 0;
  else if (event.key === "End")
    nextIndex = tabs.length - 1;
  else
    return;

  event.preventDefault();
  ActivateDetailTab(String(tabs[nextIndex].dataset.tab || ""), true);
}

function RequestPlugins() {
  SendMessage("request_plugins");
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

  if (payload.command === "list_plugins") {
    SetSelectedInstallAction(payload.install_action, false);
    if (typeof NormalizePluginSort === "function") {
      pluginSort = NormalizePluginSort(payload.sort_key, payload.sort_order);
      RenderSortHeaders();
    }
    ApplyPlugins(payload.data || []);
  } else if (payload.command === "status_message") {
    ShowStatusMessage(String(payload.message || ""), String(payload.level || "info"));
  } else if (payload.command === "capability_config") {
    ApplyCapabilityConfig(payload);
  } else if (payload.command === "capability_config_saved") {
    ApplyCapabilityConfigSaved(payload);
  }
}

// Footer status bar for the latest operation result; the native side already localizes the text.
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

function SafeJsonParse(value) {
  try {
    return JSON.parse(value);
  } catch (err) {
    return null;
  }
}

function ApplyPlugins(plugins) {
  pluginsById.clear();

  for (const plugin of plugins) {
    const key = String(plugin.plugin_key || "");
    if (!key)
      continue;
    pluginsById.set(key, plugin);
  }

  if (selectedPluginId)
    selectedPluginId = FindEquivalentPluginKey(selectedPluginId);
  if (!selectedPluginId && pluginsById.size > 0)
    selectedPluginId = String(pluginsById.keys().next().value || "");

  expandedPluginIds = new Set(Array.from(expandedPluginIds).filter((pluginKey) => pluginsById.has(pluginKey)));

  RenderPlugins();
  RenderDetails();
}

function FindEquivalentPluginKey(pluginKey) {
  if (pluginsById.has(pluginKey))
    return pluginKey;

  const cloudUuid = ExtractCloudUuid(pluginKey);
  if (!cloudUuid)
    return "";

  for (const candidateKey of pluginsById.keys()) {
    if (ExtractCloudUuid(candidateKey) === cloudUuid)
      return candidateKey;
  }
  return "";
}

function ExtractCloudUuid(pluginKey) {
  const match = String(pluginKey || "").match(/:([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$/i);
  return match ? match[1] : "";
}

function SyncPluginListHeaderGutter() {
  const listPane = document.querySelector(".plugin-list-pane");
  if (!pluginList || !listPane)
    return;

  // Keep header columns aligned with row columns when the scroll body shows a vertical scrollbar.
  const scrollbarWidth = Math.max(0, pluginList.offsetWidth - pluginList.clientWidth);
  listPane.style.setProperty("--plugin-list-scrollbar-width", `${scrollbarWidth}px`);
}

// why: paint matched-character ranges as <mark> without an innerHTML build
function ApplyHighlight(container, text, ranges) {
  if (!ranges || !ranges.length) {
    container.appendChild(document.createTextNode(text));
    return;
  }
  let pos = 0;
  for (const [start, end] of ranges) {
    if (start > pos)
      container.appendChild(document.createTextNode(text.slice(pos, start)));
    const mark = document.createElement("mark");
    mark.className = "plugin-search-hit";
    mark.textContent = text.slice(start, end);
    container.appendChild(mark);
    pos = end;
  }
  if (pos < text.length)
    container.appendChild(document.createTextNode(text.slice(pos)));
}

function RenderPlugins() {
  if (!pluginList)
    return;

  pluginList.innerHTML = "";

  if (pluginsById.size === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No plugins found";
    pluginList.appendChild(empty);
    SyncPluginListHeaderGutter();
    return;
  }

  // why: stable filter over the existing C++ sort order - no scoring, no reorder. The empty query
  //      short-circuits, leaving every existing render path untouched.
  const searching = typeof PluginSearchActive === "function" && PluginSearchActive();
  let shown = 0;

  for (const plugin of pluginsById.values()) {
    const pluginKey = String(plugin.plugin_key || "");
    const capabilities = GetCapabilities(plugin);
    const match = searching ? ComputePluginMatch(plugin) : null;
    if (searching && !match.matched)
      continue;
    shown++;

    // why: the transient override wins; while searching, auto-expand only capability matches. The
    //      base is never written while searching, so clearing it restores exactly what the user had.
    const open = searchExpandOverride.has(pluginKey)
      ? searchExpandOverride.get(pluginKey)
      : (searching ? match.hasCapMatch : expandedPluginIds.has(pluginKey));
    const isExpanded = open && capabilities.length > 0;

    const block = document.createElement("div");
    block.className = "plugin-block";
    block.dataset.pluginKey = pluginKey;
    block.classList.toggle("selected", pluginKey === selectedPluginId);
    block.classList.toggle("expanded", isExpanded);

    const row = document.createElement("div");
    row.className = "row plugin-row plugin-cols";
    row.dataset.pluginKey = pluginKey;

    if (pluginKey === selectedPluginId)
      row.classList.add("selected");

    row.appendChild(CheckCell(row, plugin));
    row.appendChild(LabelCell(plugin, isExpanded, capabilities.length, match?.nameRanges));
    row.appendChild(VersionCell(plugin));
    row.appendChild(SourceCell(plugin));
    row.appendChild(StatusCell(plugin));

    block.appendChild(row);
    if (isExpanded)
      block.appendChild(RenderCapabilityTree(plugin, capabilities, match?.capRanges));
    pluginList.appendChild(block);
  }

  // why: distinct from the size===0 "no plugins" state - here plugins exist but none match the query.
  if (searching && shown === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.appendChild(document.createTextNode('No plugins match "'));
    const term = document.createElement("b");
    term.textContent = pluginSearch.query;
    empty.appendChild(term);
    empty.appendChild(document.createTextNode('"'));
    pluginList.appendChild(empty);
  }

  // why: recompute the scrollbar gutter on every render - search and sort re-render via RenderPlugins
  SyncPluginListHeaderGutter();
}

function GetErrorText(plugin) {
  return String(plugin?.error || "").trim();
}

function GetStatus(plugin) {
  return String(plugin?.status || "");
}

function GetUpdateStatus(plugin) {
  return String(plugin?.update_status || "normal");
}

function IsPluginInstalled(plugin) {
  return plugin?.installed === true;
}

function GetInstalledVersion(plugin) {
  return String(plugin?.installed_version || "");
}

function GetLatestVersion(plugin) {
  return String(plugin?.latest_version || plugin?.version || "");
}

function GetDisplayVersion(plugin) {
  const installed = GetInstalledVersion(plugin);
  if (IsPluginInstalled(plugin) && installed)
    return installed;
  return GetLatestVersion(plugin);
}

function GetCapabilities(plugin) {
  return Array.isArray(plugin?.capabilities) ? plugin.capabilities : [];
}

function GetPluginTypes(plugin) {
  const pluralTypes = Array.isArray(plugin?.types)
    ? plugin.types
    : (Array.isArray(plugin?.capability_types) ? plugin.capability_types : []);
  const types = pluralTypes.map((type) => {
    if (typeof type === "string")
      return type;
    return String(type?.label || type?.type || type?.name || "");
  }).filter(Boolean);

  if (types.length > 0)
    return `[${types.join(", ")}]`;
  return String(plugin?.type || "-");
}

function CapabilityCanRun(plugin, capability) {
  if (capability?.type_key !== "script" || !String(capability?.name || ""))
    return false;
  if (capability?.can_run !== undefined)
    return capability.can_run === true;
  return plugin?.can_run_script === true && capability?.enabled === true;
}

function IsPluginChecked(plugin) {
  return GetStatus(plugin) === "Activated";
}

function HasMixedCapabilityState(plugin) {
  if (!IsPluginChecked(plugin))
    return false;

  const toggleableCapabilities = GetCapabilities(plugin).filter((capability) =>
    capability?.can_toggle !== false &&
    String(capability?.name || "") &&
    String(capability?.type_key || "")
  );

  const hasDisabled = toggleableCapabilities.some((capability) => capability?.enabled !== true);
  return toggleableCapabilities.length > 0 && hasDisabled;
}

function IsPluginLoading(plugin) {
  return GetStatus(plugin) === "Loading";
}

function SourceLabel(source) {
  switch (String(source || "").toLowerCase()) {
    case "mine":
      return "Mine";
    case "subscribed":
      return "Subscribed";
    default:
      return "Local";
  }
}

// Shared Local/Subscribed/Mine pill, used both after the row name and in the info panel.
function SourceBadge(source) {
  const normalized = String(source || "").toLowerCase();
  const variant = (normalized === "mine" || normalized === "subscribed") ? normalized : "local";
  const badge = document.createElement("span");
  badge.className = `plugin-source-badge source-${variant}`;
  badge.textContent = SourceLabel(source);
  return badge;
}

function CheckCell(row, plugin) {
  const checkCell = document.createElement("span");
  checkCell.className = "check-cell";
  const isLoading = IsPluginLoading(plugin);
  const checkboxLabel = document.createElement("label");
  checkboxLabel.className = "plugin-checkbox";
  if (isLoading)
    checkboxLabel.classList.add("loading");
  if (!plugin.can_toggle)
    checkboxLabel.classList.add("disabled");
  const hasMixedCapabilityState = HasMixedCapabilityState(plugin);
  if (hasMixedCapabilityState)
    checkboxLabel.classList.add("mixed");
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.className = "plugin-checkbox-input";
  checkbox.checked = IsPluginChecked(plugin);
  checkbox.indeterminate = hasMixedCapabilityState;
  checkbox.disabled = isLoading || !plugin.can_toggle;
  checkbox.dataset.pluginKey = row.dataset.pluginKey;
  if (checkbox.indeterminate) {
    checkbox.setAttribute("aria-checked", "mixed");
    checkbox.setAttribute("aria-label", "Some plugin capabilities are enabled");
  }
  const checkboxMark = document.createElement("span");
  checkboxMark.className = "plugin-checkbox-mark";
  checkboxLabel.appendChild(checkbox);
  checkboxLabel.appendChild(checkboxMark);
  checkCell.appendChild(checkboxLabel);

  return checkCell;
}

function LabelCell(plugin, isExpanded = false, capabilityCount = 0, nameRanges = null) {
  const labelCell = document.createElement("span");
  labelCell.className = "label-cell";

  const hasCloudLink = plugin.source === "mine" || plugin.source === "subscribed";
  const pluginLabelText = plugin.label || plugin.name || plugin.plugin_id || "";
  const canExpand = capabilityCount > 0;

  if (canExpand) {
    const expandButton = document.createElement("button");
    expandButton.type = "button";
    expandButton.className = "plugin-expand-btn";
    expandButton.setAttribute("aria-label", `${isExpanded ? "Collapse" : "Expand"} ${pluginLabelText || "plugin"} capabilities`);
    expandButton.setAttribute("aria-expanded", isExpanded ? "true" : "false");
    expandButton.title = isExpanded ? "Collapse capabilities" : "Expand capabilities";
    const icon = document.createElement("span");
    icon.className = "plugin-expand-icon";
    expandButton.appendChild(icon);
    labelCell.appendChild(expandButton);
  } else {
    const spacer = document.createElement("span");
    spacer.className = "plugin-expand-spacer";
    spacer.setAttribute("aria-hidden", "true");
    labelCell.appendChild(spacer);
  }

  const nameWrap = document.createElement("span");
  nameWrap.className = "plugin-name-wrap";

  const labelElement = document.createElement(hasCloudLink ? "a" : "span");
  ApplyHighlight(labelElement, pluginLabelText, nameRanges);
  labelElement.className = "plugin-name-text";

  if (hasCloudLink) {
    labelElement.href = "#";
    labelElement.classList.add("plugin-cloud-link");
    labelElement.title = "Open this plugin in your browser";
  }

  nameWrap.appendChild(labelElement);
  if (canExpand) {
    const countBadge = document.createElement("span");
    countBadge.className = "plugin-capability-count";
    countBadge.textContent = String(capabilityCount);
    countBadge.title = `${capabilityCount} capabilities`;
    nameWrap.appendChild(countBadge);
  }
  labelCell.appendChild(nameWrap);

  return labelCell;
}

function SourceCell(plugin) {
  const cell = document.createElement("span");
  const normalized = String(plugin.source || "").toLowerCase();
  const variant = (normalized === "mine" || normalized === "subscribed") ? normalized : "local";
  cell.className = `source-cell source-${variant}`;

  const sourceLabel = document.createElement("span");
  sourceLabel.className = "source-label";
  sourceLabel.textContent = SourceLabel(plugin.source);
  cell.appendChild(sourceLabel);

  return cell;
}

function RenderCapabilityTree(plugin, capabilities, capRanges = null) {
  const tree = document.createElement("div");
  tree.className = "capabilities-tree";
  tree.setAttribute("role", "group");
  tree.setAttribute("aria-label", "Capabilities");

  capabilities.forEach((capability, index) => {
    tree.appendChild(RenderCapabilityRow(plugin, capability, index === capabilities.length - 1, capRanges));
  });

  return tree;
}

function RenderCapabilityRow(plugin, capability, isLast, capRanges = null) {
  const row = document.createElement("div");
  row.className = "capability-row plugin-cols";
  row.classList.toggle("is-last", isLast);
  row.dataset.pluginKey = String(plugin?.plugin_key || "");

  row.appendChild(CapabilityCheckCell(plugin, capability));

  const nameCell = document.createElement("span");
  nameCell.className = "capability-name-cell";
  const branch = document.createElement("span");
  branch.className = "capability-branch";
  branch.setAttribute("aria-hidden", "true");
  const name = document.createElement("span");
  name.className = "capability-name";
  const capabilityLabel = String(capability?.name || "");
  ApplyHighlight(name, capabilityLabel || "-", capRanges?.get(capabilityLabel));
  nameCell.appendChild(branch);
  nameCell.appendChild(name);
  row.appendChild(nameCell);

  const typeCell = document.createElement("span");
  typeCell.className = "capability-type-cell";
  typeCell.textContent = String(capability?.type || "-");
  row.appendChild(typeCell);

  // why: empty placeholder for the new Source column so the run-action cell stays under Status.
  const sourceSpacer = document.createElement("span");
  sourceSpacer.className = "capability-source-cell";
  row.appendChild(sourceSpacer);

  const actionsCell = document.createElement("span");
  actionsCell.className = "capability-actions-cell";
  const capabilityName = String(capability?.name || "");
  if (CapabilityCanRun(plugin, capability)) {
    const runButton = document.createElement("button");
    runButton.type = "button";
    runButton.className = "capability-run-btn script-run-btn";
    runButton.title = `Run "${capabilityName}"`;
    runButton.setAttribute("aria-label", runButton.title);
    const runIcon = document.createElement("span");
    runIcon.className = "icon16 plugin-run-icon";
    runButton.appendChild(runIcon);
    runButton.addEventListener("click", (event) => {
      event.stopPropagation();
      RunScriptPlugin(String(plugin?.plugin_key || ""), capabilityName);
    });
    actionsCell.appendChild(runButton);
  }
  row.appendChild(actionsCell);

  return row;
}

function CapabilityCheckCell(plugin, capability) {
  const checkCell = document.createElement("span");
  checkCell.className = "check-cell capability-check-cell";
  const capabilityName = String(capability?.name || "");
  const capabilityType = String(capability?.type_key || "");

  if (capability?.can_toggle === false || !capabilityName || !capabilityType)
    return checkCell;

  const checkboxLabel = document.createElement("label");
  checkboxLabel.className = "plugin-checkbox";
  const checkbox = document.createElement("input");
  checkbox.type = "checkbox";
  checkbox.className = "plugin-checkbox-input capability-checkbox-input";
  checkbox.checked = capability?.enabled === true;
  checkbox.dataset.pluginKey = String(plugin?.plugin_key || "");
  checkbox.dataset.capabilityToggle = "true";
  checkbox.dataset.capabilityName = capabilityName;
  checkbox.dataset.capabilityType = capabilityType;
  checkbox.setAttribute("aria-label", `Enable ${capabilityName || "capability"}`);
  const checkboxMark = document.createElement("span");
  checkboxMark.className = "plugin-checkbox-mark";
  checkboxLabel.appendChild(checkbox);
  checkboxLabel.appendChild(checkboxMark);
  checkCell.appendChild(checkboxLabel);

  return checkCell;
}

function TableTextCell(text) {
  const cell = document.createElement("td");
  cell.textContent = String(text || "");
  return cell;
}

function VersionCell(plugin) {
  const cell = document.createElement("span");
  cell.className = "version-cell";

  const versionLabel = document.createElement("span");
  versionLabel.className = "version-value";
  versionLabel.textContent = GetDisplayVersion(plugin) || "N/A";
  cell.appendChild(versionLabel);

  const updateBadge = UpdateStatusBadge(plugin);
  if (updateBadge)
    cell.appendChild(updateBadge);

  return cell;
}

function UpdateStatusBadge(plugin) {
  const updateStatus = GetUpdateStatus(plugin);
  if (updateStatus !== "update_available" && updateStatus !== "unauthorized")
    return null;

  const badge = document.createElement("span");
  badge.className = "version-update-badge";
  if (updateStatus === "unauthorized") {
    badge.classList.add("is-warning");
    badge.title = "Unauthorized for updates";
    badge.setAttribute("aria-label", "Unauthorized for updates");
  } else {
    badge.title = "Update available";
    badge.setAttribute("aria-label", "Update available");
  }

  return badge;
}

function StatusCell(plugin) {
  const cell = document.createElement("span");
  cell.className = "status-cell";
  cell.classList.add(`status-${GetStatus(plugin).toLowerCase()}`);

  const statusLabel = document.createElement("span");
  statusLabel.className = "status-label";
  statusLabel.textContent = GetStatus(plugin);
  cell.appendChild(statusLabel);

  return cell;
}

function RenderDetails() {
  const plugin = selectedPluginId ? pluginsById.get(selectedPluginId) : null;
  const detailUpdateBadge = document.getElementById("detailUpdateBadge");
  const detailUpdateBtn = document.getElementById("detailUpdateBtn");
  const detailStatusBody = document.getElementById("detailStatusBody");

  SetText("detailInstalledVersion",
    plugin ? (IsPluginInstalled(plugin) ? (GetInstalledVersion(plugin) || "-") : "Not installed") : "-");
  SetText("detailLatestVersion", plugin ? (GetLatestVersion(plugin) || "-") : "-");
  const detailSource = document.getElementById("detailSource");
  if (detailSource) {
    detailSource.replaceChildren();
    if (plugin && plugin.source)
      detailSource.appendChild(SourceBadge(plugin.source));
    else
      detailSource.textContent = "-";
  }
  SetText("detailTypes", plugin ? GetPluginTypes(plugin) : "-");
  SetText("detailAuthor", plugin ? (plugin.author || "-") : "-");
  RenderThumbnail(plugin);
  RenderDescription(plugin);
  RenderChangelog(plugin);

  if (detailStatusBody)
    RenderDetailSummary(detailStatusBody, plugin);
  if (detailUpdateBadge)
    ApplyDetailUpdateBadge(detailUpdateBadge, plugin);
  if (detailUpdateBtn)
    ApplyDetailUpdateButton(detailUpdateBtn, plugin);

  RenderConfig(plugin);
}

// Config tab: the sidebar lists the selected plugin's configurable capabilities; the view shows the
// host's JSON editor or, when the capability ships one, its own HTML UI in a sandboxed frame. Both
// edit the same stored config; the page renders what the native side sends.

// Rows built natively by PluginConfig::capabilities_payload. Only loaded, addressable capabilities
// appear: the descriptor-only rows of an inactive plugin carry no name, so nothing to address.
function GetConfigurableCapabilities(plugin) {
  return Array.isArray(plugin?.config_capabilities) ? plugin.config_capabilities : [];
}

function RenderConfig(plugin) {
  const empty = document.getElementById("configEmpty");
  const layout = document.getElementById("configLayout");
  const sidebar = document.getElementById("configSidebar");
  if (!empty || !layout || !sidebar)
    return;

  const capabilities = plugin ? GetConfigurableCapabilities(plugin) : [];
  const pluginKey = String(plugin?.plugin_key || "");

  // A different plugin: drop the selection rather than trusting the name to mean the same here.
  if (pluginKey !== configPluginId) {
    configPluginId = pluginKey;
    selectedCapabilityName = "";
    selectedCapabilityType = "";
    ClearCapabilityConfigView();
  }

  if (!plugin || capabilities.length === 0) {
    // Capabilities only exist once the plugin is activated: say that, rather than claiming it has
    // none to configure.
    empty.textContent = !plugin
      ? "Select a plugin to configure its capabilities"
      : (IsPluginLoading(plugin)
        ? "Loading the plugin…"
        : (GetStatus(plugin) === "Activated"
          ? "This plugin exposes no capabilities"
          : "Activate this plugin to configure its capabilities"));
    empty.hidden = false;
    layout.hidden = true;
    sidebar.replaceChildren();
    ClearCapabilityConfigView();
    selectedCapabilityName = "";
    selectedCapabilityType = "";
    return;
  }

  empty.hidden = true;
  layout.hidden = false;

  // Keep the selection across a refresh if the capability is still there, else select the first.
  const stillPresent = capabilities.some((capability) =>
    capability.name === selectedCapabilityName && String(capability.type_key || "") === selectedCapabilityType);
  if (!stillPresent) {
    selectedCapabilityName = String(capabilities[0].name || "");
    selectedCapabilityType = String(capabilities[0].type_key || "");
    ClearCapabilityConfigView();
    RequestCapabilityConfig();
  }

  sidebar.replaceChildren();
  for (const capability of capabilities) {
    const name = String(capability.name || "");
    const typeKey = String(capability.type_key || "");
    const item = document.createElement("button");
    item.type = "button";
    item.className = "config-cap";
    item.dataset.capabilityName = name;
    item.dataset.capabilityType = typeKey;
    item.setAttribute("role", "option");

    const isSelected = name === selectedCapabilityName && typeKey === selectedCapabilityType;
    item.classList.toggle("selected", isSelected);
    item.setAttribute("aria-selected", isSelected ? "true" : "false");

    const label = document.createElement("span");
    label.className = "config-cap-name";
    label.textContent = name;
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

  const name = String(item.dataset.capabilityName || "");
  const typeKey = String(item.dataset.capabilityType || "");
  if (!name || (name === selectedCapabilityName && typeKey === selectedCapabilityType))
    return;

  selectedCapabilityName = name;
  selectedCapabilityType = typeKey;

  // The native reply is async: clear now so the old config cannot appear under the new selection.
  ClearCapabilityConfigView();
  RequestCapabilityConfig();
  RenderConfig(pluginsById.get(selectedPluginId));
}

function RequestCapabilityConfig() {
  if (!selectedPluginId || !selectedCapabilityName)
    return;

  SendMessage("get_capability_config", {
    plugin_key: selectedPluginId,
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
  SetConfigValidation("");
}

// Replies are async: one for a capability the user has navigated away from is dropped, never
// rendered into the current view.
function IsCurrentCapability(payload) {
  return String(payload?.plugin_key || "") === selectedPluginId &&
    String(payload?.capability_name || "") === selectedCapabilityName;
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

  const config = (payload && typeof payload.config === "object" && payload.config !== null) ? payload.config : {};
  const html = String(payload?.custom_html || "");

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
    save.disabled = message !== "";
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
  const text = document.getElementById("configText");
  if (!text || !selectedPluginId || !selectedCapabilityName)
    return;
  if (!ValidateConfigText())
    return;

  // Sent as text on purpose: the native side is the authority on validity and parses it itself.
  SendMessage("save_capability_config", {
    plugin_key: selectedPluginId,
    capability_name: selectedCapabilityName,
    capability_type: selectedCapabilityType,
    config: text.value
  });
}

// Writes the capability's own get_default_config() over whatever is stored — the host does not know
// what a plugin considers default. The native side confirms first, then replies as if it were a save.
function RestoreCapabilityConfig() {
  if (!selectedPluginId || !selectedCapabilityName)
    return;

  SendMessage("restore_capability_config", {
    plugin_key: selectedPluginId,
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
  const config = (payload && typeof payload.config === "object" && payload.config !== null) ? payload.config : {};
  const custom = document.getElementById("configCustom");
  const text = document.getElementById("configText");

  if (custom && !custom.hidden && custom.contentWindow)
    custom.contentWindow.postMessage({ __orca: "config", config: config }, "*");
  else if (text)
    text.value = JSON.stringify(config, null, 2);

  SetConfigValidation("");
}

// The whole host surface a custom config UI gets: read the config, save one, restore the plugin's
// defaults, and be told when either lands. The frame is sandboxed into an opaque origin, so this
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
  if (!data || !selectedPluginId || !selectedCapabilityName)
    return;

  if (data.__orca === "save") {
    SendMessage("save_capability_config", {
      plugin_key: selectedPluginId,
      capability_name: selectedCapabilityName,
      capability_type: selectedCapabilityType,
      config: data.config === undefined ? {} : data.config
    });
    return;
  }

  if (data.__orca === "restore")
    RestoreCapabilityConfig();
}

function RenderThumbnail(plugin) {
  const thumbnail = document.getElementById("detailThumbnail");
  if (!thumbnail)
    return;

  const url = String(plugin?.thumbnail_url || plugin?.thumbnail || plugin?.icon_url || plugin?.icon || "").trim();
  thumbnail.hidden = true;
  thumbnail.removeAttribute("src");
  if (!url)
    return;

  thumbnail.src = url;
  thumbnail.hidden = false;
}

function RenderDescription(plugin) {
  const node = document.getElementById("detailDescription");
  if (!node)
    return;

  node.replaceChildren();

  // Descriptions come from the plugin's Python header; a cloud plugin that is not installed yet has
  // no header, so link to OrcaCloud instead.
  const description = String(plugin?.description || "").trim();
  if (description && description !== "No description.") {
    node.textContent = description;
    return;
  }

  const isCloud = plugin && (plugin.source === "mine" || plugin.source === "subscribed");
  if (isCloud && String(plugin?.sharing_token || "")) {
    node.appendChild(document.createTextNode("View on OrcaCloud "));
    const link = document.createElement("a");
    link.href = "#";
    link.className = "plugin-cloud-link";
    link.textContent = "here";
    link.title = "Open this plugin in your browser";
    link.addEventListener("click", (event) => {
      event.preventDefault();
      sendOpenPluginOnCloud(String(plugin.plugin_key || ""));
    });
    node.appendChild(link);
    return;
  }

  node.textContent = "No description available";
}

function RenderChangelog(plugin) {
  const table = document.getElementById("changelogTable");
  const body = document.getElementById("changelogBody");
  const empty = document.getElementById("changelogEmpty");
  if (!table || !body || !empty)
    return;

  const changelog = Array.isArray(plugin?.changelog) ? plugin.changelog : [];
  body.replaceChildren();
  table.hidden = changelog.length === 0;
  empty.hidden = changelog.length !== 0;

  changelog.forEach((entry) => {
    const row = document.createElement("tr");
    row.appendChild(TableTextCell(entry?.version || "-"));
    row.appendChild(TableTextCell(FormatCreatedTime(entry?.created_time)));
    const changesCell = TableTextCell(entry?.changes || entry?.changelog || "-");
    changesCell.className = "changelog-changes";
    row.appendChild(changesCell);
    body.appendChild(row);
  });
}

function FormatCreatedTime(createdTime) {
  const numericTime = Number(createdTime);
  if (!Number.isFinite(numericTime) || numericTime <= 0)
    return "-";

  const milliseconds = numericTime < 1e12 ? numericTime * 1000 : numericTime;
  const date = new Date(milliseconds);
  if (!Number.isFinite(date.getTime()))
    return "-";

  return date.toISOString().slice(0, 10);
}

function SetText(id, text) {
  const node = document.getElementById(id);
  if (node)
    node.textContent = String(text || "");
}

function ApplyDetailUpdateBadge(node, plugin) {
  node.className = "version-update-badge";

  // "update_available" is already the actionable Update button, so the badge only warns about
  // unauthorized.
  const updateStatus = plugin ? GetUpdateStatus(plugin) : "normal";
  if (updateStatus === "unauthorized") {
    node.hidden = false;
    node.classList.add("is-warning");
    node.title = "Unauthorized for updates";
    node.setAttribute("aria-label", "Unauthorized for updates");
    return;
  }

  node.hidden = true;
  node.title = "";
  node.removeAttribute("aria-label");
}

function ApplyDetailUpdateButton(button, plugin) {
  const updateStatus = plugin ? GetUpdateStatus(plugin) : "normal";
  button.hidden = updateStatus !== "update_available";
  // Re-enable after each render; the click handler disables it until the catalog refreshes.
  button.disabled = false;
}

function UpdateSelectedPlugin() {
  if (!selectedPluginId)
    return;

  const plugin = pluginsById.get(selectedPluginId);
  if (!plugin || GetUpdateStatus(plugin) !== "update_available")
    return;

  const button = document.getElementById("detailUpdateBtn");
  if (button)
    button.disabled = true;

  SendMessage("update_plugin", {
    plugin_key: selectedPluginId
  });
}

function StatusDescription(plugin) {
  switch (GetStatus(plugin)) {
    case "Activated":
      return "This plugin is active and ready to be used.";
    case "Loading":
      return "This plugin is still loading.";
    case "Error":
      return "This plugin is blocked until its error is fixed.";
    case "Inactive":
    default:
      return "This plugin is inactive. Activate it to install or load it.";
  }
}

function RenderDetailSummary(container, plugin) {
  container.replaceChildren();

  if (!plugin) {
    container.textContent = "Select a plugin to view its status and details.";
    return;
  }

  const statusChip = document.createElement("span");
  statusChip.className = `detail-status-chip status-${GetStatus(plugin).toLowerCase()}`;
  statusChip.textContent = GetStatus(plugin);
  container.appendChild(statusChip);

  const message = document.createElement("div");
  const errorText = GetErrorText(plugin);
  message.className = errorText ? "detail-description detail-error-text" : "detail-description";
  message.textContent = errorText || StatusDescription(plugin);
  container.appendChild(message);

  const updateStatus = GetUpdateStatus(plugin);
  if (updateStatus === "update_available") {
    const note = document.createElement("div");
    note.className = "detail-description detail-note-text";
    note.textContent = "A newer plugin version is available.";
    container.appendChild(note);
  }

  if (updateStatus === "unauthorized") {
    const note = document.createElement("div");
    note.className = "detail-description detail-note-text is-warning";
    note.textContent = "Cloud updates are unavailable because this plugin is unauthorized for updates.";
    container.appendChild(note);
  }
}

function OnPluginListClick(event) {
  const expandButton = event.target.closest(".plugin-expand-btn");
  if (expandButton) {
    event.preventDefault();
    event.stopPropagation();

    const block = expandButton.closest(".plugin-block");
    if (!block)
      return;

    const pluginKey = String(block.dataset.pluginKey || "");
    selectedPluginId = pluginKey;
    // why: during a search the triangle writes to the transient override (read from the on-screen
    //      open state), so an auto-expanded row collapses without touching the saved layout.
    if (typeof PluginSearchActive === "function" && PluginSearchActive())
      searchExpandOverride.set(pluginKey, !block.classList.contains("expanded"));
    else if (expandedPluginIds.has(pluginKey))
      expandedPluginIds.delete(pluginKey);
    else
      expandedPluginIds.add(pluginKey);

    RenderPlugins();
    RenderDetails();
    return;
  }

  const cloudLink = event.target.closest(".plugin-cloud-link");
  if (cloudLink) {
    event.preventDefault();
    event.stopPropagation();

    const block = cloudLink.closest(".plugin-block");
    if (!block)
      return;

    selectedPluginId = String(block.dataset.pluginKey || "");
    RenderPlugins();
    RenderDetails();
    sendOpenPluginOnCloud(selectedPluginId);
    return;
  }

  const checkbox = event.target.closest("input[type='checkbox']");
  if (checkbox || event.target.closest(".plugin-checkbox"))
    return;

  if (event.target.closest(".capability-run-btn"))
    return;

  const block = event.target.closest(".plugin-block");
  if (!block)
    return;

  selectedPluginId = String(block.dataset.pluginKey || "");
  RenderPlugins();
  RenderDetails();
}

function OnPluginListChange(event) {
  const checkbox = event.target.closest("input[type='checkbox']");
  if (!checkbox)
    return;

  const pluginKey = String(checkbox.dataset.pluginKey || "");
  if (checkbox.dataset.capabilityToggle === "true") {
    const capabilityName = String(checkbox.dataset.capabilityName || "");
    selectedPluginId = pluginKey;
    checkbox.disabled = true;

    SendMessage("toggle_plugin_capability", {
      plugin_key: pluginKey,
      capability_type: String(checkbox.dataset.capabilityType || ""),
      capability_name: capabilityName,
      enabled: !!checkbox.checked
    });
    return;
  }

  selectedPluginId = pluginKey;
  checkbox.disabled = true;

  SendMessage("toggle_plugin", {
    plugin_key: pluginKey,
    enabled: !!checkbox.checked
  });
}

function RunScriptPlugin(pluginId, capabilityName = "") {
  if (!pluginId)
    return;

  const plugin = pluginsById.get(pluginId);
  const capability = capabilityName
    ? GetCapabilities(plugin).find((cap) =>
      String(cap?.name || "") === capabilityName && CapabilityCanRun(plugin, cap)
    )
    : GetCapabilities(plugin).find((cap) => CapabilityCanRun(plugin, cap));
  if (!plugin || !capability)
    return;

  SendMessage("run_script_plugin_capability", {
    plugin_key: pluginId,
    capability_name: String(capability.name || "")
  });
}

function sendOpenPluginOnCloud(pluginId) {
  const plugin = pluginsById.get(String(pluginId || ""));
  if (!plugin) return;

  const tSend = {
    sequence_id: Math.round(Date.now() / 1000),
    command: "open_plugin_on_cloud",
    sharing_token: String(plugin.sharing_token || "")
  };
  SendWXMessage(JSON.stringify(tSend));
}

function OnPluginContextMenu(event) {
  const block = event.target.closest(".plugin-block");
  if (!block)
    return;

  event.preventDefault();
  selectedPluginId = String(block.dataset.pluginKey || "");
  contextPluginId = selectedPluginId;
  RenderPlugins();
  RenderDetails();
  ShowContextMenu(event.clientX, event.clientY);
}

function ShowContextMenu(x, y) {
  const plugin = pluginsById.get(contextPluginId);
  if (!ctxMenu || !plugin)
    return;

  const actions = Array.isArray(plugin.context_actions) ? plugin.context_actions : [];
  ctxMenu.innerHTML = "";

  if (actions.length === 0)
    return;

  actions.forEach((action) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = action.danger ? "ctx-item danger" : "ctx-item";
    button.dataset.action = String(action.id || "");
    button.textContent = String(action.label || action.id || "");
    button.disabled = action.enabled === false;
    if (action.enabled === false)
      button.setAttribute("aria-disabled", "true");
    ctxMenu.appendChild(button);
  });

  ctxMenu.hidden = false;
  ctxMenu.style.left = `${Math.max(4, x)}px`;
  ctxMenu.style.top = `${Math.max(4, y)}px`;
}

function HideContextMenu() {
  if (ctxMenu)
    ctxMenu.hidden = true;
}

function OnContextMenuClick(event) {
  const button = event.target.closest("[data-action]");
  if (!button || button.hidden || button.disabled || !contextPluginId)
    return;

  const plugin = pluginsById.get(contextPluginId);
  const action = String(button.dataset.action || "");
  if (!plugin || !HasContextAction(plugin, action))
    return;

  SendMessage("plugin_menu_action", {
    plugin_key: contextPluginId,
    action: action
  });
  HideContextMenu();
}

function HasContextAction(plugin, actionId) {
  const actions = Array.isArray(plugin?.context_actions) ? plugin.context_actions : [];
  return actions.some((action) => String(action?.id || "") === actionId && action?.enabled !== false);
}
