#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/ChromaSplit.hpp"
#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// A stack with one filament per band and nothing the colour model has to think about: the split only
// ever reads the layer counts, the layer height, the base and the plate.
ChromaStack split_stack(const std::vector<int> &band_layers, int base_layers, float layer_height_mm,
                        ChromaOrientation orientation = ChromaOrientation::FaceUp, float plate_base_mm = 1.f)
{
    ChromaStack stack;
    for (int layers : band_layers) {
        ChromaBand band;
        band.layers          = layers;
        band.filament.filament = int(stack.bands.size());
        stack.bands.push_back(band);
    }
    stack.layer_height_mm  = layer_height_mm;
    stack.base_layers      = base_layers;
    stack.plate.generated  = true;
    stack.plate.width_mm   = 40.f;
    stack.plate.height_mm  = 20.f;
    stack.plate.base_mm    = plate_base_mm;
    stack.plate.cell_mm    = 1.f;
    stack.plate.orientation  = orientation;
    stack.plate.frame.width_mm = 0.f;
    return stack;
}

// A solved height map from relief layer counts: `relief[i]` layers of picture above the base.
ChromaSolve split_solve(int width, int height, const std::vector<int> &relief, int base_layers)
{
    ChromaSolve solve;
    solve.width  = width;
    solve.height = height;
    solve.levels.resize(size_t(width) * size_t(height));
    for (size_t i = 0; i < solve.levels.size(); ++i)
        solve.levels[i] = uint16_t(base_layers + relief[i]);
    return solve;
}

// How many edges of the mesh are not shared by exactly two triangles. Zero is the definition of a
// closed surface, and it is the property the whole of this file exists to hold on to: a piece with an
// edge belonging to one triangle has a hole in it, and one with an edge belonging to four is pinched
// where two parts of it touch. Either kind is refused by a mesh boolean.
size_t edges_not_shared_twice(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> edges;
    for (const Vec3i32 &t : its.indices)
        for (int k = 0; k < 3; ++k) {
            const int a = t[k], b = t[(k + 1) % 3];
            ++edges[{std::min(a, b), std::max(a, b)}];
        }
    size_t bad = 0;
    for (const auto &edge : edges)
        if (edge.second != 2)
            ++bad;
    return bad;
}

// How many edges are not traversed exactly once in each direction, which is the same surface seen
// from the winding's side: a closed mesh whose triangles all face outward uses every edge once each
// way round.
size_t edges_not_wound_consistently(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> directed;
    for (const Vec3i32 &t : its.indices)
        for (int k = 0; k < 3; ++k)
            ++directed[{t[k], t[(k + 1) % 3]}];
    size_t bad = 0;
    for (const auto &edge : directed)
        if (edge.second != 1)
            ++bad;
    return bad;
}

size_t degenerate_triangles(const indexed_triangle_set &its)
{
    size_t bad = 0;
    for (const Vec3i32 &t : its.indices) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) {
            ++bad;
            continue;
        }
        const Vec3d a = its.vertices[size_t(t[0])].cast<double>();
        const Vec3d b = its.vertices[size_t(t[1])].cast<double>();
        const Vec3d c = its.vertices[size_t(t[2])].cast<double>();
        if ((b - a).cross(c - a).norm() < 1e-12)
            ++bad;
    }
    return bad;
}

// The volume the mesh encloses, by the divergence theorem. Positive means the triangles face outward,
// which is the other half of being a printable solid.
double enclosed_volume(const indexed_triangle_set &its)
{
    double volume = 0.;
    for (const Vec3i32 &t : its.indices) {
        const Vec3d a = its.vertices[size_t(t[0])].cast<double>();
        const Vec3d b = its.vertices[size_t(t[1])].cast<double>();
        const Vec3d c = its.vertices[size_t(t[2])].cast<double>();
        volume += a.dot(b.cross(c));
    }
    return volume / 6.;
}

float lowest_z(const indexed_triangle_set &its)
{
    float z = its.vertices.front().z();
    for (const Vec3f &v : its.vertices)
        z = std::min(z, v.z());
    return z;
}

float highest_z(const indexed_triangle_set &its)
{
    float z = its.vertices.front().z();
    for (const Vec3f &v : its.vertices)
        z = std::max(z, v.z());
    return z;
}

// A ramp, a ring, two islands and a checkerboard, by name. The checkerboard is the one that matters
// most: a dithered solve scatters single texels of height, and two cells that meet only at a corner
// are what pinches a heightfield mesh into something no boolean will take.
std::vector<int> relief_pattern(const std::string &shape, int width, int height, int relief_layers)
{
    std::vector<int> relief(size_t(width) * size_t(height), 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            int value = 0;
            if (shape == "flat")
                value = relief_layers / 2;
            else if (shape == "ramp")
                value = (x * relief_layers) / width;
            else if (shape == "ring")
                value = (x > 1 && x < width - 2 && y > 1 && y < height - 2 &&
                         !(x > 3 && x < width - 4 && y > 3 && y < height - 4))
                            ? relief_layers
                            : 1;
            else if (shape == "islands")
                value = ((x > 1 && x < 4) || (x > width - 5 && x < width - 2)) && y > 1 && y < height - 2
                            ? relief_layers
                            : 1;
            else if (shape == "checkerboard")
                value = ((x + y) % 2) ? relief_layers : 0;
            relief[size_t(y) * size_t(width) + size_t(x)] = std::clamp(value, 0, relief_layers);
        }
    return relief;
}

} // namespace

TEST_CASE("Every piece a split produces is a closed, outward-facing solid", "[ChromaSplit]")
{
    const std::string shape = GENERATE(std::string("flat"), std::string("ramp"), std::string("ring"),
                                       std::string("islands"), std::string("checkerboard"));
    const int               width = 13, height = 11;
    const std::vector<int>  layers{ 3, 4, 5 };
    const ChromaStack       stack  = split_stack(layers, 3, 0.1f);
    const std::vector<int>  relief = relief_pattern(shape, width, height, 12);
    const ChromaSplitPieces out    = chroma_split_bands(stack, split_solve(width, height, relief, 3));

    REQUIRE(out.bands.size() == layers.size());
    for (const indexed_triangle_set &piece : out.bands) {
        if (piece.indices.empty())
            continue;
        REQUIRE(edges_not_shared_twice(piece) == 0);
        REQUIRE(edges_not_wound_consistently(piece) == 0);
        REQUIRE(degenerate_triangles(piece) == 0);
        REQUIRE(enclosed_volume(piece) > 0.);
    }
}

TEST_CASE("The pieces of a split add up to the plate they came from", "[ChromaSplit]")
{
    const int              width = 9, height = 7;
    const int              base = 4;
    const float            layer_height = 0.1f;
    const std::vector<int> relief = relief_pattern("ramp", width, height, 9);
    const ChromaStack      stack  = split_stack({ 3, 3, 3 }, base, layer_height);
    const ChromaSplitPieces out   = chroma_split_bands(stack, split_solve(width, height, relief, base));

    REQUIRE(out.grid_width == width);
    REQUIRE(out.grid_height == height);

    const double cell = double(out.cell_x_mm) * double(out.cell_y_mm);
    double       expected = 0.;
    for (int r : relief)
        expected += double(base + r) * double(layer_height) * cell;

    double total = 0.;
    for (const indexed_triangle_set &piece : out.bands)
        if (!piece.indices.empty())
            total += enclosed_volume(piece);
    REQUIRE_THAT(total, WithinRel(expected, 1e-5));
}

TEST_CASE("Two pieces that meet share the vertices of the face they meet at", "[ChromaSplit]")
{
    const int               width = 11, height = 9;
    const std::vector<int>  relief = relief_pattern("ramp", width, height, 9);
    const ChromaStack       stack  = split_stack({ 3, 3, 3 }, 2, 0.1f);
    const ChromaSplitPieces out    = chroma_split_bands(stack, split_solve(width, height, relief, 2));
    REQUIRE(out.bands.size() == 3);

    for (size_t i = 1; i < out.bands.size(); ++i) {
        const indexed_triangle_set &upper = out.bands[i];
        const indexed_triangle_set &lower = out.bands[i - 1];
        REQUIRE_FALSE(upper.indices.empty());
        REQUIRE_FALSE(lower.indices.empty());

        // Where they meet: the upper piece's floor, which is the lower piece's own top over that part
        // of the picture. Compared exactly, because exactly is the claim - a vertex a rounding error
        // away from its neighbour is a sliver when the two pieces are printed and assembled, and a
        // coincident-but-not-equal face when they are handed to a boolean.
        const float plane = lowest_z(upper);
        std::set<std::pair<float, float>> below;
        for (const Vec3f &v : lower.vertices)
            if (v.z() == plane)
                below.insert({ v.x(), v.y() });

        size_t on_plane = 0, missing = 0;
        for (const Vec3f &v : upper.vertices)
            if (v.z() == plane) {
                ++on_plane;
                if (below.find({ v.x(), v.y() }) == below.end())
                    ++missing;
            }
        REQUIRE(on_plane > 0);
        REQUIRE(missing == 0);
        REQUIRE_THAT(double(plane), WithinAbs(double(highest_z(lower)), 1e-9));
    }
}

TEST_CASE("A band the picture never reaches comes back empty rather than flat", "[ChromaSplit]")
{
    const int              width = 8, height = 5;
    const std::vector<int> relief(size_t(width) * size_t(height), 2);
    // Three bands over a picture that never climbs past the second layer: the first band is printed,
    // the second has no layers at all, and the third is never reached.
    const ChromaStack       stack = split_stack({ 2, 0, 4 }, 3, 0.1f);
    const ChromaSplitPieces out   = chroma_split_bands(stack, split_solve(width, height, relief, 3));

    REQUIRE(out.bands.size() == 3);
    REQUIRE_FALSE(out.bands[0].indices.empty());
    REQUIRE(out.bands[1].indices.empty());
    REQUIRE(out.bands[1].vertices.empty());
    REQUIRE(out.bands[2].indices.empty());
    REQUIRE(out.bands[2].vertices.empty());
}

TEST_CASE("A band the picture reaches in two places is one mesh of two shells", "[ChromaSplit]")
{
    const int               width = 13, height = 9;
    const std::vector<int>  relief = relief_pattern("islands", width, height, 8);
    const ChromaStack       stack  = split_stack({ 3, 5 }, 2, 0.1f);
    const ChromaSplitPieces out    = chroma_split_bands(stack, split_solve(width, height, relief, 2));

    REQUIRE(out.bands.size() == 2);
    const indexed_triangle_set &top = out.bands[1];
    REQUIRE_FALSE(top.indices.empty());
    REQUIRE(edges_not_shared_twice(top) == 0);
    REQUIRE(its_split(top).size() == 2);
}

TEST_CASE("A hole in a band is walled in rather than left open", "[ChromaSplit]")
{
    const int               width = 15, height = 13;
    const std::vector<int>  relief = relief_pattern("ring", width, height, 8);
    const ChromaStack       stack  = split_stack({ 3, 5 }, 2, 0.1f);
    const ChromaSplitPieces out    = chroma_split_bands(stack, split_solve(width, height, relief, 2));

    const indexed_triangle_set &top = out.bands[1];
    REQUIRE_FALSE(top.indices.empty());
    // One shell with a hole through it, not two.
    REQUIRE(its_split(top).size() == 1);
    REQUIRE(edges_not_shared_twice(top) == 0);
    REQUIRE(its_num_open_edges(top) == 0);
}

TEST_CASE("The first piece carries the base, so a bare column is still solid under it", "[ChromaSplit]")
{
    const int   width = 6, height = 4;
    const int   base = 5;
    const float layer_height = 0.1f;
    // Half the picture is bare base and reaches no band at all.
    std::vector<int> relief(size_t(width) * size_t(height), 0);
    for (int y = 0; y < height; ++y)
        for (int x = width / 2; x < width; ++x)
            relief[size_t(y) * size_t(width) + size_t(x)] = 4;

    const ChromaStack       stack = split_stack({ 4 }, base, layer_height);
    const ChromaSplitPieces out   = chroma_split_bands(stack, split_solve(width, height, relief, base));
    REQUIRE(out.bands.size() == 1);
    const indexed_triangle_set &piece = out.bands.front();

    REQUIRE(edges_not_shared_twice(piece) == 0);
    REQUIRE_THAT(double(lowest_z(piece)), WithinAbs(0., 1e-9));
    // The picture's lowest columns are the base and nothing more, and they are part of this piece:
    // without them the plate would have a hole through it wherever the picture is at its darkest.
    const double cell = double(out.cell_x_mm) * double(out.cell_y_mm);
    double       expected = 0.;
    for (int r : relief)
        expected += double(base + r) * double(layer_height) * cell;
    REQUIRE_THAT(enclosed_volume(piece), WithinRel(expected, 1e-5));
}

TEST_CASE("A face-down plate splits with the picture against the build plate", "[ChromaSplit]")
{
    const int   width = 11, height = 7;
    const float layer_height = 0.1f;
    const int   base = 2;
    std::vector<int> relief = relief_pattern("ramp", width, height, 6);
    // One column that climbs the whole stack, so there is a texel of the picture lying on the build
    // plate rather than set into a recess. A ramp alone never quite reaches its own top.
    relief.back() = 6;
    // Thick enough to hold the whole stack, as a face-down plate has to be.
    ChromaStack stack = split_stack({ 3, 3 }, base, layer_height, ChromaOrientation::FaceDown, 2.f);
    const ChromaSplitPieces out = chroma_split_bands(stack, split_solve(width, height, relief, base));

    REQUIRE(out.bands.size() == 2);
    for (const indexed_triangle_set &piece : out.bands) {
        REQUIRE_FALSE(piece.indices.empty());
        REQUIRE(edges_not_shared_twice(piece) == 0);
        REQUIRE(edges_not_wound_consistently(piece) == 0);
        REQUIRE(enclosed_volume(piece) > 0.);
    }

    // The stack is printed upside down: the first band is the one the backing hangs off, at the top
    // of the plate, and the last is the one against the build plate at z = 0.
    REQUIRE_THAT(double(highest_z(out.bands.front())), WithinAbs(2., 1e-6));
    REQUIRE_THAT(double(lowest_z(out.bands.back())), WithinAbs(0., 1e-9));
    REQUIRE(lowest_z(out.bands.front()) > lowest_z(out.bands.back()));

    // And it is still the same plate: every column, the backing above it included.
    const double cell    = double(out.cell_x_mm) * double(out.cell_y_mm);
    const double backing = 2. - double(stack.relief_layers()) * double(layer_height);
    double       expected = 0.;
    for (int r : relief)
        expected += (double(r) * double(layer_height) + backing) * cell;
    double total = 0.;
    for (const indexed_triangle_set &piece : out.bands)
        total += enclosed_volume(piece);
    REQUIRE_THAT(total, WithinRel(expected, 1e-5));
}

TEST_CASE("A picture painted onto somebody else's model is not split", "[ChromaSplit]")
{
    const int              width = 5, height = 5;
    const std::vector<int> relief(size_t(width) * size_t(height), 2);
    ChromaStack            stack = split_stack({ 2, 2 }, 2, 0.1f);
    stack.plate.generated        = false;

    REQUIRE(chroma_split_bands(stack, split_solve(width, height, relief, 2)).bands.empty());
}

TEST_CASE("A plate with a frame is split without it, and says so", "[ChromaSplit]")
{
    const int              width = 7, height = 5;
    const std::vector<int> relief(size_t(width) * size_t(height), 2);
    ChromaStack            stack  = split_stack({ 2, 2 }, 2, 0.1f);
    stack.plate.frame.width_mm    = 6.f;
    const ChromaSplitPieces out   = chroma_split_bands(stack, split_solve(width, height, relief, 2));

    REQUIRE(out.frame_omitted);
    REQUIRE_FALSE(out.bands.front().indices.empty());
    // The pieces are the picture's own rectangle, with nothing of the frame outside it.
    REQUIRE_THAT(double(out.cell_x_mm) * double(out.grid_width), WithinRel(double(stack.plate.width_mm), 1e-5));
}

TEST_CASE("A picture too fine to mesh cell for cell is coarsened to the budget", "[ChromaSplit]")
{
    const int              width = 64, height = 48;
    const std::vector<int> relief = relief_pattern("ramp", width, height, 6);
    const ChromaStack      stack  = split_stack({ 3, 3 }, 2, 0.1f);

    ChromaSplitOptions options;
    options.max_cells = 400;
    const ChromaSplitPieces out = chroma_split_bands(stack, split_solve(width, height, relief, 2), options);

    REQUIRE(out.grid_width * out.grid_height <= options.max_cells);
    REQUIRE(out.grid_width > 0);
    for (const indexed_triangle_set &piece : out.bands)
        if (!piece.indices.empty())
            REQUIRE(edges_not_shared_twice(piece) == 0);
    // Coarsened or not, the pieces still cover the plate's own rectangle.
    REQUIRE_THAT(double(out.cell_x_mm) * double(out.grid_width), WithinRel(double(stack.plate.width_mm), 1e-5));
}

// The one thing a flat tile needs that a stack does not give it: the space the picture leaves empty.
//
// A stack carries its picture in the height of its columns, so the plate it makes tapers - a column
// that stops early leaves a void above it, or below it face down. A tile printed flat has to be there
// at every layer, so that void is filled and the colour comes from swapping filament inside a layer
// instead of from the height of the column. The split is where the fill is made, and the property
// that makes it usable is arithmetic rather than visual: the bands and the fill have to add up to the
// plate's own box, with nothing left out and nothing counted twice.
TEST_CASE("The fill completes a split into the plate's full box", "[ChromaSplit]")
{
    const std::string shape = GENERATE("ramp", "ring", "islands", "checkerboard");
    const bool        face_down = GENERATE(false, true);
    CAPTURE(shape, face_down);

    const int              width = 13, height = 11, base = 4, relief_layers = 9;
    const float            layer_height = 0.1f;
    const std::vector<int> relief = relief_pattern(shape, width, height, relief_layers);
    // Face down the plate's own thickness has to hold the whole stack, the backing included - there
    // is nothing under the picture there, so base_mm is the slab rather than a layer beneath it.
    const float       plate_base = face_down ? float(base + relief_layers) * layer_height : 1.f;
    const ChromaStack stack      = split_stack({ 3, 3, 3 }, base, layer_height,
                                               face_down ? ChromaOrientation::FaceDown : ChromaOrientation::FaceUp,
                                               plate_base);
    const ChromaSplitPieces out = chroma_split_bands(stack, split_solve(width, height, relief, base));
    REQUIRE(out.bands.size() == 3);

    // Every one of these patterns leaves some column short of the top, so there is a void to fill.
    REQUIRE_FALSE(out.fill.indices.empty());

    // The same two properties every band piece has to have, for the same reason: the fill is handed
    // to the same printer and the same boolean as the pieces it fills between.
    CHECK(edges_not_shared_twice(out.fill) == 0);
    CHECK(edges_not_wound_consistently(out.fill) == 0);
    CHECK(degenerate_triangles(out.fill) == 0);
    CHECK(enclosed_volume(out.fill) > 0.);

    // Where it reaches, which is the plate's own face at one end and the picture's shallowest column
    // at the other. A fill that stood proud of the slab would be the taper back again, on the other
    // side of the plate.
    const double thickness  = double(base + relief_layers) * double(layer_height);
    const int    min_relief = *std::min_element(relief.begin(), relief.end());
    if (face_down) {
        // The void is under the picture, so it reaches the build plate wherever a column is short of
        // the stack's full depth, and stops at the shallowest column's own face.
        CHECK_THAT(double(lowest_z(out.fill)), WithinAbs(0., 1e-5));
        CHECK_THAT(double(highest_z(out.fill)),
                   WithinAbs(double(relief_layers - min_relief) * double(layer_height), 1e-5));
    } else {
        CHECK_THAT(double(lowest_z(out.fill)), WithinAbs(double(base + min_relief) * double(layer_height), 1e-5));
        CHECK_THAT(double(highest_z(out.fill)), WithinAbs(thickness, 1e-5));
    }

    // Bands plus fill is the plate's own box. Held to a part in a million rather than to a visible
    // tolerance because the arithmetic really is exact: a level count is a small integer and a layer
    // height is a float, so every height either piece works out is exact in double and the two land
    // on the same float.
    const double cell     = double(out.cell_x_mm) * double(out.cell_y_mm);
    const double expected = thickness * cell * double(width) * double(height);

    double total = enclosed_volume(out.fill);
    for (const indexed_triangle_set &piece : out.bands)
        if (!piece.indices.empty())
            total += enclosed_volume(piece);
    REQUIRE_THAT(total, WithinRel(expected, 1e-6));
}

TEST_CASE("A fill behind the picture completes the plate and carries its backing", "[ChromaSplit]")
{
    const std::string shape     = GENERATE("ramp", "ring", "islands", "checkerboard");
    const bool        face_down = GENERATE(false, true);
    CAPTURE(shape, face_down);

    const int              width = 13, height = 11, base = 4, relief_layers = 9;
    const float            layer_height = 0.1f;
    const std::vector<int> relief = relief_pattern(shape, width, height, relief_layers);
    const float            plate_base = face_down ? float(base + relief_layers) * layer_height : 1.f;
    const ChromaStack      stack = split_stack({ 3, 3, 3 }, base, layer_height,
                                               face_down ? ChromaOrientation::FaceDown : ChromaOrientation::FaceUp,
                                               plate_base);

    ChromaSplitOptions options;
    options.flat_mode = ChromaFlatMode::BackingBehind;
    const ChromaSplitPieces out = chroma_split_bands(stack, split_solve(width, height, relief, base), options);
    REQUIRE(out.bands.size() == 3);

    // The fill is the backing here, so it is there even where the picture reaches the whole way.
    REQUIRE_FALSE(out.fill.indices.empty());
    CHECK(edges_not_shared_twice(out.fill) == 0);
    CHECK(edges_not_wound_consistently(out.fill) == 0);
    CHECK(degenerate_triangles(out.fill) == 0);
    CHECK(enclosed_volume(out.fill) > 0.);

    // Where it reaches: the plate's far face at one end, and the shallowest column's own terrace at
    // the other. The near face belongs to the picture at every texel, which is what makes the tile
    // flat without anything of the fill showing.
    const double thickness  = double(base + relief_layers) * double(layer_height);
    const int    min_relief = *std::min_element(relief.begin(), relief.end());
    if (face_down) {
        CHECK_THAT(double(lowest_z(out.fill)), WithinAbs(double(min_relief) * double(layer_height), 1e-5));
        CHECK_THAT(double(highest_z(out.fill)), WithinAbs(thickness, 1e-5));
    } else {
        CHECK_THAT(double(lowest_z(out.fill)), WithinAbs(0., 1e-5));
        CHECK_THAT(double(highest_z(out.fill)),
                   WithinAbs(thickness - double(min_relief) * double(layer_height), 1e-5));
    }

    // Bands plus fill is the plate's own box, to the same part in a million the other arrangement is
    // held to: both are built from level counts and one float layer height.
    const double cell     = double(out.cell_x_mm) * double(out.cell_y_mm);
    const double expected = thickness * cell * double(width) * double(height);

    double total = enclosed_volume(out.fill);
    for (const indexed_triangle_set &piece : out.bands)
        if (!piece.indices.empty())
            total += enclosed_volume(piece);
    REQUIRE_THAT(total, WithinRel(expected, 1e-6));
}

TEST_CASE("A fill behind the picture hangs the first band on the face the light enters", "[ChromaSplit]")
{
    const bool face_down = GENERATE(false, true);
    CAPTURE(face_down);

    const int   width = 9, height = 7, base = 3, relief_layers = 6;
    const float layer_height = 0.1f;
    // Every column climbs the whole stack somewhere, so both bands exist in both arrangements and the
    // comparison below is about where they are and not about whether they are there.
    std::vector<int> relief = relief_pattern("ramp", width, height, relief_layers);
    relief.back() = relief_layers;
    const float       plate_base = face_down ? float(base + relief_layers) * layer_height : 1.f;
    const ChromaStack stack      = split_stack({ 3, 3 }, base, layer_height,
                                               face_down ? ChromaOrientation::FaceDown : ChromaOrientation::FaceUp,
                                               plate_base);
    const ChromaSolve solve      = split_solve(width, height, relief, base);

    ChromaSplitOptions behind;
    behind.flat_mode = ChromaFlatMode::BackingBehind;
    const ChromaSplitPieces out = chroma_split_bands(stack, solve, behind);
    REQUIRE(out.bands.size() == 2);
    REQUIRE_FALSE(out.bands.front().indices.empty());
    REQUIRE_FALSE(out.bands.back().indices.empty());

    // The picture's face is z = 0 face down and the top of the plate face up. Either way the first
    // band is the one against it: the light meets the bands in their own order, first band first.
    const double thickness = double(base + relief_layers) * double(layer_height);
    const double first_mm  = 3. * double(layer_height);
    if (face_down) {
        CHECK_THAT(double(lowest_z(out.bands.front())), WithinAbs(0., 1e-9));
        CHECK_THAT(double(highest_z(out.bands.front())), WithinAbs(first_mm, 1e-6));
        CHECK_THAT(double(lowest_z(out.bands.back())), WithinAbs(first_mm, 1e-6));
    } else {
        CHECK_THAT(double(highest_z(out.bands.front())), WithinAbs(thickness, 1e-6));
        CHECK_THAT(double(lowest_z(out.bands.front())), WithinAbs(thickness - first_mm, 1e-6));
        CHECK_THAT(double(highest_z(out.bands.back())), WithinAbs(thickness - first_mm, 1e-6));
    }

    // And the other arrangement puts it the other way round, which is the whole difference between
    // the two: the same bands, crossed in the opposite order.
    const ChromaSplitPieces front = chroma_split_bands(stack, solve);
    REQUIRE(front.bands.size() == 2);
    if (face_down)
        CHECK(lowest_z(front.bands.front()) > lowest_z(front.bands.back()));
    else
        CHECK(lowest_z(front.bands.front()) < lowest_z(front.bands.back()));
    if (face_down)
        CHECK(lowest_z(out.bands.front()) < lowest_z(out.bands.back()));
    else
        CHECK(lowest_z(out.bands.front()) > lowest_z(out.bands.back()));
}

TEST_CASE("FlatStack slides every column's own sequence up against the flat face", "[ChromaSplit]")
{
    const std::string shape     = GENERATE("ramp", "ring", "islands", "checkerboard");
    const bool        face_down = GENERATE(false, true);
    CAPTURE(shape, face_down);

    const int              width = 13, height = 11, base = 4, relief_layers = 9;
    const float            layer_height = 0.1f;
    const std::vector<int> relief = relief_pattern(shape, width, height, relief_layers);
    const float            plate_base = face_down ? float(base + relief_layers) * layer_height : 1.f;
    const ChromaStack      stack = split_stack({ 3, 3, 3 }, base, layer_height,
                                               face_down ? ChromaOrientation::FaceDown : ChromaOrientation::FaceUp,
                                               plate_base);

    ChromaSplitOptions options;
    options.flat_mode = ChromaFlatMode::FlatStack;
    const ChromaSplitPieces out = chroma_split_bands(stack, split_solve(width, height, relief, base), options);
    REQUIRE(out.bands.size() == 3);

    // Every piece is still a solid a printer and a boolean can both take, which is the property the
    // per-cell floor could most easily have broken: a wall is now built from two stretches rather than
    // one, and a stretch built twice or not at all shows up here.
    for (const indexed_triangle_set &piece : out.bands) {
        REQUIRE_FALSE(piece.indices.empty());
        CHECK(edges_not_shared_twice(piece) == 0);
        CHECK(edges_not_wound_consistently(piece) == 0);
        CHECK(degenerate_triangles(piece) == 0);
        CHECK(enclosed_volume(piece) > 0.);
    }
    CHECK(edges_not_shared_twice(out.fill) == 0);
    CHECK(edges_not_wound_consistently(out.fill) == 0);

    // The band a column ends on is the one against the face. So the last band reaches the face
    // wherever a column climbs the whole stack, and the first band reaches as deep as the deepest
    // column - which is the pair of facts that say the sequence was slid rather than kept in place.
    //
    // The first band is at the face too wherever a column is short enough to end inside it, which is
    // not an exception to that: such a column shows only the first band, so the first band is what it
    // ends on. That is what "islands" has and a ramp does not.
    const double thickness  = double(base + relief_layers) * double(layer_height);
    const double face_z     = face_down ? 0. : thickness;
    // The deepest column these patterns actually reach, which a ramp leaves one layer short of the
    // stack's own top.
    const int    max_relief = *std::max_element(relief.begin(), relief.end());
    const double deepest    = double(max_relief) * double(layer_height);
    if (face_down) {
        CHECK_THAT(double(lowest_z(out.bands.back())), WithinAbs(face_z, 1e-6));
        CHECK_THAT(double(highest_z(out.bands.front())), WithinAbs(face_z + deepest, 1e-5));
    } else {
        CHECK_THAT(double(highest_z(out.bands.back())), WithinAbs(face_z, 1e-6));
        CHECK_THAT(double(lowest_z(out.bands.front())), WithinAbs(face_z - deepest, 1e-5));
    }

    // And it is still the plate's own box: the bands tile every column from the face inwards and the
    // fill carries on to the far side.
    const double cell     = double(out.cell_x_mm) * double(out.cell_y_mm);
    const double expected = thickness * cell * double(width) * double(height);
    double       total    = enclosed_volume(out.fill);
    for (const indexed_triangle_set &piece : out.bands)
        total += enclosed_volume(piece);
    REQUIRE_THAT(total, WithinRel(expected, 1e-6));
}

TEST_CASE("FlatStack and the one-height arrangements hold the same filaments in a column", "[ChromaSplit]")
{
    const int   width = 9, height = 7, base = 3, relief_layers = 9;
    const float layer_height = 0.1f;
    // A ramp, so the columns between them take every level the stack has.
    const std::vector<int> relief = relief_pattern("ramp", width, height, relief_layers);
    const ChromaStack      stack  = split_stack({ 3, 3, 3 }, base, layer_height, ChromaOrientation::FaceDown,
                                                float(base + relief_layers) * layer_height);
    const ChromaSolve      solve  = split_solve(width, height, relief, base);

    ChromaSplitOptions flat;
    flat.flat_mode = ChromaFlatMode::FlatStack;
    ChromaSplitOptions behind;
    behind.flat_mode = ChromaFlatMode::BackingBehind;
    const ChromaSplitPieces a = chroma_split_bands(stack, solve, flat);
    const ChromaSplitPieces b = chroma_split_bands(stack, solve, behind);
    REQUIRE(a.bands.size() == b.bands.size());

    // Band for band, the same amount of filament: the two arrangements move the bands about inside the
    // slab and neither adds or drops any of one. That is the claim the color argument rests on, and it
    // is the one thing about the pair that can be measured rather than looked at.
    for (size_t i = 0; i < a.bands.size(); ++i) {
        DYNAMIC_SECTION("band " << i) {
            CHECK_THAT(enclosed_volume(a.bands[i]), WithinRel(enclosed_volume(b.bands[i]), 1e-6));
        }
    }
    CHECK_THAT(enclosed_volume(a.fill), WithinRel(enclosed_volume(b.fill), 1e-6));
}
