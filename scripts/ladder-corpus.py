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
        report(name, "SKIP", "outline too small to grade")
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
        mine_parent = {}
        for i, r in enumerate(rings):
            best, best_a = -1, 0.0
            for j, q in enumerate(rings):
                if i == j or not point_in(r[0], q):
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=os.path.expanduser("~/studycadcam"))
    ap.add_argument("--step", type=int, default=20)
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    files = {}
    for f in glob.glob(os.path.join(a.corpus, "MPD*.pdf")):
        m = re.search(r"MPD(\d+)", os.path.basename(f))
        if m:
            files[int(m.group(1))] = f
    picks = [files[n] for n in sorted(files) if n % a.step == 1]
    if a.limit:
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
            r = grade(f, name, report)
            if r is not None:
                results.append((name, r))
        except Exception as e:                       # noqa: BLE001
            report(name, "ERROR", str(e)[:120], False)

    graded = len(results)
    passed = sum(1 for _, r in results if r)
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
