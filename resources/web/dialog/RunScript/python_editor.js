// The Run Script editor's Python: Ace's Python mode, with a line that starts a cell (`# %%`) as its own token and
// Return's indent from pythonNextIndent(), and the suggestions for this build's tools and the orca.host API. The editor is
// set up by ../js/code_editor.js; the logic is run_script_model.js's.

// `linesBefore(line)` gives the text before the line Return splits (`line`, its part before the caret), for the
// triple-quoted string open where that line starts.
function definePythonMode(linesBefore) {
  ace.define("ace/mode/orca_python_highlight_rules",
    ["require", "exports", "module", "ace/lib/oop", "ace/mode/python_highlight_rules"], (require, exports) => {
      const oop = require("ace/lib/oop");
      const PythonHighlightRules = require("ace/mode/python_highlight_rules").PythonHighlightRules;
      function OrcaPythonHighlightRules() {
        PythonHighlightRules.call(this);
        // Before Python's own comment rule, which would take the line.
        this.$rules.start.unshift({ token: "cell-marker", regex: "^# %%.*$" });
      }
      oop.inherits(OrcaPythonHighlightRules, PythonHighlightRules);
      exports.OrcaPythonHighlightRules = OrcaPythonHighlightRules;
    });
  ace.define("ace/mode/orca_python",
    ["require", "exports", "module", "ace/lib/oop", "ace/mode/python", "ace/mode/orca_python_highlight_rules"], (require, exports) => {
      const oop = require("ace/lib/oop");
      const PythonMode = require("ace/mode/python").Mode;
      function Mode() {
        PythonMode.call(this);
        this.HighlightRules = require("ace/mode/orca_python_highlight_rules").OrcaPythonHighlightRules;
      }
      oop.inherits(Mode, PythonMode);
      Mode.prototype.$id = "ace/mode/orca_python";
      Mode.prototype.getNextLineIndent = (state, line, tab) =>
        RunScriptModel.pythonNextIndent(line, tab, RunScriptModel.openQuote(linesBefore(line)));
      // pythonNextIndent() already outdents after a statement that ends a block.
      Mode.prototype.checkOutdent = () => false;
      exports.Mode = Mode;
    });
}

// A tool's arguments for the detail line: each name, with its allowed values when it has some.
function toolArgsText(tool) {
  return Object.entries(tool.args || {})
    .map(([name, arg]) => (arg.enum ? name + ": " + arg.enum.join(" | ") : name))
    .join(", ");
}

// The completer the editor gets (code_editor.js's shape): the tools inside tools.call("…"), with their titles and
// arguments, and the API one level below an orca.host path, with signatures and summaries. `source` holds the catalog
// ({tools}) and the API's entries ({api}) as the host sends them.
function pythonCompleter(source) {
  return {
    suggestionsAt(text, at) {
      const lineStart = text.lastIndexOf("\n", at - 1) + 1;
      const quote = RunScriptModel.openQuote(text.slice(0, Math.max(0, lineStart - 1)));
      const context = RunScriptModel.suggestContext(text.slice(lineStart, at), quote);
      if (!context) return null;
      const from = lineStart + context.from;
      const to = at + /^\w*/.exec(text.slice(at))[0].length;
      if (context.kind === "tool")
        return { kind: "tool", from, to, quote: text[from - 1],
                 items: source.tools.map((tool) => ({ label: tool.name, detail: tool.title, info: toolArgsText(tool) })) };
      const prefix = context.path + ".";
      const items = [];
      for (const entry of source.api) {
        if (!entry.name.startsWith(prefix)) continue;
        const rest = entry.name.slice(prefix.length);
        if (!rest || rest.includes(".")) continue;
        items.push({ label: rest, detail: entry.kind, info: entry.signature + (entry.summary ? "\n" + entry.summary : "") });
      }
      return { kind: "api", from, to, items };
    },
    // A tool's name closes its string when nothing does yet.
    acceptEdit(text, found, item) {
      const closes = found.kind === "tool" && found.quote && text[found.to] !== found.quote;
      return { from: found.from, to: found.to, insert: item.label + (closes ? found.quote : ""), back: 0, reopen: false };
    },
    identifierRegexps: [/\w/],
    triggerCharacters: ['"', "'", "."],
  };
}
