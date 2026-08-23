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


def rung_kinds():
    """O1 — the offer adapts to the element under the cursor, one element type at a time."""
    print("\nO1  the offer reads what was right-clicked")
    G.enter_sketch("l")
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
    # ONE Escape, to drop the armed tool to Select. Left-click means "draw" while a tool is
    # armed, so a picking gesture has to say so first — and from a draw tool with no anchor down
    # Escape does exactly that and nothing more; it is only a second Escape, from Select, that
    # would leave the sketch.
    G.key("Escape", 0.5)
    G.clickmm((ax + bx) / 2.0, ay)
    G.xdo("keydown shift")
    G.clickmm((ax + bx) / 2.0, ay - (y1 - y0) * 0.18)
    G.xdo("keyup shift")
    o = open_offer((ax + bx) / 2.0, ay)
    G.check("OFFER", o.kind == 19, f"right-click with two picked -> {o.kind_name()}")
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
    G.enter_sketch("l")
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
             ("SkPoint", pxx, pyy)]
    seen = {}
    for name, X, Y in where:
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
            "all four selections offer a different set: "
            + ", ".join(f"{k}={len(v)}" for k, v in seen.items()))
    G.check("OFFER", seen["SkLine"] - seen["SkNone"],
            f"a picked line adds {len(seen['SkLine'] - seen['SkNone'])} verbs an empty pick has not: "
            + ", ".join(sorted(seen["SkLine"] - seen["SkNone"])[:8]))
    G.leave_sketch()
    G.reset_document()


def rung_author():
    """O3 — the target itself, authored through the menu: no tool key is pressed anywhere here."""
    print("\nO3  a precise closed profile, drawn entirely from the right-click offer")
    G.enter_sketch("p")            # 'p' only to open the session; calibration needs the Point tool
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
    G.enter_sketch("p")
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


RUNGS = {"kinds": rung_kinds, "vocabulary": rung_vocabulary,
         "author": rung_author, "no_shortcut": rung_no_shortcut}


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
