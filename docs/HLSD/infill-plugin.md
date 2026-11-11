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

Before any object is sliced, `Print::process()` collects the plugin IDs stored in all four pattern
settings with the purpose each setting drives, validates each against the project manifest and the
registry entry's supported surfaces, then pins every registry entry the job will use. Pinning is
job-scoped rather than per object or per prepare/infill step, so an unload between two objects cannot
tear down a provider that a later object still needs, and a throwing pipeline hook, cancellation or
step invalidation cannot drop a pin while a fill worker may still create a Fill. A pinned entry cannot
be replaced or reused by another provider: unloading the plugin retires it for new selections while the
already-validated job may still try to create its Fill. A missing, disabled, unloaded, differently
owned, or purpose-mismatched provider fails the slice before any fill worker runs.

A pin is also a provider lifecycle lease: while a job holds one, the registry keeps the exact provider
instance (and therefore its Python module) alive. An unload request therefore drains pinned infill
providers without a deadline instead of force-unloading them, so the slicing worker never runs Python
against a module that was torn down. A capability re-enabled while its entry is pinned revives that
entry and keeps the pin. Pins are released when `Print::process()` returns, including on failure,
cancellation and a throwing pipeline hook, and by `Print::clear()` once background processing has
stopped. A per-object step invalidation deliberately does not release them: the worker may still be
inside the fill phase.

Each use creates a fresh `Fill` adapter that holds the provider alive. The adapter delegates per-island
path generation to the capability with the selected pattern ID. Python implementations execute through
the audited GIL trampoline; native implementations must be thread-safe because separate surface
generations can run concurrently. The adapter forwards supported fill semantics such as path sorting
and origin alignment. The per-call surface purpose is carried explicitly from the setting that selected
the pattern; a plugin never infers it from density, extrusion role or pattern ID.

Surface grouping keeps a plugin pattern's purpose in the key so one ID selected for sparse, top,
bottom and internal-solid surfaces is never generated as a single group. For a built-in pattern the
purpose is not part of the key: the pattern key and extrusion role already imply it, and the group
ordering (which decides which earlier group owns an overlap while trimming) stays exactly as before
plugin support.

The Python trampoline calls `generate(pattern_id, request)`, `no_sort(pattern_id)` and
`aligned_to_origin(pattern_id)`. The pattern ID is always passed positionally, so a `*args` override
receives the same arguments, and an override declared with a different arity receives Python's normal
`TypeError` rather than a fallback call.

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

`pattern` is the host built-in `InfillPattern`, or `None` for plugin dispatch because a plugin
pattern ID has no `InfillPattern` value; `pattern_id` is the selected plugin pattern. `purpose` is
the surface setting that selected the pattern for this call, never inferred from density, extrusion
role or pattern ID. Enums cross the boundary as bound Python types (`InfillPattern`,
`ExtrusionRole`, `AdaptiveInfillMode`,
`AdaptiveInfillGradient`, `SurfaceFillOrder`, `CenterOfSurfacePattern`, `SurfacePurpose`), never as
raw integers. The flow is copied as scalars: `width`, `height`, `spacing`, `nozzle_diameter`,
`bridge` and `mm3_per_mm`.

Units follow the Fill conventions: scaled integer coordinates for `expolygon`, `direction_origin`,
`horiz_move`, `symmetric_y_axis`, `infill_lock_depth` and `skin_infill_depth`; millimetres for lengths
and flow; radians for angles; degrees for the lateral-lattice angles and the lateral honeycomb overhang
angle; fractions in `<0, 1>` for densities and the smooth factor.

## Failure and diagnostics

Unresolvable pattern ownership and failures in any per-surface callback (`generate`, `no_sort`,
`aligned_to_origin`) abort the slice as noncritical slicing errors rather than crash-style critical
errors. Every callback reports through the same path: the user-facing message stays generic, while the
failing callback and its exception are recorded in plugin diagnostics and logs, retained until plugin
reload or explicit error clearing. Preflight errors identify the unavailable pattern ID. Cancellation
remains a cancellation, not a plugin error.

## Lifecycle and UI

Each of the four pattern selectors lists the built-in keys that setting has always offered plus every
pattern of every enabled infill capability whose descriptor declares that selector's surface purpose,
and stores the pattern ID as the setting value. Plugin rows carry the provider and pattern display
labels; the stored value is always the pattern ID. A stored ID whose provider is unavailable remains
the setting's value, so it round-trips instead of silently changing pattern. Load, unload, and
enable-state changes refresh the selectors and missing-plugin notifications without resetting a running
slice. In the GUI, a completed slice whose plugin provider was replaced or removed has its plugin fills
invalidated, together with the G-code that used them, so cached plugin output cannot be exported against
a different provider; the invalidation is deferred while a slice or export is running. The recorded
provider identity describes the fills the print actually holds, so it is captured when the job runs to
completion: a slice that throws or is cancelled leaves the previous record in place, and the next check
invalidates those older fills if the provider behind their pattern has changed. A pinned provider stays
alive until the slice releases it. GUI updates are marshalled to the main thread.

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

### Pattern value validation

The four pattern settings accept a built-in key or any other nonempty value as a plugin pattern ID.
Load-time validation cannot reject an unknown ID: it may name a provider that is not installed on this
computer, and failing the load would replace the missing-plugin guidance with an unreadable project.
`validate()` therefore accepts an unknown value that is not one of the option's built-in keys, preset
and project loading preserve it verbatim, and the infill preflight refuses the slice with the pattern ID
in the error once it has checked the registry and the project manifest. The CLI takes the same path: it
has no interactive installation step, so the preflight error is the report. A built-in key that the
option does not offer (for example `gyroid` in `top_surface_pattern`) and an empty value are still
rejected at validation, as on main.

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
- Pattern IDs carry no owner information: a setting stores the ID alone, and the mapping back to the
  owning capability needs the provider itself. Two providers advertising the same pattern ID cannot be
  told apart while one of them is unavailable, so a value saved with an unavailable provider may be
  attributed to the wrong one, and missing-provider guidance for an object, volume or layer-range
  override has no setting to jump to.
