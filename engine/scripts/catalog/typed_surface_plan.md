# Typed Config Surface — Catalog Plan

Generated 2026-05-29 during Phase 0.4d implementation.

## Catalog totals

- **ConfigDef truth (`PrintConfig.cpp`)**: 845 unique keys after dedupe, 850 raw
  - Distribution: 225 coFloat / 150 coBool / 84 coFloats / 73 coString / 69 coInt / 50 coEnum / 47 coInts / 44 coStrings / 35 coFloatOrPercent / 26 coPercent / 20 coBools / 8 coPoint / 7 coPoints / 7 coEnums / 4 coPercents / 1 coPointsGroups + Nullable variants
- **GUI usage**: 565 total typed-config call sites in `src/slic3r/`
  - 456 sites with string-literal keys → catalogued by tag/type/key
  - 125 sites with variable keys (function-parametric helpers)
  - 159 distinct keys actually touched by GUI (~19% of the 845 available)
  - 179 distinct (tag, type, key) tuples

## Receiver-scope distribution (wave-1, 3 hotspots = 295 sites)

| Receiver | Plater.cpp (158) | OptionsGroup.cpp (66) | PartPlate.cpp (71) |
|---|---|---|---|
| Adhoc | 49 (31%) | 65 (99%) | ~50 (70%) |
| Project | 37 (23%) | 1 | a few |
| PrinterPreset | 36 (23%) | 0 | a few |
| PrintPreset | 17 (11%) | 0 | mixed |
| Full | 11 (7%) | 0 | a few |
| FilamentPreset | 9 (6%) | 0 | a few |

**Key insight**: Adhoc receivers (function parameters / local `DynamicPrintConfig`) dominate the GUI usage. Plater.cpp is the most "scoped" file; UI plumbing (OptionsGroup, PartPlate) is overwhelmingly Adhoc. The typed surface must support BOTH `Presets::get<K>(Scope)` AND `orca::config::get<K>(cfg)` — the latter handles the majority case.

## Type mismatches (GUI vs ConfigDef)

Cross-reference of GUI requested types against ConfigDef declared types found:

- **31 mismatches** — most are scalar vs vector mismatches where the GUI uses `.opt_int("k")` for a `coInts` key. Libslic3r supports `.opt_int(key, idx)` for per-element scalar reads of vector options; the typed surface needs a `get_at<K>(scope, idx)` variant for this shape.
- **13 unknown keys** — GUI references keys not in PrintConfig.cpp's ConfigDef:
  - AppConfig keys, NOT PrintConfig: `tag_uid`, `tray_name`, `filament_id`, `filament_exist`
  - Removed from ConfigDef but still referenced (likely dead code or migrations): `bed_temperature`, `bed_temperature_initial_layer`, `cooling`, `max_print_speed`, `max_volumetric_speed`, `support_material_auto`
  - Possibly mid-migration or BBL legacy: `filament_long_retractions_when_cut`, `filament_prime_volume`, `filament_retraction_distances_when_cut`

These mismatches/unknowns are flagged here for follow-up but DO NOT block the typed surface — the surface uses ConfigDef truth, so mismatched call sites become compile-time errors when migrated.

## Shipped (Phase 0.4d)

### `engine/include/orca/ConfigKeys.hpp` (generated, 845 key tags)

Each key becomes a tag struct with `using type`, `static constexpr name`, `static constexpr CoType co_type`, `static constexpr bool is_vector`, `static constexpr bool is_nullable`.

```cpp
namespace orca::keys {
struct layer_height       { using type = double;       static constexpr std::string_view name = "layer_height";       static constexpr CoType co_type = CoType::Float;   /*…*/ };
struct nozzle_diameter    { using type = double;       static constexpr std::string_view name = "nozzle_diameter";    static constexpr CoType co_type = CoType::Floats;  static constexpr bool is_vector = true; /*…*/ };
struct filament_colour    { using type = std::string;  static constexpr std::string_view name = "filament_colour";    static constexpr CoType co_type = CoType::Strings; static constexpr bool is_vector = true; /*…*/ };
struct support_enabled    { using type = bool;         static constexpr std::string_view name = "enable_support";     static constexpr CoType co_type = CoType::Bool;    /*…*/ };
} // namespace orca::keys
```

Regenerate from ConfigDef with: `engine/scripts/catalog/regen_keys.sh`.

### `engine/include/orca/Config.hpp` (typed accessors)

Adhoc-receiver helpers (the dominant call shape):

```cpp
template <class K>
auto orca::config::get(const Slic3r::DynamicPrintConfig& cfg) -> std::optional<typename K::type>;

template <class K>
auto orca::config::get_vec(const Slic3r::DynamicPrintConfig& cfg) -> std::optional<std::vector<typename K::type>>;

template <class K>
auto orca::config::get_at(const Slic3r::DynamicPrintConfig& cfg, std::size_t idx) -> std::optional<typename K::type>;

template <class K>
void orca::config::put(Slic3r::DynamicPrintConfig& cfg, typename K::type value);

template <class K>
void orca::config::put_vec(Slic3r::DynamicPrintConfig& cfg, std::vector<typename K::type> values);

template <class K>
void orca::config::put_at(Slic3r::DynamicPrintConfig& cfg, std::size_t idx, typename K::type value);
```

Scoped Presets methods (define in same header to avoid circular include):

```cpp
template <class K>
auto Presets::get(ConfigScope scope) const -> std::optional<typename K::type>;

template <class K>
auto Presets::get_at(ConfigScope scope, std::size_t idx) const -> std::optional<typename K::type>;

template <class K>
auto Presets::get_vec(ConfigScope scope) const -> std::optional<std::vector<typename K::type>>;

template <class K>
Result<void> Presets::put(ConfigScope scope, typename K::type value);
```

### Verified end-to-end in `engine/cli/main.cpp`

Round-trip test added to the CLI canary: `get<layer_height>` returns 0.2, `put<layer_height>` round-trips, `get_vec<nozzle_diameter>` returns the per-extruder vector. CLI output:
```
orca-engine-cli: typed surface OK (layer_height=0.200, nozzle_diameter[0]=0.400)
```

## Phase 0.4d.5 — Call-site migration (DONE via parallel agent waves)

Migrated the 456 literal-key GUI call sites using parallel sub-agent waves
(one agent per file) driven by `engine/scripts/catalog/migration_guide.md`,
rather than a clang-tooling rewriter. Each agent classified the receiver
(Adhoc `DynamicPrintConfig` vs scoped preset path), picked the right typed
template, and rewrote in place; the orchestrator build-verified after each
wave. ~330+ sites migrated across 36 files; ~80 left as documented TODOs.

### Waves run
- **Wave 1** (20 agents, smallest files): ~43 sites
- **Wave 2** (12 agents, medium files): ~161 sites
- **Wave 3** (2 agents, Plater 89 + PartPlate 56): ~70 sites
- **Enum wave** (14 agents): ~45 sites once the enum surface shipped
- **Nullable/PointsGroups wave** (7 agents): ~15 sites once those traits shipped

### API gaps found during migration and closed
- **`get/get_at/get_vec` widened to `const ConfigBase&`** (was `DynamicPrintConfig&`)
  so `print.config()` (StaticPrintConfig) and plain `DynamicConfig` receivers work.
- **`co_traits<CoType::PointsGroups>`** added → `extruder_printable_area` etc. migratable.
- **Typed enum surface** added: `get_enum<K,E>`, `get_enums<K,E>`, `get_enum_at<K,E>`,
  `put_enum<K,E>`. Reads go through `ConfigOption::getInt()` so they work for both
  `ConfigOptionEnum<E>` and `ConfigOptionEnumGeneric`. Caller names E (the tag can't).
- **Nullable reads** need no new API: `ConfigOptionXNullable` inherits its non-nullable
  base, so `get_vec`/`get_at` dynamic_cast through the base and read values fine.

### Legitimately NOT migrated (~80 TODO(orca-types) markers) — these are correct
1. **~19 unknown / AppConfig keys** (`tag_uid`, `tray_name`, `filament_id`,
   `filament_exist`, `bed_temperature*`, `cooling`, `max_print_speed`,
   `max_volumetric_speed`, `support_material_auto`, `filament_prime_volume`,
   `filament_*_retractions_when_cut`): not in PrintConfig.cpp's ConfigDef — no key
   tag exists. Either AppConfig-namespace keys or removed-but-still-referenced
   (latent bugs). Cannot use the typed surface by definition.
2. **~40 in-place mutation / captured-pointer sites**: code that takes the
   `ConfigOption*` and mutates it (`->set_at`, `->values = …`, `->resize`,
   `->push_back`, `->deserialize`, by-ref `&` aliases reused across statements).
   The value-returning typed surface is the wrong tool; these legitimately use
   the `raw_ptr()` migration scaffold. Forcing them through the typed API would
   produce worse code.
3. **5 FloatOrPercent receivers**: `Flow::new_from_config_width(...)` takes a
   `const ConfigOptionFloatOrPercent&` (it calls `get_abs_value(ref_width)`),
   so the option object is required, not the value.
4. **2 `thumbnails` sites**: latent codebase inconsistency — PrintConfig.cpp
   registers `thumbnails` as `coString` (line 6956) but the SLA export path
   casts it to `ConfigOptionPoints`. Flagged, not papered over.

### Verification at completion
- `cmake --build … libslic3r_gui` + `orca-slicer` + `orca-engine-cli`: clean.
- `ctest`: 153/153 pass.
- `verify_no_wx.sh`: engine `.so` has zero wxWidgets coupling.
- CLI canary: `typed surface OK (layer_height=0.200, nozzle_diameter[0]=0.400)` + slices a cube.

### Known accuracy gap (non-blocking)
The generated `ConfigKeys.hpp` marks `filament_flow_ratio`, `nozzle_flush_dataset`,
`extruder_printable_height` as `is_nullable=false` because PrintConfig.cpp's
ConfigDef registers them with the non-nullable `coFloats`/`coInts` type, even
though the runtime instantiates the `*Nullable` subclass. Reads work (inheritance),
and `is_nullable` isn't consulted by the read templates, so this is cosmetic.
A future `gen_keys` pass could special-case these if the nil sentinel ever matters.

## Open follow-up

- **Unknown keys**: investigate the 13 keys GUI references but ConfigDef doesn't declare. May be AppConfig (separate namespace), removed-but-still-referenced (latent bug), or pseudo-keys.
- **Type mismatches**: 31 mostly-scalar-vs-vector mismatches need per-site review during rewriter pass — most are legitimate uses of `.opt_int(key, idx)` for vector keys, not bugs.
- **Nullable enum types**: `ConfigOptionEnumsGenericNullable` is mapped to `coEnums` but the nullable-ness needs round-trip via `NullableValue<T>`. Currently `get<K>` for nullable keys just returns `nullopt` if absent — matches consumer expectations but loses the explicit null sentinel.
- **Per-key enum E**: `coEnum` keys currently project to `int`. Real typed enums (`ConfigOptionEnum<E>`) need a `get_enum<K, E>` variant that returns `std::optional<E>` directly. Not blocking but worth adding next pass.
