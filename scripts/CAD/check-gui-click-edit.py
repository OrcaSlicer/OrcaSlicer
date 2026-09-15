#!/usr/bin/env python3
"""The click-edit contract: a value field that opens must accept what is TYPED into it.

WHY THIS EXISTS SEPARATELY FROM check-gui-sketching.py. That ladder draws geometry and grades the
result, and to make its values land it calls focus_field() — one synthetic click INTO the field
before typing. Its own docstring says why:

    WITHOUT THIS THE TYPED VALUE IS SILENTLY DISCARDED. The field is shown and raised but the
    window manager does not give it the keyboard, so xdotool's digits go to the canvas and Return
    commits the value the field opened with — the pre-filled as-drawn number.

That click is a workaround for a defect, and a suite that performs it can never see the defect
again. A user cannot be told to click the field first; when they do not, they get the as-drawn
number and report "the label value is not editable". So this ladder types IMMEDIATELY after the
field opens, exactly as a person does, and fails if the prefill is what gets committed.

WHAT IT GRADES. The app emits one line per event under ORCA_CAD_UXTRACE=1:

    [UX] open    title=Length prefill=154.76
    [UX] commit  title=Length typed=80 value=80.0000
    [UX] refused title=Length typed=8O
    [UX] cancel  title=Length

For every field the driver opens it asserts: a commit arrived, what the field received is what we
typed, the parsed value equals it, and it differs from the prefill. The last clause is the one
that matters — a field that is on screen but deaf commits its prefill, and every other signal
(the field is visible, a constraint is created, the solve succeeds) looks perfectly healthy.

    scripts/CAD/check-gui-click-edit.py --display :10 --bin build/src/Release/orca-slicer

With --attach it drives an already-running app instead of launching one; the app must have been
started with ORCA_CAD_UXTRACE=1 and its stderr redirected to --trace.
Exit 0 = every field took what was typed.
"""
import argparse, json, os, re, shutil, signal, socket, subprocess, sys, tempfile, time

AP = argparse.ArgumentParser()
AP.add_argument("--display", default=os.environ.get("DISPLAY", ":10"))
AP.add_argument("--bin", default="build/src/Release/orca-slicer")
AP.add_argument("--datadir", default="")
AP.add_argument("--trace", default="")
AP.add_argument("--sock", default="/tmp/mcp-uxcheck.sock",
                help="the app's MCP socket: the oracle for whether a sketch is really open")
AP.add_argument("--attach", action="store_true", help="drive a running app; do not launch one")
AP.add_argument("--keep", action="store_true", help="leave the app running afterwards")
AP.add_argument("--no-defocus", action="store_true",
                help="do NOT take focus off the field before typing (weakens the gate; see below)")
AP.add_argument("--seed-from", default=os.path.expanduser("~/.config/OrcaCAD/OrcaSlicer.conf"),
                help="an existing OrcaSlicer.conf to copy presets/settings from")
A = AP.parse_args()

DISP = A.display
TRACE = A.trace or os.path.join(tempfile.gettempdir(), "ux-click-edit.log")
_fail = 0
_checks = 0


_n = 0


def call(method, **params):
    """One MCP request over the app's unix socket. The socket is the only witness that cannot
    lie about sketch state: the keytrace says a key ARRIVED, a screenshot says something is on
    screen, and neither distinguishes an open sketch from sketch mode with the plane offer up."""
    global _n
    _n += 1
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(A.sock)
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


def sh(cmd):
    # bash -c, NOT -lc: a login shell sources the profile on every xdotool call, and this driver
    # makes hundreds. On a GNOME box that meant im-config running per call, thousands of journal
    # lines, and a window poll slow enough to time out before the app had finished starting.
    return subprocess.run(["bash", "-c", cmd], capture_output=True, text=True).stdout


def xdo(args):
    sh(f"DISPLAY={DISP} xdotool {args}")


def key(k, pause=0.35, window=None):
    xdo(f"key {'--window ' + str(window) + ' ' if window else ''}{k}")
    time.sleep(pause)


def typ(s, pause=0.35, window=None):
    # --clearmodifiers so a modifier left down by an earlier synthetic key cannot turn digits
    # into something else; --delay 60 because ImGui reads one character per frame.
    #
    # `window` targets a specific window with XSendEvent instead of following the input focus.
    # That is the whole gate: see type_into_open_field.
    tgt = f"--window {window} " if window else ""
    xdo(f"type {tgt}--clearmodifiers --delay 60 -- '{s}'")
    time.sleep(pause)


def die(msg):
    print(f"FATAL {msg}", file=sys.stderr)
    sys.exit(2)


# ---------------------------------------------------------------- the app

_proc = None


def seed_datadir(datadir):
    """The Design tab does not exist unless enable_cad_feature is on, and it needs a RESTART.

    A fresh datadir has it off, so a driver that just points the app at an empty directory gets
    Prepare/Preview/Device/Project, no Design tab, and every rung fails for a reason that has
    nothing to do with what is being tested. Seed the flag before the first launch.
    """
    os.makedirs(datadir, exist_ok=True)
    conf = os.path.join(datadir, "OrcaSlicer.conf")
    data = {}
    if os.path.exists(A.seed_from):
        try:
            with open(A.seed_from) as f:
                data = json.load(f)
        except Exception:
            data = {}
    app = data.setdefault("app", {})
    app["enable_cad_feature"] = True
    # Deterministic starting state for the rungs that follow: the bed drawn, loops welded as the
    # ~90% case expects. A ladder whose result depends on the developer's own preferences is not
    # a gate.
    app["auto_close_sketch_loops"] = True
    # SILENCE THE NETWORK PLUGIN PROMPT. Without this, GUI_App::post_init() re-raises "Bambu
    # Network Plug-in Required" from an IDLE event — after any modal sweep this driver does at
    # startup — and ShowModal() then runs a nested event loop. The app is alive, its window is
    # there, and the MCP socket answers nothing: indistinguishable from a hang, and it was
    # investigated as one, with gdb, twice. `installed_networking` false stops the whole
    # networking-plugin path, so m_networking_need_update is never set and the dialog never
    # exists to be swept.
    app["installed_networking"] = False
    with open(conf, "w") as f:
        json.dump(data, f, indent=1)
    for sub in ("user", "system", "presets", "vendor"):
        src = os.path.join(os.path.dirname(A.seed_from), sub)
        dst = os.path.join(datadir, sub)
        if os.path.isdir(src) and not os.path.exists(dst):
            shutil.copytree(src, dst)


def launch():
    global _proc
    datadir = A.datadir or os.path.join(tempfile.gettempdir(), "orcacad-uxcheck")
    seed_datadir(datadir)
    env = dict(os.environ)
    # WAYLAND_DISPLAY MUST GO, and GDK_BACKEND must say x11. GTK prefers Wayland whenever
    # WAYLAND_DISPLAY is set and ignores DISPLAY entirely, so a driver launched from a systemd
    # user unit (which inherits it) started the app on the DESKTOP session instead of the rig:
    # the process was alive, `xdotool search` on the rig display found nothing, and the window
    # was sitting on the user's own screen. Silent, and it drives a stray app at someone's face.
    env.pop("WAYLAND_DISPLAY", None)
    env.update(DISPLAY=DISP, GDK_BACKEND="x11", ORCA_CAD_UXTRACE="1",
               LIBGL_ALWAYS_SOFTWARE="1", GALLIUM_DRIVER="llvmpipe",
               # The rig's Xvfb has no input-method daemon, and a dead ibus context makes a
               # GtkEntry drop every character while the app looks fine. It cannot affect the
               # in-canvas field (ImGui needs no IM) but the app has other text fields, and a
               # display full of IBUS warnings has cost a whole misdiagnosis before.
               GTK_IM_MODULE="gtk-im-context-simple", XMODIFIERS="@im=none",
               # The key tracer is this driver's only positive signal that a keystroke reached
               # the Design panel at all. Without it "the field never opened" is indistinguishable
               # from "we never got into sketch mode", and the first run of this ladder reported
               # seven product failures that were really one driver racing a still-loading app.
               ORCA_CAD_KEYTRACE="1", ORCA_CAD_MCP=A.sock,
               SSL_CERT_FILE="/etc/ssl/certs/ca-certificates.crt",
               WEBKIT_DISABLE_DMABUF_RENDERER="1", WEBKIT_DISABLE_COMPOSITING_MODE="1")
    if os.path.exists(A.sock):
        os.unlink(A.sock)          # a stale socket from a dead run answers nothing, slowly
    log = open(TRACE, "wb")
    _proc = subprocess.Popen([A.bin, "--datadir", datadir], env=env,
                             stdout=subprocess.DEVNULL, stderr=log)
    for _ in range(120):
        if win_id():
            return
        time.sleep(1)
    die("the app never showed a window on " + DISP)


def window_pid(w):
    """_NET_WM_PID for a window, or 0. The property is how we tell a live app from its ghost."""
    out = sh(f"DISPLAY={DISP} xprop -id {w} _NET_WM_PID 2>/dev/null")
    m = re.search(r"= *(\d+)", out)
    return int(m.group(1)) if m else 0


def pid_alive(pid):
    return pid > 0 and os.path.isdir(f"/proc/{pid}")


def win_id():
    """The main window: OURS if we launched it, otherwise the biggest LIVE top-level.

    Two rules here, each paid for.

    By PID, not by size, whenever we launched the app. An X window outlives its client if the
    connection is not torn down cleanly, and a killed OrcaSlicer can leave a full-screen ghost
    mapped on the display. It answers geometry queries exactly like the real thing, it wins "the
    biggest window" every time, and every synthetic keystroke sent to it goes nowhere. That is
    indistinguishable, from the driver's side, from an app that ignores the keyboard — which is
    the very defect this ladder exists to measure. One run reported the entire contract broken
    while the real app sat beside the ghost, untouched.

    Never by title: a saved project renames the main window.
    """
    if _proc is not None:
        for w in sh(f"DISPLAY={DISP} xdotool search --pid {_proc.pid} --onlyvisible --name '.'").split():
            g = dict(l.split("=", 1) for l in
                     sh(f"DISPLAY={DISP} xdotool getwindowgeometry --shell {w}").strip().splitlines()
                     if "=" in l)
            if "WIDTH" in g and int(g["WIDTH"]) * int(g["HEIGHT"]) > 400 * 400:
                return (w, int(g["X"]), int(g["Y"]), int(g["WIDTH"]), int(g["HEIGHT"]))
        return None
    best = None
    for w in sh(f"DISPLAY={DISP} xdotool search --onlyvisible --name '.'").split():
        g = dict(l.split("=", 1) for l in
                 sh(f"DISPLAY={DISP} xdotool getwindowgeometry --shell {w}").strip().splitlines()
                 if "=" in l)
        if "WIDTH" not in g:
            continue
        if not pid_alive(window_pid(w)):          # a ghost: no client is behind it any more
            continue
        a = int(g["WIDTH"]) * int(g["HEIGHT"])
        if a > 400 * 400 and (best is None or a > best[0]):
            best = (a, w, int(g["X"]), int(g["Y"]), int(g["WIDTH"]), int(g["HEIGHT"]))
    return best[1:] if best else None


_win = None


def win():
    global _win
    if _win is None:
        w = win_id()
        if w is None:
            die("no app window on " + DISP)
        sh(f"DISPLAY={DISP} xdotool windowactivate --sync {w[0]}")
        sh(f"DISPLAY={DISP} xdotool windowsize {w[0]} 1920 1080")
        sh(f"DISPLAY={DISP} xdotool windowmove {w[0]} 0 0")
        time.sleep(1.0)
        _win = (w[0], 0, 0, 1920, 1080)
    return _win


def click(px, py, pause=0.5, btn=1):
    _, X, Y, _, _ = win()
    xdo(f"mousemove {X+int(px)} {Y+int(py)} click --delay 120 {btn}")
    time.sleep(pause)


def visible_windows():
    """(id, name, w, h) for every MAPPED top-level, main window included.

    `--onlyvisible` is what makes this usable. Without it xdotool also returns the app's unmapped
    helper windows — a 10x10 and a 200x200 that exist for the whole session — and a caller that
    tries to reason about "extra windows" from that list is reasoning about furniture.
    """
    out = []
    for w in sh(f"DISPLAY={DISP} xdotool search --onlyvisible --name '.'").split():
        g = dict(l.split("=", 1) for l in
                 sh(f"DISPLAY={DISP} xdotool getwindowgeometry --shell {w}").strip().splitlines()
                 if "=" in l)
        if "WIDTH" not in g:
            continue
        if not pid_alive(window_pid(w)):   # see win_id(): a ghost cannot be closed, only ignored
            continue
        n = sh(f"DISPLAY={DISP} xdotool getwindowname {w}").strip()
        out.append((w, n, int(g["WIDTH"]), int(g["HEIGHT"])))
    return out


def dismiss_modals(timeout=30):
    """Close every modal over the main window, and PROVE none is left.

    This is the rung that decides whether any of the others mean anything. A fresh datadir opens
    "Bambu Network Plug-in Required" — 440x259, centred at 742,450 — which sits exactly on top of
    the point every drawing gesture in TOOLS starts from. The whole ladder then reports eleven
    product failures, all of them the driver clicking a dialog.

    The old version pressed Escape and moved on. This dialog ignores Escape, so it "dismissed"
    nothing and said so to no one; the run that found this was red for a reason that had nothing
    to do with the contract under test. Escape is still tried first because it is the gentlest
    thing that works on the wizard, then WM_DELETE_WINDOW, and then the function asserts what it
    was supposed to have achieved instead of assuming it.
    """
    # NEVER run without knowing which window to spare. The first version took `keep = main[0] if
    # main else None`, so a win_id() that raced the app's mapping made keep None and every visible
    # window a modal — this function then sent WM_DELETE to the app's own main window. The app
    # survived as a process, printed "GdkWindow unexpectedly destroyed", and answered nothing
    # afterwards; the ladder reported "no sketch opened" for 180s. Losing the main window is not a
    # state to recover from silently.
    deadline = time.time() + timeout
    keep = None
    while keep is None and time.time() < deadline:
        main = win_id()
        keep = main[0] if main else None
        if keep is None:
            time.sleep(0.5)
    if keep is None:
        die(f"no main window to protect after {timeout}s — refusing to close anything")
    scr = sh(f"DISPLAY={DISP} xdotool getdisplaygeometry").split()
    full = int(scr[0]) * int(scr[1]) if len(scr) == 2 else 1920 * 1080
    while time.time() < deadline:
        # A modal is small. Anything covering half the screen is the app, whatever id win_id()
        # happened to return this instant — a second belt on the rule above, because the cost of
        # being wrong here is an app that looks alive and answers nothing.
        extra = [x for x in visible_windows() if x[0] != keep and x[2] * x[3] < full * 0.5]
        if not extra:
            return
        for (w, n, _, _) in extra:
            sh(f"DISPLAY={DISP} xdotool windowactivate {w}")
            time.sleep(0.4)
            key("Escape", 0.4)
            if any(x[0] == w for x in visible_windows()):
                sh(f"DISPLAY={DISP} xdotool windowclose {w}")
                time.sleep(0.6)
        time.sleep(0.5)
    left = [f"{n!r} ({w}x{h})" for (i, n, w, h) in visible_windows()
            if i != keep and w * h < full * 0.5]
    die("a modal is still covering the canvas after " + str(timeout) + "s: " + ", ".join(left) +
        " — every drawing gesture would land in it, so nothing below this line could be trusted")


def dismiss_first_run():
    dismiss_modals()


# ---------------------------------------------------------------- the trace

def trace_lines():
    try:
        with open(TRACE, "r", errors="replace") as f:
            return [l.strip() for l in f if l.startswith("[UX] ")]
    except OSError:
        return []


def trace_mark():
    return len(trace_lines())


def parse(line):
    m = re.match(r"\[UX\] (\w+) title=(.*?) (.*)$", line)
    if not m:
        return None
    ev, title, rest = m.group(1), m.group(2), m.group(3)
    kv = dict(re.findall(r"(\w+)=(\S*)", rest))
    return ev, title, kv


# ---------------------------------------------------------------- grading

def check(cond, what):
    """Returns the verdict so a caller can abandon a rung whose precondition failed."""
    global _fail, _checks
    _checks += 1
    if cond:
        print(f"    ok    {what}")
    else:
        print(f"    FAIL  {what}", file=sys.stderr)
        _fail += 1
    return bool(cond)


def type_into_open_field(value, mark):
    """Type `value` into whatever field is open, WITHOUT clicking it first, and grade the pair.

    No click: the click is the workaround this ladder exists to refuse. If the field cannot take
    the keyboard on its own, `typed` will be the prefill and this fails — which is the report.
    """
    # POLL for the field. It opens from a CallAfter that runs after a re-solve, so on llvmpipe it
    # is simply not there yet when a fast driver looks — and "no field opened" is the same message
    # whether the product never opened one or the driver asked too early. Wait, then decide.
    opens = []
    deadline = time.time() + 8.0
    while time.time() < deadline:
        opens = [e for e in (parse(l) for l in trace_lines()[mark:]) if e and e[0] == "open"]
        if opens:
            break
        time.sleep(0.25)
    if not opens:
        check(False, f"a value field opened (nothing did; cannot type {value})")
        return mark
    title = opens[-1][1]
    prefill = opens[-1][2].get("prefill", "")
    m2 = trace_mark()
    # TYPE NORMALLY. NOTHING TO DEFOCUS ANY MORE.
    #
    # The value field is drawn INSIDE the GL canvas by ImGui, so it is not a window: there is no
    # second toplevel for a window manager to grant or refuse the keyboard, and the keystrokes go
    # to the app's one window exactly as a person's would. That is the entire point of the design
    # — the WM has no say — and it is why this ladder no longer tries to manufacture the failing
    # condition.
    #
    # When the field WAS a floating wxFrame, this spot held two attempts to reproduce
    # "field open, keyboard elsewhere", and both are recorded here so neither is tried again:
    #   - XSetInputFocus onto the main window (`xdotool windowfocus`): the field's own re-focus
    #     CallAfter wins the race every time; four retries all lost, and the ladder passed twice
    #     against a binary with the fix compiled out.
    #   - XSendEvent at the main window (`xdotool type --window`): GTK discards synthetic key
    #     events, so NEITHER build received anything and every run was red regardless of the code.
    # A run that used the second of those is what produced "the app never saw a digit" — a
    # property of xdotool, not of the product.
    #
    # For the in-canvas field the honest gate is simply: type, and see whether the value the app
    # commits is the value that was typed.
    diag = sh(f"DISPLAY={DISP} xdotool getwindowfocus").strip()
    typ(str(value), 0.4)
    key("Return", 0.9)
    after, commits, refused, commit_at = [], [], [], None
    deadline = time.time() + 5.0
    while time.time() < deadline:
        after = [parse(l) for l in trace_lines()[m2:]]
        commits = [(i, e) for i, e in enumerate(after) if e and e[0] == "commit"]
        refused = [e for e in after if e and e[0] == "refused"]
        if commits:
            commit_at = m2 + commits[-1][0]
            commits = [e for _, e in commits]
        if commits or refused:
            break
        time.sleep(0.25)
    if refused and not commits:
        check(False, f"{title}: field REFUSED {value!r} (typed={refused[-1][2].get('typed')!r})")
        key("Escape", 0.5)
        return trace_mark()
    if not commits:
        check(False, f"{title}: typed {value} but nothing committed — the field took no keys")
        key("Escape", 0.5)
        return trace_mark()
    typed = commits[-1][2].get("typed", "")
    got = commits[-1][2].get("value", "")
    check(typed == str(value),
          f"{title}: field received what was typed (typed={typed!r} wanted={value!r}"
          f"{'  <-- it committed its PREFILL, so it never got the keyboard' if typed == prefill else ''})")
    # A value that will not parse is a FAILED CHECK, never an exception. An unguarded float()
    # here met a locale-formatted "61,0000" and took the whole run down immediately after the
    # first check in the ladder's history had passed — the seven rungs below it were never tried
    # and the report read as a total failure.
    try:
        ok_val = abs(float(got) - float(value)) < 1e-6
    except (TypeError, ValueError):
        ok_val = False
    check(ok_val, f"{title}: committed value is {got!r} (wanted {value})")
    check(str(value) != prefill, f"{title}: the test value differs from the prefill {prefill!r}")
    # RESUME JUST AFTER THE COMMIT, not at the end of the trace. A queued chain opens its next
    # field from the commit callback, so by the time trace_mark() is read here that "open" line
    # is already written — and the next call, searching only after this mark, never sees it. The
    # rectangle's Height, the slot's Radius and the label reopen all failed as "nothing did"
    # while the trace plainly showed the field open and waiting.
    return (commit_at + 1) if commit_at is not None else trace_mark()


# ---------------------------------------------------------------- the ladder

def enter_sketch(timeout=180):
    """Open a real sketch on a real plane, and PROVE it with the socket before drawing anything.

    THE SEQUENCE MATTERS AND IT IS NOT OBVIOUS. Shift+S enters sketch MODE and pops the plane
    offer; the offer must be dismissed; and the plane itself is chosen by clicking it in the
    viewport BEFORE Shift+S. check-gui-sketching.py has always done all four steps. This ladder
    did two of them — Design tab, then Shift+S — and went straight to the tool letters.

    That intermediate state is the trap. `is_sketching` reads 1, every tool key is accepted and
    traced, and not one click draws anything, because there is no plane under them. The ladder
    then reports eleven product failures, all of them "a value field opened (nothing did)", and
    every one is the driver's. Two whole runs were spent on it.

    So the gate is the ORACLE, not the keytrace: sketch_describe answers only when a sketch is
    genuinely open. Waiting on a mode flag is what allowed the wrong state to pass for the right
    one in the first place.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        click(132, 53)                   # Design tab
        time.sleep(2.0)
        dismiss_modals()
        click(*PLANE_PX)                 # pick the plane IN THE VIEWPORT — before Shift+S
        key("shift+s", 1.0)
        key("Escape", 0.5)               # entering sketch mode pops the offer; dismiss it
        key("p", 0.6)                    # any sketch tool starts the session on that plane
        if try_call("sketch_describe") is not None:
            # NO Escape here. Every rung already opens with one to drop whatever tool the last
            # one left armed, and Escape in the Design tab walks a LIFO: first press drops the
            # armed tool, second LEAVES THE SKETCH. Pressing it here made that second press the
            # rung's own, so the ladder exited the sketch before drawing anything and then
            # reported all eleven checks failed with "nothing opened" — the tools were arming
            # into an empty Feature-mode document.
            return
    die("no sketch opened after plane click + Shift+S within "
        f"{timeout}s — sketch_describe never answered on {A.sock} (trace {TRACE})")


# tool key, the clicks that draw it, and one distinct value per queued field. The values are
# deliberately nothing like the as-drawn size, so a committed prefill cannot coincide with them.
# Where the plane label sits in the viewport before a sketch is open. Same constant the gesture
# ladder uses; it is a label on the 3D view, not a widget, so it moves only if the camera does.
PLANE_PX = (913, 359)

# Every coordinate below stays inside 1000..1400 x 500..760 — the box check-gui-sketching.py's
# calibration probes land four Points in, i.e. the region PROVEN to be live canvas on a 1920x1080
# window. Earlier values started at x=950, which is left of that box and also, on a fresh datadir,
# underneath the "Bambu Network Plug-in Required" modal.
TOOLS = [
    ("L", "Line",      [(1030, 540), (1360, 540)],                         [61]),
    ("R", "Rectangle", [(1030, 540), (1360, 730)],                         [62, 43]),
    ("C", "Circle",    [(1180, 620), (1330, 620)],                         [64]),
    ("S", "Slot",      [(1030, 580), (1300, 580), (1300, 640)],            [66]),
    ("G", "Polygon",   [(1180, 620), (1320, 620)],                         [67]),
    ("E", "Ellipse",   [(1180, 620), (1370, 620), (1180, 720)],            [68]),
    ("A", "Arc",       [(1040, 660), (1340, 660), (1190, 560)],            [69]),
]


def rung_tool(k, name, clicks, values):
    print(f"  {name}")
    key("Escape", 0.6)                   # back to Select, whatever the last tool left armed
    # Every rung re-establishes that a sketch is STILL open. One stray Escape too many leaves it,
    # and from then on every tool arms into a Feature-mode document that cannot open a value
    # field — which the checks below report as eleven independent product failures.
    if try_call("sketch_describe") is None:
        die(f"{name}: the sketch is no longer open before this rung — an earlier rung left it")
    key(k, 0.8)
    # MARK BEFORE THE CLICKS, not after. The field is opened from a CallAfter scheduled by the
    # render that follows the last click, so it can already be open by the time a mark taken
    # afterwards is read — and type_into_open_field, which only looks at events AFTER its mark,
    # then finds none and reports "a value field opened (nothing did)" for a field that is on
    # screen, open, and waiting. That message accused the product of the exact defect the ladder
    # exists to detect, from a bug in the ladder's own bookkeeping.
    mark = trace_mark()
    for (x, y) in clicks:
        click(x, y)
    for v in values:
        mark = type_into_open_field(v, mark)


def rung_rounded_rect():
    """The shape the user actually reported: a ROUNDED rectangle, Width -> Height -> Radius.

    It has no keyboard shortcut — the rectangle family binds R to CornerRect and leaves the other
    modes in the toolbar flyout — so TOOLS above cannot reach it and the whole three-step chain
    went untested. `run_verb` arms it the way the offer menu does.

    NOTE the id: the OFFER verb is `sk_rect_rounded`; `design_rect_rounded` is the ACTION name and
    run_verb throws on it, leaving the tool as Select. A run that misses that draws nothing and
    still reaches its assertions, so arm-and-verify rather than arm-and-hope.
    """
    print("  Rounded rectangle")
    key("Escape", 0.6)
    tool = None
    for _ in range(8):
        try_call("run_verb", verb="sk_rect_rounded")
        time.sleep(0.8)
        tool = (try_call("sketch_describe") or {}).get("tool")
        if tool == "rect_rounded":
            break
    if not check(tool == "rect_rounded", f"the rounded-rectangle tool armed (tool={tool!r})"):
        return
    mark = trace_mark()
    click(1030, 540); click(1330, 700); click(1300, 660)   # corners, then the radius point
    for v in (63, 41, 7):
        mark = type_into_open_field(v, mark)


def rung_label_click():
    """The user's own report: click an existing dimension label and type a new value into it.

    KEEP THE SHAPE AS DRAWN. An earlier version committed 55 and 47 into the queued chain first,
    which resized the rectangle — and then clicked the pixel where the label had been before the
    resize. It missed, every time, and reported the reopen broken. The shape's on-screen position
    is only predictable if nothing has moved it, so Escape the chain instead: the rectangle stays
    exactly between the two corners we clicked.

    FIND THE LABEL, do not assume its offset. A dimension label is drawn beside its edge at an
    offset that depends on zoom and text metrics, so a single hardcoded pixel is a guess that
    silently becomes wrong. Walk a short band across the top edge instead and stop at the first
    click that opens a field; if none of them does, that is a real failure and it says so.
    """
    print("  label click-to-edit")
    key("Escape", 0.6)                   # Select mode
    key("R", 0.8)
    click(1020, 530)
    click(1350, 740)
    time.sleep(1.5)
    key("Escape", 0.8)                   # keep as drawn: abandon the queued value chain
    time.sleep(0.8)
    key("Escape", 0.6)                   # back to Select so a click picks rather than draws

    mid_x, top_y = (1020 + 1350) // 2, 530
    candidates = [(mid_x, top_y + dy) for dy in (-26, -20, -14, -8, 0, 8, 14)]
    for (cx, cy) in candidates:
        mark = trace_mark()
        click(cx, cy)
        deadline = time.time() + 2.0
        while time.time() < deadline:
            if [e for e in (parse(l) for l in trace_lines()[mark:]) if e and e[0] == "open"]:
                check(True, f"clicking a dimension label reopened its value field (at {cx},{cy})")
                type_into_open_field(71, mark)
                return
            time.sleep(0.2)
    check(False, "clicking a dimension label reopened its value field "
                 f"(tried {len(candidates)} points across the top edge at x={mid_x})")


def main():
    if not A.attach:
        if not os.path.exists(A.bin):
            die(f"no binary at {A.bin}")
        open(TRACE, "w").close()
        launch()
        dismiss_first_run()
    win()
    print(f"click-edit ladder on {DISP}, trace {TRACE}")
    enter_sketch()
    for (k, name, clicks, values) in TOOLS:
        rung_tool(k, name, clicks, values)
    rung_rounded_rect()
    rung_label_click()
    print()
    if _fail:
        print(f"CLICK-EDIT LADDER FAILED — {_fail} of {_checks} checks", file=sys.stderr)
    else:
        print(f"CLICK-EDIT LADDER HELD — {_checks} checks")
    if _proc is not None and not A.keep:
        _proc.send_signal(signal.SIGTERM)
        try:
            _proc.wait(20)
        except subprocess.TimeoutExpired:
            _proc.kill()
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
