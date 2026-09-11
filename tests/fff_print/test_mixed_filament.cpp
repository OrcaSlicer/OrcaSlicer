#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/MultiNozzleUtils.hpp"
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

// Total sub-layer groups and per-layer mixed-filament resolutions across the whole tool ordering.
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

TEST_CASE("By-object prints without mixed filaments keep their used-filament set", "[MixedFilament]")
{
    // With no mixed slot the by-object bookkeeping stays plain: object 2 prints with filament 2,
    // so both filaments are used and no mixed filament is reported.
    DynamicPrintConfig config = multifilament_config(2, {{"print_sequence", "by object"}});
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ {}, { {"extruder", "2"} } };

    Print print;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(20), cube(20)}, print, model, config, &overrides);
    REQUIRE(print.objects().size() == 2);
    print.process();

    CHECK(print.get_slice_used_filaments(false) == std::vector<unsigned int>{0, 1});
    CHECK(print.get_slice_used_filaments(true) == std::vector<unsigned int>{0, 1});
    CHECK(print.get_slice_used_mixed_filaments().empty());
}

TEST_CASE("By-layer prints record a mixed slot's components and the slot itself", "[MixedFilament]")
{
    // Control for the by-object case below: the by-layer path publishes the physical
    // components (0-based 0 and 1) as used filaments and the mixed slot (config index 2) as
    // a used mixed filament. By-object prints must report exactly the same.
    Print print;
    Model model;
    init_print({cube(20)}, print, model, mixed_config(false));
    print.process();

    CHECK(print.get_slice_used_filaments(false) == std::vector<unsigned int>{0, 1});
    CHECK(print.get_slice_used_mixed_filaments() == std::vector<unsigned int>{2});
}

TEST_CASE("By-object prints expand a mixed slot to its components in the slice bookkeeping", "[MixedFilament]")
{
    // Sequential prints build their filament lists from unsorted per-object orderings, which
    // still carry the virtual slot (config index 2). The slice-used sets and the published
    // grouping result must see the physical components 0 and 1 instead, and the slot itself
    // must still be reported as a used mixed filament — exactly what the by-layer path yields.
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({{"print_sequence", "by object"}});

    Print print;
    Model model;
    init_print({cube(20), cube(20)}, print, model, config);
    REQUIRE(print.objects().size() == 2);
    print.process();

    const std::vector<unsigned int> components{0, 1};
    CHECK(print.get_slice_used_filaments(false) == components);
    CHECK(print.get_slice_used_filaments(true) == components);
    CHECK(print.get_slice_used_mixed_filaments() == std::vector<unsigned int>{2});

    auto group_result = print.get_layered_nozzle_group_result();
    REQUIRE(group_result != nullptr);
    CHECK(group_result->get_used_filaments() == components);
}

TEST_CASE("By-object G-code lists a mixed slot's components in the filament header", "[MixedFilament]")
{
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({{"print_sequence", "by object"}});

    Print print;
    Model model;
    init_print({cube(20), cube(20)}, print, model, config);
    const std::string gc = Slic3r::Test::gcode(print);

    REQUIRE(!gc.empty());
    // The header names the filaments that must be loaded (components 1 and 2, 1-based),
    // never the virtual slot 3.
    CHECK(gc.find("; filament: 1,2\n") != std::string::npos);
    CHECK(gc.find("; filament: 3") == std::string::npos);
}

TEST_CASE("Print::validate rejects a mixed filament as the wipe tower filament", "[MixedFilament]")
{
    // The validate backstop refuses a mixed (virtual) slot as the wipe tower filament; the GUI hides
    // the slot from that option. Two cubes on physical filaments 1 and 2 make the tower real, and the
    // region roles mixed_config() points at the slot are reset so only the tower uses it.
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({
        {"enable_prime_tower",         "1"},
        {"wipe_tower_x",               "50"}, // inside the 200x200 test bed
        {"wipe_tower_y",               "50"}, // (the default y, 220, is not)
        {"layer_change_gcode",         "G92 E0\n"}, // validate() relative-E reset, as in test_print.cpp's build_cubes
        {"outer_wall_filament_id",     "0"},
        {"inner_wall_filament_id",     "0"},
        {"sparse_infill_filament_id",  "0"},
        {"internal_solid_filament_id", "0"},
        {"top_surface_filament_id",    "0"},
        {"bottom_surface_filament_id", "0"},
    });
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{ { {"extruder", "1"} }, { {"extruder", "2"} } };

    SECTION("a physical wipe tower filament validates") {
        config.set_deserialize_strict({{"wipe_tower_filament", "2"}});
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(20), cube(20)}, print, model, config, &overrides);
        REQUIRE(print.has_wipe_tower());
        const StringObjectException err = print.validate();
        INFO(err.string);
        CHECK(err.string.empty());
    }

    SECTION("the mixed slot is refused") {
        config.set_deserialize_strict({{"wipe_tower_filament", "3"}});
        Print print;
        Model model;
        init_print(std::vector<TriangleMesh>{cube(20), cube(20)}, print, model, config, &overrides);
        REQUIRE(print.has_wipe_tower());
        const StringObjectException err = print.validate();
        CHECK_FALSE(err.string.empty());
        CHECK(err.opt_key == "wipe_tower_filament");
    }
}

TEST_CASE("Print::validate warns when a gradient mixed filament is used without sublayer mixing", "[MixedFilament]")
{
    // A gradient mixed filament only renders its gradient with the process option enabled; without
    // it ToolOrdering prints one whole component per layer and the gradient is dropped silently,
    // so validate() warns whenever the slot actually takes part in the print. The layer-change
    // reset avoids an unrelated relative-extrusion warning, as in the wipe tower test above.
    DynamicPrintConfig config = mixed_config(false);
    config.set_deserialize_strict({
        {"filament_mixed_gradient", "0,0,1"},
        {"layer_change_gcode",       "G92 E0\n"},
    });

    auto count_opt = [](Print &print, const char *opt_key) {
        std::vector<StringObjectException> warnings;
        print.validate(&warnings);
        return std::count_if(warnings.begin(), warnings.end(),
                             [&](const StringObjectException &w) { return w.opt_key == opt_key; });
    };

    SECTION("gradient slot used, sublayer mixing off") {
        Print print;
        Model model;
        init_print({cube(20)}, print, model, config);
        std::vector<StringObjectException> warnings;
        const StringObjectException err = print.validate(&warnings);
        CHECK(err.string.empty());
        const auto it = std::find_if(warnings.begin(), warnings.end(), [](const StringObjectException &w) {
            return w.opt_key == "enable_mixed_color_sublayer";
        });
        REQUIRE(it != warnings.end());
        CHECK(it->is_warning);
        CHECK(std::count_if(warnings.begin(), warnings.end(), [](const StringObjectException &w) {
                  return w.opt_key == "enable_mixed_color_sublayer";
              }) == 1);
    }

    SECTION("sublayer mixing on") {
        config.set_deserialize_strict({{"enable_mixed_color_sublayer", "1"}});
        Print print;
        Model model;
        init_print({cube(20)}, print, model, config);
        CHECK(count_opt(print, "enable_mixed_color_sublayer") == 0);
    }

    SECTION("gradient flag off") {
        config.set_deserialize_strict({{"filament_mixed_gradient", "0,0,0"}});
        Print print;
        Model model;
        init_print({cube(20)}, print, model, config);
        CHECK(count_opt(print, "enable_mixed_color_sublayer") == 0);
    }

    SECTION("mixed slot not used") {
        config.set_deserialize_strict({
            {"outer_wall_filament_id",     "0"},
            {"inner_wall_filament_id",     "0"},
            {"sparse_infill_filament_id",  "0"},
            {"internal_solid_filament_id", "0"},
            {"top_surface_filament_id",    "0"},
            {"bottom_surface_filament_id", "0"},
        });
        Print print;
        Model model;
        const std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{{{ "extruder", "1" }}};
        init_print(std::vector<TriangleMesh>{cube(20)}, print, model, config, &overrides);
        CHECK(count_opt(print, "enable_mixed_color_sublayer") == 0);
    }
}
