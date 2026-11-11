#!/usr/bin/env python3
"""Checks set_view's camera fields, target and zoom, on Prepare, Preview and the Design tab, with the
camera_state the reply gives and get_app_state.camera.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_set_view_camera.py

Straight after new_project, whose fit runs at the next frame, a target and zoom set with a direction
survive the frame a canvas screenshot draws, and so do those given with the assemble view's first
showing, which queues a fit too. A zoom above the camera's limit is clamped and listed under adjusted.
An orbit after a direction ends at the elevation and azimuth it asks for and keeps the target. Preview
shares Prepare's camera; the Design tab has its own, which takes a target and zoom too and orbits.
click_px_per_mm is the zoom over the canvas's scale. A fit on Prepare after the window was sized on the
Home tab fits the new size. On the Home tab, where no 3D view draws, a zoom comes with a note that the
queued fit will replace it. Exits 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "OrcaCube_v2.drc")
# The Design GUI tests' zoom, cad_gui.RESET_ZOOM.
DESIGN_ZOOM = 250 / 1.4**15


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def camera():
        return client.call("get_app_state").get("camera", {})

    def set_view(**args):
        """The set_view reply, or {"error": message} when it fails."""
        try:
            return client.call("set_view", **args)
        except orca_mcp.McpError as e:
            return {"error": str(e)}

    def close(a, b, tolerance):
        return isinstance(a, (int, float)) and abs(a - b) <= tolerance

    client.call("set_window", size=[1920, 1200])
    client.call("new_project", discard_changes=True)

    # 1. Prepare, straight after new_project: the fit it queued runs first, then target and zoom.
    reply = set_view(tab="prepare", camera="top", target=[50, 60, 0], zoom=3)
    state = reply.get("camera_state", {})
    check("prepare: the reply has camera_state", state, reply)
    check("prepare: the reply still echoes camera", reply.get("camera") == "top", reply)
    check("prepare: camera_state has the zoom asked for", state.get("zoom") == 3, reply)
    check("prepare: and the target", state.get("target") == [50, 60, 0], reply)
    check("prepare: camera_state looks straight down", state.get("elevation") == 90, reply)
    try:
        client.call("screenshot", target="canvas", include_image=False)
        check("prepare: the 3D view captures", True)
    except orca_mcp.McpError as e:
        check("prepare: the 3D view captures", False, str(e))
    after = camera()
    check("prepare: get_app_state.camera keeps the zoom after a frame", after.get("zoom") == 3, after)
    check("prepare: and the target", after.get("target") == [50, 60, 0], after)
    check("prepare: camera_state has a projection type", after.get("type") in ("ortho", "perspective"), after)

    # 2. Clicks: click_px_per_mm is the zoom over the canvas's scale, 1 at 100 % and 1.5 at 150 % scaling.
    px = after.get("click_px_per_mm")
    ratio = after.get("zoom", 0) / px if px else 0
    check("click_px_per_mm is the zoom over a canvas scale in [1, 3]", 1 - 1e-3 <= ratio <= 3 + 1e-3, after)

    # 3. A zoom above the camera's limit is clamped, and the reply says so.
    reply = set_view(zoom=1000)
    check("clamping: the zoom stops at 250", reply.get("camera_state", {}).get("zoom") == 250, reply)
    check("clamping: adjusted lists zoom", reply.get("adjusted") == ["zoom"], reply)

    # 4. An orbit after a direction ends where it asks and keeps the target.
    before = camera().get("target")
    reply = set_view(camera="front", orbit=[30, 20])
    state = reply.get("camera_state", {})
    check("orbit: the elevation is 20", state.get("elevation") == 20, reply)
    check("orbit: the azimuth is -60", state.get("azimuth") == -60, reply)
    check("orbit: the target is unchanged", before is not None and state.get("target") == before, f"{before} / {reply}")
    check("orbit: the orbit echo is kept", reply.get("orbit") == {"elevation": 20, "azimuth": -60}, reply)
    check("orbit: the camera echo is kept", reply.get("camera") == "front", reply)

    # 5. Preview shares Prepare's camera.
    client.call("auto_answer_dialogs", rules=[{"match": "unsaved changes", "button": "no"}])
    client.call("open_files", paths=[os.path.abspath(MODEL)])
    client.call("wait", **{"for": "idle"})
    sliced = client.await_job(client.call("slice"))
    check("preview: the cube slices", sliced.get("plates", [{}])[0].get("outcome") == "ok", sliced)
    reply = set_view(tab="preview", target=[0, 0, 0], zoom=2)
    check("preview: the reply has the zoom", reply.get("camera_state", {}).get("zoom") == 2, reply)
    after = camera()
    check("preview: get_app_state.camera has the zoom", after.get("zoom") == 2, after)
    check("preview: and the target", after.get("target") == [0, 0, 0], after)

    # 6. The Design tab: its own camera takes a target and zoom, and orbits.
    if not client.call("get_app_state").get("design_tab"):
        check("the Design tab is on (launch --cad)", False)
    else:
        # The Design view's world frame is the CAD tools', where the sketch planes' origin,
        # modeling_origin, sits at the Design bed's centre for a new design.
        origin = client.call_cad_tool("describe_scene")["modeling_origin"]
        reply = set_view(tab="design", camera="top", target=origin, zoom=DESIGN_ZOOM)
        state = reply.get("camera_state", {})
        target = state.get("target") or []
        check("design: the zoom is the one given", close(state.get("zoom"), DESIGN_ZOOM, 1e-3), reply)
        check("design: the target is modeling_origin",
              len(target) == 3 and all(close(t, o, 1e-3) for t, o in zip(target, origin)), (origin, reply))
        check("design: the camera echo is kept", reply.get("camera") == "top", reply)
        check("design: get_app_state.camera is the Design camera", close(camera().get("zoom"), DESIGN_ZOOM, 1e-3), camera())
        reply = set_view(orbit=[10, 0])
        check("design: orbit is taken", "error" not in reply, reply)
        check("design: orbit turns the Design camera, keeping its zoom",
              close(reply.get("camera_state", {}).get("zoom"), DESIGN_ZOOM, 1e-3), reply)

        # 7. The Design camera is its own: Prepare's comes back with its zoom.
        set_view(tab="prepare")
        after = camera()
        check("prepare after design: the zoom is Prepare's again", after.get("zoom") == 2, after)

    # 8. The assemble view's first showing in a session queues a fit of its objects for its first frame,
    # which a target and zoom given with it survive. On an app that has shown the view before, nothing is
    # queued and the check holds anyway.
    reply = set_view(view="assemble", camera="top", target=[10, 20, 0], zoom=4)
    check("assemble: the view shows", "error" not in reply, reply)
    try:
        client.call("screenshot", target="canvas", include_image=False)
    except orca_mcp.McpError as e:
        check("assemble: the 3D view captures", False, str(e))
    after = camera()
    check("assemble: the zoom survives the fit its first showing queues", after.get("zoom") == 4, after)
    check("assemble: and the target", after.get("target") == [10, 20, 0], after)
    set_view(view="3d")

    # 9. A fit after the window was sized on the Home tab, where no 3D view draws, fits the canvas's new size:
    # the same fit straight after keeps its zoom.
    set_view(tab="home")
    client.call("set_window", size=[1600, 1000])
    first = set_view(tab="prepare", camera="fit_objects")
    second = set_view(camera="fit_objects")
    zoom = first.get("camera_state", {}).get("zoom")
    check("resized on home: a fit on prepare uses the new size",
          zoom is not None and close(second.get("camera_state", {}).get("zoom"), zoom, 1e-3), (first, second))
    client.call("set_window", size=[1920, 1200])

    # 10. On a tab with no 3D view, the fit new_project queues waits for the view to draw: get_app_state has no
    # camera, a zoom given then is applied with a note that the fit is still queued, and the fit replaces it
    # once the view draws.
    set_view(tab="home")
    client.call("new_project", discard_changes=True)
    state = client.call("get_app_state")
    check("home: get_app_state has no camera", "camera" not in state, state.get("camera"))
    reply = set_view(zoom=3)
    check("home: the reply notes the queued fit", "note" in reply, reply)
    set_view(tab="prepare")
    client.call("screenshot", target="canvas", include_image=False)
    after = camera()
    check("home: the fit replaces the zoom once the view draws, as the note says", after.get("zoom") != 3, after)

    client.call("auto_answer_dialogs", rules=[])

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
