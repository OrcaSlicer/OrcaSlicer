#!/usr/bin/env python3
"""Render a stacked per-tool + residual resource chart from the build metrics CSV.

Expected columns (produced by Start-BuildMonitor in metrics.ps1):
  timestamp, build_cpu_percent, build_cores, build_rss_gb,
  cores_compiler, cores_linker, cores_build, cores_ccache,
  cpu_compiler, cpu_linker, cpu_build, cpu_ccache,
  ram_compiler, ram_linker, ram_build, ram_ccache

Layout
------
One Panel. Three metric blocks inside (CPU %, Cores, RAM). Each block is
a vertical bar chart: one column per sampled bucket, stacked bottom-up in
fixed tool order (compiler / linker / build / ccache) with a fifth "other"
series (total - sum(named)) stacked on top so the column height equals the
total for that bucket. A left-hand Y axis with exactly one unique label per
row and a baseline sits under each block. All blocks share the same gutter
width so they align vertically. One X axis and one legend at the bottom.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

try:
    from rich.console import Console, Group
    from rich.panel import Panel
    from rich.table import Table
    from rich.text import Text
except ImportError:
    print("This script requires 'rich': pip install rich", file=sys.stderr)
    sys.exit(2)


# ── palette ─────────────────────────────────────────────────────────────────
TOOL_ORDER = ("compiler", "linker", "build", "ccache")
TOOL_STYLE = {
    "compiler": "bright_green",
    "linker":   "bright_cyan",
    "build":    "bright_yellow",
    "ccache":   "magenta",
    "other":    "grey50",
}
TOOL_GLYPH = {
    "compiler": "█",
    "linker":   "█",
    "build":    "█",
    "ccache":   "█",
    "other":    "░",
}
TRACK_STYLE = "grey23"

# Height of each metric block (number of bar rows, including the top row).
HEIGHT_ROWS = 5


# ── data loading ────────────────────────────────────────────────────────────

@dataclass
class Series:
    t: list[str]
    cpu: list[float]
    cores: list[float]
    rss: list[float]
    per_tool: dict[str, dict[str, list[float]]]


def load(csv_path: Path) -> Series:
    t, cpu, cores, rss = [], [], [], []
    per_tool = {
        "cores": {k: [] for k in TOOL_ORDER},
        "cpu":   {k: [] for k in TOOL_ORDER},
        "rss":   {k: [] for k in TOOL_ORDER},
    }
    with csv_path.open(newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.DictReader(f):
            def fnum(key: str) -> float:
                try:
                    return float(row.get(key) or 0.0)
                except ValueError:
                    return 0.0
            t.append(row.get("timestamp", ""))
            cpu.append(fnum("build_cpu_percent"))
            cores.append(fnum("build_cores"))
            rss.append(fnum("build_rss_gb"))
            for tool in TOOL_ORDER:
                per_tool["cores"][tool].append(fnum(f"cores_{tool}"))
                per_tool["cpu"][tool].append(fnum(f"cpu_{tool}"))
                per_tool["rss"][tool].append(fnum(f"ram_{tool}"))
    return Series(t=t, cpu=cpu, cores=cores, rss=rss, per_tool=per_tool)


# ── helpers ─────────────────────────────────────────────────────────────────

def fmt_dur(t0: str, t1: str) -> str:
    try:
        secs = (datetime.fromisoformat(t1) - datetime.fromisoformat(t0)).total_seconds()
    except (ValueError, TypeError):
        return "?"
    m, s = divmod(int(secs), 60)
    return f"{m}m {s:02d}s"


def downsample(vals: list[float], n: int) -> list[float]:
    """Average buckets down to n columns."""
    if not vals or n <= 0:
        return []
    if len(vals) <= n:
        return vals
    step = len(vals) / n
    return [
        sum(vals[int(i * step):max(int(i * step) + 1, int((i + 1) * step))])
        / max(1, int((i + 1) * step) - int(i * step))
        for i in range(n)
    ]


def residual_series(named: dict[str, list[float]], totals: list[float]) -> list[float]:
    """Per-bucket `total - sum(named)`, clamped to >= 0."""
    out = []
    for i, tot in enumerate(totals):
        s = 0.0
        for v in named.values():
            if i < len(v):
                s += v[i]
        out.append(max(0.0, tot - s))
    return out


def fmt_tick(v: float, unit: str) -> str:
    if unit == "%":
        return f"{v:g}%"
    if unit == " GB":
        return f"{v:g}G"
    return f"{v:g}"


def axis_ticks(peak: float, rows: int, unit: str) -> list[str]:
    """Return exactly `rows` labels, one per bar row, top row = peak.

    Guarantees:
      * len(result) == rows
      * labels are unique
      * top label == peak (formatted)
      * bottom label == 0 (formatted)
    """
    if rows <= 0:
        return []
    if peak <= 0 or rows == 1:
        return [fmt_tick(peak if r == 0 else 0.0, unit) for r in range(rows)]

    # Build candidate values bottom-up: 0, ..., peak, aiming for `rows` entries.
    gaps = rows - 1
    raw_step = peak / gaps
    mag = 10 ** math.floor(math.log10(raw_step)) if raw_step > 0 else 1.0
    step = raw_step
    for m in (1, 2, 2.5, 5, 10):
        cand = m * mag
        if peak / cand <= gaps + 1e-9:
            step = cand
            break

    # Values for exactly `gaps` intervals, with the last snapped to `peak`.
    vals = [i * step for i in range(gaps)] + [peak]
    # If we somehow ended with duplicate formatted labels, bump precision.
    labels = [fmt_tick(v, unit) for v in vals]
    if len(set(labels)) != len(labels):
        # Increase the number of significant digits until unique.
        for prec in (1, 2, 3, 4):
            labels = [f"{v:.{prec}f}{unit}" if unit == " GB"
                      else (f"{v:.{prec}f}{unit}" if unit == "%" else f"{v:.{prec}f}")
                      for v in vals]
            if len(set(labels)) == len(labels):
                break
    # Return top-first: last value is peak, first is 0.
    return list(reversed(labels))


# ── drawing ─────────────────────────────────────────────────────────────────

def render_bars(series_by_tool: dict[str, list[float]],
                totals: list[float],
                n: int,
                peak: float,
                unit: str,
                gutter_width: int) -> Text:
    """One metric block: left Y axis + stacked bar grid + baseline."""
    peak = peak or 1.0

    cols = {tool: downsample(vals, n) for tool, vals in series_by_tool.items()}
    resid = residual_series(cols, downsample(totals, n))

    def cells(v: float) -> int:
        if v <= 0:
            return 0
        return max(1, int(round(v / peak * HEIGHT_ROWS)))

    grid = [[(" ", TRACK_STYLE) for _ in range(n)] for _ in range(HEIGHT_ROWS)]
    for c in range(n):
        row = HEIGHT_ROWS - 1
        for tool in TOOL_ORDER:
            vals = cols.get(tool) or []
            if c >= len(vals):
                continue
            for _ in range(cells(vals[c])):
                if row < 0:
                    break
                grid[row][c] = ("█", TOOL_STYLE[tool])
                row -= 1
        if c < len(resid):
            for _ in range(cells(resid[c])):
                if row < 0:
                    break
                grid[row][c] = ("░", TOOL_STYLE["other"])
                row -= 1

    labels = axis_ticks(peak, HEIGHT_ROWS, unit)   # rows labels, top first

    out = Text()
    for r in range(HEIGHT_ROWS):
        label = labels[r]
        pad = gutter_width - len(label) - 1
        if pad < 0:
            pad = 0
        out.append(" " * pad + label + " ┤", style="dim")
        for c in range(n):
            glyph, style = grid[r][c]
            out.append(glyph, style=style)
        if r != HEIGHT_ROWS - 1:
            out.append("\n")

    out.append("\n")
    out.append(" " * (gutter_width - 1) + " ┴", style="dim")
    out.append("─" * n, style="dim")
    return out


def render_block(label: str,
                 unit: str,
                 series_by_tool: dict[str, list[float]],
                 totals: list[float],
                 n: int,
                 peak: float,
                 precision: int,
                 gutter_width: int) -> Group:
    peak = peak or 1.0
    avg = (sum(totals) / len(totals)) if totals else 0.0

    header = Text()
    header.append(f"{label}  ", style="bold")
    header.append(f"peak {peak:.{precision}f}{unit}", style="dim")
    header.append("  ")
    header.append(f"avg {avg:.{precision}f}{unit}", style="dim")

    bars = render_bars(series_by_tool, totals, n, peak, unit, gutter_width)
    return Group(header, bars, Text(""))


def shared_x_axis(s: Series, n: int) -> Text:
    """One row of time labels, aligned to bar columns."""
    try:
        t0 = datetime.fromisoformat(s.t[0])
        t1 = datetime.fromisoformat(s.t[-1])
        dur = (t1 - t0).total_seconds()
    except (ValueError, TypeError):
        return Text("")

    def fmt(sec: float) -> str:
        sec = int(round(sec))
        m, s = divmod(sec, 60)
        return f"{m}m" if s == 0 and m else (f"{m}m{s:02d}s" if m else f"{s}s")

    positions = [0, n // 3, (2 * n) // 3, n - 1]
    fractions = [0.0, 1 / 3, 2 / 3, 1.0]
    labels = [fmt(dur * f) for f in fractions]

    out = Text()
    prev = 0
    for pos, lbl in zip(positions, labels):
        if pos <= prev:
            continue
        gap = pos - prev
        if gap < len(lbl) + 1:
            out.append(" " * gap)
        else:
            out.append(" " * (gap - len(lbl)))
            out.append(lbl, style="dim")
        prev = pos
    if prev < n:
        out.append(" " * (n - prev))
    return out


# ── assembly ────────────────────────────────────────────────────────────────

def render(csv_path: Path, title: str) -> Panel:
    s = load(csv_path)
    if not s.t:
        return Panel("No samples collected.", title=title, border_style="cyan")

    console = Console()
    inner_width = max(60, console.size.width - 4)
    gutter_width = 7
    n = max(20, inner_width - gutter_width - 2)

    cpu_peak  = max(s.cpu)   or 1.0
    core_peak = max(s.cores) or 1.0
    rss_peak  = max(s.rss)   or 1.0

    blocks = [
        render_block("CPU %", "%",   s.per_tool["cpu"],   s.cpu,   n, cpu_peak,  0, gutter_width),
        render_block("Cores", "",    s.per_tool["cores"], s.cores, n, core_peak, 2, gutter_width),
        render_block("RAM",   " GB", s.per_tool["rss"],   s.rss,   n, rss_peak,  1, gutter_width),
    ]

    x_axis = shared_x_axis(s, n)

    legend = Text()
    legend.append("stacked: ", style="dim")
    entries = [(t, TOOL_STYLE[t], TOOL_GLYPH[t]) for t in TOOL_ORDER] + [
        ("other", TOOL_STYLE["other"], TOOL_GLYPH["other"])
    ]
    for i, (name, style, glyph) in enumerate(entries):
        if i:
            legend.append("  ")
        legend.append(f"{glyph} ", style=style)
        legend.append(name)

    header = Text()
    header.append(f"{fmt_dur(s.t[0], s.t[-1])}", style="dim")
    header.append(f"  ·  {len(s.t)} samples", style="dim")

    table = Table.grid(padding=0, expand=True)
    table.add_column()
    table.add_row(header)
    table.add_row(Text(""))
    for b in blocks:
        table.add_row(b)
    table.add_row(x_axis)
    table.add_row(legend)

    return Panel(table, title=title, border_style="cyan")


# ── CLI ─────────────────────────────────────────────────────────────────────

def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv", type=Path)
    p.add_argument("--title", default="Build resource usage")
    args = p.parse_args()

    if not args.csv.is_file():
        print(f"Metrics CSV not found: {args.csv}", file=sys.stderr)
        return 1

    Console().print(render(args.csv, args.title))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())