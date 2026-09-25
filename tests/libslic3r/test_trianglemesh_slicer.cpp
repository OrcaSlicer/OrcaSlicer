#include <catch2/catch_all.hpp>

#include <tbb/global_control.h>

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"

using namespace Slic3r;

// The slab slicer collects each slab's intersection lines from a parallel loop over the facets.
// Its loops, and therefore the projected polygons, are derived from the order of those lines, so
// the order has to be canonical or the same mesh projects to different polygons run to run.
// The single-threaded projection is the reference; every multi-threaded run must reproduce it
// exactly, vertex order included.
TEST_CASE("Slab slicing projects the same polygons whatever the thread schedule", "[TriangleMeshSlicer]")
{
    // A dense sphere, tilted so no facet is axis aligned: thousands of upward and downward
    // facing facets spread over every slab.
    indexed_triangle_set mesh = its_make_sphere(10., 0.05);
    Transform3d trafo = Transform3d::Identity();
    trafo.rotate(Eigen::AngleAxisd(0.37, Vec3d(0.3, 0.5, 1.).normalized()));
    trafo.translate(Vec3d(1., 2., 0.));
    std::vector<float> zs;
    for (float z = -9.7f; z < 9.7f; z += 0.2f)
        zs.emplace_back(z);

    auto project = [&mesh, &trafo, &zs]() {
        std::vector<Polygons> top, bottom;
        slice_mesh_slabs(mesh, zs, trafo, &top, &bottom, nullptr, []{});
        return std::make_pair(std::move(top), std::move(bottom));
    };

    std::pair<std::vector<Polygons>, std::vector<Polygons>> reference;
    {
        tbb::global_control single_thread(tbb::global_control::max_allowed_parallelism, 1);
        reference = project();
    }
    REQUIRE(reference.first.size() == zs.size());
    REQUIRE(std::any_of(reference.first.begin(), reference.first.end(), [](const Polygons &p) { return !p.empty(); }));
    REQUIRE(std::any_of(reference.second.begin(), reference.second.end(), [](const Polygons &p) { return !p.empty(); }));

    for (int run = 0; run < 3; ++run) {
        DYNAMIC_SECTION("multi-threaded run " << run)
        {
            auto parallel = project();
            CHECK(parallel.first == reference.first);
            CHECK(parallel.second == reference.second);
        }
    }
}
