// The shared code editor's forwarded actions, run with `node --test`. Ace isn't loaded: the editor and the page are
// stand-ins with the parts these functions touch.
const { test } = require("node:test");
const assert = require("node:assert/strict");
const { runEditorAction, recordFieldKey, ownKeyRan, isEditorText } = require("./code_editor.js");

function page() {
  const editorText = { tagName: "TEXTAREA" }; // Ace's hidden textarea, inside the editor
  const prompt = { tagName: "INPUT", selected: 0, select() { this.selected++; } };
  const button = { tagName: "BUTTON" };
  // Ace's search box sits inside the editor's container, but its fields are text fields of their own.
  const search = { tagName: "INPUT", selected: 0, select() { this.selected++; } };
  const editor = {
    container: { contains: (el) => el === editorText || el === search },
    textInput: { getElement: () => editorText },
    $orcaOwnAt: {},
    ran: [],
    execCommand(name) { this.ran.push(name); },
  };
  const doc = { activeElement: editorText, ran: [], execCommand(name) { this.ran.push(name); return true; } };
  global.document = doc;
  return { editor, doc, editorText, prompt, button, search };
}

test("a forwarded undo or redo with a text field focused acts on the field, not the script", () => {
  const p = page();
  p.doc.activeElement = p.prompt;
  runEditorAction(p.editor, "undo");
  runEditorAction(p.editor, "redo");
  assert.deepEqual(p.doc.ran, ["undo", "redo"]);
  assert.deepEqual(p.editor.ran, []);
});

test("a forwarded select all with a text field focused selects the field's text", () => {
  const p = page();
  p.doc.activeElement = p.prompt;
  runEditorAction(p.editor, "select_all");
  assert.equal(p.prompt.selected, 1);
  assert.deepEqual(p.editor.ran, []);
});

test("a forwarded action with the editor or a button focused acts on the script", () => {
  const p = page();
  runEditorAction(p.editor, "undo");
  p.doc.activeElement = p.button;
  runEditorAction(p.editor, "select_all");
  assert.deepEqual(p.editor.ran, ["undo", "selectall"]);
  assert.deepEqual(p.doc.ran, []);
});

test("a field's own undo key is recorded, so the same press forwarded by the host does nothing more", () => {
  const p = page();
  p.doc.activeElement = p.prompt;
  recordFieldKey(p.editor, { target: p.prompt, key: "z", ctrlKey: true, metaKey: false, shiftKey: false, altKey: false });
  runEditorAction(p.editor, "undo");
  assert.deepEqual(p.doc.ran, []);
  assert.deepEqual(p.editor.ran, []);
});

test("a key in the editor is left to the editor's own commands", () => {
  const p = page();
  recordFieldKey(p.editor, { target: p.editorText, key: "z", ctrlKey: true, metaKey: false, shiftKey: false, altKey: false });
  assert.deepEqual(p.editor.$orcaOwnAt, {});
});

test("Ace's search field takes the forwarded actions itself, as a field outside the editor does", () => {
  const p = page();
  p.doc.activeElement = p.search;
  runEditorAction(p.editor, "undo");
  runEditorAction(p.editor, "select_all");
  assert.deepEqual(p.doc.ran, ["undo"]);
  assert.equal(p.search.selected, 1);
  assert.deepEqual(p.editor.ran, []);
  assert.equal(isEditorText(p.editor, p.search), false);
  assert.equal(isEditorText(p.editor, p.editorText), true);
});

test("a forwarded Save right after the page's own is the same key press", () => {
  const p = page();
  p.editor.$orcaOwnAt.save = Date.now();
  assert.equal(ownKeyRan(p.editor, "save"), true);
  p.editor.$orcaOwnAt.save = Date.now() - 1000;
  assert.equal(ownKeyRan(p.editor, "save"), false);
  assert.equal(ownKeyRan(p.editor, "saveAs"), false);
});
