#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <igl/Hit.h>
#include <cstddef>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/AABBTreeIndirect.hpp>
#include <limits>
#include <random>
#include <utility>
#include <vector>
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

TEST_CASE("Building a tree over a box, ray caster and closest query", "[AABBIndirect]")
{
    TriangleMesh tmesh = make_cube(1., 1., 1.);

    auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(tmesh.its.vertices, tmesh.its.indices);
    REQUIRE(! tree.empty());

    igl::Hit<float> hit;
	bool intersected = AABBTreeIndirect::intersect_ray_first_hit(
		tmesh.its.vertices, tmesh.its.indices,
		tree,
		Vec3d(0.5, 0.5, -5.),
		Vec3d(0., 0., 1.),
		hit);

    REQUIRE(intersected);
    REQUIRE(hit.t == Catch::Approx(5.));

    std::vector<igl::Hit<float>> hits;
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

namespace {

// The depth-first, left to right traversal intersect_ray_first_hit() used before it visited the nearest
// child first. The ordered traversal must return the very same hit.
template<typename RayIntersectorType, typename Scalar>
bool reference_first_hit(const RayIntersectorType &ray_intersector, size_t node_idx, Scalar min_t, igl::Hit<float> &hit)
{
    const auto &node = ray_intersector.tree.node(node_idx);
    if (! AABBTreeIndirect::detail::ray_box_intersect_invdir(ray_intersector.origin, ray_intersector.invdir,
                                                             node.bbox.template cast<Scalar>(), Scalar(0), min_t))
        return false;
    if (node.is_leaf()) {
        auto   face = ray_intersector.faces[node.idx];
        double t, u, v;
        if (AABBTreeIndirect::detail::intersect_triangle(ray_intersector.origin, ray_intersector.dir,
                                                         ray_intersector.vertices[face(0)], ray_intersector.vertices[face(1)],
                                                         ray_intersector.vertices[face(2)], t, u, v, ray_intersector.eps) &&
            t > 0.) {
            hit = igl::Hit<float>{int(node.idx), -1, float(u), float(v), float(t)};
            return true;
        }
        return false;
    }
    igl::Hit<float> left_hit;
    igl::Hit<float> right_hit;
    bool            left_ret = reference_first_hit(ray_intersector, node_idx * 2 + 1, min_t, left_hit);
    if (left_ret && left_hit.t < min_t) {
        min_t = left_hit.t;
        hit   = left_hit;
    } else
        left_ret = false;
    bool right_ret = reference_first_hit(ray_intersector, node_idx * 2 + 2, min_t, right_hit);
    if (right_ret && right_hit.t < min_t)
        hit = right_hit;
    else
        right_ret = false;
    return left_ret || right_ret;
}

// Unit cube faces split into an n x n grid of triangles: many coplanar triangles sharing edges, so rays
// through edges and vertices hit several triangles at the same distance.
indexed_triangle_set grid_cube(int n)
{
    indexed_triangle_set its;
    for (int axis = 0; axis < 3; ++axis)
        for (int side = 0; side < 2; ++side)
            for (int i = 0; i < n; ++i)
                for (int j = 0; j < n; ++j) {
                    auto vertex = [&](int a, int b) {
                        Vec3f p;
                        p[axis]           = float(side);
                        p[(axis + 1) % 3] = float(a) / n;
                        p[(axis + 2) % 3] = float(b) / n;
                        its.vertices.emplace_back(p);
                        return int(its.vertices.size()) - 1;
                    };
                    int v0 = vertex(i, j), v1 = vertex(i + 1, j), v2 = vertex(i + 1, j + 1), v3 = vertex(i, j + 1);
                    its.indices.emplace_back(v0, v1, v2);
                    its.indices.emplace_back(v0, v2, v3);
                }
    return its;
}

} // namespace

TEST_CASE("First ray hit matches the depth-first search", "[AABBIndirect]")
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> coord(-1.5, 2.5);
    std::uniform_real_distribution<double> unit(-1., 1.);

    indexed_triangle_set soup;
    for (int i = 0; i < 300; ++i) {
        Vec3f a(coord(rng), coord(rng), coord(rng));
        int   base = int(soup.vertices.size());
        soup.vertices.emplace_back(a);
        soup.vertices.emplace_back(a + Vec3f(unit(rng), unit(rng), unit(rng)) * 0.5f);
        soup.vertices.emplace_back(a + Vec3f(unit(rng), unit(rng), unit(rng)) * 0.5f);
        soup.indices.emplace_back(base, base + 1, base + 2);
    }
    const indexed_triangle_set mesh = GENERATE_COPY(grid_cube(8), its_make_sphere(1., PI / 20.), soup);

    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(mesh.vertices, mesh.indices);
    REQUIRE(! tree.empty());

    // Directions with zero components make the inverse direction infinite. Origins on the grid, aimed
    // along the axes or the diagonals, put the rays through shared edges and vertices.
    std::vector<std::pair<Vec3d, Vec3d>> rays;
    const Vec3d axis_dirs[] = {Vec3d::UnitX(), -Vec3d::UnitX(), Vec3d::UnitY(), -Vec3d::UnitY(), Vec3d::UnitZ(), -Vec3d::UnitZ(),
                               Vec3d(1., 1., 0.).normalized(), Vec3d(1., 1., 1.).normalized(), Vec3d(-1., 0., 1.).normalized()};
    for (int i = -2; i <= 10; ++i)
        for (int j = -2; j <= 10; ++j)
            for (const Vec3d &dir : axis_dirs)
                rays.emplace_back(Vec3d(i / 8., j / 8., -1.) - dir * 2., dir);
    // Random rays aimed at random mesh vertices, so that most of them hit.
    std::uniform_int_distribution<size_t> vertex_idx(0, mesh.vertices.size() - 1);
    for (int i = 0; i < 20000; ++i) {
        const Vec3d origin(coord(rng), coord(rng), coord(rng));
        const Vec3d target = mesh.vertices[vertex_idx(rng)].cast<double>() + Vec3d(unit(rng), unit(rng), unit(rng)) * 0.01;
        if ((target - origin).squaredNorm() > 1e-6)
            rays.emplace_back(origin, (target - origin).normalized());
    }

    size_t hits = 0;
    size_t mismatches = 0;
    for (const auto &[origin, dir] : rays) {
        igl::Hit<float> hit{-1, -1, 0.f, 0.f, 0.f};
        const bool      found = AABBTreeIndirect::intersect_ray_first_hit(mesh.vertices, mesh.indices, tree, origin, dir, hit);

        const auto ray_intersector = AABBTreeIndirect::detail::RayIntersector<stl_vertex, stl_triangle_vertex_indices, AABBTreeIndirect::Tree<3, float>, Vec3d>{
            mesh.vertices, mesh.indices, tree, origin, dir, Vec3d(dir.cwiseInverse()), 0.000001};
        igl::Hit<float> expected{-1, -1, 0.f, 0.f, 0.f};
        const bool expected_found = reference_first_hit(ray_intersector, size_t(0), std::numeric_limits<double>::infinity(), expected);

        hits += found;
        if (found != expected_found || (found && (hit.id != expected.id || hit.t != expected.t || hit.u != expected.u || hit.v != expected.v)))
            ++mismatches;
    }
    CHECK(hits > rays.size() / 2);
    CHECK(mismatches == 0);
}
