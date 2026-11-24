#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <utility>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Two 10 mm cubes printed as separate objects, the second shifted along X by `offset`, at the
// positions given (not arranged apart).
ConflictResultOpt conflict_between_cubes(double offset)
{
    TriangleMesh first  = make_cube(10., 10., 10.);
    TriangleMesh second = make_cube(10., 10., 10.);
    first.translate(50.f, 50.f, 0.f);
    second.translate(float(50. + offset), 50.f, 0.f);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });

    Print print;
    Model model;
    init_print({ std::move(first), std::move(second) }, print, model, config, nullptr, false);
    print.process();
    return print.get_conflict_result();
}

} // namespace

TEST_CASE("Objects whose extrusions cross are reported as a path conflict", "[ConflictChecker]")
{
    // A 5 mm shift puts half of each cube inside the other, so their walls cross on every layer of the
    // 10 mm high cubes.
    const ConflictResultOpt conflict = conflict_between_cubes(5.);
    REQUIRE(conflict.has_value());
    CHECK(conflict->_obj1 != conflict->_obj2);
    CHECK(conflict->_height <= 10.);
}

TEST_CASE("Objects apart from each other have no path conflict", "[ConflictChecker]")
{
    // 30 mm between the cube origins leaves a 20 mm gap between the 10 mm cubes.
    CHECK_FALSE(conflict_between_cubes(30.).has_value());
}

TEST_CASE("Copies of an object printed over each other are reported as a path conflict", "[ConflictChecker]")
{
    // Two instances of one 10 mm cube: not arranged, both stay at the origin of the object; arranged, they are
    // placed apart.
    const bool arranged = GENERATE(false, true);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "layer_height", "0.2" }, { "initial_layer_print_height", "0.2" } });

    TriangleMesh cube = make_cube(10., 10., 10.);
    cube.translate(50.f, 50.f, 0.f);
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(std::move(cube));
    init_print(std::move(meshes), print, model, config, nullptr, arranged, 2);
    print.process();
    CHECK(print.get_conflict_result().has_value() == ! arranged);
}
