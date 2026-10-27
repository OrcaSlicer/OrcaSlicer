#include <catch2/catch_all.hpp>
#include <vector>
#include <cstddef>
#include "libslic3r/Config.hpp"
#include <string>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

TEST_CASE("parse_mixed_components reads 1-based component ids", "[FilamentMixer]")
{
    REQUIRE(parse_mixed_components("1,3") == std::vector<unsigned int>{1, 3});
    REQUIRE(parse_mixed_components("2, 4 ,5") == std::vector<unsigned int>{2, 4, 5});

    SECTION("Malformed input yields no components") {
        REQUIRE(parse_mixed_components("").empty());
        REQUIRE(parse_mixed_components("abc").empty());
    }
}

TEST_CASE("parse_mixed_ratios normalizes to sum 1.0", "[FilamentMixer]")
{
    auto r = parse_mixed_ratios("0.7,0.3", 2);
    REQUIRE(r.size() == 2);
    REQUIRE_THAT(r[0], Catch::Matchers::WithinAbs(0.7, 1e-9));
    REQUIRE_THAT(r[1], Catch::Matchers::WithinAbs(0.3, 1e-9));

    SECTION("Unnormalized input is rescaled") {
        auto v = parse_mixed_ratios("2,2", 2);
        REQUIRE_THAT(v[0], Catch::Matchers::WithinAbs(0.5, 1e-9));
        REQUIRE_THAT(v[1], Catch::Matchers::WithinAbs(0.5, 1e-9));
    }

    SECTION("Empty or mismatched input falls back to equal shares") {
        auto v = parse_mixed_ratios("", 3);
        REQUIRE(v.size() == 3);
        for (double x : v)
            REQUIRE_THAT(x, Catch::Matchers::WithinAbs(1.0 / 3.0, 1e-9));
    }
}

TEST_CASE("has_any_mixed_filament detects mixed slots", "[FilamentMixer]")
{
    REQUIRE_FALSE(has_any_mixed_filament({}));
    REQUIRE_FALSE(has_any_mixed_filament({0, 0, 0}));
    REQUIRE(has_any_mixed_filament({0, 1, 0}));
}

TEST_CASE("expand_mixed_filaments replaces mixed slots with their components", "[FilamentMixer]")
{
    // Slot 2 (0-based) is a mix of physical filaments 1 and 2 (1-based) => 0 and 1 (0-based).
    const std::vector<unsigned char> is_mixed  = {0, 0, 1};
    const std::vector<std::string>   comp_strs = {"", "", "1,2"};

    REQUIRE(expand_mixed_filaments({2}, is_mixed, comp_strs) == std::vector<unsigned int>{0, 1});

    SECTION("Non-mixed entries pass through, result is sorted and deduplicated") {
        REQUIRE(expand_mixed_filaments({2, 0}, is_mixed, comp_strs) == std::vector<unsigned int>{0, 1});
    }
}

TEST_CASE("belt purge tower island count ignores virtual mixed slots", "[FilamentMixer][belt]")
{
    // Regression for the "extra purge tower" on a belt printer with a mixed
    // filament (MCTEST5). The belt purge prism is sized as
    //   n_islands = used_filaments.size() - 1
    // and GUI::ensure_belt_purge_tower() collected those filaments straight off
    // the model objects' extruder assignments. A mixed slot is VIRTUAL -- no
    // nozzle carries it, and ToolOrdering::resolve_mixed_filaments() replaces it
    // with its components before any G-code is emitted -- so counting it as a
    // filament of its own provisions one island that can never be reached.
    //
    // MCTEST5: five cubes on extruders 1..5, where filament 5 is a 50/50 blend of
    // filaments 2 and 4. The G-code uses only T0..T3 and reports
    // "filament used [g] = 53.35, 141.11, 40.84, 107.23, 0.00" -- filament 5
    // consumes nothing, exactly as a virtual slot should.
    const std::vector<unsigned char> is_mixed  = {0, 0, 0, 0, 1};
    const std::vector<std::string>   comp_strs = {"", "", "", "", "2,4"};

    // The set the sizer used to see: slots 0..4 (filaments 1..5).
    const std::vector<unsigned int> assigned = {0, 1, 2, 3, 4};
    const auto physical = expand_mixed_filaments(assigned, is_mixed, comp_strs);

    // Slot 4 dissolves into 1 and 3, which are already present.
    REQUIRE(physical == std::vector<unsigned int>({0, 1, 2, 3}));

    // Four physical filaments => three transitions => three islands, not four.
    REQUIRE(int(physical.size()) - 1 == 3);
    REQUIRE(int(assigned.size()) - 1 == 4);   // what it produced before the fix

    SECTION("A mixed slot whose components are otherwise unused still counts them") {
        // Only the mixed slot is assigned: it must still yield its two components,
        // i.e. one island, rather than collapsing to zero.
        const auto only_mixed = expand_mixed_filaments({4}, is_mixed, comp_strs);
        REQUIRE(only_mixed == std::vector<unsigned int>({1, 3}));
        REQUIRE(int(only_mixed.size()) - 1 == 1);
    }

    SECTION("No mixed filaments anywhere leaves the set untouched") {
        const std::vector<unsigned char> none_mixed = {0, 0, 0, 0, 0};
        REQUIRE_FALSE(has_any_mixed_filament(none_mixed));
        REQUIRE(expand_mixed_filaments(assigned, none_mixed, {"", "", "", "", ""}) == assigned);
    }
}

TEST_CASE("check_mixed_filament_integrity flags dangling component references", "[FilamentMixer]")
{
    const std::vector<unsigned char> is_mixed  = {0, 0, 1};

    SECTION("All components resolve") {
        REQUIRE(check_mixed_filament_integrity(is_mixed, {"", "", "1,2"}, 2).empty());
    }

    SECTION("A component past the physical filament count is broken") {
        auto broken = check_mixed_filament_integrity(is_mixed, {"", "", "1,9"}, 2);
        REQUIRE(broken == std::vector<size_t>{2});
    }
}

TEST_CASE("remap_mixed_components_on_delete rewrites ids around the deleted slot", "[FilamentMixer]")
{
    const std::vector<unsigned char> is_mixed = {0, 0, 0, 1};
    std::vector<std::string> comps = {"", "", "", "1,3"};

    SECTION("Deleting a filament below the references shifts them down") {
        remap_mixed_components_on_delete(is_mixed, comps, 2);
        REQUIRE(comps[3] == "1,2");
    }

    SECTION("Deleting a referenced filament zeroes that component") {
        remap_mixed_components_on_delete(is_mixed, comps, 1);
        // 1 -> 0 (deleted sentinel), 3 -> 2
        REQUIRE(comps[3] == "0,2");
    }
}

TEST_CASE("check_mixed_filament_type_consistency flags mismatched component types", "[FilamentMixer]")
{
    const std::vector<unsigned char> is_mixed  = {0, 0, 1};
    const std::vector<std::string>   comp_strs = {"", "", "1,2"};

    REQUIRE(check_mixed_filament_type_consistency(is_mixed, comp_strs, {"PLA", "PLA"}).empty());

    auto bad = check_mixed_filament_type_consistency(is_mixed, comp_strs, {"PLA", "PETG"});
    REQUIRE(bad == std::vector<size_t>{2});
}

TEST_CASE("a support-flagged component reads as its own filament type for the consistency check", "[FilamentMixer]")
{
    // The sidebar derives each component's type through DynamicPrintConfig::get_filament_type,
    // which folds filament_is_support into the type, so toggling that flag alone flips the
    // verdict and the mixed filament list has to be refreshed on filament_is_support too.
    DynamicPrintConfig plain_pla;
    plain_pla.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    plain_pla.set_key_value("filament_is_support", new ConfigOptionBools({false}));
    std::string displayed;
    REQUIRE(plain_pla.get_filament_type(displayed) == "PLA");

    DynamicPrintConfig support_pla;
    support_pla.set_key_value("filament_type", new ConfigOptionStrings({"PLA"}));
    support_pla.set_key_value("filament_is_support", new ConfigOptionBools({true}));
    REQUIRE(support_pla.get_filament_type(displayed) == "PLA-S");
    REQUIRE(displayed == "Sup.PLA");

    const std::vector<unsigned char> is_mixed  = {0, 0, 1};
    const std::vector<std::string>   comp_strs = {"", "", "1,2"};
    REQUIRE(check_mixed_filament_type_consistency(is_mixed, comp_strs, {"PLA", "PLA-S"}) == std::vector<size_t>{2});
}

TEST_CASE("gradient curves round-trip and sample monotonically", "[FilamentMixer]")
{
    SECTION("Empty input yields an empty curve") {
        REQUIRE(parse_gradient_curve("").empty());
        REQUIRE(serialize_gradient_curve(GradientCurve{}).empty());
    }

    SECTION("Legacy 2-field anchors survive a parse/serialize round trip") {
        GradientCurve c = parse_gradient_curve("0,0.15|0.5,0.5|1,0.85");
        REQUIRE(c.points.size() == 3);

        // Anchors with no tangent override serialize back to the 2-field legacy form
        // (canonical fixed-precision, so compare by re-parsing rather than by string).
        const std::string round_tripped = serialize_gradient_curve(c);
        REQUIRE(round_tripped.find(",nan") == std::string::npos);

        GradientCurve c2 = parse_gradient_curve(round_tripped);
        REQUIRE(c2.points.size() == c.points.size());
        for (size_t i = 0; i < c.points.size(); ++i) {
            REQUIRE_THAT(c2.points[i].x, Catch::Matchers::WithinAbs(c.points[i].x, 1e-4));
            REQUIRE_THAT(c2.points[i].y, Catch::Matchers::WithinAbs(c.points[i].y, 1e-4));
        }
    }

    SECTION("Sampling is clamped at the ends and monotone in between") {
        GradientCurve c = parse_gradient_curve("0,0.15|0.5,0.5|1,0.85");
        REQUIRE_THAT(sample_gradient_curve(c, 0.0), Catch::Matchers::WithinAbs(0.15, 1e-9));
        REQUIRE_THAT(sample_gradient_curve(c, 1.0), Catch::Matchers::WithinAbs(0.85, 1e-9));
        // Outside the control point range the end values are held.
        REQUIRE_THAT(sample_gradient_curve(c, -1.0), Catch::Matchers::WithinAbs(0.15, 1e-9));
        REQUIRE_THAT(sample_gradient_curve(c, 2.0), Catch::Matchers::WithinAbs(0.85, 1e-9));

        double prev = sample_gradient_curve(c, 0.0);
        for (int i = 1; i <= 20; ++i) {
            double v = sample_gradient_curve(c, i / 20.0);
            REQUIRE(v >= prev - 1e-9);
            prev = v;
        }
    }

    SECTION("A curve with fewer than two points falls back to 0.5") {
        GradientCurve c = parse_gradient_curve("0.5,0.7");
        REQUIRE_THAT(sample_gradient_curve(c, 0.3), Catch::Matchers::WithinAbs(0.5, 1e-9));
    }
}

TEST_CASE("blend_color mixes two hex colors", "[FilamentMixer]")
{
    // ratio 0 keeps the first color, ratio 1 the second.
    REQUIRE(blend_color("#FF0000", "#0000FF", 0.0f) == "#FF0000");
    REQUIRE(blend_color("#FF0000", "#0000FF", 1.0f) == "#0000FF");

    SECTION("Blue and yellow make green, not grey (pigment mixing)") {
        // The polynomial model approximates subtractive pigment behaviour.
        std::string mixed = blend_color("#0021D0", "#FCD300", 0.5f);
        REQUIRE(mixed.size() == 7);
        REQUIRE(mixed[0] == '#');
        auto comp = [&](int i) { return std::stoi(mixed.substr(1 + 2 * i, 2), nullptr, 16); };
        // Green channel should dominate red and blue.
        REQUIRE(comp(1) > comp(0));
        REQUIRE(comp(1) > comp(2));
    }
}

TEST_CASE("blend_color_multi weights components", "[FilamentMixer]")
{
    SECTION("A single component is returned unchanged") {
        REQUIRE(blend_color_multi({"#FF0000"}, {1}) == "#FF0000");
    }

    SECTION("Mixing a color with itself stays close to that color") {
        // The mixer is a degree-4 polynomial fit of pigment behaviour, so mixing a color with
        // itself lands near it rather than exactly on it; allow a small per-channel drift.
        std::string mixed = blend_color_multi({"#123456", "#123456"}, {1, 1});
        REQUIRE(mixed.size() == 7);
        auto comp = [](const std::string &hex, int i) {
            return std::stoi(hex.substr(1 + 2 * i, 2), nullptr, 16);
        };
        for (int i = 0; i < 3; ++i)
            REQUIRE(std::abs(comp(mixed, i) - comp("#123456", i)) <= 8);
    }
}

TEST_CASE("format_mixed_ratios normalises weights to four decimals", "[FilamentMixer]")
{
    REQUIRE(format_mixed_components({1, 3}) == "1,3");
    REQUIRE(format_mixed_ratios({50, 50}) == "0.5000,0.5000");
    REQUIRE(format_mixed_ratios({1, 2}) == "0.3333,0.6667");
    REQUIRE(format_mixed_ratios({1, 1}) == format_mixed_ratios({50, 50}));
}

TEST_CASE("find_fixed_mixed_filament reuses only a fixed slot of the same blend", "[FilamentMixer]")
{
    // Physical slots 0 and 1; slot 2 blends them 50:50 as a gradient, slot 3 at a fixed 50:50.
    DynamicPrintConfig cfg;
    cfg.set_key_value("filament_is_mixed", new ConfigOptionBools({false, false, true, true}));
    cfg.set_key_value("filament_mixed_components", new ConfigOptionStrings({"", "", "1,2", "1,2"}));
    cfg.set_key_value("filament_mixed_sublayer_ratios",
                      new ConfigOptionStrings({"", "", format_mixed_ratios({50, 50}), format_mixed_ratios({50, 50})}));
    cfg.set_key_value("filament_mixed_gradient", new ConfigOptionBools({false, false, true, false}));

    SECTION("The fixed slot is found, whatever scale the weights are given at") {
        REQUIRE(find_fixed_mixed_filament(cfg, {1, 2}, {50, 50}) == 3);
        REQUIRE(find_fixed_mixed_filament(cfg, {1, 2}, {1, 1}) == 3);
    }
    SECTION("A gradient slot with the same components and ratios is not a match") {
        cfg.option<ConfigOptionBools>("filament_is_mixed")->values[3] = false;
        REQUIRE(find_fixed_mixed_filament(cfg, {1, 2}, {50, 50}) == -1);
    }
    SECTION("A project without the gradient key still matches its fixed slots") {
        cfg.erase("filament_mixed_gradient");
        REQUIRE(find_fixed_mixed_filament(cfg, {1, 2}, {50, 50}) == 2);
    }
    SECTION("Another ratio or another component order is a different blend") {
        REQUIRE(find_fixed_mixed_filament(cfg, {1, 2}, {1, 2}) == -1);
        REQUIRE(find_fixed_mixed_filament(cfg, {2, 1}, {50, 50}) == -1);
    }
}
