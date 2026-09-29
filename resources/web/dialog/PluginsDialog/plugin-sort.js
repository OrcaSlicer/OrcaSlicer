// why: C++ owns ordering; this file only sends and reflects sort state.

const DEFAULT_PLUGIN_SORT = { key: "none", order: "asc" };
// note: SORT_FIELDS are the clickable columns. "none" is the baseline/cleared state, not a field -
//   it is special-cased in NormalizePluginSort and produced by CyclePluginSort's third click.
const SORT_FIELDS = new Set(["status", "name", "source", "version"]);
let pluginSort = { ...DEFAULT_PLUGIN_SORT };

// why: C++ returns canonical sort state; guard stale or malformed values before reflecting them.
function NormalizePluginSort(sortKey, sortOrder) {
  const key = String(sortKey || "");
  return {
    key: key === "none" ? "none" : (SORT_FIELDS.has(key) ? key : DEFAULT_PLUGIN_SORT.key),
    order: sortOrder === "desc" ? "desc" : DEFAULT_PLUGIN_SORT.order,
  };
}

function RequestPluginSort(sortKey, sortOrder) {
  pluginSort = NormalizePluginSort(sortKey, sortOrder);
  RenderSortHeaders();

  if (typeof SendMessage === "function")
    SendMessage("set_plugin_sort", {
      sort_key: pluginSort.key,
      sort_order: pluginSort.order,
    });
}

// why: one click per column cycles asc -> desc -> clear; setting any column clears the previous
//   one for free because C++ (and pluginSort) only ever hold a single key.
function CyclePluginSort(field) {
  if (!SORT_FIELDS.has(field))
    return;
  if (pluginSort.key !== field)
    RequestPluginSort(field, "asc");
  else if (pluginSort.order === "asc")
    RequestPluginSort(field, "desc");
  else
    RequestPluginSort("none", "asc"); // third click: back to baseline
}

// why: paints the sort indicator for headers
// e.g., when user clicks triangle to change sort order, or change to sort by a new different field
function RenderSortHeaders() {
  document.querySelectorAll(".hdr .sort-th").forEach((th) => {
    // "" | "asc" | "desc" - renders the triangle via plugin-sort.css [data-sort=...].
    th.dataset.sort = th.dataset.sortField === pluginSort.key ? pluginSort.order : "";
  });
}

function InitSortHeaders() {
  document.querySelectorAll(".hdr .sort-th").forEach((th) => {
    th.addEventListener("click", () => CyclePluginSort(th.dataset.sortField));
    // note: role="button" cells need Enter/Space to match the old dropdown's keyboard access.
    th.addEventListener("keydown", (event) => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        CyclePluginSort(th.dataset.sortField);
      }
    });
  });
  RenderSortHeaders(); // paint the initial state (baseline = no triangle)
}

// why: guarded so the module can be loaded in headless syntax checks.
if (typeof document !== "undefined")
  document.addEventListener("DOMContentLoaded", InitSortHeaders);
