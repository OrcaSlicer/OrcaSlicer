# Prusa CORE One INDX profiles

The Prusa bundle provides separate four-tool and eight-tool CORE One INDX models
with 0.4 mm high-flow nozzles and a 248 x 205 x 270 mm printable volume. These are
independent-tool printers, not single-extruder MMU3 machines. Both inherit
`fdm_machine_common_coreone_indx`, which inherits the ordinary CORE One HF
printer; the concrete printer presets supply their tool-count-dependent arrays. The bed and wizard resources come from Prusa.

The source configuration is PrusaResearch 2.5.10 from
[Prusa's profile repository](https://github.com/prusa3d/PrusaSlicer-settings-prusa-fff/blob/65c5c8f1e1c3836f306119c49d717759cbc368db/PrusaResearch/2.5.10.ini).
Its printer, process and filament settings are translated to Orca option names.
The supplied process presets cover 0.10, 0.15, 0.20 and 0.25 mm layers. Their
shared `fdm_process_coreone_indx` base inherits the ordinary CORE One HF SPEED
process, with explicit INDX differences and layer-specific child settings.
Material presets are shared with the ordinary CORE One family.

## Tool changes and extrusion state

The start script initializes a persistent `tool_init` vector, counts the tools
used by the job, homes and probes using a loaded tool, calibrates the used tools,
and primes the initial tool at the cleaning station. The change script preserves
Prusa's `G27`, `P0`, `T`, `G12`, `G750` and `M906` sequences. It updates
`e_retracted` so Orca's subsequent unretraction agrees with the script's extrusion.
Per-tool temperature commands are guarded by `is_extruder_used`.

The off-bed purge station is the default. With a prime tower enabled, a newly
used tool still receives its initial station purge, while an initialized tool
uses the source's wipe-tower preparation path. `tool_init` persists across these
changes; treating every tool selection as first use would change the purge and
deretraction sequence.

Purge volume uses the filament's minimal purge setting. Orca's optional flush
volumetric speed is divided by filament cross-sectional area before it is used
as a linear extrusion-speed override. Prusa's separate `filament_flush_volume`
override is not available in these profiles. The source's `EXCLUDE_E_START` and
`EXCLUDE_E_END` internal markers become comments rather than printer commands.

Filament-start G-code restores pressure advance with `M572` and the INDX `M573 R`
command after station purging disables pressure advance. Orca's separate automatic
pressure-advance emission is disabled by the INDX machine override. Dock-fan control
retains the source's material and layer conditions; shutdown parks the tool and
turns off the used heaters and dock fan.

## Configuration boundaries

The scripts use Orca's temperature, retraction, fan and speed option names. The
nozzle-check high-flow flag is fixed because these presets describe HF nozzles;
the abrasive-material flag is derived from the filament's required nozzle HRC.
An unset idle temperature uses Orca's zero sentinel. ABS's source XY shrinkage
compensation is represented using Orca's retained-size percentage.

Prusa's consistent-surface cooling strategy and filament-specific infill crossing
speed limits have no direct Orca profile equivalent. These presets use Orca's
native layer-time cooling and material volumetric limits. They do not add slicer
features or change existing Prusa printer profiles.
