#include <catch2/catch_all.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/LocalZOrderOptimizer.hpp"
#include "libslic3r/LocalZPlanner.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentCookbook.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"
#include "libslic3r/Print.hpp"

#include <algorithm>
#include <cmath>
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

TEST_CASE("Preview colors append one YN mix after physicals", "[MixedFilamentMatch]")
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

TEST_CASE("spectrum_cookbook_append dups and persist cap", "[spectrum_cookbook]")
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
        // Cap: 4 physical + 10 enabled = 14; room for 1 → then skipped_cap.
        const auto r = spectrum_cookbook_append(ten, 4, SPECTRUM_PAINT_ID_PERSIST_CAP);
        REQUIRE(r.added.size() == 1);
        REQUIRE(r.skipped_cap >= 1);
    }
}
