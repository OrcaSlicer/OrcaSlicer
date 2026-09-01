#!/usr/bin/env python3
"""A ladder of sketches drawn the way a person draws them: mouse gestures and typed values.

WHY THIS EXISTS, next to scripts/CAD/check-sketch-engine.py. That ladder proves the ENGINE — it feeds
geometry through the MCP socket's add_entities_scripted and grades what comes back. The socket
path skips everything the goal actually rests on: gesture state, the auto-edit queue, snapping,
inference at gesture tolerance, and the right-click offer. A ladder that only drives the socket
cannot say the Design tab meets its goal. This one draws with synthetic clicks and types the
values into the in-canvas field, then reads the result back through the socket, which is used
here ONLY as an instrument, never as an author.

Runs INSIDE the headless rig container (Xvfb :10 + openbox + the app with SNAPORCA_MCP set):

    docker cp scripts/CAD/check-gui-sketching.py snaporca-gui:/tmp/ && \
    docker exec snaporca-gui python3 /tmp/check-gui-sketching.py [rung ...]

With no arguments every rung runs. Exit 0 = every property held.
"""
import json, math, os, re, socket, subprocess, sys, time

SOCK  = os.environ.get("SNAPORCA_MCP", "/tmp/mcp.sock")
DISP  = os.environ.get("DISPLAY", ":10")
_n = 0
_fail = 0
_checks = 0

# ---------------------------------------------------------------- the instrument (read-only)

def call(method, **params):
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(SOCK)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": _n, "method": method,
                           "params": params}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(65536)
        if not d:
            break
        buf += d
    r = json.loads(buf.decode().strip())
    if "error" in r:
        raise RuntimeError(f"{method}: {r['error']['message']}")
    return r["result"]


def try_call(method, **params):
    try:
        return call(method, **params)
    except Exception:
        return None


def describe():
    return call("sketch_describe")


# ---------------------------------------------------------------- the hand (synthetic input)

_win = None

def win():
    """The app window's id and origin. Asked fresh once per run: a relaunch changes the id."""
    global _win
    # BY SIZE, never by title. Saving a project renames the window to the file, and a driver
    # that hunts for "Untitled" then reports "no app window" for an app that is running fine —
    # which is a false negative in the one place a false negative is most expensive.
    if _win is None:
        best = None
        # --class, not --name: after a project is opened the main window can come back with no
        # WM_NAME at all, and a name search then does not list it — the driver picks a 200x200
        # helper and every click lands on nothing.
        for w in sh(f"DISPLAY={DISP} xdotool search --class '.'").split():
            g = sh(f"DISPLAY={DISP} xdotool getwindowgeometry --shell {w}")
            d = dict(l.split("=", 1) for l in g.strip().splitlines() if "=" in l)
            if "WIDTH" not in d:
                continue
            a = int(d["WIDTH"]) * int(d["HEIGHT"])
            if best is None or a > best[0]:
                best = (a, w, int(d["X"]), int(d["Y"]), int(d["WIDTH"]), int(d["HEIGHT"]))
        if best is None:
            die("no app window on " + DISP)
        sh(f"DISPLAY={DISP} xdotool windowactivate --sync {best[1]}")
        _win = best[1:]
    return _win


def sh(cmd):
    return subprocess.run(["bash", "-lc", cmd], capture_output=True, text=True).stdout


def xdo(args):
    sh(f"DISPLAY={DISP} xdotool {args}")


def key(k, pause=0.35):
    xdo(f"key {k}")
    time.sleep(pause)


def typ(s, pause=0.35):
    xdo(f"type --delay 40 -- '{s}'")
    time.sleep(pause)


def click(px, py, pause=0.45, btn=1):
    _, X, Y, _, _ = win()
    xdo(f"mousemove {X+int(px)} {Y+int(py)} click --delay 120 {btn}")
    time.sleep(pause)


def click_ctrl(px, py, pause=0.45):
    """Ctrl+click: EXTEND the sketch selection instead of replacing it.

    A plain second click clears the first pick (DesignSketchTool's Select branch only keeps
    one entity unless `extend` is set), so a two-entity constraint driven by two plain clicks
    silently arrives with one pick and is rejected for the wrong reason.
    """
    _, X, Y, _, _ = win()
    xdo(f"keydown ctrl mousemove {X+int(px)} {Y+int(py)} click --delay 120 1 keyup ctrl")
    time.sleep(pause)


def move(px, py, pause=0.2):
    _, X, Y, _, _ = win()
    xdo(f"mousemove {X+int(px)} {Y+int(py)}")
    time.sleep(pause)


def shot(path):
    w, X, Y, W, H = win()
    sh(f"DISPLAY={DISP} import -window root -crop {W}x{H}+{X}+{Y} +repage {path}")


# ---------------------------------------------------------------- pixels <-> plane

# The viewport is a perspective camera looking at the sketch plane, so pixel -> plane is a
# HOMOGRAPHY, not a scale: the same pixel span covers more millimetres at the far edge than at
# the near one. Four measured correspondences determine it exactly. Measuring beats assuming —
# the camera can be anywhere, and a wrong constant silently puts every click somewhere else.
_H = None            # plane -> pixel, row-major 3x3
_SAFE = None         # (xmin, xmax, ymin, ymax) of the plane region the probes covered


def _solve(A, b):
    """Tiny dense solve; no numpy in the rig container."""
    n = len(A)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        if abs(M[p][c]) < 1e-12:
            die("calibration is degenerate — the four probe points are not in general position")
        M[c], M[p] = M[p], M[c]
        for r in range(n):
            if r == c:
                continue
            f = M[r][c] / M[c][c]
            for k in range(c, n + 1):
                M[r][k] -= f * M[c][k]
    return [M[i][n] / M[i][i] for i in range(n)]


def fit_homography(pairs):
    """pairs: [((X_mm, Y_mm), (u_px, v_px)), ...] -> 3x3 plane->pixel with h22 = 1."""
    A, b = [], []
    for (X, Y), (u, v) in pairs:
        A.append([X, Y, 1, 0, 0, 0, -u * X, -u * Y]); b.append(u)
        A.append([0, 0, 0, X, Y, 1, -v * X, -v * Y]); b.append(v)
    h = _solve(A, b)
    return [h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], 1.0]


def px(X, Y):
    """Plane millimetres -> window pixels."""
    h = _H
    w = h[6] * X + h[7] * Y + h[8]
    return ((h[0] * X + h[1] * Y + h[2]) / w, (h[3] * X + h[4] * Y + h[5]) / w)


def unpx(u, v):
    """Window pixels -> plane millimetres (the homography inverted, by hand)."""
    h = _H
    A = [[h[0] - u * h[6], h[1] - u * h[7]], [h[3] - v * h[6], h[4] - v * h[7]]]
    b = [u * h[8] - h[2], v * h[8] - h[5]]
    return tuple(_solve(A, b))


def mm_per_px(X, Y):
    """The viewport's local scale at a plane point — what the tool calls unit_per_px."""
    u, v = px(X, Y)
    a = unpx(u, v)
    b = unpx(u + 1.0, v)
    return math.dist(a, b)


def clickmm(X, Y, pause=0.45, btn=1):
    u, v = px(X, Y)
    click(u, v, pause, btn)


def movemm(X, Y, pause=0.2):
    u, v = px(X, Y)
    move(u, v, pause)


# ---------------------------------------------------------------- session control

def leave_sketch():
    """Back to a clean Feature-mode document, whatever state the last rung left behind."""
    try_call("sketch_cancel")
    for _ in range(4):
        key("Escape", 0.25)
    time.sleep(0.5)


# EVERY absolute chrome coordinate below is written in the Snapmaker fork's layout and then
# shifted by CHROME_DY, because this fork keeps mainline OrcaSlicer's top row (File / save /
# undo / redo / Calibration, with the document title) which that fork does not have. The whole
# chrome — tab strip, sketch toolbar, confirm button — sits 26 px lower here.
#
# Getting this wrong does not look like a coordinate problem. At the unshifted y the Design tab
# click landed in that toolbar, the app stayed on the Home page, and the first rung reported
# "no sketch opened after plane click + Shift+S"; the unshifted Construction checkbox reported
# "0 construction axis". Both name the wrong subsystem. Canvas coordinates are immune because
# clickmm() derives them from the live canvas geometry — only the chrome constants need this.
CHROME_DY = int(os.environ.get("SNAPORCA_CHROME_DY", "26"))

DESIGN_TAB = (128, 29 + CHROME_DY)

# Feature-tree rows, measured on the rig at 1920x1080: first row centre, then 23 px apart.
# x=300, not the label: a second click ON the label opens the inline rename, and Delete then
# edits the text instead of removing the feature.
#
# CHROME_DY applies here too, and this was the one chrome constant that did not carry it. The
# unshifted click lands 26 px BELOW the first row -- just past its 23 px height -- so the row is
# never selected, Delete does nothing, and reset_document spends 40 rounds on it before dying
# with "could not empty the feature tree". That names the feature tree, which is not the fault.
TREE_ROW0 = (300, 215 + CHROME_DY)


def go_design():
    """Make sure the Design tab is in front — loading a project lands on Prepare."""
    click(*DESIGN_TAB, pause=1.0)


def reset_document():
    """Delete every committed feature, by picking its tree row and pressing Delete.

    A rung that ends in Constrain COMMITS its sketch, and the next rung's Constrain resolves
    'the last sketch' — which is then the PREVIOUS rung's. That is how D2 first read a rectangle
    of exactly 120 x 80 back from a sketch it had drawn at 120.020087: it was grading a sketch
    left behind by an earlier run. The document is part of the fixture; reset it like one.
    """
    go_design()
    leave_sketch()
    for _ in range(40):
        if not call("describe_scene")["features"]:
            return
        click(*TREE_ROW0, pause=0.35)
        key("Delete", 0.5)
    die("could not empty the feature tree")


def enter_sketch(tool_key, plane_px=(913, 359)):
    """Enter a sketch the way the design law says: pick the plane in the viewport, then the tool.

    Shift+S enters sketch MODE and pops the offer; Escape dismisses it; the tool letter then
    starts the session on the plane the click selected. All four steps are real input — nothing
    here goes through the socket.
    """
    leave_sketch()
    click(*plane_px)
    key("shift+s", 0.8)
    key("Escape", 0.4)          # entering sketch mode pops the offer; dismiss it
    key("p", 0.6)
    if try_call("sketch_describe") is None:
        shot("/shots/gl-enter-failed.png")
        die("no sketch opened after plane click + Shift+S (see /shots/gl-enter-failed.png)")
    calibrate_here()            # THIS sketch's own camera map, on THIS sketch's own plane
    key(tool_key, 0.6)


def calibrate_here():
    """Place four Points in the sketch that is already open, solve the map, then undo them.

    PER SKETCH, not once per run. The camera is wherever the previous rung left it — reopening a
    sketch and loading a project both move it — and the plane label the entry click lands on
    moves with it, so a later sketch can end up on XZ while the map was solved on XY. Both of
    those turn into clicks that land somewhere else, and geometry that looks drawn but is not
    where it was asked for. Four points cost about four seconds and remove the whole class.
    """
    global _H
    probes = [(1000, 500), (1400, 500), (1400, 760), (1000, 760)]
    for u, v in probes:
        click(u, v)
    ents = describe()["entities"]
    if len(ents) != 4 or any(e["type"] != "point" for e in ents):
        die(f"calibration expected 4 points, got {[e['type'] for e in ents]}")
    _H = fit_homography([((e["p"][0], e["p"][1]), probes[i]) for i, e in enumerate(ents)])
    # Prove the fit by round-tripping the probes: a homography through its own four points is
    # exact, so anything but a sub-pixel residual means the points came back mismatched.
    for i, e in enumerate(ents):
        u, v = px(e["p"][0], e["p"][1])
        if abs(u - probes[i][0]) > 0.5 or abs(v - probes[i][1]) > 0.5:
            die(f"calibration residual too large at probe {i}: {(u, v)} vs {probes[i]}")
    global _SAFE
    xs = [e["p"][0] for e in ents]; ys = [e["p"][1] for e in ents]
    _SAFE = (min(xs), max(xs), min(ys), max(ys))
    for _ in range(len(probes)):
        key("ctrl+z", 0.5)                 # the probes are scaffolding, not geometry
    left = describe()["entities"]
    if left:
        die(f"{len(left)} calibration probes survived the undo")


# ---------------------------------------------------------------- typed values

# How long to wait for the in-canvas field to appear and to settle after a commit. The queue
# opens each field from a CallAfter that runs AFTER a re-solve, so on a heavy sketch the field is
# simply not there yet when a fast driver starts typing — the digits go nowhere and the value
# stays as drawn. Rungs that work on a thousand entities raise this.
PACE = 1.0


def field_win():
    """The open in-canvas value field as (x, y, w, h) in SCREEN pixels, or None.

    It is a top-level window of its own, not a child of the canvas (a native child cannot be
    composited over the double-buffered wxGLCanvas), so it is found by enumerating windows rather
    than by looking inside the app's frame. Two other small top-levels exist: the status chip,
    which lives on the bottom edge, and 1x1/10x10 helpers.
    """
    _, X, Y, W, H = win()
    for w in sh(f"DISPLAY={DISP} xdotool search --onlyvisible --class '.'").split():
        g = dict(l.split("=", 1) for l in
                 sh(f"DISPLAY={DISP} xdotool getwindowgeometry --shell {w}").strip().splitlines()
                 if "=" in l)
        if "WIDTH" not in g:
            continue
        x, y, ww, hh = int(g["X"]), int(g["Y"]), int(g["WIDTH"]), int(g["HEIGHT"])
        if ww >= W or hh < 24 or hh > 120 or ww < 40:
            continue
        if y > Y + H - 80:                 # the status chip, pinned to the bottom edge
            continue
        return (x, y, ww, hh)
    return None


def focus_field():
    """Put the keyboard in the value field, by clicking it.

    WITHOUT THIS THE TYPED VALUE IS SILENTLY DISCARDED. The field is shown and raised but the
    window manager does not give it the keyboard, so xdotool's digits go to the canvas and Return
    commits the value the field opened with — the pre-filled as-drawn number. The failure is
    invisible from the outside: a constraint IS created, the solve succeeds, and the sketch simply
    holds the dimension you did not ask for (typed 40, got 54.94). One click fixes it.
    """
    r = field_win()
    if r is None:
        return False
    x, y, w, h = r
    xdo(f"mousemove {x + w // 2} {y + h // 2} click --delay 120 1")
    time.sleep(0.3)
    return True


def value(v, pause=0.6):
    """Type one number into the open in-canvas field and commit it.

    Select-all first: the field opens pre-filled with the as-drawn value and pre-selected, but a
    pre-selection that a synthetic click has disturbed would otherwise leave the typed digits
    appended to it.
    """
    time.sleep(0.25 * PACE)
    focus_field()
    key("ctrl+a", 0.15)
    typ(str(v), 0.25)
    key("Return", pause * PACE)


def values(*vs):
    for v in vs:
        value(v)


# ---------------------------------------------------------------- grading

def say(msg):
    print(f"  {msg}")


def check(kind, cond, what):
    global _fail, _checks
    _checks += 1
    if cond:
        print(f"    {kind:9s} ok    {what}")
    else:
        print(f"    {kind:9s} FAIL  {what}", file=sys.stderr)
        _fail += 1


def near(a, b, tol=1e-6):
    return abs(a - b) <= tol


def die(msg):
    print(f"    FATAL {msg}", file=sys.stderr)
    sys.exit(2)


def lengths(ents):
    return sorted(round(e["length"], 6) for e in ents if e["type"] == "line")


def loops():
    return describe()["closed_loops"]


# =================================================================== LADDER A — one tool each
# Every 2D tool draws its primitive by gesture, then takes its exact value from the keyboard.
# The click only has to be roughly right; the typed number is what must come back exactly.

def rung_rect():
    print("\nA1  rectangle — two corners, typed 120 x 80")
    enter_sketch("r")
    clickmm(-60, -40); clickmm(60, 40)
    values(120, 80)
    d = describe()
    ls = lengths(d["entities"])
    check("LENGTH", ls == [80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    check("AREA", near(abs(lp[0]["area"]), 9600.0, 1e-6), f"area {abs(lp[0]['area']):.6f}")
    check("VERTEX", all(near(abs(e["p1"][0] - e["p0"][0]), 0, 1e-9)
                        or near(abs(e["p1"][1] - e["p0"][1]), 0, 1e-9)
                        for e in d["entities"]), "every side axis-aligned")
    leave_sketch()


def rung_circle():
    print("\nA2  circle — centre then rim, typed radius 25")
    enter_sketch("c")
    clickmm(0, 0); clickmm(30, 0)
    values(25)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "circle"]
    check("ARC", len(e) == 1 and near(e[0]["radius"], 25.0), f"radius {e[0]['radius'] if e else None}")
    check("VERTEX", len(e) == 1 and near(e[0]["center"][0], 0.0, 0.6) and near(e[0]["center"][1], 0.0, 0.6),
          f"centre {e[0]['center'] if e else None} at the clicked origin")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), math.pi * 625.0, 1e-6),
          f"area {abs(lp[0]['area']):.6f} vs pi r^2 {math.pi*625:.6f}")
    leave_sketch()


def rung_line():
    print("\nA3  line — two clicks, typed length 50 and angle 30")
    enter_sketch("l")
    clickmm(-40, -20); clickmm(10, 5)
    values(50, 30)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "line"]
    check("LENGTH", len(e) == 1 and near(e[0]["length"], 50.0), f"length {e[0]['length'] if e else None}")
    if e:
        a = math.degrees(math.atan2(e[0]["p1"][1] - e[0]["p0"][1], e[0]["p1"][0] - e[0]["p0"][0])) % 360.0
        check("ANGLE", near(a, 30.0, 1e-9), f"angle {a:.9f} deg")
    leave_sketch()


def rung_arc():
    print("\nA4  three-point arc — typed radius 40 and sweep 90")
    enter_sketch("a")
    clickmm(-40, 0); clickmm(40, 0); clickmm(0, 40)
    values(40, 90)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "arc"]
    check("ARC", len(e) == 1 and near(e[0]["radius"], 40.0), f"radius {e[0]['radius'] if e else None}")
    if e:
        sw = abs(e[0]["end_angle"] - e[0]["start_angle"]) * 180.0 / math.pi
        # 1e-7 deg, not exact: the sweep is READ BACK as end_angle - start_angle, two atan2
        # results, where the line's angle is STORED as the direction it was given. A 1e-9 deg
        # residual here is 7e-10 mm at r=40 — float round-trip, not a defect.
        check("ANGLE", near(sw, 90.0, 1e-7), f"sweep {sw:.9f} deg")
        ch = math.dist(e[0]["p0"], e[0]["p1"])
        check("VERTEX", near(ch, 40.0 * math.sqrt(2.0), 1e-6),
              f"chord {ch:.6f} vs r*sqrt2 {40*math.sqrt(2):.6f}")
    leave_sketch()


def rung_slot():
    print("\nA5  slot — typed centre distance 60, radius 10, angle 0")
    enter_sketch("s")
    clickmm(-30, 0); clickmm(30, 0); clickmm(30, 12)
    values(60, 10, 0)
    d = describe()
    arcs = [x for x in d["entities"] if x["type"] == "arc"]
    lns  = [x for x in d["entities"] if x["type"] == "line"]
    check("ARC", len(arcs) == 2 and all(near(a["radius"], 10.0) for a in arcs),
          f"two end radii {[round(a['radius'], 9) for a in arcs]}")
    check("LENGTH", len(lns) == 2 and all(near(l["length"], 60.0) for l in lns),
          f"two flanks {[round(l['length'], 9) for l in lns]}")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), 60 * 20 + math.pi * 100, 1e-6),
          f"area {abs(lp[0]['area']):.6f} vs 60*20+pi*100 {60*20+math.pi*100:.6f}")
    leave_sketch()


def rung_polygon():
    print("\nA6  polygon — typed side 30, angle 0")
    enter_sketch("g")
    clickmm(0, 0); clickmm(35, 0)
    values(30, 0)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "line"]
    ls = lengths(d["entities"])
    check("LENGTH", len(e) >= 3 and all(near(l, 30.0, 1e-9) for l in ls),
          f"{len(e)} equal sides {set(ls)}")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    if e:
        n = len(e)
        want = n * 30.0 ** 2 / (4.0 * math.tan(math.pi / n))
        check("AREA", near(abs(lp[0]["area"]), want, 1e-6),
              f"area {abs(lp[0]['area']):.6f} vs regular {n}-gon {want:.6f}")
    leave_sketch()


def rung_ellipse():
    print("\nA7  ellipse — typed major 50, minor 20")
    enter_sketch("e")
    clickmm(0, 0); clickmm(40, 0); clickmm(0, 15)
    values(50, 20)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "ellipse"]
    check("ARC", len(e) == 1, f"{len(e)} ellipse")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), math.pi * 50 * 20, 2e-2),
          f"area {abs(lp[0]['area']):.4f} vs pi*a*b {math.pi*1000:.4f}")
    leave_sketch()


def rung_point():
    print("\nA8  point — one click, no value to type")
    enter_sketch("p")
    clickmm(20, 10)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "point"]
    check("VERTEX", len(e) == 1 and near(e[0]["p"][0], 20.0, 0.6) and near(e[0]["p"][1], 10.0, 0.6),
          f"placed at {[round(v, 3) for v in e[0]['p']] if e else None}")
    leave_sketch()


def rung_spline():
    print("\nA9  spline — click control points, right-click to end")
    enter_sketch("b")
    for p in [(-40, 0), (-15, 30), (15, -30), (40, 0)]:
        clickmm(*p)
    clickmm(40, 0, btn=3)
    d = describe()
    e = [x for x in d["entities"] if x["type"] == "spline"]
    check("VERTEX", len(e) == 1, f"{len(e)} spline from 4 control points")
    leave_sketch()


# =================================================================== LADDER B — voids by hand
# The strategic target itself: one closed outer loop with internal voids, every one of them
# drawn by gesture in a single sketch and given its size from the keyboard.

def rung_voids():
    print("\nB1  closed profile with two internal voids, all by gesture")
    enter_sketch("r")
    clickmm(-60, -40); clickmm(60, 40)          # outer 120 x 80
    values(120, 80)
    key("r", 0.6)                                # same tool again, from the keyboard
    clickmm(-45, -15); clickmm(-5, 15)           # void 1: 40 x 30
    values(40, 30)
    key("c", 0.6)
    clickmm(30, 0); clickmm(42, 0)               # void 2: circle r 10
    values(10)
    d = describe()
    ls = lengths(d["entities"])
    check("LENGTH", ls == [30.0, 30.0, 40.0, 40.0, 80.0, 80.0, 120.0, 120.0], f"sides {ls}")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 3 and all(l["closed"] for l in lp), f"{len(lp)} closed loops")
    check("CLOSED", d["buildable"] and not d["open_ends"], "buildable, nothing dangling")
    # The void attribution is the property under test: the outer loop must OWN both inner ones,
    # and neither inner loop may claim a hole of its own.
    outer = max(range(len(lp)), key=lambda i: abs(lp[i]["area"]))
    holes = sorted(lp[outer]["holes"])
    check("VOID", holes == sorted(i for i in range(len(lp)) if i != outer),
          f"outer loop {outer} owns holes {holes}")
    check("VOID", all(not lp[i]["holes"] for i in range(len(lp)) if i != outer),
          "neither void claims a hole of its own")
    a = {i: abs(lp[i]["area"]) for i in range(len(lp))}
    check("AREA", near(a[outer], 9600.0, 1e-6), f"outer {a[outer]:.6f}")
    inner = sorted(a[i] for i in a if i != outer)
    check("AREA", near(inner[0], math.pi * 100, 1e-6) and near(inner[1], 1200.0, 1e-6),
          f"voids {inner[0]:.6f} (pi*100) and {inner[1]:.6f} (40*30)")
    net = a[outer] - sum(v for i, v in a.items() if i != outer)
    check("AREA", near(net, 9600.0 - 1200.0 - math.pi * 100, 1e-6), f"net material {net:.6f}")
    leave_sketch()


# =================================================================== LADDER C — combining
# Mirror, offset, trim, extend, fillet and chamfer, each driven by the same picks and the same
# on-geometry value label a person would use. The label's place is COMPUTED from the geometry
# the tool itself derives (render_op_gizmo: tip = anchor + dir * value, label = tip + dir * 1.2
# * max(15 * unit_per_px, 1e-4)) rather than hunted for in the pixels — the tool's own formula
# is the only thing that can be right by construction.

def op_label_mm(anchor, direction, value, at):
    th = max(15.0 * mm_per_px(*at), 1e-4)
    d = (direction[0] / math.hypot(*direction), direction[1] / math.hypot(*direction))
    tip = (anchor[0] + d[0] * value, anchor[1] + d[1] * value)
    return (tip[0] + d[0] * th * 1.2, tip[1] + d[1] * th * 1.2)


def mid(e):
    return ((e["p0"][0] + e["p1"][0]) / 2.0, (e["p0"][1] + e["p1"][1]) / 2.0)


def corner_of(a, b):
    """The shared endpoint of two adjacent lines, and the bisector pointing into their wedge."""
    C = min(((pa, pb) for pa in (a["p0"], a["p1"]) for pb in (b["p0"], b["p1"])),
            key=lambda t: math.dist(t[0], t[1]))[0]
    def away(e):
        f = e["p1"] if math.dist(e["p0"], C) < math.dist(e["p1"], C) else e["p0"]
        n = math.dist(f, C)
        return ((f[0] - C[0]) / n, (f[1] - C[1]) / n)
    ua, ub = away(a), away(b)
    bis = (ua[0] + ub[0], ua[1] + ub[1])
    return tuple(C), bis


def draw_rect(w, h, x0, y0):
    clickmm(x0, y0); clickmm(x0 + w, y0 + h)
    values(w, h)


def rung_fillet():
    print("\nC1  fillet — pick two legs, type radius 8 on the label")
    enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    d0 = describe()["entities"]
    a, b = corner_pair(d0)
    key("f", 0.6)
    clickmm(*mid(a)); clickmm(*mid(b))
    C, bis = corner_of(a, b)
    v0 = 0.2 * min(a["length"], b["length"])
    clickmm(*op_label_mm(C, bis, v0, C))
    values(8)
    d = describe()
    arcs = [x for x in d["entities"] if x["type"] == "arc"]
    check("ARC", len(arcs) == 1 and near(arcs[0]["radius"], 8.0), f"radius {arcs[0]['radius'] if arcs else None}")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    want = 9600.0 - 64.0 * (1.0 - math.pi / 4.0)
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), want, 1e-6),
          f"area {abs(lp[0]['area']):.6f} vs 9600 - r^2(1-pi/4) {want:.6f}")
    if arcs:
        # TANGENT: the arc centre must sit exactly r from each surviving leg's line.
        legs = [x for x in d["entities"] if x["type"] == "line"]
        ds = sorted(point_line_dist(arcs[0]["center"], l) for l in legs)[:2]
        check("TANGENT", all(near(x, 8.0, 1e-9) for x in ds), f"centre stands off both legs by {ds}")
    leave_sketch()


def rung_chamfer():
    print("\nC2  chamfer — pick two legs, type distance 10 on the label")
    enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    d0 = describe()["entities"]
    a, b = corner_pair(d0)
    key("h", 0.6)
    clickmm(*mid(a)); clickmm(*mid(b))
    C, bis = corner_of(a, b)
    clickmm(*op_label_mm(C, bis, 0.2 * min(a["length"], b["length"]), C))
    values(10)
    d = describe()
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    check("LENGTH", len(lp) == 1 and len(lp[0]["entities"]) == 5,
          f"{len(lp[0]['entities'])} sides after the cut")
    ls = sorted(e["length"] for e in d["entities"] if e["type"] == "line")
    check("LENGTH", any(near(x, 10.0 * math.sqrt(2.0), 1e-9) for x in ls),
          f"the new face is d*sqrt2 = {10*math.sqrt(2):.9f}; sides {[round(x,9) for x in ls]}")
    check("LENGTH", near(ls[1], 70.0, 1e-9) and near(ls[3], 110.0, 1e-9),
          "both legs shortened by exactly d")
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), 9600.0 - 50.0, 1e-6),
          f"area {abs(lp[0]['area']):.6f} vs 9600 - d^2/2")
    leave_sketch()


def rung_offset():
    print("\nC3  offset — pick a circle, type 5 on the label")
    enter_sketch("c")
    clickmm(0, 0); clickmm(30, 0)
    values(25)
    key("o", 0.6)
    clickmm(25, 0)                                  # pick the rim
    # Circle offset anchors at centre + (r, 0) and grows along +x; the starting value is 0.1 * 2r.
    clickmm(*op_label_mm((25.0, 0.0), (1.0, 0.0), 0.1 * 50.0, (25.0, 0.0)))
    values(5)
    d = describe()
    cs = sorted(x["radius"] for x in d["entities"] if x["type"] == "circle")
    # The gizmo's arrow starts on the +x side, so the typed 5 lands OUTWARD; what the goal cares
    # about is that the separation is exactly the number typed, on whichever side it was given.
    check("ARC", len(cs) == 2 and near(cs[0], 25.0) and near(cs[1] - cs[0], 5.0),
          f"radii {cs} — separated by exactly {cs[1]-cs[0] if len(cs)==2 else None}")
    lp = d["closed_loops"]
    check("CLOSED", len(lp) == 2 and all(l["closed"] for l in lp), f"{len(lp)} closed loops")
    check("VOID", any(l["holes"] for l in lp), "the inner circle is read as a void of the outer")
    leave_sketch()


def cross(a, b):
    """Where two lines' infinite supports meet."""
    (x1, y1), (x2, y2) = a["p0"], a["p1"]
    (x3, y3), (x4, y4) = b["p0"], b["p1"]
    d = (x2 - x1) * (y4 - y3) - (y2 - y1) * (x4 - x3)
    t = ((x3 - x1) * (y4 - y3) - (y3 - y1) * (x4 - x3)) / d
    return (x1 + t * (x2 - x1), y1 + t * (y2 - y1))


def mid_of(a, b):
    return ((a[0] + b[0]) / 2.0, (a[1] + b[1]) / 2.0)


def point_line_dist(p, l):
    (x0, y0), (x1, y1) = l["p0"], l["p1"]
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    return abs((p[0] - x0) * dy - (p[1] - y0) * dx) / n


def corner_pair(ents):
    """Two adjacent lines of a rectangle: the first line and the one sharing an endpoint."""
    ls = [e for e in ents if e["type"] == "line"]
    a = ls[0]
    for b in ls[1:]:
        if min(math.dist(pa, pb) for pa in (a["p0"], a["p1"]) for pb in (b["p0"], b["p1"])) < 1e-6:
            return a, b
    die("no adjacent pair in what should be a rectangle")


CONSTRUCTION_CHECKBOX = (419, 75 + CHROME_DY)


def draw_line(x0, y0, x1, y1, length, angle):
    clickmm(x0, y0); clickmm(x1, y1)
    values(length, angle)


def rung_mirror():
    print("\nC4  mirror — a half profile reflected about a construction axis")
    enter_sketch("l")
    click(*CONSTRUCTION_CHECKBOX)                    # the axis is reference, not material
    key("l", 0.6)
    draw_line(0, -40, 0, 40, 80, 90)                 # the axis, on x = 0
    click(*CONSTRUCTION_CHECKBOX)                    # back to real geometry
    key("l", 0.6)
    draw_line(0, -40, 50, -40, 50, 0)
    key("l", 0.6)
    draw_line(50, -40, 50, 40, 80, 90)
    key("l", 0.6)
    draw_line(50, 40, 0, 40, 50, 180)
    d0 = describe()["entities"]
    axis = [e for e in d0 if e.get("construction")]
    check("VERTEX", len(axis) == 1, f"{len(axis)} construction axis")
    half = [e for e in d0 if e["type"] == "line" and not e.get("construction")]
    check("LENGTH", len(half) == 3, f"{len(half)} lines in the half profile")
    key("m", 0.6)
    clickmm(*mid(axis[0]))
    for e in half:
        clickmm(*mid(e))
    clickmm(-90, 60)                                 # empty space confirms
    d = describe()
    real = [e for e in d["entities"] if e["type"] == "line" and not e.get("construction")]
    check("LENGTH", len(real) == 6, f"{len(real)} lines after the reflection")
    lp = [l for l in d["closed_loops"]]
    check("CLOSED", len(lp) == 1 and lp[0]["closed"], f"{len(lp)} closed loop(s)")
    # Graded against the geometry ACTUALLY DRAWN, not against the coordinates I aimed at. A
    # synthetic click lands on a whole pixel, so the half profile sits a few tenths of a
    # millimetre off the origin; the typed values fix its lengths and angles, not its anchor.
    # Demanding 8000.000000 here would grade my aim, and the mirror is what is under test.
    far = max(half, key=lambda e: e["length"])            # the edge parallel to the axis
    w   = point_line_dist(mid(far), axis[0])
    want = 2.0 * w * far["length"]
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), want, 1e-6),
          f"area {abs(lp[0]['area']):.6f} vs 2 x {w:.6f} x {far['length']:.6f} = {want:.6f}")
    # SYMMETRY is the property this rung exists for: every vertex must have its exact reflection
    # ABOUT THE AXIS THAT WAS DRAWN.
    vs = [tuple(p) for e in real for p in (e["p0"], e["p1"])]
    def refl(q):
        (ax, ay), (bx, by) = axis[0]["p0"], axis[0]["p1"]
        dx, dy = bx - ax, by - ay
        n = dx * dx + dy * dy
        t = ((q[0] - ax) * dx + (q[1] - ay) * dy) / n
        fx, fy = ax + t * dx, ay + t * dy
        return (2 * fx - q[0], 2 * fy - q[1])
    missing = [v for v in vs if not any(math.dist(refl(v), o) < 1e-9 for o in vs)]
    check("SYMMETRY", not missing,
          f"every one of {len(vs)} vertices has its exact reflection about the drawn axis")
    leave_sketch()


def rung_trim():
    print("\nC5  trim — cut one arm off a crossing")
    enter_sketch("l")
    draw_line(-50, 0, 50, 0, 100, 0)
    key("l", 0.6)
    draw_line(0, -50, 0, 50, 100, 90)
    d0 = describe()["entities"]
    horiz = min(d0, key=lambda e: abs(e["p1"][1] - e["p0"][1]))
    vert  = max(d0, key=lambda e: abs(e["p1"][1] - e["p0"][1]))
    X = cross(horiz, vert)
    left = min(horiz["p0"], horiz["p1"])              # the end that must survive
    want = math.dist(left, X)
    key("t", 0.6)
    clickmm(*mid_of(X, max(horiz["p0"], horiz["p1"])))   # the arm on the far side of the crossing
    d = describe()
    ls = sorted(round(e["length"], 9) for e in d["entities"] if e["type"] == "line")
    check("LENGTH", len(ls) == 2 and near(ls[1], vert["length"], 1e-9) and near(ls[0], want, 1e-9),
          f"lengths {ls} — the picked arm is gone at the crossing (expected {want:.9f}), "
          f"the other line untouched")
    ends = [tuple(p) for e in d["entities"] if e["type"] == "line" for p in (e["p0"], e["p1"])]
    check("VERTEX", any(math.dist(X, q) < 1e-9 for q in ends), "the cut lands exactly on the crossing")
    leave_sketch()


def rung_extend():
    print("\nC6  extend — reach a line to the one it stops short of")
    enter_sketch("l")
    draw_line(-50, 0, -10, 0, 40, 0)
    key("l", 0.6)
    draw_line(0, -50, 0, 50, 100, 90)
    d0 = describe()["entities"]
    short = min(d0, key=lambda e: e["length"])
    vert  = max(d0, key=lambda e: e["length"])
    X = cross(short, vert)
    far = min((short["p0"], short["p1"]), key=lambda q: q[0])   # the end that stays put
    near_end = max((short["p0"], short["p1"]), key=lambda q: q[0])
    want = math.dist(far, X)
    key("x", 0.6)
    clickmm(*mid_of(near_end, mid_of(far, near_end)))           # click the end that must grow
    d = describe()
    ls = sorted(round(e["length"], 9) for e in d["entities"] if e["type"] == "line")
    check("LENGTH", len(ls) == 2 and near(ls[0], want, 1e-9),
          f"lengths {ls} — {short['length']:.6f} grew to exactly {want:.9f}")
    ends = [tuple(p) for e in d["entities"] if e["type"] == "line" for p in (e["p0"], e["p1"])]
    check("VERTEX", any(math.dist(X, q) < 1e-9 for q in ends),
          "the new end sits exactly on the target line")
    leave_sketch()


# =================================================================== LADDER D — dimensions
# A drawn shape with no numbers on it, then numbers put on it by hand: the Dimension tool for a
# value, the Constrain buttons for a relation. Both must hold the value they were given AND take
# the degrees of freedom away — a dimension that moves the geometry but leaves the DoF standing
# has not constrained anything, it has only nudged it.

# Constrain-mode toolbar, measured off the rig at 1920x1080 (icon centres, 42 px apart).
#
# THIS LIST MIRRORS THE cbtn() SEQUENCE IN DesignPanel.cpp AND HAS TO BE UPDATED WHEN A BUTTON
# IS INSERTED. Positions are computed by index, so inserting a button shifts every entry after
# it and the map silently points at the wrong icon -- a rung then applies some OTHER constraint
# and fails with a geometric message that says nothing about buttons.
#
# It has already happened once, undetected: equal_radius and collinear (after "equal"), sym_v
# and sym_h (after "symmetric") and dist_x and dist_y (after "fix") were added while this list
# still had 14 names. Everything from index 6 on was wrong in both forks. Nothing caught it
# because the ladder only ever clicks "perpendicular" (3) and "equal" (5), both of which sit
# before the first insertion. The next rung to use "tangent" would have clicked "collinear".
CON_BTN_Y = 76 + CHROME_DY
CON_BTN = {n: (449 + 42 * i, CON_BTN_Y) for i, n in enumerate(
    ["horizontal", "vertical", "parallel", "perpendicular", "coincident", "equal",
     "equal_radius", "collinear", "concentric", "tangent", "midpoint", "symmetric",
     "sym_v", "sym_h", "angle", "radius", "diameter", "fix", "dist_x", "dist_y"])}

# The SAME twenty buttons, at their SKETCH-mode x. Constraining during a sketch put the group
# after the (wide) sketch toolbar instead of at the start of an otherwise empty row, so every
# button sits 228 px further right. Measured, not derived: the strip was screenshotted in sketch
# mode and the icon columns detected — first centre 677, pitch 42, twenty of them. Deriving it
# from the Constrain-mode map is exactly how this table drifted the last time.
# NOTE FOR THIS FORK: 677 was measured on the OTHER fork's rig. Y is safe (it rides CON_BTN_Y,
# which already carries CHROME_DY), but the X start depends on how wide the sketch toolbar to the
# left of this group renders, and this fork keeps mainline's top row. Re-measure before trusting
# D11 here: screenshot in sketch mode and detect the icon columns, do not derive it by offset.
CON_BTN_SKETCH = {n: (677 + 42 * i, CON_BTN_Y) for i, n in enumerate(
    ["horizontal", "vertical", "parallel", "perpendicular", "coincident", "equal",
     "equal_radius", "collinear", "concentric", "tangent", "midpoint", "symmetric",
     "sym_v", "sym_h", "angle", "radius", "diameter", "fix", "dist_x", "dist_y"])}


def draw_rect_undimensioned():
    """A rectangle by two clicks, with both queued value fields dismissed (Esc keeps it as drawn)."""
    clickmm(-60, -40); clickmm(60, 40)
    key("Escape", 0.7)      # Width  — keep as drawn
    key("Escape", 0.7)      # Height — keep as drawn


def rung_dimension():
    print("\nD1  dimension — put a length on a side that had none")
    enter_sketch("r")
    draw_rect_undimensioned()
    d0 = describe()
    dof0 = d0["dof"]
    check("VERTEX", dof0 > 0, f"the undimensioned rectangle has {dof0} degrees of freedom")
    side = max((e for e in d0["entities"] if e["type"] == "line"), key=lambda e: e["length"])
    key("d", 0.6)
    clickmm(*mid(side))
    values(90)
    d = describe()
    ls = sorted(round(e["length"], 9) for e in d["entities"] if e["type"] == "line")
    check("LENGTH", any(near(x, 90.0, 1e-9) for x in ls), f"the dimensioned side reads {ls}")
    check("VERTEX", d["dof"] < dof0, f"degrees of freedom {dof0} -> {d['dof']}")
    check("CLOSED", d["solve_ok"] and len(d["closed_loops"]) == 1, "still one closed, solved loop")
    leave_sketch()


CONFIRM_BTN = (1751, 75 + CHROME_DY)


def confirm_and_reopen():
    """(see reopen_sketch below — same two steps, kept together for the constrain rungs)"""
    """Leave Constrain with the action bar's tick, then re-open the sketch for editing.

    THE DoF HAS TO BE READ HERE, not in Constrain mode. While constraining, sketch_describe
    reports the LIVE tool's dof and constraint count, which the constrain session does not
    touch — it works on the committed feature's own entity_constraints, and the panel computes
    its readout from those. Reading during the session says 4 -> 4 for a constraint that really
    did land; reading after the round trip says 4 -> 3, and proves the constraint was persisted
    rather than merely previewed.
    """
    click(*CONFIRM_BTN, pause=1.5)
    w, X, Y, _, _ = win()
    sh(f"DISPLAY={DISP} xdotool mousemove {X+TREE_ROW0[0]} {Y+TREE_ROW0[1]} "
       f"click --repeat 2 --delay 120 1")
    time.sleep(2.0)
    return describe()


def rung_constrain():
    reset_document()
    print("\nD2  constrain — Equal length on two adjacent sides, from the Constrain toolbar")
    enter_sketch("r")
    draw_rect_undimensioned()
    a, b = corner_pair(describe()["entities"])
    check("LENGTH", not near(a["length"], b["length"], 1e-6),
          f"the two sides start unequal: {a['length']:.6f} vs {b['length']:.6f}")
    key("k", 1.5)                              # finish the sketch and enter Constrain
    # Re-read the picks from the COMMITTED sketch: finish_sketch repackages the entities, so an
    # index taken before Constrain is not the same index afterwards.
    d1 = describe()
    a, b = corner_pair(d1["entities"])
    dof0 = 4                                   # an undimensioned rectangle: position + size
    ia, ib = d1["entities"].index(a), d1["entities"].index(b)
    clickmm(*mid(a)); clickmm(*mid(b))
    click(*CON_BTN["equal"])
    time.sleep(1.0)
    d = describe()
    la, lb = d["entities"][ia]["length"], d["entities"][ib]["length"]
    check("LENGTH", near(la, lb, 1e-9), f"the two sides are now equal: {la:.9f} and {lb:.9f}")
    d2 = confirm_and_reopen()
    check("VERTEX", d2["dof"] == dof0 - 1, f"degrees of freedom {dof0} -> {d2['dof']} after the round trip")
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    ls2 = sorted(round(e["length"], 9) for e in d2["entities"] if e["type"] == "line")
    check("LENGTH", near(ls2[0], la, 1e-9) and near(ls2[-1], la, 1e-9),
          f"the geometry came back unchanged: {ls2}")
    leave_sketch()


def rung_perpendicular():
    reset_document()
    print("\nD3  constrain — two free lines made exactly perpendicular")
    enter_sketch("l")
    clickmm(-50, -30); clickmm(30, -18)
    key("Escape", 0.7); key("Escape", 0.7)
    key("l", 0.6)
    # 56 degrees off the first line, not 3. The original second point put the pair within
    # inference's perpendicular tolerance, so on a rig whose camera maps the click a pixel
    # differently the two lines arrive ALREADY at exactly 90.000000 -- inference did the job the
    # rung exists to test, and the precondition failed while every later check passed. Held on
    # one fork and failed on the other from the same source, which is the signature of a rung
    # that depends on luck. Start well outside any snap tolerance so the button has real work.
    clickmm(30, -18); clickmm(55, 35)
    key("Escape", 0.7); key("Escape", 0.7)
    d0 = describe()
    check("ANGLE", abs(angle_between(d0["entities"][0], d0["entities"][1]) - 90.0) > 1e-3,
          f"they start at {angle_between(d0['entities'][0], d0['entities'][1]):.6f} deg")
    key("k", 1.5)
    d1 = describe()
    clickmm(*mid(d1["entities"][0])); clickmm(*mid(d1["entities"][1]))
    click(*CON_BTN["perpendicular"])
    time.sleep(1.0)
    d = describe()
    ang = angle_between(d["entities"][0], d["entities"][1])
    # 1e-6 deg, which is 1.7e-8 radians: the LIVE solve converges to its own tolerance and lands
    # at 89.999999991 from a 51 deg start. The old 1e-9 held only because the pair began 3 deg
    # from square, so the correction was tiny -- it was measuring how little work the solver had
    # to do, not whether the lines came out perpendicular. The round-trip check below still
    # demands exactly 90: the committed feature re-solves from scratch and gets there.
    check("ANGLE", near(ang, 90.0, 1e-6), f"now {ang:.9f} deg")
    d2 = confirm_and_reopen()
    ang2 = angle_between(d2["entities"][0], d2["entities"][1])
    check("ANGLE", near(ang2, 90.0, 1e-9), f"still {ang2:.9f} deg after the round trip")
    check("CLOSED", d2["constraints"] > 0 and d2["solve_ok"],
          f"{d2['constraints']} constraints survived, dof {d2['dof']}")
    leave_sketch()


def angle_between(a, b):
    va = (a["p1"][0] - a["p0"][0], a["p1"][1] - a["p0"][1])
    vb = (b["p1"][0] - b["p0"][0], b["p1"][1] - b["p0"][1])
    c = (va[0] * vb[0] + va[1] * vb[1]) / (math.hypot(*va) * math.hypot(*vb))
    return math.degrees(math.acos(max(-1.0, min(1.0, c))))


def parallel_gap(a, b):
    """How far from parallel, in degrees. Parallel reads 0 or 180; both mean parallel."""
    ang = angle_between(a, b)
    return min(ang, 180.0 - ang)


def rung_parallel():
    reset_document()
    print("\nD10 constrain — two divergent lines made parallel (the button had no rung at all)")
    enter_sketch("l")
    clickmm(-50, -30); clickmm(30, -18)
    key("Escape", 0.7); key("Escape", 0.7)
    key("l", 0.6)
    # Well outside inference's 3 deg snap, for the same reason D3 starts at 56: a pair that
    # arrives already parallel would let a dead button pass the rung.
    clickmm(30, -18); clickmm(55, 35)
    key("Escape", 0.7); key("Escape", 0.7)
    d0 = describe()
    g0 = parallel_gap(d0["entities"][0], d0["entities"][1])
    check("ANGLE", g0 > 1e-3, f"they start {g0:.6f} deg from parallel")
    key("k", 1.5)
    d1 = describe()
    clickmm(*mid(d1["entities"][0])); clickmm(*mid(d1["entities"][1]))
    click(*CON_BTN["parallel"])
    time.sleep(1.0)
    d = describe()
    g = parallel_gap(d["entities"][0], d["entities"][1])
    check("ANGLE", near(g, 0.0, 1e-6), f"now {g:.9f} deg from parallel")
    check("LENGTH", d["entities"][0]["length"] > 1.0 and d["entities"][1]["length"] > 1.0,
          f"neither line collapsed: {d['entities'][0]['length']:.6f}, {d['entities'][1]['length']:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["constraints"] > 0 and d2["solve_ok"],
          f"{d2['constraints']} constraints survived the commit, dof {d2['dof']}")
    leave_sketch()


def rung_live_constrain():
    reset_document()
    print("\nD11 constrain WHILE SKETCHING — no commit, no tree pick, no padlock")
    enter_sketch("l")
    clickmm(-50, -30); clickmm(30, -18)
    key("Escape", 0.7); key("Escape", 0.7)
    key("l", 0.6)
    clickmm(30, -18); clickmm(55, 35)
    # Two Escapes only: the first clears the polyline's pending point, the second drops the
    # tool to Select. The session stays LIVE -- that is the whole point of this rung.
    key("Escape", 0.7); key("Escape", 0.7)
    d0 = describe()
    check("VERTEX", len(d0["entities"]) == 2, f"{len(d0['entities'])} entities in the live sketch")
    g0 = parallel_gap(d0["entities"][0], d0["entities"][1])
    check("ANGLE", g0 > 1e-3, f"they start {g0:.6f} deg from parallel")
    # Pick both in the LIVE session, then press the button. No "k": pressing Constrain is
    # exactly the step this rung exists to prove is no longer necessary.
    clickmm(*mid(d0["entities"][0]))
    cx, cy = mid(d0["entities"][1])
    click_ctrl(*px(cx, cy))
    click(*CON_BTN_SKETCH["parallel"])
    time.sleep(1.2)
    d = describe()
    g = parallel_gap(d["entities"][0], d["entities"][1])
    check("ANGLE", near(g, 0.0, 1e-6), f"parallel without ever leaving the sketch: {g:.9f} deg")
    check("LENGTH", d["entities"][0]["length"] > 1.0 and d["entities"][1]["length"] > 1.0,
          f"neither line collapsed: {d['entities'][0]['length']:.6f}, {d['entities'][1]['length']:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["constraints"] > 0 and d2["solve_ok"],
          f"{d2['constraints']} constraints survived the commit, dof {d2['dof']}")
    leave_sketch()


# ---- the eleven buttons no rung had ever pressed ------------------------------------------
# Twenty constraint buttons, nine of them exercised. The other eleven were "implemented" in the
# sense that the kernel builds the right def for them -- which is exactly what was true of
# Parallel this morning, right up until a user pressed it. What a kernel test cannot see: that
# the BUTTON is wired to the index its name claims. CON_BTN is 449 + 42*i over a hand-written
# name list and it has drifted once already, unnoticed for months, because nothing pressed
# anything past index 5. These rungs press the rest.

def rung_vertical():
    reset_document()
    print("\nD12 constrain — Vertical on one line, from a line that is not")
    enter_sketch("l")
    clickmm(-20, -35); clickmm(5, 30)
    key("Escape", 0.7); key("Escape", 0.7)
    d0 = describe()
    dx0 = abs(d0["entities"][0]["p1"][0] - d0["entities"][0]["p0"][0])
    check("VERTEX", dx0 > 1.0, f"it starts off-plumb by {dx0:.6f} mm")
    key("k", 1.5)
    d1 = describe()
    clickmm(*mid(d1["entities"][0]))
    click(*CON_BTN["vertical"])
    time.sleep(1.0)
    d = describe()
    e = d["entities"][0]
    dx = abs(e["p1"][0] - e["p0"][0])
    check("VERTEX", near(dx, 0.0, 1e-6), f"now plumb: dx = {dx:.9f}")
    check("LENGTH", e["length"] > 1.0, f"and it did not collapse: {e['length']:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_equal_radius_button():
    reset_document()
    print("\nD13 constrain — the equal_radius button ITSELF (index 6: past where the map drifted)")
    # D4 proves the SHARED Equal button promotes on two circles. This presses design_c_equal_radius,
    # the first button after the six that were once inserted without updating this table.
    enter_sketch("c")
    clickmm(-35, 0); clickmm(-20, 0); key("Escape", 0.7)
    key("c", 0.6)
    clickmm(35, 0); clickmm(60, 0); key("Escape", 0.7)
    d0 = describe()
    cs = [e for e in d0["entities"] if e["type"] == "circle"]
    check("ARC", len(cs) == 2 and not near(cs[0]["radius"], cs[1]["radius"], 1e-6),
          f"they start unequal: {[round(c['radius'], 6) for c in cs]}")
    key("k", 1.5)
    d1 = describe()
    cs = [e for e in d1["entities"] if e["type"] == "circle"]
    ia, ib = d1["entities"].index(cs[0]), d1["entities"].index(cs[1])
    clickmm(*rim(cs[0])); clickmm(*rim(cs[1]))
    click(*CON_BTN["equal_radius"])
    time.sleep(1.0)
    d = describe()
    ra, rb = d["entities"][ia]["radius"], d["entities"][ib]["radius"]
    check("ARC", near(ra, rb, 1e-9), f"equal by the dedicated button: {ra:.9f} and {rb:.9f}")
    check("ARC", ra > 1e-6, f"and not equal at zero: {ra:.9f}")
    leave_sketch()


def rung_concentric():
    reset_document()
    print("\nD14 constrain — Concentric puts two circles on one centre, radii untouched")
    enter_sketch("c")
    clickmm(-30, -10); clickmm(-14, -10); key("Escape", 0.7)
    key("c", 0.6)
    clickmm(28, 14); clickmm(52, 14); key("Escape", 0.7)
    d0 = describe()
    cs = [e for e in d0["entities"] if e["type"] == "circle"]
    gap0 = math.dist(cs[0]["center"], cs[1]["center"])
    r_before = sorted(round(c["radius"], 9) for c in cs)
    check("ARC", gap0 > 1.0, f"centres start {gap0:.6f} mm apart")
    key("k", 1.5)
    d1 = describe()
    cs = [e for e in d1["entities"] if e["type"] == "circle"]
    ia, ib = d1["entities"].index(cs[0]), d1["entities"].index(cs[1])
    clickmm(*rim(cs[0])); clickmm(*rim(cs[1]))
    click(*CON_BTN["concentric"])
    time.sleep(1.0)
    d = describe()
    gap = math.dist(d["entities"][ia]["center"], d["entities"][ib]["center"])
    check("ARC", near(gap, 0.0, 1e-6), f"centres now coincide: {gap:.9f} mm apart")
    # Concentric is about centres only. A solve that also equalised the radii would be wrong.
    r_after = sorted(round(d["entities"][i]["radius"], 9) for i in (ia, ib))
    check("ARC", all(near(a, b, 1e-6) for a, b in zip(r_before, r_after)),
          f"radii untouched: {r_before} -> {r_after}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_tangent():
    reset_document()
    print("\nD15 constrain — Tangent brings a line to touch a circle exactly once")
    enter_sketch("c")
    clickmm(0, 0); clickmm(20, 0); key("Escape", 0.7)
    key("l", 0.6)
    # A line that clearly MISSES the circle: its perpendicular distance from the centre is well
    # above the radius, so tangency has real work to do in the direction of shrinking the gap.
    clickmm(-45, 38); clickmm(45, 44)
    key("Escape", 0.7); key("Escape", 0.7)
    d0 = describe()
    c0 = next(e for e in d0["entities"] if e["type"] == "circle")
    l0 = next(e for e in d0["entities"] if e["type"] == "line")
    d_before = point_line_distance(c0["center"], l0)
    check("ARC", d_before > c0["radius"] + 1.0,
          f"the line misses by {d_before - c0['radius']:.6f} mm")
    key("k", 1.5)
    d1 = describe()
    c1 = next(e for e in d1["entities"] if e["type"] == "circle")
    l1 = next(e for e in d1["entities"] if e["type"] == "line")
    ic, il = d1["entities"].index(c1), d1["entities"].index(l1)
    clickmm(*rim(c1)); clickmm(*mid(l1))
    click(*CON_BTN["tangent"])
    time.sleep(1.2)
    d = describe()
    cc, ll = d["entities"][ic], d["entities"][il]
    dist = point_line_distance(cc["center"], ll)
    check("ARC", near(dist, cc["radius"], 1e-6),
          f"centre-to-line {dist:.9f} == radius {cc['radius']:.9f}")
    check("ARC", cc["radius"] > 1e-6, f"and the circle did not collapse: r = {cc['radius']:.9f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_midpoint():
    reset_document()
    print("\nD16 constrain — Midpoint drops a free point onto the middle of a line")
    enter_sketch("l")
    clickmm(-50, -20); clickmm(40, 26)
    key("Escape", 0.7); key("Escape", 0.7)
    key("p", 0.6)
    clickmm(10, -35)
    d0 = describe()
    l0 = next(e for e in d0["entities"] if e["type"] == "line")
    p0 = next(e for e in d0["entities"] if e["type"] == "point")
    mid0 = ((l0["p0"][0] + l0["p1"][0]) / 2.0, (l0["p0"][1] + l0["p1"][1]) / 2.0)
    check("VERTEX", math.dist(p0["p"], mid0) > 1.0,
          f"the point starts {math.dist(p0['p'], mid0):.6f} mm off the midpoint")
    key("k", 1.5)
    d1 = describe()
    l1 = next(e for e in d1["entities"] if e["type"] == "line")
    p1 = next(e for e in d1["entities"] if e["type"] == "point")
    il, ip = d1["entities"].index(l1), d1["entities"].index(p1)
    # Point first, then the line: Midpoint binds a POINT role on ea and a whole segment on eb.
    clickmm(*p1["p"]); clickmm(*mid(l1))
    click(*CON_BTN["midpoint"])
    time.sleep(1.2)
    d = describe()
    ll, pp = d["entities"][il], d["entities"][ip]
    m = ((ll["p0"][0] + ll["p1"][0]) / 2.0, (ll["p0"][1] + ll["p1"][1]) / 2.0)
    check("VERTEX", near(math.dist(pp["p"], m), 0.0, 1e-6),
          f"the point sits on the midpoint: off by {math.dist(pp['p'], m):.9f}")
    check("LENGTH", ll["length"] > 1.0, f"and the line did not collapse: {ll['length']:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_symmetric_line_axis():
    reset_document()
    print("\nD17 constrain — Symmetric about a PICKED axis line (the three-pick form)")
    # D7 covers the implicit-axis buttons. This is the plain Symmetric: two entities AND an axis
    # entity, three picks, the only constraint on the bar that needs a third.
    enter_sketch("l")
    clickmm(0, -40); clickmm(0, 40)          # the axis: a real line on x = 0
    key("Escape", 0.7); key("Escape", 0.7)
    key("p", 0.6)
    clickmm(-38, 12)
    key("p", 0.6)
    clickmm(14, 12)
    d0 = describe()
    ps = [e for e in d0["entities"] if e["type"] == "point"]
    check("VERTEX", not near(abs(ps[0]["p"][0]), abs(ps[1]["p"][0]), 1e-3),
          f"they start unmirrored: x = {ps[0]['p'][0]:.6f}, {ps[1]['p'][0]:.6f}")
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ax = next(e for e in d1["entities"] if e["type"] == "line")
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    iax = d1["entities"].index(ax)
    clickmm(*ps[0]["p"]); clickmm(*ps[1]["p"]); clickmm(*mid(ax))
    click(*CON_BTN["symmetric"])
    time.sleep(1.2)
    d = describe()
    # Measure against the axis WHERE IT NOW IS, not against x = 0. All three entities are free,
    # so the solver is entitled to satisfy the mirror by moving the AXIS instead of the points --
    # and it does: the pair came out symmetric about x = -8.18, which is a correct solution and
    # which an "xa == -xb" assertion calls a failure. That was this rung being wrong, not the app.
    ax_now = d["entities"][iax]
    sa = signed_point_line_distance(d["entities"][ia]["p"], ax_now)
    sb = signed_point_line_distance(d["entities"][ib]["p"], ax_now)
    ya, yb = d["entities"][ia]["p"][1], d["entities"][ib]["p"][1]
    check("VERTEX", near(sa, -sb, 1e-6),
          f"mirrored about the picked line: {sa:.9f} and {sb:.9f} from it")
    check("VERTEX", abs(sa) > 1e-6, f"and not both collapsed onto it: |d| = {abs(sa):.9f}")
    check("VERTEX", near(ya, yb, 1e-6), f"y untouched on both: {ya:.6f}, {yb:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_symmetric_h():
    reset_document()
    print("\nD18 constrain — symmetric about the HORIZONTAL axis (sym_h, the twin of D7)")
    enter_sketch("p")
    clickmm(20, -34)
    key("p", 0.6)
    clickmm(20, 9)
    d0 = describe()
    ps = [e for e in d0["entities"] if e["type"] == "point"]
    check("VERTEX", not near(abs(ps[0]["p"][1]), abs(ps[1]["p"][1]), 1e-3),
          f"they start unmirrored: y = {ps[0]['p'][1]:.6f}, {ps[1]['p'][1]:.6f}")
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    clickmm(*ps[0]["p"]); clickmm(*ps[1]["p"])
    click(*CON_BTN["sym_h"])
    time.sleep(1.0)
    d = describe()
    xa, xb = d["entities"][ia]["p"][0], d["entities"][ib]["p"][0]
    ya, yb = d["entities"][ia]["p"][1], d["entities"][ib]["p"][1]
    check("VERTEX", near(ya, -yb, 1e-9), f"mirrored across y = 0: {ya:.9f} and {yb:.9f}")
    check("VERTEX", abs(ya) > 1e-6, f"and not both collapsed onto the axis: |y| = {abs(ya):.9f}")
    check("VERTEX", near(xa, xb, 1e-6), f"x untouched on both: {xa:.6f}, {xb:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_radius():
    reset_document()
    print("\nD19 constrain — a typed Radius drives the circle to exactly that radius")
    enter_sketch("c")
    clickmm(0, 0); clickmm(31, 0); key("Escape", 0.7)
    d0 = describe()
    c0 = next(e for e in d0["entities"] if e["type"] == "circle")
    check("ARC", not near(c0["radius"], 22.0, 1e-3), f"it starts at r = {c0['radius']:.6f}")
    key("k", 1.5)
    d1 = describe()
    c1 = next(e for e in d1["entities"] if e["type"] == "circle")
    ic = d1["entities"].index(c1)
    ctr = list(c1["center"])
    clickmm(*rim(c1))
    click(*CON_BTN["radius"])
    time.sleep(0.8)
    values(22)
    d = describe()
    cc = d["entities"][ic]
    check("ARC", near(cc["radius"], 22.0, 1e-6), f"radius driven to {cc['radius']:.9f}")
    # A Radius constraint moves the RIM, never the centre.
    check("ARC", near(math.dist(cc["center"], ctr), 0.0, 1e-6),
          f"and the centre stayed put: moved {math.dist(cc['center'], ctr):.9f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_diameter():
    reset_document()
    print("\nD20 constrain — a typed Diameter is a diameter, not a radius")
    # The failure this exists for is a factor of two: Diameter wired to the Radius handler gives
    # a circle of r = 30 for a typed 30, and nothing about the sketch looks wrong.
    enter_sketch("c")
    clickmm(0, 0); clickmm(19, 0); key("Escape", 0.7)
    key("k", 1.5)
    d1 = describe()
    c1 = next(e for e in d1["entities"] if e["type"] == "circle")
    ic = d1["entities"].index(c1)
    clickmm(*rim(c1))
    click(*CON_BTN["diameter"])
    time.sleep(0.8)
    values(30)
    d = describe()
    r = d["entities"][ic]["radius"]
    check("ARC", near(r, 15.0, 1e-6), f"typed diameter 30 gives radius {r:.9f}, not 30")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_fix():
    reset_document()
    print("\nD21 constrain — Fix anchors a point, and the anchor is what the solver moves around")
    # Fix on its own is unfalsifiable: nothing moved, so nothing proves it did anything. The
    # property only becomes observable when a SECOND constraint would otherwise have moved the
    # fixed point -- so drive the pair apart and see which end travels.
    enter_sketch("p")
    clickmm(-25, 0)
    key("p", 0.6)
    clickmm(15, 0)
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    anchor = list(ps[0]["p"])
    clickmm(*ps[0]["p"])
    click(*CON_BTN["fix"])
    time.sleep(1.0)
    d = describe()
    check("VERTEX", near(math.dist(d["entities"][ia]["p"], anchor), 0.0, 1e-9),
          "the fixed point did not move when it was fixed")
    # Now demand a horizontal gap far from the current one. Only the FREE point may travel.
    clickmm(*d["entities"][ia]["p"]); clickmm(*d["entities"][ib]["p"])
    click(*CON_BTN["dist_x"])
    time.sleep(0.8)
    values(70)
    d = describe()
    moved_a = math.dist(d["entities"][ia]["p"], anchor)
    gap = abs(d["entities"][ib]["p"][0] - d["entities"][ia]["p"][0])
    check("LENGTH", near(gap, 70.0, 1e-6), f"the gap was driven to {gap:.9f}")
    check("VERTEX", near(moved_a, 0.0, 1e-6),
          f"and the anchored point held station: moved {moved_a:.9f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 1,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_distance_y():
    reset_document()
    print("\nD22 constrain — vertical distance, and it is not the straight-line one either")
    enter_sketch("p")
    clickmm(-22, -18)
    key("p", 0.6)
    clickmm(19, 24)
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    dx_before = d1["entities"][ib]["p"][0] - d1["entities"][ia]["p"][0]
    clickmm(*ps[0]["p"]); clickmm(*ps[1]["p"])
    click(*CON_BTN["dist_y"])
    time.sleep(0.8)
    values(35)
    d = describe()
    gy = abs(d["entities"][ib]["p"][1] - d["entities"][ia]["p"][1])
    gx = d["entities"][ib]["p"][0] - d["entities"][ia]["p"][0]
    check("LENGTH", near(gy, 35.0, 1e-6), f"vertical gap driven to {gy:.9f}")
    check("VERTEX", near(gx, dx_before, 1e-6),
          f"the horizontal gap is untouched at {gx:.9f} — this is not a straight-line distance")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def signed_point_line_distance(p, line):
    """Perpendicular distance with a SIGN, so the two sides of a mirror are distinguishable."""
    x0, y0 = line["p0"]; x1, y1 = line["p1"]
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    return (dy * (p[0] - x0) - dx * (p[1] - y0)) / n


def point_line_distance(p, line):
    """Perpendicular distance from a point to the INFINITE line through a segment."""
    x0, y0 = line["p0"]; x1, y1 = line["p1"]
    dx, dy = x1 - x0, y1 - y0
    n = math.hypot(dx, dy)
    return abs(dy * (p[0] - x0) - dx * (p[1] - y0)) / n


# =================================================================== DURABILITY
# Exactness that does not survive an undo or a save is not exactness.

def rim(e, deg=135.0):
    """A point on a circle's rim, away from +X.

    NOT the +X point: a click there grabs the RADIUS GRIP instead of selecting the circle,
    and a grip click REPLACES the selection with that one entity — so the second pick of a
    two-entity constraint silently discards the first.
    """
    a = math.radians(deg)
    return (e["center"][0] + e["radius"] * math.cos(a),
            e["center"][1] + e["radius"] * math.sin(a))


def rung_equal_radius():
    reset_document()
    print("\nD4  constrain — Equal on two CIRCLES means equal radius, not equal length")
    # The dead end this fixed: two circles + Equal used to emit EQUAL_LENGTH_LINES, which
    # constrains nothing on a curve. The user got a silent no-op with no error and no way to
    # tell why. One Equal button, two meanings — lines get length, curves get radius.
    # NOT dimensioned: a typed radius is a DRIVING Radius constraint, and EqualRadius on two
    # circles pinned to 15 and 25 is genuinely inconsistent -- the kernel refuses the addition
    # and is right to. Draw them at different sizes and leave the radius free.
    enter_sketch("c")
    clickmm(-35, 0); clickmm(-20, 0); key("Escape", 0.7)
    key("c", 0.6)
    clickmm(35, 0); clickmm(60, 0); key("Escape", 0.7)
    d0 = describe()
    cs = [e for e in d0["entities"] if e["type"] == "circle"]
    check("ARC", len(cs) == 2 and not near(cs[0]["radius"], cs[1]["radius"], 1e-6),
          f"they start unequal: {[round(c['radius'], 6) for c in cs]}")
    key("k", 1.5)
    d1 = describe()
    cs = [e for e in d1["entities"] if e["type"] == "circle"]
    ia, ib = d1["entities"].index(cs[0]), d1["entities"].index(cs[1])
    clickmm(*rim(cs[0])); clickmm(*rim(cs[1]))
    click(*CON_BTN["equal"])          # the SHARED Equal button, not design_c_equal_radius
    time.sleep(1.0)
    d = describe()
    ra, rb = d["entities"][ia]["radius"], d["entities"][ib]["radius"]
    check("ARC", near(ra, rb, 1e-9), f"the two radii are now equal: {ra:.9f} and {rb:.9f}")
    check("ARC", ra > 1e-6, f"and not equal at zero: {ra:.9f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    rs = sorted(round(e["radius"], 9) for e in d2["entities"] if e["type"] == "circle")
    check("ARC", len(rs) == 2 and near(rs[0], rs[-1], 1e-9),
          f"still equal after the round trip: {rs}")
    leave_sketch()


def rung_collinear():
    reset_document()
    print("\nD5  constrain — two offset lines brought onto one infinite line")
    # Oblique on purpose: axis-aligned segments pick up an auto Horizontal at draw time, and
    # this rung is about Collinear, not about interacting with inference.
    enter_sketch("l")
    clickmm(-60, -14); clickmm(-12, -6)
    key("Escape", 0.7); key("Escape", 0.7)
    key("l", 0.6)
    clickmm(12, 14); clickmm(60, 22)
    key("Escape", 0.7); key("Escape", 0.7)
    d0 = describe()
    ls = [e for e in d0["entities"] if e["type"] == "line"]
    check("VERTEX", len(ls) == 2 and abs(ls[0]["p0"][1] - ls[1]["p0"][1]) > 1.0,
          f"they start on different lines, dy = {abs(ls[0]['p0'][1] - ls[1]['p0'][1]):.6f}")
    key("k", 1.5)
    d1 = describe()
    ls = [e for e in d1["entities"] if e["type"] == "line"]
    ia, ib = d1["entities"].index(ls[0]), d1["entities"].index(ls[1])
    clickmm(*mid(ls[0])); clickmm(*mid(ls[1]))
    click(*CON_BTN["collinear"])
    time.sleep(1.0)
    d = describe()
    A, B = d["entities"][ia], d["entities"][ib]
    ax, ay = A["p0"]
    dx, dy = A["p1"][0] - ax, A["p1"][1] - ay
    cross = [dx * (q[1] - ay) - dy * (q[0] - ax) for q in (B["p0"], B["p1"])]
    check("VERTEX", all(abs(c) < 1e-6 for c in cross),
          f"both ends of the second line lie on the first: cross {[round(c, 9) for c in cross]}")
    # A line collapsed to a point is trivially collinear with anything, so the cross products
    # above pass on a degenerate solve. Both lines have to survive.
    check("LENGTH", A["length"] > 1.0 and B["length"] > 1.0,
          f"neither line collapsed: {A['length']:.6f}, {B['length']:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_distance_xy():
    reset_document()
    print("\nD6  constrain — horizontal distance, and accepting its own value moves nothing")
    enter_sketch("p")
    clickmm(-30, -20)
    key("p", 0.6)
    clickmm(25, 18)
    # A THIRD point, placed now while the point tool is still armed. The typed half of this rung
    # needs a pair that carries no dimension yet, and pressing "p" again once the constrain tool
    # has taken over does not re-arm the point tool -- the click is consumed as a pick and no
    # point appears, which read as an IndexError three lines later rather than as what it was.
    key("p", 0.6)
    clickmm(5, 5)
    d0 = describe()
    ps = [e for e in d0["entities"] if e["type"] == "point"]
    check("VERTEX", len(ps) == 3, f"three points placed: {len(ps)}")
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    ic = d1["entities"].index(ps[2])

    # THE NO-OP PROPERTY, which is the one a unit test cannot reach: the field opens pre-filled
    # with the current projected gap, and pressing Return must change nothing. The constraint is
    # SIGNED — (pB - pA).dot(axis) — so if the refs were ordered to show |delta| while the real
    # delta is negative, accepting the number on screen teleports the point across its anchor.
    before = [list(d1["entities"][ia]["p"]), list(d1["entities"][ib]["p"])]
    clickmm(*ps[0]["p"]); clickmm(*ps[1]["p"])
    click(*CON_BTN["dist_x"])
    time.sleep(0.8)
    key("Return", 1.2)                 # accept the pre-filled value, type nothing
    d = describe()
    after = [list(d["entities"][ia]["p"]), list(d["entities"][ib]["p"])]
    moved = max(abs(a - b) for pa, pb in zip(before, after) for a, b in zip(pa, pb))
    # 5 microns, not zero: the field shows two decimals, so accepting what it shows commits a
    # ROUNDED value and the geometry legitimately shifts by up to half a displayed unit. The
    # defect this guards is a sign flip, which moves the point by twice the gap — tens of
    # millimetres. A 1e-6 tolerance here failed on a 0.96 micron rounding step, which is the
    # instrument disagreeing with the display, not the app misbehaving.
    check("VERTEX", moved < 5e-3,
          f"accepting the shown value moved nothing (max {moved:.9f})")
    # "nothing moved" passes just as happily when the constraint was never applied at all --
    # which is exactly how the phantom-endpoint bug hid. The gap has to be REAL and driveable,
    # so prove the mechanism works before trusting the no-op above.
    check("LENGTH", abs(d["entities"][ib]["p"][0] - d["entities"][ia]["p"][0]) > 1.0,
          "and the two points still have a real horizontal gap to dimension")

    # Now drive it with a TYPED value, on a FRESH pair. Not on the pair just dimensioned: that
    # one already carries its DistanceX, the button rightly refuses to dimension it twice, and no
    # field opens — so the digits went nowhere and the gap kept the value the no-op step had
    # accepted. A green "gap is 54.94" for a rung that typed 40 is the rung's fault, not the app's.
    dy_before = d["entities"][ic]["p"][1] - d["entities"][ia]["p"][1]
    clickmm(*d["entities"][ia]["p"]); clickmm(*d["entities"][ic]["p"])
    click(*CON_BTN["dist_x"])
    time.sleep(0.8)
    values(40)
    d = describe()
    gx = abs(d["entities"][ic]["p"][0] - d["entities"][ia]["p"][0])
    gy = d["entities"][ic]["p"][1] - d["entities"][ia]["p"][1]
    check("LENGTH", near(gx, 40.0, 1e-6), f"horizontal gap driven to {gx:.9f}")
    check("VERTEX", near(gy, dy_before, 1e-6),
          f"the vertical gap is untouched at {gy:.9f} — this is not a straight-line distance")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_coincident_points():
    reset_document()
    print("\nD8  constrain — Coincident on two POINTS actually joins them")
    # The regression this exists for: the closest-pair search used to enumerate {P0,p0},{P1,p1}
    # on both entities, and a Point's p1 reads (0,0). For two points the phantom pair is at
    # distance 0, which ALWAYS wins, so the constraint was added against a role the solver
    # cannot resolve and dropped in silence -- constraints=0, the points never moved, no error.
    # Two points is the simplest possible use of the button and it did nothing on every press.
    enter_sketch("p")
    clickmm(-30, -20)
    key("p", 0.6)
    clickmm(25, 18)
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    gap0 = math.dist(ps[0]["p"], ps[1]["p"])
    check("VERTEX", gap0 > 1.0, f"they start apart: {gap0:.6f}")
    clickmm(*ps[0]["p"]); clickmm(*ps[1]["p"])
    click(*CON_BTN["coincident"])
    time.sleep(1.2)
    d = describe()
    gap = math.dist(d["entities"][ia]["p"], d["entities"][ib]["p"])
    check("VERTEX", gap < 1e-6, f"and are now joined: gap {gap:.9f}")
    # The count has to be read AFTER the round trip, never during the session: while
    # constraining, sketch_describe reports the LIVE tool's constraints, which the constrain
    # session never touches -- it writes the committed feature's entity_constraints. Reading it
    # here says 0 for a constraint that really did land. Same trap the D2 rung documents.
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit — a dropped one leaves this at 0")
    ps2 = [e for e in d2["entities"] if e["type"] == "point"]
    check("VERTEX", math.dist(ps2[0]["p"], ps2[1]["p"]) < 1e-6,
          "and they are still joined after the round trip")
    leave_sketch()


def rung_symmetric_axis():
    reset_document()
    print("\nD7  constrain — symmetric about the vertical axis, with no construction line")
    # Plain Symmetric needs a third pick for the mirror axis, so being symmetric about the
    # sketch's own vertical axis used to mean drawing a construction line first. This button
    # uses the implicit axis: two picks, no axis entity.
    enter_sketch("p")
    clickmm(-40, 15)
    key("p", 0.6)
    clickmm(12, 15)
    key("k", 1.5)
    d1 = describe()
    ps = [e for e in d1["entities"] if e["type"] == "point"]
    ia, ib = d1["entities"].index(ps[0]), d1["entities"].index(ps[1])
    check("VERTEX", not near(abs(ps[0]["p"][0]), abs(ps[1]["p"][0]), 1e-3),
          f"they start unmirrored: x = {ps[0]['p'][0]:.6f}, {ps[1]['p'][0]:.6f}")
    clickmm(*ps[0]["p"]); clickmm(*ps[1]["p"])
    click(*CON_BTN["sym_v"])
    time.sleep(1.0)
    d = describe()
    xa, xb = d["entities"][ia]["p"][0], d["entities"][ib]["p"][0]
    ya, yb = d["entities"][ia]["p"][1], d["entities"][ib]["p"][1]
    check("VERTEX", near(xa, -xb, 1e-9), f"mirrored across x = 0: {xa:.9f} and {xb:.9f}")
    # Both collapsing onto the axis satisfies the mirror trivially.
    check("VERTEX", abs(xa) > 1e-6, f"and not both collapsed onto the axis: |x| = {abs(xa):.9f}")
    check("VERTEX", near(ya, yb, 1e-6), f"y untouched on both: {ya:.6f}, {yb:.6f}")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["solve_ok"] and d2["constraints"] > 0,
          f"{d2['constraints']} constraints survived the commit")
    leave_sketch()


def rung_type_guards():
    reset_document()
    print("\nD9  constrain — a button that cannot apply must SAY so, not store a dead constraint")
    # Both halves were live defects found by reviewing the DistanceX/Y fix for its class.
    #
    # Horizontal on a lone Point: P1 is a role the solver cannot resolve, so the constraint was
    # dropped at ref_ok -- but still STORED. Measured before the fix: constraints 0 -> 1 after the
    # commit, point unmoved. A constraint listed in the panel that can never do anything is worse
    # than a refusal, because the panel then claims the sketch is constrained when it is not.
    enter_sketch("p")
    clickmm(-30, -20)
    key("k", 1.5)
    d = describe()
    ps = [e for e in d["entities"] if e["type"] == "point"]
    ia = d["entities"].index(ps[0])
    clickmm(*ps[0]["p"])
    click(*CON_BTN["horizontal"])
    time.sleep(1.0)
    d = describe()
    check("VERTEX", near(d["entities"][ia]["p"][0], ps[0]["p"][0], 1e-9) and
                    near(d["entities"][ia]["p"][1], ps[0]["p"][1], 1e-9),
          "Horizontal on a point moved nothing, as it must")
    d2 = confirm_and_reopen()
    check("CLOSED", d2["constraints"] == 0,
          f"and stored NO constraint: {d2['constraints']} (a dead stored one reads 1)")
    leave_sketch()

    # Angle on two circles: p1-p0 on a circle is (0,0)-centre, so the field used to pre-fill with
    # the angle between the two centre POSITION VECTORS -- 178.83 deg for two circles on the x
    # axis -- and committing it fed SLVS_C_ANGLE two circle prims, which are not directions.
    reset_document()
    enter_sketch("c")
    clickmm(-35, 0); clickmm(-20, 0); key("Escape", 0.7)
    key("c", 0.6)
    clickmm(35, 0); clickmm(60, 0); key("Escape", 0.7)
    key("k", 1.5)
    d = describe()
    cs = [e for e in d["entities"] if e["type"] == "circle"]
    clickmm(*rim(cs[0])); clickmm(*rim(cs[1]))
    click(*CON_BTN["angle"])
    time.sleep(1.0)
    check("ANGLE", field_win() is None,
          "Angle on two circles opened no value field")
    key("Escape", 0.6)
    d2 = confirm_and_reopen()
    check("CLOSED", d2["constraints"] == 0,
          f"and stored no angle: {d2['constraints']}")
    leave_sketch()


def rung_undo():
    print("\nE1  undo — the last entity goes, the rest do not move")
    enter_sketch("l")
    draw_line(-50, -30, 0, -30, 50, 0)
    key("l", 0.6); draw_line(0, -30, 0, 20, 50, 90)
    key("l", 0.6); draw_line(0, 20, -40, 20, 40, 180)
    before = describe()["entities"]
    check("LENGTH", len(before) == 3, f"{len(before)} entities drawn")
    key("ctrl+z", 1.0)
    after = describe()["entities"]
    check("VERTEX", len(after) == 2, f"{len(after)} entities after one undo")
    same = all(math.dist(a["p0"], b["p0"]) == 0.0 and math.dist(a["p1"], b["p1"]) == 0.0
               for a, b in zip(before, after))
    check("VERTEX", same, "the two survivors are bit-identical, not re-solved")
    key("ctrl+z", 1.0)
    check("VERTEX", len(describe()["entities"]) == 1, "a second undo drops one more")
    leave_sketch()


def rung_feature_undo():
    print("\nE2  undo/redo across the commit — a deleted sketch comes back exactly")
    reset_document()
    enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    click(*CONFIRM_BTN, pause=1.5)
    n0 = len(call("describe_scene")["features"])
    check("VERTEX", n0 == 1, f"{n0} feature committed")
    click(*TREE_ROW0, pause=0.4)
    key("Delete", 0.8)
    check("VERTEX", not call("describe_scene")["features"], "the tree is empty after Delete")
    key("ctrl+z", 1.5)
    check("VERTEX", len(call("describe_scene")["features"]) == 1, "undo brings the feature back")
    d = reopen_sketch()
    ls = lengths(d["entities"])
    check("LENGTH", ls == [80.0, 80.0, 120.0, 120.0], f"and it is the same rectangle: {ls}")
    lp = d["closed_loops"]
    check("AREA", len(lp) == 1 and near(abs(lp[0]["area"]), 9600.0, 1e-6),
          f"area {abs(lp[0]['area']):.6f}")
    leave_sketch()
    key("ctrl+y", 1.5)
    check("VERTEX", not call("describe_scene")["features"], "redo removes it again")
    reset_document()


PROJECT_FILE = "/tmp/gl-roundtrip.3mf"


def dialog_up():
    names = sh(f"DISPLAY={DISP} xdotool search --class '.' getwindowname %@")
    return any(n and n != "snapmaker-orca" and "file" in n.lower() for n in names.splitlines())


def file_dialog(path, settle=5.0):
    """Type an absolute path into the GTK file chooser that is up, and accept it."""
    if not dialog_up():
        die("no file chooser came up")
    key("ctrl+a", 0.3)
    typ(path, 0.5)
    key("Return", settle)
    global _win
    _win = None            # saving renames the window; drop the cached geometry


def rung_roundtrip():
    print("\nE3  save and reload — the profile comes back to the last decimal")
    reset_document()
    enter_sketch("r")
    draw_rect(120, 80, -60, -40)
    key("r", 0.6); clickmm(-45, -15); clickmm(-5, 15); values(40, 30)
    key("c", 0.6); clickmm(30, 0); clickmm(42, 0); values(10)
    before = describe()
    click(*CONFIRM_BTN, pause=1.5)
    sh(f"rm -f {PROJECT_FILE}")
    # Save AS, not Save: once a project has a path, Ctrl+S writes to it silently and no chooser
    # appears — which is correct behaviour and a trap for a driver that assumes the dialog.
    key("ctrl+shift+s", 3.0)
    file_dialog(PROJECT_FILE)
    size = sh(f"stat -c %s {PROJECT_FILE} 2>/dev/null").strip()
    check("CLOSED", size.isdigit() and int(size) > 0, f"project written, {size} bytes")
    reset_document()                                  # wipe the tree, then read it back off disk
    key("ctrl+o", 2.5)
    file_dialog(PROJECT_FILE, settle=8.0)
    go_design()                                       # opening a project lands on Prepare
    feats = call("describe_scene")["features"]
    check("VERTEX", len(feats) == 1, f"the reloaded document has {len(feats)} feature(s)")
    after = reopen_sketch()
    b = sorted((e["type"], tuple(round(c, 12) for c in (e.get("p0") or e.get("center") or e.get("p"))),
                round(e.get("length", e.get("radius", 0.0)), 12)) for e in before["entities"])
    a = sorted((e["type"], tuple(round(c, 12) for c in (e.get("p0") or e.get("center") or e.get("p"))),
                round(e.get("length", e.get("radius", 0.0)), 12)) for e in after["entities"])
    check("VERTEX", a == b, f"{len(a)} entities identical to 12 decimals after the round trip")
    lp = after["closed_loops"]
    check("CLOSED", len(lp) == 3 and after["buildable"], f"{len(lp)} loops, buildable")
    outer = max(range(len(lp)), key=lambda i: abs(lp[i]["area"]))
    check("VOID", sorted(lp[outer]["holes"]) == sorted(i for i in range(len(lp)) if i != outer),
          "the voids are still attributed to the outer loop")
    check("AREA", near(abs(lp[outer]["area"]), 9600.0, 1e-9), f"outer area {abs(lp[outer]['area']):.9f}")
    leave_sketch()
    reset_document()


def rung_scale():
    print("\nE4  scale — a gesture on top of a sketch that already holds a thousand entities")
    enter_sketch("r")
    # The heavy profile is bulk-loaded through the socket ON PURPOSE: what is under test here is
    # whether the interactive path still works with a large sketch already on screen, not where
    # that sketch came from. A plate with a 20 x 15 grid of square cut-outs — 1204 entities.
    # Sized to the region the calibration probes covered, so every part of it can actually be
    # clicked: the camera is wherever the last rung left it, and a plate drawn off-screen would
    # test nothing but my arithmetic.
    x0, x1, y0, y1 = _SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    hw, hh = (x1 - x0) * 0.44, (y1 - y0) * 0.44
    ents = [{"type": "line", "p0": [cx - hw, cy - hh], "p1": [cx + hw, cy - hh]},
            {"type": "line", "p0": [cx + hw, cy - hh], "p1": [cx + hw, cy + hh]},
            {"type": "line", "p0": [cx + hw, cy + hh], "p1": [cx - hw, cy + hh]},
            {"type": "line", "p0": [cx - hw, cy + hh], "p1": [cx - hw, cy - hh]}]
    # 300 square cut-outs in the LEFT half; the right half stays clear so the gesture below has
    # somewhere to land that is not within snapping distance of a cut-out corner.
    pitch_x, pitch_y = hw * 0.9 / 20.0, hh * 1.9 / 15.0
    side = min(pitch_x, pitch_y) * 0.4
    for i in range(20):
        for j in range(15):
            x = cx - hw * 0.95 + i * pitch_x
            y = cy - hh * 0.95 + j * pitch_y
            c = [(x, y), (x + side, y), (x + side, y + side), (x, y + side), (x, y)]
            for k in range(4):
                ents.append({"type": "line", "p0": list(c[k]), "p1": list(c[k + 1])})
    t0 = time.monotonic(); call("sketch_add", entities=ents); t_add = time.monotonic() - t0
    d0 = describe()
    check("SCALE", len(d0["entities"]) == len(ents), f"{len(d0['entities'])} entities loaded "
                                                     f"in {t_add*1000:.0f} ms")
    lp0 = d0["closed_loops"]
    check("CLOSED", len(lp0) == 301, f"{len(lp0)} closed loops")
    outer = max(range(len(lp0)), key=lambda i: abs(lp0[i]["area"]))
    check("AREA", near(abs(lp0[outer]["area"]), 4.0 * hw * hh, 1e-9),
          f"outer plate {abs(lp0[outer]['area']):.9f} vs {4.0*hw*hh:.9f}")
    check("VOID", len(lp0[outer]["holes"]) == 300,
          f"all {len(lp0[outer]['holes'])} cut-outs attributed to the plate")
    check("AREA", all(near(abs(lp0[h]["area"]), side * side, 1e-9) for h in lp0[outer]["holes"]),
          f"every cut-out is exactly {side:.6f} squared")
    # Now the part that matters: draw ONE more entity by hand, on top of all that.
    #
    # No Escape here, deliberately: this rung is the regression test for snaporca-j7gc, where a
    # bulk sketch_add made while a creation tool is armed was read as a drawn gesture, opened that
    # tool's value field and swallowed the next key and click until one Escape dismissed it. The
    # gesture below has to land on the FIRST try. Fixed by resyncing m_autoedit_seen in
    # add_entities_scripted; if this rung ever needs an Escape again, the bug is back.
    key("l", 0.8)
    global PACE
    PACE = 6.0                                     # a thousand entities re-solve between fields
    t0 = time.monotonic()
    ax, ay = cx + hw * 0.15, cy + hh * 0.55        # clear of the grid, inside the plate
    want_len = int(hw * 0.5)                       # a WHOLE number: see value() on separators
    clickmm(ax, ay); clickmm(ax + want_len, ay)
    value(want_len)
    dl = describe()
    say(f"after the typed length: solve_ok={dl['solve_ok']} constraints={dl['constraints']} "
        f"dof={dl['dof']} entities={len(dl['entities'])}")
    value(0)
    t_draw = time.monotonic() - t0
    d = describe()
    check("SCALE", len(d["entities"]) == len(ents) + 1,
          f"the gesture added exactly one entity ({t_draw:.1f} s including four synthetic events)")
    new = d["entities"][-1]
    check("LENGTH", near(new["length"], float(want_len), 1e-9),
          f"and it took its typed length exactly: {new['length']}")
    ang = math.degrees(math.atan2(new["p1"][1] - new["p0"][1], new["p1"][0] - new["p0"][0])) % 360.0
    check("ANGLE", near(ang, 0.0, 1e-9) or near(ang, 360.0, 1e-9), f"and its typed angle: {ang}")
    same = all(math.dist(a["p0"], b["p0"]) == 0.0 and math.dist(a["p1"], b["p1"]) == 0.0
               for a, b in zip(d0["entities"], d["entities"]))
    check("VERTEX", same, "and moved none of the thousand entities already there")
    PACE = 1.0
    leave_sketch()


def reopen_sketch():
    w, X, Y, _, _ = win()
    sh(f"DISPLAY={DISP} xdotool mousemove {X+TREE_ROW0[0]} {Y+TREE_ROW0[1]} "
       f"click --repeat 2 --delay 120 1")
    time.sleep(2.0)
    return describe()


def rung_mirror_arcs():
    """C4b — mirror a shape that HAS ARCS. The rung above mirrors three straight lines, which is
    why it sat green through the defect a user hit on 2026-08-23: a slot mirrored about a vertical
    line came back with its caps at r=32.2 and a 237 deg sweep, one rail collapsed from 62.9 mm to
    2.1 mm, and the ORIGINAL was wrecked along with the copy. An arc has five degrees of freedom
    and the copy was bound to its source by its CENTRE alone, so the solver was free to answer with
    a different, internally consistent sketch. Circles were unaffected — a circle has no endpoints
    to leave free — so the failure read as "circles fine, slots and rounded rectangles destroyed".

    Graded on the property the user actually stated: THE APPLIED RESULT IS THE PREVIEW. The copy is
    the source reflected, the source does not move, and no arc comes back reflex.
    """
    print("\nC4b mirror — a slot, so the reflection has arcs in it")
    enter_sketch("l")
    click(*CONSTRUCTION_CHECKBOX)
    key("l", 0.6)
    draw_line(0, -40, 0, 40, 80, 90)                 # the axis, on x = 0
    click(*CONSTRUCTION_CHECKBOX)
    key("s", 0.6)                                    # slot: two centreline ends, then the width
    clickmm(-70, -10); clickmm(-30, -10); clickmm(-30, 0)
    values(40, 10, 0)                                # typed, so the slot is exact before mirroring
    d0 = describe()["entities"]
    axis = [e for e in d0 if e.get("construction")][0]
    slot = [e for e in d0 if not e.get("construction")]
    arcs0 = [e for e in slot if e["type"] == "arc"]
    check("ARC", len(arcs0) == 2, f"{len(arcs0)} caps on the slot")

    key("m", 0.6)
    clickmm(*mid(axis))
    for e in slot:
        if e["type"] == "line":
            clickmm(*mid(e))
        else:                                        # a point ON the arc, at its mid sweep
            a = (e["start_angle"] + e["end_angle"]) / 2.0
            clickmm(e["center"][0] + e["radius"] * math.cos(a),
                    e["center"][1] + e["radius"] * math.sin(a))
    clickmm(60, 60)                                  # empty space confirms
    d1 = describe()["entities"]
    check("VERTEX", len(d1) == len(d0) + len(slot),
          f"{len(d1) - len(d0)} copies for {len(slot)} picked entities")

    # the sources, entity by entity, must be exactly where they were
    def shape_of(e):
        if e["type"] == "arc":
            return (round(e["radius"], 9), round(abs(e["end_angle"] - e["start_angle"]), 9))
        return (round(math.dist(e["p0"], e["p1"]), 9),)
    moved = [i for i, e in enumerate(d0) if shape_of(e) != shape_of(d1[i])]
    check("VERTEX", not moved, f"the mirror left every source alone (moved: {moved})")

    # and every copy is its source reflected — endpoints unordered, because a reflection
    # reverses orientation and legitimately stores p0/p1 the other way round
    (ax, ay), (bx, by) = axis["p0"], axis["p1"]
    dx, dy = bx - ax, by - ay
    n = math.hypot(dx, dy); dx, dy = dx / n, dy / n
    def refl(q):
        vx, vy = q[0] - ax, q[1] - ay
        k = 2.0 * (vx * dx + vy * dy)
        return (ax + k * dx - vx, ay + k * dy - vy)
    copies = d1[len(d0):]
    worst = 0.0
    for e in slot:
        best = min(max(min(max(math.dist(refl(e["p0"]), c["p0"]), math.dist(refl(e["p1"]), c["p1"])),
                           max(math.dist(refl(e["p0"]), c["p1"]), math.dist(refl(e["p1"]), c["p0"]))),
                       abs(shape_of(e)[0] - shape_of(c)[0]))
                   for c in copies if c["type"] == e["type"])
        worst = max(worst, best)
    check("SYMMETRY", worst <= 1e-6, f"every copy is the exact reflection (worst {worst:.9f})")
    reflex = [c for c in copies
              if c["type"] == "arc" and abs(c["end_angle"] - c["start_angle"]) > math.pi + 1e-9]
    check("ARC", not reflex, f"{len(reflex)} copied cap(s) came back reflex — the 'cloud' failure")


RUNGS = {"rect": rung_rect, "circle": rung_circle, "line": rung_line, "arc": rung_arc,
         "slot": rung_slot, "polygon": rung_polygon, "ellipse": rung_ellipse,
         "point": rung_point, "spline": rung_spline, "voids": rung_voids,
         "fillet": rung_fillet, "chamfer": rung_chamfer, "offset": rung_offset,
         "mirror": rung_mirror, "mirror_arcs": rung_mirror_arcs, "trim": rung_trim, "extend": rung_extend,
         "dimension": rung_dimension, "constrain": rung_constrain,
         "perpendicular": rung_perpendicular,
         "equal_radius": rung_equal_radius, "collinear": rung_collinear,
         "distance_xy": rung_distance_xy, "symmetric_axis": rung_symmetric_axis,
         "coincident_points": rung_coincident_points,
         "type_guards": rung_type_guards,
         "parallel": rung_parallel, "live_constrain": rung_live_constrain,
         "vertical": rung_vertical, "equal_radius_button": rung_equal_radius_button,
         "concentric": rung_concentric, "tangent": rung_tangent, "midpoint": rung_midpoint,
         "symmetric_line_axis": rung_symmetric_line_axis, "symmetric_h": rung_symmetric_h,
         "radius": rung_radius, "diameter": rung_diameter, "fix": rung_fix,
         "distance_y": rung_distance_y,
         "undo": rung_undo,
         "feature_undo": rung_feature_undo, "roundtrip": rung_roundtrip,
         "scale": rung_scale}


def main():
    want = sys.argv[1:] or list(RUNGS)
    reset_document()
    for name in want:
        if name not in RUNGS:
            die(f"unknown rung {name}; have {' '.join(RUNGS)}")
        RUNGS[name]()
    leave_sketch()
    print(f"\n{_checks - _fail}/{_checks} properties held")
    sys.exit(1 if _fail else 0)


if __name__ == "__main__":
    main()
