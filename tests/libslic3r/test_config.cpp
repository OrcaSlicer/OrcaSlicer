#include <catch2/catch_all.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PrintConfigConstants.hpp"
#include "libslic3r/LocalesUtils.hpp"

#include <cereal/types/polymorphic.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/archives/binary.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

using namespace Slic3r;

SCENARIO("Generic config validation performs as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN( "outer_wall_line_width is set to 250%, a valid value") {
            config.set_deserialize_strict("outer_wall_line_width", "250%");
            THEN( "The config is read as valid.") {
                REQUIRE(config.validate().empty());
            }
        }
        WHEN( "outer_wall_line_width is set to -10, an invalid value") {
            config.set("outer_wall_line_width", -10);
            THEN( "Validate returns error") {
                REQUIRE_FALSE(config.validate().empty());
            }
        }

        WHEN( "wall_loops is set to -10, an invalid value") {
            config.set("wall_loops", -10);
            THEN( "Validate returns error") {
                REQUIRE_FALSE(config.validate().empty());
            }
        }
    }
}

SCENARIO("Config accessor functions perform as expected.", "[Config]") {
    GIVEN("A config generated from default options") {
        Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();
        WHEN("A boolean option is set to a boolean value") {
            REQUIRE_NOTHROW(config.set("gcode_comments", true));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing a 0 or 1") {
            CHECK_NOTHROW(config.set_deserialize_strict("gcode_comments", "1"));
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == true);
            }
        }
        WHEN("A boolean option is set to a string value representing something other than 0 or 1") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", "Z"), BadOptionTypeException);
            }
            AND_THEN("Value is unchanged.") {
                REQUIRE(config.opt<ConfigOptionBool>("gcode_comments")->getBool() == false);
            }
        }
        WHEN("A boolean option is set to an int value") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("gcode_comments", 1), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set from serialized string") {
            config.set_deserialize_strict("raft_layers", "20");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionInt>("raft_layers")->getInt() == 20);
            }
        }
	WHEN("An integer-based option is set through the integer interface") {
	    config.set("raft_layers", 100);
	    THEN("The underlying value is set correctly.") {
		REQUIRE(config.opt<ConfigOptionInt>("raft_layers")->getInt() == 100);
	    }
        }
        WHEN("An floating-point option is set through the integer interface") {
            config.set("max_bridge_length", 10);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("max_bridge_length")->getFloat() == 10.0);
            }
        }
        WHEN("A floating-point option is set through the double interface") {
            config.set("max_bridge_length", 5.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionFloat>("max_bridge_length")->getFloat() == 5.5);
            }
        }
        WHEN("An integer-based option is set through the double interface") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("top_shell_layers", 5.5), BadOptionTypeException);
            }
        }
        WHEN("A numeric option is set to a non-numeric value.") {
	    auto prev_value = config.opt<ConfigOptionFloat>("max_bridge_length")->getFloat();
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set_deserialize_strict("max_bridge_length", "zzzz"), BadOptionValueException);
            }
            THEN("The value does not change.") {
                REQUIRE(config.opt<ConfigOptionFloat>("max_bridge_length")->getFloat() == prev_value);
            }
        }
        WHEN("A string option is set through the string interface") {
            config.set("machine_end_gcode", "100");
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the integer interface") {
            config.set("machine_end_gcode", 100);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == "100");
            }
        }
        WHEN("A string option is set through the double interface") {
            config.set("machine_end_gcode", 100.5);
            THEN("The underlying value is set correctly.") {
                REQUIRE(config.opt<ConfigOptionString>("machine_end_gcode")->value == float_to_string_decimal_point(100.5));
            }
        }
        WHEN("A float or percent is set as a percent through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100%");
            THEN("Value and percent flag are 100/true") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == true);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the string interface.") {
            config.set_deserialize_strict("initial_layer_line_width", "100");
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the int interface.") {
            config.set("initial_layer_line_width", 100);
            THEN("Value and percent flag are 100/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100);
            }
        }
        WHEN("A float or percent is set as a float through the double interface.") {
            config.set("initial_layer_line_width", 100.5);
            THEN("Value and percent flag are 100.5/false") {
                auto tmp = config.opt<ConfigOptionFloatOrPercent>("initial_layer_line_width");
                REQUIRE(tmp->percent == false);
                REQUIRE(tmp->value == 100.5);
            }
        }
        WHEN("A numeric vector is set from serialized string") {
	    config.set_deserialize_strict("temperature_vitrification", "10,20");
            THEN("The underlying value is set correctly.") {
                CHECK(config.opt<ConfigOptionInts>("temperature_vitrification")->get_at(0) == 10);
                CHECK(config.opt<ConfigOptionInts>("temperature_vitrification")->get_at(1) == 20);
            }
        }
	// FIXME: Design better accessors for vector elements
	// The following isn't supported and probably shouldn't be:
	// WHEN("An integer-based vector option is set through the integer interface") {
	//     config.set("temperature_vitrification", 100);
	//     THEN("The underlying value is set correctly.") {
	// 	REQUIRE(config.opt<ConfigOptionInts>("temperature_vitrification")->get_at(0) == 100);
	//     }
        // }
	WHEN("An integer-based vector option is set through the set_key_value interface") {
	    config.set_key_value("temperature_vitrification", new ConfigOptionInts{10,20});
	    THEN("The underlying value is set correctly.") {
                CHECK(config.opt<ConfigOptionInts>("temperature_vitrification")->get_at(0) == 10);
                CHECK(config.opt<ConfigOptionInts>("temperature_vitrification")->get_at(1) == 20);
	    }
        }
        WHEN("An invalid option is requested during set.") {
            THEN("A BadOptionTypeException exception is thrown.") {
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", 1.0), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", "1"), UnknownOptionException);
                REQUIRE_THROWS_AS(config.set("deadbeef_invalid_option", true), UnknownOptionException);
            }
        }

        WHEN("An invalid option is requested during get.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }
        WHEN("An invalid option is requested during opt.") {
            THEN("A UnknownOptionException exception is thrown.") {
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionString>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionFloat>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionInt>("deadbeef_invalid_option", false), UnknownOptionException);
                REQUIRE_THROWS_AS(config.option_throw<ConfigOptionBool>("deadbeef_invalid_option", false), UnknownOptionException);
            }
        }

        WHEN("getX called on an unset option.") {
            THEN("The default is returned.") {
                REQUIRE(config.opt_float("layer_height") == INITIAL_LAYER_HEIGHT);
                REQUIRE(config.opt_int("raft_layers") == INITIAL_RAFT_LAYERS);
                REQUIRE(config.opt_bool("reduce_crossing_wall") == INITIAL_REDUCE_CROSSING_WALL);
            }
        }

        WHEN("opt_float called on an option that has been set.") {
            config.set("layer_height", INITIAL_LAYER_HEIGHT*2);
            THEN("The set value is returned.") {
                REQUIRE(config.opt_float("layer_height") == INITIAL_LAYER_HEIGHT*2);
            }
        }
    }
}

SCENARIO("Config ini load/save interface", "[Config]") {
    WHEN("new_from_ini is called") {
		Slic3r::DynamicPrintConfig config;
		std::string path = std::string(TEST_DATA_DIR) + "/test_config/new_from_ini.ini";
		config.load_from_ini(path, ForwardCompatibilitySubstitutionRule::Disable);
        THEN("Config object contains ini file options.") {
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.size() == 1);
			REQUIRE(config.option_throw<ConfigOptionStrings>("filament_colour", false)->values.front() == "#ABCD");
        }
    }
}

// TODO: https://github.com/SoftFever/OrcaSlicer/issues/11269 - Is this test still relevant? Delete if not.
// It was failing so at least "nozzle_type" and "extruder_printable_area" could not be serialized
// and an exception was thrown, but "nozzle_type" has been around for at least 3 months now.
// So maybe this test and the serialization logic in Config.?pp should be deleted if it doesn't get used.
SCENARIO("DynamicPrintConfig serialization", "[Config]") {
    WHEN("DynamicPrintConfig is serialized and deserialized") {
        FullPrintConfig full_print_config;
        DynamicPrintConfig cfg;
        cfg.apply(full_print_config, false);

        std::string serialized;
        // try {
            std::ostringstream ss;
            cereal::BinaryOutputArchive oarchive(ss);
            oarchive(cfg);
            serialized = ss.str();
        // } catch (const std::runtime_error & /* e */) {
        //     // e.what();
        // }
	CAPTURE(serialized.length());

        THEN("Config object contains ini file options.") {
            DynamicPrintConfig cfg2;
            // try {
                std::stringstream ss(serialized);
                cereal::BinaryInputArchive iarchive(ss);
                iarchive(cfg2);
            // } catch (const std::runtime_error & /* e */) {
            //     // e.what();
            // }
	    CAPTURE(cfg.diff_report(cfg2));
            REQUIRE(cfg == cfg2);
        }
    }
}

SCENARIO("update_non_diff_values_to_base_config preserves child vectors when child has more extruders than parent",
         "[Config][Variant]") {
    GIVEN("A 2-extruder child printer config inheriting from a 1-extruder parent") {
        Slic3r::DynamicPrintConfig child;
        Slic3r::DynamicPrintConfig parent;

        child.set_key_value("nozzle_diameter",           new Slic3r::ConfigOptionFloats({0.4, 0.4}));
        child.set_key_value("printer_extruder_id",       new Slic3r::ConfigOptionInts({1, 2}));
        child.set_key_value("printer_extruder_variant",  new Slic3r::ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard"}));
        child.set_key_value("retraction_length",         new Slic3r::ConfigOptionFloats({1.5, 1.5}));

        parent.set_key_value("nozzle_diameter",          new Slic3r::ConfigOptionFloats({0.4}));
        parent.set_key_value("printer_extruder_id",      new Slic3r::ConfigOptionInts({1}));
        parent.set_key_value("printer_extruder_variant", new Slic3r::ConfigOptionStrings({"Direct Drive Standard"}));
        parent.set_key_value("retraction_length",        new Slic3r::ConfigOptionFloats({0.8}));

        const Slic3r::t_config_option_keys keys = {
            "retraction_length", "printer_extruder_id", "printer_extruder_variant"
        };
        const std::set<std::string> different_keys = {
            "retraction_length", "printer_extruder_id", "printer_extruder_variant"
        };

        WHEN("update_non_diff_values_to_base_config is called") {
            std::string id_name  = "printer_extruder_id";
            std::string var_name = "printer_extruder_variant";
            child.update_non_diff_values_to_base_config(
                parent, keys, different_keys, id_name, var_name,
                Slic3r::printer_options_with_variant_1,
                Slic3r::printer_options_with_variant_2);

            THEN("printer_extruder_id retains size 2") {
                REQUIRE(child.option<Slic3r::ConfigOptionInts>("printer_extruder_id")->values.size() == 2);
            }
            THEN("printer_extruder_variant retains size 2") {
                REQUIRE(child.option<Slic3r::ConfigOptionStrings>("printer_extruder_variant")->values.size() == 2);
            }
            THEN("retraction_length retains size 2") {
                REQUIRE(child.option<Slic3r::ConfigOptionFloats>("retraction_length")->values.size() == 2);
            }
            THEN("printer_extruder_id values are preserved for both extruders") {
                auto* pe_id = child.option<Slic3r::ConfigOptionInts>("printer_extruder_id");
                REQUIRE(pe_id->values.size() == 2);
                REQUIRE(pe_id->values[0] == 1);
                REQUIRE(pe_id->values[1] == 2);
            }
        }
    }
}

SCENARIO("update_diff_values_to_child_config tolerates legacy machine-limit vector sizes",
         "[Config][Variant]") {
    // Regression: loading a user printer preset that inherits a non-BBL multi-extruder base and
    // overrides stride-2 machine limits used to throw in ConfigOptionVector::set_only_diff
    // ("invalid diff_index size"). The base's machine-limit vectors get length-extended by the
    // nozzle count while it carries no printer_extruder_variant, so the base length (nozzles*2)
    // no longer matches variant_index.size()*2. The throw was caught upstream and DELETED the
    // user's preset file. The merge must instead degrade gracefully.
    GIVEN("A 4-nozzle parent with stride-2 limits extended to nozzles*2 but no printer_extruder_variant") {
        Slic3r::DynamicPrintConfig parent;
        Slic3r::DynamicPrintConfig child;

        parent.set_key_value("nozzle_diameter",
            new Slic3r::ConfigOptionFloats({0.4, 0.4, 0.4, 0.4}));
        parent.set_key_value("machine_max_acceleration_x",
            new Slic3r::ConfigOptionFloats({25000, 25000, 25000, 25000, 25000, 25000, 25000, 25000}));

        // Child user preset declares 4 extruder variants and overrides the machine limit.
        child.set_key_value("printer_extruder_id",
            new Slic3r::ConfigOptionInts({1, 2, 3, 4}));
        child.set_key_value("printer_extruder_variant",
            new Slic3r::ConfigOptionStrings({"Direct Drive Standard", "Direct Drive Standard",
                                             "Direct Drive Standard", "Direct Drive Standard"}));
        child.set_key_value("machine_max_acceleration_x",
            new Slic3r::ConfigOptionFloats({8000, 8000, 8000, 8000, 8000, 8000, 8000, 8000}));

        WHEN("update_diff_values_to_child_config merges the child overrides") {
            std::string id_name  = "printer_extruder_id";
            std::string var_name = "printer_extruder_variant";

            THEN("it does not throw on the legacy size mismatch") {
                REQUIRE_NOTHROW(parent.update_diff_values_to_child_config(
                    child, id_name, var_name,
                    Slic3r::printer_options_with_variant_1,
                    Slic3r::printer_options_with_variant_2));

                AND_THEN("the child's overridden machine limit is preserved") {
                    auto* mx = parent.option<Slic3r::ConfigOptionFloats>("machine_max_acceleration_x");
                    REQUIRE(mx != nullptr);
                    REQUIRE(mx->values.size() >= 2);
                    REQUIRE_THAT(mx->values[0], Catch::Matchers::WithinAbs(8000.0, 1e-6));
                    REQUIRE_THAT(mx->values[1], Catch::Matchers::WithinAbs(8000.0, 1e-6));
                }
            }
        }
    }
}

// SCENARIO("DynamicPrintConfig JSON serialization", "[Config]") {
//     WHEN("DynamicPrintConfig is serialized and deserialized") {
// 	auto now = std::chrono::high_resolution_clock::now();
// 	auto timestamp = now.time_since_epoch().count();
// 	std::stringstream ss;
// 	ss << "catch_test_serialization_" << timestamp << ".json";
// 	std::string filename = (fs::temp_directory_path() / ss.str()).string();

// TODO: Finish making a unit test for JSON serialization
//         FullPrintConfig full_print_config;
//         DynamicPrintConfig cfg;
//         cfg.apply(full_print_config, false);

//         std::string serialized;
//         try {
//             std::ostringstream ss;
//             cereal::BinaryOutputArchive oarchive(ss);
//             oarchive(cfg);
//             serialized = ss.str();
//         } catch (const std::runtime_error & /* e */) {
//             // e.what();
//         }
// 	CAPTURE(serialized.length());

//         THEN("Config object contains ini file options.") {
//             DynamicPrintConfig cfg2;
//             try {
//                 std::stringstream ss(serialized);
//                 cereal::BinaryInputArchive iarchive(ss);
//                 iarchive(cfg2);
//             } catch (const std::runtime_error & /* e */) {
//                 // e.what();
//             }
// 	    CAPTURE(cfg.diff_report(cfg2));
//             REQUIRE(cfg == cfg2);
//         }
//     }
// }

TEST_CASE("save_to_json round-trips plugin capability references as strings", "[Config][plugins]") {
    namespace fs = boost::filesystem;
    const fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca_plugins_%%%%-%%%%.json");
    const std::vector<std::string> refs = {
        "local_plugin;;inset",
        "cloud_plugin;550e8400-e29b-41d4-a716-446655440000;inset"
    };

    std::unique_ptr<DynamicPrintConfig> config_ptr(
        DynamicPrintConfig::new_from_defaults_keys({"slicing_pipeline_plugin"}));
    DynamicPrintConfig config = std::move(*config_ptr);
    config.option<ConfigOptionStrings>("slicing_pipeline_plugin", true)->values = refs;
    config.save_to_json(tmp.string(), "test_preset", "User", "1.0.0.0");

    nlohmann::json j;
    {
        boost::nowide::ifstream ifs(tmp.string());
        ifs >> j;
    }
    REQUIRE(j["slicing_pipeline_plugin"] == nlohmann::json(refs));
    CHECK_FALSE(j.contains("plugins"));

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
    std::map<std::string, std::string> key_values;
    std::string reason;
    REQUIRE(reloaded.load_from_json(tmp.string(), substitutions, true, key_values, reason) == 0);
    CHECK(reason.empty());
    CHECK(reloaded.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values == refs);

    fs::remove(tmp);
}

TEST_CASE("plugin capability references survive string-map serialization", "[Config][plugins]") {
    const std::vector<std::string> refs = {
        "master_plugin;;header-stamp",
        "Sample Plugin;1f998ea9-0183-4cc5-957f-4eef659ba4e6;G-code Benchmark (.py)"
    };

    DynamicPrintConfig original = DynamicPrintConfig::full_print_config();
    original.option<ConfigOptionStrings>("slicing_pipeline_plugin", true)->values = refs;

    std::map<std::string, std::string> serialized{
        {"slicing_pipeline_plugin", original.option<ConfigOptionStrings>("slicing_pipeline_plugin")->serialize()}
    };
    CHECK(serialized["slicing_pipeline_plugin"].find("\"master_plugin;;header-stamp\"") != std::string::npos);

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(serialized, ForwardCompatibilitySubstitutionRule::Disable);

    CHECK(reloaded.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values == refs);
}

TEST_CASE("parse_capability_ref parses local and cloud references", "[Config][plugin]") {
    const auto local = Slic3r::parse_capability_ref("local_plugin;;post_process");
    REQUIRE(local.has_value());
    CHECK(local->name == "local_plugin");
    CHECK(local->capability_name == "post_process");
    CHECK(local->uuid.empty());

    const auto cloud = Slic3r::parse_capability_ref(
        "cloud_plugin;550e8400-e29b-41d4-a716-446655440000;post_process");
    REQUIRE(cloud.has_value());
    CHECK(cloud->name == "cloud_plugin");
    CHECK(cloud->capability_name == "post_process");
    CHECK(cloud->uuid == "550e8400-e29b-41d4-a716-446655440000");
}

TEST_CASE("parse_capability_ref rejects malformed input", "[Config][plugin]") {
    CHECK_FALSE(Slic3r::parse_capability_ref("").has_value());
    CHECK_FALSE(Slic3r::parse_capability_ref("plugin").has_value());
    CHECK_FALSE(Slic3r::parse_capability_ref("plugin;uuid").has_value());
    CHECK_FALSE(Slic3r::parse_capability_ref(";;capability").has_value());
    CHECK_FALSE(Slic3r::parse_capability_ref(";uuid;capability").has_value());
    CHECK_FALSE(Slic3r::parse_capability_ref("plugin;;").has_value());
    CHECK_FALSE(Slic3r::parse_capability_ref("plugin;uuid;").has_value());
}

namespace {
// Installs a stub capability resolver that echoes the capability type into the reference, so tests
// can assert each plugin-backed option resolved with its own ConfigOptionDef::plugin_type. Resets
// the global resolver on teardown -- tests run in random order and other cases assert the
// no-resolver behavior (an absent "plugins" manifest).
struct PluginResolverFixture {
    PluginResolverFixture() {
        ConfigBase::set_resolve_capability_fn([](const std::string& name, const std::string& type) {
            return name.empty() ? std::string() : name + ";;" + type;
        });
    }
    ~PluginResolverFixture() { ConfigBase::set_resolve_capability_fn(nullptr); }
};
} // namespace

TEST_CASE_METHOD(PluginResolverFixture,
    "update_plugin_manifest derives references generically from plugin-backed options",
    "[Config][plugins]") {
    // Both scalar (printer_agent) and vector (slicing_pipeline_plugin) options opt in via a non-empty
    // ConfigOptionDef::plugin_type (is_plugin_backed) and are resolved with it -- there is no hardcoded
    // per-option switch. printer_agent in particular relies on its plugin_type metadata being wired up
    // (it is edited via a dedicated widget, not the plugin_picker).
    std::unique_ptr<DynamicPrintConfig> config_ptr(DynamicPrintConfig::new_from_defaults_keys(
        {"slicing_pipeline_plugin", "printer_agent"}));
    DynamicPrintConfig config = std::move(*config_ptr);
    config.option<ConfigOptionStrings>("slicing_pipeline_plugin", true)->values = {"sp"};
    config.option<ConfigOptionString>("printer_agent", true)->value            = "agent";

    config.update_plugin_manifest();
    const std::vector<std::string> manifest = config.option<ConfigOptionStrings>("plugins")->values;

    using Catch::Matchers::VectorContains;
    REQUIRE_THAT(manifest, VectorContains(std::string("sp;;slicing-pipeline")));
    REQUIRE_THAT(manifest, VectorContains(std::string("agent;;printer-connection")));
    CHECK(manifest.size() == 2);
}

TEST_CASE_METHOD(PluginResolverFixture,
    "update_plugin_manifest de-duplicates references and skips unset options",
    "[Config][plugins]") {
    std::unique_ptr<DynamicPrintConfig> config_ptr(DynamicPrintConfig::new_from_defaults_keys(
        {"slicing_pipeline_plugin", "printer_agent"}));
    DynamicPrintConfig config = std::move(*config_ptr);
    config.option<ConfigOptionStrings>("slicing_pipeline_plugin", true)->values = {"x", "x"};  // duplicate
    // printer_agent stays at its default empty value -> contributes nothing to the manifest.

    config.update_plugin_manifest();
    const std::vector<std::string> manifest = config.option<ConfigOptionStrings>("plugins")->values;

    CHECK(manifest == std::vector<std::string>{"x;;slicing-pipeline"});
}

TEST_CASE("H2C/A2L-era multi-nozzle and pre-heat config keys exist", "[config]") {
    // Foundation keys backing H2C 6-nozzle cluster grouping, the pre-heat/pre-cool time
    // model, and wipe-tower nozzle-change handling. Defaults must keep existing
    // single-nozzle printers behaving identically.
    Slic3r::DynamicPrintConfig config = Slic3r::DynamicPrintConfig::full_print_config();

    // Printer / per-extruder options
    REQUIRE(config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count") != nullptr);
    REQUIRE(config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count")->values == std::vector<int>{1});
    REQUIRE(config.option<ConfigOptionBool>("enable_pre_heating") != nullptr);
    REQUIRE(config.option<ConfigOptionBool>("enable_pre_heating")->value == false);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("hotend_cooling_rate") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("hotend_heating_rate") != nullptr);
    REQUIRE(config.option<ConfigOptionFloat>("machine_hotend_change_time") != nullptr);
    REQUIRE(config.option<ConfigOptionFloat>("machine_prepare_compensation_time") != nullptr);

    // Filament pre-cooling / ramming / nozzle-change (nc) options
    REQUIRE(config.option<ConfigOptionIntsNullable>("filament_pre_cooling_temperature") != nullptr);
    REQUIRE(config.option<ConfigOptionIntsNullable>("filament_pre_cooling_temperature_nc") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_preheat_temperature_delta") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_retract_length_nc") != nullptr);
    REQUIRE(config.option<ConfigOptionFloats>("filament_change_length_nc") != nullptr);
    REQUIRE(config.option<ConfigOptionFloats>("filament_prime_volume_nc") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_ramming_travel_time") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_ramming_travel_time_nc") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_ramming_volumetric_speed") != nullptr);
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_ramming_volumetric_speed_nc") != nullptr);

    // Spot-check defaults that must not alter existing behavior.
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_retract_length_nc")->values == std::vector<double>{10.});
    REQUIRE(config.option<ConfigOptionFloats>("filament_prime_volume_nc")->values == std::vector<double>{60.});
    REQUIRE(config.option<ConfigOptionIntsNullable>("filament_pre_cooling_temperature_nc")->values == std::vector<int>{0});
    REQUIRE(config.option<ConfigOptionFloatsNullable>("filament_ramming_volumetric_speed")->values == std::vector<double>{-1});
}

SCENARIO("ConfigOptionVector::set_to_index with stride=1 copies values correctly", "[Config][set_to_index]") {
    GIVEN("A destination vector and a source vector with 3 values") {
        Slic3r::ConfigOptionFloats dest({0.0});
        Slic3r::ConfigOptionFloats src({10.0, 20.0, 30.0});
        std::vector<int> variant_index = {0, 1, 2};
        int stride = 1;

        WHEN("set_to_index is called with stride=1") {
            dest.set_to_index(&src, variant_index, stride);

            THEN("The destination contains the source values") {
                REQUIRE(dest.values.size() == 3);
                REQUIRE(dest.values[0] == 10.0);
                REQUIRE(dest.values[1] == 20.0);
                REQUIRE(dest.values[2] == 30.0);
            }
        }
    }

    GIVEN("A destination vector and a source vector with subset mapping") {
        Slic3r::ConfigOptionFloats dest({0.0});
        Slic3r::ConfigOptionFloats src({100.0, 200.0, 300.0});
        std::vector<int> variant_index = {1, 2};
        int stride = 1;

        WHEN("set_to_index maps only indices 1 and 2") {
            dest.set_to_index(&src, variant_index, stride);

            THEN("Only the mapped values are copied, default fills the others") {
                REQUIRE(dest.values.size() == 2);
                REQUIRE(dest.values[0] == 200.0);
                REQUIRE(dest.values[1] == 300.0);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index with stride=2 copies grouped values correctly", "[Config][set_to_index]") {
    GIVEN("A destination vector and a source vector with stride=2 (e.g., nozzle groups)") {
        // Source has 4 groups of 2 values each: (10,11), (20,21), (30,31), (40,41)
        Slic3r::ConfigOptionFloats dest({0.0});
        Slic3r::ConfigOptionFloats src({10.0, 11.0, 20.0, 21.0, 30.0, 31.0, 40.0, 41.0});
        int stride = 2;

        WHEN("set_to_index maps groups 0, 1, 3") {
            std::vector<int> variant_index = {0, 1, 3};
            dest.set_to_index(&src, variant_index, stride);

            THEN("The destination has 3 groups (6 values) mapped correctly") {
                REQUIRE(dest.values.size() == 6);
                // Group 0: (10, 11)
                REQUIRE(dest.values[0] == 10.0);
                REQUIRE(dest.values[1] == 11.0);
                // Group 1: (20, 21)
                REQUIRE(dest.values[2] == 20.0);
                REQUIRE(dest.values[3] == 21.0);
                // Group 3: (40, 41)
                REQUIRE(dest.values[4] == 40.0);
                REQUIRE(dest.values[5] == 41.0);
            }
        }
    }

    GIVEN("A destination and a single-group source") {
        Slic3r::ConfigOptionFloats dest({0.0});
        // Source has 1 group of 2 values
        Slic3r::ConfigOptionFloats src({50.0, 60.0});
        int stride = 2;

        WHEN("set_to_index maps group 0 from a single-group source") {
            std::vector<int> variant_index = {0};
            dest.set_to_index(&src, variant_index, stride);

            THEN("The destination contains the single group correctly") {
                REQUIRE(dest.values.size() == 2);
                REQUIRE(dest.values[0] == 50.0);
                REQUIRE(dest.values[1] == 60.0);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index handles empty dest_index", "[Config][set_to_index]") {
    GIVEN("A destination and source with stride=2") {
        Slic3r::ConfigOptionFloats dest({0.0});
        Slic3r::ConfigOptionFloats src({10.0, 11.0, 20.0, 21.0});
        std::vector<int> variant_index = {};
        int stride = 2;

        WHEN("set_to_index is called with an empty index vector") {
            dest.set_to_index(&src, variant_index, stride);

            THEN("The destination is resized to 0") {
                REQUIRE(dest.values.size() == 0);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index handles nil values in source", "[Config][set_to_index]") {
    GIVEN("A source with a nil group (stride=2)") {
        Slic3r::ConfigOptionFloatsNullable dest({0.0});
        Slic3r::ConfigOptionFloatsNullable src({10.0, 11.0,
            Slic3r::ConfigOptionFloatsNullable::nil_value(), Slic3r::ConfigOptionFloatsNullable::nil_value(),
            30.0, 31.0});
        int stride = 2;

        WHEN("set_to_index maps all groups including the nil one") {
            std::vector<int> variant_index = {0, 1, 2};
            dest.set_to_index(&src, variant_index, stride);

            THEN("Non-nil groups are copied and the nil group keeps the default") {
                REQUIRE(dest.values.size() == 6);
                // Group 0: (10, 11) — copied
                REQUIRE(dest.values[0] == 10.0);
                REQUIRE(dest.values[1] == 11.0);
                // Group 1: nil — keeps default (the front value = 10.0)
                REQUIRE(dest.values[2] == 10.0);
                REQUIRE(dest.values[3] == 10.0);
                // Group 2: (30, 31) — copied
                REQUIRE(dest.values[4] == 30.0);
                REQUIRE(dest.values[5] == 31.0);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index handles out-of-bounds dest_index", "[Config][set_to_index]") {
    GIVEN("A source with only 2 groups (4 values) but dest_index references group 3") {
        Slic3r::ConfigOptionFloats dest({0.0});
        Slic3r::ConfigOptionFloats src({10.0, 11.0, 20.0, 21.0}); // 2 groups of stride 2
        int stride = 2;

        WHEN("set_to_index maps group 3 which is out of bounds") {
            std::vector<int> variant_index = {0, 3}; // group 3 is out of range
            dest.set_to_index(&src, variant_index, stride);

            THEN("Group 0 is copied, group 3 falls back to default without crashing") {
                REQUIRE(dest.values.size() == 4);
                // Group 0: (10, 11) — copied
                REQUIRE(dest.values[0] == 10.0);
                REQUIRE(dest.values[1] == 11.0);
                // Group 3: out of bounds — keeps default (10.0 = src.values.front())
                REQUIRE(dest.values[2] == 10.0);
                REQUIRE(dest.values[3] == 10.0);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index handles negative dest_index values", "[Config][set_to_index]") {
    GIVEN("A destination and source with a negative entry in dest_index") {
        // The dest is initially empty, so resize fills all slots with src.values.front().
        Slic3r::ConfigOptionFloats dest;
        Slic3r::ConfigOptionFloats src({100.0, 101.0, 200.0, 201.0});
        int stride = 2;

        WHEN("set_to_index maps group 0 and a negative index") {
            std::vector<int> variant_index = {-1, 0};
            dest.set_to_index(&src, variant_index, stride);

            THEN("The negative index is skipped, the valid group is copied") {
                REQUIRE(dest.values.size() == 4);
                // Position 0 (variant_index[0] = -1): skipped, keeps default fill
                // from resize (src.values.front() = 100.0, applied to all new elements)
                REQUIRE(dest.values[0] == 100.0);
                REQUIRE(dest.values[1] == 100.0);
                // Position 1 (variant_index[1] = 0): copied from group 0 of src
                REQUIRE(dest.values[2] == 100.0);
                REQUIRE(dest.values[3] == 101.0);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index handles single-element groups with stride=1", "[Config][set_to_index]") {
    GIVEN("A destination re-mapping one variant index with a stride=1 source") {
        // Simulates the PrintObject.cpp code path: stride=1, variant_index={1}
        Slic3r::ConfigOptionFloats dest({99.0, 99.0, 99.0, 99.0}); // pre-sized for 4 extruders
        Slic3r::ConfigOptionFloats src({0.5, 0.6, 0.7, 0.8});      // 4 extruder values
        std::vector<int> variant_index = {1}; // only extruder 1 is active
        int stride = 1;

        WHEN("set_to_index is called") {
            dest.set_to_index(&src, variant_index, stride);

            THEN("Only the mapped value is copied, rest are defaulted") {
                REQUIRE(dest.values.size() == 1);
                REQUIRE(dest.values[0] == 0.6);
            }
        }
    }
}

SCENARIO("ConfigOptionVector::set_to_index throws on incompatible type", "[Config][set_to_index]") {
    GIVEN("A Floats destination and an Ints source") {
        Slic3r::ConfigOptionFloats dest({0.0});
        Slic3r::ConfigOptionInts src({1, 2, 3});
        std::vector<int> variant_index = {0};
        int stride = 1;

        WHEN("set_to_index is called with mismatched types") {
            THEN("A ConfigurationError is thrown") {
                REQUIRE_THROWS_AS(dest.set_to_index(&src, variant_index, stride), Slic3r::ConfigurationError);
            }
        }
    }
}
