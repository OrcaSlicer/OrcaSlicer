#!/usr/bin/env python3
"""Checks view_set's camera fields, look_at and zoom, on Prepare, Preview and the Design tab, with the
camera_state the reply gives and app_state.camera.

    tools/mcp/orca_mcp.py launch --cad
    tools/mcp/test_set_view_camera.py

Straight after project_new, whose fit runs at the next frame, a look_at and zoom set with a direction
survive the frame a canvas screenshot draws, and so do those given with the assemble view's first
showing, which queues a fit too. A zoom above the camera's limit is clamped and listed under adjusted.
An orbit after a direction ends at the elevation and azimuth it asks for and keeps look_at. Preview
shares Prepare's camera; the Design tab has its own, which takes a look_at and zoom too and orbits.
click_px_per_mm is the zoom over the canvas's scale. A fit on Prepare after the window was sized on the
Home tab fits the new size. On the Home tab, where no 3D view draws, a zoom comes with a note that the
queued fit will replace it. The test starts from start() on the Klipper printer, which gives the perspective
projection and the 1600x900 window. Exits 0 when every check passes.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import orca_mcp  # noqa: E402
from checks import GENERIC_PLA, KLIPPER, KLIPPER_STD, start, wait  # noqa: E402

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "resources", "handy_models", "OrcaCube_v2.drc")
# The Design GUI tests' zoom where a canvas has one GL pixel per click pixel, cad_gui.RESET_ZOOM.
DESIGN_ZOOM = 250 / 1.4**15


def main():
    client, _session = orca_mcp.session_client()
    checks = []

    def check(name, ok, detail=""):
        checks.append((name, bool(ok), detail))

    def camera():
        return client.call("app_state").get("camera", {})

    def view_set(**args):
        """The view_set reply, or {"error": message} when it fails."""
        try:
            return client.call("view_set", **args)
        except orca_mcp.McpError as e:
            return {"error": str(e)}

    def close(a, b, tolerance):
        return isinstance(a, (int, float)) and abs(a - b) <= tolerance

    started = start(client)
    check("start: the Klipper printer with its default process and filament",
          started == {"printer": KLIPPER, "process": KLIPPER_STD, "filaments": [GENERIC_PLA]}, started)
    # The first scenario runs straight after a project_new of its own, while the fit it queues has yet to run.
    client.call("project_new", discard_changes=True)

    # 1. Prepare, straight after project_new: the fit it queued runs first, then look_at and zoom.
    reply = view_set(tab="prepare", camera="top", look_at=[50, 60, 0], zoom=3)
    state = reply.get("camera_state", {})
    check("prepare: the reply has camera_state", state, reply)
    check("prepare: the reply still echoes camera", reply.get("camera") == "top", reply)
    check("prepare: camera_state has the zoom asked for", state.get("zoom") == 3, reply)
    check("prepare: and look_at", state.get("look_at") == [50, 60, 0], reply)
    check("prepare: camera_state looks straight down", state.get("elevation") == 90, reply)
    # A capture of the 3D view itself, not a blank one marked approximate.
    try:
        shot = client.call("window_screenshot", target="canvas", include_image=False)
        check("prepare: the 3D view captures", shot.get("source") == "canvas" and shot.get("width", 0) > 0 and
              shot.get("height", 0) > 0 and "approximate" not in shot, shot)
    except orca_mcp.McpError as e:
        check("prepare: the 3D view captures", False, str(e))
    after = camera()
    check("prepare: app_state.camera keeps the zoom after a frame", after.get("zoom") == 3, after)
    check("prepare: and look_at", after.get("look_at") == [50, 60, 0], after)
    # The projection start() sets; auto_perspective, which would change it with the view's direction, is off.
    check("prepare: camera_state has a projection type", after.get("type") == "perspective", after)

    # 2. Clicks: click_px_per_mm is the zoom over the canvas's scale, 1 at 100 % and 1.5 at 150 % scaling.
    px = after.get("click_px_per_mm")
    ratio = after.get("zoom", 0) / px if px else 0
    check("click_px_per_mm is the zoom over a canvas scale in [1, 3]", 1 - 1e-3 <= ratio <= 3 + 1e-3, after)

    # 3. A zoom above the camera's limit is clamped, and the reply says so.
    reply = view_set(zoom=1000)
    check("clamping: the zoom stops at 250", reply.get("camera_state", {}).get("zoom") == 250, reply)
    check("clamping: adjusted lists zoom", reply.get("adjusted") == ["zoom"], reply)

    # 4. An orbit after a direction ends where it asks and keeps look_at.
    before = camera().get("look_at")
    reply = view_set(camera="front", orbit=[30, 20])
    state = reply.get("camera_state", {})
    check("orbit: the elevation is 20", state.get("elevation") == 20, reply)
    check("orbit: the azimuth is -60", state.get("azimuth") == -60, reply)
    check("orbit: look_at is unchanged", before is not None and state.get("look_at") == before, f"{before} / {reply}")
    check("orbit: the orbit echo is kept", reply.get("orbit") == {"elevation": 20, "azimuth": -60}, reply)
    check("orbit: the camera echo is kept", reply.get("camera") == "front", reply)

    # 5. Preview shares Prepare's camera.
    client.call("dialog_auto_answer", rules=[{"match": "unsaved changes", "button": "no"}])
    client.call("project_import", paths=[os.path.abspath(MODEL)])
    wait(client, "idle")
    sliced = client.await_job(client.call("plate_slice"))
    check("preview: the cube slices", sliced.get("plates", [{}])[0].get("outcome") == "ok", sliced)
    reply = view_set(tab="preview", look_at=[0, 0, 0], zoom=2)
    check("preview: the reply has the zoom", reply.get("camera_state", {}).get("zoom") == 2, reply)
    after = camera()
    check("preview: app_state.camera has the zoom", after.get("zoom") == 2, after)
    check("preview: and look_at", after.get("look_at") == [0, 0, 0], after)

    # 6. The Design tab: its own camera takes a look_at and zoom, and orbits.
    if not client.call("app_state").get("design_tab"):
        check("the Design tab is on (launch --cad)", False)
    else:
        # The Design view's world frame is the Design commands', where the sketch planes' origin,
        # modeling_origin, sits at the Design bed's centre for a new design.
        origin = client.call_command("design_describe")["modeling_origin"]
        reply = view_set(tab="design", camera="top", look_at=origin, zoom=DESIGN_ZOOM)
        state = reply.get("camera_state", {})
        look_at = state.get("look_at") or []
        check("design: the zoom is the one given", close(state.get("zoom"), DESIGN_ZOOM, 1e-3), reply)
        check("design: look_at is modeling_origin",
              len(look_at) == 3 and all(close(t, o, 1e-3) for t, o in zip(look_at, origin)), (origin, reply))
        check("design: the camera echo is kept", reply.get("camera") == "top", reply)
        check("design: app_state.camera is the Design camera", close(camera().get("zoom"), DESIGN_ZOOM, 1e-3), camera())
        reply = view_set(orbit=[10, 0])
        check("design: orbit is taken", "error" not in reply, reply)
        check("design: orbit turns the Design camera, keeping its zoom",
              close(reply.get("camera_state", {}).get("zoom"), DESIGN_ZOOM, 1e-3), reply)

        # 7. The Design camera is its own: Prepare's comes back with its zoom.
        view_set(tab="prepare")
        after = camera()
        check("prepare after design: the zoom is Prepare's again", after.get("zoom") == 2, after)

    # 8. The assemble view's first showing in a session queues a fit of its objects for its first frame,
    # which a look_at and zoom given with it survive. On an app that has shown the view before, nothing is
    # queued and the check holds anyway.
    reply = view_set(canvas="assemble", camera="top", look_at=[10, 20, 0], zoom=4)
    check("assemble: the view shows", "error" not in reply, reply)
    try:
        client.call("window_screenshot", target="canvas", include_image=False)
    except orca_mcp.McpError as e:
        check("assemble: the 3D view captures", False, str(e))
    after = camera()
    check("assemble: the zoom survives the fit its first showing queues", after.get("zoom") == 4, after)
    check("assemble: and look_at", after.get("look_at") == [10, 20, 0], after)
    view_set(canvas="3d")

    # 9. A fit after the window was sized on the Home tab, where no 3D view draws, fits the canvas's new size:
    # the same fit straight after keeps its zoom.
    view_set(tab="home")
    client.call("window_set", size=[1400, 800])
    first = view_set(tab="prepare", camera="fit_objects")
    second = view_set(camera="fit_objects")
    zoom = first.get("camera_state", {}).get("zoom")
    check("resized on home: a fit on prepare uses the new size",
          zoom is not None and close(second.get("camera_state", {}).get("zoom"), zoom, 1e-3), (first, second))

    # 10. On a tab with no 3D view, the fit project_new queues waits for the view to draw: app_state has no
    # camera, a zoom given then is applied with a note that the fit is still queued, and the fit replaces it
    # once the view draws.
    view_set(tab="home")
    client.call("project_new", discard_changes=True)
    state = client.call("app_state")
    check("home: app_state has no camera", "camera" not in state, state.get("camera"))
    reply = view_set(zoom=3)
    check("home: the reply notes the queued fit", "note" in reply, reply)
    view_set(tab="prepare")
    client.call("window_screenshot", target="canvas", include_image=False)
    after = camera()
    check("home: the fit replaces the zoom once the view draws, as the note says", after.get("zoom") != 3, after)

    failed = [c for c in checks if not c[1]]
    for name, ok, detail in checks:
        print(("PASS " if ok else "FAIL ") + name + ("" if ok or detail == "" else f": {detail}"))
    print(f"{len(checks) - len(failed)}/{len(checks)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
