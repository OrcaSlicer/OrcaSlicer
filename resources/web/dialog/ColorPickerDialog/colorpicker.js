"use strict";
(() => {
  if (window.ORCA_COLOR_PICKER_LANGUAGE) document.documentElement.lang = window.ORCA_COLOR_PICKER_LANGUAGE;
  const T = (key) => window.ORCA_UI_STRINGS?.[key] || key;
  document.title = T("Color Picker");
  for (const element of document.querySelectorAll("[data-i18n]")) element.textContent = T(element.dataset.i18n);
  for (const attribute of ["aria-label", "title"])
    for (const element of document.querySelectorAll(`[data-i18n-${attribute}]`))
      element.setAttribute(attribute, T(element.getAttribute(`data-i18n-${attribute}`)));
  function paletteName(name) {
    const ral = name.match(/^(\d{4}) (.+)$/);
    return ral ? `${ral[1]} ${T(ral[2])}` : /^%\d+$/.test(name) ? name : T(name);
  }
  const model = window.ColorPickerModel;
  const palettes = window.ColorPickerPalettes;
  const byId = (id) => document.getElementById(id);
  const hexInput = byId("hexInput"), canvas = byId("spectrum"), context = canvas.getContext("2d");
  const paletteCombo = byId("paletteCombo"), palette = byId("palette"), preview = byId("colorPreview");
  const modeSwitch = byId("modeSwitch"), endpointSwitch = byId("gradientSwitch");
  const rgbPairs = ["r", "g", "b"].map((name) => [byId(name + "Slider"), byId(name + "Value")]);
  const hslPairs = ["h", "s", "l"].map((name) => [byId(name + "Slider"), byId(name + "Value")]);
  const alphaPair = [byId("aSlider"), byId("aValue")];
  const pageId = Date.now().toString(36) + Math.random().toString(36).slice(2);
  let state = model.createState(), favorites = [], dragging = false, initialized = false, favoritesWritable = true;
  let lastContentHeight = 0, resizePending = false;
  // Prefer the HSL sliders' own values while they still produce the current color: hue 360
  // and 0 are both red, and greys, black and white have no hue or saturation in RGB.
  function displayHsl(color) {
    const current = hslPairs.map(([slider]) => Number(slider.value));
    const rgb = model.hslToRgb(...current);
    return rgb.every((value, index) => value === color[index]) ? current : model.rgbToHsl(...color.slice(0, 3));
  }

  function emit(command, data = {}) {
    window.dispatchEvent(new CustomEvent("color-picker-message", {detail: {command, ...data}}));
    if (window.wx && typeof window.wx.postMessage === "function")
      window.wx.postMessage(JSON.stringify({command, page_id: pageId, ...data}));
  }

  // Body height is intrinsic, independent of viewport height. Observing it
  // catches capabilities, notice, font/theme changes, and wrapped translations.
  function scheduleResize() {
    if (!initialized || resizePending) return;
    resizePending = true;
    requestAnimationFrame(() => {
      resizePending = false;
      const height = Math.min(900, Math.max(300, Math.ceil(document.body.getBoundingClientRect().height)));
      if (height !== lastContentHeight) {
        lastContentHeight = height;
        emit("resize", {height});
      }
    });
  }
  new ResizeObserver(scheduleResize).observe(document.body);

  function setSwatch(element, selection) {
    element.style.setProperty("--swatch-color", model.selectionBackground(selection));
  }

  function render(resetEditors = false) {
    const color = model.activeColor(state), hsl = displayHsl(color);
    if (resetEditors || hexInput.checkValidity()) {
      hexInput.value = model.toHex(color).slice(1, state.options.allow_alpha ? 9 : 7);
      hexInput.setCustomValidity("");
    }
    // Each thumb shows only its own channel: R black→red, G black→green, B black→blue,
    // H pure hue, S the color at that saturation, L grey at that lightness.
    const rgbThumbs = [`rgb(${color[0]},0,0)`, `rgb(0,${color[1]},0)`, `rgb(0,0,${color[2]})`];
    const hslThumbs = [`hsl(${hsl[0]},100%,50%)`, `hsl(${hsl[0]},${hsl[1]}%,${hsl[2]}%)`, `hsl(0,0%,${hsl[2]}%)`];
    for (const [pairs, values, thumbs] of [[rgbPairs, color, rgbThumbs], [hslPairs, hsl, hslThumbs]]) {
      pairs.forEach(([slider, input], index) => {
        slider.value = values[index];
        if (resetEditors) input.setCustomValidity("");
        if (resetEditors || input.checkValidity()) input.value = Math.round(values[index] * 10) / 10;
        slider.style.setProperty("--thumb-color", thumbs[index]);
      });
    }
    // Percentage is a display only until explicitly edited. Never round it back
    // into the alpha byte on endpoint switches, RGB edits, or confirmation.
    alphaPair.forEach((input) => {
      if (resetEditors) input.setCustomValidity("");
      if (resetEditors || input.checkValidity()) input.value = Math.round(color[3] / 255 * 1000) / 10;
    });
    const rgbHex = model.toHex([...color.slice(0, 3), 255]);
    alphaPair[0].style.setProperty("--track-gradient", `linear-gradient(to right, ${rgbHex.slice(0,7)}00, ${rgbHex})`);
    alphaPair[0].style.setProperty("--thumb-color", rgbHex);
    byId("sSlider").style.setProperty("--track-gradient", `linear-gradient(to right, hsl(${hsl[0]},0%,${hsl[2]}%), hsl(${hsl[0]},100%,${hsl[2]}%))`);
    byId("tab1").checked = state.mode === "solid";
    byId("tab2").checked = state.mode === "gradient";
    endpointSwitch.checked = state.active === 1;
    byId("gradient-switch").classList.toggle("hidden", state.mode !== "gradient");
    setSwatch(preview, model.exportSelection(state));
    updateIndicator(hsl);
  }

  function populatePalette() {
    palette.replaceChildren();
    for (const entry of palettes[paletteCombo.value] || []) {
      const selection = {type: entry.grad ? "gradient" : "solid", colors: entry.grad ? entry.grad.split("-") : [entry.hex]};
      if (!model.normalizeSelection(selection, state.options)) continue;
      const row = document.createElement("button"); row.type = "button"; row.className = "color-item";
      const swatch = document.createElement("span"); swatch.className = "color-swatch";
      const name = document.createElement("span"); name.className = "color-name"; name.textContent = paletteName(entry.name);
      setSwatch(swatch, model.normalizeSelection(selection, {allow_gradient: true, allow_alpha: true}));
      row.append(swatch, name); row.title = paletteName(entry.name);
      row.addEventListener("click", () => {
        // Solid palette colors edit the active endpoint while gradient mode is on.
        const changed = entry.grad ? model.applySelection(state, selection) : model.setHex(state, entry.hex);
        if (changed) render(true);
      });
      palette.appendChild(row);
    }
  }

  function drawFavorites() {
    const grid = byId("userColors"); grid.replaceChildren();
    const visible = favorites.filter((favorite) => state.options.allow_gradient || favorite.type === "solid");
    for (let index = 0; index < 36; ++index) {
      const swatch = document.createElement("button"); swatch.type = "button"; swatch.className = "saved-swatch";
      const favorite = visible[index];
      if (favorite) {
        setSwatch(swatch, favorite); swatch.title = favorite.colors.join(" → ");
        swatch.addEventListener("click", () => { if (model.applySelection(state, favorite)) render(true); });
      } else { swatch.classList.add("empty"); swatch.disabled = true; }
      grid.appendChild(swatch);
    }
  }

  function init(payload) {
    if (!payload || typeof payload !== "object" || (payload.options !== undefined && (!payload.options || typeof payload.options !== "object"))) return false;
    const next = model.createState(payload.options, payload.selection);
    if (!next) return false;
    const notice = byId("selectionNotice");
    notice.hidden = payload.preserve_multi_color !== true;
    notice.textContent = T("The current multi-color selection is kept until you choose a different color.");
    state = next; favorites = model.normalizeFavorites(payload.favorites || []);
    favoritesWritable = payload.favorites_writable !== false;
    byId("saveColorBtn").disabled = !favoritesWritable;
    byId("tab2").nextElementSibling.classList.toggle("hidden", !state.options.allow_gradient);
    for (const option of paletteCombo.options) option.hidden = option.value === "gradient" && !state.options.allow_gradient;
    if (!state.options.allow_gradient && paletteCombo.value === "gradient") paletteCombo.value = "basic";
    byId("alphaControls").classList.toggle("hidden", !state.options.allow_alpha);
    hexInput.maxLength = state.options.allow_alpha ? 9 : 7;
    populatePalette(); drawFavorites(); render(true);
    initialized = true;
    byId("pickerEditors").disabled = false;
    return true;
  }

  function focusInput() {
    if (!initialized) return;
    // Restore initial keyboard focus without replacing an editor's in-progress input.
    const active = document.activeElement;
    if (!active || active === document.body || active === document.documentElement)
      hexInput.focus({preventScroll: true});
  }

  function handleMessage(payload) {
    if (!payload || typeof payload !== "object" || payload.page_id !== pageId) return false;
    if (payload.command === "init") {
      const options = payload.options;
      if (!options || typeof options.allow_gradient !== "boolean" || typeof options.allow_alpha !== "boolean" ||
          !model.normalizeSelection(payload.selection, options) || !validFavorites(payload.favorites) ||
          (payload.favorites_writable !== undefined && typeof payload.favorites_writable !== "boolean") ||
          (payload.preserve_multi_color !== undefined && typeof payload.preserve_multi_color !== "boolean")) return false;
      if (initialized) return true;
      if (!init(payload)) return false;
      emit("initialized");
      scheduleResize();
      return true;
    }
    if (payload.command === "favorites" && initialized && validFavorites(payload.favorites)) {
      favorites = model.normalizeFavorites(payload.favorites); drawFavorites(); return true;
    }
    return false;
  }

  function validFavorites(values) {
    return Array.isArray(values) && values.length <= 24 && values.every((value) => model.normalizeSelection(value, {allow_gradient: true, allow_alpha: true}));
  }

  function drawSpectrum() {
    const rect = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.round(rect.width)); canvas.height = Math.max(1, Math.round(rect.height));
    const hue = context.createLinearGradient(0, 0, canvas.width, 0);
    for (let i = 0; i <= 6; ++i) hue.addColorStop(i / 6, `hsl(${i * 60},100%,50%)`);
    context.fillStyle = hue; context.fillRect(0, 0, canvas.width, canvas.height);
    const shade = context.createLinearGradient(0, 0, 0, canvas.height);
    shade.addColorStop(0, "#FFFFFF"); shade.addColorStop(0.5, "#FFFFFF00"); shade.addColorStop(0.5, "#00000000"); shade.addColorStop(1, "#000000");
    context.fillStyle = shade; context.fillRect(0, 0, canvas.width, canvas.height);
  }

  function updateIndicator(hsl) {
    const indicator = byId("colorIndicator");
    indicator.style.left = hsl[0] / 360 * (canvas.width - 1) + "px";
    indicator.style.top = (1 - hsl[2] / 100) * (canvas.height - 1) + "px";
  }

  // Spectrum position maps straight to HSL: x → hue 0..360, y → lightness 100..0, saturation 100.
  // Setting the HSL sliders lets displayHsl() keep these exact values, including hue 360 at the
  // right edge and the hue of the white and black corners.
  function sampleSpectrum(event) {
    const rect = canvas.getBoundingClientRect();
    const fraction = (client, start, size, pixels) =>
      Math.max(0, Math.min(pixels - 1, Math.floor((client - start) * pixels / size))) / Math.max(1, pixels - 1);
    const hsl = [fraction(event.clientX, rect.left, rect.width, canvas.width) * 360, 100,
                 (1 - fraction(event.clientY, rect.top, rect.height, canvas.height)) * 100]
      .map((value) => Math.round(value * 10) / 10);
    hslPairs.forEach(([slider], index) => { slider.value = hsl[index]; });
    model.setRgb(state, model.hslToRgb(...hsl)); render();
  }

  function bindPair(pair, update) {
    const [slider, input] = pair;
    input.required = true;
    for (const control of pair) {
      control.addEventListener("input", () => {
        control.setCustomValidity("");
        if (control.value === "" || !control.checkValidity()) return;
        const value = Number(control.value);
        if (!Number.isFinite(value)) return;
        slider.value = value; update();
      });
      control.addEventListener("wheel", (event) => {
        event.preventDefault();
        slider.value = Math.max(Number(slider.min), Math.min(Number(slider.max), Number(slider.value) + (event.deltaY > 0 ? -5 : 5)));
        update();
      }, {passive: false});
    }
  }

  rgbPairs.forEach((pair) => bindPair(pair, () => { model.setRgb(state, rgbPairs.map(([slider]) => Number(slider.value))); render(); }));
  hslPairs.forEach((pair) => bindPair(pair, () => { model.setRgb(state, model.hslToRgb(...hslPairs.map(([slider]) => Number(slider.value)))); render(); }));
  bindPair(alphaPair, () => { model.setAlphaPercent(state, Number(alphaPair[0].value)); render(); });
  hexInput.addEventListener("input", () => {
    const text = hexInput.value;
    const caret = hexInput.selectionStart;
    const valid = model.setHex(state, text.startsWith("#") ? text : "#" + text);
    hexInput.setCustomValidity(valid ? "" : T("Enter a complete hexadecimal color"));
    if (valid) {
      render();
      // Do not append alpha after the sixth digit while the user is typing RGBA.
      hexInput.value = text; hexInput.setSelectionRange(caret, caret);
    }
  });
  hexInput.addEventListener("blur", () => { if (hexInput.checkValidity()) render(); });
  modeSwitch.addEventListener("change", () => {
    byId("rgbSliders").classList.toggle("hidden", modeSwitch.checked);
    byId("hslSliders").classList.toggle("hidden", !modeSwitch.checked);
  });
  for (const [id, mode] of [["tab1", "solid"], ["tab2", "gradient"]]) byId(id).addEventListener("change", () => { model.setMode(state, mode); render(true); });
  endpointSwitch.addEventListener("change", () => { state.active = endpointSwitch.checked ? 1 : 0; render(true); });
  paletteCombo.addEventListener("change", populatePalette);
  canvas.addEventListener("pointerdown", (event) => { dragging = true; canvas.setPointerCapture(event.pointerId); sampleSpectrum(event); });
  canvas.addEventListener("pointermove", (event) => { if (dragging) sampleSpectrum(event); });
  canvas.addEventListener("pointerup", () => { dragging = false; });
  canvas.addEventListener("pointercancel", () => { dragging = false; });
  window.addEventListener("resize", () => { drawSpectrum(); render(); });
  function validateEditors() {
    const pairs = modeSwitch.checked ? hslPairs : rgbPairs;
    const inputs = [hexInput, ...pairs.map(([, input]) => input)];
    if (state.options.allow_alpha) inputs.push(alphaPair[1]);
    for (const input of inputs) {
      if (input === hexInput) continue;
      input.setCustomValidity("");
      const validity = input.validity;
      if (!validity.valid)
        input.setCustomValidity(T(validity.rangeUnderflow || validity.rangeOverflow ? "Value is out of range." : "Invalid input"));
    }
    return inputs.every((input) => input.reportValidity());
  }
  function confirm() {
    if (initialized && validateEditors()) emit("confirm", {selection: model.exportSelection(state)});
  }
  byId("saveColorBtn").addEventListener("click", () => {
    if (!initialized || !favoritesWritable || !validateEditors()) return;
    const selection = model.exportSelection(state);
    const key = JSON.stringify(selection);
    favorites = [selection, ...favorites.filter((favorite) => JSON.stringify(favorite) !== key)].slice(0, 24);
    drawFavorites(); emit("update_favorites", {favorites});
  });
  byId("confirmBtn").addEventListener("click", confirm);
  byId("cancelBtn").addEventListener("click", () => emit("cancel"));
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape") { event.preventDefault(); emit("cancel"); }
    else if (event.key === "Enter" && event.target instanceof HTMLInputElement && ["text", "number"].includes(event.target.type)) {
      event.preventDefault(); confirm();
    }
  });

  window.ColorPickerDialog = {
    init, handleMessage, focusInput, exportSelection: () => model.exportSelection(state),
    setFavorites: (values) => { favorites = model.normalizeFavorites(values); drawFavorites(); }
  };
  drawSpectrum(); populatePalette(); drawFavorites(); render(true); emit("ready");
})();
