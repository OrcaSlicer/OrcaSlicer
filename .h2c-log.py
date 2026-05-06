#!/usr/bin/env python3
"""
H2C debug-log analyzer.

Parses an OrcaSlicer debug log to extract H2C nozzle / extruder / sync state
that's relevant to the multi-nozzle slot-assignment work (`add_h2c` branch).

Usage:
    .h2c-log.py [LOG_PATH]            # default: latest debug_*.log* in
                                      # ~/.config/OrcaSlicer/log
    .h2c-log.py --section nozzle      # only nozzle.info dumps
    .h2c-log.py --section sync        # only MQTT sync events
    .h2c-log.py --section heal        # only self-heal events
    .h2c-log.py --section payload     # raw 'merged playload=' (truncated by logger)
    .h2c-log.py --last N              # only last N matches per section

Notes:
  * The "merged playload=" log line truncates JSON at the first close-brace+
    newline. For full nozzle.info parsing we look at the raw 'json::parse' /
    push_status MQTT payloads when available.
  * Bit semantics (from DevNozzleSystem.cpp:786 ParseV2_0):
      njon["id"] hex bit 1 == 1  -> nozzle is in the rack (holder)
      njon["id"] hex bit 1 == 0  -> nozzle is mounted on an extruder
      njon["id"] hex bit 0       -> nozzle id (per-extruder or per-rack-slot)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path

LOG_DIR = Path.home() / ".config" / "OrcaSlicer" / "log"


def latest_log() -> Path:
    candidates = sorted(LOG_DIR.glob("debug_*.log*"), key=lambda p: p.stat().st_mtime)
    if not candidates:
        sys.exit(f"no debug_*.log* under {LOG_DIR}")
    return candidates[-1]


def balanced_json(text: str, start: int) -> tuple[str, int] | None:
    """Return (json_text, end_index_exclusive) for the JSON object starting at
    text[start] == '{'. Honors string escapes. Returns None if unbalanced."""
    if text[start] != "{":
        return None
    depth = 0
    in_str = False
    esc = False
    i = start
    while i < len(text):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                in_str = False
        else:
            if c == '"':
                in_str = True
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[start : i + 1], i + 1
        i += 1
    return None


def extract_payloads(text: str) -> list[dict]:
    """Pull every balanced JSON object printed after a `playload=` or
    `parse_json:` marker. Returns dicts that parse cleanly."""
    out: list[dict] = []
    for marker in ("merged playload=", "parse_json:"):
        pos = 0
        while True:
            idx = text.find(marker, pos)
            if idx < 0:
                break
            brace = text.find("{", idx, idx + 4096)
            if brace < 0:
                pos = idx + len(marker)
                continue
            blob = balanced_json(text, brace)
            pos = idx + len(marker)
            if not blob:
                continue
            try:
                out.append(json.loads(blob[0]))
            except json.JSONDecodeError:
                # Most "merged playload" lines are truncated by the logger and
                # won't parse. That's expected; we still want any complete ones.
                pass
    return out


def extract_nozzle_info(payloads: list[dict]) -> list[dict]:
    """Find every distinct nozzle.info[] dump."""
    # Walk every payload looking for `nozzle` keys (the H2C path is
    # /print/device/nozzle, but the parser also accepts top-level / device
    # paths so we don't pin to one).
    seen: dict[str, dict] = {}

    def visit(o: object) -> None:
        if isinstance(o, dict):
            for k, v in o.items():
                if k == "nozzle" and isinstance(v, dict) and "info" in v:
                    key = json.dumps(v, sort_keys=True)
                    if key not in seen:
                        seen[key] = v
                visit(v)
        elif isinstance(o, list):
            for v in o:
                visit(v)

    for p in payloads:
        visit(p)
    return list(seen.values())


def parse_nozzle_id(raw_id: int) -> dict:
    """Reproduce DevUtil::get_hex_bits() for the id field — bit 0 = slot id,
    bit 1 = in_rack flag."""
    nozzle_id = (raw_id >> 0) & 0xF
    in_rack = (raw_id >> 4) & 0xF
    return {"raw": hex(raw_id), "slot_id": nozzle_id, "in_rack": in_rack == 1}


def parse_nozzle_type(type_str: str) -> tuple[str, str]:
    """Mirror DevNozzleSystem.cpp:s_parse_nozzle_type. Returns (flow, kind).
    HS01 -> ("S"=Standard, "01"=HardenedSteel). HS00 same flow, different kind."""
    if not type_str or len(type_str) < 4:
        return ("?", "?")
    flow_map = {"S": "Standard", "H": "HighFlow", "F": "FullFeature"}
    kind_map = {"01": "HardenedSteel", "02": "StainlessSteel", "03": "TungstenCarbide", "00": "Generic"}
    return (flow_map.get(type_str[1], type_str[1]), kind_map.get(type_str[2:4], type_str[2:4]))


def render_nozzle(nozzle: dict) -> str:
    lines = []
    if "exist" in nozzle:
        lines.append(f"  exist (16-bit) = {nozzle['exist']:#06x}")
    if "state" in nozzle:
        s = nozzle["state"]
        lines.append(
            f"  state = {s:#06x}  -> state_0_4={s & 0xF} reading_count={(s>>4)&0xF} reading_idx={(s>>8)&0xF}"
        )
    info = nozzle.get("info", [])
    lines.append(f"  info[] = {len(info)} entries")
    for item in info:
        bits = parse_nozzle_id(int(item.get("id", 0)))
        loc = "RACK" if bits["in_rack"] else "EXT "
        diameter = item.get("diameter", "?")
        ntype = item.get("type", "?")
        flow, kind = parse_nozzle_type(str(ntype))
        stat = item.get("stat", "")
        # Per DevNozzle::AtRightExtruder: rack nozzles ALWAYS map to right
        # extruder; mounted nozzles use slot id (0=right MAIN, 1=left DEPUTY).
        if bits["in_rack"]:
            ext = "R"
        else:
            ext = "R" if bits["slot_id"] == 0 else "L"
        lines.append(
            f"    {loc} slot={bits['slot_id']} ext={ext} raw={bits['raw']} dia={diameter} flow={flow} kind={kind} stat={stat}"
        )

    # Compute what `sync_machine_nozzle_inventory_to_preset` would write to
    # PresetBundle::extruder_nozzle_stat. Mirrors DevNozzleSystem.cpp:506-510.
    # Note: candidate is keyed by (extruder_id, volume_type) ONLY — no diameter
    # awareness. With multiple diameters of same flow type per extruder, the
    # last group's count overwrites earlier ones (THIS IS THE 0,0,0,0 BUG).
    groups: list[tuple[int, str, float, int]] = []  # (ext, flow, dia, count)

    def emplace(ext: int, flow: str, dia: float, count: int) -> None:
        for i, g in enumerate(groups):
            if g[0] == ext and g[1] == flow and abs(g[2] - dia) < 1e-6:
                groups[i] = (g[0], g[1], g[2], g[3] + count)
                return
        groups.append((ext, flow, dia, count))

    # First pass: mounted nozzles (one per extruder)
    for item in info:
        bits = parse_nozzle_id(int(item.get("id", 0)))
        if bits["in_rack"]:
            continue
        flow, _ = parse_nozzle_type(str(item.get("type", "")))
        ext = 0 if bits["slot_id"] != 0 else 1  # 0=L, 1=R per LOGIC ids
        # Actually DevNozzle::AtLeftExtruder: m_nozzle_id == DEPUTY(1) -> left
        # DevNozzle::AtRightExtruder: m_nozzle_id == MAIN(0) -> right
        # LOGIC_L_EXTRUDER_ID=0, LOGIC_R_EXTRUDER_ID=1
        ext = 0 if bits["slot_id"] == 1 else 1  # mounted slot 1 -> L (logic 0); slot 0 -> R (logic 1)
        emplace(ext, flow, float(item.get("diameter", 0)), 1)
    # Second pass: rack nozzles (always map to right = logic 1)
    for item in info:
        bits = parse_nozzle_id(int(item.get("id", 0)))
        if not bits["in_rack"]:
            continue
        flow, _ = parse_nozzle_type(str(item.get("type", "")))
        emplace(1, flow, float(item.get("diameter", 0)), 1)

    lines.append("")
    lines.append("  Aggregated nozzle_groups (matches DevNozzleSystem::GetNozzleGroups):")
    for g in groups:
        lines.append(f"    ext={g[0]} flow={g[1]} dia={g[2]} count={g[3]}")

    # Predict the candidate map (ext -> {flow: count}) under current bug:
    candidate_buggy: dict[int, dict[str, int]] = {}
    for ext, flow, _dia, count in groups:
        candidate_buggy.setdefault(ext, {})[flow] = count  # = NOT +=
    lines.append("")
    lines.append("  Predicted CURRENT extruder_nozzle_stats (bug: overwrites by flow only):")
    for ext in sorted(candidate_buggy):
        parts = ",".join(f"{f}#{c}" for f, c in candidate_buggy[ext].items())
        lines.append(f"    ext={ext}: {parts or '(empty)'}")

    # Predict the FIXED candidate (sum over diameters per flow):
    candidate_fixed: dict[int, dict[str, int]] = {}
    for ext, flow, _dia, count in groups:
        d = candidate_fixed.setdefault(ext, {})
        d[flow] = d.get(flow, 0) + count
    lines.append("")
    lines.append("  Predicted FIXED extruder_nozzle_stats (sum across diameters):")
    for ext in sorted(candidate_fixed):
        parts = ",".join(f"{f}#{c}" for f, c in candidate_fixed[ext].items())
        lines.append(f"    ext={ext}: {parts or '(empty)'}")

    return "\n".join(lines)


def grep_lines(text: str, pattern: str) -> list[str]:
    rx = re.compile(pattern)
    return [ln for ln in text.splitlines() if rx.search(ln)]


def section_payload(text: str) -> None:
    payloads = extract_payloads(text)
    print(f"== Payloads (parsed) ==  count={len(payloads)}")
    for p in payloads[:3]:
        print(json.dumps(p, indent=2)[:2000])
        print("---")


def section_nozzle(text: str) -> None:
    payloads = extract_payloads(text)
    nozzles = extract_nozzle_info(payloads)
    print(f"== Distinct nozzle.info dumps ==  unique={len(nozzles)}")
    if not nozzles:
        print("  (none parsed — check if log was truncated by 'merged playload=' wrapper)")
        return
    for n in nozzles:
        print(render_nozzle(n))
        print()


def section_sync(text: str, last: int | None) -> None:
    rows = grep_lines(text, r"sync_machine_nozzle_inventory")
    if last:
        rows = rows[-last:]
    print(f"== sync_machine_nozzle_inventory log lines ==  shown={len(rows)}")
    for ln in rows:
        print(ln.strip())


def section_heal(text: str, last: int | None) -> None:
    rows = grep_lines(text, r"on_printer_model_change|stale extruder_nozzle_stats|re-deriv|self.?heal")
    if last:
        rows = rows[-last:]
    print(f"== self-heal / printer-model-change events ==  shown={len(rows)}")
    counts = Counter(re.sub(r"^\[.*?\]", "", re.sub(r"^\S+ \S+", "", ln)).strip() for ln in rows)
    for msg, ct in counts.most_common():
        print(f"  x{ct}  {msg[:160]}")


def section_summary(text: str) -> None:
    print("== Summary ==")
    payloads = extract_payloads(text)
    nozzles = extract_nozzle_info(payloads)
    print(f"  parsed payloads: {len(payloads)}    distinct nozzle dumps: {len(nozzles)}")

    def count(p: str) -> int:
        return len(grep_lines(text, p))

    print(f"  sync_machine_nozzle_inventory events:    {count('sync_machine_nozzle_inventory')}")
    print(f"  on_printer_model_change resets:          {count('on_printer_model_change: reset')}")
    print(f"  self-heal (re-deriving) triggered:       {count('re-deriving')}")
    print(f"  extruder_nozzle_stats lines (general):   {count('extruder_nozzle_stat')}")
    print()
    if nozzles:
        print("  Latest nozzle.info[]:")
        print(render_nozzle(nozzles[-1]))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="path to debug_*.log; defaults to most recent")
    ap.add_argument("--section", choices=["summary", "nozzle", "sync", "heal", "payload"], default="summary")
    ap.add_argument("--last", type=int, default=None, help="limit to last N matches")
    args = ap.parse_args()

    log_path = Path(args.log) if args.log else latest_log()
    if not log_path.exists():
        sys.exit(f"missing log: {log_path}")
    print(f"# log: {log_path}  ({log_path.stat().st_size // 1024} KiB)")
    text = log_path.read_text(errors="replace")

    if args.section == "summary":
        section_summary(text)
    elif args.section == "nozzle":
        section_nozzle(text)
    elif args.section == "sync":
        section_sync(text, args.last)
    elif args.section == "heal":
        section_heal(text, args.last)
    elif args.section == "payload":
        section_payload(text)


if __name__ == "__main__":
    main()
