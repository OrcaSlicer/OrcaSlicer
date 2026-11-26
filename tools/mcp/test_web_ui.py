#!/usr/bin/env python3
"""Checks the GUI tools on web pages, on the pages of the MCP web fixture plugin.
Its design is the test_web_ui entry of tools/mcp/test-entries/windows_web.md.

    tools/mcp/test_web_ui.py --install DATADIR      # side-loads the fixture plugin into DATADIR
    tools/mcp/orca_mcp.py launch --datadir DATADIR
    tools/mcp/test_web_ui.py

The plugin (tools/mcp/tests/plugins/mcp_web_fixture) opens a page with every kind of control as a
modal plugin dialog, as a modeless window and as a dock panel, and has a configuration UI the Plugins
dialog shows in a sandboxed frame. The checks: what widget_list lists of a page; each action through
widget_click, widget_set_value, keyboard_press and mouse_gesture, with the effect the page logs, the value read back and what the
reply says changed on the page; a credential in a page's URL shown REDACTED; the permission prompt the
plugin's thread opens, answered by dialog_answer and by a rule registered while it is open; the refusals (covered, disabled, file input, a page under the topmost modal); the timeout contract (a
page busy past the deadline never runs the queued click, a slow handler's outcome is unknown);
locators into the shadow root, the frame and the configuration frame, and the journal's locators;
stale refs; closed:true from a page that closes its dialog through a reference to window.wx kept from
before, with another client's snapshots running meanwhile, and from orca.submit; dialog_answer by a
page button and its refusal of apply; an auto-answer rule registered before the dialog opens; the web
budget with the Home page and a modeless page window open; the plugin's page among the main window's tabs,
which the Go to tab action shows by its id; webview_run_javascript's el. It starts on the Generic Klipper Printer 0.4
nozzle, and the stored state its checks read is the plugin's own, written during the run. Exits 0 when every check
passes.
"""

import glob
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, start, until  # noqa: E402

FIXTURE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "tests", "plugins", "mcp_web_fixture")
DIALOG = "plugin_script_action:MCP web fixture dialog:mcp_web_fixture"
WINDOW = "plugin_script_action:MCP web fixture window:mcp_web_fixture"
PANEL = "plugin_script_action:MCP web fixture panel:mcp_web_fixture"
THREAD = "plugin_script_action:MCP web fixture thread:mcp_web_fixture"
# The fixture's page among the main window's tabs, by the id its tab has there.
PAGE_TAB = "plugin.mcp_web_fixture.MCP web fixture page"
# How long a dialog_answer may take before the test reads it as hung.
HANG_S = 30
CAPABILITIES = ["MCP web fixture dialog", "MCP web fixture window", "MCP web fixture panel", "MCP web fixture thread",
                "MCP web fixture page"]


def install(datadir):
    """Copies the fixture plugin into DATADIR's orca_plugins, enabled, as a person's side-load would. It writes the
    plugin afresh, its install state with every capability on and no stored permission, so that a run on a folder an
    earlier run left starts from the same plugin."""
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

    # `detail` may be a callable, called only when the check fails, so a passing check makes no round trip for it.
    def check(name, ok, detail=""):
        if not ok and callable(detail):
            detail = detail()
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
            _, texts = client.call_with_text("widget_list", **args)
        except orca_mcp.McpError:
            return ""
        return texts[0] if texts else ""

    def ref_of(text, needle):
        for line in text.splitlines():
            if needle in line and "[" in line:
                return line.split("[", 1)[1].split("]", 1)[0]
        return None

    def dialogs():
        return [d["title"] for d in client.call("app_state")["dialogs"]]

    def becomes(fn, what):
        """Whether fn() turns true within until()'s 30 s, asked every 0.2 s; when it does not, until()'s account of
        what was seen is printed, and the check given the result fails."""
        try:
            until(fn, what, interval_s=0.2)
            return True
        except TimeoutError as e:
            print(e, flush=True)
            return False

    def answer(button):
        """dialog_answer's reply, {"error": ...} when it is refused, or when it has not replied within HANG_S."""
        try:
            return client.call("dialog_answer", _timeout=HANG_S, button=button)
        except orca_mcp.McpError as e:
            return {"error": str(e)}
        except OSError as e:  # the client's timeout
            return {"error": f"no reply within {HANG_S} s: {e}"}

    def page_log(target=None):
        args = {"code": "document.getElementById('log').textContent"}
        if target:
            args["target"] = target
        return call("webview_run_javascript", **args).get("result") or ""

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
        r = call("webview_run_javascript", **args)
        if "error" in r:
            check(f"the page takes its hook for the next {op}", False, r)

    def stall(op, past=0.5, **args):
        """The page is busy from when the request arrives until `past` seconds past the request's own deadline:
        the call gives up on it, and the agent refuses it as expired once the page is free."""
        when_asked(op, before="while (Date.now() < req.expires + %d) {}" % (past * 1000), **args)

    def settled_tree(**args):
        """A listing every page and configuration frame answered: a frame gets a second through its page's relay,
        which a loaded machine can miss, and a page three, so the listing is asked again until they all have, for up
        to 30 s; then the last listing is given as it is."""
        last = [""]

        def answered():
            last[0] = tree(**args)
            return last[0] and "did not answer" not in last[0]

        try:
            until(answered, "a listing every page answered", interval_s=0.2)
        except TimeoutError:
            pass
        return last[0]

    def python_log():
        """This app's plugin stderr log, where the fixture writes what its dialog submitted. The app makes it when the
        plugin first writes to it, so until then the newest log is an earlier app's, which holds the same line."""
        text = ""
        for log in glob.glob(os.path.join(session["datadir"], "log", f"python_*_{session['pid']}.log")):
            with open(log, encoding="utf-8", errors="replace") as f:
                text += f.read()
        return text

    def open_fixture():
        client.call("action_run", id=DIALOG)
        return (becomes(lambda: dialogs() == ["MCP web fixture"], "the fixture dialog is the one open") and
                becomes(lambda: 'heading "Fixture"' in tree(target="dialog"), "the fixture dialog's page is listed"))

    started = start(client)
    check("start: the Klipper printer with its default process and filament",
          started == {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}, started)
    # The replays write their output here rather than into the data folder.
    out = tempfile.mkdtemp(prefix="web_ui-")

    # ---- What a page is listed as ----
    check("the fixture dialog opens", open_fixture(), lambda: dialogs())
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
    call("replay_export", clear=True)
    r = call("widget_set_value", target={"css": "#name"}, value="Ada")
    check("widget_set_value sets a text field and reads it back", r.get("value") == "Ada", r)
    r = call("widget_set_value", target={"web": "Colour"}, value="blue")
    check("widget_set_value picks an option by its text, case aside", r.get("value") == "Blue", r)
    r = call("widget_set_value", target={"web": "I agree"}, value=True)
    check("widget_set_value ticks a checkbox", r.get("checked") is True, r)
    r = call("widget_set_value", target={"web": "Fancy"}, value=True)
    check("widget_set_value ticks a checkbox drawn as its label", r.get("checked") is True, r)
    call("widget_set_value", target={"web": "Controlled"}, value="xyz")
    r = call("widget_set_value", target={"web": "Level"}, value=55)
    check("widget_set_value sets a range", r.get("value") == "55", r)
    r = call("widget_set_value", target={"web": "Notes"}, value="new text")
    check("widget_set_value sets editable text", r.get("value") == "new text", r)
    r = call("widget_set_value", target={"web": "1 / 2"}, value="333")
    check("widget_set_value sets a table cell's field by its headers", r.get("value") == "333", r)
    card = ref_of(t, 'button "Card one"')
    r = call("widget_click", target=card)
    check("widget_click clicks a clickable div by ref", r.get("clicked") == "Card one", r)
    steps = call("replay_export").get("replay", {}).get("steps", [])
    clicks = [s for s in steps if s.get("tool") == "widget_click"]
    recorded = clicks[-1]["args"]["target"] if clicks else steps
    check("the journal records the ref's locator", isinstance(recorded, dict) and recorded.get("within") == "MCP web fixture" and
          ("css" in recorded or "web" in recorded), recorded)
    r = call("widget_click", target=recorded)
    check("and the recorded locator replays", r.get("clicked") == "Card one", r)
    r = call("widget_click", target={"web": "Card two"})
    check("widget_click reaches a handler added in script", r.get("clicked") == "Card two", r)
    r = call("widget_click", target={"web": "Large", "kind": "radio"})
    check("widget_click chooses a radio button", r.get("checked") is True, r)
    call("keyboard_press", target={"web": "Watched"}, text="ab")
    call("widget_click", target={"web": "First"})
    call("keyboard_press", target={"web": "First"}, keys="Return")
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
    call("widget_click", target={"web": "Card two"})
    check("a click ends a press a down left", page_log().count("pad up") == ups + 1, lambda: page_log())
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("widget_click", target={"web": "Disabled"})
    check("so does a click the page refuses", "disabled" in r.get("error", "") and page_log().count("pad up") == ups + 2, lambda: [r, page_log()])
    # Left and Right on a selection collapse it to its start and its end.
    caret = []
    for keys in ("Left", "Right"):
        call("webview_run_javascript", code="el.value = 'abcd'; el.focus(); el.setSelectionRange(1, 3); 1", target=ref_of(tree(target="dialog"), 'text "Name"'))
        call("keyboard_press", target={"css": "#name"}, keys=keys)
        caret.append(call("webview_run_javascript", code="document.getElementById('name').selectionStart").get("result"))
    call("widget_set_value", target={"css": "#name"}, value="Ada")
    check("Left and Right collapse a selection to its start and end", caret == [1, 3], caret)
    call("mouse_gesture", target={"web": "Capture pad"}, action="drag", x=5, y=5, to=[60, 30], steps=4)
    call("webview_run_javascript", code="document.getElementById('scroller').scrollTop = 0; 1")
    r = call("mouse_gesture", target={"web": "Scroll me"}, action="wheel", delta=-3)
    top = call("webview_run_javascript", code="document.getElementById('scroller').scrollTop").get("result")
    check("a wheel turn on a scroller scrolls it", isinstance(top, (int, float)) and top > 0, [r, top])
    r = call("widget_click", target={"web": "Deep"})
    check("widget_click scrolls an element out of view into it", r.get("clicked") == "Deep", r)
    r = call("widget_click", target={"web": "Leftmost"})
    check("widget_click scrolls a scroller back to a control left of it", r.get("clicked") == "Leftmost", r)
    r = call("widget_click", target={"web": "Inner"})
    check("widget_click reaches a same-origin frame", r.get("clicked") == "Inner", r)
    r = call("widget_click", target={"css": "#shadowbtn"})
    check("a CSS locator reaches into a shadow root", r.get("clicked") == "Shadow", r)
    log = page_log()
    # The pad logs offsetX/Y, which count from inside its 1 px border; mouse_gesture's x/y from its outer edge.
    for line in ["name change Ada", "colour Blue", "agree true", "fancy true", "controlled xyz", "card1", "card2", "keyup sees ab",
                 "go clicked", "f2 submitted by go-value", "pad down 4,4", "pad up 59,29 after 4 moves", "pad shifted 1",
                 "cap samples 5", "deep", "leftmost", "inner", "shadow"]:
        check(f"the page saw {line!r}", line in log, log)

    # ---- Locators ----
    r = call("widget_click", target={"css": ".card"})
    check("a locator with several matches asks for index", "2 page elements match" in r.get("error", ""), r)
    r = call("widget_click", target={"css": ".card", "index": 1})
    check("index picks among the matches", r.get("clicked") == "Card two", r)
    # A locator waits for a match to show until 2 s before its timeout, but at least 3 s or all of a shorter
    # timeout, as here; nothing will show.
    r = call("widget_click", target={"web": "Nope"}, timeout_s=1)
    check("a locator that matches nothing says so", "No page element matches" in r.get("error", ""), r)
    r = call("widget_click", target={"web": "Some bold text in one paragraph."})
    check("a locator reaches a text label, through the block it is in", r.get("kind") == "label", r)
    r = call("widget_click", target={"web": "Deep", "kind": "label"})
    check("text repeating a button's label is reached with kind label", "error" not in r and "deep text" in page_log(), r)

    # ---- Refusals ----
    r = call("widget_click", target={"web": "Under"})
    check("a covered element is refused, naming what covers it", "covered by (span#veil)" in r.get("error", ""), r)
    r = call("widget_click", target={"web": "OK"}, x=500, y=5)
    check("a point outside the element is refused", "outside the element" in r.get("error", ""), r)
    r = call("widget_click", target={"web": "Disabled"})
    check("a disabled element is refused", "disabled" in r.get("error", ""), r)
    r = call("widget_set_value", target={"web": "Upload"}, value="x")
    check("a file input is refused", "file input" in r.get("error", ""), r)
    fixed = ref_of(tree(target="dialog"), 'text "Fixed"')
    call("keyboard_press", target=fixed, text="zz")
    r = call("webview_run_javascript", code="el.value", target=fixed)
    check("keys leave a read-only field as it was", r.get("result") == "fixed", r)
    r = call("keyboard_press", target={"web": "Locked"}, text="zz")
    check("keys to a field in a disabled fieldset are refused", "disabled" in r.get("error", ""), r)
    # Refused clicks and dialog_answer still end a press a down left.
    ups = page_log().count("pad up")
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    view = ref_of(tree(target="dialog", query="webview"), "webview")
    r = call("widget_click", target=view)
    check("widget_click on a web view itself points to its page's elements", "That is a web view" in r.get("error", ""), r)
    check("and ends a press a down left", page_log().count("pad up") == ups + 1, lambda: page_log())
    r = call("widget_set_value", target=view, value="x")
    check("so does widget_set_value", "That is a web view" in r.get("error", ""), r)
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("dialog_answer", button="Under")
    check("dialog_answer refuses a page button something covers", "covered" in r.get("error", ""), r)
    check("and ends a press a down left too", page_log().count("pad up") == ups + 2, lambda: page_log())
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("mouse_gesture", target={"web": "Nope"}, action="up", timeout_s=1)
    check("so does an up whose target can't be found", "No page element matches" in r.get("error", "") and page_log().count("pad up") == ups + 3,
          lambda: [r, page_log()])
    # A release names the press it ends, so one for an earlier press leaves the page's newer one held
    # (ids count from 1, and this down is far from the first).
    call("mouse_gesture", target={"web": "Pad"}, action="down", x=5, y=5)
    r = call("webview_run_javascript", code="JSON.stringify(window.__orcaMcp.call({op: 'release', press: 1}))")
    check("a release naming an earlier press leaves the newer one held", page_log().count("pad up") == ups + 3, lambda: [r, page_log()])
    call("widget_click", target={"web": "Card one"})
    check("which the next click releases", page_log().count("pad up") == ups + 4, lambda: page_log())
    # A down whose reply never comes may still have pressed, so it is on record and the next click
    # releases it (the slow pad's handler outlasts the 1 s the down waits).
    r = call("mouse_gesture", target={"web": "Slow pad"}, action="down", timeout_s=1)
    check("a down the page answers too late has no outcome", "error" in r, r)
    call("widget_click", target={"web": "Card one"})
    check("and the next click releases its press all the same",
          becomes(lambda: "slow pad up" in page_log(), "the page logs slow pad up"), lambda: page_log())
    r = call("webview_run_javascript", code="window.__orcaMcp.call({op: 'snapshot', expires: Date.now() - 1})")
    check("the page refuses a request past its deadline", isinstance(r.get("result"), dict) and r["result"].get("code") == "expired", r)

    # ---- The timeout contract ----
    r = call("widget_click", target={"web": "Slow"}, timeout_s=1)
    check("a slow handler's outcome is unknown", "outcome is unknown" in r.get("error", ""), r)
    check("and its effect shows later", becomes(lambda: "slow done" in page_log(), "the page logs slow done"))
    before = page_log().splitlines().count("ok")
    ok = ref_of(tree(target="dialog"), 'button "OK"')
    stall("act")
    r = call("widget_click", target=ok, timeout_s=1)
    check("a click queued behind a busy page has an unknown outcome", "outcome is unknown" in r.get("error", ""), r)
    check("and the page refuses it once free",
          becomes(lambda: page_log() != "", "the page answers again") and page_log().splitlines().count("ok") == before,
          lambda: page_log())

    # ---- Changing pages and stale refs ----
    r = call("widget_click", target={"web": "Later"})
    check("a click's reply lists what its handler added to the page", r.get("page", {}).get("changed") is True and
          'button "Made later"' in r["page"].get("added", []), r)
    check("an element a handler adds later shows in the next snapshot",
          becomes(lambda: 'button "Made later"' in tree(target="dialog"), "the snapshot lists Made later"))
    r = call("widget_click", target={"web": "Nothing"})
    check("a click that changes nothing on its page says so", r.get("page") == {"changed": False}, r)
    r = call("widget_click", target={"web": "Add extra"})
    check("a change the snapshot notes (a select's new option) is a change", r.get("page", {}).get("changed") is True and
          any("options: One | Two" in line for line in r["page"].get("added", [])), r)
    made = ref_of(tree(target="dialog"), 'button "Made later"')
    call("webview_run_javascript", code="document.getElementById('made').remove(); 1")
    r = call("widget_click", target=made)
    check("a ref to a removed element is stale", "gone from the page" in r.get("error", ""), r)
    r = call("widget_click", target={"web": "Jump"})
    check("a link that changes the URL replies navigated", r.get("navigated") is True and r.get("url", "").endswith("#bottom"), r)
    r = call("widget_click", target={"web": "Up"})
    check("a second fragment change replies navigated too", r.get("navigated") is True and r.get("url", "").endswith("#top"), r)
    r = call("webview_run_javascript", code="location.hash = 'evaluated'; 1")
    check("a script's fragment change replies navigated with the new url", r.get("navigated") is True and
          r.get("url", "").endswith("#evaluated"), r)
    r = call("widget_click", target={"web": "Token link"})
    check("a navigated url shows a credential parameter's value as REDACTED", r.get("navigated") is True and
          r.get("url", "").endswith("#orca-token=REDACTED&view=2"), r)
    # The snapshot answers, and lists the link just clicked, so it is of the page whose URL holds the secret.
    snapshot = tree(target="dialog")
    check("and the snapshot shows no secret either", 'link "Token link"' in snapshot and "fixture-secret" not in snapshot,
          lambda: snapshot or "widget_list gave no snapshot")

    name = ref_of(tree(target="dialog"), 'text "Name"')
    r = call("webview_run_javascript", code="el.value", target=name)
    check("webview_run_javascript runs with a page element as el", r.get("result") == "Ada", r)
    r = call("widget_click", target={"css": "#name"})
    check("widget_click focuses a text field", r.get("clicked") == "Name", r)
    r = call("keyboard_press", text="Z")
    check("keys without a target go to the dialog page's focused element",
          "Z" in (call("webview_run_javascript", code="el.value", target=name).get("result") or ""), r)
    r = call("keyboard_press", target=ref_of(tree(target="dialog", query="webview"), "webview"), text="Q")
    check("keys to a web view itself go to its page's focused element",
          "ZQ" in (call("webview_run_javascript", code="el.value", target=name).get("result") or ""), r)
    steps = call("replay_export").get("replay", {}).get("steps", [])
    keyed = [s for s in steps if s.get("tool") == "keyboard_press"]
    check("and the journal names the web view by its locator", keyed and keyed[-1]["args"].get("target", {}).get("class") == "webview",
          keyed[-1]["args"] if keyed else steps)
    # Typed text in editable content, where the editing command is unavailable: the text still goes in.
    notes = ref_of(tree(target="dialog"), 'text "Notes"')
    call("webview_run_javascript", code="document.__exec = document.execCommand; document.execCommand = function () { return false; }; 1")
    before_notes = call("webview_run_javascript", code="el.textContent", target=notes).get("result") or ""
    call("keyboard_press", target=notes, text="XY")
    call("keyboard_press", target=notes, keys="Backspace")
    r = call("webview_run_javascript", code="el.textContent", target=notes)
    # And without Selection.modify (where the caret's text node gives the character).
    call("webview_run_javascript", code="window.__modify = Selection.prototype.modify; delete Selection.prototype.modify; 1")
    call("keyboard_press", target=notes, keys="Backspace")
    after = call("webview_run_javascript", code="el.textContent", target=notes).get("result")
    call("webview_run_javascript", code="Selection.prototype.modify = window.__modify; document.execCommand = document.__exec; 1")
    check("typing and Backspace in editable content work without the editing command", "X" in (r.get("result") or "") and
          "XY" not in (r.get("result") or ""), r)
    check("and Backspace without Selection.modify too", after == before_notes, [before_notes, after])
    # With the caret at an element boundary between two text nodes, without Selection.modify either.
    boundary = ("var n = el; n.textContent = ''; n.appendChild(document.createTextNode('ab')); "
                "n.appendChild(document.createTextNode('cd')); n.focus(); var r = document.createRange(); r.setStart(n, 1); "
                "r.collapse(true); var s = getSelection(); s.removeAllRanges(); s.addRange(r); 1")
    results = []
    for keys in ("Backspace", "Delete"):
        call("webview_run_javascript", code=boundary, target=notes)
        call("webview_run_javascript", code="document.execCommand = function () { return false; }; "
                                    "window.__modify2 = Selection.prototype.modify; delete Selection.prototype.modify; 1")
        call("keyboard_press", target=notes, keys=keys)
        results.append(call("webview_run_javascript", code="el.textContent", target=notes).get("result"))
        call("webview_run_javascript", code="Selection.prototype.modify = window.__modify2; document.execCommand = document.__exec; 1")
    check("a delete at an element boundary takes the character beside it", results == ["acd", "abd"], results)
    # Tab follows the composed focus order: the shadow root's button, the host's slotted one, past an
    # anchor with no href, into the frame; and back.
    call("widget_click", target={"css": "#shadowbtn"})
    call("keyboard_press", keys="Tab")
    check("Tab moves from a shadow root's button to the slotted one", re.search(r'button "Slotted".*\[focused\]', tree(target="dialog")))
    call("keyboard_press", keys="Tab")
    check("then past an anchor with no href into the frame", re.search(r'button "Inner".*\[focused\]', tree(target="dialog")))
    call("keyboard_press", keys="Shift+Tab")
    check("Shift+Tab moves back to the slotted button", re.search(r'button "Slotted".*\[focused\]', tree(target="dialog")))
    call("widget_click", target={"web": "Fixed"})
    call("keyboard_press", keys="Tab")
    check("Tab reaches the control in a disabled fieldset's legend", re.search(r'checkbox "Unlock".*\[focused\]', tree(target="dialog")))
    call("keyboard_press", keys="Tab")
    check("then skips a control the fieldset disables", re.search(r'spin "Fixed number".*\[focused\]', tree(target="dialog")), lambda: tree(target="dialog", query="Fixed"))
    r = call("widget_click", target={"web": "Unlock"})
    check("a control in a disabled fieldset's legend takes a click", r.get("checked") is True, r)
    call("keyboard_press", target={"web": "Fixed number"}, keys="Up")
    r = call("webview_run_javascript", code="document.getElementById('ron').value")
    check("an arrow leaves a read-only number field as it was", r.get("result") in (5, "5"), r)
    # Without requestSubmit (as on Safari 15), Enter's submission validates the form first.
    call("webview_run_javascript", code="window.__requestSubmit = HTMLFormElement.prototype.requestSubmit; "
                                "delete HTMLFormElement.prototype.requestSubmit; 1")
    call("keyboard_press", target={"web": "Needed"}, keys="Return")
    invalid_submitted = "f3 submitted" in page_log()
    call("widget_set_value", target={"web": "Needed"}, value="here")
    call("keyboard_press", target={"web": "Needed"}, keys="Return")
    call("webview_run_javascript", code="HTMLFormElement.prototype.requestSubmit = window.__requestSubmit; 1")
    check("a form with an empty required field is not submitted", not invalid_submitted, lambda: page_log())
    check("and is once it is filled in", "f3 submitted" in page_log(), lambda: page_log())
    r = call("keyboard_press", target={"css": "#name"}, text="\U0001F600")
    typed = call("webview_run_javascript", code="el.value", target=name).get("result") or ""
    check("a character outside the BMP is typed whole", typed.endswith("\U0001F600"), r)
    call("keyboard_press", target={"css": "#name"}, keys="Backspace")
    check("and Backspace takes it away whole", call("webview_run_javascript", code="el.value", target=name).get("result") == typed[:-1], typed)
    # A caret placed between an emoji's halves still takes the whole emoji, and arrows step over one.
    for keys, kept in (("Backspace", "ab"), ("Delete", "ab")):
        call("webview_run_javascript", code="el.value = 'a\\uD83D\\uDE00b'; el.focus(); el.setSelectionRange(2, 2); 1", target=name)
        call("keyboard_press", target={"css": "#name"}, keys=keys)
        value = call("webview_run_javascript", code="el.value", target=name).get("result")
        check(f"{keys} with the caret inside an emoji takes it whole", value == kept, value)
    call("webview_run_javascript", code="el.value = 'a\\uD83D\\uDE00b'; el.focus(); el.setSelectionRange(1, 1); 1", target=name)
    call("keyboard_press", target={"css": "#name"}, keys="Right")
    r = call("webview_run_javascript", code="el.selectionStart", target=name)
    check("Right steps over an emoji", r.get("result") == 3, r)
    call("keyboard_press", target={"css": "#name"}, keys="Left")
    r = call("webview_run_javascript", code="el.selectionStart", target=name)
    check("and Left back over it", r.get("result") == 1, r)
    call("widget_set_value", target={"css": "#name"}, value="Ada")
    # A page button that only shows after dialog_answer's 3 s is not pressed late: the page shows it half a
    # second past the deadline of the first press it is asked for.
    when_asked("press", after="setTimeout(function () { var b = document.createElement('button'); b.textContent = 'Shown late'; "
                              "b.onclick = function () { log('shown late clicked'); }; document.body.appendChild(b); }, "
                              "req.expires + 500 - Date.now());")
    # A call that looked on past its 3 s would find the button and press it.
    r = answer("Shown late")
    check("dialog_answer gives up on a page button that has not shown by its 3 s",
          "page has no button" in r.get("error", ""), r)
    check("and the button it gave up on was not pressed once it showed",
          becomes(lambda: 'button "Shown late"' in tree(target="dialog"), "the page shows Shown late") and
          "shown late clicked" not in page_log())
    r = call("dialog_answer", button="Shown late")
    check("once shown it is pressed", r.get("pressed") is True and "shown late clicked" in page_log(), r)
    t0 = ref_of(tree(target="dialog"), 'button "Card one"')
    call("replay_export", clear=True)
    call("window_screenshot", target=t0, include_image=False, save_path=os.path.join(os.environ.get("TEMP", "/tmp"), "mcp_web_shot.png"), overwrite=True)
    shots = [s for s in call("replay_export").get("replay", {}).get("steps", []) if s.get("tool") == "window_screenshot"]
    check("a page element's screenshot is journalled by its web view's locator",
          shots and shots[-1]["args"].get("target") == {"class": "webview", "within": "MCP web fixture"}, shots)

    # ---- dialog_answer ----
    r = call("dialog_answer", button="OK")
    check("dialog_answer presses a page button by label", r.get("web") is True and r.get("pressed") is True, r)
    check("and the page saw the press",
          becomes(lambda: page_log().splitlines().count("ok") == before + 1, "the page logs one more ok"), lambda: page_log())
    r = call("dialog_answer", button="apply")
    check("dialog_answer refuses ok-like answers the page has no button for", "page has no button" in r.get("error", ""), r)
    # A page that listed its buttons and then stops answering still gets that refusal: it answers the first
    # ask and stalls on the next.
    stall("press", skip=1)
    r = call("dialog_answer", button="Nope")
    check("a page busy after listing its buttons gets the no-button refusal, listing them", "page has no button" in r.get("error", "") and
          "page_buttons" in r.get("error", ""), r)
    # The stall ends half a second after the reply; a script runs once it has.
    r = call("webview_run_javascript", code="1", timeout_s=30)
    check("the page answers again once its stall ends", r.get("result") == 1, r)

    # ---- Closing through a kept window.wx while another client snapshots ----
    other = orca_mcp.OrcaMcp(session["port"], session["token"])
    snapshots = []

    def keep_snapping():
        for _ in range(15):
            try:
                other.call("widget_list")
                snapshots.append(True)
            except orca_mcp.McpError as e:
                snapshots.append(str(e))
            time.sleep(0.05)

    snapper = threading.Thread(target=keep_snapping)
    snapper.start()
    r = call("widget_click", target={"web": "Close through wx"})
    snapper.join()
    check("a page closing its dialog through a kept window.wx replies closed", r.get("closed") is True, r)
    check("snapshots during the close all answered", snapshots == [True] * 15, snapshots)
    check("the dialog is gone", becomes(lambda: dialogs() == [], "no dialog is open"))

    # ---- The host's permission prompt for the plugin's thread ----
    def prompt():
        return next((d for d in client.call("app_state")["dialogs"] if d.get("title") == "Plugin permission request"), None)

    r = call("action_run", id=THREAD)
    check("the plugin's thread opens a permission prompt",
          becomes(lambda: prompt() is not None, "the permission prompt is open"), r)
    box = prompt() or {}
    check("it is listed with its buttons", [b.get("label") for b in box.get("buttons", [])] == ["Yes", "No"], box)
    if r.get("status") == "blocked_by_dialog":
        check("the blocked note says to answer it with dialog_answer", "dialog_answer" in r.get("note", ""), r)
    r = call("dialog_answer", button="No")
    check("dialog_answer presses the prompt's button", r.get("pressed") is True and r.get("closed") is True and
          r.get("dialogs", []) == [], r)
    check("the prompt is gone", becomes(lambda: prompt() is None, "the permission prompt is gone"))
    # A press held on a page when the second prompt opens ends before the rule answers the prompt.
    call("action_run", id=WINDOW)
    held_log = {"web": "Card one", "within": "MCP web fixture window"}
    check("the modeless window opens", becomes(lambda: 'button "Card one"' in tree(query="Card one"),
                                               "the modeless window's page is listed"))
    ups = page_log(held_log).count("pad up")
    call("mouse_gesture", target={"web": "Pad", "within": "MCP web fixture window"}, action="down", x=5, y=5)
    call("action_run", id=THREAD)
    check("the plugin asks again, having been refused", becomes(lambda: prompt() is not None, "the permission prompt is open"))
    r = call("dialog_auto_answer", rules=[{"match": "requesting permission to create a thread", "button": "no"}])
    check("a rule registered while the prompt is open answers it", r.get("answered_open_dialog", {}).get("button") == "no" and
          any(rule.get("uses") == 1 for rule in r.get("rules", [])), r)
    check("and the prompt is gone again",
          becomes(lambda: prompt() is None, "the permission prompt is gone"), lambda: dialogs())
    check("and the press held on a page ended first",
          becomes(lambda: page_log(held_log).count("pad up") == ups + 1, "the window's page logs one more pad up"),
          lambda: page_log(held_log))
    call("dialog_auto_answer", rules=[])

    # ---- The Home page, a modeless page window and the web budget ----
    client.call("action_run", id="orca_command:go_to_tab:orca", args={"tab": "home"})
    client.call("action_run", id=WINDOW)
    check("the modeless window's page is listed",
          becomes(lambda: 'button "Card one"' in tree(query="Card one"), "the modeless window's page is listed"))
    native = client.call("widget_list", include_web=False)["nodes"]
    t = settled_tree(max_nodes=native)
    window = t.split('"MCP web fixture window"', 1)
    check("with pages listed, max_nodes still holds every widget", len(window) == 2 and 'button "Card one"' in window[1], t[-1500:])
    check("the Home page is listed too", "homepage" in t and t.count("[d") > 20, t[:1500])
    r = call("widget_click", target={"web": "Card one"})
    check("a modeless page takes input", r.get("clicked") == "Card one", r)

    # ---- A modal over it: the page behind takes no input; closing through orca.submit ----
    check("the fixture dialog opens over the window", open_fixture(), lambda: dialogs())
    t = tree()
    check("the pages behind the modal are not read", t.count("behind a modal dialog") >= 2, t[:2000])
    r = call("widget_click", target={"web": "Card one"})
    check("a locator without within looks only in the modal's page", r.get("clicked") == "Card one", r)
    r = call("widget_click", target={"web": "Card one", "within": "MCP web fixture window"})
    check("a page under the topmost modal takes no input", "modal" in r.get("error", "").lower(), r)
    call("widget_set_value", target={"css": "#name"}, value="Zed")
    r = call("widget_click", target={"web": "Submit"})
    check("orca.submit replies closed", r.get("closed") is True, r)
    check("the plugin got what the page submitted",
          becomes(lambda: 'submitted {"colour": "Green", "name": "Zed"}' in python_log(), "the Python log holds the submit"))
    r = call("widget_click", target={"web": "Card two", "within": "MCP web fixture window"})
    check("the modeless window's page takes input again", r.get("clicked") == "Card two", r)

    # ---- An auto-answer registered before the dialog opens ----
    call("dialog_auto_answer", rules=[{"match": "^MCP web fixture$", "button": "Submit"}])
    client.call("action_run", id=DIALOG)
    check("an auto-answer rule answers the web dialog on its page", becomes(lambda: dialogs() == [], "no dialog is open"),
          lambda: dialogs())
    rules = client.call("app_state").get("auto_answer_rules", [])
    check("and counts its use", any(rule.get("uses", 0) >= 1 for rule in rules), rules)
    call("dialog_auto_answer", rules=[])

    # ---- Closing a web dialog whose page is busy, or that closes before its click's reply ----
    # Past its 3 s, cancel ends the dialog itself, and its reply has no web: a reply with web true says the page
    # pressed its button, which it could only after the call had waited on it. The busy page stays busy 2 s past the
    # press's deadline and then presses its Cancel, its handler taken off, and the stalling Cancel's handler replies
    # once it ends, 10 s on, so a call that waited on either replies with web, the dialog still open.
    check("the fixture dialog opens to be cancelled", open_fixture(), lambda: dialogs())
    when_asked("press", before="while (Date.now() < req.expires + 2000) {} req.expires = 0;",
               setup="document.getElementById('stallcancel').onclick = null;")
    r = answer("cancel")
    check("cancel ends a web dialog itself while its page is busy", r.get("closed") is True and "web" not in r, r)
    check("the busy dialog is gone", becomes(lambda: dialogs() == [], "no dialog is open"), lambda: dialogs())
    # The page's own Cancel, whose handler outlasts the 3 s: still running when cancel ends the dialog.
    check("the fixture dialog opens for its stalling Cancel", open_fixture(), lambda: dialogs())
    r = answer("cancel")
    check("cancel ends the dialog natively while the page's Cancel handler still runs",
          r.get("closed") is True and "web" not in r, r)
    check("the stalled dialog is gone", becomes(lambda: dialogs() == [], "no dialog is open"), lambda: dialogs())
    check("the fixture dialog opens to close at once", open_fixture(), lambda: dialogs())
    r = call("widget_click", target={"web": "Close at once"})
    check("a page that closes its dialog before the click's reply replies closed", r.get("closed") is True and "error" not in r, r)
    check("that dialog is gone", becomes(lambda: dialogs() == [], "no dialog is open"), lambda: dialogs())

    # ---- A plugin's page among the main window's tabs: the Go to tab action shows it by its id ----
    # The plugin registers its page once it has loaded, which may be after the app reports ready.
    shown = {}

    def go_to_page_tab():
        shown.clear()
        shown.update(call("action_run", id="orca_command:go_to_tab:orca", args={"tab": PAGE_TAB}))
        return shown.get("tab") == PAGE_TAB

    check("the Go to tab action shows the plugin's page", becomes(go_to_page_tab, "Go to tab shows the plugin's page"),
          lambda: shown)
    check("and the window shows it", client.call("app_state").get("tab") == PAGE_TAB, lambda: client.call("app_state").get("tab"))

    # ---- A dock panel, beside the 3D view ----
    client.call("action_run", id="orca_command:go_to_tab:orca", args={"tab": "prepare"})
    client.call("action_run", id=PANEL)
    check("the dock panel's page is listed",
          becomes(lambda: 'button "Ping"' in tree(query="Ping"), "the dock panel's page is listed"))
    # Its Content-Security-Policy forbids eval: the agent works there, webview_run_javascript says why it can't.
    r = call("widget_click", target={"web": "Ping"})
    check("its page, whose CSP forbids eval, takes a click", r.get("clicked") == "Ping", r)
    ping = ref_of(tree(query="Ping"), 'button "Ping"')
    check("and saw it", becomes(lambda: 'label "ping"' in tree(query="ping"), "the panel's log reads ping"),
          lambda: tree(query="ping"))
    # WebView2 does not hold the injected script to the page's policy; WKWebView and WebKitGTK do, and the
    # script runs through eval, which the policy forbids.
    r = call("webview_run_javascript", code="1", target=ping)
    check("webview_run_javascript there runs, or names the page's Content-Security-Policy",
          r.get("result") == 1 if os.name == "nt" else "Content-Security-Policy" in r.get("error", ""), r)
    # A press left on the window's pad ends when a click lands on another page.
    window_log = {"web": "Card one", "within": "MCP web fixture window"}
    ups = page_log(window_log).count("pad up")
    window_pad = {"web": "Pad", "within": "MCP web fixture window"}
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    call("widget_click", target={"web": "Ping"})
    check("a click on another page ends a press held on the first", page_log(window_log).count("pad up") == ups + 1, lambda: page_log(window_log))
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    call("mouse_gesture", target={"web": "Ping"}, action="up", x=3, y=3)
    check("so does an up on another page", page_log(window_log).count("pad up") == ups + 2, lambda: page_log(window_log))
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    r = call("widget_click", target={"label": "Filament", "class": "Label"})
    check("and a click on a widget", page_log(window_log).count("pad up") == ups + 3, lambda: [r, page_log(window_log)])
    # A release the page can't take while it is busy is asked again by the next gesture; the one it
    # takes late has passed its deadline and is not run. The gestures are widget clicks, which wait on no
    # page.
    filament = {"label": "Filament", "class": "Label"}
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    stall("release", target=window_log)
    call("widget_click", target=filament)
    missed = page_log(window_log).count("pad up")  # runs once the page is free, after the late release
    call("widget_click", target=filament)
    check("a release a busy page missed is asked again", missed == ups + 3 and page_log(window_log).count("pad up") == ups + 4,
          lambda: [missed, page_log(window_log)])
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
    call("widget_click", target=filament)
    check("a press is released beside a busy page's, whose release goes unanswered",
          r.get("done") == "down" and dock_ups() == docked + 1, lambda: [r, docked, tree(query="dock up")])
    missed = page_log(window_log).count("pad up")
    call("widget_click", target=filament)
    check("and the busy page's press stays on record until it answers",
          missed == ups + 4 and page_log(window_log).count("pad up") == ups + 5, lambda: [missed, page_log(window_log)])
    # An up the busy page takes too late is past its deadline and not run, so its press stays on
    # record and the next gesture releases it. (By ref, which is resolved without asking the page.)
    pad_ref = ref_of(tree(query="Pad"), 'canvas "Pad"')
    call("mouse_gesture", target=pad_ref, action="down", x=5, y=5)
    stall("act", target=window_log)
    r = call("mouse_gesture", target=pad_ref, action="up", x=5, y=5, timeout_s=1)
    missed = page_log(window_log).count("pad up")
    call("widget_click", target=filament)
    check("an up the page takes too late leaves its press for the next gesture",
          "error" in r and missed == ups + 5 and page_log(window_log).count("pad up") == ups + 6, lambda: [r, missed, page_log(window_log)])
    # A replay's press on a page ends with the replay, as a widget's does.
    pressing = {"orca_replay_version": "0.2.0", "name": "press", "steps": [
        {"tool": "mouse_gesture", "args": {"target": {"web": "Pad", "within": "MCP web fixture window"}, "action": "down", "x": 5, "y": 5}}]}
    res = client.await_job(client.call("replay_run", replay=pressing, out_dir=os.path.join(out, "press_replay")))
    check("a replay's press on a page ends with the replay",
          res.get("passed") is True and
          becomes(lambda: page_log(window_log).count("pad up") == ups + 7, "the window's page logs one more pad up"),
          lambda: [res, page_log(window_log)])
    # Even when the page is busy as the replay ends: the release, which has no deadline, waits for the page,
    # busy for 3 s from when the release reaches it.
    pressing["steps"].append({"tool": "webview_run_javascript", "args": {
        "target": {"web": "Card one", "within": "MCP web fixture window"},
        "code": hook_code("release", before="var t = Date.now(); while (Date.now() - t < 3000) {}")}})
    res = client.await_job(client.call("replay_run", replay=pressing, out_dir=os.path.join(out, "press_busy_replay")))
    check("a replay's press ends with the replay while its page is busy",
          res.get("passed") is True and
          becomes(lambda: page_log(window_log).count("pad up") == ups + 8, "the window's page logs one more pad up"),
          lambda: [res, page_log(window_log)])
    # An auto-answer rule's press is a click too, so a press held on a page ends first.
    call("mouse_gesture", target=window_pad, action="down", x=5, y=5)
    call("dialog_auto_answer", rules=[{"match": "^MCP web fixture$", "button": "Submit"}])
    client.call("action_run", id=DIALOG)
    check("an auto-answer rule's press ends a press held on a page",
          becomes(lambda: dialogs() == [], "no dialog is open") and
          becomes(lambda: page_log(window_log).count("pad up") == ups + 9, "the window's page logs one more pad up"),
          lambda: page_log(window_log))
    call("dialog_auto_answer", rules=[])
    # A page that does not answer may hold another match, so a locator is refused until it does.
    def pings():
        """The panel's ping log as its label, without the ref the snapshot renumbers: asked again for up to 30 s while
        the panel's page has not answered, as a listing with a query leaves such a page out. A log not found reads as
        ""."""
        def line():
            return next((line for line in tree(query="ping").splitlines() if 'label "ping' in line), "")

        try:
            return until(line, "the panel's ping log is listed", interval_s=0.2).split("]", 1)[-1].split("@", 1)[0].strip()
        except TimeoutError:
            return ""

    # Two clicks on Ping so far: "its page, whose CSP forbids eval, takes a click" and the one that ends the
    # press held on the window's pad.
    two_pings = 'label "ping ping"'
    pinged = pings()
    # The panel's page has the call's 5 s to answer, and the window's stalls past them.
    stall("find", target=window_log)
    r = call("widget_click", target={"web": "Ping"}, timeout_s=5)
    check("a locator is refused while another candidate page does not answer", "may not be the only match" in r.get("error", ""), r)
    after = pings()
    check("and the panel's own page never saw that click", pinged == two_pings and after == two_pings, [pinged, after])
    # So is one while another candidate page fails the search: it breaks what the agent reads until that
    # search has been answered.
    when_asked("find", target=window_log, after="window.getComputedStyle = styles;",
               setup="var styles = window.getComputedStyle; window.getComputedStyle = function () { throw new Error('broken'); };")
    r = call("widget_click", target={"web": "Ping"})
    check("a locator is refused while another candidate page fails the search", "may not be the only match" in r.get("error", ""), r)
    # A page that keeps the agent from installing (its own non-configurable __orcaMcp) can't run it at
    # all, so it holds nothing to reach and the match on the panel is used.
    call("webview_run_javascript", target={"web": "Card one", "within": "MCP web fixture window"},
         code="Object.defineProperty(window, '__orcaMcp', {value: {v: 'other'}, configurable: false, writable: false}); 1")
    r = call("webview_run_javascript", code="1", target={"class": "webview", "within": "MCP web fixture window"})
    check("that page fails to run the agent", "could not run" in r.get("error", ""), r)
    r = call("widget_click", target={"web": "Ping"})
    check("a locator still reaches the match beside a page that can't run the agent", r.get("clicked") == "Ping", r)
    # A dock panel stays beside the 3D view until it is closed, and start() closes none, so its own page closes it.
    r = call("widget_click", target={"web": "Close panel"})
    check("the dock panel's page closes the panel",
          becomes(lambda: 'button "Ping"' not in tree(query="Ping"), "the dock panel is gone"), r)

    # ---- A plugin's configuration UI, in the Plugins dialog's sandboxed frame ----
    plugins = {"within": "Plugins"}
    speed = int(time.time()) % 89 + 11

    def plugins_lines(needle):
        """The lines holding `needle` of the Plugins dialog listed whole, once its page and the configuration frame
        have answered: a listing with a query leaves out a frame that did not answer rather than noting it."""
        ref = next((w["ref"] for w in call("window_list").get("items", []) if w.get("title") == "Plugins"), None)
        return [line for line in (settled_tree(target=ref) if ref else "").splitlines() if needle in line]

    def plugins_line(needle):
        return next(iter(plugins_lines(needle)), "")

    client.call("action_run", id="orca_command:open_plugins:orca")
    check("the Plugins dialog lists the fixture",
          becomes(lambda: "MCP Web Fixture" in tree(query="MCP Web Fixture"), "the Plugins dialog lists the fixture"))
    try:
        client.call("widget_list", target="dialog")
        message = ""
    except orca_mcp.McpError as e:
        message = str(e)
    check("root dialog without a modal names the modeless Plugins dialog", '"Plugins"' in message, message)
    r = call("widget_click", target={"web": "Expand MCP Web Fixture capabilities", **plugins})
    check("the fixture's capabilities expand", r.get("clicked") == "Expand MCP Web Fixture capabilities", r)
    r = call("widget_click", target={"web": "MCP web fixture dialog", "kind": "label", **plugins})
    check("its dialog capability is selected", r.get("clicked") == "MCP web fixture dialog", r)
    r = call("widget_click", target={"web": "Config", "kind": "tab", **plugins})
    check("the Config tab opens", r.get("checked") is True, r)
    check("the configuration frame's controls are listed",
          becomes(lambda: 'spin "Speed"' in tree(query="Speed"), "the configuration frame's Speed is listed"),
          lambda: tree(query="Config"))
    r = call("widget_set_value", target={"web": "Speed", **plugins}, value=speed)
    check("widget_set_value reaches into the frame", r.get("value") == str(speed), r)
    r = call("widget_set_value", target={"web": "Enabled", **plugins}, value=True)
    check("a checkbox in the frame", r.get("checked") is True, r)
    save_line = plugins_line('button "Save"')
    check("a frame element's id is listed as its CSS selector", re.search(r'button "Save" name=#save', save_line), save_line)
    save = ref_of(save_line, 'button "Save"')
    r = call("widget_click", target=save)
    check("widget_click reaches into the frame by ref", r.get("clicked") == "Save", r)
    check("and its reply lists what the frame's page changed", r.get("page", {}).get("changed") is True and
          any("saved" in line for line in r["page"].get("added", [])), r)
    config = os.path.join(session["datadir"], "orca_plugins", "config.json")

    def saved():
        try:
            with open(config) as f:
                return any(c.get("cap_config") == {"speed": speed, "enabled": True} for c in json.load(f).get("config", []))
        except (OSError, ValueError):
            return False

    check("the plugin's configuration was saved", becomes(saved, "config.json holds the speed saved"))
    # A press in the configuration frame ends with a click on the dialog's page: the frame runs its own
    # agent, which the page's release reaches.
    def frame_ups():
        return plugins_line("frame up").count("frame up")
    before = frame_ups()
    r = call("mouse_gesture", target={"css": "#save", **plugins}, action="down")
    call("widget_click", target={"web": "Config", "kind": "tab", **plugins})
    check("a click on the page ends a press held in its configuration frame",
          r.get("done") == "down" and becomes(lambda: frame_ups() == before + 1, "the frame logs one more frame up"),
          lambda: [r, before, tree(query="frame up")])
    for action, name in (("up", "an up"), ("move", "a move")):
        before = frame_ups()
        call("mouse_gesture", target={"css": "#save", **plugins}, action="down")
        r = call("mouse_gesture", target={"web": "Config", "kind": "tab", **plugins}, action=action)
        check(f"so does {name} on the page",
              r.get("done") == action and becomes(lambda: frame_ups() == before + 1, "the frame logs one more frame up"),
              lambda: [r, before, tree(query="frame up")])
    # A frame too busy to take its release keeps the press on record; the release it takes late is past
    # its deadline and not run, and the next gesture asks again. The frame stalls on the release itself.
    # (Widget clicks: the busy frame holds its page too.)
    call("widget_click", target={"web": "Stall on release", **plugins})
    before = frame_ups()
    call("mouse_gesture", target={"css": "#save", **plugins}, action="down")
    call("widget_click", target={"label": "Filament", "class": "Label"})
    missed = frame_ups()
    call("widget_click", target={"label": "Filament", "class": "Label"})
    check("a release a busy frame missed is asked again",
          missed == before and becomes(lambda: frame_ups() == before + 1, "the frame logs one more frame up"),
          lambda: [before, missed, tree(query="frame up")])
    # Two unnamed Refresh buttons in the frame and the dialog page's own: the journal's index counts
    # them all, as a replay searches the page with its frame.
    refreshes = plugins_lines('button "Refresh"')
    check("the page's and the frame's Refresh buttons are listed", len(refreshes) == 3, refreshes)
    call("replay_export", clear=True)
    second = refreshes[-1].split("[", 1)[1].split("]", 1)[0] if len(refreshes) == 3 else None
    call("widget_click", target=second)
    clicks = [s for s in call("replay_export").get("replay", {}).get("steps", []) if s.get("tool") == "widget_click"]
    recorded = clicks[-1]["args"]["target"] if clicks else {}
    check("the journal records the frame button's index among every match", recorded.get("index") == 2, recorded)
    call("widget_click", target=recorded)
    frame_log = plugins_line("twin")
    check("and replaying it clicks the same button", frame_log.count("twin 2") == 2 and "twin 1" not in frame_log, frame_log)
    r = call("widget_click", target={"web": "Rewrite", **plugins})
    check("the frame rewrites itself", r.get("clicked") == "Rewrite", r)
    r = call("widget_click", target=save)
    check("a frame ref to a control the frame replaced is stale", "gone from the page" in r.get("error", ""), r)
    r = call("widget_click", target={"css": "#save", **plugins})
    check("the same control by locator works", r.get("clicked") == "Save", r)
    save = ref_of(plugins_line('button "Save"'), 'button "Save"')
    r = call("widget_click", target={"web": "Reload", **plugins})
    check("a click that reloads the frame says its page could not be compared, not that nothing changed",
          r.get("page", {}).get("partial") is True and "changed" not in r["page"], r)
    check("the frame's page reloads",
          becomes(lambda: 'button "Save"' in tree(query="Save"), "the reloaded frame's Save is listed"))
    r = call("widget_click", target=save)
    check("a frame ref from before the frame reloaded is stale", "page changed" in r.get("error", ""), r)
    r = call("widget_click", target={"css": "#save", **plugins})
    check("and the control in the new document by locator works", r.get("clicked") == "Save", r)
    # A configuration frame that fails the search, or does not answer, may hold another match, as such a
    # page may; within can't leave a frame of the matching page out, so it is not suggested.
    call("widget_click", target={"web": "Break", **plugins})
    r = call("widget_click", target={"web": "Config", "kind": "tab", **plugins})
    check("a locator is refused while a configuration frame fails the search", "may not be the only match" in r.get("error", "") and
          "add within" not in r.get("error", ""), r)
    # Last in the Plugins dialog: the muted frame answers nothing more, its own click's reply included.
    call("widget_click", target={"web": "Mute", **plugins}, timeout_s=2)
    r = call("widget_click", target={"web": "Config", "kind": "tab", **plugins})
    check("a locator is refused while a configuration frame does not answer", "may not be the only match" in r.get("error", ""), r)
    r = call("widget_click", target={"web": "Config", "kind": "tab", **plugins, "index": 0})
    check("even with an index", "may not be the only match" in r.get("error", ""), r)
    # Each round asks the muted frame again; what it never answered is given up, not kept: the last
    # round's request, and none of those whose caller stopped waiting (the Mute click's).
    for _ in range(3):
        tree(query="Config")
    plugins_view = {"class": "webview", "within": "Plugins"}

    def relaying():
        return call("webview_run_javascript", code="window.__orcaMcp.call({op: 'doc'}).relaying", target=plugins_view).get("result")

    r = relaying()
    check("the page keeps no more than the last round's unanswered request", r in (0, 1), r)
    # A script calling the agent itself gives no deadline; the request is given up all the same, a second past the
    # tools' default of 10 s. It is a watch, which the page relays outside its rounds: a new round gives up the last
    # round's request, so the listings and the click below leave the watch waiting while they run.
    asked = time.time()
    call("webview_run_javascript", code="window.__orcaMcp.call({op: 'watch', watch: 'no deadline'}).watching", target=plugins_view)
    # A change on the page itself is still told beside the muted frame, as partial. (By ref: a locator
    # is refused while the frame doesn't answer.)
    call("webview_run_javascript", target=plugins_view,
         code="var b = document.createElement('button'); b.textContent = 'Add note'; "
                "b.style.cssText = 'position: fixed; left: 4px; top: 4px; z-index: 99999'; b.onclick = function () { "
                "var p = document.createElement('p'); p.textContent = 'note added'; "
                "p.style.cssText = 'position: fixed; left: 4px; top: 40px; z-index: 99999; background: white'; "
                "document.body.appendChild(p); }; document.body.appendChild(b); 1")
    r = call("widget_click", target=ref_of(tree(query="Add note"), 'button "Add note"'), timeout_s=5)
    check("a page change beside a frame that doesn't answer is listed, as partial",
          r.get("page", {}).get("partial") is True and r["page"].get("changed") is True and 'label "note added"' in r["page"].get("added", []), r)
    # The click's own requests to the frame have its 5 s, so the watch is the last one waiting.
    becomes(lambda: relaying() == 0, "the page relays no request")
    gone = time.time() - asked
    check("a request with no deadline is given up after the tools' default", relaying() == 0 and gone >= 10, f"{gone:.1f} s")

    print(f"{len(failed)} checks failed" if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
