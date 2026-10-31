const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const sent = [];

function fakeJQuery() {
  const node = {
    hide() { return node; },
    show() { return node; },
    attr() { return node; },
    is() { return false; },
    offset() { return { top: 0, left: 0 }; },
    width() { return 100; },
    height() { return 100; },
    trigger() { return node; },
    off() { return node; },
    on() { return node; }
  };
  return node;
}

const ctx = {
  console,
  JSON,
  Date,
  Math,
  $: fakeJQuery,
  SendWXMessage: message => sent.push(message)
};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext(
  fs.readFileSync(path.join(__dirname, "js/home-calibration-button.js"), "utf8"),
  ctx
);

ctx.OnSelectCalibrationTest(5);
assert.strictEqual(sent.length, 1);
const message = JSON.parse(sent[0]);
assert.strictEqual(message.command, "homepage_calibration_test");
assert.strictEqual(message.data.kind, "5", "a tile sends its CalibKind ordinal");

// Mirrors CalibKind in src/slic3r/GUI/MainFrame.hpp.
const kindNames = {
  0: "cali_temperature",
  1: "cali_max_volumetric_speed",
  2: "cali_pressure_advance",
  3: "cali_flow_ratio",
  4: "cali_retraction",
  5: "cali_cornering",
  6: "cali_input_shaping_frequency",
  7: "cali_input_shaping_damping",
  8: "cali_vfa"
};

const html = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
const tiles = [...html.matchAll(/OnSelectCalibrationTest\((\d+)\)[\s\S]{0,400}?CaliItemName"><span data-i18n="([^"]+)"/g)]
  .map(match => ({ kind: Number(match[1]), name: match[2] }));

assert.strictEqual(tiles.length, 9, "every CalibKind has one tile");

for (const tile of tiles)
  assert.strictEqual(tile.name, kindNames[tile.kind], `tile ${tile.kind} must open ${kindNames[tile.kind]}`);

assert.deepStrictEqual(
  [
    ...new Set(tiles.map(tile => tile.kind))].sort(),
  [0, 1, 2, 3, 4, 5, 6, 7, 8],
  "each ordinal appears exactly once"
);
console.log("home-calibration-button: ok");
