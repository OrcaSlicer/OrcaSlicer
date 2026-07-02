// why: C++ owns ordering; this file only sends and reflects sort state.

const DEFAULT_PLUGIN_SORT = { key: "status", order: "asc" };
const SORT_FIELDS = new Set(["status", "name", "source"]);
let pluginSort = { ...DEFAULT_PLUGIN_SORT };

// why: C++ returns canonical sort state; guard stale or malformed values before reflecting them.
function NormalizePluginSort(sortKey, sortOrder) {
  const key = String(sortKey || "");
  return {
    key: SORT_FIELDS.has(key) ? key : DEFAULT_PLUGIN_SORT.key,
    order: sortOrder === "desc" ? "desc" : DEFAULT_PLUGIN_SORT.order,
  };
}

function RequestPluginSort(sortKey, sortOrder) {
  pluginSort = NormalizePluginSort(sortKey, sortOrder);
  RenderSortMenuState();

  if (typeof SendMessage === "function")
    SendMessage("set_plugin_sort", {
      sort_key: pluginSort.key,
      sort_order: pluginSort.order,
    });
}

let sortMenuEl = null;
let sortMenuButton = null;
let sortCurrentLabel = null;

function InitSortDropdown() {
  sortMenuEl = document.getElementById("sortMenu");
  sortMenuButton = document.getElementById("sort_menu_btn");
  sortCurrentLabel = document.getElementById("sort_current_label");
  if (!sortMenuButton || !sortMenuEl)
    return;
  sortMenuButton.addEventListener("click", ToggleSortMenu);
  sortMenuEl.addEventListener("click", OnSortMenuClick);
  RenderSortMenuState();
}

function ToggleSortMenu(event) {
  event.preventDefault();
  event.stopPropagation();
  if (!sortMenuEl)
    return;
  const willShow = sortMenuEl.hidden;
  // why: our stopPropagation blocks index.js's outside-click handler, so close the sibling explore menu ourselves.
  if (willShow && typeof HideExploreMenu === "function")
    HideExploreMenu();
  sortMenuEl.hidden = !willShow;
  sortMenuButton.setAttribute("aria-expanded", willShow ? "true" : "false");
}

function HideSortMenu() {
  if (!sortMenuEl || sortMenuEl.hidden)
    return;
  sortMenuEl.hidden = true;
  sortMenuButton.setAttribute("aria-expanded", "false");
}

function OnSortMenuClick(event) {
  const item = event.target.closest("[data-sort-field],[data-sort-order]");
  if (!item)
    return;
  event.preventDefault();
  const sortKey = item.dataset.sortField || pluginSort.key;
  const sortOrder = item.dataset.sortOrder || pluginSort.order;
  // why: leave the menu open so the user can set field then order in one visit; outside-click/Escape close it.
  RequestPluginSort(sortKey, sortOrder);
}

function RenderSortMenuState() {
  const field = sortMenuEl?.querySelector(`[data-sort-field="${pluginSort.key}"]`);
  if (sortCurrentLabel)
    sortCurrentLabel.textContent = field?.textContent.trim() || "Status";
  sortMenuButton?.classList.toggle("order-desc", pluginSort.order === "desc");
  sortMenuEl?.querySelectorAll("[data-sort-field]").forEach((el) =>
    el.setAttribute("aria-checked", el.dataset.sortField === pluginSort.key ? "true" : "false"));
  sortMenuEl?.querySelectorAll("[data-sort-order]").forEach((el) =>
    el.setAttribute("aria-checked", el.dataset.sortOrder === pluginSort.order ? "true" : "false"));
}

// why: guarded so the module can be loaded in headless syntax checks.
// note: owns its own lifecycle + outside-click/Escape, so index.js's OnInit needs no edit.
if (typeof document !== "undefined") {
  document.addEventListener("DOMContentLoaded", InitSortDropdown);
  document.addEventListener("click", (event) => { if (!event.target.closest(".sort-dropdown")) HideSortMenu(); });
  document.addEventListener("keydown", (event) => { if (event.key === "Escape") HideSortMenu(); });
}
