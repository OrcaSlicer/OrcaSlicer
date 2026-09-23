# Plugin Host Feature Contracts

`orca.host.feature_contracts()` reports the semantic contracts implemented by
the current host build.  It is a discovery API for plugin authors; it is not a
host version, build identity, plugin capability, or permission grant.

Each result is a copied `FeatureContract` with these scalar fields:

- `feature_id`
- `major_version`
- `minor_version`

The host currently advertises the following contracts:

| Feature ID | Contract | Host surface |
| --- | --- | --- |
| `isolated_fff_simulation` | 1.0 | opaque isolated FFF slicing job |
| `process_preset_transaction` | 1.0 | opaque Process preset transaction |
| `printer_preset_transaction` | 1.0 | opaque Printer preset transaction |
| `filament_slot_transaction` | 1.0 | opaque Filament slot transaction |

A plugin must require the exact feature ID and a compatible major version
before using a corresponding factory.  A missing entry, an unknown ID, or an
incompatible major version means that feature is unavailable.  Plugins must not
infer a contract from an OrcaSlicer executable name, a build string, or the
presence of an unrelated binding.  A future minor version may add only
backwards-compatible copied result fields; an incompatible change requires a
new major version.

Feature contracts are intentionally separate from the plugin capability
registry.  The registry describes plugin-owned capabilities, their enable state
and configuration.  Feature contracts describe closed host APIs compiled into
the application.  Neither API creates a mutable host reference.

The isolated FFF contract owns an immutable baseline and all worker state; it
accepts copied canonical patches and returns copied results.  The Process and
Printer contracts are closed domain-specific transactions with strict staging,
validation, persistence read-back and rollback.  The Filament contract is also
closed, but its identity includes the active physical slot and mapping state;
mixed, shared or stale mappings are rejected rather than reassigned.  The
contracts do not expose `Print`, `Preset`, `PresetCollection`,
`DynamicPrintConfig`, or mutable mapping containers.
