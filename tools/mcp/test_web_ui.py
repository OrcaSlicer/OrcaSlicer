#!/usr/bin/env python3
"""Checks the GUI tools on web pages, on the pages of the MCP web fixture plugin.

    tools/mcp/test_web_ui.py --install DATADIR      # side-loads the fixture plugin into DATADIR
    tools/mcp/orca_mcp.py launch --datadir DATADIR
    tools/mcp/test_web_ui.py

The plugin (tools/mcp/tests/plugins/mcp_web_fixture) opens a page with every kind of control as a
modal plugin dialog, as a modeless window and as a dock panel, and has a configuration UI the Plugins
dialog shows in a sandboxed frame. The checks: what ui_snapshot lists of a page; each action through
ui_click, ui_input, ui_key and ui_mouse, with the effect the page logs, the value read back and what the
reply says changed on the page; a credential in a page's URL shown REDACTED; the native permission
prompt the plugin's thread opens, pressed by dialog_respond and by a rule registered while it is open
(not on macOS); the refusals (covered, disabled, file input, a page under the topmost modal); the timeout contract (a
page busy past the deadline never runs the queued click, a slow handler's outcome is unknown);
locators into the shadow root, the frame and the configuration frame, and the journal's locators;
stale refs; closed:true from a page that closes its dialog through a reference to window.wx kept from
before, with another client's snapshots running meanwhile, and from orca.submit; dialog_respond by a
page button and its refusal of apply; an auto-answer rule registered before the dialog opens; the web
budget with the Home page and a modeless page window open; webview_eval's el. Exits 0 when every
check passes.
"""

import glob
import json
import os
import re
import shutil
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "plugins", "mcp_web_fixture")
DIALOG = "plugin_script_action:MCP web fixture dialog:mcp_web_fixture"
WINDOW = "plugin_script_action:MCP web fixture window:mcp_web_fixture"
PANEL = "plugin_script_action:MCP web fixture panel:mcp_web_fixture"
THREAD = "plugin_script_action:MCP web fixture thread:mcp_web_fixture"
CAPABILITIES = ["MCP web fixture dialog", "MCP web fixture window", "MCP web fixture panel", "MCP web fixture thread"]


def install(datadir):
    """Copies the fixture plugin into DATADIR's orca_plugins, enabled, as a person's side-load would."""
    dest = os.path.join(datadir, "orca_plugins", "mcp_web_fixture")
    os.makedirs(dest, exist_ok=True)
    shutil.copy(os.path.join(FIXTURE, "mcp_web_fixture.py"), dest)
    with open(os.path.join(dest, ".install_state.json"), "w") as f:
        json.dump({"installed_from": "local", "installed_version": "1.0.0", "plugin_name": "MCP Web Fixture", "enabled": True,
                   "capabilities": [{name: True} for name in CAPABILITIES]}, f, indent=2)
    print(f"installed the fixture plugin into {dest}")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--install":
        install(sys.argv[2])
        return 0
    client, session = orca_mcp.session_client()
    failed = []

    def check(name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"), flush=True)
        if not ok:
            failed.append(name)

    def call(tool, **args):
        try:
            return client.call(tool, **args)
        except orca_mcp.McpError as e:
            return {"error": str(e)}

    def tree(**args):
        try:
            _, texts = client.call_with_text("ui_snapshot", **args)
        except orca_mcp.McpError:
            return ""
        return texts[0] if texts else ""

    def ref_of(text, needle):
        for line in text.splitlines():
            if needle in line and "[" in line:
                return line.split("[", 1)[1].split("]", 1)[0]
        return None

    def dialogs():
        return [d["title"] for d in client.call("get_state")["dialogs"]]

    def wait_for(predicate, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if predicate():
                return True
            time.sleep(0.2)
        return False

    def page_log(target=None):
        args = {"script": "document.getElementById('log').textContent"}
        if target:
            args["target"] = target
        return call("webview_eval", **args).get("result") or ""

    def python_log():
        """The newest plugin stderr log, where the fixture writes what its dialog submitted."""
        logs = glob.glob(os.path.join(session["datadir"], "log", "python_*.log"))
        if not logs:
            return ""
        with open(max(logs, key=os.path.getmtime), encoding="utf-8", errors="replace") as f:
            return f.read()

    def open_fixture():
        client.call("run_action", id=DIALOG)
        return wait_for(lambda: dialogs() == ["MCP web fixture"]) and wait_for(lambda: 'heading "Fixture"' in tree(root="dialog"))

    client.call("new_project", discard_changes=True)
    call("dialog_auto_answer", rules=[])

    # ---- What a page is listed as ----
    check("the fixture dialog opens", open_fixture(), dialogs())
    t = tree(root="dialog")
    for needle in ['heading "Fixture"', 'text "Name"', 'spin "Count" = "3"', 'choice "Colour" = "Green"', "[options: Red | Green | Blue]",
                   'checkbox "I agree"', 'checkbox "Fancy"', 'radio "Small"', 'slider "Level" = "20"', 'text "Notes" = "editable"',
                   'button "Card one"', 'button "Disabled"', 'row "Alpha"', 'file "Upload"', 'canvas "Pad"',
                   'button "Shadow"', 'button "Inner"', 'link "Jump"', 'Some bold text in one paragraph.']:
        check(f"the snapshot lists {needle}", needle in t, needle)
    check("a table cell's field is named by its row and column headers", 'text "1 / 2" = "91"' in t)
    check("a selected class marks a clickable div checked", re.search(r'button "Opt B".*\[checked\]', t))
    check("an unselected class marks it nothing", re.search(r'button "Opt A"[^\n]*\[(un)?checked\]', t) is None)
    check("nor does a non-selected class", re.search(r'button "Opt C"[^\n]*\[(un)?checked\]', t) is None)
    check("nor an unSelected one", re.search(r'button "Opt D"[^\n]*\[(un)?checked\]', t) is None)
    check("a control scrolled out left of its scroller is listed", 'button "Leftmost"' in t)
    check("a hidden element is not listed", 'button "Hidden"' not in t)
    check("a disabled button says so", re.search(r'button "Disabled".*\[disabled\]', t))
    check("the checked radio says so", re.search(r'radio "Small".*\[checked\]', t))
    if failed:
        print(t)

    # ---- Actions and their effects ----
    call("export_script", clear=True)
    r = call("ui_input", target={"css": "#name"}, value="Ada")
    check("ui_input sets a text field and reads it back", r.get("value") == "Ada", r)
    r = call("ui_input", target={"web": "Colour"}, value="blue")
    check("ui_input picks an option by its text, case aside", r.get("value") == "Blue", r)
    r = call("ui_input", target={"web": "I agree"}, value=True)
    check("ui_input ticks a checkbox", r.get("checked") is True, r)
    r = call("ui_input", target={"web": "Fancy"}, value=True)
    check("ui_input ticks a checkbox drawn as its label", r.get("checked") is True, r)
    call("ui_input", target={"web": "Controlled"}, value="xyz")
    r = call("ui_input", target={"web": "Level"}, value=55)
    check("ui_input sets a range", r.get("value") == "55", r)
    r = call("ui_input", target={"web": "Notes"}, value="new text")
    check("ui_input sets editable text", r.get("value") == "new text", r)
    r = call("ui_input", target={"web": "1 / 2"}, value="333")
    check("ui_input sets a table cell's field by its headers", r.get("value") == "333", r)
    card = ref_of(t, 'button "Card one"')
    r = call("ui_click", target=card)
    check("ui_click clicks a clickable div by ref", r.get("clicked") == "Card one", r)
    steps = call("export_script").get("script", {}).get("steps", [])
    clicks = [s for s in steps if s.get("tool") == "ui_click"]
    recorded = clicks[-1]["args"]["target"] if clicks else steps
    check("the journal records the ref's locator", isinstance(recorded, dict) and recorded.get("within") == "MCP web fixture" and
          ("css" in recorded or "web" in recorded), recorded)
    r = call("ui_click", target=recorded)
    check("and the recorded locator replays", r.get("clicked") == "Card one", r)
    r = call("ui_click", target={"web": "Card two"})
    check("ui_click reaches a handler added in script", r.get("clicked") == "Card two", r)
    r = call("ui_click", target={"web": "Large", "kind": "radio"})
    check("ui_click chooses a radio button", r.get("checked") is True, r)
    call("ui_key", target={"web": "Watched"}, text="ab")
    call("ui_click", target={"web": "First"})
    call("ui_key", target={"web": "First"}, keys="Return")
    r = call("ui_mouse", target={"web": "Pad"}, action="drag", x=5, y=5, to=[60, 30], steps=4)
    check("ui_mouse drags on a canvas", r.get("done") == "drag", r)
    # A gesture split across calls: down's modifiers stay pressed until its up, and a move may leave
    # the element (only a press is hit tested).
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5, modifiers=["shift"])
    r = call("ui_mouse", target={"web": "Pad"}, action="move", x=200, y=20)
    check("a move outside the element is taken", r.get("done") == "move", r)
    call("ui_mouse", target={"web": "Pad"}, action="move", x=30, y=20)
    call("ui_mouse", target={"web": "Pad"}, action="up", x=40, y=20)
    # A click is another gesture, which ends a press a down left, even one the page then refuses.
    ups = page_log().count("pad up")
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5)
    call("ui_click", target={"web": "Card two"})
    check("a click ends a press a down left", page_log().count("pad up") == ups + 1, page_log())
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("ui_click", target={"web": "Disabled"})
    check("so does a click the page refuses", "disabled" in r.get("error", "") and page_log().count("pad up") == ups + 2, [r, page_log()])
    # Left and Right on a selection collapse it to its start and its end.
    caret = []
    for keys in ("Left", "Right"):
        call("webview_eval", script="el.value = 'abcd'; el.focus(); el.setSelectionRange(1, 3); 1", target=ref_of(tree(root="dialog"), 'text "Name"'))
        call("ui_key", target={"css": "#name"}, keys=keys)
        caret.append(call("webview_eval", script="document.getElementById('name').selectionStart").get("result"))
    call("ui_input", target={"css": "#name"}, value="Ada")
    check("Left and Right collapse a selection to its start and end", caret == [1, 3], caret)
    call("ui_mouse", target={"web": "Capture pad"}, action="drag", x=5, y=5, to=[60, 30], steps=4)
    call("webview_eval", script="document.getElementById('scroller').scrollTop = 0; 1")
    r = call("ui_mouse", target={"web": "Scroll me"}, action="wheel", delta=-3)
    top = call("webview_eval", script="document.getElementById('scroller').scrollTop").get("result")
    check("a wheel turn on a scroller scrolls it", isinstance(top, (int, float)) and top > 0, [r, top])
    r = call("ui_click", target={"web": "Deep"})
    check("ui_click scrolls an element out of view into it", r.get("clicked") == "Deep", r)
    r = call("ui_click", target={"web": "Leftmost"})
    check("ui_click scrolls a scroller back to a control left of it", r.get("clicked") == "Leftmost", r)
    r = call("ui_click", target={"web": "Inner"})
    check("ui_click reaches a same-origin frame", r.get("clicked") == "Inner", r)
    r = call("ui_click", target={"css": "#shadowbtn"})
    check("a CSS locator reaches into a shadow root", r.get("clicked") == "Shadow", r)
    log = page_log()
    # The pad logs offsetX/Y, which count from inside its 1 px border; ui_mouse's x/y from its outer edge.
    for line in ["name change Ada", "colour Blue", "agree true", "fancy true", "controlled xyz", "card1", "card2", "keyup sees ab",
                 "go clicked", "f2 submitted by go-value", "pad down 4,4", "pad up 59,29 after 4 moves", "pad shifted 1",
                 "cap samples 5", "deep", "leftmost", "inner", "shadow"]:
        check(f"the page saw {line!r}", line in log, log)

    # ---- Locators ----
    r = call("ui_click", target={"css": ".card"})
    check("a locator with several matches asks for index", "2 page elements match" in r.get("error", ""), r)
    r = call("ui_click", target={"css": ".card", "index": 1})
    check("index picks among the matches", r.get("clicked") == "Card two", r)
    r = call("ui_click", target={"web": "Nope"})
    check("a locator that matches nothing says so", "No page element matches" in r.get("error", ""), r)
    r = call("ui_click", target={"web": "Some bold text in one paragraph."})
    check("a locator reaches a text label, through the block it is in", r.get("kind") == "label", r)
    r = call("ui_click", target={"web": "Deep", "kind": "label"})
    check("text repeating a button's label is reached with kind label", "error" not in r and "deep text" in page_log(), r)

    # ---- Refusals ----
    r = call("ui_click", target={"web": "Under"})
    check("a covered element is refused, naming what covers it", "covered by (span#veil)" in r.get("error", ""), r)
    r = call("ui_click", target={"web": "OK"}, x=500, y=5)
    check("a point outside the element is refused", "outside the element" in r.get("error", ""), r)
    r = call("ui_click", target={"web": "Disabled"})
    check("a disabled element is refused", "disabled" in r.get("error", ""), r)
    r = call("ui_input", target={"web": "Upload"}, value="x")
    check("a file input is refused", "file input" in r.get("error", ""), r)
    fixed = ref_of(tree(root="dialog"), 'text "Fixed"')
    call("ui_key", target=fixed, text="zz")
    r = call("webview_eval", script="el.value", target=fixed)
    check("keys leave a read-only field as it was", r.get("result") == "fixed", r)
    r = call("ui_key", target={"web": "Locked"}, text="zz")
    check("keys to a field in a disabled fieldset are refused", "disabled" in r.get("error", ""), r)
    # Refused clicks and dialog_respond still end a press a down left.
    ups = page_log().count("pad up")
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5)
    view = ref_of(tree(root="dialog", query="webview"), "webview")
    r = call("ui_click", target=view)
    check("ui_click on a web view itself points to its page's elements", "That is a web view" in r.get("error", ""), r)
    check("and ends a press a down left", page_log().count("pad up") == ups + 1, page_log())
    r = call("ui_input", target=view, value="x")
    check("so does ui_input", "That is a web view" in r.get("error", ""), r)
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("dialog_respond", button="Under")
    check("dialog_respond refuses a page button something covers", "covered" in r.get("error", ""), r)
    check("and ends a press a down left too", page_log().count("pad up") == ups + 2, page_log())
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("ui_mouse", target={"web": "Nope"}, action="up")
    check("so does an up whose target can't be found", "No page element matches" in r.get("error", "") and page_log().count("pad up") == ups + 3,
          [r, page_log()])
    # A release names the press it ends, so one for an earlier press leaves the page's newer one held
    # (ids count from 1, and this down is far from the first).
    call("ui_mouse", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("webview_eval", script="JSON.stringify(window.__orcaMcp.call({op: 'release', press: 1}))")
    check("a release naming an earlier press leaves the newer one held", page_log().count("pad up") == ups + 3, [r, page_log()])
    call("ui_click", target={"web": "Card one"})
    check("which the next click releases", page_log().count("pad up") == ups + 4, page_log())
    # A down whose reply never comes may still have pressed, so it is on record and the next click
    # releases it (the slow pad's handler outlasts the 1 s the down waits).
    r = call("ui_mouse", target={"web": "Slow pad"}, action="down", timeout_s=1)
    check("a down the page answers too late has no outcome", "error" in r, r)
    call("ui_click", target={"web": "Card one"})
    check("and the next click releases its press all the same", wait_for(lambda: "slow pad up" in page_log(), 3), page_log())
    r = call("webview_eval", script="window.__orcaMcp.call({op: 'snapshot', expires: Date.now() - 1})")
    check("the page refuses a request past its deadline", isinstance(r.get("result"), dict) and r["result"].get("code") == "expired", r)

    # ---- The timeout contract ----
    r = call("ui_click", target={"web": "Slow"}, timeout_s=1)
    check("a slow handler's outcome is unknown", "outcome is unknown" in r.get("error", ""), r)
    check("and its effect shows later", wait_for(lambda: "slow done" in page_log(), 8))
    # By ref, so the click request itself is what waits behind the busy page (a locator's find would).
    before = page_log().splitlines().count("ok")
    ok = ref_of(tree(root="dialog"), 'button "OK"')
    call("webview_eval", script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 3000) {} }, 0); 1")
    r = call("ui_click", target=ok, timeout_s=1)
    check("a click queued behind a busy page has an unknown outcome", "outcome is unknown" in r.get("error", ""), r)
    time.sleep(3)
    check("and the page refuses it once free", wait_for(lambda: page_log() != "", 3) and page_log().splitlines().count("ok") == before,
          page_log())

    # ---- Changing pages and stale refs ----
    r = call("ui_click", target={"web": "Later"})
    check("a click's reply lists what its handler added to the page", r.get("page", {}).get("changed") is True and
          'button "Made later"' in r["page"].get("added", []), r)
    check("an element a handler adds later shows in the next snapshot", wait_for(lambda: 'button "Made later"' in tree(root="dialog"), 3))
    r = call("ui_click", target={"web": "Nothing"})
    check("a click that changes nothing on its page says so", r.get("page") == {"changed": False}, r)
    r = call("ui_click", target={"web": "Add extra"})
    check("a change the snapshot notes (a select's new option) is a change", r.get("page", {}).get("changed") is True and
          any("options: One | Two" in line for line in r["page"].get("added", [])), r)
    made = ref_of(tree(root="dialog"), 'button "Made later"')
    call("webview_eval", script="document.getElementById('made').remove(); 1")
    r = call("ui_click", target=made)
    check("a ref to a removed element is stale", "gone from the page" in r.get("error", ""), r)
    r = call("ui_click", target={"web": "Jump"})
    check("a link that changes the URL replies navigated", r.get("navigated") is True and r.get("url", "").endswith("#bottom"), r)
    r = call("ui_click", target={"web": "Up"})
    check("a second fragment change replies navigated too", r.get("navigated") is True and r.get("url", "").endswith("#top"), r)
    r = call("webview_eval", script="location.hash = 'evaluated'; 1")
    check("a script's fragment change replies navigated with the new url", r.get("navigated") is True and
          r.get("url", "").endswith("#evaluated"), r)
    r = call("ui_click", target={"web": "Token link"})
    check("a navigated url shows a credential parameter's value as REDACTED", r.get("navigated") is True and
          r.get("url", "").endswith("#orca-token=REDACTED&view=2"), r)
    check("and the snapshot shows no secret either", "fixture-secret" not in tree(root="dialog"), tree(root="dialog", query="webview"))

    name = ref_of(tree(root="dialog"), 'text "Name"')
    r = call("webview_eval", script="el.value", target=name)
    check("webview_eval runs with a page element as el", r.get("result") == "Ada", r)
    r = call("ui_click", target={"css": "#name"})
    check("ui_click focuses a text field", r.get("clicked") == "Name", r)
    r = call("ui_key", text="Z")
    check("keys without a target go to the dialog page's focused element",
          "Z" in (call("webview_eval", script="el.value", target=name).get("result") or ""), r)
    r = call("ui_key", target=ref_of(tree(root="dialog", query="webview"), "webview"), text="Q")
    check("keys to a web view itself go to its page's focused element",
          "ZQ" in (call("webview_eval", script="el.value", target=name).get("result") or ""), r)
    steps = call("export_script").get("script", {}).get("steps", [])
    keyed = [s for s in steps if s.get("tool") == "ui_key"]
    check("and the journal names the web view by its locator", keyed and keyed[-1]["args"].get("target", {}).get("class") == "webview",
          keyed[-1]["args"] if keyed else steps)
    # Typed text in editable content, where the editing command is unavailable: the text still goes in.
    notes = ref_of(tree(root="dialog"), 'text "Notes"')
    call("webview_eval", script="document.__exec = document.execCommand; document.execCommand = function () { return false; }; 1")
    before_notes = call("webview_eval", script="el.textContent", target=notes).get("result") or ""
    call("ui_key", target=notes, text="XY")
    call("ui_key", target=notes, keys="Backspace")
    r = call("webview_eval", script="el.textContent", target=notes)
    # And without Selection.modify (where the caret's text node gives the character).
    call("webview_eval", script="window.__modify = Selection.prototype.modify; delete Selection.prototype.modify; 1")
    call("ui_key", target=notes, keys="Backspace")
    after = call("webview_eval", script="el.textContent", target=notes).get("result")
    call("webview_eval", script="Selection.prototype.modify = window.__modify; document.execCommand = document.__exec; 1")
    check("typing and Backspace in editable content work without the editing command", "X" in (r.get("result") or "") and
          "XY" not in (r.get("result") or ""), r)
    check("and Backspace without Selection.modify too", after == before_notes, [before_notes, after])
    # With the caret at an element boundary between two text nodes, without Selection.modify either.
    boundary = ("var n = el; n.textContent = ''; n.appendChild(document.createTextNode('ab')); "
                "n.appendChild(document.createTextNode('cd')); n.focus(); var r = document.createRange(); r.setStart(n, 1); "
                "r.collapse(true); var s = getSelection(); s.removeAllRanges(); s.addRange(r); 1")
    results = []
    for keys in ("Backspace", "Delete"):
        call("webview_eval", script=boundary, target=notes)
        call("webview_eval", script="document.execCommand = function () { return false; }; "
                                    "window.__modify2 = Selection.prototype.modify; delete Selection.prototype.modify; 1")
        call("ui_key", target=notes, keys=keys)
        results.append(call("webview_eval", script="el.textContent", target=notes).get("result"))
        call("webview_eval", script="Selection.prototype.modify = window.__modify2; document.execCommand = document.__exec; 1")
    check("a delete at an element boundary takes the character beside it", results == ["acd", "abd"], results)
    # Tab follows the composed focus order: the shadow root's button, the host's slotted one, past an
    # anchor with no href, into the frame; and back.
    call("ui_click", target={"css": "#shadowbtn"})
    call("ui_key", keys="Tab")
    check("Tab moves from a shadow root's button to the slotted one", re.search(r'button "Slotted".*\[focused\]', tree(root="dialog")))
    call("ui_key", keys="Tab")
    check("then past an anchor with no href into the frame", re.search(r'button "Inner".*\[focused\]', tree(root="dialog")))
    call("ui_key", keys="Shift+Tab")
    check("Shift+Tab moves back to the slotted button", re.search(r'button "Slotted".*\[focused\]', tree(root="dialog")))
    call("ui_click", target={"web": "Fixed"})
    call("ui_key", keys="Tab")
    check("Tab reaches the control in a disabled fieldset's legend", re.search(r'checkbox "Unlock".*\[focused\]', tree(root="dialog")))
    call("ui_key", keys="Tab")
    check("then skips a control the fieldset disables", re.search(r'spin "Fixed number".*\[focused\]', tree(root="dialog")), tree(root="dialog", query="Fixed"))
    r = call("ui_click", target={"web": "Unlock"})
    check("a control in a disabled fieldset's legend takes a click", r.get("checked") is True, r)
    call("ui_key", target={"web": "Fixed number"}, keys="Up")
    r = call("webview_eval", script="document.getElementById('ron').value")
    check("an arrow leaves a read-only number field as it was", r.get("result") in (5, "5"), r)
    # Without requestSubmit (as on Safari 15), Enter's submission validates the form first.
    call("webview_eval", script="window.__requestSubmit = HTMLFormElement.prototype.requestSubmit; "
                                "delete HTMLFormElement.prototype.requestSubmit; 1")
    call("ui_key", target={"web": "Needed"}, keys="Return")
    invalid_submitted = "f3 submitted" in page_log()
    call("ui_input", target={"web": "Needed"}, value="here")
    call("ui_key", target={"web": "Needed"}, keys="Return")
    call("webview_eval", script="HTMLFormElement.prototype.requestSubmit = window.__requestSubmit; 1")
    check("a form with an empty required field is not submitted", not invalid_submitted, page_log())
    check("and is once it is filled in", "f3 submitted" in page_log(), page_log())
    r = call("ui_key", target={"css": "#name"}, text="\U0001F600")
    typed = call("webview_eval", script="el.value", target=name).get("result") or ""
    check("a character outside the BMP is typed whole", typed.endswith("\U0001F600"), r)
    call("ui_key", target={"css": "#name"}, keys="Backspace")
    check("and Backspace takes it away whole", call("webview_eval", script="el.value", target=name).get("result") == typed[:-1], typed)
    # A caret placed between an emoji's halves still takes the whole emoji, and arrows step over one.
    for keys, kept in (("Backspace", "ab"), ("Delete", "ab")):
        call("webview_eval", script="el.value = 'a\\uD83D\\uDE00b'; el.focus(); el.setSelectionRange(2, 2); 1", target=name)
        call("ui_key", target={"css": "#name"}, keys=keys)
        value = call("webview_eval", script="el.value", target=name).get("result")
        check(f"{keys} with the caret inside an emoji takes it whole", value == kept, value)
    call("webview_eval", script="el.value = 'a\\uD83D\\uDE00b'; el.focus(); el.setSelectionRange(1, 1); 1", target=name)
    call("ui_key", target={"css": "#name"}, keys="Right")
    r = call("webview_eval", script="el.selectionStart", target=name)
    check("Right steps over an emoji", r.get("result") == 3, r)
    call("ui_key", target={"css": "#name"}, keys="Left")
    r = call("webview_eval", script="el.selectionStart", target=name)
    check("and Left back over it", r.get("result") == 1, r)
    call("ui_input", target={"css": "#name"}, value="Ada")
    # A page button that only shows after dialog_respond's 3 s is not pressed late.
    call("ui_click", target={"web": "Slow show"})
    start = time.time()
    r = call("dialog_respond", button="Shown late")
    elapsed = time.time() - start
    check("dialog_respond stops looking for a page button after 3 s", "page has no button" in r.get("error", "") and elapsed < 4.5,
          [r, round(elapsed, 2)])
    time.sleep(1.5)
    check("and the button it gave up on was not pressed once it showed", "shown late clicked" not in page_log())
    r = call("dialog_respond", button="Shown late")
    check("once shown it is pressed", r.get("pressed") is True and "shown late clicked" in page_log(), r)
    t0 = ref_of(tree(root="dialog"), 'button "Card one"')
    call("export_script", clear=True)
    call("screenshot", target=t0, include_image=False, save_path=os.path.join(os.environ.get("TEMP", "/tmp"), "mcp_web_shot.png"), overwrite=True)
    shots = [s for s in call("export_script").get("script", {}).get("steps", []) if s.get("tool") == "screenshot"]
    check("a page element's screenshot is journalled by its web view's locator",
          shots and shots[-1]["args"].get("target") == {"class": "webview", "within": "MCP web fixture"}, shots)

    # ---- dialog_respond ----
    r = call("dialog_respond", button="OK")
    check("dialog_respond presses a page button by label", r.get("web") is True and r.get("pressed") is True, r)
    check("and the page saw the press", wait_for(lambda: page_log().splitlines().count("ok") == before + 1, 3), page_log())
    r = call("dialog_respond", button="apply")
    check("dialog_respond refuses ok-like answers the page has no button for", "page has no button" in r.get("error", ""), r)
    # A page that listed its buttons and then stops answering still gets that refusal.
    call("webview_eval", script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 4000) {} }, 400); 1")
    r = call("dialog_respond", button="Nope")
    check("a page busy after listing its buttons gets the no-button refusal, listing them", "page has no button" in r.get("error", "") and
          "page_buttons" in r.get("error", ""), r)
    time.sleep(2)

    # ---- Closing through a kept window.wx while another client snapshots ----
    other = orca_mcp.OrcaMcp(session["port"], session["token"])
    snapshots = []

    def keep_snapping():
        for _ in range(15):
            try:
                other.call("ui_snapshot")
                snapshots.append(True)
            except orca_mcp.McpError as e:
                snapshots.append(str(e))
            time.sleep(0.05)

    snapper = threading.Thread(target=keep_snapping)
    snapper.start()
    r = call("ui_click", target={"web": "Close through wx"})
    snapper.join()
    check("a page closing its dialog through a kept window.wx replies closed", r.get("closed") is True, r)
    check("snapshots during the close all answered", snapshots == [True] * 15, snapshots)
    check("the dialog is gone", wait_for(lambda: dialogs() == []))

    # ---- A native message box: the host's permission prompt for the plugin's thread ----
    if sys.platform == "darwin":
        print("SKIP native message boxes: wx runs no events while an alert is open on macOS, so only a rule registered "
              "before one opens answers it")
    else:
        def prompt():
            return next((d for d in client.call("get_state")["dialogs"] if d.get("title") == "Plugin permission request"), None)

        r = call("run_action", id=THREAD)
        check("the plugin's thread opens a permission prompt", wait_for(lambda: prompt() is not None, 10), r)
        box = prompt() or {}
        check("it is listed as native with its buttons", box.get("native") is True and
              [b.get("label") for b in box.get("buttons", [])] == ["Yes", "No"], box)
        if r.get("status") == "blocked_by_dialog":
            check("the blocked note says to answer it with dialog_respond", "dialog_respond" in r.get("note", ""), r)
        r = call("dialog_respond", button="No")
        check("dialog_respond presses the open message box's button", r.get("pressed") is True and r.get("closed") is True and
              r.get("dialogs", []) == [], r)
        check("the prompt is gone", wait_for(lambda: prompt() is None, 5))
        # A press held on a page when the second prompt opens ends before the rule presses the box.
        call("run_action", id=WINDOW)
        held_log = {"web": "Card one", "within": "MCP web fixture window"}
        check("the modeless window opens", wait_for(lambda: 'button "Card one"' in tree(query="Card one"), 10))
        ups = page_log(held_log).count("pad up")
        call("ui_mouse", target={"web": "Pad", "within": "MCP web fixture window"}, action="down", x=5, y=5)
        call("run_action", id=THREAD)
        check("the plugin asks again, having been refused", wait_for(lambda: prompt() is not None, 10))
        r = call("dialog_auto_answer", rules=[{"match": "requesting permission to create a thread", "button": "no"}])
        check("a rule registered while a message box is open answers it", r.get("answered_open_dialog", {}).get("button") == "no" and
              any(rule.get("uses") == 1 for rule in r.get("rules", [])), r)
        check("and the prompt is gone again", wait_for(lambda: prompt() is None, 5), dialogs())
        check("and the press held on a page ended first", wait_for(lambda: page_log(held_log).count("pad up") == ups + 1, 3),
              page_log(held_log))
        call("dialog_auto_answer", rules=[])

    # ---- The Home page, a modeless page window and the web budget ----
    client.call("run_action", id="orca_command:go_to_tab:orca", param="home")
    client.call("run_action", id=WINDOW)
    check("the modeless window's page is listed", wait_for(lambda: 'button "Card one"' in tree(query="Card one"), 10))
    native = client.call("ui_snapshot", include_web=False)["nodes"]
    t = tree(max_nodes=native)
    window = t.split('"MCP web fixture window"', 1)
    check("with pages listed, max_nodes still holds every widget", len(window) == 2 and 'button "Card one"' in window[1], t[-1500:])
    check("the Home page is listed too", "homepage" in t and t.count("[d") > 20, t[:1500])
    r = call("ui_click", target={"web": "Card one"})
    check("a modeless page takes input", r.get("clicked") == "Card one", r)

    # ---- A modal over it: the page behind takes no input; closing through orca.submit ----
    check("the fixture dialog opens over the window", open_fixture(), dialogs())
    t = tree()
    check("the pages behind the modal are not read", t.count("behind a modal dialog") >= 2, t[:2000])
    r = call("ui_click", target={"web": "Card one"})
    check("a locator without within looks only in the modal's page", r.get("clicked") == "Card one", r)
    r = call("ui_click", target={"web": "Card one", "within": "MCP web fixture window"})
    check("a page under the topmost modal takes no input", "modal" in r.get("error", "").lower(), r)
    call("ui_input", target={"css": "#name"}, value="Zed")
    r = call("ui_click", target={"web": "Submit"})
    check("orca.submit replies closed", r.get("closed") is True, r)
    check("the plugin got what the page submitted", wait_for(lambda: 'submitted {"colour": "Green", "name": "Zed"}' in python_log(), 5))
    r = call("ui_click", target={"web": "Card two", "within": "MCP web fixture window"})
    check("the modeless window's page takes input again", r.get("clicked") == "Card two", r)

    # ---- An auto-answer registered before the dialog opens ----
    call("dialog_auto_answer", rules=[{"match": "^MCP web fixture$", "button": "Submit"}])
    client.call("run_action", id=DIALOG)
    check("an auto-answer rule answers the web dialog on its page", wait_for(lambda: dialogs() == [], 15), dialogs())
    rules = client.call("get_state").get("auto_answer_rules", [])
    check("and counts its use", any(rule.get("uses", 0) >= 1 for rule in rules), rules)
    call("dialog_auto_answer", rules=[])

    # ---- Closing a web dialog whose page is busy, or that closes before its click's reply ----
    check("the fixture dialog opens to be cancelled", open_fixture(), dialogs())
    call("webview_eval", script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 5000) {} }, 0); 1")
    start = time.time()
    r = call("dialog_respond", button="cancel")
    elapsed = time.time() - start
    check("cancel ends a web dialog whose page is busy, after the 3 s", r.get("closed") is True and elapsed < 4.5, [r, round(elapsed, 2)])
    check("the busy dialog is gone", wait_for(lambda: dialogs() == [], 8), dialogs())
    # The page's own Cancel, whose handler outlasts the 3 s: still running when cancel ends the dialog.
    check("the fixture dialog opens for its stalling Cancel", open_fixture(), dialogs())
    start = time.time()
    r = call("dialog_respond", button="cancel")
    elapsed = time.time() - start
    check("cancel ends the dialog natively while the page's Cancel handler still runs", r.get("closed") is True and elapsed < 4.5,
          [r, round(elapsed, 2)])
    check("the stalled dialog is gone", wait_for(lambda: dialogs() == [], 8), dialogs())
    check("the fixture dialog opens to close at once", open_fixture(), dialogs())
    r = call("ui_click", target={"web": "Close at once"})
    check("a page that closes its dialog before the click's reply replies closed", r.get("closed") is True and "error" not in r, r)
    check("that dialog is gone", wait_for(lambda: dialogs() == []), dialogs())

    # ---- A dock panel, beside the 3D view ----
    client.call("run_action", id="orca_command:go_to_tab:orca", param="prepare")
    client.call("run_action", id=PANEL)
    check("the dock panel's page is listed", wait_for(lambda: 'button "Ping"' in tree(query="Ping"), 10))
    # Its Content-Security-Policy forbids eval: the agent works there, webview_eval says why it can't.
    r = call("ui_click", target={"web": "Ping"})
    check("its page, whose CSP forbids eval, takes a click", r.get("clicked") == "Ping", r)
    ping = ref_of(tree(query="Ping"), 'button "Ping"')
    check("and saw it", wait_for(lambda: 'label "ping"' in tree(query="ping"), 3), tree(query="ping"))
    # Whether the port's script channel is held to the page's CSP varies (WebView2's is not).
    r = call("webview_eval", script="1", target=ping)
    check("webview_eval there runs, or names the page's Content-Security-Policy",
          r.get("result") == 1 or "Content-Security-Policy" in r.get("error", ""), r)
    # A press left on the window's pad ends when a click lands on another page.
    window_log = {"web": "Card one", "within": "MCP web fixture window"}
    ups = page_log(window_log).count("pad up")
    window_pad = {"web": "Pad", "within": "MCP web fixture window"}
    call("ui_mouse", target=window_pad, action="down", x=5, y=5)
    call("ui_click", target={"web": "Ping"})
    check("a click on another page ends a press held on the first", page_log(window_log).count("pad up") == ups + 1, page_log(window_log))
    call("ui_mouse", target=window_pad, action="down", x=5, y=5)
    call("ui_mouse", target={"web": "Ping"}, action="up", x=3, y=3)
    check("so does an up on another page", page_log(window_log).count("pad up") == ups + 2, page_log(window_log))
    call("ui_mouse", target=window_pad, action="down", x=5, y=5)
    r = call("ui_click", target={"label": "Filament", "class": "Label"})
    check("and a click on a widget", page_log(window_log).count("pad up") == ups + 3, [r, page_log(window_log)])
    # A release the page can't take while it is busy is asked again by the next gesture; the one it
    # takes late has passed its deadline and is not run. The page stays busy well past the release's
    # 1 s, as the eval that starts it returns only after its own probe of the page gives up (1 s), and
    # the gestures are widget clicks, which wait on no page.
    filament = {"label": "Filament", "class": "Label"}
    call("ui_mouse", target=window_pad, action="down", x=5, y=5)
    call("webview_eval", target={"web": "Card one", "within": "MCP web fixture window"},
         script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 6000) {} }, 0); 1")
    call("ui_click", target=filament)
    missed = page_log(window_log).count("pad up")  # runs once the page is free, after the late release
    call("ui_click", target=filament)
    check("a release a busy page missed is asked again", missed == ups + 3 and page_log(window_log).count("pad up") == ups + 4,
          [missed, page_log(window_log)])
    # A down on another page meanwhile keeps both presses on record, and the page still busy holds up
    # none of the other releases. (The dock pad by ref: a locator would be refused while the busy page
    # doesn't answer. Its down still waits up to 3 s on the busy page for the journal's index, so the
    # page stays busy for 10 s.)
    def dock_ups():
        return next((line for line in tree(query="dock up").splitlines() if "dock up" in line), "").count("dock up")
    dock = ref_of(tree(query="Dock pad"), 'canvas "Dock pad"')
    docked = dock_ups()
    call("ui_mouse", target=window_pad, action="down", x=5, y=5)
    call("webview_eval", target={"web": "Card one", "within": "MCP web fixture window"},
         script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 10000) {} }, 0); 1")
    r = call("ui_mouse", target=dock, action="down", x=5, y=5)
    call("ui_click", target=filament)
    check("a press is released beside a busy page's, whose release goes unanswered",
          r.get("done") == "down" and dock_ups() == docked + 1, [r, docked, tree(query="dock up")])
    missed = page_log(window_log).count("pad up")
    call("ui_click", target=filament)
    check("and the busy page's press stays on record until it answers",
          missed == ups + 4 and page_log(window_log).count("pad up") == ups + 5, [missed, page_log(window_log)])
    # An up the busy page takes too late is past its deadline and not run, so its press stays on
    # record and the next gesture releases it. (By ref, which is resolved without asking the page.)
    pad_ref = ref_of(tree(query="Pad"), 'canvas "Pad"')
    call("ui_mouse", target=pad_ref, action="down", x=5, y=5)
    call("webview_eval", target={"web": "Card one", "within": "MCP web fixture window"},
         script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 6000) {} }, 0); 1")
    r = call("ui_mouse", target=pad_ref, action="up", x=5, y=5, timeout_s=1)
    missed = page_log(window_log).count("pad up")
    call("ui_click", target=filament)
    check("an up the page takes too late leaves its press for the next gesture",
          "error" in r and missed == ups + 5 and page_log(window_log).count("pad up") == ups + 6, [r, missed, page_log(window_log)])
    # A script's press on a page ends with the script, as a widget's does.
    pressing = {"orca_script": 1, "name": "press", "steps": [
        {"tool": "ui_mouse", "args": {"target": {"web": "Pad", "within": "MCP web fixture window"}, "action": "down", "x": 5, "y": 5}}]}
    res = client.await_job(client.call("run_script", script=pressing, out_dir=os.path.join(session["datadir"], "press_script")))
    check("a script's press on a page ends with the script",
          res.get("passed") is True and wait_for(lambda: page_log(window_log).count("pad up") == ups + 7, 3), [res, page_log(window_log)])
    # Even when the page is busy as the script ends: the release waits for the page.
    pressing["steps"].append({"tool": "webview_eval", "args": {
        "target": {"web": "Card one", "within": "MCP web fixture window"},
        "script": "setTimeout(function () { var t = Date.now(); while (Date.now() - t < 3000) {} }, 0); 1"}})
    res = client.await_job(client.call("run_script", script=pressing, out_dir=os.path.join(session["datadir"], "press_busy_script")))
    check("a script's press ends with the script while its page is busy",
          res.get("passed") is True and wait_for(lambda: page_log(window_log).count("pad up") == ups + 8, 8), [res, page_log(window_log)])
    # An auto-answer rule's press is a click too, so a press held on a page ends first.
    call("ui_mouse", target=window_pad, action="down", x=5, y=5)
    call("dialog_auto_answer", rules=[{"match": "^MCP web fixture$", "button": "Submit"}])
    client.call("run_action", id=DIALOG)
    check("an auto-answer rule's press ends a press held on a page",
          wait_for(lambda: dialogs() == [], 15) and wait_for(lambda: page_log(window_log).count("pad up") == ups + 9, 3), page_log(window_log))
    call("dialog_auto_answer", rules=[])
    # A page that does not answer may hold another match, so a locator is refused until it does.
    def pings():
        line = next((line for line in tree(query="ping").splitlines() if 'label "ping' in line), "")
        return line.split("]", 1)[-1].split("@", 1)[0].strip()  # the label, without the ref the snapshot renumbers
    pinged = pings()
    call("webview_eval", target={"web": "Card one", "within": "MCP web fixture window"},
         script="setTimeout(function () { var t = Date.now(); while (Date.now() - t < 4000) {} }, 0); 1")
    r = call("ui_click", target={"web": "Ping"}, timeout_s=2)
    check("a locator is refused while another candidate page does not answer", "may not be the only match" in r.get("error", ""), r)
    time.sleep(2.5)
    check("and the panel's own page never saw that click", pings() == pinged, [pinged, pings()])
    # So is one while another candidate page fails the search (it broke what the agent reads).
    call("webview_eval", target={"web": "Card one", "within": "MCP web fixture window"},
         script="var s = window.getComputedStyle; window.getComputedStyle = function () { throw new Error('broken'); }; "
                "setTimeout(function () { window.getComputedStyle = s; }, 3000); 1")
    r = call("ui_click", target={"web": "Ping"})
    check("a locator is refused while another candidate page fails the search", "may not be the only match" in r.get("error", ""), r)
    time.sleep(3.5)
    # A page that keeps the agent from installing (its own non-configurable __orcaMcp) can't run it at
    # all, so it holds nothing to reach and the match on the panel is used.
    call("webview_eval", target={"web": "Card one", "within": "MCP web fixture window"},
         script="Object.defineProperty(window, '__orcaMcp', {value: {v: 'other'}, configurable: false, writable: false}); 1")
    r = call("webview_eval", script="1", target={"class": "webview", "within": "MCP web fixture window"})
    check("that page fails to run the agent", "could not run" in r.get("error", ""), r)
    r = call("ui_click", target={"web": "Ping"})
    check("a locator still reaches the match beside a page that can't run the agent", r.get("clicked") == "Ping", r)

    # ---- A plugin's configuration UI, in the Plugins dialog's sandboxed frame ----
    plugins = {"within": "Plugins"}
    speed = int(time.time()) % 89 + 11
    client.call("run_action", id="orca_command:open_plugins:orca")
    check("the Plugins dialog lists the fixture", wait_for(lambda: "MCP Web Fixture" in tree(query="MCP Web Fixture"), 15))
    try:
        client.call("ui_snapshot", root="dialog")
        message = ""
    except orca_mcp.McpError as e:
        message = str(e)
    check("root dialog without a modal names the modeless Plugins dialog", '"Plugins"' in message, message)
    r = call("ui_click", target={"web": "Expand MCP Web Fixture capabilities", **plugins})
    check("the fixture's capabilities expand", r.get("clicked") == "Expand MCP Web Fixture capabilities", r)
    r = call("ui_click", target={"web": "MCP web fixture dialog", "kind": "label", **plugins})
    check("its dialog capability is selected", r.get("clicked") == "MCP web fixture dialog", r)
    r = call("ui_click", target={"web": "Config", "kind": "tab", **plugins})
    check("the Config tab opens", r.get("checked") is True, r)
    check("the configuration frame's controls are listed", wait_for(lambda: 'spin "Speed"' in tree(query="Speed"), 5), tree(query="Config"))
    r = call("ui_input", target={"web": "Speed", **plugins}, value=speed)
    check("ui_input reaches into the frame", r.get("value") == str(speed), r)
    r = call("ui_input", target={"web": "Enabled", **plugins}, value=True)
    check("a checkbox in the frame", r.get("checked") is True, r)
    check("a frame element's id is listed as its CSS selector", re.search(r'button "Save" name=#save', tree(query="Save")), tree(query="Save"))
    save = ref_of(tree(query="Save"), 'button "Save"')
    r = call("ui_click", target=save)
    check("ui_click reaches into the frame by ref", r.get("clicked") == "Save", r)
    check("and its reply lists what the frame's page changed", r.get("page", {}).get("changed") is True and
          any("saved" in line for line in r["page"].get("added", [])), r)
    config = os.path.join(session["datadir"], "orca_plugins", "config.json")

    def saved():
        try:
            with open(config) as f:
                return any(c.get("cap_config") == {"speed": speed, "enabled": True} for c in json.load(f).get("config", []))
        except (OSError, ValueError):
            return False

    check("the plugin's configuration was saved", wait_for(saved, 5))
    # A press in the configuration frame ends with a click on the dialog's page: the frame runs its own
    # agent, which the page's release reaches.
    def frame_ups():
        return next((line for line in tree(query="frame up").splitlines() if "frame up" in line), "").count("frame up")
    before = frame_ups()
    r = call("ui_mouse", target={"css": "#save", **plugins}, action="down")
    call("ui_click", target={"web": "Config", "kind": "tab", **plugins})
    check("a click on the page ends a press held in its configuration frame",
          r.get("done") == "down" and wait_for(lambda: frame_ups() == before + 1, 3), [r, before, tree(query="frame up")])
    for action, name in (("up", "an up"), ("move", "a move")):
        before = frame_ups()
        call("ui_mouse", target={"css": "#save", **plugins}, action="down")
        r = call("ui_mouse", target={"web": "Config", "kind": "tab", **plugins}, action=action)
        check(f"so does {name} on the page", r.get("done") == action and wait_for(lambda: frame_ups() == before + 1, 3),
              [r, before, tree(query="frame up")])
    # A frame too busy to take its release keeps the press on record; the release it takes late is past
    # its deadline and not run, and the next gesture asks again. (Widget clicks: the busy frame holds
    # its page too.)
    stalled = time.time()
    call("ui_click", target={"web": "Stall soon", **plugins})
    before = frame_ups()
    call("ui_mouse", target={"css": "#save", **plugins}, action="down")
    time.sleep(max(0, stalled + 3.5 - time.time()))
    call("ui_click", target={"label": "Filament", "class": "Label"})
    time.sleep(max(0, stalled + 7.5 - time.time()))
    missed = frame_ups()
    call("ui_click", target={"label": "Filament", "class": "Label"})
    check("a release a busy frame missed is asked again", missed == before and wait_for(lambda: frame_ups() == before + 1, 3),
          [before, missed, tree(query="frame up")])
    # Two unnamed Refresh buttons in the frame and the dialog page's own: the journal's index counts
    # them all, as a replay searches the page with its frame.
    refreshes = [line for line in tree(query="Refresh").splitlines() if 'button "Refresh"' in line]
    check("the page's and the frame's Refresh buttons are listed", len(refreshes) == 3, refreshes)
    call("export_script", clear=True)
    second = refreshes[-1].split("[", 1)[1].split("]", 1)[0] if len(refreshes) == 3 else None
    call("ui_click", target=second)
    clicks = [s for s in call("export_script").get("script", {}).get("steps", []) if s.get("tool") == "ui_click"]
    recorded = clicks[-1]["args"]["target"] if clicks else {}
    check("the journal records the frame button's index among every match", recorded.get("index") == 2, recorded)
    call("ui_click", target=recorded)
    frame_log = next((line for line in tree(query="twin").splitlines() if "twin" in line), "")
    check("and replaying it clicks the same button", frame_log.count("twin 2") == 2 and "twin 1" not in frame_log, frame_log)
    r = call("ui_click", target={"web": "Rewrite", **plugins})
    check("the frame rewrites itself", r.get("clicked") == "Rewrite", r)
    r = call("ui_click", target=save)
    check("a frame ref to a control the frame replaced is stale", "gone from the page" in r.get("error", ""), r)
    r = call("ui_click", target={"css": "#save", **plugins})
    check("the same control by locator works", r.get("clicked") == "Save", r)
    save = ref_of(tree(query="Save"), 'button "Save"')
    r = call("ui_click", target={"web": "Reload", **plugins})
    check("a click that reloads the frame says its page could not be compared, not that nothing changed",
          r.get("page", {}).get("partial") is True and "changed" not in r["page"], r)
    check("the frame's page reloads", wait_for(lambda: 'button "Save"' in tree(query="Save"), 5))
    r = call("ui_click", target=save)
    check("a frame ref from before the frame reloaded is stale", "page changed" in r.get("error", ""), r)
    r = call("ui_click", target={"css": "#save", **plugins})
    check("and the control in the new document by locator works", r.get("clicked") == "Save", r)
    # A configuration frame that fails the search, or does not answer, may hold another match, as such a
    # page may; within can't leave a frame of the matching page out, so it is not suggested.
    call("ui_click", target={"web": "Break", **plugins})
    r = call("ui_click", target={"web": "Config", "kind": "tab", **plugins}, timeout_s=3)
    check("a locator is refused while a configuration frame fails the search", "may not be the only match" in r.get("error", "") and
          "add within" not in r.get("error", ""), r)
    time.sleep(3.5)
    # Last in the Plugins dialog: the muted frame answers nothing more, its own click's reply included.
    call("ui_click", target={"web": "Mute", **plugins}, timeout_s=2)
    r = call("ui_click", target={"web": "Config", "kind": "tab", **plugins}, timeout_s=3)
    check("a locator is refused while a configuration frame does not answer", "may not be the only match" in r.get("error", ""), r)
    r = call("ui_click", target={"web": "Config", "kind": "tab", **plugins, "index": 0}, timeout_s=3)
    check("even with an index", "may not be the only match" in r.get("error", ""), r)
    # Each round asks the muted frame again; what it never answered is given up, not kept: the last
    # round's request, and none of those whose caller stopped waiting (the Mute click's).
    for _ in range(3):
        tree(query="Config")
    r = call("webview_eval", script="window.__orcaMcp.call({op: 'doc'}).relaying", target={"class": "webview", "within": "Plugins"})
    check("the page keeps no more than the last round's unanswered request", r.get("result") in (0, 1), r)
    # A script calling the agent itself gives no deadline; the request is given up all the same.
    call("webview_eval", script="window.__orcaMcp.call({op: 'snapshot'}).nodes.length", target={"class": "webview", "within": "Plugins"})
    time.sleep(11.5)
    r = call("webview_eval", script="window.__orcaMcp.call({op: 'doc'}).relaying", target={"class": "webview", "within": "Plugins"})
    check("a request with no deadline is given up after the tools' default", r.get("result") == 0, r)
    # A change on the page itself is still told beside the muted frame, as partial. (By ref: a locator
    # is refused while the frame doesn't answer.)
    call("webview_eval", target={"class": "webview", "within": "Plugins"},
         script="var b = document.createElement('button'); b.textContent = 'Add note'; "
                "b.style.cssText = 'position: fixed; left: 4px; top: 4px; z-index: 99999'; b.onclick = function () { "
                "var p = document.createElement('p'); p.textContent = 'note added'; "
                "p.style.cssText = 'position: fixed; left: 4px; top: 40px; z-index: 99999; background: white'; "
                "document.body.appendChild(p); }; document.body.appendChild(b); 1")
    r = call("ui_click", target=ref_of(tree(query="Add note"), 'button "Add note"'), timeout_s=5)
    check("a page change beside a frame that doesn't answer is listed, as partial",
          r.get("page", {}).get("partial") is True and r["page"].get("changed") is True and 'label "note added"' in r["page"].get("added", []), r)

    # ---- Orca's setup wizard: a ref from before its page navigated is stale ----
    client.call("run_action", id="orca_command:help_setup_wizard:orca")
    check("the setup wizard opens", wait_for(lambda: 'button "Next"' in tree(root="dialog"), 30))
    next_ref = ref_of(tree(root="dialog"), 'button "Next"')
    r = call("ui_click", target=next_ref)
    check("Next navigates to the printer page", r.get("navigated") is True and "/guide/21/" in r.get("url", ""), r)
    r = call("ui_click", target=next_ref)
    check("the ref from the page before is stale", "page changed" in r.get("error", ""), r)
    r = call("webview_eval", script="location.reload(); 1")
    check("a reload at the same URL replies navigated", r.get("navigated") is True, r)
    r = call("dialog_respond", button="close")
    check("dialog_respond close replies closed", r.get("closed") is True, r)
    check("the wizard closes", wait_for(lambda: dialogs() == []), r)

    print(f"{len(failed)} checks failed" if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
