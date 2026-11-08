// The code editor of the bundled pages: Ace, vendored unmodified in resources/web/include/ace and loaded before this
// file by script tags, since the pages are file:// URLs. Everything an editor uses is vendored, so nothing here may make
// Ace fetch a file: workers are off, snippets are off, completers are passed as arrays (`true` would add Ace's snippet and
// keyword completers), the theme is an object (code_editor.css holds its colours), modes are objects, and the default
// commands that would load an extension the repo doesn't vendor are removed. Ace's own text (the search box, the
// suggestion list, the read-only note, folding, the ARIA labels) comes from the page's strings under "code_editor"
// (WebViewHostDialog::code_editor_strings()).

// Ace's default commands that load a file the repo doesn't vendor (the settings menu, the error marker, the prompt behind
// Go to line, the command palette and the mode list).
const UNVENDORED_COMMANDS = ["showSettingsMenu", "goToNextError", "goToPreviousError", "gotoline", "openCommandPalette",
                             "openCommandPallete", "modeSelect"];

// How long after Ace's own undo, redo or select all the same action forwarded by the host is the same key press.
const FORWARDED_KEY_WINDOW_MS = 150;

const IS_MAC = typeof navigator !== "undefined" && /Mac/.test(navigator.platform || navigator.userAgent);

let codeEditorConfigured = false;

// Ace's messages from the page's strings: each %s, in order, becomes Ace's $0, $1.
function configureCodeEditor() {
  if (codeEditorConfigured) return;
  codeEditorConfigured = true;
  const strings = typeof ORCA_UI_STRINGS !== "undefined" && ORCA_UI_STRINGS && ORCA_UI_STRINGS.code_editor;
  if (!strings) return;
  const messages = {};
  for (const [key, text] of Object.entries(strings)) {
    let n = 0;
    messages[key] = String(text).replace(/%s/g, () => "$" + n++);
  }
  ace.config.setMessages(messages);
}

function pageIsDark() { return document.documentElement.getAttribute("data-orca-theme") === "dark"; }

function setEditorDark(editor, dark) { editor.setTheme({ cssClass: "ace-orca", isDark: !!dark, cssText: "" }); }

// A position in the text as an offset, and back.
function indexOf(editor, pos) { return editor.session.doc.positionToIndex(pos); }
function positionOf(editor, index) { return editor.session.doc.indexToPosition(index); }

// Writes `edit` ({from, to, insert, back}, offsets in the text) as one change, the caret `back` characters before the end
// of what was inserted.
function applyEditorEdit(editor, edit) {
  const Range = ace.require("ace/range").Range;
  const start = positionOf(editor, edit.from);
  const end = positionOf(editor, edit.to);
  editor.session.replace(new Range(start.row, start.column, end.row, end.column), edit.insert);
  editor.selection.moveToPosition(positionOf(editor, edit.from + edit.insert.length - (edit.back || 0)));
  editor.renderer.scrollCursorIntoView();
}

// A page's completer ({suggestionsAt, acceptEdit, identifierRegexps, triggerCharacters}) as an Ace completer. The list
// shows what suggestionsAt() finds at the caret, in its order; accepting an item finds the range again at the caret,
// since Ace keeps the list open and filters it while more is typed without asking again, and writes acceptEdit()'s edit.
function aceCompleter(completer) {
  const adapted = {
    identifierRegexps: completer.identifierRegexps,
    triggerCharacters: completer.triggerCharacters,
    getCompletions(editor, session, pos, prefix, callback) {
      // Ctrl+Space opens the list as asked for; typing opens it on its own (autoShown).
      const explicit = !(editor.completer && editor.completer.autoShown);
      const found = completer.suggestionsAt(editor.getValue(), indexOf(editor, pos), explicit);
      if (!found || !found.items.length) return callback(null, []);
      const count = found.items.length;
      // Each completion names its completer, whose insertMatch() Ace then calls.
      callback(null, found.items.map((item, i) => ({ caption: item.label, value: item.label, meta: item.detail || "", score: count - i, item,
                                                     completer: adapted })));
    },
    getDocTooltip(completion) {
      if (completion.item && completion.item.info && !completion.docText) completion.docText = completion.item.info;
    },
    insertMatch(editor, data) {
      const text = editor.getValue();
      const found = completer.suggestionsAt(text, indexOf(editor, editor.getCursorPosition()), true);
      if (!found) return;
      const edit = completer.acceptEdit(text, found, data.item);
      applyEditorEdit(editor, edit);
      // After the list has closed for this acceptance.
      if (edit.reopen) setTimeout(() => editor.execCommand("startAutocomplete"), 0);
    },
  };
  return adapted;
}

// An editor in `element`: options = {mode (an Ace mode id), tabSize, placeholder, completer, onChange(text), commands}.
// Each command is {name, bindKey: {win, mac}, exec(editor), readOnly}; one that doesn't edit the text is readOnly, since
// Ace refuses any other in a read-only editor. onChange runs once per batch of changes, from the session's change event,
// so the host's copy is never behind (Ace's own input event comes later).
function createCodeEditor(element, options) {
  configureCodeEditor();
  const completers = options.completer ? [aceCompleter(options.completer)] : false;
  const editor = ace.edit(element, {
    useWorker: false,
    enableSnippets: false,
    enableBasicAutocompletion: completers,
    enableLiveAutocompletion: completers,
    useSoftTabs: true,
    tabSize: options.tabSize || 2,
    showPrintMargin: false,
    fixedWidthGutter: true,
    highlightActiveLine: false,
    fontSize: "12px",
    placeholder: options.placeholder || "",
  });
  editor.session.setNewLineMode("unix");
  const Mode = ace.require(options.mode).Mode;
  editor.session.setMode(new Mode());
  setEditorDark(editor, pageIsDark());
  new MutationObserver(() => setEditorDark(editor, pageIsDark()))
    .observe(document.documentElement, { attributes: true, attributeFilter: ["data-orca-theme"] });

  for (const name of UNVENDORED_COMMANDS) editor.commands.removeCommand(name);
  // Shift+Enter is the page's (Run cell) even while the suggestion list is open.
  if (completers) ace.require("ace/autocomplete").Autocomplete.for(editor).keyboardHandler.removeCommand("Shift-Return");
  editor.$orcaCommands = new Set();
  for (const command of options.commands || []) {
    editor.commands.addCommand(command);
    editor.$orcaCommands.add(command.name);
  }

  editor.$orcaOwnAt = {};
  editor.commands.on("afterExec", (e) => {
    if (!editor.$orcaForwarding) editor.$orcaOwnAt[e.command.name] = Date.now();
  });
  document.addEventListener("keydown", (e) => recordFieldKey(editor, e), true);
  let reported = false;
  editor.session.on("change", () => {
    if (editor.$orcaQuiet || reported || !options.onChange) return;
    reported = true;
    queueMicrotask(() => {
      reported = false;
      options.onChange(editor.getValue());
    });
  });
  return editor;
}

// Replaces the text without reporting it as an edit, with no undo history and the caret at the start.
function setEditorText(editor, text) {
  editor.$orcaQuiet = true;
  try {
    editor.session.setValue(text);
  } finally {
    editor.$orcaQuiet = false;
  }
  editor.session.getUndoManager().reset();
  editor.clearSelection();
  editor.moveCursorTo(0, 0);
  editor.renderer.scrollToRow(0);
}

function closeEditorSuggestions(editor) {
  if (editor.completer && editor.completer.activated) editor.completer.detach();
}

// ---- keys -------------------------------------------------------------------------------------------------------

// KeyChord::to_string()'s special key names and Ace's.
const CHORD_TO_ACE_KEYS = { Backspace: "Backspace", Tab: "Tab", Enter: "Return", Esc: "Esc", Space: "Space", Del: "Delete",
                            Ins: "Insert", Home: "Home", End: "End", PgUp: "PageUp", PgDn: "PageDown", Left: "Left",
                            Right: "Right", Up: "Up", Down: "Down" };
const ACE_TO_CHORD_KEYS = { backspace: "Backspace", tab: "Tab", return: "Enter", enter: "Enter", esc: "Esc", escape: "Esc",
                            space: "Space", delete: "Del", del: "Del", insert: "Ins", home: "Home", end: "End",
                            pageup: "PgUp", pagedown: "PgDn", left: "Left", right: "Right", up: "Up", down: "Down" };

// A chord in KeyChord::to_string()'s canonical form ("Ctrl+Shift+R", Ctrl being ⌘ on macOS) in Ace's ("Ctrl-Shift-R" off
// macOS, "Command-Shift-R" on it). Never the translated text the menus show.
function aceKey(chord) {
  const parts = chord.length > 1 && chord.endsWith("+") ? [...chord.slice(0, -2).split("+").filter(Boolean), "+"] : chord.split("+");
  const key = parts.pop();
  const name = CHORD_TO_ACE_KEYS[key] || key;
  const win = [];
  const mac = [];
  for (const modifier of parts) {
    if (modifier === "Ctrl") { win.push("Ctrl"); mac.push("Command"); }
    else if (modifier === "Shift") { win.push("Shift"); mac.push("Shift"); }
    else if (modifier === "Alt") { win.push("Alt"); mac.push("Option"); }
    else if (modifier === "RawCtrl") mac.push("Ctrl");
  }
  return { win: [...win, name].join("-"), mac: [...mac, name].join("-") };
}

// An Ace key binding on this platform ("Ctrl-Shift-K", "Command-Alt-Up") in KeyChord's canonical form, or "" for one it
// can't name (a key chain, a numpad key).
function chordOf(binding) {
  if (/\s/.test(binding)) return "";
  const parts = binding.endsWith("--") ? [...binding.slice(0, -2).split("-").filter(Boolean), "-"] : binding.split("-");
  const key = parts.pop();
  const mods = new Set();
  for (const part of parts) {
    const p = part.toLowerCase();
    const control = p === "ctrl" || p === "control";
    // Ace's Ctrl is the Control key on macOS, where KeyChord's Ctrl is Command.
    if (p === "command" || p === "cmd" || (control && !IS_MAC)) mods.add("Ctrl");
    else if (control) mods.add("RawCtrl");
    else if (p === "shift") mods.add("Shift");
    else if (p === "alt" || p === "option") mods.add("Alt");
    else return "";
  }
  const name = ACE_TO_CHORD_KEYS[key.toLowerCase()] || (/^f\d+$/i.test(key) ? key.toUpperCase() : key.length === 1 ? key.toUpperCase() : "");
  if (!name) return "";
  return ["Ctrl", "Shift", "Alt", "RawCtrl"].filter((m) => mods.has(m)).map((m) => m + "+").join("") + name;
}

// The chords Ace's own commands bind on this platform, in KeyChord's canonical form, leaving out the page's commands
// (Save, Run, the toggle are the app's): what the host disables in its menus while the editor has the focus. Ace's own
// bindings never change, so the list doesn't change with a rebinding.
function editorChords(editor) {
  const platform = editor.commands.platform;
  const chords = new Set();
  for (const [name, command] of Object.entries(editor.commands.commands)) {
    if (editor.$orcaCommands.has(name) || !command.bindKey) continue;
    const binding = typeof command.bindKey === "string" ? command.bindKey : command.bindKey[platform];
    if (!binding) continue;
    for (const one of binding.split("|")) {
      const chord = chordOf(one.trim());
      if (chord) chords.add(chord);
    }
  }
  return [...chords];
}

// The editor's own text input, where its commands take the keys. Ace's search box sits inside the editor too, but its
// fields are text fields of their own.
function isEditorText(editor, el) { return !!el && el === editor.textInput.getElement(); }

// A text field other than the editor's text (a console prompt, Ace's search fields), which does its own undo, redo and
// select all.
function isOtherTextField(editor, el) {
  return !!el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA") && !isEditorText(editor, el);
}

// Whether the page's own `name` command (an editor command, or a key the page handled itself) ran within
// FORWARDED_KEY_WINDOW_MS: the same key press the host then forwards from its menu.
function ownKeyRan(editor, name) { return Date.now() - (editor.$orcaOwnAt[name] || 0) < FORWARDED_KEY_WINDOW_MS; }

// A key pressed in such a field: its undo, redo or select all is recorded as the editor's own commands are, so the same
// press forwarded by the host's menu does nothing more.
function recordFieldKey(editor, e) {
  if (!isOtherTextField(editor, e.target) || !(IS_MAC ? e.metaKey : e.ctrlKey) || e.altKey) return;
  const key = (e.key || "").toLowerCase();
  const name = key === "a" && !e.shiftKey ? "selectall" : key === "z" ? (e.shiftKey ? "redo" : "undo") : key === "y" && !e.shiftKey ? "redo" : "";
  if (name) editor.$orcaOwnAt[name] = Date.now();
}

// Runs a forwarded "undo", "redo" or "select_all" (the host's Edit menu) unless the same action ran by its own key within
// FORWARDED_KEY_WINDOW_MS, so one press acts once whichever way it reached the page. A focused text field outside the
// editor takes it; otherwise the editor does.
function runEditorAction(editor, action) {
  const name = { undo: "undo", redo: "redo", select_all: "selectall" }[action];
  if (!name || ownKeyRan(editor, name)) return;
  const field = document.activeElement;
  if (isOtherTextField(editor, field)) {
    if (name === "selectall") field.select();
    else document.execCommand(name);
    return;
  }
  editor.$orcaForwarding = true;
  try {
    editor.execCommand(name);
  } finally {
    editor.$orcaForwarding = false;
  }
}

if (typeof module !== "undefined") module.exports = { runEditorAction, recordFieldKey, ownKeyRan, isEditorText };
