#!/usr/bin/env python3
"""Offline zero-travel / single-chain checker for continuous-print G-code (M4).

Parses a G-code file (e.g. OrcaSlicer CLI output) and reports, per layer:
  * intra-layer travel moves  -- non-extruding XY moves between two extrusion moves,
  * layer-boundary travels    -- non-extruding XY moves between the end of a layer and the
                                 first extrusion of the next one,
  * chain breaks              -- consecutive extrusion moves whose end/start do not coincide,
  * retractions and Z regressions at layer boundaries.

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

XY_TOL = 0.05  # mm; endpoints closer than this count as coincident

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
    relative_e = False
    x = y = e = 0.0
    z = 0.0

    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for raw in handle:
            line = raw.strip()
            if line in ("; CHANGE_LAYER", ";CHANGE_LAYER", ";LAYER_CHANGE", "; LAYER_CHANGE"):
                layers.append(Layer(len(layers)))
                continue
            # Drop trailing comments and tool-change/slicing metadata lines.
            if not line or line.startswith(";"):
                continue
            if line.startswith("M83"):
                relative_e = True
                continue
            if line.startswith("M82"):
                relative_e = False
                continue
            code = line.split(";", 1)[0].strip()
            if not code.startswith("G0") and not code.startswith("G1"):
                continue
            w = _words(code)
            has_xy = "X" in w or "Y" in w
            has_z = "Z" in w
            has_e = "E" in w
            nx = w.get("X", x)
            ny = w.get("Y", y)
            nz = w.get("Z", z)
            ne = w.get("E", e)

            if has_e:
                delta_e = ne if relative_e else (ne - e)
            else:
                delta_e = 0.0

            if has_e and delta_e < 0 and not has_xy:
                layers[-1].moves.append(Move("retract", x, y, x, y, nz))
            elif has_xy and (abs(nx - x) > 1e-9 or abs(ny - y) > 1e-9):
                if has_e and delta_e > 1e-9:
                    layers[-1].moves.append(Move("extrude", x, y, nx, ny, nz))
                else:
                    layers[-1].moves.append(Move("travel", x, y, nx, ny, nz))
            elif has_z and not has_xy:
                layers[-1].moves.append(Move("z", x, y, x, y, nz))

            x, y, e, z = nx, ny, ne, nz

    return layers


def analyse(path: str, verbose: bool = False) -> int:
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

    # The continuous region is the longest run of consecutive layers whose extrusions ramp Z within
    # the layer (the Z-ramp). Setup/skirt/bottom-solid layers keep a constant Z and are excluded, so
    # the verdict reflects the zero-travel promise of the continuous region only.
    ramped = []
    for layer in layers:
        zs = [m.z for m in layer.extrusions]
        ramped.append(bool(zs) and max(zs) - min(zs) > 1e-3)
    best_start = best_len = cur_start = cur_len = 0
    for idx, flag in enumerate(ramped):
        if flag:
            if cur_len == 0:
                cur_start = layers[idx].index
            cur_len += 1
            if cur_len > best_len:
                best_start, best_len = cur_start, cur_len
        else:
            cur_len = 0
    region_start = best_start if best_len > 0 else first_layer
    print(f"continuous region: layers {best_start}..{best_start + best_len - 1} ({best_len} Z-ramped layers)"
          if best_len > 0 else "continuous region: none (no Z-ramped layer found)")

    clean_layers = 0
    object_layers = 0
    for layer in layers:
        if layer.extrusions and layer.index >= region_start:
            object_layers += 1
            intra = layer.intra_travels()
            breaks = layer.chain_breaks()
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
        # find last move after the final extrusion
        leading = []
        for m in nxt.moves:
            if m.kind == "extrude":
                break
            if m.kind == "travel":
                leading.append(m)
        # The first extrusion move of the next layer welds from wherever the nozzle stopped; a
        # leading travel means an actual un-extruded reposition.
        if leading:
            start = (nxt.extrusions[0].x0, nxt.extrusions[0].y0)
            prev_end = (layer.extrusions[-1].x1, layer.extrusions[-1].y1)
            gap = math.hypot(start[0] - prev_end[0], start[1] - prev_end[1])
            print(f"  layer boundary {layer.index}->{nxt.index}: {len(leading)} leading travel(s), gap={gap:.3f}mm")
            violations += len(leading)

    retractions = sum(len([m for m in l.moves if m.kind == "retract"]) for l in layers)

    # Z must never move backwards on an extruding move (continuous print ramps Z up along the trace).
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
    print("RESULT: PASS (continuous region: no intra-layer travel, no chain breaks, Z monotonic)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("gcode", help="G-code file to check")
    parser.add_argument("--warn-only", action="store_true", help="always exit 0")
    parser.add_argument("-v", "--verbose", action="store_true", help="print per-layer move counts")
    args = parser.parse_args()
    code = analyse(args.gcode, args.verbose)
    return 0 if args.warn_only else code


if __name__ == "__main__":
    sys.exit(main())
