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

  const MAX_GRADIENT_COLORS    = 7;  // Most colors a gradient may have. A caller allows 2 (the default) up to this many.
  const NEW_GRADIENT_HUE_SHIFT = 75; // Hue step used for every newly created gradient color.
  const gradientLimit = (value) => Math.max(2, Math.min(MAX_GRADIENT_COLORS, Math.floor(Number(value)) || 2));

  // Selection types the bridge carries. A co-extruded selection is two or more solid colors side by
  // side along the filament; the page edits it in Solid mode, as solid color blocks.
  const MULTI_COLOR_MODES = ["gradient", "coextruded"];
  const typeAllowed = (options, type) => type === "solid" || (type === "gradient" && options.allow_gradient === true) ||
                                         (type === "coextruded" && options.allow_coextruded === true);

  // `maxGradientColors` caps how many colors a gradient or co-extruded selection may have (2 when omitted).
  function normalizeSelection(value, options = {}, maxGradientColors = 2) {
    if (!value || !["solid", ...MULTI_COLOR_MODES].includes(value.type) || !Array.isArray(value.colors)) return null;
    if (!typeAllowed(options, value.type)) return null;
    const count = value.colors.length;
    if (value.type === "solid" ? count !== 1 : count < 2 || count > gradientLimit(maxGradientColors)) return null;
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

  // options.max_gradient_colors (2..7, default 2) is how many colors this page lets a gradient or co-extruded selection have.
  function createState(options = {}, selection = {type: "solid", colors: ["#808080"]}) {
    const capabilities = {allow_gradient: options.allow_gradient === true, allow_alpha: options.allow_alpha === true,
                          allow_coextruded: options.allow_coextruded === true};
    // Solid and Gradient keep their own colors. solid holds one color, or more for a co-extruded selection.
    // Each stays null until a selection provides it or the user switches to that mode.
    const state = {options: capabilities, maxGradientColors: gradientLimit(options.max_gradient_colors),
                   mode: "solid", active: 0, solid: null, gradient: null};
    if (!applySelection(state, selection)) return null;
    return state;
  }

  // The colors the current mode edits.
  function modeColors(state) { return state.mode === "solid" ? state.solid : state.gradient; }

  function applySelection(state, value) {
    const selection = normalizeSelection(value, state.options, state.maxGradientColors);
    if (!selection) return false;
    const colors = selection.colors.map(parseHex);
    state.mode = selection.type === "gradient" ? "gradient" : "solid";
    if (state.mode === "solid") state.solid = colors;
    else state.gradient = colors;
    state.active = Math.min(state.active, colors.length - 1); // Fewer colors must not leave the selected one past the end.
    return true;
  }

  function activeColor(state) { return modeColors(state)[state.active]; }

  function setMode(state, mode) {
    if (mode !== "solid" && (mode !== "gradient" || !state.options.allow_gradient)) return false;
    // A new gradient starts at the selected solid color and ends at its contrast color;
    // a new solid starts at the selected gradient color.
    if (mode === "gradient" && !state.gradient) {
      const solid = state.solid[Math.min(state.active, state.solid.length - 1)];
      state.gradient = [[...solid], contrastColor(solid, NEW_GRADIENT_HUE_SHIFT)];
    }
    if (mode === "solid" && !state.solid) state.solid = [[...state.gradient[state.active]]];
    state.mode = mode;
    state.active = Math.min(state.active, modeColors(state).length - 1);
    return true;
  }

  // Fewest colors the current mode keeps: a gradient needs 2, Solid can go back to a single color.
  const minColors = (state) => state.mode === "solid" ? 1 : 2;
  // Solid only gets more colors when this caller allows co-extruded selections.
  function canAddColor(state) {
    return (state.mode === "gradient" || state.options.allow_coextruded) && modeColors(state).length < state.maxGradientColors;
  }

  // Adds a gradient color or, in Solid mode, a co-extruded color after the selected one.
  function addGradientColor(state) {
    if (!canAddColor(state)) return false;
    const colors = modeColors(state);
    colors.splice(state.active + 1, 0, contrastColor(colors[state.active], NEW_GRADIENT_HUE_SHIFT));
    state.active += 1;
    return true;
  }

  function removeGradientColor(state) {
    const colors = modeColors(state);
    if (colors.length <= minColors(state)) return false;
    colors.splice(state.active, 1);
    state.active = Math.min(state.active, colors.length - 1);
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
    modeColors(state)[state.active] = color;
    return true;
  }

  // Solid mode with more than one color is a co-extruded selection.
  function exportSelection(state) {
    const colors = modeColors(state);
    const type = state.mode === "gradient" ? "gradient" : colors.length > 1 ? "coextruded" : "solid";
    return normalizeSelection({type, colors: colors.map(toHex)}, state.options, state.maxGradientColors);
  }

  // Favorites retain full capabilities regardless of what this caller can select.
  const ALL_CAPABILITIES = {allow_gradient: true, allow_alpha: true, allow_coextruded: true};
  function normalizeFavorites(values) {
    if (!Array.isArray(values)) return [];
    return values.slice(0, 24).map((value) => normalizeSelection(value, ALL_CAPABILITIES, MAX_GRADIENT_COLORS)).filter(Boolean);
  }

  function selectionBackground(value) {
    if (value.type === "gradient") return `linear-gradient(to right, ${value.colors.join(", ")})`;
    if (value.type === "coextruded") {
      // Each color repeats at its segment's start and end, so the segments have hard edges.
      const edge = (index) => `${Math.round(index * 10000 / value.colors.length) / 100}%`;
      return `linear-gradient(to right, ${value.colors.map((color, index) => `${color} ${edge(index)} ${edge(index + 1)}`).join(", ")})`;
    }
    return `linear-gradient(${value.colors[0]}, ${value.colors[0]})`;
  }

  return {MAX_GRADIENT_COLORS, MULTI_COLOR_MODES, ALL_CAPABILITIES, NEW_GRADIENT_HUE_SHIFT, contrastColor, parseHex, toHex, normalizeSelection, rgbToHsl, hslToRgb, createState, applySelection, activeColor, setMode,
          modeColors, minColors, canAddColor, addGradientColor, removeGradientColor, setRgb, setAlphaPercent, setHex, exportSelection, normalizeFavorites, selectionBackground};
});
