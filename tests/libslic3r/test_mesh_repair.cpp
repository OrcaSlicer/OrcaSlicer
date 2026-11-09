#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/MeshRepair.hpp>
#include <libslic3r/Point.hpp>
#include <libslic3r/libslic3r.h>
#include <libslic3r/TriangleMesh.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinRel;

namespace {

indexed_triangle_set cube(double size, const Vec3d &offset = Vec3d::Zero())
{
    TriangleMesh mesh = make_cube(size, size, size);
    mesh.translate(offset.cast<float>());
    return mesh.its;
}

indexed_triangle_set merged(const std::vector<indexed_triangle_set> &parts)
{
    indexed_triangle_set out;
    for (const indexed_triangle_set &part : parts)
        its_merge(out, part);
    return out;
}

// Every triangle with its own three vertices, as some exporters write meshes.
indexed_triangle_set soup(const indexed_triangle_set &its)
{
    indexed_triangle_set out;
    for (const stl_triangle_vertex_indices &f : its.indices) {
        const int first = int(out.vertices.size());
        for (int k = 0; k < 3; ++k)
            out.vertices.push_back(its.vertices[f[k]]);
        out.indices.emplace_back(first, first + 1, first + 2);
    }
    return out;
}

double regular_polygon_area(int sides, double radius) { return 0.5 * sides * radius * radius * std::sin(2. * PI / sides); }

// Closed by vertex indices, outward oriented and free of self-intersections.
void check_solid(const TriangleMesh &mesh, double volume)
{
    CHECK(its_num_open_edges(mesh.its) == 0);
    CHECK_THAT(its_volume(mesh.its), WithinRel(volume, 1e-4));
    CHECK_FALSE(MeshBoolean::cgal::does_self_intersect(mesh));
}

} // namespace

TEST_CASE("A triangle soup repairs into the solid it describes", "[MeshRepair]")
{
    TriangleMesh mesh(soup(cube(20.)));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.modified);
    check_solid(mesh, 20. * 20. * 20.);
    CHECK(mesh.its.vertices.size() == 8);
}

TEST_CASE("Flipped triangles take the orientation of their neighbours", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    std::swap(its.indices[1][1], its.indices[1][2]);
    std::swap(its.indices[5][1], its.indices[5][2]);
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("An inside-out mesh is turned outward", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    its_flip_triangles(its);
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("Holes are filled", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    its.indices.erase(its.indices.begin(), its.indices.begin() + 3);
    TriangleMesh mesh(std::move(its));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.filled_holes > 0);
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("Cracks between triangles that should share corners are closed", "[MeshRepair]")
{
    indexed_triangle_set its = soup(cube(20.));
    // Shift the corners of two triangles by far less than a micron, as float rounding in an exporter would.
    for (std::size_t v = 0; v < 6; ++v)
        its.vertices[v] += Vec3f(1e-5f, -1e-5f, 1e-5f);
    TriangleMesh mesh(std::move(its));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.welded_vertices > 0);
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("A T-junction is closed", "[MeshRepair]")
{
    // The top of the cube is split at the middle of its front edge, which the front side does not share.
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {20, 0, 0}, {20, 20, 0}, {0, 20, 0}, {0, 0, 20}, {20, 0, 20}, {20, 20, 20}, {0, 20, 20}, {10, 0, 20}};
    its.indices  = {{0, 2, 1}, {0, 3, 2}, {4, 8, 7}, {8, 6, 7}, {8, 5, 6}, {0, 1, 5}, {0, 5, 4},
                    {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 20. * 20. * 20.);
}

TEST_CASE("Crossing shells stay separate parts and are made solid", "[MeshRepair]")
{
    indexed_triangle_set inside_out = cube(20., {10., 10., 10.});
    its_flip_triangles(inside_out);
    TriangleMesh mesh(merged({cube(20.), inside_out}));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.reversed_shells == 1);
    CHECK(its_num_open_edges(mesh.its) == 0);
    CHECK_THAT(its_volume(mesh.its), WithinRel(2 * 8000., 1e-4));
    CHECK(mesh.split().size() == 2);
}

TEST_CASE("Solids touching face to face in a soup lose the wall between them", "[MeshRepair]")
{
    // The mirrored copy shares the wall at x = 10 with exactly the same triangles, reversed.
    TriangleMesh right = make_cube(10., 10., 10.);
    right.mirror(X);
    right.translate(20.f, 0.f, 0.f);
    TriangleMesh mesh(soup(merged({cube(10.), right.its})));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 2 * 1000.);
    CHECK(mesh.split().size() == 1);
}

TEST_CASE("A shell inside a solid follows the nonzero winding rule of slicing", "[MeshRepair]")
{
    SECTION("Oriented like the outer shell, it adds nothing and is removed")
    {
        TriangleMesh mesh(merged({cube(20.), cube(10., {5., 5., 5.})}));
        REQUIRE(repair_mesh(mesh));
        check_solid(mesh, 8000.);
        CHECK(mesh.its.indices.size() == 12);
    }
    SECTION("Oriented inward, it is a cavity and stays")
    {
        indexed_triangle_set cavity = cube(10., {5., 5., 5.});
        its_flip_triangles(cavity);
        TriangleMesh mesh(merged({cube(20.), cavity}));
        MeshRepairStats stats;
        REQUIRE(repair_mesh(mesh, &stats));
        CHECK_FALSE(stats.modified);
        CHECK_THAT(its_volume(mesh.its), WithinRel(8000. - 1000., 1e-4));
    }
}

TEST_CASE("A pipe missing its end caps gets annular caps", "[MeshRepair]")
{
    const int    n = 64;
    const double h = 20., r_out = 10., r_in = 7.;
    indexed_triangle_set its;
    for (const double r : {r_out, r_in})
        for (int k = 0; k < n; ++k) {
            const double a = 2. * PI * k / n;
            its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), 0.f);
            its.vertices.emplace_back(float(r * std::cos(a)), float(r * std::sin(a)), float(h));
        }
    for (int k = 0; k < n; ++k) {
        const int a0 = 2 * k, b0 = a0 + 1, a1 = 2 * ((k + 1) % n), b1 = a1 + 1;
        its.indices.emplace_back(a0, a1, b1);
        its.indices.emplace_back(a0, b1, b0);
        its.indices.emplace_back(2 * n + a0, 2 * n + b1, 2 * n + a1);
        its.indices.emplace_back(2 * n + a0, 2 * n + b0, 2 * n + b1);
    }
    TriangleMesh mesh(std::move(its));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, h * (regular_polygon_area(n, r_out) - regular_polygon_area(n, r_in)));
}

TEST_CASE("A stray flat triangle is dropped", "[MeshRepair]")
{
    indexed_triangle_set its = cube(20.);
    its.vertices.insert(its.vertices.end(), {{30, 0, 0}, {40, 0, 0}, {30, 10, 0}});
    its.indices.emplace_back(8, 9, 10);
    TriangleMesh mesh(std::move(its));
    MeshRepairStats stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK(stats.removed_shells == 1);
    check_solid(mesh, 8000.);
}

TEST_CASE("A two-sided export keeps one side of each triangle", "[MeshRepair]")
{
    indexed_triangle_set back = cube(20.);
    its_flip_triangles(back);
    TriangleMesh mesh(soup(merged({cube(20.), back})));
    REQUIRE(repair_mesh(mesh));
    check_solid(mesh, 8000.);
}

TEST_CASE("A valid mesh is left untouched", "[MeshRepair]")
{
    TriangleMesh       mesh = make_sphere(10., 2. * PI / 36.);
    const TriangleMesh original = mesh;
    MeshRepairStats    stats;
    REQUIRE(repair_mesh(mesh, &stats));
    CHECK_FALSE(stats.modified);
    CHECK(mesh.its.vertices == original.its.vertices);
    CHECK(mesh.its.indices == original.its.indices);
}

TEST_CASE("A mesh enclosing no volume is reported and left untouched", "[MeshRepair]")
{
    indexed_triangle_set its;
    its.vertices = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 10, 0}};
    its.indices  = {{0, 1, 2}, {0, 2, 3}};
    TriangleMesh mesh(its);
    std::string  error;
    CHECK_FALSE(repair_mesh(mesh, nullptr, &error));
    CHECK_FALSE(error.empty());
    CHECK(mesh.its.indices.size() == 2);
}

TEST_CASE("Progress is reported in order and throwing from it cancels the repair", "[MeshRepair]")
{
    TriangleMesh     mesh(soup(cube(20.)));
    std::vector<int> reported;
    REQUIRE(repair_mesh(mesh, nullptr, nullptr, [&reported](int percent) { reported.push_back(percent); }));
    CHECK_FALSE(reported.empty());
    CHECK(std::is_sorted(reported.begin(), reported.end()));

    struct Canceled {};
    TriangleMesh       canceled(soup(cube(20.)));
    const TriangleMesh original = canceled;
    CHECK_THROWS_AS(repair_mesh(canceled, nullptr, nullptr, [](int) { throw Canceled(); }), Canceled);
    CHECK(canceled.its.vertices == original.its.vertices);
    CHECK(canceled.its.indices == original.its.indices);
}
