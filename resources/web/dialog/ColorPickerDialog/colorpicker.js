"use strict";
(() => {
  if (window.ORCA_COLOR_PICKER_LANGUAGE) document.documentElement.lang = window.ORCA_COLOR_PICKER_LANGUAGE;
  const T = (key) => window.ORCA_UI_STRINGS?.[key] || key;
  document.title = T("Color Picker");
  for (const element of document.querySelectorAll("[data-i18n]")) element.textContent = T(element.dataset.i18n);
  for (const attribute of ["aria-label", "title", "placeholder"])
    for (const element of document.querySelectorAll(`[data-i18n-${attribute}]`))
      element.setAttribute(attribute, T(element.getAttribute(`data-i18n-${attribute}`)));
  function paletteName(name) {
    const ral = name.match(/^(\d{4}) (.+)$/);
    return ral ? `${ral[1]} ${T(ral[2])}` : /^%\d+$/.test(name) ? name : T(name);
  }

  const FAVORITE_SLOTS              = 16;
  const SPECTRUM_FOLLOWS_SATURATION = true; // true: the spectrum is drawn at the current saturation, false: it always shows full saturation
  const MULTI_COLOR_GRADIENTS       = true; // true: gradients may have more than 2 colors (− / + buttons and the multi-color palette entries), false: exactly 2 colors.
                                            // up to the native limit the host sends as options.max_gradient_colors.
  let   maxGradientColors           = 2;    // Stays 2 until init, and with a host that does not send a limit.

  const model = window.ColorPickerModel;
  const palettes = window.ColorPickerPalettes;
  const byId = (id) => document.getElementById(id);
  const canvas = byId("spectrum"), context = canvas.getContext("2d");
  const paletteCombo = byId("paletteCombo"), palette = byId("palette"), preview = byId("colorPreview"), confirmBtn = byId("confirmBtn");
  const modeSwitch = byId("modeSwitch"), satSlider = byId("satSlider"), opacitySlider = byId("opacitySlider");
  const gradientBar = byId("gradientCtrlBar"), gradientStops = byId("gradientStops"), gradientAdd = byId("gradientAdd"), gradientRemove = byId("gradientRemove");
  const hostGradientLimit = (options) => options.max_gradient_colors === undefined ? 2 : options.max_gradient_colors;
  const originalPreview = byId("originalPreview"), palettePanel = byId("palettePanel"), paletteToggle = byId("paletteToggle");
  const paletteSelect = byId("paletteSelect"), paletteFilter = byId("paletteFilter"), paletteSearchToggle = byId("paletteSearchToggle");
  const rgbInputs = ["r", "g", "b"].map((name) => byId(name + "Value"));
  const hslInputs = ["h", "s", "l"].map((name) => byId(name + "Value"));
  const hexInput = byId("hexInput"), opacityInput = byId("oValue");
  const pageId = Date.now().toString(36) + Math.random().toString(36).slice(2);
  let state = model.createState(), favorites = [], initialized = false, favoritesWritable = true;
  // The selection the dialog opened with; the original swatch shows it and restores it on click.
  let originalSelection = model.exportSelection(state);
  let lastContentHeight = 0, lastContentWidth = 0, resizePending = false;
  // Last HSL shown or set. Kept while it still produces the current color: hue 360 and 0 are
  // both red, and greys, black and white have no hue or saturation in RGB.
  let hslMemory = [0, 0, 50];
  function displayHsl(color) {
    const rgb = model.hslToRgb(...hslMemory);
    if (!rgb.every((value, index) => value === color[index])) hslMemory = model.rgbToHsl(...color.slice(0, 3));
    return hslMemory;
  }
  // Copy of `values` with one entry replaced.
  const replaced = (values, index, value) => values.map((current, i) => i === index ? value : current);
  function setHslChannel(index, value) {
    hslMemory = replaced(displayHsl(model.activeColor(state)), index, value);
    model.setRgb(state, model.hslToRgb(...hslMemory));
  }
  const round1 = (value) => Math.round(value * 10) / 10;

  function emit(command, data = {}) {
    window.dispatchEvent(new CustomEvent("color-picker-message", {detail: {command, ...data}}));
    if (window.wx && typeof window.wx.postMessage === "function")
      window.wx.postMessage(JSON.stringify({command, page_id: pageId, ...data}));
  }

  function scheduleResize() {
    if (!initialized || resizePending) return;
    resizePending = true;
    requestAnimationFrame(() => {
      resizePending = false;
      const rect = document.body.getBoundingClientRect();
      const height = Math.min(900 , Math.max(300, Math.ceil(rect.height)));
      const width  = Math.min(1200, Math.max(300, Math.ceil(rect.width )));
      if (height !== lastContentHeight || width !== lastContentWidth) {
        lastContentHeight = height; lastContentWidth = width;
        emit("resize", {width, height});
      }
    });
  }
  new ResizeObserver(scheduleResize).observe(document.body);

  function setSwatch(element, selection) {
    element.style.setProperty("--swatch-color", model.selectionBackground(selection));
  }

  // `editing` is the control the user is typing in. Its text is left alone so the caret,
  // partial input like "05" and fine steps that round-trip through bytes aren't overwritten.
  function render(resetEditors = false, editing = null) {
    const color = model.activeColor(state), hsl = displayHsl(color);
    const show = (input, value) => {
      if (input === editing) return;
      if (resetEditors) input.setCustomValidity("");
      if (resetEditors || input.checkValidity()) input.value = value;
    };
    show(hexInput, model.toHex(color).slice(1, state.options.allow_alpha ? 9 : 7));
    rgbInputs.forEach((input, index) => show(input, color[index]));
    // Shown in whole numbers; decimals can still be typed, and hslMemory keeps exact values.
    hslInputs.forEach((input, index) => show(input, Math.round(hsl[index])));
    // Percentage is a display only until explicitly edited. Never round it back
    // into the alpha byte on endpoint switches, RGB edits, or confirmation.
    show(opacityInput, Math.round(color[3] / 255 * 100));
    if (opacitySlider !== editing) opacitySlider.value = color[3] / 255 * 100;
    const rgb = color.slice(0, 3).join(",");
    opacitySlider.style.setProperty("--track-gradient", `linear-gradient(to right, rgba(${rgb},0), rgb(${rgb}))`);
    opacitySlider.style.setProperty("--thumb-color", `rgba(${rgb},${color[3] / 255})`);
    if (satSlider !== editing) satSlider.value = hsl[1];
    satSlider.style.setProperty("--track-gradient", `linear-gradient(to right, hsl(${hsl[0]},0%,${hsl[2]}%), hsl(${hsl[0]},100%,${hsl[2]}%))`);
    satSlider.style.setProperty("--thumb-color", `hsl(${hsl[0]},${hsl[1]}%,${hsl[2]}%)`);
    modeSwitch.checked = state.mode === "gradient";
    const selection = model.exportSelection(state);
    // Kept in the layout (visibility only) so the grid doesn't shift between Solid and Gradient.
    gradientBar.classList.toggle("invisible", state.mode !== "gradient");
    if (state.mode === "gradient") renderGradient(selection);
    setSwatch(preview, selection);
    preview.title = `${T("Save current color")}\n${selection.colors.join(" → ")}`;
    if (SPECTRUM_FOLLOWS_SATURATION && hsl[1] !== spectrumSaturation) { spectrumSaturation = hsl[1]; paintSpectrum(); }
    updateIndicator(hsl);
    updateConfirm();
  }

  // One thumb per gradient color, evenly spaced; rebuilt only when the number of colors changes.
  function renderGradient(selection) {
    const count = selection.colors.length;
    setSwatch(gradientStops.querySelector(".gradient-track"), selection);
    let thumbs = [...gradientStops.querySelectorAll(".gradient-thumb")];
    if (thumbs.length !== count) {
      thumbs.forEach((thumb) => thumb.remove());
      thumbs = selection.colors.map((_, index) => {
        const thumb = document.createElement("button");
        thumb.type = "button"; thumb.className = "gradient-thumb"; thumb.dataset.index = index;
        thumb.style.setProperty("--stop", index / (count - 1));
        thumb.setAttribute("aria-label", count === 2 ? T(index ? "Gradient end" : "Gradient start") : `${T("Gradient color")} ${index + 1}`);
        gradientStops.appendChild(thumb);
        return thumb;
      });
    }
    thumbs.forEach((thumb, index) => {
      thumb.style.setProperty("--thumb-color", selection.colors[index]);
      thumb.setAttribute("aria-pressed", String(state.active === index));
    });

    const multi = maxGradientColors > 2; // −/+ buttons only exist when more than 2 colors are allowed; − also needs a color to spare.
    gradientAdd.parentElement.classList.toggle("hidden", !multi);
    gradientRemove.classList.toggle("hidden", !multi || count <= 2);
    gradientAdd.classList.toggle("hidden", !multi);
    // aria-disabled instead of disabled: a disabled button gets no hover, so its tooltip would never show.
    const full = count >= maxGradientColors;
    gradientAdd.setAttribute("aria-disabled", String(full));
    gradientAdd.title = full ? T("Up to %d colors supported.").replace("%d", maxGradientColors) : T("Add color");
  }

  // Every editor the user can see; alpha only when this caller allows it.
  function editors() {
    const inputs = [hexInput, ...rgbInputs, ...hslInputs];
    if (state.options.allow_alpha) inputs.push(opacityInput);
    return inputs;
  }
  // OK stays disabled while any visible editor holds an invalid or incomplete value.
  function updateConfirm() {
    confirmBtn.disabled = !editors().every((input) => input.checkValidity());
  }

  // Matches the filter against the shown (translated) name, the English name and the hex digits.
  function paletteMatches(entry, query) {
    if (!query) return true;
    const haystack = [paletteName(entry.name), entry.name, entry.hex || "", entry.grad || ""].join(" ").toLowerCase();
    return query.split(/\s+/).every((word) => haystack.includes(word.replace(/^#/, "")));
  }

  function populatePalette() {
    palette.replaceChildren();
    const query = paletteSearchToggle.getAttribute("aria-pressed") === "true" ? paletteFilter.value.trim().toLowerCase() : "";
    for (const entry of palettes[paletteCombo.value] || []) {
      const selection = {type: entry.grad ? "gradient" : "solid", colors: entry.grad ? entry.grad.split("-") : [entry.hex]};
      if (!model.normalizeSelection(selection, state.options, maxGradientColors) || !paletteMatches(entry, query)) continue;
      const row    = document.createElement("button"); row.type = "button"; row.className = "color-item";
      const swatch = document.createElement("span"); swatch.className = "color-swatch";
      const name   = document.createElement("span"); name.className = "color-name"; name.textContent = paletteName(entry.name);
      setSwatch(swatch, model.normalizeSelection(selection, {allow_gradient: true, allow_alpha: true}, maxGradientColors));
      row.append(swatch, name); row.title = paletteName(entry.name);
      row.addEventListener("click", () => {
        // Solid palette colors edit the active endpoint while gradient mode is on.
        const changed = entry.grad ? model.applySelection(state, selection) : model.setHex(state, entry.hex);
        if (changed) render(true);
      });
      palette.appendChild(row);
    }
    byId("paletteEmpty").classList.toggle("hidden", palette.childElementCount > 0);
    palette.classList.toggle("hidden", palette.childElementCount === 0);
  }

  // Sidebar state the host keeps in the app config: open/closed and the chosen palette.
  const uiState = () => ({sidebar_open: palettePanel.classList.contains("open"), palette: paletteCombo.value});
  function setSidebarOpen(open) {
    palettePanel.classList.toggle("open", open);
    paletteToggle.setAttribute("aria-expanded", String(open));
  }
  // The search button swaps the combo box for a filter box; closing it clears the filter.
  function setPaletteSearch(open) {
    paletteSearchToggle.setAttribute("aria-pressed", String(open));
    paletteSelect.classList.toggle("hidden", open);
    paletteFilter.classList.toggle("hidden", !open);
    if (open) paletteFilter.focus();
    else paletteFilter.value = "";
    populatePalette();
  }

  function drawFavorites() {
    const grid = byId("userColors"); grid.replaceChildren();
    // Favorites keep every color, but only those this page can select are shown.
    const visible = favorites.filter((favorite) => model.normalizeSelection(favorite, {...state.options, allow_alpha: true}, maxGradientColors));
    for (let index = 0; index < FAVORITE_SLOTS; ++index) {
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
    maxGradientColors = Math.min(MULTI_COLOR_GRADIENTS ? model.MAX_GRADIENT_COLORS : 2, hostGradientLimit(payload.options || {}));
    // A gradient the host allows but this page does not (switch off) opens as its first color, like
    // other multi-color selections the picker cannot edit; OK then replaces it, Cancel keeps it.
    let selection = payload.selection, preserve = payload.preserve_multi_color === true;
    if (selection?.type === "gradient" && selection.colors?.length > maxGradientColors) {
      selection = {type: "solid", colors: [selection.colors[0]]};
      preserve = true;
    }
    const next = model.createState({...payload.options, max_gradient_colors: maxGradientColors}, selection);
    if (!next) return false;
    byId("selectionNotice").hidden = !preserve;
    state = next; favorites = model.normalizeFavorites(payload.favorites || []);
    originalSelection = model.exportSelection(state);
    setSwatch(originalPreview, originalSelection);
    originalPreview.title = `${T("Original color")}\n${originalSelection.colors.join(" → ")}`;
    favoritesWritable = payload.favorites_writable !== false;
    preview.disabled = !favoritesWritable;
    byId("modeControls").classList.toggle("hidden", !state.options.allow_gradient);
    for (const option of paletteCombo.options) option.hidden = option.value === "gradient" && !state.options.allow_gradient;
    const ui = payload.ui || {}; // Restore the sidebar the way it was left; a palette this caller can't use falls back to basic.
    const saved = [...paletteCombo.options].find((option) => option.value === ui.palette && !option.hidden);
    if (saved) paletteCombo.value = saved.value;
    if (!state.options.allow_gradient && paletteCombo.value === "gradient") paletteCombo.value = "basic";
    setSidebarOpen(ui.sidebar_open === true);
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
      const hostLimit = options && hostGradientLimit(options);
      if (!options || typeof options.allow_gradient !== "boolean" || typeof options.allow_alpha !== "boolean" ||
          !Number.isInteger(hostLimit) || hostLimit < 2 || hostLimit > model.MAX_GRADIENT_COLORS ||
          !model.normalizeSelection(payload.selection, options, hostLimit) || !validFavorites(payload.favorites) ||
          (payload.favorites_writable !== undefined && typeof payload.favorites_writable !== "boolean") ||
          (payload.preserve_multi_color !== undefined && typeof payload.preserve_multi_color !== "boolean") ||
          (payload.ui !== undefined && (!payload.ui || typeof payload.ui !== "object" ||
            (payload.ui.sidebar_open !== undefined && typeof payload.ui.sidebar_open !== "boolean") ||
            (payload.ui.palette !== undefined && typeof payload.ui.palette !== "string")))) return false;
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
    return Array.isArray(values) && values.length <= 24 && values.every((value) => model.normalizeSelection(value, {allow_gradient: true, allow_alpha: true}, model.MAX_GRADIENT_COLORS));
  }

  // Saturation the spectrum is currently painted at (always 100 when the option is off).
  let spectrumSaturation = 100;
  function drawSpectrum() {
    const rect = canvas.getBoundingClientRect();
    canvas.width = Math.max(1, Math.round(rect.width)); canvas.height = Math.max(1, Math.round(rect.height));
    paintSpectrum();
  }
  function paintSpectrum() {
    const hue = context.createLinearGradient(0, 0, canvas.width, 0);
    for (let i = 0; i <= 6; ++i) hue.addColorStop(i / 6, `hsl(${i * 60},100%,50%)`);
    context.fillStyle = hue; context.fillRect(0, 0, canvas.width, canvas.height);
    const shade = context.createLinearGradient(0, 0, 0, canvas.height);
    shade.addColorStop(0, "#FFFFFF"); shade.addColorStop(0.5, "#FFFFFF00"); shade.addColorStop(0.5, "#00000000"); shade.addColorStop(1, "#000000");
    context.fillStyle = shade; context.fillRect(0, 0, canvas.width, canvas.height);
    // HSL desaturation blends each color toward the grey of the same lightness, and on this
    // canvas that grey only depends on the row (white at the top, black at the bottom).
    if (spectrumSaturation < 100) {
      const grey = context.createLinearGradient(0, 0, 0, canvas.height);
      grey.addColorStop(0, "#FFFFFF"); grey.addColorStop(1, "#000000");
      context.globalAlpha = 1 - spectrumSaturation / 100;
      context.fillStyle = grey; context.fillRect(0, 0, canvas.width, canvas.height);
      context.globalAlpha = 1;
    }
  }

  const indicator = byId("colorIndicator");
  function updateIndicator(hsl) {
    indicator.style.left = hsl[0] / 360 * (canvas.width - 1) + "px";
    indicator.style.top = (1 - hsl[2] / 100) * (canvas.height - 1) + "px";
  }

  // Spectrum position maps straight to hue (x, 0..360) and lightness (y, 100..0); saturation is
  // the third dimension and comes from the slider below. hslMemory keeps these exact values,
  // including hue 360 at the right edge and the hue of the white and black corners.
  function sampleSpectrum(event) {
    const rect = canvas.getBoundingClientRect();
    const fraction = (client, start, size, pixels) =>
      Math.max(0, Math.min(pixels - 1, Math.floor((client - start) * pixels / size))) / Math.max(1, pixels - 1);
    const saturation = displayHsl(model.activeColor(state))[1];
    hslMemory = [round1(fraction(event.clientX, rect.left, rect.width, canvas.width) * 360), saturation,
                 round1((1 - fraction(event.clientY, rect.top, rect.height, canvas.height)) * 100)];
    model.setRgb(state, model.hslToRgb(...hslMemory));
    // A pick replaces every editor's value, so clear any invalid state left from typing.
    render(true);
  }

  const clamp = (control, value) => Math.min(Number(control.max), Math.max(Number(control.min), value));
  function bindValue(control, apply) {
    // Number fields are required, so an empty field is invalid; a range input always has a value.
    if (control.type === "number") control.required = true;
    const commit = (value) => { apply(value); render(false, control); };
    control.addEventListener("input", () => {
      control.setCustomValidity("");
      // Out-of-range numbers snap to the field's min/max instead of becoming invalid.
      if (control.value !== "") {
        const clamped = clamp(control, Number(control.value));
        if (clamped !== Number(control.value)) control.value = clamped;
      }
      if (control.checkValidity()) commit(Number(control.value));
    });
    control.addEventListener("wheel", (event) => {
      event.preventDefault();
      const step = control.type === "range" ? 5 : 1;
      control.value = clamp(control, (Number(control.value) || 0) + (event.deltaY > 0 ? -step : step));
      control.setCustomValidity("");
      commit(Number(control.value));
    }, {passive: false});
    // Normalize the text (e.g. "05" → "5") once the user leaves the field.
    control.addEventListener("blur", () => { if (control.checkValidity()) render(); });
  }

  rgbInputs.forEach((input, index) => bindValue(input, (value) => model.setRgb(state, replaced(model.activeColor(state).slice(0, 3), index, value))));
  hslInputs.forEach((input, index) => bindValue(input, (value) => setHslChannel(index, value)));
  bindValue(satSlider, (value) => setHslChannel(1, value));
  bindValue(opacityInput, (value) => model.setAlphaPercent(state, value));
  bindValue(opacitySlider, (value) => model.setAlphaPercent(state, value));
  // The opacity slider takes the gradient bar's row while the opacity field or the slider is in
  // use: it opens when the field is clicked or focused, and closes on a click or focus elsewhere.
  const thirdRow = byId("thirdRow"), alphaControls = byId("alphaControls");
  const inOpacityControls = (target) => target instanceof Element && target.closest("#alphaControls, #opacitySliderBar");
  const setOpacitySliderOpen = (open) => thirdRow.classList.toggle("opacity-open", open);
  for (const type of ["focusin", "click"]) alphaControls.addEventListener(type, () => setOpacitySliderOpen(true));
  for (const type of ["pointerdown", "focusin"])
    document.addEventListener(type, (event) => { if (!inOpacityControls(event.target)) setOpacitySliderOpen(false); });
  hexInput.addEventListener("input", () => {
    const text = hexInput.value;
    const valid = model.setHex(state, text.startsWith("#") ? text : "#" + text);
    hexInput.setCustomValidity(valid ? "" : T("Enter a complete hexadecimal color"));
    // render() leaves the typed text alone, so alpha isn't appended after the sixth digit.
    if (valid) render(false, hexInput);
  });
  hexInput.addEventListener("blur", () => { if (hexInput.checkValidity()) render(); });
  modeSwitch.addEventListener("change", () => {

    setOpacitySliderOpen(false); // Close the opacity slider before the row changes, so it doesn't linger over the gradient bar.
    model.setMode(state, modeSwitch.checked ? "gradient" : "solid");
    render(true);
  });
  // Click on the track selects the nearest thumb. Keyboard users tab to a thumb and press Enter or Space.
  gradientStops.addEventListener("click", (event) => {
    if (state.mode !== "gradient") return;
    // A thumb is read from its index: a keyboard-triggered click has no pointer position.
    const thumb = event.target.closest(".gradient-thumb"), rect = gradientStops.getBoundingClientRect();
    const last = state.gradient.length - 1;
    // Thumbs sit at 10px + (width - 20px) * i / last, matching the CSS.
    const fraction = Math.max(0, Math.min(1, (event.clientX - rect.left - 10) / Math.max(1, rect.width - 20)));
    const index = thumb ? Number(thumb.dataset.index) : Math.round(fraction * last);
    if (index !== state.active) { state.active = index; render(true); }
  });
  gradientAdd.addEventListener("click", () => { if (gradientAdd.getAttribute("aria-disabled") !== "true" && model.addGradientColor(state)) render(true); });
  gradientRemove.addEventListener("click", () => { if (model.removeGradientColor(state)) render(true); });
  originalPreview.addEventListener("click", () => { if (model.applySelection(state, originalSelection)) render(true); });
  paletteToggle.addEventListener("click", () => {
    setSidebarOpen(!palettePanel.classList.contains("open"));
    if (initialized) emit("update_ui", uiState());
  });
  paletteCombo.addEventListener("change", () => {
    populatePalette();
    if (initialized) emit("update_ui", uiState());
  });
  paletteSearchToggle.addEventListener("click", () => setPaletteSearch(paletteSearchToggle.getAttribute("aria-pressed") !== "true"));
  paletteFilter.addEventListener("input", populatePalette);
  // Pointer capture ends by itself on pointerup/pointercancel, so it doubles as the drag state.
  canvas.addEventListener("pointerdown", (event) => { canvas.setPointerCapture(event.pointerId); sampleSpectrum(event); });
  canvas.addEventListener("pointermove", (event) => { if (canvas.hasPointerCapture(event.pointerId)) sampleSpectrum(event); });
  // The spectrum takes the panel's free height, which also changes with the notice, alpha and
  // gradient controls, so redraw on any size change rather than on window resize only.
  new ResizeObserver(() => { drawSpectrum(); updateIndicator(displayHsl(model.activeColor(state))); }).observe(canvas.parentElement);
  // Bubble phase on document runs after each editor's own handler has set its validity.
  document.addEventListener("input", updateConfirm);
  // Hex keeps its own message from its input handler; number fields get a translated one.
  function validateEditors() {
    const inputs = editors();
    for (const input of inputs.filter((input) => input !== hexInput)) {
      input.setCustomValidity("");
      const {valid, rangeUnderflow, rangeOverflow} = input.validity;
      if (!valid) input.setCustomValidity(T(rangeUnderflow || rangeOverflow ? "Value is out of range." : "Invalid input"));
    }
    return inputs.every((input) => input.reportValidity());
  }
  function confirm() {
    if (initialized && validateEditors()) emit("confirm", {selection: model.exportSelection(state)});
  }
  preview.addEventListener("click", () => {
    if (!initialized || !favoritesWritable || !validateEditors()) return;
    const selection = model.exportSelection(state);
    const key = JSON.stringify(selection);
    favorites = [selection, ...favorites.filter((favorite) => JSON.stringify(favorite) !== key)].slice(0, FAVORITE_SLOTS);
    drawFavorites(); emit("update_favorites", {favorites});
  });
  confirmBtn.addEventListener("click", confirm);
  byId("cancelBtn").addEventListener("click", () => emit("cancel"));
  document.addEventListener("keydown", (event) => {
    // Escape in the filter box closes the search first; a second Escape cancels the dialog.
    if (event.key === "Escape" && event.target === paletteFilter) { event.preventDefault(); setPaletteSearch(false); paletteSearchToggle.focus(); }
    else if (event.key === "Escape") { event.preventDefault(); emit("cancel"); }
    else if (event.key === "Enter" && event.target instanceof HTMLInputElement && ["text", "number"].includes(event.target.type)) {
      event.preventDefault(); confirm();
    }
  });

  window.ColorPickerDialog = {
    init, handleMessage, focusInput, exportSelection: () => model.exportSelection(state),
    setFavorites: (values) => { favorites = model.normalizeFavorites(values); drawFavorites(); }
  };
  // The host passes the saved sidebar state before the page loads (the same values come again in
  // init), so the first paint already matches the window size the host opened with.
  const earlyUi = window.ORCA_COLOR_PICKER_UI;
  if (earlyUi && typeof earlyUi === "object") {
    if (typeof earlyUi.palette === "string" && [...paletteCombo.options].some((option) => option.value === earlyUi.palette))
      paletteCombo.value = earlyUi.palette;
    if (typeof earlyUi.sidebar_open === "boolean") setSidebarOpen(earlyUi.sidebar_open);
  }
  setSwatch(originalPreview, originalSelection);
  drawSpectrum(); populatePalette(); drawFavorites(); render(true); emit("ready");
})();
