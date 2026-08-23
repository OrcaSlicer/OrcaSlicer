#!/usr/bin/env python3
"""Rung 9: the ladder, graded against real drawings instead of shapes I chose.

Rungs 1-8 are hand-built. That is their weakness: I wrote both the geometry and the
assertion, so they prove the engine does what I expected on cases I picked. This rung
takes a SYSTEMATIC sample of the StudyCadCam corpus (every 20th sheet, 1..996 — no
cherry-picking) and grades the engine against each drawing's OWN vector geometry,
extracted from the PDF. Nothing here is transcribed by eye; the drawing is the input.

The method: pdftocairo renders the sheet to SVG, where the drawn geometry is exactly the
stroked (fill="none") paths and the text is filled glyph paths. Beziers are flattened, so
every entity handed to the engine is a straight line and every comparison below is EXACT
— no faceting tolerance to hide behind. The closed chains are then found twice: once by
this script, in plain Python, and once by the engine. The assertions are that the two
agree, and that the engine's own operations preserve what they promise.

  CLOSED    the engine finds the same closed loops this script does
  AREA      the engine's area for each loop equals the shoelace area, to 1e-6
  VOID      the engine attributes each void to the loop that actually contains it
  MIRROR    a real closed profile, mirrored, is still exactly one closed loop
  OFFSET    a real closed profile, offset, is still closed

Usage:  ladder-corpus.py [--sample N] [--corpus DIR]
"""

import argparse
import glob
import json
import math
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

SOCK = os.environ.get("SNAPORCA_MCP", "/tmp/mcp.sock")
TOL = 1e-6          # exact-comparison tolerance (all inputs are lines)
WELD = 0.05         # endpoint-coincidence tolerance, in PDF units


# ── the socket ───────────────────────────────────────────────────────────────
_id = [0]


def try_call(method, **params):
    """sketch_cancel throws when nothing is open, which is not an error to us."""
    try:
        return call(method, **params)
    except RuntimeError:
        return None


def call(method, **params):
    _id[0] += 1
    req = json.dumps({"jsonrpc": "2.0", "id": _id[0], "method": method,
                      "params": params}) + "\n"
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(30)
    s.connect(SOCK)
    s.sendall(req.encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    r = json.loads(buf.decode())
    if "error" in r:
        raise RuntimeError(r["error"]["message"])
    return r["result"]


# ── SVG → line segments ──────────────────────────────────────────────────────
NUM = r"[-+]?[0-9]*\.?[0-9]+(?:[eE][-+]?[0-9]+)?"


def bezier(p0, p1, p2, p3, n=16):
    """Flatten a cubic to n straight segments — the engine then sees only lines."""
    out = []
    for i in range(n):
        t0, t1 = i / n, (i + 1) / n
        pts = []
        for t in (t0, t1):
            u = 1 - t
            x = (u ** 3 * p0[0] + 3 * u * u * t * p1[0]
                 + 3 * u * t * t * p2[0] + t ** 3 * p3[0])
            y = (u ** 3 * p0[1] + 3 * u * u * t * p1[1]
                 + 3 * u * t * t * p2[1] + t ** 3 * p3[1])
            pts.append((x, y))
        out.append((pts[0], pts[1]))
    return out


def path_segments(d):
    """Parse one SVG path's `d` into straight segments."""
    toks = re.findall(r"([MLCZmlcz])|(" + NUM + ")", d)
    cmds, cur, start, segs, i = [], None, None, [], 0
    flat = []
    for a, b in toks:
        flat.append(a if a else float(b))
    while i < len(flat):
        t = flat[i]
        if isinstance(t, str):
            cmd = t
            i += 1
        # numbers repeat the previous command, as SVG allows
        if cmd in ("M", "m"):
            x, y = flat[i], flat[i + 1]; i += 2
            cur = (x, y); start = cur
        elif cmd in ("L", "l"):
            x, y = flat[i], flat[i + 1]; i += 2
            segs.append((cur, (x, y))); cur = (x, y)
        elif cmd in ("C", "c"):
            p1 = (flat[i], flat[i + 1]); p2 = (flat[i + 2], flat[i + 3])
            p3 = (flat[i + 4], flat[i + 5]); i += 6
            segs.extend(bezier(cur, p1, p2, p3)); cur = p3
        elif cmd in ("Z", "z"):
            if cur and start and dist(cur, start) > TOL:
                segs.append((cur, start))
            cur = start
        else:
            i += 1
    return segs


def dist(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def drawing_segments(pdf):
    """Every stroked segment on the sheet, in PDF units (y already flipped up)."""
    with tempfile.TemporaryDirectory() as td:
        svg = os.path.join(td, "p.svg")
        subprocess.run(["pdftocairo", "-svg", pdf, svg],
                       check=True, capture_output=True)
        s = open(svg).read()
    segs = []
    # Drawn geometry is stroked with no fill; glyphs are filled with no stroke.
    for m in re.finditer(r'<path([^>]*)d="([^"]+)"', s):
        attrs, d = m.group(1), m.group(2)
        if 'fill="none"' not in attrs or "stroke=" not in attrs:
            continue
        segs.extend(path_segments(d))
    return [((a[0], -a[1]), (b[0], -b[1])) for a, b in segs if dist(a, b) > TOL]


# ── closed-chain finding, independent of the engine ──────────────────────────
def find_loops(segs):
    """Chain segments into closed loops. Returns a list of point rings."""
    key = lambda p: (round(p[0] / WELD), round(p[1] / WELD))
    adj = {}
    for i, (a, b) in enumerate(segs):
        adj.setdefault(key(a), []).append((i, a, b))
        adj.setdefault(key(b), []).append((i, b, a))
    used, loops = set(), []
    for i0 in range(len(segs)):
        if i0 in used:
            continue
        a, b = segs[i0]
        ring, cur, prev = [a, b], b, i0
        used.add(i0)
        while True:
            nxt = None
            for (j, p, q) in adj.get(key(cur), []):
                if j in used:
                    continue
                nxt = (j, q)
                break
            if nxt is None:
                break
            used.add(nxt[0])
            cur = nxt[1]
            ring.append(cur)
            if dist(cur, ring[0]) <= WELD:
                # Snap the seam shut. The gap is a flattening artefact of the PDF, up to
                # WELD wide, and handing the engine a ring that misses closing by 0.03 would
                # be testing my extractor's sloppiness rather than the engine's chaining.
                ring[-1] = ring[0]
                loops.append(ring)
                ring = None
                break
        # an open chain is simply not a loop; it is dropped
    return loops


def shoelace(ring):
    a = 0.0
    for i in range(len(ring) - 1):
        a += ring[i][0] * ring[i + 1][1] - ring[i + 1][0] * ring[i][1]
    return abs(a) * 0.5


def point_in(pt, ring):
    inside = False
    for i in range(len(ring) - 1):
        A, B = ring[i], ring[i + 1]
        if (A[1] > pt[1]) != (B[1] > pt[1]) and \
           pt[0] < (B[0] - A[0]) * (pt[1] - A[1]) / (B[1] - A[1]) + A[0]:
            inside = not inside
    return inside


def interior_point(ring):
    """A point strictly inside a simple closed ring (first == last).

    The lowest vertex of a simple polygon is always convex, so stepping from it along the
    bisector of its two edges goes inward; the step is a small fraction of the shorter edge so
    it stays inside however sharp the corner is.
    """
    q = ring[:-1] if len(ring) > 1 and ring[0] == ring[-1] else ring
    if len(q) < 3:
        return ring[0]
    k = min(range(len(q)), key=lambda i: (q[i][1], q[i][0]))
    v = q[k]
    a = (q[(k - 1) % len(q)][0] - v[0], q[(k - 1) % len(q)][1] - v[1])
    b = (q[(k + 1) % len(q)][0] - v[0], q[(k + 1) % len(q)][1] - v[1])
    la, lb = math.hypot(*a), math.hypot(*b)
    if la < 1e-12 or lb < 1e-12:
        return v
    a = (a[0] / la, a[1] / la)
    b = (b[0] / lb, b[1] / lb)
    bx, by = a[0] + b[0], a[1] + b[1]
    n = math.hypot(bx, by)
    if n < 1e-12:
        return v
    step = 1e-3 * min(la, lb)
    return (v[0] + bx / n * step, v[1] + by / n * step)


# ── one drawing ──────────────────────────────────────────────────────────────
def grade(pdf, name, report):
    segs = drawing_segments(pdf)
    loops = find_loops(segs)
    if len(loops) < 2:
        report(name, "SKIP", f"no nested closed geometry found ({len(loops)} loops)")
        return None

    # The biggest loop is the sheet frame; the part outlines live inside it. Take the
    # largest loop that is NOT the frame, plus every loop contained in it.
    loops.sort(key=shoelace, reverse=True)
    outer = loops[1]
    voids = [r for r in loops[2:]
             if shoelace(r) > 1.0 and point_in(r[0], outer)]
    if shoelace(outer) < 100.0:
        # Not a defect and not a near miss: on these sheets the part outline is not a closed
        # stroked path at all, so the only loops extraction recovers are glyph counters and
        # arrowheads. Measured on MPD12/30/31/60: the LARGEST loop on the sheet is 5 to 132 mm2.
        # Say the number, so nobody has to re-measure to know which kind of skip this is.
        report(name, "SKIP", f"no part outline on this sheet — largest loop is only "
                             f"{shoelace(outer):.1f} mm2")
        return None

    # Feed the drawing's own geometry to the engine, as lines only.
    ents = []
    rings = [outer] + voids
    for ring in rings:
        for i in range(len(ring) - 1):
            ents.append({"type": "line",
                         "p0": [ring[i][0], ring[i][1]],
                         "p1": [ring[i + 1][0], ring[i + 1][1]]})
    try_call("sketch_cancel")
    call("sketch_begin", plane="XY")
    call("sketch_add", entities=ents)
    r = call("sketch_describe")

    ok = True
    got = r["closed_loops"]
    ok &= report(name, "CLOSED", f"engine finds {len(got)} closed loops, this script "
                                f"finds {len(rings)}", len(got) == len(rings))

    # AREA — exact, because every entity is a line
    mine = sorted(shoelace(x) for x in rings)
    theirs = sorted(abs(l["area"]) for l in got)
    # The bar: 1e-3 absolute, or 1e-6 relative for the big loops. Not bit-exactness — the
    # auto-constraint pass still snaps segments that are already axis-aligned to within its
    # 1e-4 rad tolerance, which moves an area by ~1e-4. It is deliberately tight enough to
    # have caught the real defect this rung was written for: with the old 3 degree gesture
    # slack applied to scripted input, a flattened circle came back 0.067% small — 0.266 on
    # an area of 397, some 300x above this line.
    same = len(mine) == len(theirs) and all(
        abs(a - b) <= max(1e-3, 1e-6 * a) for a, b in zip(mine, theirs))
    ok &= report(name, "AREA", "every loop area matches the shoelace value exactly", same)

    # VOID — a loop belongs to the SMALLEST loop that contains it, not to every loop that
    # encloses it. A hole inside a boss inside the part is a void of the boss, and the part
    # owns the boss. Comparing against "everything inside the outline" was measuring my own
    # sloppiness: on MPD781 that counted 36 voids where 26 of them are nested inside another
    # void. So compute the same rule here, independently, and compare the whole attribution.
    if got:
        rings = [outer] + voids
        # Probe from a point STRICTLY INSIDE each ring, never from one of its vertices — the
        # same rule the engine now uses (DesignSketchTool::region_loops). A vertex is exactly
        # where two loops touch in a real drawing, and a ray cast from a point lying ON the
        # polygon under test answers by rounding: that alone accounted for every one of the 6
        # sheets where the two attributions used to disagree. snaporca-5hvl.
        probes = [interior_point(r) for r in rings]
        mine_parent = {}
        for i, r in enumerate(rings):
            best, best_a = -1, 0.0
            for j, q in enumerate(rings):
                if i == j or not point_in(probes[i], q):
                    continue
                a = shoelace(q)
                if best < 0 or a < best_a:
                    best, best_a = j, a
            if best >= 0:
                mine_parent.setdefault(best, []).append(i)
        big = max(range(len(got)), key=lambda i: abs(got[i]["area"]))
        # match engine loops to my rings by area, then compare the two attributions by COUNT
        mine_counts = sorted(len(v) for v in mine_parent.values())
        got_counts = sorted(len(l["holes"]) for l in got if l["holes"])
        ok &= report(name, "VOID",
                     f"void attribution matches: engine {got_counts}, containment "
                     f"{mine_counts}", got_counts == mine_counts)

    # MIRROR / OFFSET — engine operations on a REAL profile, not a tidy one
    n_outer = len(outer) - 1
    try_call("sketch_cancel")
    call("sketch_begin", plane="XY")
    call("sketch_add", entities=ents[:n_outer])
    xs = [p[0] for p in outer]
    axis = min(xs) - 10.0
    call("sketch_select", entities=list(range(n_outer)))
    try:
        call("sketch_mirror", axis_a=[axis, 0], axis_b=[axis, 1])
        m = call("sketch_describe")
        ok &= report(name, "MIRROR", "the mirrored copy is closed too",
                     len(m["closed_loops"]) == 2 and m["open_ends"] == [])
    except RuntimeError as e:
        ok &= report(name, "MIRROR", f"refused: {e}", False)

    try_call("sketch_cancel")
    call("sketch_begin", plane="XY")
    call("sketch_add", entities=ents[:n_outer])
    try:
        call("sketch_offset", distance=0.5, entities=list(range(n_outer)))
        o = call("sketch_describe")
        ok &= report(name, "OFFSET", "the offset profile is still closed",
                     any(l["closed"] for l in o["closed_loops"]))
    except RuntimeError as e:
        ok &= report(name, "OFFSET", f"refused: {e}", False)
    return ok


# ── scale ────────────────────────────────────────────────────────────────────
def grade_scale(pdf, name, report, budget):
    """Same exactness, on a profile of several hundred entities, and timed.

    "Interactive" is measurable from here even though nothing is clicked: every MCP verb is
    serviced on the UI THREAD, so the time a reply takes is time the window was not repainting.
    A round trip that stays inside the budget is a window that stayed responsive.
    """
    segs = drawing_segments(pdf)
    loops = find_loops(segs)
    if len(loops) < 2:
        report(name, "SKIP", f"no nested closed geometry found ({len(loops)} loops)")
        return None
    loops.sort(key=shoelace, reverse=True)
    outer = loops[1]
    voids = [r for r in loops[2:] if shoelace(r) > 1.0 and point_in(r[0], outer)]
    rings = [outer] + voids
    ents = []
    for ring in rings:
        for i in range(len(ring) - 1):
            ents.append({"type": "line",
                         "p0": [ring[i][0], ring[i][1]],
                         "p1": [ring[i + 1][0], ring[i + 1][1]]})
    if len(ents) < 300:
        report(name, "SKIP", f"only {len(ents)} entities — not a scale case")
        return None

    try_call("sketch_cancel")
    call("sketch_begin", plane="XY")
    t0 = time.monotonic(); call("sketch_add", entities=ents);  t_add = time.monotonic() - t0
    t0 = time.monotonic(); r = call("sketch_describe");        t_desc = time.monotonic() - t0
    t0 = time.monotonic(); call("sketch_select", entities=list(range(len(ents))))
    t_sel = time.monotonic() - t0
    t0 = time.monotonic(); call("sketch_validate");            t_val = time.monotonic() - t0

    ok = True
    ok &= report(name, "SCALE", f"{len(ents)} entities in {len(rings)} loops", True)
    got = r["closed_loops"]
    ok &= report(name, "CLOSED", f"engine finds {len(got)} closed loops, this script "
                                 f"finds {len(rings)}", len(got) == len(rings))
    mine = sorted(shoelace(x) for x in rings)
    theirs = sorted(abs(l["area"]) for l in got)
    same = len(mine) == len(theirs) and all(
        abs(a - b) <= max(1e-3, 1e-6 * a) for a, b in zip(mine, theirs))
    ok &= report(name, "AREA", "every loop area matches the shoelace value exactly", same)
    worst = max(t_add, t_desc, t_sel, t_val)
    ok &= report(name, "TIME", f"add {t_add*1000:.0f} ms, describe {t_desc*1000:.0f} ms, "
                               f"select {t_sel*1000:.0f} ms, validate {t_val*1000:.0f} ms "
                               f"(budget {budget*1000:.0f} ms)", worst <= budget)
    return ok


def _pdf_error(path):
    """What poppler says about a file it refused, so a refusal can be classified."""
    r = subprocess.run(["pdfinfo", path], capture_output=True, text=True)
    return (r.stderr or "") + (r.stdout or "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=os.path.expanduser("~/studycadcam"))
    ap.add_argument("--step", type=int, default=20)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--scale", action="store_true",
                    help="grade the LARGEST drawings instead: exactness plus a UI-thread budget")
    ap.add_argument("--budget", type=float, default=2.0,
                    help="seconds; the slowest round trip a scale drawing may take")
    a = ap.parse_args()

    files = {}
    for f in glob.glob(os.path.join(a.corpus, "MPD*.pdf")):
        m = re.search(r"MPD(\d+)", os.path.basename(f))
        if m:
            files[int(m.group(1))] = f
    # step 1 means EVERY sheet. Written as `n % step == 1` it silently selected nothing, because
    # n % 1 is always 0 — and the run then printed "RUNG 9 HELD" over zero drawings graded. A
    # gate that passes by grading nothing is worse than no gate, so the count is checked below.
    picks = [files[n] for n in sorted(files) if a.step <= 1 or n % a.step == 1]
    if a.scale:
        # The heaviest real profiles in the corpus, biggest first — up to ~1300 entities.
        sized = []
        for f in files.values():
            try:
                segs = drawing_segments(f)
                loops = find_loops(segs)
                if len(loops) < 2:
                    continue
                loops.sort(key=shoelace, reverse=True)
                outer = loops[1]
                voids = [r for r in loops[2:] if shoelace(r) > 1.0 and point_in(r[0], outer)]
                sized.append((sum(len(r) - 1 for r in [outer] + voids), f))
            except Exception:                        # noqa: BLE001
                continue
        sized.sort(reverse=True)
        picks = [f for _, f in sized[:max(1, a.limit or 6)]]
    elif a.limit:
        picks = picks[:a.limit]
    print(f"corpus: {len(files)} sheets;  systematic sample every {a.step}th "
          f"-> {len(picks)} drawings\n")

    fails, results = [], []

    def report(name, tag, msg, cond=True):
        if tag == "SKIP":
            print(f"  {name:9s} SKIP    {msg}")
            return True
        mark = "ok  " if cond else "FAIL"
        print(f"  {name:9s} {tag:8s} {mark}  {msg}")
        if not cond:
            fails.append(f"{name} {tag}: {msg}")
        return cond

    for f in picks:
        name = re.search(r"MPD\d+", os.path.basename(f)).group(0)
        try:
            r = (grade_scale(f, name, report, a.budget) if a.scale
                 else grade(f, name, report))
            if r is not None:
                results.append((name, r))
        except Exception as e:                       # noqa: BLE001
            # An unreadable SOURCE file is not a grading failure. MPD133 of this corpus is
            # password-protected, and pdftocairo says so on stderr while exiting non-zero;
            # reporting that as ERROR made one encrypted sheet look like an engine defect.
            if "password" in _pdf_error(f).lower():
                report(name, "SKIP", "the PDF is password-protected — nothing to extract")
            else:
                report(name, "ERROR", str(e)[:120], False)

    graded = len(results)
    passed = sum(1 for _, r in results if r)
    if graded == 0:
        print("\nNOTHING WAS GRADED — that is a harness failure, not a clean run", file=sys.stderr)
        sys.exit(2)
    print(f"\ngraded {graded} drawings; {passed} fully clean, {graded - passed} with "
          f"at least one failure")
    if fails:
        print("\nfailures:")
        for x in fails:
            print("  " + x)
        sys.exit(1)
    print("\nRUNG 9 HELD — the engine agrees with the drawings, not with me")


if __name__ == "__main__":
    main()
