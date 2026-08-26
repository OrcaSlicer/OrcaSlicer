#include <catch2/catch_all.hpp>

#include "libslic3r/LocalZOrderOptimizer.hpp"
#include "libslic3r/LocalZPlanner.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Print.hpp"

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
