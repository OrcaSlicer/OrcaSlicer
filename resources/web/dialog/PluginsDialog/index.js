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
let selectedPluginId = "";
let contextPluginId = "";
let activeDetailTab = "plugin-info";
let selectedInstallAction = "explore";

let pluginList = null;
let ctxMenu = null;
let exploreMenu = null;
let exploreMenuButton = null;

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

  // why: our stopPropagation blocks the outside-click handler, so close the sibling sort menu ourselves (mirror of ToggleSortMenu).
  if (typeof HideSortMenu === "function")
    HideSortMenu();

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
      RenderSortMenuState();
    }
    ApplyPlugins(payload.data || []);
  } else if (payload.command === "status_message") {
    ShowStatusMessage(String(payload.message || ""), String(payload.level || "info"));
  }
}

// Renders the latest plugin/capability operation result in the footer status bar. The result
// persists until the next operation replaces it; the native side already localizes the text.
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
  SyncPluginListHeaderGutter();
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

function RenderPlugins() {
  if (!pluginList)
    return;

  pluginList.innerHTML = "";

  if (pluginsById.size === 0) {
    const empty = document.createElement("div");
    empty.className = "empty-state";
    empty.textContent = "No plugins found";
    pluginList.appendChild(empty);
    return;
  }

  for (const plugin of pluginsById.values()) {
    const pluginKey = String(plugin.plugin_key || "");
    const capabilities = GetCapabilities(plugin);
    const isExpanded = expandedPluginIds.has(pluginKey) && capabilities.length > 0;
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
    row.appendChild(LabelCell(plugin, isExpanded, capabilities.length));
    row.appendChild(VersionCell(plugin));
    row.appendChild(StatusCell(plugin));

    block.appendChild(row);
    if (isExpanded)
      block.appendChild(RenderCapabilityTree(plugin, capabilities));
    pluginList.appendChild(block);
  }
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

// Primary version shown in the row: the installed version once installed, otherwise the latest available.
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

  const hasEnabled = toggleableCapabilities.some((capability) => capability?.enabled === true);
  const hasDisabled = toggleableCapabilities.some((capability) => capability?.enabled !== true);
  return hasEnabled && hasDisabled;
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

function LabelCell(plugin, isExpanded = false, capabilityCount = 0) {
  const labelCell = document.createElement("span");
  labelCell.className = "label-cell";

  // Add hyperlink
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
  labelElement.textContent = pluginLabelText;
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
  labelCell.appendChild(SourceBadge(plugin.source));

  return labelCell;
}

function RenderCapabilityTree(plugin, capabilities) {
  const tree = document.createElement("div");
  tree.className = "capabilities-tree";
  tree.setAttribute("role", "group");
  tree.setAttribute("aria-label", "Capabilities");

  capabilities.forEach((capability, index) => {
    tree.appendChild(RenderCapabilityRow(plugin, capability, index === capabilities.length - 1));
  });

  return tree;
}

function RenderCapabilityRow(plugin, capability, isLast) {
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
  name.textContent = String(capability?.name || "") || "-";
  nameCell.appendChild(branch);
  nameCell.appendChild(name);
  row.appendChild(nameCell);

  const typeCell = document.createElement("span");
  typeCell.className = "capability-type-cell";
  typeCell.textContent = String(capability?.type || "-");
  row.appendChild(typeCell);

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

  // Descriptions come only from the plugin's Python header (local/installed plugins). A cloud plugin
  // that is not installed yet has no header, so show a link to view it on OrcaCloud instead.
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

  // The "update_available" state is represented by the actionable Update button next to the
  // version, so the detail panel only shows the passive badge for the unauthorized warning.
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
    if (expandedPluginIds.has(pluginKey))
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

  SendMessage("run_script_plugin", {
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
