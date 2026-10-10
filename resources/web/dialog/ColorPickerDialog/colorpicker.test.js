"use strict";
const {test} = require("node:test");
const assert = require("node:assert/strict");
const model = require("./colorpicker-model.js");

test("complete RGB/RGBA hex accepts mixed case and rejects partial or unbounded input", () => {
  assert.deepEqual(model.parseHex("#Ab12cD80"), [171,18,205,128]);
  for (const hex of ["", "123456", "#123", "#1234567", "#123456789", "#12345Z", "#123456GG", " #123456", null]) assert.equal(model.parseHex(hex), null);
  for (let byte = 0; byte <= 255; ++byte) {
    const hex = model.toHex([byte, byte, byte, byte]);
    assert.deepEqual(model.parseHex(hex), [byte,byte,byte,byte]);
  }
});

test("HSL conversion preserves primary channels and hue wraps at 360", () => {
  for (const rgb of [[255,0,0], [0,255,0], [0,0,255], [128,128,128], [37,129,240]]) assert.deepEqual(model.hslToRgb(...model.rgbToHsl(...rgb)), rgb);
  assert.deepEqual(model.hslToRgb(360,100,50), [255,0,0]);
  assert.deepEqual(model.hslToRgb(240,100,50), [0,0,255]);
});

test("capabilities default off and reject gradient through every model entry", () => {
  const gradient = {type: "gradient", colors: ["#FF000080", "#0000FF20"]};
  const state = model.createState();
  assert.deepEqual(state.options, {allow_gradient: false, allow_alpha: false});
  assert.equal(model.createState({}, gradient), null);
  assert.equal(model.applySelection(state, gradient), false);
  assert.equal(model.setMode(state, "gradient"), false);
  assert.equal(model.setAlphaPercent(state, 20), false);
  assert.equal(model.createState({allow_alpha: true}, gradient), null);
  const opaque = model.createState({allow_gradient: true}, gradient);
  assert.deepEqual(model.exportSelection(opaque), {type: "gradient", colors: ["#FF0000FF", "#0000FFFF"]});
});

test("gradient endpoint edits keep order and preserve unedited byte alpha", () => {
  const initial = {type: "gradient", colors: ["#FF000001", "#0000FF80"]};
  const state = model.createState({allow_gradient: true, allow_alpha: true}, initial);
  state.active = 0; model.setRgb(state, [0,255,0]);
  state.active = 1; model.setHex(state, "#112233");
  assert.deepEqual(model.exportSelection(state), {type: "gradient", colors: ["#00FF0001", "#11223380"]});
  model.setAlphaPercent(state, 0);
  assert.deepEqual(model.exportSelection(state).colors, ["#00FF0001", "#11223300"]);
  model.setAlphaPercent(state, 100);
  assert.equal(model.exportSelection(state).colors[1], "#112233FF");
  assert.deepEqual(initial.colors, ["#FF000001", "#0000FF80"]);
});

test("favorites retain alpha and gradient even when the selection disables them", () => {
  const favorite = {type: "solid", colors: ["#ABCDEF80"]};
  const gradient = {type: "gradient", colors: ["#FFFFFF00", "#00000010"]};
  const favorites = model.normalizeFavorites([favorite, gradient]);
  const state = model.createState();
  assert.equal(model.applySelection(state, favorites[0]), true);
  assert.deepEqual(model.exportSelection(state), {type: "solid", colors: ["#ABCDEFFF"]});
  assert.deepEqual(favorites, [favorite, gradient]);
  assert.equal(model.applySelection(state, favorites[1]), false);
  assert.equal(model.normalizeFavorites(Array(25).fill(favorite)).length, 24);
});

test("invalid cardinality and channel edits cannot change a selection", () => {
  const state = model.createState({allow_alpha: true});
  const before = model.exportSelection(state);
  for (const value of [{type: "solid", colors: []}, {type: "solid", colors: ["#112233", "#445566"]}, {type: "gradient", colors: ["#112233"]}, {type: "other", colors: ["#112233"]}]) assert.equal(model.applySelection(state, value), false);
  assert.equal(model.setHex(state, "#GG2233"), false);
  for (const value of [-1, 101, NaN, Infinity]) assert.equal(model.setAlphaPercent(state, value), false);
  assert.equal(model.setRgb(state, [256,0,0]), false);
  assert.deepEqual(model.exportSelection(state), before);
});
