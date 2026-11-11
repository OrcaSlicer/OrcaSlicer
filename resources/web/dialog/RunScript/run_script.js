// The Run Script panel's page. The host (RunScriptPanel) keeps the script, its file and the session, and runs what this
// page asks for; this page edits the script, draws the margin and the console, and reports every edit as it happens, so
// the host's copy is never behind.

const S = {
  ready: false,
  none: true,          // no script open
  fileName: "",
  savedText: "",       // the text as opened or saved; null while the host says it's edited and the page never saw it saved
  session: "none",     // none | idle | working | threads | ending
  pending: null,       // a run asked for, until the host starts or refuses it
  run: null,           // the run going: {kind, cells, text, cell, reported, runningCell, block, cellBlock}
  toggleChord: "",
  recent: [],
  rail: new Map(),     // cell index -> {state: "ran" | "failed", text: the cell's text as it ran}
  cellsRun: new Set(), // the cells that ran in this session
  counts: null,        // {cells_text, variables_text} from the host
  message: null,       // a refusal or an error, shown in the status line until the next run
  cellView: RunScriptModel.cellView(), // how the editor shows cells: {style, highlight}, from the Cells menu
  layout: RunScriptModel.paneLayout(), // {split: the editor's share of the width, hidden: "" | "editor" | "console"}
  news: "",            // what the hidden console's strip shows: "" | running | fresh | failed
};
// What the editor suggests: the host's tool catalog and the orca.host API's entries.
const source = { tools: [], api: [] };
const CONSOLE_LIMIT = 2 * 1024 * 1024;

let editor = null;
let railFrame = 0;
let railDecorations = [];
let railMarkers = [];
let consoleSize = 0;
let outputBlock = null; // the newest block, where output goes

function $(id) { return document.getElementById(id); }

function send(command, extra) { SendWXMessage(JSON.stringify(Object.assign({ command }, extra || {}))); }

function el(tag, className, text) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (text !== undefined && text !== null) node.textContent = text;
  return node;
}

function seconds(ms) { return T("seconds", "%s s", (ms / 1000).toFixed(1)); }

// ---- the script ---------------------------------------------------------------------------------------------------

function scriptText() { return editor.getValue(); }

function isEdited() { return !S.none && (S.savedText === null || scriptText() !== S.savedText); }

function cellText(cell, lines) { return lines.slice(cell.start - 1, cell.end).join("\n"); }

function applyScript(script) {
  S.none = !!script.none;
  S.fileName = S.none ? "" : script.file_name || "";
  closeEditorSuggestions(editor);
  setEditorText(editor, S.none ? "" : script.text || "");
  S.savedText = S.none ? "" : script.edited ? null : RunScriptModel.editorText(script.text || "");
  resetSessionMarks();
  render();
}

function resetSessionMarks() {
  S.rail.clear();
  S.cellsRun.clear();
  S.counts = null;
  scheduleRail();
}

function onEdit(text) {
  send("edit", { text, edited: isEdited() });
  scheduleRail();
  renderChip();
}

function save(pick) {
  if (!S.none) send("save", { pick });
}

// ---- the margin -----------------------------------------------------------------------------------------------------

function scheduleRail() {
  if (!railFrame) railFrame = requestAnimationFrame(drawRail);
}

function cellState(cell, lines) {
  if (S.run && S.run.runningCell === cell.index) return "running";
  const mark = S.rail.get(cell.index);
  if (!mark) return "idle";
  if (mark.state === "ran" && RunScriptModel.edited(cellText(cell, lines), mark.text)) return "edited";
  return mark.state;
}

// The cells of the rows on screen, at most once a frame: a bar per cell in the margin, in its state (wider for the cell at
// the cursor while the highlight is on), and the rows the Cells menu's view marks behind the text.
function drawRail() {
  railFrame = 0;
  const session = editor.session;
  for (const [row, cls] of railDecorations) session.removeGutterDecoration(row, cls);
  for (const id of railMarkers) session.removeMarker(id);
  railDecorations = [];
  railMarkers = [];
  editor.container.classList.toggle("cells-bands", S.cellView.style === "bands");
  drawEditorStrip();
  if (S.none) return;
  const text = scriptText();
  const lines = text.split("\n");
  const first = editor.renderer.getFirstVisibleRow();
  const last = Math.min(editor.renderer.getLastVisibleRow() + 1, lines.length - 1);
  const add = (row, cls) => {
    session.addGutterDecoration(row, cls);
    railDecorations.push([row, cls]);
  };
  const cells = RunScriptModel.splitCells(text);
  const current = S.cellView.highlight ? RunScriptModel.cellAt(cells, editor.getCursorPosition().row + 1) : null;
  const shown = [];
  for (const cell of cells) {
    if (cell.end - 1 < first || cell.start - 1 > last) continue;
    shown.push(cell);
    const cls = "rail-" + cellState(cell, lines);
    for (let row = Math.max(cell.start - 1, first); row <= Math.min(cell.end - 1, last); ++row) {
      add(row, cls);
      if (row === cell.start - 1) add(row, "rail-first");
      if (row === cell.end - 1) add(row, "rail-last");
      if (cell === current) add(row, "rail-current");
    }
  }
  // Each marker ends past the end of its last line, so it covers every screen row of a line that wraps.
  const Range = ace.require("ace/range").Range;
  for (const mark of RunScriptModel.cellMarks(shown, lines, S.cellView, current ? current.index : 0))
    railMarkers.push(session.addMarker(new Range(mark.from, 0, mark.to, Infinity), mark.cls, "fullLine", false));
}

// ---- Add a cell -----------------------------------------------------------------------------------------------------

// The + in the margin a row below the last line, which adds a cell at the end; hovering it shows where. Ace has no cells,
// so the page places it after each render. Hidden while the editor can't be edited or the row is off screen.
let addButton = null;
let addHint = null;

function setUpAddCell() {
  addButton = el("button", "add-cell", "+");
  addButton.type = "button";
  addButton.title = T("add_cell_tip", "Add a cell at the end of the script");
  addButton.setAttribute("aria-label", T("add_cell", "Add a cell"));
  addHint = el("div", "add-hint", T("add_cell", "Add a cell"));
  addHint.setAttribute("aria-hidden", "true");
  editor.container.append(addButton, addHint);
  // Ace would move the caret on the press before the click; the page's own outside-press handler doesn't see it then,
  // so the press closes the menus itself.
  addButton.addEventListener("mousedown", (e) => {
    e.stopPropagation();
    closeMenus();
  });
  addButton.addEventListener("click", addCell);
  editor.renderer.on("afterRender", placeAddButton);
  // Room below the last line for the + when the script is longer than the editor.
  editor.renderer.setScrollMargin(0, 24, 0, 0);
}

function placeAddButton() {
  if (!addButton) return;
  const r = editor.renderer;
  const box = editor.container.getBoundingClientRect();
  const top = r.textToScreenCoordinates(editor.session.getLength() - 1, Infinity).pageY - box.top + r.lineHeight;
  const shown = !S.none && !editor.getReadOnly() && top >= 0 && top + r.lineHeight <= box.height;
  addButton.hidden = addHint.hidden = !shown;
  if (!shown) return;
  addButton.style.top = top + Math.round((r.lineHeight - addButton.offsetHeight) / 2) + "px";
  addHint.style.top = top + "px";
  addHint.style.height = r.lineHeight + "px";
  addHint.style.left = r.textToScreenCoordinates(0, 0).pageX - box.left + "px";
}

function addCell() {
  if (S.none || editor.getReadOnly()) return;
  const edit = RunScriptModel.addCellEdit(editor.session.getDocument().getAllLines());
  editor.session.insert(edit.at, edit.text);
  editor.clearSelection();
  editor.moveCursorTo(edit.caret.row, edit.caret.column);
  editor.scrollToLine(edit.caret.row + 1, false, true);
  editor.focus();
}

// ---- the divider and the hidden sides ------------------------------------------------------------------------------

// The divider dragged: the share it had when the drag started, which a side the drag hides comes back at, and the side
// the drag would hide if it ended now.
let splitDrag = null;

const NEWS_TIPS = {
  "": ["show_console", "Show the console"],
  running: ["show_console_running", "Show the console: a script is running"],
  fresh: ["show_console_new", "Show the console: it has new output"],
  failed: ["show_console_failed", "Show the console: the last run failed"],
};

// Whether the side right of the divider shows the console, rather than the start page.
function consoleShown() { return !S.none; }

function renderLayout() {
  const panes = $("panes");
  const hidden = RunScriptModel.hiddenSide(S.layout.hidden, consoleShown());
  panes.style.setProperty("--split", S.layout.split * 100 + "%");
  panes.dataset.hidden = hidden;
  $("hide-console").hidden = !consoleShown();
  $("split").setAttribute("aria-valuenow", String(Math.round(S.layout.split * 100)));
  const tip = T(...NEWS_TIPS[S.news]);
  $("console-strip").title = tip;
  $("console-strip").setAttribute("aria-label", tip);
  $("console-news").className = "news " + S.news;
  if (hidden === "editor") scheduleRail();
  else if (!S.none) editor.resize();
}

// The hidden editor's strip: a bar per cell, in the margin's states.
function drawEditorStrip() {
  const bars = $("editor-strip-cells");
  bars.textContent = "";
  if (S.layout.hidden !== "editor" || S.none) return;
  const text = scriptText();
  const lines = text.split("\n");
  for (const cell of RunScriptModel.splitCells(text)) bars.append(el("i", cellState(cell, lines)));
}

// Sets the layout, and has the host keep it unless `save` is false (a drag keeps it when it ends). The console's strip
// starts blank, or running while a run goes.
function setLayout(layout, save) {
  if (layout.hidden !== S.layout.hidden) S.news = layout.hidden === "console" && S.run ? "running" : "";
  S.layout = layout;
  renderLayout();
  if (save !== false) send("pane_layout", layout);
}

// Hides a side, the divider coming back at `split`, and leaves the focus on the strip that brings it back.
function hideSide(side, split) {
  setLayout({ split: split === undefined ? S.layout.split : split, hidden: side });
  $(side + "-strip").focus();
}

// Brings the hidden side back, with the focus in it: the editor, or the console's prompt line while it takes a line.
function showBothSides() {
  const was = S.layout.hidden;
  setLayout({ split: S.layout.split, hidden: "" });
  if (was === "editor" && !S.none) editor.focus();
  else if (was === "console" && !$("console").hidden && !$("prompt-input").disabled) $("prompt-input").focus();
  else $("split").focus();
}

// What the hidden console's strip shows after a run starts or ends, or output comes.
function noteConsole(event) {
  if (RunScriptModel.hiddenSide(S.layout.hidden, consoleShown()) !== "console") return;
  const news = RunScriptModel.consoleNews(S.news, event);
  if (news === S.news) return;
  S.news = news;
  renderLayout();
}

function setUpSplit() {
  const split = $("split");
  split.addEventListener("pointerdown", (e) => {
    if (e.button !== 0 || e.target.closest("button")) return;
    e.preventDefault();
    split.setPointerCapture(e.pointerId);
    splitDrag = { from: S.layout.split, hide: "" };
    $("panes").classList.add("dragging");
  });
  split.addEventListener("pointermove", (e) => {
    if (!splitDrag) return;
    const box = $("panes").getBoundingClientRect();
    const to = RunScriptModel.dragSplit(e.clientX - box.left, box.width);
    splitDrag.hide = RunScriptModel.hiddenSide(to.hide, consoleShown());
    $("panes").dataset.pending = splitDrag.hide;
    setLayout({ split: to.split, hidden: "" }, false);
  });
  const endDrag = () => {
    if (!splitDrag) return;
    const { from, hide } = splitDrag;
    splitDrag = null;
    $("panes").classList.remove("dragging");
    $("panes").dataset.pending = "";
    if (hide) hideSide(hide, from);
    else send("pane_layout", S.layout);
  };
  split.addEventListener("pointerup", endDrag);
  split.addEventListener("lostpointercapture", endDrag);
  split.addEventListener("dblclick", (e) => {
    if (!e.target.closest("button")) setLayout({ split: RunScriptModel.paneLayout().split, hidden: "" });
  });
  // A window splitter's keys; Enter hides the console while the side shows it. The grip's buttons take their own.
  split.addEventListener("keydown", (e) => {
    if (e.target !== split) return;
    if (e.key === "Enter" && !e.shiftKey && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
      if (consoleShown()) hideSide("console");
      return;
    }
    const to = RunScriptModel.splitByKey(S.layout.split, e.key, $("panes").getBoundingClientRect().width);
    if (to === null) return;
    e.preventDefault();
    setLayout({ split: to, hidden: "" });
  });
  $("hide-editor").addEventListener("click", () => hideSide("editor"));
  $("hide-console").addEventListener("click", () => hideSide("console"));
  $("editor-strip").addEventListener("click", showBothSides);
  $("console-strip").addEventListener("click", showBothSides);
}

// ---- running ------------------------------------------------------------------------------------------------------

function busy() { return !!S.pending || !!S.run || S.session === "working"; }

function canRun() { return S.ready && !S.none && !busy() && S.session !== "ending"; }

function runAll() {
  closeEditorSuggestions(editor);
  if (!canRun()) return;
  const text = scriptText();
  const cells = RunScriptModel.splitCells(text);
  S.pending = { kind: "file", cells, text };
  S.message = null;
  send("run", { kind: "file", text, cell_starts: cells.map((cell) => cell.start) });
  render();
}

function runCell() {
  closeEditorSuggestions(editor);
  if (!canRun()) return;
  const text = scriptText();
  const cells = RunScriptModel.splitCells(text);
  const cell = RunScriptModel.cellAt(cells, editor.getCursorPosition().row + 1);
  const code = cellText(cell, text.split("\n"));
  S.pending = { kind: "cell", cells, text, cell, code };
  S.message = null;
  send("run", { kind: "cell", text, first_line: cell.start, last_line: cell.end, code });
  render();
}

function runConsole() {
  const input = $("prompt-input");
  const code = input.value;
  if (!code.trim() || !S.ready || busy() || S.session === "ending") return;
  input.value = "";
  S.pending = { kind: "console", code };
  S.message = null;
  send("run", { kind: "console", text: S.none ? "" : scriptText(), code });
  render();
}

function onRunStarted(msg) {
  const asked = S.pending || { kind: msg.kind };
  S.pending = null;
  S.run = Object.assign({ reported: [], runningCell: null }, asked, { kind: msg.kind, started: Date.now() });
  if (msg.kind === "file" && !S.run.cells) {
    // A run the page didn't ask for in this load (it was rebuilt meanwhile): the cells are the editor's.
    S.run.text = scriptText();
    S.run.cells = RunScriptModel.splitCells(S.run.text);
  }
  if (msg.kind === "file") {
    // Run all runs in a session of its own.
    if (S.session !== "none") addDivider(T("session_restarted", "Session restarted for Run all"));
    resetSessionMarks();
    S.run.block = newRunAllBlock();
  } else if (msg.kind === "cell") {
    S.run.runningCell = asked.cell.index;
    S.run.block = newBlock(T("cell_n", "Cell %s", asked.cell.index), asked.cell.title);
  } else
    S.run.block = newBlock(null, null, ">>> " + asked.code);
  editor.setReadOnly(true);
  noteConsole("start");
  scheduleRail();
  render();
}

function onCell(msg) {
  const run = S.run;
  if (!run || run.kind !== "file" || !run.cells) return;
  const cell = run.cells[msg.index];
  if (!cell) return;
  const lines = run.text.split("\n");
  // The cell before it has run.
  if (run.reported.length) {
    const before = run.cells[run.reported[run.reported.length - 1]];
    S.rail.set(before.index, { state: "ran", text: cellText(before, lines) });
    if (run.cellBlock) finishBlock(run.cellBlock, Date.now() - run.cellBlock.started, false);
  }
  run.reported.push(msg.index);
  run.runningCell = cell.index;
  run.cellBlock = newCellRow(run.block, cell);
  scheduleRail();
  renderStatus();
}

function onRunFinished(msg) {
  const run = S.run || Object.assign({ reported: [] }, S.pending || {});
  S.pending = null;
  S.run = null;
  const failed = msg.status !== "finished";
  if (run.cellBlock) finishBlock(run.cellBlock, Date.now() - run.cellBlock.started, failed);
  const block = run.block || outputBlock || newBlock(T("run_all", "Run all"));
  finishBlock(block, msg.duration_ms, failed);
  // How it ended goes under what ran last: Run all's last cell, or the run's own block.
  const last = run.cellBlock || block;
  if (msg.status === "refused") {
    S.message = msg.message || "";
    appendLine(last, msg.message || "", "err");
  } else if (msg.status === "raised")
    appendTraceback(last, msg.traceback || msg.message || "", msg.script_file);
  else if (msg.status_text)
    appendLine(last, msg.status_text, failed ? "err" : "muted");
  if (!last.printed && !failed && run.kind !== "file") appendLine(last, T("no_output", "(no output)"), "muted");
  // The cell Run all stopped in opens, so its error shows.
  if (failed && run.cellBlock) {
    setRowOpen(run.cellBlock, true);
    renderExpandAll(run.block);
  }
  revealLog(true);

  const lines = (run.text || "").split("\n");
  if (run.kind === "cell" && msg.status !== "refused" && msg.status !== "unavailable") {
    S.rail.set(run.cell.index, { state: failed ? "failed" : "ran", text: run.code });
    S.cellsRun.add(run.cell.index);
  } else if (run.kind === "file" && run.cells && msg.status !== "refused") {
    RunScriptModel.railAfterFile(run.cells, run.reported, msg).forEach((state, i) => {
      if (state === "idle") return;
      S.rail.set(i + 1, { state, text: cellText(run.cells[i], lines) });
      S.cellsRun.add(i + 1);
    });
  }
  if (msg.status !== "refused") send("counts", { cells: S.cellsRun.size, variables: msg.variables || 0 });
  editor.setReadOnly(S.none);
  scheduleRail();
  render();
  noteConsole(failed ? "failed" : "finished");
  // The prompt was off while its line ran; the next line goes in it.
  if (run.kind === "console" && !$("prompt-input").disabled) $("prompt-input").focus();
}

function onRunRefused(msg) {
  S.pending = null;
  S.message = msg.message || "";
  render();
}

// ---- the console ------------------------------------------------------------------------------------------------

function newBlock(head, title, code) {
  const block = el("div", "block");
  const top = el("div", "run-head busy");
  if (head) top.append(el("b", "", head));
  if (title) top.append(el("span", "t", title));
  if (code) top.append(el("span", "code", code));
  const duration = el("span", "d");
  top.append(duration);
  block.append(top);
  block.head = top;
  block.duration = duration;
  block.size = 0;
  block.started = Date.now();
  $("log").append(block);
  outputBlock = block;
  revealLog(true);
  return block;
}

// Run all's block: a row for each cell the run reaches, folded, and Expand all in its heading once there is one.
function newRunAllBlock() {
  const block = newBlock(T("run_all", "Run all"));
  const all = el("button", "expand-all");
  all.type = "button";
  all.hidden = true;
  all.addEventListener("click", () => {
    const open = !allRowsOpen(block);
    for (const row of block.cells.children) setRowOpen(row, open);
    renderExpandAll(block);
  });
  block.head.insertBefore(all, block.duration);
  block.expandAll = all;
  block.cells = el("div", "cells");
  block.append(block.cells);
  return block;
}

// The row of the cell Run all runs now, in `group`, the run's block. Folded, its heading shows what the cell showed last
// (a line it printed, its result or its error), and a click opens it. Output goes in it, as in a block.
function newCellRow(group, cell) {
  const row = el("div", "cell-row running");
  const head = el("button", "cell-head busy");
  head.type = "button";
  head.append(el("span", "chev"), el("b", "", T("cell_n", "Cell %s", cell.index)));
  if (cell.title) head.append(el("span", "t", cell.title));
  const peek = el("span", "peek");
  const duration = el("span", "d");
  head.append(peek, duration);
  const body = el("div", "cell-body");
  row.append(head, body);
  Object.assign(row, { head, peek, duration, body, group, size: 0, started: Date.now(), tail: "" });
  setRowOpen(row, false);
  head.addEventListener("click", () => {
    setRowOpen(row, body.hidden);
    renderExpandAll(group);
  });
  group.cells.append(row);
  renderExpandAll(group);
  outputBlock = row;
  revealLog(true);
  return row;
}

function setRowOpen(row, open) {
  row.body.hidden = !open;
  row.classList.toggle("open", open);
  row.head.setAttribute("aria-expanded", String(open));
}

function allRowsOpen(group) { return [...group.cells.children].every((row) => !row.body.hidden); }

function renderExpandAll(group) {
  group.expandAll.hidden = !group.cells.children.length;
  group.expandAll.textContent = allRowsOpen(group) ? T("collapse_all", "Collapse all") : T("expand_all", "Expand all");
}

// What a folded row shows: `text` on one line, in the error colour for an error. Empty text leaves what it showed.
function setPeek(row, text, err) {
  if (!text) return;
  row.peek.textContent = text.slice(0, 500);
  row.peek.classList.toggle("err", !!err);
}

// Where a block's output goes: a row's body, or the block itself.
function contentOf(block) { return block.body || block; }

function finishBlock(block, ms, failed) {
  block.head.classList.remove("busy");
  block.head.classList.toggle("failed", !!failed);
  if (ms !== undefined && ms !== null) block.duration.textContent = seconds(ms);
  if (block.peek) {
    block.classList.remove("running");
    block.classList.toggle("failed", !!failed);
    if (!block.printed) setPeek(block, T("no_output", "(no output)"));
  }
}

function addDivider(text) { $("log").append(el("div", "divider", text)); }

function logAtBottom() {
  const log = $("log");
  return log.scrollHeight - log.scrollTop - log.clientHeight < 24;
}

function revealLog(force) {
  const log = $("log");
  if (force || logAtBottom()) log.scrollTop = log.scrollHeight;
}

// Empties the console and leaves the session alone. A run going keeps its heading, emptied, and its output goes on under it;
// in Run all, that is the run's block with the row of the cell it runs now.
function clearConsole() {
  const kept = RunScriptModel.keptByClear(S.run);
  const log = $("log");
  log.textContent = "";
  consoleSize = 0;
  for (const block of kept) {
    if (block.cells) {
      block.cells.replaceChildren();
      block.replaceChildren(block.head, block.cells);
    } else if (block.body) {
      block.body.replaceChildren();
      block.tail = "";
      block.peek.textContent = "";
    } else
      block.replaceChildren(block.head);
    block.size = 0;
    (block.group ? block.group.cells : log).append(block);
  }
  if (kept.length && kept[0].cells) renderExpandAll(kept[0]);
  outputBlock = kept.length ? kept[kept.length - 1] : null;
}

// `size` is what the block holds now, for the console's limit, a row's counted in its run's block too; `printed`,
// whether it ever showed output, Clear or not.
function count(block, n) {
  block.size += n;
  block.printed = true;
  if (block.group) block.group.size += n;
  consoleSize += n;
  // The console keeps its last 2 MiB, dropping its oldest blocks.
  const log = $("log");
  while (consoleSize > CONSOLE_LIMIT && log.children.length > 1) {
    const oldest = log.firstElementChild;
    consoleSize -= oldest.size || 0;
    oldest.remove();
  }
}

// The output a row keeps to find its last line, which a long line can't push out.
const PEEK_TAIL = 4096;

function appendOutput(text) {
  const atBottom = logAtBottom();
  const block = outputBlock || newBlock(null);
  const box = contentOf(block);
  let out = box.lastElementChild;
  if (!out || !out.classList.contains("out") || out.classList.length > 1) {
    out = el("div", "out");
    box.append(out);
  }
  out.append(document.createTextNode(text));
  count(block, text.length);
  if (block.peek) {
    block.tail = (block.tail + text).slice(-PEEK_TAIL);
    setPeek(block, RunScriptModel.lastLine(block.tail));
  }
  if (atBottom) revealLog(true);
  noteConsole("output");
}

// A result JSON holds, as the host sends it: laid out to read, in the editor's JSON colours. A folded row shows it as it
// came, on one line.
function appendResult(text) {
  const atBottom = logAtBottom();
  const block = outputBlock || newBlock(null);
  const out = el("div", "out json");
  const shown = RunScriptModel.formatJson(text);
  const tokens = RunScriptModel.jsonTokens(shown);
  if (!tokens) out.textContent = shown;
  else
    for (const token of tokens)
      out.append(token.type === "space" ? document.createTextNode(token.text) : el("span", "j-" + token.type, token.text));
  contentOf(block).append(out);
  // The layout is what the console holds.
  count(block, shown.length);
  if (block.peek) {
    block.tail = "";
    setPeek(block, text);
  }
  if (atBottom) revealLog(true);
  noteConsole("output");
}

function appendLine(block, text, cls) {
  contentOf(block).append(el("div", "out " + cls, text));
  count(block, text.length);
  if (block.peek) setPeek(block, RunScriptModel.lastLine(text), cls === "err");
  revealLog(false);
}

// A traceback whose lines in the script are links that move the cursor there.
function appendTraceback(block, traceback, scriptFile) {
  const out = el("div", "out err");
  let at = 0;
  for (const link of RunScriptModel.tracebackLinks(traceback, scriptFile || "")) {
    out.append(document.createTextNode(traceback.slice(at, link.start)));
    const a = el("a", "link", traceback.slice(link.start, link.end));
    a.href = "#";
    a.addEventListener("click", (e) => {
      e.preventDefault();
      goToLine(link.line);
    });
    out.append(a);
    at = link.end;
  }
  out.append(document.createTextNode(traceback.slice(at)));
  contentOf(block).append(out);
  count(block, traceback.length);
  if (block.peek) setPeek(block, RunScriptModel.lastLine(traceback), true);
  revealLog(false);
}

function goToLine(line) {
  editor.gotoLine(line, 0, true);
  editor.scrollToLine(line - 1, true, true);
  editor.focus();
}

// ---- rendering -----------------------------------------------------------------------------------------------------

function renderChip() {
  $("file-chip").hidden = S.none;
  $("file-name").textContent = S.fileName || T("untitled_script", "Untitled script");
  $("file-edited").hidden = !isEdited();
  $("editor-strip-name").textContent = S.none ? "" : $("file-name").textContent;
}

function sessionItem(text, cls) { return el("span", "item" + (cls ? " " + cls : ""), text); }

// The session line: its items apart, since one sentence can't hold two counts.
function renderSession() {
  const box = $("session");
  box.textContent = "";
  box.className = "session";
  box.append(el("span", "dot"));
  if (S.session === "ending") return box.append(sessionItem(T("session_ending", "Ending the session…")));
  if (busy()) {
    box.classList.add("busy");
    return box.append(sessionItem(T("running", "Running"), "lead"));
  }
  if (S.session === "none") return box.append(sessionItem(T("no_session", "No session yet")));
  box.classList.add("live");
  box.append(sessionItem(T("session", "Session"), "lead"));
  if (S.counts) {
    box.append(sessionItem(S.counts.cells_text));
    box.append(sessionItem(S.counts.variables_text));
  }
  if (S.session === "threads") box.append(sessionItem(T("threads_running", "Threads are running")));
}

function runningText() {
  const run = S.run;
  if (!run) return "";
  if (run.runningCell && run.cells) return T("running_cell", "Running cell %s of %s", run.runningCell, run.cells.length);
  if (run.kind === "file") return T("running_file", "Running the file");
  return T("running", "Running");
}

function renderStatus() {
  const bar = $("status");
  bar.classList.remove("is-error");
  if (S.run) {
    $("status-main").textContent = runningText();
    $("status-detail").textContent = T("hands_off", "Don't use OrcaSlicer until the script finishes.");
  } else if (S.message) {
    bar.classList.add("is-error");
    $("status-main").textContent = S.message;
    $("status-detail").textContent = "";
  } else if (!S.ready) {
    $("status-main").textContent = T("starting", "OrcaSlicer is still starting…");
    $("status-detail").textContent = "";
  } else {
    bar.hidden = true;
    return;
  }
  bar.hidden = false;
}

function render() {
  renderChip();
  const working = busy();
  const ending = S.session === "ending";
  $("new").disabled = working;
  $("open").disabled = working;
  $("close-script").disabled = working || S.none;
  $("save").disabled = S.none;
  $("run-all").disabled = !canRun();
  $("run-cell").disabled = !canRun();
  $("stop").disabled = !(working || S.session === "threads");
  $("restart").disabled = S.session === "none" || working || ending;
  const promptOff = !S.ready || working || ending;
  $("prompt-input").disabled = promptOff;
  $("prompt-input").closest(".prompt-line").classList.toggle("off", promptOff);

  $("editor").hidden = S.none;
  $("editor-empty").hidden = !S.none;
  editor.setReadOnly(S.none || !!S.run);
  $("editor").classList.toggle("is-readonly", !!S.run);
  $("start").hidden = !S.none;
  $("console").hidden = S.none;
  renderLayout();
  placeAddButton();
  renderSession();
  renderStatus();
}

function renderRecent() {
  const menu = $("open-recent");
  const list = $("start-recent");
  menu.textContent = "";
  list.textContent = "";
  for (const item of S.recent) {
    const entry = el("button", "menu-item");
    entry.type = "button";
    entry.setAttribute("role", "menuitem");
    entry.append(el("span", "f", item.name), el("span", "d", item.folder));
    entry.addEventListener("click", () => {
      closeMenus();
      send("open_recent", { path: item.path });
    });
    menu.append(entry);
    const row = el("li");
    const button = el("button");
    button.type = "button";
    button.append(el("span", "f", item.name), el("span", "d", item.folder));
    button.addEventListener("click", () => send("open_recent", { path: item.path }));
    row.append(button);
    list.append(row);
  }
  menu.hidden = !S.recent.length;
  $("start-no-recent").hidden = !!S.recent.length;
}

// ---- the toolbar's menus: Open and Cells ----------------------------------------------------------------------------

// Each menu, by the id of the button that opens it.
const MENUS = { open: "open-menu", cells: "cells-menu" };

// Opens the menu of `button`, closing the other, and focuses `item`. A menu that would run past the page's right edge
// opens leftwards from its button instead.
function openMenu(button, item) {
  closeMenus();
  const menu = $(MENUS[button]);
  menu.classList.remove("right");
  menu.hidden = false;
  $(button).setAttribute("aria-expanded", "true");
  if (menu.getBoundingClientRect().right > document.documentElement.clientWidth) menu.classList.add("right");
  if (item) item.focus();
}

function closeMenus() {
  for (const [button, menu] of Object.entries(MENUS)) {
    $(menu).hidden = true;
    $(button).setAttribute("aria-expanded", "false");
  }
}

function menuOpen() { return Object.values(MENUS).some((menu) => !$(menu).hidden); }

function renderCellsMenu() {
  for (const item of document.querySelectorAll("#cells-menu [data-style]"))
    item.setAttribute("aria-checked", String(item.dataset.style === S.cellView.style));
  $("cells-highlight").setAttribute("aria-checked", String(S.cellView.highlight));
}

// A pick in the Cells menu, drawn at once and saved by the host. The menu stays open, so the styles can be compared.
function setCellView(view) {
  S.cellView = view;
  renderCellsMenu();
  scheduleRail();
  send("cell_view", view);
}

// ---- keys -----------------------------------------------------------------------------------------------------------

const EVENT_KEY_NAMES = { Enter: "Enter", Escape: "Esc", " ": "Space", Backspace: "Backspace", Delete: "Del", Insert: "Ins",
                          PageUp: "PgUp", PageDown: "PgDn", ArrowLeft: "Left", ArrowRight: "Right", ArrowUp: "Up", ArrowDown: "Down",
                          Tab: "Tab", Home: "Home", End: "End" };

// A key event as KeyChord::to_string() writes a chord: Ctrl (⌘ on macOS), Shift, Alt, RawCtrl, then the key.
function chordOfEvent(e) {
  let key = "";
  if (/^Key[A-Z]$/.test(e.code)) key = e.code.slice(3);
  else if (/^Digit\d$/.test(e.code)) key = e.code.slice(5);
  else if (EVENT_KEY_NAMES[e.key]) key = EVENT_KEY_NAMES[e.key];
  else if (/^F\d+$/.test(e.key)) key = e.key;
  else if (e.key.length === 1) key = e.key.toUpperCase();
  if (!key) return "";
  const mods = [];
  if (IS_MAC ? e.metaKey : e.ctrlKey) mods.push("Ctrl");
  if (e.shiftKey) mods.push("Shift");
  if (e.altKey) mods.push("Alt");
  if (IS_MAC && e.ctrlKey) mods.push("RawCtrl");
  return mods.map((m) => m + "+").join("") + key;
}

function markOwnKey(name) { editor.$orcaOwnAt[name] = Date.now(); }

// The editor's commands: Run all, Run cell, Save, Save as, the panel's toggle, and Ctrl+R, which does nothing. None
// edits the text, so all work while the editor is read-only.
function pageCommands() {
  const commands = [
    { name: "runAll", bindKey: { win: "Ctrl-Return", mac: "Command-Return" }, exec: () => runAll(), readOnly: true },
    { name: "runCell", bindKey: { win: "Shift-Return", mac: "Shift-Return" }, exec: () => runCell(), readOnly: true },
    { name: "save", bindKey: { win: "Ctrl-S", mac: "Command-S" }, exec: () => save(false), readOnly: true },
    { name: "saveAs", bindKey: { win: "Ctrl-Shift-S", mac: "Command-Shift-S" }, exec: () => save(true), readOnly: true },
    { name: "noReload", bindKey: { win: "Ctrl-R", mac: "Command-R" }, exec: () => {}, readOnly: true },
  ];
  if (S.toggleChord) commands.push(toggleCommand());
  return commands;
}

function toggleCommand() { return { name: "togglePanel", bindKey: aceKey(S.toggleChord), exec: () => send("toggle"), readOnly: true }; }

function setToggleChord(chord) {
  S.toggleChord = chord || "";
  editor.commands.removeCommand("togglePanel");
  if (S.toggleChord) {
    editor.commands.addCommand(toggleCommand());
    editor.$orcaCommands.add("togglePanel");
  }
}

// Keys elsewhere on the page; the editor's own commands take them in the editor.
function onDocumentKey(e) {
  if (isEditorText(editor, e.target)) return;
  const chord = chordOfEvent(e);
  if (!chord) return;
  const act = (fn) => {
    e.preventDefault();
    fn();
  };
  if (chord === S.toggleChord) act(() => send("toggle"));
  else if (chord === "Ctrl+S") act(() => (markOwnKey("save"), save(false)));
  else if (chord === "Ctrl+Shift+S") act(() => (markOwnKey("saveAs"), save(true)));
  else if (chord === "Ctrl+Enter") act(runAll);
  else if (chord === "Shift+Enter") act(runCell);
  else if (chord === "Ctrl+R") act(() => {});
  else if (chord === "Esc" && menuOpen()) act(closeMenus);
}

// A key the host forwards from its menus while the panel has the focus, unless the page took the same press itself.
function onForwardedKey(action) {
  if (action === "save" || action === "save_as") {
    const name = action === "save" ? "save" : "saveAs";
    if (!ownKeyRan(editor, name)) save(action === "save_as");
  } else
    runEditorAction(editor, action);
}

// ---- host messages ----------------------------------------------------------------------------------------------

function HandleStudio(msg) {
  switch (msg.command) {
  case "state":
    S.ready = !!msg.ready;
    S.recent = msg.recent || [];
    S.session = msg.session || "none";
    setToggleChord(msg.toggle_key && msg.toggle_key.chord);
    S.cellView = RunScriptModel.cellView(msg.cell_view);
    S.layout = RunScriptModel.paneLayout(msg.pane_layout);
    renderCellsMenu();
    applyScript(msg.script || { none: true });
    renderRecent();
    break;
  case "service":
    S.ready = !!msg.ready;
    render();
    break;
  case "catalog": source.tools = msg.tools || []; break;
  case "api": source.api = msg.entries || []; break;
  case "script": applyScript(msg); break;
  case "saved":
    S.fileName = msg.file_name;
    S.savedText = RunScriptModel.editorText(msg.text);
    onEdit(scriptText());
    break;
  case "recent":
    S.recent = msg.items || [];
    renderRecent();
    break;
  case "error":
    S.message = msg.message;
    render();
    break;
  case "run_started": onRunStarted(msg); break;
  case "run_refused": onRunRefused(msg); break;
  case "output": appendOutput(msg.text); break;
  case "result": appendResult(msg.text); break;
  case "cell": onCell(msg); break;
  case "run_finished": onRunFinished(msg); break;
  case "session":
    S.session = msg.state;
    // A session that ended takes what ran in it along: every cell is grey again.
    if (S.session === "none" && !S.run) resetSessionMarks();
    render();
    break;
  case "counts":
    S.counts = msg;
    render();
    break;
  case "key": onForwardedKey(msg.action); break;
  case "shortcuts": setToggleChord(msg.toggle_key && msg.toggle_key.chord); break;
  }
}

function OnInit() {
  TranslatePage();
  // Ace has already split the line when it asks for the next line's indent, so the line is the one above the caret.
  definePythonMode((line) => {
    let row = editor.getCursorPosition().row;
    if (row > 0 && editor.session.getLine(row - 1) === line) --row;
    return editor.session.getLines(0, row - 1).join("\n");
  });
  editor = createCodeEditor($("editor"), {
    mode: "ace/mode/orca_python",
    tabSize: 4,
    completer: pythonCompleter(source),
    onChange: onEdit,
    commands: pageCommands(),
  });
  editor.session.on("changeScrollTop", scheduleRail);
  editor.renderer.on("resize", scheduleRail);
  editor.session.on("changeFold", scheduleRail);
  editor.selection.on("changeCursor", () => {
    if (S.cellView.highlight) scheduleRail();
  });
  setUpAddCell();
  setUpSplit();

  const runAllKey = T("key_run_all", "Ctrl+Enter");
  const runCellKey = T("key_run_cell", "Shift+Enter");
  $("run-all-key").textContent = runAllKey;
  $("run-cell-key").textContent = runCellKey;
  $("run-all").title = T("run_all_tip", "Restart the session and run every cell (%s)", runAllKey);
  $("run-cell").title = T("run_cell_tip", "Run the cell at the cursor (%s)", runCellKey);
  $("save").title = T("save_tip", "Save the script (%s)", T("key_save", "Ctrl+S"));
  $("new").title = T("new_tip", "Start a new script");
  $("open").title = T("open_tip", "Open a Python script");
  $("stop").title = T("stop_tip", "Interrupt the code that runs now and the threads the session started");
  $("restart").title = T("restart_tip", "End the session: its variables, imports and threads go");
  $("clear").title = T("clear_tip", "Clear the console: the session and its variables stay");
  $("cells").title = T("cells_tip", "How the editor shows the script's cells");
  renderCellsMenu();
  AppendTemplate($("start-text"), T("start_script_text",
    "A Python script runs inside OrcaSlicer and can call every tool an AI agent can, plus the whole orca.host API. Split it into cells with %s and try one cell at a time."),
    el("code", "", "# %%"));

  $("new").addEventListener("click", () => send("new"));
  $("start-new").addEventListener("click", () => send("new"));
  $("start-open").addEventListener("click", () => send("open"));
  $("open").addEventListener("click", () => ($("open-menu").hidden ? openMenu("open", $("open-file")) : closeMenus()));
  $("open-file").addEventListener("click", () => {
    closeMenus();
    send("open");
  });
  $("close-script").addEventListener("click", () => {
    closeMenus();
    send("close_script");
  });
  $("cells").addEventListener("click", () =>
    ($("cells-menu").hidden ? openMenu("cells", $("cells-menu").querySelector('[aria-checked="true"]')) : closeMenus()));
  for (const item of document.querySelectorAll("#cells-menu [data-style]"))
    item.addEventListener("click", () => setCellView({ style: item.dataset.style, highlight: S.cellView.highlight }));
  $("cells-highlight").addEventListener("click", () => setCellView({ style: S.cellView.style, highlight: !S.cellView.highlight }));
  document.addEventListener("mousedown", (e) => {
    if (menuOpen() && !e.target.closest(".menu-wrap")) closeMenus();
  });
  $("save").addEventListener("click", () => save(false));
  $("run-all").addEventListener("click", runAll);
  $("run-cell").addEventListener("click", runCell);
  $("stop").addEventListener("click", () => send("stop"));
  $("restart").addEventListener("click", () => send("restart"));
  $("clear").addEventListener("click", clearConsole);
  $("prompt-input").addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey && !e.ctrlKey && !e.metaKey && !e.altKey) {
      e.preventDefault();
      runConsole();
    }
  });
  document.addEventListener("keydown", onDocumentKey);

  render();
  send("init");
  // The keys the editor takes, which the host keeps from its menus while the panel has the focus.
  send("editor_keys", { chords: editorChords(editor) });
}
