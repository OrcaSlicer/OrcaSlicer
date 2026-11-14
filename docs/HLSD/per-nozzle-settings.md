# Per-nozzle settings — High Level Design

## Purpose and scope

On printers with several extruders, or with extruders that take different
nozzles, some settings must differ per nozzle: speeds and accelerations depend
on the nozzle's flow, retraction on the drive, and a filament's flow ratio or
temperature on both. These settings are stored once per *nozzle variant* instead
of once per preset. This document covers how the variants are stored, how a
value is found for an extruder, how the settings tabs edit them, and how the copy
button moves values between extruders or variants.

## Variants and columns

A variant is the pair of an extruder type and a nozzle volume type, written as
one string such as "Direct Drive Standard" or "Direct Drive High Flow". The
printer's `extruder_variant_list` holds, per extruder, the comma-separated
variants that extruder can take. Which one is in use comes from the project's
`nozzle_volume_type`, one entry per extruder, set in the sidebar. A Hybrid
extruder carries both Standard and High Flow nozzles, and each filament's own
volume type picks between them.

Each per-variant setting is a vector with one value per *column*. Two parallel
vectors describe the columns of a preset:

| Scope    | Settings                                          | Column keys                                       | Values per column |
|----------|---------------------------------------------------|---------------------------------------------------|-------------------|
| Process  | `print_options_with_variant`                      | `print_extruder_id`, `print_extruder_variant`     | 1                 |
| Printer  | `printer_options_with_variant_1`                  | `printer_extruder_id`, `printer_extruder_variant` | 1                 |
| Printer  | `printer_options_with_variant_2` (machine limits) | `printer_extruder_id`, `printer_extruder_variant` | 2: normal, silent |
| Filament | `filament_options_with_variant`                   | `filament_extruder_variant`                       | 1                 |

A process or printer column belongs to one extruder (the id, 1-based) and one
variant. A filament column belongs only to a variant, and every extruder of that
variant shares it. Printer settings outside these sets that are still vectors,
such as `nozzle_diameter`, `extruder_type` or `extruder_offset`, have one value
per extruder instead.

`DynamicPrintConfig::get_index_for_extruder` finds the column of an extruder:
the first whose id matches and whose variant string equals the extruder's type
and nozzle volume type, Hybrid counting as Standard. Given a stride, it returns
the position of the column's first value, and -1 when there is no such column.

## Loading, sizing and slicing

Profiles may store fewer values than columns, often a single value meant for
every column. `extend_default_config_length` pads every per-variant setting to
its column count with its first value whenever a preset is loaded: system, user,
cloud and project presets alike. User presets start from their parent's padded
vectors and apply their own values per variant. After loading, every column has
a value of its own, so editing one column never changes another.

The printer's columns follow its extruder count. `set_num_extruders` rebuilds
`printer_extruder_id` and `printer_extruder_variant` from `extruder_variant_list`
and resizes the per-variant and per-extruder printer settings, padding new
entries with the first value.

A process ships with columns for the tools of its profile. Raising the printer's
extruder count does not give it columns for the new tools, so
`add_process_columns_for_new_tools` adds one per variant of each such tool,
copied from tool 1's column of the same variant (the first column when tool 1
has no such variant). The settings tabs run it in memory on every copy the
edited process is compared with: the selected preset, its parent, the edited
preset and, when it was clean, the saved baseline. The added columns therefore
never show as changes. A process with a single column is left alone: its one
value applies to every tool, and splitting it would make an edit reach the first
tool only.

For slicing, `update_values_to_printer_extruders` reduces each per-variant
setting to one value per slot, a slot being an extruder and a nozzle volume type
it prints with. A slot without a column of its own reads the first column. A
process that still has one column is given a column per extruder variant first
(`ensure_process_variant_columns`), all holding that value.

## Editing in the settings tabs

The process and printer tabs have an extruder switch, the filament tab a variant
switch. Its tags name the selection as the user sees it: "T2 HF" on the process
tab, "Extruder 2: HF" on the printer tab, "Direct Drive HF" on the filament tab,
and the left and right nozzle names on Bambu dual-nozzle printers.
`switch_excluder` points every per-variant field of the pages at the selected
column. On the printer's Extruder page, the fields of per-extruder settings point
at the selected extruder instead. Groups holding such fields draw the per-nozzle
icon next to them.

## Copying between extruders and variants

The copy button next to the switch copies the values modified on the current
page, for the selected extruder or variant, to the others. What a value may be
copied to depends on the page:

- On the process tab and the printer's Motion ability page, to every extruder
  with the same extruder type whose nozzle has the selected nozzle volume type,
  or is Hybrid. Values go into that extruder's column of the same variant.
- On the printer's Extruder page, per-extruder settings go to every other
  extruder. Per-variant settings follow the rule above, so an extruder with
  another nozzle volume type gets only the per-extruder ones.
- On the filament tab, to the filament's other variants.

Values are never copied across nozzle volume types on the process and printer
tabs: Standard and High Flow values are tuned separately, and a copy would
overwrite one with the other.

The copy is confirmed in the unsaved changes dialog in a copy mode. It shows one
column per extruder or variant, titled with the switch tags, and one row per
setting. Each setting carries its own position in every column, because one page
can mix per-variant and per-extruder settings. A cell whose value would change
has a checkbox, ticked by default. A cell without a value of that setting stays
empty. Tick-all boxes cover each row, group, category and column, and one covers
everything. Only the ticked values are written.

## Implementation and verification

The column handling lives in `PrintConfig.cpp` (`get_index_for_extruder`,
`update_values_to_printer_extruders`, `set_num_extruders`) and `Preset.cpp`
(`extend_default_config_length`). `add_process_columns_for_new_tools` is in
`ParameterUtils.cpp`. The switches, the copy button and the in-memory
completion are in `Tab.cpp`, the dialog in `UnsavedChangesDialog.cpp`.

`tests/libslic3r/test_parameter_utils.cpp` covers the columns added for new
tools, and `tests/libslic3r/test_config_variant_expansion.cpp` the slicing
expansion.
