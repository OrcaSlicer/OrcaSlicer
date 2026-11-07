#!/usr/bin/env python3
"""Mesh -> STEP through Mesh2Solid (https://mesh2solid.thavision.com/), the STEP file comes back, optionally into a running Orca-CAD.

  mesh_to_step.py part.stl                          -> part.step next to the input
  mesh_to_step.py part.stl -o out.step --headed     -> same, with the browser window visible (needs a display)
  mesh_to_step.py part.stl --import-to /tmp/orca-nd.sock   -> then import_step into that Orca-CAD (the path must be readable by that app)

Mesh2Solid is a browser-only tool (free for private, non-commercial use; processed in the page; no API), so this drives it with Playwright
(chromium, SwiftShader WebGL when there is no GPU). Only the free controls are used: Open Mesh, Analyse Mesh, Download STEP. The Pro panel,
the Fusion button and any licence/payment field are never touched. Meshes of 100k+ triangles are slow: decimate first (see
jw-docs/assembly/m2s/dec.py) or pass --max-tris to refuse them.

ScanRuler is not a one-shot converter: it is an agent-driven, feature-by-feature reverse engineering session that the user watches
(see ~/projects/scanruler and scanruler-connect.sh), so it has no engine here.

Exit 0 = a STEP file exists, is non-empty and has a STEP header (its path is printed). Anything else exits non-zero and says why on stderr.
Needs: pip install playwright && python -m playwright install chromium; trimesh only for the triangle count (optional).
"""
import argparse, json, socket, sys, time
from pathlib import Path

URL = "https://mesh2solid.thavision.com/"
EXTS = {".stl", ".obj", ".3mf", ".ply"}


def tri_count(path):
    try:
        import trimesh
        return len(trimesh.load(str(path), force="mesh").faces)
    except Exception:
        return None


def mesh2solid(mesh, out, headed, timeout):
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        b = p.chromium.launch(headless=not headed,
                              args=["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader", "--ignore-gpu-blocklist"])
        try:
            pg = b.new_page(viewport={"width": 1400, "height": 900}, accept_downloads=True)
            pg.goto(URL, wait_until="networkidle", timeout=60000)
            pg.set_input_files("input[type=file]", str(mesh))
            pg.wait_for_timeout(5000)
            pg.get_by_role("button", name="Analyse Mesh").click()
            t0, last, stable = time.time(), "", 0
            while time.time() - t0 < timeout and stable < 10:        # done when the status text has been still for 10 s
                pg.wait_for_timeout(2000)
                cur = pg.evaluate("document.body.innerText.slice(0,1500)")
                stable = stable + 2 if cur == last else 0
                last = cur
            if time.time() - t0 >= timeout:
                sys.exit(f"Mesh2Solid still analysing after {timeout}s; page says: {last[-300:]!r}")
            with pg.expect_download(timeout=300000) as d:
                pg.get_by_role("button", name="Download STEP").click()
            d.value.save_as(str(out))
            return last
        finally:
            b.close()


def orca_import(sock, step):
    s = socket.socket(socket.AF_UNIX); s.settimeout(300); s.connect(sock)
    s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": "import_step", "params": {"path": str(step)}}) + "\n").encode())
    buf = b""
    while b"\n" not in buf:
        d = s.recv(1 << 20)
        if not d: break
        buf += d
    r = json.loads(buf.split(b"\n")[0])
    if "error" in r: sys.exit(f"Orca-CAD refused the STEP: {r['error']}")
    return r.get("result")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("mesh", type=Path)
    p.add_argument("-o", "--output", type=Path)
    p.add_argument("--headed", action="store_true", help="show the browser window")
    p.add_argument("--max-tris", type=int, default=0, help="refuse meshes with more triangles than this (0 = no limit)")
    p.add_argument("--import-to", metavar="SOCKET", help="Orca-CAD MCP socket: import the STEP there afterwards")
    p.add_argument("--timeout", type=int, default=1500, help="seconds to wait for the analysis")
    a = p.parse_args()
    if not a.mesh.is_file() or a.mesh.suffix.lower() not in EXTS: sys.exit(f"{a.mesh}: not a file of {sorted(EXTS)}")
    n = tri_count(a.mesh)
    if a.max_tris and n and n > a.max_tris: sys.exit(f"{a.mesh.name}: {n} triangles > --max-tris {a.max_tris}; decimate it first")
    out = (a.output or a.mesh.with_suffix(".step")).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    status = mesh2solid(a.mesh.resolve(), out, a.headed, a.timeout)
    if not out.is_file() or out.stat().st_size < 200 or b"ISO-10303" not in out.read_bytes()[:300]:
        sys.exit(f"{out}: Mesh2Solid gave no valid STEP; page says: {status[-300:]!r}")
    print(out)
    if a.import_to:
        print(json.dumps(orca_import(a.import_to, out))[:400])


if __name__ == "__main__":
    main()
