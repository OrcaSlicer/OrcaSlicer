#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Two physical filaments plus one mixed slot (config index 2, 1-based id 3) blending them 60/40.
// The mixed arrays are parallel to filament_colour and must be sized to the filament count.
// Note ConfigOptionBools deserializes on ',' while ConfigOptionStrings uses ';'.
DynamicPrintConfig mixed_config(bool sublayer_on, const char *ratios = "0.6,0.4")
{
    DynamicPrintConfig config = multifilament_config(3);
    config.set_deserialize_strict({
        {"filament_is_mixed",               "0,0,1"},
        {"filament_mixed_components",       ";;1,2"},
        {"filament_mixed_sublayer_ratios",  std::string(";;") + ratios},
        {"filament_mixed_gradient",         "0,0,0"},
        {"filament_mixed_gradient_range",   ";;"},
        {"filament_mixed_gradient_curve",   ";;"},
        {"filament_mixed_gradient_per_part","0,0,0"},
        {"enable_mixed_color_sublayer",     sublayer_on ? "1" : "0"},
        // Assign every region role to the mixed slot so it actually participates in slicing.
        {"outer_wall_filament_id",          "3"},
        {"inner_wall_filament_id",          "3"},
        {"sparse_infill_filament_id",       "3"},
        {"internal_solid_filament_id",      "3"},
        {"top_surface_filament_id",         "3"},
        {"bottom_surface_filament_id",      "3"},
    });
    return config;
}

// Total sub-layer groups and per-layer DRR resolutions across the whole tool ordering.
void count_mixed(ToolOrdering &to, size_t &groups, size_t &resolutions)
{
    groups = resolutions = 0;
    for (const LayerTools &lt : to.layer_tools()) {
        groups      += lt.mixed_sub_layer_groups.size();
        resolutions += lt.mixed_filament_resolution.size();
    }
}

} // namespace

TEST_CASE("enable_mixed_color_sublayer reaches the Print config", "[MixedFilament]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(true));

    // The option lives in PrintConfig; if it did not survive Print::apply the slicer would
    // silently fall back to the whole-layer path.
    CHECK(print.config().enable_mixed_color_sublayer.value == true);
    REQUIRE(print.config().filament_is_mixed.values.size() == 3);
    CHECK(print.config().filament_is_mixed.values[2] == true);
    REQUIRE(print.config().filament_mixed_components.values.size() == 3);
    CHECK(print.config().filament_mixed_components.values[2] == "1,2");
}

TEST_CASE("Mixed filament splits layers into sub-layers when the option is on", "[MixedFilament]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(true));
    print.process();

    ToolOrdering &to = const_cast<ToolOrdering &>(print.tool_ordering());
    REQUIRE(!to.layer_tools().empty());

    size_t groups = 0, resolutions = 0;
    count_mixed(to, groups, resolutions);

    INFO("layers=" << to.layer_tools().size() << " groups=" << groups);
    CHECK(groups > 0);
}

TEST_CASE("Mixed filament alternates whole layers when the option is off", "[MixedFilament]")
{
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(false));
    print.process();

    ToolOrdering &to = const_cast<ToolOrdering &>(print.tool_ordering());
    REQUIRE(!to.layer_tools().empty());

    size_t groups = 0, resolutions = 0;
    count_mixed(to, groups, resolutions);

    // With splitting off the slot is realized by the deficit round-robin scheduler instead:
    // no sub-layer groups, but a per-layer resolution to one physical component.
    INFO("layers=" << to.layer_tools().size() << " resolutions=" << resolutions);
    CHECK(groups == 0);
    CHECK(resolutions > 0);
}

TEST_CASE("Sub-layer splitting emits the scaled sub-heights into G-code", "[MixedFilament]")
{
    // layer_height 0.2 split 60/40 gives sub-layers of 0.12 and 0.08. The emitter reports the
    // sub-height (not the nominal layer height) in the HEIGHT tag and scales flow to match.
    DynamicPrintConfig config = mixed_config(true);
    config.set_deserialize_strict({{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    print.process();
    const std::string gc = Slic3r::Test::gcode(print);

    REQUIRE(!gc.empty());
    INFO("gcode bytes=" << gc.size());
    CHECK(gc.find(";HEIGHT:0.12") != std::string::npos);
    CHECK(gc.find(";HEIGHT:0.08") != std::string::npos);
}

TEST_CASE("Whole-layer mixing emits only the nominal layer height", "[MixedFilament]")
{
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({{"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"}});

    Print print;
    Model model;
    init_print({cube(20)}, print, model, config);
    print.process();
    const std::string gc = Slic3r::Test::gcode(print);

    REQUIRE(!gc.empty());
    // No sub-layer split, so the 60/40 sub-heights must never appear.
    CHECK(gc.find(";HEIGHT:0.12") == std::string::npos);
    CHECK(gc.find(";HEIGHT:0.08") == std::string::npos);
}
