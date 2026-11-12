const pluginSearch = { query: "", caseSensitive: false, wholeWord: false };

function PluginSearchActive() {
  return pluginSearch.query.length > 0;
}

// why: matcher (FoldChar/Norm/FuzzyRanges/WholeWordRanges) lives in shared ../js/fuzzy-search.js,
//      loaded before this script - it is shared with the Speed Dial popup. Cc = pluginSearch.caseSensitive.
function MatchText(text, query) {
  if (!query)
    return [];
  return pluginSearch.wholeWord
    ? WholeWordRanges(text, query, pluginSearch.caseSensitive)
    : FuzzyRanges(text, query, pluginSearch.caseSensitive);
}

// Per-plugin evaluator consumed by RenderPlugins. The name text mirrors LabelCell's pluginLabelText so
// highlight offsets line up with what is rendered. Capability names exist for loaded plugins only.
function ComputePluginMatch(plugin) {
  const name = plugin.label || plugin.name || plugin.plugin_id || "";
  const nameRanges = MatchText(name, pluginSearch.query);
  const capabilities = Array.isArray(plugin?.capabilities) ? plugin.capabilities : [];
  const capRanges = new Map();
  for (const capability of capabilities) {
    const key = String(capability?.name || "");
    const ranges = MatchText(key, pluginSearch.query);
    if (ranges)
      capRanges.set(key, ranges);
  }
  return {
    matched: !!nameRanges || capRanges.size > 0,
    nameRanges,
    capRanges,
    hasCapMatch: capRanges.size > 0,
  };
}

// --- widget wiring ---
let pluginSearchInput = null;
let pluginSearchClear = null;
let pluginSearchCc = null;
let pluginSearchW = null;

function InitPluginSearch() {
  pluginSearchInput = document.getElementById("plugin_search_input");
  pluginSearchClear = document.getElementById("plugin_search_clear");
  pluginSearchCc = document.getElementById("plugin_search_cc");
  pluginSearchW = document.getElementById("plugin_search_w");
  if (!pluginSearchInput)
    return;

  // why: common.js installs a document-level onkeydown that cancels the default action of every key
  //      (returnValue=false) to block webview shortcuts; on the way up it also swallows typing. Stop the
  //      field's keydowns from bubbling to it so the input stays editable, leaving the global guard intact.
  pluginSearchInput.addEventListener("keydown", (event) => event.stopPropagation());

  pluginSearchInput.addEventListener("input", OnPluginSearchInput);
  pluginSearchClear?.addEventListener("click", ClearPluginSearch);
  pluginSearchCc?.addEventListener("click", () => TogglePluginSearchFlag(pluginSearchCc, "caseSensitive"));
  pluginSearchW?.addEventListener("click", () => TogglePluginSearchFlag(pluginSearchW, "wholeWord"));
  SyncPluginSearchClear();
}

function OnPluginSearchInput() {
  pluginSearch.query = pluginSearchInput.value;
  // why: emptying the box by editing (not just the x) also ends the search - drop the transient vetoes.
  if (!pluginSearch.query)
    ClearSearchExpandOverride();
  SyncPluginSearchClear();
  RenderPluginsIfReady();
}

function ClearPluginSearch() {
  pluginSearch.query = "";
  if (pluginSearchInput)
    pluginSearchInput.value = "";
  ClearSearchExpandOverride();
  SyncPluginSearchClear();
  RenderPluginsIfReady();
  pluginSearchInput?.focus();
}

function TogglePluginSearchFlag(button, key) {
  pluginSearch[key] = !pluginSearch[key];
  button.classList.toggle("on", pluginSearch[key]);
  button.setAttribute("aria-pressed", String(pluginSearch[key]));
  RenderPluginsIfReady();
}

// why: toggle visibility (not display / the hidden attribute) so the x keeps its reserved slot and
//      showing or hiding it never reflows the Cc / W buttons.
function SyncPluginSearchClear() {
  if (pluginSearchClear)
    pluginSearchClear.style.visibility = pluginSearch.query.length ? "visible" : "hidden";
}

// why: searchExpandOverride lives in index.js; guard so this module stays loadable on its own.
function ClearSearchExpandOverride() {
  if (typeof searchExpandOverride !== "undefined")
    searchExpandOverride.clear();
}

function RenderPluginsIfReady() {
  if (typeof RenderPlugins === "function")
    RenderPlugins();
}

// why: guarded so the module can be loaded in headless syntax checks; mirrors plugin-sort.js.
if (typeof document !== "undefined")
  document.addEventListener("DOMContentLoaded", InitPluginSearch);
