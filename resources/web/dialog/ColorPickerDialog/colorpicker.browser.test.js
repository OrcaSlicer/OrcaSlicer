// Optional real-DOM regressions. Use an existing Playwright installation:
// ORCA_PLAYWRIGHT_MODULE=<module path> ORCA_BROWSER_EXECUTABLE=<browser path>
// node --test resources/web/dialog/ColorPickerDialog/colorpicker.browser.test.js
const {test, before, after} = require("node:test");
const assert = require("node:assert/strict");
const {readFileSync} = require("node:fs");
const {resolve} = require("node:path");
const {pathToFileURL} = require("node:url");
let chromium;
try { ({chromium} = require(process.env.ORCA_PLAYWRIGHT_MODULE || "playwright")); }
catch (error) { if (error.code !== "MODULE_NOT_FOUND") throw error; }
const options = {skip: chromium ? false : "Use an existing Playwright installation via ORCA_PLAYWRIGHT_MODULE"};
const url = pathToFileURL(resolve(__dirname, "index.html")).href;
let browser;
before(async () => { if (chromium) browser = await chromium.launch({headless: true, ...(process.env.ORCA_BROWSER_EXECUTABLE ? {executablePath: process.env.ORCA_BROWSER_EXECUTABLE} : {})}); });
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
    assert.equal(await page.locator("label[for=tab2]").isVisible(), gradient);
    assert.equal(await page.locator("#alphaControls").isVisible(), alpha);
    assert.equal(await page.locator("#userColors button:not(.empty)").count(), gradient ? 2 : 1);
    await page.locator("#userColors button:not(.empty)").last().click();
    const selection = await page.evaluate(() => ColorPickerDialog.exportSelection());
    assert.deepEqual(selection, {type: "solid", colors: [alpha ? "#ABCDEF40" : "#ABCDEFFF"]});
    assert.equal(await send(init(ready, gradient, alpha)), true);
    assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), selection, "repeated ready/init retains edits");
    await page.locator("#saveColorBtn").click();
    const favorites = await page.evaluate(() => messages.at(-1).favorites);
    assert(favorites.some(favorite => favorite.colors[0] === "#ABCDEF40"), "original favorites retain their alpha");
    assert(favorites.some(favorite => favorite.type === "gradient"), "filtered gradients remain stored");
    await page.locator("#confirmBtn").click();
    assert.deepEqual(await page.evaluate(() => messages.at(-1).selection), selection);
  });
}

test("Visible incomplete editors block confirmation and favorites until recovery", options, async t => {
  const {page, ready, send} = await open(t); await send(init(ready, true, true));
  for (const [id, value, valid] of [["rValue", "999", "20"], ["rValue", "", "21"], ["aValue", "101", "50"], ["hexInput", "GG", "12345680"]]) {
    await page.locator("#" + id).fill(value);
    const confirms = await count(page, "confirm"), saves = await count(page, "update_favorites");
    await page.locator("#confirmBtn").click(); await page.locator("#saveColorBtn").click();
    assert.equal(await count(page, "confirm"), confirms); assert.equal(await count(page, "update_favorites"), saves);
    await page.locator("#" + id).fill(valid); await page.locator("#" + id).press("Enter");
    assert.equal(await count(page, "confirm"), confirms + 1);
  }
  await page.locator("#hValue").evaluate(element => { element.value = ""; }); // Hidden HSL input must not block RGB confirmation.
  const confirms = await count(page, "confirm"); await page.locator("#confirmBtn").click();
  assert.equal(await count(page, "confirm"), confirms + 1);
  await page.locator("#rValue").fill(""); await page.locator("#rValue").press("Escape");
  assert.equal(await page.evaluate(() => messages.at(-1).command), "cancel");
});

test("Endpoint edits and favorites retain ordered byte alpha", options, async t => {
  const {page, ready, send} = await open(t);
  await send({...init(ready, true, true), selection: {type: "gradient", colors: ["#FF000001", "#0000FF80"]}});
  await page.locator("label[for=gradientSwitch]").click();
  await page.locator('.color-item[title="Red"]').first().click();
  assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), {type: "gradient", colors: ["#FF000001", "#FF000080"]});
  await page.locator("#hexInput").fill("0000FF80"); await page.locator("label[for=gradientSwitch]").click();
  assert.equal(await page.locator("#hexInput").inputValue(), "FF000001");
  await page.locator("#saveColorBtn").click();
  assert.deepEqual(await page.evaluate(() => messages.at(-1).favorites[0]), {type: "gradient", colors: ["#FF000001", "#0000FF80"]});
});

test("Read-only favorites remain selectable and late documents cannot initialize", options, async t => {
  const {page, ready, send} = await open(t); await send({...init(ready), favorites_writable: false});
  assert.equal(await page.locator("#saveColorBtn").isDisabled(), true);
  await page.locator("#userColors button:not(.empty)").first().click();
  assert.equal(await page.locator("#hexInput").inputValue(), "ABCDEF");
  await page.reload();
  assert.equal(await send(init(ready)), false); assert.equal(await page.locator("#confirmBtn").isDisabled(), true);
});

test("Document strings translate palette names and editor errors while retaining RAL codes and numeric labels", options, async t => {
  const source = readFileSync(resolve(__dirname, "../../../../src/slic3r/GUI/ColorPickerDialog.cpp"), "utf8");
  const keys = [...source.matchAll(/_u8L\("([^"\n]+)"\)/g)].map(match => match[1]);
  const strings = Object.fromEntries(keys.map(key => [key, "Translated: " + key]));
  const {page, ready, send} = await open(t, strings); await send({...init(ready, true, true), preserve_multi_color: true});
  assert.equal(await page.title(), strings["Color Picker"]);
  for (const element of await page.locator("[data-i18n]").all()) assert.equal(await element.textContent(), strings[await element.getAttribute("data-i18n")]);
  for (const attribute of ["aria-label", "title"]) for (const element of await page.locator(`[data-i18n-${attribute}]`).all())
    assert.equal(await element.getAttribute(attribute), strings[await element.getAttribute(`data-i18n-${attribute}`)]);
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
  for (const [id, invalid, valid, key] of [["rValue", "999", "18", "Value is out of range."],
                                         ["rValue", "", "18", "Invalid input"],
                                         ["aValue", "101", "50", "Value is out of range."]]) {
    const confirms = await count(page, "confirm");
    await page.locator("#" + id).fill(invalid);
    await page.locator("#confirmBtn").click();
    assert.equal(await count(page, "confirm"), confirms);
    assert.equal(await page.locator("#" + id).evaluate(element => element.validationMessage), strings[key]);
    await page.locator("#" + id).fill(valid);
    assert.equal(await page.locator("#" + id).evaluate(element => element.validationMessage), "");
    await page.locator("#confirmBtn").click();
    assert.equal(await count(page, "confirm"), confirms + 1);
  }
  await page.locator("label[for=modeSwitch]").click();
  await page.locator("#hValue").fill("361");
  await page.locator("#saveColorBtn").click();
  assert.equal(await page.locator("#hValue").evaluate(element => element.validationMessage), strings["Value is out of range."]);
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
  assert.deepEqual(await page.locator("#slider-type-switch .switch-label").allTextContents(), ["RGB", "HSL"]);
  assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), {type: "solid", colors: ["#012345FF"]});
});

test("Live app theme overrides the opposite system scheme without changing colors", options, async t => {
  const {page, ready, send} = await open(t); await send({...init(ready, true, true), selection: {type: "gradient", colors: ["#FF000001", "#0000FF80"]}});
  const initial = await page.evaluate(() => ({selection: ColorPickerDialog.exportSelection(), spectrum: document.getElementById("spectrum").toDataURL()}));
  for (const [theme, system, background] of [["light", "dark", "rgb(255, 255, 255)"], ["dark", "light", "rgb(45, 45, 49)"]]) {
    await page.emulateMedia({colorScheme: system});
    await page.evaluate(value => {
      document.documentElement.dataset.orcaTheme = value;
      const colors = value === "light" ? {bg: "#ffffff", fg: "#262e30", muted: "#363636", border: "#dbdbdb", accent: "#009688"} : {bg: "#2d2d31", fg: "#efeff0", muted: "#b2b3b5", border: "#36363b", accent: "#00675b"};
      for (const [key, color] of Object.entries(colors)) document.documentElement.style.setProperty("--orca-" + key, color);
      document.documentElement.style.setProperty("--orca-accent-fg", "#ffffff");
    }, theme);
    assert.equal(await page.locator("body").evaluate(element => getComputedStyle(element).backgroundColor), background);
    assert.equal(await page.locator("html").evaluate(element => getComputedStyle(element).colorScheme), theme);
    assert.deepEqual(await page.evaluate(() => ({selection: ColorPickerDialog.exportSelection(), spectrum: document.getElementById("spectrum").toDataURL()})), initial);
    for (const id of ["modeSwitch", "gradientSwitch"]) for (const checked of [false, true]) {
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
      assert.equal(result.selected, result.accentForeground, `${theme} ${id} selected text uses the accent foreground`);
      assert.equal(result.unselected, result.mutedForeground, `${theme} ${id} unselected text stays muted`);
      assert.deepEqual(await page.evaluate(() => ColorPickerDialog.exportSelection()), initial.selection, "switching editors/endpoints keeps color and byte alpha");
    }
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
      return ["confirmBtn", "cancelBtn", ...(document.getElementById("alphaControls").classList.contains("hidden") ? [] : ["aValue"])].map(id => {
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
    await page.setViewportSize({width: 550, height: 300}); // Native work-area cap keeps long content scrollable.
    assert(await page.evaluate(() => {
      const favorites = document.getElementById("userColors").getBoundingClientRect();
      const preview = document.getElementById("colorPreview").getBoundingClientRect();
      return favorites.right <= preview.left;
    }), "scrollbars do not overlap favorites and preview");
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
