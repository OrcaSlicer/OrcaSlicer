"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const model = require("./run_script_model.js");

test("a file without a marker is one cell", () => {
  assert.deepEqual(model.splitCells("x = 1\ny = 2\n"), [{ index: 1, start: 1, end: 3, title: "" }]);
});

test("a marker on line 1 starts the first cell, and its text is the cell's title", () => {
  assert.deepEqual(model.splitCells("# %% Load\nx = 1\n# %%Slice  \ny = 2"), [
    { index: 1, start: 1, end: 2, title: "Load" },
    { index: 2, start: 3, end: 4, title: "Slice" },
  ]);
});

test("lines before the first marker are cell 1, and an indented marker is a comment", () => {
  assert.deepEqual(model.splitCells("import os\n\n# %% A\nif True:\n    # %% not a cell\n    x = 1"), [
    { index: 1, start: 1, end: 2, title: "" },
    { index: 2, start: 3, end: 6, title: "A" },
  ]);
});

test("the cell at a line is the one whose lines hold it", () => {
  const cells = model.splitCells("a\n# %% B\nb\n# %% C\nc");
  assert.equal(model.cellAt(cells, 1).index, 1);
  assert.equal(model.cellAt(cells, 3).index, 2);
  assert.equal(model.cellAt(cells, 5).index, 3);
});

test("Return indents after a colon or an opening bracket, outdents after a statement that ends a block", () => {
  const tab = "    ";
  assert.equal(model.pythonNextIndent("def f():  # c", tab), "    ");
  assert.equal(model.pythonNextIndent("x = 1", tab), "");
  assert.equal(model.pythonNextIndent("    return x", tab), "");
  assert.equal(model.pythonNextIndent("    pass", tab), "");
  assert.equal(model.pythonNextIndent("foo(", tab), "    ");
  assert.equal(model.pythonNextIndent('x = "a:"', tab), "");
  assert.equal(model.pythonNextIndent("# a:", tab), "");
  assert.equal(model.pythonNextIndent("return", tab), "");
  assert.equal(model.pythonNextIndent("        if x:", tab), "            ");
  assert.equal(model.pythonNextIndent("    returned = 1", tab), "    ");
});

test("suggestions come inside tools.call's string and after orca.host., never inside another string", () => {
  assert.deepEqual(model.suggestContext('tools.call("sl'), { kind: "tool", from: 12, typed: "sl" });
  assert.deepEqual(model.suggestContext("orca.host.tools.call('"), { kind: "tool", from: 22, typed: "" });
  assert.deepEqual(model.suggestContext("orca.host.ui.me"), { kind: "api", path: "orca.host.ui", from: 13, typed: "me" });
  assert.deepEqual(model.suggestContext("x = orca.host."), { kind: "api", path: "orca.host", from: 14, typed: "" });
  assert.equal(model.suggestContext('x = "tools.call("'), null);
  assert.equal(model.suggestContext("# orca.host.ui"), null);
});

test("a traceback's lines in the script are links, other files' and the console's are not", () => {
  const script = "C:\\scripts\\demo.py";
  const traceback = 'Traceback (most recent call last):\n' +
    '  File "C:\\scripts\\demo.py", line 7, in <module>\n' +
    '  File "C:\\lib\\helper.py", line 3, in f\n' +
    '  File "<console>", line 1, in <module>\n' +
    '  File "C:\\scripts\\demo.py", line 12\n' +
    "SyntaxError: invalid syntax\n";
  const links = model.tracebackLinks(traceback, script);
  assert.deepEqual(links.map((l) => l.line), [7, 12]);
  for (const link of links)
    assert.match(traceback.slice(link.start, link.end), /^File "C:\\scripts\\demo\.py", line \d+$/);
});

test("Run all's margin: every cell ran when it finished", () => {
  const cells = model.splitCells("a\n# %% b\nb\n# %% c\nc");
  assert.deepEqual(model.railAfterFile(cells, [0, 1, 2], { status: "finished", error_line: 0, executed: true, cells_reported: true }),
                   ["ran", "ran", "ran"]);
});

test("Run all's margin: a raise at a line marks the cells before it ran, its own failed, the rest not run", () => {
  const cells = model.splitCells("a\n# %% b\nb\n# %% c\nc");
  const raised = { status: "raised", error_line: 3, executed: true };
  assert.deepEqual(model.railAfterFile(cells, [0, 1], Object.assign({ cells_reported: true }, raised)), ["ran", "failed", "idle"]);
  assert.deepEqual(model.railAfterFile(cells, [], Object.assign({ cells_reported: false }, raised)), ["ran", "failed", "idle"]);
});

test("Run all's margin: a syntax error marks its cell failed and none ran", () => {
  const cells = model.splitCells("a\n# %% b\nb(\n# %% c\nc");
  assert.deepEqual(model.railAfterFile(cells, [], { status: "raised", error_line: 3, executed: false, cells_reported: false }),
                   ["idle", "failed", "idle"]);
  assert.deepEqual(model.railAfterFile(cells, [], { status: "raised", error_line: 0, executed: false, cells_reported: false }),
                   ["idle", "idle", "idle"]);
});

test("Run all's margin: a stop marks the last reported cell, and none without reports", () => {
  const cells = model.splitCells("a\n# %% b\nb\n# %% c\nc");
  assert.deepEqual(model.railAfterFile(cells, [0, 1], { status: "cancelled", error_line: 0, executed: true, cells_reported: true }),
                   ["ran", "failed", "idle"]);
  assert.deepEqual(model.railAfterFile(cells, [], { status: "cancelled", error_line: 0, executed: true, cells_reported: false }),
                   ["idle", "idle", "idle"]);
});

test("a cell is edited when its text differs from the text that ran", () => {
  assert.equal(model.edited("x = 1\n", "x = 1\n"), false);
  assert.equal(model.edited("x = 2\n", "x = 1\n"), true);
});

test("a script's text is compared with the line breaks the editor holds", () => {
  assert.equal(model.editorText("a = 1\r\nb = 2\r\n"), "a = 1\nb = 2\n");
  assert.equal(model.editorText("a = 1\rb = 2\n"), "a = 1\nb = 2\n");
  assert.equal(model.editorText("a = 1\nb = 2\n"), "a = 1\nb = 2\n");
});

test("a triple-quoted string open at a line's start is found from the lines before it", () => {
  assert.equal(model.openQuote('def f():\n    """Doc:'), '"""');
  assert.equal(model.openQuote("s = '''a\nb"), "'''");
  assert.equal(model.openQuote('s = """a"""\n# """'), null);
  assert.equal(model.openQuote('s = "a"\nt = 1'), null);
});

test("Return inside a multi-line string keeps the indent, and code after the string closes is read as code", () => {
  const tab = "    ";
  assert.equal(model.pythonNextIndent('    """Doc:', tab), "    ");
  assert.equal(model.pythonNextIndent("    return the value", tab, '"""'), "    ");
  assert.equal(model.pythonNextIndent('    return the value"""', tab, '"""'), "    ");
  assert.equal(model.pythonNextIndent('    """ + f(', tab, '"""'), "        ");
});

test("no suggestions inside a multi-line string, and suggestions again after it closes", () => {
  assert.equal(model.suggestContext('tools.call("', '"""'), null);
  assert.equal(model.suggestContext("orca.host.", "'''"), null);
  assert.equal(model.suggestContext('"""; tools.call("', '"""').kind, "tool");
});

test("Clear keeps only the blocks a run still writes to: its own and the cell it runs now", () => {
  assert.deepEqual(model.keptByClear(null), []);
  assert.deepEqual(model.keptByClear({ kind: "cell", block: "Cell 2" }), ["Cell 2"]);
  assert.deepEqual(model.keptByClear({ kind: "console", block: ">>> x" }), [">>> x"]);
  assert.deepEqual(model.keptByClear({ kind: "file", block: "Run all" }), ["Run all"]);
  assert.deepEqual(model.keptByClear({ kind: "file", block: "Run all", cellBlock: "Cell 3" }), ["Run all", "Cell 3"]);
});

test("the cell view is the saved one, and Dividers with the cell at the cursor highlighted when nothing usable is saved", () => {
  assert.deepEqual(model.cellView(undefined), { style: "dividers", highlight: true });
  assert.deepEqual(model.cellView({ style: "", highlight: true }), { style: "dividers", highlight: true });
  assert.deepEqual(model.cellView({ style: "zebra", highlight: true }), { style: "dividers", highlight: true });
  assert.deepEqual(model.cellView({ style: "bands", highlight: false }), { style: "bands", highlight: false });
  for (const style of ["margin", "dividers", "bands", "shaded"]) assert.equal(model.cellView({ style }).style, style);
});

test("Dividers draw a line above every cell but one starting on the first line", () => {
  const marks = (text) => model.cellMarks(model.splitCells(text), text.split("\n"), { style: "dividers", highlight: false }, 0);
  assert.deepEqual(marks("x = 1\n# %% B\ny\n# %% C\nz"), [
    { from: 1, to: 1, cls: "cell-divider" },
    { from: 3, to: 3, cls: "cell-divider" },
  ]);
  assert.deepEqual(marks("# %% A\nx\n# %% B\ny"), [{ from: 2, to: 2, cls: "cell-divider" }]);
  assert.deepEqual(marks("x = 1\ny = 2"), []);
});

test("Header bands mark each cell's # %% line, not the lines before the first marker", () => {
  const marks = (text) => model.cellMarks(model.splitCells(text), text.split("\n"), { style: "bands", highlight: false }, 0);
  assert.deepEqual(marks("import os\n# %% B\ny"), [{ from: 1, to: 1, cls: "cell-band" }]);
  assert.deepEqual(marks("# %% A\nx\n# %%\ny"), [
    { from: 0, to: 0, cls: "cell-band" },
    { from: 2, to: 2, cls: "cell-band" },
  ]);
});

test("Shaded cells shade every second cell, and Margin bars only marks no rows", () => {
  const text = "# %% A\na\n# %% B\nb\n# %% C\nc\n# %% D\nd";
  const marks = (style) => model.cellMarks(model.splitCells(text), text.split("\n"), { style, highlight: false }, 0);
  assert.deepEqual(marks("shaded"), [
    { from: 2, to: 3, cls: "cell-shade" },
    { from: 6, to: 7, cls: "cell-shade" },
  ]);
  assert.deepEqual(marks("margin"), []);
});

test("the highlight covers the cell at the cursor in every style, under the style's own lines", () => {
  const text = "# %% A\na\n# %% B\nb1\nb2\n# %% C\nc";
  const marks = (style, highlight, current) =>
    model.cellMarks(model.splitCells(text), text.split("\n"), { style, highlight }, current);
  assert.deepEqual(marks("margin", true, 2), [{ from: 2, to: 4, cls: "cell-current" }]);
  assert.deepEqual(marks("dividers", true, 2), [
    { from: 2, to: 4, cls: "cell-current" },
    { from: 2, to: 2, cls: "cell-divider" },
    { from: 5, to: 5, cls: "cell-divider" },
  ]);
  assert.deepEqual(marks("shaded", true, 2), [
    { from: 2, to: 4, cls: "cell-shade" },
    { from: 2, to: 4, cls: "cell-current" },
  ]);
  assert.deepEqual(marks("margin", false, 2), []);
  assert.deepEqual(marks("margin", true, 0), []);
});

test("a JSON result splits into the tokens its highlight colours, each as Python wrote it", () => {
  const tokens = model.jsonTokens('{"a": [1.0, true, null, "x, y: z"]}').map((t) => [t.type, t.text]);
  assert.deepEqual(tokens, [
    ["punct", "{"], ["key", '"a"'], ["punct", ":"], ["space", " "], ["punct", "["], ["number", "1.0"], ["punct", ","],
    ["space", " "], ["literal", "true"], ["punct", ","], ["space", " "], ["literal", "null"], ["punct", ","], ["space", " "],
    ["string", '"x, y: z"'], ["punct", "]"], ["punct", "}"],
  ]);
});

test("a JSON result keeps an array or object on one line while it fits the width", () => {
  assert.equal(model.formatJson('{"offset": [128, 128, 0], "mirrored": false}'), '{"offset": [128, 128, 0], "mirrored": false}');
});

test("a JSON result past the width takes a line per member, two spaces a level, keeping members that fit whole", () => {
  const text = '{"objects": [{"index": 0, "name": "3DBenchy", "offset": [128, 128, 0], "rotation": [0, 0, 0]}], "total": 1}';
  assert.equal(model.formatJson(text, 40), [
    "{",
    '  "objects": [',
    "    {",
    '      "index": 0,',
    '      "name": "3DBenchy",',
    '      "offset": [128, 128, 0],',
    '      "rotation": [0, 0, 0]',
    "    }",
    "  ],",
    '  "total": 1',
    "}",
  ].join("\n"));
});

test("a JSON result's numbers, strings and empty members stay as Python wrote them", () => {
  const text = '[1.0, 12345678901234567890123, -1e-07, "a \\"b\\" [c], {d}: e", "\u00e9", [], {}]';
  assert.equal(model.formatJson(text, 20), [
    "[",
    "  1.0,",
    "  12345678901234567890123,",
    "  -1e-07,",
    '  "a \\"b\\" [c], {d}: e",',
    '  "\u00e9",',
    "  [],",
    "  {}",
    "]",
  ].join("\n"));
});

test("text that isn't JSON is shown as it is", () => {
  assert.equal(model.formatJson("{'a': 1}"), "{'a': 1}");
  assert.equal(model.formatJson('{"a": [1, 2}'), '{"a": [1, 2}');
});

test("a folded cell shows the last line it printed that has text", () => {
  assert.equal(model.lastLine("slicing plate 1\nslicing plate 2\n\n"), "slicing plate 2");
  assert.equal(model.lastLine("a\r\nb\r\n"), "b");
  assert.equal(model.lastLine("\n  \n"), "");
});

test("Add a cell puts a marker after a blank line at the end, with the caret where its title goes", () => {
  // The script ends with a newline, so its last line is empty.
  assert.deepEqual(model.addCellEdit(["x = 1", ""]), { at: { row: 1, column: 0 }, text: "\n# %% \n", caret: { row: 2, column: 5 } });
  assert.deepEqual(model.addCellEdit(["x = 1", "", ""]), { at: { row: 2, column: 0 }, text: "# %% \n", caret: { row: 2, column: 5 } });
  assert.deepEqual(model.addCellEdit(["x = 1"]), { at: { row: 0, column: 5 }, text: "\n\n# %% \n", caret: { row: 2, column: 5 } });
  assert.deepEqual(model.addCellEdit(["x = 1", "    "]), { at: { row: 1, column: 4 }, text: "\n# %% \n", caret: { row: 2, column: 5 } });
  assert.deepEqual(model.addCellEdit([""]), { at: { row: 0, column: 0 }, text: "# %% \n", caret: { row: 0, column: 5 } });
});

test("a JSON result whose layout would grow past four times its length stays on one line", () => {
  // Nested past the width at every level, its layout is mostly indentation.
  let text = '"' + "x".repeat(80) + '"';
  for (let level = 0; level < 60; ++level) text = '{"a": ' + text + "}";
  assert.equal(model.formatJson(text), text);
});

test("the panel's split and hidden side come back as it kept them, and anything else as the default", () => {
  assert.deepEqual(model.paneLayout(undefined), { split: 0.6, hidden: "" });
  assert.deepEqual(model.paneLayout({ split: "0.35", hidden: "console" }), { split: 0.35, hidden: "console" });
  assert.deepEqual(model.paneLayout({ split: 0.7, hidden: "editor" }), { split: 0.7, hidden: "editor" });
  for (const split of ["", "x", 0, 1, -0.2, 1.5, NaN, null, undefined]) assert.equal(model.paneLayout({ split }).split, 0.6);
  assert.equal(model.paneLayout({ split: 0.5, hidden: "both" }).hidden, "");
});

test("the divider leaves the editor 220 px and the console 200 px, or their shares of a pane too narrow for both", () => {
  assert.equal(model.clampSplit(0.1, 1000), 0.22);
  assert.equal(model.clampSplit(0.9, 1000), 0.8);
  assert.equal(model.clampSplit(0.5, 1000), 0.5);
  assert.equal(model.clampSplit(0.1, 300), 0.5238);
  assert.equal(model.clampSplit(0.9, 300), 0.5238);
});

test("dragging the divider 48 px past a side's smallest width hides that side", () => {
  assert.deepEqual(model.dragSplit(500, 1000), { split: 0.5, hide: "" });
  assert.deepEqual(model.dragSplit(172, 1000), { split: 0.22, hide: "" });
  assert.deepEqual(model.dragSplit(171, 1000), { split: 0.22, hide: "editor" });
  assert.deepEqual(model.dragSplit(848, 1000), { split: 0.8, hide: "" });
  assert.deepEqual(model.dragSplit(849, 1000), { split: 0.8, hide: "console" });
  // A pane too narrow for both: each side hides 48 px past the point the divider stops at.
  assert.equal(model.dragSplit(157, 300).hide, "");
  assert.equal(model.dragSplit(100, 300).hide, "editor");
  assert.equal(model.dragSplit(210, 300).hide, "console");
});

test("the divider's arrows move it a twentieth of the pane, Home and End to a side's smallest width", () => {
  assert.equal(model.splitByKey(0.6, "ArrowLeft", 1000), 0.55);
  assert.equal(model.splitByKey(0.6, "ArrowRight", 1000), 0.65);
  assert.equal(model.splitByKey(0.78, "ArrowRight", 1000), 0.8);
  assert.equal(model.splitByKey(0.6, "Home", 1000), 0.22);
  assert.equal(model.splitByKey(0.6, "End", 1000), 0.8);
  assert.equal(model.splitByKey(0.6, "a", 1000), null);
});

test("the console's strip shows a run going, then new output, and a failed run until the next one", () => {
  assert.equal(model.consoleNews("", "output"), "fresh");
  assert.equal(model.consoleNews("", "start"), "running");
  assert.equal(model.consoleNews("running", "output"), "running");
  assert.equal(model.consoleNews("running", "finished"), "fresh");
  assert.equal(model.consoleNews("running", "failed"), "failed");
  assert.equal(model.consoleNews("failed", "output"), "failed");
  assert.equal(model.consoleNews("failed", "start"), "running");
});

test("the console's side hides only while it shows the console, not the start page or the note that Python is off", () => {
  assert.equal(model.hiddenSide("console", true), "console");
  assert.equal(model.hiddenSide("console", false), "");
  assert.equal(model.hiddenSide("editor", false), "editor");
  assert.equal(model.hiddenSide("editor", true), "editor");
  assert.equal(model.hiddenSide("", true), "");
});
