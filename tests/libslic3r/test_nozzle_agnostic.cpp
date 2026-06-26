#include <catch2/catch_all.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/NozzleAgnostic.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

// Unit tests for the nozzle-agnostic conversion helpers in NozzleAgnostic.{hpp,cpp}:
// conversion math, the in-scope tag rule, idempotency, the abs->percent->abs round-trip
// against get_abs_value, mixed-nozzle detection, and base-nozzle resolution.

using namespace Slic3r;

TEST_CASE("nozzle_agnostic to_percent", "[nozzle_agnostic]")
{
    SECTION("to_percent divides the absolute width by the base nozzle") {
        CHECK(nozzle_agnostic::to_percent(0.42, 0.4) == Catch::Approx(105.0));
        CHECK(nozzle_agnostic::to_percent(0.20, 0.4) == Catch::Approx(50.0));
        CHECK(nozzle_agnostic::to_percent(0.60, 0.4) == Catch::Approx(150.0));
    }
}

TEST_CASE("nozzle_agnostic convert_field turns an absolute width into a percent", "[nozzle_agnostic]")
{
    const ConfigOptionFloatOrPercent abs(0.42, /*percent=*/false);
    const ConfigOptionFloatOrPercent out = nozzle_agnostic::convert_field(abs, 0.4);
    CHECK(out.percent == true);
    CHECK(out.value == Catch::Approx(105.0));
}

TEST_CASE("nozzle_agnostic convert_field rounds the percent to 0.1", "[nozzle_agnostic]")
{
    // 0.45 / 0.4 = 112.5% (exact, one decimal -> unchanged).
    CHECK(nozzle_agnostic::convert_field(ConfigOptionFloatOrPercent(0.45, false), 0.4).value
          == Catch::Approx(112.5));
    // 0.42 / 0.32 = 131.25% -> rounds to 131.3%.
    CHECK(nozzle_agnostic::convert_field(ConfigOptionFloatOrPercent(0.42, false), 0.32).value
          == Catch::Approx(131.3));
    // 0.5 / 0.6 = 83.333...% -> rounds to 83.3%.
    CHECK(nozzle_agnostic::convert_field(ConfigOptionFloatOrPercent(0.50, false), 0.6).value
          == Catch::Approx(83.3));
}

TEST_CASE("nozzle_agnostic one percent value auto-scales across nozzles", "[nozzle_agnostic]")
{
    // A percent of nozzle_diameter resolves to the correct absolute width on every nozzle.
    const ConfigOptionFloatOrPercent pct(105.0, /*percent=*/true);
    CHECK(pct.get_abs_value(0.4) == Catch::Approx(0.42));
    CHECK(pct.get_abs_value(0.2) == Catch::Approx(0.21));
    CHECK(pct.get_abs_value(0.6) == Catch::Approx(0.63));
    CHECK(pct.get_abs_value(0.8) == Catch::Approx(0.84));
}

TEST_CASE("nozzle_agnostic convert_field is idempotent", "[nozzle_agnostic]")
{
    SECTION("an already-percent field is returned unchanged") {
        const ConfigOptionFloatOrPercent pct(105.0, /*percent=*/true);
        const ConfigOptionFloatOrPercent once = nozzle_agnostic::convert_field(pct, 0.4);
        CHECK(once.percent == true);
        CHECK(once.value == Catch::Approx(105.0));
    }
    SECTION("converting twice does not drift") {
        const ConfigOptionFloatOrPercent abs(0.42, /*percent=*/false);
        const ConfigOptionFloatOrPercent once  = nozzle_agnostic::convert_field(abs, 0.4);
        const ConfigOptionFloatOrPercent twice = nozzle_agnostic::convert_field(once, 0.4);
        CHECK(twice.percent == once.percent);
        CHECK(twice.value == Catch::Approx(once.value));
    }
    SECTION("a non-positive base nozzle leaves the value untouched") {
        const ConfigOptionFloatOrPercent abs(0.42, /*percent=*/false);
        const ConfigOptionFloatOrPercent out = nozzle_agnostic::convert_field(abs, 0.0);
        CHECK(out.percent == false);
        CHECK(out.value == Catch::Approx(0.42));
    }
}

TEST_CASE("nozzle_agnostic abs -> percent -> abs round-trips", "[nozzle_agnostic]")
{
    const ConfigOptionFloatOrPercent abs(0.42, /*percent=*/false);
    const ConfigOptionFloatOrPercent pct = nozzle_agnostic::convert_field(abs, 0.4);
    CHECK(pct.get_abs_value(0.4) == Catch::Approx(0.42));
}

TEST_CASE("nozzle_agnostic is_mixed_nozzle detects distinct diameters", "[nozzle_agnostic]")
{
    CHECK_FALSE(nozzle_agnostic::is_mixed_nozzle({}));
    CHECK_FALSE(nozzle_agnostic::is_mixed_nozzle({0.4}));
    CHECK_FALSE(nozzle_agnostic::is_mixed_nozzle({0.4, 0.4}));
    CHECK(nozzle_agnostic::is_mixed_nozzle({0.4, 0.2, 0.6, 0.8}));
}

TEST_CASE("nozzle_agnostic resolve_base_nozzle resolves or refuses", "[nozzle_agnostic]")
{
    PresetBundle bundle;

    auto load_printer = [&bundle](const std::string &name, std::vector<double> nozzles) {
        DynamicPrintConfig cfg;
        cfg.set_key_value("nozzle_diameter", new ConfigOptionFloats(std::move(nozzles)));
        bundle.printers.load_preset(name + ".json", name, cfg, /*select=*/false);
    };
    load_printer("SingleNozzle", {0.4});
    load_printer("OtherNozzle",  {0.6});

    auto make_process = [](std::vector<std::string> printers) {
        Preset p(Preset::TYPE_PRINT, "Process");
        p.config.set_key_value("compatible_printers", new ConfigOptionStrings(std::move(printers)));
        return p;
    };

    SECTION("exactly one base nozzle resolves to that diameter") {
        const Preset process = make_process({"SingleNozzle"});
        const std::optional<double> base = nozzle_agnostic::resolve_base_nozzle(process, bundle);
        REQUIRE(base.has_value());
        CHECK(*base == Catch::Approx(0.4));
    }
    SECTION("two different bases refuse rather than guess") {
        const Preset process = make_process({"SingleNozzle", "OtherNozzle"});
        CHECK_FALSE(nozzle_agnostic::resolve_base_nozzle(process, bundle).has_value());
    }
    SECTION("no compatible printers refuses") {
        const Preset process = make_process({});
        CHECK_FALSE(nozzle_agnostic::resolve_base_nozzle(process, bundle).has_value());
    }
}

TEST_CASE("nozzle_agnostic has_absolute_scoped_fields gates on absolute widths", "[nozzle_agnostic]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : nozzle_agnostic::scoped_keys(print_config_def))
        cfg.set_deserialize_strict(key, "100%");
    CHECK_FALSE(nozzle_agnostic::has_absolute_scoped_fields(cfg));

    cfg.set_deserialize_strict("outer_wall_line_width", "0.42");
    CHECK(nozzle_agnostic::has_absolute_scoped_fields(cfg));
}

TEST_CASE("nozzle_agnostic scoped_keys follows the ratio_over tag rule", "[nozzle_agnostic]")
{
    const std::vector<std::string> keys = nozzle_agnostic::scoped_keys(print_config_def);
    auto has = [&keys](const char *k) {
        return std::find(keys.begin(), keys.end(), std::string(k)) != keys.end();
    };

    // Known nozzle-diameter-scaled line-width fields are in scope.
    CHECK(has("line_width"));
    CHECK(has("outer_wall_line_width"));
    CHECK(has("inner_wall_line_width"));
    CHECK(has("initial_layer_line_width"));
    CHECK(has("sparse_infill_line_width"));
    CHECK(has("internal_solid_infill_line_width"));
    CHECK(has("top_surface_line_width"));
    CHECK(has("support_line_width"));
    CHECK(has("bridge_line_width"));

    // A field that is not coFloatOrPercent-over-nozzle_diameter is excluded.
    CHECK_FALSE(has("wall_loops")); // coInt

    // The rule, not a hardcoded list: every scoped key satisfies the tag predicate.
    for (const std::string &key : keys) {
        const ConfigOptionDef &def = print_config_def.options.at(key);
        CHECK(def.type == coFloatOrPercent);
        CHECK(def.ratio_over == "nozzle_diameter");
        CHECK(nozzle_agnostic::is_in_scope(def));
    }

    // is_in_scope rejects an out-of-scope def directly.
    CHECK_FALSE(nozzle_agnostic::is_in_scope(print_config_def.options.at("wall_loops")));
}
