#include <catch2/catch_all.hpp>

#include <cstdlib>

#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;

static inline Slic3r::Point3 random_point3(float LO=-50, float HI=50)
{
    Vec3f pt = Vec3f(LO, LO, LO) + (Vec3d(rand(), rand(), rand()) * (HI-LO) / RAND_MAX).cast<float>();
	return Point3(pt.cast<coord_t>());
}


// build a sample extrusion entity collection with random start and end points.
static Slic3r::ExtrusionPath random_path(size_t length = 20, float LO = -50, float HI = 50)
{
    ExtrusionPath t {erPerimeter, 1.0, 1.0, 1.0};
    for (size_t j = 0; j < length; ++ j)
        t.polyline.append(random_point3(LO, HI));
    return t;
}

static Slic3r::ExtrusionPaths random_paths(size_t count = 10, size_t length = 20, float LO = -50, float HI = 50)
{
    Slic3r::ExtrusionPaths p;
    for (size_t i = 0; i < count; ++ i)
        p.push_back(random_path(length, LO, HI));
    return p;
}

SCENARIO("Polygon flattening", "[ExtrusionEntity]") {
    srand(0xDEADBEEF); // consistent seed for test reproducibility.

    // Generate one specific random path set and save it for later comparison
    Slic3r::ExtrusionPaths nosort_path_set = random_paths();

    Slic3r::ExtrusionEntityCollection sub_nosort;
    sub_nosort.append(nosort_path_set);
    sub_nosort.no_sort = true;

    Slic3r::ExtrusionEntityCollection sub_sort;
    sub_sort.no_sort = false;
    sub_sort.append(random_paths());

    GIVEN("A Extrusion Entity Collection with a child that has one child that is marked as no-sort") {
        Slic3r::ExtrusionEntityCollection sample;
        Slic3r::ExtrusionEntityCollection output;

        sample.append(sub_sort);
        sample.append(sub_nosort);
        sample.append(sub_sort);

        WHEN("The EEC is flattened with default options (preserve_order=false)") {
			output = sample.flatten();
            THEN("The output EEC contains no Extrusion Entity Collections") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection();}) == 0);
            }
        }
        WHEN("The EEC is flattened with preservation (preserve_order=true)") {
			output = sample.flatten(true);
            THEN("The output EECs contains one EEC.") {
                CHECK(std::count_if(output.entities.cbegin(), output.entities.cend(), [=](const ExtrusionEntity* e) {return e->is_collection();}) == 1);
            }
            AND_THEN("The ordered EEC contains the same order of elements than the original") {
                // find the entity in the collection
                for (auto e : output.entities)
                    if (e->is_collection()) {
                        ExtrusionEntityCollection *temp = dynamic_cast<ExtrusionEntityCollection*>(e);
                        // check each Extrusion path against nosort_path_set to see if the first and last match the same
                        CHECK(nosort_path_set.size() == temp->entities.size());
                        for (size_t i = 0; i < nosort_path_set.size(); ++ i) {
                            CHECK(temp->entities[i]->first_point() == nosort_path_set[i].first_point());
                            CHECK(temp->entities[i]->last_point() == nosort_path_set[i].last_point());
                        }
                    }
            }
        }
    }
}

static ExtrusionPaths straight_path(const std::vector<double> &xs)
{
    ExtrusionPath path{erExternalPerimeter, 1.0, 0.45f, 0.2f};
    for (double x : xs)
        path.polyline.append(Point3::new_scale(x, 0., 0.));
    return {path};
}

TEST_CASE("Scarf ramp ends on the next loop vertex instead of leaving a short stub", "[ExtrusionEntity]")
{
    using Catch::Matchers::WithinAbs;
    // A 20 mm scarf in 10 steps: a remainder shorter than half a 2 mm step is snapped forward.
    const double slope_length = 20.;
    const double max_segment  = scale_(slope_length / 10);

    SECTION("a 0.09 mm remainder extends the ramp to the vertex") {
        ExtrusionPaths     paths = straight_path({0., 5., 10., 15., 20.09, 25., 30.});
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.starts.size() == 1);
        REQUIRE(loop.ends.size() == 1);
        REQUIRE(loop.paths.size() == 1);
        CHECK_THAT(unscale_(loop.starts.front().polyline.last_point().x()), WithinAbs(20.09, 1e-3));
        CHECK_THAT(unscale_(loop.ends.front().polyline.last_point().x()), WithinAbs(20.09, 1e-3));
        CHECK_THAT(unscale_(loop.paths.front().polyline.first_point().x()), WithinAbs(20.09, 1e-3));
        CHECK_THAT(unscale_(loop.paths.front().polyline.lines().front().length()), WithinAbs(4.91, 1e-3));
    }

    SECTION("a remainder longer than half a step keeps the exact scarf length") {
        ExtrusionPaths     paths = straight_path({0., 5., 10., 15., 21.5, 25., 30.});
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.starts.size() == 1);
        REQUIRE(loop.paths.size() == 1);
        CHECK_THAT(unscale_(loop.starts.front().polyline.last_point().x()), WithinAbs(20., 1e-3));
        CHECK_THAT(unscale_(loop.paths.front().polyline.first_point().x()), WithinAbs(20., 1e-3));
        CHECK_THAT(unscale_(loop.paths.front().polyline.lines().front().length()), WithinAbs(1.5, 1e-3));
    }

    SECTION("the ramp never grows by more than a millimetre, whatever the step size") {
        ExtrusionPaths     paths = straight_path({0., 5., 10., 15., 21.5, 25., 30.});
        ExtrusionLoopSloped loop(paths, 0., slope_length, scale_(slope_length), 0.); // a single 20 mm step
        REQUIRE(loop.paths.size() == 1);
        CHECK_THAT(unscale_(loop.starts.front().polyline.last_point().x()), WithinAbs(20., 1e-3));
    }

    SECTION("snapping onto the path's last vertex leaves no single-point flat path") {
        ExtrusionPaths     paths = straight_path({0., 5., 10., 15., 20.5});
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.starts.size() == 1);
        CHECK(loop.paths.empty());
        CHECK_THAT(unscale_(loop.starts.front().polyline.last_point().x()), WithinAbs(20.5, 1e-3));
    }
}

TEST_CASE("Scarf loop drops the micro segments the seam insertion leaves at both ends", "[ExtrusionEntity]")
{
    using Catch::Matchers::WithinAbs;
    const double slope_length = 20.;
    const double max_segment  = scale_(slope_length / 10);

    SECTION("a 3 um segment at each end of a single path is removed, the seam point stays") {
        ExtrusionPaths     paths = straight_path({0., 0.003, 5., 10., 15., 21.5, 25., 29.997, 30.});
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.starts.size() == 1);
        REQUIRE(loop.paths.size() == 1);
        const Polyline3 &start = loop.starts.front().polyline;
        CHECK_THAT(unscale_(start.first_point().x()), WithinAbs(0., 1e-4));
        CHECK_THAT(unscale_(start.lines().front().length()), WithinAbs(1.25, 1e-3)); // 5 mm halved twice
        const Polyline3 &flat = loop.paths.front().polyline;
        CHECK_THAT(unscale_(flat.last_point().x()), WithinAbs(30., 1e-4));
        CHECK_THAT(unscale_(flat.lines().back().length()), WithinAbs(5., 1e-3));
    }

    SECTION("a micro path of its own is dropped and the neighbour ends at the seam point") {
        ExtrusionPaths paths = straight_path({0., 0.003});
        ExtrusionPaths rest  = straight_path({0.003, 5., 10., 15., 21.5, 25., 30.});
        paths.push_back(rest.front());
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.starts.size() == 1);
        CHECK_THAT(unscale_(loop.starts.front().polyline.first_point().x()), WithinAbs(0., 1e-4));
        CHECK_THAT(unscale_(loop.starts.front().polyline.lines().front().length()), WithinAbs(1.25, 1e-3));
    }

    SECTION("a scarf covering the whole loop still ends at full flow after a trim") {
        // The caller sizes the scarf from the untrimmed loop: 10.003 mm here, 10 mm after the trim.
        ExtrusionPaths     paths = straight_path({0., 0.003, 5., 10.});
        ExtrusionLoopSloped loop(paths, 0., 10.003, max_segment, 0.);
        REQUIRE(loop.starts.size() == 1);
        CHECK(loop.paths.empty());
        CHECK_THAT(loop.starts.back().slope_end.e_ratio, WithinAbs(1., 1e-9));
        CHECK_THAT(unscale_(loop.starts.back().polyline.last_point().x()), WithinAbs(10., 1e-4));
    }

    SECTION("segments longer than the tolerance are kept") {
        ExtrusionPaths     paths = straight_path({0., 0.3, 5., 10., 15., 21.5, 25., 29.7, 30.});
        ExtrusionLoopSloped loop(paths, 0., slope_length, max_segment, 0.);
        REQUIRE(loop.paths.size() == 1);
        CHECK_THAT(unscale_(loop.starts.front().polyline.lines().front().length()), WithinAbs(0.3, 1e-3));
        CHECK_THAT(unscale_(loop.paths.front().polyline.lines().back().length()), WithinAbs(0.3, 1e-3));
    }
}
