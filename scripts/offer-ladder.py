#!/usr/bin/env python3
"""The OFFER ladder: prove that right-click is the pivot, and that it adapts to what was clicked.

The gesture ladder (scripts/gui-ladder.py) proved the TARGET — a complex closed profile, precise
in vertices, lengths, arcs and symmetry, with its voids correctly attributed. It proved it by
arming every tool with a letter key. That leaves the goal's own MECHANISM untested: the design
logic pivots on right-click, and the verbs offered are supposed to adapt to the element under the
cursor. Half the vocabulary is only reachable that way — 47 of 86 Design-tab verbs have a GUI
action and no shortcut, so a key-driven ladder cannot reach them at all.

This ladder drives the menu. Nothing here is asserted from pixels:

  WHAT WAS CLICKED  -> the offer's own [OFFER] trace, emitted by show_offer_menu from the same
                       loop that builds the rows (SNAPORCA_KEYTRACE). It cannot drift from what
                       the user is shown, which a hand-written expectation list would.
  WHAT IS OFFERED   -> the same trace, compared against DesignOffer.hpp parsed independently.
                       "The menu shows exactly the verbs the table says apply here" is a
                       property; a copied list of row names is a transcription.
  WHAT IT PRODUCED  -> the MCP socket, read-only, exactly as in the gesture ladder.

Run inside the rig container, with the app launched under SNAPORCA_KEYTRACE=1:

    docker exec snaporca-gui python3 /OrcaSlicer/scripts/offer-ladder.py [rung ...]
"""
import importlib.util
import math
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# The gesture ladder owns the hand and the eye: the homography per sketch, the window lookup by
# class, the synthetic click, the typed value, the socket. Importing it is the only way those
# stay one implementation — a second copy would drift the first time a rig detail moved.
_spec = importlib.util.spec_from_file_location("gui_ladder", os.path.join(HERE, "gui-ladder.py"))
G = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(G)

LOG = os.environ.get("SNAPORCA_GUI_LOG", "/tmp/gui-session.log")

# The rig container's /OrcaSlicer is the image's own baked source tree, not this checkout, so the
# generated header is not where a repo-relative path expects it. Look in both places and say which
# one was read — a ladder that silently graded against the WRONG table would be worse than one
# that refuses to start.
HEADER_CANDIDATES = [os.environ.get("SNAPORCA_OFFER_HPP", ""),
                     os.path.join(HERE, "..", "src", "slic3r", "GUI", "CAD", "DesignOffer.hpp"),
                     os.path.join(HERE, "DesignOffer.hpp")]

# OfferSel, in the order the generated enum declares it. The trace reports the integer; a test
# that printed "kind=16" and expected the reader to know what that is would be half a test.
SEL = ["None", "FacePlanar", "FaceCyl", "FaceOther", "EdgeStr", "EdgeCirc", "Vertex",
       "BodySolid", "BodySheet", "Bodies2", "DatumPlane", "DatumAxis", "CoordSys", "Art",
       "SkLoop", "SkNone", "SkLine", "SkArc", "SkPoint", "Sk2Ent"]


# ---------------------------------------------------------------- the table, parsed

def load_table():
    """Every verb in DesignOffer.hpp, as dicts. The independent half of the comparison.

    Parsed from the generated header rather than from tool_atlas.json on purpose: the header is
    what the binary was compiled from, and the two have been out of step before (snaporca-ziam,
    where regenerating the header silently dropped the Constrain row).
    """
    path = next((p for p in HEADER_CANDIDATES if p and os.path.exists(p)), None)
    if path is None:
        raise SystemExit("no DesignOffer.hpp found; set SNAPORCA_OFFER_HPP or copy it beside "
                         "this script (tried: " + ", ".join(filter(None, HEADER_CANDIDATES)) + ")")
    print(f"offer table: {os.path.realpath(path)}")
    src = open(path).read()
    body = src[src.index("kOfferVerbs[]"):]
    body = body[:body.index("\n};")]
    out = []
    for line in body.splitlines():
        line = line.strip()
        if not line.startswith('{"'):
            continue
        # id, name, row, key, action, refusal, accepts, need_bodies, need_sketches, need_sheet,
        # sketch_mode, family, ... — split on top-level commas, respecting quotes.
        f, cur, q, esc = [], "", False, False
        for ch in line[1:]:
            if esc:
                cur += ch; esc = False; continue
            if ch == "\\":
                cur += ch; esc = True; continue
            if ch == '"':
                q = not q
            if ch == "," and not q:
                f.append(cur.strip()); cur = ""; continue
            if ch == "}" and not q:
                break
            cur += ch
        f.append(cur.strip())
        if len(f) < 12:
            continue
        lit = lambda s: None if s == "nullptr" else s.strip('"')
        out.append({"id": lit(f[0]), "name": lit(f[1]), "row": int(f[2]), "key": lit(f[3]),
                    "action": lit(f[4]), "accepts": int(f[6].rstrip("u"), 0),
                    "need_bodies": int(f[7]), "need_sketches": int(f[8]),
                    "need_sheet": f[9] == "true", "sketch_mode": f[10] == "true",
                    "family": lit(f[11])})
    return out


TABLE = load_table()


def predicted(kind, sketching, bodies=0, sketches=0, sheet=False):
    """The verbs show_offer_menu should list for this selection — the table's own answer.

    Mirrors the `applies` lambda and the sketch_mode gate in DesignPanel::show_offer_menu. If the
    two ever disagree, one of them is the bug; this ladder says which selection exposed it.
    """
    bit = 1 << kind
    return [v["id"] for v in TABLE
            if v["sketch_mode"] == sketching and (v["accepts"] & bit)
            and v["need_bodies"] <= bodies and v["need_sketches"] <= sketches
            and (not v["need_sheet"] or sheet)]


# ---------------------------------------------------------------- the trace, read back

def log_mark():
    """Where the log ends now, so the next read sees only this gesture's lines."""
    try:
        return os.path.getsize(LOG)
    except OSError:
        return 0


def log_since(mark):
    with open(LOG, "rb") as fh:
        fh.seek(mark)
        return fh.read().decode("utf-8", "replace")


class Offer:
    """One opening of the offer menu, as the app described it while building the rows."""

    def __init__(self, text):
        self.kind = None
        self.sketching = None
        self.entries = []       # top level, in order: dicts with label/enabled/verbs
        self.verbs = []         # every live verb id, in menu order
        for m in re.finditer(r"^\[OFFER\] (.*)$", text, re.M):
            self._line(m.group(1))

    def _line(self, s):
        head = re.match(r"open kind=(\d+) sketching=(\d+) bodies=(\d+)", s)
        if head:
            self.kind = int(head.group(1))
            self.sketching = head.group(2) == "1"
            self.entries = []
            self.verbs = []
            return
        dis = re.match(r"row=(\d+) (.*?) DISABLED \((.*)\)$", s)
        if dis:
            self.entries.append({"row": int(dis.group(1)), "label": dis.group(2),
                                 "enabled": False, "kids": [], "why": dis.group(3)})
            return
        one = re.match(r"row=(\d+) (.*?) -> (\S+)(?: \(no GUI route\))?$", s)
        if one:
            routed = "(no GUI route)" not in s
            self.entries.append({"row": int(one.group(1)), "label": one.group(2),
                                 "enabled": routed, "kids": [], "verb": one.group(3)})
            self.verbs.append(one.group(3))
            return
        sub = re.match(r"row=(\d+) (.*?) > (.*)$", s)
        if sub:
            row, label, rest = int(sub.group(1)), sub.group(2), sub.group(3)
            routed = "(no GUI route)" not in rest
            rest = rest.replace(" (no GUI route)", "")
            fam, vid = (rest.split(" > ", 1) + [None])[:2] if " > " in rest else (None, rest)
            if not self.entries or self.entries[-1]["row"] != row or "verb" in self.entries[-1]:
                self.entries.append({"row": row, "label": label, "enabled": True, "kids": []})
            self.entries[-1]["kids"].append({"family": fam, "verb": vid, "enabled": routed})
            self.verbs.append(vid)
            return

    def kind_name(self):
        return SEL[self.kind] if self.kind is not None and self.kind < len(SEL) else str(self.kind)

    def path_to(self, verb):
        """Keyboard path to a verb: how many Downs at each level, top level first.

        GTK skips insensitive items on arrow navigation, so the count is over ENABLED entries
        only — which is exactly why the trace records the enabled state per row instead of the
        ladder assuming every row is live.
        """
        n = 0
        for e in self.entries:
            if not e["enabled"]:
                continue
            n += 1
            if e.get("verb") == verb:
                return [n]
            if e["kids"]:
                # Families become a nested submenu at the position of their first member.
                pos, seen = 0, []
                for k in e["kids"]:
                    if k["family"]:
                        if k["family"] not in seen:
                            seen.append(k["family"])
                            pos += 1
                        fam_pos = pos
                        if k["verb"] == verb:
                            inner = [x for x in e["kids"] if x["family"] == k["family"]]
                            j = sum(1 for x in inner[:inner.index(k) + 1] if x["enabled"])
                            return [n, fam_pos, j]
                        continue
                    if not k["enabled"]:
                        continue
                    pos += 1
                    if k["verb"] == verb:
                        return [n, pos]
        return None


def open_offer(X=None, Y=None, pause=1.2):
    """Right-click (on the plane point given, or wherever the cursor is) and read the offer back.

    The menu is modal — PopupMenu blocks the main thread — so no socket call may be made between
    this and choose()/dismiss(). Every assertion about the menu comes from the trace.
    """
    mark = log_mark()
    if X is None:
        G.xdo("click --delay 120 3")
    else:
        G.clickmm(X, Y, pause=0.5, btn=3)
    time.sleep(pause)
    return Offer(log_since(mark))


def choose(offer, verb):
    """Walk the open menu to a verb with the keyboard and activate it."""
    path = offer.path_to(verb)
    if path is None:
        G.die(f"{verb} is not in the offer (kind={offer.kind_name()}, has {offer.verbs})")
    for level, downs in enumerate(path):
        # At the top level nothing is highlighted when the menu pops, so the first Down lands on
        # entry 1. Inside a submenu GTK has ALREADY highlighted its first item as part of opening
        # it, so reaching entry k takes k-1 more. Getting this wrong is silent: the walk activates
        # a neighbouring verb and the rung grades a shape nobody asked for — the first run of this
        # rung drew a circle of area 45238.93 and called it a rectangle.
        for _ in range(downs if level == 0 else downs - 1):
            G.key("Down", 0.12)
        if level < len(path) - 1:
            G.key("Right", 0.35)          # open the submenu; its first item is now highlighted
    G.key("Return", 0.9)


def dismiss(offer=None):
    """Escape ONLY when a menu is really open.

    A stray Escape on the canvas is not harmless: with no tool armed and no points down, the
    sketch's layered exit reads it as "leave the sketch", and the next socket call answers
    "no sketch is open" three rungs from where the mistake was made.
    """
    if offer is not None and offer.kind is None:
        return
    G.key("Escape", 0.5)


def menu_windows():
    """X windows that are override-redirect popups — evidence the menu really opened."""
    out = []
    for w in G.sh(f"DISPLAY={G.DISP} xdotool search --class '.'").split():
        g = G.sh(f"DISPLAY={G.DISP} xdotool getwindowgeometry --shell {w}")
        d = dict(l.split("=", 1) for l in g.strip().splitlines() if "=" in l)
        if "WIDTH" in d and 60 < int(d["WIDTH"]) < 700 and 40 < int(d["HEIGHT"]) < 900:
            out.append(w)
    return out


# ---------------------------------------------------------------- rungs

def draw_line_at(x0, y0, x1, y1, length):
    """Draw one horizontal line and COMMIT it by typing its length and angle.

    Deliberately no Escape. Escape is overloaded in a sketch — field, then tool, then the sketch
    itself — so a driver that presses it one time too many leaves the session and every later
    assertion answers "no sketch is open" from three rungs away. Typing the value closes the
    field, which is what the gesture ladder proved commits exactly.
    """
    G.key("l", 0.5)
    G.clickmm(x0, y0)
    G.clickmm(x1, y1)
    G.values(int(length), 0)
    keep_as_drawn()          # drain any straggler field before the caller clicks anything


def rung_kinds():
    """O1 — the offer adapts to the element under the cursor, one element type at a time."""
    print("\nO1  the offer reads what was right-clicked")
    fresh_sketch("l")
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0

    # Empty space first: nothing is selected, so the sketch vocabulary's no-selection row set.
    # Right-click has two jobs on a draw tool, and which one it does depends on whether an
    # anchor is down. Both are asserted here: the version that consumed EVERY right-click made
    # the offer unreachable from any armed tool (snaporca-ghcz), which is the goal's own
    # mechanism failing silently.
    hi = y1 - (y1 - y0) * 0.12
    o = open_offer(cx, hi)
    G.check("OFFER", o.kind is not None,
            "Line armed, nothing anchored: right-click opens the offer")
    G.check("OFFER", o.kind == 15, f"right-click on empty space -> {o.kind_name()}")
    G.check("OFFER", o.sketching, "the sketch vocabulary is the one being offered")
    dismiss(o)

    # ...and with an anchor down it abandons the anchor instead, offering nothing.
    G.key("l", 0.5)
    G.clickmm(cx, hi)                          # anchor the first point
    o = open_offer(cx + (x1 - x0) * 0.1, hi)
    G.check("OFFER", o.kind is None,
            "with an anchor down, the same gesture abandons it and does not offer")
    o = open_offer(cx, hi)
    G.check("OFFER", o.kind is not None, "and the offer is back on the next right-click")
    dismiss(o)
    G.key("Escape", 0.4)

    # A line.
    ax, ay = x0 + (x1 - x0) * 0.15, cy
    bx, by = x0 + (x1 - x0) * 0.55, cy
    draw_line_at(ax, ay, bx, by, 40)
    o = open_offer((ax + bx) / 2.0, ay)
    G.check("OFFER", o.kind == 16, f"right-click on a line -> {o.kind_name()}")
    dismiss(o)

    # A circle: every curve takes the same vocabulary, which is what SkArc means.
    G.key("c", 0.5)
    ccx, ccy = x0 + (x1 - x0) * 0.30, cy + (y1 - y0) * 0.25
    G.clickmm(ccx, ccy)
    G.clickmm(ccx + (x1 - x0) * 0.10, ccy)
    G.values(20)
    ents = G.describe()["entities"]
    circ = [e for e in ents if e["type"] == "circle"]
    G.check("OFFER", len(circ) == 1, "one circle drawn to right-click on")
    r = circ[0]["radius"]
    o = open_offer(circ[0]["center"][0] + r, circ[0]["center"][1])
    G.check("OFFER", o.kind == 17, f"right-click on a circle -> {o.kind_name()}")
    dismiss(o)

    # A point.
    G.key("p", 0.5)
    pxx, pyy = x0 + (x1 - x0) * 0.80, cy + (y1 - y0) * 0.25
    G.clickmm(pxx, pyy)
    o = open_offer(pxx, pyy)
    G.check("OFFER", o.kind == 18, f"right-click on a point -> {o.kind_name()}")
    dismiss(o)

    # Two entities: a second line, then both picked. Two LINES rather than line-plus-point on
    # purpose — the Sk2Ent vocabulary (angle, equal, parallel, the two-entity constraints) is
    # about pairs of curves, so the pair the ladder builds should be the pair the verbs mean.
    draw_line_at(ax, ay - (y1 - y0) * 0.18, bx, ay - (y1 - y0) * 0.18, 40)
    # ONE Escape, to drop the armed tool to Select, and only once the value fields are quiet.
    # Left-click means "draw" while a tool is armed, so a picking gesture has to say so first —
    # and from a draw tool with no anchor down Escape does exactly that and nothing more; it is
    # only a second Escape, from Select, that would leave the sketch.
    keep_as_drawn()
    G.key("Escape", 0.5)
    G.clickmm(*on_line((ax, ay), (bx, ay)))
    G.xdo("keydown shift")
    G.clickmm(*on_line((ax, ay - (y1 - y0) * 0.18), (bx, ay - (y1 - y0) * 0.18)))
    G.xdo("keyup shift")
    d = G.describe()
    G.check("OFFER", len(d["selection"]) == 2,
            f"two entities picked: {d['selection']} (tool={d['tool']} pending={d['pending']} "
            f"editing={d['editing']})")
    o = open_offer((ax + bx) / 2.0, ay)
    G.check("OFFER", o.kind == 19,
            f"right-click with two picked -> {o.kind_name()}"
            + ("" if o.kind is not None else
               f" (no menu: tool={G.describe()['tool']} pending={G.describe()['pending']} "
               f"editing={G.describe()['editing']})"))
    dismiss(o)
    G.leave_sketch()
    G.reset_document()


def rung_vocabulary():
    """O2 — the rows offered are exactly the ones the table says apply to that selection.

    Five selections, not one. The interesting failure is not "the menu is empty", it is "the
    menu is the same whatever you clicked" — and only comparing several kinds against their own
    predictions can tell those apart.
    """
    print("\nO2  the menu and the offer table agree, selection by selection")
    fresh_sketch("l")
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    ax, ay = x0 + (x1 - x0) * 0.15, cy
    bx = x0 + (x1 - x0) * 0.55
    draw_line_at(ax, ay, bx, ay, 40)

    G.key("c", 0.5)
    ccx, ccy = x0 + (x1 - x0) * 0.30, cy + (y1 - y0) * 0.25
    G.clickmm(ccx, ccy); G.clickmm(ccx + (x1 - x0) * 0.10, ccy)
    G.values(20)
    circ = [e for e in G.describe()["entities"] if e["type"] == "circle"][0]

    G.key("p", 0.5)
    pxx, pyy = x0 + (x1 - x0) * 0.80, cy + (y1 - y0) * 0.25
    G.clickmm(pxx, pyy)

    where = [("SkNone", cx, y1 - (y1 - y0) * 0.12),
             ("SkLine", (ax + bx) / 2.0, ay),
             ("SkArc", circ["center"][0] + circ["radius"], circ["center"][1]),
             ("SkPoint", pxx, pyy),
             ("Sk2Ent", None, None)]
    seen = {}
    for name, X, Y in where:
        if name == "Sk2Ent":
            # The two-entity vocabulary was the one selection nothing compared against the table,
            # and it is where the missing row hid: sk_angdist accepts Sk2Ent and nothing else, so
            # an off-by-one that dropped the LAST verb was invisible from every other selection.
            keep_as_drawn()
            G.key("Escape", 0.5)
            G.clickmm(*on_line((ax, ay), (bx, ay)))
            G.xdo("keydown shift")
            # The TOP of the circle, not its +X point: the radius grip lives there, and a click on
            # a grip arms a handle drag which REPLACES the selection with that one entity. The
            # pick then silently collapses to one and the offer answers SkLine — right, for the
            # selection that actually existed.
            G.clickmm(circ["center"][0], circ["center"][1] + circ["radius"])
            G.xdo("keyup shift")
            picked = len(G.describe()["selection"])
            G.check("OFFER", picked == 2, f"two entities picked for the pair vocabulary: {picked}")
            X, Y = (ax + bx) / 2.0, ay
        o = open_offer(X, Y)
        want = sorted(predicted(o.kind, o.sketching))
        got = sorted(o.verbs)
        G.check("OFFER", o.kind is not None and o.kind_name() == name and got == want,
                f"{o.kind_name()}: {len(got)} verbs, exactly the table's set"
                + ("" if got == want else f"\n      menu  {got}\n      table {want}"))
        seen[name] = set(got)
        dismiss(o)

    # And the sets are genuinely DIFFERENT — an offer that adapts is not one that always shows
    # the same rows. Without this, four identical menus would have passed four checks.
    G.check("OFFER", len(set(map(frozenset, seen.values()))) == len(seen),
            "every selection offers a different set: "
            + ", ".join(f"{k}={len(v)}" for k, v in seen.items()))
    G.check("OFFER", seen["SkLine"] - seen["SkNone"],
            f"a picked line adds {len(seen['SkLine'] - seen['SkNone'])} verbs an empty pick has not: "
            + ", ".join(sorted(seen["SkLine"] - seen["SkNone"])[:8]))
    G.leave_sketch()
    G.reset_document()


def rung_author():
    """O3 — the target itself, authored through the menu: no tool key is pressed anywhere here."""
    print("\nO3  a precise closed profile, drawn entirely from the right-click offer")
    fresh_sketch("p")            # 'p' only to open the session; calibration needs the Point tool
    G.key("Escape", 0.4)
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    w, h = 120.0, 80.0

    o = open_offer(cx, cy)
    G.check("OFFER", o.kind == 15, f"empty sketch -> {o.kind_name()}")
    choose(o, "sk_rect")
    G.clickmm(cx - w / 2.0, cy - h / 2.0)
    G.clickmm(cx + w / 2.0, cy + h / 2.0)
    G.values(int(w), int(h))
    ents = G.describe()["entities"]
    G.check("LENGTH", G.lengths(ents) == [80.0, 80.0, 120.0, 120.0],
            f"sides {G.lengths(ents)} — typed through the offer, exact")
    lp = G.loops()
    G.check("CLOSED", len(lp) == 1, f"{len(lp)} closed loop(s)")
    G.check("AREA", G.near(abs(lp[0]["area"]), w * h, 1e-9), f"area {abs(lp[0]['area']):.6f}")
    G.leave_sketch()
    G.reset_document()


def rung_no_shortcut():
    """O4 — a tool with NO keyboard route at all, armed from the menu and graded on its geometry.

    This is the half of the vocabulary a key-driven ladder cannot reach: 47 of the 86 Design-tab
    verbs have a GUI action and no shortcut, and for those the offer is not one door, it is the
    only door. Arming the tool is not the assertion — the exact rectangle it then draws is.
    """
    print("\nO4  a tool that has no shortcut, reached the only way it can be")
    fresh_sketch("p")
    G.key("Escape", 0.5)
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0

    o = open_offer(cx, cy)
    keyless = [v for v in TABLE if v["id"] in o.verbs and not v["key"] and v["action"]]
    G.check("OFFER", len(keyless) >= 10,
            f"{len(keyless)} of the {len(o.verbs)} verbs offered here have no shortcut at all")
    G.check("OFFER", any(v["id"] == "sk_rect_center" for v in keyless),
            "centre rectangle among them — unreachable from the keyboard")

    w, h = 120.0, 80.0
    choose(o, "sk_rect_center")
    G.clickmm(cx, cy)                        # centre
    G.clickmm(cx + w / 2.0, cy + h / 2.0)    # corner
    G.values(int(w), int(h))
    ents = G.describe()["entities"]
    G.check("LENGTH", G.lengths(ents) == [80.0, 80.0, 120.0, 120.0],
            f"sides {G.lengths(ents)} from a tool with no key")
    lp = G.loops()
    G.check("CLOSED", len(lp) == 1 and G.near(abs(lp[0]["area"]), w * h, 1e-9),
            f"{len(lp)} closed loop, area {abs(lp[0]['area']):.6f}")
    xs = sorted({round(p, 6) for e in ents for p in (e["p0"][0], e["p1"][0])})
    ys = sorted({round(p, 6) for e in ents for p in (e["p0"][1], e["p1"][1])})
    # Centred on the CLICK, to within the click itself. A synthetic click lands on a whole
    # pixel, so the plane point it names is only ever as exact as one pixel is — grading this to
    # 1e-6 would be grading the homography, not the tool. What is exact is the SHAPE, and that
    # is asserted above; what is asserted here is that this was a centre rectangle and not a
    # corner one, which a whole pixel is plenty to tell apart at 120 x 80.
    tol = 1.5 * G.mm_per_px(cx, cy)
    mx, my = (xs[0] + xs[-1]) / 2.0, (ys[0] + ys[-1]) / 2.0
    G.check("SYMMETRY", abs(mx - cx) <= tol and abs(my - cy) <= tol,
            f"centred on the click within {tol:.3f} mm (one pixel): "
            f"off by {abs(mx - cx):.4f}, {abs(my - cy):.4f} — a CENTRE rectangle, not a corner one")
    G.leave_sketch()
    G.reset_document()



# ---------------------------------------------------------------- fixtures

def keep_as_drawn():
    """Close an in-canvas value field if one is open, keeping the geometry as drawn.

    Checked, never assumed. Escape is overloaded: with a field open it means keep-as-drawn, with
    none open it drops the armed tool, and one Escape too many leaves the sketch. The socket now
    reports whether a field IS open ("editing"), so this presses the key only when it means what
    the caller wants it to mean.
    """
    n = 0
    # A LOOP, not one press: the auto-edit queue opens the next field from a CallAfter as the
    # previous one commits (a rectangle queues Width then Height), so one Escape leaves a second
    # field on screen and the canvas still frozen. The loop stops the moment nothing is open,
    # which is what keeps the last press from being the one that drops the tool.
    #
    # And it waits for QUIET, not for a single false reading. A field that has not opened YET
    # reads exactly like one that will never open, so a driver that looks once, sees nothing and
    # moves on gets frozen by the field that arrives a moment later — with no symptom except
    # that clicks stop working. Measured: this rung passed alone and failed inside the gate,
    # where the app is warmer and the CallAfter lands later; the diagnostic that found it was
    # editing=True with an empty selection after two clicks that should have picked two entities.
    for _ in range(8):
        time.sleep(0.35 * G.PACE)
        if not G.describe().get("editing"):
            time.sleep(0.35 * G.PACE)
            if not G.describe().get("editing"):
                return n
        G.key("Escape", 0.45)
        n += 1
    return n


def clear_sketch():
    """Empty the live sketch through the socket.

    Fixture TEARDOWN, not the thing under test: what is being graded is always the geometry a
    verb just produced, never how the canvas got emptied. Doing it through the socket keeps each
    verb's rung independent without paying for a fresh sketch (four calibration probes) each time.
    """
    keep_as_drawn()          # a shape left mid-edit freezes the canvas for whatever comes next
    n = len(G.describe()["entities"])
    if n:
        G.call("sketch_delete", entities=list(range(n)))


# Which tool each creation verb is supposed to arm. The menu walk counts rows, and a walk that
# lands ONE ROW OFF arms a neighbouring tool and then draws something plausible with it — the
# first run of this rung drew a circle and graded it as a rectangle. Asserting the armed tool
# turns that whole class of silent misnavigation into a loud failure at the point it happens.
ARMS = {"sk_polyline": "polyline", "sk_rect": "rect_corner", "sk_rect_center": "rect_center",
        "sk_rect_oblique": "rect_oblique", "sk_rect_rounded": "rect_rounded",
        "sk_circle_2pt": "circle_2pt", "sk_circle_3pt": "circle_3pt",
        "sk_arc_tangent": "arc_tangent", "sk_arc_center": "arc_center",
        "sk_slot_arc": "slot_arc", "sk_ellipse_arc": "ellipse_arc",
        "sk_poly_3": "polygon", "sk_poly_4": "polygon", "sk_poly_5": "polygon",
        "sk_poly_8": "polygon", "sk_poly_12": "polygon",
        "sk_move": "move", "sk_rotate": "rotate", "sk_scale": "scale",
        "sk_array": "array", "sk_array_polar": "array_polar"}


def arm(verb, X, Y, check_tool=True):
    """Open the offer on empty plane at (X, Y) and pick a verb out of it. No key is ever pressed."""
    o = open_offer(X, Y)
    if o.kind is None:
        G.die(f"the offer did not open for {verb} (tool={G.describe().get('tool')}, "
              f"pending={G.describe().get('pending')})")
    choose(o, verb)
    if check_tool and verb in ARMS:
        got = G.describe().get("tool")
        G.check("OFFER", got == ARMS[verb], f"{verb} armed the {got} tool")
    return o


def ents(kind=None):
    e = G.describe()["entities"]
    return [x for x in e if kind is None or x["type"] == kind]


def clicked(X, Y):
    """The plane point the app REALLY saw for clickmm(X, Y).

    A synthetic click lands on a whole pixel, so the plane point it names is not the one asked
    for. Rounding the pixel and mapping it back is what the app got, and grading a construction
    against it is grading the tool rather than the driver's arithmetic.
    """
    u, v = G.px(X, Y)
    return G.unpx(int(u), int(v))


def fresh_sketch(tool):
    """Enter a sketch from a KNOWN empty state, whatever the previous rung or run left behind.

    gui-ladder's enter_sketch dismisses the old session with keys, and a key is exactly what an
    open value field swallows — so a session that should have been cancelled survives, the four
    calibration probes land in it on top of whatever was already there, and the run dies with
    "calibration expected 4 points, got 7". Cancelling through the socket cannot be swallowed:
    it reaches the tool directly. This is fixture teardown, not the thing under test.
    """
    G.try_call("sketch_cancel")
    G.key("Escape", 0.3)
    G.enter_sketch(tool)


def on_line(a, b, t=0.3):
    """A point a fraction t ALONG a line — never its midpoint.

    A left-click within ~24 px of a live dimension label opens that dimension's value editor
    instead of selecting anything (the Select branch tests m_live_quotes before it picks), and a
    line's Length quote sits at its middle. Clicking there froze the canvas on an open field, so
    the following clicks and the right-click all landed on nothing and the pair vocabulary was
    never reached. It bit only when the previous step had left the line selected — live quotes are
    drawn for the SELECTION — which is why it passed alone and failed inside the gate.
    """
    return (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)


def poly_click(pt):
    """One click of a multi-segment tool, then close whatever value field that click opened.

    The polyline arms a Length field after EVERY segment, and a field freezes the canvas — so a
    driver that just clicks four times places two points and loses the rest. Nothing had ever
    exercised the polyline (it has no shortcut), so nothing had ever met this.
    """
    G.clickmm(*pt)
    keep_as_drawn()


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def spread(vals):
    return max(vals) - min(vals)


# ---------------------------------------------------------------- the keyless 2D vocabulary

def rung_curves():
    """O5 — every 2D creation verb that has no shortcut, drawn from the offer and graded exactly.

    These are reachable ONLY from the right-click menu, so nothing has ever exercised them. The
    assertions are CONSTRUCTION invariants — a regular polygon's vertices are equidistant, a
    tangent arc meets its line at a right angle to the radius, a circumscribed polygon's
    circumradius is the inscribed one's over cos(pi/n) — because those hold exactly whatever
    pixel the click landed on. Where a value field opens, the typed value is graded exactly too.
    """
    print("\nO5  the 2D creation verbs that have no keyboard route")
    fresh_sketch("p")
    G.key("Escape", 0.5)
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    W, H = (x1 - x0), (y1 - y0)
    free = (cx, y1 - H * 0.10)          # a corner of the safe box that stays empty to right-click

    # --- Polyline: an explicitly CLOSED chain, which is the goal's own shape ------------------
    clear_sketch()
    arm("sk_polyline", *free)
    ring = [(cx - W * 0.20, cy - H * 0.15), (cx + W * 0.20, cy - H * 0.15),
            (cx + W * 0.20, cy + H * 0.15), (cx - W * 0.20, cy + H * 0.15)]
    for pt in ring:
        poly_click(pt)
    poly_click(ring[0])                 # click the start again: the explicit close
    lines = ents("line")
    lp = G.loops()
    G.check("CLOSED", len(lines) == 4 and len(lp) == 1,
            f"sk_polyline: {len(lines)} lines, {len(lp)} closed loop — closed by clicking the start")

    # --- Oblique rectangle: three clicks, and the point of it is that it is NOT axis-aligned --
    clear_sketch()
    arm("sk_rect_oblique", *free)
    a = (cx - W * 0.20, cy - H * 0.10)
    b = (cx + W * 0.15, cy + H * 0.05)          # first edge, deliberately skew
    G.clickmm(*a); G.clickmm(*b); G.clickmm(cx - W * 0.10, cy + H * 0.20)
    q = ents("line")
    G.check("LENGTH", len(q) == 4, f"sk_rect_oblique: {len(q)} lines")
    if len(q) == 4:
        L = sorted(round(e["length"], 9) for e in q)
        G.check("LENGTH", L[0] == L[1] and L[2] == L[3],
                f"opposite sides equal to 1e-9: {L}")
        angs = []
        for i in range(4):
            for j in range(i + 1, 4):
                u = (q[i]["p1"][0] - q[i]["p0"][0], q[i]["p1"][1] - q[i]["p0"][1])
                v = (q[j]["p1"][0] - q[j]["p0"][0], q[j]["p1"][1] - q[j]["p0"][1])
                c = abs(u[0] * v[0] + u[1] * v[1]) / (math.hypot(*u) * math.hypot(*v))
                angs.append(c)
        G.check("ANGLE", sum(1 for c in angs if c < 1e-9) == 4,
                f"four right angles to 1e-9 ({sum(1 for c in angs if c < 1e-9)} perpendicular pairs)")
        d0 = (q[0]["p1"][0] - q[0]["p0"][0], q[0]["p1"][1] - q[0]["p0"][1])
        G.check("ANGLE", abs(d0[0]) > 1e-6 and abs(d0[1]) > 1e-6,
                f"and it really is oblique: first edge {math.degrees(math.atan2(*d0[::-1])):.3f} deg")

    # --- Rounded rectangle: four lines, four arcs, one radius --------------------------------
    clear_sketch()
    arm("sk_rect_rounded", *free)
    G.clickmm(cx - W * 0.20, cy - H * 0.15)
    G.clickmm(cx + W * 0.20, cy + H * 0.15)
    G.clickmm(cx + W * 0.20 - W * 0.04, cy + H * 0.15)     # third click sets the radius
    ls, ar = ents("line"), ents("arc")
    G.check("ARC", len(ls) == 4 and len(ar) == 4, f"sk_rect_rounded: {len(ls)} lines + {len(ar)} arcs")
    if len(ar) == 4:
        rr = sorted(round(e["radius"], 9) for e in ar)
        G.check("ARC", spread(rr) == 0.0, f"all four fillets share one radius to 1e-9: {rr[0]}")
        lp = G.loops()
        G.check("CLOSED", len(lp) == 1, f"{len(lp)} closed loop")
        if len(lp) == 1:
            xs = [p for e in ls for p in (e["p0"][0], e["p1"][0])]
            ys = [p for e in ls for p in (e["p0"][1], e["p1"][1])]
            # The four straight sides already span the FULL outer box — the top edge runs from
            # x_min+r to x_max-r at y_max — so their bbox is the rectangle itself, and the
            # rounding costs the four corner squares less their quarter-discs: r^2(4 - pi).
            w, h, r = max(xs) - min(xs), max(ys) - min(ys), rr[0]
            want = w * h - r * r * (4 - math.pi)
            G.check("AREA", G.near(abs(lp[0]["area"]), want, 1e-6),
                    f"area {abs(lp[0]['area']):.9f} vs W*H - r^2(4-pi) {want:.9f}")

    # --- Two-point circle: the two clicks are the ends of a diameter --------------------------
    clear_sketch()
    arm("sk_circle_2pt", *free)
    p_a = (cx - W * 0.18, cy - H * 0.10)
    p_b = (cx + W * 0.18, cy + H * 0.10)
    G.clickmm(*p_a); G.clickmm(*p_b)
    c2 = ents("circle")
    G.check("ARC", len(c2) == 1, f"sk_circle_2pt: {len(c2)} circle")
    if c2:
        A, B = clicked(*p_a), clicked(*p_b)
        mid = ((A[0] + B[0]) / 2.0, (A[1] + B[1]) / 2.0)
        tol = 1.5 * G.mm_per_px(cx, cy)
        G.check("VERTEX", dist(c2[0]["center"], mid) <= tol,
                f"centred on the midpoint of the two clicks (off by {dist(c2[0]['center'], mid):.4f} mm)")
        G.check("ARC", abs(c2[0]["radius"] - dist(A, B) / 2.0) <= tol,
                f"radius {c2[0]['radius']:.6f} vs half the click separation {dist(A, B) / 2.0:.6f}")
        opened = bool(G.describe().get("editing"))
        G.check("OFFER", opened, "a radius field opens for it, as it does for the keyed circle")
        if opened:
            G.value(30)
            got = ents("circle")[0]["radius"]
            G.check("ARC", G.near(got, 30.0, 1e-9),
                    f"and it takes a typed radius exactly: {got:.9f} (asked 30.0)")
            # The DoF of ONE CIRCLE is three. Asserted here because it is where the lie showed:
            # after a delete the solver was never re-run, so this reported the DoF of the
            # geometry that had just been erased. snaporca-ua9g.
            G.check("VERTEX", G.describe()["dof"] == 2,
                    f"and the sketch reports the DoF of what is actually in it: {G.describe()['dof']}")

    # --- Three-point circle: all three clicks lie on it ---------------------------------------
    clear_sketch()
    arm("sk_circle_3pt", *free)
    three = [(cx - W * 0.18, cy), (cx, cy + H * 0.18), (cx + W * 0.16, cy - H * 0.06)]
    for pt in three:
        G.clickmm(*pt)
    c3 = ents("circle")
    G.check("ARC", len(c3) == 1, f"sk_circle_3pt: {len(c3)} circle")
    if c3:
        ds = [dist(clicked(*pt), c3[0]["center"]) for pt in three]
        tol = 1.5 * G.mm_per_px(cx, cy)
        G.check("ARC", spread(ds) <= tol and abs(ds[0] - c3[0]["radius"]) <= tol,
                f"all three clicks lie on it: distances {[round(d, 4) for d in ds]} "
                f"vs radius {c3[0]['radius']:.4f}")

    # --- Centre arc: centre, start, end -------------------------------------------------------
    clear_sketch()
    arm("sk_arc_center", *free)
    C = (cx, cy)
    G.clickmm(*C); G.clickmm(cx + W * 0.15, cy); G.clickmm(cx, cy + H * 0.15)
    aa = ents("arc")
    G.check("ARC", len(aa) == 1, f"sk_arc_center: {len(aa)} arc")
    if aa:
        tol = 1.5 * G.mm_per_px(cx, cy)
        G.check("VERTEX", dist(aa[0]["center"], clicked(*C)) <= tol,
                f"centred on the first click (off by {dist(aa[0]['center'], clicked(*C)):.4f} mm)")
        for nm, pt in (("start", aa[0]["p0"]), ("end", aa[0]["p1"])):
            G.check("ARC", abs(dist(pt, aa[0]["center"]) - aa[0]["radius"]) < 1e-9,
                    f"its {nm} sits exactly on the radius, to 1e-9")

    # --- Tangent arc: the construction property, exact whatever the click ---------------------
    clear_sketch()
    G.key("l", 0.5)                              # fixture: one line for the arc to leave tangentially
    la, lb = (cx - W * 0.20, cy - H * 0.05), (cx + W * 0.05, cy - H * 0.05)
    G.clickmm(*la); G.clickmm(*lb)
    G.values(40, 0)
    line = ents("line")[0]
    G.key("Escape", 0.5)
    arm("sk_arc_tangent", *free)
    G.clickmm(*lb)                               # start snaps onto the line's endpoint
    G.clickmm(cx + W * 0.12, cy + H * 0.12)
    ta = ents("arc")
    G.check("ARC", len(ta) == 1, f"sk_arc_tangent: {len(ta)} arc off the line's endpoint")
    if ta:
        end = min((ta[0]["p0"], ta[0]["p1"]), key=lambda q: dist(q, line["p1"]))
        rad = (end[0] - ta[0]["center"][0], end[1] - ta[0]["center"][1])
        d = (line["p1"][0] - line["p0"][0], line["p1"][1] - line["p0"][1])
        cosang = abs(rad[0] * d[0] + rad[1] * d[1]) / (math.hypot(*rad) * math.hypot(*d))
        G.check("TANGENT", cosang < 1e-9,
                f"its radius at the shared end is perpendicular to the line to 1e-9 (cos={cosang:.2e})")

    # --- Arc slot: two concentric arcs, one width --------------------------------------------
    clear_sketch()
    arm("sk_slot_arc", *free)
    G.clickmm(cx - W * 0.15, cy)                 # start
    G.clickmm(cx, cy - H * 0.10)                 # centre
    G.clickmm(cx + W * 0.15, cy)                 # end direction
    G.clickmm(cx + W * 0.15, cy + H * 0.04)      # width
    sa = ents("arc")
    G.check("ARC", len(sa) >= 2, f"sk_slot_arc: {len(sa)} arcs")
    if len(sa) >= 2:
        # Group by centre rather than by size: an arc slot is two RAILS about a common centre
        # plus two end caps about their own, and "the two biggest arcs" is not the same set —
        # it picked a rail and a cap and called them non-concentric.
        groups = {}
        for e in sa:
            k = (round(e["center"][0], 9), round(e["center"][1], 9))
            groups.setdefault(k, []).append(e["radius"])
        rails = max(groups.values(), key=len)
        G.check("ARC", len(rails) == 2,
                f"two rails share one centre to 1e-9 (radii {[round(r, 6) for r in sorted(rails)]}), "
                f"{len(groups) - 1} cap centre(s) besides")

    # --- Ellipse arc: five clicks, and now the socket can actually see its parameters ---------
    clear_sketch()
    arm("sk_ellipse_arc", *free)
    G.clickmm(cx, cy)
    G.clickmm(cx + W * 0.18, cy)
    G.clickmm(cx, cy + H * 0.10)
    G.clickmm(cx + W * 0.18, cy)
    G.clickmm(cx, cy + H * 0.10)
    ea = ents("ellipse_arc")
    G.check("ARC", len(ea) == 1, f"sk_ellipse_arc: {len(ea)} ellipse arc")
    if ea and "radius" in ea[0]:
        G.check("ARC", ea[0]["radius"] > ea[0]["rminor"] > 0,
                f"semi-axes a={ea[0]['radius']:.6f} b={ea[0]['rminor']:.6f}, a > b > 0")
        for nm, pt in (("start", ea[0]["p0"]), ("end", ea[0]["p1"])):
            X = (pt[0] - ea[0]["center"][0], pt[1] - ea[0]["center"][1])
            ph = ea[0]["rotation"]
            u = (X[0] * math.cos(ph) + X[1] * math.sin(ph)) / ea[0]["radius"]
            v = (-X[0] * math.sin(ph) + X[1] * math.cos(ph)) / ea[0]["rminor"]
            G.check("ARC", abs(u * u + v * v - 1.0) < 1e-9,
                    f"its {nm} satisfies (x/a)^2+(y/b)^2 = 1 to 1e-9")

    # --- The five fixed-count polygons: regular, to 1e-9 --------------------------------------
    for verb, n in (("sk_poly_3", 3), ("sk_poly_4", 4), ("sk_poly_5", 5),
                    ("sk_poly_8", 8), ("sk_poly_12", 12)):
        clear_sketch()
        arm(verb, *free)
        G.clickmm(cx, cy)
        G.clickmm(cx + W * 0.15, cy)
        q = ents("line")
        if len(q) != n:
            G.check("LENGTH", False, f"{verb}: {len(q)} sides, expected {n}")
            continue
        L = [round(e["length"], 9) for e in q]
        ctr = clicked(cx, cy)
        R = [dist(e["p0"], ctr) for e in q]
        G.check("LENGTH", spread(L) == 0.0 and spread(R) < 1.5 * G.mm_per_px(cx, cy),
                f"{verb}: {n} equal sides to 1e-9 ({L[0]:.9f}), all vertices on one circle")

    # --- Inscribed vs circumscribed: the exact ratio between them -----------------------------
    radii = {}
    for verb, fit in (("sk_poly_inscribed", "inscribed"), ("sk_poly_circumscribed", "circumscribed")):
        clear_sketch()
        arm(verb, *free)                          # a tool PARAMETER, chosen from the menu
        arm("sk_poly_5", *free)
        G.clickmm(cx, cy)
        G.clickmm(cx + W * 0.15, cy)
        q = ents("line")
        ctr = clicked(cx, cy)
        radii[fit] = dist(q[0]["p0"], ctr) if q else 0.0
    want = 1.0 / math.cos(math.pi / 5.0)
    got = (radii["circumscribed"] / radii["inscribed"]) if radii["inscribed"] else 0.0
    G.check("ARC", G.near(got, want, 1e-6),
            f"circumscribed/inscribed circumradius = {got:.9f} vs 1/cos(pi/5) = {want:.9f} "
            "— the two fits are genuinely different constructions")
    G.leave_sketch()
    G.reset_document()


def tf_fixture(cx, cy, W, L=40):
    """One horizontal line of exactly L mm, drawn by key. Fixture, not the thing under test.

    Horizontal and exactly L because every transform assertion below is derived from it: the
    gizmo seeds its parameters from the target's own size (pivot = the line's midpoint, handle
    radius = half its length), so knowing the line exactly is what makes the handle and its value
    label land on a computable pixel instead of a guessed one.
    """
    G.key("l", 0.5)
    G.clickmm(cx - W * 0.10, cy)
    G.clickmm(cx + W * 0.10, cy)
    G.values(L, 0)
    e = ents("line")[0]
    G.key("Escape", 0.5)
    return e


def tf_label(pivot, handle, at):
    """Where the gizmo prints its value — the same formula render_tf_gizmo uses.

    label = handle + outward * 1.2 * max(15 px, 1e-4), outward = the pivot -> handle direction.
    Recomputing it here rather than hunting for it in pixels is what keeps this a click on a
    control and not a search: if the formula ever moves, this rung fails loudly instead of
    clicking somewhere harmless.
    """
    th = max(15.0 * G.mm_per_px(*at), 1e-4)
    d = (handle[0] - pivot[0], handle[1] - pivot[1])
    n = math.hypot(*d) or 1.0
    return (handle[0] + d[0] / n * th * 1.2, handle[1] + d[1] / n * th * 1.2)


def rung_transforms():
    """O6 — Move, Rotate, Scale, Array and Polar array: five verbs, none with a shortcut.

    Each is a gizmo, so the whole gesture is menu -> pick -> click the value label -> type ->
    click empty to apply, with no keyboard route anywhere in it. The assertions are the exact
    ones the operation promises: a translation moves every point by the typed amount and nothing
    else, a rotation turns the direction by the typed angle and leaves the length alone, a scale
    multiplies the length and leaves the direction alone.
    """
    print("\nO6  the 2D transforms — gizmo verbs, none of them on the keyboard")
    fresh_sketch("p")
    G.key("Escape", 0.5)
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    W, H = (x1 - x0), (y1 - y0)
    free = (cx, y1 - H * 0.10)
    away = (x0 + W * 0.03, y0 + H * 0.03)        # empty plane: the click that applies a gizmo
    L = 40.0
    half = L / 2.0
    step = max(half * 1.5, 1.0)

    def pivot_of(e):
        return ((e["p0"][0] + e["p1"][0]) / 2.0, (e["p0"][1] + e["p1"][1]) / 2.0)

    def direction(e):
        return math.degrees(math.atan2(e["p1"][1] - e["p0"][1], e["p1"][0] - e["p0"][0]))

    # --- Move: 25 mm along +X, and nothing else changes --------------------------------------
    clear_sketch()
    before = tf_fixture(cx, cy, W, L)
    # The transforms are offered for a SELECTION, not for empty space — so the right-click that
    # opens the menu happens ON the line, which is also what selects it. Then one more click
    # picks it as the gizmo's target: choosing the verb sets the mode, it does not carry a pick.
    arm("sk_move", *pivot_of(before))
    G.clickmm(*pivot_of(before))                  # pick the line
    piv = pivot_of(before)
    G.clickmm(*tf_label(piv, (piv[0] + step, piv[1]), (cx, cy)))
    G.value(25)
    G.clickmm(*away)                              # empty click applies
    after = ents("line")
    G.check("VERTEX", len(after) == 1, f"sk_move: {len(after)} line after the transform")
    if len(after) == 1:
        dx = [after[0]["p0"][0] - before["p0"][0], after[0]["p1"][0] - before["p1"][0]]
        dy = [after[0]["p0"][1] - before["p0"][1], after[0]["p1"][1] - before["p1"][1]]
        G.check("LENGTH", all(abs(v - 25.0) < 1e-9 for v in dx) and all(abs(v) < 1e-9 for v in dy),
                f"every point moved by exactly +25.000000000 in X and 0 in Y: dx={dx} dy={dy}")

    # --- Rotate: 30 degrees about the centroid, length untouched ------------------------------
    clear_sketch()
    before = tf_fixture(cx, cy, W, L)
    # The transforms are offered for a SELECTION, not for empty space — so the right-click that
    # opens the menu happens ON the line, which is also what selects it. Then one more click
    # picks it as the gizmo's target: choosing the verb sets the mode, it does not carry a pick.
    arm("sk_rotate", *pivot_of(before))
    G.clickmm(*pivot_of(before))
    piv = pivot_of(before)
    h = (piv[0] + half * math.cos(math.pi / 4), piv[1] + half * math.sin(math.pi / 4))
    G.clickmm(*tf_label(piv, h, (cx, cy)))
    G.value(30)
    G.clickmm(*away)
    after = ents("line")
    G.check("VERTEX", len(after) == 1, f"sk_rotate: {len(after)} line")
    if len(after) == 1:
        turned = (direction(after[0]) - direction(before)) % 360.0
        G.check("ANGLE", min(abs(turned - 30.0), abs(turned - 210.0)) < 1e-9,
                f"turned by exactly {turned:.9f} deg")
        G.check("LENGTH", abs(after[0]["length"] - before["length"]) < 1e-9,
                f"and its length is untouched: {after[0]['length']:.9f}")

    # --- Scale: x3 about the centroid, direction untouched ------------------------------------
    clear_sketch()
    before = tf_fixture(cx, cy, W, L)
    # The transforms are offered for a SELECTION, not for empty space — so the right-click that
    # opens the menu happens ON the line, which is also what selects it. Then one more click
    # picks it as the gizmo's target: choosing the verb sets the mode, it does not carry a pick.
    arm("sk_scale", *pivot_of(before))
    G.clickmm(*pivot_of(before))
    piv = pivot_of(before)
    G.clickmm(*tf_label(piv, (piv[0] + 2.0 * half, piv[1]), (cx, cy)))
    G.value(3)
    G.clickmm(*away)
    after = ents("line")
    G.check("VERTEX", len(after) == 1, f"sk_scale: {len(after)} line")
    if len(after) == 1:
        G.check("LENGTH", abs(after[0]["length"] - 3.0 * before["length"]) < 1e-9,
                f"length {before['length']:.9f} -> {after[0]['length']:.9f}, exactly x3")
        G.check("ANGLE", abs(direction(after[0]) - direction(before)) < 1e-9,
                "and its direction is untouched to 1e-9")

    # --- Linear array: 4 copies at an exact pitch ---------------------------------------------
    clear_sketch()
    before = tf_fixture(cx, cy, W, L)
    # The transforms are offered for a SELECTION, not for empty space — so the right-click that
    # opens the menu happens ON the line, which is also what selects it. Then one more click
    # picks it as the gizmo's target: choosing the verb sets the mode, it does not carry a pick.
    arm("sk_array", *pivot_of(before))
    G.clickmm(*pivot_of(before))
    piv = pivot_of(before)
    # A single LINE target seeds the spacing PERPENDICULAR to it, which for a horizontal line
    # is +Y. That is the tool's own rule, not an assumption: see tf_pick's Array branch.
    G.clickmm(*tf_label(piv, (piv[0], piv[1] + step), (cx, cy)))
    G.value(20)
    th = max(15.0 * G.mm_per_px(cx, cy), 1e-4)
    G.clickmm(piv[0] + th * 1.5, piv[1] + th * 1.5)      # the "xN" count label
    G.value(4)
    G.clickmm(*away)
    rows = sorted(ents("line"), key=lambda e: e["p0"][1])
    G.check("VERTEX", len(rows) == 4, f"sk_array: {len(rows)} lines (1 original + 3 copies)")
    if len(rows) == 4:
        pitch = [round(rows[i + 1]["p0"][1] - rows[i]["p0"][1], 9) for i in range(3)]
        G.check("LENGTH", pitch == [20.0, 20.0, 20.0], f"pitch exactly {pitch} mm")
        G.check("LENGTH", spread([round(e["length"], 9) for e in rows]) == 0.0,
                "and every copy is the same length to 1e-9")

    # --- Polar array: 6 copies, 60 degrees apart, sharing one centre --------------------------
    clear_sketch()
    before = tf_fixture(cx, cy, W, L)
    # The transforms are offered for a SELECTION, not for empty space — so the right-click that
    # opens the menu happens ON the line, which is also what selects it. Then one more click
    # picks it as the gizmo's target: choosing the verb sets the mode, it does not carry a pick.
    arm("sk_array_polar", *pivot_of(before))
    G.clickmm(*pivot_of(before))
    piv = pivot_of(before)
    G.clickmm(*tf_label(piv, (piv[0] + half, piv[1]), (cx, cy)))
    G.value(360)
    th = max(15.0 * G.mm_per_px(cx, cy), 1e-4)
    G.clickmm(piv[0] + th * 1.5, piv[1] + th * 1.5)
    G.value(6)
    G.clickmm(*away)
    spokes = ents("line")
    G.check("VERTEX", len(spokes) == 6, f"sk_array_polar: {len(spokes)} lines")
    if len(spokes) == 6:
        mids = [((e["p0"][0] + e["p1"][0]) / 2.0, (e["p0"][1] + e["p1"][1]) / 2.0) for e in spokes]
        G.check("VERTEX", max(dist(m, mids[0]) for m in mids) < 1e-9,
                "all six share one centre to 1e-9 — rotated about the pivot, not scattered")
        # mod 360, not 180. A line carries an orientation, and folding the six directions into a
        # half-turn collapses opposite spokes onto each other: a perfectly even star then reads
        # as gaps of [0, 60, 0, 60, 0] and the rung fails on its own arithmetic.
        angs = sorted(direction(e) % 360.0 for e in spokes)
        gaps = [round(angs[(i + 1) % 6] - angs[i], 9) % 360.0 for i in range(6)]
        G.check("ANGLE", all(abs(g - 60.0) < 1e-9 for g in gaps),
                f"and they are 60 deg apart all the way round: {gaps}")
    G.leave_sketch()
    G.reset_document()


def rung_art():
    """O7 — Text and SVG: the last two 2D verbs, and the only two that open a dialog.

    Both are keyless, so the offer is their only door; both also leave the canvas for a modal
    window, which is why nothing that drives the canvas had ever reached them. The properties
    graded are the ones that survive a change of font or of importer scale: how many CLOSED loops
    came back, and the exact aspect ratio of a shape whose proportions are known.
    """
    print("\nO7  Text and SVG — the two verbs that go through a dialog")
    fresh_sketch("p")
    G.key("Escape", 0.5)
    x0, x1, y0, y1 = G._SAFE
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    H = y1 - y0
    free = (cx, y1 - H * 0.10)

    # --- Text ---------------------------------------------------------------------------------
    clear_sketch()
    o = open_offer(*free)
    G.check("OFFER", "sk_text" in o.verbs, "sk_text is offered on an empty sketch")
    choose(o, "sk_text")
    time.sleep(1.5)
    names = G.sh(f"DISPLAY={G.DISP} xdotool search --name '.' getwindowname %@").split("\n")
    G.check("OFFER", any(n.strip() == "Text" for n in names),
            "choosing it opens the Text dialog")
    G.typ("LT", 0.4)
    G.key("Return", 2.5)
    lp = G.loops()
    G.check("CLOSED", len(lp) == 2 and all(l["closed"] for l in lp),
            f"two letters came back as {len(lp)} closed loops")
    G.check("VERTEX", all(abs(l["area"]) > 1.0 for l in lp),
            f"both enclose real area: {[round(abs(l['area']), 3) for l in lp]}")

    # --- SVG ----------------------------------------------------------------------------------
    # A file whose proportions are known EXACTLY, so the assertion does not depend on what the
    # importer decides a user unit is: a 40 x 20 path is 2:1 at any scale.
    # FILLED, not stroked. A stroked path imports as its stroke OUTLINE — two loops, an outer and
    # an inner, each inflated by half the stroke width — so the shape that comes back is 8 lines
    # at 1.952 : 1 and the assertion would be grading the pen, not the importer.
    svg = "/tmp/offer-ladder-2to1.svg"
    G.sh("cat > %s <<'EOF'\n<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"40\" height=\"20\" "
         "viewBox=\"0 0 40 20\"><path d=\"M0,0 L40,0 L40,20 L0,20 Z\" fill=\"black\"/>"
         "</svg>\nEOF" % svg)
    clear_sketch()
    o = open_offer(*free)
    G.check("OFFER", "sk_svg" in o.verbs, "sk_svg is offered too")
    choose(o, "sk_svg")
    time.sleep(2.0)
    G.key("ctrl+l", 0.6)            # GTK's own "type a path" entry: never guess at the file list
    G.typ(svg, 0.5)
    G.key("Return", 3.0)
    ls = ents("line")
    lp = G.loops()
    G.check("CLOSED", len(lp) == 1 and len(ls) == 4,
            f"the imported path is {len(ls)} lines and {len(lp)} closed loop")
    if ls:
        xs = [p for e in ls for p in (e["p0"][0], e["p1"][0])]
        ys = [p for e in ls for p in (e["p0"][1], e["p1"][1])]
        w, h = max(xs) - min(xs), max(ys) - min(ys)
        # 1e-6, not 1e-9, and the reason is measured rather than tuned away: the imported box is
        # 10.583333000 x 5.291667000 where 40 and 20 user units at 25.4/96 are 10.58333333... and
        # 5.29166666..., so the SVG path coordinates arrive ROUNDED TO SIX DECIMAL PLACES (both
        # numbers are exactly 6 dp, one rounded down and one up — which is also why the ratio is
        # 1.999999811 rather than 2). Everything the sketcher itself draws is exact to 1e-9; this
        # 1e-6 belongs to the import path alone, and it is the band the assertion allows.
        G.check("LENGTH", abs(w / h - 2.0) < 1e-6,
                f"and its proportions survived the import: {w:.9f} x {h:.9f} = {w / h:.9f} : 1 "
                f"(the import rounds coordinates to 1e-6 mm)")
    G.leave_sketch()
    G.reset_document()


# Every 2D verb this ladder drives from the menu, by id. Kept as data so the coverage claim can
# be CHECKED rather than asserted in prose: rung_coverage compares it against the offer table and
# fails the moment a keyless sketch verb exists that nothing here exercises.
DRIVEN = {
    "sk_rect_center",                                           # O4
    "sk_polyline", "sk_rect_oblique", "sk_rect_rounded",        # O5
    "sk_circle_2pt", "sk_circle_3pt", "sk_arc_center", "sk_arc_tangent",
    "sk_slot_arc", "sk_ellipse_arc",
    "sk_poly_3", "sk_poly_4", "sk_poly_5", "sk_poly_8", "sk_poly_12",
    "sk_poly_inscribed", "sk_poly_circumscribed",
    "sk_move", "sk_rotate", "sk_scale", "sk_array", "sk_array_polar",   # O6
    "sk_text", "sk_svg",                                        # O7
}


def rung_coverage():
    """O8 — the coverage claim, checked against the table instead of written in a comment.

    "Every 2D verb with no keyboard route is exercised" is the whole point of the rungs above, and
    a claim like that rots the day someone adds a verb. Here it is arithmetic: the set of keyless
    sketch verbs in DesignOffer.hpp, minus the set this file drives, must be empty.
    """
    print("\nO8  coverage — every keyless 2D verb, checked against the table")
    sk = [v for v in TABLE if v["sketch_mode"]]
    keyless = {v["id"] for v in sk if v["action"] and not v["key"]}
    keyed = {v["id"] for v in sk if v["key"]}
    dead = {v["id"] for v in sk if not v["action"]}
    missing = keyless - DRIVEN
    G.check("OFFER", not missing,
            f"all {len(keyless)} keyless 2D verbs are driven from the menu"
            + ("" if not missing else f" — MISSING: {sorted(missing)}"))
    G.check("OFFER", not (DRIVEN - keyless - keyed),
            f"and nothing is driven that is not in the table: {sorted(DRIVEN - keyless - keyed)}")
    G.check("OFFER", not dead,
            f"no 2D verb is a dead row: {len(sk)} sketch verbs, {len(keyed)} with a shortcut, "
            f"{len(keyless)} without, {len(dead)} with no GUI route at all")


RUNGS = {"kinds": rung_kinds, "vocabulary": rung_vocabulary,
         "author": rung_author, "no_shortcut": rung_no_shortcut,
         "curves": rung_curves, "transforms": rung_transforms,
         "art": rung_art, "coverage": rung_coverage}


def main():
    if not os.path.exists(LOG):
        G.die(f"no {LOG} — launch the app through scripts/gui-session.sh")
    if "[OFFER]" not in open(LOG, errors="replace").read()[-400000:]:
        print(f"note: no [OFFER] lines in {LOG} yet — the app must run with SNAPORCA_KEYTRACE=1")
    want = sys.argv[1:] or list(RUNGS)
    # TWICE. From a cold launch the app shows the Home page over the Design tab, and the first
    # click only selects the tab — the second is what brings the viewport forward. A ladder that
    # clicked once drew its whole first rung into a webview.
    G.go_design(); G.go_design()
    G.key("Escape", 0.4)
    G.reset_document()
    for name in want:
        if name not in RUNGS:
            G.die(f"unknown rung {name}; have {' '.join(RUNGS)}")
        RUNGS[name]()
    print(f"\n{G._checks - G._fail}/{G._checks} properties held")
    sys.exit(1 if G._fail else 0)


if __name__ == "__main__":
    main()
