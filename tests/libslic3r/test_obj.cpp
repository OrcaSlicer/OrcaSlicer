#include <array>
#include <catch2/catch_all.hpp>

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/catch_test_macros.hpp>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"

#include <boost/nowide/fstream.hpp>
#include <string>
#include "libslic3r/Point.hpp"
#include <cstddef>

#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

struct LoadedObj
{
    bool         ok{false};
    TriangleMesh mesh;
    ObjInfo      info;
};

// Loads an OBJ made of the given lines, with a material library that defines material "a".
LoadedObj load_textured_obj(const std::string &body)
{
    ScopedTemporaryFile obj(".obj");
    ScopedTemporaryFile mtl(".mtl");
    {
        boost::nowide::ofstream out(mtl.string());
        out << "newmtl a\nKd 1 0 0\n";
    }
    {
        boost::nowide::ofstream out(obj.string());
        out << "mtllib " << mtl.path().filename().string() << "\n" << body;
    }
    LoadedObj   loaded;
    std::string message;
    loaded.ok = load_obj(obj.string().c_str(), &loaded.mesh, loaded.info, message);
    return loaded;
}

// A tetrahedron with a material and two texture coordinates, (0.25, 0.5) and (0.75, 1).
// Only the first face and the vt lines are varied; the other three faces reference vt 1.
LoadedObj load_textured_tetrahedron(const std::string &first_face, const std::string &vts = "vt 0.25 0.5\nvt 0.75 1\n")
{
    return load_textured_obj("v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n" + vts + "usemtl a\n" + first_face + "\n" +
                             "f 1/1 2/1 4/1\nf 1/1 4/1 3/1\nf 2/1 3/1 4/1\n");
}

// Texture coordinate n is (n / 10, n / 20), so a UV identifies the vt it came from.
void check_uv_is_vt(const Vec2f &uv, int vt)
{
    CHECK_THAT(uv.x(), WithinAbs(vt / 10., 1e-6));
    CHECK_THAT(uv.y(), WithinAbs(vt / 20., 1e-6));
}

} // namespace

TEST_CASE("An out-of-range texture index falls back to a zero UV and keeps the geometry", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1/1000000000 3/1 2/1");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0., 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0., 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(0.5, 1e-6));
}

TEST_CASE("A face without texture indices loads among faces that have them", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1 3 2");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    // One UV entry per face, so later faces keep their own coordinates.
    REQUIRE(loaded.info.uvs.size() == 4);
    for (const Vec2f &uv : loaded.info.uvs.front()) {
        CHECK_THAT(uv.x(), WithinAbs(0., 1e-6));
        CHECK_THAT(uv.y(), WithinAbs(0., 1e-6));
    }
    CHECK_THAT(loaded.info.uvs[1][0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(loaded.info.uvs[1][0].y(), WithinAbs(0.5, 1e-6));
}

TEST_CASE("A negative texture index counts back from the last texture coordinate", "[OBJ]")
{
    // -1 is the most recent vt (0.75, 1), -2 the one before it (0.25, 0.5).
    const LoadedObj loaded = load_textured_tetrahedron("f 1/-2 3/-1 2/-1");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(1., 1e-6));
}

TEST_CASE("Texture coordinates with a w component are kept", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_tetrahedron("f 1/1 3/2 2/2", "vt 0.25 0.5 0\nvt 0.75 1 0\n");

    REQUIRE(loaded.ok);
    CHECK(loaded.mesh.facets_count() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(1., 1e-6));
}

TEST_CASE("A texture coordinate with w does not shift the indices of the ones after it", "[OBJ][Regression]")
{
    // The w on the first vt used to drop that line, so vt 2 resolved to the third coordinate.
    const LoadedObj loaded = load_textured_tetrahedron("f 1/2 3/3 2/-1", "vt 0.1 0.2 0\nvt 0.25 0.5\nvt 0.75 1\n");

    REQUIRE(loaded.ok);
    REQUIRE(loaded.info.uvs.size() == 4);
    const std::array<Vec2f, 3> &uv = loaded.info.uvs.front();
    CHECK_THAT(uv[0].x(), WithinAbs(0.25, 1e-6));
    CHECK_THAT(uv[0].y(), WithinAbs(0.5, 1e-6));
    CHECK_THAT(uv[1].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[1].y(), WithinAbs(1., 1e-6));
    CHECK_THAT(uv[2].x(), WithinAbs(0.75, 1e-6));
    CHECK_THAT(uv[2].y(), WithinAbs(1., 1e-6));
}

TEST_CASE("Both triangles of a quad take the texture coordinates of their own corners", "[OBJ][Regression]")
{
    const LoadedObj loaded = load_textured_obj("v 0 0 0\nv 10 0 0\nv 10 10 0\nv 0 10 0\n"
                                               "vt 0.1 0.05\nvt 0.2 0.1\nvt 0.3 0.15\nvt 0.4 0.2\n"
                                               "usemtl a\n"
                                               "f 1/1 2/2 3/3 4/4\n");

    REQUIRE(loaded.ok);
    REQUIRE(loaded.mesh.facets_count() == 2);
    REQUIRE(loaded.info.uvs.size() == 2);
    check_uv_is_vt(loaded.info.uvs[0][0], 1);
    check_uv_is_vt(loaded.info.uvs[0][1], 2);
    check_uv_is_vt(loaded.info.uvs[0][2], 3);
    check_uv_is_vt(loaded.info.uvs[1][0], 1);
    check_uv_is_vt(loaded.info.uvs[1][1], 3);
    check_uv_is_vt(loaded.info.uvs[1][2], 4);
}

TEST_CASE("Texture coordinates follow the corners of a mesh that is flipped on load", "[OBJ][Regression]")
{
    // The faces wind inwards, so the loader flips them. Vertex n uses vt n.
    const LoadedObj loaded = load_textured_obj("v 0 0 0\nv 10 0 0\nv 0 10 0\nv 0 0 10\n"
                                               "vt 0.1 0.05\nvt 0.2 0.1\nvt 0.3 0.15\nvt 0.4 0.2\n"
                                               "usemtl a\n"
                                               "f 1/1 2/2 3/3\nf 1/1 4/4 2/2\nf 1/1 3/3 4/4\nf 2/2 4/4 3/3\n");

    REQUIRE(loaded.ok);
    const indexed_triangle_set &its = loaded.mesh.its;
    CHECK(its_volume(its) > 0.f);
    REQUIRE(its.indices.size() == 4);
    REQUIRE(loaded.info.uvs.size() == 4);
    for (size_t face = 0; face < its.indices.size(); ++face)
        for (int corner = 0; corner < 3; ++corner)
            check_uv_is_vt(loaded.info.uvs[face][corner], its.indices[face][corner] + 1);
}
