// The host surface a plugin capability's custom configuration UI gets, shared by the Plugins dialog
// and by a preset's plugin configuration dialog. Both sandbox the page into an opaque origin, so this
// bridge is its only channel — and both must offer plugin authors the same one.

// The host theme (WebViewHostDialog::host_theme_vars_css) is injected as a ":root{--orca-*}" rule in
// <style id="orca-host-theme-vars">, but only on the top-level page, so a sandboxed config UI never
// sees it unless we hand it over. Relay that CSS verbatim instead of re-deriving it: C++ stays the
// only place the contract is spelled out, and the host re-themes the style in place, so reading it
// always yields the live theme.
function OrcaThemeSnapshot() {
  const style = document.getElementById("orca-host-theme-vars");
  return {
    theme: document.documentElement.getAttribute("data-orca-theme") === "dark" ? "dark" : "light",
    css: style ? style.textContent : ""
  };
}

// Inlined into a <script> or a JSON payload: a stored "</script>" would close the tag early, so
// escape "<" — the literal stays valid JSON.
function OrcaInlineJson(value) {
  return JSON.stringify(value).replace(/</g, "\\u003c");
}

// What a custom UI can learn about the surface it is edited on, kept to what changes the page's own
// behavior: "Restore defaults" writes the plugin's get_default_config() in the Plugins dialog but
// drops the preset's override in a preset dialog, so a page needs the scope to label its own button.
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
    if (next && typeof next.css === "string") theme = next;
    var style = document.getElementById("orca-host-theme-vars");
    if (style) style.textContent = theme.css;
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

// The host re-themes an open dialog in place (WebViewHostDialog::host_theme_apply_js), which a
// sandboxed child frame never sees. Relay it so a custom UI follows a light/dark switch live.
function OrcaWatchThemeForFrame(frameId) {
  const relay = () => {
    const frame = document.getElementById(frameId);
    if (!frame || frame.hidden || !frame.contentWindow)
      return;
    frame.contentWindow.postMessage({ __orca: "theme", theme: OrcaThemeSnapshot() }, "*");
  };
  new MutationObserver(relay).observe(document.documentElement, {
    attributes: true,
    attributeFilter: ["data-orca-theme"]
  });
}
