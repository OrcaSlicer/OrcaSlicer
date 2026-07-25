// Regression tests for the DOM-free Speed Dial helpers.
// Run: node resources/web/dialog/SpeedDial/speeddial.test.js
const assert = require("assert");
const fs = require("fs");
const vm = require("vm");

const ctx = {};
ctx.window = ctx;
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(__dirname + "/../../js/fuzzy-search.js", "utf8"), ctx);
vm.runInContext(fs.readFileSync(__dirname + "/speeddial.js", "utf8"), ctx);

assert.equal(typeof ctx.parseId, "undefined", "opaque action ids must never be parsed");

const duplicateActions = [
  { id: "0123456789abcdef", title: "Repair", source: "Mesh Tools" },
  { id: "fedcba9876543210", title: "Repair", source: "Mesh Tools" }
];
assert.equal(
  ctx.actionLabel(duplicateActions[0], duplicateActions),
  "Repair from Mesh Tools (0123456789abcdef)",
  "duplicate labels should use the opaque id without interpreting its contents"
);

assert.equal(ctx.shouldRenderActionList(""), false, "an empty search keeps the action list hidden");
assert.equal(ctx.shouldRenderActionList("  "), false, "whitespace-only search keeps the action list hidden");
assert.equal(ctx.shouldRenderActionList("r"), true, "typing starts rendering matching actions");

assert.equal(
  ctx.selectedActionId({ zone: "list", i: 0 }, duplicateActions, [], ""),
  null,
  "Enter with an empty query must not resolve to an action the blank list never showed"
);
assert.equal(
  ctx.selectedActionId({ zone: "list", i: 0 }, duplicateActions, [], "rep"),
  "0123456789abcdef",
  "a typed query resolves the list selection"
);
assert.equal(
  ctx.selectedActionId({ zone: "fav", i: 0 }, duplicateActions, ["fedcba9876543210"], ""),
  "fedcba9876543210",
  "favourites stay runnable with an empty query - the fav bar is always visible"
);

console.log("ok");
