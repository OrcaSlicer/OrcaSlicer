#include <catch2/catch_all.hpp>

#include "libslic3r/CompatibilityPolicy.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace Slic3r;

namespace {

// Build a full config (all option keys present) with the given filament_type
// array. Used as either the project (full) config or the user's current config.
DynamicPrintConfig make_config(const std::vector<std::string>& filament_types)
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("filament_type", new ConfigOptionStrings(filament_types));
    return cfg;
}

bool contains(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

} // namespace

TEST_CASE("filter keeps process and filament keys and drops printer keys", "[CompatibilityPolicy]")
{
    DynamicPrintConfig full = make_config({"PLA", "PETG"});
    DynamicPrintConfig user = make_config({"PLA", "PETG"});

    // Representative process keys.
    full.set("wall_loops", 4);
    full.set("layer_height", 0.2);
    full.set_deserialize_strict("sparse_infill_density", "15%");
    // Representative filament keys.
    full.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    full.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable({1.0, 1.0}));
    // Printer / machine keys are already present in full_print_config()
    // (nozzle_diameter, printable_area, gcode_flavor, machine_start_gcode).

    const auto result = CompatibilityPolicy::filter(full, user);

    // Process keys are carried into the payload.
    REQUIRE(result.payload.has("wall_loops"));
    REQUIRE(result.payload.has("layer_height"));
    REQUIRE(result.payload.has("sparse_infill_density"));
    // Filament keys are carried (both slots match the user's types).
    REQUIRE(result.payload.has("filament_type"));
    REQUIRE(result.payload.has("filament_diameter"));
    REQUIRE(result.payload.has("filament_retraction_length"));

    // Printer / machine keys are never carried.
    REQUIRE_FALSE(result.payload.has("nozzle_diameter"));
    REQUIRE_FALSE(result.payload.has("printable_area"));
    REQUIRE_FALSE(result.payload.has("gcode_flavor"));
    REQUIRE_FALSE(result.payload.has("machine_start_gcode"));
    // ... and are reported as dropped.
    REQUIRE(contains(result.dropped, "nozzle_diameter"));
    REQUIRE(contains(result.dropped, "printable_area"));
    REQUIRE(contains(result.dropped, "gcode_flavor"));
    REQUIRE(contains(result.dropped, "machine_start_gcode"));
}

TEST_CASE("filter drops bookkeeping keys", "[CompatibilityPolicy]")
{
    DynamicPrintConfig full = make_config({"PLA"});
    DynamicPrintConfig user = make_config({"PLA"});

    full.set_key_value("compatible_printers", new ConfigOptionStrings({"any"}));
    full.set("inherits", "Base", true);
    full.set("print_settings_id", "0.20mm Standard", true);
    full.set_key_value("filament_settings_id", new ConfigOptionStrings({"Generic PLA"}));
    full.set("enable_filament_dynamic_map", true, true);

    const auto result = CompatibilityPolicy::filter(full, user);

    REQUIRE_FALSE(result.payload.has("compatible_printers"));
    REQUIRE_FALSE(result.payload.has("inherits"));
    REQUIRE_FALSE(result.payload.has("print_settings_id"));
    REQUIRE_FALSE(result.payload.has("filament_settings_id"));
    REQUIRE_FALSE(result.payload.has("enable_filament_dynamic_map"));
    REQUIRE(contains(result.dropped, "compatible_printers"));
    REQUIRE(contains(result.dropped, "inherits"));
    REQUIRE(contains(result.dropped, "print_settings_id"));
    REQUIRE(contains(result.dropped, "filament_settings_id"));
    REQUIRE(contains(result.dropped, "enable_filament_dynamic_map"));
}

TEST_CASE("filter drops cross-reference filament keys", "[CompatibilityPolicy]")
{
    DynamicPrintConfig full = make_config({"PLA"});
    DynamicPrintConfig user = make_config({"PLA"});

    full.set("support_filament", 0);
    full.set("support_interface_filament", 0);
    full.set_key_value("default_filament_profile", new ConfigOptionStrings({"Generic PLA"}));

    const auto result = CompatibilityPolicy::filter(full, user);

    REQUIRE_FALSE(result.payload.has("support_filament"));
    REQUIRE_FALSE(result.payload.has("support_interface_filament"));
    REQUIRE_FALSE(result.payload.has("default_filament_profile"));
    REQUIRE(contains(result.dropped, "support_filament"));
    REQUIRE(contains(result.dropped, "support_interface_filament"));
    REQUIRE(contains(result.dropped, "default_filament_profile"));
}

TEST_CASE("filter gates filament slots by matching filament type", "[CompatibilityPolicy]")
{
    // Slot 0 is PLA on both sides (matches); slot 1 is PETG vs ABS (does not).
    DynamicPrintConfig full = make_config({"PLA", "PETG"});
    DynamicPrintConfig user = make_config({"PLA", "ABS"});

    full.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 1.75}));
    full.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable({1.0, 2.0}));
    user.set_key_value("filament_diameter", new ConfigOptionFloats({1.75, 2.85}));
    user.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable({0.5, 0.5}));

    const auto result = CompatibilityPolicy::filter(full, user);

    // Only slot 0 (PLA) matched.
    REQUIRE(result.filament_slots.size() == 1);
    REQUIRE(result.filament_slots[0] == 0);

    // Matched slot 0 keeps the project value.
    auto* frl = result.payload.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(frl != nullptr);
    REQUIRE(frl->values[0] == 1.0);
    // Non-matched slot 1 is nil'd for a nullable option.
    REQUIRE(frl->is_nil(1));

    // Non-nullable filament option: non-matched slot falls back to the user's value.
    auto* fd = result.payload.option<ConfigOptionFloats>("filament_diameter");
    REQUIRE(fd != nullptr);
    REQUIRE(fd->values[0] == 1.75);
    REQUIRE(fd->values[1] == 2.85);
}

TEST_CASE("apply writes process and filament keys but never printer keys", "[CompatibilityPolicy]")
{
    DynamicPrintConfig payload;
    payload.set("wall_loops", 4, true);
    payload.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    // Defensive: a printer key present in the payload must be dropped.
    payload.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));

    DynamicPrintConfig user = make_config({"PLA"});

    DynamicPrintConfig out;
    const auto result = CompatibilityPolicy::apply(out, payload, user);

    REQUIRE(out.has("wall_loops"));
    REQUIRE(out.opt_int("wall_loops") == 4);
    REQUIRE(out.has("filament_type"));
    // Printer key never lands in out.
    REQUIRE_FALSE(out.has("nozzle_diameter"));
    REQUIRE(contains(result.applied, "wall_loops"));
    REQUIRE(contains(result.applied, "filament_type"));
    REQUIRE(contains(result.dropped, "nozzle_diameter"));
}

TEST_CASE("apply resizes filament arrays to the user's extruder count", "[CompatibilityPolicy]")
{
    DynamicPrintConfig payload;
    payload.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PLA"}));
    payload.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable({1.0, 2.0, 3.0, 4.0}));
    payload.set_key_value("bridge_speed", new ConfigOptionFloats({50, 60, 70, 80}));

    // User has 2 extruders (nozzle_diameter drives the extruder count).
    DynamicPrintConfig user = make_config({"PLA", "PLA"});
    user.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4}));

    DynamicPrintConfig out;
    const auto result = CompatibilityPolicy::apply(out, payload, user);

    // Filament array of length 4 is resized to the user's 2 extruders.
    auto* frl = out.option<ConfigOptionFloatsNullable>("filament_retraction_length");
    REQUIRE(frl != nullptr);
    REQUIRE(frl->values.size() == 2);
    REQUIRE(frl->values[0] == 1.0);
    REQUIRE(frl->values[1] == 2.0);

    // Process vector options are resized to the extruder count too.
    auto* bs = out.option<ConfigOptionFloats>("bridge_speed");
    REQUIRE(bs != nullptr);
    REQUIRE(bs->values.size() == 2);
    REQUIRE(bs->values[0] == 50);
    REQUIRE(bs->values[1] == 60);
}

TEST_CASE("apply fills extra per-extruder process slots with user values", "[CompatibilityPolicy]")
{
    DynamicPrintConfig payload;
    payload.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    // Per-extruder process vector (in print_options_with_variant) with fewer entries
    // than the user's extruder count.
    payload.set_key_value("bridge_speed", new ConfigOptionFloats({50}));

    // User has 3 extruders, each with its own bridge_speed value.
    DynamicPrintConfig user = make_config({"PLA"});
    user.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4}));
    user.set_key_value("bridge_speed", new ConfigOptionFloats({10, 20, 30}));

    DynamicPrintConfig out;
    const auto result = CompatibilityPolicy::apply(out, payload, user);

    auto* bs = out.option<ConfigOptionFloats>("bridge_speed");
    REQUIRE(bs != nullptr);
    REQUIRE(bs->values.size() == 3);
    // Slot 0 keeps the payload value; the extra slots inherit the user's own values
    // rather than being filled with defaults.
    REQUIRE(bs->values[0] == 50);
    REQUIRE(bs->values[1] == 20);
    REQUIRE(bs->values[2] == 30);
}

TEST_CASE("apply does not resize non-per-extruder process vectors", "[CompatibilityPolicy]")
{
    DynamicPrintConfig payload;
    payload.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    // post_process is a variable-length process vector (coStrings), NOT per-extruder.
    payload.set_key_value("post_process", new ConfigOptionStrings({"cmd1", "cmd2"}));

    // User has 4 extruders.
    DynamicPrintConfig user = make_config({"PLA"});
    user.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4, 0.4, 0.4, 0.4}));

    DynamicPrintConfig out;
    const auto result = CompatibilityPolicy::apply(out, payload, user);

    // post_process keeps its original length and must NOT be resized to 4.
    auto* pp = out.option<ConfigOptionStrings>("post_process");
    REQUIRE(pp != nullptr);
    REQUIRE(pp->values.size() == 2);
    REQUIRE(pp->values[0] == "cmd1");
    REQUIRE(pp->values[1] == "cmd2");
    REQUIRE(contains(result.applied, "post_process"));
}

TEST_CASE("apply transfers non-critical filament settings on a type mismatch", "[CompatibilityPolicy]")
{
    // Project filament type is PLA, user's is PETG: no slot matches, so all
    // filament slots are gated off. Non-material-critical filament settings must
    // still be carried over for the user (the maximum-transfer boundary).
    DynamicPrintConfig payload;
    payload.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    payload.set_key_value("filament_flow_ratio", new ConfigOptionFloatsNullable({1.0}));
    payload.set_key_value("pressure_advance", new ConfigOptionFloats({0.05}));
    payload.set_key_value("filament_prime_volume", new ConfigOptionFloats({2.5}));
    payload.set_key_value("filament_loading_speed", new ConfigOptionFloats({30}));

    DynamicPrintConfig user = make_config({"PETG"});

    DynamicPrintConfig out;
    const auto result = CompatibilityPolicy::apply(out, payload, user);

    // These tuning / mechanical settings transfer across the type mismatch.
    REQUIRE(out.has("filament_flow_ratio"));
    REQUIRE_THAT(out.opt_float_nullable("filament_flow_ratio", 0), Catch::Matchers::WithinRel(1.0, 1e-6));
    REQUIRE(out.has("pressure_advance"));
    REQUIRE(out.has("filament_prime_volume"));
    REQUIRE(out.has("filament_loading_speed"));
    REQUIRE(contains(result.applied, "filament_flow_ratio"));
    REQUIRE(contains(result.applied, "pressure_advance"));
    REQUIRE(contains(result.applied, "filament_prime_volume"));
    REQUIRE(contains(result.applied, "filament_loading_speed"));
    // The material-critical filament_type (identity) is gated, but the non-critical
    // tuning keys must not be reported as material-dropped.
    REQUIRE_FALSE(contains(result.material_dropped, "filament_flow_ratio"));
    REQUIRE_FALSE(contains(result.material_dropped, "pressure_advance"));
    REQUIRE_FALSE(contains(result.material_dropped, "filament_prime_volume"));
    REQUIRE_FALSE(contains(result.material_dropped, "filament_loading_speed"));
    REQUIRE(contains(result.material_dropped, "filament_type"));
}

TEST_CASE("apply drops material-critical filament settings on a type mismatch", "[CompatibilityPolicy]")
{
    // Project filament type is PLA, user's is PETG: no slot matches. Material
    // identity, temperature, retraction, flow-cap and per-extruder-mapping keys
    // must be reported as material_dropped and never land in `out`.
    DynamicPrintConfig payload;
    payload.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    payload.set_key_value("nozzle_temperature", new ConfigOptionInts({210}));
    payload.set_key_value("filament_diameter", new ConfigOptionFloats({1.75}));
    payload.set_key_value("filament_retraction_length", new ConfigOptionFloatsNullable({1.0}));
    payload.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats({15}));
    payload.set_key_value("filament_extruder_variant", new ConfigOptionStrings({"HF 0.4 nozzle"}));

    DynamicPrintConfig user = make_config({"PETG"});

    DynamicPrintConfig out;
    const auto result = CompatibilityPolicy::apply(out, payload, user);

    // None of the material-critical keys land in the applied config.
    REQUIRE_FALSE(out.has("filament_type"));
    REQUIRE_FALSE(out.has("nozzle_temperature"));
    REQUIRE_FALSE(out.has("filament_diameter"));
    REQUIRE_FALSE(out.has("filament_retraction_length"));
    REQUIRE_FALSE(out.has("filament_max_volumetric_speed"));
    REQUIRE_FALSE(out.has("filament_extruder_variant"));
    // ... and each is reported separately so the UI can warn.
    REQUIRE(contains(result.material_dropped, "filament_type"));
    REQUIRE(contains(result.material_dropped, "nozzle_temperature"));
    REQUIRE(contains(result.material_dropped, "filament_diameter"));
    REQUIRE(contains(result.material_dropped, "filament_retraction_length"));
    REQUIRE(contains(result.material_dropped, "filament_max_volumetric_speed"));
    REQUIRE(contains(result.material_dropped, "filament_extruder_variant"));
    // Nothing that was material-dropped should also be reported as applied.
    for (const std::string& key : result.material_dropped)
        REQUIRE_FALSE(contains(result.applied, key));
}

TEST_CASE("serialize and deserialize payload round-trips values including percent", "[CompatibilityPolicy]")
{
    DynamicPrintConfig payload;
    payload.set("wall_loops", 4, true);
    payload.set("layer_height", 0.2, true);
    // coFloatOrPercent key set as a percentage.
    payload.set_deserialize_strict("initial_layer_line_width", "100%");

    const std::string json = CompatibilityPolicy::serialize_payload(payload);
    REQUIRE_FALSE(json.empty());

    std::vector<std::string> substituted;
    const DynamicPrintConfig reloaded = CompatibilityPolicy::deserialize_payload(json, substituted);

    REQUIRE(reloaded.has("wall_loops"));
    REQUIRE(reloaded.opt_int("wall_loops") == 4);
    REQUIRE(reloaded.has("layer_height"));
    REQUIRE_THAT(reloaded.opt_float("layer_height"), Catch::Matchers::WithinAbs(0.2, 1e-6));

    // The percent flag survives the round-trip.
    auto* ilw = reloaded.option<ConfigOptionFloatOrPercent>("initial_layer_line_width");
    REQUIRE(ilw != nullptr);
    REQUIRE(ilw->percent == true);
    REQUIRE_THAT(ilw->value, Catch::Matchers::WithinAbs(100.0, 1e-6));

    // No legacy renames for these current keys.
    REQUIRE(substituted.empty());
}
