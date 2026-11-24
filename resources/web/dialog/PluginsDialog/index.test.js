// Regression tests for the Plugins dialog update control.
// Run: node resources/web/dialog/PluginsDialog/index.test.js
"use strict";

const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const { test } = require("node:test");
const assert = require("node:assert/strict");

// index.js is a browser script, not a module. Load it the way the page does, with just enough of
// the host globals present for its top-level declarations. Only pure render helpers are exercised.
function loadPluginDialog() {
  const context = {
    console,
    T: (_key, fallback) => (fallback === undefined ? _key : fallback)
  };
  context.window = context;
  vm.createContext(context);
  vm.runInContext(fs.readFileSync(path.join(__dirname, "index.js"), "utf8"), context);
  return context;
}

test("update control stays disabled while the plugin is loading", () => {
  const dialog = loadPluginDialog();
  const button = { hidden: true, disabled: false };

  dialog.ApplyDetailUpdateButton(button, { status: "Loading", update_status: "update_available" });

  assert.equal(button.hidden, false, "an available update is shown while loading");
  assert.equal(button.disabled, true, "the update must not be startable during a load");
});

test("update control is enabled once loading finishes", () => {
  const dialog = loadPluginDialog();
  const button = { hidden: true, disabled: true };

  dialog.ApplyDetailUpdateButton(button, { status: "Activated", update_status: "update_available" });

  assert.equal(button.hidden, false);
  assert.equal(button.disabled, false);
});

test("update control stays hidden when no update is available, even while loading", () => {
  const dialog = loadPluginDialog();
  const button = { hidden: false, disabled: true };

  dialog.ApplyDetailUpdateButton(button, { status: "Loading", update_status: "normal" });

  assert.equal(button.hidden, true);
});
