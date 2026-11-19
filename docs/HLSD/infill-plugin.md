# Infill Plugins

## Purpose and scope

Infill plugins supply patterns for sparse, internal solid, top surface and bottom surface infill.
For each surface island, a plugin produces open paths; OrcaSlicer retains the normal fill lifecycle,
including extrusion conversion, flow, gap fill, path ordering and G-code generation. Support and
ironing patterns remain built-in.

A capability identifies a provider and can advertise several patterns. Each pattern has a stable,
nonlocalized ID, a display name and one or more supported surface purposes. The four selectors store
the pattern ID, not the provider name. Existing built-in values remain unchanged.

## Python API

A Python infill capability subclasses `orca.infill.InfillPluginCapabilityBase`, which derives from
`orca.PythonPluginBase`. Its interface is:

| Method | Contract |
| --- | --- |
| `get_name()` | Required. Returns the capability's provider name. |
| `patterns()` | Optional. Returns a nonempty sequence of `orca.infill.PatternDescriptor` objects. The default is one sparse pattern whose ID and display name are the capability name. |
| `generate(pattern_id, request)` | Required. Returns open paths for the supplied island. |
| `no_sort(pattern_id)` | Optional, defaults to `False`. If true, preserves the returned path order through the G-code path planner. |
| `aligned_to_origin(pattern_id)` | Optional, defaults to `False`. If true, the host shifts the island into a frame centered on the fill bounding box before calling `generate()`, then shifts the returned paths back. |

`PatternDescriptor(id, display_name="", supported_surfaces=[SurfacePurpose.Sparse])` describes a
selectable pattern. An empty display name defaults to the ID. `supported_surfaces` must not be empty;
its values are `orca.infill.SurfacePurpose.Sparse`, `InternalSolid`, `TopSurface` and
`BottomSurface`. A descriptor may support multiple purposes. IDs must be nonempty, must not contain
`;`, and must be unique across all loaded infill capabilities and built-in pattern keys. This includes
disabled but still loaded capabilities.

The host resolves descriptors when the capability loads. It passes `pattern_id` positionally to each
per-pattern callback. Callbacks are synchronous and run on slicing workers; they must not manipulate
the GUI. Python callbacks acquire the GIL, while native providers must handle concurrent calls
themselves.

### Path contract

`generate()` returns a sequence of paths. An empty sequence means no infill. Each path has at least
two points and can be an `(N, 2)` NumPy `int64` array, an `orca.host.Polygon` treated as an open
path, or a sequence of `orca.host.Point` values. Coordinates are scaled integers, in the same
working frame as `request.expolygon`. The host does not rescale or clip the returned paths; they
must remain inside that expolygon.

The expolygon is already adjusted for overlap and solid spacing. When `aligned_to_origin()` is true,
it is also already in the shifted working frame. Plugins should not apply the overlap again.

### Request snapshot

`request` is an owned value snapshot, not a reference to the live print, and may be retained after
the callback. It contains resolved fill parameters, not the entire `FillParams` or a print configuration
pointer. Its fields are:

| Group | Fields |
| --- | --- |
| Island and context | `expolygon`, `layer_id`, `object_id`, `layer_index`, `region_id`, `region_ids`, `thickness_layers`, `z`, `spacing`, `overlap`, `angle`, `direction_angle`, `direction_origin`, `purpose` |
| General fill | `density`, `multiline`, `anchor_length`, `anchor_length_max`, `resolution`, `layer_height`, `flow`, `flow_width`, `flow_height`, `extrusion_role`, `pattern`, `smooth_factor`, `no_extrusion_overlap`, `fill_order`, `horiz_move` |
| Flags | `dont_adjust`, `monotonic`, `complete`, `use_arachne`, `dont_sort`, `can_reverse`, `using_internal_flow` |
| Pattern-specific | `gyroid_optimized`, `adaptive_infill_mode`, `adaptive_infill_interior_density`, `adaptive_infill_gradient`, `lateral_lattice_angle_1`, `lateral_lattice_angle_2`, `infill_overhang_angle`, `symmetric_infill_y_axis`, `symmetric_y_axis`, `infill_complete_top`, `locked_zag`, `infill_lock_depth`, `skin_infill_depth`, `center_of_surface_pattern` |
| Computed properties | `full_infill`, `dont_connect` |

`purpose` identifies the setting that selected the pattern. Do not infer it from density, extrusion
role or pattern ID. `pattern` is a bound built-in `InfillPattern`, or `None` for plugin dispatch;
the plugin ID is the separate `pattern_id` argument. Other enum-valued fields also use bound Python
enum types, not raw integers.

`flow` is an `orca.infill.Flow` snapshot with `width`, `height`, `spacing`, `nozzle_diameter`,
`bridge` and `mm3_per_mm`. `flow_width` and `flow_height` are compatibility shortcuts for the
corresponding flow values. Plugin fill uses ordinary paths, not Arachne's variable-width paths.

Coordinates in `expolygon`, `direction_origin`, `horiz_move`, `symmetric_y_axis`,
`infill_lock_depth` and `skin_infill_depth` use scaled units. Use the live scaling factor or
`orca.slicing.unscale()` to convert. Lengths and flow dimensions use millimetres;
`flow.mm3_per_mm` uses mm³/mm. `angle` and `direction_angle` use radians, counterclockwise from +X;
the lateral lattice and honeycomb overhang angles use degrees. Densities and `smooth_factor` are
fractions from 0 to 1.

`object_id` identifies the object owning the fill's layer. `layer_index` is the zero-based index in
that object's layers, excluding raft and support layers; use it rather than `layer_id` for layer
caches. `layer_id` is the raw Fill layer ID, which includes the raft offset in normal infill but is
already object-relative during anchoring. Cache entries must still be considered stale when the print
changes.

`region_id` is the index of the representative region whose settings and output are used.
`region_ids` contains all contributing region indices. A grouped call covers all those regions, not
just `region_id`. When identity is unavailable, the individual IDs are `-1` and `region_ids` is
empty.

## Selection and slicing

`InfillFactory` resolves canonical pattern IDs to built-in or plugin factories. Built-in metadata
defines the key, display label and supported surfaces; enabled capabilities publish plugin patterns
to the same registry. Plugin dispatch uses the pattern ID, with surface purpose carried separately
in the fill context. Some internal-only built-in fills still use an enum-based creation path.

Before processing fills, `Print::process()` resolves and validates plugin patterns needed by objects
whose infill steps will run. It checks each selected purpose and pins the registry entries for the
job. An unavailable pattern or unsupported purpose fails preflight before fill workers run. Plugin
surface groups include purpose in their grouping key, so one ID used by different settings is not
generated as a single group. Built-in grouping retains its existing behavior.

Each use creates a fresh `Fill` adapter, which holds the provider alive and delegates island generation.
Job pins also keep the exact provider instance alive. Disabling or unloading retires its entries for
new selections, but a pinned job can continue to use them. Re-enabling the same pinned entry revives
it without losing the pin. Pins are released when `Print::process()` exits, including on failure or
cancellation, or by `Print::clear()` after background processing stops. Per-object invalidation does
not release a running job's pins.

Plugin factories use weak provider references; registry leases and active fills hold strong references.
The plugin manager retains its provider while jobs and active calls drain, so a Python module is not
torn down while slicing uses it. Unloading an infill provider waits without a deadline for those
leases and calls; application exit is instead bounded by the GUI exit watchdog. Publishing and
retracting patterns are serialized and check the capability's current enabled state.

## Persistence and user experience

Selectors show their existing built-in choices and patterns from enabled capabilities that support
the selector's purpose. Plugin rows show provider and pattern labels, but store the ID. A missing
provider does not replace a stored ID: projects and presets retain it so users can restore the plugin.

When the GUI saves a preset or project, it resolves known plugin patterns against loaded capability
descriptors, including disabled capabilities, to preserve full plugin references in the manifest.
Headless saves without a capability resolver leave the existing manifest untouched. The manifest
supports plugin management and diagnostics; it does not bind an ID to a provider for slicing.
`InfillFactory` selects the currently registered provider. Once an original provider is unloaded,
another provider can register the same ID and will then be used. Users must ensure that the installed
provider is the intended one.

The four settings accept their offered built-in keys and other nonempty strings as possible plugin
IDs. Empty values and built-in keys not offered by that setting are invalid. Unknown IDs must survive
loading, because their plugin may be missing; slicing validation and preflight report them as
unavailable instead of silently falling back. The GUI reports the pattern ID in its slicing-blocked
notification.

Load, unload and enable-state changes refresh selectors and missing-plugin notifications on the GUI
thread without resetting a running slice. When a provider changes or disappears, the GUI invalidates
affected cached plugin fills and their G-code; it defers invalidation while slicing or export is
running. The print records provider identities for generated plugin fills and uses them to detect
changes.

Before the GUI slicing worker calls `Print::process()`, it waits up to 10 seconds in total for
manifest-referenced packages that are still loading. Direct callers, including CLI and calibration
helpers, do not wait; preflight reports an unavailable pattern. CLI/headless plugin loading is outside
this integration.

Python callback failures and standard exceptions from native callbacks abort the slice as noncritical
slicing errors. The user-facing message is generic; the callback name and exception are retained in
plugin diagnostics and logs until reload or explicit clearing. Cancellation remains cancellation.

## Compatibility limits

- Older OrcaSlicer versions treat plugin pattern IDs as unknown enum values, substitute each setting's
  built-in default and drop the unknown capability from the manifest. Downgrade preservation is not
  supported.
- Pattern IDs do not encode ownership. If the original provider is missing, the factory cannot
  identify it reliably or offer its installation or activation. A setting inside an object, volume or
  layer-range override may also have no corresponding UI location to jump to.
