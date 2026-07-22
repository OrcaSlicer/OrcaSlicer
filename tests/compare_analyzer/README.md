# Compare Analyzer — G-code Slicing Comparison Tools

Tools for deep comparison and analysis of `.3mf` slicing project files, designed for
verifying multi-nozzle (H2C carousel) and multi-extruder slicing correctness.

## Tools

### `compare_slices.py` — Slice Comparison Analyzer

Deep comparison of two `.3mf` files (OrcaSlicer, BambuStudio, or any compatible slicer).
Generates a comprehensive Markdown report covering:

- **Filament usage** — per-filament weight/length with color mapping
- **Nozzle/extruder mapping** — Vortek carousel slot assignments
- **Tool change sequences** — T-code ordering and count
- **Prime tower analysis** — tower entries, G-code line count
- **Temperature timeline** — pre-heat lead times, target temperatures per tool change
- **Retract parameters** — M620.11 analysis during nozzle switches
- **Filament change G-code blocks** — line-by-line diff of change_filament_gcode
- **Control command diff** — timeline of M/G-code differences
- **Critical discrepancy detection** — automatic flagging of weight/time anomalies

#### Usage

```bash
# Compare two slice files
python3 compare_slices.py file1.3mf file2.3mf

# With custom labels
python3 compare_slices.py file1.3mf file2.3mf --labels "Upstream" "Fixed"
```

#### Output
Markdown report saved to `mp_reports/compare_report_YYYYMMDD_HHMMSS.md`

#### Example: Detecting H2C purge regression
```
⚠️ CRITICAL DISCREPANCY: Huge difference in part weight:
  OrcaSlicer 60.90 g vs BambuStudio 17.47 g (difference 43.43 g or 71.3%).
  The reason is incorrect nozzle mapping, causing huge AMS flushing.
```

---

### `show_temp_plot.py` — Temperature Timeline Plotter

Generates interactive HTML temperature plots for analyzing thermal profiles during
multi-nozzle prints. Visualizes heater temperature commands (M104/M109) per tool change,
showing pre-heat timing and temperature convergence.

#### Architecture
- H2C dual-extruder layout with Vortek carousel nozzles
- Physical heaters mapped dynamically:
  - Heater 0: Extruder 2 (right nozzle slot, T0/T2/T3/T4)
  - Heater 1: Extruder 1 (left nozzle slot, T1)
- Active heater mapping derived from G-code temperature signals

#### Usage

```bash
# Single file analysis
python3 show_temp_plot.py file.3mf

# Side-by-side comparison of two files
python3 show_temp_plot.py file1.3mf file2.3mf
```

#### Output
Interactive HTML report saved to Desktop as `temp_plot_v3.html`

---

## Requirements

- **Python 3.8+**
- **No external dependencies** — uses only Python standard library
  (`json`, `zipfile`, `xml.etree.ElementTree`, `difflib`, `webbrowser`)

## Use Cases

1. **Regression testing** — compare slices before/after code changes to verify
   no unintended differences in purge volumes, tool ordering, or temperature timing
2. **BBS compatibility verification** — compare OrcaSlicer output against BambuStudio
   reference slices to ensure behavioral parity
3. **H2C carousel validation** — verify per-slot nozzle tracking produces correct
   purge volumes (not collapsed per-extruder)
4. **Temperature protocol analysis** — verify pre-heat lead times and cooling
   temperatures during nozzle changes match expected profiles
