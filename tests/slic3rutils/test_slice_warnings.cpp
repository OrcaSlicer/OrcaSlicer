// The Agent bridge's slice summary `warnings` array (WebAppSettings::print_warnings_json): every
// current warning of the Print's and its objects' steps, with the object named for object steps.
// Tag: [webapp].

#include <catch2/catch_all.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/WebAppSettings.hpp"

#include "fff_print/test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;
using nlohmann::json;

namespace {

DynamicPrintConfig warnings_config(bool nonplanar)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height", 0.2 },
        { "initial_layer_print_height", 0.2 },
        { "top_shell_layers", 4 },
        { "nonplanar_layers", nonplanar ? 1 : 0 },
        { "nonplanar_max_angle", 12 },
        { "nonplanar_max_height", 5 },
    });
    return config;
}

} // namespace

TEST_CASE("Slice summary warnings: a skipped nonplanar object reports its posSlice warning", "[webapp]")
{
    // A 76 degree pyramid has no top gentle enough for nonplanar layers, so posSlice adds
    // "Nonplanar layers skipped for <object>: ...".
    Model model;
    Print print;
    init_print({ mesh(TestMesh::pyramid) }, print, model, warnings_config(true));
    print.process();

    REQUIRE(!print.objects().empty());
    const std::string object = print.objects().front()->model_object()->name;

    const json warnings = GUI::WebAppSettings::print_warnings_json(print);
    REQUIRE(warnings.is_array());
    bool found = false;
    for (const json& w : warnings) {
        REQUIRE(w.contains("message"));
        REQUIRE(w.contains("level"));
        CHECK((w["level"] == "warning" || w["level"] == "critical"));
        if (w["message"].get<std::string>().find("Nonplanar layers skipped") != std::string::npos) {
            found = true;
            CHECK(w["level"] == "warning");
            REQUIRE(w.contains("object"));
            CHECK(w["object"] == object);
        }
    }
    CHECK(found);
}

TEST_CASE("Slice summary warnings: a plain cube has no nonplanar warning", "[webapp]")
{
    Model model;
    Print print;
    init_print({ cube(20) }, print, model, warnings_config(false));
    print.process();

    const json warnings = GUI::WebAppSettings::print_warnings_json(print);
    REQUIRE(warnings.is_array());
    for (const json& w : warnings)
        CHECK(w["message"].get<std::string>().find("Nonplanar") == std::string::npos);
}
