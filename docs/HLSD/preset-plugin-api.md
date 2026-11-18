# Preset Plugin API — High Level Design

## Purpose and scope

The preset plugin API lets Python plugins update the presets OrcaSlicer is using,
so users do not have to perform every routine setting change by hand. It covers
process, printer, and filament presets. All three use one staged update lifecycle;
the way a target is selected is specific to the preset type.

The API edits Orca's managed edited preset, not a detached copy or a saved preset
behind the UI's back. It does not save to disk. Saving, discarding, and transferring
unsaved edits remain under Orca's existing preset workflow.

The API does not create or delete presets or provide off-tab editing. For filament,
choosing a different `preset_name` as the update target also selects that preset for
the requested slot, following the normal UI flow. A standalone slot-assignment or
preset-fork operation is outside this API's contract.

## Preset and slot model

Each preset collection maintains a managed edited copy for its selected preset.
Settings tabs apply changes to that copy and own the normal dirty-state, validation,
save/discard/transfer, and downstream refresh behavior. Plugin updates must use the
same path so that the settings tab, dirty indicators, and effective print
configuration observe a consistent state.

Filament selection adds a slot-to-preset mapping. `PresetBundle::filaments` is the
catalog, while `PresetBundle::filament_presets` maps each slot to a preset name.
Several slots may refer to the same preset. A filament update edits that shared
preset, so all slots referring to it are affected. Slot numbers exposed by the API
are 1-based, matching the UI.

## API shape

Each preset API exposes `begin_update()`, which selects and binds one target, and
returns a transaction. The transaction stages setting changes using `set()` and
applies them only on explicit `commit()`.

```python
# Filament is targeted by its 1-based slot. Omitting preset_name keeps the
# preset currently assigned to the slot; providing it selects that preset for
# the slot before staging updates.
tx = orca.host.preset.filament.begin_update(
    slot_number=2,
    preset_name="Generic PLA"
)
tx.set("filament_flow_ratio", "0.98")
tx.set("filament_retract_length", "0.8")
result = tx.commit()

# Process and printer default to their active presets. A caller can request a
# different preset that is selectable in the corresponding UI dropdown.
tx = orca.host.preset.process.begin_update(
    preset_name="0.20mm Standard"
)
tx.set("layer_height", "0.2")
tx.commit()
```

The example is the API contract, and the Python bridge provides it. `begin_update()`
is synchronous: it blocks the background Script worker until the target selection and
any unsaved-edit dialog settle, then returns the bound transaction. Orca's UI thread
stays responsive because the Script capability runs on its own worker. `set()` and
`commit()` run synchronously on that same worker. The Script worker and its optional
coroutine loop are described in
[python-plugin-async-bridge.md](python-plugin-async-bridge.md).

### Target resolution

- **Filament:** `slot_number` identifies the slot. If `preset_name` is omitted,
  the transaction targets the preset currently assigned to that slot. If supplied,
  it requests that preset for the slot and selects it through the normal UI flow;
  the name must be among the presets offered by that slot's dropdown, with one
  exception: the preset currently assigned to the slot stays targetable even when the
  slot's dropdown hides it (for example an incompatible-but-selected preset). Selecting
  a different preset changes the slot-to-preset mapping. In both cases the transaction
  edits the shared preset, not an isolated slot override.
- **Process and printer:** omitting `preset_name` targets the currently active
  preset. Supplying `preset_name` requests the preset by the name shown in its
  dropdown. It must be one of the choices the dropdown currently offers, rather
  than merely any preset present in the collection. This is particularly important
  for process presets, whose selectable choices can depend on the current context.
- A transaction targets exactly one preset. Multi-preset transactions are not
  supported: applying them would require switching among targets while the
  application has only one managed edited copy per collection.

Targets are resolved through Orca's normal preset-selection model. The API should
not accept raw preset pointers or filesystem paths as target identities. A missing,
unavailable, or ambiguous target fails explicitly instead of silently selecting a
different preset.

## Selection and user interaction

`begin_update()` resolves its target through shared binding helpers
(`bind_preset_target` for process/printer, `bind_filament_target` for filament) and
performs the selection only when the requested target differs from the active target;
otherwise it returns a transaction without switching. The helpers use Orca's normal
preset selection flow, `Tab::select_preset()`, including updating a filament slot
assignment when requested. If the normal selection flow requires user input, it opens
the relevant existing dialog(s), including unsaved-edit choices such as save, discard,
or transfer. The call remains blocked on the Script worker until those interactions
finish; it returns the bound transaction, or raises on a cancellation, without
applying staged settings. A canceled selection does not change the requested target or
filament slot assignment, but it does not roll back decisions the user already accepted
in an earlier dialog. The helpers then confirm that the requested preset actually
became the edited preset; a successful selection request alone is not sufficient. There
is no separate `switch_preset` plugin API.

If the requested target is already active, `begin_update()` returns without a
selection prompt, apart from the UI-thread dispatch needed to bind the transaction.
If a different preset is requested, Orca selects it through the normal UI flow. For
filament, this also updates the requested slot's assignment when `preset_name`
differs from its current assignment. When the current preset has unsaved changes,
the Script worker stays blocked until the user chooses among Orca's existing options,
such as transfer, discard, or cancel.

The API never suppresses that warning, chooses an option for the user, or switches
away from dirty edits silently. Cancel aborts the operation before any staged target
changes are applied. Process and printer updates follow this rule too when the
plugin requests a different `preset_name`; they normally return immediately when
the active preset is the target.

## Staging and commit semantics

`set(key, value)` stages a serialized config value and does not mutate the preset.
The transaction is the boundary for a related group of changes, avoiding
intermediate partially-updated states. `commit()` runs synchronously on the background
Script worker. At commit, the API validates keys and values against the target preset
type and applies the batch through `Tab::apply_preset_config_values`, the existing tab
and settings-group update path. Existing setting-specific validation, correction, and
callbacks remain authoritative; plugin updates do not create a parallel setter with
different behavior.

A successful commit updates dirty state and downstream configuration/Plater state
once for the batch. The edited preset remains unsaved; persistence continues through
Orca's normal save action. A validation or commit failure restores the edited preset's
config to its pre-batch state and recomputes the state derived from it (compatibility
marks, the preset selector, dependent tabs, the Plater and the printer agent), so no
staged setting is left partially applied. That guarantee is limited to the edited
preset and its config-derived state: an `on_change` callback is a GUI method, not a pure
function of the config, so the transaction does not reverse a preset or slot selection
made while resolving the target, a dialog decision the user already accepted, an undo
snapshot, a dependent preset's rewritten config, or anything already sent to a device.
Exact all-or-nothing across those external effects is not achievable while the
authoritative GUI callbacks run; the boundary is documented on
`Tab::apply_preset_config_values`. A filament slot assignment made as part of target
selection is likewise not a staged setting: after the user accepts the selection flow it
may remain changed even if a later commit fails, and the API does not switch back or
reverse the user's selection or any accepted transfer/discard decision for the previous
preset.

## Outcomes and consistency

The API distinguishes target resolution failures, invalid setting/value, user
cancellation, stale filament-slot mapping, and commit failure. A result identifies
the target preset, whether preset selection changed, and (for filament) whether the
slot assignment changed and which slots share the edited preset. A canceled
selection applies neither the requested slot assignment nor staged target changes;
a failed commit applies none of the staged target settings.

Filament slot assignments can change between `begin_update()` and `commit()`. The API
detects that and rejects a stale operation instead of silently editing the new
slot assignment. It must not hold raw preset pointers across reloads. Conflicting
duplicate writes to one setting in a transaction are rejected rather than resolved
by incidental call order.

The existing one-setting filament call,
`orca.host.preset.filament.update(slot_number, key, value)`, remains available as a
compatibility shorthand. It shares the staged API's target resolution, selection,
validation, and commit path, and it is synchronous like `begin_update()`: both block
the background Script worker while any selection dialog settles.

## Design principles

- **Reduce routine interaction, preserve agency.** Clean switches should be
  seamless; unsaved user work must still receive the normal warning and decision.
- **One source of truth.** UI and plugins share the managed edited copy and setting
  update path, including validation and callbacks.
- **The UI workflow is authoritative.** The API must enter through the same
  selection and setting-change workflows as the UI, not reproduce their logic or
  write config values directly. This keeps existing and future side effects active:
  `on_change` handling for mutually exclusive, invalid, or out-of-range settings;
  compatibility and dependent-preset updates; and printer-agent changes when the
  selected printer changes.
- **Explicit target and explicit commit.** Plugins know which preset they edit and
  when staged changes become visible.
- **No hidden persistence.** Plugin edits remain unsaved until the user saves them
  through Orca.
- **Shared means shared.** Editing a filament preset through one slot affects every
  slot assigned to it. Choosing another preset for a slot is explicit in
  `preset_name`; slot-specific divergence would require a separate fork operation.
- **Consistent lifecycle, type-specific targeting.** Process, printer, and
  filament share transaction and error semantics while respecting their existing
  selection models.

## Implementation in the current codebase

The transaction API is implemented. The current plugin surface exposes `begin_update()`,
`PresetTransaction` with `set()`/`commit()`, and the per-setting `update()` compatibility
shorthand. For filament, `update_filament_slot()` resolves the preset already assigned
to a slot, selects it through `Tab::select_preset()`, and applies one setting through
`Tab::apply_preset_config_values()`; it does not change the slot assignment.

`begin_update()` and `update()` are synchronous blocking calls
(`begin_preset_update` / `begin_filament_update`). They marshal the target binding to
the UI thread and block the background Script worker until the selection settles, so
the wait never blocks the UI thread. They are valid only on that worker, never from the
UI thread or a slicing pipeline hook.

`Tab::select_preset()` is the basis for switching to process, printer, or filament
presets and for invoking the normal unsaved-edit flow. The flow is type-specific: a
process selection can trigger compatibility handling for filaments, and a printer
selection can involve technology changes and dependent preset compatibility. It may
therefore show more than one dialog. Cancellation of a later step does not necessarily
undo decisions the user already accepted in an earlier dialog. Target resolution
preserves those side effects and then verifies the resulting edited target.

Filament `preset_name` is an additional integration on top of `Tab::select_preset()`:
selecting a different name for a slot also updates the slot-to-preset mapping and its
dependent UI/configuration state, mirroring the filament branch of the Plater's normal
selection path (`PresetBundle::set_filament_preset()` plus the project/selection refresh,
the dynamic filament list, the conditional support-filament flushing recalc, the slot
combo refresh and its AMS sync badge). A mixed-color virtual slot is not targetable.

Process and printer dropdown choices are constrained by visibility and compatibility,
and compatibility may depend on the active context. Target resolution checks the tab
dropdown's actual items before the selection flow, so a target the user cannot pick
fails without raising that flow's unsaved-edit prompt; a name two entries carry is
rejected as ambiguous. A filament `preset_name` resolves against the requested slot's
own sidebar selector rather than the tab's, so that slot's compatibility filter is the
one that applies. A name the slot's dropdown does not show is rejected unless it names
that slot's current assignment (a hidden or incompatible-but-selected preset), which
stays targetable.

`commit()` applies its batch through `Tab::apply_preset_config_values`, which parses and
validates the whole batch before touching the edited preset and restores the edited
preset and its config-derived state on failure (see "Staging and commit semantics" for
the limit of that rollback). Since `begin_update()` performs target selection before the
caller stages values, invalid staged values can still follow a user-visible selection
prompt; that is a consequence of this API shape and remains explicit in its behavior.

## Integration boundary

The plugin API coordinates target resolution, preset selection, staged values, and
the settings commit path. It does not own or duplicate preset dirty-state logic,
selection prompts, setting validation, or persistence. Target changes flow through
the same UI selection path so dependent behavior—such as selecting the printer
agent—still runs. Setting changes flow through the same UI change path so `on_change`
and related validation/correction behavior still runs. The integration bridges UI
prompts to the Python operation through the blocking `begin_update()` and reuses
the normal Plater slot-selection behavior for explicit filament targets, and commits a
batch without losing existing per-setting behavior.
