"use strict";
const { test } = require("node:test");
const assert = require("node:assert/strict");
const editor = require("./replay_editor.js");

// Two tools, as the host's catalog lists them: one taking a plain number, one an enum and a nullable number.
editor.setEditorTools([
  { name: "slice", title: "Slice", args: { plate: { type: "integer" } } },
  { name: "set_view", title: "Set the view", args: { view: { type: "string", enum: ["top", "front"] }, zoom: { type: ["number", "null"] } } },
]);

// What suggestionsAt() finds with the caret at the end of `text`.
function at(text, explicit) { return editor.suggestionsAt(text, text.length, !!explicit); }
function labels(found) { return found.items.map((item) => item.label); }

test("a replay's root keys are suggested in a key's string", () => {
  const found = at('{\n  "');
  assert.equal(found.kind, "key");
  assert.equal(found.from, 5);
  assert.equal(found.to, 5);
  assert.deepEqual(labels(found), ["orca_replay", "name", "vars", "steps"]);
});

test("a step's keys are suggested inside steps[], leaving out the ones it has", () => {
  const found = at('{"steps": [{"tool": "slice", "');
  assert.equal(found.kind, "key");
  assert.deepEqual(labels(found), ["args", "expect", "optional", "await", "save_as", "timeout_s"]);
});

test("a step's tool is suggested from the catalog with its title and arguments", () => {
  const text = '{"steps": [{"tool": "sl';
  const found = at(text);
  assert.equal(found.kind, "value");
  assert.equal(found.from, text.length - 2);
  assert.equal(found.to, text.length);
  assert.deepEqual(found.items, [
    { label: "slice", detail: "Slice", info: "args: plate" },
    { label: "set_view", detail: "Set the view", info: "args: view, zoom" },
  ]);
});

test("a tool's arguments are suggested in its args with their types and allowed values", () => {
  const found = at('{"steps": [{"tool": "set_view", "args": {"');
  assert.equal(found.kind, "key");
  assert.deepEqual(found.items, [
    { label: "view", detail: "string", info: "top | front" },
    { label: "zoom", detail: "number | null", info: "" },
  ]);
});

test("an argument's allowed values are suggested as its value", () => {
  const found = at('{"steps": [{"tool": "set_view", "args": {"view": "');
  assert.equal(found.kind, "value");
  assert.deepEqual(labels(found), ["top", "front"]);
});

test("a placeholder suggests the folders, the vars and the saved jobs", () => {
  const text = '{"vars": {"part": 1}, "steps": [{"tool": "slice", "save_as": "s1"}, {"tool": "set_view", "args": {"view": "${';
  const found = at(text);
  assert.equal(found.kind, "var");
  assert.equal(found.from, text.length);
  assert.equal(found.to, text.length);
  assert.deepEqual(labels(found), ["replay_dir", "out_dir", "vars.part", "jobs.s1"]);
});

test("nothing is suggested outside a string unless asked for, nor in a comment", () => {
  assert.equal(at('{"steps": [{'), null);
  const asked = at('{"steps": [{', true);
  assert.equal(asked.kind, "key");
  assert.equal(asked.quoted, false);
  assert.equal(at('// "'), null);
});

test("an accepted key gets its colon and an empty value with the caret inside", () => {
  const text = '{"';
  const found = at(text);
  const steps = found.items.find((item) => item.label === "steps");
  assert.deepEqual(editor.acceptEdit(text, found, steps), { from: 2, to: 2, insert: 'steps": []', back: 1, reopen: false });
  // A string's value opens the suggestions again inside its quotes.
  const name = found.items.find((item) => item.label === "name");
  assert.deepEqual(editor.acceptEdit(text, found, name), { from: 2, to: 2, insert: 'name": ""', back: 1, reopen: true });
  // A number waits after the colon.
  const version = found.items.find((item) => item.label === "orca_replay");
  assert.deepEqual(editor.acceptEdit(text, found, version), { from: 2, to: 2, insert: 'orca_replay": 1', back: 0, reopen: false });
});

test("a key accepted before an existing colon replaces only the key", () => {
  const text = '{"na": 1}';
  const found = editor.suggestionsAt(text, 4, false);
  const name = found.items.find((item) => item.label === "name");
  assert.deepEqual(editor.acceptEdit(text, found, name), { from: 2, to: 5, insert: 'name"', back: 0, reopen: false });
});

test("a value accepted in a closed string replaces it up to its quote", () => {
  const text = '{"steps": [{"tool": "sl"}]}';
  const caret = text.indexOf('sl"') + 2;
  const found = editor.suggestionsAt(text, caret, false);
  assert.equal(found.from, caret - 2);
  assert.equal(found.to, caret + 1);
  assert.deepEqual(editor.acceptEdit(text, found, { label: "slice" }), { from: caret - 2, to: caret + 1, insert: 'slice"', back: 0, reopen: false });
});

test("an accepted placeholder closes its brace, over one already typed", () => {
  const text = '{"vars": {"part": 1}, "steps": [{"tool": "x", "args": {"p": "${va}"}}]}';
  const caret = text.indexOf("${va") + 4;
  const found = editor.suggestionsAt(text, caret, false);
  assert.equal(found.kind, "var");
  assert.deepEqual(editor.acceptEdit(text, found, { label: "vars.part" }), { from: caret - 2, to: caret + 1, insert: "vars.part}", back: 0, reopen: false });
});

test("Return keeps the indent, adds a level after an opening bracket and splits a pair", () => {
  assert.deepEqual(editor.newlineEdit("{", 1, 1), { from: 1, to: 1, insert: "\n  ", back: 0 });
  assert.deepEqual(editor.newlineEdit("{}", 1, 1), { from: 1, to: 1, insert: "\n  \n", back: 1 });
  assert.deepEqual(editor.newlineEdit("  []", 3, 3), { from: 3, to: 3, insert: "\n    \n  ", back: 3 });
  const line = '  "a": 1,';
  assert.deepEqual(editor.newlineEdit(line, line.length, line.length), { from: line.length, to: line.length, insert: "\n  ", back: 0 });
});
