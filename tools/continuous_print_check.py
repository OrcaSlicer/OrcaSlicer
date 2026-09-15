#!/usr/bin/env python3
"""Offline zero-travel / single-chain checker for continuous-print G-code (M4).

Parses a G-code file (e.g. OrcaSlicer CLI output) and reports, per layer:
  * intra-layer travel moves  -- non-extruding XY moves between two extrusion moves,
  * layer-boundary travels    -- non-extruding XY moves between the end of a layer and the
                                 first extrusion of the next one,
  * chain breaks              -- consecutive extrusion moves whose end/start do not coincide,
  * retractions, Z regressions, and non-flat object layers.

A pure zero-travel continuous print has zero travels and zero chain breaks in every object
layer. Setup moves (initial homing/priming, skirt, wipe tower) before the first extrusion of a
layer are reported separately and are not counted as object violations.

Exit code is 0 when no violations were found, 1 otherwise (use --warn-only to always exit 0).
"""

from __future__ import annotations

import argparse
import math
import re
import sys
from dataclasses import dataclass, field

XY_TOL = 0.001  # mm, matching exported XY precision
Z_TOL = 0.0001

_WORD_RE = re.compile(r"([A-Za-z])\s*(-?(?:\d+\.?\d*|\.\d+))")


def _words(line: str) -> dict[str, float]:
    return {k.upper(): float(v) for k, v in _WORD_RE.findall(line)}


@dataclass
class Move:
    kind: str  # "extrude" | "travel" | "z" | "retract"
    x0: float
    y0: float
    x1: float
    y1: float
    z: float = 0.0


@dataclass
class Layer:
    index: int
    moves: list[Move] = field(default_factory=list)

    @property
    def extrusions(self) -> list[Move]:
        return [m for m in self.moves if m.kind == "extrude"]

    def intra_travels(self) -> list[Move]:
        idx = [i for i, m in enumerate(self.moves) if m.kind == "extrude"]
        if len(idx) < 2:
            return []
        first, last = idx[0], idx[-1]
        return [m for i, m in enumerate(self.moves) if first < i < last and m.kind == "travel"]

    def chain_breaks(self) -> list[tuple[Move, Move]]:
        breaks = []
        ex = self.extrusions
        for a, b in zip(ex, ex[1:]):
            if math.hypot(b.x0 - a.x1, b.y0 - a.y1) > XY_TOL:
                breaks.append((a, b))
        return breaks


def parse(path: str) -> list[Layer]:
    layers: list[Layer] = [Layer(0)]
    relative_e = relative_xyz = False
    x = y = z = e = 0.0
    role = ""
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if line in ("; CHANGE_LAYER", ";CHANGE_LAYER", ";LAYER_CHANGE", "; LAYER_CHANGE"):
                layers.append(Layer(len(layers)))
                continue
            # Roles are modal and can be omitted at a layer boundary.
            if line.startswith(";TYPE:"):
                role = line[6:].strip()
            elif line.startswith("; FEATURE: "):
                role = line[11:].strip()
            code = line.split(";", 1)[0].strip()
            if not code:
                continue
            cmd = code.split()[0]
            if cmd == "M83": relative_e = True
            if cmd == "M82": relative_e = False
            if cmd == "G91": relative_xyz = True
            if cmd == "G90": relative_xyz = False
            w = _words(code)
            if cmd == "G92":
                x, y, z, e = (w.get(k, old) for k, old in zip("XYZE", (x, y, z, e)))
                continue
            if cmd not in ("G0", "G1", "G2", "G3"):
                continue
            nx, ny, nz = (old + w.get(k, 0) if relative_xyz else w.get(k, old)
                          for k, old in zip("XYZ", (x, y, z)))
            ne = w.get("E", 0 if relative_e else e)
            delta_e = ne if relative_e else ne - e
            moved_xy = math.hypot(nx-x, ny-y) > 1e-9 or cmd in ("G2", "G3")
            if layers[-1].index > 0 and role not in ("Custom", "Skirt", "Brim", "Wipe tower"):
                if moved_xy:
                    kind = "extrude" if delta_e > 1e-9 else "travel"
                    layers[-1].moves.append(Move(kind, x, y, nx, ny, nz))
                elif delta_e < 0:
                    layers[-1].moves.append(Move("retract", x, y, x, y, nz))
                elif "Z" in w:
                    layers[-1].moves.append(Move("z", x, y, x, y, nz))
            x, y, z = nx, ny, nz
            if not relative_e:
                e = ne
    return layers


def analyse(path: str, verbose: bool = False, allow_z_ramp: bool = False) -> int:
    layers = parse(path)
    violations = 0
    print(f"file: {path}")
    print(f"layers: {len(layers)}")
    if verbose:
        for layer in layers:
            kinds: dict[str, int] = {}
            for m in layer.moves:
                kinds[m.kind] = kinds.get(m.kind, 0) + 1
            print(f"  [layer {layer.index}] {kinds}")
    first_layer = None
    for layer in layers:
        if layer.extrusions:
            first_layer = layer.index
            break
    if first_layer is None:
        print("no extrusion moves found")
        return 1

    # Every model layer is checked, including solid bottom/top and fallback layers.
    # A Z-ramp is no longer a reliable way to identify continuous printing.
    region_start = first_layer
    print(f"object region: layers {first_layer}..{layers[-1].index}")

    clean_layers = 0
    object_layers = 0
    for layer in layers:
        if layer.extrusions and layer.index >= region_start:
            object_layers += 1
            intra = layer.intra_travels()
            breaks = layer.chain_breaks()
            zs = [m.z for m in layer.extrusions]
            if not allow_z_ramp and max(zs) - min(zs) > Z_TOL:
                print(f"  layer {layer.index}: non-flat Z {min(zs):.4f}..{max(zs):.4f}")
                violations += 1
            if intra or breaks:
                print(f"  layer {layer.index}: intra-travel={len(intra)} chain-breaks={len(breaks)}")
                for move in intra[:3]:
                    print(f"    travel  ({move.x0:.3f},{move.y0:.3f}) -> ({move.x1:.3f},{move.y1:.3f})")
                for a, b in breaks[:3]:
                    gap = math.hypot(b.x0 - a.x1, b.y0 - a.y1)
                    print(f"    break   ({a.x1:.3f},{a.y1:.3f}) -> ({b.x0:.3f},{b.y0:.3f}) gap={gap:.3f}mm")
                violations += len(intra) + len(breaks)
            else:
                clean_layers += 1

    # Layer-boundary travels: a non-extruding XY move after the last extrusion of a layer that is
    # followed (in the next layer) by an extrusion, i.e. the nozzle was repositioned without extruding.
    for layer, nxt in zip(layers, layers[1:]):
        if not layer.extrusions or not nxt.extrusions or layer.index < region_start:
            continue
        last_extrusion = max(i for i, m in enumerate(layer.moves) if m.kind == "extrude")
        first_extrusion = next(i for i, m in enumerate(nxt.moves) if m.kind == "extrude")
        boundary = layer.moves[last_extrusion+1:] + nxt.moves[:first_extrusion]
        travels = [m for m in boundary if m.kind == "travel"]
        a, b = layer.extrusions[-1], nxt.extrusions[0]
        gap = math.hypot(b.x0-a.x1, b.y0-a.y1)
        if travels or gap > XY_TOL:
            print(f"  layer boundary {layer.index}->{nxt.index}: {len(travels)} travel(s), gap={gap:.3f}mm")
            violations += max(1, len(travels))

    retractions = sum(len([m for m in l.moves if m.kind == "retract"]) for l in layers)

    # Model extrusion Z must never move backwards between or within layers.
    z_regressions = 0
    last_z = None
    for layer in layers:
        if layer.index < region_start:
            continue
        for m in layer.extrusions:
            if last_z is not None and m.z < last_z - 1e-4:
                if z_regressions < 5:
                    print(f"  Z regression at layer {layer.index}: {last_z:.3f} -> {m.z:.3f}")
                z_regressions += 1
            last_z = m.z
    violations += z_regressions

    print(f"continuous-region layers: {object_layers}, fully zero-travel/single-chain: {clean_layers}")
    print(f"retractions: {retractions}, Z regressions: {z_regressions}")
    if violations:
        print(f"RESULT: FAIL ({violations} violation(s) inside the continuous region)")
        return 1
    print("RESULT: PASS (all model layers: zero travel, connected XY, "
          + ("Z monotonic)" if allow_z_ramp else "fixed layer Z)"))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("gcode", help="G-code file to check")
    parser.add_argument("--warn-only", action="store_true", help="always exit 0")
    parser.add_argument("-v", "--verbose", action="store_true", help="print per-layer move counts")
    parser.add_argument("--allow-z-ramp", action="store_true", help="allow varying Z within layers when auditing historical spiral output")
    args = parser.parse_args()
    code = analyse(args.gcode, args.verbose, args.allow_z_ramp)
    return 0 if args.warn_only else code


if __name__ == "__main__":
    sys.exit(main())
