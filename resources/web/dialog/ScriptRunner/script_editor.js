// The script editor: a textarea over a highlighted copy of its text (style.css says why), a line
// gutter, and suggestions for the script's keys, tool names, a tool's args and their allowed values,
// and ${…} placeholders. The textarea's value is the script; everything here reads it.

// The copy's padding and line height, as style.css sets them for #highlight and #source.
const PAD_X = 8;
const PAD_Y = 6;
const LINE = 18;

// The keys of a script and of a step, with the JSON type of their values. Accepting a key writes a
// value of that type after its colon (an empty string, object or array with the caret inside, or true),
// or the item's own `then`; for a number the caret waits after the colon.
const ROOT_KEYS = [
  { label: "orca_script", detail: "integer", then: "1" },
  { label: "name", detail: "string" },
  { label: "vars", detail: "object" },
  { label: "steps", detail: "array" },
];
const STEP_KEYS = [
  { label: "tool", detail: "string" },
  { label: "args", detail: "object" },
  { label: "expect", detail: "object" },
  { label: "optional", detail: "boolean" },
  { label: "await", detail: "boolean" },
  { label: "save_as", detail: "string" },
  { label: "timeout_s", detail: "integer" },
];
const EMPTY_VALUES = { string: '""', object: "{}", array: "[]", boolean: "true" };
const MATCHERS = ["eq", "ne", "gt", "ge", "lt", "le", "contains", "regex", "exists", "empty"];

// Comments, strings (opening quote and body, then the closing quote; an unclosed one ends with its
// line), numbers, literals and punctuation: what the highlighter paints and the reader walks.
const TOKEN = /(\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:[^"\\\n]|\\.)*)("?)|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b|([{}[\],:])/g;

const ed = {
  tools: new Map(), // tool name -> {title, args: {name: {type, enum}}}, from the host
  lineCount: 0,
  composing: false,
  inserting: false,
  pop: null, // the open suggestions: what suggestionsAt() found, filtered, and the active item
};

function escapeHtml(text) { return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }

function span(cls, html) { return '<span class="' + cls + '">' + html + "</span>"; }

// ---- highlighting -------------------------------------------------------------------------------

function highlightHtml(text) {
  const colon = /\s*:/y;
  let out = "";
  let last = 0;
  let m;
  TOKEN.lastIndex = 0;
  while ((m = TOKEN.exec(text))) {
    out += escapeHtml(text.slice(last, m.index));
    last = TOKEN.lastIndex;
    const tok = escapeHtml(m[0]);
    if (m[1] !== undefined) out += span("t-com", tok);
    else if (m[2] !== undefined) {
      colon.lastIndex = last;
      out += span(colon.test(text) ? "t-key" : "t-str", tok.replace(/\$\{[^}"]*\}/g, (v) => span("t-var", v)));
    } else if (m[4] !== undefined || m[5] !== undefined) out += span("t-num", tok);
    else out += span("t-punct", tok);
  }
  return out + escapeHtml(text.slice(last));
}

function refreshEditor() {
  const source = $("source");
  const text = source.value;
  // A last empty line has no height in the copy, where the textarea gives it one.
  $("highlight").innerHTML = highlightHtml(text) + (text === "" || text.endsWith("\n") ? " " : "");
  const lines = text.split("\n").length;
  if (lines !== ed.lineCount) {
    ed.lineCount = lines;
    $("gutter").textContent = Array.from({ length: lines }, (_, i) => i + 1).join("\n");
  }
  // Sized to the copy, the textarea never needs to scroll; what the browser scrolled to show the
  // caret before the copy grew is undone, and the editor's own scroller shows it instead.
  source.scrollTop = 0;
  source.scrollLeft = 0;
}

// Where the character at `index` starts, in pixels from the top left of .code.
function offsetAt(index) {
  const text = $("source").value;
  const lineStart = text.lastIndexOf("\n", index - 1) + 1;
  const measure = $("measure");
  measure.textContent = text.slice(lineStart, index);
  const line = text.slice(0, lineStart).split("\n").length - 1;
  return { x: PAD_X + measure.getBoundingClientRect().width, y: PAD_Y + line * LINE };
}

function revealCaret() {
  const editor = $("editor");
  const source = $("source");
  const gutter = $("gutter").offsetWidth;
  // The end of a selection that moves, which is its start when it was made backwards.
  const at = offsetAt(source.selectionDirection === "backward" ? source.selectionStart : source.selectionEnd);
  if (at.y - PAD_Y < editor.scrollTop) editor.scrollTop = at.y - PAD_Y;
  else if (at.y + LINE + PAD_Y > editor.scrollTop + editor.clientHeight) editor.scrollTop = at.y + LINE + PAD_Y - editor.clientHeight;
  const x = gutter + at.x;
  if (x - PAD_X < editor.scrollLeft + gutter) editor.scrollLeft = Math.max(0, x - PAD_X - gutter);
  else if (x + PAD_X > editor.scrollLeft + editor.clientWidth) editor.scrollLeft = x + PAD_X - editor.clientWidth;
}

// ---- reading the structure --------------------------------------------------------------------

// Walks the tokens before `at` keeping a frame per open object ({key, colon, keys, tool}) and
// array ({array, index}). Returns the frames, the string `at` is in (after its opening quote) if
// any, and the vars and saved job names seen; null when `at` is in a comment.
function readScript(text, at) {
  const frames = [];
  const vars = new Set();
  const jobs = new Set();
  let m;
  TOKEN.lastIndex = 0;
  while ((m = TOKEN.exec(text)) && m.index < at) {
    const end = TOKEN.lastIndex;
    const top = frames[frames.length - 1];
    if (m[1] !== undefined) {
      if (end > at || (end === at && !m[1].endsWith("*/"))) return null;
      continue;
    }
    if (m[2] !== undefined) {
      const closed = m[3] === '"';
      if (at < end || (at === end && !closed))
        return { frames, vars, jobs, string: { start: m.index, end, closed } };
      const value = m[2].slice(1);
      if (top && !top.array) {
        if (!top.colon) {
          top.key = value;
          top.keys.add(value);
          if (frames.length === 2 && frames[0].key === "vars") vars.add(value);
        } else if (top.key === "tool") top.tool = value;
        else if (top.key === "save_as") jobs.add(value);
      }
      continue;
    }
    switch (m[6]) {
    case "{": frames.push({ array: false, key: null, colon: false, keys: new Set(), tool: null }); break;
    case "[": frames.push({ array: true, index: 0 }); break;
    case "}": case "]": frames.pop(); break;
    case ":": if (top && !top.array) top.colon = true; break;
    case ",":
      if (top && top.array) top.index++;
      else if (top) { top.key = null; top.colon = false; }
      break;
    }
  }
  return { frames, vars, jobs, string: null };
}

// The step a path inside steps[] belongs to, as its frame: frames are root, steps, step, ...
function stepTool(frames) {
  const step = frames[2];
  return step && !step.array ? ed.tools.get(step.tool) : null;
}

function typeText(type) { return Array.isArray(type) ? type.join(" | ") : type || ""; }

function keyItems(frames, path) {
  const top = frames[frames.length - 1];
  let items = [];
  if (path.length === 0) items = ROOT_KEYS;
  else if (path.length === 2 && path[0] === "steps") items = STEP_KEYS;
  else if (path.length === 3 && path[0] === "steps" && path[2] === "args") {
    const tool = stepTool(frames);
    if (tool)
      items = Object.entries(tool.args).map(([name, arg]) => ({
        label: name,
        detail: typeText(arg.type),
        info: arg.enum ? arg.enum.join(" | ") : "",
      }));
  } else if (path.length === 4 && path[0] === "steps" && path[2] === "expect")
    items = MATCHERS.map((label) => ({ label }));
  return items.filter((item) => !top.keys.has(item.label));
}

function valueItems(frames, path) {
  if (path.length === 3 && path[0] === "steps" && path[2] === "tool")
    return [...ed.tools.values()].map((tool) => {
      const args = Object.keys(tool.args);
      return { label: tool.name, detail: tool.title, info: args.length ? "args: " + args.join(", ") : "" };
    });
  if (path.length === 4 && path[0] === "steps" && path[2] === "args") {
    const tool = stepTool(frames);
    const arg = tool && tool.args[path[3]];
    if (arg && arg.enum) return arg.enum.filter((v) => typeof v === "string").map((label) => ({ label }));
  }
  return [];
}

// What can go at `at`: the range a suggestion replaces, how it is written there, and the candidates.
// Inside a string that is a key or a value, or after an unclosed ${ in a string; elsewhere only when
// asked (`explicit`), at a key or value position.
function suggestionsAt(text, at, explicit) {
  const read = readScript(text, at);
  if (!read || !read.frames.length) return null;
  const { frames, string } = read;
  const top = frames[frames.length - 1];
  const path = frames.slice(0, -1).map((f) => (f.array ? f.index : f.key));
  const wordEnd = at + /^[\w.-]*/.exec(text.slice(at))[0].length;
  if (string) {
    const typed = text.slice(string.start + 1, at);
    const open = typed.lastIndexOf("${");
    if (open >= 0 && typed.indexOf("}", open) < 0) {
      const items = ["script_dir", "out_dir", ...[...read.vars].map((k) => "vars." + k), ...[...read.jobs].map((k) => "jobs." + k)];
      return { kind: "var", from: string.start + 3 + open, to: wordEnd, items: items.map((label) => ({ label })) };
    }
  } else if (!explicit) return null;
  // A suggestion replaces what was typed and, in a closed string, the rest of it up to the quote.
  const found = string
    ? { quoted: true, from: string.start + 1, to: string.closed ? string.end : wordEnd }
    : { quoted: false, from: at - /[\w.-]*$/.exec(text.slice(0, at))[0].length, to: wordEnd };
  // Unquoted, only where a key or value can start, not right after one.
  if (!string && !/(^|[{[,:])\s*$/.test(text.slice(0, found.from))) return null;
  if (!top.array && !top.colon) return Object.assign(found, { kind: "key", items: keyItems(frames, path) });
  const value = [...path, top.array ? top.index : top.key];
  return Object.assign(found, { kind: "value", items: valueItems(frames, value) });
}

// ---- suggestions ------------------------------------------------------------------------------

// Items starting with what was typed come first, then those containing it.
function filterItems(items, typed) {
  const needle = typed.toLowerCase();
  const starts = [];
  const contains = [];
  for (const item of items) {
    const pos = item.label.toLowerCase().indexOf(needle);
    if (pos === 0) starts.push(Object.assign({ match: 0 }, item));
    else if (pos > 0) contains.push(Object.assign({ match: pos }, item));
  }
  return starts.concat(contains);
}

function updateSuggest(explicit) {
  const source = $("source");
  if (source.readOnly || source.selectionStart !== source.selectionEnd) return closeSuggest();
  const at = source.selectionEnd;
  const found = suggestionsAt(source.value, at, explicit);
  if (!found) return closeSuggest();
  const typed = source.value.slice(found.from, at);
  const items = filterItems(found.items, typed);
  if (!items.length) return closeSuggest();
  const was = ed.pop && ed.pop.items[ed.pop.active].label;
  ed.pop = Object.assign(found, { items, typed, active: Math.max(0, items.findIndex((item) => item.label === was)) });
  renderSuggest();
}

function closeSuggest() {
  if (!ed.pop) return;
  ed.pop = null;
  $("suggest").hidden = true;
  const source = $("source");
  source.setAttribute("aria-expanded", "false");
  source.removeAttribute("aria-activedescendant");
}

function renderSuggest() {
  const pop = ed.pop;
  const list = $("suggest-list");
  list.textContent = "";
  pop.items.forEach((item, i) => {
    const li = el("li", i === pop.active ? "is-active" : "");
    li.id = "suggest-" + i;
    li.setAttribute("role", "option");
    li.setAttribute("aria-selected", String(i === pop.active));
    const name = el("span", "s-name");
    const n = pop.typed.length;
    name.append(item.label.slice(0, item.match));
    if (n) name.appendChild(el("b", "", item.label.slice(item.match, item.match + n)));
    name.append(item.label.slice(item.match + n));
    li.appendChild(name);
    if (item.detail) li.appendChild(el("span", "s-detail", item.detail));
    // Keeps the caret in the editor; the click itself accepts.
    li.addEventListener("mousedown", (e) => {
      e.preventDefault();
      acceptSuggest(i);
    });
    list.appendChild(li);
  });
  const active = pop.items[pop.active];
  $("suggest-detail").hidden = !active.info;
  $("suggest-detail").textContent = active.info || "";
  const source = $("source");
  source.setAttribute("aria-expanded", "true");
  source.setAttribute("aria-activedescendant", "suggest-" + pop.active);
  const box = $("suggest");
  box.hidden = false;
  placeSuggest();
  $("suggest-" + pop.active).scrollIntoView({ block: "nearest" });
}

// Under the caret's line with the names starting under what was typed; above the line when the
// space below is too small.
function placeSuggest() {
  if (!ed.pop) return;
  const box = $("suggest");
  const code = document.querySelector(".code").getBoundingClientRect();
  const at = offsetAt(ed.pop.from);
  const left = code.left + at.x - 11;
  const top = code.top + at.y;
  box.style.left = Math.max(8, Math.min(left, innerWidth - box.offsetWidth - 8)) + "px";
  const below = top + LINE + 3;
  box.style.top = (below + box.offsetHeight <= innerHeight - 8 ? below : Math.max(8, top - box.offsetHeight - 3)) + "px";
}

function moveSuggest(step) {
  const pop = ed.pop;
  pop.active = (pop.active + step + pop.items.length) % pop.items.length;
  renderSuggest();
}

// Replaces [from, to) with `text` through the editing commands, so undo takes it back, and puts the
// caret `back` characters before the end of what was inserted.
function insertText(from, to, text, back) {
  const source = $("source");
  source.setSelectionRange(from, to);
  ed.inserting = true;
  if (!document.execCommand("insertText", false, text)) {
    source.setRangeText(text, from, to, "end");
    source.dispatchEvent(new Event("input"));
  }
  ed.inserting = false;
  const caret = from + text.length - (back || 0);
  source.setSelectionRange(caret, caret);
}

function acceptSuggest(index) {
  const pop = ed.pop;
  const item = pop.items[index === undefined ? pop.active : index];
  const text = $("source").value;
  closeSuggest();
  if (pop.kind === "var") {
    insertText(pop.from, text[pop.to] === "}" ? pop.to + 1 : pop.to, item.label + "}");
    return;
  }
  let insert = (pop.quoted ? "" : '"') + item.label + '"';
  let back = 0;
  let then = "";
  if (pop.kind === "key" && !/^\s*:/.test(text.slice(pop.to))) {
    then = item.then !== undefined ? item.then : EMPTY_VALUES[item.detail] || "";
    insert += ": " + then;
    back = then.length === 2 ? 1 : 0;
  }
  insertText(pop.from, pop.to, insert, back);
  if (then === '""') updateSuggest(false);
}

// Return keeps the line's indent, one level more after an opening bracket; between a pair of
// brackets the closing one goes on its own line.
function newline() {
  const source = $("source");
  const text = source.value;
  const start = source.selectionStart;
  const lineStart = text.lastIndexOf("\n", start - 1) + 1;
  const indent = /^[ \t]*/.exec(text.slice(lineStart, start))[0];
  const opens = /[{[]\s*$/.test(text.slice(lineStart, start));
  const closes = /^[ \t]*[}\]]/.test(text.slice(source.selectionEnd));
  if (opens && closes) insertText(start, source.selectionEnd, "\n" + indent + "  \n" + indent, indent.length + 1);
  else insertText(start, source.selectionEnd, "\n" + indent + (opens ? "  " : ""));
}

// ---- events -----------------------------------------------------------------------------------

function onEditorInput(e) {
  refreshEditor();
  revealCaret();
  if (ed.inserting || ed.composing) return;
  const type = e.inputType || "";
  if (type === "insertText" || (ed.pop && type.startsWith("delete"))) updateSuggest(false);
  else closeSuggest();
}

function onEditorKey(e) {
  if (e.isComposing || e.keyCode === 229) return;
  // Ctrl/Cmd+Enter runs the script (script_runner.js).
  if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) return;
  if (ed.pop) {
    switch (e.key) {
    case "ArrowDown": moveSuggest(1); break;
    case "ArrowUp": moveSuggest(-1); break;
    case "PageDown": moveSuggest(Math.min(6, ed.pop.items.length - 1 - ed.pop.active)); break;
    case "PageUp": moveSuggest(-Math.min(6, ed.pop.active)); break;
    case "Enter": case "Tab": acceptSuggest(); break;
    case "Escape": closeSuggest(); break;
    // Moving the caret leaves what the suggestions would replace; the key itself goes on.
    case "ArrowLeft": case "ArrowRight": case "Home": case "End": closeSuggest(); return;
    default: return;
    }
    e.preventDefault();
    return;
  }
  if (e.code === "Space" && e.ctrlKey && !e.metaKey && !e.altKey) {
    e.preventDefault();
    updateSuggest(true);
  } else if (e.key === "Enter" && !e.shiftKey && !e.altKey && !e.target.readOnly) {
    e.preventDefault();
    newline();
  }
}

function setEditorTools(tools) {
  ed.tools = new Map(tools.map((tool) => [tool.name, tool]));
}

function initEditor() {
  const source = $("source");
  source.addEventListener("input", onEditorInput);
  source.addEventListener("keydown", onEditorKey);
  source.addEventListener("keyup", (e) => {
    if (/^(Arrow|Page|Home|End)/.test(e.key)) revealCaret();
  });
  source.addEventListener("blur", closeSuggest);
  source.addEventListener("mousedown", closeSuggest);
  source.addEventListener("scroll", () => {
    source.scrollTop = 0;
    source.scrollLeft = 0;
  });
  source.addEventListener("compositionstart", () => {
    ed.composing = true;
    closeSuggest();
    $("editor").classList.add("is-composing");
  });
  source.addEventListener("compositionend", () => {
    ed.composing = false;
    $("editor").classList.remove("is-composing");
    refreshEditor();
  });
  $("editor").addEventListener("scroll", placeSuggest);
  window.addEventListener("resize", closeSuggest);
  refreshEditor();
}
