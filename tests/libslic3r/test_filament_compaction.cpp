#include <catch2/catch_all.hpp>

#include "libslic3r/FilamentCompaction.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

// Orca: dense tool numbering for printers whose firmware has no macro past its last tool (the
// WonderMaker ZR Ultra S). The project may hold any number of filaments; a plate that uses at
// most tool_count of them must reach the printer as T0..T(n-1), which is what these transforms
// arrange -- see FilamentCompaction.hpp for why it happens at the top of Print::apply rather than
// at each point that emits a tool number.

namespace {

// A model with one printable object per requested 1-based filament. Instances must be marked
// Inside: Print::apply builds print objects only from printable instances, and used_filament_slots
// mirrors that so an object parked on another plate contributes no filaments.
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

// The minimum a plate needs for the collector's feature gates to read as "off".
DynamicPrintConfig plain_config(size_t filament_count)
{
    DynamicPrintConfig config;
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("raft_layers", new ConfigOptionInt(0));
    config.set_key_value("enable_prime_tower", new ConfigOptionBool(false));
    config.set_key_value("filament_colour", new ConfigOptionStrings(std::vector<std::string>(filament_count, "#FFFFFF")));
    return config;
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
    CHECK(used_filament_slots(model, plain_config(8)) == std::vector<int>{2, 5});
}

TEST_CASE("An object on another plate contributes no filament", "[FilamentCompaction]")
{
    Model model = model_using({3, 7});
    // Not inside the current plate's print volume -- exactly how Print::apply skips it.
    model.objects[1]->instances[0]->print_volume_state = ModelInstancePVS_Fully_Outside;
    CHECK(used_filament_slots(model, plain_config(8)) == std::vector<int>{2});
}

TEST_CASE("A plate already using a dense prefix needs no renumbering", "[FilamentCompaction]")
{
    // Identity is reported as empty so the caller skips the model copy entirely.
    const FilamentCompaction compaction = build_filament_compaction(model_using({1, 2}), plain_config(8));
    CHECK(compaction.slot_of_tool.empty());
    CHECK(compaction.is_identity());
}

TEST_CASE("A sparse plate compacts to dense tool numbers in slot order", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = build_filament_compaction(model_using({4, 7}), plain_config(8));
    REQUIRE(compaction.slot_of_tool == std::vector<int>{3, 6});
    CHECK(compaction.tool_of_slot(3) == 0);
    CHECK(compaction.tool_of_slot(6) == 1);
    // A slot the plate does not print has no tool.
    CHECK(compaction.tool_of_slot(0) == -1);
}

TEST_CASE("Compaction renumbers the model's filament references", "[FilamentCompaction]")
{
    Model model = model_using({4, 7});
    model.objects[0]->volumes[0]->config.set("extruder", 4);
    model.objects[1]->config.set("support_filament", 7);

    const FilamentCompaction compaction = build_filament_compaction(model, plain_config(8));
    apply_filament_compaction(model, model, compaction);

    CHECK(model.objects[0]->config.option("extruder")->getInt() == 1);
    CHECK(model.objects[0]->volumes[0]->config.option("extruder")->getInt() == 1);
    CHECK(model.objects[1]->config.option("extruder")->getInt() == 2);
    CHECK(model.objects[1]->config.option("support_filament")->getInt() == 2);
}

TEST_CASE("Compaction gathers per-filament config vectors in dense order", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(4);
    config.set_key_value("filament_colour", new ConfigOptionStrings({"#000000", "#111111", "#222222", "#333333"}));
    config.set_key_value("filament_type", new ConfigOptionStrings({"PLA", "PETG", "ABS", "TPU"}));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({200, 240, 260, 220}));
    // Feature references are 1-based and follow the same renumbering.
    config.set_key_value("support_filament", new ConfigOptionInt(4));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {1, 3}; // the plate prints project filaments 2 and 4

    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionStrings>("filament_colour")->values == std::vector<std::string>{"#111111", "#333333"});
    CHECK(config.option<ConfigOptionStrings>("filament_type")->values == std::vector<std::string>{"PETG", "TPU"});
    CHECK(config.option<ConfigOptionInts>("nozzle_temperature")->values == std::vector<int>{240, 220});
    CHECK(config.option<ConfigOptionInt>("support_filament")->value == 2);
}

TEST_CASE("Compaction gathers the flush volume matrix on both axes", "[FilamentCompaction]")
{
    DynamicPrintConfig config = plain_config(3);
    // Row = source filament, column = target. Entry value is 10*source + target, 0-based.
    config.set_key_value("flush_volumes_matrix",
                         new ConfigOptionFloats({0, 1, 2, 10, 11, 12, 20, 21, 22}));
    config.set_key_value("flush_volumes_vector", new ConfigOptionFloats({100, 200, 300}));

    FilamentCompaction compaction;
    compaction.slot_of_tool = {0, 2};
    apply_filament_compaction(config, compaction);

    CHECK(config.option<ConfigOptionFloats>("flush_volumes_matrix")->values == std::vector<double>{0, 2, 20, 22});
    CHECK(config.option<ConfigOptionFloats>("flush_volumes_vector")->values == std::vector<double>{100, 300});
}

TEST_CASE("Compaction gathers every per-head flush block on a multi-head printer", "[FilamentCompaction]")
{
    // On a multi-head printer the flush matrix is one filament x filament block PER HEAD
    // (g-code export validates size == flush_multiplier.size() * f^2), and flush_volumes_vector
    // carries the load/unload PAIR per filament. Leaving either at the project size while
    // filament_colour shrinks fails that validation and aborts export -- hit on a real
    // four-head ZR Ultra S the first time a plate used a sparse filament subset.
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
    // The compacted model is rebuilt from the source on EVERY Print::apply. Its config and
    // painting timestamps must mirror the source's: a fresh stamp on identical derived content
    // reads as a user edit, invalidates the slice that just finished, and the GUI loops
    // slice/discard forever with no visible error.
    Model model = model_using({4, 7});
    const DynamicPrintConfig config = plain_config(8);

    Model first  = model;
    Model second = model;
    apply_filament_compaction(first, model, build_filament_compaction(model, config));
    apply_filament_compaction(second, model, build_filament_compaction(model, config));

    // ModelConfigObject hides timestamp() at its own level; the ModelConfig base exposes it.
    auto config_stamp = [](const ModelObject* object) {
        return static_cast<const ModelConfig&>(object->config).timestamp();
    };
    for (size_t i = 0; i < model.objects.size(); ++i) {
        CHECK(config_stamp(first.objects[i]) == config_stamp(model.objects[i]));
        CHECK(config_stamp(second.objects[i]) == config_stamp(model.objects[i]));
    }
}

TEST_CASE("A filament the plate does not print keeps its number", "[FilamentCompaction]")
{
    // support_filament here points at a filament the plate never prints (support is off). Giving
    // it a dense tool would claim a tool the plate does not use; leaving it alone is correct.
    Model model = model_using({4, 7});
    model.objects[0]->config.set("support_filament", 2);
    const FilamentCompaction compaction = build_filament_compaction(model, plain_config(8));
    apply_filament_compaction(model, model, compaction);
    CHECK(model.objects[0]->config.option("support_filament")->getInt() == 2);
}

// A mixed filament is a virtual slot: ToolOrdering prints its components, never the slot itself.
// The plate's tools are therefore the components, and the mix -- which objects still reference
// and whose config rows must travel along -- is numbered after them so T0..T(n-1) stay exactly
// what the printer loads.
TEST_CASE("A mixed slot is used through its components", "[FilamentCompaction]")
{
    // Filament 5 blends 1 and 3; the plate prints only filament 5.
    const Model model = model_using({5});
    CHECK(used_filament_slots(model, mixed_config(6, 5, "1,3")) == std::vector<int>{0, 2});
}

TEST_CASE("A used mix is numbered after the physical tools", "[FilamentCompaction]")
{
    const FilamentCompaction compaction = build_filament_compaction(model_using({5}), mixed_config(6, 5, "1,3"));
    REQUIRE(compaction.slot_of_tool == std::vector<int>{0, 2, 4});
    CHECK(compaction.tool_of_slot(4) == 2);
}

TEST_CASE("Four physical filaments and their mixes need no renumbering", "[FilamentCompaction]")
{
    // The field case: filaments 1-4 loaded, 5 a mix of two of them, on a four-tool printer.
    const FilamentCompaction compaction = build_filament_compaction(model_using({1, 2, 3, 4, 5}), mixed_config(5, 5, "2,4"));
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
    // The field case: a filament-1 cube painted with slot 8, a mix of loaded filaments 4 and 5.
    Model         model  = model_using({1});
    ModelVolume*  volume = model.objects[0]->volumes[0];
    TriangleSelector selector(volume->mesh());
    selector.set_facet(0, EnforcerBlockerType::Extruder8);
    volume->mmu_segmentation_facets.set_data(selector.serialize());
    // get_extruders caches the painted list keyed on the painting's timestamp; prime it so the
    // copy below inherits a cache that names the project slots.
    REQUIRE(volume->get_extruders() == std::vector<int>{8, 1});

    const DynamicPrintConfig config = mixed_config(8, 8, "4,5");
    CHECK(used_filament_slots(model, config) == std::vector<int>{0, 3, 4});

    const FilamentCompaction compaction = build_filament_compaction(model, config);
    REQUIRE(compaction.slot_of_tool == std::vector<int>{0, 3, 4, 7});

    Model copy = model;
    apply_filament_compaction(copy, model, compaction);
    // The copy keeps the source's timestamp by design, so a stale cache would still say 8 here.
    CHECK(copy.objects[0]->volumes[0]->get_extruders() == std::vector<int>{4, 1});
}
