#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"

#include "test_helpers.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Overlapping objects are reported at their lowest conflicting layer", "[ConflictChecker]")
{
    // Two 20mm cubes that overlap by 10mm cross each other's walls on every layer. The layers are checked in parallel,
    // and the lowest of them is reported, whichever thread finds a conflict first.
    Print print;
    Model model;
    Test::place_two_cubes_apart(-10., {{"layer_height", 0.2}}, print, model);
    print.process();

    const ConflictResultOpt conflict = print.get_conflict_result();
    REQUIRE(conflict.has_value());
    CHECK_THAT(conflict->_height, WithinAbs(print.objects().front()->layers().front()->bottom_z(), EPSILON));
}
