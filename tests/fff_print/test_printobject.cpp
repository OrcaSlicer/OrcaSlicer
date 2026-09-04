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
                                              std::vector<unsigned char> is_support = {})
{
    if (is_support.empty())
        is_support.assign(types.size(), false);
    DynamicPrintConfig config;
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

TEST_CASE("Auto support filaments resolve in the order their picks depend on each other", "[PrintObject]")
{
    Model              model;
    const ModelObject &object = auto_support_object(model, 1); // printed in PLA
    // Two candidates the PLA object does not bond to: a soluble one, which outranks the plain PETG.
    const DynamicPrintConfig config = auto_support_config({"PLA", "PETG", "PVA"}, {false, false, true}, {"#FFFFFF", "#000000", "#000000"});

    SECTION("interface and ironing share one pick, the base avoids it") {
        int base = SUPPORT_FILAMENT_AUTO, interface_filament = SUPPORT_FILAMENT_AUTO, ironing = SUPPORT_FILAMENT_AUTO;
        PrintObject::resolve_auto_support_filaments(object, 3, &config, true, true, base, interface_filament, ironing);
        REQUIRE(interface_filament == 3);
        REQUIRE(ironing == 3);
        REQUIRE(base == 2); // kept off the interface's filament
    }
    SECTION("without \"Avoid interface filament for base\" all three share one pick") {
        int base = SUPPORT_FILAMENT_AUTO, interface_filament = SUPPORT_FILAMENT_AUTO, ironing = SUPPORT_FILAMENT_AUTO;
        PrintObject::resolve_auto_support_filaments(object, 3, &config, true, false, base, interface_filament, ironing);
        REQUIRE(base == 3);
        REQUIRE(interface_filament == 3);
        REQUIRE(ironing == 3);
    }
    SECTION("a filament that is not Auto passes through and still constrains the base") {
        int base = SUPPORT_FILAMENT_AUTO, interface_filament = 2, ironing = 0;
        PrintObject::resolve_auto_support_filaments(object, 3, &config, true, true, base, interface_filament, ironing);
        REQUIRE(interface_filament == 2);
        REQUIRE(ironing == 0);
        REQUIRE(base == 3);
    }
    SECTION("without a config to resolve against, Auto becomes Default") {
        int base = SUPPORT_FILAMENT_AUTO, interface_filament = SUPPORT_FILAMENT_AUTO, ironing = 1;
        PrintObject::resolve_auto_support_filaments(object, 3, nullptr, true, true, base, interface_filament, ironing);
        REQUIRE(base == 0);
        REQUIRE(interface_filament == 0);
        REQUIRE(ironing == 1);
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
