#include <catch2/catch_all.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {

using Catch::Matchers::WithinRel;

// Square of the given (scaled) side, lower-left at the origin. bed_idx starts at
// 0 because arrange() seeds the nester's bin from it (see ModelArrange.cpp).
ArrangePolygon make_square(coord_t side)
{
    ArrangePolygon ap;
    Polygon        p;
    p.points = {Point(0, 0), Point(side, 0), Point(side, side), Point(0, side)};
    ap.poly  = ExPolygon(p);
    ap.bed_idx = 0;
    return ap;
}

ArrangePolygons squares(int n, double side_mm)
{
    ArrangePolygons items;
    for (int i = 0; i < n; ++i)
        items.emplace_back(make_square(scaled(side_mm)));
    return items;
}

// Bed [0,0]..[w,h] in scaled coordinates.
BoundingBox bed(double w_mm, double h_mm)
{
    return BoundingBox(Point(0, 0), Point(scaled(w_mm), scaled(h_mm)));
}

// The default progress callback prints to stdout; silence it.
ArrangeParams quiet_params(coord_t min_dist = 0)
{
    ArrangeParams p{min_dist};
    p.progressind = [](unsigned, std::string) {};
    return p;
}

ExPolygons placed_shapes(const ArrangePolygons &items)
{
    ExPolygons out;
    out.reserve(items.size());
    for (const ArrangePolygon &ap : items)
        out.emplace_back(ap.transformed_poly());
    return out;
}

// Area double-counted across the shapes: the sum counts overlaps twice, the
// union once, so the difference is the overlapping area (0 when disjoint).
double overlap_area(const ExPolygons &shapes)
{
    double sum = 0;
    for (const ExPolygon &e : shapes)
        sum += e.area();
    double uni = 0;
    for (const ExPolygon &e : union_ex(shapes))
        uni += e.area();
    return sum - uni;
}

// Relative tolerance absorbs the area-unit rounding the clipper union introduces.
bool disjoint(const ExPolygons &shapes)
{
    double total = 0;
    for (const ExPolygon &e : shapes)
        total += e.area();
    return overlap_area(shapes) <= total * 1e-9;
}

void require_no_overlap(const ArrangePolygons &items)
{
    REQUIRE(disjoint(placed_shapes(items)));
}

} // namespace

// Prove the overlap check the other tests rely on actually detects overlap.
TEST_CASE("overlap_area detects overlap and ignores touching edges", "[Arrange]")
{
    auto square_at = [](double x_mm) {
        ArrangePolygon ap = make_square(scaled(20.));
        ap.translation    = Vec2crd(scaled(x_mm), 0);
        return ap.transformed_poly();
    };
    ExPolygon a = square_at(0.);

    SECTION("disjoint shapes are reported disjoint") {
        REQUIRE(disjoint({a, square_at(30.)}));
    }
    SECTION("edge-touching shapes are reported disjoint") {
        REQUIRE(disjoint({a, square_at(20.)}));
    }
    SECTION("overlapping shapes are not, and the area is measured") {
        REQUIRE_FALSE(disjoint({a, square_at(10.)}));
        REQUIRE_THAT(overlap_area({a, square_at(10.)}),
                     WithinRel(double(scaled(10.)) * scaled(20.), 1e-9)); // 10x20 mm
    }
}

TEST_CASE("Arrange places every item on the physical bed", "[Arrange]")
{
    ArrangePolygons items = squares(5, 20.);
    arrange(items, bed(200, 200), quiet_params(scaled(1.)));

    for (const ArrangePolygon &ap : items)
        REQUIRE(ap.bed_idx == 0);
}

TEST_CASE("Arranged items stay within the bed", "[Arrange]")
{
    ArrangePolygons items = squares(6, 30.);
    arrange(items, bed(200, 200), quiet_params(scaled(1.)));

    for (const ArrangePolygon &ap : items) {
        REQUIRE(ap.bed_idx == 0);
        REQUIRE(bed(200, 200).contains(ap.transformed_poly().contour.bounding_box()));
    }
}

TEST_CASE("Arranged items do not overlap", "[Arrange]")
{
    ArrangePolygons items = squares(6, 40.);
    arrange(items, bed(250, 250), quiet_params(scaled(2.)));

    require_no_overlap(items);
}

TEST_CASE("Arrange spaces items by their inflation", "[Arrange]")
{
    // Per-item inflation is how the arranger enforces clearance (the GUI fills it
    // from min_obj_distance). Two items inflated 4mm each end up >= 8mm apart.
    ArrangePolygons items = squares(4, 20.);
    for (ArrangePolygon &ap : items)
        ap.inflation = scaled(4.);
    arrange(items, bed(200, 200), quiet_params());

    // Axis-aligned squares are their own bounding boxes, so the clearance between
    // a pair is the distance between their boxes (1mm slack for nester rounding).
    std::vector<BoundingBox> boxes;
    for (const ExPolygon &e : placed_shapes(items))
        boxes.push_back(e.contour.bounding_box());

    double min_gap = std::numeric_limits<double>::max();
    for (size_t i = 0; i < boxes.size(); ++i)
        for (size_t j = i + 1; j < boxes.size(); ++j) {
            coord_t sx = std::max<coord_t>(0, std::max(boxes[j].min.x() - boxes[i].max.x(),
                                                       boxes[i].min.x() - boxes[j].max.x()));
            coord_t sy = std::max<coord_t>(0, std::max(boxes[j].min.y() - boxes[i].max.y(),
                                                       boxes[i].min.y() - boxes[j].max.y()));
            min_gap = std::min(min_gap, std::sqrt(double(sx) * sx + double(sy) * sy));
        }

    REQUIRE(min_gap >= double(scaled(8.)) - double(scaled(0.5)));
}

TEST_CASE("An item larger than the bed cannot be placed", "[Arrange]")
{
    ArrangePolygons items;
    items.emplace_back(make_square(scaled(20.)));
    items.emplace_back(make_square(scaled(400.))); // far bigger than the bed

    arrange(items, bed(200, 200), quiet_params(scaled(1.)));

    REQUIRE(items[0].bed_idx == 0);
    REQUIRE(items[1].bed_idx == UNARRANGED);
}

TEST_CASE("Items overflowing one bed spill onto virtual beds", "[Arrange]")
{
    ArrangePolygons items = squares(8, 90.); // eight 90mm squares cannot share a 200x200 bed
    arrange(items, bed(200, 200), quiet_params(scaled(2.)));

    int max_bed = 0;
    for (const ArrangePolygon &ap : items) {
        REQUIRE(ap.bed_idx >= 0); // placed somewhere
        max_bed = std::max(max_bed, ap.bed_idx);
    }
    REQUIRE(max_bed >= 1); // at least one on a virtual bed
}

TEST_CASE("Arrange handles an empty input", "[Arrange]")
{
    ArrangePolygons items;
    REQUIRE_NOTHROW(arrange(items, bed(200, 200), quiet_params()));
    REQUIRE(items.empty());
}

TEST_CASE("Arrange without final alignment keeps items disjoint", "[Arrange]")
{
    // do_final_align = false selects Alignment::DONT_ALIGN (skips recentering).
    ArrangePolygons items  = squares(6, 40.);
    ArrangeParams   params = quiet_params(scaled(2.));
    params.do_final_align  = false;

    arrange(items, bed(250, 250), params);

    for (const ArrangePolygon &ap : items)
        REQUIRE(ap.bed_idx == 0);
    require_no_overlap(items);
}

TEST_CASE("Arrange aligns the pile to a custom center", "[Arrange]")
{
    // align_center != (0.5, 0.5) selects Alignment::USER_DEFINED.
    ArrangePolygons items  = squares(5, 30.);
    ArrangeParams   params = quiet_params(scaled(2.));
    params.align_center    = Vec2d(0.3, 0.7);

    arrange(items, bed(250, 250), params);

    for (const ArrangePolygon &ap : items)
        REQUIRE(ap.bed_idx == 0);
    require_no_overlap(items);
}

TEST_CASE("Bed exclusions apply by physical nozzle and overlapping Z range", "[Arrange][ExclusionVolume][MultiNozzle]")
{
    ArrangePolygon object = make_square(scaled(20.));
    object.has_z_range = true;
    object.z_min = 0.0;
    object.z_max = 20.0;
    object.bed_exclusion_extruder_ids = {1};

    ArrangePolygon exclusion = make_square(scaled(20.));
    exclusion.is_bed_exclusion = true;
    exclusion.has_z_range = true;
    exclusion.z_min = 10.0;
    exclusion.z_max = 30.0;
    exclusion.bed_exclusion_extruder_id = 1;

    CHECK(bed_exclusion_applies(object, exclusion));

    exclusion.bed_exclusion_extruder_id = 0;
    CHECK_FALSE(bed_exclusion_applies(object, exclusion));

    // Empty physical-nozzle assignments mean unresolved, so arrangement stays conservative.
    object.bed_exclusion_extruder_ids.clear();
    CHECK(bed_exclusion_applies(object, exclusion));

    object.bed_exclusion_extruder_ids = {0};
    object.z_max = 9.0;
    CHECK_FALSE(bed_exclusion_applies(object, exclusion));

    // Touching at one Z plane is intentionally treated as a collision.
    object.z_max = 10.0;
    CHECK(bed_exclusion_applies(object, exclusion));
}

TEST_CASE("Shared bed exclusions apply to every arranged object", "[Arrange][ExclusionVolume]")
{
    ArrangePolygon object = make_square(scaled(20.));
    object.bed_exclusion_extruder_ids = {1};

    ArrangePolygon shared = make_square(scaled(20.));
    shared.is_bed_exclusion = true;
    shared.bed_exclusion_extruder_id = -1;

    CHECK(has_bed_exclusion_regions({shared}));
    CHECK_FALSE(has_bed_exclusion_regions({object}));
    CHECK(bed_exclusion_applies(object, shared));
}

TEST_CASE("Conservative exclusion validation invalidates only relevant placements", "[Arrange][ExclusionVolume][MultiNozzle]")
{
    ArrangePolygon exclusion = make_square(scaled(20.));
    exclusion.is_bed_exclusion = true;
    exclusion.bed_exclusion_extruder_id = 0;
    exclusion.has_z_range = true;
    exclusion.z_min = 0.0;
    exclusion.z_max = 30.0;

    ArrangePolygon relevant = make_square(scaled(10.));
    relevant.bed_exclusion_extruder_ids = {0};
    relevant.has_z_range = true;
    relevant.z_min = 0.0;
    relevant.z_max = 10.0;

    ArrangePolygon other_nozzle = relevant;
    other_nozzle.bed_exclusion_extruder_ids = {1};

    ArrangePolygon above = relevant;
    above.z_min = 31.0;
    above.z_max = 40.0;

    ArrangePolygons items{relevant, other_nozzle, above};
    invalidate_bed_exclusion_conflicts(items, {exclusion});

    CHECK(items[0].bed_idx == UNARRANGED);
    CHECK(items[1].bed_idx == 0);
    CHECK(items[2].bed_idx == 0);
}

TEST_CASE("Arrange keeps relevant objects outside conditional exclusion obstacles", "[Arrange][ExclusionVolume]")
{
    ArrangePolygon exclusion = make_square(scaled(30.));
    exclusion.translation = Vec2crd(scaled(35.0), scaled(35.0));
    exclusion.is_bed_exclusion = true;
    exclusion.bed_exclusion_extruder_id = 0;

    ArrangePolygons items = squares(4, 20.0);
    for (ArrangePolygon &item : items)
        item.bed_exclusion_extruder_ids = {0};

    arrange(items, {exclusion}, bed(100.0, 100.0), quiet_params(scaled(1.0)));

    const ExPolygon excluded_shape = exclusion.transformed_poly();
    for (const ArrangePolygon &item : items) {
        REQUIRE(item.bed_idx == 0);
        CHECK(intersection(ExPolygons{item.transformed_poly()}, ExPolygons{excluded_shape}).empty());
    }
    require_no_overlap(items);
}
