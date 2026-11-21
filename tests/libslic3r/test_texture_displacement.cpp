#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <memory>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <boost/filesystem/path.hpp>
#include <boost/filesystem/operations.hpp>
#include <catch2/catch_test_macros.hpp>
#include <ios>
#include <iterator>
#include <utility>
#include "libslic3r/Point.hpp"
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <map>
#include <math.h>
#include "libslic3r/TextureBake/TextureBakeDisplace.hpp"
#include "libslic3r/TextureBake/TextureBakeIndex.hpp"
#include <tuple>
#include <Eigen/Geometry>
#include "libslic3r/libslic3r.h"
#include <catch2/catch_message.hpp>
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <boost/filesystem.hpp>

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TextureBake/TextureBakeDecimate.hpp"
#include "libslic3r/TextureBake/TextureBakeFlip.hpp"
#include "libslic3r/TextureBake/TextureBakeMesh.hpp"
#include "libslic3r/TextureBake/TextureBakeRepair.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/PNGReadWrite.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

// Encodes a flat (uniform-value) grayscale image through Slic3r's own PNG writer/reader round
// trip, so decode_height_texture() (which only accepts true 8-bit grayscale PNG) is guaranteed a
// compatible file, exactly like the GUI's "Add texture" import path does.
static std::shared_ptr<std::vector<unsigned char>> make_flat_gray_png(uint8_t value, size_t w = 4, size_t h = 4)
{
    std::vector<uint8_t> pixels(w * h, value);
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), w, h, pixels));

    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);

    REQUIRE_FALSE(bytes.empty());
    return std::make_shared<std::vector<unsigned char>>(std::move(bytes));
}

// The same round trip for a flat colour image, which decode_height_texture() reads through its colour path.
static std::shared_ptr<std::vector<unsigned char>> make_flat_rgb_png(uint8_t r, uint8_t g, uint8_t b, size_t w = 4, size_t h = 4)
{
    std::vector<uint8_t> rgb;
    for (size_t i = 0; i < w * h; ++i)
        rgb.insert(rgb.end(), { r, g, b });
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_rgb_to_file(tmp_path.string(), w, h, rgb));

    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);

    REQUIRE_FALSE(bytes.empty());
    return std::make_shared<std::vector<unsigned char>>(std::move(bytes));
}

// A hard-edged black/white checkerboard, the worst case for a height map: every texel boundary is a
// step, which is precisely the relief the post-process smoothing exists to round off.
static std::shared_ptr<std::vector<unsigned char>> make_checkerboard_png(size_t w = 16, size_t h = 16)
{
    std::vector<uint8_t> pixels(w * h);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x)
            pixels[y * w + x] = ((x / 2 + y / 2) % 2) ? 255 : 0;
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), w, h, pixels));

    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);

    REQUIRE_FALSE(bytes.empty());
    return std::make_shared<std::vector<unsigned char>>(std::move(bytes));
}

// The bake never drives relief below the model's own resting plane (see build_texture_displacement()),
// so on a fully painted closed solid the vertices already sitting on that plane - a cube's four bottom
// corners, whose normals point downwards - are clamped in Z and do not move by the full depth. The
// tests below are about the displacement maths, so they check the vertices the clamp cannot touch;
// the clamp itself has its own test.

// The tests below check the classic, topology-preserving bake vertex by vertex, so they select it
// explicitly: the default pipeline rebuilds the topology and has no vertex correspondence to check.
static TextureDisplacementOptions classic_options()
{
    TextureDisplacementOptions o;
    o.pipeline_v2 = false;
    return o;
}

static bool above_resting_plane(const indexed_triangle_set &mesh, size_t vi)
{
    float bottom = std::numeric_limits<float>::max();
    for (const Vec3f &v : mesh.vertices)
        bottom = std::min(bottom, v.z());
    return mesh.vertices[vi].z() > bottom + 1e-4f;
}

TEST_CASE("TextureDisplacement: decode_height_texture round-trips an 8-bit grayscale PNG", "[TextureDisplacement]")
{
    TextureDisplacementLayer layer;
    layer.image_data = make_flat_gray_png(128, 4, 4);

    DecodedHeightTexture tex = decode_height_texture(layer);
    REQUIRE_FALSE(tex.empty());
    CHECK(tex.width == 4);
    CHECK(tex.height == 4);
    REQUIRE_THAT(tex.sample(Vec2f(0.5f, 0.5f)), WithinAbs(128.0 / 255.0, 1.0 / 255.0));
}

TEST_CASE("TextureDisplacement: an empty layer list leaves the mesh unchanged", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const std::vector<TextureDisplacementLayer> layers; // none
    TextureDisplacementFacetsData facets{};              // all empty

    const indexed_triangle_set result = build_texture_displacement(cube, layers, facets, classic_options());

    REQUIRE(result.vertices.size() == cube.vertices.size());
    REQUIRE(result.indices.size() == cube.indices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i)
        for (int c = 0; c < 3; ++c)
            CHECK(result.vertices[i](c) == cube.vertices[i](c));
}

TEST_CASE("TextureDisplacement: fully painting a mesh displaces every vertex along its own normal", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const TriangleMesh cube_mesh(cube);

    TriangleSelector selector(cube_mesh);
    for (int f = 0; f < int(cube.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType::ENFORCER);

    TextureDisplacementFacetsData facets{};
    facets[0] = selector.serialize();

    TextureDisplacementLayer layer;
    layer.slot        = 0;
    layer.depth_mm    = 2.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data  = make_flat_gray_png(255); // sample() == 1.0 everywhere -> full depth_mm displacement

    const indexed_triangle_set result = build_texture_displacement(cube, {layer}, facets, classic_options());

    REQUIRE(result.vertices.size() == cube.vertices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i) {
        if (!above_resting_plane(cube, i))
            continue;
        const float moved = (result.vertices[i] - cube.vertices[i]).norm();
        CHECK_THAT(moved, WithinAbs(layer.depth_mm, 1e-3f));
    }
}

// Paints every facet of `mesh` into a serialized mask, the way "Select whole model" does.
static TriangleSelector::TriangleSplittingData paint_whole_mesh(const indexed_triangle_set &mesh)
{
    const TriangleMesh tm(mesh);
    TriangleSelector   selector(tm);
    for (int f = 0; f < int(mesh.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType::ENFORCER);
    return selector.serialize();
}

// Paints a disc of the top face of `mesh` (the plane z = `top`) the way the gizmo's brush does: the
// selector splits the triangles under it, so the stroke need not follow the mesh.
static TriangleSelector::TriangleSplittingData paint_top_disc(const indexed_triangle_set &mesh, float top,
                                                              const Vec2f &centre, float radius)
{
    const TriangleMesh tm(mesh);
    const Vec3f        at(centre.x(), centre.y(), top);
    int                start = -1;
    Vec3d              foot;
    AABBMesh(tm).squared_distance(at.cast<double>(), start, foot); // the brush starts on the face under it
    TriangleSelector selector(tm);
    selector.select_patch(start,
                          TriangleSelector::SinglePointCursor::cursor_factory(
                              at, at + Vec3f(0.f, 0.f, 80.f), radius, TriangleSelector::CursorType::SPHERE,
                              Transform3d::Identity(), TriangleSelector::ClippingPlane()),
                          EnforcerBlockerType::ENFORCER, Transform3d::Identity(), /* triangle_splitting */ true);
    return selector.serialize();
}

// Regression test for the bug this feature shipped with: with two layers painted over the same
// area, the second one was silently dropped (its paint mask was remapped onto the mesh the first
// layer had already displaced, which routinely produced an empty bitstream). Every layer is now
// evaluated against the original mesh instead, so both must show up in the total.
TEST_CASE("TextureDisplacement: a second layer over the same area is applied too", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);
    facets[1] = facets[0]; // both layers cover the whole cube

    TextureDisplacementLayer base;
    base.slot         = 0;
    base.depth_mm     = 1.0f;
    base.tiling_scale = 5.0f;
    base.image_data   = make_flat_gray_png(255); // height 1.0 everywhere

    TextureDisplacementLayer second = base;
    second.slot       = 1;
    second.depth_mm   = 0.5f;
    second.blend_mode = TextureBlendMode::Add;

    const indexed_triangle_set result = build_texture_displacement(cube, {base, second}, facets, classic_options());

    // Topology is preserved exactly, so vertices can be compared 1:1 with the input.
    REQUIRE(result.vertices.size() == cube.vertices.size());
    REQUIRE(result.indices.size() == cube.indices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i)
        if (above_resting_plane(cube, i))
            CHECK_THAT((result.vertices[i] - cube.vertices[i]).norm(), WithinAbs(1.5f, 1e-3f)); // 1.0 + 0.5, not just 1.0
}

TEST_CASE("TextureDisplacement: blend modes combine a layer with the ones below it", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);
    facets[1] = facets[0];

    TextureDisplacementLayer base;
    base.slot         = 0;
    base.depth_mm     = 2.0f;
    base.tiling_scale = 5.0f;
    base.image_data   = make_flat_gray_png(255); // -> contributes exactly +2.0 mm

    TextureDisplacementLayer second = base;
    second.slot     = 1;
    second.depth_mm = 0.5f; // -> its own value is 0.5 mm

    // Expected total displacement for each mode, given base = 2.0 mm and second = 0.5 mm. Multiply
    // and Divide treat the layer's value as a factor relative to 1 mm (see TextureBlendMode).
    const auto expected = GENERATE(table<TextureBlendMode, float>({
        { TextureBlendMode::Add,      2.5f },  // 2.0 + 0.5
        { TextureBlendMode::Subtract, 1.5f },  // 2.0 - 0.5
        { TextureBlendMode::Multiply, 1.0f },  // 2.0 * 0.5
        { TextureBlendMode::Divide,   4.0f },  // 2.0 / 0.5
    }));
    second.blend_mode = std::get<0>(expected);

    const indexed_triangle_set result = build_texture_displacement(cube, {base, second}, facets, classic_options());

    REQUIRE(result.vertices.size() == cube.vertices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i)
        if (above_resting_plane(cube, i))
            CHECK_THAT((result.vertices[i] - cube.vertices[i]).norm(), WithinAbs(std::get<1>(expected), 1e-3f));
}

TEST_CASE("TextureDisplacement: the lowest layer ignores its blend mode", "[TextureDisplacement]")
{
    // Multiply against the implicit zero base would annihilate the only layer present; the first
    // layer to reach a vertex always starts the total off additively instead.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 2.0f;
    layer.tiling_scale = 5.0f;
    layer.blend_mode   = TextureBlendMode::Multiply;
    layer.image_data   = make_flat_gray_png(255);

    const indexed_triangle_set result = build_texture_displacement(cube, {layer}, facets, classic_options());

    for (size_t i = 0; i < cube.vertices.size(); ++i)
        if (above_resting_plane(cube, i))
            CHECK_THAT((result.vertices[i] - cube.vertices[i]).norm(), WithinAbs(2.0f, 1e-3f));
}

TEST_CASE("TextureDisplacement: relief is never driven below the model's resting plane", "[TextureDisplacement]")
{
    // A fully painted cube displaces outward everywhere, which on the bottom face means straight
    // down - through the build plate. That geometry cannot be printed, so it is clamped back up.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 2.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data   = make_flat_gray_png(255); // full depth everywhere

    float bottom = std::numeric_limits<float>::max();
    for (const Vec3f &v : cube.vertices)
        bottom = std::min(bottom, v.z());

    const indexed_triangle_set result = build_texture_displacement(cube, {layer}, facets, classic_options());

    REQUIRE(result.vertices.size() == cube.vertices.size());
    for (const Vec3f &v : result.vertices)
        CHECK(v.z() >= bottom - 1e-4f);

    // ...and the clamp is confined to Z: a bottom corner still moves outwards in X and Y by the same
    // amount it would have, rather than being pinned wholesale.
    bool any_bottom_moved_sideways = false;
    for (size_t i = 0; i < cube.vertices.size(); ++i)
        if (!above_resting_plane(cube, i) &&
            (result.vertices[i].head<2>() - cube.vertices[i].head<2>()).norm() > 1e-3f)
            any_bottom_moved_sideways = true;
    CHECK(any_bottom_moved_sideways);
}

TEST_CASE("TextureDisplacement: depth is measured in world millimetres, not the volume's own", "[TextureDisplacement]")
{
    // The same painted patch, baked once untransformed and once through a 3x scale. "Depth (mm)" is
    // a millimetre on the printed part, so the *world* relief must come out the same height either
    // way - which means the vertices of the scaled volume move by a third as much in its own
    // coordinates. Baking both in volume space instead gave a 3x deeper relief on the scaled one.
    indexed_triangle_set fan;
    fan.vertices = { {0.f, 0.f, 1.f}, {1.f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {-1.f, 0.f, 1.f}, {0.f, -1.f, 1.f} };
    fan.indices  = { {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1} };

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(fan);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 1.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data   = make_flat_gray_png(255);

    const indexed_triangle_set plain  = build_texture_displacement(fan, {layer}, facets, classic_options());
    Transform3d scale3 = Transform3d::Identity();
    scale3.scale(Vec3d(3.0, 3.0, 3.0));
    const indexed_triangle_set scaled = build_texture_displacement(fan, {layer}, facets, classic_options(), {}, nullptr, scale3);

    REQUIRE(plain.vertices.size() == fan.vertices.size());
    REQUIRE(scaled.vertices.size() == fan.vertices.size());
    for (size_t i = 0; i < fan.vertices.size(); ++i) {
        CHECK_THAT(plain.vertices[i].z() - fan.vertices[i].z(), WithinAbs(1.0f, 1e-3f));
        // A third of the movement locally is the same movement once the 3x scale is applied.
        CHECK_THAT(scaled.vertices[i].z() - fan.vertices[i].z(), WithinAbs(1.0f / 3.0f, 1e-3f));
    }
}

TEST_CASE("TextureDisplacement: a mirrored placement still raises the relief outwards", "[TextureDisplacement]")
{
    // Mirroring reverses the winding, and every normal in the bake is derived from the winding - so
    // without correcting for it the whole relief is carved into the surface instead of raised off it.
    indexed_triangle_set fan;
    fan.vertices = { {0.f, 0.f, 1.f}, {1.f, 0.f, 1.f}, {0.f, 1.f, 1.f}, {-1.f, 0.f, 1.f}, {0.f, -1.f, 1.f} };
    fan.indices  = { {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1} };

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(fan);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 1.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data   = make_flat_gray_png(255);

    // Mirrored in X: the patch's outward direction in world space is still +Z, so in the volume's own
    // coordinates the vertices must still move +Z.
    Transform3d mirror_x = Transform3d::Identity();
    mirror_x.scale(Vec3d(-1.0, 1.0, 1.0));
    const indexed_triangle_set result = build_texture_displacement(fan, {layer}, facets, classic_options(), {}, nullptr, mirror_x);

    REQUIRE(result.vertices.size() == fan.vertices.size());
    for (size_t i = 0; i < fan.vertices.size(); ++i)
        CHECK_THAT(result.vertices[i].z() - fan.vertices[i].z(), WithinAbs(1.0f, 1e-3f));
}

TEST_CASE("TextureDisplacement: the patch border is displaced by default and pinned on request", "[TextureDisplacement]")
{
    // A small triangle fan around a central vertex O, with 4 outer points A/B/C/D forming 4
    // triangles T0..T3 in the XY plane. Only T0, T1, T2 are painted, T3 is left unpainted, so:
    //   O: touches all 4 triangles (incl. unpainted T3)      -> patch border
    //   A: touches T0 (painted) and T3 (unpainted)           -> patch border
    //   D: touches T2 (painted) and T3 (unpainted)           -> patch border
    //   B: touches only T0 and T1 (both painted)              -> interior
    //   C: touches only T1 and T2 (both painted)              -> interior
    indexed_triangle_set fan;
    fan.vertices = { {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {-1.f, 0.f, 0.f}, {0.f, -1.f, 0.f} };
    fan.indices  = { {0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1} };

    const TriangleMesh fan_mesh(fan);
    TriangleSelector    selector(fan_mesh);
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    selector.set_facet(1, EnforcerBlockerType::ENFORCER);
    selector.set_facet(2, EnforcerBlockerType::ENFORCER);
    // facet 3 (T3) is left at its default EnforcerBlockerType::NONE.

    TextureDisplacementFacetsData facets{};
    facets[0] = selector.serialize();

    TextureDisplacementLayer layer;
    layer.slot        = 0;
    layer.depth_mm    = 1.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data  = make_flat_gray_png(255);

    // The bake is topology-preserving, so it only ever moves fan's own vertices, in order.
    auto moved = [&](const indexed_triangle_set &result, size_t i) {
        return (result.vertices[i] - fan.vertices[i]).norm() > 1e-6f;
    };

    SECTION("by default the whole painted patch moves, border included")
    {
        const indexed_triangle_set result = build_texture_displacement(fan, {layer}, facets, classic_options());
        REQUIRE(result.vertices.size() == fan.vertices.size());
        for (size_t i = 0; i < fan.vertices.size(); ++i)
            CHECK(moved(result, i));
        // ... straight along the painted surface's own normal (+Z here), by the full depth. Nothing
        // has torn: the unpainted triangle T3 simply shares the moved vertices.
        for (size_t i = 0; i < fan.vertices.size(); ++i)
            CHECK_THAT(result.vertices[i].z() - fan.vertices[i].z(), WithinAbs(1.0f, 1e-3f));
        CHECK(result.indices == fan.indices);
    }

    SECTION("pinning the border holds exactly the vertices an unpainted triangle also uses")
    {
        TextureDisplacementOptions options = classic_options();
        options.displace_border = false;
        const indexed_triangle_set result = build_texture_displacement(fan, {layer}, facets, options);
        CHECK_FALSE(moved(result, 0)); // O: border
        CHECK_FALSE(moved(result, 1)); // A: border
        CHECK_FALSE(moved(result, 4)); // D: border
        CHECK(moved(result, 2));       // B: interior
        CHECK(moved(result, 3));       // C: interior
    }

}

TEST_CASE("TextureDisplacement: post-process smoothing relaxes only what moved", "[TextureDisplacement]")
{
    // A checkerboard height map on a fine grid gives a relief full of hard steps - exactly what the
    // smoothing pass is for. Only the central square is painted, so the patch has a real border.
    indexed_triangle_set plane;
    plane.vertices = { { 0.f, 0.f, 0.f }, { 20.f, 0.f, 0.f }, { 20.f, 20.f, 0.f }, { 0.f, 20.f, 0.f } };
    plane.indices  = { { 0, 1, 2 }, { 0, 2, 3 } };
    const indexed_triangle_set grid = subdivide_mesh_uniform(plane, 1.f, 6);
    REQUIRE(grid.indices.size() > 256);

    std::vector<uint8_t> painted(grid.indices.size(), 0);
    const TriangleMesh   grid_mesh(grid);
    TriangleSelector     selector(grid_mesh);
    for (int i = 0; i < int(grid.indices.size()); ++i) {
        const auto &t = grid.indices[i];
        float       cx = 0.f, cy = 0.f;
        for (int k = 0; k < 3; ++k) {
            cx += grid.vertices[t[k]].x() / 3.f;
            cy += grid.vertices[t[k]].y() / 3.f;
        }
        if (cx > 5.f && cx < 15.f && cy > 5.f && cy < 15.f) {
            selector.set_facet(i, EnforcerBlockerType::ENFORCER);
            painted[size_t(i)] = 1;
        }
    }
    REQUIRE(std::count(painted.begin(), painted.end(), uint8_t(1)) > 32);
    TextureDisplacementFacetsData facets{};
    facets[0] = selector.serialize();

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 2.0f;
    layer.tiling_scale = 6.0f;

    // The patch rim: vertices shared by a painted and an unpainted triangle - exactly the set
    // TextureDisplacementOptions::smooth_skip_border holds out of the relaxation.
    std::vector<uint8_t> rim(grid.vertices.size(), 0), inside(grid.vertices.size(), 0);
    for (size_t i = 0; i < grid.indices.size(); ++i)
        for (int k = 0; k < 3; ++k)
            (painted[i] ? inside : rim)[size_t(grid.indices[i][k])] = 1;
    size_t rim_count = 0;
    for (size_t v = 0; v < rim.size(); ++v) {
        rim[v] = (rim[v] && inside[v]) ? 1 : 0;
        rim_count += rim[v];
    }
    REQUIRE(rim_count > 8);

    TextureDisplacementOptions options = classic_options();
    options.smooth_enabled    = true;
    options.smooth_strength   = 0.5f;
    options.smooth_iterations = 4;

    SECTION("it rounds off the steps without touching the topology")
    {
        layer.image_data = make_checkerboard_png();

        // Dirichlet energy over the mesh's edges. Laplacian relaxation is gradient descent on exactly
        // this, so it is the quantity guaranteed to fall - unlike the min/max spread of z, which is
        // pinned by whichever vertices are held (the unpainted surface at zero, and by default the
        // patch rim as well) and so need not move at all.
        auto roughness = [](const indexed_triangle_set &its) {
            double e = 0.0;
            for (const auto &t : its.indices)
                for (int k = 0; k < 3; ++k) {
                    const double d = double(its.vertices[t[k]].z()) - double(its.vertices[t[(k + 1) % 3]].z());
                    e += d * d;
                }
            return e;
        };

        const indexed_triangle_set raw = build_texture_displacement(grid, { layer }, facets, classic_options());
        REQUIRE(roughness(raw) > 0.0); // the checkerboard really did produce relief to smooth

        const indexed_triangle_set smoothed = build_texture_displacement(grid, { layer }, facets, options);
        CHECK(roughness(smoothed) < roughness(raw));
        CHECK(smoothed.indices == raw.indices);               // ... without touching the topology
        CHECK(smoothed.vertices.size() == raw.vertices.size());
        for (size_t v = 0; v < rim.size(); ++v)               // ... and the held rim is bit-identical
            if (rim[v])
                CHECK_THAT(smoothed.vertices[v].z(), WithinAbs(raw.vertices[v].z(), 1e-6f));
    }

    SECTION("\"ignore outer ring\" decides whether the patch rim relaxes")
    {
        // A flat white texture makes this exact rather than statistical: every painted vertex is
        // displaced to precisely depth_mm, so a movable interior vertex sees nothing but neighbours at
        // its own height and cannot move, while every rim vertex has at least one neighbour outside the
        // paint pinned at zero and so must come down the moment it is allowed to.
        layer.image_data = make_flat_gray_png(255);
        const indexed_triangle_set raw = build_texture_displacement(grid, { layer }, facets, classic_options());

        options.smooth_skip_border      = true;
        const indexed_triangle_set kept = build_texture_displacement(grid, { layer }, facets, options);

        options.smooth_skip_border         = false;
        const indexed_triangle_set relaxed = build_texture_displacement(grid, { layer }, facets, options);

        // Asserted on z alone, not on the whole position: relaxation averages all three coordinates,
        // and while the tangential drift cancels by symmetry on a regular grid it does so only up to
        // floating-point summation order, which is not something to pin down across three platforms.
        // Height is what the option is about and it is exact - every neighbour of a movable vertex sits
        // at the same height, so its own height cannot move.
        for (size_t v = 0; v < rim.size(); ++v)
            if (rim[v]) {
                CHECK_THAT(kept.vertices[v].z(), WithinAbs(raw.vertices[v].z(), 1e-6f)); // held
                CHECK(relaxed.vertices[v].z() < raw.vertices[v].z());                    // melted down
            }
    }
}

TEST_CASE("TextureDisplacement: smooth_mesh_vertices holds everything outside its mask", "[TextureDisplacement]")
{
    // A single spike on a flat sheet: relaxing it must pull the spike down and leave every vertex
    // that is not flagged movable at exactly the coordinates it started at.
    indexed_triangle_set plane;
    plane.vertices = { { 0.f, 0.f, 0.f }, { 8.f, 0.f, 0.f }, { 8.f, 8.f, 0.f }, { 0.f, 8.f, 0.f } };
    plane.indices  = { { 0, 1, 2 }, { 0, 2, 3 } };
    indexed_triangle_set grid = subdivide_mesh_uniform(plane, 1.f, 4);

    // Raise one interior vertex, and let only it and its immediate neighbours move.
    size_t spike = 0;
    float  best  = std::numeric_limits<float>::max();
    for (size_t i = 0; i < grid.vertices.size(); ++i)
        if (const float d = (grid.vertices[i] - Vec3f(4.f, 4.f, 0.f)).norm(); d < best) {
            best  = d;
            spike = i;
        }
    grid.vertices[spike].z() = 5.f;

    std::vector<uint8_t> movable(grid.vertices.size(), 0);
    movable[spike] = 1;
    for (const auto &t : grid.indices)
        for (int e = 0; e < 3; ++e)
            if (size_t(t[e]) == spike)
                for (int k = 0; k < 3; ++k)
                    movable[size_t(t[k])] = 1;

    const indexed_triangle_set before = grid;
    smooth_mesh_vertices(grid, movable, 0.5f, 3);

    CHECK(grid.vertices[spike].z() < before.vertices[spike].z()); // the spike came down
    CHECK(grid.vertices[spike].z() > 0.f);                        // but was not flattened outright
    CHECK(grid.indices == before.indices);                        // topology untouched
    for (size_t i = 0; i < grid.vertices.size(); ++i)
        if (!movable[i])
            CHECK_THAT((grid.vertices[i] - before.vertices[i]).norm(), WithinAbs(0.f, 1e-9f));

    // Guard rails: each of these must leave the mesh byte-identical.
    for (const auto &noop : { std::make_pair(0.f, 3), std::make_pair(0.5f, 0) }) {
        indexed_triangle_set copy = before;
        smooth_mesh_vertices(copy, movable, noop.first, noop.second);
        CHECK(copy.vertices == before.vertices);
    }
    indexed_triangle_set copy = before;
    smooth_mesh_vertices(copy, std::vector<uint8_t>(3, 1), 0.5f, 3); // mis-sized mask
    CHECK(copy.vertices == before.vertices);
}

// Every undirected edge of a closed manifold mesh is shared by exactly two triangles. A T-junction
// (a hanging node where a refined region meets a coarse one) breaks that: the coarse side spans an
// edge that the fine side has replaced with two half-edges, so those three edges each show up an
// odd number of times. Counting edge uses is therefore an exact crack detector for a closed mesh.
static bool every_edge_used_twice(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> uses;
    for (const auto &t : its.indices)
        for (int e = 0; e < 3; ++e) {
            int a = t[e], b = t[(e + 1) % 3];
            if (a > b)
                std::swap(a, b);
            ++uses[{ a, b }];
        }
    for (const auto &[edge, n] : uses)
        if (n != 2)
            return false;
    return true;
}

TEST_CASE("TextureDisplacement: adaptive subdivision is conformal and region-restricted", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    REQUIRE(every_edge_used_twice(cube)); // sanity: the input really is a closed manifold

    auto longest_edge = [](const indexed_triangle_set &its, const stl_triangle_vertex_indices &t) {
        float m = 0.f;
        for (int e = 0; e < 3; ++e)
            m = std::max(m, (its.vertices[t[e]] - its.vertices[t[(e + 1) % 3]]).norm());
        return m;
    };

    SECTION("whole-mesh region refines everywhere and stays conformal")
    {
        std::vector<uint8_t> region(cube.indices.size(), 1);
        std::vector<int>     source;
        const indexed_triangle_set out = subdivide_mesh_adaptive(cube, region, 3.f, 100000, &source);

        CHECK(out.indices.size() > cube.indices.size()); // it actually refined
        CHECK(every_edge_used_twice(out));               // ... without opening a single crack

        // Refinement runs to completion, not for a fixed number of passes: with the whole mesh in the
        // region and budget to spare, *every* edge really does end up at or below the target. This is
        // the regression that matters - an earlier version quietly stopped a long way short, having
        // spent its pass budget grading the coarse surroundings.
        float worst = 0.f;
        for (const auto &t : out.indices)
            worst = std::max(worst, longest_edge(out, t));
        CHECK(worst <= 3.f);

        REQUIRE(source.size() == out.indices.size());
        for (int s : source)
            CHECK((s >= 0 && s < int(cube.indices.size()))); // every child names a real parent
    }

    SECTION("a partial region refines only there, and the boundary is still crack-free")
    {
        // Refine only the triangles whose centroid is in the upper (z > 5) half of the cube.
        std::vector<uint8_t> region(cube.indices.size(), 0);
        size_t               region_count = 0;
        for (size_t i = 0; i < cube.indices.size(); ++i) {
            const auto &t = cube.indices[i];
            const float cz = (cube.vertices[t[0]].z() + cube.vertices[t[1]].z() + cube.vertices[t[2]].z()) / 3.f;
            if (cz > 5.f) {
                region[i] = 1;
                ++region_count;
            }
        }
        REQUIRE(region_count > 0);

        std::vector<int>           source;
        const indexed_triangle_set out = subdivide_mesh_adaptive(cube, region, 2.f, 100000, &source);

        CHECK(out.indices.size() > cube.indices.size());
        CHECK(every_edge_used_twice(out)); // the refined/coarse seam has no T-junction

        // Inside the region the target is actually met - refinement is not cut short by a pass budget.
        // Outside it, only the graded transition band conformality requires is touched, so plenty of
        // the unpainted mesh is still coarser than the target: the region was not a suggestion.
        float max_in = 0.f, max_out = 0.f;
        for (size_t i = 0; i < out.indices.size(); ++i) {
            float &acc = region[source[i]] ? max_in : max_out;
            acc        = std::max(acc, longest_edge(out, out.indices[i]));
        }
        CHECK(max_in <= 2.f);
        CHECK(max_out > 2.f);

        std::vector<uint8_t>       all(cube.indices.size(), 1);
        const indexed_triangle_set whole = subdivide_mesh_adaptive(cube, all, 2.f, 100000);
        CHECK(out.indices.size() < whole.indices.size()); // ... and it cost less than doing the lot
    }

    SECTION("the triangle budget caps the result and still leaves a conformal mesh")
    {
        std::vector<uint8_t>       region(cube.indices.size(), 1);
        const indexed_triangle_set out = subdivide_mesh_adaptive(cube, region, 0.05f, /*max_triangles*/ 500);
        CHECK(out.indices.size() <= 500);
        CHECK(out.indices.size() > cube.indices.size()); // it spent the budget rather than giving up
        CHECK(every_edge_used_twice(out));               // stopping on the budget is not a crack
    }

    SECTION("an empty region is a no-op")
    {
        std::vector<uint8_t>       region(cube.indices.size(), 0);
        const indexed_triangle_set out = subdivide_mesh_adaptive(cube, region, 1.f, 100000, nullptr);
        CHECK(out.indices.size() == cube.indices.size());
        CHECK(out.vertices.size() == cube.vertices.size());
    }
}

TEST_CASE("TextureDisplacement: feature-adaptive subdivision follows curvature, not slope", "[TextureDisplacement]")
{
    // A flat sheet, tessellated into a regular grid to give the bisector something to work with.
    indexed_triangle_set plane;
    plane.vertices = { { 0.f, 0.f, 0.f }, { 1.f, 0.f, 0.f }, { 1.f, 1.f, 0.f }, { 0.f, 1.f, 0.f } };
    plane.indices  = { { 0, 1, 2 }, { 0, 2, 3 } };
    const indexed_triangle_set grid = subdivide_mesh_uniform(plane, 0.15f, 5); // ~uniform grid of small triangles
    REQUIRE(grid.indices.size() > 32);

    const std::vector<uint8_t> region(grid.indices.size(), 1);

    auto longest_edge = [](const indexed_triangle_set &its, const stl_triangle_vertex_indices &t) {
        float m = 0.f;
        for (int e = 0; e < 3; ++e)
            m = std::max(m, (its.vertices[t[e]] - its.vertices[t[(e + 1) % 3]]).norm());
        return m;
    };
    auto centroid_xy = [](const indexed_triangle_set &its, const stl_triangle_vertex_indices &t) {
        return Vec2f((its.vertices[t[0]].x() + its.vertices[t[1]].x() + its.vertices[t[2]].x()) / 3.f,
                     (its.vertices[t[0]].y() + its.vertices[t[1]].y() + its.vertices[t[2]].y()) / 3.f);
    };

    SECTION("a sharp hill refines densely at its center and leaves flat corners coarse")
    {
        // A tight Gaussian hill at the sheet's center: strong curvature near (0.5, 0.5), flat far away.
        HeightFieldSampler hill = [](const Vec3f &p, const Vec3f &) {
            const float r2 = (p.x() - 0.5f) * (p.x() - 0.5f) + (p.y() - 0.5f) * (p.y() - 0.5f);
            return 1.0f * std::exp(-r2 / 0.02f);
        };

        // Baseline max edge 0.3 is coarser than the grid's own edges, so the baseline adds nothing
        // here - this isolates the *curvature* contribution (the grid already meets the baseline).
        std::vector<int>           source;
        const indexed_triangle_set out =
            subdivide_mesh_adaptive(grid, region, /*max edge*/ 0.3f, 200000, &source, hill, /*tol*/ 0.02f,
                                    /*min_edge*/ 0.01f);

        CHECK(out.indices.size() > grid.indices.size()); // the hill forced real refinement

        // The largest triangle near the hill's center must be much smaller than the largest in a flat
        // corner - i.e. triangles went where the curvature is, not spread evenly.
        float near_max = 0.f, far_max = 0.f;
        for (const auto &t : out.indices) {
            const Vec2f c   = centroid_xy(out, t);
            const float r   = (c - Vec2f(0.5f, 0.5f)).norm();
            const float len = longest_edge(out, t);
            if (r < 0.1f)
                near_max = std::max(near_max, len);
            else if (r > 0.45f)
                far_max = std::max(far_max, len);
        }
        REQUIRE(near_max > 0.f);
        REQUIRE(far_max > 0.f);
        CHECK(near_max < far_max); // finer at the hill than on the flats
    }

    SECTION("a linear ramp has zero curvature and is left untouched")
    {
        // Height varies, but linearly - a flat triangle represents it exactly, so the chord error is
        // zero everywhere and nothing should be split. This is the case a gradient-based criterion
        // would wrongly over-refine.
        HeightFieldSampler ramp = [](const Vec3f &p, const Vec3f &) { return 2.0f * p.x(); };

        // Same coarse baseline (0.3) that the grid already meets, so any split would be curvature-
        // driven - and a ramp has none.
        const indexed_triangle_set out =
            subdivide_mesh_adaptive(grid, region, /*max edge*/ 0.3f, 200000, nullptr, ramp, /*tol*/ 0.02f,
                                    /*min_edge*/ 0.01f);

        CHECK(out.indices.size() == grid.indices.size()); // not one extra triangle
    }

    SECTION("the max-edge baseline still applies in feature mode")
    {
        // A height field that is flat everywhere the four sample points of a coarse triangle happen to
        // land, but not in between - the aliasing case where a chord test alone reports no error and
        // refinement stalls before it ever starts. The baseline is what stops that: it guarantees a
        // sampling density fine enough for the curvature test to see the texture at all.
        HeightFieldSampler flat = [](const Vec3f &, const Vec3f &) { return 0.f; };

        const indexed_triangle_set out =
            subdivide_mesh_adaptive(grid, region, /*max edge*/ 0.03f, 200000, nullptr, flat, /*tol*/ 0.02f,
                                    /*min_edge*/ 0.001f);

        CHECK(out.indices.size() > grid.indices.size());
        float worst = 0.f;
        for (const auto &t : out.indices)
            worst = std::max(worst, longest_edge(out, t));
        CHECK(worst <= 0.03f);
    }
}

// A 2x2 truecolour PNG: red, green / blue, white. Written out as bytes rather than encoded here
// because libslic3r only *writes* grayscale PNGs (png::write_gray_to_file) - which is also exactly
// why the colour path exists: the GUI importer stores colour images through wxImage instead.
static std::shared_ptr<std::vector<unsigned char>> make_rgb_png_2x2()
{
    static const unsigned char bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x02, 0x00, 0x00, 0x00, 0xfd, 0xd4, 0x9a, 0x73, 0x00, 0x00, 0x00,
        0x14, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xc0,
        0x00, 0xc2, 0x0c, 0xff, 0xff, 0xff, 0xff, 0x0f, 0x00, 0x1f, 0xee, 0x05,
        0xfb, 0x60, 0x6c, 0x70, 0xf2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e,
        0x44, 0xae, 0x42, 0x60, 0x82,
    };
    return std::make_shared<std::vector<unsigned char>>(std::begin(bytes), std::end(bytes));
}

// The same image with an opaque alpha channel, so four bytes per pixel instead of three.
static std::shared_ptr<std::vector<unsigned char>> make_rgba_png_2x2()
{
    static const unsigned char bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00,
        0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
        0x1f, 0x0c, 0x81, 0x34, 0x18, 0x00, 0x00, 0x49, 0xc8, 0x09, 0xf7, 0x03,
        0xd9, 0x64, 0xf1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82,
    };
    return std::make_shared<std::vector<unsigned char>>(std::begin(bytes), std::end(bytes));
}

TEST_CASE("TextureDisplacement: a colour texture decodes to both colour and height", "[TextureDisplacement]")
{
    TextureDisplacementLayer layer;
    layer.slot       = 0;
    // Row stride differs between the two, and both must come out the same way up.
    layer.image_data = GENERATE(make_rgb_png_2x2(), make_rgba_png_2x2());

    const DecodedHeightTexture tex = decode_height_texture(layer);
    REQUIRE_FALSE(tex.empty());
    REQUIRE(tex.has_color());
    REQUIRE(tex.width == 2);
    REQUIRE(tex.height == 2);
    REQUIRE(tex.rgb.size() == 2 * 2 * 3);

    // Row-major, top-to-bottom: red, green / blue, white.
    CHECK(tex.rgb[0] == 255);  CHECK(tex.rgb[1] == 0);    CHECK(tex.rgb[2] == 0);
    CHECK(tex.rgb[3] == 0);    CHECK(tex.rgb[4] == 255);  CHECK(tex.rgb[5] == 0);
    CHECK(tex.rgb[6] == 0);    CHECK(tex.rgb[7] == 0);    CHECK(tex.rgb[8] == 255);
    CHECK(tex.rgb[9] == 255);  CHECK(tex.rgb[10] == 255); CHECK(tex.rgb[11] == 255);

    // Height is the luminance, with wxImage::ConvertToGreyscale()'s coefficients - which is what
    // makes a texture displace identically whether it was imported before or after colour was kept.
    CHECK(int(tex.pixels[0]) == int(std::lround(0.299 * 255))); // red
    CHECK(int(tex.pixels[1]) == int(std::lround(0.587 * 255))); // green
    CHECK(int(tex.pixels[2]) == int(std::lround(0.114 * 255))); // blue
    CHECK(int(tex.pixels[3]) == 255);                           // white
}

TEST_CASE("A 16-bit colour texture decodes to nothing rather than noise", "[TextureDisplacement]")
{
    // The 2x2 image above at 16 bits per channel. The texture library converts such a file to 8-bit
    // on load, so it can only arrive here stored as-is, from a project file.
    static const unsigned char bytes[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02,
        0x10, 0x02, 0x00, 0x00, 0x00, 0xad, 0x44, 0x46, 0x30, 0x00, 0x00, 0x00,
        0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0xf8, 0xff, 0x9f, 0x01,
        0x0c, 0x60, 0x34, 0x90, 0x01, 0x01, 0x00, 0x75, 0xa4, 0x0b, 0xf5, 0x97,
        0xf4, 0x36, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
        0x42, 0x60, 0x82,
    };
    TextureDisplacementLayer layer;
    layer.slot       = 0;
    layer.image_data = std::make_shared<std::vector<unsigned char>>(std::begin(bytes), std::end(bytes));

    CHECK(decode_height_texture(layer).empty());
}

TEST_CASE("A truncated texture decodes to nothing instead of aborting", "[TextureDisplacement]")
{
    // A half-copied file in the texture folder, or a damaged project file. libpng reports this by
    // longjmp, and aborts the process if the decoder has not set a jump buffer to land on.
    const auto whole = GENERATE(make_flat_gray_png(128, 16, 16), make_rgb_png_2x2());
    TextureDisplacementLayer layer;
    layer.slot       = 0;
    layer.image_data = std::make_shared<std::vector<unsigned char>>(whole->begin(), whole->begin() + whole->size() / 2);

    CHECK(decode_height_texture(layer).empty());
}

TEST_CASE("TextureDisplacement: a grayscale texture reports no colour", "[TextureDisplacement]")
{
    // The shipped library is all grayscale, and has_color() is what the whole colour feature keys
    // off - a height map must never look like it has colours to apply.
    TextureDisplacementLayer layer;
    layer.slot       = 0;
    layer.image_data = make_flat_gray_png(128);

    const DecodedHeightTexture tex = decode_height_texture(layer);
    REQUIRE_FALSE(tex.empty());
    CHECK_FALSE(tex.has_color());
    CHECK(tex.rgb.empty());

    Vec3f out(9.f, 9.f, 9.f);
    CHECK_FALSE(sample_layer_color(tex, layer, Vec3f::Zero(), Vec3f::UnitZ(), out));
    CHECK(out.x() == 9.f); // left untouched on a false return
}

// Two triangles making a 10x10 quad in the z=0 plane.
static indexed_triangle_set color_test_quad()
{
    indexed_triangle_set quad;
    quad.vertices = { Vec3f(0, 0, 0), Vec3f(10, 0, 0), Vec3f(10, 10, 0), Vec3f(0, 10, 0) };
    quad.indices  = { { 0, 1, 2 }, { 0, 2, 3 } };
    return quad;
}

TEST_CASE("TextureDisplacement: colour is reported per triangle and only where painted", "[TextureDisplacement]")
{
    const indexed_triangle_set quad = color_test_quad();

    TextureDisplacementLayer layer;
    layer.slot              = 0;
    layer.image_data        = make_rgb_png_2x2();
    layer.color_enabled     = true;
    layer.depth_mm          = 0.f; // colour only, so this isolates the colour path from the geometry
    layer.tiling_scale      = 100.f;
    layer.projection_method = TextureProjectionMethod::Triplanar;

    TriangleMesh     mesh(quad);
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::ENFORCER); // only the first triangle

    TextureDisplacementFacetsData facets;
    facets[0] = selector.serialize();

    // A three-entry palette matched in plain RGB: all this test needs is *an* index. The perceptual
    // matching is the GUI's (make_palette_quantizer), and is deliberately not under test here.
    const std::array<Vec3f, 3> palette = { Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1) };
    TextureColorRequest        request;
    std::vector<uint8_t>       triangle_color;
    request.out_triangle = &triangle_color;
    request.quantize     = [&palette](const Vec3f &rgb) {
        int   best = 0;
        float bd   = std::numeric_limits<float>::max();
        for (int i = 0; i < 3; ++i)
            if (const float d = (palette[size_t(i)] - rgb).squaredNorm(); d < bd) {
                bd   = d;
                best = i;
            }
        return best;
    };

    const indexed_triangle_set out = build_texture_displacement(quad, { layer }, facets, classic_options(), {}, &request);

    REQUIRE_FALSE(out.indices.empty());
    REQUIRE(triangle_color.size() == quad.indices.size());
    // The painted triangle takes a filament; the unpainted one is left at 0, which is
    // EnforcerBlockerType::NONE - "use the volume's own filament". That is what confines the effect
    // to the painted area without having to invent a colour for everything outside it.
    CHECK(triangle_color[0] != 0);
    CHECK(triangle_color[1] == 0);
}

TEST_CASE("TextureDisplacement: a layer that is not colouring reports no colours", "[TextureDisplacement]")
{
    const indexed_triangle_set quad = color_test_quad();

    TextureDisplacementLayer layer;
    layer.slot          = 0;
    layer.image_data    = make_rgb_png_2x2();
    layer.color_enabled = false; // the checkbox is off: colour stays off even on a colour texture
    layer.depth_mm      = 1.f;

    TriangleMesh     mesh(quad);
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::ENFORCER);
    selector.set_facet(1, EnforcerBlockerType::ENFORCER);

    TextureDisplacementFacetsData facets;
    facets[0] = selector.serialize();

    TextureColorRequest  request;
    std::vector<uint8_t> triangle_color;
    request.out_triangle = &triangle_color;
    request.quantize     = [](const Vec3f &) { return 0; };

    build_texture_displacement(quad, { layer }, facets, classic_options(), {}, &request);

    REQUIRE(triangle_color.size() == quad.indices.size());
    CHECK(triangle_color[0] == 0);
    CHECK(triangle_color[1] == 0);
}

TEST_CASE("TextureDisplacement: subdivision refines a colour boundary a flat height field hides",
          "[TextureDisplacement]")
{
    // A cube with a flat height field, so *nothing* in the height criteria has any reason to refine
    // it - which is exactly the case the colour criterion exists for. Closed, so every_edge_used_twice()
    // is an exact crack detector: the colour criterion goes through the same conformal bisection as
    // everything else and must not be able to open one.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const std::vector<uint8_t> region(cube.indices.size(), REFINE_PAINTED);

    auto longest_edge = [](const indexed_triangle_set &its, const stl_triangle_vertex_indices &t) {
        float m = 0.f;
        for (int e = 0; e < 3; ++e)
            m = std::max(m, (its.vertices[t[e]] - its.vertices[t[(e + 1) % 3]]).norm());
        return m;
    };

    // One filament on each side of x = 5: a step, with no gradient anywhere for a chord test to see.
    ColorFieldSampler split_at_five = [](const Vec3f &p, const Vec3f &) { return p.x() < 5.f ? 0 : 1; };
    ColorFieldSampler all_one       = [](const Vec3f &, const Vec3f &) { return 0; };

    SECTION("a colour boundary gets triangles")
    {
        const indexed_triangle_set out =
            subdivide_mesh_adaptive(cube, region, /*max edge*/ 0.f, 200000, nullptr, nullptr, 0.f,
                                    /*min_edge*/ 0.05f, /*border*/ 0.f, nullptr, split_at_five,
                                    /*colour edge*/ 0.5f);

        CHECK(out.indices.size() > cube.indices.size());
        CHECK(every_edge_used_twice(out)); // still watertight

        // Every triangle still straddling the boundary must be down at the target.
        for (const auto &t : out.indices) {
            bool straddles = false;
            for (int i = 1; i < 3; ++i)
                if ((out.vertices[t[i]].x() < 5.f) != (out.vertices[t[0]].x() < 5.f))
                    straddles = true;
            if (straddles)
                CHECK(longest_edge(out, t) <= 0.5f + 1e-4f);
        }
    }

    SECTION("a uniform colour adds nothing")
    {
        const indexed_triangle_set out =
            subdivide_mesh_adaptive(cube, region, /*max edge*/ 0.f, 200000, nullptr, nullptr, 0.f,
                                    /*min_edge*/ 0.05f, /*border*/ 0.f, nullptr, all_one,
                                    /*colour edge*/ 0.5f);

        CHECK(out.indices.size() == cube.indices.size());
    }

    SECTION("no colour sampler leaves the mesh alone")
    {
        // The regression this guards: the colour criterion must be inert when nothing is colouring,
        // or every bake would start refining geometry for no reason.
        const indexed_triangle_set out =
            subdivide_mesh_adaptive(cube, region, /*max edge*/ 0.f, 200000, nullptr, nullptr, 0.f,
                                    /*min_edge*/ 0.05f, /*border*/ 0.f, nullptr, nullptr,
                                    /*colour edge*/ 0.5f);

        CHECK(out.indices.size() == cube.indices.size());
    }
}

// ---------------------------------------------------------------------------------------------
// Step cutter
// ---------------------------------------------------------------------------------------------

// A flat square sheet in the (x, y) plane split into right triangles of the given edge, with the
// diagonal alternated so the mesh has no preferred direction. Normals +z.
static indexed_triangle_set make_flat_sheet(float size, float edge)
{
    indexed_triangle_set its;
    const int            n = std::max(1, int(std::lround(size / edge)));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            its.vertices.emplace_back(size * float(i) / float(n), size * float(j) / float(n), 0.f);
    const auto id = [n](int i, int j) { return j * (n + 1) + i; };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            if ((i + j) % 2 == 0) {
                its.indices.emplace_back(id(i, j), id(i + 1, j), id(i + 1, j + 1));
                its.indices.emplace_back(id(i, j), id(i + 1, j + 1), id(i, j + 1));
            } else {
                its.indices.emplace_back(id(i, j), id(i + 1, j), id(i, j + 1));
                its.indices.emplace_back(id(i + 1, j), id(i + 1, j + 1), id(i, j + 1));
            }
        }
    return its;
}

// Every interior edge of a sheet is shared by exactly two triangles; only the sheet's own border may
// be used once.
static bool sheet_is_manifold(const indexed_triangle_set &its, float size)
{
    std::map<std::pair<int, int>, int> uses;
    for (const auto &t : its.indices)
        for (int e = 0; e < 3; ++e) {
            int a = t[e], b = t[(e + 1) % 3];
            if (a > b)
                std::swap(a, b);
            ++uses[{ a, b }];
        }
    for (const auto &[edge, n] : uses) {
        const Vec3f &pa = its.vertices[size_t(edge.first)], &pb = its.vertices[size_t(edge.second)];
        const bool   border = (pa.x() == pb.x() && (pa.x() == 0.f || pa.x() == size)) ||
                            (pa.y() == pb.y() && (pa.y() == 0.f || pa.y() == size));
        if (n > 2 || (n == 1 && !border))
            return false;
    }
    return true;
}

TEST_CASE("TextureDisplacement: the step cutter turns a stepped field into walls", "[TextureDisplacement]")
{
    // Square posts 1.2 mm wide on a 2 mm pitch, as a binary field: a step everywhere along the post
    // edges, flat everywhere else. 1 mm triangles, so every post edge crosses several of them.
    constexpr float RELIEF = 0.4f, SIZE = 6.f, STEP_W = 0.05f, GAP = 0.075f;
    const auto      posts = [](float x, float y) {
        const float fx = std::fmod(std::fmod(x, 2.f) + 2.f, 2.f), fy = std::fmod(std::fmod(y, 2.f) + 2.f, 2.f);
        return (fx > 0.4f && fx < 1.6f && fy > 0.4f && fy < 1.6f) ? RELIEF : 0.f;
    };
    const HeightFieldSampler   sampler = [&](const Vec3f &p, const Vec3f &) { return posts(p.x(), p.y()); };
    const indexed_triangle_set sheet   = make_flat_sheet(SIZE, 1.f);
    const std::vector<uint8_t> region(sheet.indices.size(), 1);

    std::vector<int>           source;
    size_t                     cuts = 0;
    const indexed_triangle_set cut  = cut_mesh_at_steps(sheet, region, sampler, STEP_W, GAP, 0.f, &source, &cuts);

    CHECK(cuts > 0);
    CHECK(cut.indices.size() > sheet.indices.size());
    REQUIRE(source.size() == cut.indices.size());
    CHECK(sheet_is_manifold(cut, SIZE));

    // Displace along the cut mesh's own area-weighted vertex normals, as the bake does.
    std::vector<Vec3f> normal(cut.vertices.size(), Vec3f::Zero());
    for (const auto &t : cut.indices) {
        const Vec3f &a = cut.vertices[size_t(t[0])], &b = cut.vertices[size_t(t[1])], &c = cut.vertices[size_t(t[2])];
        const Vec3f  n = (b - a).cross(c - a);
        CHECK(n.z() > 0.f); // nothing inverted, nothing degenerate
        for (int k = 0; k < 3; ++k)
            normal[size_t(t[k])] += n;
    }
    indexed_triangle_set displaced = cut;
    for (size_t v = 0; v < displaced.vertices.size(); ++v) {
        const Vec3f n = normal[v].normalized();
        displaced.vertices[v] += n * sampler(cut.vertices[v], n);
    }

    // The defining property of the cut: a triangle that spans both heights is a wall, and a wall stands
    // within the seam gap of a step. Anywhere else a mixed triangle would be a ramp - exactly what
    // refinement leaves and the cutter is there to remove.
    const auto near_step = [&](const Vec3f &p) {
        const float h = posts(p.x(), p.y());
        for (int k = 0; k < 8; ++k) {
            const float ang = float(k) * float(M_PI) / 4.f;
            if (posts(p.x() + GAP * std::cos(ang), p.y() + GAP * std::sin(ang)) != h)
                return true;
        }
        return false;
    };
    size_t walls = 0, ramps = 0;
    for (const auto &t : displaced.indices) {
        const float z0 = displaced.vertices[size_t(t[0])].z(), z1 = displaced.vertices[size_t(t[1])].z(),
                    z2 = displaced.vertices[size_t(t[2])].z();
        if (std::abs(z0 - z1) < 1e-4f && std::abs(z1 - z2) < 1e-4f)
            continue; // flat: on one level
        bool wall = true;
        for (int k = 0; k < 3; ++k)
            wall = wall && near_step(cut.vertices[size_t(t[k])]);
        (wall ? walls : ramps)++;
    }
    CHECK(walls > 0);
    CHECK(ramps == 0);
}

TEST_CASE("TextureDisplacement: the step cutter passes a smooth field through untouched", "[TextureDisplacement]")
{
    // A wide hill: its mid-level contour runs through the sheet, but nowhere is it a step, so there is
    // nothing to cut - refinement is the right tool for it.
    const HeightFieldSampler   hill = [](const Vec3f &p, const Vec3f &) {
        const float r2 = (p.x() - 3.f) * (p.x() - 3.f) + (p.y() - 3.f) * (p.y() - 3.f);
        return 0.4f * std::exp(-r2 / 3.f);
    };
    const indexed_triangle_set sheet = make_flat_sheet(6.f, 1.f);
    const std::vector<uint8_t> region(sheet.indices.size(), 1);

    std::vector<int>           source;
    size_t                     cuts = 0;
    const indexed_triangle_set out  = cut_mesh_at_steps(sheet, region, hill, 0.05f, 0.075f, 0.f, &source, &cuts);

    CHECK(cuts == 0);
    CHECK(out.indices.size() == sheet.indices.size());
    CHECK(out.vertices.size() == sheet.vertices.size());
    REQUIRE(source.size() == sheet.indices.size());
    for (size_t i = 0; i < source.size(); ++i)
        CHECK(source[i] == int(i));
}

TEST_CASE("TextureDisplacement: the step cutter leaves features at the step's own scale to refinement",
          "[TextureDisplacement]")
{
    // Square posts filling the middle half of each cell, binary and sharp at every crossing - the only
    // difference between the two is the pitch (offset so no post edge lies along a mesh edge). Posts a
    // step width or so across have no pure interior for a seam copy to land in, and cutting them would
    // double the triangles for walls no bigger than the blur.
    constexpr float STEP_W = 0.05f;
    const float     pitch  = GENERATE(0.12f, 0.6f);
    const auto      posts  = [pitch](const Vec3f &p, const Vec3f &) {
        const float fx = std::fmod(p.x() + 0.17f, pitch) / pitch, fy = std::fmod(p.y() + 0.31f, pitch) / pitch;
        return (fx > 0.25f && fx < 0.75f && fy > 0.25f && fy < 0.75f) ? 0.4f : 0.f;
    };
    const indexed_triangle_set sheet = make_flat_sheet(6.f, 1.f);
    const std::vector<uint8_t> region(sheet.indices.size(), 1);

    size_t                     cuts = 0;
    const indexed_triangle_set out  = cut_mesh_at_steps(sheet, region, posts, STEP_W, STEP_W, 0.f, nullptr, &cuts);

    if (0.5f * pitch < 2.25f * STEP_W) {
        CHECK(cuts == 0);
        CHECK(out.indices.size() == sheet.indices.size());
    } else {
        CHECK(cuts > 0);
        CHECK(sheet_is_manifold(out, 6.f));
    }
}

TEST_CASE("TextureDisplacement: the step cutter only cuts inside the region", "[TextureDisplacement]")
{
    // A single step at x = 3 across the whole sheet, but only the left half is painted.
    const HeightFieldSampler   stripe = [](const Vec3f &p, const Vec3f &) { return p.x() > 3.f ? 0.4f : 0.f; };
    const indexed_triangle_set sheet  = make_flat_sheet(6.f, 1.f);

    SECTION("an empty region is a no-op")
    {
        const std::vector<uint8_t> none(sheet.indices.size(), 0);
        size_t                     cuts = 0;
        const indexed_triangle_set out  = cut_mesh_at_steps(sheet, none, stripe, 0.05f, 0.075f, 0.f, nullptr, &cuts);
        CHECK(cuts == 0);
        CHECK(out.indices.size() == sheet.indices.size());
    }

    SECTION("unpainted triangles away from the paint are untouched")
    {
        std::vector<uint8_t> region(sheet.indices.size(), 0);
        for (size_t t = 0; t < sheet.indices.size(); ++t) {
            const auto &f = sheet.indices[t];
            const float cy = (sheet.vertices[size_t(f[0])].y() + sheet.vertices[size_t(f[1])].y() +
                              sheet.vertices[size_t(f[2])].y()) / 3.f;
            region[t] = cy < 3.f ? 1 : 0; // paint the lower half
        }
        std::vector<int>           source;
        size_t                     cuts = 0;
        const indexed_triangle_set out  = cut_mesh_at_steps(sheet, region, stripe, 0.05f, 0.075f, 0.f, &source, &cuts);
        CHECK(cuts > 0);
        CHECK(sheet_is_manifold(out, 6.f));
        // An unpainted triangle that shares no edge with a painted one comes out exactly as it went in.
        std::vector<int> descendants(sheet.indices.size(), 0);
        for (int s : source)
            ++descendants[size_t(s)];
        for (size_t t = 0; t < sheet.indices.size(); ++t) {
            const auto &f = sheet.indices[t];
            float       ymin = 6.f;
            for (int k = 0; k < 3; ++k)
                ymin = std::min(ymin, sheet.vertices[size_t(f[k])].y());
            if (ymin > 3.f) // strictly above the painted half, so no shared edge with it
                CHECK(descendants[t] == 1);
        }
    }
}

// ---- Image adjustments (the gizmo's image-adjustment panel) ----------------------------------------

// A grayscale ramp 0..255 across the width, so every input level is present exactly once per row and a
// tonal curve can be read straight off the output.
static DecodedHeightTexture make_gray_ramp(int w = 256, int h = 4)
{
    DecodedHeightTexture t;
    t.width  = w;
    t.height = h;
    t.pixels.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            t.pixels[size_t(y) * size_t(w) + size_t(x)] = uint8_t(x * 255 / std::max(1, w - 1));
    return t;
}

// A one-texel checkerboard of 0 and 255 - the highest spatial frequency the grid can hold, which is what
// a blur has to remove and a downscale has to average rather than drop.
static DecodedHeightTexture make_gray_checkerboard(int w = 16, int h = 16)
{
    DecodedHeightTexture t;
    t.width  = w;
    t.height = h;
    t.pixels.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            t.pixels[size_t(y) * size_t(w) + size_t(x)] = ((x + y) % 2 == 0) ? 0 : 255;
    return t;
}

static DecodedHeightTexture make_rgb_flat(uint8_t r, uint8_t g, uint8_t b, int w = 8, int h = 8)
{
    DecodedHeightTexture t;
    t.width  = w;
    t.height = h;
    t.pixels.resize(size_t(w) * size_t(h));
    t.rgb.resize(size_t(w) * size_t(h) * 3);
    for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        t.rgb[i * 3]     = r;
        t.rgb[i * 3 + 1] = g;
        t.rgb[i * 3 + 2] = b;
        t.pixels[i]      = uint8_t(std::lround(0.299 * r + 0.587 * g + 0.114 * b));
    }
    return t;
}

TEST_CASE("an adjustment that moves no slider returns the image unchanged", "[TextureDisplacement]")
{
    const DecodedHeightTexture src = make_gray_ramp();
    const DecodedHeightTexture out = apply_texture_adjustments(src, TextureAdjustments{});
    CHECK(out.width == src.width);
    CHECK(out.height == src.height);
    CHECK(out.pixels == src.pixels);
    CHECK(TextureAdjustments{}.identity());
}

TEST_CASE("brightness reaches pure white and pure black only at the ends of its range", "[TextureDisplacement]")
{
    const DecodedHeightTexture src = make_gray_ramp();

    TextureAdjustments up;
    up.brightness = 1.f;
    const DecodedHeightTexture white = apply_texture_adjustments(src, up);
    CHECK(std::all_of(white.pixels.begin(), white.pixels.end(), [](uint8_t v) { return v == 255; }));

    TextureAdjustments down;
    down.brightness = -1.f;
    const DecodedHeightTexture black = apply_texture_adjustments(src, down);
    CHECK(std::all_of(black.pixels.begin(), black.pixels.end(), [](uint8_t v) { return v == 0; }));
}

TEST_CASE("brightness lifts the image without flattening it into a clipped plateau", "[TextureDisplacement]")
{
    // The reason brightness is a lift toward white rather than an additive offset: an offset would drive
    // everything above mid-grey to a flat 255, and since this image is also the height map that region
    // would bake as flat relief. Half a stop up must leave almost nothing at pure white.
    const DecodedHeightTexture src = make_gray_ramp();
    TextureAdjustments         adj;
    adj.brightness = 0.5f;
    const DecodedHeightTexture out = apply_texture_adjustments(src, adj);

    REQUIRE(out.pixels.size() == src.pixels.size());
    const size_t clipped = size_t(std::count(out.pixels.begin(), out.pixels.end(), uint8_t(255)));
    CHECK(clipped * 100 < out.pixels.size()); // under 1%; an additive shift would clip half the ramp

    // Every texel got brighter, and the curve stays ordered: brighter in means brighter out.
    for (size_t i = 0; i < out.pixels.size(); ++i)
        REQUIRE(out.pixels[i] >= src.pixels[i]);
    for (int x = 1; x < out.width; ++x)
        REQUIRE(out.pixels[size_t(x)] >= out.pixels[size_t(x - 1)]);
}

TEST_CASE("contrast at its lowest flattens the image to mid-grey and at its highest spreads it apart",
          "[TextureDisplacement]")
{
    const DecodedHeightTexture src = make_gray_ramp();

    TextureAdjustments flat;
    flat.contrast = -1.f;
    const DecodedHeightTexture grey = apply_texture_adjustments(src, flat);
    // One value everywhere - the pivot. Which exact level that rounds to is not the point.
    CHECK(std::adjacent_find(grey.pixels.begin(), grey.pixels.end(), std::not_equal_to<uint8_t>()) ==
          grey.pixels.end());

    TextureAdjustments hard;
    hard.contrast = 1.f;
    const DecodedHeightTexture split = apply_texture_adjustments(src, hard);
    // A threshold about the pivot: the dark end of the ramp goes to black, the light end to white.
    CHECK(split.pixels[0] == 0);
    CHECK(split.pixels[size_t(split.width - 1)] == 255);
}

TEST_CASE("blur removes the finest detail and sharpness exaggerates an edge", "[TextureDisplacement]")
{
    // Blur: a one-texel checkerboard is the highest frequency the grid holds, so a blur has to collapse it
    // toward its own mean. Measured as mean absolute deviation, which needs no assumption about the mean.
    const DecodedHeightTexture checker = make_gray_checkerboard(32, 32);
    const auto                 deviation = [](const DecodedHeightTexture &t) {
        double mean = 0.;
        for (uint8_t v : t.pixels)
            mean += double(v);
        mean /= double(t.pixels.size());
        double dev = 0.;
        for (uint8_t v : t.pixels)
            dev += std::abs(double(v) - mean);
        return dev / double(t.pixels.size());
    };
    TextureAdjustments blur;
    blur.blur = 1.f;
    CHECK(deviation(apply_texture_adjustments(checker, blur)) < 0.25 * deviation(checker));

    // Sharpness: an unsharp mask pushes each side of a step further from the other, so the step
    // overshoots - darker than the dark side just inside it, brighter than the bright side just inside.
    DecodedHeightTexture step;
    step.width  = 64;
    step.height = 8;
    step.pixels.assign(size_t(step.width) * size_t(step.height), 100);
    for (int y = 0; y < step.height; ++y)
        for (int x = step.width / 2; x < step.width; ++x)
            step.pixels[size_t(y) * size_t(step.width) + size_t(x)] = 150;

    TextureAdjustments sharp;
    sharp.sharpness = 1.f;
    const DecodedHeightTexture sharpened = apply_texture_adjustments(step, sharp);
    const size_t               row       = size_t(step.height / 2) * size_t(step.width);
    const size_t               mid       = size_t(step.width / 2);
    CHECK(sharpened.pixels[row + mid - 1] < 100); // undershoot on the dark side of the step
    CHECK(sharpened.pixels[row + mid] > 150);     // overshoot on the light side
}

TEST_CASE("a grayscale image stays grayscale and its hue slider does nothing", "[TextureDisplacement]")
{
    const DecodedHeightTexture src = make_gray_ramp();
    REQUIRE_FALSE(src.has_color());

    TextureAdjustments hue;
    hue.hue_deg = 120.f;
    const DecodedHeightTexture out = apply_texture_adjustments(src, hue);
    // Not promoted to RGB: a grey image has no colour to offer the layer's colour feature, and storing it
    // as RGB would triple the PNG for a row of identical greys.
    CHECK_FALSE(out.has_color());
    CHECK(out.rgb.empty());
    CHECK(out.pixels == src.pixels);
}

TEST_CASE("a colour image's height stays the luminance of its adjusted colour", "[TextureDisplacement]")
{
    // The invariant that keeps the relief and the colour from drifting apart: whatever the adjustment did
    // to the channels, the height is recomputed from the channels that came out.
    const DecodedHeightTexture src = make_rgb_flat(200, 60, 30);
    REQUIRE(src.has_color());

    TextureAdjustments adj;
    adj.brightness = 0.2f;
    adj.contrast   = 0.3f;
    adj.hue_deg    = 75.f;
    const DecodedHeightTexture out = apply_texture_adjustments(src, adj);
    REQUIRE(out.has_color());

    for (size_t i = 0; i < out.pixels.size(); ++i) {
        const long expect = std::lround(0.299 * out.rgb[i * 3] + 0.587 * out.rgb[i * 3 + 1] +
                                        0.114 * out.rgb[i * 3 + 2]);
        REQUIRE(long(out.pixels[i]) == expect);
    }
    // The hue turn actually moved the colour, so the check above is not passing on an unchanged image.
    CHECK(out.rgb != src.rgb);
}

TEST_CASE("a full turn of the hue leaves a colour where it started", "[TextureDisplacement]")
{
    const DecodedHeightTexture src = make_rgb_flat(200, 60, 30);
    TextureAdjustments         adj;
    adj.hue_deg                    = 360.f;
    const DecodedHeightTexture out = apply_texture_adjustments(src, adj);
    REQUIRE(out.has_color());
    // Rounding through 8-bit channels, so a level or two of slack rather than exact equality.
    for (size_t i = 0; i < out.rgb.size(); ++i)
        REQUIRE(std::abs(int(out.rgb[i]) - int(src.rgb[i])) <= 2);
}

TEST_CASE("the adjustment blur radius ramps from zero and stays under its cost cap", "[TextureDisplacement]")
{
    CHECK_THAT(adjustment_blur_radius(0.f, 512, 512), Catch::Matchers::WithinAbs(0.f, 1e-6f));
    // 5% of the short side, as the Smoothing slider uses.
    CHECK_THAT(adjustment_blur_radius(1.f, 512, 512), Catch::Matchers::WithinAbs(25.6f, 1e-3f));
    CHECK_THAT(adjustment_blur_radius(0.5f, 512, 512), Catch::Matchers::WithinAbs(12.8f, 1e-3f));
    // Capped, so a large image at the top of the slider cannot cost without bound.
    CHECK_THAT(adjustment_blur_radius(1.f, 8192, 8192), Catch::Matchers::WithinAbs(48.f, 1e-3f));
    // Sized off the short side, not the long one.
    CHECK_THAT(adjustment_blur_radius(1.f, 4096, 200), Catch::Matchers::WithinAbs(10.f, 1e-3f));
}

TEST_CASE("downscaling a height texture averages the texels it drops and never upscales",
          "[TextureDisplacement]")
{
    // Averaging, not decimation: a one-texel checkerboard halved must come out uniformly mid-grey. Picking
    // one texel per block would instead come out all black or all white - detail the adjustment never
    // produced, shown in the preview.
    const DecodedHeightTexture small = downscale_height_texture(make_gray_checkerboard(16, 16), 8);
    CHECK(small.width == 8);
    CHECK(small.height == 8);
    CHECK(std::all_of(small.pixels.begin(), small.pixels.end(), [](uint8_t v) { return v == 127; }));

    // Already within the limit: handed back as it is.
    const DecodedHeightTexture ramp = make_gray_ramp(64, 4);
    const DecodedHeightTexture same = downscale_height_texture(ramp, 256);
    CHECK(same.width == ramp.width);
    CHECK(same.height == ramp.height);
    CHECK(same.pixels == ramp.pixels);

    // Aspect preserved, and a colour source keeps its colour.
    const DecodedHeightTexture wide = downscale_height_texture(make_rgb_flat(200, 60, 30, 400, 100), 100);
    CHECK(wide.width == 100);
    CHECK(wide.height == 25);
    CHECK(wide.has_color());
}

TEST_CASE("a layer's live image adjustment reaches its decode", "[TextureDisplacement]")
{
    // This is what makes the image-adjustment panel's sliders move the relief: decode_height_texture()
    // applies the layer's preview_adjust, so everything downstream of the decode - the displacement, the
    // colour, the thumbnails, the bake - sees the adjusted image without being told about the panel.
    TextureDisplacementLayer layer;
    layer.image_data = make_checkerboard_png(32, 32);

    const DecodedHeightTexture raw = decode_height_texture(layer);
    REQUIRE_FALSE(raw.empty());

    // An untouched panel, and a closed one, cost nothing and change nothing.
    layer.preview_adjust = TextureAdjustments{};
    CHECK(decode_height_texture(layer).pixels == raw.pixels);

    layer.preview_adjust.brightness = 0.5f;
    const DecodedHeightTexture bright = decode_height_texture(layer);
    CHECK(bright.pixels != raw.pixels);
    CHECK(bright.pixels == apply_texture_adjustments(raw, layer.preview_adjust).pixels);

    // Back to identity gives the stored image back, so closing the panel really does revert the layer.
    layer.preview_adjust = TextureAdjustments{};
    CHECK(decode_height_texture(layer).pixels == raw.pixels);
}

TEST_CASE("changing a live adjustment is not served the previous one from cache", "[TextureDisplacement]")
{
    // The decode caches by the image_data pointer, which does not change while a slider moves - so the
    // adjustment has to be part of the key or every slider move past the first would redraw the same
    // pixels. Same for the smoothed copy, which is computed on top of the adjusted one.
    TextureDisplacementLayer layer;
    layer.image_data = make_checkerboard_png(32, 32);
    const DecodedHeightTexture raw = decode_height_texture(layer);
    REQUIRE_FALSE(raw.empty());

    layer.preview_adjust.brightness = 0.5f;
    const std::vector<uint8_t> bright = decode_height_texture(layer).pixels;
    layer.preview_adjust.brightness   = -0.5f;
    const std::vector<uint8_t> dark   = decode_height_texture(layer).pixels;
    CHECK(bright != dark);
    CHECK(dark == apply_texture_adjustments(raw, layer.preview_adjust).pixels);

    // The same question again with smoothing on, which is cached separately and keyed by both.
    layer.smoothing                 = 0.5f;
    layer.preview_adjust.brightness = 0.5f;
    const std::vector<uint8_t> smooth_bright = decode_height_texture(layer).pixels;
    layer.preview_adjust.brightness          = -0.5f;
    const std::vector<uint8_t> smooth_dark   = decode_height_texture(layer).pixels;
    CHECK(smooth_bright != smooth_dark);
}

// ---------------------------------------------------------------------------------------------
// Texture smoothing
// ---------------------------------------------------------------------------------------------

TEST_CASE("TextureDisplacement: texture smoothing is a wrapped box blur whatever the radius", "[TextureDisplacement]")
{
    // A small pseudo-random grey image, encoded through the PNG writer so decode_height_texture() takes
    // its normal path. The expected result is the plain definition of the blur - two passes of a
    // (2r+1) box, horizontal then vertical, wrapping at the edges - which the sliding-window
    // implementation must reproduce byte for byte.
    const size_t w = 37, h = 23;
    std::vector<uint8_t> src(w * h);
    uint32_t seed = 12345;
    for (uint8_t &p : src) { seed = seed * 1664525u + 1013904223u; p = uint8_t(seed >> 24); }
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), w, h, src));
    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);
    REQUIRE_FALSE(bytes.empty());

    TextureDisplacementLayer layer;
    layer.image_data = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    // radius = smoothing * 0.05 * min(w, h) = 0.05 * 23 * smoothing; 1.0 gives 1.15 -> whole radius 2,
    // cross-faded 57.5 % toward the blurred image (see smooth_height_pixels()).
    layer.smoothing = 1.f;
    const DecodedHeightTexture tex = decode_height_texture(layer);
    REQUIRE(tex.width == int(w));
    REQUIRE(tex.height == int(h));

    const int   radius = 2;
    const float mixf   = std::clamp((0.05f * 23.f) / 2.f, 0.f, 1.f);
    auto box = [&](std::vector<uint8_t> px) {
        const int   window = 2 * radius + 1;
        const float inv    = 1.f / float(window);
        std::vector<uint8_t> t2(px.size());
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = 0; y < int(h); ++y)
                for (int x = 0; x < int(w); ++x) {
                    float s = 0.f;
                    for (int k = -radius; k <= radius; ++k) s += float(px[size_t(y) * w + size_t(((x + k) % int(w) + int(w)) % int(w))]);
                    t2[size_t(y) * w + size_t(x)] = uint8_t(std::lround(s * inv));
                }
            for (int x = 0; x < int(w); ++x)
                for (int y = 0; y < int(h); ++y) {
                    float s = 0.f;
                    for (int k = -radius; k <= radius; ++k) s += float(t2[size_t(((y + k) % int(h) + int(h)) % int(h)) * w + size_t(x)]);
                    px[size_t(y) * w + size_t(x)] = uint8_t(std::lround(s * inv));
                }
        }
        return px;
    };
    const std::vector<uint8_t> blurred = box(src);
    size_t mismatches = 0;
    for (size_t i = 0; i < w * h; ++i) {
        const uint8_t expected = uint8_t(std::lround(float(src[i]) + (float(blurred[i]) - float(src[i])) * mixf));
        mismatches += tex.pixels[i] != expected;
    }
    CHECK(mismatches == 0);

    // Asking again with the same smoothing is served from the cache and must be identical.
    const DecodedHeightTexture again = decode_height_texture(layer);
    CHECK(again.pixels == tex.pixels);
}

// ---------------------------------------------------------------------------------------------
// Edge flips along the height field (v2 pipeline)
// ---------------------------------------------------------------------------------------------

TEST_CASE("TextureDisplacement: edge flips lay a stepped field's wall along the grid's diagonals", "[TextureDisplacement]")
{
    // A regular grid crossed by a step at 30 degrees. Before flipping, the step's wall zigzags: many
    // triangles have corners on both sides of it. Flipping each quad's diagonal to follow the step must
    // cut that count down, without changing the triangle count, the winding, or the manifoldness.
    const indexed_triangle_set sheet = make_flat_sheet(6.f, 0.2f);
    const float                c = std::cos(0.5236f), s = std::sin(0.5236f);
    const auto                 side_of = [&](const Vec3f &p) { return c * p.x() + s * p.y() > 3.5f; };
    const TextureBake::HeightSampleFn field = [&](const Vec3f &p, const Vec3f &, const Vec3f &) {
        return side_of(p) ? 0.4f : 0.f;
    };
    const auto mixed = [&](const TextureBake::TriSoup &g) {
        size_t n = 0;
        for (size_t t = 0; t < g.triangle_count(); ++t) {
            const bool a = side_of(g.pos[t * 3]), b = side_of(g.pos[t * 3 + 1]), d = side_of(g.pos[t * 3 + 2]);
            n += (a != b || b != d);
        }
        return n;
    };

    const TextureBake::TriSoup    before = TextureBake::to_soup(sheet);
    const TextureBake::FlipResult after  = TextureBake::flip_edges_to_height(before, {}, field, TextureBake::FlipSettings{}, {});

    CHECK(after.flipped > 0);
    REQUIRE(after.geometry.triangle_count() == before.triangle_count());
    const size_t mixed_before = mixed(before), mixed_after = mixed(after.geometry);
    CHECK(mixed_after < mixed_before);
    // Every triangle still faces up, and none collapsed.
    for (size_t t = 0; t < after.geometry.triangle_count(); ++t) {
        const Vec3f n = (after.geometry.pos[t * 3 + 1] - after.geometry.pos[t * 3]).cross(after.geometry.pos[t * 3 + 2] - after.geometry.pos[t * 3]);
        CHECK(n.z() > 1e-6f);
    }
    // Manifold: rebuild an indexed mesh from the soup and count edge uses.
    indexed_triangle_set rebuilt;
    std::map<std::tuple<int, int, int>, int> ids;
    for (const Vec3f &p : after.geometry.pos) {
        const auto key = std::make_tuple(int(std::lround(p.x() * 1e4)), int(std::lround(p.y() * 1e4)), int(std::lround(p.z() * 1e4)));
        auto it = ids.find(key);
        if (it == ids.end()) { it = ids.emplace(key, int(rebuilt.vertices.size())).first; rebuilt.vertices.push_back(p); }
        (void) it;
    }
    for (size_t t = 0; t < after.geometry.triangle_count(); ++t) {
        int idx[3];
        for (int k = 0; k < 3; ++k) {
            const Vec3f &p = after.geometry.pos[t * 3 + size_t(k)];
            idx[k] = ids.at(std::make_tuple(int(std::lround(p.x() * 1e4)), int(std::lround(p.y() * 1e4)), int(std::lround(p.z() * 1e4))));
        }
        rebuilt.indices.emplace_back(idx[0], idx[1], idx[2]);
    }
    CHECK(sheet_is_manifold(rebuilt, 6.f));
}

// ---------------------------------------------------------------------------------------------
// Automatic resolution (v2 pipeline)
// ---------------------------------------------------------------------------------------------

TEST_CASE("TextureDisplacement: automatic resolution follows the model's size", "[TextureDisplacement]")
{
    // A 20 mm cube: diagonal 34.64 mm, so diagonal / 250 = 0.1386 mm, rounded up to 0.14.
    const indexed_triangle_set cube = its_make_cube(20.f, 20.f, 20.f);
    TextureDisplacementLayer   layer;
    layer.image_data   = make_checkerboard_png(16, 16);
    layer.tiling_scale = 8.f;

    SECTION("edge from the diagonal, budget the standard 750 k")
    {
        const V2Resolution rec = recommend_v2_resolution(cube, { layer });
        CHECK_THAT(rec.edge_mm, WithinAbs(0.14f, 1e-4f));
        CHECK(rec.budget_k == 750);
        CHECK_THAT(rec.texel_mm, WithinAbs(0.5f, 1e-4f)); // reported for the panel
    }

    SECTION("the world transform scales the diagonal")
    {
        const V2Resolution rec = recommend_v2_resolution(cube, { layer }, Transform3d(Eigen::Scaling(3.0)));
        CHECK_THAT(rec.edge_mm, WithinAbs(0.42f, 1e-4f)); // 103.9 / 250 = 0.4157 -> 0.42
    }

    SECTION("a tiny model stops at the 0.05 mm floor")
    {
        const indexed_triangle_set small = its_make_cube(2.f, 2.f, 2.f);
        const V2Resolution         rec   = recommend_v2_resolution(small, { layer });
        CHECK_THAT(rec.edge_mm, WithinAbs(0.05f, 1e-4f));
    }

    SECTION("the texture's sharpness class is still measured")
    {
        const TextureDetail sharp = analyze_texture_detail(layer);
        CHECK_THAT(sharp.pixels_per_edge, WithinAbs(1.f, 1e-6f));
        TextureDisplacementLayer flat;
        flat.image_data = make_flat_gray_png(128, 16, 16);
        CHECK_THAT(analyze_texture_detail(flat).pixels_per_edge, WithinAbs(4.f, 1e-6f));
    }
}

TEST_CASE("TextureDisplacement: the default pipeline colours its rebuilt triangles where painted", "[TextureDisplacement]")
{
    // The same quad and 2x2 colour texture as the classic-path colour test, but baked through the
    // one-run pipeline, which rebuilds the topology: every output triangle has to be coloured from the
    // texture at its own position, and only those over the painted half of the quad.
    const indexed_triangle_set quad = color_test_quad();
    TextureDisplacementLayer   layer;
    layer.slot              = 0;
    layer.image_data        = make_rgb_png_2x2();
    layer.color_enabled     = true;
    layer.depth_mm          = 0.f;
    layer.tiling_scale      = 10.f; // one tile over the whole quad, so all four texels show
    layer.projection_method = TextureProjectionMethod::Triplanar;

    TriangleMesh     mesh(quad);
    TriangleSelector selector(mesh);
    selector.set_facet(0, EnforcerBlockerType::ENFORCER); // the lower-right half only
    TextureDisplacementFacetsData facets;
    facets[0] = selector.serialize();

    const std::array<Vec3f, 3> palette = { Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1) };
    TextureColorRequest        request;
    std::vector<uint8_t>       triangle_color;
    request.out_triangle = &triangle_color;
    request.quantize     = [&palette](const Vec3f &rgb) {
        int   best = 0;
        float bd   = std::numeric_limits<float>::max();
        for (int i = 0; i < 3; ++i)
            if (const float d = (palette[size_t(i)] - rgb).squaredNorm(); d < bd) { bd = d; best = i; }
        return best;
    };

    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = 1.f;
    options.v2_max_triangles_k = 0; // no simplification: a plain refined quad
    const indexed_triangle_set out = build_texture_displacement(quad, { layer }, facets, options, {}, &request);

    REQUIRE(out.indices.size() > 2);
    REQUIRE(triangle_color.size() == out.indices.size());
    size_t coloured = 0, plain = 0;
    std::set<uint8_t> distinct;
    for (size_t i = 0; i < out.indices.size(); ++i) {
        const stl_triangle_vertex_indices &t = out.indices[i];
        const Vec3f c = (out.vertices[size_t(t[0])] + out.vertices[size_t(t[1])] + out.vertices[size_t(t[2])]) / 3.f;
        const bool  painted_side = c.y() < c.x(); // below the diagonal: triangle 0 of the quad
        if (triangle_color[i] > 0) { ++coloured; distinct.insert(triangle_color[i]); CHECK(painted_side); }
        else                       { ++plain; CHECK_FALSE(painted_side); }
    }
    CHECK(coloured > 0);
    CHECK(plain > 0);
    CHECK(distinct.size() >= 2); // the texture has four colours across the quad, so one side sees several
}

// Per chart of an unwrap: whether it is a topological disk (V - E + F = 1, one boundary loop), the only shape a
// single island can be laid flat from.
static std::vector<bool> unwrap_charts_are_disks(const PatchUnwrap &u)
{
    std::vector<std::set<int>>                 verts(size_t(u.chart_count));
    std::vector<std::set<std::pair<int, int>>> edges(size_t(u.chart_count));
    std::vector<int>                           faces(size_t(u.chart_count), 0);
    for (const stl_triangle_vertex_indices &tri : u.indices) {
        const int c = u.vertex_chart[size_t(tri[0])];
        ++faces[size_t(c)];
        for (int i = 0; i < 3; ++i) {
            verts[size_t(c)].insert(tri[i]);
            edges[size_t(c)].insert({ std::min(tri[i], tri[(i + 1) % 3]), std::max(tri[i], tri[(i + 1) % 3]) });
        }
    }
    // Boundary loops per chart: flood the boundary edges' vertices.
    std::vector<std::vector<int>> boundary_adj(u.uvs.size());
    for (const auto &[a, b] : u.boundary_edges) {
        boundary_adj[size_t(a)].push_back(b);
        boundary_adj[size_t(b)].push_back(a);
    }
    std::vector<int>  loops(size_t(u.chart_count), 0);
    std::vector<bool> seen(u.uvs.size(), false);
    for (size_t v = 0; v < u.uvs.size(); ++v) {
        if (seen[v] || boundary_adj[v].empty())
            continue;
        ++loops[size_t(u.vertex_chart[v])];
        std::vector<int> stack{ int(v) };
        seen[v] = true;
        while (!stack.empty()) {
            const int x = stack.back();
            stack.pop_back();
            for (const int y : boundary_adj[size_t(x)])
                if (!seen[size_t(y)]) {
                    seen[size_t(y)] = true;
                    stack.push_back(y);
                }
        }
    }
    std::vector<bool> disks(size_t(u.chart_count));
    for (int c = 0; c < u.chart_count; ++c)
        disks[size_t(c)] = int(verts[size_t(c)].size()) - int(edges[size_t(c)].size()) + faces[size_t(c)] == 1 &&
                           loops[size_t(c)] == 1;
    return disks;
}

TEST_CASE("TextureDisplacement: the default pipeline bakes an unwrap (LSCM) layer", "[TextureDisplacement]")
{
    // The one-run pipeline samples per point, and an unwrap has no per-point formula, so unwrap layers
    // used to be dropped from it altogether: the bake moved nothing, and the job then cleared the paint
    // as if it had baked - "paint, unwrap, bake, and the painted region just disappears".
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    TextureDisplacementLayer   layer;
    layer.slot              = 0;
    layer.image_data        = make_flat_gray_png(255);
    layer.depth_mm          = 0.5f;
    layer.tiling_scale      = 10.f;
    layer.projection_method = TextureProjectionMethod::LSCM;

    TextureDisplacementFacetsData facets;
    facets[0] = paint_whole_mesh(cube);

    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = 2.f;
    options.v2_max_triangles_k = 0;
    const indexed_triangle_set out = build_texture_displacement(cube, { layer }, facets, options);
    REQUIRE(!out.vertices.empty());

    // A flat white texture at depth 0.5 pushes the faces out by 0.5, so the bounding box grows on
    // every side (face interiors move the full depth; only corners, moving along their blended normal,
    // move less). Before the fix it stayed exactly [0, 10].
    Vec3f lo = out.vertices.front(), hi = lo;
    for (const Vec3f &v : out.vertices) {
        lo = lo.cwiseMin(v);
        hi = hi.cwiseMax(v);
    }
    CHECK(lo.x() < -0.4f);
    CHECK(hi.z() > 10.4f);
}

TEST_CASE("Per-corner LSCM UVs give each triangle its own island's placement", "[TextureDisplacement]")
{
    // compute_lscm_uvs() has to collapse a seam vertex onto one chart, because displacement is
    // per vertex. Everything that samples per *triangle* must not: a cube corner belongs to three
    // faces, so the collapse handed a triangle at an unjoined seam a neighbouring island's placement.
    // That showed up as one visibly skewed triangle per face, and as every island's texture following
    // the lowest-numbered island whenever it was dragged.
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const PatchUnwrap          unwrap = compute_patch_unwrap(cube, 30.f, 0.f);
    REQUIRE(unwrap.chart_count == 6);
    // The map back to the patch's own triangle order, without which there are no per-corner UVs.
    REQUIRE(unwrap.source_face.size() == unwrap.indices.size());

    TextureDisplacementLayer layer;
    layer.projection_method   = TextureProjectionMethod::LSCM;
    layer.lscm_seam_angle_deg = 30.f;
    layer.islands             = compute_connected_net(unwrap);
    REQUIRE(layer.islands.size() == 6);
    // Move one island by hand. Its neighbours must stay exactly where they were.
    layer.islands[0].offset += Vec2f(37.f, -19.f);

    const std::vector<Vec2f> corner = compute_lscm_corner_uvs(cube, layer);
    REQUIRE(corner.size() == cube.indices.size() * 3);

    for (size_t t = 0; t < unwrap.indices.size(); ++t) {
        const size_t f = size_t(unwrap.source_face[t]);
        REQUIRE(f < cube.indices.size());
        // A triangle lies in exactly one chart, so any of its corners names that chart.
        const int c = unwrap.vertex_chart[size_t(unwrap.indices[t][0])];
        for (int k = 0; k < 3; ++k) {
            const Vec2f want = apply_island_transform(unwrap.uvs[size_t(unwrap.indices[t][k])], c, unwrap, layer.islands);
            CHECK_THAT(corner[f * 3 + size_t(k)].x(), WithinAbs(want.x(), 1e-4));
            CHECK_THAT(corner[f * 3 + size_t(k)].y(), WithinAbs(want.y(), 1e-4));
        }
    }

    // And the per-vertex path must disagree somewhere - otherwise this test proves nothing, because
    // the bug it guards against is precisely that the two were the same thing.
    const std::vector<Vec2f> per_vertex = compute_lscm_uvs(cube, layer);
    REQUIRE(per_vertex.size() == cube.vertices.size());
    bool differs = false;
    for (size_t f = 0; f < cube.indices.size() && !differs; ++f)
        for (int k = 0; k < 3; ++k)
            if ((corner[f * 3 + size_t(k)] - per_vertex[size_t(cube.indices[f][k])]).norm() > 1e-3f)
                differs = true;
    CHECK(differs);
}

// ---------------------------------------------------------------------------------------------
// UV orientation: which way up a texture lands on the model.
//
// v = 0 is the height map's *first* pixel row, i.e. its top (DecodedHeightTexture::sample(), and
// the UV editor negates Y on screen for the same reason). So "the image is the right way up" is the
// statement that the row a point samples gets *lower* as the point moves up the face, and "it is
// not left-right mirrored" is that the column gets higher as the point moves to the right of a
// viewer standing outside the model. Both are measured below by sampling a ramp whose value says
// which row (or column) it came from, which settles the question without having to reason about the
// uv transform at all.

static constexpr int   RAMP_N      = 64;   // the ramp texture's size, in texels
static constexpr float RAMP_TILING = 40.f; // mm per tile
static constexpr float CUBE_HALF   = 5.f;  // half extent of the cube the probes walk over
// How far a probe steps off a face centre. Short on purpose: the cylindrical and spherical u is an
// arc length (angle * radius), so a long step also changes the radius, and that term swamps the
// angular one. It is symmetric about the face centre, so at a short step it cancels between the two
// readings a check compares.
static constexpr float RAMP_STEP   = 1.f;
// What one step's worth of travel is in ramp rows: 2 * RAMP_STEP mm of separation, RAMP_N texels per
// RAMP_TILING mm. Halved as the margin a check demands, so "flat" cannot pass as either direction.
static constexpr double RAMP_MARGIN = RAMP_N * RAMP_STEP / RAMP_TILING;

// A height map whose rows (or columns) carry a linear ramp: row r holds 3 + 4r, so the sampled
// height identifies the row. Built directly rather than through a PNG because these tests are about
// the projection, not the decoder - decode_height_texture() has its own tests.
static DecodedHeightTexture ramp_texture(bool by_row)
{
    DecodedHeightTexture t;
    t.width = t.height = RAMP_N;
    t.pixels.resize(size_t(RAMP_N) * size_t(RAMP_N));
    for (int y = 0; y < RAMP_N; ++y)
        for (int x = 0; x < RAMP_N; ++x)
            t.pixels[size_t(y) * size_t(RAMP_N) + size_t(x)] = uint8_t(3 + 4 * (by_row ? y : x));
    return t;
}

// uv 0.5 at the cube's centre, and a tile wide enough that every point the probes visit stays well
// inside the one tile - so no reading is a wrapped-around neighbour of the one it is compared with.
static TextureDisplacementLayer ramp_layer(TextureProjectionMethod method)
{
    TextureDisplacementLayer layer;
    layer.projection_method = method;
    layer.tiling_scale      = RAMP_TILING;
    layer.offset            = Vec2f(0.5f, 0.5f);
    layer.depth_mm          = 1.f;
    return layer;
}

// Which row (or column) of `ramp` the layer reads at `p`. Fractional, because a point between two
// texels reads the ramp between them; only the direction it moves in matters here.
static double ramp_index(const DecodedHeightTexture &ramp, const TextureDisplacementLayer &layer, const Vec3f &p,
                         const Vec3f &n, const Vec3f &center = Vec3f(Vec3f::Zero()),
                         const Vec3f &axis = Vec3f(Vec3f::UnitZ()))
{
    return (double(sample_layer_height(ramp, layer, p, n, center, axis, nullptr)) * 255.0 - 3.0) / 4.0;
}

// How a viewer standing outside the model sees a face: their right, and their up. Up is world +Z on
// the four upright faces; on the top and bottom, where +Z is edge on, it is world +Y.
static void outside_viewer_frame(const Vec3f &n, Vec3f &right, Vec3f &up)
{
    up    = (std::abs(n.z()) < 0.9f) ? Vec3f(0.f, 0.f, 1.f) : Vec3f(0.f, 1.f, 0.f);
    right = Vec3f((-n).cross(up).normalized());
}

static const std::array<std::pair<const char *, Vec3f>, 6> CUBE_FACES = { {
    { "+X", Vec3f(1.f, 0.f, 0.f) },
    { "-X", Vec3f(-1.f, 0.f, 0.f) },
    { "+Y", Vec3f(0.f, 1.f, 0.f) },
    { "-Y", Vec3f(0.f, -1.f, 0.f) },
    { "+Z", Vec3f(0.f, 0.f, 1.f) },
    { "-Z", Vec3f(0.f, 0.f, -1.f) },
} };

TEST_CASE("the box projection lands a texture upright and unmirrored on all six faces of a cube",
          "[TextureDisplacement]")
{
    const DecodedHeightTexture     rows  = ramp_texture(true);
    const DecodedHeightTexture     cols  = ramp_texture(false);
    const TextureDisplacementLayer layer = ramp_layer(TextureProjectionMethod::Triplanar);

    for (const auto &[name, n] : CUBE_FACES) {
        DYNAMIC_SECTION("face " << name)
        {
            Vec3f right, up;
            outside_viewer_frame(n, right, up);
            const Vec3f centre = n * CUBE_HALF;

            // Upright: the row read near the top of the face is nearer the image's first row.
            const double row_up   = ramp_index(rows, layer, centre + up * RAMP_STEP, n);
            const double row_down = ramp_index(rows, layer, centre - up * RAMP_STEP, n);
            INFO("row at the top of the face " << row_up << ", at the bottom " << row_down);
            CHECK(row_down - row_up > RAMP_MARGIN);

            // Unmirrored: the column read to the viewer's right is further into the image. Without a
            // per-side sign this fails on exactly the three faces facing away - -X, +Y and -Z - which
            // is the mirroring a bare v flip leaves behind.
            const double col_left  = ramp_index(cols, layer, centre - right * RAMP_STEP, n);
            const double col_right = ramp_index(cols, layer, centre + right * RAMP_STEP, n);
            INFO("column at the left of the face " << col_left << ", at the right " << col_right);
            CHECK(col_right - col_left > RAMP_MARGIN);
        }
    }
}

TEST_CASE("every projection method lands a texture upright on an upright face", "[TextureDisplacement]")
{
    const DecodedHeightTexture rows = ramp_texture(true);
    // ViewProjected is one flat projection from the captured camera rather than a per-face map, so its
    // "up" is the projector's own (view_project_up), which the default capture puts at +Y.
    const auto method = GENERATE(TextureProjectionMethod::Triplanar, TextureProjectionMethod::Cylindrical,
                                 TextureProjectionMethod::Spherical, TextureProjectionMethod::ViewProjected);
    const TextureDisplacementLayer layer = ramp_layer(method);

    for (const auto &[name, n] : CUBE_FACES) {
        // Only the four upright faces: on the top and bottom a cylinder's v is constant (they are its
        // caps) and a sphere's is at an extremum (they are its poles), so neither says anything about
        // which way up the image is.
        if (std::abs(n.z()) > 0.9f)
            continue;
        DYNAMIC_SECTION("method " << int(method) << ", face " << name)
        {
            const Vec3f up = (method == TextureProjectionMethod::ViewProjected) ? layer.view_project_up :
                                                                                 Vec3f(0.f, 0.f, 1.f);
            const Vec3f centre = n * CUBE_HALF;
            const double row_up   = ramp_index(rows, layer, centre + up * RAMP_STEP, n);
            const double row_down = ramp_index(rows, layer, centre - up * RAMP_STEP, n);
            INFO("row at the top " << row_up << ", at the bottom " << row_down);
            CHECK(row_down - row_up > RAMP_MARGIN);
        }
    }
}

TEST_CASE("the cylindrical and spherical wraps run left to right as seen from outside", "[TextureDisplacement]")
{
    const DecodedHeightTexture cols = ramp_texture(false);
    const auto method = GENERATE(TextureProjectionMethod::Cylindrical, TextureProjectionMethod::Spherical);
    const TextureDisplacementLayer layer = ramp_layer(method);

    for (const auto &[name, n] : CUBE_FACES) {
        // The caps and poles are out, as above - u is undefined on the axis. So is whichever face the
        // atan2 branch cut runs through, where u jumps from +pi to -pi: that is a real seam in both
        // projections, and a reading taken across it compares two opposite sides of the model.
        if (std::abs(n.z()) > 0.9f)
            continue;
        const bool on_the_cut = (method == TextureProjectionMethod::Cylindrical) ? (n.y() < -0.9f) : (n.x() < -0.9f);
        if (on_the_cut)
            continue;
        DYNAMIC_SECTION("method " << int(method) << ", face " << name)
        {
            Vec3f right, up;
            outside_viewer_frame(n, right, up);
            const Vec3f  centre    = n * CUBE_HALF;
            const double col_left  = ramp_index(cols, layer, centre - right * RAMP_STEP, n);
            const double col_right = ramp_index(cols, layer, centre + right * RAMP_STEP, n);
            INFO("column at the left " << col_left << ", at the right " << col_right);
            CHECK(col_right - col_left > RAMP_MARGIN);
        }
    }
}

TEST_CASE("an unwrapped cube's flat charts are laid out the right way up", "[TextureDisplacement]")
{
    indexed_triangle_set cube = its_make_cube(2. * CUBE_HALF, 2. * CUBE_HALF, 2. * CUBE_HALF);
    for (Vec3f &v : cube.vertices)
        v -= Vec3f(CUBE_HALF, CUBE_HALF, CUBE_HALF);

    const PatchUnwrap unwrap = compute_patch_unwrap(cube, LSCM_DEFAULT_SEAM_ANGLE_DEG);
    REQUIRE_FALSE(unwrap.empty());
    REQUIRE(unwrap.chart_count == 6); // one flat chart per face, cut at the 90-degree edges

    // Each chart carries one face. The four upright ones span world Z, and their v has to grow
    // downwards like every other projection's - whatever the packing then did with the island.
    int upright_charts = 0;
    for (int c = 0; c < unwrap.chart_count; ++c) {
        int top = -1, bottom = -1;
        for (size_t i = 0; i < unwrap.uvs.size(); ++i) {
            if (unwrap.vertex_chart[i] != c)
                continue;
            const float z = cube.vertices[size_t(unwrap.source_vertex[i])].z();
            if (top < 0 || z > cube.vertices[size_t(unwrap.source_vertex[size_t(top)])].z())
                top = int(i);
            if (bottom < 0 || z < cube.vertices[size_t(unwrap.source_vertex[size_t(bottom)])].z())
                bottom = int(i);
        }
        REQUIRE(top >= 0);
        REQUIRE(bottom >= 0);
        const float v_top = unwrap.uvs[size_t(top)].y(), v_bottom = unwrap.uvs[size_t(bottom)].y();
        if (std::abs(v_top - v_bottom) < 1e-3f)
            continue; // a cap: the whole chart is at one height, so it has no up
        ++upright_charts;
        DYNAMIC_SECTION("chart " << c)
        {
            INFO("v at the chart's topmost vertex " << v_top << ", at its bottommost " << v_bottom);
            CHECK(v_bottom - v_top > 1.f);
        }
    }
    CHECK(upright_charts == 4); // the four sides; the top and bottom faces are the caps skipped above
}

TEST_CASE("project_planar agrees with the box projection's own axes", "[TextureDisplacement]")
{
    // The two express the same table in different shapes - one a pair of world axes for the GUI, the
    // other the closed form the bake's hot path uses - so they are checked against each other here
    // rather than left to drift apart.
    const Vec3f p(2.f, -3.f, 7.f);
    for (const auto &[name, n] : CUBE_FACES) {
        DYNAMIC_SECTION("face " << name)
        {
            Vec3f u_axis, v_axis;
            box_projection_axes(n, u_axis, v_axis);
            const Vec2f planar = project_planar(p, n);
            CHECK_THAT(planar.x(), WithinAbs(p.dot(u_axis), 1e-5f));
            CHECK_THAT(planar.y(), WithinAbs(p.dot(v_axis), 1e-5f));
            // Right-handed for a viewer outside the model: u to their right, v down, so u x v points
            // into the surface. Getting this wrong is exactly what mirrors a face.
            CHECK_THAT(u_axis.cross(v_axis).dot(n), WithinAbs(-1.f, 1e-5f));
        }
    }
}

// An image with a different value in each quadrant, so one bake says both which way up it landed
// and whether it came out mirrored. Distinct enough that a wrong quadrant cannot be mistaken for the
// right one: top-left 255, top-right 170, bottom-left 85, bottom-right 0.
static std::shared_ptr<std::vector<unsigned char>> make_quadrant_marker_png(size_t w = 32, size_t h = 32)
{
    std::vector<uint8_t> pixels(w * h);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x)
            pixels[y * w + x] = (y < h / 2) ? ((x < w / 2) ? 255 : 170) : ((x < w / 2) ? 85 : 0);
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), w, h, pixels));

    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);

    REQUIRE_FALSE(bytes.empty());
    return std::make_shared<std::vector<unsigned char>>(std::move(bytes));
}

TEST_CASE("baking an asymmetric image puts the top of the image at the top of the model", "[TextureDisplacement]")
{
    // An upright wall in the plane y = 0, facing -Y, tessellated so that each of the image's four
    // quadrants gets a vertex at its centre. A flat sheet rather than a cube because every vertex
    // normal is then exactly the face normal, which is what makes the expected depth below an exact
    // number instead of a tri-planar blend of three of them.
    constexpr int   STEPS = 4;    // cells per side, so 5x5 vertices
    constexpr float SPAN  = 20.f; // mm, the wall's width and height
    indexed_triangle_set wall;
    for (int j = 0; j <= STEPS; ++j)
        for (int i = 0; i <= STEPS; ++i)
            wall.vertices.emplace_back(-SPAN / 2.f + SPAN * float(i) / float(STEPS), 0.f,
                                       SPAN * float(j) / float(STEPS));
    const auto at = [](int i, int j) { return j * (STEPS + 1) + i; };
    for (int j = 0; j < STEPS; ++j)
        for (int i = 0; i < STEPS; ++i) {
            // Wound so that (+x) x (+z) = -y: the wall faces -Y, away from the origin.
            wall.indices.emplace_back(at(i, j), at(i + 1, j), at(i + 1, j + 1));
            wall.indices.emplace_back(at(i, j), at(i + 1, j + 1), at(i, j + 1));
        }

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(wall);

    TextureDisplacementLayer layer;
    layer.slot              = 0;
    layer.projection_method = TextureProjectionMethod::Triplanar;
    layer.image_data        = make_quadrant_marker_png();
    layer.depth_mm          = 2.f;
    // The wall is 20 mm across and one tile is 40 mm, so it covers the middle half of the image in
    // both axes: the quadrant boundaries land on x = 0 and z = 10, and the probed vertices sit a
    // whole number of texels away from both.
    layer.tiling_scale      = 40.f;
    layer.offset            = Vec2f(0.5f, 0.75f);
    // A single decal rather than a repeat, so that getting the orientation wrong cannot be masked by
    // the wrap bringing the flipped coordinate back into the image at a plausible-looking row. With
    // tiling off, uv stays inside [0.25, 0.75] in both axes only while v runs the right way.
    layer.tile_enabled      = false;

    const indexed_triangle_set result = build_texture_displacement(wall, { layer }, facets, classic_options());
    REQUIRE(result.vertices.size() == wall.vertices.size());

    // Seen from outside this face the viewer's right is +X and their up is +Z, so the image's
    // top-left quadrant has to land at low x and high z. Each entry: the vertex, and the image value
    // whose quadrant it must have read.
    const std::array<std::tuple<const char *, int, int, int>, 4> expected = { {
        { "top-left (255)", 1, 3, 255 },
        { "top-right (170)", 3, 3, 170 },
        { "bottom-left (85)", 1, 1, 85 },
        { "bottom-right (0)", 3, 1, 0 },
    } };

    for (const auto &[name, i, j, value] : expected) {
        DYNAMIC_SECTION("quadrant " << name)
        {
            const size_t vi = size_t(at(i, j));
            // The whole displacement is along -Y, so this is the relief's height at that vertex.
            const float raised = wall.vertices[vi].y() - result.vertices[vi].y();
            INFO("vertex at " << wall.vertices[vi].x() << ", " << wall.vertices[vi].z());
            CHECK_THAT(raised, WithinAbs(layer.depth_mm * float(value) / 255.f, 1e-3f));
            // Nothing moved sideways or vertically: a mirrored or rotated basis would still have
            // displaced along the normal, so this is here to catch the relief leaning instead.
            CHECK_THAT(result.vertices[vi].x(), WithinAbs(wall.vertices[vi].x(), 1e-4f));
            CHECK_THAT(result.vertices[vi].z(), WithinAbs(wall.vertices[vi].z(), 1e-4f));
        }
    }
}

TEST_CASE("The Cylindrical/Spherical patch frame is the bake's own, so a preview can share it", "[TextureDisplacement]")
{
    // The fast preview reconstructs these two projections in the fragment shader and needs the very
    // centroid and axis the bake wraps around - a patch-only normal average would sometimes quantize
    // to a different world axis and wrap the texture the other way round.
    const indexed_triangle_set cube    = its_make_cube(10., 10., 10.);
    const std::vector<Vec3f>   normals = texture_displacement_vertex_normals(cube);
    REQUIRE(normals.size() == cube.vertices.size());

    // Just the +X face: its average normal is +X, so the cylinder axis must be a world axis
    // perpendicular to it, and the centroid must sit on that face.
    indexed_triangle_set face;
    face.vertices = cube.vertices;
    for (const stl_triangle_vertex_indices &t : cube.indices) {
        const Vec3f n = (cube.vertices[size_t(t[1])] - cube.vertices[size_t(t[0])])
                            .cross(cube.vertices[size_t(t[2])] - cube.vertices[size_t(t[0])]);
        if (n.normalized().x() > 0.99f)
            face.indices.push_back(t);
    }
    REQUIRE(face.indices.size() == 2);

    Vec3f center, axis, average_normal;
    texture_displacement_patch_frame(face, normals, center, axis, average_normal);
    CHECK_THAT(center.x(), WithinAbs(10., 1e-4));
    CHECK_THAT(std::abs(axis.x()), WithinAbs(0., 1e-4)); // never the face's own normal direction
    CHECK_THAT(axis.norm(), WithinAbs(1., 1e-4));

    // An empty patch must not divide by zero; it falls back to +Z.
    texture_displacement_patch_frame(indexed_triangle_set{}, normals, center, axis, average_normal);
    CHECK_THAT(center.norm(), WithinAbs(0., 1e-6));
    CHECK_THAT(axis.z(), WithinAbs(1., 1e-6));
}

TEST_CASE("A cube unwraps into one island per face, laid out as a connected net", "[TextureDisplacement]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    const PatchUnwrap          unwrap = compute_patch_unwrap(cube, 30.f, 0.f);
    // Two triangles per face, and every face edge but the diagonals is a 90 degree crease.
    REQUIRE(unwrap.chart_count == 6);

    const std::vector<TextureIsland> islands = compute_connected_net(unwrap);
    REQUIRE(islands.size() == 6);

    std::vector<std::array<Vec2f, 3>> placed;
    std::vector<int>                  placed_chart;
    for (const stl_triangle_vertex_indices &tri : unwrap.indices) {
        const int            c = unwrap.vertex_chart[size_t(tri[0])];
        std::array<Vec2f, 3> t;
        for (int k = 0; k < 3; ++k)
            t[size_t(k)] = apply_island_transform(unwrap.uvs[size_t(tri[k])], c, unwrap, islands);
        placed.push_back(t);
        placed_chart.push_back(c);
    }

    SECTION("no two faces overlap")
    {
        // A 10 mm cube: the six 100 mm2 faces laid flat without overlap cover 600 mm2, which triangles overlapping
        // anywhere could not add up to within their joint bounding box... so test it directly, by sampling.
        float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
        for (const auto &t : placed)
            for (const Vec2f &p : t) {
                lo_x = std::min(lo_x, p.x());
                lo_y = std::min(lo_y, p.y());
                hi_x = std::max(hi_x, p.x());
                hi_y = std::max(hi_y, p.y());
            }
        const auto inside = [](const std::array<Vec2f, 3> &t, const Vec2f &p) {
            const auto cross = [](const Vec2f &a, const Vec2f &b) { return a.x() * b.y() - a.y() * b.x(); };
            const float d0 = cross(t[1] - t[0], p - t[0]), d1 = cross(t[2] - t[1], p - t[1]), d2 = cross(t[0] - t[2], p - t[2]);
            return (d0 > 1e-3f && d1 > 1e-3f && d2 > 1e-3f) || (d0 < -1e-3f && d1 < -1e-3f && d2 < -1e-3f);
        };
        int overlapping = 0;
        for (float x = lo_x + 0.13f; x < hi_x; x += 0.5f)
            for (float y = lo_y + 0.17f; y < hi_y; y += 0.5f) {
                int covering = 0;
                for (const auto &t : placed)
                    covering += inside(t, Vec2f(x, y)) ? 1 : 0;
                overlapping += covering > 1 ? 1 : 0;
            }
        CHECK(overlapping == 0);
    }

    SECTION("every face shares an edge with another face of the net")
    {
        std::vector<bool> joined(6, false);
        for (size_t i = 0; i < placed.size(); ++i)
            for (size_t j = 0; j < placed.size(); ++j) {
                if (placed_chart[i] == placed_chart[j])
                    continue;
                int shared_corners = 0;
                for (const Vec2f &p : placed[i])
                    for (const Vec2f &q : placed[j])
                        shared_corners += (p - q).norm() < 1e-3f ? 1 : 0;
                if (shared_corners >= 2)
                    joined[size_t(placed_chart[i])] = true;
            }
        for (int c = 0; c < 6; ++c)
            CHECK(joined[size_t(c)]);
    }
}

TEST_CASE("Unwrapping a closed cylinder cuts its side until every island is a disk", "[TextureDisplacement]")
{
    // The side is smooth, so the seam angle alone leaves it as one ring-shaped chart, which cannot be laid flat.
    const indexed_triangle_set cylinder = its_make_cylinder(5., 20., 2. * PI / 36.);
    const PatchUnwrap          unwrap   = compute_patch_unwrap(cylinder, 30.f, 0.f);
    REQUIRE(unwrap.chart_count >= 4); // two caps and at least two side pieces

    const std::vector<bool> disks = unwrap_charts_are_disks(unwrap);
    for (int c = 0; c < unwrap.chart_count; ++c)
        CHECK(disks[size_t(c)]);
}

TEST_CASE("A UV edit on one copy of a seam vertex leaves its other copies alone", "[TextureDisplacement]")
{
    const indexed_triangle_set cube   = its_make_cube(10., 10., 10.);
    PatchUnwrap                unwrap = compute_patch_unwrap(cube, 30.f, 0.f);

    // A cube corner has a copy in each of the three faces around it.
    const int                 edited_copy = 0;
    const int                 mesh_vertex = unwrap.source_vertex[size_t(edited_copy)];
    std::vector<int>          other_copies;
    for (size_t i = 0; i < unwrap.uvs.size(); ++i)
        if (int(i) != edited_copy && unwrap.source_vertex[i] == mesh_vertex)
            other_copies.push_back(int(i));
    REQUIRE_FALSE(other_copies.empty());

    const std::vector<Vec2f> before = unwrap.uvs;
    const Vec2f              target = before[size_t(edited_copy)] + Vec2f(3.f, -2.f);
    const std::vector<bool>  edited = apply_lscm_uv_overrides(unwrap, { { lscm_uv_override_key(edited_copy), target } });

    CHECK_THAT(unwrap.uvs[size_t(edited_copy)].x(), WithinAbs(target.x(), 1e-6));
    CHECK_THAT(unwrap.uvs[size_t(edited_copy)].y(), WithinAbs(target.y(), 1e-6));
    CHECK(edited[size_t(edited_copy)]);
    for (const int i : other_copies) {
        CHECK_THAT(unwrap.uvs[size_t(i)].x(), WithinAbs(before[size_t(i)].x(), 1e-6));
        CHECK_THAT(unwrap.uvs[size_t(i)].y(), WithinAbs(before[size_t(i)].y(), 1e-6));
        CHECK_FALSE(edited[size_t(i)]);
    }
}

TEST_CASE("TextureDisplacement: moving the model about the plate does not move the texture on it", "[TextureDisplacement]")
{
    // The bake projects in world millimetres but anchored at the volume's origin, so an instance
    // translated across the plate bakes exactly the relief the same instance bakes at the origin. The
    // classic path keeps the topology, so the comparison is vertex by vertex.
    const indexed_triangle_set cube = subdivide_mesh_uniform(its_make_cube(10.f, 10.f, 10.f), 1.f, 6);
    TextureDisplacementLayer   layer;
    layer.slot         = 0;
    layer.image_data   = make_checkerboard_png(16, 16);
    layer.tiling_scale = 4.f;
    layer.depth_mm     = 0.5f;
    TextureDisplacementFacetsData facets;
    facets[0] = paint_whole_mesh(cube);

    const indexed_triangle_set at_origin = build_texture_displacement(cube, { layer }, facets, classic_options());
    const indexed_triangle_set moved     = build_texture_displacement(cube, { layer }, facets, classic_options(), {}, nullptr,
                                                                      Transform3d(Eigen::Translation3d(123.4, -56.7, 0.0)));
    REQUIRE(moved.vertices.size() == at_origin.vertices.size());
    float max_diff = 0.f;
    for (size_t i = 0; i < moved.vertices.size(); ++i)
        max_diff = std::max(max_diff, (moved.vertices[i] - at_origin.vertices[i]).norm());
    CHECK(max_diff < 1e-3f);
}

TEST_CASE("Parts anchored at one point carry one continuous texture", "[TextureDisplacement]")
{
    // The same surface, once as a part whose origin is the anchor and once as a part whose own origin
    // sits `offset` away from it - the way the bodies of a multibody object are each centred on their
    // own origin. Anchored at the same point they must bake the same relief; anchored at their own
    // origins, the second is the tiled pattern shifted by `offset`.
    const indexed_triangle_set cube = subdivide_mesh_uniform(its_make_cube(10.f, 10.f, 10.f), 1.f, 6);
    // Not a multiple of the 4 mm tile, and exact in binary like the grid itself: the checkerboard's steps
    // fall right on grid vertices, so a vertex rounded across a step would bake the other side of it.
    const Vec3d                offset(1.25, 2.5, 0.75);
    indexed_triangle_set       shifted = cube;
    for (Vec3f &v : shifted.vertices)
        v -= offset.cast<float>();
    const Transform3d shifted_to_world = Transform3d(Eigen::Translation3d(offset));

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.image_data   = make_checkerboard_png(16, 16);
    layer.tiling_scale = 4.f;
    layer.depth_mm     = 0.5f;
    TextureDisplacementFacetsData facets;
    facets[0] = paint_whole_mesh(cube);

    const indexed_triangle_set reference = build_texture_displacement(cube, { layer }, facets, classic_options());
    const Vec3d                anchor    = Vec3d::Zero();
    const indexed_triangle_set anchored  = build_texture_displacement(shifted, { layer }, facets, classic_options(), {},
                                                                      nullptr, shifted_to_world, nullptr, nullptr,
                                                                      nullptr, &anchor);
    const indexed_triangle_set own_origin = build_texture_displacement(shifted, { layer }, facets, classic_options(), {},
                                                                       nullptr, shifted_to_world);
    REQUIRE(anchored.vertices.size() == reference.vertices.size());
    REQUIRE(own_origin.vertices.size() == reference.vertices.size());

    float anchored_diff = 0.f, own_origin_diff = 0.f;
    for (size_t i = 0; i < reference.vertices.size(); ++i) {
        anchored_diff   = std::max(anchored_diff, (anchored.vertices[i] + offset.cast<float>() - reference.vertices[i]).norm());
        own_origin_diff = std::max(own_origin_diff, (own_origin.vertices[i] + offset.cast<float>() - reference.vertices[i]).norm());
    }
    CHECK(anchored_diff < 1e-3f);
    CHECK(own_origin_diff > 0.1f);
}

TEST_CASE("A part held above the plate keeps the relief on its underside", "[TextureDisplacement]")
{
    // A fully painted cube displaces its bottom face straight down by the full depth. On its own, that
    // relief is clamped to the cube's bottom (see "relief is never driven below the model's resting
    // plane"); as a part of an object resting `drop` below it, it is clamped only at the object's plane.
    const bool                 v2   = GENERATE(false, true);
    const indexed_triangle_set cube = subdivide_mesh_uniform(its_make_cube(10.f, 10.f, 10.f), 2.f, 6);

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);
    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 2.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data   = make_flat_gray_png(255); // full depth everywhere

    TextureDisplacementOptions options = classic_options();
    options.pipeline_v2                = v2;
    options.v2_refine_mm               = 2.f;
    options.v2_max_triangles_k         = 0;

    const auto lowest_after = [&](float drop) {
        const indexed_triangle_set out = build_texture_displacement(cube, { layer }, facets, options, {}, nullptr,
                                                                    Transform3d::Identity(), nullptr, nullptr, nullptr,
                                                                    nullptr, drop);
        REQUIRE_FALSE(out.vertices.empty());
        float lowest = std::numeric_limits<float>::max();
        for (const Vec3f &v : out.vertices)
            lowest = std::min(lowest, v.z());
        return lowest;
    };

    CAPTURE(v2);
    // The cube's bottom is z = 0. With the object resting 5 mm lower, the 2 mm relief fits above it...
    CHECK_THAT(lowest_after(5.f), WithinAbs(-2.0f, 1e-3f));
    // ...with the object resting 1 mm lower, it is clamped at that plane...
    CHECK_THAT(lowest_after(1.f), WithinAbs(-1.0f, 1e-3f));
    // ...and as the lowest part, at its own bottom.
    CHECK_THAT(lowest_after(0.f), WithinAbs(0.0f, 1e-3f));
}

// Longest edge over the shortest altitude: 1.15 for an equilateral triangle, 2 for a right isosceles
// one, unbounded for a needle.
static float triangle_aspect(const Vec3f &a, const Vec3f &b, const Vec3f &c)
{
    const float longest    = std::max({ (b - a).norm(), (c - b).norm(), (a - c).norm() });
    const float twice_area = (b - a).cross(c - a).norm();
    return twice_area > 0.f ? longest * longest / twice_area : std::numeric_limits<float>::infinity();
}

TEST_CASE("TextureDisplacement: baking a second face leaves the first face's relief untouched", "[TextureDisplacement]")
{
    // Paint the top of a cube and bake, then paint the front of the *result* and bake again, the way
    // the gizmo does (each bake replaces the mesh and clears the baked paint). The top is unpainted the
    // second time round, so it is excluded from refinement and pinned by the displacement: every
    // vertex of its relief must still be there, in the same number of triangles, and no needle may
    // appear on it.
    const indexed_triangle_set cube = subdivide_mesh_uniform(its_make_cube(20., 20., 20.), 2.f, 6);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.image_data   = make_checkerboard_png(16, 16);
    layer.tiling_scale = 5.f;
    layer.depth_mm     = 0.5f;

    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = 0.5f;
    options.v2_max_triangles_k = 0; // no simplification

    // Paints exactly the triangles whose three corners satisfy `on_face` - no brush spill.
    const auto paint_where = [](const indexed_triangle_set &mesh, auto on_face) {
        const TriangleMesh tm(mesh);
        TriangleSelector   selector(tm);
        for (size_t f = 0; f < mesh.indices.size(); ++f) {
            const stl_triangle_vertex_indices &t = mesh.indices[f];
            if (on_face(mesh.vertices[size_t(t[0])]) && on_face(mesh.vertices[size_t(t[1])]) &&
                on_face(mesh.vertices[size_t(t[2])]))
                selector.set_facet(int(f), EnforcerBlockerType::ENFORCER);
        }
        return selector.serialize();
    };
    const auto on_top   = [](const Vec3f &v) { return v.z() > 19.9f; };
    const auto on_front = [](const Vec3f &v) { return v.y() < 0.1f; };

    // The top region of a result: its vertices, and its triangles' count and worst aspect ratio.
    struct TopRegion
    {
        std::vector<Vec3f> vertices;
        size_t             triangles  = 0;
        float              max_aspect = 0.f;
    };
    const auto top_region = [&on_top](const indexed_triangle_set &mesh) {
        TopRegion r;
        for (const Vec3f &v : mesh.vertices)
            if (on_top(v))
                r.vertices.push_back(v);
        for (const stl_triangle_vertex_indices &t : mesh.indices) {
            const Vec3f &a = mesh.vertices[size_t(t[0])], &b = mesh.vertices[size_t(t[1])], &c = mesh.vertices[size_t(t[2])];
            if (!on_top(a) || !on_top(b) || !on_top(c))
                continue;
            ++r.triangles;
            r.max_aspect = std::max(r.max_aspect, triangle_aspect(a, b, c));
        }
        return r;
    };

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_where(cube, on_top);
    const indexed_triangle_set first = build_texture_displacement(cube, { layer }, facets, options);
    REQUIRE(first.indices.size() > cube.indices.size());
    const TopRegion top_before = top_region(first);
    REQUIRE(top_before.triangles > 0);
    // The relief really is there: the checkerboard raises part of the top by the full depth.
    float top_z_max = 0.f;
    for (const Vec3f &v : top_before.vertices)
        top_z_max = std::max(top_z_max, v.z());
    REQUIRE(top_z_max > 20.3f);
    REQUIRE(top_before.max_aspect < 20.f);

    // Only the front of the baked mesh is painted for the second bake.
    facets[0] = paint_where(first, on_front);
    REQUIRE_FALSE(facets[0].triangles_to_split.empty());

    const auto check_top_untouched = [&](const indexed_triangle_set &second) {
        REQUIRE_FALSE(second.indices.empty()); // an aborted bake returns {}
        const TopRegion top_after = top_region(second);

        // Every vertex of the first relief still exists, at the same place. O(n*m) over a few
        // thousand vertices each, restricted to the top region on both sides.
        size_t missing = 0;
        Vec3f  first_missing = Vec3f::Zero();
        for (const Vec3f &v : top_before.vertices) {
            bool found = false;
            for (const Vec3f &w : top_after.vertices)
                if ((w - v).squaredNorm() <= 1e-6f) { // within 1e-3 mm
                    found = true;
                    break;
                }
            if (!found) {
                if (missing == 0)
                    first_missing = v;
                ++missing;
            }
        }
        INFO("first vertex of the top relief missing from the second bake: " << first_missing.transpose());
        CHECK(missing == 0);

        // Nothing was added to or taken from the top either - its triangles are excluded from the
        // refinement, and a rim edge it shares with the front was already at the refine length.
        CHECK(top_after.triangles == top_before.triangles);

        // And no needles: the worst triangle on the top is no worse than after the first bake.
        INFO("worst top-face aspect ratio after the second bake: " << top_after.max_aspect
             << ", after the first: " << top_before.max_aspect);
        CHECK(top_after.max_aspect < 20.f);
    };

    SECTION("bake mode: no simplification")
    {
        check_top_untouched(build_texture_displacement(first, { layer }, facets, options));
    }

    SECTION("export mode: simplification and T-junction repair run over the excluded region too")
    {
        // A budget far below the mesh forces the decimation (the locked top alone is over it, so it
        // only harvests flat faces) and with it the repair pass - the two stages that walk every face,
        // excluded ones included.
        TextureDisplacementOptions export_options = options;
        export_options.v2_max_triangles_k         = 1;
        check_top_untouched(build_texture_displacement(first, { layer }, facets, export_options));
    }
}

TEST_CASE("TextureDisplacement: a brush stroke smaller than a triangle displaces only the stroke", "[TextureDisplacement]")
{
    // A plain 12-triangle cube. The selector splits the top triangle under a 3 mm spherical brush,
    // so the painted pieces are far smaller than the triangle. The one-run pipeline used to include
    // the whole source triangle: the entire top face rose.
    const indexed_triangle_set    cube = its_make_cube(20.f, 20.f, 20.f);
    TextureDisplacementFacetsData facets;
    facets[0] = paint_top_disc(cube, 20.f, Vec2f(10.f, 10.f), 3.f);
    REQUIRE(TriangleSelector::has_facets(facets[0], EnforcerBlockerType::ENFORCER));

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.image_data   = make_flat_gray_png(255, 8, 8); // uniform full height: every painted point rises
    layer.tiling_scale = 4.f;
    layer.depth_mm     = 0.5f;
    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = 0.5f;
    options.v2_max_triangles_k = 0;
    const indexed_triangle_set out = build_texture_displacement(cube, { layer }, facets, options);
    REQUIRE(out.indices.size() > cube.indices.size());

    size_t raised_inside = 0, raised_outside = 0, outside = 0;
    for (const Vec3f &v : out.vertices) {
        if (v.z() < 19.9f)
            continue; // not the top face
        const float r = (Vec2f(v.x(), v.y()) - Vec2f(10.f, 10.f)).norm();
        if (r < 2.f && v.z() > 20.3f)
            ++raised_inside;
        if (r > 4.5f) {
            ++outside;
            if (v.z() > 20.01f)
                ++raised_outside;
        }
    }
    CHECK(raised_inside > 0);   // the stroke itself is displaced
    CHECK(outside > 0);
    CHECK(raised_outside == 0); // the rest of the face, inside the same source triangle, is not
}

TEST_CASE("TextureDisplacement: a texture of flat colours is told apart from a continuous one", "[TextureDisplacement]")
{
    // The verdict that decides whether a layer may use filament mixes: a checkerboard is two colours,
    // a ramp spreads over every level.
    TextureDisplacementLayer checker;
    checker.image_data = make_checkerboard_png(32, 32);
    CHECK(analyze_texture_detail(checker).flat_colors);

    std::vector<uint8_t> ramp(64 * 64);
    for (size_t y = 0; y < 64; ++y)
        for (size_t x = 0; x < 64; ++x)
            ramp[y * 64 + x] = uint8_t((x * 4 + y) & 255);
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), 64, 64, ramp));
    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);
    TextureDisplacementLayer gradient;
    gradient.image_data = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    const TextureDetail d = analyze_texture_detail(gradient);
    CHECK_FALSE(d.flat_colors);
    CHECK(d.flat_share < 0.85f);
}

TEST_CASE("Each layer's texture is sampled only on its own painted area", "[TextureDisplacement]")
{
    // Two layers with different textures and depths, one painted on the cube's top, one on its -X side.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const TriangleMesh         cube_mesh(cube);
    const auto paint_facing = [&](const Vec3f &dir) {
        TriangleSelector selector(cube_mesh);
        for (int f = 0; f < int(cube.indices.size()); ++f) {
            const stl_triangle_vertex_indices &t = cube.indices[size_t(f)];
            const Vec3f n = (cube.vertices[size_t(t[1])] - cube.vertices[size_t(t[0])])
                                .cross(cube.vertices[size_t(t[2])] - cube.vertices[size_t(t[0])])
                                .normalized();
            if (n.dot(dir) > 0.99f)
                selector.set_facet(f, EnforcerBlockerType::ENFORCER);
        }
        return selector.serialize();
    };
    TextureDisplacementFacetsData facets{};
    facets[0] = paint_facing(Vec3f::UnitZ());
    facets[1] = paint_facing(-Vec3f::UnitX());

    TextureDisplacementLayer top;
    top.slot         = 0;
    top.depth_mm     = 1.0f;
    top.tiling_scale = 5.0f;
    top.image_data   = make_flat_gray_png(255);
    TextureDisplacementLayer side = top;
    side.slot       = 1;
    side.depth_mm   = 0.5f;
    side.image_data = make_flat_gray_png(64);

    const HeightFieldSampler both      = make_combined_displacement_sampler(cube, { top, side }, facets);
    const HeightFieldSampler top_only  = make_combined_displacement_sampler(cube, { top }, facets);
    const HeightFieldSampler side_only = make_combined_displacement_sampler(cube, { side }, facets);
    REQUIRE(both);
    REQUIRE(top_only);
    REQUIRE(side_only);

    const Vec3f on_top(5.f, 5.f, 10.f), on_side(0.f, 5.f, 5.f), unpainted(5.f, 10.f, 5.f);
    CHECK_THAT(both(on_top, Vec3f::UnitZ()), WithinAbs(top_only(on_top, Vec3f::UnitZ()), 1e-5f));
    CHECK_THAT(both(on_side, -Vec3f::UnitX()), WithinAbs(side_only(on_side, -Vec3f::UnitX()), 1e-5f));
    CHECK_THAT(both(unpainted, Vec3f::UnitY()), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A budget met by collapsing flat faces alone removes no detail", "[TextureDisplacement]")
{
    // 12 * 4^5 = 12288 triangles on six flat faces, which can go down to 12 without moving the surface.
    const TextureBake::TriSoup cube = TextureBake::to_soup(subdivide_mesh_uniform(its_make_cube(20., 20., 20.), 0.f, 5));
    const TextureBake::DecimateResult res = TextureBake::decimate(cube, 1000, true, 0.005);
    CHECK(res.geometry.triangle_count() <= 1000);
    CHECK_FALSE(res.target_cost_detail);
}

TEST_CASE("A budget met only by flattening curvature removes detail", "[TextureDisplacement]")
{
    // A 10 degree sphere of radius 10 bulges about 0.04 mm out of each facet, far past a 0.005 mm tolerance.
    const TextureBake::TriSoup sphere = TextureBake::to_soup(its_make_sphere(10., PI / 18.));
    const TextureBake::DecimateResult res = TextureBake::decimate(sphere, 100, true, 0.005);
    CHECK(res.geometry.triangle_count() <= 100);
    CHECK(res.target_cost_detail);
}

TEST_CASE("Cancelling while flat faces are harvested past the budget stops the decimation", "[TextureDisplacement]")
{
    // 12 * 4^6 = 49152 triangles: the budget is met almost at once, and the harvest after it runs to
    // a dozen triangles over several times the two polling intervals a cancel takes to be seen.
    const TextureBake::TriSoup cube   = TextureBake::to_soup(subdivide_mesh_uniform(its_make_cube(20., 20., 20.), 0.f, 6));
    const size_t               target = cube.triangle_count() - 1000;
    const size_t full = TextureBake::decimate(cube, target, true, 0.005).geometry.triangle_count();

    // The Cancel button pressed once the bar is full: every call after the first to report 1.0 cancels.
    bool       full_seen = false;
    const auto cancel_when_full = [&full_seen](double f) {
        if (full_seen)
            return false;
        full_seen = f >= 1.0;
        return true;
    };
    const size_t canceled = TextureBake::decimate(cube, target, true, 0.005, {}, cancel_when_full).geometry.triangle_count();
    CHECK(full_seen);
    CHECK(canceled <= target);
    CHECK(canceled > full);
}

TEST_CASE("Baking twice keeps the mesh closed and carries the first bake's colours across",
          "[TextureDisplacement][Regression]")
{
    // Bake, colour part of what came back, bake again. The one-run pipeline rebuilds the topology, so
    // the colour recorded against the first bake's mesh indexes triangles that no longer mean the same
    // thing. Taken across by triangle id it lands on unrelated faces - a bake almost always grows the
    // triangle count, so every stale id passes the only bound check deserialize() makes - which both
    // scatters colour over surfaces nothing painted and destroys what the first pass laid down.
    // transfer_facet_paint() carries it across by position instead - every triangle of the new mesh
    // takes the state painted at its foot point on the old one - which is what makes successive bakes
    // accumulate. See TextureDisplacementBakeJob, which is where the two meet.
    constexpr float            side    = 20.f;
    constexpr float            refine  = 0.5f;
    constexpr float            radius  = 4.f;
    const indexed_triangle_set cube    = its_make_cube(side, side, side);
    const Vec2f                first_stroke(6.f, 6.f), second_stroke(14.f, 14.f);

    TextureDisplacementLayer layer;
    layer.slot          = 0;
    layer.image_data    = make_rgb_png_2x2();
    layer.tiling_scale  = 5.f;
    layer.depth_mm      = 0.5f;
    layer.color_enabled = true;

    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = refine;
    options.v2_max_triangles_k = 750;

    // Three primaries, so a palette entry is legible in the assertions below.
    const std::array<Vec3f, 3> palette = { Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(0, 0, 1) };
    TextureColorRequest        request;
    request.quantize = [&palette](const Vec3f &rgb) {
        int   best = 0;
        float bd   = std::numeric_limits<float>::max();
        for (int i = 0; i < 3; ++i)
            if (const float d = (palette[size_t(i)] - rgb).squaredNorm(); d < bd) { bd = d; best = i; }
        return best;
    };

    const auto bake = [&](const indexed_triangle_set &mesh, const Vec2f &stroke, std::vector<uint8_t> &color) {
        TextureDisplacementFacetsData facets;
        facets[0] = paint_top_disc(mesh, side, stroke, radius);
        TextureColorRequest req = request;
        req.out_triangle        = &color;
        return build_texture_displacement(mesh, { layer }, facets, options, {}, &req);
    };
    // The pipeline's own counters, over welded positions, so a mesh watertight only after welding
    // still reads as closed.
    const auto defects = [](const indexed_triangle_set &mesh) {
        return TextureBake::count_edge_defects(TextureBake::to_soup(mesh));
    };

    std::vector<uint8_t>       color1;
    const indexed_triangle_set first = bake(cube, first_stroke, color1);
    REQUIRE(first.indices.size() > cube.indices.size());
    REQUIRE(color1.size() == first.indices.size());
    const TextureBake::EdgeDefects d1 = defects(first);
    INFO("first bake: " << d1.triangles << " triangles, " << d1.open << " open, " << d1.non_manifold << " non-manifold");
    CHECK(d1.open == 0);
    CHECK(d1.non_manifold == 0);

    // The colour the first bake committed, as TextureDisplacementBakeJob writes it.
    TriangleSelector::TriangleSplittingData painted_first;
    {
        const TriangleMesh mesh(first);
        TriangleSelector   selector(mesh);
        for (size_t i = 0; i < color1.size(); ++i)
            if (color1[i] != 0)
                selector.set_facet(int(i), EnforcerBlockerType(color1[i]));
        painted_first = selector.serialize();
    }
    REQUIRE_FALSE(painted_first.bitstream.empty());

    std::vector<uint8_t>       color2;
    const indexed_triangle_set second = bake(first, second_stroke, color2);
    REQUIRE(color2.size() == second.indices.size());
    REQUIRE(second.indices.size() != first.indices.size()); // the topology really did change
    const TextureBake::EdgeDefects d2 = defects(second);
    INFO("second bake: " << d2.triangles << " triangles, " << d2.open << " open, " << d2.non_manifold << " non-manifold");
    CHECK(d2.open == 0);
    CHECK(d2.non_manifold == 0);

    // The commit: the earlier colour carried onto the new topology, this bake's own colour over it.
    std::vector<uint8_t> committed(second.indices.size(), 0);
    {
        const TriangleMesh mesh(second);
        TriangleSelector   selector(mesh);
        const std::vector<uint8_t> carried = transfer_facet_paint(first, painted_first, second);
        REQUIRE(carried.size() == second.indices.size());
        for (size_t i = 0; i < carried.size(); ++i)
            if (carried[i] != 0)
                selector.set_facet(int(i), EnforcerBlockerType(carried[i]));
        for (size_t i = 0; i < color2.size(); ++i)
            if (color2[i] != 0)
                selector.set_facet(int(i), EnforcerBlockerType(color2[i]));
        for (int state = 1; state <= int(palette.size()); ++state) {
            std::vector<int> src;
            if (selector.get_facets_strict(EnforcerBlockerType(state), &src).indices.empty())
                continue;
            for (const int s : src)
                if (s >= 0 && size_t(s) < committed.size() && committed[size_t(s)] == 0)
                    committed[size_t(s)] = uint8_t(state);
        }
    }

    // Just the triangles the first bake coloured, to ask "how far is this from colour that was really
    // laid down" of anything the commit painted.
    indexed_triangle_set coloured_first;
    coloured_first.vertices = first.vertices;
    for (size_t i = 0; i < color1.size(); ++i)
        if (color1[i] != 0)
            coloured_first.indices.push_back(first.indices[i]);
    const AABBTreeIndirect::Tree3f tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(
        coloured_first.vertices, coloured_first.indices);

    // A triangle may carry colour the second bake did not produce only where the first bake's colour
    // actually was. A foot point sits on the surface the bake started from, which the relief has since
    // moved, and the transfer can widen the region by a triangle, so the margin is twice the edge
    // length the bake was told to refine to; landing anywhere else means the mask was read against a
    // mesh it no longer matches.
    size_t own = 0, carried_over = 0, stranded = 0;
    float  worst = 0.f;
    for (size_t i = 0; i < second.indices.size(); ++i) {
        if (committed[i] == 0)
            continue;
        if (i < color2.size() && color2[i] != 0) {
            CHECK(committed[i] == color2[i]); // this bake's own colour wins where it painted
            ++own;
            continue;
        }
        const stl_triangle_vertex_indices &t = second.indices[i];
        const Vec3f g = (second.vertices[size_t(t[0])] + second.vertices[size_t(t[1])] +
                         second.vertices[size_t(t[2])]) / 3.f;
        size_t      hit = 0;
        Vec3f       foot;
        const float d = std::sqrt(std::max(0.f, AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                                                    coloured_first.vertices, coloured_first.indices, tree, g, hit, foot)));
        worst = std::max(worst, d);
        if (d <= 2.f * refine)
            ++carried_over;
        else
            ++stranded;
    }
    INFO("committed: " << own << " from this bake, " << carried_over << " carried over, " << stranded
                       << " stranded, worst distance from the first bake's colour " << worst << " mm");
    CHECK(own > 0);
    CHECK(carried_over > 0);   // the first bake's colour survived the second
    CHECK(stranded == 0);      // and nothing was painted where neither bake put colour
}

TEST_CASE("A second bake beside a first comes out as fine as a single bake", "[TextureDisplacement]")
{
    // A stroke in each of the cube's two top triangles, baked one after the other. The first bake used
    // to leave the unpainted ground fanned into slivers from a far corner, or re-triangulated by the
    // flat harvest into long triangles, and the second bake refined those shapes rather than the
    // cube's own grid: its stroke came out many times as dense, with walls off the texture's lines.
    const indexed_triangle_set cube = its_make_cube(20.f, 20.f, 20.f);
    const Vec2f                first_stroke(5.f, 15.f), second_stroke(15.f, 5.f);
    constexpr float            radius = 3.f;

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.image_data   = make_checkerboard_png(16, 16);
    layer.tiling_scale = 5.f;
    layer.depth_mm     = 0.5f;
    TextureDisplacementOptions options;
    options.pipeline_v2        = true;
    options.v2_refine_mm       = 0.5f;
    options.v2_max_triangles_k = 750; // export: the flat harvest runs
    const auto bake = [&](const indexed_triangle_set &mesh, const Vec2f &stroke) {
        TextureDisplacementFacetsData facets;
        facets[0] = paint_top_disc(mesh, 20.f, stroke, radius);
        return build_texture_displacement(mesh, { layer }, facets, options);
    };
    // Aspect ratios of the top-face triangles whose centroid's distance from `centre` passes `keep`.
    const auto top_aspects = [](const indexed_triangle_set &mesh, const Vec2f &centre, auto keep) {
        std::vector<float> out;
        for (const stl_triangle_vertex_indices &t : mesh.indices) {
            const Vec3f &a = mesh.vertices[size_t(t[0])], &b = mesh.vertices[size_t(t[1])], &c = mesh.vertices[size_t(t[2])];
            const Vec3f  g = (a + b + c) / 3.f;
            if (g.z() > 19.9f && keep((Vec2f(g.x(), g.y()) - centre).norm()))
                out.push_back(triangle_aspect(a, b, c));
        }
        return out;
    };
    const auto in_stroke = [&](float d) { return d < radius; };

    const indexed_triangle_set first = bake(cube, first_stroke);
    REQUIRE(first.indices.size() > cube.indices.size());
    // What the first bake leaves around its stroke is ground the second one refines: whole pieces of
    // the cube's grid, not slivers.
    const std::vector<float> ground = top_aspects(first, first_stroke, [&](float d) { return d > radius + 1.f; });
    REQUIRE(!ground.empty());
    const float worst = *std::max_element(ground.begin(), ground.end());
    INFO("worst aspect ratio on the ground the first bake left: " << worst);
    CHECK(worst < 6.f);

    const size_t single = top_aspects(bake(cube, second_stroke), second_stroke, in_stroke).size();
    const size_t second = top_aspects(bake(first, second_stroke), second_stroke, in_stroke).size();
    INFO("second stroke baked alone: " << single << " triangles, after the first: " << second);
    REQUIRE(single > 0);
    CHECK(second <= single * 5 / 4);
}

// The .3mf stores the layer stack as JSON (see texture_displacement_layers_to_json). Two properties
// matter: everything the UV editor and the panel can set has to survive a round trip, and a document
// written by a build that knew fewer fields has to keep loading, with the missing ones left at their
// defaults rather than zeroed.
TEST_CASE("Texture displacement layers survive a JSON round trip", "[TextureDisplacement]")
{
    std::vector<TextureDisplacementLayer> layers(2);
    TextureDisplacementLayer             &a = layers[0];
    a.slot                 = 0;
    a.name                 = "Bark";
    a.path                 = "/textures/bark.png";
    a.depth_mm             = 1.25f;
    a.tiling_scale         = 7.5f;
    a.rotation_deg         = 30.f;
    a.offset               = Vec2f(0.25f, -0.5f);
    a.invert               = true;
    a.midlevel             = 0.125f;
    a.smoothing            = 0.75f;
    a.edge_smoothing       = true;
    a.edge_smoothing_amount = 0.25f;
    a.auto_connect_islands = false;
    a.tile_enabled         = false;
    a.tile_method          = TextureTileMethod::MirroredRepeat;
    a.projection_method    = TextureProjectionMethod::LSCM;
    a.blend_mode           = TextureBlendMode::Subtract;
    a.color_enabled        = true;
    a.lscm_seam_angle_deg  = 45.f;
    a.island_padding_mm    = 0.5f;
    a.lscm_seam_edges      = { { 1, 2 }, { 3, 5 } };
    a.islands              = { TextureIsland{ Vec2f(1.f, 2.f), 15.f, 1.5f }, TextureIsland{} };
    a.island_groups        = { 0, 0 };
    a.lscm_uv_overrides    = { { -4, Vec2f(0.5f, 0.75f) } };

    TextureDisplacementLayer &b = layers[1];
    b.slot                    = 1;
    b.projection_method       = TextureProjectionMethod::ViewProjected;
    b.view_project_right      = Vec3f(0.f, 1.f, 0.f);
    b.view_project_up         = Vec3f(0.f, 0.f, 1.f);
    b.view_project_projective = true;
    for (size_t i = 0; i < b.view_project_matrix.size(); ++i)
        b.view_project_matrix[i] = float(i) + 0.5f;

    TextureDisplacementOptions options;
    options.displace_border   = false;
    options.smooth_enabled    = true;
    options.smooth_strength   = 0.6f;
    options.smooth_iterations = 5;
    options.pipeline_v2       = false;
    options.v2_max_triangles_k = 250;
    options.color_mix_count   = 3;
    options.color_despeckle   = 4;
    options.active_slot       = 3;
    options.panel_mode        = 1;

    const std::string json = texture_displacement_layers_to_json(layers, options);

    std::vector<TextureDisplacementLayer> read_layers;
    TextureDisplacementOptions            read_options;
    REQUIRE(texture_displacement_layers_from_json(json, read_layers, read_options));
    REQUIRE(read_layers.size() == layers.size());

    const TextureDisplacementLayer &ra = read_layers[0];
    CHECK(ra.name == a.name);
    CHECK(ra.path == a.path);
    CHECK_THAT(ra.depth_mm, WithinAbs(a.depth_mm, 1e-6f));
    CHECK_THAT(ra.tiling_scale, WithinAbs(a.tiling_scale, 1e-6f));
    CHECK_THAT(ra.rotation_deg, WithinAbs(a.rotation_deg, 1e-6f));
    CHECK(ra.offset.isApprox(a.offset));
    CHECK(ra.invert == a.invert);
    CHECK_THAT(ra.midlevel, WithinAbs(a.midlevel, 1e-6f));
    CHECK_THAT(ra.smoothing, WithinAbs(a.smoothing, 1e-6f));
    CHECK(ra.edge_smoothing == a.edge_smoothing);
    CHECK_THAT(ra.edge_smoothing_amount, WithinAbs(a.edge_smoothing_amount, 1e-6f));
    CHECK(ra.auto_connect_islands == a.auto_connect_islands);
    CHECK(ra.tile_enabled == a.tile_enabled);
    CHECK(ra.tile_method == a.tile_method);
    CHECK(ra.projection_method == a.projection_method);
    CHECK(ra.blend_mode == a.blend_mode);
    CHECK(ra.color_enabled == a.color_enabled);
    CHECK_THAT(ra.lscm_seam_angle_deg, WithinAbs(a.lscm_seam_angle_deg, 1e-6f));
    CHECK_THAT(ra.island_padding_mm, WithinAbs(a.island_padding_mm, 1e-6f));
    CHECK(ra.lscm_seam_edges == a.lscm_seam_edges);
    REQUIRE(ra.islands.size() == a.islands.size());
    CHECK(ra.islands[0].offset.isApprox(a.islands[0].offset));
    CHECK_THAT(ra.islands[0].rotation_deg, WithinAbs(a.islands[0].rotation_deg, 1e-6f));
    CHECK_THAT(ra.islands[0].scale, WithinAbs(a.islands[0].scale, 1e-6f));
    CHECK(ra.island_groups == a.island_groups);
    REQUIRE(ra.lscm_uv_overrides.size() == 1);
    CHECK(ra.lscm_uv_overrides[0].first == -4);
    CHECK(ra.lscm_uv_overrides[0].second.isApprox(Vec2f(0.5f, 0.75f)));

    const TextureDisplacementLayer &rb = read_layers[1];
    CHECK(rb.projection_method == TextureProjectionMethod::ViewProjected);
    CHECK(rb.view_project_right.isApprox(b.view_project_right));
    CHECK(rb.view_project_up.isApprox(b.view_project_up));
    CHECK(rb.view_project_projective);
    CHECK(rb.view_project_matrix == b.view_project_matrix);

    CHECK(read_options.displace_border == options.displace_border);
    CHECK(read_options.smooth_enabled == options.smooth_enabled);
    CHECK_THAT(read_options.smooth_strength, WithinAbs(options.smooth_strength, 1e-6f));
    CHECK(read_options.smooth_iterations == options.smooth_iterations);
    CHECK(read_options.pipeline_v2 == options.pipeline_v2);
    CHECK(read_options.v2_max_triangles_k == options.v2_max_triangles_k);
    CHECK(read_options.color_mix_count == options.color_mix_count);
    CHECK(read_options.color_despeckle == options.color_despeckle);
    // Where the panel was left comes back with the rest, so a reopened project is on the same layer.
    CHECK(read_options.active_slot == options.active_slot);
    CHECK(read_options.panel_mode == options.panel_mode);
}

TEST_CASE("Texture displacement JSON keeps defaults for keys it does not carry", "[TextureDisplacement]")
{
    // What a build that knew fewer fields would have written: one layer, almost nothing set.
    const std::string sparse = R"({"version":1,"layers":[{"slot":2,"name":"Old","depth_mm":0.75}]})";

    std::vector<TextureDisplacementLayer> layers;
    TextureDisplacementOptions            options;
    REQUIRE(texture_displacement_layers_from_json(sparse, layers, options));
    REQUIRE(layers.size() == 1);

    const TextureDisplacementLayer  &l = layers[0];
    const TextureDisplacementLayer   fresh;
    const TextureDisplacementOptions defaults;
    CHECK(l.slot == 2);
    CHECK(l.name == "Old");
    CHECK_THAT(l.depth_mm, WithinAbs(0.75f, 1e-6f));
    // Everything absent keeps the struct's own default rather than becoming zero.
    CHECK_THAT(l.tiling_scale, WithinAbs(fresh.tiling_scale, 1e-6f));
    CHECK(l.auto_connect_islands == fresh.auto_connect_islands);
    CHECK(l.tile_enabled == fresh.tile_enabled);
    CHECK(l.projection_method == fresh.projection_method);
    CHECK_THAT(l.lscm_seam_angle_deg, WithinAbs(fresh.lscm_seam_angle_deg, 1e-6f));
    CHECK(options.color_mix_count == defaults.color_mix_count);
    CHECK(options.pipeline_v2 == defaults.pipeline_v2);
    CHECK(options.active_slot == defaults.active_slot);
    CHECK(options.panel_mode == defaults.panel_mode);

    std::vector<TextureDisplacementLayer> unused_layers;
    TextureDisplacementOptions            unused_options;
    CHECK(!texture_displacement_layers_from_json("not json at all", unused_layers, unused_options));
}

TEST_CASE("Texture displacement JSON with a wrong-typed element or an out-of-range slot is rejected", "[TextureDisplacement]")
{
    const std::string layer = GENERATE(as<std::string>{},
                                       R"({"slot":0,"offset":["x",0]})",
                                       R"({"slot":0,"view_project_right":[null,0,0]})",
                                       R"({"slot":0,"view_project_matrix":[0,0,0,0,0,0,0,0,0,0,0,"x"]})",
                                       R"({"slot":0,"lscm_seam_edges":[1,null]})",
                                       R"({"slot":0,"islands":[[0,0,"x",1]]})",
                                       R"({"slot":0,"lscm_uv_overrides":[[null,0,0]]})",
                                       R"({"slot":8})",
                                       R"({"slot":-1})",
                                       R"({"slot":"x"})",
                                       R"({"name":"No slot"})");
    INFO(layer);

    std::vector<TextureDisplacementLayer> layers(1);
    layers.front().name = "Kept";
    TextureDisplacementOptions options;
    options.color_mix_count = 7;

    bool parsed = true;
    REQUIRE_NOTHROW(parsed = texture_displacement_layers_from_json(R"({"version":1,"layers":[)" + layer + "]}", layers, options));
    CHECK_FALSE(parsed);
    REQUIRE(layers.size() == 1);
    CHECK(layers.front().name == "Kept");
    CHECK(options.color_mix_count == 7);
}

TEST_CASE("whether a layer's texture has colour agrees with its decode", "[TextureDisplacement]")
{
    TextureDisplacementLayer gray;
    gray.image_data = make_flat_gray_png(128);
    CHECK_FALSE(height_texture_has_color(gray));

    TextureDisplacementLayer color;
    color.image_data = make_flat_rgb_png(200, 40, 10);
    // Before the image is decoded and after, and whatever the smoothing.
    CHECK(height_texture_has_color(color));
    CHECK(decode_height_texture(color).has_color());
    CHECK(height_texture_has_color(color));
    color.smoothing = 0.5f;
    CHECK(height_texture_has_color(color));

    CHECK_FALSE(height_texture_has_color(TextureDisplacementLayer{}));
}

// ---------------------------------------------------------------------------------------------
// The model's own UV channel, and the Override blend mode
// ---------------------------------------------------------------------------------------------

// A grayscale image that is black in its left half and white in its right half, so where a sample
// lands along u is readable straight off the height it comes back with.
static std::shared_ptr<std::vector<unsigned char>> make_left_right_split_png(size_t w = 16, size_t h = 16)
{
    std::vector<uint8_t> pixels(w * h);
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x)
            pixels[y * w + x] = (x < w / 2) ? 0 : 255;
    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path()
        / boost::filesystem::unique_path("texdisp_test_%%%%%%%%.png");
    REQUIRE(Slic3r::png::write_gray_to_file(tmp_path.string(), w, h, pixels));

    std::vector<unsigned char> bytes;
    {
        std::ifstream ifs(tmp_path.string(), std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    }
    boost::system::error_code ec;
    boost::filesystem::remove(tmp_path, ec);

    REQUIRE_FALSE(bytes.empty());
    return std::make_shared<std::vector<unsigned char>>(std::move(bytes));
}

// Where a sheet coordinate lands in uv: across [UV_LO, UV_LO + UV_SPAN] rather than the full [0, 1].
// The inset keeps the tests off the exact edge of an untiled placement, where there is deliberately no
// texture at all rather than its border texel (see DecodedHeightTexture::sample()) - a real model's
// channel does reach 1.0, and a vertex sitting exactly there displaces by nothing on purpose.
static constexpr float UV_LO   = 0.1f;
static constexpr float UV_SPAN = 0.8f;

static float sheet_uv_at(float coord, float size, bool mirror = false)
{
    const float t = coord / size;
    return UV_LO + UV_SPAN * (mirror ? 1.f - t : t);
}

// One UV per triangle corner: u follows x, v follows y. `mirror_u` mirrors u, which is what the tests
// use to show the bake is really reading the channel rather than projecting the position.
static std::vector<Vec2f> sheet_uv_channel(const indexed_triangle_set &sheet, float size, bool mirror_u = false)
{
    std::vector<Vec2f> uv(sheet.indices.size() * 3);
    for (size_t f = 0; f < sheet.indices.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const Vec3f &p = sheet.vertices[size_t(sheet.indices[f][k])];
            uv[f * 3 + size_t(k)] = Vec2f(sheet_uv_at(p.x(), size, mirror_u), sheet_uv_at(p.y(), size));
        }
    return uv;
}

TEST_CASE("Override blend replaces the displacement under it instead of combining with it",
          "[TextureDisplacement]")
{
    CHECK_THAT(blend_displacement(3.f, 1.f, TextureBlendMode::Override), WithinAbs(1.f, 1e-6f));
    // Nothing underneath yet is still just the value, so an Override layer at the bottom of the
    // stack behaves exactly like the Add the bake forces there.
    CHECK_THAT(blend_displacement(0.f, 1.f, TextureBlendMode::Override), WithinAbs(1.f, 1e-6f));
    // The sign carries, and a zero sample really does flatten what is under it - that is the point.
    CHECK_THAT(blend_displacement(3.f, -1.f, TextureBlendMode::Override), WithinAbs(-1.f, 1e-6f));
    CHECK_THAT(blend_displacement(3.f, 0.f, TextureBlendMode::Override), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("an Override layer leaves only its own relief where two layers overlap", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    TextureDisplacementLayer base;
    base.slot         = 0;
    base.depth_mm     = 2.0f;
    base.tiling_scale = 5.0f;
    base.image_data   = make_flat_gray_png(255); // full depth everywhere

    TextureDisplacementLayer second = base;
    second.slot       = 1;
    second.depth_mm   = 0.5f;
    second.blend_mode = TextureBlendMode::Override;

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);
    facets[1] = facets[0];

    const indexed_triangle_set overridden = build_texture_displacement(cube, {base, second}, facets, classic_options());
    REQUIRE(overridden.vertices.size() == cube.vertices.size());

    // Add would have piled the two up to 2.5 mm; Override keeps the upper layer's 0.5 mm alone.
    TextureDisplacementLayer added = second;
    added.blend_mode               = TextureBlendMode::Add;
    const indexed_triangle_set summed = build_texture_displacement(cube, {base, added}, facets, classic_options());
    REQUIRE(summed.vertices.size() == cube.vertices.size());

    for (size_t i = 0; i < cube.vertices.size(); ++i) {
        if (!above_resting_plane(cube, i))
            continue;
        CHECK_THAT((overridden.vertices[i] - cube.vertices[i]).norm(), WithinAbs(0.5f, 1e-3f));
        CHECK_THAT((summed.vertices[i] - cube.vertices[i]).norm(), WithinAbs(2.5f, 1e-3f));
    }
}

TEST_CASE("the ModelUV mapping displaces through the model's own UV channel", "[TextureDisplacement]")
{
    constexpr float            size  = 10.f;
    const indexed_triangle_set sheet = make_flat_sheet(size, 1.f);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 1.0f;
    layer.image_data   = make_left_right_split_png();
    layer.tile_enabled = false; // the layout is the placement; nothing should repeat
    layer.projection_method = TextureProjectionMethod::ModelUV;

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(sheet);

    const std::vector<Vec2f> uv      = sheet_uv_channel(sheet, size);
    const std::vector<Vec2f> flipped = sheet_uv_channel(sheet, size, /* mirror_u */ true);

    const indexed_triangle_set out = build_texture_displacement(sheet, {layer}, facets, classic_options(), {}, nullptr,
                                                                 Transform3d::Identity(), nullptr, nullptr, &uv);
    REQUIRE(out.vertices.size() == sheet.vertices.size());

    // The image is black for u < 0.5 and white above it, and the channel's u rises with x across the
    // sheet, so it comes out flat on its left half and raised by the full depth on its right.
    size_t low = 0, high = 0;
    for (size_t i = 0; i < sheet.vertices.size(); ++i) {
        const float x    = sheet.vertices[i].x();
        const float dz   = out.vertices[i].z() - sheet.vertices[i].z();
        if (x < 0.4f * size) {
            CHECK_THAT(dz, WithinAbs(0.f, 1e-3f));
            ++low;
        } else if (x > 0.6f * size) {
            CHECK_THAT(dz, WithinAbs(layer.depth_mm, 1e-3f));
            ++high;
        }
    }
    REQUIRE(low > 0);
    REQUIRE(high > 0);

    // Mirroring the channel mirrors the relief: the mapping really is the channel's, not a planar
    // projection of the position (which would be unchanged by it).
    const indexed_triangle_set mirrored = build_texture_displacement(sheet, {layer}, facets, classic_options(), {},
                                                                      nullptr, Transform3d::Identity(), nullptr, nullptr,
                                                                      &flipped);
    REQUIRE(mirrored.vertices.size() == sheet.vertices.size());
    for (size_t i = 0; i < sheet.vertices.size(); ++i) {
        const float x = sheet.vertices[i].x();
        if (x < 0.4f * size)
            CHECK_THAT(mirrored.vertices[i].z() - sheet.vertices[i].z(), WithinAbs(layer.depth_mm, 1e-3f));
        else if (x > 0.6f * size)
            CHECK_THAT(mirrored.vertices[i].z() - sheet.vertices[i].z(), WithinAbs(0.f, 1e-3f));
    }
}

TEST_CASE("a ModelUV layer falls back to Triplanar when the volume carries no UV channel",
          "[TextureDisplacement]")
{
    constexpr float            size  = 10.f;
    const indexed_triangle_set sheet = make_flat_sheet(size, 2.f);

    TextureDisplacementLayer layer;
    layer.slot         = 0;
    layer.depth_mm     = 1.0f;
    layer.tiling_scale = 5.0f;
    layer.image_data   = make_flat_gray_png(255);

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(sheet);

    layer.projection_method                 = TextureProjectionMethod::Triplanar;
    const indexed_triangle_set as_triplanar = build_texture_displacement(sheet, {layer}, facets, classic_options());
    layer.projection_method                 = TextureProjectionMethod::ModelUV;
    const indexed_triangle_set no_channel    = build_texture_displacement(sheet, {layer}, facets, classic_options());

    REQUIRE(no_channel.vertices.size() == as_triplanar.vertices.size());
    for (size_t i = 0; i < as_triplanar.vertices.size(); ++i)
        CHECK_THAT((no_channel.vertices[i] - as_triplanar.vertices[i]).norm(), WithinAbs(0.f, 1e-5f));
}

TEST_CASE("a bake with colour off on every layer reports no colour at all", "[TextureDisplacement]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);

    TextureDisplacementLayer layer;
    layer.slot          = 0;
    layer.depth_mm      = 1.0f;
    layer.tiling_scale  = 5.0f;
    layer.image_data    = make_flat_rgb_png(200, 40, 10); // has colour to give...
    layer.color_enabled = false;                          // ...but is not asked for it

    TextureDisplacementFacetsData facets{};
    facets[0] = paint_whole_mesh(cube);

    std::vector<uint8_t> triangle_color;
    TextureColorRequest  request;
    request.quantize     = [](const Vec3f &) { return 0; }; // would colour everything, if asked
    request.out_triangle = &triangle_color;

    const indexed_triangle_set out = build_texture_displacement(cube, {layer}, facets, classic_options(), {}, &request);
    REQUIRE_FALSE(out.vertices.empty());
    // Every entry 0 ("this triangle takes no colour from the texture"), so the commit leaves whatever
    // the model was already painted with exactly as it was.
    for (const uint8_t entry : triangle_color)
        CHECK(entry == 0);
}

TEST_CASE("transfer_uv_channel resamples a UV channel onto a subdivided copy of the same surface",
          "[TextureDisplacement]")
{
    constexpr float            size   = 10.f;
    const indexed_triangle_set coarse = make_flat_sheet(size, 5.f);
    const indexed_triangle_set fine   = make_flat_sheet(size, 1.f);
    const std::vector<Vec2f>   uv     = sheet_uv_channel(coarse, size);

    const std::vector<Vec2f> moved = transfer_uv_channel(coarse, uv, fine);
    REQUIRE(moved.size() == fine.indices.size() * 3);

    // The channel is affine in position here (u = x/size, v = y/size), so a correct transfer
    // reproduces it exactly at every corner of the finer mesh.
    for (size_t f = 0; f < fine.indices.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const Vec3f &p = fine.vertices[size_t(fine.indices[f][k])];
            CHECK_THAT(moved[f * 3 + size_t(k)].x(), WithinAbs(sheet_uv_at(p.x(), size), 1e-4f));
            CHECK_THAT(moved[f * 3 + size_t(k)].y(), WithinAbs(sheet_uv_at(p.y(), size), 1e-4f));
        }

    // A mismatched channel is refused rather than read past its end.
    CHECK(transfer_uv_channel(coarse, {Vec2f::Zero()}, fine).empty());
    CHECK(transfer_uv_channel(coarse, uv, indexed_triangle_set{}).empty());
}

TEST_CASE("the model's per-corner UVs survive a brush split and collapse to one per vertex",
          "[TextureDisplacement]")
{
    constexpr float            size  = 10.f;
    const indexed_triangle_set sheet = make_flat_sheet(size, 5.f);
    const std::vector<Vec2f>   uv    = sheet_uv_channel(sheet, size);

    // The whole sheet as a patch, with the source map the channel is read through.
    const TriangleMesh tm(sheet);
    TriangleSelector   selector(tm);
    for (int f = 0; f < int(sheet.indices.size()); ++f)
        selector.set_facet(f, EnforcerBlockerType::ENFORCER);
    std::vector<int>           source;
    const indexed_triangle_set patch = selector.get_facets_strict(EnforcerBlockerType::ENFORCER, &source);
    REQUIRE(patch.indices.size() == source.size());

    const std::vector<Vec2f> corner = compute_model_corner_uvs(sheet, uv, patch, source);
    REQUIRE(corner.size() == patch.indices.size() * 3);
    // An unsplit patch triangle *is* its source triangle, so its corners reproduce the channel.
    for (size_t f = 0; f < patch.indices.size(); ++f)
        for (int k = 0; k < 3; ++k) {
            const Vec3f &p = patch.vertices[size_t(patch.indices[f][k])];
            CHECK_THAT(corner[f * 3 + size_t(k)].x(), WithinAbs(sheet_uv_at(p.x(), size), 1e-4f));
            CHECK_THAT(corner[f * 3 + size_t(k)].y(), WithinAbs(sheet_uv_at(p.y(), size), 1e-4f));
        }

    const std::vector<Vec2f> per_vertex = collapse_corner_uvs(patch, corner);
    REQUIRE(per_vertex.size() == patch.vertices.size());
    for (size_t i = 0; i < patch.vertices.size(); ++i) {
        CHECK_THAT(per_vertex[i].x(), WithinAbs(sheet_uv_at(patch.vertices[i].x(), size), 1e-4f));
        CHECK_THAT(per_vertex[i].y(), WithinAbs(sheet_uv_at(patch.vertices[i].y(), size), 1e-4f));
    }

    // Mismatched inputs report nothing rather than guessing.
    CHECK(compute_model_corner_uvs(sheet, {Vec2f::Zero()}, patch, source).empty());
    CHECK(compute_model_corner_uvs(sheet, uv, patch, {}).empty());
    CHECK(collapse_corner_uvs(patch, {Vec2f::Zero()}).empty());
}

TEST_CASE("facet paint is carried onto a rebuilt mesh by each facet's foot point", "[TextureDisplacement]")
{
    constexpr float            size   = 10.f;
    const indexed_triangle_set coarse = make_flat_sheet(size, 2.5f);

    // Two filaments, split down the middle - the pattern the transfer has to reproduce.
    const TriangleMesh coarse_mesh(coarse);
    TriangleSelector   painter(coarse_mesh);
    for (size_t t = 0; t < coarse.indices.size(); ++t) {
        const stl_triangle_vertex_indices &tri = coarse.indices[t];
        const float x = (coarse.vertices[size_t(tri[0])].x() + coarse.vertices[size_t(tri[1])].x() +
                         coarse.vertices[size_t(tri[2])].x()) / 3.f;
        painter.set_facet(int(t), x < 0.5f * size ? EnforcerBlockerType::Extruder1 : EnforcerBlockerType::Extruder2);
    }
    const TriangleSelector::TriangleSplittingData paint = painter.serialize();

    // A finer mesh, lifted clear of the original surface the way a bake's relief lifts it. This is
    // what ModelVolume::restore_painting()'s 0.01 mm overlap remap cannot follow.
    indexed_triangle_set lifted = make_flat_sheet(size, 1.f);
    for (Vec3f &v : lifted.vertices)
        v.z() += 0.5f;

    const std::vector<uint8_t> state = transfer_facet_paint(coarse, paint, lifted);
    REQUIRE(state.size() == lifted.indices.size());

    size_t left = 0, right = 0;
    for (size_t t = 0; t < lifted.indices.size(); ++t) {
        const stl_triangle_vertex_indices &tri = lifted.indices[t];
        const float x = (lifted.vertices[size_t(tri[0])].x() + lifted.vertices[size_t(tri[1])].x() +
                         lifted.vertices[size_t(tri[2])].x()) / 3.f;
        // Only away from the split: a facet straddling it may legitimately land on either side.
        if (x < 0.4f * size) {
            CHECK(state[t] == uint8_t(EnforcerBlockerType::Extruder1));
            ++left;
        } else if (x > 0.6f * size) {
            CHECK(state[t] == uint8_t(EnforcerBlockerType::Extruder2));
            ++right;
        }
    }
    REQUIRE(left > 0);
    REQUIRE(right > 0);

    // An unpainted source has nothing to carry, which is not the same as carrying NONE everywhere:
    // the caller has to tell those apart to decide between resetting the annotation and writing one.
    CHECK(transfer_facet_paint(coarse, TriangleSelector::TriangleSplittingData{}, lifted).empty());
    CHECK(transfer_facet_paint(coarse, paint, indexed_triangle_set{}).empty());
}

TEST_CASE("an unpainted part of the source carries no paint onto the rebuilt mesh", "[TextureDisplacement]")
{
    constexpr float            size   = 10.f;
    const indexed_triangle_set coarse = make_flat_sheet(size, 2.5f);

    // Only the left half is painted at all; the right half must come back NONE rather than borrowing
    // the nearest painted colour from across the model.
    const TriangleMesh coarse_mesh(coarse);
    TriangleSelector   painter(coarse_mesh);
    for (size_t t = 0; t < coarse.indices.size(); ++t) {
        const stl_triangle_vertex_indices &tri = coarse.indices[t];
        const float x = (coarse.vertices[size_t(tri[0])].x() + coarse.vertices[size_t(tri[1])].x() +
                         coarse.vertices[size_t(tri[2])].x()) / 3.f;
        if (x < 0.5f * size)
            painter.set_facet(int(t), EnforcerBlockerType::Extruder3);
    }

    const std::vector<uint8_t> state = transfer_facet_paint(coarse, painter.serialize(), coarse);
    REQUIRE(state.size() == coarse.indices.size());
    for (size_t t = 0; t < coarse.indices.size(); ++t) {
        const stl_triangle_vertex_indices &tri = coarse.indices[t];
        const float x = (coarse.vertices[size_t(tri[0])].x() + coarse.vertices[size_t(tri[1])].x() +
                         coarse.vertices[size_t(tri[2])].x()) / 3.f;
        if (x < 0.4f * size)
            CHECK(state[t] == uint8_t(EnforcerBlockerType::Extruder3));
        else if (x > 0.6f * size)
            CHECK(state[t] == uint8_t(EnforcerBlockerType::NONE));
    }
}
