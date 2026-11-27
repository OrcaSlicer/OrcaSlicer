"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const model = require("./agent_activity_model.js");

// An entry as the host sends it, with what a test leaves out filled in.
function entry(fields) {
  return Object.assign({ id: 1, parent: 0, number: 1, kind: "call", source: "agent", source_label: "Claude Code", plugin: "",
                         tool: "app_state", title: "Get app state", what: "", read: false, status: "done", error: "", job: "",
                         started: 1000, ended: 1200, request: {}, response: {}, rule: 0 }, fields);
}

test("the host's changes merge into the list, and a reset starts it over", () => {
  const store = model.createStore();
  model.apply(store, { reset: true, entries: [entry({ id: 1 }), entry({ id: 2, status: "running" })] });
  model.apply(store, { entries: [entry({ id: 2, status: "done" }), entry({ id: 3 })], removed: [1] });
  assert.deepEqual(Array.from(store.keys()).sort(), [2, 3]);
  assert.equal(store.get(2).status, "done");
  model.apply(store, { reset: true, entries: [entry({ id: 9 })] });
  assert.deepEqual(Array.from(store.keys()), [9]);
});

test("a run's steps nest under it in order, and a step whose run is gone stands at the top", () => {
  const store = model.createStore();
  model.apply(store, { entries: [entry({ id: 5, parent: 1 }), entry({ id: 1 }), entry({ id: 3, parent: 1 }), entry({ id: 7, parent: 4 })] });
  const { top, children } = model.tree(store);
  assert.deepEqual(top.map((e) => e.id), [1, 7]);
  assert.deepEqual(children.get(1).map((e) => e.id), [3, 5]);
});

test("Changes leaves out reads, Failures shows failed calls, and a run shows when one of its steps matches", () => {
  const store = model.createStore();
  model.apply(store, { entries: [
    entry({ id: 1, read: true }),
    entry({ id: 2, title: "Slice", what: "Plate 1" }),
    entry({ id: 3, tool: "replay_run", title: "Run a replay" }),
    entry({ id: 4, parent: 3, title: "Export", status: "failed", error: "Disk full" }),
    entry({ id: 5, kind: "dialog", title: "Answered a dialog", read: false }),
  ] });
  const { top, children } = model.tree(store);
  const shown = (filter) => top.filter((e) => model.showsAtTop(e, filter, children)).map((e) => e.id);
  assert.deepEqual(shown({ show: "all" }), [1, 2, 3, 5]);
  assert.deepEqual(shown({ show: "changes" }), [2, 3, 5]);
  assert.deepEqual(shown({ show: "failures" }), [3]);
  assert.deepEqual(shown({ show: "all", find: "plate 1" }), [2]);
  assert.deepEqual(shown({ show: "all", find: "EXPORT" }), [3]);
  // Under a shown run, Everything with nothing to find shows every step; otherwise the ones that match.
  const step = store.get(4);
  assert.equal(model.showsNested(step, { show: "all" }, children), true);
  assert.equal(model.showsNested(step, { show: "changes", find: "slice" }, children), false);
});

test("the source menu lists each source once with its newest label, and a plugin's entries by plugin", () => {
  const store = model.createStore();
  model.apply(store, { entries: [
    entry({ id: 1, source: "agent", source_label: "Claude Code" }),
    entry({ id: 2, source: "plugin", plugin: "brim", source_label: "Brim Ears" }),
    entry({ id: 3, source: "agent", source_label: "Agent" }),
    entry({ id: 4, source: "plugin", plugin: "fill", source_label: "Fill" }),
    entry({ id: 5, parent: 2, source: "plugin", plugin: "brim", source_label: "Brim Ears" }),
  ] });
  const { top, children } = model.tree(store);
  assert.deepEqual(model.sources(top).map((s) => [s.key, s.label, s.count]),
                   [["agent", "Agent", 2], ["plugin:brim", "Brim Ears", 1], ["plugin:fill", "Fill", 1]]);
  assert.deepEqual(top.filter((e) => model.showsAtTop(e, { show: "all", source: "plugin:brim" }, children)).map((e) => e.id), [2]);
});

test("the counts are of top-level calls, without dialogs, and the status names who works", () => {
  const top = [entry({ id: 1 }), entry({ id: 2, status: "failed" }), entry({ id: 3, kind: "dialog" }),
               entry({ id: 4, status: "running", source_label: "Action Replay" })];
  assert.deepEqual(model.counts(top), { calls: 3, failed: 1 });
  assert.deepEqual(model.status(top), { busy: true, text: "Action Replay is working" });
  assert.deepEqual(model.status([entry({ id: 1 })]), { busy: false, text: "Idle" });
  assert.deepEqual(model.status([]), { busy: false, text: "Nothing yet" });
});

test("durations read in tenths, seconds, then minutes", () => {
  assert.equal(model.duration(420), "0.4 s");
  assert.equal(model.duration(12400), "12 s");
  assert.equal(model.duration(125000), "2 min 5 s");
});

test("the response note says how the call answered, or that it still runs", () => {
  const now = 10000;
  assert.equal(model.responseNote(entry({ started: 1000, ended: 1300 }), now), "answered in 0.3 s");
  assert.equal(model.responseNote(entry({ job: "j4", started: 1000, ended: 9000 }), now), "ran as job j4; finished after 8.0 s");
  assert.equal(model.responseNote(entry({ job: "j4", status: "running", ended: 0 }), now), "running as job j4; no result yet");
  assert.equal(model.responseNote(entry({ status: "running", ended: 0 }), now), "waiting for the reply");
  assert.equal(model.responseNote(entry({ status: "failed", started: 1000, ended: 1500 }), now), "failed after 0.5 s");
});

test("the request note names an agent's tools/call, through tool_call where it went, and a step's run", () => {
  const started = new Date(2026, 10, 23, 14, 2, 5).getTime();
  const call = entry({ started, request: { jsonrpc: "2.0", id: 3, method: "tools/call", params: { name: "plate_slice" } } });
  assert.equal(model.requestNote(call), "tools/call from Claude Code at 14:02:05");
  const wrapped = entry({ started, request: { jsonrpc: "2.0", id: 4, method: "tools/call", params: { name: "tool_call", arguments: { tool: "sketch_create" } } } });
  assert.equal(model.requestNote(wrapped), "tools/call from Claude Code through tool_call, at 14:02:05");
  assert.equal(model.requestNote(entry({ parent: 2 }), "Run a replay"), "made by Run a replay");
  assert.equal(model.requestNote(entry({ started, kind: "work", source: "run_script" })), "run in the Run Script panel at 14:02:05");
});

test("a payload the host left out for its size is told apart from one that only looks like it", () => {
  assert.equal(model.truncated({ truncated: true, bytes: 40000 }), true);
  assert.equal(model.truncated({ truncated: true, bytes: 40000, extra: 1 }), false);
  assert.equal(model.truncated({ truncated: "yes", bytes: 1 }), false);
  assert.equal(model.truncated(null), false);
});

test("the filters reach every depth: a replay's script's steps show or hide by their own match", () => {
  const store = model.createStore();
  model.apply(store, { entries: [
    entry({ id: 1, tool: "replay_run", title: "Run a replay" }),
    entry({ id: 2, parent: 1, tool: "script_run", title: "Run a Python script" }),
    entry({ id: 3, parent: 2, read: true, title: "List objects" }),
    entry({ id: 4, parent: 2, title: "Export", status: "failed", error: "Disk full" }),
    entry({ id: 5, read: true, title: "Get app state" }),
  ] });
  const tree = model.tree(store);
  const failures = model.visibility(tree, { show: "failures" });
  assert.deepEqual(Array.from(failures.shown).sort(), [1, 2, 4]);
  assert.deepEqual(Array.from(failures.last).sort(), [1, 2, 4]); // each list's last shown entry
  const all = model.visibility(tree, { show: "all" });
  assert.deepEqual(Array.from(all.shown).sort(), [1, 2, 3, 4, 5]);
  assert.deepEqual(Array.from(all.last).sort(), [2, 4, 5]);
  const changes = model.visibility(tree, { show: "changes" });
  assert.equal(changes.shown.has(3), false);
  assert.equal(changes.shown.has(5), false);
});

test("a withheld response is told by the entry, not by what the response holds", () => {
  assert.equal(model.responseState(entry({ withheld: true, response: null })), "withheld");
  assert.equal(model.responseState(entry({ withheld: false, response: { withheld: true } })), "value");
  assert.equal(model.responseState(entry({ status: "running", response: null })), "pending");
  assert.equal(model.responseState(entry({ response: { ok: true } })), "value");
});

test("a dialog's rule links the call that set it only while the list holds that call", () => {
  const store = model.createStore();
  model.apply(store, { entries: [entry({ id: 4, tool: "dialog_auto_answer" }), entry({ id: 6, kind: "dialog", rule: 4 })] });
  assert.equal(model.ruleSetter(store.get(6), store).id, 4);
  assert.equal(model.ruleSetter(entry({ kind: "dialog", rule: 0 }), store), null);
  model.apply(store, { entries: [], removed: [4] });
  assert.equal(model.ruleSetter(store.get(6), store), null);
});
