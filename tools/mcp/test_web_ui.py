#!/usr/bin/env python3
"""Checks the GUI tools on web pages, on the pages of the MCP web fixture plugin.

    tools/mcp/test_web_ui.py --install DATADIR      # side-loads the fixture plugin into DATADIR
    tools/mcp/orca_mcp.py launch --datadir DATADIR
    tools/mcp/test_web_ui.py

The plugin (tools/mcp/tests/plugins/mcp_web_fixture) opens a page with every kind of control as a
modal plugin dialog, as a modeless window and as a dock panel, and has a configuration UI the Plugins
dialog shows in a sandboxed frame. The checks: what list_widgets lists of a page; each action through
click, set_widget_value, press_keys and mouse_gesture, with the effect the page logs, the value read back and what the
reply says changed on the page; a credential in a page's URL shown REDACTED; the permission prompt the
plugin's thread opens, answered by answer_dialog and by a rule registered while it is open; the refusals (covered, disabled, file input, a page under the topmost modal); the timeout contract (a
page busy past the deadline never runs the queued click, a slow handler's outcome is unknown);
locators into the shadow root, the frame and the configuration frame, and the journal's locators;
stale refs; closed:true from a page that closes its dialog through a reference to window.wx kept from
before, with another client's snapshots running meanwhile, and from orca.submit; answer_dialog by a
page button and its refusal of apply; an auto-answer rule registered before the dialog opens; the web
budget with the Home page and a modeless page window open; run_javascript's el. Exits 0 when every
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
            _, texts = client.call_with_text("list_widgets", **args)
        except orca_mcp.McpError:
            return ""
        return texts[0] if texts else ""

    def ref_of(text, needle):
        for line in text.splitlines():
            if needle in line and "[" in line:
                return line.split("[", 1)[1].split("]", 1)[0]
        return None

    def dialogs():
        return [d["title"] for d in client.call("get_app_state")["dialogs"]]

    def wait_for(predicate, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            if predicate():
                return True
            time.sleep(0.2)
        return False

    def page_log(target=None):
        args = {"code": "document.getElementById('log').textContent"}
        if target:
            args["target"] = target
        return call("run_javascript", **args).get("result") or ""

    def hook_code(op, before="", after="", setup="", skip=0, times=1):
        """Page JavaScript that runs code around the page agent's answer to its next `times` requests of `op`, once
        `skip` of them have been let through: `setup` at once, then `before` and `after` each time, with the
        request as `req`. A page here goes busy or changes because it was asked, not on a timer, so what a check
        needs the page to be doing when a request arrives holds however slowly the machine runs."""
        return ("(function () { var a = window.__orcaMcp, call = a.call, seen = 0, left = %d; %s"
                " a.call = function (req) { var hit = req.op === '%s' && seen++ >= %d && left-- > 0; if (hit && !left) a.call = call;"
                " if (hit) { %s } var r = call.apply(this, arguments); if (hit) { %s } return r; }; })(); 1"
                % (times, setup, op, skip, before, after))

    def when_asked(op, target=None, **hook):
        args = {"code": hook_code(op, **hook)}
        if target:
            args["target"] = target
        r = call("run_javascript", **args)
        if "error" in r:
            check(f"the page takes its hook for the next {op}", False, r)

    def stall(op, past=0.5, **args):
        """The page is busy from when the request arrives until `past` seconds past the request's own deadline:
        the call gives up on it, and the agent refuses it as expired once the page is free."""
        when_asked(op, before="while (Date.now() < req.expires + %d) {}" % (past * 1000), **args)

    def settled_tree(timeout=15, **args):
        """A listing every page and configuration frame answered: a frame gets a second through its page's relay,
        which a loaded machine can miss, and a page three, so the listing is asked again until they all have."""
        end = time.time() + timeout
        while True:
            text = tree(**args)
            if (text and "did not answer" not in text) or time.time() >= end:
                return text
            time.sleep(0.2)

    def python_log():
        """The newest plugin stderr log, where the fixture writes what its dialog submitted."""
        logs = glob.glob(os.path.join(session["datadir"], "log", "python_*.log"))
        if not logs:
            return ""
        with open(max(logs, key=os.path.getmtime), encoding="utf-8", errors="replace") as f:
            return f.read()

    def open_fixture():
        client.call("run_action", id=DIALOG)
        return wait_for(lambda: dialogs() == ["MCP web fixture"]) and wait_for(lambda: 'heading "Fixture"' in tree(target="dialog"))

    client.call("new_project", discard_changes=True)
    call("auto_answer_dialogs", rules=[])

    # ---- What a page is listed as ----
    check("the fixture dialog opens", open_fixture(), dialogs())
    t = tree(target="dialog")
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
    call("export_replay", clear=True)
    r = call("set_widget_value", target={"css": "#name"}, value="Ada")
    check("set_widget_value sets a text field and reads it back", r.get("value") == "Ada", r)
    r = call("set_widget_value", target={"web": "Colour"}, value="blue")
    check("set_widget_value picks an option by its text, case aside", r.get("value") == "Blue", r)
    r = call("set_widget_value", target={"web": "I agree"}, value=True)
    check("set_widget_value ticks a checkbox", r.get("checked") is True, r)
    r = call("set_widget_value", target={"web": "Fancy"}, value=True)
    check("set_widget_value ticks a checkbox drawn as its label", r.get("checked") is True, r)
    call("set_widget_value", target={"web": "Controlled"}, value="xyz")
    r = call("set_widget_value", target={"web": "Level"}, value=55)
    check("set_widget_value sets a range", r.get("value") == "55", r)
    r = call("set_widget_value", target={"web": "Notes"}, value="new text")
    check("set_widget_value sets editable text", r.get("value") == "new text", r)
    r = call("set_widget_value", target={"web": "1 / 2"}, value="333")
    check("set_widget_value sets a table cell's field by its headers", r.get("value") == "333", r)
    card = ref_of(t, 'button "Card one"')
    r = call("click", target=card)
    check("click clicks a clickable div by ref", r.get("clicked") == "Card one", r)
    steps = call("export_replay").get("replay", {}).get("steps", [])
    clicks = [s for s in steps if s.get("tool") == "click"]
    recorded = clicks[-1]["args"]["target"] if clicks else steps
    check("the journal records the ref's locator", isinstance(recorded, dict) and recorded.get("within") == "MCP web fixture" and
          ("css" in recorded or "web" in recorded), recorded)
    r = call("click", target=recorded)
    check("and the recorded locator replays", r.get("clicked") == "Card one", r)
    r = call("click", target={"web": "Card two"})
    check("click reaches a handler added in script", r.get("clicked") == "Card two", r)
    r = call("click", target={"web": "Large", "kind": "radio"})
    check("click chooses a radio button", r.get("checked") is True, r)
    call("press_keys", target={"web": "Watched"}, text="ab")
    call("click", target={"web": "First"})
    call("press_keys", target={"web": "First"}, keys="Return")
    r = call("mouse_gesture", target={"web": "Pad"}, action="drag", x=5, y=5, to=[60, 30], steps=4)
    check("mouse_gesture drags on a canvas", r.get("done") == "drag", r)
    # A gesture split across calls: down's modifiers stay pressed until its up, and a move may leave
    # the element (only a press is hit tested).
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5, modifiers=["shift"])
    r = call("mouse_gesture", target={"web": "Pad"}, action="move", x=200, y=20)
    check("a move outside the element is taken", r.get("done") == "move", r)
    call("mouse_gesture", target={"web": "Pad"}, action="move", x=30, y=20)
    call("mouse_gesture", target={"web": "Pad"}, action="up", x=40, y=20)
    # A click is another gesture, which ends a press a down left, even one the page then refuses.
    ups = page_log().count("pad up")
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    call("click", target={"web": "Card two"})
    check("a click ends a press a down left", page_log().count("pad up") == ups + 1, page_log())
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("click", target={"web": "Disabled"})
    check("so does a click the page refuses", "disabled" in r.get("error", "") and page_log().count("pad up") == ups + 2, [r, page_log()])
    # Left and Right on a selection collapse it to its start and its end.
    caret = []
    for keys in ("Left", "Right"):
        call("run_javascript", code="el.value = 'abcd'; el.focus(); el.setSelectionRange(1, 3); 1", target=ref_of(tree(target="dialog"), 'text "Name"'))
        call("press_keys", target={"css": "#name"}, keys=keys)
        caret.append(call("run_javascript", code="document.getElementById('name').selectionStart").get("result"))
    call("set_widget_value", target={"css": "#name"}, value="Ada")
    check("Left and Right collapse a selection to its start and end", caret == [1, 3], caret)
    call("mouse_gesture", target={"web": "Capture pad"}, action="drag", x=5, y=5, to=[60, 30], steps=4)
    call("run_javascript", code="document.getElementById('scroller').scrollTop = 0; 1")
    r = call("mouse_gesture", target={"web": "Scroll me"}, action="wheel", delta=-3)
    top = call("run_javascript", code="document.getElementById('scroller').scrollTop").get("result")
    check("a wheel turn on a scroller scrolls it", isinstance(top, (int, float)) and top > 0, [r, top])
    r = call("click", target={"web": "Deep"})
    check("click scrolls an element out of view into it", r.get("clicked") == "Deep", r)
    r = call("click", target={"web": "Leftmost"})
    check("click scrolls a scroller back to a control left of it", r.get("clicked") == "Leftmost", r)
    r = call("click", target={"web": "Inner"})
    check("click reaches a same-origin frame", r.get("clicked") == "Inner", r)
    r = call("click", target={"css": "#shadowbtn"})
    check("a CSS locator reaches into a shadow root", r.get("clicked") == "Shadow", r)
    log = page_log()
    # The pad logs offsetX/Y, which count from inside its 1 px border; mouse_gesture's x/y from its outer edge.
    for line in ["name change Ada", "colour Blue", "agree true", "fancy true", "controlled xyz", "card1", "card2", "keyup sees ab",
                 "go clicked", "f2 submitted by go-value", "pad down 4,4", "pad up 59,29 after 4 moves", "pad shifted 1",
                 "cap samples 5", "deep", "leftmost", "inner", "shadow"]:
        check(f"the page saw {line!r}", line in log, log)

    # ---- Locators ----
    r = call("click", target={"css": ".card"})
    check("a locator with several matches asks for index", "2 page elements match" in r.get("error", ""), r)
    r = call("click", target={"css": ".card", "index": 1})
    check("index picks among the matches", r.get("clicked") == "Card two", r)
    r = call("click", target={"web": "Nope"})
    check("a locator that matches nothing says so", "No page element matches" in r.get("error", ""), r)
    r = call("click", target={"web": "Some bold text in one paragraph."})
    check("a locator reaches a text label, through the block it is in", r.get("kind") == "label", r)
    r = call("click", target={"web": "Deep", "kind": "label"})
    check("text repeating a button's label is reached with kind label", "error" not in r and "deep text" in page_log(), r)

    # ---- Refusals ----
    r = call("click", target={"web": "Under"})
    check("a covered element is refused, naming what covers it", "covered by (span#veil)" in r.get("error", ""), r)
    r = call("click", target={"web": "OK"}, x=500, y=5)
    check("a point outside the element is refused", "outside the element" in r.get("error", ""), r)
    r = call("click", target={"web": "Disabled"})
    check("a disabled element is refused", "disabled" in r.get("error", ""), r)
    r = call("set_widget_value", target={"web": "Upload"}, value="x")
    check("a file input is refused", "file input" in r.get("error", ""), r)
    fixed = ref_of(tree(target="dialog"), 'text "Fixed"')
    call("press_keys", target=fixed, text="zz")
    r = call("run_javascript", code="el.value", target=fixed)
    check("keys leave a read-only field as it was", r.get("result") == "fixed", r)
    r = call("press_keys", target={"web": "Locked"}, text="zz")
    check("keys to a field in a disabled fieldset are refused", "disabled" in r.get("error", ""), r)
    # Refused clicks and answer_dialog still end a press a down left.
    ups = page_log().count("pad up")
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    view = ref_of(tree(target="dialog", query="webview"), "webview")
    r = call("click", target=view)
    check("click on a web view itself points to its page's elements", "That is a web view" in r.get("error", ""), r)
    check("and ends a press a down left", page_log().count("pad up") == ups + 1, page_log())
    r = call("set_widget_value", target=view, value="x")
    check("so does set_widget_value", "That is a web view" in r.get("error", ""), r)
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("answer_dialog", button="Under")
    check("answer_dialog refuses a page button something covers", "covered" in r.get("error", ""), r)
    check("and ends a press a down left too", page_log().count("pad up") == ups + 2, page_log())
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("mouse_gesture", target={"web": "Nope"}, action="up")
    check("so does an up whose target can't be found", "No page element matches" in r.get("error", "") and page_log().count("pad up") == ups + 3,
          [r, page_log()])
    # A release names the press it ends, so one for an earlier press leaves the page's newer one held
    # (ids count from 1, and this down is far from the first).
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("run_javascript", code="JSON.stringify(window.__orcaMcp.call({op: 'release', press: 1}))")
    check("a release naming an earlier press leaves the newer one held", page_log().count("pad up") == ups + 3, [r, page_log()])
    call("click", target={"web": "Card one"})
    check("which the next click releases", page_log().count("pad up") == ups + 4, page_log())
    # A down whose reply never comes may still have pressed, so it is on record and the next click
    # releases it (the slow pad's handler outlasts the 1 s the down waits).
    r = call("mouse_gesture", target={"web": "Slow pad"}, action="down", timeout_s=1)
    check("a down the page answers too late has no outcome", "error" in r, r)
    call("click", target={"web": "Card one"})
    check("and the next click releases its press all the same", wait_for(lambda: "slow pad up" in page_log(), 3), page_log())
    r = call("run_javascript", code="window.__orcaMcp.call({op: 'snapshot', expires: Date.now() - 1})")
    check("the page refuses a request past its deadline", isinstance(r.get("result"), dict) and r["result"].get("code") == "expired", r)

    # ---- The timeout contract ----
    r = call("click", target={"web": "Slow"}, timeout_s=1)
    check("a slow handler's outcome is unknown", "outcome is unknown" in r.get("error", ""), r)
    check("and its effect shows later", wait_for(lambda: "slow done" in page_log(), 8))
    before = page_log().splitlines().count("ok")
    ok = ref_of(tree(target="dialog"), 'button "OK"')
    stall("act")
    r = call("click", target=ok, timeout_s=1)
    check("a click queued behind a busy page has an unknown outcome", "outcome is unknown" in r.get("error", ""), r)
    check("and the page refuses it once free", wait_for(lambda: page_log() != "", 3) and page_log().splitlines().count("ok") == before,
          page_log())

    # ---- Changing pages and stale refs ----
    r = call("click", target={"web": "Later"})
    check("a click's reply lists what its handler added to the page", r.get("page", {}).get("changed") is True and
          'button "Made later"' in r["page"].get("added", []), r)
    check("an element a handler adds later shows in the next snapshot", wait_for(lambda: 'button "Made later"' in tree(target="dialog"), 3))
    r = call("click", target={"web": "Nothing"})
    check("a click that changes nothing on its page says so", r.get("page") == {"changed": False}, r)
    r = call("click", target={"web": "Add extra"})
    check("a change the snapshot notes (a select's new option) is a change", r.get("page", {}).get("changed") is True and
          any("options: One | Two" in line for line in r["page"].get("added", [])), r)
    made = ref_of(tree(target="dialog"), 'button "Made later"')
    call("run_javascript", code="document.getElementById('made').remove(); 1")
    r = call("click", target=made)
    check("a ref to a removed element is stale", "gone from the page" in r.get("error", ""), r)
    r = call("click", target={"web": "Jump"})
    check("a link that changes the URL replies navigated", r.get("navigated") is True and r.get("url", "").endswith("#bottom"), r)
    r = call("click", target={"web": "Up"})
    check("a second fragment change replies navigated too", r.get("navigated") is True and r.get("url", "").endswith("#top"), r)
    r = call("run_javascript", code="location.hash = 'evaluated'; 1")
    check("a script's fragment change replies navigated with the new url", r.get("navigated") is True and
          r.get("url", "").endswith("#evaluated"), r)
    r = call("click", target={"web": "Token link"})
    check("a navigated url shows a credential parameter's value as REDACTED", r.get("navigated") is True and
          r.get("url", "").endswith("#orca-token=REDACTED&view=2"), r)
    check("and the snapshot shows no secret either", "fixture-secret" not in tree(target="dialog"), tree(target="dialog", query="webview"))

    name = ref_of(tree(target="dialog"), 'text "Name"')
    r = call("run_javascript", code="el.value", target=name)
    check("run_javascript runs with a page element as el", r.get("result") == "Ada", r)
    r = call("click", target={"css": "#name"})
    check("click focuses a text field", r.get("clicked") == "Name", r)
    r = call("press_keys", text="Z")
    check("keys without a target go to the dialog page's focused element",
          "Z" in (call("run_javascript", code="el.value", target=name).get("result") or ""), r)
    r = call("press_keys", target=ref_of(tree(target="dialog", query="webview"), "webview"), text="Q")
    check("keys to a web view itself go to its page's focused element",
          "ZQ" in (call("run_javascript", code="el.value", target=name).get("result") or ""), r)
    steps = call("export_replay").get("replay", {}).get("steps", [])
    keyed = [s for s in steps if s.get("tool") == "press_keys"]
    check("and the journal names the web view by its locator", keyed and keyed[-1]["args"].get("target", {}).get("class") == "webview",
          keyed[-1]["args"] if keyed else steps)
    # Typed text in editable content, where the editing command is unavailable: the text still goes in.
    notes = ref_of(tree(target="dialog"), 'text "Notes"')
    call("run_javascript", code="document.__exec = document.execCommand; document.execCommand = function () { return false; }; 1")
    before_notes = call("run_javascript", code="el.textContent", target=notes).get("result") or ""
    call("press_keys", target=notes, text="XY")
    call("press_keys", target=notes, keys="Backspace")
    r = call("run_javascript", code="el.textContent", target=notes)
    # And without Selection.modify (where the caret's text node gives the character).
    call("run_javascript", code="window.__modify = Selection.prototype.modify; delete Selection.prototype.modify; 1")
    call("press_keys", target=notes, keys="Backspace")
    after = call("run_javascript", code="el.textContent", target=notes).get("result")
    call("run_javascript", code="Selection.prototype.modify = window.__modify; document.execCommand = document.__exec; 1")
    check("typing and Backspace in editable content work without the editing command", "X" in (r.get("result") or "") and
          "XY" not in (r.get("result") or ""), r)
    check("and Backspace without Selection.modify too", after == before_notes, [before_notes, after])
    # With the caret at an element boundary between two text nodes, without Selection.modify either.
    boundary = ("var n = el; n.textContent = ''; n.appendChild(document.createTextNode('ab')); "
                "n.appendChild(document.createTextNode('cd')); n.focus(); var r = document.createRange(); r.setStart(n, 1); "
                "r.collapse(true); var s = getSelection(); s.removeAllRanges(); s.addRange(r); 1")
    results = []
    for keys in ("Backspace", "Delete"):
        call("run_javascript", code=boundary, target=notes)
        call("run_javascript", code="document.execCommand = function () { return false; }; "
                                    "window.__modify2 = Selection.prototype.modify; delete Selection.prototype.modify; 1")
        call("press_keys", target=notes, keys=keys)
        results.append(call("run_javascript", code="el.textContent", target=notes).get("result"))
        call("run_javascript", code="Selection.prototype.modify = window.__modify2; document.execCommand = document.__exec; 1")
    check("a delete at an element boundary takes the character beside it", results == ["acd", "abd"], results)
    # Tab follows the composed focus order: the shadow root's button, the host's slotted one, past an
    # anchor with no href, into the frame; and back.
    call("click", target={"css": "#shadowbtn"})
    call("press_keys", keys="Tab")
    check("Tab moves from a shadow root's button to the slotted one", re.search(r'button "Slotted".*\[focused\]', tree(target="dialog")))
    call("press_keys", keys="Tab")
    check("then past an anchor with no href into the frame", re.search(r'button "Inner".*\[focused\]', tree(target="dialog")))
    call("press_keys", keys="Shift+Tab")
    check("Shift+Tab moves back to the slotted button", re.search(r'button "Slotted".*\[focused\]', tree(target="dialog")))
    call("click", target={"web": "Fixed"})
    call("press_keys", keys="Tab")
    check("Tab reaches the control in a disabled fieldset's legend", re.search(r'checkbox "Unlock".*\[focused\]', tree(target="dialog")))
    call("press_keys", keys="Tab")
    check("then skips a control the fieldset disables", re.search(r'spin "Fixed number".*\[focused\]', tree(target="dialog")), tree(target="dialog", query="Fixed"))
    r = call("click", target={"web": "Unlock"})
    check("a control in a disabled fieldset's legend takes a click", r.get("checked") is True, r)
    call("press_keys", target={"web": "Fixed number"}, keys="Up")
    r = call("run_javascript", code="document.getElementById('ron').value")
    check("an arrow leaves a read-only number field as it was", r.get("result") in (5, "5"), r)
    # Without requestSubmit (as on Safari 15), Enter's submission validates the form first.
    call("run_javascript", code="window.__requestSubmit = HTMLFormElement.prototype.requestSubmit; "
                                "delete HTMLFormElement.prototype.requestSubmit; 1")
    call("press_keys", target={"web": "Needed"}, keys="Return")
    invalid_submitted = "f3 submitted" in page_log()
    call("set_widget_value", target={"web": "Needed"}, value="here")
    call("press_keys", target={"web": "Needed"}, keys="Return")
    call("run_javascript", code="HTMLFormElement.prototype.requestSubmit = window.__requestSubmit; 1")
    check("a form with an empty required field is not submitted", not invalid_submitted, page_log())
    check("and is once it is filled in", "f3 submitted" in page_log(), page_log())
    r = call("press_keys", target={"css": "#name"}, text="\U0001F600")
    typed = call("run_javascript", code="el.value", target=name).get("result") or ""
    check("a character outside the BMP is typed whole", typed.endswith("\U0001F600"), r)
    call("press_keys", target={"css": "#name"}, keys="Backspace")
    check("and Backspace takes it away whole", call("run_javascript", code="el.value", target=name).get("result") == typed[:-1], typed)
    # A caret placed between an emoji's halves still takes the whole emoji, and arrows step over one.
    for keys, kept in (("Backspace", "ab"), ("Delete", "ab")):
        call("run_javascript", code="el.value = 'a\\uD83D\\uDE00b'; el.focus(); el.setSelectionRange(2, 2); 1", target=name)
        call("press_keys", target={"css": "#name"}, keys=keys)
        value = call("run_javascript", code="el.value", target=name).get("result")
        check(f"{keys} with the caret inside an emoji takes it whole", value == kept, value)
    call("run_javascript", code="el.value = 'a\\uD83D\\uDE00b'; el.focus(); el.setSelectionRange(1, 1); 1", target=name)
    call("press_keys", target={"css": "#name"}, keys="Right")
    r = call("run_javascript", code="el.selectionStart", target=name)
    check("Right steps over an emoji", r.get("result") == 3, r)
    call("press_keys", target={"css": "#name"}, keys="Left")
    r = call("run_javascript", code="el.selectionStart", target=name)
    check("and Left back over it", r.get("result") == 1, r)
    call("set_widget_value", target={"css": "#name"}, value="Ada")
    # A page button that only shows after answer_dialog's 3 s is not pressed late: the page shows it half a
    # second past the deadline of the first press it is asked for.
    when_asked("press", after="setTimeout(function () { var b = document.createElement('button'); b.textContent = 'Shown late'; "
                              "b.onclick = function () { log('shown late clicked'); }; document.body.appendChild(b); }, "
                              "req.expires + 500 - Date.now());")
    start = time.time()
    r = call("answer_dialog", button="Shown late")
    elapsed = time.time() - start
    # Its last ask has expired by the time the button shows; 8 s leaves a loaded machine room past the 3 s.
    check("answer_dialog stops looking for a page button after 3 s", "page has no button" in r.get("error", "") and elapsed < 8,
          [r, round(elapsed, 2)])
    check("and the button it gave up on was not pressed once it showed",
          wait_for(lambda: 'button "Shown late"' in tree(target="dialog"), 10) and "shown late clicked" not in page_log())
    r = call("answer_dialog", button="Shown late")
    check("once shown it is pressed", r.get("pressed") is True and "shown late clicked" in page_log(), r)
    t0 = ref_of(tree(target="dialog"), 'button "Card one"')
    call("export_replay", clear=True)
    call("screenshot", target=t0, include_image=False, save_path=os.path.join(os.environ.get("TEMP", "/tmp"), "mcp_web_shot.png"), overwrite=True)
    shots = [s for s in call("export_replay").get("replay", {}).get("steps", []) if s.get("tool") == "screenshot"]
    check("a page element's screenshot is journalled by its web view's locator",
          shots and shots[-1]["args"].get("target") == {"class": "webview", "within": "MCP web fixture"}, shots)

    # ---- answer_dialog ----
    r = call("answer_dialog", button="OK")
    check("answer_dialog presses a page button by label", r.get("web") is True and r.get("pressed") is True, r)
    check("and the page saw the press", wait_for(lambda: page_log().splitlines().count("ok") == before + 1, 3), page_log())
    r = call("answer_dialog", button="apply")
    check("answer_dialog refuses ok-like answers the page has no button for", "page has no button" in r.get("error", ""), r)
    # A page that listed its buttons and then stops answering still gets that refusal: it answers the first
    # ask and stalls on the next.
    stall("press", skip=1)
    r = call("answer_dialog", button="Nope")
    check("a page busy after listing its buttons gets the no-button refusal, listing them", "page has no button" in r.get("error", "") and
          "page_buttons" in r.get("error", ""), r)
    time.sleep(2)  # outlasts the stall, which ends half a second after the reply

    # ---- Closing through a kept window.wx while another client snapshots ----
    other = orca_mcp.OrcaMcp(session["port"], session["token"])
    snapshots = []

    def keep_snapping():
        for _ in range(15):
            try:
                other.call("list_widgets")
                snapshots.append(True)
            except orca_mcp.McpError as e:
                snapshots.append(str(e))
            time.sleep(0.05)

    snapper = threading.Thread(target=keep_snapping)
    snapper.start()
    r = call("click", target={"web": "Close through wx"})
    snapper.join()
    check("a page closing its dialog through a kept window.wx replies closed", r.get("closed") is True, r)
    check("snapshots during the close all answered", snapshots == [True] * 15, snapshots)
    check("the dialog is gone", wait_for(lambda: dialogs() == []))

    # ---- The host's permission prompt for the plugin's thread ----
    def prompt():
        return next((d for d in client.call("get_app_state")["dialogs"] if d.get("title") == "Plugin permission request"), None)

    r = call("run_action", id=THREAD)
    check("the plugin's thread opens a permission prompt", wait_for(lambda: prompt() is not None, 10), r)
    box = prompt() or {}
    check("it is listed with its buttons", [b.get("label") for b in box.get("buttons", [])] == ["Yes", "No"], box)
    if r.get("status") == "blocked_by_dialog":
        check("the blocked note says to answer it with answer_dialog", "answer_dialog" in r.get("note", ""), r)
    r = call("answer_dialog", button="No")
    check("answer_dialog presses the prompt's button", r.get("pressed") is True and r.get("closed") is True and
          r.get("dialogs", []) == [], r)
    check("the prompt is gone", wait_for(lambda: prompt() is None, 5))
    # A press held on a page when the second prompt opens ends before the rule answers the prompt.
    call("run_action", id=WINDOW)
    held_log = {"web": "Card one", "within": "MCP web fixture window"}
    check("the modeless window opens", wait_for(lambda: 'button "Card one"' in tree(query="Card one"), 10))
    ups = page_log(held_log).count("pad up")
    call("mouse_gesture", target={"web": "Pad", "within": "MCP web fixture window"}, action="down", x=5, y=5)
    call("run_action", id=THREAD)
    check("the plugin asks again, having been refused", wait_for(lambda: prompt() is not None, 10))
    r = call("auto_answer_dialogs", rules=[{"match": "requesting permission to create a thread", "button": "no"}])
    check("a rule registered while the prompt is open answers it", r.get("answered_open_dialog", {}).get("button") == "no" and
          any(rule.get("uses") == 1 for rule in r.get("rules", [])), r)
    check("and the prompt is gone again", wait_for(lambda: prompt() is None, 5), dialogs())
    check("and the press held on a page ended first", wait_for(lambda: page_log(held_log).count("pad up") == ups + 1, 3),
          page_log(held_log))
    call("auto_answer_dialogs", rules=[])

    # ---- The Home page, a modeless page window and the web budget ----
    client.call("run_action", id="orca_command:go_to_tab:orca", args={"tab": "home"})
    client.call("run_action", id=WINDOW)
    check("the modeless window's page is listed", wait_for(lambda: 'button "Card one"' in tree(query="Card one"), 10))
    native = client.call("list_widgets", include_web=False)["nodes"]
    t = settled_tree(max_nodes=native)
    window = t.split('"MCP web fixture window"', 1)
    check("with pages listed, max_nodes still holds every widget", len(window) == 2 and 'button "Card one"' in window[1], t[-1500:])
    check("the Home page is listed too", "homepage" in t and t.count("[d") > 20, t[:1500])
    r = call("click", target={"web": "Card one"})
    check("a modeless page takes input", r.get("clicked") == "Card one", r)

    # ---- A modal over it: the page behind takes no input; closing through orca.submit ----
    check("the fixture dialog opens over the window", open_fixture(), dialogs())
    t = tree()
    check("the pages behind the modal are not read", t.count("behind a modal dialog") >= 2, t[:2000])
    r = call("click", target={"web": "Card one"})
    check("a locator without within looks only in the modal's page", r.get("clicked") == "Card one", r)
    r = call("click", target={"web": "Card one", "within": "MCP web fixture window"})
    check("a page under the topmost modal takes no input", "modal" in r.get("error", "").lower(), r)
    call("set_widget_value", target={"css": "#name"}, value="Zed")
    r = call("click", target={"web": "Submit"})
    check("orca.submit replies closed", r.get("closed") is True, r)
    check("the plugin got what the page submitted", wait_for(lambda: 'submitted {"colour": "Green", "name": "Zed"}' in python_log(), 5))
    r = call("click", target={"web": "Card two", "within": "MCP web fixture window"})
    check("the modeless window's page takes input again", r.get("clicked") == "Card two", r)

    # ---- An auto-answer registered before the dialog opens ----
    call("auto_answer_dialogs", rules=[{"match": "^MCP web fixture$", "button": "Submit"}])
    client.call("run_action", id=DIALOG)
    check("an auto-answer rule answers the web dialog on its page", wait_for(lambda: dialogs() == [], 15), dialogs())
    rules = client.call("get_app_state").get("auto_answer_rules", [])
    check("and counts its use", any(rule.get("uses", 0) >= 1 for rule in rules), rules)
    call("auto_answer_dialogs", rules=[])

    # ---- Closing a web dialog whose page is busy, or that closes before its click's reply ----
    # The page stays busy to 10 s and its Cancel handler runs as long, so ending before 8 s, which leaves a
    # loaded machine room past the 3 s, shows the call did not wait on either.
    check("the fixture dialog opens to be cancelled", open_fixture(), dialogs())
    stall("press", past=7)
    start = time.time()
    r = call("answer_dialog", button="cancel")
    elapsed = time.time() - start
    check("cancel ends a web dialog whose page is busy, after the 3 s", r.get("closed") is True and elapsed < 8, [r, round(elapsed, 2)])
    check("the busy dialog is gone", wait_for(lambda: dialogs() == [], 8), dialogs())
    # The page's own Cancel, whose handler outlasts the 3 s: still running when cancel ends the dialog.
    check("the fixture dialog opens for its stalling Cancel", open_fixture(), dialogs())
    start = time.time()
    r = call("answer_dialog", button="cancel")
    elapsed = time.time() - start
    check("cancel ends the dialog natively while the page's Cancel handler still runs", r.get("closed") is True and elapsed < 8,
          [r, round(elapsed, 2)])
    check("the stalled dialog is gone", wait_for(lambda: dialogs() == [], 8), dialogs())
    check("the fixture dialog opens to close at once", open_fixture(), dialogs())
    r = call("click", target={"web": "Close at once"})
    check("a page that closes its dialog before the click's reply replies closed", r.get("closed") is True and "error" not in r, r)
    check("that dialog is gone", wait_for(lambda: dialogs() == []), dialogs())

    # ---- A dock panel, beside the 3D view ----
    client.call("run_action", id="orca_command:go_to_tab:orca", args={"tab": "prepare"})
    client.call("run_action", id=PANEL)
    check("the dock panel's page is listed", wait_for(lambda: 'button "Ping"' in tree(query="Ping"), 10))
    # Its Content-Security-Policy forbids eval: the agent works there, run_javascript says why it can't.
    r = call("click", target={"web": "Ping"})
    check("its page, whose CSP forbids eval, takes a click", r.get("clicked") == "Ping", r)
    ping = ref_of(tree(query="Ping"), 'button "Ping"')
    check("and saw it", wait_for(lambda: 'label "ping"' in tree(query="ping"), 3), tree(query="ping"))
    # Whether the port's script channel is held to the page's CSP varies (WebView2's is not).
    r = call("run_javascript", code="1", target=ping)
    check("run_javascript there runs, or names the page's Content-Security-Policy",
          r.get("result") == 1 or "Content-Security-Policy" in r.get("error", ""), r)
    # A press left on the window's pad ends when a click lands on another page.
    window_log = {"web": "Card one", "within": "MCP web fixture window"}
    ups = page_log(window_log).count("pad up")
    window_pad = {"web": "Pad", "within": "MCP web fixture window"}
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    call("click", target={"web": "Ping"})
    check("a click on another page ends a press held on the first", page_log(window_log).count("pad up") == ups + 1, page_log(window_log))
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    call("mouse_gesture", target={"web": "Ping"}, action="up", x=3, y=3)
    check("so does an up on another page", page_log(window_log).count("pad up") == ups + 2, page_log(window_log))
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    r = call("click", target={"label": "Filament", "class": "Label"})
    check("and a click on a widget", page_log(window_log).count("pad up") == ups + 3, [r, page_log(window_log)])
    # A release the page can't take while it is busy is asked again by the next gesture; the one it
    # takes late has passed its deadline and is not run. The gestures are widget clicks, which wait on no
    # page.
    filament = {"label": "Filament", "class": "Label"}
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    stall("release", target=window_log)
    call("click", target=filament)
    missed = page_log(window_log).count("pad up")  # runs once the page is free, after the late release
    call("click", target=filament)
    check("a release a busy page missed is asked again", missed == ups + 3 and page_log(window_log).count("pad up") == ups + 4,
          [missed, page_log(window_log)])
    # A down on another page meanwhile keeps both presses on record, and the busy page holds up none of
    # the other releases: it stalls on the releases the dock pad's down and the widget click after it ask
    # for. (The dock pad by ref, which is resolved without asking the busy page.)
    def dock_ups():
        return next((line for line in tree(query="dock up").splitlines() if "dock up" in line), "").count("dock up")
    dock = ref_of(tree(query="Dock pad"), 'canvas "Dock pad"')
    docked = dock_ups()
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    stall("release", target=window_log, times=2)
    r = call("mouse_gesture", target=dock, action="down", x=5, y=5)
    call("click", target=filament)
    check("a press is released beside a busy page's, whose release goes unanswered",
          r.get("done") == "down" and dock_ups() == docked + 1, [r, docked, tree(query="dock up")])
    missed = page_log(window_log).count("pad up")
    call("click", target=filament)
    check("and the busy page's press stays on record until it answers",
          missed == ups + 4 and page_log(window_log).count("pad up") == ups + 5, [missed, page_log(window_log)])
    # An up the busy page takes too late is past its deadline and not run, so its press stays on
    # record and the next gesture releases it. (By ref, which is resolved without asking the page.)
    pad_ref = ref_of(tree(query="Pad"), 'canvas "Pad"')
    call("mouse_gesture", target=pad_ref, action="down", x=5, y=5)
    stall("act", target=window_log)
    r = call("mouse_gesture", target=pad_ref, action="up", x=5, y=5, timeout_s=1)
    missed = page_log(window_log).count("pad up")
    call("click", target=filament)
    check("an up the page takes too late leaves its press for the next gesture",
          "error" in r and missed == ups + 5 and page_log(window_log).count("pad up") == ups + 6, [r, missed, page_log(window_log)])
    # A replay's press on a page ends with the replay, as a widget's does.
    pressing = {"orca_replay": 1, "name": "press", "steps": [
        {"tool": "mouse_gesture", "args": {"target": {"web": "Pad", "within": "MCP web fixture window"}, "action": "down", "x": 5, "y": 5}}]}
    res = client.await_job(client.call("run_replay", replay=pressing, out_dir=os.path.join(session["datadir"], "press_replay")))
    check("a replay's press on a page ends with the replay",
          res.get("passed") is True and wait_for(lambda: page_log(window_log).count("pad up") == ups + 7, 3), [res, page_log(window_log)])
    # Even when the page is busy as the replay ends: the release, which has no deadline, waits for the page,
    # busy for 3 s from when the release reaches it.
    pressing["steps"].append({"tool": "run_javascript", "args": {
        "target": {"web": "Card one", "within": "MCP web fixture window"},
        "code": hook_code("release", before="var t = Date.now(); while (Date.now() - t < 3000) {}")}})
    res = client.await_job(client.call("run_replay", replay=pressing, out_dir=os.path.join(session["datadir"], "press_busy_replay")))
    check("a replay's press ends with the replay while its page is busy",
          res.get("passed") is True and wait_for(lambda: page_log(window_log).count("pad up") == ups + 8, 8), [res, page_log(window_log)])
    # An auto-answer rule's press is a click too, so a press held on a page ends first.
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    call("auto_answer_dialogs", rules=[{"match": "^MCP web fixture$", "button": "Submit"}])
    client.call("run_action", id=DIALOG)
    check("an auto-answer rule's press ends a press held on a page",
          wait_for(lambda: dialogs() == [], 15) and wait_for(lambda: page_log(window_log).count("pad up") == ups + 9, 3), page_log(window_log))
    call("auto_answer_dialogs", rules=[])
    # A page that does not answer may hold another match, so a locator is refused until it does.
    def pings():
        """The panel's ping log, which holds a ping by now: asked again while the panel's page has not answered,
        as a listing with a query leaves such a page out."""
        end = time.time() + 15
        while True:
            line = next((line for line in tree(query="ping").splitlines() if 'label "ping' in line), "")
            if line or time.time() >= end:
                return line.split("]", 1)[-1].split("@", 1)[0].strip()  # the label, without the ref the snapshot renumbers
            time.sleep(0.2)
    pinged = pings()
    # The panel's page has the call's 5 s to answer, and the window's stalls past them.
    stall("find", target=window_log)
    r = call("click", target={"web": "Ping"}, timeout_s=5)
    check("a locator is refused while another candidate page does not answer", "may not be the only match" in r.get("error", ""), r)
    check("and the panel's own page never saw that click", pings() == pinged, [pinged, pings()])
    # So is one while another candidate page fails the search: it breaks what the agent reads until that
    # search has been answered.
    when_asked("find", target=window_log, after="window.getComputedStyle = styles;",
               setup="var styles = window.getComputedStyle; window.getComputedStyle = function () { throw new Error('broken'); };")
    r = call("click", target={"web": "Ping"})
    check("a locator is refused while another candidate page fails the search", "may not be the only match" in r.get("error", ""), r)
    # A page that keeps the agent from installing (its own non-configurable __orcaMcp) can't run it at
    # all, so it holds nothing to reach and the match on the panel is used.
    call("run_javascript", target={"web": "Card one", "within": "MCP web fixture window"},
         code="Object.defineProperty(window, '__orcaMcp', {value: {v: 'other'}, configurable: false, writable: false}); 1")
    r = call("run_javascript", code="1", target={"class": "webview", "within": "MCP web fixture window"})
    check("that page fails to run the agent", "could not run" in r.get("error", ""), r)
    r = call("click", target={"web": "Ping"})
    check("a locator still reaches the match beside a page that can't run the agent", r.get("clicked") == "Ping", r)

    # ---- A plugin's configuration UI, in the Plugins dialog's sandboxed frame ----
    plugins = {"within": "Plugins"}
    speed = int(time.time()) % 89 + 11

    def plugins_lines(needle):
        """The lines holding `needle` of the Plugins dialog listed whole, once its page and the configuration frame
        have answered: a listing with a query leaves out a frame that did not answer rather than noting it."""
        ref = next((w["ref"] for w in call("list_windows").get("items", []) if w.get("title") == "Plugins"), None)
        return [line for line in (settled_tree(target=ref) if ref else "").splitlines() if needle in line]

    def plugins_line(needle):
        return next(iter(plugins_lines(needle)), "")

    client.call("run_action", id="orca_command:open_plugins:orca")
    check("the Plugins dialog lists the fixture", wait_for(lambda: "MCP Web Fixture" in tree(query="MCP Web Fixture"), 15))
    try:
        client.call("list_widgets", target="dialog")
        message = ""
    except orca_mcp.McpError as e:
        message = str(e)
    check("root dialog without a modal names the modeless Plugins dialog", '"Plugins"' in message, message)
    r = call("click", target={"web": "Expand MCP Web Fixture capabilities", **plugins})
    check("the fixture's capabilities expand", r.get("clicked") == "Expand MCP Web Fixture capabilities", r)
    r = call("click", target={"web": "MCP web fixture dialog", "kind": "label", **plugins})
    check("its dialog capability is selected", r.get("clicked") == "MCP web fixture dialog", r)
    r = call("click", target={"web": "Config", "kind": "tab", **plugins})
    check("the Config tab opens", r.get("checked") is True, r)
    check("the configuration frame's controls are listed", wait_for(lambda: 'spin "Speed"' in tree(query="Speed"), 15), tree(query="Config"))
    r = call("set_widget_value", target={"web": "Speed", **plugins}, value=speed)
    check("set_widget_value reaches into the frame", r.get("value") == str(speed), r)
    r = call("set_widget_value", target={"web": "Enabled", **plugins}, value=True)
    check("a checkbox in the frame", r.get("checked") is True, r)
    save_line = plugins_line('button "Save"')
    check("a frame element's id is listed as its CSS selector", re.search(r'button "Save" name=#save', save_line), save_line)
    save = ref_of(save_line, 'button "Save"')
    r = call("click", target=save)
    check("click reaches into the frame by ref", r.get("clicked") == "Save", r)
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
        return plugins_line("frame up").count("frame up")
    before = frame_ups()
    r = call("mouse_gesture", target={"css": "#save", **plugins}, action="down")
    call("click", target={"web": "Config", "kind": "tab", **plugins})
    check("a click on the page ends a press held in its configuration frame",
          r.get("done") == "down" and wait_for(lambda: frame_ups() == before + 1, 3), [r, before, tree(query="frame up")])
    for action, name in (("up", "an up"), ("move", "a move")):
        before = frame_ups()
        call("mouse_gesture", target={"css": "#save", **plugins}, action="down")
        r = call("mouse_gesture", target={"web": "Config", "kind": "tab", **plugins}, action=action)
        check(f"so does {name} on the page", r.get("done") == action and wait_for(lambda: frame_ups() == before + 1, 3),
              [r, before, tree(query="frame up")])
    # A frame too busy to take its release keeps the press on record; the release it takes late is past
    # its deadline and not run, and the next gesture asks again. The frame stalls on the release itself.
    # (Widget clicks: the busy frame holds its page too.)
    call("click", target={"web": "Stall on release", **plugins})
    before = frame_ups()
    call("mouse_gesture", target={"css": "#save", **plugins}, action="down")
    call("click", target={"label": "Filament", "class": "Label"})
    missed = frame_ups()
    call("click", target={"label": "Filament", "class": "Label"})
    check("a release a busy frame missed is asked again", missed == before and wait_for(lambda: frame_ups() == before + 1, 3),
          [before, missed, tree(query="frame up")])
    # Two unnamed Refresh buttons in the frame and the dialog page's own: the journal's index counts
    # them all, as a replay searches the page with its frame.
    refreshes = plugins_lines('button "Refresh"')
    check("the page's and the frame's Refresh buttons are listed", len(refreshes) == 3, refreshes)
    call("export_replay", clear=True)
    second = refreshes[-1].split("[", 1)[1].split("]", 1)[0] if len(refreshes) == 3 else None
    call("click", target=second)
    clicks = [s for s in call("export_replay").get("replay", {}).get("steps", []) if s.get("tool") == "click"]
    recorded = clicks[-1]["args"]["target"] if clicks else {}
    check("the journal records the frame button's index among every match", recorded.get("index") == 2, recorded)
    call("click", target=recorded)
    frame_log = plugins_line("twin")
    check("and replaying it clicks the same button", frame_log.count("twin 2") == 2 and "twin 1" not in frame_log, frame_log)
    r = call("click", target={"web": "Rewrite", **plugins})
    check("the frame rewrites itself", r.get("clicked") == "Rewrite", r)
    r = call("click", target=save)
    check("a frame ref to a control the frame replaced is stale", "gone from the page" in r.get("error", ""), r)
    r = call("click", target={"css": "#save", **plugins})
    check("the same control by locator works", r.get("clicked") == "Save", r)
    save = ref_of(plugins_line('button "Save"'), 'button "Save"')
    r = call("click", target={"web": "Reload", **plugins})
    check("a click that reloads the frame says its page could not be compared, not that nothing changed",
          r.get("page", {}).get("partial") is True and "changed" not in r["page"], r)
    check("the frame's page reloads", wait_for(lambda: 'button "Save"' in tree(query="Save"), 15))
    r = call("click", target=save)
    check("a frame ref from before the frame reloaded is stale", "page changed" in r.get("error", ""), r)
    r = call("click", target={"css": "#save", **plugins})
    check("and the control in the new document by locator works", r.get("clicked") == "Save", r)
    # A configuration frame that fails the search, or does not answer, may hold another match, as such a
    # page may; within can't leave a frame of the matching page out, so it is not suggested.
    call("click", target={"web": "Break", **plugins})
    r = call("click", target={"web": "Config", "kind": "tab", **plugins})
    check("a locator is refused while a configuration frame fails the search", "may not be the only match" in r.get("error", "") and
          "add within" not in r.get("error", ""), r)
    # Last in the Plugins dialog: the muted frame answers nothing more, its own click's reply included.
    call("click", target={"web": "Mute", **plugins}, timeout_s=2)
    r = call("click", target={"web": "Config", "kind": "tab", **plugins})
    check("a locator is refused while a configuration frame does not answer", "may not be the only match" in r.get("error", ""), r)
    r = call("click", target={"web": "Config", "kind": "tab", **plugins, "index": 0})
    check("even with an index", "may not be the only match" in r.get("error", ""), r)
    # Each round asks the muted frame again; what it never answered is given up, not kept: the last
    # round's request, and none of those whose caller stopped waiting (the Mute click's).
    for _ in range(3):
        tree(query="Config")
    r = call("run_javascript", code="window.__orcaMcp.call({op: 'doc'}).relaying", target={"class": "webview", "within": "Plugins"})
    check("the page keeps no more than the last round's unanswered request", r.get("result") in (0, 1), r)
    # A script calling the agent itself gives no deadline; the request is given up all the same.
    call("run_javascript", code="window.__orcaMcp.call({op: 'snapshot'}).nodes.length", target={"class": "webview", "within": "Plugins"})
    time.sleep(11.5)
    r = call("run_javascript", code="window.__orcaMcp.call({op: 'doc'}).relaying", target={"class": "webview", "within": "Plugins"})
    check("a request with no deadline is given up after the tools' default", r.get("result") == 0, r)
    # A change on the page itself is still told beside the muted frame, as partial. (By ref: a locator
    # is refused while the frame doesn't answer.)
    call("run_javascript", target={"class": "webview", "within": "Plugins"},
         code="var b = document.createElement('button'); b.textContent = 'Add note'; "
                "b.style.cssText = 'position: fixed; left: 4px; top: 4px; z-index: 99999'; b.onclick = function () { "
                "var p = document.createElement('p'); p.textContent = 'note added'; "
                "p.style.cssText = 'position: fixed; left: 4px; top: 40px; z-index: 99999; background: white'; "
                "document.body.appendChild(p); }; document.body.appendChild(b); 1")
    r = call("click", target=ref_of(tree(query="Add note"), 'button "Add note"'), timeout_s=5)
    check("a page change beside a frame that doesn't answer is listed, as partial",
          r.get("page", {}).get("partial") is True and r["page"].get("changed") is True and 'label "note added"' in r["page"].get("added", []), r)

    # ---- Orca's setup wizard: a ref from before its page navigated is stale ----
    client.call("run_action", id="orca_command:help_setup_wizard:orca")
    check("the setup wizard opens", wait_for(lambda: 'button "Next"' in tree(target="dialog"), 90), tree(target="dialog")[:1500])
    next_ref = ref_of(tree(target="dialog"), 'button "Next"')
    r = call("click", target=next_ref)
    check("Next navigates to the printer page", r.get("navigated") is True and "/guide/21/" in r.get("url", ""), r)
    # The reply comes once the web view's URL changes, which the old document can outlast for a while; the
    # ref is stale once the new one runs.
    wait_for(lambda: "/guide/21/" in str(call("run_javascript", code="location.pathname").get("result")), 10)
    r = call("click", target=next_ref)
    check("the ref from the page before is stale", "page changed" in r.get("error", ""), r)
    r = call("run_javascript", code="location.reload(); 1")
    check("a reload at the same URL replies navigated", r.get("navigated") is True, r)
    r = call("answer_dialog", button="close")
    check("answer_dialog close replies closed", r.get("closed") is True, r)
    check("the wizard closes", wait_for(lambda: dialogs() == []), r)

    print(f"{len(failed)} checks failed" if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
