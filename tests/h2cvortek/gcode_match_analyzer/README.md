# G-code Match & Correctness Analyzer for H2C Printers

This tool is designed to analyze the slicing quality of G-code files generated for Bambu Lab H2C printers and compare the output of Bambu Studio and OrcaSlicer.

## Features

1. **Single File Analysis (Correctness Mode)**:
   - Verifies layer geometry (Z-height alignment).
   - Validates tool retraction values (expects `M620.11 O1 T18` for PLA and `T14` for PETG-CF).
   - Checks pre-cooling hotend addressing (verifies that `M104` cools the parked nozzle instead of the active one).
   - Detects duplicate/conflicting `M104` temperature commands in a single toolchange block.
   - Monitors the pre-cooling temperature floor (fails if pre-cooling drops below 150°C).
   - Tracks purging (flushing) volumes per tool based on `M620.10` commands.
   - Verifies extruder change long retractions (`M620.11 K1 R10` to avoid FTS sensor jams).
   - Validates temperature scheduler command pairing/balancing (`M632`/`M633`).
   - Assesses PETG interface tower oozing compensation configs (`filament_tower_interface_pre_extrusion_length`).

2. **Dual File Comparison (Match Mode)**:
   - Automatically detects logical tool swapping (e.g. BBS T0 $\leftrightarrow$ Orca T4).
   - Compares G-code command distributions (G1, G2, G3, M104, M204, M73, etc.).
   - Matches Z-heights and line counts layer-by-layer.
   - Compares filament consumption by printing feature type (Feature Extrusion Profile).
   - Compares flushing volumes across logically mapped tool channels.
   - Performs command-by-command analysis of logically aligned toolchange blocks.

---

## Usage

The script is written in pure Python 3 and has no external dependencies.

### 1. Print Report to Console (Default)

If you don't specify the `--to-file` flag, the report is printed directly to the standard output (console):

```bash
# Analyze a single file for correctness
python3 analyzer.py -f1 /path/to/my_slice.gcode

# Compare two slices
python3 analyzer.py -f1 /path/to/10bbl.gcode -f2 /path/to/16orca.gcode
```

### 2. Save Reports to Files (`--to-file`)

When the `--to-file` flag is provided, the tool saves the human-readable text report (`.txt`) and structured statistics (`.json` for agent analysis):

```bash
# Save report for a single file
python3 analyzer.py -f1 /path/to/my_slice.gcode --to-file

# Save comparison report for two files
python3 analyzer.py -f1 /path/to/10bbl.gcode -f2 /path/to/16orca.gcode --to-file
```

### Command Line Arguments
* `-f1` / `--file1`: Path to the first G-code file (required).
* `-f2` / `--file2`: Path to the second G-code file for comparison (optional).
* `--to-file`: Flag to write output to files instead of console (optional).
* `-o` / `--output`: Output file prefix (optional, active only when `--to-file` is set; defaults to Desktop or current directory).
