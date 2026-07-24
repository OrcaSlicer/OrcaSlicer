// The host surface a plugin capability's custom configuration UI gets, shared by the Plugins dialog
// and by a preset's plugin configuration dialog. Both host the page in an iframe sandboxed into an
// opaque origin, so this bridge is its only channel, and both must offer plugin authors exactly the
// same one — hence a single module rather than a copy per dialog.

// The host theme "contract" (WebViewHostDialog::host_theme_vars_css). The document-start injector
// stamps it on the top-level page only — it returns early in child frames — so a sandboxed config UI
// never sees it unless we hand it over.
const ORCA_THEME_VARS = [
  "--orca-bg",
  "--orca-fg",
  "--orca-muted",
  "--orca-border",
  "--orca-accent",
  "--orca-accent-fg",
  "--orca-font"
];

// Read the contract off this page as it is rendering right now, so the frame always opens in the
// live theme rather than whatever the app started in.
function OrcaThemeSnapshot() {
  const style = getComputedStyle(document.documentElement);
  const vars = {};
  ORCA_THEME_VARS.forEach((name) => {
    // Values are host-produced colors and a pre-sanitized font stack; strip anything that could end
    // the declaration or the <style> block it gets inlined into.
    const value = String(style.getPropertyValue(name) || "").trim().replace(/[<>{};]/g, "");
    if (value)
      vars[name] = value;
  });
  return {
    theme: document.documentElement.getAttribute("data-orca-theme") === "dark" ? "dark" : "light",
    vars: vars
  };
}

// Inlined into a <script> or a JSON payload: a stored "</script>" would close the tag early, so
// escape "<" — the literal stays valid JSON.
function OrcaInlineJson(value) {
  return JSON.stringify(value === undefined ? null : value).replace(/</g, "\\u003c");
}

// What a custom UI can learn about the surface it is being edited on. Kept to what changes the
// page's own behavior: "Restore defaults" writes the plugin's get_default_config() in the Plugins
// dialog but drops the preset's override in a preset dialog, so a page cannot label its own button
// without knowing the scope.
function OrcaConfigContext(payload, scope) {
  return {
    scope: scope,
    readOnly: payload ? payload.read_only === true : false,
    hasPresetOverride: payload ? payload.has_preset_override === true : false
  };
}

// The whole host surface: read the config, save one, restore defaults, follow the theme, and be told
// when any of it lands.
function BuildCustomConfigDocument(html, config, context) {
  const theme = OrcaThemeSnapshot();
  const bridge = `<style id="orca-host-theme-vars"></style>
<script>
(function () {
  var handlers = [];
  var themeHandlers = [];
  var current = ${OrcaInlineJson(config === undefined ? {} : config)};
  var context = ${OrcaInlineJson(context || {})};
  var theme = ${OrcaInlineJson(theme)};

  function applyTheme(next) {
    if (next && next.vars) theme = next;
    var css = ":root{";
    for (var name in theme.vars)
      if (Object.prototype.hasOwnProperty.call(theme.vars, name)) css += name + ":" + theme.vars[name] + ";";
    css += "color-scheme:" + theme.theme + ";}";
    var style = document.getElementById("orca-host-theme-vars");
    if (style) style.textContent = css;
    if (document.documentElement) document.documentElement.setAttribute("data-orca-theme", theme.theme);
    themeHandlers.forEach(function (handler) {
      try { handler(theme.theme); } catch (e) {}
    });
  }

  window.orca = {
    getConfig: function () { return current; },
    saveConfig: function (cfg) { parent.postMessage({ __orca: "save", config: cfg }, "*"); },
    restoreDefaults: function () { parent.postMessage({ __orca: "restore" }, "*"); },
    getContext: function () { return context; },
    onConfig: function (cb) {
      if (typeof cb !== "function") return;
      handlers.push(cb);
      try { cb(current); } catch (e) {}
    },
    onTheme: function (cb) {
      if (typeof cb !== "function") return;
      themeHandlers.push(cb);
      try { cb(theme.theme); } catch (e) {}
    }
  };

  window.addEventListener("message", function (event) {
    if (!event.data) return;
    if (event.data.__orca === "theme") { applyTheme(event.data.theme); return; }
    if (event.data.__orca !== "config") return;
    current = event.data.config || {};
    if (event.data.context) context = event.data.context;
    handlers.forEach(function (handler) {
      try { handler(current); } catch (e) {}
    });
  });

  applyTheme();
})();
<\/script>`;
  return bridge + html;
}

// The host re-themes an open dialog in place (WebViewHostDialog::host_theme_apply_js rewrites the
// injected variables and re-stamps data-orca-theme), which a sandboxed child frame never sees. Relay
// it so a custom UI follows a light/dark switch without being reopened.
function OrcaWatchThemeForFrame(getFrame) {
  const relay = () => {
    const frame = getFrame();
    if (!frame || frame.hidden || !frame.contentWindow)
      return;
    frame.contentWindow.postMessage({ __orca: "theme", theme: OrcaThemeSnapshot() }, "*");
  };
  new MutationObserver(relay).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["data-orca-theme"]
  });
}
