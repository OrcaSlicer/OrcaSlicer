#include <catch2/catch_all.hpp>

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
