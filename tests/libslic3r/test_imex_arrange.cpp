#include <catch2/catch_all.hpp>

#include <algorithm>
#include <optional>
#include <vector>

#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_floating_point.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/IMEXArrange.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A 300 x 200 mm bed. A two-carriage IDEX in a parallel mode gives T0 the left half, and a
// mirror mode adds a 30 mm strip along that half's inner edge.
const BoundingBoxf kBed{Vec2d(0., 0.), Vec2d(300., 200.)};
const BoundingBoxf kLeftHalf{Vec2d(0., 0.), Vec2d(150., 200.)};
const BoundingBoxf kRightHalf{Vec2d(150., 0.), Vec2d(300., 200.)};
const BoundingBoxf kStrip{Vec2d(120., 0.), Vec2d(150., 200.)};

const ImexArrangeZones kCopy{kLeftHalf, {}};
const ImexArrangeZones kMirror{kLeftHalf, {kStrip}};

// A 1 mm bed edge margin, and no rotation, so every expected box below is exact.
arrangement::ArrangeParams test_params()
{
    arrangement::ArrangeParams params;
    params.progressind     = {};
    params.bed_shrink_x    = 1.f;
    params.bed_shrink_y    = 1.f;
    params.allow_rotations = false;
    return params;
}

// The bed shrunk by that 1 mm margin, as get_shrink_bedpts() hands it to the arranger.
Points shrunk_bed()
{
    return scaled(BoundingBoxf(Vec2d(1., 1.), Vec2d(299., 199.))).polygon().points;
}

// A `width` x `depth` mm part, kept 1 mm from its neighbors, ready to arrange: on bed 0, as
// get_instance_arrange_poly() leaves it.
arrangement::ArrangePolygon part(double width, double depth)
{
    arrangement::ArrangePolygon ap;
    ap.poly.contour = scaled(BoundingBoxf(Vec2d(-width / 2., -depth / 2.), Vec2d(width / 2., depth / 2.))).polygon();
    ap.inflation    = scaled(1.);
    ap.extrude_ids  = {0};
    ap.height       = 10.;
    ap.bed_idx      = 0;
    return ap;
}

arrangement::ArrangePolygons squares(int count, double size)
{
    arrangement::ArrangePolygons out(count, part(size, size));
    for (int i = 0; i < count; ++i)
        out[i].itemid = i;
    return out;
}

// Arranges `items` the way ArrangeJob does.
void arrange(arrangement::ArrangePolygons& items, const ImexArrangeInput& input)
{
    const arrangement::ArrangeParams params = test_params();
    const ImexArranger               imex(input, params, shrunk_bed());
    arrangement::ArrangePolygons     fixed;
    imex.add_keep_outs(fixed);
    const arrangement::ArrangePolygons before = items;
    arrangement::arrange(items, fixed, imex.bed_shape(), params);
    imex.finish(items, before, fixed, params);
}

BoundingBoxf extents(const arrangement::ArrangePolygons& items)
{
    BoundingBox bb;
    for (const arrangement::ArrangePolygon& ap : items)
        bb.merge(get_extents(ap.transformed_poly()));
    return BoundingBoxf(unscaled(bb.min), unscaled(bb.max));
}

bool crosses(const arrangement::ArrangePolygon& ap, const BoundingBoxf& box)
{
    return !intersection(ap.transformed_poly().contour, scaled(box).polygon()).empty();
}

} // namespace

TEST_CASE("ImexArranger changes nothing when no plate has zones", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    input.beds        = {std::nullopt, std::nullopt};
    input.adds_plates = true;
    const ImexArranger imex(input, test_params(), shrunk_bed());

    CHECK_FALSE(imex.active());
    CHECK(imex.bed_shape() == shrunk_bed());
    arrangement::ArrangePolygons fixed;
    imex.add_keep_outs(fixed);
    CHECK(fixed.empty());
}

TEST_CASE("Plates sharing a primary zone arrange inside it, clear of the strip", "[IMEXArrange][IMEX]")
{
    // The second plate computes the same zone from where it sits, off in the last bits.
    ImexArrangeZones shifted = kMirror;
    shifted.primary_zone.max.x() += 1e-9;
    ImexArrangeInput input;
    input.beds        = {kMirror, shifted};
    input.adds_plates = true;
    input.added_plate = kMirror;

    // The bed shape is the zone less the bed's 1 mm edge margin.
    const ImexArranger imex(input, test_params(), shrunk_bed());
    const BoundingBox  shape(imex.bed_shape());
    CHECK(shape.min == scaled(Vec2d(1., 1.)));
    CHECK(shape.max == scaled(Vec2d(149., 199.)));

    // Enough parts that, centered in the zone, they would reach the strip.
    arrangement::ArrangePolygons items = squares(20, 20.);
    arrange(items, input);
    for (const arrangement::ArrangePolygon& ap : items) {
        CHECK(ap.bed_idx >= 0);
        CHECK_FALSE(crosses(ap, kStrip));
    }
}

TEST_CASE("Beside a plate in Primary, a parallel plate's parts are centered in its zone", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    input.beds        = {kMirror, std::nullopt};
    input.adds_plates = true;

    // Different zones: the bed shape stays the whole bed, and the mirror plate is fenced in.
    const ImexArranger imex(input, test_params(), shrunk_bed());
    CHECK(imex.bed_shape() == shrunk_bed());

    arrangement::ArrangePolygons items = squares(4, 20.);
    arrange(items, input);
    for (const arrangement::ArrangePolygon& ap : items)
        REQUIRE(ap.bed_idx == 0);
    const BoundingBoxf pile = extents(items);
    CHECK(kLeftHalf.contains(pile.min));
    CHECK(kLeftHalf.contains(pile.max));
    // Centered in the zone, rather than piled against its edge nearest the bed's center.
    CHECK_THAT(pile.center().x(), WithinAbs(75., 5.));
    CHECK_THAT(pile.center().y(), WithinAbs(100., 5.));
}

TEST_CASE("A plate in Primary uses the whole bed beside a parallel plate", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    input.beds        = {std::nullopt, kMirror};
    input.adds_plates = true;

    arrangement::ArrangePolygons items = squares(4, 20.);
    arrange(items, input);
    for (const arrangement::ArrangePolygon& ap : items)
        REQUIRE(ap.bed_idx == 0);
    CHECK_THAT(extents(items).center().x(), WithinAbs(150., 5.));
}

TEST_CASE("A part too big for its primary zone is left unarranged", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    SECTION("arranging one plate, the zone is the bed shape")
    {
        input.beds = {kCopy};
    }
    SECTION("arranging all plates, the zone is fenced in")
    {
        input.beds        = {kCopy, std::nullopt};
        input.adds_plates = true;
    }

    // Narrower than the bed but wider than the zone.
    arrangement::ArrangePolygons items{part(180., 50.)};
    arrange(items, input);
    CHECK(items.front().bed_idx == arrangement::UNARRANGED);
}

TEST_CASE("A strip keeps room for the bed margin and the brim", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    input.beds = {kMirror};
    input.brim = 5.;
    const ImexArranger imex(input, test_params(), shrunk_bed());

    arrangement::ArrangePolygons fixed;
    imex.add_keep_outs(fixed);
    REQUIRE(fixed.size() == 1);
    // Grown by the 1 mm margin and the 5 mm brim on every side.
    const BoundingBox strip = get_extents(fixed.front().poly);
    CHECK(strip.min == scaled(Vec2d(114., -6.)));
    CHECK(strip.max == scaled(Vec2d(156., 206.)));
}

TEST_CASE("A plate the arrange adds takes the zones it will have", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    input.beds        = {kCopy};
    input.adds_plates = true;

    SECTION("the same zone keeps it the bed shape, with the new plates' strips")
    {
        input.added_plate = kMirror;
        const ImexArranger imex(input, test_params(), shrunk_bed());
        CHECK(imex.bed_shape() != shrunk_bed());
        arrangement::ArrangePolygons fixed;
        imex.add_keep_outs(fixed);
        auto on = [&fixed](int bed_idx) {
            return std::count_if(fixed.begin(), fixed.end(), [bed_idx](const arrangement::ArrangePolygon& ap) { return ap.bed_idx == bed_idx; });
        };
        CHECK(on(0) == 0);
        CHECK(on(1) == 1);
    }
    SECTION("a new plate in Primary gets the whole bed, so the zone is fenced in instead")
    {
        const ImexArranger imex(input, test_params(), shrunk_bed());
        CHECK(imex.bed_shape() == shrunk_bed());
        arrangement::ArrangePolygons fixed;
        imex.add_keep_outs(fixed);
        for (const arrangement::ArrangePolygon& ap : fixed)
            CHECK(ap.bed_idx == 0);
    }
    SECTION("another zone makes the whole bed the bed shape")
    {
        input.added_plate = ImexArrangeZones{kRightHalf, {}};
        const ImexArranger imex(input, test_params(), shrunk_bed());
        CHECK(imex.bed_shape() == shrunk_bed());
    }
    SECTION("arranging one plate never adds one")
    {
        input.adds_plates = false;
        input.added_plate = ImexArrangeZones{kRightHalf, {}};
        const ImexArranger imex(input, test_params(), shrunk_bed());
        CHECK(imex.bed_shape() != shrunk_bed());
    }
}

TEST_CASE("A zone narrower than its margins takes no parts", "[IMEXArrange][IMEX]")
{
    ImexArrangeInput input;
    input.beds = {ImexArrangeZones{BoundingBoxf(Vec2d(0., 0.), Vec2d(1.5, 200.)), {}}};
    const ImexArranger imex(input, test_params(), shrunk_bed());
    CHECK(imex.bed_shape() == shrunk_bed());

    arrangement::ArrangePolygons items = squares(1, 20.);
    arrange(items, input);
    CHECK(items.front().bed_idx == arrangement::UNARRANGED);
}


TEST_CASE("Parts that spill onto a new plate in Primary use its whole bed", "[IMEXArrange][IMEX]")
{
    // Every plate in copy mode, while a plate the arrange adds is in Primary.
    ImexArrangeInput input;
    input.beds        = {kCopy};
    input.adds_plates = true;

    // The zone holds 3 x 4 of these 40 mm parts, so the rest spill onto a new plate.
    arrangement::ArrangePolygons items = squares(16, 40.);
    arrange(items, input);

    arrangement::ArrangePolygons spilled;
    for (const arrangement::ArrangePolygon& ap : items) {
        REQUIRE(ap.bed_idx >= 0);
        if (ap.bed_idx == 0) {
            const BoundingBoxf placed = extents({ap});
            CHECK(kLeftHalf.contains(placed.min));
            CHECK(kLeftHalf.contains(placed.max));
        } else
            spilled.push_back(ap);
    }
    REQUIRE_FALSE(spilled.empty());
    // Centered on the whole bed, not packed into the copy plate's zone.
    CHECK_THAT(extents(spilled).center().x(), WithinAbs(150., 10.));
}
