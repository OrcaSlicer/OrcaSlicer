# Typed-Config Surface — Per-File Migration Guide

This is the reference doc for agents migrating GUI call sites from the runtime
`cfg.option<T>("k")` / `cfg.opt_int("k")` style to the typed surface in
`engine/include/orca/Config.hpp` + `engine/include/orca/ConfigKeys.hpp`.

## What you are doing

Rewriting **literal-key** typed-config call sites in ONE file. Variable-key
sites (where the key is a `std::string` parameter) are NOT in scope — leave
those alone. The catalog at `engine/scripts/catalog/gui_usage.raw.tsv` is the
authoritative list of literal-key sites; your file's row count there is the
expected number of rewrites.

## Receiver classification → choose Adhoc vs Scoped API

Look at the receiver expression directly to the left of `.option<>` /
`.opt_*`:

| Receiver shape | API |
|---|---|
| Local `DynamicPrintConfig` / `DynamicPrintConfig&` / `DynamicPrintConfig*` param | `::orca::config::get<::orca::keys::K>(cfg)` |
| `m_plater->config()` or similar method returning `DynamicPrintConfig*` | `::orca::config::get<::orca::keys::K>(*cfg)` |
| `::orca::session().presets().raw_ptr()->full_config()` | Adhoc on the returned config |
| `::orca::session().presets().raw_ptr()->project_config` | `::orca::session().presets().get<::orca::keys::K>(::orca::ConfigScope::Project)` |
| `::orca::session().presets().raw_ptr()->prints.get_edited_preset().config` | `Scope::PrintPreset` |
| `::orca::session().presets().raw_ptr()->printers.get_edited_preset().config` | `Scope::PrinterPreset` |
| `::orca::session().presets().raw_ptr()->filaments.get_edited_preset().config` | `Scope::FilamentPreset` |
| Anything else / unclear receiver | **Leave the site alone, add `// TODO(orca-types):` comment with reason** |

**Member/local alias note**: some files cache `PresetBundle*` into a member or
local before dereferencing (e.g. `Tab.cpp` has `m_preset_bundle = ::orca::session().presets().raw_ptr();`
and then calls `m_preset_bundle->printers.get_edited_preset().config`). Treat
the alias EXACTLY like the raw `::orca::session().presets().raw_ptr()` path
for purposes of Scope mapping — `m_preset_bundle->printers...` → PrinterPreset,
`m_preset_bundle->prints...` → PrintPreset, etc.

**Default to Adhoc** when the call site already holds a `DynamicPrintConfig`
lvalue — there's no benefit to going through Scoped.

## Call-shape rewrites

`K` = `::orca::keys::<key_name>` (key name from the string literal in the call site).

**Scalar read** (the dominant case):
```cpp
cfg.option<ConfigOptionFloat>("layer_height")->value
// →
::orca::config::get<::orca::keys::layer_height>(cfg).value_or(0.0)

cfg.opt_float("layer_height")
// →
::orca::config::get<::orca::keys::layer_height>(cfg).value_or(0.0)
```

**Vector element read** (per-index access of a `coXXXs` key):
```cpp
cfg.option<ConfigOptionFloats>("nozzle_diameter")->values[idx]
// →
::orca::config::get_at<::orca::keys::nozzle_diameter>(cfg, idx).value_or(0.0)

cfg.opt_float("nozzle_diameter", idx)   // scalar-read of vector key
// →
::orca::config::get_at<::orca::keys::nozzle_diameter>(cfg, idx).value_or(0.0)

cfg.opt_int("filament_extruder_id", idx)
// →
::orca::config::get_at<::orca::keys::filament_extruder_id>(cfg, idx).value_or(0)
```

**Full vector read**:
```cpp
cfg.option<ConfigOptionFloats>("nozzle_diameter")->values
// →
::orca::config::get_vec<::orca::keys::nozzle_diameter>(cfg).value_or(std::vector<double>{})
```

If the returned vector is captured by const reference (`const auto& vs = ...->values`),
the typed surface returns by value, so change to `auto vs = ...` (no `&`).
If the site only reads `->size()` or iterates, swap to the optional vector and
do `vec ? vec->size() : 0` / `for (auto v : vec.value_or(std::vector<T>{}))`.

**Scalar write** (auto-creates the option):
```cpp
cfg.option<ConfigOptionInt>("k", true)->value = X
// →
::orca::config::put<::orca::keys::k>(cfg, X)
```

**Vector write**:
```cpp
cfg.option<ConfigOptionFloats>("k", true)->values = std::move(vec)
// →
::orca::config::put_vec<::orca::keys::k>(cfg, std::move(vec))
```

**Vector element write**:
```cpp
cfg.option<ConfigOptionFloats>("k", true)->set_at(idx, v)
cfg.option<ConfigOptionFloats>("k", true)->values[idx] = v
// →
::orca::config::put_at<::orca::keys::k>(cfg, idx, v)
```

## Default values for `.value_or(...)`

Pick the type-correct zero:
- `bool` → `false`
- `int` / `unsigned char` (Bools storage) → `0`
- `double` (Float/Floats/Percent/etc.) → `0.0`
- `std::string` → `{}` (empty string)
- `Slic3r::FloatOrPercent` → `Slic3r::FloatOrPercent{0.0, false}`
- `Slic3r::Vec2d` → `Slic3r::Vec2d::Zero()`
- vector return → `std::vector<T>{}`

**Only change the default if the original code had explicit fallback logic.**
If the original was `cfg.opt_float("k")` with no fallback, the original would
have crashed on missing key — preserve that contract by using `value_or(0.0)`
(or whatever zero matches the type). If you see existing fallback handling
nearby (e.g. `if (cfg.has("k")) ...`), keep it intact and put the typed call
inside it.

## Required include

Add at the top of the file (after the libslic3r includes, before any
project-local Slic3r/GUI includes), if not already present:

```cpp
#include "orca/Config.hpp"
```

`orca/Config.hpp` pulls in `orca/ConfigKeys.hpp`, `orca/Presets.hpp`, and
`libslic3r/PrintConfig.hpp` transitively.

## Typed enum surface (NEW — enums are now migratable)

The typed surface now has enum helpers in `orca/Config.hpp`. The caller names
the C++ enum type E (ConfigKeys.hpp doesn't know it):

```cpp
// scalar enum read: cfg.option<ConfigOptionEnum<BedType>>("curr_bed_type")->value
//                   OR cfg.opt_enum<BedType>("curr_bed_type")
::orca::config::get_enum<::orca::keys::curr_bed_type, BedType>(cfg).value_or(static_cast<BedType>(0))

// vector enum read (coEnums / ConfigOptionEnumsGeneric), whole vector:
::orca::config::get_enums<::orca::keys::nozzle_volume_type, NozzleVolumeType>(cfg).value_or(std::vector<NozzleVolumeType>{})

// vector enum element: opt->get_at(i) cast to NozzleVolumeType
::orca::config::get_enum_at<::orca::keys::nozzle_volume_type, NozzleVolumeType>(cfg, i).value_or(static_cast<NozzleVolumeType>(0))

// scalar enum write (needs DynamicConfig&):
::orca::config::put_enum<::orca::keys::curr_bed_type, BedType>(cfg, BedType::btPEI);
```

Determine E from the original call: `option<ConfigOptionEnum<E>>(...)` or
`opt_enum<E>(...)` names E directly; for `ConfigOptionEnumsGeneric` sites that
cast elements like `static_cast<NozzleVolumeType>(opt->get_at(i))`, E is the
cast target. Use the enum's natural zero (`static_cast<E>(0)`) as the
`value_or` default to preserve the prior crash-on-missing contract loosely.

If a site captures the option POINTER and reuses it (`->size()`, multiple
`->get_at(i)`, mutation), still prefer migrating reads to `get_enums`/
`get_enum_at`, but if that orphans downstream pointer ops, SKIP with TODO.

## Hard skip list — DO NOT migrate

1. **Variable-key sites**: anything where the key argument is not a string
   literal (`opt_float(key)`, `option<T>(opt_key)`, etc.). These will be in
   `engine/scripts/catalog/gui_usage.varkey.tsv` and are out of scope.

2. ~~Typed-enum reads~~ — NOW MIGRATABLE via the typed enum surface above.
   Use get_enum / get_enums / get_enum_at. Only skip if the site captures the
   option pointer and reuses it in a way the value-returning API can't express.

3. **Nullable reads**: `option<ConfigOptionFloatsNullable>("k")` — the
   nullable semantics aren't fully wired yet. Skip and mark.

4. **Unknown keys** (the 13 keys not in ConfigDef):
   `tag_uid`, `tray_name`, `filament_id`, `filament_exist`, `bed_temperature`,
   `bed_temperature_initial_layer`, `cooling`, `max_print_speed`,
   `max_volumetric_speed`, `support_material_auto`,
   `filament_long_retractions_when_cut`, `filament_prime_volume`,
   `filament_retraction_distances_when_cut`. These don't have key tags;
   compile would fail. Skip and mark.

5. **By-reference captures of the underlying ConfigOption** that mutate
   through `&`: e.g. `auto& opt = *cfg.option<ConfigOptionFloats>("k");
   opt.values.push_back(...)`. The typed `get_vec` returns by value, so the
   semantics differ. Skip and mark.

6. **libslic3r internal calls**: anything under `src/libslic3r/`. Your file
   should not be there — if you find yourself there, stop.

For every skip, add a comment on the line:
```cpp
// TODO(orca-types): manual migration — <one-line reason>
```

## Process

1. `Read` the assigned file in full.
2. `Read` `engine/include/orca/Config.hpp` and skim — you'll need to know
   exactly which template names exist (`get`, `get_at`, `get_vec`, `put`,
   `put_at`, `put_vec`).
3. `Grep` the file for `.option<\|\.opt_int\|\.opt_float\|\.opt_string\|\.opt_bool\|\.opt_enum\|\.opt_pct` to find every call site.
4. Cross-check against the catalog rows for your file in
   `engine/scripts/catalog/gui_usage.raw.tsv` — the count should match.
5. For each site, classify the receiver shape, pick Adhoc or Scoped, pick the
   right template (get / get_at / get_vec / put / put_at / put_vec), and
   write the rewrite.
6. Add `#include "orca/Config.hpp"` if absent.
7. Edit each site with the `Edit` tool.
8. Run a syntax sanity check by re-reading 5 lines around each edit; confirm
   the rewrite is well-formed.
9. **Do not run cmake/build/ctest** — the orchestrator will batch-verify
   after the whole wave lands.

## Hard constraints (sub-agent rules)

- **NEVER `git stash`** (or any `git stash *` variant). EVER. NO EXCEPTIONS.
  If you think you need to inspect pre-edit state, use `git diff HEAD <path>`
  or `git show HEAD:<path>`.
- **NEVER hand-edit any package manifest** (`CMakeLists.txt`, `package.json`,
  `Cargo.toml`, etc.). Your job is source-only.
- **NEVER look up versions of anything** — you're not adding deps.
- **Do not run cmake / make / ninja / ctest.** Orchestrator verifies.
- **Do not introduce new abstractions** — just rewrite the call sites
  one-for-one.
- **Do not refactor surrounding code.** Tight diff.
- **Do not delete comments** unless they explicitly describe the
  pre-migration call shape.
- **Preserve semantics**: the rewritten code must produce the same runtime
  behavior. When in doubt, skip with a TODO comment.

## Output format (when you finish)

Reply with:

1. Total sites in the file (per catalog)
2. Number migrated successfully
3. Number skipped with TODO (and one-line reason for each)
4. Any anomalies you noticed (mismatched types, dead code, etc.)
