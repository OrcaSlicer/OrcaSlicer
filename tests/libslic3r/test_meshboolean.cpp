#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Point.hpp>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

using namespace Slic3r;

TEST_CASE("CGAL and TriangleMesh conversions", "[MeshBoolean]") {
    TriangleMesh sphere = make_sphere(1.);
    
    auto cgalmesh_ptr = MeshBoolean::cgal::triangle_mesh_to_cgal(sphere);
    
    REQUIRE(cgalmesh_ptr);
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(*cgalmesh_ptr));
    
    TriangleMesh M = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgalmesh_ptr);
    
    REQUIRE(M.its.vertices.size() == sphere.its.vertices.size());
    REQUIRE(M.its.indices.size() == sphere.its.indices.size());
    
    REQUIRE(M.volume() == Catch::Approx(sphere.volume()));
    
    REQUIRE(! MeshBoolean::cgal::does_self_intersect(M));
}

TEST_CASE("CGAL volume checks ignore unreferenced vertices", "[MeshBoolean][Regression]") {
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    // Leave isolated vertices before, between, and after the vertices used by faces.
    mesh.vertices.insert(mesh.vertices.begin(), Vec3f(-1.f, -1.f, -1.f));
    mesh.vertices.insert(mesh.vertices.begin() + 4, Vec3f(5.f, 5.f, 5.f));
    mesh.vertices.emplace_back(20.f, 20.f, 20.f);
    for (Vec3i32 &face : mesh.indices)
        for (int i = 0; i < 3; ++i)
            face[i] += face[i] >= 3 ? 2 : 1;

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    REQUIRE(cgal_mesh);
    REQUIRE(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
    REQUIRE_FALSE(MeshBoolean::cgal::does_self_intersect(*cgal_mesh));

    TriangleMesh result = MeshBoolean::cgal::cgal_to_triangle_mesh(*cgal_mesh);
    REQUIRE(result.its.indices.size() == mesh.indices.size());
    for (const Vec3i32 &face : result.its.indices)
        for (int i = 0; i < 3; ++i) {
            REQUIRE(face[i] >= 0);
            REQUIRE(static_cast<std::size_t>(face[i]) < result.its.vertices.size());
        }
    REQUIRE_THAT(result.volume(), Catch::Matchers::WithinRel(1000., 1e-6));
}

TEST_CASE("CGAL volume checks reject open meshes with unreferenced vertices", "[MeshBoolean][Regression]") {
    indexed_triangle_set mesh = its_make_cube(10., 10., 10.);
    mesh.indices.pop_back();
    mesh.vertices.emplace_back(20.f, 20.f, 20.f);

    auto cgal_mesh = MeshBoolean::cgal::triangle_mesh_to_cgal(mesh);
    REQUIRE(cgal_mesh);
    REQUIRE_FALSE(MeshBoolean::cgal::does_bound_a_volume(*cgal_mesh));
}

TEST_CASE("mcut difference handles source splits between cuts", "[MeshBoolean]") {
    TriangleMesh body = make_cube(30., 10., 10.);

    TriangleMesh tool;

    // First cut splits the source into two disconnected components.
    TriangleMesh slab = make_cube(2., 12., 20.);
    slab.translate(Vec3f(14.f, -1.f, -5.f));
    its_merge(tool.its, slab.its);

    // These cuts must still be applied after the source has been split.
    TriangleMesh left_hole = make_cube(4., 4., 20.);
    left_hole.translate(Vec3f(3.f, 3.f, -5.f));
    its_merge(tool.its, left_hole.its);

    TriangleMesh right_hole = make_cube(4., 4., 20.);
    right_hole.translate(Vec3f(21.f, 3.f, -5.f));
    its_merge(tool.its, right_hole.its);

    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(body, tool, result, "A_NOT_B");

    REQUIRE(result.size() == 1);

    const std::vector<indexed_triangle_set> components =
        its_split(result.front().its);

    REQUIRE(components.size() == 2);

    // 3000 - 200 - 160 - 160 = 2480.
    REQUIRE_THAT(
        result.front().volume(),
        Catch::Matchers::WithinRel(2480., 1e-3)
    );
}

namespace {

TriangleMesh overlapping_cube()
{
    TriangleMesh cube = make_cube(10., 10., 10.);
    cube.translate(Vec3f(5.f, 5.f, 5.f));
    return cube;
}

} // namespace

TEST_CASE("mcut boolean returns nothing when cancelled before the first dispatch", "[MeshBoolean]") {
    const TriangleMesh body = make_cube(10., 10., 10.);
    const TriangleMesh tool = overlapping_cube();

    for (const char *op : {"UNION", "A_NOT_B", "INTERSECTION"}) {
        std::vector<TriangleMesh> result;
        MeshBoolean::mcut::make_boolean(body, tool, result, op, [] { return true; });
        INFO(op);
        REQUIRE(result.empty());
    }
}

TEST_CASE("mcut boolean reports increasing progress that ends at 100", "[MeshBoolean]") {
    TriangleMesh body = make_cube(10., 10., 10.);
    TriangleMesh second = make_cube(10., 10., 10.);
    second.translate(Vec3f(30.f, 0.f, 0.f));
    its_merge(body.its, second.its);

    TriangleMesh tool = make_cube(50., 2., 20.);
    tool.translate(Vec3f(-5.f, 4.f, -5.f));

    for (const char *op : {"UNION", "A_NOT_B", "INTERSECTION"}) {
        std::vector<float> reports;
        std::vector<TriangleMesh> result;
        MeshBoolean::mcut::make_boolean(body, tool, result, op, {}, [&reports](float p) { reports.push_back(p); });
        INFO(op);
        REQUIRE_FALSE(result.empty());
        REQUIRE_FALSE(reports.empty());
        for (size_t i = 1; i < reports.size(); ++i)
            REQUIRE(reports[i] >= reports[i - 1]);
        REQUIRE(reports.front() >= 0.f);
        REQUIRE_THAT(reports.back(), Catch::Matchers::WithinAbs(100., 1e-3));
    }
}

TEST_CASE("mcut boolean rejects an unknown operation", "[MeshBoolean]") {
    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(make_cube(10., 10., 10.), overlapping_cube(), result, "XOR");
    REQUIRE(result.empty());

    auto src = MeshBoolean::mcut::triangle_mesh_to_mcut(make_cube(10., 10., 10.).its);
    auto cut = MeshBoolean::mcut::triangle_mesh_to_mcut(overlapping_cube().its);
    REQUIRE_FALSE(MeshBoolean::mcut::do_boolean(*src, *cut, "XOR"));
}

namespace {

TriangleMesh cube_at(double size, const Vec3f &origin)
{
    TriangleMesh cube = make_cube(size, size, size);
    cube.translate(origin);
    return cube;
}

} // namespace

TEST_CASE("Union of overlapping meshes gives the volume they cover together", "[MeshBoolean]") {
    const std::vector<TriangleMesh> cubes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(5.f, 0.f, 0.f)), cube_at(10., Vec3f(10.f, 0.f, 0.f))};

    std::optional<TriangleMesh> result = MeshBoolean::ops::union_all(cubes);
    REQUIRE(result);
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(2000., 1e-3));
}

TEST_CASE("Intersection does not depend on the input order", "[MeshBoolean]") {
    std::vector<TriangleMesh> cubes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(5.f, 0.f, 0.f)), cube_at(10., Vec3f(2.f, 2.f, 2.f))};
    // [5, 10] x [2, 10] x [2, 10]
    const double expected = 5. * 8. * 8.;

    std::vector<size_t> order{0, 1, 2};
    do {
        std::vector<TriangleMesh> ordered;
        for (size_t i : order)
            ordered.push_back(cubes[i]);
        std::optional<TriangleMesh> result = MeshBoolean::ops::intersect_all(ordered);
        INFO("order " << order[0] << order[1] << order[2]);
        REQUIRE(result);
        REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(expected, 1e-3));
    } while (std::next_permutation(order.begin(), order.end()));
}

TEST_CASE("Intersection fails when one mesh is apart", "[MeshBoolean]") {
    const std::vector<TriangleMesh> cubes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(5.f, 0.f, 0.f)), cube_at(10., Vec3f(50.f, 0.f, 0.f))};
    REQUIRE_FALSE(MeshBoolean::ops::intersect_all(cubes));
}

TEST_CASE("Subtraction keeps every piece and one result per input", "[MeshBoolean]") {
    TriangleMesh bar = make_cube(30., 10., 10.);
    TriangleMesh slab = make_cube(2., 12., 20.);
    slab.translate(Vec3f(14.f, -1.f, -5.f));
    const TriangleMesh far = cube_at(10., Vec3f(100.f, 0.f, 0.f));

    std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_each({bar, far}, {slab});
    REQUIRE(results.size() == 2);
    REQUIRE(results[0]);
    REQUIRE(its_split(results[0]->its).size() == 2);
    REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(3000. - 200., 1e-3));
    // A cutter that misses leaves the input whole, in its own slot.
    REQUIRE(results[1]);
    REQUIRE_THAT(results[1]->volume(), Catch::Matchers::WithinRel(1000., 1e-3));
}

TEST_CASE("Subtraction by overlapping cutters removes the volume they cover together", "[MeshBoolean]") {
    const TriangleMesh bar = make_cube(30., 10., 10.);
    TriangleMesh hole_a = make_cube(4., 4., 20.);
    hole_a.translate(Vec3f(3.f, 3.f, -5.f));
    TriangleMesh hole_b = make_cube(4., 4., 20.);
    hole_b.translate(Vec3f(5.f, 3.f, -5.f));

    std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_union({bar}, {hole_a, hole_b});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0]);
    // The two holes overlap into one 6 x 4 hole through the 10 mm bar.
    REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(3000. - 240., 1e-3));
}

TEST_CASE("A mirrored input gives the same result as the original", "[MeshBoolean]") {
    const TriangleMesh other = cube_at(10., Vec3f(5.f, 0.f, 0.f));
    TriangleMesh mirrored = cube_at(10., Vec3f(-10.f, 0.f, 0.f));
    Transform3d mirror = Transform3d::Identity();
    mirror.scale(Vec3d(-1., 1., 1.));
    mirrored.transform(mirror);
    REQUIRE(its_volume(mirrored.its) < 0.f);
    MeshBoolean::ops::normalize_input(mirrored);

    std::optional<TriangleMesh> from_mirrored = MeshBoolean::ops::union_all({mirrored, other});
    std::optional<TriangleMesh> from_plain = MeshBoolean::ops::union_all({cube_at(10., Vec3f::Zero()), other});
    REQUIRE(from_mirrored);
    REQUIRE(from_plain);
    REQUIRE_THAT(from_mirrored->volume(), Catch::Matchers::WithinRel(double(from_plain->volume()), 1e-3));
}

namespace {

// A 20 mm cube with a 10 mm cavity in its middle: 7000 mm³.
TriangleMesh hollow_cube()
{
    TriangleMesh hollow = cube_at(20., Vec3f::Zero());
    TriangleMesh cavity = cube_at(10., Vec3f(5.f, 5.f, 5.f));
    cavity.flip_triangles();
    its_merge(hollow.its, cavity.its);
    hollow = TriangleMesh(std::move(hollow.its));
    MeshBoolean::ops::normalize_input(hollow);
    return hollow;
}

// A 2 x 2 peg along X from x = -5 to x = 10: through the wall at x = 0..5 and 5 mm into the cavity.
TriangleMesh peg_into_cavity()
{
    TriangleMesh peg = make_cube(15., 2., 2.);
    peg.translate(Vec3f(-5.f, 9.f, 9.f));
    return peg;
}

} // namespace

TEST_CASE("A hollow input keeps its cavity", "[MeshBoolean]") {
    const TriangleMesh hollow = hollow_cube();

    SECTION("when the cutter doesn't reach it") {
        // Cuts a 2 x 2 column out of one edge, nowhere near the cavity.
        TriangleMesh column = make_cube(4., 4., 30.);
        column.translate(Vec3f(-2.f, -2.f, -5.f));
        std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_each({hollow}, {column});
        REQUIRE(results.size() == 1);
        REQUIRE(results[0]);
        REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(7000. - 80., 1e-3));
    }
    SECTION("when a cutter reaches into it") {
        std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_each({hollow}, {peg_into_cavity()});
        REQUIRE(results.size() == 1);
        REQUIRE(results[0]);
        // Only the 5 mm of peg inside the wall removes material.
        REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(7000. - 20., 1e-3));
    }
    SECTION("when it is the cutter") {
        std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_each({cube_at(30., Vec3f(-5.f, -5.f, -5.f))}, {hollow});
        REQUIRE(results.size() == 1);
        REQUIRE(results[0]);
        // The material in the cavity stays.
        REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(27000. - 8000. + 1000., 1e-3));
    }
    SECTION("in an intersection with a mesh around it") {
        std::optional<TriangleMesh> result = MeshBoolean::ops::intersect_all({hollow, cube_at(30., Vec3f(-5.f, -5.f, -5.f))});
        REQUIRE(result);
        REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(7000., 1e-3));
    }
    SECTION("in a union with a peg reaching into it") {
        std::optional<TriangleMesh> result = MeshBoolean::ops::union_all({hollow, peg_into_cavity()});
        REQUIRE(result);
        // The peg adds its 5 mm outside the cube and its 5 mm in the cavity.
        REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(7000. + 20. + 20., 1e-3));
    }
    SECTION("in a union with a part lying in it") {
        std::optional<TriangleMesh> result = MeshBoolean::ops::union_all({hollow, cube_at(4., Vec3f(8.f, 8.f, 8.f))});
        REQUIRE(result);
        REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(7000. + 64., 1e-3));
    }
    SECTION("and a cut reaching into it can be cancelled while it runs") {
        // Not cancelled when the cut starts, cancelled by the time it is done.
        int        checks = 0;
        const auto cancel = [&checks] { return ++checks > 1; };
        REQUIRE(MeshBoolean::ops::subtract_each({hollow}, {peg_into_cavity()}, cancel).empty());
    }
    SECTION("in a union of its own shells") {
        // One mesh: the hollow cube and a 4 x 4 block through its wall, stopping just short of the cavity.
        TriangleMesh mesh  = hollow;
        TriangleMesh block = make_cube(6.98, 4., 4.);
        block.translate(Vec3f(-2.f, 8.f, 8.f));
        its_merge(mesh.its, block.its);
        mesh = TriangleMesh(std::move(mesh.its));
        std::optional<TriangleMesh> result = MeshBoolean::ops::union_all({mesh});
        REQUIRE(result);
        // Only the 2 mm of block outside the cube adds material.
        REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(7000. + 32., 1e-3));
    }
    SECTION("and the operations reaching into it end their progress at 100") {
        std::vector<float> reported;
        const auto         progress = [&reported](float p) { reported.push_back(p); };
        REQUIRE(MeshBoolean::ops::union_all({hollow, peg_into_cavity()}, {}, progress));
        REQUIRE_FALSE(reported.empty());
        REQUIRE(std::is_sorted(reported.begin(), reported.end()));
        REQUIRE(reported.back() == 100.f);
    }
}

TEST_CASE("Cancelling stops every operation", "[MeshBoolean]") {
    const std::vector<TriangleMesh> cubes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(5.f, 0.f, 0.f))};
    const auto cancel = [] { return true; };

    REQUIRE_FALSE(MeshBoolean::ops::union_all(cubes, cancel));
    REQUIRE_FALSE(MeshBoolean::ops::intersect_all(cubes, cancel));
    REQUIRE(MeshBoolean::ops::subtract_each({cubes[0]}, {cubes[1]}, cancel).empty());
    REQUIRE(MeshBoolean::ops::subtract_union({cubes[0]}, {cubes[1]}, cancel).empty());
}

TEST_CASE("An object's solid is its parts minus its negative volumes", "[MeshBoolean]") {
    const std::vector<TriangleMesh> parts{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(5.f, 0.f, 0.f))};
    TriangleMesh hole = make_cube(4., 4., 20.);
    hole.translate(Vec3f(3.f, 3.f, -5.f));

    std::optional<TriangleMesh> with_hole = MeshBoolean::ops::solid(parts, {hole});
    REQUIRE(with_hole);
    REQUIRE_THAT(with_hole->volume(), Catch::Matchers::WithinRel(1500. - 160., 1e-3));

    std::optional<TriangleMesh> without = MeshBoolean::ops::solid(parts, {});
    REQUIRE(without);
    REQUIRE_THAT(without->volume(), Catch::Matchers::WithinRel(1500., 1e-3));

    REQUIRE_FALSE(MeshBoolean::ops::solid({}, {hole}));
}

TEST_CASE("mcut boolean stopped part way leaves the source mesh untouched", "[MeshBoolean]") {
    // Two source shells, so the cut takes two dispatches; cancel after the first.
    TriangleMesh body = cube_at(10., Vec3f::Zero());
    its_merge(body.its, cube_at(10., Vec3f(30.f, 0.f, 0.f)).its);
    TriangleMesh tool = make_cube(50., 2., 20.);
    tool.translate(Vec3f(-5.f, 4.f, -5.f));

    auto src = MeshBoolean::mcut::triangle_mesh_to_mcut(body.its);
    auto cut = MeshBoolean::mcut::triangle_mesh_to_mcut(tool.its);
    int  calls = 0;
    REQUIRE_FALSE(MeshBoolean::mcut::do_boolean(*src, *cut, "A_NOT_B", [&calls] { return ++calls > 1; }));
    REQUIRE(calls > 1);
    REQUIRE_THAT(MeshBoolean::mcut::mcut_to_triangle_mesh(*src).volume(), Catch::Matchers::WithinRel(2000., 1e-6));

    calls = 0;
    std::vector<TriangleMesh> result;
    MeshBoolean::mcut::make_boolean(body, tool, result, "A_NOT_B", [&calls] { return ++calls > 1; });
    REQUIRE(result.empty());
}

TEST_CASE("Union counts a mesh once when it joins only some of the others", "[MeshBoolean]") {
    // A and B are apart; C overlaps B only.
    const std::vector<TriangleMesh> meshes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(30.f, 0.f, 0.f)),
                                           cube_at(10., Vec3f(35.f, 0.f, 0.f))};
    std::optional<TriangleMesh> result = MeshBoolean::ops::union_all(meshes);
    REQUIRE(result);
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(1000. + 1500., 1e-3));
    REQUIRE(its_split(result->its).size() == 2);
}

TEST_CASE("Union joins meshes that are apart through a later bridge", "[MeshBoolean]") {
    TriangleMesh bridge = make_cube(30., 4., 4.);
    bridge.translate(Vec3f(0.f, 3.f, 3.f));
    const std::vector<TriangleMesh> meshes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(20.f, 0.f, 0.f)), bridge};
    std::optional<TriangleMesh> result = MeshBoolean::ops::union_all(meshes);
    REQUIRE(result);
    // The bridge adds its 10 mm middle stretch.
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(2000. + 160., 1e-3));
    REQUIRE(its_split(result->its).size() == 1);
}

TEST_CASE("Union absorbs a mesh lying inside another", "[MeshBoolean]") {
    const std::vector<TriangleMesh> meshes{cube_at(20., Vec3f::Zero()), cube_at(5., Vec3f(5.f, 5.f, 5.f))};
    std::optional<TriangleMesh> result = MeshBoolean::ops::union_all(meshes);
    REQUIRE(result);
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(8000., 1e-3));
}

TEST_CASE("Subtraction cuts only the shells the cutter reaches", "[MeshBoolean]") {
    TriangleMesh two = cube_at(10., Vec3f::Zero());
    its_merge(two.its, cube_at(10., Vec3f(30.f, 0.f, 0.f)).its);
    TriangleMesh hole = make_cube(4., 4., 20.);
    hole.translate(Vec3f(3.f, 3.f, -5.f));

    std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_each({TriangleMesh(std::move(two.its))}, {hole});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0]);
    REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(2000. - 160., 1e-3));
}

TEST_CASE("Subtraction drops a shell the cutter swallows whole", "[MeshBoolean]") {
    TriangleMesh two = cube_at(10., Vec3f::Zero());
    its_merge(two.its, cube_at(4., Vec3f(33.f, 3.f, 3.f)).its);
    const TriangleMesh cutter = cube_at(10., Vec3f(30.f, 0.f, 0.f));

    std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_union({TriangleMesh(std::move(two.its))}, {cutter});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0]);
    REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(1000., 1e-3));

    // A mesh wholly inside the cutter leaves nothing.
    results = MeshBoolean::ops::subtract_each({cube_at(4., Vec3f(33.f, 3.f, 3.f))}, {cutter});
    REQUIRE(results.size() == 1);
    REQUIRE_FALSE(results[0]);
}

TEST_CASE("Intersection keeps a mesh lying inside the other", "[MeshBoolean]") {
    const std::vector<TriangleMesh> meshes{cube_at(20., Vec3f::Zero()), cube_at(5., Vec3f(5.f, 5.f, 5.f))};
    std::optional<TriangleMesh> result = MeshBoolean::ops::intersect_all(meshes);
    REQUIRE(result);
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(125., 1e-3));
}

TEST_CASE("Intersection fails for meshes whose boxes overlap but not their volumes", "[MeshBoolean]") {
    // An L in the XY plane and a cube sitting in its notch.
    TriangleMesh l_shape = make_cube(20., 10., 10.);
    its_merge(l_shape.its, cube_at(10., Vec3f(0.f, 10.f, 0.f)).its);
    std::optional<TriangleMesh> l_solid = MeshBoolean::ops::union_all({TriangleMesh(std::move(l_shape.its))});
    REQUIRE(l_solid);
    const TriangleMesh notch = cube_at(8., Vec3f(11.f, 11.f, 1.f));

    REQUIRE_FALSE(MeshBoolean::ops::intersect_all({*l_solid, notch}));
}

TEST_CASE("An object's solid counts overlapping parts of two instances once each", "[MeshBoolean]") {
    // Two instances 50 mm apart, each with two overlapping parts.
    std::vector<TriangleMesh> parts;
    for (float x : {0.f, 50.f}) {
        parts.push_back(cube_at(10., Vec3f(x, 0.f, 0.f)));
        parts.push_back(cube_at(10., Vec3f(x + 5.f, 0.f, 0.f)));
    }
    std::optional<TriangleMesh> result = MeshBoolean::ops::solid(parts, {});
    REQUIRE(result);
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(2. * 1500., 1e-3));
    REQUIRE(its_split(result->its).size() == 2);
}

TEST_CASE("Boolean operations report increasing progress that ends at 100", "[MeshBoolean]") {
    const std::vector<TriangleMesh> cubes{cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(5.f, 0.f, 0.f)), cube_at(10., Vec3f(2.f, 2.f, 2.f))};
    const auto check = [](const std::vector<float> &reports) {
        REQUIRE_FALSE(reports.empty());
        for (size_t i = 1; i < reports.size(); ++i)
            REQUIRE(reports[i] >= reports[i - 1]);
        REQUIRE_THAT(reports.back(), Catch::Matchers::WithinAbs(100., 1e-3));
    };
    std::vector<float> reports;
    const auto         record = [&reports](float p) { reports.push_back(p); };

    REQUIRE(MeshBoolean::ops::union_all(cubes, {}, record));
    check(reports);
    reports.clear();
    REQUIRE(MeshBoolean::ops::intersect_all(cubes, {}, record));
    check(reports);
    reports.clear();
    REQUIRE_FALSE(MeshBoolean::ops::subtract_each({cubes[0]}, {cubes[1], cubes[2]}, {}, record).empty());
    check(reports);
    reports.clear();
    REQUIRE_FALSE(MeshBoolean::ops::subtract_union({cubes[0]}, {cubes[1], cubes[2]}, {}, record).empty());
    check(reports);
    reports.clear();
    REQUIRE(MeshBoolean::ops::solid({cubes[0], cubes[1]}, {cubes[2]}, {}, record));
    check(reports);
}

TEST_CASE("Subtraction cancelled after its last cut returns nothing", "[MeshBoolean]") {
    const TriangleMesh a = cube_at(10., Vec3f::Zero());
    const TriangleMesh b = cube_at(10., Vec3f(5.f, 0.f, 0.f));
    // Lets the cut run, then reports cancelled from the next check on.
    bool       done   = false;
    const auto cancel = [&done] { return done; };
    const auto finish = [&done](float p) { if (p >= 100.f) done = true; };

    REQUIRE(MeshBoolean::ops::subtract_each({a}, {b}, cancel, finish).empty());
    done = false;
    REQUIRE(MeshBoolean::ops::subtract_union({a}, {b}, cancel, finish).empty());
}

TEST_CASE("Union keeps a part that starts inside another and sticks out of it", "[MeshBoolean]") {
    // The peg's first vertex is inside the body, but it sticks 1 mm out, so it must not be absorbed. mcut joins these
    // two, so this covers the joined path; a dispatch mcut refuses is not covered.
    const TriangleMesh body = cube_at(20., Vec3f::Zero());
    const TriangleMesh peg  = cube_at(4., Vec3f(17.f, 8.f, 8.f));

    std::optional<TriangleMesh> result = MeshBoolean::ops::union_all({body, peg});
    REQUIRE(result);
    REQUIRE_THAT(result->volume(), Catch::Matchers::WithinRel(8000. + 16., 1e-3));
}

TEST_CASE("Subtraction keeps the end of a part that sticks out of the cutter", "[MeshBoolean]") {
    // The part's first vertex is inside the cutter, but it sticks 1 mm out, so it must not be dropped. mcut cuts these
    // two, so this covers the cut path; a dispatch mcut refuses is not covered.
    const TriangleMesh part   = cube_at(4., Vec3f(17.f, 8.f, 8.f));
    const TriangleMesh cutter = cube_at(20., Vec3f::Zero());

    std::vector<std::optional<TriangleMesh>> results = MeshBoolean::ops::subtract_each({part}, {cutter});
    REQUIRE(results.size() == 1);
    REQUIRE(results[0]);
    REQUIRE_THAT(results[0]->volume(), Catch::Matchers::WithinRel(16., 1e-3));
}

TEST_CASE("Intersection of meshes that only nearly touch is empty, not failed", "[MeshBoolean]") {
    // 0.02 mm apart: their boxes overlap, their volumes don't.
    bool failed = true;
    REQUIRE_FALSE(MeshBoolean::ops::intersect_all({cube_at(10., Vec3f::Zero()), cube_at(10., Vec3f(10.02f, 0.f, 0.f))}, {}, {}, &failed));
    REQUIRE_FALSE(failed);
}

