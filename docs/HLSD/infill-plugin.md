# Infill Plugins — High-Level Design

## Purpose

Infill plugins provide infill patterns for the four plugin-backed pattern settings — sparse,
internal solid, top surface and bottom surface — while retaining OrcaSlicer's native fill lifecycle. A
plugin returns open paths for one surface island; the host remains responsible for extrusion conversion,
flow, gap fill, path ordering, and G-code generation.

## Capability identity and project references

One infill capability is a provider identity and may advertise several selectable patterns. Each
pattern has a stable, nonlocalized ID that is the value stored in a pattern setting, plus a
presentation-only display name and the set of surface purposes it can fill (`orca.infill.PatternDescriptor`).
A pattern is selectable only in the settings whose purpose it declares (`Sparse`, `InternalSolid`,
`TopSurface`, `BottomSurface`); one descriptor may support several purposes. Pattern IDs are unique
within the capability, across loaded capabilities, and against built-in pattern keys. A capability's
name remains the provider identity used in the project manifest.

Saving a preset or project records the owning capability's full plugin reference, including local or
cloud plugin identity, for every plugin-backed pattern setting that holds a plugin ID. Slicing resolves
each selected pattern ID to its registry entry and requires that entry's owning capability to match the
project reference exactly and to support the purpose of the setting that selected it. It does not
substitute another loaded provider with the same pattern ID, it does not dispatch the pattern for a
purpose it did not declare, and it does not fall back to a built-in pattern. Existing built-in setting
values remain unchanged.

## Slicing path

Every pattern with a canonical config key is created through `InfillFactory` by that single string ID.
InfillFactory registers built-in factories lazily from one explicit metadata switch that owns each
built-in pattern's config key, display label and supported surfaces. Plugin factories are registered
when an enabled infill capability loads and removed when it is disabled or unloaded. The registry entry
carries the owning plugin and capability, so creation never needs a provider reference from its caller.

Before infill enters TBB parallel work, `PrintObject` collects the plugin IDs stored in all four
pattern settings with the purpose each setting drives, validates each against the project manifest and
the registry entry's supported surfaces, then pins the registry entry for the duration of the infill
job. A pinned entry cannot be replaced or reused by another provider: unloading the plugin retires it
for new selections while the already-validated job may still try to create its Fill. A missing,
disabled, unloaded, differently owned, or purpose-mismatched provider fails the slice before any fill
worker runs.

A pin is also a provider lifecycle lease: while a job holds one, the registry keeps the exact provider
instance (and therefore its Python module) alive. An unload request therefore drains pinned infill
providers without a deadline instead of force-unloading them, so the slicing worker never runs Python
against a module that was torn down. A capability re-enabled while its entry is pinned revives that
entry and keeps the pin. Pins are released when infill finishes, and on every failure or cancellation
path in `prepare_infill()` and `infill()`.

Each use creates a fresh `Fill` adapter that holds the provider alive. The adapter delegates per-island
path generation to the capability with the selected pattern ID. Python implementations execute through
the audited GIL trampoline; native implementations must be thread-safe because separate surface
generations can run concurrently. The adapter forwards supported fill semantics such as path sorting
and origin alignment. The per-call surface purpose is carried explicitly from the setting that selected
the pattern; a plugin never infers it from density, extrusion role or pattern ID.

The Python trampoline accepts both the current override signatures (`generate(pattern_id, request)`,
`no_sort(pattern_id)`, `aligned_to_origin(pattern_id)`) and the pre-multi-pattern forms
(`generate(request)`, `no_sort()`, `aligned_to_origin()`). It selects the form by asking
`inspect.signature` whether the callable binds the current argument count, so a `TypeError` raised
inside plugin logic is never treated as a signature mismatch and the plugin is only called once.
`*args` overrides are treated as the current form; a callable whose signature cannot be inspected
falls back to the current form, and the normal call then reports any mismatch.

The request is a value snapshot in the host's scaled-coordinate convention. Plugin paths pass through
the ordinary Fill conversion path, so the host applies flow and gap fill and emits the resulting G-code.
The host does not clip returned plugin paths again; they must remain inside the supplied expolygon.

## Request snapshot

`generate(pattern_id, request)` receives an owned copy of the values the host resolved for that call.
The request mirrors `FillParams` except its `PrintRegionConfig*`: it holds no pointer or reference into
the live print and stays valid after `generate()` returns. Values a pattern does not use yet are still
carried, so a plugin can rely on the same semantics as a built-in filler without reading print settings
itself.

It carries the per-call context (`expolygon`, `layer_id`, `object_id`, `layer_index`, `region_id`,
`region_ids`, `thickness_layers`, `z`, `spacing`, `overlap`, `angle`, `direction_angle`,
`direction_origin`, `purpose`) and the resolved `FillParams` values: density,
multiline, anchors, resolution, layer height, the resolved flow, extrusion role, smooth factor, pattern,
and the pattern-specific fields (adaptive infill, lateral lattice, lateral honeycomb, locked-zag,
symmetric infill, centered-surface grouping, gyroid optimization and 3D-honeycomb completion), plus the
boolean flags (`dont_adjust`, `monotonic`, `complete`, `use_arachne`, `dont_sort`, `can_reverse`,
`using_internal_flow`).

Identity fields are values, never pointers: `object_id` is `PrintObject::id().id` of the object owning
the fill's layer (normally the same value as `ctx.object.id()`; a shared-layer alias reports the
layer's owner), `layer_index` is zero-based in `PrintObject::layers()` (the object layers, which
exclude raft and support layers), and `region_id`/`region_ids` are indices into `Layer::regions()`.
`layer_id` stays the raw Fill layer id: `Layer::id()` (which is offset by the raft-layer count) in
normal infill, and an already object-relative id while anchoring. A cache keyed by layer must
therefore use `layer_index`. For a grouped surface, `region_id` is the first (representative) region whose
settings the host used and whose `LayerRegion` receives the generated paths; `region_ids` lists every
contributing region; the generated paths cover
all of them and must not be attributed to `region_id` alone. Unavailable identities are `-1` and an
empty `region_ids`.

`pattern` is the host `InfillPattern`; plugin dispatch reports `InfillPattern.Plugin`, while
`pattern_id` is the selected plugin pattern. `purpose` is the surface setting that selected the
pattern for this call, never inferred from density, extrusion role or pattern ID. Enums cross the
boundary as bound Python types (`InfillPattern`, `ExtrusionRole`, `AdaptiveInfillMode`,
`AdaptiveInfillGradient`, `SurfaceFillOrder`, `CenterOfSurfacePattern`, `SurfacePurpose`), never as
raw integers. The flow is copied as scalars: `width`, `height`, `spacing`, `nozzle_diameter`,
`bridge` and `mm3_per_mm`.

Units follow the Fill conventions: scaled integer coordinates for `expolygon`, `direction_origin`,
`horiz_move`, `symmetric_y_axis`, `infill_lock_depth` and `skin_infill_depth`; millimetres for lengths
and flow; radians for angles; degrees for the lateral-lattice angles and the lateral honeycomb overhang
angle; fractions in `<0, 1>` for densities and the smooth factor.

## Failure and diagnostics

Unresolvable pattern ownership and generation failures abort the slice as noncritical slicing errors
rather than crash-style critical errors. The user-facing message stays generic. Detailed provider
identity and exception information are recorded in plugin diagnostics and logs, retained until plugin
reload or explicit error clearing. Cancellation remains a cancellation, not a plugin error.

## Lifecycle and UI

Each of the four pattern selectors lists the built-in keys that setting has always offered plus every
pattern of every enabled infill capability whose descriptor declares that selector's surface purpose,
and stores the pattern ID as the setting value. Plugin rows carry the provider and pattern display
labels; the stored value is always the pattern ID. A stored ID whose provider is unavailable remains
the setting's value, so it round-trips instead of silently changing pattern. Load, unload, and
enable-state changes refresh all four live selectors, revalidate project references, and invalidate
stale slicing state. GUI updates are marshalled to the main thread.

Publishing (load and enable) and retracting (disable and unload) a capability's pattern IDs are
serialized against each other. Each pass reads the capability's live enable state before touching the
registry, so a pass that started against an older state can never republish an ID whose capability is
now disabled or unloaded, and a package cannot commit while another provider still owns one of its IDs.

Before the GUI slicing worker enters `Print::process()`, it waits once, bounded, for packages the print
manifest references that are still loading. This keeps a slice started from a load notification from
missing a plugin whose load is already in flight. Outside the GUI path (CLI, or a caller that invokes
`Print::process()` directly) no such wait exists; the infill preflight then reports the pattern as
unavailable rather than waiting.

Plugin factories hold only a weak provider reference. The registry entry holds the published provider
lease, an in-flight Fill owns a strong reference for as long as it exists, and dropping the final
reference decrefs the Python object under the interpreter lease and GIL, so unload cannot free a
provider that is still generating paths. The plugin manager keeps its own provider reference until
every job lease has been released, so releasing a lease never performs that final decref.

## Scope and constraints

Sparse, internal solid, top surface and bottom surface patterns are plugin-backed and dispatched. The
support and ironing pattern options stay built-in enums because their fill paths are not part of this
integration. Plain fill creation is still ID-only: `Fill::new_from_type`, `InfillFactory` and the plugin
adapter take a pattern ID, while the surface purpose travels as per-call host state next to the other
Fill context. CLI/headless plugin loading is not part of this integration. Project and preset values
must remain backward compatible, and plugin code must not manipulate the GUI from slicing workers.

An older application that opens a project or preset containing a plugin pattern ID in any of the four
settings reads it as an unknown enum value: the option is substituted with its built-in default and the
unknown capability is dropped from the manifest, so it slices with that default rather than the plugin
pattern. The plugin ID itself is preserved only by the newer application because the string setting
stores it verbatim.

## Known limitations

- The bounded pre-slice wait covers the GUI slicing worker only. A caller that invokes
  `Print::process()` directly (CLI, calibration helpers) does not wait for a plugin that is still
  loading; the infill preflight then reports the pattern as unavailable.
- Unloading an infill plugin whose selected pattern is still pinned by a running slice waits for that
  slice to finish or cancel rather than unloading its module. App exit is bounded by the GUI exit
  watchdog, not by the unload itself.
- A project saved by this version and opened by an older one loses plugin pattern IDs in the four
  pattern settings (they fall back to each option's built-in default) and loses the plugin reference
  from the manifest. Downgrade support is not attempted.
