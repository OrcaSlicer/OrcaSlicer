#define NOMINMAX
#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <boost/filesystem.hpp>

#include "libslic3r/TextureDisplacement.hpp"
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

    const indexed_triangle_set result = build_texture_displacement(cube, layers, facets);

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

    const indexed_triangle_set result = build_texture_displacement(cube, {layer}, facets);

    REQUIRE(result.vertices.size() == cube.vertices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i) {
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

    const indexed_triangle_set result = build_texture_displacement(cube, {base, second}, facets);

    // Topology is preserved exactly, so vertices can be compared 1:1 with the input.
    REQUIRE(result.vertices.size() == cube.vertices.size());
    REQUIRE(result.indices.size() == cube.indices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i)
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

    const indexed_triangle_set result = build_texture_displacement(cube, {base, second}, facets);

    REQUIRE(result.vertices.size() == cube.vertices.size());
    for (size_t i = 0; i < cube.vertices.size(); ++i)
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

    const indexed_triangle_set result = build_texture_displacement(cube, {layer}, facets);

    for (size_t i = 0; i < cube.vertices.size(); ++i)
        CHECK_THAT((result.vertices[i] - cube.vertices[i]).norm(), WithinAbs(2.0f, 1e-3f));
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
        const indexed_triangle_set result = build_texture_displacement(fan, {layer}, facets);
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
        TextureDisplacementOptions options;
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

    TextureDisplacementOptions options;
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

        const indexed_triangle_set raw = build_texture_displacement(grid, { layer }, facets);
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
        const indexed_triangle_set raw = build_texture_displacement(grid, { layer }, facets);

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

    SECTION("a sharp bump refines densely at its center and leaves flat corners coarse")
    {
        // A tight Gaussian bump at the sheet's center: strong curvature near (0.5, 0.5), flat far away.
        HeightFieldSampler bump = [](const Vec3f &p, const Vec3f &) {
            const float r2 = (p.x() - 0.5f) * (p.x() - 0.5f) + (p.y() - 0.5f) * (p.y() - 0.5f);
            return 1.0f * std::exp(-r2 / 0.02f);
        };

        // Baseline max edge 0.3 is coarser than the grid's own edges, so the baseline adds nothing
        // here - this isolates the *curvature* contribution (the grid already meets the baseline).
        std::vector<int>           source;
        const indexed_triangle_set out =
            subdivide_mesh_adaptive(grid, region, /*max edge*/ 0.3f, 200000, &source, bump, /*tol*/ 0.02f,
                                    /*min_edge*/ 0.01f);

        CHECK(out.indices.size() > grid.indices.size()); // the bump forced real refinement

        // The largest triangle near the bump's center must be much smaller than the largest in a flat
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

TEST_CASE("TextureDisplacement: a colour texture decodes to both colour and height", "[TextureDisplacement]")
{
    TextureDisplacementLayer layer;
    layer.slot       = 0;
    layer.image_data = make_rgb_png_2x2();

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

    const indexed_triangle_set out = build_texture_displacement(quad, { layer }, facets, {}, {}, &request);

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

    build_texture_displacement(quad, { layer }, facets, {}, {}, &request);

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
