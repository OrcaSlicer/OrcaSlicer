#include <catch2/catch_all.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/GCodeReader.hpp"

#include "test_helpers.hpp"

#include <iterator>
#include <set>

using namespace Slic3r;
using namespace Slic3r::Test;

SCENARIO("Object layer heights", "[PrintObject]") {
    GIVEN("A 20mm cube") {
        WHEN("sliced with a 2mm layer height and a 3mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               2 },
                { "nozzle_diameter",            3 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
            THEN("The output vector has 10 entries") {
                REQUIRE(layers.size() == 10);
            }
            AND_THEN("Each layer is approximately 2mm above the previous Z") {
                coordf_t last = 0.0;
                for (size_t i = 0; i < layers.size(); ++ i) {
                    REQUIRE_THAT(layers[i]->print_z - last, Catch::Matchers::WithinAbs(2.0, 1e-4));
                    last = layers[i]->print_z;
                }
            }
        }
        WHEN("sliced with a 10mm layer height and an 11mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               10 },
                { "nozzle_diameter",            11 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 3 entries") {
                REQUIRE(layers.size() == 3);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE_THAT(layers.front()->print_z, Catch::Matchers::WithinAbs(2.0, 1e-4));
            }
            AND_THEN("Layer 1 is at 12mm") {
                REQUIRE_THAT(layers[1]->print_z, Catch::Matchers::WithinAbs(12.0, 1e-4));
            }
        }
        WHEN("sliced with a 15mm layer height and a 16mm nozzle") {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "initial_layer_print_height", 2 },
                { "layer_height",               15 },
                { "nozzle_diameter",            16 }
	        });
            ConstLayerPtrsAdaptor layers = print.objects().front()->layers();
			THEN("The output vector has 2 entries") {
                REQUIRE(layers.size() == 2);
            }
            AND_THEN("Layer 0 is at 2mm") {
                REQUIRE_THAT(layers[0]->print_z, Catch::Matchers::WithinAbs(2.0, 1e-4));
            }
            AND_THEN("Layer 1 is at 17mm") {
                REQUIRE_THAT(layers[1]->print_z, Catch::Matchers::WithinAbs(17.0, 1e-4));
            }
        }
        WHEN("layer height exceeds the nozzle diameter") {
            // Orca does not clamp an over-large layer height to the nozzle; it
            // rejects the slice during flow computation. Pin that behavior.
            THEN("Slicing is rejected") {
                Slic3r::Print print;
                REQUIRE_THROWS(Slic3r::Test::init_and_process_print({cube(20)}, print, {
                    { "initial_layer_print_height", 0.3 },
                    { "layer_height",               0.5 },
                    { "nozzle_diameter",            0.4 }
                }));
            }
        }
    }
}

SCENARIO("Perimeter generation", "[PrintObject]") {
    GIVEN("20mm cube and default config") {
        WHEN("make_perimeters() is called")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, { { "sparse_infill_density", 0 } });
			const PrintObject &object = *print.objects().front();
            THEN("Every layer in region 0 has 1 island of perimeters") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.entities.size() == 1);
            }
        }
        WHEN("wall_loops is set to 3")  {
            Slic3r::Print print;
            Slic3r::Test::init_and_process_print({cube(20)}, print, {
                { "sparse_infill_density", 0 },
                { "wall_loops",            3 }
            });
            const PrintObject &object = *print.objects().front();
            THEN("Every layer in region 0 has 3 perimeter loops") {
                for (const Layer *layer : object.layers())
                    REQUIRE(layer->regions().front()->perimeters.items_count() == 3);
            }
        }
    }
}

TEST_CASE("Initial layer height is honored", "[PrintObject]")
{
    const std::string gcode = Slic3r::Test::slice({cube(20)}, {
        { "initial_layer_print_height", 0.3 },
        { "layer_height",               0.2 },
        { "z_hop",                      0 } // keep recorded Z equal to the printed layer height
    });

    std::set<double> layer_zs;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&layer_zs] (GCodeReader& self, const GCodeReader::GCodeLine& line) {
        if (line.extruding(self) && line.dist_XY(self) > 0)
            layer_zs.insert(self.z());
    });

    REQUIRE(layer_zs.size() > 1);
    REQUIRE_THAT(*layer_zs.begin(),            Catch::Matchers::WithinAbs(0.3, 1e-4));
    REQUIRE_THAT(*std::next(layer_zs.begin()), Catch::Matchers::WithinAbs(0.5, 1e-4));
}

// Config carrying just the filament-scope keys PrintObject::resolve_auto_support_filament reads.
static DynamicPrintConfig auto_support_config(std::vector<std::string>  types,
                                              std::vector<unsigned char> soluble,
                                              std::vector<std::string>  colours,
                                              std::vector<unsigned char> is_support = {},
                                              std::vector<unsigned char> is_mixed   = {},
                                              std::vector<std::string>  mixed_components = {})
{
    if (is_support.empty())
        is_support.assign(types.size(), false);
    if (is_mixed.empty())
        is_mixed.assign(types.size(), false);
    if (mixed_components.empty())
        mixed_components.assign(types.size(), std::string{});
    DynamicPrintConfig config;
    config.set_key_value("filament_is_mixed",              new ConfigOptionBools(std::move(is_mixed)));
    config.set_key_value("filament_mixed_components",      new ConfigOptionStrings(std::move(mixed_components)));
    config.set_key_value("filament_type",                  new ConfigOptionStrings(std::move(types)));
    config.set_key_value("filament_soluble",               new ConfigOptionBools(std::move(soluble)));
    config.set_key_value("filament_is_support",            new ConfigOptionBools(std::move(is_support)));
    config.set_key_value("filament_colour",                new ConfigOptionStrings(std::move(colours)));
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    return config;
}

// A single-volume object printed with `extruder` (1-based).
static ModelObject& auto_support_object(Model &model, int extruder)
{
    ModelObject *object = model.add_object();
    object->add_volume(cube(20));
    object->volumes.front()->config.set_key_value("extruder", new ConfigOptionInt(extruder));
    return *object;
}

TEST_CASE("Auto support filament picks a filament that does not bond to the object", "[PrintObject]")
{
    Model              model;
    const ModelObject &object = auto_support_object(model, 1); // printed in PLA

    SECTION("a non-bonding material is preferred over the object's own") {
        // PLA does not bond to PET, so the PETG filament is the one that detaches cleanly.
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG"}, {false, false}, {"#FFFFFF", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 2, config, true) == 2);
    }
    SECTION("a soluble filament wins over a merely non-bonding one") {
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG", "PVA"}, {false, false, true}, {"#FFFFFF", "#000000", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true) == 3);
    }
    SECTION("a support material wins over a plain filament of the same compatibility") {
        // Both candidates are PETG, so they are equally incompatible with the PLA object and only the support
        // flag separates them. Filament 2 has the object's exact colour, so colour alone would pick it.
        const bool               soluble = GENERATE(true, false); // flagged soluble, or flagged a support filament
        const DynamicPrintConfig config  = auto_support_config({"PLA", "PETG", "PETG"},
                                                              {false, false, soluble},
                                                              {"#FFFFFF", "#FFFFFF", "#000000"},
                                                              {false, false, !soluble});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true) == 3);
    }
    SECTION("the excluded filament is not picked") {
        // "Avoid interface filament for base" keeps the base off the interface's filament.
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG", "PVA"}, {false, false, true}, {"#FFFFFF", "#000000", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true, 3) == 2);
    }
    SECTION("only bonding filaments available falls back to the object's own filament") {
        // Both filaments are PLA, so no filament would detach: reuse the object's to avoid mixing colours.
        const DynamicPrintConfig config = auto_support_config({"PLA", "PLA"}, {false, false}, {"#FFFFFF", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 2, config, true) == 1);
    }
    SECTION("a single filament resolves to Default") {
        const DynamicPrintConfig config = auto_support_config({"PLA"}, {false}, {"#FFFFFF"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 1, config, true) == 0);
    }
    SECTION("support disabled resolves to Default") {
        const DynamicPrintConfig config = auto_support_config({"PLA", "PETG"}, {false, false}, {"#FFFFFF", "#000000"});
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 2, config, false) == 0);
    }
}

// A mixed-color slot is virtual: it prints as its component filaments and can never be loaded as a
// support filament itself. Its own filament_type describes no real spool, so reading it here picks
// the wrong support material - or none at all, leaving "Auto" resolving to Default.
TEST_CASE("Auto support filament resolves a mixed slot to its components", "[PrintObject][FilamentMixer]")
{
    Model model;

    SECTION("the object's material comes from the components, not the mixed slot's own type") {
        // Slot 4 is a mix of the two PLA filaments, but carries a stale PETG type of its own.
        // The object is painted with it, so the material it really prints is PLA.
        const ModelObject &object = auto_support_object(model, 4);
        const DynamicPrintConfig config = auto_support_config(
            {"PETG", "PLA", "PLA", "PETG"},
            {false, false, false, false},
            {"#FFFFFF", "#000000", "#00FF00", "#0000FF"},
            {},
            {false, false, false, true},
            {"", "", "", "2,3"});

        // Filament 1 is the only real PETG, and PETG does not bond to the PLA the mix prints.
        // Reading the mixed slot's own PETG type instead would rule filament 1 out as "bonding"
        // and settle on a PLA filament, which is exactly what support must not be made of.
        REQUIRE(PrintObject::resolve_auto_support_filament(object, 4, config, true) == 1);
    }

    SECTION("a mixed slot is never chosen as the support filament") {
        const ModelObject &object = auto_support_object(model, 1); // printed in PLA
        // Slot 3 is a mix of the two PLA filaments and looks attractive on type alone (PETG does
        // not bond to PLA), but it cannot be loaded, so the fallback to the object's own applies.
        const DynamicPrintConfig config = auto_support_config(
            {"PLA", "PLA", "PETG"},
            {false, false, false},
            {"#FFFFFF", "#000000", "#0000FF"},
            {},
            {false, false, true},
            {"", "", "1,2"});

        REQUIRE(PrintObject::resolve_auto_support_filament(object, 3, config, true) == 1);
    }
}

TEST_CASE("Auto support filament makes a single-material plate use two filaments", "[PrintObject]")
{
    // Regression guard: the auto-picked interface filament has to show up in Print::extruders(), otherwise the
    // print looks single-filament and the prime tower gets normalized away.
    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, {
        { "enable_support",             true },
        { "support_interface_filament", SUPPORT_FILAMENT_AUTO },
        // "Auto" only applies to printers with one nozzle per filament.
        { "single_extruder_multi_material", false },
        { "filament_type",              "PLA;PETG" },
        { "nozzle_diameter",            "0.4,0.4" },
        { "filament_diameter",          "1.75,1.75" },
        { "filament_soluble",           "0,0" },
        { "filament_colour",            "#FFFFFF;#000000" }
    });

    REQUIRE(print.objects().front()->config().support_interface_filament.value == 2);
    REQUIRE(print.extruders().size() == 2);
}

TEST_CASE("Auto support filament is re-resolved when the filament properties change", "[PrintObject]")
{
    // Regression guard: flagging a filament soluble makes Auto pick it, and clearing the flag again has to
    // hand the support back to the next best material instead of keeping the stale pick.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",                 true },
        { "support_interface_filament",     SUPPORT_FILAMENT_AUTO },
        { "single_extruder_multi_material", false },
        { "filament_type",                  "ABS;PLA;PETG" },
        { "nozzle_diameter",                "0.4,0.4,0.4" },
        { "filament_diameter",              "1.75,1.75,1.75" },
        // The object prints in ABS; the PLA is much closer to its colour than the PETG.
        { "filament_colour",                "#800080;#2F26A6;#C9C9C9" },
        { "filament_soluble",               "0,0,1" }
    });

    Slic3r::Print print;
    Slic3r::Model model;
    Slic3r::Test::init_print({cube(20)}, print, model, config);
    // Soluble beats a closer colour, so the PETG wins.
    REQUIRE(print.objects().front()->config().support_interface_filament.value == 3);

    config.set_deserialize_strict({ { "filament_soluble", "0,0,0" } });
    print.apply(model, config);
    // Both are now plain filaments, equally incompatible with the ABS object: the closer colour decides.
    REQUIRE(print.objects().front()->config().support_interface_filament.value == 2);
}

// "Auto" support filament deliberately picks a material that does NOT bond to the object — that is
// how the support detaches. Applying the material-bonding rule to it therefore rejects exactly the
// prints the feature was made for, so a filament used only for support is exempt from that rule.
// It stays subject to the temperature rules, which are about printing the two together at all.
TEST_CASE("A support-only filament is exempt from the material bonding rule", "[Print][SupportFilament]")
{
    // PLA object, PETG support: known-incompatible, which is the property support wants.
    const std::vector<std::string> types        = { "PLA", "PETG" };
    const std::vector<int>         temps        = { 220, 240 };
    const std::vector<int>         range_lows   = { 190, 220 };
    const std::vector<int>         range_highs  = { 250, 260 };

    SECTION("both printing object geometry is still rejected") {
        REQUIRE(Print::check_multi_filaments_compatibility(types, temps, range_lows, range_highs) ==
                FilamentCompatibilityType::IncompatibleMaterials);
    }

    SECTION("the second used only for support is accepted") {
        REQUIRE(Print::check_multi_filaments_compatibility(types, temps, range_lows, range_highs, { 0, 1 }) ==
                FilamentCompatibilityType::Compatible);
    }

    SECTION("a support-only filament outside the object's temperature range is still flagged") {
        // The exemption covers bonding only. Printing the support filament at 300 puts it above
        // the object filament's 250 max, which the temperature rule must still catch.
        const std::vector<int> hot_temps       = { 220, 300 };
        const std::vector<int> hot_range_highs = { 250, 320 };
        REQUIRE(Print::check_multi_filaments_compatibility(types, hot_temps, range_lows, hot_range_highs, { 0, 1 }) ==
                FilamentCompatibilityType::HighLowMixed);
    }
}

// A mixed-color slot is virtual: it is realized as its component filaments, so its own
// filament_type belongs to no spool. Checking that phantom type against the object's real
// materials reported a PLA-only object as printing incompatible materials.
TEST_CASE("The object material check resolves mixed slots to their components", "[Print][FilamentMixer]")
{
    // Slot 3 is a mix of the two PLA filaments but carries a stale PETG type of its own, and the
    // object is printed with it. PETG and PLA are known not to bond, so reading the slot's own type
    // makes the object look self-incompatible.
    auto make_config = [](bool declare_mix) {
        // Three nozzles: a multi-tool printer, where check_object_materials_valid is the check that
        // runs (the single-nozzle one is gated behind nozzles < 2).
        DynamicPrintConfig config = multifilament_config(3, {
            { "filament_type",   "PLA;PLA;PETG" },
            { "nozzle_diameter", "0.4,0.4,0.4"  },
        });
        if (declare_mix)
            config.set_deserialize_strict({
                { "filament_is_mixed",         "0,0,1" },
                { "filament_mixed_components", ";;1,2" },
            });
        return config;
    };
    // The object prints its body in filament 1 (PLA) and its top surfaces with the mixed slot, so
    // the two meet inside one object and the material rule has a pair to compare.
    const std::vector<std::vector<Slic3r::ConfigBase::SetDeserializeItem>> printed_with_slot_3 = {
        { { "extruder", "1" }, { "top_surface_filament_id", "3" } }
    };
    auto warns_about_materials = [&](bool declare_mix) {
        Print print;
        Model model;
        init_print({ cube(20) }, print, model, make_config(declare_mix), &printed_with_slot_3);
        std::vector<StringObjectException> warnings;
        warnings.push_back(print.validate(&warnings)); // the check may report either way
        for (const StringObjectException &warning : warnings)
            if (warning.string.find("materials are incompatible") != std::string::npos)
                return true;
        return false;
    };

    // Without the mixed metadata the phantom PETG is taken at face value, which is the bug.
    CHECK(warns_about_materials(false));
    // Declared as a mix, the slot is checked as its PLA components and the object is fine.
    CHECK_FALSE(warns_about_materials(true));
}

