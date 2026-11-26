// The Agent Activity page's pure logic: the host's entries merged into one list and nested by run, what the filters
// show, what the toolbar counts and names, and the times and notes a step shows. Loaded by the page as a script and by
// agent_activity_model.test.js as a module.
(function (root, factory) {
  "use strict";
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.AgentActivityModel = api;
})(typeof globalThis === "object" ? globalThis : this, function () {
  "use strict";

  // The page's T() (i18n.js) where it is loaded; the English with its %s filled in a test.
  function tr(key, fallback, ...args) {
    if (typeof T === "function") return T(key, fallback, ...args);
    let text = fallback;
    args.forEach((arg) => { text = text.replace("%s", () => String(arg)); });
    return text;
  }

  // ---- the list ----------------------------------------------------------------------------------------------------

  // The entries by id. apply() merges a host message {reset, entries, removed}: with reset, what the host sends is all
  // there is.
  function createStore() { return new Map(); }
  function apply(store, msg) {
    if (msg.reset) store.clear();
    for (const entry of msg.entries || []) store.set(entry.id, entry);
    for (const id of msg.removed || []) store.delete(id);
  }

  // The top-level entries oldest first, and the entries nested under each id, oldest first. An entry whose parent is
  // gone stands at the top level.
  function tree(store) {
    const top = [];
    const children = new Map();
    const ids = Array.from(store.keys()).sort((a, b) => a - b);
    for (const id of ids) {
      const entry = store.get(id);
      if (entry.parent && store.has(entry.parent)) {
        if (!children.has(entry.parent)) children.set(entry.parent, []);
        children.get(entry.parent).push(entry);
      } else top.push(entry);
    }
    return { top, children };
  }

  // ---- filters -----------------------------------------------------------------------------------------------------

  // What the source menu sorts by: a plugin's entries by the plugin.
  function sourceKey(entry) { return entry.source === "plugin" ? "plugin:" + (entry.plugin || "") : entry.source || ""; }

  // Whether `entry`, or an entry nested under it, passes Everything / Changes / Failures and Find a call. A dialog a rule
  // answered changed something: a button was pressed.
  function matches(entry, filter, children) {
    const find = (filter.find || "").trim().toLowerCase();
    const self = (filter.show === "all" || !filter.show || (filter.show === "changes" && !entry.read) ||
                  (filter.show === "failures" && entry.status === "failed")) &&
                 (!find || [entry.title, entry.what, entry.tool, entry.source_label].join(" ").toLowerCase().includes(find));
    return self || (children.get(entry.id) || []).some((c) => matches(c, filter, children));
  }

  // Whether a top-level entry shows: its source is the one the menu picked, and it matches.
  function showsAtTop(entry, filter, children) {
    return (!filter.source || filter.source === "all" || sourceKey(entry) === filter.source) && matches(entry, filter, children);
  }

  // Whether an entry nested under a shown one shows: all of them under Everything with nothing to find, else those
  // that match.
  function showsNested(entry, filter, children) {
    return ((filter.show === "all" || !filter.show) && !(filter.find || "").trim()) || matches(entry, filter, children);
  }

  // Which entries show under `filter`, and which shown entry ends its list, where its thread stops: {shown, last}, sets of
  // ids. A top-level entry shows by showsAtTop(), an entry nested under a shown one by showsNested(), at any depth.
  function visibility(tree, filter) {
    const shown = new Set();
    const last = new Set();
    const walk = (entries, top) => {
      let end = null;
      for (const entry of entries) {
        if (!(top ? showsAtTop(entry, filter, tree.children) : showsNested(entry, filter, tree.children))) continue;
        shown.add(entry.id);
        end = entry;
        walk(tree.children.get(entry.id) || [], false);
      }
      if (end) last.add(end.id);
    };
    walk(tree.top, true);
    return { shown, last };
  }

  // ---- the toolbar -------------------------------------------------------------------------------------------------

  // The calls listed at the top level, which the last number counts, and how many of them failed. A dialog is no call.
  function counts(top) {
    const calls = top.filter((e) => e.kind !== "dialog");
    return { calls: calls.length, failed: calls.filter((e) => e.status === "failed").length };
  }

  // The sources of the top-level entries, in the order they first came, with their labels and how many entries each.
  function sources(top) {
    const out = new Map();
    for (const entry of top) {
      const key = sourceKey(entry);
      if (!out.has(key)) out.set(key, { key, source: entry.source, label: entry.source_label || "", count: 0 });
      const item = out.get(key);
      ++item.count;
      // The newest label: an agent's name may have become "Agent" since.
      if (entry.source_label) item.label = entry.source_label;
    }
    return Array.from(out.values());
  }

  // What the status says: who works now (the newest running top-level entry's source), else whether anything ran.
  function status(top) {
    const running = top.filter((e) => e.status === "running");
    if (running.length) return { busy: true, text: tr("working", "%s is working", running[running.length - 1].source_label) };
    return { busy: false, text: top.length ? tr("idle", "Idle") : tr("nothing_yet", "Nothing yet") };
  }

  // ---- a step ------------------------------------------------------------------------------------------------------

  // A duration: tenths of a second under ten seconds, then whole seconds, then minutes and seconds.
  function duration(ms) {
    const s = Math.max(0, ms) / 1000;
    if (s < 60) return tr("seconds", "%s s", s < 10 ? s.toFixed(1) : Math.floor(s));
    return tr("minutes_seconds", "%s min %s s", Math.floor(s / 60), Math.floor(s % 60));
  }

  // The time of day `ms` (since the epoch) was, as hours, minutes and seconds.
  function clock(ms) {
    const d = new Date(ms);
    const two = (n) => String(n).padStart(2, "0");
    return `${two(d.getHours())}:${two(d.getMinutes())}:${two(d.getSeconds())}`;
  }

  function elapsed(entry, now) { return (entry.ended || now) - entry.started; }

  // Calls a person sees run for longer than this ran as a job the agent was told to wait for.
  const LONG_MS = 1500;

  // The note beside Response: how the call answered, or that it still runs.
  function responseNote(entry, now) {
    const took = duration(elapsed(entry, now));
    if (entry.status === "running")
      return entry.job ? tr("running_no_result", "running as job %s; no result yet", entry.job) : tr("waiting_reply", "waiting for the reply");
    if (entry.status === "failed") return tr("failed_after", "failed after %s", took);
    if (entry.job && elapsed(entry, now) > LONG_MS) return tr("ran_as_job", "ran as job %s; finished after %s", entry.job, took);
    return tr("answered_in", "answered in %s", took);
  }

  // Whether an agent's request went through tool_call, the tool for commands the tool list leaves out.
  function viaToolCall(entry) {
    const params = entry.request && entry.request.params;
    return !!params && params.name === "tool_call";
  }

  // The note beside Request: where it came from.
  function requestNote(entry, parentTitle) {
    const at = clock(entry.started);
    if (entry.parent && parentTitle) return tr("made_by", "made by %s", parentTitle);
    if (entry.kind === "work") return tr("work_at", "run in the Run Script panel at %s", at);
    if (entry.source === "agent" && entry.request && entry.request.method === "tools/call")
      return viaToolCall(entry) ? tr("tools_call_via", "tools/call from %s through tool_call, at %s", entry.source_label, at)
                                : tr("tools_call_from", "tools/call from %s at %s", entry.source_label, at);
    return tr("sent_at", "sent at %s", at);
  }

  // What an entry's Response section shows: "withheld" when the host can't show it (it repeats a job's result whose masking
  // record the host no longer has), "pending" while there is none yet, else "value".
  function responseState(entry) {
    if (entry.withheld === true) return "withheld";
    return entry.response === null || entry.response === undefined ? "pending" : "value";
  }

  // The entry of the call that set the rule a dialog's answer came from, or null when the list doesn't hold it.
  function ruleSetter(entry, store) {
    return (entry.rule && store.get(entry.rule)) || null;
  }

  // A request or response the host left out for its size: {truncated: true, bytes}.
  function truncated(value) {
    return !!value && typeof value === "object" && value.truncated === true && typeof value.bytes === "number" && Object.keys(value).length === 2;
  }

  return { tr, createStore, apply, tree, sourceKey, matches, showsAtTop, showsNested, visibility, counts, sources, status, duration, clock,
           elapsed, responseNote, requestNote, viaToolCall, responseState, ruleSetter, truncated };
});
