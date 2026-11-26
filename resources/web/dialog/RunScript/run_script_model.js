// The Run Script page's pure logic: cells, Python's Return indent, where suggestions go, a traceback's links, the
// margin after Run all, what Clear keeps, the rows each cell style marks, a JSON result's tokens and layout, a folded
// cell's line, the edit Add a cell makes, and the divider between the editor and the console. Loaded by the page as a
// script and by run_script.test.js as a module.
(function (root, factory) {
  "use strict";
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.RunScriptModel = api;
})(typeof globalThis === "object" ? globalThis : this, function () {
  "use strict";

  // A line that starts a cell: "# %%" in the first column; the rest of the line is the cell's title.
  const CELL_MARKER = /^# %%(.*)$/;

  // The cells of `text`, 1-based lines: [{index, start, end, title}]. The lines before the first marker are cell 1.
  function splitCells(text) {
    const cells = [];
    let current = null;
    text.split("\n").forEach((raw, i) => {
      const line = i + 1;
      const marker = CELL_MARKER.exec(raw.replace(/\r$/, ""));
      if (marker) {
        if (current) current.end = line - 1;
        current = { index: cells.length + 1, start: line, end: line, title: marker[1].trim() };
        cells.push(current);
      } else if (!current) {
        current = { index: 1, start: 1, end: line, title: "" };
        cells.push(current);
      } else current.end = line;
    });
    return cells;
  }

  // The cell whose lines hold `line`; the last cell for a line past the end.
  function cellAt(cells, line) {
    for (const cell of cells)
      if (line >= cell.start && line <= cell.end) return cell;
    return cells.length ? cells[cells.length - 1] : null;
  }

  // Where `line` stands at `end`: in a string (its quote), in a comment, or in code. `quote` is the triple-quoted string
  // open when the line starts (openQuote()), if any.
  function stateAt(line, end, quote = null) {
    for (let i = 0; i < end; ++i) {
      if (quote) {
        if (line[i] === "\\") ++i;
        else if (line.startsWith(quote, i)) {
          i += quote.length - 1;
          quote = null;
        }
        continue;
      }
      const c = line[i];
      if (c === "#") return { quote: null, comment: true, at: i };
      if (c === '"' || c === "'") {
        quote = line.startsWith(c.repeat(3), i) ? c.repeat(3) : c;
        i += quote.length - 1;
      }
    }
    return { quote, comment: false, at: end };
  }

  // The line's code: what comes before a comment that isn't inside a string.
  function codeOf(line) {
    const state = stateAt(line, line.length);
    return state.comment ? line.slice(0, state.at) : line;
  }

  // The triple-quoted string open at the end of `text` (the lines before the caret's line), or null: only such a string
  // runs on past its line.
  function openQuote(text) {
    let quote = null;
    for (const line of text.split("\n")) {
      const state = stateAt(line, line.length, quote);
      quote = state.quote && state.quote.length === 3 ? state.quote : null;
    }
    return quote;
  }

  // Where the string `quote` that `line` starts inside closes: the index after its closing quote, or the line's length.
  function closeOf(line, quote) {
    for (let i = 0; i < line.length; ++i) {
      if (line[i] === "\\") ++i;
      else if (line.startsWith(quote, i)) return i + quote.length;
    }
    return line.length;
  }

  // The indent Return gives the next line: the line's own, one level (`tab`) more when its code ends with a colon or an
  // opening bracket, one level less (not below none) when its statement ends a block (return, pass, raise, break,
  // continue). `quote` is the triple-quoted string open when the line starts: its text up to where it closes isn't code,
  // and a line that ends inside one keeps its indent.
  function pythonNextIndent(lineBeforeCaret, tab, quote = null) {
    const indent = /^\s*/.exec(lineBeforeCaret)[0];
    const end = stateAt(lineBeforeCaret, lineBeforeCaret.length, quote);
    if (end.quote && end.quote.length === 3) return indent;
    const code = codeOf(quote ? lineBeforeCaret.slice(closeOf(lineBeforeCaret, quote)) : lineBeforeCaret).trimEnd();
    if (/[:([{]$/.test(code)) return indent + tab;
    if (/^(return|pass|raise|break|continue)\b/.test(code.trim())) return indent.slice(0, Math.max(0, indent.length - tab.length));
    return indent;
  }

  // What suggestions go at the end of `lineBeforeCaret`: the tools inside the string tools.call( opens, or the API one
  // level below an orca.host path; null in another string, in a comment or anywhere else. `quote` is the triple-quoted
  // string open when the line starts.
  function suggestContext(lineBeforeCaret, quote = null) {
    const tool = /((?:\w+\.)*tools\.call\(\s*)(["'])(\w*)$/.exec(lineBeforeCaret);
    if (tool) {
      const state = stateAt(lineBeforeCaret, tool.index, quote);
      if (state.quote || state.comment) return null;
      return { kind: "tool", from: lineBeforeCaret.length - tool[3].length, typed: tool[3] };
    }
    const api = /\b(orca\.host(?:\.\w+)*)\.(\w*)$/.exec(lineBeforeCaret);
    if (api) {
      const state = stateAt(lineBeforeCaret, api.index, quote);
      if (state.quote || state.comment) return null;
      return { kind: "api", path: api[1], from: lineBeforeCaret.length - api[2].length, typed: api[2] };
    }
    return null;
  }

  // The spans of `traceback` that name a line of the script (File "<scriptFile>", line N), with that line.
  function tracebackLinks(traceback, scriptFile) {
    const links = [];
    const pattern = /File "([^"]*)", line (\d+)/g;
    let m;
    while ((m = pattern.exec(traceback)))
      if (m[1] === scriptFile) links.push({ start: m.index, end: m.index + m[0].length, line: Number(m[2]) });
    return links;
  }

  // The margin after Run all, one state per cell ("ran", "failed", "idle"), from the cells the run reported (0-based
  // indexes, in order) and how it ended. Finished: every cell ran. Raised at a line once the code started: the cells
  // before that line's cell ran, it failed. Raised before the code started (a syntax error, dependencies that didn't
  // install): only a syntax error's cell is marked, failed. Stopped, or raised at no line of the file: the cells before the
  // last one reported ran, that one failed; none without reports.
  function railAfterFile(cells, reportedCells, outcome) {
    const states = cells.map(() => "idle");
    if (outcome.status === "finished") return states.fill("ran");
    if (outcome.status === "raised") {
      const at = outcome.error_line > 0 ? cellAt(cells, outcome.error_line) : null;
      if (!outcome.executed) {
        if (at) states[at.index - 1] = "failed";
        return states;
      }
      if (at) {
        for (let i = 0; i < at.index - 1; ++i) states[i] = "ran";
        states[at.index - 1] = "failed";
        return states;
      }
    } else if (outcome.status !== "cancelled" && outcome.status !== "timed_out") return states;
    if (reportedCells.length) {
      const last = Math.min(reportedCells[reportedCells.length - 1], states.length - 1);
      for (let i = 0; i < last; ++i) states[i] = "ran";
      states[last] = "failed";
    }
    return states;
  }

  // Whether a cell's text has changed since it ran.
  function edited(cellText, ranText) { return cellText !== ranText; }

  // A script's text with the line breaks the editor holds (LF), whatever the file has, so a CRLF file isn't edited as
  // it opens.
  function editorText(text) { return text.replace(/\r\n?/g, "\n"); }

  // The console blocks Clear keeps, in order: the ones the run going still writes to, its own block and, in Run all, the
  // block of the cell it runs now. None when nothing runs.
  function keptByClear(run) { return run ? [run.block, run.cellBlock].filter(Boolean) : []; }

  // How the editor shows cells, from the Cells menu: a style, and whether the cell at the cursor is highlighted.
  const CELL_STYLES = ["margin", "dividers", "bands", "shaded"];

  // The cell view to draw from what the host saved: a missing or unknown style is Dividers, and the highlight is on unless
  // it was turned off.
  function cellView(saved) {
    const style = saved && CELL_STYLES.includes(saved.style) ? saved.style : "dividers";
    return { style, highlight: !saved || saved.highlight !== false };
  }

  // The rows `view` marks behind the text of `cells` (0-based, inclusive, one class each), in the order they are drawn:
  // Shaded cells' shade on every second cell, the highlight on cell `current` (its index, 0 for none), then the style's
  // lines on top, a divider above each cell that doesn't start the file or a band on each cell's "# %%" line.
  function cellMarks(cells, lines, view, current) {
    const marks = [];
    const add = (from, to, cls) => marks.push({ from, to, cls });
    if (view.style === "shaded")
      for (const cell of cells) if (cell.index % 2 === 0) add(cell.start - 1, cell.end - 1, "cell-shade");
    if (view.highlight)
      for (const cell of cells) if (cell.index === current) add(cell.start - 1, cell.end - 1, "cell-current");
    for (const cell of cells) {
      const row = cell.start - 1;
      if (view.style === "dividers" && row > 0) add(row, row, "cell-divider");
      else if (view.style === "bands" && CELL_MARKER.test((lines[row] || "").replace(/\r$/, ""))) add(row, row, "cell-band");
    }
    return marks;
  }

  // ---- a result that is JSON: the host sends it compact, as json.dumps writes it -------------------------------------

  const JSON_TOKEN = /\s+|"(?:[^"\\]|\\.)*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?|true|false|null|[{}[\],:]/y;

  // `text`'s tokens, {type: "key" | "string" | "number" | "literal" | "punct" | "space", text}, each as written, so a
  // number keeps every digit Python wrote; null when something in it is no JSON token.
  function jsonTokens(text) {
    const tokens = [];
    const re = new RegExp(JSON_TOKEN.source, "y");
    for (let at = 0; at < text.length;) {
      re.lastIndex = at;
      const match = re.exec(text);
      if (!match) return null;
      const t = match[0];
      const first = t[0];
      const type = /\s/.test(first) ? "space" : first === '"' ? "string" : first === "-" || (first >= "0" && first <= "9") ? "number"
                 : first >= "a" && first <= "z" ? "literal" : "punct";
      tokens.push({ type, text: t });
      at += t.length;
    }
    // A string before a colon is a key.
    for (let i = 0; i < tokens.length; ++i) {
      if (tokens[i].type !== "string") continue;
      let next = i + 1;
      while (tokens[next] && tokens[next].type === "space") ++next;
      if (tokens[next] && tokens[next].text === ":") tokens[i].type = "key";
    }
    return tokens;
  }

  // How much longer than the host's one-line JSON its layout may be: deep nesting is mostly indentation, which would hold
  // the console far past what the runner's output allowance counted.
  const JSON_MAX_GROWTH = 4;

  // `text` laid out to read: an array or object whose one-line form fits in `width` with its indent stays on one line,
  // any other takes a line per member, indented two spaces a level. Numbers and strings stay as written. Text that isn't
  // one JSON value, or whose layout would be more than JSON_MAX_GROWTH times as long, comes back as it is.
  function formatJson(text, width = 72) {
    const tokens = jsonTokens(text);
    if (!tokens) return text;
    const items = tokens.filter((t) => t.type !== "space");
    let at = 0;
    // A value: a scalar's text, or {open, close, members: [{key, value}]}, with key null in an array.
    const value = () => {
      const t = items[at++];
      if (!t || t.type === "key" || (t.type === "punct" && t.text !== "{" && t.text !== "[")) throw new SyntaxError();
      if (t.type !== "punct") return t.text;
      const node = { open: t.text, close: t.text === "{" ? "}" : "]", members: [] };
      if (items[at] && items[at].text === node.close) return ++at, node;
      for (;;) {
        let key = null;
        if (node.open === "{") {
          key = items[at++];
          if (!key || key.type !== "key" || !items[at] || items[at++].text !== ":") throw new SyntaxError();
          key = key.text;
        }
        node.members.push({ key, value: value() });
        const next = items[at++];
        if (next && next.text === node.close) return node;
        if (!next || next.text !== ",") throw new SyntaxError();
      }
    };
    let root;
    try {
      root = value();
    } catch (e) {
      return text;
    }
    if (at !== items.length) return text;
    const lead = (member) => (member.key === null ? "" : member.key + ": ");
    // The length of a value's one-line form, kept on the node.
    const size = (node) => {
      if (typeof node === "string") return node.length;
      if (node.size === undefined)
        node.size = 2 + node.members.reduce((n, m, i) => n + (i ? 2 : 0) + lead(m).length + size(m.value), 0);
      return node.size;
    };
    const flat = (node) => (typeof node === "string" ? node : node.open + node.members.map((m) => lead(m) + flat(m.value)).join(", ") + node.close);
    const lay = (node, indent, before) => {
      if (typeof node === "string" || indent.length + before.length + size(node) <= width) return flat(node);
      const inner = indent + "  ";
      return node.open + "\n" + node.members.map((m) => inner + lead(m) + lay(m.value, inner, lead(m))).join(",\n") + "\n" + indent + node.close;
    };
    const laid = lay(root, "", "");
    return laid.length > JSON_MAX_GROWTH * text.length ? text : laid;
  }

  // ---- Run all's folded cells and Add a cell --------------------------------------------------------------------------

  // The last line of `text` that has more than spaces, without its line break: what a folded cell shows.
  function lastLine(text) {
    const lines = text.split("\n");
    for (let i = lines.length - 1; i >= 0; --i) {
      const line = lines[i].replace(/\r$/, "");
      if (line.trim()) return line;
    }
    return "";
  }

  // The edit Add a cell makes to the editor's `lines`: a "# %% " line at the end, a blank line before it unless the script
  // is empty, the file still ending with a line break, and the caret after the marker, where the cell's title goes.
  function addCellEdit(lines) {
    const last = lines.length - 1;
    const line = lines[last];
    const before = last > 0 ? lines[last - 1] : "";
    let text;
    if (line === "") text = last === 0 || !before.trim() ? "# %% \n" : "\n# %% \n";
    else text = line.trim() ? "\n\n# %% \n" : "\n# %% \n";
    const breaks = text.indexOf("#");
    return { at: { row: last, column: line.length }, text, caret: { row: last + breaks, column: 5 } };
  }

  // ---- the divider between the editor and the console ----------------------------------------------------------------

  // The editor's share of the panel's width, which the panel keeps as a share so a pane resized keeps its proportions;
  // the smallest widths the divider leaves each side; and how far past one a drag goes to hide that side.
  const SPLIT_DEFAULT = 0.6;
  const MIN_EDITOR = 220;
  const MIN_CONSOLE = 200;
  const HIDE_PAST = 48;
  const HIDDEN_SIDES = ["editor", "console"];

  // The layout the panel kept, {split, hidden}: a split that is not a share strictly between 0 and 1 is the default, and
  // a hidden side that is neither side is none. The host keeps the split as text.
  function paneLayout(saved) {
    const split = saved && saved.split !== null && saved.split !== "" ? Number(saved.split) : NaN;
    return {
      split: split > 0 && split < 1 ? split : SPLIT_DEFAULT,
      hidden: saved && HIDDEN_SIDES.includes(saved.hidden) ? saved.hidden : "",
    };
  }

  // Where the divider can be in a panel `width` px wide, as shares: from the editor's smallest width to the console's.
  // A panel too narrow for both stops the divider where the two smallest widths' shares meet.
  function splitBounds(width) {
    const low = MIN_EDITOR / width, high = (width - MIN_CONSOLE) / width;
    if (low <= high) return { low, high };
    const meet = MIN_EDITOR / (MIN_EDITOR + MIN_CONSOLE);
    return { low: meet, high: meet };
  }

  // `split` within the bounds, to four places: the value the host keeps.
  function clampSplit(split, width) {
    const { low, high } = splitBounds(width);
    return Math.round(Math.min(Math.max(split, low), high) * 10000) / 10000;
  }

  // The divider dragged to `x` px from the panel's left: where it goes, and the side to hide when the drag ends there,
  // which is the one it went more than HIDE_PAST px past the bound of.
  function dragSplit(x, width) {
    const { low, high } = splitBounds(width);
    const hide = x < low * width - HIDE_PAST ? "editor" : x > high * width + HIDE_PAST ? "console" : "";
    return { split: clampSplit(x / width, width), hide };
  }

  // The divider's keys, as a window splitter's: the arrows move it a twentieth of the width, Home and End to a bound.
  // Null for any other key.
  function splitByKey(split, key, width) {
    const to = { ArrowLeft: split - 0.05, ArrowRight: split + 0.05, Home: 0, End: 1 }[key];
    return to === undefined ? null : clampSplit(to, width);
  }

  // The side hidden as the panel draws it, from the side the layout hides. The console's side hides only while it shows
  // the console (`consoleShown`): with no script open it holds the start page, which stays in sight.
  function hiddenSide(hidden, consoleShown) { return hidden === "console" && !consoleShown ? "" : hidden; }

  // What the hidden console's strip shows after `event` (start, output, finished, failed), from what it shows now
  // (nothing, running, fresh, failed): a run going, then that there is output not seen, red from a failed run until the
  // next run starts. Output while a run goes or after one failed changes nothing.
  function consoleNews(news, event) {
    if (event === "start") return "running";
    if (event === "finished") return "fresh";
    if (event === "failed") return "failed";
    return news || "fresh";
  }

  return { splitCells, cellAt, pythonNextIndent, suggestContext, tracebackLinks, railAfterFile, edited, stateAt, editorText,
           openQuote, keptByClear, cellView, cellMarks, jsonTokens, formatJson, lastLine, addCellEdit, paneLayout, clampSplit,
           dragSplit, splitByKey, hiddenSide, consoleNews };
});
