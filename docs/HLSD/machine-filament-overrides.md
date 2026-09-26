# Machine-owned filament tool-change settings

The printer setting `machine_filament_overrides` controls whether machine-owned
ramming and filament-change settings replace the selected filaments' values.
The checkbox is in Printer Settings > Multimaterial. It defaults to off, including
when loading an older printer preset or project without this setting.

Supported settings have a `machine_` prefix on their existing filament option
name. The supported set covers ramming, loading and unloading speeds, cooling
moves, stamping, tool-change delay, multi-tool ramming, minimum wipe-tower purge,
filament start/end G-code, and automatic pressure-advance emission. Their types and numeric bounds follow
the underlying filament options. Each machine override is an array:

- An empty array, or an omitted setting, leaves the filament value in effect.
- One element replaces that setting for every filament, regardless of brand,
  material type, or tool mapping.
- `[""]` explicitly clears a filament G-code script.
- Multiple elements are rejected; these are uniform machine values, not another
  per-filament configuration table.

Both preset-composition paths apply these values after combining the selected
presets. FDM normalization also applies them to directly loaded configurations,
before wipe-tower planning and G-code generation. The filament presets themselves
are not modified. Disabling the checkbox and recomposing the configuration uses
their original settings again. The printer options use the normal preset and
project serialization paths; no material-name matching or custom migration is
needed for existing presets with the feature disabled.

## Prusa MMU3

`fdm_machine_common_nextruder` supplies shared ramming and loading/cooling
values with the override disabled. Ordinary MK4, MK4S and CORE One printers
inherit this base without applying its filament values. MMU3 printers inherit
their ordinary printer family and enable the override, retaining their own
MMU startup, shutdown and tool-change settings.

The common values use the existing CORE One MMU3 PLA ramming and loading/cooling
sequence as the uniform machine baseline. Filament-start G-code and automatic
pressure-advance settings remain under each material's control. Filament-end
G-code is cleared. CORE One materials share ordinary presets with MMU3;
migration aliases resolve the former MMU3 copies, including PA and PC to the
Prusa generics. PCTG uses the unchanged system generic without a migration alias. Those replacements use their
ordinary material tuning. Disabling the override uses the surviving material
preset's handling settings. MMU3 purge volumes remain material-specific.

MMU process presets inherit ordinary MK4 or CORE One processes and retain
explicit differences for MMU printing. Thin preset wrappers preserve existing
names, compatibility, defaults and wipe-tower tuning.

The MK4 MMU3 standard 0.4, standard 0.6 and HF 0.4 machines and their default SPEED
processes are translated from [PrusaResearch 2.5.10](https://github.com/prusa3d/PrusaSlicer-settings-prusa-fff/blob/65c5c8f1e1c3836f306119c49d717759cbc368db/PrusaResearch/2.5.10.ini).
They retain the source's 250 x 210 x 220 mm volume, MMU setup and parking distances,
firmware checks, mesh leveling, purge line and shutdown sequence. Five source MMU
slots become one physical nozzle in Orca. The default materials are existing
OrcaFilamentLibrary presets; no MK4 MMU3-specific filament copies are required.
Purge-volume calculation uses Orca's existing filament/flush settings rather than
Prusa's `multimaterial_purging` option. Source process settings without an Orca
equivalent are not added as inert JSON keys.

The common MMU3 configuration deliberately overrides material-specific ramming
and swap scripts. It is not a claim that one physical tune has been qualified for
every filament formulation.

## Prusa INDX and XL

INDX also inherits the ordinary CORE One HF printer, but explicitly clears the
shared MMU ramming-curve override. It enables its own overrides for loading, cooling, multi-tool
ramming and minimum purge settings. Its cleaning-station start and tool-change
scripts read `filament_minimal_purge_on_wipe_tower`, so the machine fixes that
value at the source profile's 10 mm³ for every material. No other purge/prime
option is overridden. The common filament-start script emits `M572` using the
selected material's `pressure_advance`, followed by the firmware restore command
`M573 R`. Material presets retain their different pressure advance, temperatures,
flow limits and cooling values.

XL and XL 5T share material presets. Their material-dependent multi-tool ramming
values remain filament settings, including disabled ramming for FLEX and the
larger volume for Prusament PETG; no uniform XL override replaces those values.
