"""Drive the Design tab of a running OrcaSlicer the way a person does, for the Design-tab GUI tests.

Every action goes through the MCP server's UI tools (view_set, widget_click, mouse_gesture, keyboard_press,
widget_set_value, dialog_answer); the Design commands are used only to read results back (sketch_describe,
describe_design, feature_describe), and the right-click menu's layout is read from the Design table's source,
scripts/CAD/tool_atlas.json, as the app's own table is generated from it. Nothing here knows a pixel: buttons and rows are
found by label, and canvas positions come from a calibration that maps the open sketch's plane
(millimetres) to canvas pixels, so the tests run at any window size, screen scaling and theme. Labels, menu paths and verb names are
English, which start() checks.

    gui = CadGui(orca_mcp.session_client()[0])
    gui.start()   # checks.start()'s baseline, then the Design tab's own start
    sys.exit(gui.run({"rect": rung_rect}, sys.argv[1:]))
"""

import collections
import json
import math
import os
import re
import sys
import time

import checks
import orca_mcp

# Labels carry characters such as "—" and "·", and output piped to a file on Windows is encoded in
# the system's legacy code page, which may lack them: they print escaped rather than raise.
sys.stdout.reconfigure(errors="backslashreplace")

# The Design table's source, which DesignOffer.hpp, the right-click menu and the Design commands are made from.
ATLAS = os.path.join(os.path.dirname(os.path.abspath(__file__)), os.pardir, os.pardir, "scripts", "CAD", "tool_atlas.json")

# The 3D canvas by its window name, which every platform lists; only Windows gives it as its label too.
CANVAS = {"name": "OrcaSceneCanvas"}
NEW_DESIGN = "New Design — clear the feature tree"

# The Design view's switches as a new data folder and a newly started app have them, under view_set's names: the
# Origin row's planes hidden (design_show_origin, unset in a new folder), the bed shown (design_show_bed's default),
# and the helper planes and axes of keys P and A hidden, as each app starts. The baseline leaves them to the Design
# tests (TESTS.md), so reset_switches() sets them, with the section view off.
SWITCHES = {"origin": False, "bed": True, "origin_planes": False, "world_axes": False}

# reset_view()'s zoom, in click pixels per mm at the camera's target, about 1.6: close enough that a
# 40 mm line is long beside the pixel-sized reach of its value label, and far enough that the region
# calibrate() measures holds every shape the tests draw at the default window. In click pixels, so
# the tests' clicks have the same room on every display; view_set takes screenshot pixels, which
# are GL pixels, two per click pixel on a Retina display.
RESET_ZOOM = 250 / 1.4 ** 15

# Where calibrate() clicks its probes, as shares of the canvas's width and height: four around
# its centre and a fifth off both of the plane's axes through it. The second layout is used only
# when the sketch already held geometry and a probe of the first snapped onto it; its band lies
# further out, where the tests draw nothing, and still clear of the panels along the canvas's
# edges. In an empty sketch nothing can snap a probe, so one off its click there is a fault.
PROBE_LAYOUTS = (((0.35, 0.35), (0.65, 0.35), (0.65, 0.65), (0.35, 0.65), (0.30, 0.45)),
                 ((0.25, 0.25), (0.75, 0.25), (0.75, 0.75), (0.25, 0.75), (0.22, 0.42)))

# DesignSketchTool's pixel budgets are in the canvas's GL pixels, which its mouse events carry: two
# per click pixel on a Retina display. CadGui.budget() turns one into click pixels.
# Its pick budgets in Select: a click within kLabelPx of a value label's centre takes the label, and
# one within kPickPx of a handle, a point or a curve takes it.
LABEL_PICK_PX = 24.0
PICK_PX = 8.0
# How far past those budgets clear_pick() keeps a click, in click pixels: a label's centre is read
# from the window it is drawn in, to a pixel or so.
PICK_MARGIN_PX = 3.0
# DesignSketchTool::constraint_badges: each constraint's badge is an 11 px cell, the badges of one
# entity stacked up the plane's +Y from its middle 1.5 cells apart, the first one step up. A click
# within 0.6 cells of a badge's centre takes it.
BADGE_CELL_PX = 11.0
BADGE_STEP_PX = 1.5 * BADGE_CELL_PX
BADGE_PICK_PX = 0.6 * BADGE_CELL_PX

# One line of widget_list's tree. rect is (x, y, w, h) in logical pixels, from the main window's
# client area for a window's widgets and from the canvas's top left for its ImGui items, or None.
# parent is the index of the parent node in the list widgets() returns, or None at the top.
# focused: the line is marked [focused], the window or ImGui text field that typed keys go to.
Widget = collections.namedtuple("Widget", "ref kind label value name rect depth parent focused")
# The canvas's open value field: its title (such as "Length"), the value it opened with, its ref,
# and whether it holds the keyboard yet.
ValueField = collections.namedtuple("ValueField", "title prefill ref focused")
# A right click's offer menu. items are label paths ("Create>Rectangle>Corner rectangle"); the
# header, the disabled first item naming the selection ("Sketch — nothing selected"), is left out
# of items and disabled.
Offer = collections.namedtuple("Offer", "header items disabled checked")


def until(fn, timeout=30.0, interval=0.05):
    """Poll fn until it returns something true, or timeout seconds pass; returns its last value.

    The Design tab's poller: every 0.05 s, since a click there takes effect within a frame or two. It never raises,
    so that a caller can check what it saw or raise with it; the deadline only bounds a hang."""
    deadline = time.monotonic() + timeout
    while True:
        value = fn()
        if value or time.monotonic() >= deadline:
            return value
        time.sleep(interval)


def reset_switches(client):
    """Put the Design view's switches as SWITCHES gives them and its section view off, on the Design tab, which
    view_set shows to take them; raises when its reply reads them otherwise."""
    design = client.call("view_set", tab="design", section={"on": False}, **SWITCHES).get("design", {})
    got = {k: design.get(k) for k in SWITCHES}
    if got != SWITCHES or (design.get("section") or {}).get("on") is not False:
        raise RuntimeError(f"view_set left the Design view's switches at {got}, section {design.get('section')}, "
                           f"not at {SWITCHES} with the section off")


def _same(a, b, tol=1e-12):
    """sketch_describe values equal, numbers to within tol mm. A constrained sketch is solved again
    after an undo, which can leave a coordinate a rounding step from where it was."""
    if isinstance(a, bool) or isinstance(b, bool):
        return a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return abs(a - b) <= tol
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(_same(a[k], b[k], tol) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(_same(x, y, tol) for x, y in zip(a, b))
    return a == b


# ---------------------------------------------------------------- widget_list's tree

# The parts of a line that can follow its value, in their order; a label or value ends at a quote
# followed by one of them or by the end of the line.
_AFTER_VALUE = (" (", " name=", " option=", ' in "', " [")


def parse_widgets(text):
    """widget_list's text tree as [Widget]. A line is
    `[ref] kind "label" = "value" (class) name=NAME option=O in "W" [flags] @x,y wxh`, every part
    after the kind optional and in that order."""
    out, stack = [], []
    for line in text.splitlines():
        stripped = line.lstrip(" ")
        if not stripped.startswith("["):
            continue
        close = stripped.find("] ")
        if close < 0:
            continue
        depth = (len(line) - len(stripped)) // 2
        ref = stripped[1:close]
        rest = stripped[close + 2:]
        kind, _, rest = rest.partition(" ")
        rest = " " + rest if rest else ""
        rect = None
        at = rest.rfind(" @")
        if at >= 0:
            try:
                xy, wh = rest[at + 2:].split(" ")
                x, y = (int(v) for v in xy.split(","))
                w, h = (int(v) for v in wh.split("x"))
                rect, rest = (x, y, w, h), rest[:at]
            except ValueError:
                pass
        label = value = name = ""
        if rest.startswith(' "'):
            label, rest = _quoted(rest, 2, (' = "',) + _AFTER_VALUE)
        if rest.startswith(' = "'):
            value, rest = _quoted(rest, 4, _AFTER_VALUE)
        if rest.startswith(" ("):
            rest = rest[rest.find(")") + 1:]
        if rest.startswith(" name="):
            ends = [i for i in (rest.find(t, 6) for t in _AFTER_VALUE[2:]) if i >= 0]
            end = min(ends, default=len(rest))
            name, rest = rest[6:end], rest[end:]
        while stack and out[stack[-1]].depth >= depth:
            stack.pop()
        out.append(Widget(ref, kind, label, value, name, rect, depth, stack[-1] if stack else None,
                          " [focused]" in rest))
        stack.append(len(out) - 1)
    return out


def _quoted(rest, start, then):
    """The text of a quoted field opening at rest[start] and the rest after its closing quote: the
    first quote followed by the end or by one of the fields `then` that can come next."""
    k = start
    while True:
        k = rest.find('"', k)
        if k < 0:
            return rest[start:], ""
        after = rest[k + 1:]
        if not after or any(after.startswith(t) for t in then):
            return rest[start:k], after
        k += 1


# ---------------------------------------------------------------- pixels <-> plane

# The viewport is a perspective camera looking at the sketch plane, so pixel <-> plane is a
# homography, not a scale: the same pixel span covers more millimetres at the far edge than at the
# near one. Four measured correspondences determine it exactly.

def _solve(A, b):
    """A small dense solve by Gauss-Jordan elimination with partial pivoting."""
    n = len(A)
    M = [row[:] + [b[i]] for i, row in enumerate(A)]
    for c in range(n):
        p = max(range(c, n), key=lambda r: abs(M[r][c]))
        if abs(M[p][c]) < 1e-12:
            raise RuntimeError("calibration is degenerate: the probe points are not in general position")
        M[c], M[p] = M[p], M[c]
        for r in range(n):
            if r == c:
                continue
            f = M[r][c] / M[c][c]
            for k in range(c, n + 1):
                M[r][k] -= f * M[c][k]
    return [M[i][n] / M[i][i] for i in range(n)]


def fit_homography(pairs):
    """pairs: [((X_mm, Y_mm), (u_px, v_px)), ...] -> 3x3 plane-to-pixel map, row-major, h22 = 1."""
    A, b = [], []
    for (X, Y), (u, v) in pairs:
        A.append([X, Y, 1, 0, 0, 0, -u * X, -u * Y])
        b.append(u)
        A.append([0, 0, 0, X, Y, 1, -v * X, -v * Y])
        b.append(v)
    h = _solve(A, b)
    return h + [1.0]


# ---------------------------------------------------------------- what a click can take

def _project(H, X, Y):
    """The point (X, Y) through the homography H (a row-major 3x3, as fit_homography() returns)."""
    w = H[6] * X + H[7] * Y + H[8]
    return ((H[0] * X + H[1] * Y + H[2]) / w, (H[3] * X + H[4] * Y + H[5]) / w)


def _handles(e):
    """The plane points of a sketch_describe entity that a click takes before the entity: a line's
    ends, a circle's centre and its radius grip on the +X side (DesignSketchTool::build_handles),
    a point itself."""
    if e["type"] == "line":
        return [e["p0"], e["p1"]]
    if e["type"] == "circle":
        return [e["center"], (e["center"][0] + e["radius"], e["center"][1])]
    if e["type"] == "point":
        return [e["p"]]
    raise ValueError(f"clear_pick knows lines, circles and points, not a {e['type']}")


def _outline(e, n=240):
    """Plane points along a sketch_describe entity, close enough together to stand for it."""
    if e["type"] == "line":
        (ax, ay), (bx, by) = e["p0"], e["p1"]
        return [(ax + (bx - ax) * k / n, ay + (by - ay) * k / n) for k in range(n + 1)]
    if e["type"] == "circle":
        (cx, cy), r = e["center"], e["radius"]
        return [(cx + r * math.cos(2 * math.pi * k / n), cy + r * math.sin(2 * math.pi * k / n))
                for k in range(n + 1)]
    return _handles(e)


def _polyline_dist(q, pts):
    """The distance from q to the polyline through pts, in the units of both."""
    if len(pts) == 1:
        return math.dist(q, pts[0])
    best = math.inf
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        dx, dy = bx - ax, by - ay
        t = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((q[0] - ax) * dx + (q[1] - ay) * dy) / (dx * dx + dy * dy)))
        best = min(best, math.dist(q, (ax + t * dx, ay + t * dy)))
    return best


def _rect_dist(q, rect):
    """The distance from q to the rectangle (x, y, w, h), 0 inside it."""
    x, y, w, h = rect
    return math.hypot(max(x - q[0], 0.0, q[0] - (x + w)), max(y - q[1], 0.0, q[1] - (y + h)))


class CadGui:
    """The Design tab of the app `client` (an orca_mcp.OrcaMcp) talks to, driven through its UI.

    Every method returns once the app has handled its input; the ones that wait for a result poll
    with until() rather than sleep. Checks are counted, and run() turns them into an exit code.
    """

    def __init__(self, client):
        # The checks compute with the design's numbers (a sketch's corners square to 1e-9), so they read them whole.
        client.exact_numbers = True
        self.client = client
        self.failures = 0
        self.safe = None   # (x0, x1, y0, y1) mm: the plane region the calibration probes covered
        self._H = None     # plane -> canvas pixel
        self._size = None  # (w, h) of the canvas _H maps onto
        self.gl_per_px = 1.0  # the canvas's GL pixels per click pixel, from reset_view()'s camera_state
        self._maps = {}  # _view_key() -> (_H, safe) fitted on that view

    # ------------------------------------------------------------ results

    def check(self, what, ok, detail=None):
        print(f"ok   {what}" if ok else f"FAIL {what}: {detail}", flush=True)
        if not ok:
            self.failures += 1
        return bool(ok)

    def run(self, rungs, names):
        """Run the rungs named (all of them when none is), in order; 1 if any check failed, else 0.

        A rung that raises fails as a check named after it. After a rung that failed, recover()
        puts the app back in a clean state, so the next rung does not inherit a held button, an
        open dialog, window or sketch, or an armed tool."""
        names = list(names) or list(rungs)
        unknown = [n for n in names if n not in rungs]
        if unknown:
            print(f"FAIL unknown rung {' '.join(unknown)}: have {' '.join(rungs)}")
            return 1
        for name in names:
            failed = self.failures
            try:
                rungs[name]()
            except Exception as e:  # a rung's error is its failure, and the run goes on
                self.check(name, False, f"{type(e).__name__}: {e}")
            if self.failures > failed:
                self.recover()
        return 1 if self.failures else 0

    def recover(self):
        """Back to an empty design with no sketch open and the view's switches as start() set them,
        whatever a failed rung left behind.

        Dialogs go first, since the canvas takes no input while one is open, then the modeless
        dialogs a rung opened, such as the Design tab's Text dialog. A press a rung left held is
        released before reset_view(), whose N would otherwise act under it."""
        steps = [("cancel dialogs", self._cancel_dialogs),
                 ("cancel modeless dialogs", self._cancel_modeless),
                 ("show the Design tab and its switches", lambda: reset_switches(self.client)),
                 ("release the mouse", lambda: self.gesture(action="up", **self._canvas_centre())),
                 ("reset the view", self.reset_view),
                 ("Escape", lambda: self.keys("Escape", target=CANVAS)),
                 ("cancel the sketch", lambda: self._sketch_or_none() and self.cancel()),
                 ("new design", self.new_design)]
        for what, step in steps:
            try:
                step()
            except Exception as e:  # the next step may still clean up what this one could not
                self.check(f"recover: {what}", False, f"{type(e).__name__}: {e}")

    def _cancel_dialogs(self):
        for _ in range(5):
            dialogs = self.client.call("app_state").get("dialogs", [])
            if not dialogs:
                return
            buttons = {b.get("label", "").lower() for b in dialogs[-1].get("buttons", [])}
            button = next((b for b in ("cancel", "no", "close") if b in buttons), "cancel")
            self.client.call("dialog_answer", button=button)
        raise RuntimeError("dialogs keep opening")

    def _cancel_modeless(self):
        """Close each shown dialog that is not modal as a person would: with its own Cancel, or with
        Escape in it when it has none, as the Speed Dial has none. dialog_answer and app_state's
        dialogs know only modal ones, so these are found among window_list's windows. The Cancel is
        looked for among that window's own widgets: within an empty title, as the Speed Dial's is,
        matches every window, so a click by label could press another's, such as the Design tab's."""
        for _ in range(5):
            shown = self._modeless_windows()
            if not shown:
                return
            _, texts = self.client.call_with_text("widget_list", target=shown[0]["ref"], max_nodes=5000,
                                                  max_web_nodes=5000)
            own = parse_widgets(texts[0] if texts else "")
            cancel = next((w for w in own if w.kind == "button" and w.label == "Cancel"), None)
            if cancel is not None:
                self.client.call("widget_click", target=cancel.ref)
            else:
                page = next((w for w in own if w.kind == "webview"), None)
                self.keys("Escape", target=page.ref if page is not None else shown[0]["ref"])
            if not until(lambda: len(self.modeless()) < len(shown)):
                raise RuntimeError(f"the dialog {shown[0]['title']!r} stays open after its "
                                   f"{'Cancel' if cancel is not None else 'Escape'}: {self.modeless()}")
        raise RuntimeError(f"modeless dialogs keep opening: {self.modeless()}")

    # ------------------------------------------------------------ reading

    def cad(self, tool, /, **args):
        """A Design command's reply. The command's name is positional-only, so an argument named tool
        (boolean's) goes to the command."""
        return self.client.call_command(tool, **args)

    def sketch(self):
        return self.cad("sketch_describe")

    def _sketch_or_none(self):
        """sketch_describe, or None when no sketch is open. Any other error (a modal dialog refuses
        every Design command while it is open) is raised, since it says nothing about the sketch."""
        try:
            return self.sketch()
        except orca_mcp.McpError as e:
            if "no sketch is open" in str(e):
                return None
            raise

    def widgets(self):
        """widget_list's tree as [Widget], asked for at the tool's largest size. A tree larger than
        that raises: the nodes past the cut include the canvas and its ImGui items, which would
        otherwise read as not shown."""
        _, texts = self.client.call_with_text("widget_list", max_nodes=5000, max_web_nodes=5000)
        text = texts[0] if texts else ""
        cut = next((line for line in text.splitlines() if line.startswith("... ") and " more nodes" in line), None)
        if cut is not None:
            raise RuntimeError(f"widget_list cut its tree short: {cut}")
        return parse_widgets(text)

    def modeless(self):
        """The titles of the shown dialogs that are not modal."""
        return [w["title"] for w in self._modeless_windows()]

    def _modeless_windows(self):
        """window_list's shown dialogs that are not modal, leaving out the person's own (hidden), such
        as a floating Run Script panel, which no tool reaches."""
        items = self.client.call("window_list").get("items", [])
        return [w for w in items if w.get("kind") == "dialog" and not w.get("modal") and not w.get("hidden")]

    @staticmethod
    def _canvas(ws):
        canvas = next((w for w in ws if w.kind == "canvas" and w.name == CANVAS["name"]), None)
        if canvas is None or canvas.rect is None:
            raise RuntimeError("the Design canvas is not shown")
        return canvas

    def _canvas_centre(self):
        _, _, w, h = self._canvas(self.widgets()).rect
        return {"x": w // 2, "y": h // 2}

    def picked(self):
        """How many entities Constrain mode has picked, read from the Design panel's status line
        ("2 picked  ·  now choose a constraint …"): 0 while it asks for the first pick, None while
        it shows anything else. sketch_describe's selection is the sketch session's and does not
        follow these picks."""
        for w in self.widgets():
            if w.kind != "text":
                continue
            m = re.match(r"(\d+) picked\b", w.label)
            if m:
                return int(m.group(1))
            if w.label.startswith("Constrain — click"):
                return 0
        return None

    def value_field(self):
        """The Design canvas's open value field, or None."""
        ws = self.widgets()
        canvas = ws.index(self._canvas(ws))

        def inside(i):
            while i is not None:
                if i == canvas:
                    return True
                i = ws[i].parent
            return False

        for w in ws:
            if w.kind == "imgui" and w.name == "sketchvalue_in" and inside(w.parent):
                title = next((t.label for t in ws if t.parent == w.parent and t.kind == "text"), "")
                return ValueField(title, w.value, w.ref, w.focused)
        return None

    def typing_field(self):
        """The canvas's open value field once it takes keys, or None.

        The field is listed from the frame that first draws it and takes the keyboard on the next,
        as a person sees it and then its cursor, and keys sent before then are lost. So whatever
        types into the field or commits it with Return waits for this, not for value_field()."""
        field = self.value_field()
        return field if field is not None and field.focused else None

    def settle(self):
        """Wait until the app has run what the last input set off, so that what is read next is what the input left.

        The Design tab answers a click or a key in its handler and in work the handler, or the frame drawn after the
        input, queues on the UI thread: the sketch a plane pick opens, the value field a drawn shape opens
        (DesignSketchTool's draw-then-edit chain), the next field a Return opens. mouse_gesture and keyboard_press draw
        a frame after each event they send, and the app answers each call on the UI thread after the work queued there
        before it, so once app_wait {for: "idle"} answers, which also needs no dialog open and no job running, that
        work has run."""
        checks.wait(self.client, "idle", 30)

    def opened_field(self):
        """The value field the last input left open, or None when it left none, read once the app has run what the
        input set off (settle()): widget_list draws the canvas before it lists it, so it lists a field that is open,
        as an open sketch's editing says."""
        self.settle()
        field = self.value_field()
        if field is None and (self._sketch_or_none() or {}).get("editing"):
            field = until(self.value_field)
            if field is None:
                raise RuntimeError("sketch_describe says a value field is open, and widget_list lists none")
        return field

    # ------------------------------------------------------------ input

    def keys(self, chord, target=None):
        args = {"keys": chord}
        if target is not None:
            args["target"] = target
        return self.client.call("keyboard_press", **args)

    def text(self, s):
        return self.client.call("keyboard_press", text=s)

    def press(self, label, within=None):
        """Click a widget by its label, in the window titled `within` when one is named. A click
        that opens a dialog replies blocked_by_dialog."""
        target = {"label": label}
        if within is not None:
            target["within"] = within
        return self.client.call("widget_click", target=target)

    def fill(self, target, value, submit=True):
        """Give the widget `target` names (a widget_list locator) `value` with widget_set_value,
        the way into a native text field, which key presses never type into. `submit` also
        presses Enter in it, as the tool does by default."""
        return self.client.call("widget_set_value", target=target, value=value, submit=submit)

    def row(self, label, double=False):
        return self.client.call("widget_click", target={"row": label}, double=double)

    def menu(self, path):
        return self.client.call("widget_click", target={"menu": path})

    def gesture(self, **args):
        return self.client.call("mouse_gesture", canvas="design", **args)

    def _answer(self, reply, title, button="yes"):
        """When `reply` is a call held by the dialog titled `title`, answer it and finish the call."""
        if reply.get("status") != "blocked_by_dialog":
            return reply
        self.client.call("dialog_answer", button=button, dialog={"title": title})
        return self.client.await_job(reply, timeout=60)

    # ------------------------------------------------------------ session

    def start(self, first_showing=False):
        """Put the app in the state every Design test starts from: check the UI is English, put the app
        in the baseline (checks.start()), set the view's switches (reset_switches()), make the window
        wide enough for the sketch ribbon, show the Design tab in reset_view()'s view and start an
        empty design.

        `first_showing` first checks that the Design tab lays out its canvas on its first showing as on
        a later one, which only an app that has not built the tab yet can tell: the test that asks for
        it runs in a newly started app. It comes before checks.start(), whose cancel_tool builds the
        tab unshown, so that the tab is built by its first showing, as a person's first click on it
        builds it."""
        if not self.client.call("app_state").get("design_tab"):
            print("the Design tab is off: launch the app with tools/mcp/orca_mcp.py launch --cad")
            sys.exit(2)
        ws = self.widgets()
        if not (any(w.kind == "menu" and w.label == "File" for w in ws)
                and any(w.kind == "tab" and w.label == "Design" for w in ws)):
            print("the UI must be in English: set Preferences > General > Language to English and restart")
            sys.exit(2)
        if first_showing:
            # A canvas laid out under a ribbon of the wrong height moves when the tab is shown again,
            # and a click aimed through a map fitted before the move misses by that much.
            self.client.call("view_set", tab="design")
            first = self._canvas(self.widgets()).rect
            self.client.call("view_set", tab="prepare")
            self.client.call("view_set", tab="design")
            again = self._canvas(self.widgets()).rect
            self.check("the Design tab's first showing lays out its canvas as a later one does", first == again,
                       f"{first}, then {again}")
        checks.start(self.client)
        reset_switches(self.client)
        # The ribbon shows its sketch buttons only while a sketch is open, so one is opened to see
        # where Cancel, the last of them, lands. It sits after a stretch spacer, so once the window
        # is wider than the ribbon it moves in from the canvas's right edge (the window's).
        self.show_design()
        self.new_design()
        self._open_sketch()
        ws = self.widgets()
        cancel = next((w for w in ws if w.kind == "button" and w.label == "Cancel" and w.rect), None)
        if cancel is None:
            raise RuntimeError("the sketch ribbon shows no Cancel button")
        cx, _, cw, _ = self._canvas(ws).rect
        over = cancel.rect[0] + cancel.rect[2] - (cx + cw)
        self.cancel()
        if over > 0:
            width, height = self.client.call("app_state")["window"]["size"]
            self.client.call("window_set", size=[width + over + cancel.rect[3], height])
        self.show_design()
        self.new_design()

    def show_design(self):
        """Bring the Design tab to the front, in the view reset_view() sets."""
        self.client.call("view_set", tab="design")
        self.reset_view()

    def reset_view(self):
        """Put the Design camera in one view, whatever an earlier test, rung or project open left:
        the top camera at RESET_ZOOM, centred on describe_design's modeling_origin, the sketch planes'
        origin, and squared to the sketch's plane when a sketch is open.

        view_set's look_at is in the Design view's world coordinates, where modeling_origin is not
        zero: a new design puts it at the Design bed's centre. Its directions fit only what has volumes, so on an
        empty design they turn the camera and keep its zoom and centre; the look_at and zoom it
        applies after the direction set both. N in a sketch squares the view to the sketch's plane
        and centres it on the plane's origin, keeping the zoom. Every map calibrate() fits stands
        on this view, so the camera view_set reports is checked. Its camera_state gives the canvas's
        GL pixels per click pixel, and the view is set again when they differ from the ones the zoom
        assumed."""
        origin = self.cad("describe_design")["modeling_origin"]

        def aim():
            return self.client.call("view_set", tab="design", camera="top", look_at=origin,
                                    zoom=RESET_ZOOM * self.gl_per_px).get("camera_state", {})

        state = aim()
        if state.get("click_px_per_mm"):
            gl_per_px = state["zoom"] / state["click_px_per_mm"]
            if abs(gl_per_px - self.gl_per_px) > 1e-3:
                self.gl_per_px = gl_per_px
                state = aim()
        zoom, target = state.get("zoom"), state.get("look_at")
        if (zoom is None or abs(zoom - RESET_ZOOM * self.gl_per_px) > 1e-3 or target is None
                or max(abs(a - b) for a, b in zip(target, origin)) > 1e-3):
            raise RuntimeError(f"view_set left the Design camera at {state}, not at modeling_origin "
                               f"{origin} with zoom {RESET_ZOOM * self.gl_per_px:.4f}")
        if self._sketch_or_none():
            self.keys("n", target=CANVAS)

    def new_design(self):
        """Clear the feature tree with the ribbon's New Design, confirming when it asks."""
        self._answer(self.press(NEW_DESIGN), "New Design")
        features = self.cad("describe_design")["features"]
        if features:
            raise RuntimeError(f"New Design left {len(features)} feature(s)")

    def _sketch_ok(self):
        return self._sketch_or_none() is not None

    def _open_sketch(self):
        """Open a sketch on the XY reference plane, by the route enter_sketch() describes. Each click's
        outcome is read once the app has run what the click set off (settle()), so a click that
        picked no plane is told from one whose sketch is still opening."""
        if self._sketch_or_none():
            raise RuntimeError("a sketch is already open")
        self.show_design()
        self.keys("Shift+S", target=CANVAS)
        _, _, w, h = self._canvas(self.widgets()).rect
        step = 0.02 * min(w, h)
        for k in range(1, 16):
            self.gesture(x=round(w / 2 + k * step), y=round(h / 2 + k * step))
            self.settle()
            if self._sketch_ok():
                return
        raise RuntimeError("no sketch opened: no click along the XY plane's diagonal picked it")

    def enter_sketch(self, tool_key=None):
        """Open a sketch on the XY plane through the UI, calibrate on it and arm `tool_key`; returns
        sketch_describe.

        The route to the plane, with no pixel constant: show_design() shows the Design tab in the
        top camera, which looks straight down on the modeling origin, so the origin is at the
        canvas's centre. Shift+S on the canvas brings the reference planes up. The XY plane's
        square lies off the origin toward the canvas's lower right (the squares sit in the +X, -Y,
        +Z octant), from a small gap out to a size set by the bed, so the clicks walk out from the
        centre along that diagonal, in steps of 2% of the canvas's smaller side, until one opens
        the sketch. A click that misses lands on the bed or the background and leaves the plane
        choice waiting for the next. Along that diagonal only the XY square can be hit: the XZ and
        YZ squares are seen edge-on, along the axes. The click that picks the plane also pops the
        tool offer, which the reply lists and nothing shows. calibrate() then squares the view on
        the sketch, in the camera show_design() set, and measures it."""
        self._open_sketch()
        self.calibrate(aimed=True)
        if tool_key:
            self.keys(tool_key, target=CANVAS)
        return self.sketch()

    def confirm(self):
        self.press("Confirm")
        if not until(lambda: self._sketch_or_none() is None):
            raise RuntimeError("the sketch is still open after Confirm")

    def cancel(self):
        """Leave the open sketch with the ribbon's Cancel, discarding what it holds."""
        self._answer(self.press("Cancel"), "Discard sketch")
        if not until(lambda: self._sketch_or_none() is None):
            raise RuntimeError("the sketch is still open after Cancel")

    def constrain(self):
        """Commit the open sketch and enter Constrain mode on it with K, as a person does; returns
        sketch_describe.

        Constrain mode hides the sketch's banner above the canvas, so the canvas grows and the
        view moves with its centre. The map follows that move (_follow_canvas()) rather than being
        measured again, since Constrain mode has no Point tool to place probes with."""
        self.keys("k", target=CANVAS)
        if not until(lambda: (self._sketch_or_none() or {}).get("tool") == "constrain"):
            raise RuntimeError("K did not enter Constrain mode")
        self._follow_canvas()
        return self.sketch()

    def reopen(self, row=None):
        """Open a committed sketch again from its Feature tree row and calibrate on it; returns
        sketch_describe.

        `row` names the row. None takes the newest sketch feature describe_design lists: New Design
        keeps counting features, so the first sketch of a design is "Sketch 1" only in a session
        that has committed none before it."""
        if row is None:
            sketches = [f["name"] for f in self.cad("describe_design")["features"] if f.get("type") == "Sketch"]
            if not sketches:
                raise RuntimeError("no sketch feature to reopen")
            row = sketches[-1]
        self.row(row, double=True)
        if not until(self._sketch_ok):
            raise RuntimeError(f"no sketch opened from the row {row}")
        self.calibrate()
        return self.sketch()

    # ------------------------------------------------------------ plane and pixels

    def calibrate(self, aimed=False):
        """Put the open sketch in the view reset_view() sets and find its plane-to-pixel map:
        reuse the one fitted on that view before, or place Points, fit the map to them and undo
        them.

        Done for every sketch entered or reopened, on the view reset_view() sets, so the map is
        the same whatever came before and every click lands where the test means it to. `aimed`
        says the camera is the one show_design() has just set, as enter_sketch() leaves it, so N
        alone squares it to the sketch's plane; otherwise reset_view() sets the camera first.

        A map is reused while the canvas's size and the camera are the ones it was fitted on
        (_view_key()), which fix how the plane projects, and one probe confirms it (_check_map()):
        what the key cannot show, another plane seen through the same camera or a probe snapped
        onto the sketch's geometry, puts that probe off the map, and the map is fitted afresh.

        A fit takes four probes at fixed shares of the canvas, and a fifth checks it
        (PROBE_LAYOUTS). A probe that snapped onto geometry already in the sketch shows as a probe
        off its click, and the probes are then placed again in the next layout, whose band lies
        elsewhere. In an empty sketch there is nothing to snap onto, so a probe off its click
        means the clicks did not land where the view puts them, a fault in the app or the
        harness, and it raises at once. The sketch is left as it was, in Select."""
        if aimed:
            self.keys("n", target=CANVAS)
        else:
            self.reset_view()
        _, _, w, h = self._canvas(self.widgets()).rect
        key = self._view_key(w, h)
        if key in self._maps and self._check_map(self._maps[key][0], w, h):
            H, self.safe = self._maps[key]
            self._H, self._size = list(H), (w, h)
            return
        snapped = []
        for layout in PROBE_LAYOUTS:
            probes = [(round(w * fx), round(h * fy)) for fx, fy in layout]
            start, new = self._place_probes(probes)
            self._H = fit_homography([((e["p"][0], e["p"][1]), probes[i]) for i, e in enumerate(new[:4])])
            # A homography through its own four points is exact, so the first four can only show
            # a failed solve. A probe that snapped onto geometry already in the sketch sits where
            # the snap put it, a few pixels from its click, and the Point tool adds no constraint
            # for that, so the fifth is what catches it: its point is where the click unprojects,
            # to well under a pixel, only when all five landed where they were clicked.
            off = []
            for i, e in enumerate(new):
                u, v = self.px(*e["p"])
                if abs(u - probes[i][0]) > 0.5 or abs(v - probes[i][1]) > 0.5:
                    off.append(f"probe {i} at {(round(u, 2), round(v, 2))} for {probes[i]}")
            self._undo_probes(start, len(new))
            if not off:
                xs, ys = [e["p"][0] for e in new[:4]], [e["p"][1] for e in new[:4]]
                self.safe = (min(xs), max(xs), min(ys), max(ys))
                self._size = (w, h)
                # A copy: _follow_canvas() replaces _H in Constrain mode, and the stored map stays the fitted one.
                self._maps[key] = (list(self._H), self.safe)
                return
            if not start["entities"]:
                raise RuntimeError("calibration probes are off their clicks in an empty sketch, where nothing "
                                   "can snap them, so the clicks do not land where the view puts them (an app or "
                                   "harness fault): " + ", ".join(off))
            snapped.append(", ".join(off))
        raise RuntimeError("calibration probes are off their clicks in every layout, so they snapped onto "
                           "the sketch: " + "; ".join(snapped))

    def _view_key(self, w, h):
        """What a plane-to-pixel map stands on besides the sketch's plane: the canvas's size and
        the Design camera app_state reports."""
        camera = self.client.call("app_state").get("camera") or {}
        return (w, h, json.dumps(camera, sort_keys=True))

    def _check_map(self, H, w, h):
        """Whether the map H holds on the open sketch: a Point clicked where a fit's fifth probe
        goes lands within 0.5 px of where H puts it. The probe is undone either way."""
        fx, fy = PROBE_LAYOUTS[0][4]
        probe = (round(w * fx), round(h * fy))
        start, new = self._place_probes([probe])
        self._undo_probes(start, 1)
        u, v = _project(H, *new[0]["p"])
        return abs(u - probe[0]) <= 0.5 and abs(v - probe[1]) <= 0.5

    def _place_probes(self, probes):
        """Click a Point at each of `probes` (pixels) in the open sketch; returns sketch_describe
        before the clicks and the new points."""
        start = self.sketch()
        before = start["entities"]
        self.keys("p", target=CANVAS)
        for u, v in probes:
            self.gesture(x=u, y=v)
        placed = self.sketch()
        ents = placed["entities"]
        new = ents[len(before):]
        if not _same(ents[:len(before)], before) or len(new) != len(probes) or any(e["type"] != "point" for e in new):
            raise RuntimeError(f"calibration expected {len(probes)} new points, got {[e['type'] for e in new]}")
        if placed["constraints"] != start["constraints"]:
            raise RuntimeError(f"the calibration probes constrained the sketch: constraints went from "
                               f"{start['constraints']} to {placed['constraints']}")
        return start, new

    def _undo_probes(self, start, n):
        """Take away the `n` probes _place_probes() placed on the sketch `start`, back in Select."""
        self.keys("Escape", target=CANVAS)  # Point back to Select
        for _ in range(n):
            self.keys("Ctrl+Z", target=CANVAS)  # the probes are scaffolding, not geometry
        left = self.sketch()
        if (not _same(left["entities"], start["entities"]) or left["constraints"] != start["constraints"]
                or left["tool"] != "select"):
            raise RuntimeError(f"the calibration probes survived the undo: {len(left['entities'])} entities, "
                               f"{left['constraints']} constraints, tool {left['tool']}")

    def _follow_canvas(self):
        """Keep the map through a change of the canvas's size, with the camera left where it was.

        The camera's scale, pixels per millimetre, does not depend on the canvas's size, and its
        view is centred on the canvas, so a resize leaves each plane point's offset from the
        canvas's centre as it was: the map moves by the move of the centre."""
        _, _, w, h = self._canvas(self.widgets()).rect
        dx, dy = (w - self._size[0]) / 2, (h - self._size[1]) / 2
        H = self._H
        self._H = [H[0] + dx * H[6], H[1] + dx * H[7], H[2] + dx * H[8],
                   H[3] + dy * H[6], H[4] + dy * H[7], H[5] + dy * H[8], H[6], H[7], H[8]]
        self._size = (w, h)

    def px(self, X, Y):
        """Plane millimetres -> canvas pixels."""
        return _project(self._H, X, Y)

    def unpx(self, u, v):
        """Canvas pixels -> plane millimetres (the homography inverted)."""
        h = self._H
        A = [[h[0] - u * h[6], h[1] - u * h[7]], [h[3] - v * h[6], h[4] - v * h[7]]]
        b = [u * h[8] - h[2], v * h[8] - h[5]]
        return tuple(_solve(A, b))

    def mm_per_px(self, X, Y):
        """The view's local scale at a plane point, what the sketch tool calls unit_per_px."""
        u, v = self.px(X, Y)
        return math.dist(self.unpx(u, v), self.unpx(u + 1.0, v))

    def _pixel(self, X, Y):
        u, v = self.px(X, Y)
        return round(u), round(v)

    def click_mm(self, X, Y, **kw):
        u, v = self._pixel(X, Y)
        return self.gesture(x=u, y=v, **kw)

    def landed(self, X, Y):
        """The plane point click_mm(X, Y) lands on: the whole pixel the click is rounded to, mapped
        back. A construction graded against it grades the tool, not the click's rounding."""
        return self.unpx(*self._pixel(X, Y))

    def budget(self, gl_px):
        """A length in the canvas's GL pixels, as DesignSketchTool's budgets are, in click pixels."""
        return gl_px / self.gl_per_px

    def badge(self, line, k):
        """Where badge `k` (from 0) of the sketch line `line` is drawn, in mm (BADGE_STEP_PX)."""
        (ax, ay), (bx, by) = line["p0"], line["p1"]
        mx, my = (ax + bx) / 2.0, (ay + by) / 2.0
        return (mx, my + self.budget(BADGE_STEP_PX) * (k + 1) * self.mm_per_px(mx, my))

    def move_mm(self, X, Y):
        u, v = self._pixel(X, Y)
        return self.gesture(action="move", x=u, y=v)

    def drag_mm(self, a, b, **kw):
        u, v = self._pixel(*a)
        return self.gesture(action="drag", x=u, y=v, to=list(self._pixel(*b)), **kw)

    def overlays(self):
        """The rects (x, y, w, h), in canvas pixels, of the ImGui windows the canvas shows: its
        status line, and each value label in a window of its own centred on the label."""
        ws = self.widgets()
        canvas = ws.index(self._canvas(ws))
        return [w.rect for w in ws if w.kind == "imgui_window" and w.parent == canvas and w.rect]

    def clear_pick(self, entity):
        """A plane point on `entity` (a line or circle of the open sketch, found again by its
        index) where a left click in Select takes that entity and nothing else; raises when the
        view leaves none.

        A click in Select takes the first of these in reach: a value label within LABEL_PICK_PX
        of its centre, then a handle, a point or a curve within PICK_PX
        (DesignSketchTool::on_mouse_impl). So of the whole pixels along the entity, the click
        goes to the one farthest past those budgets from everything else: every ImGui window on
        the canvas and its centre (the labels shown, read now, since which show depends on what
        is picked), the entity's own handles, and every other entity and its handles. It must
        clear each by PICK_MARGIN_PX, at any zoom, since the budgets are in pixels. Badges are
        not counted: the sketches this is used on hold only dimensions, which show as labels."""
        d = self.sketch()
        target = d["entities"][entity["index"]]
        if target["type"] not in ("line", "circle"):
            raise ValueError(f"clear_pick picks lines and circles, not a {target['type']}")
        others = [e for e in d["entities"] if e["index"] != target["index"]]
        windows = self.overlays()
        curve = [self.px(*p) for p in _outline(target)]
        avoid_points = [self.px(*p) for e in [target] + others for p in _handles(e)]
        avoid_curves = [[self.px(*p) for p in _outline(e)] for e in others]
        w, h = self._size
        pick, label = self.budget(PICK_PX), self.budget(LABEL_PICK_PX)
        best = None
        for q in sorted({(round(u), round(v)) for u, v in curve}):
            if not (0 <= q[0] < w and 0 <= q[1] < h):
                continue
            slack = min([pick - _polyline_dist(q, curve)]
                        + [math.dist(q, a) - pick for a in avoid_points]
                        + [_polyline_dist(q, c) - pick for c in avoid_curves]
                        + [math.dist(q, (x + rw / 2.0, y + rh / 2.0)) - label for x, y, rw, rh in windows]
                        + [_rect_dist(q, r) for r in windows])
            if best is None or slack > best[0]:
                best = (slack, q)
        if best is None or best[0] < PICK_MARGIN_PX:
            raise AssertionError(f"no pixel along the {target['type']} {target['index']} is {PICK_MARGIN_PX} px clear "
                                 f"of the labels and other geometry a click there could take"
                                 + (f" (best {best[0]:.1f} px at {best[1]})" if best else ""))
        return self.unpx(*best[1])

    # ------------------------------------------------------------ values

    def value(self, v):
        """Type `v` into the value field the canvas has open and commit it with Return.

        The field opens pre-filled and selected, and the canvas holds the keyboard, so the digits go
        in with no click and no target, as a person types them."""
        field = until(self.typing_field)
        if field is None:
            shown = self.value_field()
            raise AssertionError(f"no value field open to take {v}" if shown is None else
                                 f"the {shown.title} field never took the keyboard to take {v}")
        self.text(str(v))
        self.keys("Return")

        def moved_on():  # Return closes the field or opens the next one
            f = self.value_field()
            return f is None or (f.title, f.prefill) != (field.title, field.prefill)

        until(moved_on)

    def values(self, *vs):
        for v in vs:
            self.value(v)

    # ------------------------------------------------------------ menus

    def offer(self, X, Y):
        """The offer menu a right click at (X, Y) mm opens, listed and not shown."""
        menu = self.click_mm(X, Y, button="right").get("popup_menu") or {}
        items = list(menu.get("items", []))
        disabled = list(menu.get("disabled", []))
        header = items[0] if items and items[0] in disabled else None
        if header is not None:
            items.pop(0)
            disabled.remove(header)
        return Offer(header, items, disabled, list(menu.get("checked", [])))

    def choose(self, X, Y, path):
        """Run the item `path` of the offer menu a right click at (X, Y) mm opens."""
        return self.click_mm(X, Y, button="right", popup_item=path)

    def verbs(self, sketch_mode=True):
        """The verbs of the right-click menu, in the Design table's order, for the selection it would be built
        for now (describe_design's selection): those of the sketch vocabulary, or with sketch_mode False of the
        model one, or with None both. Each has id, name, row, row_name, family (its submenu within the row,
        or None), key, action, sketch_mode, applies (it accepts the selection), modal and hint, as the table
        has them; entries with no menu item (commands only) are left out."""
        with open(ATLAS, encoding="utf-8") as f:
            atlas = json.load(f)
        slots = atlas["slots"]
        rows = [s["id"] for s in slots]
        selection = self.cad("describe_design")["selection"]["id"]
        out = []
        for v in atlas["verbs"]:
            if v["slot"] is None:
                continue
            sketch = v.get("mode") == "sketch"
            if sketch_mode is not None and sketch != sketch_mode:
                continue
            row = rows.index(v["slot"])
            out.append({"id": v["id"], "name": v["name"], "row": row, "row_name": slots[row]["label"],
                        "family": v.get("family"), "key": v.get("key"), "action": v.get("action"),
                        "sketch_mode": sketch, "applies": selection in v["accepts"],
                        "modal": bool(v.get("modal")), "hint": v.get("hint")})
        return out
