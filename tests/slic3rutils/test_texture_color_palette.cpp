// The texture displacement gizmo's palette helpers live in libslic3r_gui; this is the suite that links it.
// Same Windows include prologue as test_filament_bitmap_utils.cpp (wx pulls in <windows.h>; keep
// WIN32_LEAN_AND_MEAN / NOMINMAX ahead of the Catch2 headers).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/Color.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "slic3r/GUI/Gizmos/GLGizmoTextureDisplacement.hpp"
#include "slic3r/Utils/ColorSpaceConvert.hpp"
#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Gizmo     = Slic3r::GUI::GLGizmoTextureDisplacement;
using Entry     = Gizmo::PaletteEntry;
using MixTarget = Gizmo::MixTarget;

namespace {

const ColorRGBA BLACK{ 0.f, 0.f, 0.f, 1.f };
const ColorRGBA WHITE{ 1.f, 1.f, 1.f, 1.f };
const ColorRGBA RED{ 1.f, 0.f, 0.f, 1.f };
const ColorRGBA BLUE{ 0.f, 0.f, 1.f, 1.f };
const ColorRGBA YELLOW{ 1.f, 1.f, 0.f, 1.f };

MixTarget target(const Vec3f &rgb, float weight)
{
    MixTarget t;
    RGB2Lab(rgb.x(), rgb.y(), rgb.z(), &t.lab.x(), &t.lab.y(), &t.lab.z());
    t.weight = weight;
    return t;
}

// The colour a mixed slot of these two filaments shows, as the sidebar computes it.
Vec3f slot_color(const ColorRGBA &a, const ColorRGBA &b, int a_percent)
{
    ColorRGB c;
    REQUIRE(decode_color(blend_color_multi({ encode_color(a), encode_color(b) }, { a_percent, 100 - a_percent }), c));
    return Vec3f(c.r(), c.g(), c.b());
}

// A colour image layer, through Slic3r's own PNG writer so decode_height_texture() reads it the way it
// reads an imported texture.
TextureDisplacementLayer color_layer(int w, int h, const std::vector<uint8_t> &rgb)
{
    ScopedTemporaryFile png(".png");
    REQUIRE(png::write_rgb_to_file(png.string(), size_t(w), size_t(h), rgb));
    std::ifstream              in(png.string(), std::ios::binary);
    std::vector<unsigned char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    REQUIRE_FALSE(bytes.empty());

    TextureDisplacementLayer layer;
    layer.image_data    = std::make_shared<std::vector<unsigned char>>(std::move(bytes));
    layer.color_enabled = true;
    return layer;
}

// A red/green ramp over a fixed blue: colours spread over many bins, so the image is not flat-colour.
TextureDisplacementLayer gradient_layer()
{
    const int            n = 64;
    std::vector<uint8_t> rgb;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            rgb.push_back(uint8_t(x * 4));
            rgb.push_back(uint8_t(y * 4));
            rgb.push_back(128);
        }
    return color_layer(n, n, rgb);
}

// A 2048x1100 image, over mix_targets()' sampling budget, that is pure red wherever `red(x, y)` holds and
// elsewhere a gradient spread over far more than eight coarse bins, so the image is not flat-colour.
template<class RedFn> TextureDisplacementLayer striped_layer(RedFn red)
{
    const int            w = 2048, h = 1100;
    std::vector<uint8_t> rgb;
    rgb.reserve(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool is_red = red(x, y);
            rgb.push_back(is_red ? 255 : uint8_t(x * 255 / w));
            rgb.push_back(is_red ? 0 : uint8_t(y * 255 / h));
            rgb.push_back(is_red ? 0 : 128);
        }
    return color_layer(w, h, rgb);
}

// How much of the targets' weight is pure red.
float red_weight(const std::vector<MixTarget> &targets)
{
    Vec3f red;
    RGB2Lab(1.f, 0.f, 0.f, &red.x(), &red.y(), &red.z());
    float weight = 0.f;
    for (const MixTarget &t : targets)
        if ((t.lab - red).norm() < 3.f)
            weight += t.weight;
    return weight;
}

} // namespace

TEST_CASE("mix targets cover each colouring photo layer once and skip the rest", "[TextureColorPalette][TextureDisplacement]")
{
    TextureDisplacementLayer photo = gradient_layer();

    const std::vector<MixTarget> targets = Gizmo::mix_targets({ photo });
    REQUIRE_FALSE(targets.empty());
    float total = 0.f;
    for (const MixTarget &t : targets)
        total += t.weight;
    REQUIRE_THAT(total, WithinAbs(1., 1e-4));

    SECTION("A layer that does not colour gives no targets") {
        photo.color_enabled = false;
        REQUIRE(Gizmo::mix_targets({ photo }).empty());
    }
    SECTION("A flat-colour image prints in single filaments, so it gives no targets") {
        const TextureDisplacementLayer flat = color_layer(8, 8, std::vector<uint8_t>(8 * 8 * 3, 200));
        REQUIRE(Gizmo::mix_targets({ flat }).empty());
    }
}

TEST_CASE("mix targets of a large image weigh each colour by its share, whatever its stripes", "[TextureColorPalette][TextureDisplacement]")
{
    // Sampled rather than read in full, these must not line up with the samples: a fixed sampling step
    // sees only one phase of a stripe pattern, and a generator whose offsets repeat sees only some.
    SECTION("Red on every other column") {
        CHECK_THAT(red_weight(Gizmo::mix_targets({ striped_layer([](int x, int) { return x % 2 == 0; }) })),
                   WithinAbs(1. / 2., 0.03));
    }
    SECTION("Red on every fourth column") {
        CHECK_THAT(red_weight(Gizmo::mix_targets({ striped_layer([](int x, int) { return x % 4 == 0; }) })),
                   WithinAbs(1. / 4., 0.03));
    }
    SECTION("Red on diagonals") {
        CHECK_THAT(red_weight(Gizmo::mix_targets({ striped_layer([](int x, int y) { return (x - y) % 3 == 0; }) })),
                   WithinAbs(1. / 3., 0.03));
    }
}

TEST_CASE("the mix ranked first is the one the image needs most", "[TextureColorPalette][TextureDisplacement]")
{
    // The whole image is exactly the colour of a 1:1 black/white slot.
    const std::vector<MixTarget> targets = { target(slot_color(BLACK, WHITE, 50), 1.f) };
    const std::vector<Entry>     ranked  = Gizmo::rank_mixes({ BLACK, WHITE }, targets, 1);
    REQUIRE(ranked.size() == 1);
    CHECK(ranked.front().a == 0);
    CHECK(ranked.front().b == 1);
    CHECK(ranked.front().num * 2 == ranked.front().den);
}

TEST_CASE("an image the filaments already match ranks no mixes", "[TextureColorPalette][TextureDisplacement]")
{
    const std::vector<MixTarget> targets = { target(Vec3f(1.f, 0.f, 0.f), 0.5f), target(Vec3f(0.f, 0.f, 1.f), 0.5f) };
    REQUIRE(Gizmo::rank_mixes({ RED, BLUE }, targets, 8).empty());
}

TEST_CASE("ranked mixes stay within the limit and show their slot's colour", "[TextureColorPalette][TextureDisplacement]")
{
    const std::vector<ColorRGBA> filaments = { RED, BLUE, YELLOW };
    const std::vector<MixTarget> targets   = Gizmo::mix_targets({ gradient_layer() });

    for (const int limit : { 1, 3 }) {
        const std::vector<Entry> ranked = Gizmo::rank_mixes(filaments, targets, limit);
        CHECK(int(ranked.size()) <= limit);
        CHECK_FALSE(ranked.empty());
    }

    const std::vector<Entry> ranked = Gizmo::rank_mixes(filaments, targets, 6);
    for (const Entry &e : ranked) {
        REQUIRE(e.is_mix());
        const Vec3f expected = slot_color(filaments[size_t(e.a)], filaments[size_t(e.b)], e.a_percent());
        CHECK_THAT(e.rgb.x(), WithinAbs(expected.x(), 1e-6));
        CHECK_THAT(e.rgb.y(), WithinAbs(expected.y(), 1e-6));
        CHECK_THAT(e.rgb.z(), WithinAbs(expected.z(), 1e-6));
    }
}

TEST_CASE("picked mixes never need more slots than the project has free", "[TextureColorPalette][TextureDisplacement]")
{
    const std::vector<Entry> ranking = { { Vec3f::Zero(), 0, 1, 1, 2 }, { Vec3f::Zero(), 0, 1, 1, 3 }, { Vec3f::Zero(), 0, 1, 2, 3 } };
    const auto               reusable_second = [](const Entry &e) { return e.num == 1 && e.den == 3; };

    SECTION("The count caps the pick") {
        REQUIRE(Gizmo::pick_mixes(ranking, 2, 10, nullptr).size() == 2);
    }
    SECTION("With no free slot only a mix that already has one is kept") {
        const std::vector<Entry> picked = Gizmo::pick_mixes(ranking, 3, 0, reusable_second);
        REQUIRE(picked.size() == 1);
        CHECK(picked.front().den == 3);
        CHECK(picked.front().num == 1);
    }
    SECTION("A reusable mix costs no free slot") {
        const std::vector<Entry> picked = Gizmo::pick_mixes(ranking, 3, 1, reusable_second);
        REQUIRE(picked.size() == 2);
        CHECK(picked[0].den == 2);
        CHECK(picked[1].den == 3);
    }
}

TEST_CASE("a baked mix paints the slot it was given, wherever that slot sits", "[TextureColorPalette][TextureDisplacement]")
{
    // Two filaments, then two mixes of them. Palette index 2 is a mix, but its slot need not be
    // filament 2: a project that already holds other mixed slots puts it further along.
    const std::vector<Entry> palette = Gizmo::make_palette({ BLACK, WHITE }, { { Vec3f::Zero(), 0, 1, 1, 2 }, { Vec3f::Zero(), 0, 1, 1, 3 } });
    REQUIRE(palette.size() == 4);
    // Filament 0, the 1:1 mix twice, nothing: the 1:2 mix is never used.
    const std::vector<uint8_t> triangle_color = { 1, 3, 3, 0 };

    std::vector<Entry> asked;
    const auto         slot_seven = [&asked](const Entry &e) {
        asked.push_back(e);
        return 7;
    };
    const std::vector<int> filament = Gizmo::palette_filaments(palette, triangle_color, slot_seven);
    REQUIRE(filament.size() == 4);
    CHECK(filament[0] == 0);
    CHECK(filament[1] == 1);
    CHECK(filament[2] == 7);
    CHECK(filament[3] == -1);
    // Asking creates a slot, so an unused mix is never asked for.
    REQUIRE(asked.size() == 1);
    CHECK(asked.front().den == 2);

    SECTION("A mix that gets no slot prints in its dominant component") {
        const std::vector<int> fallback = Gizmo::palette_filaments(palette, { 3, 4 }, [](const Entry &) { return -1; });
        CHECK(fallback[2] == 0); // 1:1 - the first component
        CHECK(fallback[3] == 1); // 1 part black in 3 - white dominates
    }
}

TEST_CASE("the model's colour paint is drawn by filament, and only where no layer paint covers it", "[TextureColorPalette][TextureDisplacement]")
{
    // A strip of four triangles: filament 2 on the first and last, filament 5 on the second, and the third
    // left to the volume's own filament.
    indexed_triangle_set strip;
    strip.vertices = { Vec3f(0, 0, 0), Vec3f(1, 0, 0), Vec3f(0, 1, 0), Vec3f(1, 1, 0), Vec3f(0, 2, 0), Vec3f(1, 2, 0) };
    strip.indices  = { stl_triangle_vertex_indices(0, 1, 2), stl_triangle_vertex_indices(1, 3, 2), stl_triangle_vertex_indices(2, 3, 4),
                       stl_triangle_vertex_indices(3, 5, 4) };
    const TriangleMesh mesh(strip);
    TriangleSelector   paint(mesh);
    paint.set_facet(0, EnforcerBlockerType(2));
    paint.set_facet(1, EnforcerBlockerType(5));
    paint.set_facet(3, EnforcerBlockerType(2));

    const Gizmo::PaintedColors colors = Gizmo::painted_colors(mesh, paint.serialize());
    // Grouped by filament; the triangle in the volume's own filament is never drawn.
    REQUIRE(colors.facets.indices.size() == 3);
    REQUIRE(colors.source.size() == 3);
    REQUIRE(colors.state.size() == 3);
    CHECK(colors.state == std::vector<int>{ 2, 2, 5 });
    CHECK(colors.source == std::vector<int>{ 0, 3, 1 });

    SECTION("A model triangle a layer's paint covers is left to the preview") {
        std::vector<bool> excluded(mesh.its.indices.size(), false);
        excluded[3] = true;
        const std::vector<size_t> kept = colors.outside(excluded);
        REQUIRE(kept.size() == 2);
        CHECK(colors.source[kept[0]] == 0);
        CHECK(colors.source[kept[1]] == 1);
    }
    SECTION("A mask shorter than the model leaves the rest drawn") {
        CHECK(colors.outside({ true }).size() == 2);
    }
}
