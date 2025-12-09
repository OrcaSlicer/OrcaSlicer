#include <catch2/catch_all.hpp>
#include "test_utils.hpp"

#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>

using namespace Slic3r;

TEST_CASE("Building a tree over a box, ray caster and closest query", "[AABBIndirect]")
{
    TriangleMesh tmesh = make_cube(1., 1., 1.);

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tmesh.its.vertices, tmesh.its.indices);
    REQUIRE(! tree.empty());

    igl::Hit hit;
	bool intersected = AABBTreeIndirect::intersect_ray_first_hit(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
		Vec3d(0.5, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hit);

    REQUIRE(intersected);
    REQUIRE(hit.t == Catch::Approx(5.));

    std::vector<igl::Hit> hits;
	bool intersected2 = AABBTreeIndirect::intersect_ray_all_hits(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hits);
    REQUIRE(intersected2);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits.front().t == Catch::Approx(5.));
    REQUIRE(hits.back().t == Catch::Approx(6.));

    size_t hit_idx;
    Vec3d  closest_point;
    double squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, -5.),
		hit_idx, closest_point);
    REQUIRE(squared_distance == Catch::Approx(5. * 5.));
    REQUIRE(closest_point.x() == Catch::Approx(0.3));
    REQUIRE(closest_point.y() == Catch::Approx(0.5));
    REQUIRE(closest_point.z() == Catch::Approx(0.));

    squared_distance = AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
        Vec3d(0.3, 0.5, 5.),
		hit_idx, closest_point);
    REQUIRE(squared_distance == Catch::Approx(4. * 4.));
    REQUIRE(closest_point.x() == Catch::Approx(0.3));
    REQUIRE(closest_point.y() == Catch::Approx(0.5));
    REQUIRE(closest_point.z() == Catch::Approx(1.));
}
