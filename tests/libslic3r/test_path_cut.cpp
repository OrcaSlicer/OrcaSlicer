#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <libslic3r/PathCutConnectors.hpp>
#include <libslic3r/PathCutGeometry.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/libslic3r.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <utility>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// Number of edges used by exactly one face (0 for a closed mesh) and the largest number of faces on one edge.
void edge_counts(const indexed_triangle_set &its, size_t &open, size_t &max_faces)
{
    std::map<std::pair<int, int>, size_t> count;
    for (const auto &f : its.indices)
        for (int e = 0; e < 3; ++e) {
            const int a = f[e], b = f[(e + 1) % 3];
            ++count[{std::min(a, b), std::max(a, b)}];
        }
    open = max_faces = 0;
    for (const auto &[edge, n] : count) {
        if (n == 1) ++open;
        max_faces = std::max(max_faces, n);
    }
}

void require_closed_parts(const std::vector<indexed_triangle_set> &parts)
{
    for (const auto &p : parts) {
        size_t open, max_faces;
        edge_counts(p, open, max_faces);
        CHECK(open == 0);
        CHECK(max_faces == 2);
        CHECK(its_volume(p) > 0.);
    }
}

} // namespace

TEST_CASE("A wavy path picked from the front splits a cube into two closed parts", "[PathCut]")
{
    // 20 mm cube, path drawn looking along +Y, zig-zagging across X at mid height.
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    const double               kerf = 0.2;
    const std::vector<PathCut::V3> pts{{2., 0., 8.}, {7., 0., 13.}, {13., 0., 7.}, {18., 0., 12.}};

    const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3::Constant(20.), kerf);
    REQUIRE(vc.valid());

    const PathCut::CutResult res = PathCut::cut_with_slab(cube, vc, false);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    REQUIRE(parts.size() == 2);
    require_closed_parts(parts);

    const PathCut::Grouped g = PathCut::group_components(parts, [&vc](const PathCut::V3 &p) { return vc.side(p); }, false);
    CHECK(g.n_a == 1);
    CHECK(g.n_b == 1);
    // The two halves add up to the cube minus a wall kerf thick, 20 mm deep, as long as the path inside the cube.
    const double total = its_volume(g.side_a) + its_volume(g.side_b);
    CHECK(total < 8000.);
    CHECK(total > 8000. - kerf * 20. * 40.);
}

TEST_CASE("A view path limited in depth cuts the front box and leaves the one behind it whole", "[PathCut]")
{
    // Looking along +Y: a front box (y 0..10) and a back box (y 20..30).
    indexed_triangle_set boxes = its_make_cube(20., 10., 20.), back = its_make_cube(20., 10., 20.);
    its_translate(back, Vec3f(0.f, 20.f, 0.f));
    its_merge(boxes, back);
    const std::vector<PathCut::V3> pts{{2., 0., 10.}, {10., 0., 12.}, {18., 0., 9.}};

    SECTION("15 mm behind the points stops in the gap")
    {
        PathCut::ViewLimits lim;
        lim.back = 15.;
        const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3(20., 30., 20.), 0.2, lim);
        REQUIRE(vc.limited());
        const PathCut::CutResult res = PathCut::cut_with_slab(boxes, vc, vc.limited());
        REQUIRE(res.error.empty());
        const auto parts = its_split(res.mesh);
        CHECK(parts.size() == 3);
        require_closed_parts(parts);
        bool joined = true;
        const PathCut::Grouped g = PathCut::group_cut(res, [&vc](const PathCut::V3 &p) { return vc.side(p); }, false, &joined);
        CHECK_FALSE(joined);
        CHECK(g.n_a + g.n_b == 3);
    }
    SECTION("25 mm behind the points ends inside the back box: the front box is cut, the back box is left whole")
    {
        PathCut::ViewLimits lim;
        lim.back = 25.;
        const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3(20., 30., 20.), 0.2, lim);
        const PathCut::CutResult res = PathCut::cut_with_slab(boxes, vc, vc.limited());
        REQUIRE(res.error.empty());
        const auto parts = its_split(res.mesh);
        CHECK(parts.size() == 3);
        require_closed_parts(parts);
        const bool back_whole = std::any_of(parts.begin(), parts.end(), [](const indexed_triangle_set &p) { return std::abs(its_volume(p) - 4000.) < 1.; });
        CHECK(back_whole);
    }
    SECTION("5 mm behind the points ends inside the front box, the only one it reaches, and is reported")
    {
        PathCut::ViewLimits lim;
        lim.back = 5.;
        const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3(20., 30., 20.), 0.2, lim);
        CHECK_FALSE(PathCut::cut_with_slab(boxes, vc, vc.limited()).error.empty());
    }
}

TEST_CASE("A loop cap leaves other parts of the model next to the loop uncut", "[PathCut]")
{
    // Cylinder r = 5 (the limb) and a 5 x 10 x 30 box 2 mm beside it, both crossing the plane of a loop
    // around the cylinder at z = 15. Only the cylinder is cut; the box lies outside the loop.
    const double         r = 5.;
    indexed_triangle_set model = its_make_cylinder(r, 30.);
    indexed_triangle_set box   = its_make_cube(5., 10., 30.);
    for (Vec3f &v : box.vertices) v += Vec3f(7.f, -5.f, 0.f);
    its_merge(model, box);
    std::vector<PathCut::V3> loop;
    for (int i = 0; i < 12; ++i) {
        const double a = 2. * PI * i / 12.;
        loop.emplace_back(r * std::cos(a), r * std::sin(a), 15.);
    }
    PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 1.5, [](const PathCut::V3 &p) { return p; });
    REQUIRE(lc.valid());
    REQUIRE(PathCut::fit_loop_margin(model, lc));

    const PathCut::CutResult res = PathCut::cut_with_slab(model, lc, true);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    REQUIRE(parts.size() == 3);
    require_closed_parts(parts);
    const bool box_whole = std::any_of(parts.begin(), parts.end(), [](const indexed_triangle_set &p) { return std::abs(its_volume(p) - 1500.) < 1.; });
    CHECK(box_whole);
}

TEST_CASE("A cut exactly along a loop splits only the surface the loop runs on", "[PathCut]")
{
    // Cylinder r = 5 with a wavy loop around it, and a box 2 mm beside it crossing the loop's height.
    const double         r = 5.;
    indexed_triangle_set model = its_make_cylinder(r, 30.);
    indexed_triangle_set box   = its_make_cube(5., 10., 30.);
    for (Vec3f &v : box.vertices) v += Vec3f(7.f, -5.f, 0.f);
    its_merge(model, box);
    std::vector<PathCut::V3> loop;
    for (int i = 0; i < 12; ++i) {
        const double a = 2. * PI * i / 12.;
        loop.emplace_back(r * std::cos(a), r * std::sin(a), 15. + std::sin(2. * a));
    }
    const PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 1.5, [](const PathCut::V3 &p) { return p; }, false);
    REQUIRE(lc.valid());

    const PathCut::CutResult res = PathCut::cut_along_loop(model, lc, 1.5);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    REQUIRE(parts.size() == 3);
    require_closed_parts(parts);
    const bool box_whole = std::any_of(parts.begin(), parts.end(), [](const indexed_triangle_set &p) { return std::abs(its_volume(p) - 1500.) < 1.; });
    CHECK(box_whole);

    bool joined = false;
    const PathCut::Grouped g = PathCut::group_cut(res, [](const PathCut::V3 &) { return 0; }, true, &joined);
    CHECK_FALSE(joined);
    // The cylinder is split about its middle; the box, untouched, stays with the lower part.
    const double half = PI * r * r * 15.;
    CHECK_THAT(std::min(its_volume(g.side_a), its_volume(g.side_b)), WithinAbs(half, 0.1 * half));
}

TEST_CASE("A cut along a loop around a part still connected elsewhere is made and reported as joined", "[PathCut]")
{
    // A ring (torus, R = 15, r = 4): a loop around its tube cuts it, but the ring stays in one piece the
    // other way round, like an arm whose sword rests on a foot. The cut is made so the rest can be cut next.
    const double         R = 15., r = 4.;
    constexpr int        nu = 64, nv = 24;
    indexed_triangle_set ring;
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            const double u = 2. * PI * i / nu, v = 2. * PI * j / nv;
            ring.vertices.emplace_back(float((R + r * std::cos(v)) * std::cos(u)), float((R + r * std::cos(v)) * std::sin(u)),
                                       float(r * std::sin(v)));
        }
    auto at = [](int i, int j) { return ((i + nu) % nu) * nv + (j + nv) % nv; };
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            ring.indices.emplace_back(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            ring.indices.emplace_back(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }
    REQUIRE(its_volume(ring) > 0.);

    std::vector<PathCut::V3> loop;
    const double             u0 = 0.05; // between two rings of vertices
    for (int k = 0; k < 12; ++k) {
        const double v = 2. * PI * k / 12.;
        loop.emplace_back((R + r * std::cos(v)) * std::cos(u0), (R + r * std::cos(v)) * std::sin(u0), r * std::sin(v));
    }
    const PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 1.5, [](const PathCut::V3 &p) { return p; }, false);
    REQUIRE(lc.valid());

    const PathCut::CutResult res = PathCut::cut_along_loop(ring, lc, 1.5);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    CHECK(parts.size() == 1);
    require_closed_parts(parts);
    CHECK(its_volume(res.mesh) < its_volume(ring)); // the kerf is gone

    bool joined = false;
    PathCut::group_cut(res, [](const PathCut::V3 &) { return 0; }, true, &joined);
    CHECK(joined);
    CHECK_FALSE(PathCut::joined_routes(res).empty());
}

TEST_CASE("Connectors go on the faces of every cut it took to separate the pieces", "[PathCut]")
{
    // A ring (torus, R = 15, r = 4) cut around its tube on one side stays in one piece; a second loop on
    // the other side separates it. The pieces come apart along both faces, so connectors go on both.
    const double         R = 15., r = 4.;
    constexpr int        nu = 64, nv = 24;
    indexed_triangle_set ring;
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            const double u = 2. * PI * i / nu, v = 2. * PI * j / nv;
            ring.vertices.emplace_back(float((R + r * std::cos(v)) * std::cos(u)), float((R + r * std::cos(v)) * std::sin(u)),
                                       float(r * std::sin(v)));
        }
    auto at = [](int i, int j) { return ((i + nu) % nu) * nv + (j + nv) % nv; };
    for (int i = 0; i < nu; ++i)
        for (int j = 0; j < nv; ++j) {
            ring.indices.emplace_back(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            ring.indices.emplace_back(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }
    auto cut_around = [R, r](const indexed_triangle_set &its, double u0) {
        std::vector<PathCut::V3> loop;
        for (int k = 0; k < 12; ++k) {
            const double v = 2. * PI * k / 12.;
            loop.emplace_back((R + r * std::cos(v)) * std::cos(u0), (R + r * std::cos(v)) * std::sin(u0), r * std::sin(v));
        }
        const PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 1.5, [](const PathCut::V3 &p) { return p; }, false);
        REQUIRE(lc.valid());
        return PathCut::cut_along_loop(its, lc, 1.5);
    };
    auto no_side = [](const PathCut::V3 &) { return 0; };

    const PathCut::CutResult first = cut_around(ring, 0.05);
    REQUIRE(first.error.empty());
    bool joined = false;
    PathCut::group_cut(first, no_side, true, &joined);
    REQUIRE(joined);
    indexed_triangle_set first_faces; // as the tool keeps them: a copy of the cap triangles
    for (size_t i = first.first_cap; i < first.mesh.indices.size(); ++i) {
        const int base = int(first_faces.vertices.size());
        for (int k = 0; k < 3; ++k) first_faces.vertices.push_back(first.mesh.vertices[size_t(first.mesh.indices[i][k])]);
        first_faces.indices.emplace_back(base, base + 1, base + 2);
    }

    PathCut::CutResult second = cut_around(first.mesh, PI + 0.05);
    REQUIRE(second.error.empty());
    std::vector<signed char> vertex_side;
    const PathCut::Grouped   g = PathCut::group_cut(second, no_side, true, &joined, &vertex_side);
    REQUIRE_FALSE(joined);
    PathCut::add_earlier_faces(second, first_faces, vertex_side);
    CHECK(second.earlier_faces.size() == first_faces.indices.size());

    const int                   level = PathCut::peg_side(second, g.side_a, g.side_b);
    const indexed_triangle_set &from  = level == 1 ? g.side_a : g.side_b;
    const indexed_triangle_set &to    = level == 1 ? g.side_b : g.side_a;
    PathCut::ConnectorParams    prm;
    prm.type  = PathCut::ConnectorType::Plug;
    prm.count = 2;
    prm.size  = 2.;
    prm.depth = 3.;
    prm.wall  = 0.5;
    std::string                    error;
    const PathCut::ConnectorResult c = PathCut::place_connectors(second, level, from, to, prm, error);
    REQUIRE(error.empty());
    REQUIRE(c.positions.size() == 2);
    // One on each face: the first cut's at x = +R, the second's at x = -R.
    CHECK(std::max(c.positions[0].x(), c.positions[1].x()) > 10.);
    CHECK(std::min(c.positions[0].x(), c.positions[1].x()) < -10.);
    for (const PathCut::V3 &p : c.positions) {
        CHECK(PathCut::point_inside(from, p - 1. * c.axis));
        CHECK(PathCut::point_inside(to, p + prm.depth * c.axis));
    }
}

TEST_CASE("A cut exactly along a loop around a pipe keeps its bore open in both pieces", "[PathCut]")
{
    // A pipe (outer r = 8, bore r = 4, h = 30) cut by a loop around its outside at mid height: the walls of
    // the bore cross the cap and must be cut too, leaving the bore open, not closed over by the cap.
    const double         R = 8., r = 4., h = 30.;
    constexpr int        n = 48;
    indexed_triangle_set pipe;
    for (int ring = 0; ring < 4; ++ring) // outer bottom, outer top, inner bottom, inner top
        for (int i = 0; i < n; ++i) {
            const double a = 2. * PI * i / n, rr = ring < 2 ? R : r, z = ring % 2 ? h : 0.;
            pipe.vertices.emplace_back(float(rr * std::cos(a)), float(rr * std::sin(a)), float(z));
        }
    auto v = [](int ring, int i) { return ring * n + (i + n) % n; };
    for (int i = 0; i < n; ++i) {
        pipe.indices.emplace_back(v(0, i), v(0, i + 1), v(1, i + 1)); // outside wall, facing out
        pipe.indices.emplace_back(v(0, i), v(1, i + 1), v(1, i));
        pipe.indices.emplace_back(v(2, i), v(3, i + 1), v(2, i + 1)); // bore wall, facing the axis
        pipe.indices.emplace_back(v(2, i), v(3, i), v(3, i + 1));
        pipe.indices.emplace_back(v(1, i), v(1, i + 1), v(3, i + 1)); // top ring, facing up
        pipe.indices.emplace_back(v(1, i), v(3, i + 1), v(3, i));
        pipe.indices.emplace_back(v(0, i), v(2, i + 1), v(0, i + 1)); // bottom ring, facing down
        pipe.indices.emplace_back(v(0, i), v(2, i), v(2, i + 1));
    }
    const double full = PI * (R * R - r * r) * h;
    REQUIRE_THAT(its_volume(pipe), WithinAbs(full, 0.02 * full));

    std::vector<PathCut::V3> loop;
    for (int k = 0; k < 12; ++k) {
        const double a = 2. * PI * (k + 0.5) / 12.;
        loop.emplace_back(R * std::cos(a), R * std::sin(a), 15. + 0.5 * std::sin(a));
    }
    const PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 1.5, [](const PathCut::V3 &p) { return p; }, false);
    REQUIRE(lc.valid());
    const PathCut::CutResult res = PathCut::cut_along_loop(pipe, lc, 1.5);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    REQUIRE(parts.size() == 2);
    require_closed_parts(parts);
    // Each piece is half the pipe: with the bore closed over by the cap, the walls of the whole bore would
    // stay with one piece and the volumes would be far apart.
    for (const auto &p : parts)
        CHECK_THAT(its_volume(p), WithinAbs(0.5 * full, 0.05 * full));
}

TEST_CASE("A loop following the model's edges runs along a ridge between the picked points", "[PathCut]")
{
    // A roof (z = |x| / 2, a ridge along x = 0) meshed on a 0.5 mm grid; two points picked 1.5 mm off the
    // ridge near its ends: between them the loop follows the ridge, not the straight line (which it does
    // with no pull towards the edges).
    constexpr int n = 41;
    const double  step = 0.5;
    indexed_triangle_set roof;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const double x = -10. + step * i, y = step * j;
            roof.vertices.emplace_back(float(x), float(y), float(0.5 * std::abs(x)));
        }
    auto at = [](int i, int j) { return j * n + i; };
    for (int j = 0; j + 1 < n; ++j)
        for (int i = 0; i + 1 < n; ++i) {
            roof.indices.emplace_back(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            roof.indices.emplace_back(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }
    const PathCut::CreaseGraph     graph(roof);
    const std::vector<PathCut::V3> picked{{1.5, 1., 0.75}, {1.5, 19., 0.75}, {-9., 10., 4.5}};
    auto count_on_ridge = [&](double pull, size_t &between) {
        size_t on_ridge = 0;
        between         = 0;
        for (const PathCut::V3 &p : graph.loop(picked, pull, 0.5))
            if (p.y() > 4. && p.y() < 16. && p.x() > -1.) {
                ++between;
                if (std::abs(p.x()) < 0.6) ++on_ridge;
            }
        return on_ridge;
    };
    size_t between = 0;
    const size_t on_ridge = count_on_ridge(0.85, between);
    REQUIRE(between > 10);
    CHECK(on_ridge == between);
    CHECK(count_on_ridge(0., between) == 0);
}

TEST_CASE("A cut along a loop of the mesh's own edges splits exactly there, the bore open in both pieces", "[PathCut]")
{
    // A pipe (outer r = 8, bore r = 4, h = 30) meshed in 1 mm rings: the ring of the outside wall at
    // z = 15 is cut exactly along its edges; the bore walls crossing the cut face are cut too.
    const double  R = 8., r = 4., h = 30.;
    constexpr int n = 48, rows = 31;
    indexed_triangle_set pipe;
    for (int wall = 0; wall < 2; ++wall) // outside, bore
        for (int k = 0; k < rows; ++k)
            for (int i = 0; i < n; ++i) {
                const double a = 2. * PI * i / n, rr = wall == 0 ? R : r;
                pipe.vertices.emplace_back(float(rr * std::cos(a)), float(rr * std::sin(a)), float(h * k / (rows - 1)));
            }
    auto v = [](int wall, int k, int i) { return (wall * rows + k) * n + (i + n) % n; };
    for (int k = 0; k + 1 < rows; ++k)
        for (int i = 0; i < n; ++i) {
            pipe.indices.emplace_back(v(0, k, i), v(0, k, i + 1), v(0, k + 1, i + 1)); // outside, facing out
            pipe.indices.emplace_back(v(0, k, i), v(0, k + 1, i + 1), v(0, k + 1, i));
            pipe.indices.emplace_back(v(1, k, i), v(1, k + 1, i + 1), v(1, k, i + 1)); // bore, facing the axis
            pipe.indices.emplace_back(v(1, k, i), v(1, k + 1, i), v(1, k + 1, i + 1));
        }
    for (int i = 0; i < n; ++i) {
        pipe.indices.emplace_back(v(0, rows - 1, i), v(0, rows - 1, i + 1), v(1, rows - 1, i + 1)); // top ring
        pipe.indices.emplace_back(v(0, rows - 1, i), v(1, rows - 1, i + 1), v(1, rows - 1, i));
        pipe.indices.emplace_back(v(0, 0, i), v(1, 0, i + 1), v(0, 0, i + 1)); // bottom ring
        pipe.indices.emplace_back(v(0, 0, i), v(1, 0, i), v(1, 0, i + 1));
    }
    const double full = its_volume(pipe);
    REQUIRE(full > 0.);

    std::vector<int> ring;
    for (int i = 0; i < n; ++i) ring.push_back(v(0, 15, i));
    const PathCut::CutResult res = PathCut::cut_along_edge_loop(pipe, ring, 0.2);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    REQUIRE(parts.size() == 2);
    require_closed_parts(parts);
    for (const auto &p : parts) {
        CHECK_THAT(its_volume(p), WithinAbs(0.5 * full, 0.03 * full));
        // The cut runs exactly along the ring: the outside of each piece ends at z = 15.
        bool on_ring = false;
        for (const auto &q : p.vertices)
            on_ring = on_ring || (std::abs(q.z() - 15.f) < 1e-4f && std::abs(std::hypot(q.x(), q.y()) - R) < 1e-3);
        CHECK(on_ring);
    }

    // The same ring found from a few points picked on it, following the edges.
    const PathCut::CreaseGraph     graph(pipe);
    const std::vector<PathCut::V3> picked{{R, 0., 15.}, {-0.5 * R, 0.866 * R, 15.}, {-0.5 * R, -0.866 * R, 15.}};
    const std::vector<int>         found = graph.loop_vertices(picked);
    REQUIRE(found.size() >= 3);
    for (int id : found)
        CHECK_THAT(double(pipe.vertices[size_t(id)].z()), WithinAbs(15., 1e-4));
}

TEST_CASE("The preview draws the detached piece apart from the rest", "[PathCut]")
{
    // A 10 mm cube resting on top of a 20 mm one: drawn 3 mm (the minimum) further up.
    indexed_triangle_set stay  = its_make_cube(20., 20., 20.);
    indexed_triangle_set moved = its_make_cube(10., 10., 10.);
    for (Vec3f &v : moved.vertices) v += Vec3f(5.f, 5.f, 20.f);
    const PathCut::V3 shift = PathCut::preview_shift(moved, stay);
    CHECK_THAT(shift.norm(), WithinAbs(6., 1e-9));
    CHECK(shift.z() > 0.);
    CHECK_THAT(shift.x(), WithinAbs(0., 1e-9));
    // A large detached piece goes 10 % of its size, at most 30 mm; an empty piece is not moved.
    indexed_triangle_set big = its_make_cube(400., 400., 400.);
    for (Vec3f &v : big.vertices) v += Vec3f(0.f, 0.f, 20.f);
    CHECK_THAT(PathCut::preview_shift(big, stay).norm(), WithinAbs(30., 1e-9));
    CHECK(PathCut::preview_shift(indexed_triangle_set{}, stay).norm() == 0.);
}

TEST_CASE("A tilted loop around a cylinder detaches its end", "[PathCut]")
{
    // Cylinder r = 5, h = 30, loop around it tilted between z = 13 and z = 17.
    const double               r = 5., kerf = 0.2;
    const indexed_triangle_set cyl = its_make_cylinder(r, 30.);
    std::vector<PathCut::V3>   loop;
    for (int i = 0; i < 12; ++i) {
        const double a = 2. * PI * i / 12.;
        loop.emplace_back(r * std::cos(a), r * std::sin(a), 15. + 2. * std::cos(a));
    }
    PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, kerf, 1.5, [](const PathCut::V3 &p) { return p; });
    REQUIRE(lc.valid());
    REQUIRE(PathCut::fit_loop_margin(cyl, lc));

    const PathCut::CutResult res = PathCut::cut_with_slab(cyl, lc, true);
    REQUIRE(res.error.empty());
    const auto parts = its_split(res.mesh);
    REQUIRE(parts.size() == 2);
    require_closed_parts(parts);

    const PathCut::Grouped g = PathCut::group_components(parts, [&lc](const PathCut::V3 &p) { return lc.side(p); }, true);
    REQUIRE(g.n_a == 1);
    REQUIRE(g.n_b == 1);
    // Each side holds about half the cylinder.
    const double half = PI * r * r * 15.;
    CHECK_THAT(its_volume(g.side_a), WithinAbs(half, 0.1 * half));
    CHECK_THAT(its_volume(g.side_b), WithinAbs(half, 0.1 * half));
}

TEST_CASE("A loop that misses part of the cross-section is reported instead of cutting halfway", "[PathCut]")
{
    // Loop around one of two boxes standing side by side, with a cap so wide it reaches the other
    // box: the cap rim runs through it.
    indexed_triangle_set u = its_make_cube(4., 4., 20.);
    indexed_triangle_set leg2 = its_make_cube(4., 4., 20.);
    its_translate(leg2, Vec3f(6.f, 0.f, 0.f));
    its_merge(u, leg2);
    std::vector<PathCut::V3> loop;
    for (int i = 0; i < 8; ++i) {
        const double a = 2. * PI * i / 8.;
        loop.emplace_back(2. + 2.9 * std::cos(a), 2. + 2.9 * std::sin(a), 10.);
    }
    PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 5., [](const PathCut::V3 &p) { return p; });
    REQUIRE(lc.valid());
    const PathCut::CutResult res = PathCut::cut_with_slab(u, lc, true);
    CHECK_FALSE(res.error.empty());
}

TEST_CASE("A loop that folds back on itself is rejected", "[PathCut]")
{
    // Figure-eight seen from its mean plane.
    std::vector<PathCut::V3> loop;
    for (int i = 0; i < 16; ++i) {
        const double t = 2. * PI * i / 16.;
        loop.emplace_back(10. * std::sin(t), 5. * std::sin(2. * t), 0.);
    }
    const PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, 0.2, 1.5, [](const PathCut::V3 &p) { return p; });
    CHECK_FALSE(lc.valid());
    CHECK_FALSE(lc.error.empty());
}

TEST_CASE("Two cubes touching along an edge are separated without opening either", "[PathCut]")
{
    indexed_triangle_set a = its_make_cube(10., 10., 10.), b = its_make_cube(10., 10., 10.);
    its_translate(b, Vec3f(10.f, 0.f, 10.f));
    its_merge(a, b);
    its_merge_vertices(a); // the edge x = 10, z = 10 is now shared by four faces
    size_t open, max_faces;
    edge_counts(a, open, max_faces);
    REQUIRE(max_faces == 4);

    CHECK(PathCut::make_edge_manifold(a) > 0);
    edge_counts(a, open, max_faces);
    CHECK(open == 0);
    CHECK(max_faces == 2);
}

TEST_CASE("Zero-thickness sheets glued to a mesh are removed before cutting", "[PathCut]")
{
    indexed_triangle_set its = its_make_cube(10., 10., 10.);
    // The same triangle twice with opposite winding makes edges shared by four faces.
    const auto f = its.indices.front();
    its.indices.emplace_back(f[0], f[1], f[2]);
    its.indices.emplace_back(f[0], f[2], f[1]);
    size_t open, max_faces;
    edge_counts(its, open, max_faces);
    REQUIRE(max_faces > 2);

    CHECK(PathCut::make_edge_manifold(its) > 0);
    edge_counts(its, open, max_faces);
    CHECK(max_faces == 2);
}

TEST_CASE("Connectors on a wavy cut face fit inside both parts", "[PathCut]")
{
    // 30 x 30 x 40 box cut by a wavy path around z = 20, seen from the front.
    const indexed_triangle_set box = its_make_cube(30., 30., 40.);
    const std::vector<PathCut::V3> pts{{-2., 0., 18.}, {10., 0., 22.}, {20., 0., 17.}, {32., 0., 21.}};
    const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3(30., 30., 40.), 0.2);
    REQUIRE(vc.valid());
    const PathCut::CutResult res = PathCut::cut_with_slab(box, vc, false);
    REQUIRE(res.error.empty());
    bool joined = true;
    const PathCut::Grouped g = PathCut::group_cut(res, [&vc](const PathCut::V3 &p) { return vc.side(p); }, false, &joined);
    REQUIRE_FALSE(joined);

    // The cut face looks up on the lower part: the pegs go there, pointing up.
    const int                   level = PathCut::peg_side(res, g.side_a, g.side_b);
    const indexed_triangle_set &from  = level == 1 ? g.side_a : g.side_b;
    const indexed_triangle_set &to    = level == 1 ? g.side_b : g.side_a;

    PathCut::ConnectorParams prm;
    prm.type  = PathCut::ConnectorType::Plug;
    prm.count = 3;
    std::string                    error;
    const PathCut::ConnectorResult c = PathCut::place_connectors(res, level, from, to, prm, error);
    REQUIRE(error.empty());
    REQUIRE(c.positions.size() == 3);
    CHECK(c.axis.z() > 0.9);
    require_closed_parts({c.pegs, c.holes_to});
    for (const PathCut::V3 &p : c.positions) {
        CHECK(PathCut::point_inside(from, p - 1. * c.axis));      // peg base sunk into the lower part
        CHECK(PathCut::point_inside(to, p + prm.depth * c.axis)); // peg tip inside the upper part
    }
    for (size_t i = 0; i < c.positions.size(); ++i)
        for (size_t j = i + 1; j < c.positions.size(); ++j)
            CHECK((c.positions[i] - c.positions[j]).norm() > prm.size + 2. * (prm.clearance + prm.wall));

    SECTION("separate pins get holes on both sides")
    {
        prm.type                        = PathCut::ConnectorType::Dowel;
        const PathCut::ConnectorResult d = PathCut::place_connectors(res, level, from, to, prm, error);
        REQUIRE_FALSE(d.positions.empty());
        CHECK(d.pegs.indices.empty());
        require_closed_parts({d.holes_from, d.holes_to, d.pin});
        CHECK_THAT(its_volume(d.pin), WithinAbs(PI * 2.5 * 2.5 * 10., 2.));
    }
    SECTION("connectors too big for the part are reported")
    {
        prm.size  = 40.;
        const PathCut::ConnectorResult big = PathCut::place_connectors(res, level, from, to, prm, error);
        CHECK(big.positions.empty());
        CHECK_FALSE(error.empty());
    }
}

TEST_CASE("A flat loop cap cuts along the mean plane of the loop", "[PathCut]")
{
    // Wavy loop around a cylinder: with a flat cap both cut faces are planes a kerf apart.
    const double               r = 5., kerf = 0.2;
    const indexed_triangle_set cyl = its_make_cylinder(r, 30.);
    std::vector<PathCut::V3>   loop;
    for (int i = 0; i < 12; ++i) {
        const double a = 2. * PI * i / 12.;
        loop.emplace_back(r * std::cos(a), r * std::sin(a), 15. + 1.5 * std::cos(2. * a));
    }
    PathCut::LoopCutter lc = PathCut::make_loop_cutter(loop, kerf, 1.5, [](const PathCut::V3 &p) { return p; }, true);
    REQUIRE(lc.valid());
    REQUIRE(PathCut::fit_loop_margin(cyl, lc));
    const PathCut::CutResult res = PathCut::cut_with_slab(cyl, lc, true);
    REQUIRE(res.error.empty());
    require_closed_parts(its_split(res.mesh));
    for (size_t i = res.first_cap; i < res.first_cap + res.cap_triangles; ++i)
        for (int k = 0; k < 3; ++k) {
            const PathCut::V3 p = res.mesh.vertices[size_t(res.mesh.indices[i][k])].cast<double>();
            CHECK_THAT(std::abs(lc.frame.height(p) - lc.hmean), WithinAbs(0.5 * kerf, 1e-3));
        }
}

TEST_CASE("Curls from hand jitter are cut out of a view path; a path doubling back is refused", "[PathCut]")
{
    const indexed_triangle_set cube = its_make_cube(20., 20., 20.);
    SECTION("two points picked almost on top of each other, the second slightly behind")
    {
        const std::vector<PathCut::V3> pts{{-2., 0., 10.}, {6., 0., 10.5}, {9., 0., 10.}, {8.5, 0., 10.3}, {10., 0., 9.8}, {14., 0., 9.5}, {22., 0., 10.}};
        const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3::Constant(20.), 0.2);
        REQUIRE(vc.valid());
        size_t i, j;
        PathCut::V2 x;
        CHECK_FALSE(PathCut::first_self_crossing(vc.path, i, j, x));
        const PathCut::CutResult res = PathCut::cut_with_slab(cube, vc, false);
        REQUIRE(res.error.empty());
        require_closed_parts(its_split(res.mesh));
    }
    SECTION("points picked back and forth")
    {
        const std::vector<PathCut::V3> pts{{-2., 0., 10.}, {15., 0., 10.}, {5., 0., 12.}, {22., 0., 8.}};
        CHECK_FALSE(PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3::Constant(20.), 0.2).valid());
    }
}

namespace {

// 12 x 12 x 40 bar cut by a wavy path around z = 20 seen from the front; the lower part gets the pegs.
struct BarCut
{
    PathCut::CutResult res;
    PathCut::Grouped   g;
    int                level{0};
    const indexed_triangle_set &from() const { return level == 1 ? g.side_a : g.side_b; }
    const indexed_triangle_set &to() const { return level == 1 ? g.side_b : g.side_a; }
    // The centroid of the cut face triangle of `from` closest to (x, y) across the bar.
    PathCut::V3 face_point(const PathCut::ConnectorPlacer &pl, double x, double y) const
    {
        PathCut::V3 best = PathCut::V3::Zero();
        double      bd   = 1e9;
        for (const auto &t : pl.face().indices) {
            const PathCut::V3 c = (pl.face().vertices[t[0]].cast<double>() + pl.face().vertices[t[1]].cast<double>() +
                                   pl.face().vertices[t[2]].cast<double>()) / 3.;
            const double d = std::hypot(c.x() - x, c.y() - y);
            if (d < bd) { bd = d; best = c; }
        }
        return best;
    }
};

BarCut cut_bar()
{
    BarCut b;
    const indexed_triangle_set     bar = its_make_cube(12., 12., 40.);
    const std::vector<PathCut::V3> pts{{-2., 0., 19.}, {6., 0., 21.}, {14., 0., 19.5}};
    const PathCut::ViewCutter vc = PathCut::make_view_cutter(pts, PathCut::V3::UnitY(), PathCut::V3::Zero(), PathCut::V3(12., 12., 40.), 0.2);
    REQUIRE(vc.valid());
    b.res = PathCut::cut_with_slab(bar, vc, false);
    REQUIRE(b.res.error.empty());
    bool joined = true;
    b.g         = PathCut::group_cut(b.res, [&vc](const PathCut::V3 &p) { return vc.side(p); }, false, &joined);
    REQUIRE_FALSE(joined);
    b.level = PathCut::peg_side(b.res, b.g.side_a, b.g.side_b);
    return b;
}

} // namespace

TEST_CASE("A square connector is checked by its own outline, not the circle around its corners", "[PathCut]")
{
    // A 7.2 mm square needs 3.6 + clearance + wall = 4.8 mm of room each way on a 12 mm wide face: it fits.
    // The circle through its corners plus the wall (6.4 mm) would not.
    const BarCut             b = cut_bar();
    PathCut::ConnectorParams prm;
    prm.type  = PathCut::ConnectorType::Plug;
    prm.shape = PathCut::ConnectorShape::Square;
    prm.count = 1;
    prm.size  = 7.2;
    std::string                    error;
    const PathCut::ConnectorResult c = PathCut::place_connectors(b.res, b.level, b.from(), b.to(), prm, error);
    CHECK(error.empty());
    REQUIRE(c.positions.size() == 1);
    require_closed_parts({c.pegs, c.holes_to});

    prm.size = 10.5; // 5.25 + 1.2 = 6.45 > 6: does not fit
    const PathCut::ConnectorResult big = PathCut::place_connectors(b.res, b.level, b.from(), b.to(), prm, error);
    CHECK(big.positions.empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Connectors placed by hand are checked spot by spot", "[PathCut]")
{
    const BarCut             b = cut_bar();
    PathCut::ConnectorParams prm;
    prm.type = PathCut::ConnectorType::Plug;
    prm.size = 4.;
    const PathCut::ConnectorPlacer pl(b.res, b.level, b.from(), b.to(), prm);
    REQUIRE(pl.error().empty());

    std::vector<PathCut::ConnectorSpot> placed;
    PathCut::ConnectorSpot              spot;
    CHECK(pl.try_at(b.face_point(pl, 6., 6.), spot, placed) == PathCut::ConnectorFit::Ok);
    placed.push_back(spot);
    CHECK(pl.try_at(b.face_point(pl, 6.5, 6.), spot, placed) == PathCut::ConnectorFit::Overlap);
    CHECK(pl.try_at(b.face_point(pl, 0.5, 6.), spot, {}) == PathCut::ConnectorFit::Edge);

    const PathCut::ConnectorResult c = pl.build(placed);
    REQUIRE(c.positions.size() == 1);
    require_closed_parts({c.pegs, c.holes_to});
}

TEST_CASE("A connector that does not fit is still drawn where it was tried", "[PathCut]")
{
    // While placing by hand, the connector under the mouse is shown in red where it does not fit: the
    // spot is only its position then, and it must still give a solid at that place.
    const BarCut             b = cut_bar();
    PathCut::ConnectorParams prm;
    prm.type = PathCut::ConnectorType::Plug;
    prm.size = 4.;
    const PathCut::ConnectorPlacer pl(b.res, b.level, b.from(), b.to(), prm);
    REQUIRE(pl.error().empty());

    const PathCut::V3      edge = b.face_point(pl, 0.5, 6.);
    PathCut::ConnectorSpot spot;
    REQUIRE(pl.try_at(edge, spot) == PathCut::ConnectorFit::Edge);
    PathCut::ConnectorSpot shown;
    shown.p = edge;
    const PathCut::ConnectorResult c = pl.build({shown});
    REQUIRE(c.positions.size() == 1);
    CHECK_FALSE(c.pegs.indices.empty());
    CHECK_THAT(c.positions.front().x(), WithinAbs(edge.x(), 1e-9));
}

TEST_CASE("Connectors tilted by hand stay parallel at the tilt asked for, and can be placed anyway", "[PathCut]")
{
    const BarCut             b = cut_bar();
    PathCut::ConnectorParams prm;
    prm.type = PathCut::ConnectorType::Plug;
    prm.size = 4.;
    const PathCut::ConnectorPlacer straight(b.res, b.level, b.from(), b.to(), prm);
    prm.tilt = 20.;
    prm.turn = 90.;
    const PathCut::ConnectorPlacer tilted(b.res, b.level, b.from(), b.to(), prm);
    REQUIRE(straight.error().empty());
    REQUIRE(tilted.error().empty());
    const double angle = std::acos(std::clamp(straight.axis().dot(tilted.axis()), -1., 1.)) * 180. / PI;
    CHECK_THAT(angle, WithinAbs(20., 1e-6));
    double tilt = 0., turn = 0.; // and back, as the direction orb reads it
    PathCut::tilt_of(tilted.pull_axis(), tilted.axis(), tilt, turn);
    CHECK_THAT(tilt, WithinAbs(20., 1e-6));
    CHECK_THAT(turn, WithinAbs(90., 1e-6));

    // Near the edge it does not fit, but forced it is still built there.
    const PathCut::V3      edge = b.face_point(tilted, 0.5, 6.);
    PathCut::ConnectorSpot spot;
    REQUIRE(tilted.try_at(edge, spot) != PathCut::ConnectorFit::Ok);
    const PathCut::ConnectorSpot forced = tilted.forced_at(edge);
    CHECK(forced.forced);
    const PathCut::ConnectorResult c = tilted.build({forced});
    REQUIRE(c.positions.size() == 1);
    CHECK_FALSE(c.pegs.indices.empty());
    CHECK_FALSE(c.holes_to.indices.empty());
}

TEST_CASE("New connectors keep clear of holes already in the part", "[PathCut]")
{
    // A 2 x 2 mm negative volume crossing the cut face near a corner.
    const BarCut         b    = cut_bar();
    indexed_triangle_set hole = its_make_cube(2., 2., 10.);
    for (auto &v : hole.vertices) v += Vec3f(3.f, 3.f, 15.f);
    PathCut::ConnectorParams prm;
    prm.type  = PathCut::ConnectorType::Plug;
    prm.size  = 4.;
    prm.count = 1;
    const PathCut::ConnectorPlacer pl(b.res, b.level, b.from(), b.to(), prm, &hole);
    PathCut::ConnectorSpot         spot;
    CHECK(pl.try_at(b.face_point(pl, 4., 4.), spot) == PathCut::ConnectorFit::Hole);

    std::string                    error;
    const PathCut::ConnectorResult c = PathCut::place_connectors(b.res, b.level, b.from(), b.to(), prm, error, {}, &hole);
    REQUIRE(c.positions.size() == 1);
    const PathCut::V3 &p = c.positions.front();
    // The hole plus its clearance and wall (2 + 0.2 + 1) stays clear of the 2 x 2 hole.
    const double dx = std::max({0., 3. - p.x(), p.x() - 5.}), dy = std::max({0., 3. - p.y(), p.y() - 5.});
    CHECK(std::hypot(dx, dy) > 3.2);
}

TEST_CASE("Connectors follow the direction the pieces come apart in, not the mean normal", "[PathCut]")
{
    // A U-shaped cut face: bottom facing -y, walls facing -x and +x, the left wall three times as long.
    // The mean normal leans towards it; the pieces only come apart straight along -y.
    std::vector<PathCut::V3> faces;
    for (int i = 0; i < 4; ++i) faces.emplace_back(0., -1., 0.);  // bottom, area 4
    for (int i = 0; i < 12; ++i) faces.emplace_back(-1., 0., 0.); // left wall, area 12
    for (int i = 0; i < 4; ++i) faces.emplace_back(1., 0., 0.);   // right wall, area 4
    double worst = -2.;
    const PathCut::V3 d = PathCut::pull_direction(faces, &worst);
    CHECK(d.y() < -0.999); // within a couple of degrees of the walls
    CHECK_THAT(worst, WithinAbs(0., 0.025));

    // A flat face: its normal. A dome: about its axis, all of it facing that way.
    CHECK_THAT(PathCut::pull_direction({PathCut::V3(0., 0., 2.), PathCut::V3(0., 0., 1.)}).z(), WithinAbs(1., 1e-9));
    std::vector<PathCut::V3> dome;
    for (int i = 0; i < 24; ++i)
        for (int j = 1; j <= 4; ++j) {
            const double a = 2. * PI * i / 24., t = 0.3 * j;
            dome.emplace_back(std::sin(t) * std::cos(a), std::sin(t) * std::sin(a), std::cos(t));
        }
    const PathCut::V3 dd = PathCut::pull_direction(dome, &worst);
    CHECK(dd.z() > 0.999);
    CHECK(worst > 0.3);
}
