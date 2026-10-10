/* Pure color/state helpers shared by the page and its node:test contract tests. */
(function (root, factory) {
  "use strict";
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.ColorPickerModel = api;
})(typeof globalThis === "object" ? globalThis : this, function () {
  "use strict";

  function parseHex(hex) {
    if (typeof hex !== "string" || !/^#[0-9a-f]{6}([0-9a-f]{2})?$/i.test(hex)) return null;
    return [1, 3, 5, 7].map((offset) => offset === 7 && hex.length === 7 ? 255 : parseInt(hex.slice(offset, offset + 2), 16));
  }

  function toHex(color) {
    return "#" + color.map((byte) => Math.round(byte).toString(16).padStart(2, "0")).join("").toUpperCase();
  }

  function normalizeSelection(value, options = {}) {
    if (!value || !["solid", "gradient"].includes(value.type) || !Array.isArray(value.colors)) return null;
    if (value.type === "gradient" && !options.allow_gradient) return null;
    if (value.colors.length !== (value.type === "solid" ? 1 : 2)) return null;
    const colors = value.colors.map(parseHex);
    if (colors.some((color) => color === null)) return null;
    if (!options.allow_alpha) colors.forEach((color) => { color[3] = 255; });
    return {type: value.type, colors: colors.map(toHex)};
  }

  function rgbToHsl(r, g, b) {
    r /= 255; g /= 255; b /= 255;
    const max = Math.max(r, g, b), min = Math.min(r, g, b), delta = max - min;
    let h = 0, s = 0;
    const l = (max + min) / 2;
    if (delta) {
      s = delta / (1 - Math.abs(2 * l - 1));
      if (max === r) h = ((g - b) / delta + (g < b ? 6 : 0)) / 6;
      else if (max === g) h = ((b - r) / delta + 2) / 6;
      else h = ((r - g) / delta + 4) / 6;
    }
    return [h * 360, s * 100, l * 100];
  }

  function hslToRgb(h, s, l) {
    h = ((h % 360) + 360) % 360 / 60; s /= 100; l /= 100;
    const c = (1 - Math.abs(2 * l - 1)) * s;
    const x = c * (1 - Math.abs(h % 2 - 1)), m = l - c / 2;
    const sectors = [[c,x,0], [x,c,0], [0,c,x], [0,x,c], [x,0,c], [c,0,x]];
    return sectors[Math.floor(h)].map((channel) => Math.round((channel + m) * 255));
  }

  // Rotate the hue by `hueShift` degrees and move lightness toward its opposite in proportion
  // (0 = same color, 90 = moderate, 180 = complementary with flipped lightness). A negative
  // value rotates the other way. Greys have no usable hue, so only their lightness moves.
  // Keeps the original alpha.
  function contrastColor(color, hueShift = 90) {
    if (!Number.isFinite(hueShift)) hueShift = 90;
    const degrees = Math.max(-180, Math.min(180, hueShift));
    const amount = Math.abs(degrees) / 180;
    const [h, s, l] = rgbToHsl(color[0], color[1], color[2]);
    const grey = s < 10;
    const target = grey ? (l < 50 ? 100 : 0) : 100 - l;
    const rgb = hslToRgb(h + degrees, grey ? 0 : s, l + (target - l) * amount);
    return [...rgb, color[3]];
  }

  function createState(options = {}, selection = {type: "solid", colors: ["#808080"]}) {
    const capabilities = {allow_gradient: options.allow_gradient === true, allow_alpha: options.allow_alpha === true};
    // gradient stays null until a selection provides one or the user switches to gradient mode.
    const state = {options: capabilities, mode: "solid", active: 0, solid: [128,128,128,255], gradient: null};
    if (!applySelection(state, selection)) return null;
    return state;
  }

  function applySelection(state, value) {
    const selection = normalizeSelection(value, state.options);
    if (!selection) return false;
    state.mode = selection.type;
    const colors = selection.colors.map(parseHex);
    if (state.mode === "solid") state.solid = colors[0];
    else state.gradient = colors;
    return true;
  }

  function activeColor(state) { return state.mode === "solid" ? state.solid : state.gradient[state.active]; }

  function setMode(state, mode) {
    if (mode !== "solid" && (mode !== "gradient" || !state.options.allow_gradient)) return false;
    // A new gradient starts at the current solid color and ends at its contrast color.
    if (mode === "gradient" && !state.gradient) state.gradient = [[...state.solid], contrastColor(state.solid, 90)];
    state.mode = mode;
    return true;
  }

  function setRgb(state, rgb) {
    if (rgb.length !== 3 || rgb.some((v) => !Number.isFinite(v) || v < 0 || v > 255)) return false;
    const color = activeColor(state);
    rgb.forEach((v, i) => { color[i] = Math.round(v); });
    return true;
  }

  function setAlphaPercent(state, value) {
    if (!state.options.allow_alpha || !Number.isFinite(value) || value < 0 || value > 100) return false;
    activeColor(state)[3] = Math.round(value * 255 / 100);
    return true;
  }

  function setHex(state, hex) {
    const color = parseHex(hex);
    if (!color) return false;
    if (hex.length === 7) color[3] = activeColor(state)[3];
    if (!state.options.allow_alpha) color[3] = 255;
    if (state.mode === "solid") state.solid = color;
    else state.gradient[state.active] = color;
    return true;
  }

  function exportSelection(state) {
    const colors = state.mode === "solid" ? [state.solid] : state.gradient;
    return normalizeSelection({type: state.mode, colors: colors.map(toHex)}, state.options);
  }

  // Favorites retain full capabilities regardless of what this caller can select.
  function normalizeFavorites(values) {
    if (!Array.isArray(values)) return [];
    return values.slice(0, 24).map((value) => normalizeSelection(value, {allow_gradient: true, allow_alpha: true})).filter(Boolean);
  }

  function selectionBackground(value) {
    return value.type === "gradient" ? `linear-gradient(to right, ${value.colors[0]}, ${value.colors[1]})` : `linear-gradient(${value.colors[0]}, ${value.colors[0]})`;
  }

  return {parseHex, toHex, normalizeSelection, rgbToHsl, hslToRgb, createState, applySelection, activeColor, setMode, setRgb, setAlphaPercent, setHex, exportSelection, normalizeFavorites, selectionBackground};
});
