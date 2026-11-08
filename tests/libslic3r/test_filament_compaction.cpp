#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/FilamentCompaction.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>
#include "libslic3r/Config.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

namespace {

// One object per 1-based filament. Only instances marked Inside count toward the plate.
Model model_using(const std::vector<int>& filaments_1based)
{
    Model model;
    for (int filament : filaments_1based) {
        ModelObject* object = model.add_object();
        object->add_volume(make_cube(10, 10, 10), ModelVolumeType::MODEL_PART, false);
        object->config.set("extruder", filament);
        ModelInstance* instance = object->add_instance();
        instance->print_volume_state = ModelInstancePVS_Inside;
    }
    return model;
}

// Every feature that could add a filament is off.
DynamicPrintConfig plain_config(size_t filament_count)
{
    DynamicPrintConfig config;
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("raft_layers", new ConfigOptionInt(0));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>(filament_count, "#FFFFFF")));
    config.set_key_value("filament_diameter", new ConfigOptionFloats(std::vector<double>(filament_count, 1.75)));
    return config;
}

ModelFilaments filaments_of(const Model& model, const DynamicPrintConfig& config_in)
{
    DynamicPrintConfig full = DynamicPrintConfig::full_print_config();
    full.apply(config_in);
    return Print::collect_model_filaments(model, full, FeatureGate::Printed, /*with_wipe_tower=*/true);
}

FilamentCompaction compaction_of(const Model& model, const DynamicPrintConfig& config, size_t namespace_size,
                                 const std::vector<int>& demoted = {})
{
    return build_filament_compaction(filaments_of(model, config), demoted, config, namespace_size);
}

// plain_config with project filament `mix_1based` a mix of `components` ("1,3": 1-based).
DynamicPrintConfig mixed_config(size_t filament_count, int mix_1based, const std::string& components)
{
    DynamicPrintConfig config = plain_config(filament_count);
    std::vector<unsigned char> is_mixed(filament_count, 0);
    std::vector<std::string>   comps(filament_count, "");
    is_mixed[mix_1based - 1] = 1;
    comps[mix_1based - 1]    = components;
    config.set_key_value("filament_is_mixed", new ConfigOptionBools(is_mixed));
    config.set_key_value("filament_mixed_components", new ConfigOptionStrings(comps));
    return config;
}

} // namespace

TEST_CASE("Used filament slots are the plate's filaments, 0-based and ascending", "[FilamentCompaction]")
{
    const Model model = model_using({6, 3, 3});
    CHECK(compaction_of(model, plain_config(8), 32).used_slots == std::vector<int>{2, 5});
}

TEST_CASE("An object on another plate contributes no filament", "[FilamentCompaction]")
{
    Model model = model_using({3, 7});
    // Not inside the current plate's print volume -- exactly how Print::apply skips it.
    model.objects[1]->instances[0]->print_volume_state = ModelInstancePVS_Fully_Outside;
    CHECK(compaction_of(model, plain_config(8), 32).used_slots == std::vector<int>{2});
}

TEST_CASE("A plate is renumbered whenever a used filament is not on its own tool", "[FilamentCompaction]")
{
    CHECK(compaction_of(model_using({1, 2}), plain_config(8), 4).is_identity());
    CHECK(compaction_of(model_using({4, 7}), plain_config(8), 32).is_identity());
    CHECK(compaction_of(model_using({4, 7}), plain_config(8), 7).is_identity());

    // 4 keeps tool 4, 7 takes the free tool 1; tools 2 and 3 hold the unused filaments 2 and 3.
    const FilamentCompaction compaction = compaction_of(model_using({4, 7}), plain_config(8), 4);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{6, 1, 2, 3});
    CHECK(compaction.tool_of_slot(3) == 3);
    CHECK(compaction.tool_of_slot(6) == 0);
    CHECK(compaction.tool_of_slot(0) == -1);
}

TEST_CASE("A plate's manual map is honoured and a stray one in automatic mode is not", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(8);
    config.set_key_value("filament_map", new ConfigOptionInts({3, 1, 2, 4, 5, 6, 7, 8}));
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(FilamentMapMode::fmmManual));
    // filament 1 -> tool 3, filament 3 -> tool 2 (pinned); filament 2 takes the free tool 1.
    CHECK(compaction_of(model_using({1, 2, 3}), config, 4).slot_of_tool == std::vector<int>{1, 2, 0});

    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(FilamentMapMode::fmmAutoForFlush));
    CHECK(compaction_of(model_using({1, 2, 3}), config, 4).is_identity());
}

TEST_CASE("A g-code tool number maps back to the project filament it prints", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = compaction_of(model_using({4, 7}), plain_config(8), 4);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{6, 1, 2, 3});
    CHECK(compaction.project_slot_of_tool(0) == 6);
    CHECK(compaction.project_slot_of_tool(3) == 3);
    // A tool holding an unused filament reports that filament.
    CHECK(compaction.project_slot_of_tool(1) == 1);

    // Without compaction the tool number is the project slot.
    const FilamentCompaction identity;
    REQUIRE(identity.slot_of_tool.empty());
    CHECK(identity.project_slot_of_tool(0) == 0);
    CHECK(identity.project_slot_of_tool(5) == 5);

    // An uncovered or negative tool passes through for the caller's own range check.
    CHECK(compaction.project_slot_of_tool(5) == 5);
    CHECK(compaction.project_slot_of_tool(-1) == -1);

    // A filler tool reports the first used filament, whose column it copies.
    FilamentCompaction filled;
    filled.slot_of_tool = {0, -1, -1, 1};
    CHECK(filled.project_slot_of_tool(2) == 0);
}

TEST_CASE("Compaction renumbers the model's filament references", "[FilamentCompaction]")
{
    Model model = model_using({4, 7});
    model.objects[0]->volumes[0]->config.set("extruder", 4);
    model.objects[1]->config.set("support_filament", 7);

    // Filament 4 keeps tool 4; filament 7 takes the free tool 1.
    const FilamentCompaction compaction = compaction_of(model, plain_config(8), 4);
    apply_filament_compaction(model, model, compaction);

    CHECK(model.objects[0]->config.option("extruder")->getInt() == 4);
    CHECK(model.objects[0]->volumes[0]->config.option("extruder")->getInt() == 4);
    CHECK(model.objects[1]->config.option("extruder")->getInt() == 1);
    CHECK(model.objects[1]->config.option("support_filament")->getInt() == 1);
}

TEST_CASE("Compaction gathers per-filament config vectors in dense order", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(4);
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#111111", "#222222", "#333333"}));
    config.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PETG", "ABS", "TPU"}));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({200, 240, 260, 220}));
    // Feature references are 1-based.
    config.set_key_value("support_filament", new ConfigOptionInt(4));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {1, 3};

    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{"#111111", "#333333"});
    CHECK(config.option<ConfigOptionStrings>("filament_type")->values == std::vector<std::string>{"PETG", "TPU"});
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{240, 220});
    CHECK(config.option<ConfigOptionInt>("support_filament")->value == 2);
}

TEST_CASE("Compaction gathers every per-head flush block on a multi-head printer", "[FilamentCompaction]")
{
    // G-code export validates the matrix size as heads * f^2.
    DynamicPrintConfig config = plain_config(3);
    // Two heads, 3x3 each. Entry = 100*head + 10*source + target.
    config.set_key_value("flush_volumes_matrix",
                         new ConfigOptionFloats({  0,   1,   2,  10,  11,  12,  20,  21,  22,
                                                 100, 101, 102, 110, 111, 112, 120, 121, 122}));
    // Load/unload pair per filament: {f0_load, f0_unload, f1_load, ...}.
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({10, 11, 20, 21, 30, 31}));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {0, 2};
    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values ==
          std::vector<double>{0, 2, 20, 22, 100, 102, 120, 122});
    CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values ==
          std::vector<double>{10, 11, 30, 31});
}

TEST_CASE("Recompacting an unchanged source leaves every timestamp unchanged", "[FilamentCompaction]")
{
    // The compacted model is rebuilt on every Print::apply; a fresh timestamp would read as an
    // edit and invalidate the slice, looping forever.
    Model model = model_using({4, 7});
    const DynamicPrintConfig config = plain_config(8);

    Model first  = model;
    Model second = model;
    apply_filament_compaction(first, model, compaction_of(model, config, 4));
    apply_filament_compaction(second, model, compaction_of(model, config, 4));

    // ModelConfigObject hides timestamp() at its own level; the ModelConfig base exposes it.
    auto config_stamp = [](const ModelObject* object) {
        return static_cast<const ModelConfig&>(object->config).timestamp();
    };
    for (size_t i = 0; i < model.objects.size(); ++i) {
        CHECK(config_stamp(first.objects[i]) == config_stamp(model.objects[i]));
        CHECK(config_stamp(second.objects[i]) == config_stamp(model.objects[i]));
    }
}

TEST_CASE("A reference to a filament the plate does not print becomes inherit", "[FilamentCompaction]")
{
    // Support is off, so filament 6 never prints and gets no tool (filaments 2 and 3 fill the
    // holes); keeping "6" would name whichever filament holds that tool number.
    Model model = model_using({4, 7});
    model.objects[0]->config.set("support_filament", 6);
    const FilamentCompaction compaction = compaction_of(model, plain_config(8), 4);
    apply_filament_compaction(model, model, compaction);
    CHECK(model.objects[0]->config.option("support_filament")->getInt() == 0);
}

TEST_CASE("Filaments are grouped by how sure it is that they print", "[FilamentCompaction]")
{
    Model model = model_using({4});
    DynamicPrintConfig config = plain_config(8);
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_filament", new ConfigOptionInt(2));
    config.set_key_value("top_surface_filament_id", new ConfigOptionInt(7));
    const ModelFilaments filaments = filaments_of(model, config);
    CHECK(filaments.parts == std::vector<unsigned int>{ 3 });
    CHECK(std::count(filaments.features.begin(), filaments.features.end(), 6u) == 1);
    CHECK(filaments.support == std::vector<unsigned int>{ 1 });
}

TEST_CASE("A renumbered plate numbers part filaments first and support last", "[FilamentCompaction]")
{
    // Support may generate nothing, so it must not push a part filament past the last tool. A slot
    // the last slice showed never prints (`demoted`) is numbered last.
    const auto [demoted, slot_of_tool] = GENERATE(table<std::vector<int>, std::vector<int>>({
        { {},    { 4, 5, 2, 7, 1 } },
        { { 2 }, { 4, 1, 5, 7, 2 } },
    }));
    Model model = model_using({3, 5, 6, 8});
    DynamicPrintConfig config = plain_config(8);
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_filament", new ConfigOptionInt(2));
    const FilamentCompaction compaction = compaction_of(model, config, 4, demoted);
    CHECK(compaction.slot_of_tool == slot_of_tool);
    CHECK(compaction.certain_count == 4);   // a count, not a prefix
    CHECK(compaction.used_slots == std::vector<int>{ 1, 2, 4, 5, 7 });
}

TEST_CASE("A plate that fits the namespace keeps its numbers but still knows its filaments", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = compaction_of(model_using({1, 3}), plain_config(8), 4);
    CHECK(compaction.slot_of_tool.empty());
    CHECK(compaction.used_slots == std::vector<int>{ 0, 2 });
    CHECK(compaction.certain_count == 2);
}

// A mix is a virtual slot: ToolOrdering prints its components, never the slot itself.
TEST_CASE("A mixed slot is used through its components", "[FilamentCompaction]")
{
    const Model model = model_using({5});
    CHECK(compaction_of(model, mixed_config(6, 5, "1,3"), 32).used_slots == std::vector<int>{0, 2});
}

TEST_CASE("A used mix is numbered after the physical tools", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = compaction_of(model_using({5}), mixed_config(6, 5, "1,3"), 2);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{0, 2, 4});
    CHECK(compaction.tool_of_slot(4) == 2);
}

TEST_CASE("Four physical filaments and their mixes need no renumbering", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = compaction_of(model_using({1, 2, 3, 4, 5}), mixed_config(5, 5, "2,4"), 4);
    CHECK(compaction.slot_of_tool.empty());
}

TEST_CASE("Compaction renumbers a mix's components", "[FilamentCompaction]")
{
    DynamicPrintConfig config = mixed_config(6, 5, "1,3");
    FilamentCompaction compaction;
    compaction.slot_of_tool = {0, 2, 4};
    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionBools>("filament_is_mixed")->values == std::vector<unsigned char>{0, 0, 1});
    CHECK(config.option<ConfigOptionStrings>("filament_mixed_components")->values == std::vector<std::string>{"", "", "1,2"});
}

TEST_CASE("A painted mix is renumbered in the copy's painted-filament list", "[FilamentCompaction]")
{
    Model         model  = model_using({1});
    ModelVolume*  volume = model.objects[0]->volumes[0];
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder8);
    volume->mmu_segmentation_facets.set_data(selector.serialize());
    // get_extruders caches by the painting's timestamp; prime it so the copy inherits a stale cache.
    REQUIRE(volume->get_extruders() == std::vector<int>{8, 1});

    const DynamicPrintConfig config = mixed_config(8, 8, "4,5");
    CHECK(compaction_of(model, config, 32).used_slots == std::vector<int>{0, 3, 4});

    // Three tools: 1 keeps tool 1, 4 and 5 take tools 2 and 3, the mix follows the physical tools.
    const FilamentCompaction compaction = compaction_of(model, config, 3);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{0, 3, 4, 7});

    Model copy = model;
    apply_filament_compaction(copy, model, compaction);
    CHECK(copy.objects[0]->volumes[0]->get_extruders() == std::vector<int>{4, 1});
}

// resolve_filament_tools: `order` is 0-based used slots, surest first; result is slot_of_tool.
TEST_CASE("A used filament within the tools keeps its own tool", "[FilamentCompaction]")
{
    // 3 keeps T2 (0-based), 5 takes the lowest free tool; T1 is filled with the unused filament 2.
    CHECK(resolve_filament_tools({2, 4}, {}, {}, 4, 8) == std::vector<int>{4, 1, 2});
    CHECK(resolve_filament_tools({4, 2}, {}, {}, 4, 8) == std::vector<int>{4, 1, 2});
    // The U1 plate {1,2,3,5}: filament 5 takes the free tool 4.
    CHECK(resolve_filament_tools({0, 1, 2, 4}, {}, {}, 4, 8) == std::vector<int>{0, 1, 2, 4});
    CHECK(resolve_filament_tools({1, 3, 5, 7}, {}, {}, 4, 8) == std::vector<int>{5, 1, 7, 3});
    CHECK(resolve_filament_tools({4, 5, 6, 7}, {}, {}, 4, 8) == std::vector<int>{4, 5, 6, 7});
    // Nothing to do when every filament is on its own tool.
    CHECK(resolve_filament_tools({0, 1, 2}, {}, {}, 4, 8) == std::vector<int>{0, 1, 2});
}

TEST_CASE("A manual entry pins a filament to its tool and the rest take free tools", "[FilamentCompaction]")
{
    // Filament 2 pinned to tool 3; 1 keeps tool 1; 3 takes the lowest free tool 2; 5 takes tool 4.
    std::vector<int> manual(8, -1);
    manual[1] = 2;
    CHECK(resolve_filament_tools({0, 1, 2, 4}, manual, {}, 4, 8) == std::vector<int>{0, 2, 1, 4});
    // An entry past the last tool is automatic.
    manual.assign(8, -1);
    manual[4] = 9;
    CHECK(resolve_filament_tools({0, 1, 2, 4}, manual, {}, 4, 8) == std::vector<int>{0, 1, 2, 4});
    // Entries of filaments the plate does not use are ignored.
    manual.assign(8, -1);
    manual[7] = 0;
    CHECK(resolve_filament_tools({0, 1}, manual, {}, 4, 8) == std::vector<int>{0, 1});
}

TEST_CASE("Two used filaments pinned to one tool are refused", "[FilamentCompaction]")
{
    std::vector<int> manual(8, -1);
    manual[0] = 1;
    manual[2] = 1;
    CHECK_THROWS_AS(resolve_filament_tools({0, 2}, manual, {}, 4, 8), DuplicateToolAssignment);
}

TEST_CASE("A hole below the highest tool is filled with an unused filament or left as a filler", "[FilamentCompaction]")
{
    // Two project filaments, filament 2 pinned to tool 4: nothing is left for tools 2 and 3.
    std::vector<int> manual(2, -1);
    manual[1] = 3;
    CHECK(resolve_filament_tools({0, 1}, manual, {}, 4, 2) == std::vector<int>{0, -1, -1, 1});
}

TEST_CASE("Filaments beyond the tool count are numbered past the last tool, least sure last", "[FilamentCompaction]")
{
    CHECK(resolve_filament_tools({0, 1, 2, 3, 4}, {}, {}, 4, 8) == std::vector<int>{0, 1, 2, 3, 4});
    // The least sure slot overflows even when its own tool is free.
    CHECK(resolve_filament_tools({0, 1, 2, 5, 3}, {}, {}, 4, 8) == std::vector<int>{0, 1, 2, 5, 3});
    // Slot 3 was demoted (it never printed last time): it loses its own tool and overflows.
    CHECK(resolve_filament_tools({0, 1, 2, 3, 5}, {}, {3}, 4, 8) == std::vector<int>{0, 1, 2, 5, 3});
    CHECK(resolve_filament_tools({0, 1, 2, 3, 5}, {}, {}, 4, 8) == std::vector<int>{0, 1, 2, 3, 5});
}

// The slice info stores a 1-based extruder per project filament, so the plate's assignment is
// published in that shape; a filament with no tool reports extruder 1.
TEST_CASE("The resolved assignment is published per project filament", "[FilamentCompaction]")
{
    FilamentCompaction compaction;
    compaction.slot_of_tool = {4, 1, 2, 3};
    compaction.used_slots   = {3, 4};
    CHECK(compaction.project_filament_map(8, 4) == std::vector<int>{1, 2, 3, 4, 1, 1, 1, 1});

    const FilamentCompaction identity;
    CHECK(identity.project_filament_map(6, 4) == std::vector<int>{1, 2, 3, 4, 1, 1});
}

// The engine prints an object with no filament of its own with filament 1, read as tool 0.
TEST_CASE("An object without a filament of its own is pinned to filament 1's tool", "[FilamentCompaction]")
{
    Model model = model_using({1, 2});
    model.objects[0]->config.set("extruder", 0);   // 0 = no filament of its own
    DynamicPrintConfig config = plain_config(4);
    config.set_key_value("filament_map", new ConfigOptionInts({3, 1, 2, 4}));
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(FilamentMapMode::fmmManual));

    const FilamentCompaction compaction = compaction_of(model, config, 4);
    REQUIRE(compaction.tool_of_slot(0) == 2);
    apply_filament_compaction(model, model, compaction);
    CHECK(model.objects[0]->config.option("extruder")->getInt() == 3);
    // An object that names its filament is renumbered, not pinned.
    CHECK(model.objects[1]->config.option("extruder")->getInt() == 1);
}
