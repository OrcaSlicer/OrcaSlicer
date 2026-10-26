// Optional real-DOM regressions. Use an existing Playwright installation:
// ORCA_PLAYWRIGHT_MODULE=<module path> ORCA_BROWSER_EXECUTABLE=<browser path>
// ORCA_BROWSER_ENGINE=webkit exercises the macOS WebView layout engine.
// node --test resources/web/dialog/ColorPickerDialog/colorpicker.browser.test.js
const {test, before, after} = require("node:test");
const assert = require("node:assert/strict");
const {readFileSync} = require("node:fs");
const {resolve} = require("node:path");
const {pathToFileURL} = require("node:url");
const engineName = process.env.ORCA_BROWSER_ENGINE || "chromium";
let engine;
try { engine = require(process.env.ORCA_PLAYWRIGHT_MODULE || "playwright")[engineName]; }
catch (error) { if (error.code !== "MODULE_NOT_FOUND") throw error; }
const options = {skip: engine ? false : "Use an existing Playwright installation via ORCA_PLAYWRIGHT_MODULE"};
const url = pathToFileURL(resolve(__dirname, "index.html")).href;
let browser;
before(async () => { if (engine) browser = await engine.launch({headless: true, ...(process.env.ORCA_BROWSER_EXECUTABLE ? {executablePath: process.env.ORCA_BROWSER_EXECUTABLE} : {})}); });
after(async () => { if (browser) await browser.close(); });
async function open(t, strings = {}, language = "") {
  const page = await browser.newPage({viewport: {width: 600, height: 660}}), errors = [];
  page.on("pageerror", error => errors.push(error.message));
  t.after(async () => { await page.close(); assert.deepEqual(errors, []); });
  await page.addInitScript(({table, language}) => {
    window.ORCA_UI_STRINGS = table;
    window.ORCA_COLOR_PICKER_LANGUAGE = language;
    window.messages = [];
    window.wx = {postMessage: text => messages.push(JSON.parse(text))};
  }, {table: strings, language});
  await page.goto(url);
  const ready = await page.evaluate(() => messages[0]);
  assert.equal(ready.command, "ready");
  return {page, ready, send: payload => page.evaluate(value => ColorPickerDialog.handleMessage(value), payload)};
}
function init(ready, gradient = false, alpha = false) {
  return {command: "init", page_id: ready.page_id, options: {allow_gradient: gradient, allow_alpha: alpha},
    selection: {type: "solid", colors: ["#01234567"]},
    ui: {sidebar_open: true, palette: "basic"},
    favorites: [{type: "gradient", colors: ["#FF000040", "#0000FF90"]}, {type: "solid", colors: ["#ABCDEF40"]}]};
}
async function count(page, command) { return page.evaluate(value => messages.filter(message => message.command === value).length, command); }

for (const gradient of [false, true]) for (const alpha of [false, true]) {
  test(`Native capabilities gate gradient=${gradient} and alpha=${alpha}`, options, async t => {
    const {page, ready, send} = await open(t);
    assert.equal(await page.locator("#confirmBtn").isDisabled(), true);
    assert.equal(await send({...init(ready), options: {allow_gradient: "yes", allow_alpha: alpha}}), false);
    assert.equal(await send({...init(ready), page_id: "stale"}), false);
    assert.equal(await send(init(ready, gradient, alpha)), true);
    assert.equal(await page.locator("#modeControls").isVisible(), gradient);
    assert.equal(await page.locator("#alphaControls").isVisible(), alpha);
    assert.equal(await page.locator("#userColors button:not(.empty)").count(), gradient ? 2 : 1);
    await page.locator("#userColors button:not(.empty)").last().click();
    const selection = await page.evaluate(() => ColorPickerDialog.exportSelection());
    assert.deepEqual(selection, {type: "solid", colors: [alpha ? "#ABCDEF40" : "#ABCDEFFF"]});
    assert.equal(await send(init(ready, gradient, alpha)), true);
    assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), selection, "repeated ready/init retains edits");
    await page.locator("#colorPreview").click();
    const favorites = await page.evaluate(() => messages.at(-1).favorites);
    assert(favorites.some(favorite => favorite.colors[0] === "#ABCDEF40"), "original favorites retain their alpha");
    assert(favorites.some(favorite => favorite.type === "gradient"), "filtered gradients remain stored");
    await page.locator("#confirmBtn").click();
    assert.deepEqual(await page.evaluate(() => messages.at(-1).selection), selection);
  });
}

test("Visible incomplete editors block confirmation and favorites until recovery", options, async t => {
  const {page, ready, send} = await open(t); await send(init(ready, true, true));
  for (const [id, value, valid] of [["rValue", "", "21"], ["oValue", "", "50"], ["hexInput", "GG", "12345680"]]) {
    await page.locator("#" + id).fill(value);
    const confirms = await count(page, "confirm"), saves = await count(page, "update_favorites");
    await page.locator("#confirmBtn").evaluate(element => element.click()); await page.locator("#colorPreview").click();
    assert.equal(await count(page, "confirm"), confirms); assert.equal(await count(page, "update_favorites"), saves);
    await page.locator("#" + id).fill(valid); await page.locator("#" + id).press("Enter");
    assert.equal(await count(page, "confirm"), confirms + 1);
  }
  await page.locator("#hValue").fill(""); // HSL is now visible beside RGB, so incomplete HSL must block.
  const hslConfirms = await count(page, "confirm");
  await page.locator("#confirmBtn").evaluate(element => element.click());
  assert.equal(await count(page, "confirm"), hslConfirms);
  await page.locator("#hValue").fill("100");
  const confirms = await count(page, "confirm"); await page.locator("#confirmBtn").click();
  assert.equal(await count(page, "confirm"), confirms + 1);
  await page.locator("#rValue").fill(""); await page.locator("#rValue").press("Escape");
  assert.equal(await page.evaluate(() => messages.at(-1).command), "cancel");
});

test("Numeric channel and opacity edits clamp to their field bounds", options, async t => {
  const {page, ready, send} = await open(t); await send(init(ready, true, true));
  for (const [id, input, expected] of [["rValue", "999", "255"], ["rValue", "-1", "0"],
                                     ["hValue", "361", "360"], ["oValue", "101", "100"], ["oValue", "-1", "0"]]) {
    await page.locator("#" + id).fill(input);
    assert.equal(await page.locator("#" + id).inputValue(), expected);
    assert.equal(await page.locator("#" + id).evaluate(element => element.checkValidity()), true);
  }
  assert.equal((await page.evaluate(() => ColorPickerDialog.exportSelection())).colors[0].slice(-2), "00");
});

test("Endpoint edits and favorites retain ordered byte alpha", options, async t => {
  const {page, ready, send} = await open(t);
  await send({...init(ready, true, true), selection: {type: "gradient", colors: ["#FF000001", "#0000FF80"]}});
  await page.locator(".gradient-thumb[data-index='1']").click();
  await page.locator('.color-item[title="Red"]').first().click();
  assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), {type: "gradient", colors: ["#FF000001", "#FF000080"]});
  await page.locator("#hexInput").fill("0000FF80"); await page.locator(".gradient-thumb[data-index='0']").click();
  assert.equal(await page.locator("#hexInput").inputValue(), "FF000001");
  await page.locator("#colorPreview").click();
  assert.deepEqual(await page.evaluate(() => messages.at(-1).favorites[0]), {type: "gradient", colors: ["#FF000001", "#0000FF80"]});
});

test("Read-only favorites remain selectable and late documents cannot initialize", options, async t => {
  const {page, ready, send} = await open(t); await send({...init(ready), favorites_writable: false});
  assert.equal(await page.locator("#colorPreview").isDisabled(), true);
  await page.locator("#userColors button:not(.empty)").first().click();
  assert.equal(await page.locator("#hexInput").inputValue(), "ABCDEF");
  await page.reload();
  assert.equal(await send(init(ready)), false); assert.equal(await page.locator("#confirmBtn").isDisabled(), true);
});

test("Document strings translate palette names and editor errors while retaining RAL codes and numeric labels", options, async t => {
  const source = readFileSync(resolve(__dirname, "../../../../src/slic3r/GUI/ColorPickerDialog.cpp"), "utf8");
  const keys = [...source.matchAll(/_u8L\("([^"\n]+)"\)/g)].map(match => match[1]);
  const strings = Object.fromEntries(keys.map(key => [key, "Translated: " + key]));
  const {page, ready, send} = await open(t, strings); await send({...init(ready, true, true), options: {allow_gradient: true, allow_alpha: true, max_gradient_colors: 7}, preserve_multi_color: true});
  assert.equal(await page.title(), strings["Color Picker"]);
  for (const element of await page.locator("[data-i18n]").all()) assert.equal(await element.textContent(), strings[await element.getAttribute("data-i18n")]);
  for (const attribute of ["aria-label", "title"]) for (const element of await page.locator(`[data-i18n-${attribute}]`).all())
    assert.equal((await element.getAttribute(attribute)).split("\n")[0], strings[await element.getAttribute(`data-i18n-${attribute}`)]);
  assert.equal(await page.locator("#selectionNotice").textContent(), strings["The current multi-color selection is kept until you choose a different color."]);
  for (const palette of ["basic", "orca", "ral", "gradient"]) {
    await page.selectOption("#paletteCombo", palette);
    const names = await page.evaluate(value => ColorPickerPalettes[value].map(entry => entry.name), palette);
    assert.deepEqual(await page.locator(".color-name").allTextContents(), names.map(name => {
      const ral = name.match(/^(\d{4}) (.+)$/); return ral ? `${ral[1]} ${strings[ral[2]]}` : /^%\d+$/.test(name) ? name : strings[name];
    }));
  }
  await page.locator("#hexInput").fill("bad");
  assert.equal(await page.locator("#hexInput").evaluate(element => element.validationMessage), strings["Enter a complete hexadecimal color"]);
  await page.locator("#hexInput").fill("12345680");
  for (const [id, invalid, valid, key] of [["rValue", "", "18", "Invalid input"],
                                         ["oValue", "", "50", "Invalid input"]]) {
    const confirms = await count(page, "confirm");
    await page.locator("#" + id).fill(invalid);
    await page.locator("#confirmBtn").evaluate(element => element.click());
    await page.locator("#colorPreview").click(); // Saving invokes the same translated validation while OK is disabled.
    assert.equal(await count(page, "confirm"), confirms);
    assert.equal(await page.locator("#" + id).evaluate(element => element.validationMessage), strings[key]);
    await page.locator("#" + id).fill(valid);
    assert.equal(await page.locator("#" + id).evaluate(element => element.validationMessage), "");
    await page.locator("#confirmBtn").click();
    assert.equal(await count(page, "confirm"), confirms + 1);
  }
  await page.locator("#hValue").fill("361");
  await page.locator("#colorPreview").click();
  assert.equal(await page.locator("#hValue").inputValue(), "360", "out-of-range hue clamps at its boundary");
  await page.locator(".color-item").first().click();
  assert.equal(await page.locator("#hValue").evaluate(element => element.validationMessage), "", "a new palette selection clears the previous editor error");
});

test("Document language and escaped labels preserve untranslated English fallback", options, async t => {
  const cancel = '取消 "Cancel" \\ /\n下一行';
  const {page, ready, send} = await open(t, {Cancel: cancel}, "zh-CN");
  await send(init(ready));
  assert.equal(await page.locator("html").getAttribute("lang"), "zh-CN");
  assert.equal(await page.locator("#cancelBtn").textContent(), cancel);
  assert.equal(await page.title(), "Color Picker");
  assert.equal(await page.locator(".color-name").first().textContent(), "Red");
  assert.deepEqual(await page.locator("label[for=hValue],label[for=sValue],label[for=lValue],label[for=rValue],label[for=gValue],label[for=bValue]").allTextContents(), ["H", "S", "L", "R", "G", "B"]);
  assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), {type: "solid", colors: ["#012345FF"]});
});

test("Global control colors follow the system while host semantic colors follow the app", options, async t => {
  const {page, ready, send} = await open(t); await send({...init(ready, true, true), selection: {type: "gradient", colors: ["#FF000001", "#0000FF80"]}});
  const initial = await page.evaluate(() => ({selection: ColorPickerDialog.exportSelection(), spectrum: document.getElementById("spectrum").toDataURL()}));
  for (const theme of ["light", "dark"]) for (const system of ["light", "dark"]) {
    await page.emulateMedia({colorScheme: system});
    await page.evaluate(value => {
      document.documentElement.dataset.orcaTheme = value;
      const colors = value === "light" ? {bg: "#ffffff", fg: "#262e30", muted: "#363636", border: "#dbdbdb", accent: "#009688"} : {bg: "#2d2d31", fg: "#efeff0", muted: "#b2b3b5", border: "#36363b", accent: "#00675b"};
      for (const [key, color] of Object.entries(colors)) document.documentElement.style.setProperty("--orca-" + key, color);
      document.documentElement.style.setProperty("--orca-accent-fg", "#ffffff");
    }, theme);
    await page.evaluate(async () => {
      await Promise.all(document.getAnimations().map(animation => animation.finished.catch(() => {})));
    });
    const colors = await page.evaluate(() => {
      const body = getComputedStyle(document.body);
      const tokenColor = token => {
        const probe = document.createElement("span"); probe.style.cssText = `position:absolute;visibility:hidden;color:var(${token})`;
        document.body.appendChild(probe); const color = getComputedStyle(probe).color; probe.remove(); return color;
      };
      const mode = document.querySelector("label[for=modeSwitch]");
      return {background: body.backgroundColor, foreground: body.color,
        modeBackground: getComputedStyle(mode).backgroundColor,
        accent: getComputedStyle(mode, "::before").backgroundColor,
        hostPanel: tokenColor("--panel"), hostText: tokenColor("--text")};
    });
    assert.equal(colors.background, system === "light" ? "rgb(255, 255, 255)" : "rgb(45, 45, 49)", `${theme} app / ${system} system body`);
    assert.equal(colors.foreground, system === "light" ? "rgb(38, 46, 48)" : "rgb(239, 239, 240)");
    assert.equal(colors.modeBackground, system === "light" ? "rgb(240, 240, 240)" : "rgb(36, 36, 40)");
    assert.equal(colors.accent, theme === "light" ? "rgb(0, 150, 136)" : "rgb(0, 103, 91)", "shared accent follows the injected app theme");
    assert.equal(colors.hostPanel, theme === "light" ? "rgb(255, 255, 255)" : "rgb(45, 45, 49)");
    assert.equal(colors.hostText, theme === "light" ? "rgb(38, 46, 48)" : "rgb(239, 239, 240)");
    assert.deepEqual(await page.evaluate(() => ({selection: ColorPickerDialog.exportSelection(), spectrum: document.getElementById("spectrum").toDataURL()})), initial);
    for (const id of ["modeSwitch"]) for (const checked of [false, true]) {
      if (await page.locator("#" + id).isChecked() !== checked) await page.locator(`label[for=${id}]`).click();
      await page.evaluate(async () => {
        await Promise.all(document.getAnimations().map(animation => animation.finished.catch(() => {})));
      });
      const result = await page.locator(`label[for=${id}]`).evaluate((element, selected) => {
        const tokenColor = token => {
          const probe = document.createElement("span"); probe.style.cssText = `position:absolute;visibility:hidden;color:var(${token})`;
          document.body.appendChild(probe); const color = getComputedStyle(probe).color; probe.remove(); return color;
        };
        const labels = element.querySelectorAll(".switch-label");
        return {selected: getComputedStyle(labels[selected ? 1 : 0]).color,
          unselected: getComputedStyle(labels[selected ? 0 : 1]).color,
          accentForeground: tokenColor("--button-fg-light"), mutedForeground: tokenColor("--fg-color-label")};
      }, checked);
      assert.equal(result.selected, result.accentForeground, `${theme} app / ${system} system ${id} selected text uses the accent foreground`);
      assert.equal(result.selected, "rgb(254, 254, 254)", "global selected text stays light in both system schemes");
      assert.equal(result.unselected, result.mutedForeground, `${theme} app / ${system} system ${id} unselected text stays muted`);
      assert.equal(result.unselected, system === "light" ? "rgb(54, 54, 54)" : "rgb(178, 179, 181)");
      const selected = await page.evaluate(() => ColorPickerDialog.exportSelection());
      assert.equal(selected.type, checked ? "gradient" : "solid");
      if (checked) assert.deepEqual(selected, initial.selection, "returning to gradient keeps ordered endpoint byte alpha");
    }
  }
});

test("Sidebar resizes from a compact viewport and shrinks after every collapse", options, async t => {
  const {page, ready, send} = await open(t);
  await page.setViewportSize({width: 340, height: 380});
  await send({...init(ready, true, true), ui: {sidebar_open: false, palette: "basic"}});
  const selection = await page.evaluate(() => ColorPickerDialog.exportSelection());
  for (const [index, sidebarOpen] of [false, true, false, true, false].entries()) {
    const width = sidebarOpen ? 510 : 340;
    await page.waitForFunction(expected => messages.filter(message => message.command === "resize").at(-1)?.width === expected, width);
    const size = await page.evaluate(() => messages.filter(message => message.command === "resize").at(-1));
    assert.equal(size.height, 380);
    await page.setViewportSize({width: size.width, height: size.height});
    await page.waitForTimeout(100);
    assert(await page.evaluate(() => {
      const body = document.body.getBoundingClientRect();
      const picker = document.querySelector(".right-panel").getBoundingClientRect();
      return body.width === innerWidth && picker.right <= body.right && picker.bottom <= body.bottom;
    }), "the host viewport contains the full picker");
    const resizes = await count(page, "resize");
    await page.waitForTimeout(100);
    assert.equal(await count(page, "resize"), resizes, "fitting the host does not cause a resize loop");
    assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), selection);
    if (index < 4) await page.locator("#paletteToggle").click();
  }
});

test("Floating panel fits capabilities and wrapped translations without a resize feedback loop", options, async t => {
  const strings = {Color: "A long translated color label", Gradient: "A long translated gradient label", Cancel: "A longer translated cancel action", OK: "A longer translated confirmation action",
    "The current multi-color selection is kept until you choose a different color.": "A translated explanation that wraps across the compact floating panel and keeps the original multi-color selection until another color is chosen."};
  for (const gradient of [false, true]) for (const alpha of [false, true]) {
    const {page, ready, send} = await open(t, strings);
    await page.setViewportSize({width: 550, height: 520});
    await send({...init(ready, gradient, alpha), preserve_multi_color: true});
    await page.waitForFunction(() => messages.some(message => message.command === "resize"));
    const height = await page.evaluate(() => messages.filter(message => message.command === "resize").at(-1).height);
    assert(height >= 300 && height <= 900);
    assert.equal(height, await page.locator("body").evaluate(element => Math.ceil(element.getBoundingClientRect().height)));
    assert(await page.evaluate(() => messages.findIndex(message => message.command === "initialized") < messages.findIndex(message => message.command === "resize")));
    await page.setViewportSize({width: 550, height});
    await page.waitForTimeout(100);
    const resizes = await count(page, "resize");
    await page.setViewportSize({width: 550, height: height + 100});
    await page.waitForTimeout(100);
    assert.equal(await count(page, "resize"), resizes, "extra viewport height never changes intrinsic content height");
    const controls = await page.evaluate(() => {
      const body = document.body.getBoundingClientRect();
      return ["confirmBtn", "cancelBtn", ...(document.getElementById("alphaControls").classList.contains("hidden") ? [] : ["oValue"])].map(id => {
        const rect = document.getElementById(id).getBoundingClientRect();
        return rect.left >= body.left && rect.right <= body.right && rect.bottom <= body.bottom;
      });
    });
    assert(controls.every(Boolean), "wrapped buttons and opacity fit inside the panel");
    const edited = await page.evaluate(() => ColorPickerDialog.exportSelection());
    await page.evaluate(() => {
      document.getElementById("selectionNotice").textContent += " Additional wrapped text. ".repeat(15);
    });
    await page.waitForFunction(previous => messages.filter(message => message.command === "resize").length > previous, resizes);
    assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), edited, "content resize leaves the selection untouched");
    // The compact layout now puts saved swatches to the right of both previews.
    assert(await page.evaluate(() => {
      const favorites = document.getElementById("userColors").getBoundingClientRect();
      const preview = document.getElementById("colorPreview").getBoundingClientRect();
      return favorites.left >= preview.right;
    }), "favorites do not overlap the preview");
    await page.locator("#confirmBtn").scrollIntoViewIfNeeded();
    assert.equal(await page.locator("#confirmBtn").isVisible(), true);
  }
});

test("Native focus hook waits for initialization and preserves active partial edits", options, async t => {
  const {page, ready, send} = await open(t);
  await page.evaluate(() => ColorPickerDialog.focusInput());
  assert.notEqual(await page.evaluate(() => document.activeElement.id), "hexInput");
  await send(init(ready, true, true));
  await page.evaluate(() => ColorPickerDialog.focusInput());
  assert.equal(await page.evaluate(() => document.activeElement.id), "hexInput");
  await page.locator("#rValue").fill("");
  const selection = await page.evaluate(() => ColorPickerDialog.exportSelection());
  await page.evaluate(() => ColorPickerDialog.focusInput());
  assert.equal(await page.evaluate(() => document.activeElement.id), "rValue");
  assert.equal(await page.locator("#rValue").inputValue(), "");
  assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), selection);
  assert.equal(await count(page, "confirm"), 0);
  await page.locator("#rValue").press("Escape");
  assert.equal(await count(page, "cancel"), 1);
});
