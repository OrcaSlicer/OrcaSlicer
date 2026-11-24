#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstddef>
#include <limits>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Fill/Lightning/DistanceField.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

TEST_CASE("Lightning distance field hands out each sample once when the overhang reaches past the outlines", "[Lightning][Regression]")
{
    const coord_t supporting_radius = scaled<coord_t>(4.4);
    // DistanceField samples the overhang on a grid of this pitch.
    const coord_t cell_size = supporting_radius / 6;

    const coord_t side             = scaled<coord_t>(10.);
    const Polygon outline          = BoundingBox(Point(0, 0), Point(side, side)).polygon();
    const BoundingBox outline_bbox = get_extents(outline);

    // Every sample of this sliver lies past one side of the outlines.
    const coord_t gap        = cell_size / 2;
    const coord_t reach      = cell_size * 5 / 2;
    const BoundingBox sliver = GENERATE_COPY(BoundingBox(Point(0, -reach), Point(side, -gap)),
                                             BoundingBox(Point(-reach, 0), Point(-gap, side)),
                                             BoundingBox(Point(0, side + gap), Point(side, side + reach)),
                                             BoundingBox(Point(side + gap, 0), Point(side + reach, side)));
    CAPTURE(sliver.min.x(), sliver.min.y(), sliver.max.x(), sliver.max.y());

    FillLightning::DistanceField distance_field(supporting_radius, {outline}, outline_bbox, {sliver.polygon()});

    bool sample_outside_outlines = false;
    size_t previous_idx          = std::numeric_limits<size_t>::max();
    size_t idx                   = 0;
    Point location;
    while (distance_field.tryGetNextPoint(&location, &idx, idx)) {
        REQUIRE(idx != previous_idx);
        previous_idx            = idx;
        sample_outside_outlines = sample_outside_outlines || !outline_bbox.contains(location);
        distance_field.update(location + Point(0, cell_size), location);
    }
    CHECK(sample_outside_outlines);
}
