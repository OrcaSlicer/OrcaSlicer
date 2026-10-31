# 3D-Fuel OrcaSlicer filament profiles

This workspace contains a starter set of brand-wide profiles for 3D-Fuel. Per the [OrcaSlicer profile guide](https://github.com/orcaslicer/orcaslicer/wiki/how_to_create_profiles#vendor-index-file), filament brands are registered in the shared `OrcaFilamentLibrary`, not as printer vendors. The index registers each material's non-selectable `@base` profile and selectable `@System` profile.

The index in this workspace is a template containing only the 3D-Fuel entries. **Do not use it to replace OrcaSlicer's existing `OrcaFilamentLibrary.json`.** Merge these entries into the existing index in an OrcaSlicer source checkout and preserve its other profiles and metadata.
3D-Fuel git hub (https://github.com/3DFuel/Orcaslicer-Profiles.git) for process files