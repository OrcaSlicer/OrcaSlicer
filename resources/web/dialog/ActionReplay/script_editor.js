// The replay editor's language: a replay's highlighting (the orca_replay mode), and suggestions for its keys, tool
// names, a tool's args and their allowed values, and ${…} placeholders, with what accepting one and Return write. The
// editor is Ace, set up by ../js/code_editor.js; the pure functions here are also loaded by script_editor.test.js.

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
// line), numbers, literals and punctuation: what the reader walks.
const TOKEN = /(\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:[^"\\\n]|\\.)*)("?)|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)|\b(true|false|null)\b|([{}[\],:])/g;

const ed = {
  tools: new Map(), // tool name -> {title, args: {name: {type, enum}}}, from the host
  editor: null,     // the Ace editor, once initEditor() made it
};

// ---- highlighting ---------------------------------------------------------------------------------

// Ace's JSON mode with a replay's own highlighting: // and /* */ comments, keys (a string a colon follows on its line),
// strings, ${…} inside keys and strings, numbers, true/false/null and punctuation, coloured by code_editor.css.
function defineReplayMode() {
  ace.define("ace/mode/orca_replay_highlight_rules", ["require", "exports", "module", "ace/lib/oop", "ace/mode/text_highlight_rules"],
    (require, exports) => {
      const oop = require("ace/lib/oop");
      const TextHighlightRules = require("ace/mode/text_highlight_rules").TextHighlightRules;
      function ReplayHighlightRules() {
        const placeholder = { token: "orca_var", regex: /\$\{[^}"]*\}/ };
        this.$rules = {
          start: [
            { token: "comment", regex: /\/\/.*$/ },
            { token: "comment", regex: /\/\*/, next: "comment" },
            { token: "variable", regex: /"(?=(?:[^"\\]|\\.)*"\s*:)/, next: "key" },
            { token: "string", regex: /"/, next: "string" },
            { token: "constant.numeric", regex: /-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/ },
            { token: "constant.language", regex: /\b(?:true|false|null)\b/ },
            { token: "paren.lparen", regex: /[[{]/ },
            { token: "paren.rparen", regex: /[\]}]/ },
            { token: "punctuation", regex: /[,:]/ },
          ],
          key: [placeholder, { token: "variable", regex: /\\./ }, { token: "variable", regex: /"|$/, next: "start" }, { defaultToken: "variable" }],
          string: [placeholder, { token: "string", regex: /\\./ }, { token: "string", regex: /"|$/, next: "start" }, { defaultToken: "string" }],
          comment: [{ token: "comment", regex: /\*\//, next: "start" }, { defaultToken: "comment" }],
        };
      }
      oop.inherits(ReplayHighlightRules, TextHighlightRules);
      exports.ReplayHighlightRules = ReplayHighlightRules;
    });
  ace.define("ace/mode/orca_replay", ["require", "exports", "module", "ace/lib/oop", "ace/mode/json", "ace/mode/orca_replay_highlight_rules"],
    (require, exports) => {
      const oop = require("ace/lib/oop");
      const JsonMode = require("ace/mode/json").Mode;
      function Mode() {
        JsonMode.call(this);
        this.HighlightRules = require("ace/mode/orca_replay_highlight_rules").ReplayHighlightRules;
      }
      oop.inherits(Mode, JsonMode);
      Mode.prototype.$id = "ace/mode/orca_replay";
      // A replay is JSON with comments: JSON's worker would flag them.
      Mode.prototype.createWorker = () => null;
      exports.Mode = Mode;
    });
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

// ---- suggestions -----------------------------------------------------------------------------------

// What accepting `item` of what suggestionsAt() `found` writes: [from, to) replaced with `insert`, the
// caret `back` characters before its end, and whether the suggestions open again there. A key gets its
// colon and an empty value of its type (unless a colon follows already), with the caret inside a
// string, object or array; a ${…} gets its closing brace.
function acceptEdit(text, found, item) {
  if (found.kind === "var")
    return { from: found.from, to: text[found.to] === "}" ? found.to + 1 : found.to, insert: item.label + "}", back: 0, reopen: false };
  let insert = (found.quoted ? "" : '"') + item.label + '"';
  let back = 0;
  let then = "";
  if (found.kind === "key" && !/^\s*:/.test(text.slice(found.to))) {
    then = item.then !== undefined ? item.then : EMPTY_VALUES[item.detail] || "";
    insert += ": " + then;
    back = then.length === 2 ? 1 : 0;
  }
  return { from: found.from, to: found.to, insert, back, reopen: then === '""' };
}

// What Return writes for the selection [start, end): the line's indent, one level more after an
// opening bracket; between a pair of brackets the closing one goes on its own line, the caret `back`
// characters before the end on the line between.
function newlineEdit(text, start, end) {
  const lineStart = text.lastIndexOf("\n", start - 1) + 1;
  const indent = /^[ \t]*/.exec(text.slice(lineStart, start))[0];
  const opens = /[{[]\s*$/.test(text.slice(lineStart, start));
  const closes = /^[ \t]*[}\]]/.test(text.slice(end));
  if (opens && closes) return { from: start, to: end, insert: "\n" + indent + "  \n" + indent, back: indent.length + 1 };
  return { from: start, to: end, insert: "\n" + indent + (opens ? "  " : ""), back: 0 };
}

// ---- the editor ------------------------------------------------------------------------------------

function setEditorTools(tools) {
  ed.tools = new Map(tools.map((tool) => [tool.name, tool]));
}

// What the replay editor suggests: the word Ace filters by is the text from where a suggestion starts to the caret, so
// it takes the characters of a tool name, a key and vars.x; typing a quote or a brace (of ${) opens the list.
const replayCompleter = {
  suggestionsAt,
  acceptEdit,
  identifierRegexps: [/[\w.-]/],
  triggerCharacters: ['"', "{"],
};

// Makes the editor in #editor: `onChange(text)` for every edit, and the page's `commands` (Run, Save, Save as) beside
// Return, which writes newlineEdit()'s edit, since Ace's own brace handling splits only {}.
function initEditor(onChange, commands) {
  defineReplayMode();
  const newlineCommand = {
    name: "replayNewline",
    bindKey: { win: "Return", mac: "Return" },
    exec: (editor) => {
      const range = editor.getSelectionRange();
      const doc = editor.session.doc;
      applyEditorEdit(editor, newlineEdit(editor.getValue(), doc.positionToIndex(range.start), doc.positionToIndex(range.end)));
    },
  };
  ed.editor = createCodeEditor(document.getElementById("editor"), {
    mode: "ace/mode/orca_replay",
    tabSize: 2,
    placeholder: T("source_placeholder", "Write a replay here, or open one."),
    completer: replayCompleter,
    onChange,
    commands: [newlineCommand, ...commands],
  });
  return ed.editor;
}

// The language's pure functions, for the page's tests (node --test), which load this file as a module.
if (typeof module === "object" && module.exports)
  module.exports = { readScript, suggestionsAt, acceptEdit, newlineEdit, setEditorTools };
