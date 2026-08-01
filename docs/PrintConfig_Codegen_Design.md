# PrintConfig Codegen — Design Document

## 1. Problem Statement

Every config setting in OrcaSlicer (e.g. `travel_speed`, `wipe_distance`) is independently maintained as string literals across ~12 locations in the codebase with zero compile-time validation linking them.

A single setting like `wipe_distance` appears in:

| # | Location | What's duplicated |
|---|----------|-------------------|
| 1 | `PrintConfig.cpp` `init_fff_params()` | Key, type, label, tooltip, default, constraints |
| 2 | `PrintConfig.hpp` struct members | Type + member name (must match key string) |
| 3 | `Preset.cpp` option lists | Key string in serialization lists |
| 4 | `PrintConfig.cpp` extruder/filament lists | Key string in 4 sub-lists |
| 5 | `PrintConfig.cpp` variant option sets | Key string in variant sets |
| 6 | `Print.cpp` invalidation chains | `opt_key == "..."` checks |
| 7 | `Tab.cpp` GUI layout | `append_single_option_line("key")` |
| 8 | GUI files (`Field.cpp`, `OptionsGroup.cpp`) | `opt_key == "..."` special-case handling |
| 9 | `PrintConfig.cpp` `handle_legacy()` | Old-to-new key name mapping |
| 10 | `PrintConfig.cpp` `new_def` G-code placeholders | Re-declared type, label, tooltip |
| 11 | `resources/profiles/*.json` | Key strings as JSON keys |

### Consequences

- Adding a new setting requires editing ~12 files manually
- A typo in any one location causes a silent bug (no compile-time validation)
- Print providers cannot add/customize settings without forking the C++ codebase
- No cross-language tooling (Python scripts, web editors) can consume the schema
- No build-time validation of profile JSONs against the canonical option list

---

## 2. Goals

- **Single source of truth** for all setting definitions
- **Compile-time safety** against key mismatches across the codebase
- **Adding a new setting = editing 1 file** (instead of ~12)
- **Enable print providers** to customize settings (defaults, constraints, visibility) without C++ changes
- **Cross-language API generation** (Python, TypeScript, JSON Schema)
- **Build-time validation** of profile JSONs
- **Hidden mode** — setting exists in config/serialization but is not shown in UI
- **Disabled mode** — setting shown in UI but greyed out / non-editable
- **Automated UI layout** — new settings with GUI annotations appear in UI without manual `Tab.cpp` edits

### Non-goals

- Runtime performance changes (slicing engine untouched)
- Changing the `.3mf` wire format (cereal/JSON serialization preserved)

---

## 3. Current Architecture

### 3.1 Type System

`Config.hpp` defines ~15 `ConfigOptionType` variants:

```cpp
coFloat, coInt, coBool, coString, coPercent, coFloatOrPercent,
coPoint, coPoint3, coEnum,
coFloats, coInts, coBools, coStrings, coPercents, coFloatsOrPercents,
coPoints, coEnums
```

Each has a corresponding C++ class (`ConfigOptionFloat`, `ConfigOptionBools`, etc.) with virtual `serialize()`/`deserialize()` methods:

```cpp
class ConfigOption {
    virtual ConfigOptionType type() const = 0;
    virtual std::string serialize() const = 0;
    virtual bool deserialize(const std::string &str, bool append = false) = 0;
    virtual ConfigOption* clone() const = 0;
    // ...
};

class ConfigOptionFloat : public ConfigOptionSingle<double> {
    ConfigOptionType type() const override { return coFloat; }
    std::string serialize() const override { /* double -> string */ }
    bool deserialize(const std::string &str, bool append) override { /* string -> double */ }
};
```

### 3.2 Definition Layer

`ConfigOptionDef` holds all metadata for one setting:

```
opt_key, type, nullable, default_value,
label, full_label, category, tooltip, sidetext,
mode (Simple/Advanced/Develop),
min, max, max_literal, ratio_over,
gui_type, multiline, full_width, height,
enum_values, enum_labels, enum_keys_map,
aliases, shortcut
```

All ~500 settings are registered in `PrintConfigDef::init_fff_params()` (~6000 lines) into the global `print_config_def` singleton. Each registration block:

```cpp
def = this->add("bridge_flow", coFloat);    // register key + type
def->label = L("Bridge flow ratio");        // UI label
def->category = L("Quality");               // tab category
def->tooltip = L("...");                    // tooltip
def->min = 0;                              // constraint
def->max = 2.0;                            // constraint
def->mode = comAdvanced;                   // visibility mode
def->set_default_value(new ConfigOptionFloat(1)); // default
```

### 3.3 Storage Layer

Two parallel systems:

**StaticPrintConfig** — compiled-in struct fields via `PRINT_CONFIG_CLASS_DEFINE` macro. Name-to-byte-offset cache. Used in the slicing engine for direct member access (performance-critical).

**DynamicPrintConfig** — `std::map<string, ConfigOptionUniquePtr>`. Used in GUI and for diff/apply operations.

Class hierarchy:

```
FullPrintConfig
├── PrintObjectConfig       (~120 fields)
├── PrintRegionConfig       (~80 fields)
└── PrintConfig
    ├── MachineEnvelopeConfig (~25 fields)
    └── GCodeConfig          (~80 fields)
```

### 3.4 Serialization

- **JSON presets**: `ConfigBase::save_to_json()` / `load_from_json()` — iterates option keys, calls `opt->serialize()` to string, writes JSON key-value pairs
- **Binary .3mf**: cereal archives via `load_option_from_archive()` / `save_option_to_archive()`
- Both formats use the same string keys as identifiers

### 3.5 Invalidation

`Print::invalidate_state_by_config_options()` contains large `opt_key == "..."` chains that classify each changed option key into pipeline steps to invalidate:

```
posSlice, posPerimeters, posInfill, posSupportMaterial,
psGCodeExport, psSkirtBrim, psWipeTower
```

### 3.6 GUI Binding

- `Tab.cpp` builds UI via `append_single_option_line("key_name")` — looks up `ConfigOptionDef` from `print_config_def`, auto-creates the appropriate widget
- `ConfigManipulation.cpp` contains `toggle_print_fff_options()` — imperative logic that reads config values and toggles field visibility


## 4. Proposed Solution

### 4.1 Architecture

Protobuf as schema + codegen, NOT a runtime replacement.

```
┌─────────────────────┐                 ┌──────────────────────────────┐
│  src/PrintConfigs/  │     codegen     │ PrintConfigDef_generated.cpp │  done
│  *.proto            │ ──────────────> │ Preset_options_generated.cpp │  done
│  layout.yaml        │                 │ Invalidation_generated.cpp   │  done
│                     │                 │ OptionKeys_generated.cpp     │  done
└─────────────────────┘                 │ PrintConfig_generated.hpp    │  future
                                        │ TabLayout_generated.cpp      │  future
                                        └──────────────────────────────┘
```

### 4.2 Proto Schema Design

#### 4.2.1 Custom Field Options

`src/PrintConfigs/config_metadata.proto` defines custom extensions covering all `ConfigOptionDef` metadata:

```protobuf
syntax = "proto3";
import "google/protobuf/descriptor.proto";
package orca;

enum ConfigMode {
  MODE_SIMPLE   = 0;
  MODE_ADVANCED = 1;
  MODE_DEVELOP  = 2;
}

enum PresetType {
  PRESET_PRINT    = 0;
  PRESET_FILAMENT = 1;
  PRESET_PRINTER  = 2;
}

enum InvalidationStep {
  STEP_GCODE_EXPORT = 0;
  STEP_SKIRT_BRIM   = 1;
  STEP_WIPE_TOWER   = 2;
  STEP_SLICE        = 3;
  STEP_PERIMETERS   = 4;
  STEP_INFILL       = 5;
  STEP_SUPPORT      = 6;
  STEP_NONE         = 7;
}

enum OptionListMembership {
  LIST_NONE                     = 0;
  LIST_EXTRUDER_OPTION_KEYS     = 1;
  LIST_FILAMENT_OPTION_KEYS     = 2;
  LIST_VARIANT_OPTION_KEYS      = 3;
}

extend google.protobuf.FieldOptions {
  // Display metadata
  string  label         = 50001;
  string  full_label    = 50002;
  string  tooltip       = 50003;
  string  category      = 50004;
  string  sidetext      = 50005;

  // Numeric constraints
  double  min_value     = 50006;
  double  max_value     = 50007;
  double  max_literal   = 50008;

  // UI behavior
  ConfigMode mode       = 50009;
  string  ratio_over    = 50010;
  bool    multiline     = 50013;
  bool    full_width    = 50014;
  int32   height        = 50015;

  // Classification
  PresetType preset                       = 50011;
  repeated InvalidationStep invalidates   = 50012;
  repeated OptionListMembership list_membership = 50018;

  // Migration
  string  legacy_name   = 50016;

  // Nullable support (for ConfigOptionFloatsNullable, etc.)
  bool    is_nullable   = 50017;

  // GUI type override (e.g. "i_enum_open", "color", "f_enum_open")
  string  gui_type      = 50019;
  string  gui_flags     = 50020;

  // Enum metadata
  string  enum_keys_map_ref = 50021;
  bool    no_cli            = 50022;
  bool    readonly          = 50023;

  // C++ codegen hints
  string  co_type_hint  = 50024;

  // Default value — constructor args only (e.g. "1.0", "5000.0, 5000.0")
  // Codegen reconstructs full C++ from co_type + this value
  string  default_value     = 50025;
  bool    has_default        = 50028;  // proto3 can't distinguish empty string from unset

  // Enum values and labels
  repeated string enum_value_entries  = 50026;
  repeated string enum_label_entries  = 50027;
}

extend google.protobuf.MessageOptions {
  // Virtual preset keys: keys that belong to this preset type in Preset.cpp
  // option lists but have no ConfigOptionDef entry (printer identity fields,
  // host/connectivity settings, filament retraction overrides, compatibility
  // flags, cross-preset keys). The codegen emits these directly into the
  // s_Preset_*_options array alongside the field-derived keys.
  // To add a virtual key: add one option line here and re-run codegen.
  repeated string virtual_preset_keys = 60001;
}
```

#### 4.2.2 Setting Files

Settings are split into three `.proto` files by preset type. Each setting becomes a proto field with annotations:

| File | Contents |
|------|----------|
| `src/PrintConfigs/print.proto` | ~477 print/process settings |
| `src/PrintConfigs/filament.proto` | ~103 filament settings |
| `src/PrintConfigs/printer.proto` | ~42 printer settings |

Each file also carries message-level `virtual_preset_keys` declarations (see §5.2.3).

Example field:

```protobuf
float travel_speed = 42 [
  (label)         = "Travel",
  (tooltip)       = "Speed of travel which is faster and without extrusion.",
  (sidetext)      = "mm/s",
  (min_value)     = 1,
  (mode)          = MODE_ADVANCED,
  (preset)        = PRESET_PRINT,
  (has_default)   = true,
  (default_value) = "200",
  (invalidates)   = STEP_GCODE_EXPORT
];
```

#### 4.2.3 Virtual Preset Keys

The `s_Preset_*_options` vectors in `Preset.cpp` need to include keys beyond those with `ConfigOptionDef` entries — for example, printer identity fields (`printer_technology`, `printable_area`), connectivity settings (`host_type`, `print_host`), filament retraction overrides (`filament_retraction_length`, `filament_z_hop`, …), and cross-preset keys that belong to multiple preset types.

These are declared directly in the `.proto` message body using the `virtual_preset_keys` message option:

```protobuf
message PrinterSettings {

  // Virtual keys (not in PrintConfigDef)
  option (virtual_preset_keys) = "printer_technology";
  option (virtual_preset_keys) = "printable_area";
  option (virtual_preset_keys) = "host_type";
  option (virtual_preset_keys) = "print_host";
  // ... etc

  // Cross-preset keys (defined in print.proto, also saved in printer presets)
  option (virtual_preset_keys) = "single_extruder_multi_material";
  option (virtual_preset_keys) = "wipe_tower_type";
  // ... etc

  float extruder_clearance_height_to_rod = 1 [ ... ];
  // ...
}
```

The codegen reads these and merges them (deduplicated, sorted) with the field-derived keys into the generated `s_Preset_printer_options` vector. No hand-written extender struct in `Preset.cpp` is needed.

#### 4.2.4 Type Mapping

| C++ Type | Proto Representation | Notes |
|---|---|---|
| `ConfigOptionFloat` | `float field = N` | |
| `ConfigOptionInt` | `int32 field = N` | |
| `ConfigOptionBool` | `bool field = N` | |
| `ConfigOptionString` | `string field = N` | |
| `ConfigOptionFloats` | `repeated float field = N` | Per-extruder vectors |
| `ConfigOptionInts` | `repeated int32 field = N` | |
| `ConfigOptionBools` | `repeated bool field = N` | |
| `ConfigOptionStrings` | `repeated string field = N` | |
| `ConfigOptionPercent` | `float field = N` | `(co_type_hint) = "coPercent"` |
| `ConfigOptionPercents` | `repeated float field = N` | `(co_type_hint) = "coPercents"` |
| `ConfigOptionFloatOrPercent` | `FloatOrPercent field = N` | Custom wrapper message |
| `ConfigOptionEnum<T>` | `int32 field = N` | `(co_type_hint) = "coEnum"` + `(enum_keys_map_ref)` |
| `ConfigOptionPoint` | `Point2D field = N` | Custom wrapper message |
| `ConfigOptionFloatsNullable` | `repeated float field = N` | `(is_nullable) = true` |

#### 4.2.5 UI Layout File

`src/PrintConfigs/layout.yaml` declares the UI tab/page/group structure used by `Tab.cpp`. It lists field names in display order under their respective groups. The codegen will eventually use this to generate `TabLayout_generated.cpp` (future phase).

### 4.3 Code Generator Outputs

| Output | Replaces | Status |
|---|---|---|
| `PrintConfigDef_generated.cpp` | `init_fff_params()` body (~6000 lines) | Done |
| `Preset_options_generated.cpp` | `s_Preset_*_options` string vectors | Done |
| `Invalidation_generated.cpp` | `opt_key ==` chains in `Print.cpp` | Done |
| `OptionKeys_generated.cpp` | Extruder/filament key lists | Done |
| `PrintConfig_generated.hpp` | `PRINT_CONFIG_CLASS_DEFINE` macro blocks | Future |
| `TabLayout_generated.cpp` | `append_single_option_line()` calls in `Tab.cpp` | Future |

### 4.4 CMake Integration

```cmake
add_custom_command(
  OUTPUT ${GENERATED_SOURCES}
  COMMAND ${ORCA_CODEGEN_PYTHON} tools/run_codegen.py --no-validate
  DEPENDS src/PrintConfigs/*.proto src/PrintConfigs/layout.yaml tools/config_codegen.py
)
```

Generated files are **not** checked into the repo — `src/slic3r/GUI/generated/` and `config.desc` are gitignored, and every build produces them. Each build script and CI job runs `tools/run_codegen.py` before configuring; a fresh clone that goes straight to `cmake` gets them generated at configure time.

`tools/run_codegen.py` resolves its own toolchain (`tools/codegen_toolchain.py`), so no caller has to install anything:

- **protoc** — `$PROTOC`, then `PATH`, then a cached copy, then a pinned checksum-verified release downloaded into `.codegen-tools/` (also gitignored)
- **protobuf + pyyaml** — the calling interpreter if it has them, otherwise a virtualenv under `.codegen-tools/` that the script re-execs into

Set `PROTOC=<path>` to build offline or against a distro protoc.

### 4.5 Provider Customization

Providers ship an overlay file alongside their existing JSON profiles:

```yaml
# resources/profiles/Creality/settings_overlay.yaml
overrides:
  travel_speed:
    max_value: 600
    default: 300
  travel_speed_z:
    mode: hidden          # not relevant for this printer
  firmware_retraction:
    mode: disabled        # shown but locked — firmware handles this
custom_options:
  - key: creality_vibration_compensation
    type: bool
    label: "Vibration Compensation"
    default: true
    category: "Quality"
    mode: advanced
    gui_page: "Quality"
    gui_group: "Other"
```

Custom options get field numbers > 1000 to avoid conflicts.

---

## 5. What Changes vs. What Stays

### Changes (generated from proto)

| Artifact | Current | After | Status |
|---|---|---|---|
| `init_fff_params()` body (~6000 lines) | Hand-written C++ | `#include` of generated file | Done |
| `s_Preset_*_options` lists | Hand-written string vectors | Generated from `(preset)` + `virtual_preset_keys` | Done |
| `invalidate_state_by_config_options()` | Hand-written `opt_key ==` chains | Generated map lookup | Done |
| Extruder/filament key lists | Hand-written string vectors | Generated from `(list_membership)` | Done |
| `PRINT_CONFIG_CLASS_DEFINE` blocks in `.hpp` | Hand-written macros | Generated from `.proto` | Future |
| `Tab.cpp` `append_single_option_line()` layout | Hand-written per-setting calls | Generated from `layout.yaml` + `(tab_*)` annotations | Future |

### Stays manual (NOT generated)

| Component | Reason |
|---|---|
| Conditional visibility (`toggle_print_fff_options`) | Complex runtime logic depending on config values; cannot be declaratively expressed |
| Custom GUI rendering (`Field.cpp`, `OptionsGroup.cpp`) | Case-specific widget behavior (color pickers, special enums) |
| `handle_legacy()` | Migration logic; partially automatable via `(legacy_name)` but complex transforms stay manual |
| Enum C++ maps (top of `PrintConfig.cpp`) | Could eventually generate from proto enums |

---

## 6. Developer Workflow

### Adding a new setting

1. Add a field to the appropriate `.proto` file (`print.proto`, `filament.proto`, or `printer.proto`) with all relevant annotations
2. Run `python tools/run_codegen.py`
3. Commit the `.proto` change — the generated files are gitignored build output, never committed

### Adding a virtual preset key

Virtual keys are preset option keys that have no `ConfigOptionDef` (printer identity fields, connectivity settings, etc.) or that exist in one preset type's proto but also need to appear in another preset's options list.

1. Add `option (virtual_preset_keys) = "key_name";` in the appropriate `.proto` message body
2. Run `python tools/run_codegen.py`

### Running the codegen pipeline manually

```bash
# Full pipeline: compile protos → generate C++ → validate
python tools/run_codegen.py

# Validate only (check generated files are up to date)
python tools/run_codegen.py --validate-only

# Inject invalidation/list-membership annotations from Print.cpp / PrintConfig.cpp
python tools/annotate_protos.py [--dry-run]
```

---

## 7. File Layout

```
src/PrintConfigs/
├── config_metadata.proto              # Custom field/message option extensions
├── layout.yaml                        # UI tab/page/group structure (Tab.cpp layout)
├── print.proto                        # ~477 print/process settings
├── filament.proto                     # ~103 filament settings
└── printer.proto                      # ~42 printer/machine settings

tools/
├── run_codegen.py                     # Full pipeline script — the entry point everything calls
├── codegen_toolchain.py               # Resolves protoc / protobuf / pyyaml
├── config_codegen.py                  # Proto descriptor → C++ codegen
├── config_metadata.py                 # Reads the orca.* option extensions from the descriptor set
└── validate_codegen.py                # Generated vs original validation

src/slic3r/GUI/generated/              # gitignored — regenerated by every build
├── PrintConfigDef_generated.cpp       # init_fff_params() body — #included by PrintConfig.cpp
├── Preset_options_generated.cpp       # s_Preset_*_options — #included by Preset.cpp
├── Invalidation_generated.cpp         # s_print_steps_map + s_object_steps_map — #included by Print.cpp
├── OptionKeys_generated.cpp           # s_extruder_option_keys, s_filament_option_keys
└── TabLayout_generated.cpp            # Tab page/group layout from layout.yaml

.codegen-tools/                        # gitignored — downloaded protoc + bootstrap virtualenv

cmake/modules/
└── ConfigCodegen.cmake                # CMake integration (build-time regeneration)

docs/
└── PrintConfig_Codegen_Design.md      # This design document
```
