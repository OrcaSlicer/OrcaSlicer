#!/usr/bin/env python3
"""Checks where webview_eval runs while a modal dialog is open, where a script cannot look.

    tools/mcp/orca_mcp.py launch
    tools/mcp/test_webview_modal.py

Scripts run in the web view of the topmost modal dialog only when it is the setup wizard or a preset
bundle dialog: the Flushing volumes dialog is refused by name, so is the Home tab's web view behind a
modal and the bundle dialog's own web view once the export bundle dialog is above it; the export
dialog, topmost, takes scripts. A page that closes its dialog replies closed at once, not after the
timeout. Without a modal, the Home tab's web view still takes scripts. Exits 0 when every check
passes.
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def refusal(**args):
        try:
            client.call("webview_eval", **args)
            return ""
        except orca_mcp.McpError as e:
            return str(e)

    def dialogs():
        return [d["title"] for d in client.call("get_state")["dialogs"]]

    def wait_dialogs(count):
        for _ in range(100):
            if len(dialogs()) == count:
                return True
            time.sleep(0.1)
        return False

    def home_webview():
        snap, texts = client.call_with_text("ui_snapshot", root="main", query="webview")
        for line in (texts[0] if texts else "").splitlines():
            if "webview" in line and "[" in line:
                return line.split("[", 1)[1].split("]", 1)[0]
        return None

    client.call("new_project", discard_changes=True)

    # ---- The Home tab's web view, without and with a modal open ----
    client.call("set_view", tab="home")
    home = home_webview()
    check("the Home tab shows a web view", home is not None)
    try:
        value = client.call("webview_eval", script="1 + 1")["result"]
    except orca_mcp.McpError as e:
        value = str(e)
    check("without a modal, the only web view takes a script", value == 2, value)
    bundle = client.call("run_action", id="orca_command:preset_bundle:orca")
    check("the bundle dialog opens", wait_dialogs(1), dialogs())
    message = refusal(script="1", target=home)
    check("a web view behind the modal is refused", "modal dialog is open" in message, message)
    check("the bundle dialog's page takes a script", client.call("webview_eval", script="document.title")["result"] == "Preset Bundle")

    # ---- A bundle dialog under the export bundle dialog ----
    client.call("webview_eval", script="SendWXMessage(JSON.stringify({command: 'export_page'}))")
    check("the export bundle dialog opens above it", wait_dialogs(2), dialogs())
    message = refusal(script="1", target={"class": "webview", "within": "Preset Bundle"})
    check("the dialog under the topmost is refused", "is under" in message, message)
    url = client.call("webview_eval", script="1")["url"]
    check("without a target the topmost dialog's page is used", "ExportPresetDialog" in url, url)
    client.call("dialog_respond", button="close")
    check("the export dialog closes", wait_dialogs(1), dialogs())

    # ---- A page that closes its dialog replies at once ----
    start = time.time()
    reply = client.call("webview_eval", script="document.getElementById('close_btn').click()")
    elapsed = time.time() - start
    check("closing the dialog from its page replies closed", reply.get("closed") is True, reply)
    check("and does so within a second", elapsed < 1.0, f"{elapsed:.2f} s")
    client.await_job(bundle)
    check("no dialog is left", wait_dialogs(0), dialogs())

    # ---- The Flushing volumes dialog is not on the list ----
    client.call("set_view", tab="prepare")
    client.call("ui_click", target={"label": "Add one filament"})
    flush = client.call("ui_click", target={"label": "Flushing volumes"})
    check("the Flushing volumes dialog opens", wait_dialogs(1), dialogs())
    message = refusal(script="document.title")
    check("its page is refused", "only in the setup wizard and the preset bundle dialogs" in message, message)
    client.call("dialog_respond", button="close")
    client.await_job(flush)
    check("no dialog is left at the end", wait_dialogs(0), dialogs())

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
