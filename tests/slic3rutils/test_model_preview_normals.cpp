#include <catch2/catch_test_macros.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewNormals.hpp"
#include <cmath>

using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {
indexed_triangle_set hinged(float height)
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0,0,0), Vec3f(1,0,0), Vec3f(0,1,0), Vec3f(1,1,height)};
    mesh.indices = {Vec3i32(0,1,2), Vec3i32(2,1,3)};
    return mesh;
}
}

TEST_CASE("Preview normals smooth a shallow crease on both incident faces", "[ModelPreviewNormals]")
{
    auto mesh = hinged(.2f);
    const auto normals = ModelPreviewNormals::corner_normals(mesh);
    REQUIRE(normals.size() == 6);
    CHECK((normals[1] - normals[4]).norm() < 1e-5f);
    CHECK((normals[2] - normals[3]).norm() < 1e-5f);
    CHECK(normals[0].z() > .99f);
    CHECK(normals[1].z() < 1.f);
    CHECK(normals[1].z() > .98f);
}

TEST_CASE("Preview normals keep a sharp fold and inverted thin sheet separate", "[ModelPreviewNormals]")
{
    auto mesh = hinged(2.f);
    const auto sharp = ModelPreviewNormals::corner_normals(mesh);
    CHECK((sharp[1] - sharp[4]).norm() > .5f);
    mesh.indices[1] = Vec3i32(1,2,3);
    const auto inverted = ModelPreviewNormals::corner_normals(mesh);
    CHECK(inverted[1].z() > .99f);
    CHECK(inverted[3].z() < -.25f);
}

TEST_CASE("Preview normals do not cross a nonmanifold edge or vertex-only contact", "[ModelPreviewNormals]")
{
    auto mesh = hinged(.2f);
    mesh.vertices.push_back(Vec3f(1,1,-.2f));
    mesh.indices.emplace_back(1,2,4);
    const auto nonmanifold = ModelPreviewNormals::corner_normals(mesh);
    CHECK((nonmanifold[1] - nonmanifold[4]).norm() > .1f);

    mesh.indices.pop_back();
    mesh.vertices.emplace_back(2,0,0);
    mesh.vertices.emplace_back(1,-1,0);
    mesh.indices[1] = Vec3i32(1,5,6);
    const auto vertex_only = ModelPreviewNormals::corner_normals(mesh);
    CHECK(vertex_only[1].z() > .99f);
    CHECK(vertex_only[3].z() < -.9f);
}
