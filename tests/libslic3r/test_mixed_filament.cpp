#include <catch2/catch_all.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/LocalZOrderOptimizer.hpp"
#include "libslic3r/LocalZPlanner.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCookbook.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/prusa_fdm_mixer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;

TEST_CASE("MixedFilament ratio 1:1 resolve", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1");
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.is_mixed(3, 2));
    REQUIRE_FALSE(mgr.is_mixed(1, 2));
    REQUIRE(mgr.total_filaments(2) == 3);
    // Layers alternate A, B
    REQUIRE(mgr.resolve(3, 2, 0) == 1);
    REQUIRE(mgr.resolve(3, 2, 1) == 2);
    REQUIRE(mgr.resolve(3, 2, 2) == 1);
    REQUIRE(mgr.resolve(3, 2, 3) == 2);
}

TEST_CASE("MixedFilament ratio 2:1 resolve", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,2,1");
    // cycle = 3: A,A,B
    REQUIRE(mgr.resolve(3, 2, 0) == 1);
    REQUIRE(mgr.resolve(3, 2, 1) == 1);
    REQUIRE(mgr.resolve(3, 2, 2) == 2);
    REQUIRE(mgr.resolve(3, 2, 3) == 1);
    REQUIRE(mgr.resolve(3, 2, 4) == 1);
    REQUIRE(mgr.resolve(3, 2, 5) == 2);
}

TEST_CASE("MixedFilament pattern 112 sequence", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    // A=1,B=2, pattern 112 → T0,T0,T1 (physical 1,1,2)
    mgr.load_definitions("1,2,1,1,1,112");
    REQUIRE(mgr.resolve(3, 2, 0) == 1);
    REQUIRE(mgr.resolve(3, 2, 1) == 1);
    REQUIRE(mgr.resolve(3, 2, 2) == 2);
    REQUIRE(mgr.resolve(3, 2, 3) == 1);
    REQUIRE(mgr.resolve(3, 2, 4) == 1);
    REQUIRE(mgr.resolve(3, 2, 5) == 2);
}

TEST_CASE("MixedFilament serialize load round-trip with pattern", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,2,1,112");
    const std::string ser = mgr.serialize_definitions();
    REQUIRE(ser.find("112") != std::string::npos);

    MixedFilamentManager round;
    round.load_definitions(ser);
    REQUIRE(round.enabled_count() == 1);
    REQUIRE(round.mixed_filaments().front().manual_pattern == "112");
    REQUIRE(round.mixed_filaments().front().ratio_a == 2);
    REQUIRE(round.mixed_filaments().front().ratio_b == 1);
    // Pattern wins over ratio
    REQUIRE(round.resolve(3, 2, 0) == 1);
    REQUIRE(round.resolve(3, 2, 1) == 1);
    REQUIRE(round.resolve(3, 2, 2) == 2);
}

TEST_CASE("MixedFilament total_filaments and is_mixed", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1;1,3,1,1,1");
    // Two enabled mixes on 4 physicals
    REQUIRE(mgr.enabled_count() == 2);
    REQUIRE(mgr.total_filaments(4) == 6);
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE(mgr.is_mixed(6, 4));
    REQUIRE_FALSE(mgr.is_mixed(4, 4));
    REQUIRE_FALSE(mgr.is_mixed(7, 4));
}

TEST_CASE("MixedFilament three pair-mixes on four physicals", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1;1,3,1,1,1;2,3,1,1,1");
    REQUIRE(mgr.enabled_count() == 3);
    REQUIRE(mgr.total_filaments(4) == 7);
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE(mgr.is_mixed(6, 4));
    REQUIRE(mgr.is_mixed(7, 4));
    REQUIRE_FALSE(mgr.is_mixed(4, 4));
    REQUIRE_FALSE(mgr.is_mixed(8, 4));

    // Independent cadences: ID5 1/2, ID6 1/3, ID7 2/3
    REQUIRE(mgr.resolve(5, 4, 0) == 1);
    REQUIRE(mgr.resolve(5, 4, 1) == 2);
    REQUIRE(mgr.resolve(6, 4, 0) == 1);
    REQUIRE(mgr.resolve(6, 4, 1) == 3);
    REQUIRE(mgr.resolve(7, 4, 0) == 2);
    REQUIRE(mgr.resolve(7, 4, 1) == 3);

    const std::string ser = mgr.serialize_definitions();
    MixedFilamentManager round;
    round.load_definitions(ser);
    REQUIRE(round.enabled_count() == 3);
    REQUIRE(round.total_filaments(4) == 7);
    REQUIRE(round.resolve(5, 4, 0) == 1);
    REQUIRE(round.resolve(5, 4, 1) == 2);
    REQUIRE(round.resolve(6, 4, 0) == 1);
    REQUIRE(round.resolve(6, 4, 1) == 3);
    REQUIRE(round.resolve(7, 4, 0) == 2);
    REQUIRE(round.resolve(7, 4, 1) == 3);
    REQUIRE(MixedFilamentManager::max_filament_id(ser, 4) == 7);
}

TEST_CASE("MixedFilament pattern separators normalize", "[MixedFilament]")
{
    REQUIRE(MixedFilamentManager::normalize_manual_pattern("1-1-2") == "112");
    REQUIRE(MixedFilamentManager::normalize_manual_pattern("1 1 2") == "112");
    // First group only for comma-grouped patterns
    REQUIRE(MixedFilamentManager::normalize_manual_pattern("12,21") == "12");
}

TEST_CASE("MixedFilament append_physical_0based", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1");
    std::vector<unsigned int> out;
    mgr.append_physical_0based(3, 2, out);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0] == 0);
    REQUIRE(out[1] == 1);
}

TEST_CASE("MixedFilament xa/xb offsets serialize without breaking M4 rows", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,2,1,xa0.2,xb-0.1");
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.mixed_filaments().front().ratio_a == 2);
    REQUIRE(mgr.mixed_filaments().front().ratio_b == 1);
    REQUIRE(mgr.mixed_filaments().front().component_a_surface_offset == Catch::Approx(0.2f));
    REQUIRE(mgr.mixed_filaments().front().component_b_surface_offset == Catch::Approx(-0.1f));

    const std::string ser = mgr.serialize_definitions();
    REQUIRE(ser.find("xa") != std::string::npos);
    REQUIRE(ser.find("xb") != std::string::npos);

    MixedFilamentManager m4;
    m4.load_definitions("1,2,1,2,1");
    REQUIRE(m4.serialize_definitions() == "1,2,1,2,1");
    REQUIRE(m4.resolve(3, 2, 0) == 1);
    REQUIRE(m4.resolve(3, 2, 2) == 2);

    REQUIRE(mgr.component_surface_offset(3, 2, 0) == Catch::Approx(0.2f));
    REQUIRE(mgr.component_surface_offset(3, 2, 2) == Catch::Approx(-0.1f));
}

TEST_CASE("LocalZ rematerialize stack holds first layer then 0.16+0.08", "[LocalZ][MixedFilament]")
{
    const std::vector<double> heights = rematerialize_layer_heights({0.20, 0.24, 0.24}, 2, 1, 0.08);
    REQUIRE(heights.size() == 5);
    REQUIRE(heights[0] == Catch::Approx(0.20));
    REQUIRE(heights[1] == Catch::Approx(0.16));
    REQUIRE(heights[2] == Catch::Approx(0.08));
    REQUIRE(heights[3] == Catch::Approx(0.16));
    REQUIRE(heights[4] == Catch::Approx(0.08));
}

TEST_CASE("Bias apply_surface_offset contracts a square", "[MixedFilament][Bias]")
{
    ExPolygon sq;
    sq.contour = {
        Point::new_scale(0, 0),
        Point::new_scale(10, 0),
        Point::new_scale(10, 10),
        Point::new_scale(0, 10),
    };
    const ExPolygons src = { sq };
    const ExPolygons contracted = apply_surface_offset(src, 0.2f);
    const ExPolygons expanded   = apply_surface_offset(src, -0.2f);
    REQUIRE_FALSE(contracted.empty());
    REQUIRE_FALSE(expanded.empty());
    REQUIRE(area(contracted) < area(src));
    REQUIRE(area(expanded) > area(src));
    REQUIRE(apply_surface_offset(src, 0.f).size() == src.size());
}

TEST_CASE("LocalZ pair height 0.24 at 2:1 splits to 0.16+0.08", "[LocalZ][MixedFilament]")
{
    const LocalZPassHeights h = plan_local_z_pair_heights(0.24, 2, 1, 0.08);
    REQUIRE(h.split);
    REQUIRE(h.height_a == Catch::Approx(0.16));
    REQUIRE(h.height_b == Catch::Approx(0.08));
}

TEST_CASE("LocalZ pair height 0.20 at 2:1 refuses below min 0.08", "[LocalZ][MixedFilament]")
{
    const LocalZPassHeights h = plan_local_z_pair_heights(0.20, 2, 1, 0.08);
    REQUIRE_FALSE(h.split);
    REQUIRE(h.height_a == Catch::Approx(0.20));
}

TEST_CASE("LocalZ pair height 0.20 at 1:1 splits to 0.10+0.10", "[LocalZ][MixedFilament]")
{
    const LocalZPassHeights h = plan_local_z_pair_heights(0.20, 1, 1, 0.08);
    REQUIRE(h.split);
    REQUIRE(h.height_a == Catch::Approx(0.10));
    REQUIRE(h.height_b == Catch::Approx(0.10));
}

TEST_CASE("LocalZ pattern rows do not split", "[LocalZ][MixedFilament]")
{
    MixedFilament mf;
    mf.ratio_a = 2;
    mf.ratio_b = 1;
    mf.manual_pattern = "112";
    const LocalZPassHeights h = plan_local_z_pair_heights(0.24, mf, 0.08);
    REQUIRE_FALSE(h.split);
}

TEST_CASE("LocalZ height-gradient interpolates 100:0 to 0:100", "[LocalZ][MixedFilament]")
{
    const auto bottom = interpolate_pair_ratio_by_z(0.0);
    const auto mid    = interpolate_pair_ratio_by_z(0.5);
    const auto top    = interpolate_pair_ratio_by_z(1.0);
    REQUIRE(bottom.first == 100);
    REQUIRE(bottom.second == 0);
    REQUIRE(mid.first == 50);
    REQUIRE(mid.second == 50);
    REQUIRE(top.first == 0);
    REQUIRE(top.second == 100);

    const LocalZPassHeights mid_h = plan_local_z_pair_heights(0.24, mid.first, mid.second, 0.08);
    REQUIRE(mid_h.split);
    REQUIRE(mid_h.height_a == Catch::Approx(0.12));
    REQUIRE(mid_h.height_b == Catch::Approx(0.12));

    MixedFilament mf;
    mf.component_a = 1;
    mf.component_b = 2;
    REQUIRE(gradient_fallback_extruder_1based(mf, 100, 0, 4) == 1u);
    REQUIRE(gradient_fallback_extruder_1based(mf, 0, 100, 4) == 2u);
    // Unsplittable 3:1 at 0.20 mm (0.05 < 0.08) → dominant A, not M4 layer cadence.
    REQUIRE_FALSE(plan_local_z_pair_heights(0.20, 3, 1, 0.08).split);
    REQUIRE(gradient_fallback_extruder_1based(mf, 3, 1, 4) == 1u);
    REQUIRE(gradient_fallback_extruder_1based(mf, 1, 3, 4) == 2u);
}

TEST_CASE("LocalZInterval and SubLayerPlan defaults", "[LocalZ]")
{
    LocalZInterval interval;
    REQUIRE(interval.layer_id == 0);
    REQUIRE(interval.sublayer_count == 0);
    REQUIRE_FALSE(interval.has_mixed_paint);

    SubLayerPlan plan;
    REQUIRE(plan.layer_id == 0);
    REQUIRE(plan.pass_index == 0);
    REQUIRE_FALSE(plan.split_interval);
    REQUIRE(plan.extruder_1based == 0);
}

TEST_CASE("LocalZOrderOptimizer: bucket_contains_extruder finds present IDs", "[LocalZOrderOptimizer]")
{
    using namespace Slic3r::LocalZOrderOptimizer;
    const std::vector<unsigned int> bucket = {1, 3, 5};
    CHECK(bucket_contains_extruder(bucket, 1));
    CHECK(bucket_contains_extruder(bucket, 3));
    CHECK_FALSE(bucket_contains_extruder(bucket, 2));
    CHECK_FALSE(bucket_contains_extruder(bucket, -1));
}

TEST_CASE("LocalZOrderOptimizer: order_bucket_extruders rotates current extruder to front", "[LocalZOrderOptimizer]")
{
    using namespace Slic3r::LocalZOrderOptimizer;
    const std::vector<unsigned int> result = order_bucket_extruders({1, 2, 3}, 2);
    REQUIRE(result.size() == 3);
    CHECK(result.front() == 2u);
}

TEST_CASE("LocalZOrderOptimizer: order_bucket_extruders moves preferred_last to back", "[LocalZOrderOptimizer]")
{
    using namespace Slic3r::LocalZOrderOptimizer;
    const std::vector<unsigned int> result = order_bucket_extruders({1, 2, 3}, 1, 2);
    REQUIRE(result.size() == 3);
    CHECK(result.front() == 1u);
    CHECK(result.back() == 2u);
}

TEST_CASE("LocalZOrderOptimizer: order_pass_group prefers bucket containing active extruder", "[LocalZOrderOptimizer]")
{
    using namespace Slic3r::LocalZOrderOptimizer;
    const std::vector<std::vector<unsigned int>> group = {
        {1, 2},
        {3, 4},
        {5, 6},
    };
    const std::vector<size_t> order = order_pass_group(group, 3);
    REQUIRE(order.size() == 3);
    CHECK(order[0] == 1u);
}

TEST_CASE("MixedFilament 3-component c3,rc1 resolve and empty pattern", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1,c3,rc1");
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.mixed_filaments().front().component_c == 3u);
    REQUIRE(mgr.mixed_filaments().front().ratio_c == 1);
    REQUIRE(mgr.mixed_filaments().front().manual_pattern.empty());
    REQUIRE(mgr.resolve(5, 4, 0) == 1);
    REQUIRE(mgr.resolve(5, 4, 1) == 2);
    REQUIRE(mgr.resolve(5, 4, 2) == 3);
    REQUIRE(mgr.resolve(5, 4, 3) == 1);
    REQUIRE(mgr.serialize_definitions() == "1,2,1,1,1,c3,rc1");

    MixedFilamentManager c_only;
    c_only.load_definitions("1,2,1,1,1,c3");
    REQUIRE(c_only.mixed_filaments().front().manual_pattern.empty());
    REQUIRE(c_only.mixed_filaments().front().component_c == 3u);
    REQUIRE(c_only.mixed_filaments().front().ratio_c == 1);
    REQUIRE(c_only.serialize_definitions() == "1,2,1,1,1,c3,rc1");
}

TEST_CASE("MixedFilament pattern 123 without cN still 1,2,3", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1,123");
    REQUIRE(mgr.mixed_filaments().front().component_c == 0u);
    REQUIRE(mgr.mixed_filaments().front().manual_pattern == "123");
    REQUIRE(mgr.resolve(5, 4, 0) == 1);
    REQUIRE(mgr.resolve(5, 4, 1) == 2);
    REQUIRE(mgr.resolve(5, 4, 2) == 3);
}

TEST_CASE("MixedFilament pattern token 3 maps to C when C is not physical 3", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1,123,c4");
    REQUIRE(mgr.mixed_filaments().front().component_c == 4u);
    REQUIRE(mgr.mixed_filaments().front().manual_pattern == "123");
    REQUIRE(mgr.resolve(5, 4, 0) == 1);
    REQUIRE(mgr.resolve(5, 4, 1) == 2);
    REQUIRE(mgr.resolve(5, 4, 2) == 4);
}

TEST_CASE("MixedFilament pair serialize stays exact 1,2,1,1,1", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1");
    REQUIRE(mgr.serialize_definitions() == "1,2,1,1,1");
    REQUIRE(mgr.mixed_filaments().front().component_c == 0u);
    REQUIRE(mgr.mixed_filaments().front().ratio_c == 0);
}

TEST_CASE("MixedFilament 0006 six-pair string IDs 5-10 cadences unchanged", "[MixedFilament]")
{
    const std::string six =
        "1,2,1,1,1;1,3,1,1,1;2,3,1,1,1;1,4,1,1,1;2,4,1,1,1;3,4,1,1,1";
    MixedFilamentManager mgr;
    mgr.load_definitions(six);
    REQUIRE(mgr.enabled_count() == 6);
    REQUIRE(mgr.total_filaments(4) == 10);
    REQUIRE(mgr.is_mixed(5, 4));
    REQUIRE(mgr.is_mixed(10, 4));
    REQUIRE_FALSE(mgr.is_mixed(4, 4));
    REQUIRE_FALSE(mgr.is_mixed(11, 4));

    REQUIRE(mgr.resolve(5, 4, 0) == 1);
    REQUIRE(mgr.resolve(5, 4, 1) == 2);
    REQUIRE(mgr.resolve(6, 4, 0) == 1);
    REQUIRE(mgr.resolve(6, 4, 1) == 3);
    REQUIRE(mgr.resolve(7, 4, 0) == 2);
    REQUIRE(mgr.resolve(7, 4, 1) == 3);
    REQUIRE(mgr.resolve(8, 4, 0) == 1);
    REQUIRE(mgr.resolve(8, 4, 1) == 4);
    REQUIRE(mgr.resolve(9, 4, 0) == 2);
    REQUIRE(mgr.resolve(9, 4, 1) == 4);
    REQUIRE(mgr.resolve(10, 4, 0) == 3);
    REQUIRE(mgr.resolve(10, 4, 1) == 4);
    REQUIRE(mgr.serialize_definitions() == six);
}

TEST_CASE("MixedFilament append_physical_0based includes C and pattern 123", "[MixedFilament]")
{
    MixedFilamentManager ratio;
    ratio.load_definitions("1,2,1,1,1,c3,rc1");
    std::vector<unsigned int> out_ratio;
    ratio.append_physical_0based(5, 4, out_ratio);
    REQUIRE(out_ratio == std::vector<unsigned int>{0, 1, 2});

    MixedFilamentManager pat_unset;
    pat_unset.load_definitions("1,2,1,1,1,123");
    std::vector<unsigned int> out_unset;
    pat_unset.append_physical_0based(5, 4, out_unset);
    REQUIRE(out_unset == std::vector<unsigned int>{0, 1, 2});

    MixedFilamentManager pat_set;
    pat_set.load_definitions("1,2,1,1,1,123,c3");
    REQUIRE(pat_set.mixed_filaments().front().component_c == 3u);
    std::vector<unsigned int> out_set;
    pat_set.append_physical_0based(5, 4, out_set);
    REQUIRE(out_set == std::vector<unsigned int>{0, 1, 2});
}

TEST_CASE("LocalZ three-component c3 row does not split", "[LocalZ][MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1,c3,rc1");
    REQUIRE(mgr.enabled_count() == 1);
    const LocalZPassHeights h = plan_local_z_pair_heights(0.24, mgr.mixed_filaments().front(), 0.08);
    REQUIRE_FALSE(h.split);
    REQUIRE(h.height_a == Catch::Approx(0.24));
}

TEST_CASE("MixedFilament A=3,B=4,C=1 1:1:1 hits those physicals", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("3,4,1,1,1,c1,rc1");
    REQUIRE(mgr.mixed_filaments().front().component_a == 3u);
    REQUIRE(mgr.mixed_filaments().front().component_b == 4u);
    REQUIRE(mgr.mixed_filaments().front().component_c == 1u);
    REQUIRE(mgr.resolve(5, 4, 0) == 3);
    REQUIRE(mgr.resolve(5, 4, 1) == 4);
    REQUIRE(mgr.resolve(5, 4, 2) == 1);
}

TEST_CASE("MixedFilament C-layer surface offset is 0 even with xb", "[MixedFilament]")
{
    MixedFilamentManager mgr;
    mgr.load_definitions("1,2,1,1,1,c3,rc1,xb0.2");
    REQUIRE(mgr.mixed_filaments().front().component_b_surface_offset == Catch::Approx(0.2f));
    REQUIRE(mgr.resolve(5, 4, 2) == 3);
    REQUIRE(mgr.component_surface_offset(5, 4, 2) == Catch::Approx(0.f));
    REQUIRE(mgr.component_surface_offset(5, 4, 1) == Catch::Approx(0.2f));
}

TEST_CASE("mixed_filament_painted_ids_would_shift on C-only or rc-only edit", "[MixedFilament]")
{
    const std::string base = "1,2,1,1,1,c3,rc1";
    const std::string c_only = "1,2,1,1,1,c4,rc1";
    const std::string rc_only = "1,2,1,1,1,c3,rc2";
    CHECK(mixed_filament_painted_ids_would_shift(base, c_only, 4, {5}));
    CHECK(mixed_filament_painted_ids_would_shift(base, rc_only, 4, {5}));
    CHECK_FALSE(mixed_filament_painted_ids_would_shift(base, base, 4, {5}));
}

TEST_CASE("spectrum_stamp_legacy_process_gradient tags pair rows only", "[spectrum_legacy_g_stamp]")
{
    MixedFilament pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;

    MixedFilament patterned = pair;
    patterned.manual_pattern = "112";

    MixedFilament c3 = pair;
    c3.component_c = 3;
    c3.ratio_c     = 1;

    MixedFilament disabled_pair = pair;
    disabled_pair.enabled = false;

    {
        std::vector<MixedFilament> rows{pair, patterned, c3, disabled_pair};
        spectrum_stamp_legacy_process_gradient(rows, true);
        REQUIRE(rows[0].gradient_enabled);
        REQUIRE_FALSE(rows[1].gradient_enabled);
        REQUIRE_FALSE(rows[2].gradient_enabled);
        REQUIRE(rows[3].gradient_enabled);
    }
    {
        std::vector<MixedFilament> rows{pair, patterned, c3};
        spectrum_stamp_legacy_process_gradient(rows, false);
        REQUIRE_FALSE(rows[0].gradient_enabled);
        REQUIRE_FALSE(rows[1].gradient_enabled);
        REQUIRE_FALSE(rows[2].gradient_enabled);
    }
    {
        MixedFilament already_g = pair;
        already_g.gradient_enabled = true;
        std::vector<MixedFilament> rows{already_g, pair};
        spectrum_stamp_legacy_process_gradient(rows, true);
        REQUIRE(rows[0].gradient_enabled);
        REQUIRE_FALSE(rows[1].gradient_enabled);
    }
}

TEST_CASE("mixed_filament_painted_ids_would_shift 0005 stamp exception", "[spectrum_legacy_g_stamp]")
{
    // Stamp-only pair false→true is not a shift when process on and old has no g.
    CHECK_FALSE(mixed_filament_painted_ids_would_shift("1,2,1,1,1", "1,2,1,1,1,g", 4, {5}, true));
    // Process off / omitted 5th arg: real g add → shift.
    CHECK(mixed_filament_painted_ids_would_shift("1,2,1,1,1", "1,2,1,1,1,g", 4, {5}, false));
    CHECK(mixed_filament_painted_ids_would_shift("1,2,1,1,1", "1,2,1,1,1,g", 4, {5}));

    // New-world: Mix 5 g off.
    CHECK(mixed_filament_painted_ids_would_shift("1,2,1,1,1,g;1,3,1,1,1", "1,2,1,1,1;1,3,1,1,1", 4, {5},
                                                 true));
    // New-world: Mix 6 g on.
    CHECK(mixed_filament_painted_ids_would_shift("1,2,1,1,1,g;1,3,1,1,1", "1,2,1,1,1,g;1,3,1,1,1,g", 4,
                                                 {6}, true));

    // Legacy leftover: Mix 6 left untagged while Mix 5 stamped.
    CHECK(mixed_filament_painted_ids_would_shift("1,2,1,1,1;1,3,1,1,1", "1,2,1,1,1,g;1,3,1,1,1", 4, {6},
                                                 true));
    CHECK_FALSE(mixed_filament_painted_ids_would_shift("1,2,1,1,1;1,3,1,1,1", "1,2,1,1,1,g;1,3,1,1,1", 4,
                                                       {5}, true));

    // Stamp + ratio still shifts.
    CHECK(mixed_filament_painted_ids_would_shift("1,2,1,1,1", "1,2,1,2,1,g", 4, {5}, true));

    // Pattern sibling left untagged was never interpolating → not a shift.
    CHECK_FALSE(mixed_filament_painted_ids_would_shift("1,2,1,1,1,112;1,3,1,1,1",
                                                       "1,2,1,1,1,112;1,3,1,1,1,g", 4, {5}, true));
}

namespace {

ColorRGB decode_hex_or_fail(const std::string &hex)
{
    ColorRGB c;
    REQUIRE(decode_color(hex, c));
    return c;
}

std::vector<ColorRGB> panchroma_physicals()
{
    return {
        decode_hex_or_fail("#08ABFB"), // 1 C
        decode_hex_or_fail("#D93B90"), // 2 M
        decode_hex_or_fail("#F9ED3D"), // 3 Y
        decode_hex_or_fail("#9199A4"), // 4 Grey K
    };
}

std::vector<ColorRGB> rgbw_physicals()
{
    return {
        decode_hex_or_fail("#E72F1D"), // 1 R
        decode_hex_or_fail("#06924D"), // 2 G
        decode_hex_or_fail("#003776"), // 3 B
        decode_hex_or_fail("#EBF7FF"), // 4 W
    };
}

int mix_period(const MixedFilament &mf)
{
    int p = mf.ratio_a + mf.ratio_b;
    if (mf.component_c != 0)
        p += mf.ratio_c;
    return p;
}

} // namespace

TEST_CASE("Match pure C and Grey are Physical slots", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const MixMatchResult        cyan = match_printable_mix(phys[0], phys);
    REQUIRE(cyan.valid);
    REQUIRE(cyan.kind == MixMatchResult::Kind::Physical);
    REQUIRE(cyan.physical_id == 1u);
    REQUIRE(cyan.recipe_row.empty());

    const MixMatchResult grey = match_printable_mix(phys[3], phys);
    REQUIRE(grey.valid);
    REQUIRE(grey.kind == MixMatchResult::Kind::Physical);
    REQUIRE(grey.physical_id == 4u);
    REQUIRE(grey.recipe_row.empty());
}

TEST_CASE("Match black maps to Grey physical not a CMY stack", "[MixedFilamentMatch]")
{
    const MixMatchResult r = match_printable_mix(decode_hex_or_fail("#000000"), panchroma_physicals());
    REQUIRE(r.valid);
    REQUIRE(r.kind == MixMatchResult::Kind::Physical);
    REQUIRE(r.physical_id == 4u);
    REQUIRE(r.recipe_row.empty());
}

TEST_CASE("prusa_fdm_mixer golden #009bc3+#f6b921 1:1 is #519e5f", "[MixedFilamentMatch]")
{
    const std::vector<prusa_fdm_mixer::Part> parts = {
        { "#009bc3", 0.5 },
        { "#f6b921", 0.5 },
    };
    const prusa_fdm_mixer::RGB rgb = prusa_fdm_mixer::mix_rgb(parts);
    const std::string          hex = prusa_fdm_mixer::mix(parts);
    auto lower = [](std::string s) {
        for (char &c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    REQUIRE(lower(hex) == "#519e5f");
    REQUIRE(rgb.r == 0x51);
    REQUIRE(rgb.g == 0x9e);
    REQUIRE(rgb.b == 0x5f);
}

TEST_CASE("prusa_fdm_mixer gradient-safe single part ratio 1.0", "[MixedFilamentMatch]")
{
    const std::vector<prusa_fdm_mixer::Part> parts = { { "#08ABFB", 1.0 } };
    REQUIRE(prusa_fdm_mixer::mix(parts) == "#08abfb");
    const prusa_fdm_mixer::RGB rgb = prusa_fdm_mixer::mix_rgb(parts);
    REQUIRE(rgb.r == 0x08);
    REQUIRE(rgb.g == 0xab);
    REQUIRE(rgb.b == 0xfb);
}

TEST_CASE("Predicted A==B C+C 1:1 returns physical Cyan", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    MixedFilament               same;
    same.component_a = 1;
    same.component_b = 1;
    same.ratio_a     = 1;
    same.ratio_b     = 1;
    same.enabled     = true;
    const ColorRGB got = predicted_swatch_for_mix(same, phys);
    REQUIRE(got == phys[0]);
    REQUIRE(got == decode_hex_or_fail("#08ABFB"));
}

TEST_CASE("Match C+M 1:1 predicted serializes gcd-reduced 1,2,1,1,1", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    MixedFilament               pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    pair.enabled     = true;
    const ColorRGB         yn = predicted_swatch_for_mix(pair, phys);
    const MixMatchResult   r  = match_printable_mix(yn, phys);
    REQUIRE(r.valid);
    REQUIRE(r.kind == MixMatchResult::Kind::Mix);
    REQUIRE(r.mix.component_a == 1u);
    REQUIRE(r.mix.component_b == 2u);
    REQUIRE(r.mix.component_c == 0u);
    REQUIRE(r.recipe_row == "1,2,1,1,1");
    REQUIRE(r.recipe_row.find('c') == std::string::npos);
    REQUIRE(mix_period(r.mix) >= 2);
    REQUIRE(mix_period(r.mix) <= 3);

    MixedFilamentManager mgr;
    mgr.load_definitions(r.recipe_row);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.serialize_definitions() == "1,2,1,1,1");

    const ColorRGB avg = lerp(phys[0], phys[1], 0.5f);
    REQUIRE(std::abs(yn.r() - avg.r()) + std::abs(yn.g() - avg.g()) + std::abs(yn.b() - avg.b()) > 1e-4f);
}

TEST_CASE("Match linear C+M midpoint is a pair of 1+2 without cN", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const ColorRGB              mid  = lerp(phys[0], phys[1], 0.5f);
    const MixMatchResult        r    = match_printable_mix(mid, phys);
    REQUIRE(r.valid);
    REQUIRE(r.kind == MixMatchResult::Kind::Mix);
    REQUIRE(r.mix.component_c == 0u);
    REQUIRE(r.recipe_row.find('c') == std::string::npos);
    const bool pair_12 = (r.mix.component_a == 1u && r.mix.component_b == 2u) ||
                         (r.mix.component_a == 2u && r.mix.component_b == 1u);
    REQUIRE(pair_12);
    REQUIRE(mix_period(r.mix) >= 2);
    REQUIRE(mix_period(r.mix) <= 3);
}

TEST_CASE("Match brown is a short CMY mix that round-trips", "[MixedFilamentMatch]")
{
    const MixMatchResult r = match_printable_mix(decode_hex_or_fail("#99401B"), panchroma_physicals());
    REQUIRE(r.valid);
    REQUIRE(r.kind == MixMatchResult::Kind::Mix);
    REQUIRE(mix_period(r.mix) <= 4);
    REQUIRE(mix_period(r.mix) >= 2);
    std::set<unsigned> ids = {r.mix.component_a, r.mix.component_b};
    if (r.mix.component_c != 0)
        ids.insert(r.mix.component_c);
    const int cmy = int(ids.count(1u) + ids.count(2u) + ids.count(3u));
    REQUIRE(cmy >= 2);
    REQUIRE_FALSE(r.recipe_row.empty());

    MixedFilamentManager mgr;
    mgr.load_definitions(r.recipe_row);
    REQUIRE(mgr.enabled_count() == 1);
    REQUIRE(mgr.serialize_definitions() == r.recipe_row);
}

TEST_CASE("Match mountain earth is a mix not Grey-only", "[MixedFilamentMatch]")
{
    const MixMatchResult r = match_printable_mix(decode_hex_or_fail("#A47C6F"), panchroma_physicals());
    REQUIRE(r.valid);
    REQUIRE(r.kind == MixMatchResult::Kind::Mix);
    REQUIRE(mix_period(r.mix) <= 4);
    REQUIRE(r.physical_id != 4u);
    const bool grey_only = r.mix.component_a == 4u && r.mix.component_b == 4u && r.mix.component_c == 0u;
    REQUIRE_FALSE(grey_only);
}

TEST_CASE("Match is stable for the same target", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const ColorRGB              brown = decode_hex_or_fail("#99401B");
    const MixMatchResult        a     = match_printable_mix(brown, phys);
    const MixMatchResult        b     = match_printable_mix(brown, phys);
    REQUIRE(a.valid);
    REQUIRE(a.kind == b.kind);
    REQUIRE(a.recipe_row == b.recipe_row);
    REQUIRE(a.physical_id == b.physical_id);
}

TEST_CASE("Match #RRGGBBFF equals #RRGGBB", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const MixMatchResult        a    = match_printable_mix(decode_hex_or_fail("#08ABFB"), phys);
    const MixMatchResult        b    = match_printable_mix(decode_hex_or_fail("#08ABFBFF"), phys);
    REQUIRE(a.valid);
    REQUIRE(b.valid);
    REQUIRE(a.kind == b.kind);
    REQUIRE(a.physical_id == b.physical_id);
    REQUIRE(a.recipe_row == b.recipe_row);
    REQUIRE(normalize_mix_match_hex("  99401b") == "#99401B");
    REQUIRE(normalize_mix_match_hex("#99401BFF") == "#99401BFF");
    REQUIRE(normalize_mix_match_hex("not-hex").empty());
}

TEST_CASE("spectrum_match_same_target compares decoded ColorRGB", "[MixedFilamentMatch]")
{
    const ColorRGB a = decode_hex_or_fail("#CE921A");
    const ColorRGB b = decode_hex_or_fail("#ce921a");
    const ColorRGB c = decode_hex_or_fail("#CE921AFF");
    const ColorRGB d = decode_hex_or_fail("#99401B");

    CHECK_FALSE(spectrum_match_same_target(false, a, a));
    CHECK(spectrum_match_same_target(true, a, a));
    CHECK_FALSE(spectrum_match_same_target(true, a, d));
    CHECK(spectrum_match_same_target(true, a, b));
    CHECK(spectrum_match_same_target(true, a, c));
    CHECK(spectrum_match_same_target(true, b, c));
}

TEST_CASE("Predicted swatch uses all live physicals not a 4-slot truncate", "[MixedFilamentMatch]")
{
    std::vector<ColorRGB> six = panchroma_physicals();
    six.push_back(decode_hex_or_fail("#FF8800"));
    six.push_back(decode_hex_or_fail("#00FF88"));
    const std::vector<ColorRGB> first4(six.begin(), six.begin() + 4);

    MixedFilament pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    pair.enabled     = true;

    const ColorRGB on_six  = predicted_swatch_for_mix(pair, six);
    const ColorRGB on_four = predicted_swatch_for_mix(pair, first4);
    REQUIRE(on_six != on_four);

    const ColorRGB yn4  = predicted_swatch_for_mix(pair, panchroma_physicals());
    const ColorRGB avg4 = lerp(panchroma_physicals()[0], panchroma_physicals()[1], 0.5f);
    REQUIRE(yn4 == on_four);
    const float yn_vs_lerp = std::abs(yn4.r() - avg4.r()) + std::abs(yn4.g() - avg4.g()) + std::abs(yn4.b() - avg4.b());
    REQUIRE_THAT(yn_vs_lerp, !Catch::Matchers::WithinAbs(0.f, 1e-4f));

    MixedFilament slot5;
    slot5.component_a = 5;
    slot5.component_b = 5;
    slot5.ratio_a     = 1;
    slot5.ratio_b     = 1;
    slot5.enabled     = true;
    const ColorRGB got5       = predicted_swatch_for_mix(slot5, six);
    const ColorRGB old_clamp  = predicted_swatch_for_mix(slot5, first4);

    MixedFilament slot4;
    slot4.component_a = 4;
    slot4.component_b = 4;
    slot4.ratio_a     = 1;
    slot4.ratio_b     = 1;
    slot4.enabled     = true;
    // n!=4 → no default Panchroma TDs; 1:1 of slot 5 equals mixing physicals[4].
    const std::vector<ColorRGB> mix_slot5{six[4], six[4]};
    MixedFilament               as_pair;
    as_pair.component_a = 1;
    as_pair.component_b = 2;
    as_pair.ratio_a     = 1;
    as_pair.ratio_b     = 1;
    REQUIRE(got5 == predicted_swatch_for_mix(as_pair, mix_slot5));
    REQUIRE(got5 != six[3]);
    REQUIRE(got5 != predicted_swatch_for_mix(slot4, six));
    REQUIRE(got5 != old_clamp);
    REQUIRE(old_clamp == predicted_swatch_for_mix(slot4, first4));
}

TEST_CASE("Match ignores physicals beyond slot 4", "[MixedFilamentMatch]")
{
    std::vector<ColorRGB> phys = panchroma_physicals();
    phys.push_back(decode_hex_or_fail("#FF00FF"));
    const MixMatchResult neon = match_printable_mix(phys.back(), phys);
    REQUIRE(neon.valid);
    if (neon.kind == MixMatchResult::Kind::Physical)
        REQUIRE(neon.physical_id <= 4u);
    else {
        REQUIRE(neon.mix.component_a <= 4u);
        REQUIRE(neon.mix.component_b <= 4u);
        REQUIRE(neon.mix.component_c <= 4u);
        REQUIRE(neon.recipe_row.find("c5") == std::string::npos);
    }

    const MixMatchResult cyan = match_printable_mix(phys[0], phys);
    REQUIRE(cyan.valid);
    REQUIRE(cyan.kind == MixMatchResult::Kind::Physical);
    REQUIRE(cyan.physical_id == 1u);
}

TEST_CASE("Match 0 or 1 physical does not crash", "[MixedFilamentMatch]")
{
    const ColorRGB black = ColorRGB::BLACK();
    const MixMatchResult empty = match_printable_mix(black, {});
    REQUIRE_FALSE(empty.valid);

    const ColorRGB one = decode_hex_or_fail("#08ABFB");
    const MixMatchResult single = match_printable_mix(one, {one});
    REQUIRE(single.valid);
    REQUIRE(single.kind == MixMatchResult::Kind::Physical);
    REQUIRE(single.physical_id == 1u);
    REQUIRE(single.recipe_row.empty());

    const MixMatchResult other = match_printable_mix(black, {one});
    REQUIRE(other.valid);
    REQUIRE(other.kind == MixMatchResult::Kind::Physical);
}

TEST_CASE("Preview colors append one ColorMix mix after physicals", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const std::vector<ColorRGB> out  = preview_filament_colors(phys, "1,2,1,1,1");
    REQUIRE(out.size() == 5);
    REQUIRE(out[0] == phys[0]);

    MixedFilament pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    pair.enabled     = true;
    REQUIRE(out[4] == predicted_swatch_for_mix(pair, phys));
    REQUIRE(out[4] != out[0]);
}

TEST_CASE("Preview colors empty mix string equals physicals", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const std::vector<ColorRGB> out  = preview_filament_colors(phys, "");
    REQUIRE(out.size() == 4);
    REQUIRE(out == phys);
}

TEST_CASE("Preview colors drop disabled mix rows", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const std::vector<ColorRGB> out  = preview_filament_colors(phys, "1,2,0,1,1");
    REQUIRE(out.size() == 4);
    REQUIRE(out == phys);
}

TEST_CASE("Preview colors 3-component mix is not a first-component clone", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    const std::vector<ColorRGB> out  = preview_filament_colors(phys, "1,2,1,1,1,c3,rc1");
    REQUIRE(out.size() == 5);

    MixedFilament triple;
    triple.component_a = 1;
    triple.component_b = 2;
    triple.component_c = 3;
    triple.ratio_a     = 1;
    triple.ratio_b     = 1;
    triple.ratio_c     = 1;
    triple.enabled     = true;
    REQUIRE(out[4] == predicted_swatch_for_mix(triple, phys));
    REQUIRE(out[4] != phys[0]);
    REQUIRE(out[4] != phys[1]);
    REQUIRE(out[4] != phys[2]);
}

TEST_CASE("Preview colors cap at 16 total", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys = panchroma_physicals();
    std::string defs;
    for (int i = 0; i < 12; ++i) {
        if (!defs.empty())
            defs += ';';
        defs += "1,2,1,1,1";
    }
    const std::vector<ColorRGB> out = preview_filament_colors(phys, defs);
    REQUIRE(out.size() == 16);
}

TEST_CASE("Preview colors keep all 17 physicals without mix append", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> physicals = {
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
        decode_hex_or_fail("#FF0000"),
        decode_hex_or_fail("#00FF00"),
        decode_hex_or_fail("#0000FF"),
        decode_hex_or_fail("#FFFF00"),
        decode_hex_or_fail("#FF00FF"),
        decode_hex_or_fail("#00FFFF"),
        decode_hex_or_fail("#111111"),
        decode_hex_or_fail("#222222"),
        decode_hex_or_fail("#333333"),
        decode_hex_or_fail("#444444"),
        decode_hex_or_fail("#555555"),
        decode_hex_or_fail("#666666"),
        decode_hex_or_fail("#777777"),
    };
    REQUIRE(physicals.size() == 17);
    const std::vector<ColorRGB> out = preview_filament_colors(physicals, "1,2,1,1,1");
    REQUIRE(out.size() == 17);
    for (size_t i = 0; i < 17; ++i)
        REQUIRE(out[i] == physicals[i]);
}

TEST_CASE("Preview colors keep all 16 physicals without mix append", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> physicals = {
        decode_hex_or_fail("#08ABFB"),
        decode_hex_or_fail("#D93B90"),
        decode_hex_or_fail("#F9ED3D"),
        decode_hex_or_fail("#9199A4"),
        decode_hex_or_fail("#FF0000"),
        decode_hex_or_fail("#00FF00"),
        decode_hex_or_fail("#0000FF"),
        decode_hex_or_fail("#FFFF00"),
        decode_hex_or_fail("#FF00FF"),
        decode_hex_or_fail("#00FFFF"),
        decode_hex_or_fail("#111111"),
        decode_hex_or_fail("#222222"),
        decode_hex_or_fail("#333333"),
        decode_hex_or_fail("#444444"),
        decode_hex_or_fail("#555555"),
        decode_hex_or_fail("#666666"),
    };
    REQUIRE(physicals.size() == 16);
    const std::vector<ColorRGB> out = preview_filament_colors(physicals, "1,2,1,1,1");
    REQUIRE(out.size() == 16);
    for (size_t i = 0; i < 16; ++i)
        REQUIRE(out[i] == physicals[i]);
}

TEST_CASE("Preview colors empty physicals is empty", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> out = preview_filament_colors({}, "1,2,1,1,1");
    REQUIRE(out.empty());
}

TEST_CASE("mix_recipe_label pair and triple with names or digits", "[MixedFilamentMatch]")
{
    const std::vector<std::string> names{"C", "M", "Y", "K"};
    MixedFilament                  pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    REQUIRE(mix_recipe_label(pair, &names) == "C+M 1:1");
    REQUIRE(mix_recipe_label(pair, nullptr) == "1+2 1:1");

    MixedFilament triple;
    triple.component_a = 1;
    triple.component_b = 2;
    triple.component_c = 3;
    triple.ratio_a     = 1;
    triple.ratio_b     = 1;
    triple.ratio_c     = 2;
    const std::string named = mix_recipe_label(triple, &names);
    REQUIRE(named.find("C+M+Y") != std::string::npos);
    REQUIRE(named.find("1:1:2") != std::string::npos);
}

TEST_CASE("mix_surface_label named and unnamed", "[MixedFilamentMatch]")
{
    const std::vector<std::string> names{"C", "M", "Y", "K"};
    MixedFilament                  pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    REQUIRE(mix_surface_label(5, pair, &names) == "Mix 5  C+M 1:1");
    REQUIRE(mix_surface_label(5, pair, nullptr) == "Mix 5  1+2 1:1");
}

namespace {

bool same_match_result(const MixMatchResult &a, const MixMatchResult &b)
{
    if (a.valid != b.valid || a.kind != b.kind || a.distance != b.distance)
        return false;
    if (a.kind == MixMatchResult::Kind::Physical)
        return a.physical_id == b.physical_id;
    return a.recipe_row == b.recipe_row && a.mix.component_a == b.mix.component_a &&
           a.mix.component_b == b.mix.component_b && a.mix.component_c == b.mix.component_c &&
           a.mix.ratio_a == b.mix.ratio_a && a.mix.ratio_b == b.mix.ratio_b &&
           a.mix.ratio_c == b.mix.ratio_c;
}

int mix_min_share_percent(const MixedFilament &mf)
{
    int period = mf.ratio_a + mf.ratio_b;
    int mn     = std::min(mf.ratio_a, mf.ratio_b);
    if (mf.component_c != 0) {
        period += mf.ratio_c;
        mn = std::min(mn, mf.ratio_c);
    }
    return period <= 0 ? 0 : (mn * 100) / period;
}

} // namespace

TEST_CASE("match_printable_candidates #CE921A ranked list and equals mix front", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#CE921A");
    const auto                  cands  = match_printable_candidates(target, phys);
    REQUIRE_FALSE(cands.empty());
    REQUIRE(cands.size() <= 12);

    const MixMatchResult best = match_printable_mix(target, phys);
    REQUIRE(same_match_result(cands[0], best));

    bool found_mix = false;
    for (const MixMatchResult &c : cands) {
        if (c.kind != MixMatchResult::Kind::Mix)
            continue;
        found_mix = true;
        std::set<unsigned> ids = {c.mix.component_a, c.mix.component_b};
        if (c.mix.component_c != 0)
            ids.insert(c.mix.component_c);
        const int used = int(ids.count(1u) + ids.count(2u) + ids.count(3u));
        REQUIRE(used >= 2);
        break;
    }
    REQUIRE(found_mix);

    for (size_t i = 1; i < cands.size(); ++i) {
        REQUIRE(cands[i - 1].distance <= cands[i].distance);
        if (cands[i - 1].distance == cands[i].distance) {
            const int p0 = (cands[i - 1].kind == MixMatchResult::Kind::Physical)
                               ? 1
                               : mix_period(cands[i - 1].mix);
            const int p1 = (cands[i].kind == MixMatchResult::Kind::Physical) ? 1 : mix_period(cands[i].mix);
            REQUIRE(p0 <= p1);
        }
    }

    const auto at25 = match_printable_candidates(target, phys, nullptr, 4, 25, 12);
    REQUIRE_FALSE(at25.empty());
    REQUIRE(same_match_result(at25[0], match_printable_mix(target, phys)));
}

TEST_CASE("match_printable_candidates integer min-share filter", "[MixedFilamentMatch]")
{
    MixedFilament r21;
    r21.ratio_a = 2;
    r21.ratio_b = 1;
    REQUIRE(mix_min_share_percent(r21) == 33);
    MixedFilament r31;
    r31.ratio_a = 3;
    r31.ratio_b = 1;
    REQUIRE(mix_min_share_percent(r31) == 25);

    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#CE921A");
    const auto                  cands  = match_printable_candidates(target, phys, nullptr, 4, 33, 64);
    REQUIRE_FALSE(cands.empty());

    bool saw_11 = false;
    bool saw_21 = false;
    for (const MixMatchResult &c : cands) {
        if (c.kind != MixMatchResult::Kind::Mix)
            continue;
        REQUIRE(mix_min_share_percent(c.mix) >= 33);
        const bool is_11 = c.mix.component_c == 0 && c.mix.ratio_a == 1 && c.mix.ratio_b == 1;
        const bool is_21 = c.mix.component_c == 0 &&
                           ((c.mix.ratio_a == 2 && c.mix.ratio_b == 1) || (c.mix.ratio_a == 1 && c.mix.ratio_b == 2));
        const bool is_31 = c.mix.component_c == 0 &&
                           ((c.mix.ratio_a == 3 && c.mix.ratio_b == 1) || (c.mix.ratio_a == 1 && c.mix.ratio_b == 3));
        const bool is_211 = c.mix.component_c != 0 && mix_period(c.mix) == 4 &&
                            mix_min_share_percent(c.mix) == 25;
        REQUIRE_FALSE(is_31);
        REQUIRE_FALSE(is_211);
        saw_11 = saw_11 || is_11;
        saw_21 = saw_21 || is_21;
    }
    REQUIRE(saw_11);
    REQUIRE(saw_21);
}

TEST_CASE("match_printable_candidates black front equals mix Grey physical", "[MixedFilamentMatch]")
{
    const std::vector<ColorRGB> phys   = panchroma_physicals();
    const ColorRGB              target = decode_hex_or_fail("#000000");
    const auto                  cands  = match_printable_candidates(target, phys);
    REQUIRE_FALSE(cands.empty());
    const MixMatchResult best = match_printable_mix(target, phys);
    REQUIRE(same_match_result(cands[0], best));
    REQUIRE(cands[0].kind == MixMatchResult::Kind::Physical);
    REQUIRE(cands[0].physical_id == 4u);
}

namespace {

int cookbook_period(const MixedFilament &mf)
{
    int p = mf.ratio_a + mf.ratio_b;
    if (mf.component_c != 0)
        p += mf.ratio_c;
    return std::max(1, p);
}

std::string serialize_mix_vec(const std::vector<MixedFilament> &rows)
{
    std::string out;
    for (const MixedFilament &mf : rows) {
        std::ostringstream oss;
        oss << mf.component_a << ',' << mf.component_b << ',' << (mf.enabled ? 1 : 0) << ',' << mf.ratio_a
            << ',' << mf.ratio_b;
        if (mf.component_c != 0)
            oss << ",c" << mf.component_c << ",rc" << mf.ratio_c;
        MixedFilamentManager one;
        one.load_definitions(oss.str());
        if (!out.empty())
            out += ';';
        out += one.serialize_definitions();
    }
    return out;
}

} // namespace

TEST_CASE("spectrum_cookbook recipes n=4 house table", "[spectrum_cookbook]")
{
    const auto recipes = spectrum_cookbook_recipes(4);
    REQUIRE(recipes.size() == 11);

    std::vector<MixedFilament> first6(recipes.begin(), recipes.begin() + 6);
    for (const MixedFilament &mf : first6) {
        REQUIRE(mf.component_c == 0u);
        REQUIRE(mf.ratio_c == 0);
        REQUIRE(cookbook_period(mf) <= 4);
    }
    REQUIRE(serialize_mix_vec(first6) ==
            "1,2,1,1,1;1,3,1,1,1;2,3,1,1,1;1,4,1,1,1;2,4,1,1,1;3,4,1,1,1");

    REQUIRE(recipes[6].component_a == 1u);
    REQUIRE(recipes[6].component_b == 3u);
    REQUIRE(recipes[6].ratio_a == 1);
    REQUIRE(recipes[6].ratio_b == 2);
    REQUIRE(recipes[7].component_a == 2u);
    REQUIRE(recipes[7].component_b == 3u);
    REQUIRE(recipes[7].ratio_a == 1);
    REQUIRE(recipes[7].ratio_b == 2);
    REQUIRE(recipes[8].component_a == 1u);
    REQUIRE(recipes[8].component_b == 2u);
    REQUIRE(recipes[8].ratio_a == 1);
    REQUIRE(recipes[8].ratio_b == 2);

    REQUIRE(recipes[9].component_c == 3u);
    REQUIRE(recipes[9].ratio_c == 1);
    REQUIRE(recipes[10].component_c == 3u);
    REQUIRE(recipes[10].ratio_c == 2);

    for (const MixedFilament &mf : recipes)
        REQUIRE(cookbook_period(mf) <= 4);

    REQUIRE(serialize_mix_vec({recipes[9]}).find("c3,rc1") != std::string::npos);
    REQUIRE(serialize_mix_vec({recipes[10]}).find("c3,rc2") != std::string::npos);
}

TEST_CASE("spectrum_cookbook recipes n=2/3/5 and empty", "[spectrum_cookbook]")
{
    REQUIRE(spectrum_cookbook_recipes(0).empty());
    REQUIRE(spectrum_cookbook_recipes(1).empty());

    const auto n2 = spectrum_cookbook_recipes(2);
    REQUIRE(n2.size() == 2);
    REQUIRE(n2[0].component_a == 1u);
    REQUIRE(n2[0].component_b == 2u);
    REQUIRE(n2[0].ratio_a == 1);
    REQUIRE(n2[0].ratio_b == 1);
    REQUIRE(n2[0].component_c == 0u);
    REQUIRE(n2[1].component_a == 1u);
    REQUIRE(n2[1].component_b == 2u);
    REQUIRE(n2[1].ratio_a == 1);
    REQUIRE(n2[1].ratio_b == 2);
    REQUIRE(n2[1].component_c == 0u);

    const auto n3 = spectrum_cookbook_recipes(3);
    REQUIRE(n3.size() == 8);
    std::vector<MixedFilament> first3(n3.begin(), n3.begin() + 3);
    REQUIRE(serialize_mix_vec(first3) == "1,2,1,1,1;1,3,1,1,1;2,3,1,1,1");
    for (const MixedFilament &mf : n3) {
        REQUIRE(mf.component_a != 4u);
        REQUIRE(mf.component_b != 4u);
        REQUIRE(mf.component_c != 4u);
        REQUIRE(cookbook_period(mf) <= 4);
    }

    const auto n4 = spectrum_cookbook_recipes(4);
    const auto n5 = spectrum_cookbook_recipes(5);
    REQUIRE(n5.size() == n4.size());
    for (size_t i = 0; i < n4.size(); ++i) {
        REQUIRE(spectrum_cookbook_same_recipe(n4[i], n5[i]));
        REQUIRE(n4[i].component_a == n5[i].component_a);
        REQUIRE(n4[i].component_b == n5[i].component_b);
        REQUIRE(n4[i].component_c == n5[i].component_c);
        REQUIRE(n4[i].ratio_a == n5[i].ratio_a);
        REQUIRE(n4[i].ratio_b == n5[i].ratio_b);
        REQUIRE(n4[i].ratio_c == n5[i].ratio_c);
    }
}

TEST_CASE("spectrum_cookbook_same_recipe gcd and leftover ratio_c", "[spectrum_cookbook]")
{
    MixedFilament clean;
    clean.component_a = 1;
    clean.component_b = 2;
    clean.ratio_a     = 1;
    clean.ratio_b     = 1;

    MixedFilament twotwo;
    twotwo.component_a = 1;
    twotwo.component_b = 2;
    twotwo.ratio_a     = 2;
    twotwo.ratio_b     = 2;
    REQUIRE(spectrum_cookbook_same_recipe(clean, twotwo));

    MixedFilament one_two;
    one_two.component_a = 1;
    one_two.component_b = 2;
    one_two.ratio_a     = 1;
    one_two.ratio_b     = 2;
    REQUIRE_FALSE(spectrum_cookbook_same_recipe(clean, one_two));

    MixedFilament triple;
    triple.component_a = 1;
    triple.component_b = 2;
    triple.component_c = 3;
    triple.ratio_a     = 1;
    triple.ratio_b     = 1;
    triple.ratio_c     = 1;
    REQUIRE_FALSE(spectrum_cookbook_same_recipe(clean, triple));

    MixedFilament leftover = clean;
    leftover.ratio_c       = 1; // component_c still 0
    REQUIRE(spectrum_cookbook_same_recipe(clean, leftover));
    REQUIRE_FALSE(spectrum_cookbook_same_recipe(leftover, triple));
}

TEST_CASE("spectrum_cookbook_append dups and mix-row cap", "[spectrum_cookbook]")
{
    {
        const auto r = spectrum_cookbook_append({}, 4);
        REQUIRE(r.added.size() == 11);
        REQUIRE(r.skipped_duplicate == 0);
        REQUIRE(r.skipped_cap == 0);
    }

    {
        MixedFilamentManager six;
        six.load_definitions("1,2,1,1,1;1,3,1,1,1;2,3,1,1,1;1,4,1,1,1;2,4,1,1,1;3,4,1,1,1");
        const auto r = spectrum_cookbook_append(six.mixed_filaments(), 4);
        REQUIRE(r.added.size() == 5);
        REQUIRE(r.skipped_duplicate == 6);
        REQUIRE(r.skipped_cap == 0);
    }

    {
        const auto all = spectrum_cookbook_recipes(4);
        const auto r   = spectrum_cookbook_append(all, 4);
        REQUIRE(r.added.empty());
        REQUIRE(r.skipped_duplicate >= 11);
        REQUIRE(r.skipped_cap == 0);
    }

    {
        // 10 enabled non-cookbook rows (3:1 is not in the house table).
        std::vector<MixedFilament> ten(10);
        for (MixedFilament &mf : ten) {
            mf.component_a = 1;
            mf.component_b = 2;
            mf.ratio_a     = 3;
            mf.ratio_b     = 1;
            mf.enabled     = true;
        }
        // After enabled-only semantics, 10 existing + cap 12 → room for 2.
        const auto r = spectrum_cookbook_append(ten, 4, 12);
        REQUIRE(r.added.size() == 2);
        REQUIRE(r.skipped_cap >= 1);
    }
}

TEST_CASE("spectrum mix rows unglue from paint persist 16", "[spectrum_mix_rows]")
{
    REQUIRE(SPECTRUM_MIX_ENABLED_CAP == 64);
    REQUIRE(SPECTRUM_PAINT_ID_PERSIST_CAP == 16);
    REQUIRE(spectrum_mix_enabled_fits(34));
    REQUIRE(spectrum_mix_enabled_fits(64, 0));
    REQUIRE_FALSE(spectrum_mix_enabled_fits(64, 1));
    REQUIRE_FALSE(spectrum_mix_enabled_fits(65, 0));
    REQUIRE(spectrum_mix_enabled_fits(10, 1, 12));
    REQUIRE_FALSE(spectrum_mix_enabled_fits(12, 1, 12));

    static const unsigned pair_a[] = {1, 1, 1, 2, 2, 3};
    static const unsigned pair_b[] = {2, 3, 4, 3, 4, 4};
    static const int      pair_ratios[][2] = {{1, 1}, {2, 1}, {1, 2}, {3, 1}, {1, 3}};

    std::vector<MixedFilament> rows;
    rows.reserve(34);
    for (size_t p = 0; p < 6; ++p) {
        for (size_t r = 0; r < 5; ++r) {
            MixedFilament mf;
            mf.component_a = pair_a[p];
            mf.component_b = pair_b[p];
            mf.component_c = 0;
            mf.ratio_a     = pair_ratios[r][0];
            mf.ratio_b     = pair_ratios[r][1];
            mf.ratio_c     = 0;
            mf.enabled     = true;
            rows.push_back(mf);
        }
    }
    const struct Triple {
        unsigned a, b, c;
        int      ra, rb, rc;
    } triples[] = {
        {1, 2, 3, 1, 1, 1},
        {1, 2, 3, 1, 1, 2},
        {1, 2, 4, 1, 1, 1},
        {1, 3, 4, 1, 1, 1},
    };
    for (const Triple &t : triples) {
        MixedFilament mf;
        mf.component_a = t.a;
        mf.component_b = t.b;
        mf.component_c = t.c;
        mf.ratio_a     = t.ra;
        mf.ratio_b     = t.rb;
        mf.ratio_c     = t.rc;
        mf.enabled     = true;
        REQUIRE(mf.component_c != mf.component_a);
        REQUIRE(mf.component_c != mf.component_b);
        REQUIRE(mf.ratio_a + mf.ratio_b + mf.ratio_c <= 4);
        rows.push_back(mf);
    }
    REQUIRE(rows.size() == 34);

    for (size_t i = 0; i < rows.size(); ++i) {
        for (size_t j = i + 1; j < rows.size(); ++j) {
            REQUIRE_FALSE(spectrum_cookbook_same_recipe(rows[i], rows[j]));
        }
    }

    std::ostringstream oss;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i)
            oss << ';';
        const MixedFilament &mf = rows[i];
        oss << mf.component_a << ',' << mf.component_b << ",1," << mf.ratio_a << ',' << mf.ratio_b;
        if (mf.component_c != 0)
            oss << ",c" << mf.component_c << ",rc" << mf.ratio_c;
    }
    const std::string ser = oss.str();
    MixedFilamentManager mgr;
    mgr.load_definitions(ser);
    const std::string round = mgr.serialize_definitions();
    MixedFilamentManager loaded;
    loaded.load_definitions(round);
    REQUIRE(MixedFilamentManager::max_filament_id(round, 4) == 38);
    REQUIRE(loaded.enabled_count() == 34);

    const MixedFilament *mix20 = loaded.mixed_filament_from_id(20, 4);
    const MixedFilament *mix38 = loaded.mixed_filament_from_id(38, 4);
    REQUIRE(mix20 != nullptr);
    REQUIRE(mix38 != nullptr);
    REQUIRE_FALSE(spectrum_cookbook_same_recipe(*mix20, *mix38));
    const bool mix20_layers_differ = loaded.resolve(20, 4, 0) != loaded.resolve(20, 4, 1);
    const bool mix38_layers_differ = loaded.resolve(38, 4, 0) != loaded.resolve(38, 4, 1);
    REQUIRE((mix20_layers_differ || mix38_layers_differ));

    REQUIRE(spectrum_volume_extruder_keep(38, 4, 38));
    REQUIRE(spectrum_paint_id_limit(4, 38, 0) == 16);

    // SHOULD: pattern 1234 cycles physicals 1–4 (token 4 is not covered by existing [MixedFilament] cases).
    MixedFilamentManager pat;
    std::string          pad;
    for (int i = 0; i < 33; ++i) {
        if (i)
            pad += ';';
        pad += "1,2,1,1,1";
    }
    pad += ";1,2,1,1,1,1234";
    pat.load_definitions(pad);
    REQUIRE(pat.resolve(38, 4, 0) == 1);
    REQUIRE(pat.resolve(38, 4, 1) == 2);
    REQUIRE(pat.resolve(38, 4, 2) == 3);
    REQUIRE(pat.resolve(38, 4, 3) == 4);
}

TEST_CASE("spectrum gradient parse/serialize ,g and g1; g0/g2 not pattern", "[spectrum_gradient]")
{
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,g");
        REQUIRE(mgr.enabled_count() == 1);
        REQUIRE(mgr.mixed_filaments().front().gradient_enabled);
        const std::string out = mgr.serialize_definitions();
        REQUIRE(out.find(",g") != std::string::npos);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1");
        REQUIRE_FALSE(mgr.mixed_filaments().front().gradient_enabled);
        REQUIRE(mgr.serialize_definitions().find(",g") == std::string::npos);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,G1");
        REQUIRE(mgr.mixed_filaments().front().gradient_enabled);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,g0");
        REQUIRE_FALSE(mgr.mixed_filaments().front().gradient_enabled);
        REQUIRE(mgr.mixed_filaments().front().manual_pattern.empty());
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,g2");
        REQUIRE_FALSE(mgr.mixed_filaments().front().gradient_enabled);
        REQUIRE(mgr.mixed_filaments().front().manual_pattern.empty());
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,xa0.1,g");
        REQUIRE(mgr.mixed_filaments().front().gradient_enabled);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,c3,rc1,g");
        const MixedFilament &mf = mgr.mixed_filaments().front();
        REQUIRE(mf.gradient_enabled);
        REQUIRE(mf.component_c == 3u);
        REQUIRE_FALSE(spectrum_mix_is_height_gradient(mf, true, true));
    }
}

TEST_CASE("spectrum_mix_is_height_gradient three-branch table", "[spectrum_gradient]")
{
    MixedFilament pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;

    MixedFilament g_pair = pair;
    g_pair.gradient_enabled = true;
    REQUIRE(spectrum_mix_is_height_gradient(g_pair, false, false));
    REQUIRE(spectrum_mix_is_height_gradient(g_pair, true, true));

    REQUIRE(spectrum_mix_is_height_gradient(pair, true, false));
    REQUIRE_FALSE(spectrum_mix_is_height_gradient(pair, true, true));
    REQUIRE_FALSE(spectrum_mix_is_height_gradient(pair, false, false));

    MixedFilament patterned = g_pair;
    patterned.manual_pattern = "112";
    REQUIRE_FALSE(spectrum_mix_is_height_gradient(patterned, true, true));

    MixedFilament triple = g_pair;
    triple.component_c = 3;
    REQUIRE_FALSE(spectrum_mix_is_height_gradient(triple, true, true));
}

TEST_CASE("spectrum_mix_layer_ratio gate DoD-2", "[spectrum_gradient]")
{
    MixedFilament g_row;
    g_row.component_a       = 1;
    g_row.component_b       = 2;
    g_row.ratio_a           = 2;
    g_row.ratio_b           = 1;
    g_row.gradient_enabled  = true;
    const auto mid = spectrum_mix_layer_ratio(g_row, 0.5, false, false);
    REQUIRE(mid.first == 50);
    REQUIRE(mid.second == 50);

    MixedFilament sibling;
    sibling.component_a = 1;
    sibling.component_b = 2;
    sibling.ratio_a     = 2;
    sibling.ratio_b     = 1;
    const auto stay = spectrum_mix_layer_ratio(sibling, 0.5, true, true);
    REQUIRE(stay.first == 2);
    REQUIRE(stay.second == 1);

    const auto legacy = spectrum_mix_layer_ratio(sibling, 0.5, true, false);
    REQUIRE(legacy.first == 50);
    REQUIRE(legacy.second == 50);
}

TEST_CASE("mix_recipe_label height gradient A->B", "[spectrum_gradient]")
{
    const std::vector<std::string> names{"C", "M", "Y", "K"};
    MixedFilament                  g;
    g.component_a       = 1;
    g.component_b       = 3;
    g.ratio_a           = 1;
    g.ratio_b           = 1;
    g.gradient_enabled  = true;
    REQUIRE(mix_recipe_label(g, &names) == "C->Y");

    MixedFilament g12 = g;
    g12.component_b   = 2;
    REQUIRE(mix_recipe_label(g12, nullptr) == "1->2");

    MixedFilament pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    REQUIRE(mix_recipe_label(pair, &names) == "C+M 1:1");
}

TEST_CASE("spectrum cards match in order; shuffle and wrong card fail", "[spectrum_rgb]")
{
    const SpectrumPhysicalCard cmyk = spectrum_panchroma_cmyk_card();
    const SpectrumPhysicalCard rgbw = spectrum_panchroma_rgbw_card();
    REQUIRE(spectrum_physicals_match_card(panchroma_physicals(), cmyk));
    REQUIRE_FALSE(spectrum_physicals_match_card(rgbw_physicals(), cmyk));
    REQUIRE(spectrum_physicals_match_card(rgbw_physicals(), rgbw));

    std::vector<ColorRGB> shuffled = {
        decode_hex_or_fail("#E72F1D"),
        decode_hex_or_fail("#003776"),
        decode_hex_or_fail("#06924D"),
        decode_hex_or_fail("#EBF7FF"),
    };
    REQUIRE_FALSE(spectrum_physicals_match_card(shuffled, rgbw));
    const std::vector<float> shuffled_td = spectrum_default_td_scale(shuffled);
    REQUIRE(shuffled_td.size() == 4);
    for (float s : shuffled_td)
        REQUIRE(s == Catch::Approx(1.f));
}

TEST_CASE("spectrum_default_td_scale CMYK RGBW random and explicit", "[spectrum_rgb]")
{
    const std::vector<float> cmyk_td = spectrum_default_td_scale(panchroma_physicals());
    REQUIRE(cmyk_td.size() == 4);
    REQUIRE(cmyk_td[2] == Catch::Approx(1.f / 14.f));

    const std::vector<float> rgbw_td = spectrum_default_td_scale(rgbw_physicals());
    REQUIRE(rgbw_td.size() == 4);
    REQUIRE(rgbw_td[2] == Catch::Approx(1.f / 0.3f));
    REQUIRE(rgbw_td[2] != Catch::Approx(1.f / 14.f));

    const std::vector<ColorRGB> random4 = {
        decode_hex_or_fail("#111111"),
        decode_hex_or_fail("#111111"),
        decode_hex_or_fail("#111111"),
        decode_hex_or_fail("#111111"),
    };
    const std::vector<float> ones = spectrum_default_td_scale(random4);
    REQUIRE(ones.size() == 4);
    for (float s : ones)
        REQUIRE(s == Catch::Approx(1.f));

    std::vector<ColorRGB> three = panchroma_physicals();
    three.resize(3);
    const std::vector<float> three_td = spectrum_default_td_scale(three);
    REQUIRE(three_td.size() == 3);
    for (float s : three_td)
        REQUIRE(s == Catch::Approx(1.f));

    const std::vector<float> explicit_td = {2.f, 4.f, 5.f, 10.f};
    const std::vector<float> wins = spectrum_default_td_scale(rgbw_physicals(), &explicit_td);
    REQUIRE(wins.size() == 4);
    REQUIRE(wins[0] == Catch::Approx(0.5f));
    REQUIRE(wins[1] == Catch::Approx(0.25f));
    REQUIRE(wins[2] == Catch::Approx(0.2f));
    REQUIRE(wins[3] == Catch::Approx(0.1f));
}

TEST_CASE("Match RGBW physicals and dark-neutral Blue; CMYK black Grey", "[spectrum_rgb]")
{
    const std::vector<ColorRGB> rgbw = rgbw_physicals();
    const MixMatchResult        red  = match_printable_mix(rgbw[0], rgbw);
    REQUIRE(red.valid);
    REQUIRE(red.kind == MixMatchResult::Kind::Physical);
    REQUIRE(red.physical_id == 1u);

    const MixMatchResult white = match_printable_mix(rgbw[3], rgbw);
    REQUIRE(white.valid);
    REQUIRE(white.kind == MixMatchResult::Kind::Physical);
    REQUIRE(white.physical_id == 4u);

    const MixMatchResult black_rgbw = match_printable_mix(decode_hex_or_fail("#000000"), rgbw);
    REQUIRE(black_rgbw.valid);
    REQUIRE(black_rgbw.kind == MixMatchResult::Kind::Physical);
    REQUIRE(black_rgbw.physical_id == 3u);
    REQUIRE(black_rgbw.recipe_row.empty());

    const MixMatchResult black_cmyk =
        match_printable_mix(decode_hex_or_fail("#000000"), panchroma_physicals());
    REQUIRE(black_cmyk.valid);
    REQUIRE(black_cmyk.kind == MixMatchResult::Kind::Physical);
    REQUIRE(black_cmyk.physical_id == 4u);
}

TEST_CASE("Match RGBW ColorMix midpoint 1+2 and RGBW recipe labels", "[spectrum_rgb]")
{
    const std::vector<ColorRGB> rgbw = rgbw_physicals();
    MixedFilament               pair;
    pair.component_a = 1;
    pair.component_b = 2;
    pair.ratio_a     = 1;
    pair.ratio_b     = 1;
    pair.enabled     = true;
    const ColorRGB       yn = predicted_swatch_for_mix(pair, rgbw);
    const MixMatchResult r  = match_printable_mix(yn, rgbw);
    REQUIRE(r.valid);
    REQUIRE(r.kind == MixMatchResult::Kind::Mix);
    REQUIRE(r.mix.component_a == 1u);
    REQUIRE(r.mix.component_b == 2u);
    REQUIRE(r.mix.component_c == 0u);
    REQUIRE(r.mix.ratio_a == 1);
    REQUIRE(r.mix.ratio_b == 1);

    MixedFilamentManager mgr;
    mgr.load_definitions(r.recipe_row);
    REQUIRE(mgr.enabled_count() == 1);

    const std::vector<std::string> names{"R", "G", "B", "W"};
    REQUIRE(mix_recipe_label(pair, &names) == "R+G 1:1");
    REQUIRE(mix_recipe_label(pair, nullptr) == "1+2 1:1");
}

TEST_CASE("spectrum_stamp_slot_hexes n<4 / RGBW / append FF keeps extras", "[spectrum_rgb]")
{
    const std::array<std::string, 4> hexes = spectrum_rgbw_hexes();
    REQUIRE(hexes[0] == "#E72F1D");
    REQUIRE(hexes[1] == "#06924D");
    REQUIRE(hexes[2] == "#003776");
    REQUIRE(hexes[3] == "#EBF7FF");

    std::vector<std::string> three{"#111111", "#222222", "#333333"};
    const auto               three_copy = three;
    REQUIRE_FALSE(spectrum_stamp_slot_hexes(three, hexes));
    REQUIRE(three == three_copy);

    std::vector<std::string> four{"#AAAAAA", "#BBBBBB", "#CCCCCC", "#DDDDDD"};
    REQUIRE(spectrum_stamp_slot_hexes(four, hexes));
    REQUIRE(four.size() == 4);
    REQUIRE(four[0] == "#E72F1D");
    REQUIRE(four[1] == "#06924D");
    REQUIRE(four[2] == "#003776");
    REQUIRE(four[3] == "#EBF7FF");

    std::vector<std::string> six{"#AAAAAAFF", "#BBBBBB", "#CCCCCC", "#DDDDDD", "#EEEEEE", "#FFFFFF"};
    REQUIRE(spectrum_stamp_slot_hexes(six, hexes));
    REQUIRE(six.size() == 6);
    REQUIRE(six[0] == "#E72F1DFF");
    REQUIRE(six[1] == "#06924DFF");
    REQUIRE(six[2] == "#003776FF");
    REQUIRE(six[3] == "#EBF7FFFF");
    REQUIRE(six[4] == "#EEEEEE");
    REQUIRE(six[5] == "#FFFFFF");
}

TEST_CASE("spectrum_stamp_multi_heads n<4 / grow / keep extras", "[spectrum_rgb]")
{
    {
        std::vector<std::string> multi{"#KEEP0", "#KEEP1"};
        const auto               multi_copy = multi;
        const std::vector<std::string> colour{"#111111", "#222222", "#333333"};
        REQUIRE_FALSE(spectrum_stamp_multi_heads(multi, colour));
        REQUIRE(multi == multi_copy);
    }
    {
        std::vector<std::string>       multi;
        const std::vector<std::string> colour{"#E72F1D", "#06924D", "#003776", "#EBF7FF"};
        REQUIRE(spectrum_stamp_multi_heads(multi, colour));
        REQUIRE(multi.size() == 4);
        REQUIRE(multi[0] == colour[0]);
        REQUIRE(multi[1] == colour[1]);
        REQUIRE(multi[2] == colour[2]);
        REQUIRE(multi[3] == colour[3]);
    }
    {
        std::vector<std::string> multi{"#OLD0", "#OLD1", "#OLD2", "#OLD3", "#KEEP4", "#AA #BB"};
        const std::vector<std::string> colour{
            "#E72F1DFF", "#06924DFF", "#003776FF", "#EBF7FFFF", "#EXTRA4", "#EXTRA5"};
        REQUIRE(spectrum_stamp_multi_heads(multi, colour));
        REQUIRE(multi.size() == 6);
        REQUIRE(multi[0] == "#E72F1DFF");
        REQUIRE(multi[1] == "#06924DFF");
        REQUIRE(multi[2] == "#003776FF");
        REQUIRE(multi[3] == "#EBF7FFFF");
        REQUIRE(multi[4] == "#KEEP4");
        REQUIRE(multi[5] == "#AA #BB");
    }
}

TEST_CASE("spectrum perimeter mod parse/serialize ,p and helpers", "[spectrum_perimeter_mod]")
{
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,p");
        REQUIRE(mgr.enabled_count() == 1);
        REQUIRE(mgr.mixed_filaments().front().perimeter_modulation);
        const std::string out = mgr.serialize_definitions();
        REQUIRE(out.find(",p") != std::string::npos);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1");
        REQUIRE_FALSE(mgr.mixed_filaments().front().perimeter_modulation);
        REQUIRE(mgr.serialize_definitions().find('p') == std::string::npos);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,P1");
        REQUIRE(mgr.mixed_filaments().front().perimeter_modulation);
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,p0");
        REQUIRE_FALSE(mgr.mixed_filaments().front().perimeter_modulation);
        REQUIRE(mgr.mixed_filaments().front().manual_pattern.empty());
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,p2");
        REQUIRE_FALSE(mgr.mixed_filaments().front().perimeter_modulation);
        REQUIRE(mgr.mixed_filaments().front().manual_pattern.empty());
    }
    {
        MixedFilamentManager mgr;
        mgr.load_definitions("1,2,1,1,1,xa0.2,g,p");
        const MixedFilament &mf = mgr.mixed_filaments().front();
        REQUIRE(mf.perimeter_modulation);
        REQUIRE(mf.gradient_enabled);
        REQUIRE(mf.component_a_surface_offset == Catch::Approx(0.2f));
    }
    {
        REQUIRE(spectrum_perimeter_mod_magnitude_mm(0.4f) == Catch::Approx(0.16f));
        REQUIRE(spectrum_perimeter_mod_magnitude_mm(1.0f) == Catch::Approx(0.35f));
        REQUIRE(spectrum_perimeter_mod_magnitude_mm(0.f) == Catch::Approx(0.16f));
        REQUIRE(spectrum_perimeter_mod_magnitude_mm(std::numeric_limits<float>::quiet_NaN()) ==
                Catch::Approx(0.16f));
    }
    {
        const std::vector<double> size1{0.6};
        REQUIRE(spectrum_nozzle_mm_for_physical(size1, 2) == Catch::Approx(0.6f));
        REQUIRE(spectrum_nozzle_mm_for_physical(size1, 1) == Catch::Approx(0.6f));
        REQUIRE(spectrum_nozzle_mm_for_physical(size1, 0) == Catch::Approx(0.6f));

        const std::vector<double> empty;
        REQUIRE(spectrum_nozzle_mm_for_physical(empty, 1) == Catch::Approx(0.4f));
        REQUIRE(spectrum_nozzle_mm_for_physical(empty, 2) == Catch::Approx(0.4f));

        const std::vector<double> ultra_s{0.4, 0.4, 0.4, 0.4};
        REQUIRE(spectrum_nozzle_mm_for_physical(ultra_s, 2) == Catch::Approx(0.4f));

        const std::vector<double> hetero{0.4, 0.6};
        REQUIRE(spectrum_nozzle_mm_for_physical(hetero, 2) == Catch::Approx(0.6f));
    }
    {
        MixedFilament mf;
        mf.component_a            = 1;
        mf.component_b            = 2;
        mf.perimeter_modulation   = true;
        REQUIRE(spectrum_perimeter_mod_offset(mf, 1, 0.4f) == Catch::Approx(-0.16f));
        REQUIRE(spectrum_perimeter_mod_offset(mf, 2, 0.4f) == Catch::Approx(0.16f));
        REQUIRE(spectrum_perimeter_mod_offset(mf, 3, 0.4f) == Catch::Approx(0.f));
    }
    {
        MixedFilament mf;
        mf.component_a            = 1;
        mf.component_b            = 2;
        mf.perimeter_modulation   = true;
        const float nozzle = spectrum_nozzle_mm_for_physical(std::vector<double>{0.6}, 2);
        REQUIRE(spectrum_perimeter_mod_offset(mf, 1, nozzle) == Catch::Approx(-0.24f));
        REQUIRE(spectrum_perimeter_mod_offset(mf, 2, nozzle) == Catch::Approx(0.24f));
    }
    {
        MixedFilament mf;
        mf.component_a                  = 1;
        mf.component_b                  = 2;
        mf.perimeter_modulation         = true;
        mf.component_a_surface_offset   = 0.2f;
        mf.component_b_surface_offset   = 0.f;
        REQUIRE(spectrum_perimeter_mod_offset(mf, 1, 0.4f) == Catch::Approx(0.2f));
        REQUIRE(spectrum_perimeter_mod_offset(mf, 2, 0.4f) == Catch::Approx(0.f));
    }
    {
        MixedFilament mf;
        mf.component_a          = 1;
        mf.component_b          = 2;
        mf.perimeter_modulation = false;
        REQUIRE(spectrum_perimeter_mod_offset(mf, 1, 0.4f) == Catch::Approx(0.f));
        REQUIRE(spectrum_perimeter_mod_offset(mf, 2, 0.4f) == Catch::Approx(0.f));
    }
}
