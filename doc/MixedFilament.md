# Mixed Filament

Ported from [OrcaSlicer-FullSpectrum](https://github.com/SoftFever/OrcaSlicer-FullSpectrum)
with contributions from Rad, Justin Hayes, Calogero Guagenti, xSil3nt, and ratdoux.

---

## User Guide

### What It Does

Mixed Filament lets a single virtual filament slot alternate between two
physical filaments across layers (or within a layer in Pointillisme mode),
producing blended or gradient-like colours on single-extruder printers.

### Enabling

1. Load a multi-colour (multi-extruder) profile with at least 2 filaments.
2. The **Mixed Filaments** panel appears automatically in the right-hand sidebar
   when 2 or more filaments are configured.
3. Each auto-generated row represents one pair of physical filaments.
   Toggle a row to enable it; the total filament count grows to include the
   virtual slot.

### Sidebar Workflow

- **Add** — creates a custom row for the same pair or a different ratio.
- **Edit** — opens `MixedFilamentConfigPanel` to adjust ratio, pattern,
  surface offset (bias), and distribution mode.
- **Delete** — marks the row deleted; existing painted geometry retains its
  virtual filament ID until you re-slice or repaint.

Painting with a virtual filament ID behaves the same as painting with a
physical one — use the Multi-Material Painting gizmo and select the virtual
slot from the colour palette.

### Color Match Dialog

Open via the colour swatch on a mixed row. The dialog (`MixedFilamentColorMatchDialog`)
shows a live preview strip of the blended result and lets you adjust ratio
until the preview matches your target colour. The preview accounts for
surface-offset bias when enabled.

### Anti-Banding Options

| Setting | What it does |
|---|---|
| `mixed_filament_advanced_dithering` | Uses an ordered dither pattern instead of simple A-then-B runs. Reduces stripe visibility on some hue pairs. More experimental than the default. |
| `dithering_local_z_mode` | Splits each blended layer into two sub-layers whose heights are proportional to the mix ratio (e.g. 66/33 at 0.12 mm → 0.08 mm + 0.04 mm). Produces the smoothest colour gradients. |
| `dithering_local_z_whole_objects` | Extends Local-Z splitting beyond painted masks to cover the entire object cross-section. Useful when mixed walls surround a painted zone. |
| `dithering_local_z_direct_multicolor` | For rows with 3 or more physical components, allocates Local-Z sub-layers directly across all components with carry-over error correction instead of collapsing to pair cadence. More toolchanges; less banding. |

### Gotchas

- **Single-extruder warning** — Mixed Filament requires a physical toolchange
  between the two components. On a true single-nozzle printer this means a
  manual filament swap. Verify your printer profile supports `T0`/`T1` before
  using mixed slots in a production print.
- **Variable-layer interaction** — If Variable Layer Height is enabled, Local-Z
  sub-layer heights are recomputed per interval. The mix ratio is preserved but
  the absolute sub-layer heights change with the variable height. Review the
  layer preview after applying variable layers.
- **Custom sequence disabled** — OrcaSlicer's "custom toolchange sequence" is
  suppressed when mixed filaments are active (`PlateSettingsDialog`). The
  virtual-to-physical resolution must control toolchange order; a user-defined
  sequence would break it.
- **Stable IDs** — each mixed row carries a `stable_id` (64-bit). If you
  reorder or delete rows and then load an older project, the ID remap in
  `PresetBundle::update_mixed_filament_id_remap` translates painted geometry
  to the correct new virtual slot. Do not rely on the 1-based filament index
  as a stable identifier.

---

## Developer Guide

### Core Data Structures

```
src/libslic3r/MixedFilament.hpp   — MixedFilament struct, MixedFilamentManager
src/libslic3r/MixedFilament.cpp   — serialization, resolve(), auto_generate()
src/libslic3r/LocalZOrderOptimizer.hpp — bucket-ordering helpers for Local-Z
```

The key scalar fields on `MixedFilament`:

- `component_a`, `component_b` — 1-based physical filament indices.
- `ratio_a`, `ratio_b` — layer-alternation cadence numerators.
- `mix_b_percent` — nominal colour mix (used for Local-Z height computation
  and the Color Match preview; does not change the cadence).
- `stable_id` — monotonically increasing 64-bit ID assigned at construction.
  Never reused. Survives serialization round-trips.
- `distribution_mode` — selects between `Simple`, `SameLayerPointillisme`,
  and `GroupedManual`.

### Seam: Adding New Distribution Modes

`MixedFilamentManager::resolve()` in `MixedFilament.cpp` is the single
dispatch point that maps `(virtual_filament_id, num_physical, layer_index)`
to a physical extruder. The current switch covers `Simple` and
`SameLayerPointillisme`. A new mode is added by:

1. Adding a value to the `MixedFilament::DistributionMode` enum in
   `MixedFilament.hpp`.
2. Adding a `case` to `MixedFilamentManager::resolve()` in `MixedFilament.cpp`.
3. Serializing the new mode token in `serialize_custom_entries` /
   `load_custom_entries` (format is a semicolon-delimited row string; see
   existing tokens for the convention).

G-code emission (`src/libslic3r/GCode/`) reads only the physical ID returned
by `resolve()`, so new modes are automatically emitted without further changes.

### Seam: New Toolchange-Cost Heuristics

`LocalZOrderOptimizer` (`src/libslic3r/LocalZOrderOptimizer.hpp`) exposes:

- `order_bucket_extruders(bucket, current, preferred_last)` — reorders a
  single-layer bucket to minimise toolchanges given the current active extruder.
- `order_pass_group(group, current_extruder)` — greedy walk across a set of
  buckets (one per Local-Z sub-layer) to minimise total transitions.

To add a new heuristic (e.g. cost-based look-ahead), replace or wrap
`order_pass_group`. The caller in `PrintObjectSlice.cpp` passes the result
directly into the sub-layer plan, so the heuristic is fully decoupled from
the plan builder.

### Seam: New Picker Shapes in the Color Map Panel

`MixedFilamentColorMapPanel` (`src/slic3r/GUI/MixedFilamentColorMapPanel.hpp`)
renders a 2-D colour map using a set of geometry "types" (currently strip and
gradient). Each type is a small self-contained rendering path keyed by an enum
value. New shapes are added by:

1. Adding an enum value to `MixedFilamentColorMapPanel::GeometryType`.
2. Implementing the corresponding `Paint*` helper (follow `PaintStrip` as a
   template).
3. Wiring the new type into the `switch` in `OnPaint`.

### Persistence (3MF)

The entire mixed-filament state is stored as a single string key
`mixed_filament_definitions` in the project config block (section `[presets]`
in the 3MF metadata).

Round-trip path:

```
MixedFilamentManager::serialize_custom_entries()
  called by PresetBundle::sync_mixed_filaments_to_config()
  written by bbs_3mf: store_bbs_3mf → config.set("presets", "mixed_filament_definitions", ...)

load_bbs_3mf → config.get("presets", "mixed_filament_definitions")
  stored in project_config["mixed_filament_definitions"]
  read by PresetBundle::sync_mixed_filaments_from_config()
    → mixed_filaments.auto_generate(colours)
    → mixed_filaments.load_custom_entries(defs, colours)
```

Auto-generated rows are *not* written to the definitions string; they are
rebuilt from the filament colour list. Only `custom == true` rows are stored.

See `tests/fff_print/test_mixed_filament_e2e.cpp` for regression tests
covering this path.

### ID Remap

When filaments are added, removed, or reordered, virtual IDs shift.
`PresetBundle::update_mixed_filament_id_remap(old_mixed, old_count, new_count)`
produces a `remap` vector where `remap[old_virtual_id] = new_virtual_id`.
Painted triangle mesh face data uses these IDs; the remap is applied in
`TriangleSelectorMixed` after any filament list change.
