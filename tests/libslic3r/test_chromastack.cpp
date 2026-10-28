#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

#include <cereal/archives/binary.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/ChromaStack.hpp"
#include "libslic3r/TransmissionDistance.hpp"
#include "libslic3r/TextureDisplacement.hpp"
#include "libslic3r/PNGReadWrite.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

ChromaFilament filament(int index, const ColorRGB &color, float td_mm)
{
    ChromaFilament f;
    f.filament = index;
    f.color    = color;
    f.td_mm    = td_mm;
    return f;
}

ChromaBand band(const ChromaFilament &f, int layers)
{
    ChromaBand b;
    b.filament = f;
    b.layers   = layers;
    return b;
}

// An opaque filament (TD 1 hides what is under it within 0.1 mm) and a translucent one (TD 8 needs
// most of a millimetre), which is the whole range the model has to behave sensibly over.
const ChromaFilament WHITE_OPAQUE      = filament(0, ColorRGB(1.f, 1.f, 1.f), 1.f);
const ChromaFilament BLACK_OPAQUE      = filament(1, ColorRGB(0.f, 0.f, 0.f), 1.f);
const ChromaFilament BLACK_TRANSLUCENT = filament(2, ColorRGB(0.f, 0.f, 0.f), 8.f);

float luminance(const ColorRGB &c) { return 0.2126f * c.r() + 0.7152f * c.g() + 0.0722f * c.b(); }

// A picture that is the same picture every run, so a test comparing two solves is comparing the
// solves rather than the noise. Anything with a spread of colours in it will do; what matters is that
// it is not a flat field, where every pixel reaches the same answer by every route.
std::vector<uint8_t> fixed_picture(int width, int height)
{
    std::vector<uint8_t> out(size_t(width) * size_t(height) * 3);
    uint32_t             state = 123456789u;
    for (uint8_t &v : out) {
        state = state * 1103515245u + 12345u;
        v     = uint8_t((state >> 16) & 0xffu);
    }
    return out;
}

// A picture of one colour, which is what a test that is about heights rather than about colours wants:
// every pixel then starts from the same column and an edit is the only thing that can move it.
std::vector<uint8_t> flat_picture(const ColorRGB &color, int width, int height)
{
    std::vector<uint8_t> out(size_t(width) * size_t(height) * 3);
    for (size_t i = 0; i < out.size(); i += 3) {
        out[i]     = uint8_t(std::lround(color.r() * 255.f));
        out[i + 1] = uint8_t(std::lround(color.g() * 255.f));
        out[i + 2] = uint8_t(std::lround(color.b() * 255.f));
    }
    return out;
}

// CIELAB and CIE76 the way ChromaStack.cpp computes them, so a test can say which reachable colour is
// nearest a pixel without asking the thing under test.
std::array<float, 3> to_lab(float r, float g, float b)
{
    auto        s2l = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    const float rl = s2l(r), gl = s2l(g), bl = s2l(b);
    const float x = (0.4124f * rl + 0.3576f * gl + 0.1805f * bl) / 0.95047f;
    const float y = (0.2126f * rl + 0.7152f * gl + 0.0722f * bl);
    const float z = (0.0193f * rl + 0.1192f * gl + 0.9505f * bl) / 1.08883f;
    auto        f = [](float t) { return t > 0.008856f ? std::cbrt(t) : (7.787f * t + 16.f / 116.f); };
    const float fx = f(x), fy = f(y), fz = f(z);
    return { 116.f * fy - 16.f, 500.f * (fx - fy), 200.f * (fy - fz) };
}

float cie76(const std::array<float, 3> &p, const std::array<float, 3> &q)
{
    const float dl = p[0] - q[0], da = p[1] - q[1], db = p[2] - q[2];
    return std::sqrt(dl * dl + da * da + db * db);
}

// A byte mask over width x height, with everything left of `split` inside the region.
std::vector<uint8_t> left_half_mask(int width, int height, int split)
{
    std::vector<uint8_t> mask(size_t(width) * size_t(height), 0);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < split; ++x)
            mask[size_t(y) * size_t(width) + size_t(x)] = 1;
    return mask;
}

} // namespace

TEST_CASE("A stack's palette has one entry per printed layer", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 4;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 10) };

    // The base is backing under the picture, not part of a band: the bands are the 12 layers the
    // picture is built from, and the stack is those on top of the 4 the model already has.
    REQUIRE(stack.relief_layers() == 12);
    REQUIRE(stack.total_layers() == 16);
    REQUIRE_THAT(stack.depth_mm(), WithinAbs(12 * 0.08f, 1e-6f));

    const std::vector<ColorRGB> palette = chroma_palette(stack);
    REQUIRE(palette.size() == 17);
    // Index 0 is the base's own colour, so a column one layer tall is exactly its filament - whatever
    // that filament's transmission distance.
    REQUIRE(palette[0] == palette[1]);
    REQUIRE_THAT(luminance(palette[1]), WithinAbs(1.f, 1e-3f));
}

TEST_CASE("A run of one filament gives the same palette however it is split into bands", "[ChromaStack]")
{
    // The property that justifies compositing layer by layer at all: transmittance multiplies, so
    // cutting a run in two must not change the colours. A model without it would make the palette
    // depend on how the stack was typed in.
    ChromaStack one;
    one.layer_height_mm = 0.1f;
    one.base_layers     = 0;
    one.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };

    ChromaStack split = one;
    split.bands         = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 3), band(BLACK_TRANSLUCENT, 5) };

    const std::vector<ColorRGB> a = chroma_palette(one);
    const std::vector<ColorRGB> b = chroma_palette(split);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); ++i)
        REQUIRE_THAT(luminance(a[i]), WithinAbs(luminance(b[i]), 1e-5f));
}

TEST_CASE("A translucent filament darkens gradually and an opaque one at once", "[ChromaStack]")
{
    ChromaStack translucent;
    translucent.layer_height_mm = 0.08f;
    translucent.base_layers     = 0;
    translucent.bands           = { band(WHITE_OPAQUE, 1), band(BLACK_TRANSLUCENT, 12) };
    const std::vector<ColorRGB> slow = chroma_palette(translucent);

    // Monotonically darker, and still short of the filament's own black after a single layer: that
    // gradient is what a ChromaStack stack prints its midtones with.
    for (size_t i = 2; i < slow.size(); ++i)
        REQUIRE(luminance(slow[i]) < luminance(slow[i - 1]));
    REQUIRE(luminance(slow[2]) > 0.5f);
    // Twelve layers of a TD 8 filament pass under 3% of the light, which is the dark end of the ramp
    // rather than true black - and that is the honest prediction, not a rounding artifact.
    REQUIRE(luminance(slow.back()) < 0.25f);

    ChromaStack opaque;
    opaque.layer_height_mm = 0.08f;
    opaque.base_layers     = 0;
    opaque.bands           = { band(WHITE_OPAQUE, 1), band(BLACK_OPAQUE, 12) };
    const std::vector<ColorRGB> fast = chroma_palette(opaque);
    // TD 1 is opaque within about 0.1 mm, so two layers of it leave little of the white showing.
    REQUIRE(luminance(fast[3]) < 0.2f);
}

TEST_CASE("Solving a picture of the palette's own colors reproduces it exactly", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };

    const std::vector<ColorRGB> palette = chroma_palette(stack);
    const int                   relief  = stack.relief_layers();

    // One pixel per reachable height, in order.
    std::vector<uint8_t> rgb;
    for (int i = 0; i <= relief; ++i) {
        const ColorRGB &c = palette[size_t(stack.base_layers + i)];
        rgb.push_back(uint8_t(std::lround(c.r() * 255.f)));
        rgb.push_back(uint8_t(std::lround(c.g() * 255.f)));
        rgb.push_back(uint8_t(std::lround(c.b() * 255.f)));
    }

    const ChromaSolve solve = chroma_solve(rgb.data(), relief + 1, 1, stack);
    REQUIRE_FALSE(solve.empty());
    REQUIRE(solve.width == relief + 1);
    REQUIRE(solve.levels.size() == size_t(relief + 1));
    // Every pixel comes back as the colour it was generated from, and the error is rounding only.
    //
    // The colour rather than the height: the bottom band prints in the same filament as the base, so
    // its first layers show exactly the colour the base already showed, and any of those heights is a
    // correct answer for that pixel. Asserting the height would be asserting which of several equally
    // right answers the search happens to reach first.
    for (int i = 0; i <= relief; ++i) {
        const int chosen = int(solve.levels[size_t(i)]);
        REQUIRE(chosen >= stack.base_layers);
        REQUIRE(chosen <= stack.total_layers());
        const ColorRGB &got  = palette[size_t(chosen)];
        const ColorRGB &want = palette[size_t(stack.base_layers + i)];
        REQUIRE_THAT(got.r(), WithinAbs(want.r(), 2.f / 255.f));
        REQUIRE_THAT(got.g(), WithinAbs(want.g(), 2.f / 255.f));
        REQUIRE_THAT(got.b(), WithinAbs(want.b(), 2.f / 255.f));
    }
    // Only 8-bit rounding of the palette separates the two, which near black is still under one dE.
    REQUIRE(solve.max_delta_e < 2.);

    // The height map's ends are the base and the top of the stack, which is what ties a grey level to
    // a printed layer.
    REQUIRE(solve.grey.front() == 0);
    REQUIRE(solve.grey.back() == 255);
}

TEST_CASE("A picture the filaments cannot reach is reported as a poor match", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };

    // Saturated red against a stack of white and black: no height can produce it, and the panel's
    // warning depends on the solve saying so rather than silently picking a grey.
    const std::vector<uint8_t> red{ 255, 0, 0 };
    const ChromaSolve        solve = chroma_solve(red.data(), 1, 1, stack);
    REQUIRE_FALSE(solve.empty());
    REQUIRE(solve.mean_delta_e > 15.);
}

TEST_CASE("Dithering keeps every pixel in the stack's range", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };
    stack.dither          = true;

    // A flat mid-grey: the error diffusion has to spread it over neighbouring heights without ever
    // leaving the stack, which is what a clamped palette index would quietly get wrong.
    std::vector<uint8_t> grey(size_t(16 * 16) * 3, 128);
    const ChromaSolve  solve = chroma_solve(grey.data(), 16, 16, stack);
    REQUIRE_FALSE(solve.empty());
    REQUIRE(solve.levels.size() == size_t(16 * 16));
    const auto [lo, hi] = std::minmax_element(solve.levels.begin(), solve.levels.end());
    REQUIRE(int(*lo) >= stack.base_layers);
    REQUIRE(int(*hi) <= stack.total_layers());
}

TEST_CASE("The solved height map is an 8-bit grayscale PNG of the picture's size", "[ChromaStack]")
{
    // The displacement pipeline reads one thing from a layer: an 8-bit grayscale PNG. A solve that
    // produced anything else would leave the relief silently flat.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 6) };

    std::vector<uint8_t> picture(size_t(8 * 4) * 3);
    for (size_t i = 0; i < picture.size(); ++i)
        picture[i] = uint8_t(i % 256);

    const ChromaSolve              solve = chroma_solve(picture.data(), 8, 4, stack);
    const std::vector<unsigned char> png   = chroma_encode_height_png(solve);
    REQUIRE_FALSE(png.empty());

    const png::ReadBuf  buf{ png.data(), png.size() };
    png::ImageGreyscale decoded;
    REQUIRE(png::is_png(buf));
    REQUIRE(png::decode_png(buf, decoded));
    REQUIRE(decoded.cols == 8);
    REQUIRE(decoded.rows == 4);
    REQUIRE(decoded.buf == solve.grey);
}

TEST_CASE("Each band becomes a height range, and a repeated filament is not a swap", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 0;
    stack.bands           = { band(WHITE_OPAQUE, 5), band(BLACK_OPAQUE, 3), band(BLACK_OPAQUE, 2) };

    const std::vector<ChromaRange> ranges = chroma_ranges(stack, 0.);
    // Three bands, two filaments: the second and third merge, because nothing is swapped between them.
    REQUIRE(ranges.size() == 2);
    REQUIRE_THAT(ranges[0].z_min, WithinAbs(0., 1e-9));
    REQUIRE_THAT(ranges[0].z_max, WithinAbs(0.5, 1e-9));
    REQUIRE(ranges[0].filament == WHITE_OPAQUE.filament);
    REQUIRE_THAT(ranges[1].z_min, WithinAbs(0.5, 1e-9));
    REQUIRE_THAT(ranges[1].z_max, WithinAbs(1.0, 1e-9));
    REQUIRE(ranges[1].filament == BLACK_OPAQUE.filament);
}

TEST_CASE("A default stack puts the lightest filament at the bottom", "[ChromaStack]")
{
    // Light to dark is what gives the long tonal ramp; the order is the one thing a seeded stack has
    // to get right, since everything else is a layer count the user can drag.
    const std::vector<ChromaFilament> loaded{ BLACK_OPAQUE, WHITE_OPAQUE,
                                                filament(3, ColorRGB(0.5f, 0.5f, 0.5f), 4.f) };
    const ChromaStack stack = chroma_default_stack(loaded, 0.08f, 4, 24);

    REQUIRE(stack.bands.size() == 3);
    REQUIRE(stack.bands.front().filament.filament == WHITE_OPAQUE.filament);
    REQUIRE(stack.bands.back().filament.filament == BLACK_OPAQUE.filament);
    REQUIRE(stack.base_layers == 4);
    // The base is printed in the bottom band, so the relief is what was asked for and the base sits
    // underneath it.
    REQUIRE(stack.total_layers() == 4 + 24);
    REQUIRE(stack.relief_layers() == 24);
    REQUIRE_THAT(stack.layer_height_mm, WithinRel(0.08f));
}

TEST_CASE("An empty stack or picture solves to nothing", "[ChromaStack]")
{
    ChromaStack empty;
    REQUIRE(chroma_palette(empty).empty());
    REQUIRE(chroma_ranges(empty, 0.).empty());

    // A base with nothing on it: there is no picture to solve, and no filament to print the base in.
    ChromaStack base_only;
    base_only.layer_height_mm = 0.08f;
    base_only.base_layers     = 4;
    REQUIRE(base_only.relief_layers() == 0);
    REQUIRE(chroma_palette(base_only).empty());
    const std::vector<uint8_t> pixel{ 128, 128, 128 };
    REQUIRE(chroma_solve(pixel.data(), 1, 1, base_only).empty());

    ChromaStack usable;
    usable.layer_height_mm = 0.08f;
    usable.base_layers     = 2;
    usable.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_OPAQUE, 4) };
    REQUIRE(chroma_solve(nullptr, 4, 4, usable).empty());
}

TEST_CASE("The base extends the bottom band's range down to the first layer", "[ChromaStack]")
{
    // The base is not a swap of its own: it prints in whatever is loaded at the start, which is the
    // bottom band's filament, so the first range has to reach the build plate.
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 20;
    stack.bands           = { band(WHITE_OPAQUE, 5), band(BLACK_OPAQUE, 5) };

    const std::vector<ChromaRange> ranges = chroma_ranges(stack, 0.);
    REQUIRE(ranges.size() == 2);
    REQUIRE_THAT(ranges[0].z_min, WithinAbs(0., 1e-9));
    REQUIRE_THAT(ranges[0].z_max, WithinAbs(2.5, 1e-9)); // 20 base + 5 band layers at 0.1 mm
    REQUIRE_THAT(ranges[1].z_min, WithinAbs(2.5, 1e-9));
    REQUIRE_THAT(ranges[1].z_max, WithinAbs(3.0, 1e-9));
    REQUIRE(ranges.back().z_max == Catch::Approx(double(stack.total_layers()) * double(stack.layer_height_mm)));
}

TEST_CASE("A surface's color follows the printed layer it sits at", "[ChromaStack]")
{
    // What the preview colours by: a swap happens at one print Z, so the layer a surface sits in is
    // what decides its colour, whatever the height map says.
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 3), band(BLACK_OPAQUE, 5) };

    REQUIRE(chroma_level_at(stack, 0.05) == 1);  // inside the first layer
    REQUIRE(chroma_level_at(stack, 0.10) == 1);  // exactly its top, which that layer printed
    REQUIRE(chroma_level_at(stack, 0.15) == 2);
    REQUIRE(chroma_level_at(stack, 0.50) == 5);  // the top of the base plus the white band
    // Clamped at both ends: below the plate and above the stack there is nothing else to show.
    REQUIRE(chroma_level_at(stack, -1.) == 1);
    REQUIRE(chroma_level_at(stack, 99.) == stack.total_layers());
}

TEST_CASE("How fast a filament covers is its TD against the layer height", "[ChromaStack]")
{
    // The number that decides whether a band is a gradient or a step, and the one the panel reports:
    // a filament hides what is under it at about a tenth of its TD, so how many layers that is
    // depends entirely on how thick they are.
    REQUIRE_THAT(chroma_layers_to_opaque(4.f, 0.2f), WithinRel(2.f, 1e-4f));
    REQUIRE_THAT(chroma_layers_to_opaque(4.f, 0.08f), WithinRel(5.f, 1e-4f));
    REQUIRE_THAT(chroma_layers_to_opaque(8.f, 0.08f), WithinRel(10.f, 1e-4f));
    // An opaque filament at a coarse layer height covers within one layer, which is where a stack
    // stops having any colours between its filaments at all.
    REQUIRE(chroma_layers_to_opaque(1.f, 0.2f) < 1.f);
    REQUIRE_THAT(chroma_layers_to_opaque(4.f, 0.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A finer layer height buys colors between the filaments", "[ChromaStack]")
{
    // The whole of the user-visible problem in one assertion: the same stack, the same filaments, two
    // layer heights. The coarse one steps from white to black in two layers; the fine one fades.
    const auto distinct = [](float layer_height) {
        ChromaStack stack;
        stack.layer_height_mm = layer_height;
        stack.base_layers     = 0;
        stack.bands           = { band(WHITE_OPAQUE, 1), band(filament(5, ColorRGB(0.f, 0.f, 0.f), 8.f), 10) };
        const std::vector<ColorRGB> palette = chroma_palette(stack);
        int                         n       = 0;
        for (size_t i = 1; i < palette.size(); ++i)
            if (std::abs(luminance(palette[i]) - luminance(palette[i - 1])) > 0.02f)
                ++n;
        return n;
    };
    REQUIRE(distinct(0.08f) > distinct(0.3f));
}

TEST_CASE("Layers past a filament's covering depth add no color", "[ChromaStack]")
{
    // The band is twice as long as the filament needs to hide what is under it, so half of it shows a
    // colour the layer below already showed. That is invisible in the stack and obvious here.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 0;
    stack.bands           = { band(WHITE_OPAQUE, 1), band(filament(5, ColorRGB(0.f, 0.f, 0.f), 4.f), 10) };

    // Trimming keeps every layer that still changes the colour and drops the rest, so the stack gets
    // shorter and loses none of its colours.
    const int distinct = chroma_distinct_levels(stack);
    const int before   = stack.relief_layers();
    chroma_trim_bands(stack);
    REQUIRE(stack.relief_layers() <= before);
    REQUIRE(chroma_distinct_levels(stack) == distinct);
    // The white band is a single layer and has nothing to give up.
    REQUIRE(stack.bands.front().layers == 1);

    // A light band over a dark one is the case where layers really are wasted: it saturates within a
    // few layers, and the rest of a long band shows what the layer below already showed.
    ChromaStack light_over_dark;
    light_over_dark.layer_height_mm = 0.08f;
    light_over_dark.base_layers     = 0;
    light_over_dark.bands = { band(filament(5, ColorRGB(0.f, 0.f, 0.f), 4.f), 1), band(WHITE_OPAQUE, 14) };
    const int kept = chroma_distinct_levels(light_over_dark);
    chroma_trim_bands(light_over_dark);
    REQUIRE(light_over_dark.bands.back().layers < 14);
    REQUIRE(chroma_distinct_levels(light_over_dark) == kept);
}

TEST_CASE("A picture's colours become a weighted histogram", "[ChromaStack]")
{
    // Three quarters red, one quarter white: the bins carry that share, which is what makes a stack
    // that gets the red right score better than one that gets the white right.
    std::vector<uint8_t> rgb;
    for (int i = 0; i < 3; ++i) {
        rgb.push_back(230); rgb.push_back(20); rgb.push_back(20);
    }
    rgb.push_back(250); rgb.push_back(250); rgb.push_back(250);

    const ChromaHistogram hist = chroma_histogram(rgb.data(), 4, 1);
    REQUIRE(hist.bins.size() == 2);
    REQUIRE_THAT(hist.bins[0].second, WithinAbs(0.75f, 1e-5f));
    REQUIRE_THAT(hist.bins[1].second, WithinAbs(0.25f, 1e-5f));
    REQUIRE(hist.bins[0].first.r() > hist.bins[0].first.g());
    REQUIRE(chroma_histogram(nullptr, 4, 1).empty());
}

TEST_CASE("The picker chooses the filaments that reach the picture", "[ChromaStack]")
{
    // A picture of greens, and a choice between a green and two filaments that cannot make one. The
    // green has to be in the stack whatever else is.
    std::vector<uint8_t> rgb;
    for (int i = 0; i < 8; ++i) {
        rgb.push_back(40); rgb.push_back(150); rgb.push_back(60);
    }
    const ChromaHistogram hist = chroma_histogram(rgb.data(), 8, 1);

    const std::vector<ChromaFilament> available{ WHITE_OPAQUE, BLACK_OPAQUE,
                                                 filament(7, ColorRGB(0.16f, 0.59f, 0.24f), 4.f) };
    const ChromaStack chosen = chroma_auto_stack(hist, available, 2, 0.08f, 4, 20);
    REQUIRE(chosen.bands.size() == 2);
    bool has_green = false;
    for (const ChromaBand &b : chosen.bands)
        if (b.filament.filament == 7)
            has_green = true;
    REQUIRE(has_green);
    // And it is a better answer than the two it left out.
    ChromaStack without;
    without.layer_height_mm = 0.08f;
    without.base_layers     = 4;
    without.bands           = { band(WHITE_OPAQUE, 10), band(BLACK_OPAQUE, 10) };
    REQUIRE(chroma_stack_error(hist, chosen) < chroma_stack_error(hist, without));
}

TEST_CASE("More filaments cannot make the match worse", "[ChromaStack]")
{
    // The reason the panel offers 2, 3, 4 and 6: each is a real trade of swaps against accuracy, and
    // the trade only runs one way.
    std::vector<uint8_t> rgb;
    const uint8_t        pixels[][3] = { { 220, 40, 30 }, { 250, 200, 40 }, { 30, 70, 160 }, { 240, 240, 235 } };
    for (const auto &p : pixels)
        for (int i = 0; i < 3; ++i) {
            rgb.push_back(p[0]); rgb.push_back(p[1]); rgb.push_back(p[2]);
        }
    const ChromaHistogram hist = chroma_histogram(rgb.data(), 12, 1);

    const std::vector<ChromaFilament> available{
        WHITE_OPAQUE, BLACK_OPAQUE,
        filament(5, ColorRGB(0.86f, 0.16f, 0.12f), 4.f), filament(6, ColorRGB(0.98f, 0.78f, 0.16f), 5.f),
        filament(7, ColorRGB(0.12f, 0.27f, 0.63f), 4.f), filament(8, ColorRGB(0.16f, 0.59f, 0.24f), 4.f)
    };
    const double two  = chroma_stack_error(hist, chroma_auto_stack(hist, available, 2, 0.08f, 4, 24));
    const double four = chroma_stack_error(hist, chroma_auto_stack(hist, available, 4, 0.08f, 4, 24));
    REQUIRE(four <= two);
    REQUIRE(chroma_auto_stack(hist, available, 4, 0.08f, 4, 24).bands.size() == 4);
    // Nothing to choose from, or no room to print it, is an empty stack rather than a guess.
    REQUIRE(chroma_auto_stack(hist, {}, 4, 0.08f, 4, 24).bands.empty());
    REQUIRE(chroma_auto_stack(hist, available, 4, 0.08f, 4, 0).bands.empty());
}

TEST_CASE("Suggested colors beat the filaments that are loaded", "[ChromaStack]")
{
    // A picture of oranges and blues against a drawer holding neither. Picking from what is loaded has
    // a ceiling; the suggestion is what tells the user to go and buy an orange.
    std::vector<uint8_t> rgb;
    const uint8_t        pixels[][3] = { { 232, 126, 34 }, { 41, 98, 177 }, { 232, 126, 34 }, { 20, 20, 24 } };
    for (const auto &p : pixels)
        for (int i = 0; i < 4; ++i) {
            rgb.push_back(p[0]); rgb.push_back(p[1]); rgb.push_back(p[2]);
        }
    const ChromaHistogram hist = chroma_histogram(rgb.data(), 16, 1);

    const std::vector<ChromaFilament> loaded{ WHITE_OPAQUE, BLACK_OPAQUE };
    const std::vector<ChromaSuggestion> suggested = chroma_suggest_colors(hist, 3, loaded, 0.08f, 24);
    REQUIRE(suggested.size() == 3);

    // Every suggestion says how translucent it wants to be and how far the nearest loaded filament is,
    // which together are the whole shopping list.
    for (const ChromaSuggestion &s : suggested) {
        REQUIRE(s.td_mm > 0.f);
        REQUIRE(s.layers >= 1);
        REQUIRE(s.nearest >= 0);
    }

    // The suggested set reaches the picture better than the best stack buildable from the drawer.
    ChromaStack mine;
    mine.layer_height_mm = 0.08f;
    mine.base_layers     = 0;
    mine.bands           = { band(WHITE_OPAQUE, 8), band(BLACK_OPAQUE, 8) };

    ChromaStack theirs;
    theirs.layer_height_mm = 0.08f;
    theirs.base_layers     = 0;
    for (const ChromaSuggestion &s : suggested)
        theirs.bands.push_back(band(filament(-1, s.color, s.td_mm), s.layers));

    REQUIRE(chroma_stack_error(hist, theirs) < chroma_stack_error(hist, mine));
    // And at least one of them is a long way from anything in the drawer, which is the point.
    float furthest = 0.f;
    for (const ChromaSuggestion &s : suggested)
        furthest = std::max(furthest, s.nearest_de);
    REQUIRE(furthest > 10.f);
}

TEST_CASE("Suggestions are ordered light to dark, and bounded", "[ChromaStack]")
{
    std::vector<uint8_t> rgb;
    const uint8_t        pixels[][3] = { { 240, 240, 230 }, { 180, 60, 50 }, { 30, 30, 36 } };
    for (const auto &p : pixels)
        for (int i = 0; i < 5; ++i) {
            rgb.push_back(p[0]); rgb.push_back(p[1]); rgb.push_back(p[2]);
        }
    const ChromaHistogram hist = chroma_histogram(rgb.data(), 15, 1);

    const std::vector<ChromaSuggestion> s = chroma_suggest_colors(hist, 3, {}, 0.08f, 24);
    REQUIRE(s.size() == 3);
    const auto lum = [](const ColorRGB &c) { return 0.2126f * c.r() + 0.7152f * c.g() + 0.0722f * c.b(); };
    REQUIRE(lum(s.front().color) > lum(s.back().color));
    // Nothing loaded means nothing to compare against, not a wrong answer.
    REQUIRE(s.front().nearest == -1);
    // A picture with nothing in it, or no room to print it, suggests nothing rather than guessing.
    REQUIRE(chroma_suggest_colors(ChromaHistogram{}, 3, {}, 0.08f, 24).empty());
    REQUIRE(chroma_suggest_colors(hist, 3, {}, 0.08f, 0).empty());
}

TEST_CASE("The calibration wedge is a staircase of one layer per step", "[ChromaStack]")
{
    ChromaWedge wedge;
    wedge.steps           = 12;
    wedge.layer_height_mm = 0.08f;
    wedge.base_layers     = 5;
    wedge.step_depth_mm   = 7.f;
    wedge.width_mm        = 36.f;

    const indexed_triangle_set its = chroma_wedge_mesh(wedge);
    REQUIRE_FALSE(its.indices.empty());

    // The base plus one box per step, each box 8 vertices and 12 facets.
    REQUIRE(its.vertices.size() == size_t(8 * (wedge.steps + 1)));
    REQUIRE(its.indices.size() == size_t(12 * (wedge.steps + 1)));

    BoundingBoxf3 box;
    for (const Vec3f &v : its.vertices)
        box.merge(v.cast<double>());
    REQUIRE_THAT(box.min.z(), WithinAbs(0., 1e-6));
    // The tallest step stands `steps` layers above a base of `base_layers`.
    REQUIRE_THAT(box.max.z(), WithinAbs(double(wedge.base_layers + wedge.steps) * 0.08, 1e-5));
    REQUIRE_THAT(box.max.y(), WithinAbs(double(wedge.steps) * 7., 1e-5));
    // Every fifth step is widened so the steps can be counted in fives.
    REQUIRE(box.max.x() > double(wedge.width_mm));
}

TEST_CASE("A read-off step is a transmission distance", "[ChromaStack]")
{
    // The measurement and the model are the same relationship from opposite ends: a filament that
    // covers at step 5 of an 0.08 mm wedge is one that chroma_layers_to_opaque() then says covers in
    // five layers at that height.
    const float td = chroma_td_from_step(5, 0.08f);
    REQUIRE_THAT(td, WithinRel(4.f, 1e-4f));
    REQUIRE_THAT(chroma_layers_to_opaque(td, 0.08f), WithinRel(5.f, 1e-4f));

    REQUIRE_THAT(chroma_td_from_step(12, 0.08f), WithinRel(9.6f, 1e-4f));
    REQUIRE_THAT(chroma_td_from_step(0, 0.08f), WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(chroma_td_from_step(5, 0.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A stack survives the project file", "[ChromaStack]")
{
    // The height map alone is not the feature: without the picture and the filaments a saved project
    // reopens as a relief layer whose texture happens to be a staircase, and every colour it printed
    // is gone. The .3mf writes the layers as JSON, so this is where that is decided.
    std::vector<TextureDisplacementLayer> layers(1);
    TextureDisplacementLayer             &l = layers.front();
    l.slot           = 2;
    l.chroma_enabled = true;
    l.chroma_source_in_3mf = "Metadata/texture_displacement/1_2_source.png";
    l.chroma.layer_height_mm = 0.08f;
    l.chroma.base_layers     = 135;
    l.chroma.base_auto       = false;
    l.chroma.dither          = true;
    l.chroma.applied_ranges  = 123456789ull;
    l.chroma.bands = { band(filament(0, ColorRGB(0.78f, 0.27f, 0.17f), 10.f), 7),
                       band(filament(3, ColorRGB(0.08f, 0.08f, 0.09f), 2.5f), 11) };
    l.chroma.bands.front().filament.name = "Red ABS";

    const TextureDisplacementOptions options;
    std::vector<TextureDisplacementLayer> back;
    TextureDisplacementOptions            back_options;
    REQUIRE(texture_displacement_layers_from_json(texture_displacement_layers_to_json(layers, options), back,
                                                  back_options));
    REQUIRE(back.size() == 1);

    const TextureDisplacementLayer &r = back.front();
    REQUIRE(r.chroma_enabled);
    REQUIRE(r.chroma_source_in_3mf == l.chroma_source_in_3mf);
    REQUIRE(r.chroma.base_layers == 135);
    REQUIRE_FALSE(r.chroma.base_auto);
    REQUIRE(r.chroma.dither);
    REQUIRE(r.chroma.applied_ranges == 123456789ull);
    REQUIRE(r.chroma.bands.size() == 2);
    REQUIRE(r.chroma.bands[0].filament.filament == 0);
    REQUIRE(r.chroma.bands[0].filament.name == "Red ABS");
    REQUIRE(r.chroma.bands[0].layers == 7);
    REQUIRE_THAT(r.chroma.bands[0].filament.td_mm, WithinRel(10.f, 1e-4f));
    REQUIRE(r.chroma.bands[1].filament.filament == 3);
    // Colours come back as the bytes they are shown and printed as.
    REQUIRE_THAT(r.chroma.bands[0].filament.color.r(), WithinAbs(0.78f, 2.f / 255.f));
    REQUIRE_THAT(r.chroma.bands[1].filament.color.b(), WithinAbs(0.09f, 2.f / 255.f));

    // The palette the reloaded stack reaches is the one it was saved with.
    REQUIRE(chroma_palette(r.chroma).size() == chroma_palette(l.chroma).size());

    // A relief layer carries none of it, and reads back as one.
    std::vector<TextureDisplacementLayer> plain(1);
    plain.front().slot = 0;
    std::vector<TextureDisplacementLayer> plain_back;
    REQUIRE(texture_displacement_layers_from_json(texture_displacement_layers_to_json(plain, options), plain_back,
                                                  back_options));
    REQUIRE_FALSE(plain_back.front().chroma_enabled);
}

TEST_CASE("A layer's optical depth is what both the palette and the shader absorb by", "[ChromaStack]")
{
    // The shader composites the stack itself rather than reading a table, so it needs the same number
    // the palette absorbs by. One function owns the relation between a TD, a layer height and an
    // absorption, and this is what keeps the preview and the solve from drifting apart.
    const float density = chroma_layer_density(4.f, 0.08f);
    REQUIRE(density > 0.f);

    // One layer of it over an opaque white, by hand, is what the palette's second entry holds.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 0;
    stack.bands           = { band(WHITE_OPAQUE, 1), band(filament(5, ColorRGB(0.f, 0.f, 0.f), 4.f), 4) };
    const std::vector<ColorRGB> palette = chroma_palette(stack);
    REQUIRE(palette.size() == 6);

    const float t = std::exp(-density);
    auto s2l = [](float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); };
    auto l2s = [](float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f; };
    // White under one layer of black: (1 - t) * 0 + t * white, in linear light.
    REQUIRE_THAT(palette[2].r(), WithinAbs(l2s(t * s2l(1.f)), 2.f / 255.f));

    // Four layers of it is the same as one layer four times, which is why a band can be one step.
    REQUIRE_THAT(palette[5].r(), WithinAbs(l2s(std::pow(t, 4.f) * s2l(1.f)), 2.f / 255.f));
    // A thicker layer of the same filament takes out more.
    REQUIRE(chroma_layer_density(4.f, 0.2f) > density);
    REQUIRE_THAT(chroma_layer_density(4.f, 0.f), WithinAbs(0.f, 1e-6f));
}

TEST_CASE("A tilted surface's relief is deepened so its steps still land on the solved heights",
          "[ChromaStack]")
{
    // A relief of d mm along a surface's normal only raises the print by d * cos(tilt), so on a face
    // that is not level the steps would land low and every colour in the picture would come out as
    // the one below it. The correction is a per-texel factor on the height map and a deeper layer.
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 10;
    stack.bands           = { band(WHITE_OPAQUE, 10), band(filament(1, ColorRGB(0.f, 0.f, 0.f), 4.f), 10) };
    const float depth = stack.depth_mm();
    REQUIRE_THAT(depth, WithinAbs(2.f, 1e-5f)); // 20 relief layers of 0.1 mm

    ChromaSolve solve;
    solve.width  = 2;
    solve.height = 1;
    solve.levels = { 10, 30 };
    solve.grey   = { 0, 255 };

    SECTION("a level patch is left exactly as it was")
    {
        ChromaSurface flat;
        flat.width  = 2;
        flat.height = 1;
        flat.facing = { 1.f, 1.f };
        REQUIRE_THAT(chroma_tilt_relief(solve, stack, flat), WithinAbs(depth, 1e-5f));
        REQUIRE(solve.grey == std::vector<uint8_t>{ 0, 255 });
    }

    SECTION("a surface that was never measured is left as it was")
    {
        REQUIRE_THAT(chroma_tilt_relief(solve, stack, ChromaSurface{}), WithinAbs(depth, 1e-5f));
        REQUIRE(solve.grey == std::vector<uint8_t>{ 0, 255 });
    }

    SECTION("a 60 degree texel reaches the same print height as a level one did")
    {
        ChromaSurface tilted;
        tilted.width  = 2;
        tilted.height = 1;
        tilted.facing = { 1.f, 0.5f }; // level, and 60 degrees off it
        const float deepened = chroma_tilt_relief(solve, stack, tilted);
        // The steepest texel needed twice the relief, so the layer carries twice the depth.
        REQUIRE_THAT(deepened, WithinAbs(2.f * depth, 1e-4f));
        // ...and that texel still displaces the full relief, which at 60 degrees is twice as far
        // along the normal as it rises in Z.
        const float along_normal = float(solve.grey[1]) / 255.f * deepened;
        REQUIRE_THAT(along_normal * 0.5f, WithinAbs(depth, 0.02f));
        // The level texel beside it is untouched in Z: it now reads a smaller fraction of a deeper
        // layer, and the product is what it always was.
        solve.grey = { 128, 255 };
        ChromaSolve same = solve;
        const float again = chroma_tilt_relief(same, stack, tilted);
        REQUIRE_THAT(float(same.grey[0]) / 255.f * again, WithinAbs(128.f / 255.f * depth, 0.02f));
    }

    SECTION("past the limit the correction stops rather than running away")
    {
        // cos(89 degrees) is almost nothing, and 1/it is almost everything. The clamp is what keeps a
        // sliver of a nearly vertical wall from deepening the whole layer by a factor of fifty.
        ChromaSurface steep;
        steep.width  = 2;
        steep.height = 1;
        steep.facing = { 1.f, float(std::cos(89. * M_PI / 180.)) };
        const float capped = chroma_tilt_relief(solve, stack, steep);
        REQUIRE_THAT(capped, WithinAbs(depth / std::cos(CHROMA_MAX_TILT_DEG * float(M_PI) / 180.f), 1e-3f));
    }
}

// Every edge of a closed solid is shared by exactly two triangles. A plate that fails this has a hole
// in it, which slices into a part with a missing wall.
static bool is_closed(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> edges;
    for (const stl_triangle_vertex_indices &t : its.indices)
        for (int i = 0; i < 3; ++i) {
            const int a = t[i], b = t[(i + 1) % 3];
            ++edges[{ std::min(a, b), std::max(a, b) }];
        }
    for (const auto &[edge, count] : edges)
        if (count != 2)
            return false;
    return true;
}

TEST_CASE("A picture that has no model gets a plate made for it", "[ChromaStack]")
{
    ChromaPlate plate;
    plate.generated = true;
    plate.width_mm  = 80.f;
    plate.height_mm = 60.f;
    plate.base_mm   = 1.2f;
    plate.cell_mm   = 4.f; // coarse, so the test is about the shape rather than about CGAL's patience

    SECTION("with no frame it is a slab the size of the picture")
    {
        plate.frame.width_mm = 0.f;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE_FALSE(mesh.empty());
        REQUIRE(mesh.picture_triangles > 0);
        REQUIRE(is_closed(mesh.its));

        const BoundingBoxf3 box = bounding_box(mesh.its);
        REQUIRE_THAT(box.size().x(), WithinAbs(80., 1e-3));
        REQUIRE_THAT(box.size().y(), WithinAbs(60., 1e-3));
        REQUIRE_THAT(box.min.z(), WithinAbs(0., 1e-5));
        REQUIRE_THAT(box.max.z(), WithinAbs(1.2, 1e-5));
    }

    SECTION("the picture's own face comes first, and all of it is the surface the picture prints on")
    {
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE(mesh.picture_triangles > 0);
        REQUIRE(mesh.picture_triangles < mesh.its.indices.size()); // there is a frame and a backing too
        for (size_t i = 0; i < mesh.picture_triangles; ++i)
            for (int k = 0; k < 3; ++k)
                REQUIRE_THAT(mesh.its.vertices[size_t(mesh.its.indices[i][k])].z(), WithinAbs(1.2f, 1e-4f));
    }

    SECTION("a frame grows the plate by its width on every side and stands as proud as its rise")
    {
        plate.frame.width_mm = 6.f;
        plate.frame.rise_mm  = 2.f;
        plate.frame.profile  = ChromaFrame::Profile::Round;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE(is_closed(mesh.its));

        const BoundingBoxf3 box = bounding_box(mesh.its);
        REQUIRE_THAT(box.size().x(), WithinAbs(80. + 12., 1e-3));
        REQUIRE_THAT(box.size().y(), WithinAbs(60. + 12., 1e-3));
        REQUIRE_THAT(box.max.z(), WithinAbs(1.2 + 2., 1e-4));
    }

    SECTION("a flat frame is level with the picture however much rise it was given")
    {
        plate.frame.width_mm = 5.f;
        plate.frame.rise_mm  = 3.f;
        plate.frame.profile  = ChromaFrame::Profile::Flat;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE_THAT(bounding_box(mesh.its).max.z(), WithinAbs(1.2, 1e-4));
    }

    SECTION("an oval frame holds an oval picture")
    {
        plate.frame.shape    = ChromaFrame::Shape::Ellipse;
        plate.frame.width_mm = 5.f;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE(is_closed(mesh.its));
        // The picture's boundary is inscribed in its rectangle, so the face reaches the rectangle's
        // edges at the middle of each side and nothing of it is in the corners.
        float worst = 0.f;
        for (size_t i = 0; i < mesh.picture_triangles; ++i)
            for (int k = 0; k < 3; ++k) {
                const Vec3f &v = mesh.its.vertices[size_t(mesh.its.indices[i][k])];
                worst = std::max(worst, std::hypot(v.x() / 40.f, v.y() / 30.f));
            }
        REQUIRE(worst < 1.001f); // every point of the picture is inside the ellipse
    }
}

TEST_CASE("A frame's profile is a cross-section, not a shape of its own", "[ChromaStack]")
{
    ChromaFrame frame;
    frame.width_mm = 8.f;
    frame.rise_mm  = 2.f;

    SECTION("it always arrives exactly where the picture's surface is")
    {
        for (ChromaFrame::Profile p : { ChromaFrame::Profile::Bevel, ChromaFrame::Profile::Step,
                                        ChromaFrame::Profile::Round, ChromaFrame::Profile::Ogee,
                                        ChromaFrame::Profile::Mat, ChromaFrame::Profile::Cove,
                                        ChromaFrame::Profile::Step2 }) {
            frame.profile = p;
            const std::vector<Vec2f> profile = chroma_frame_profile(frame, 1.f);
            REQUIRE(profile.size() >= 2);
            REQUIRE_THAT(profile.front().x(), WithinAbs(0.f, 1e-6f));
            REQUIRE_THAT(profile.back().x(), WithinAbs(8.f, 1e-6f));
            REQUIRE_THAT(profile.back().y(), WithinAbs(1.f, 1e-6f)); // the picture's own surface
            // It never dips below the picture, and never rises past what it was given.
            for (const Vec2f &s : profile) {
                REQUIRE(s.y() >= 1.f - 1e-5f);
                REQUIRE(s.y() <= 3.f + 1e-5f);
                REQUIRE(s.x() >= -1e-5f);
                REQUIRE(s.x() <= 8.f + 1e-5f);
            }
        }
    }

    SECTION("a mat's window really is cut at 45 degrees")
    {
        // The whole point of a mount board: the run of the cut equals its drop, so the board's
        // thickness shows as an even bright line all the way round the picture.
        frame.profile = ChromaFrame::Profile::Mat;
        const std::vector<Vec2f> profile = chroma_frame_profile(frame, 1.f);
        REQUIRE(profile.size() == 3);
        const Vec2f lip = profile[1], window = profile[2];
        REQUIRE_THAT(window.x() - lip.x(), WithinAbs(lip.y() - window.y(), 1e-4f));
    }

    SECTION("a cove is hollow where a round is full")
    {
        // Halfway in, the two curves are on opposite sides of the straight ramp between the same two
        // ends. That is the only thing that distinguishes them, and it is what the eye reads.
        frame.profile = ChromaFrame::Profile::Round;
        const std::vector<Vec2f> round = chroma_frame_profile(frame, 1.f);
        frame.profile = ChromaFrame::Profile::Cove;
        const std::vector<Vec2f> cove = chroma_frame_profile(frame, 1.f);
        REQUIRE(round.size() == cove.size());
        const size_t mid    = round.size() / 2;
        const float  ramp   = 1.f + 2.f * (1.f - round[mid].x() / 8.f);
        REQUIRE(round[mid].y() > ramp);
        REQUIRE(cove[mid].y() < ramp);
    }

    SECTION("no width is no frame at all")
    {
        frame.width_mm = 0.f;
        REQUIRE(chroma_frame_profile(frame, 1.f).empty());
    }

    SECTION("nothing to rise by is a flat border whatever the profile says")
    {
        frame.profile = ChromaFrame::Profile::Ogee;
        frame.rise_mm = 0.f;
        const std::vector<Vec2f> profile = chroma_frame_profile(frame, 1.f);
        REQUIRE(profile.size() == 2);
        REQUIRE_THAT(profile.front().y(), WithinAbs(1.f, 1e-6f));
    }
}

TEST_CASE("The staged solve with its options defaulted is the solve it replaced", "[ChromaStack]")
{
    // The solve was one pass and is now five stages with an options struct beside the stack. Nothing
    // about that may change what comes out of a defaulted run: a saved project re-solves to the height
    // map it was printing, and a stack that has none of the new settings behaves as it always did.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 3;
    stack.bands           = { band(WHITE_OPAQUE, 3), band(BLACK_TRANSLUCENT, 9) };

    constexpr int              width = 24, height = 11;
    const std::vector<uint8_t> picture = fixed_picture(width, height);

    const ChromaSolve legacy    = chroma_solve(picture.data(), width, height, stack);
    const ChromaSolve defaulted = chroma_solve(picture.data(), width, height, stack, ChromaSolveOptions{});
    REQUIRE_FALSE(legacy.empty());
    REQUIRE(legacy.levels == defaulted.levels);
    REQUIRE(legacy.grey == defaulted.grey);
    REQUIRE(legacy.preview == defaulted.preview);
    REQUIRE(legacy.delta == defaulted.delta);
    REQUIRE_THAT(legacy.mean_delta_e, WithinAbs(defaulted.mean_delta_e, 1e-12));
    REQUIRE_THAT(legacy.max_delta_e, WithinAbs(defaulted.max_delta_e, 1e-12));

    SECTION("and every pixel still gets the nearest colour the stack can print, in CIE76")
    {
        // Worked out here rather than by calling the solve again: two calls of the same code agreeing
        // would not notice if the restructuring had broken the match itself.
        const std::vector<ColorRGB> palette = chroma_palette(stack);
        const int                   relief  = stack.relief_layers();
        std::vector<std::array<float, 3>> reachable;
        for (int i = 0; i <= relief; ++i) {
            const ColorRGB &c = palette[size_t(stack.base_layers + i)];
            reachable.push_back(to_lab(c.r(), c.g(), c.b()));
        }

        int missed = 0;
        for (size_t px = 0; px < legacy.levels.size(); ++px) {
            const std::array<float, 3> target = to_lab(picture[px * 3] / 255.f, picture[px * 3 + 1] / 255.f,
                                                       picture[px * 3 + 2] / 255.f);
            float                      best   = std::numeric_limits<float>::max();
            for (const std::array<float, 3> &candidate : reachable)
                best = std::min(best, cie76(target, candidate));
            const int chose = int(legacy.levels[px]) - stack.base_layers;
            // Within a thousandth of a dE of the best there is: the tolerance is for the last bit of
            // the arithmetic, not for a different choice.
            if (cie76(target, reachable[size_t(chose)]) > best + 1e-3f)
                ++missed;
        }
        REQUIRE(missed == 0);
    }

    SECTION("and a dither cell of one texel is the Floyd-Steinberg it always was")
    {
        ChromaStack dithered = stack;
        dithered.dither      = true;
        ChromaSolveOptions one_texel;
        one_texel.dither_cell_px = 1;

        const ChromaSolve a = chroma_solve(picture.data(), width, height, dithered);
        const ChromaSolve b = chroma_solve(picture.data(), width, height, dithered, one_texel);
        REQUIRE(a.levels == b.levels);
        REQUIRE(a.grey == b.grey);
        REQUIRE(a.preview == b.preview);
    }
}

TEST_CASE("The solve reports per pixel how far it was from the picture", "[ChromaStack]")
{
    // The mean and the worst answer "will this stack reproduce my picture"; they cannot answer "which
    // part of it". A photograph with one unreachable red flower in it and a photograph that is wrong
    // all over have the same mean, and only one of them is worth reprinting in other filaments.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };
    const std::vector<ColorRGB> palette = chroma_palette(stack);

    SECTION("a colour the stack prints exactly costs nothing")
    {
        const std::vector<uint8_t> pixel = flat_picture(palette[size_t(stack.base_layers + 4)], 1, 1);
        const ChromaSolve          solve = chroma_solve(pixel.data(), 1, 1, stack);
        REQUIRE(solve.delta.size() == 1);
        REQUIRE(solve.delta[0] == 0);
    }

    SECTION("a byte covers the range the number is read in")
    {
        // Halfway between two reachable colours, which is as wrong as a pixel can be while the stack
        // is still reaching it: a few dE, well inside the scale.
        const ColorRGB            &lo    = palette[size_t(stack.base_layers + 3)];
        const ColorRGB            &hi    = palette[size_t(stack.base_layers + 4)];
        const std::vector<uint8_t> pixel = flat_picture(ColorRGB(0.5f * (lo.r() + hi.r()), 0.5f * (lo.g() + hi.g()),
                                                                 0.5f * (lo.b() + hi.b())),
                                                        1, 1);
        const ChromaSolve          solve = chroma_solve(pixel.data(), 1, 1, stack);
        REQUIRE(solve.delta.size() == 1);
        REQUIRE(solve.mean_delta_e > 0.5);
        REQUIRE(solve.mean_delta_e < double(CHROMA_ERROR_IMAGE_FULL_SCALE_DE));
        // The scale is fixed rather than stretched to this picture's worst pixel: an image whose
        // meaning moved with the picture could not be compared between two stacks, which is the one
        // thing it is for.
        const long expected = std::lround(solve.mean_delta_e * 255. / double(CHROMA_ERROR_IMAGE_FULL_SCALE_DE));
        REQUIRE(long(solve.delta[0]) == expected);
    }

    SECTION("and anything past the end of that range clamps")
    {
        // Saturated red against white and black: nowhere near anything this stack can print.
        const std::vector<uint8_t> pixel{ 255, 0, 0 };
        const ChromaSolve          solve = chroma_solve(pixel.data(), 1, 1, stack);
        REQUIRE(solve.max_delta_e > double(CHROMA_ERROR_IMAGE_FULL_SCALE_DE));
        REQUIRE(solve.delta[0] == 255);
    }

    SECTION("and it comes out as a grayscale PNG of the picture's size")
    {
        const std::vector<uint8_t>       picture = fixed_picture(8, 5);
        const ChromaSolve                solve   = chroma_solve(picture.data(), 8, 5, stack);
        const std::vector<unsigned char> png     = chroma_encode_error_png(solve);
        REQUIRE_FALSE(png.empty());

        const png::ReadBuf  buf{ png.data(), png.size() };
        png::ImageGreyscale decoded;
        REQUIRE(png::is_png(buf));
        REQUIRE(png::decode_png(buf, decoded));
        REQUIRE(decoded.cols == 8);
        REQUIRE(decoded.rows == 5);
        REQUIRE(decoded.buf == solve.delta);
    }
}

TEST_CASE("A dithered solve changes no region smaller than its cell", "[ChromaStack]")
{
    // Diffusing the error one texel at a time scatters single-texel changes of height, and a column
    // narrower than the nozzle's line width is not a thin wall but nothing at all - the slicer drops
    // it, and the preview then promises a tone the print cannot make. Diffusing at cell granularity is
    // what makes the smallest region whose height changes something the printer can build.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 10) };
    stack.dither          = true;

    constexpr int              width = 32, height = 24, cell = 4;
    const std::vector<uint8_t> picture = fixed_picture(width, height);

    ChromaSolveOptions options;
    options.dither_cell_px = cell;
    const ChromaSolve solve = chroma_solve(picture.data(), width, height, stack, options);
    REQUIRE_FALSE(solve.empty());

    int ragged = 0;
    for (int cy = 0; cy * cell < height; ++cy)
        for (int cx = 0; cx * cell < width; ++cx) {
            const uint8_t first = solve.grey[size_t(cy * cell) * size_t(width) + size_t(cx * cell)];
            for (int y = cy * cell; y < std::min((cy + 1) * cell, height); ++y)
                for (int x = cx * cell; x < std::min((cx + 1) * cell, width); ++x)
                    if (solve.grey[size_t(y) * size_t(width) + size_t(x)] != first)
                        ++ragged;
        }
    REQUIRE(ragged == 0);

    // And it is still a dither rather than one flat colour: the cell is the grain of the stipple, not
    // the end of it.
    const auto [lo, hi] = std::minmax_element(solve.grey.begin(), solve.grey.end());
    REQUIRE(*lo < *hi);
}

TEST_CASE("A high quality match reaches the same kind of answer as the fast one", "[ChromaStack]")
{
    // CIE76 is here because the match used to run on the UI thread. It now runs on a worker, so the
    // better metric is affordable for the run that gets printed - and whichever metric chose it, what
    // comes out is the same three images and the same two numbers.
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };

    constexpr int              width = 16, height = 9;
    const std::vector<uint8_t> picture = fixed_picture(width, height);

    ChromaSolveOptions careful;
    careful.high_quality = true;

    const ChromaSolve fast = chroma_solve(picture.data(), width, height, stack);
    const ChromaSolve good = chroma_solve(picture.data(), width, height, stack, careful);
    REQUIRE_FALSE(good.empty());
    REQUIRE(good.width == fast.width);
    REQUIRE(good.height == fast.height);
    REQUIRE(good.levels.size() == fast.levels.size());
    REQUIRE(good.grey.size() == fast.grey.size());
    REQUIRE(good.preview.size() == fast.preview.size());
    REQUIRE(good.delta.size() == fast.delta.size());

    const auto [lo, hi] = std::minmax_element(good.levels.begin(), good.levels.end());
    REQUIRE(int(*lo) >= stack.base_layers);
    REQUIRE(int(*hi) <= stack.total_layers());

    SECTION("and a picture of the palette's own colours is exact either way")
    {
        // The two metrics may disagree about the odd pixel of a photograph, which is the point of
        // having the better one. Neither is allowed to be wrong about a colour that is in the palette.
        const std::vector<ColorRGB> palette = chroma_palette(stack);
        const int                   relief  = stack.relief_layers();
        std::vector<uint8_t>        rgb;
        for (int i = 0; i <= relief; ++i) {
            const ColorRGB &c = palette[size_t(stack.base_layers + i)];
            rgb.push_back(uint8_t(std::lround(c.r() * 255.f)));
            rgb.push_back(uint8_t(std::lround(c.g() * 255.f)));
            rgb.push_back(uint8_t(std::lround(c.b() * 255.f)));
        }

        const ChromaSolve a = chroma_solve(rgb.data(), relief + 1, 1, stack);
        const ChromaSolve b = chroma_solve(rgb.data(), relief + 1, 1, stack, careful);
        REQUIRE(a.levels == b.levels);
        REQUIRE(b.max_delta_e < 1.);
    }
}

TEST_CASE("A retouch edit moves the columns it covers and leaves the rest alone", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 3), band(BLACK_TRANSLUCENT, 9) };
    const int                   relief  = stack.relief_layers();
    const std::vector<ColorRGB> palette = chroma_palette(stack);

    // One colour the stack prints exactly, so every column starts in the same place and an edit is the
    // only thing that can move one.
    constexpr int              width = 16, height = 8;
    const std::vector<uint8_t> picture = flat_picture(palette[size_t(stack.base_layers + 7)], width, height);

    const ChromaSolve plain = chroma_solve(picture.data(), width, height, stack);
    REQUIRE_FALSE(plain.empty());
    const int started_at = int(plain.levels.front()) - stack.base_layers;
    REQUIRE(started_at > 0);
    REQUIRE(started_at < relief);

    const std::vector<uint8_t> mask = left_half_mask(width, height, width / 2);
    ChromaRetouchEdit          edit;
    edit.region = chroma_mask_encode(mask.data(), width, height);
    REQUIRE_FALSE(edit.region.empty());

    const auto level_at = [&](const ChromaSolve &solve, int x, int y) {
        return int(solve.levels[size_t(y) * size_t(width) + size_t(x)]) - stack.base_layers;
    };

    SECTION("a delta moves it by that many layers")
    {
        edit.target         = ChromaRetouchEdit::Target::Delta;
        edit.value          = 2;
        stack.retouch.edits = { edit };

        const ChromaSolve solve = chroma_solve(picture.data(), width, height, stack);
        REQUIRE(level_at(solve, 0, 0) == started_at + 2);
        REQUIRE(level_at(solve, width - 1, 0) == started_at);
        // An edit is the user overruling the match, so the error image has to report what will be
        // printed rather than what the match would have chosen.
        REQUIRE(solve.delta[0] > plain.delta[0]);
    }

    SECTION("a delta that would leave the stack stops at the top of it")
    {
        edit.target         = ChromaRetouchEdit::Target::Delta;
        edit.value          = 500;
        stack.retouch.edits = { edit };
        REQUIRE(level_at(chroma_solve(picture.data(), width, height, stack), 0, 0) == relief);
    }

    SECTION("and one that would leave it the other way stops at the base")
    {
        edit.target         = ChromaRetouchEdit::Target::Delta;
        edit.value          = -500;
        stack.retouch.edits = { edit };
        REQUIRE(level_at(chroma_solve(picture.data(), width, height, stack), 0, 0) == 0);
    }

    SECTION("a band target puts the column at that band's top")
    {
        REQUIRE(chroma_band_top(stack, 0) == 3);
        REQUIRE(chroma_band_top(stack, 1) == relief);

        edit.target         = ChromaRetouchEdit::Target::Band;
        edit.value          = 0;
        stack.retouch.edits = { edit };

        const ChromaSolve solve = chroma_solve(picture.data(), width, height, stack);
        REQUIRE(level_at(solve, 0, 0) == 3);
        REQUIRE(level_at(solve, width - 1, 0) == started_at);
    }

    SECTION("a band target of minus one is the bare base")
    {
        edit.target         = ChromaRetouchEdit::Target::Band;
        edit.value          = -1;
        stack.retouch.edits = { edit };
        REQUIRE(level_at(chroma_solve(picture.data(), width, height, stack), 0, 0) == 0);
    }

    SECTION("an edit that is turned off changes nothing at all")
    {
        edit.target         = ChromaRetouchEdit::Target::Band;
        edit.value          = -1;
        edit.enabled        = false;
        stack.retouch.edits = { edit };

        const ChromaSolve solve = chroma_solve(picture.data(), width, height, stack);
        REQUIRE(solve.levels == plain.levels);
        REQUIRE(solve.grey == plain.grey);
        REQUIRE(solve.delta == plain.delta);
    }

    SECTION("the later of two edits over the same pixels wins")
    {
        ChromaRetouchEdit first  = edit;
        first.target             = ChromaRetouchEdit::Target::Band;
        first.value              = 0;
        ChromaRetouchEdit second = edit;
        second.target            = ChromaRetouchEdit::Target::Band;
        second.value             = 1;

        stack.retouch.edits = { first, second };
        REQUIRE(level_at(chroma_solve(picture.data(), width, height, stack), 0, 0) == chroma_band_top(stack, 1));

        stack.retouch.edits = { second, first };
        REQUIRE(level_at(chroma_solve(picture.data(), width, height, stack), 0, 0) == chroma_band_top(stack, 0));
    }

    SECTION("a mask painted on the picture applies to a solve run on a coarser grid")
    {
        // The panel solves a preview at a fraction of the picture's resolution and the final pass at
        // the picture's own, so one mask has to serve both; it is sampled rather than rescaled.
        const std::vector<uint8_t> painted = left_half_mask(2 * width, 2 * height, width);
        edit.region                        = chroma_mask_encode(painted.data(), 2 * width, 2 * height);
        edit.target                        = ChromaRetouchEdit::Target::Delta;
        edit.value                         = 3;
        stack.retouch.edits                = { edit };

        const ChromaSolve solve = chroma_solve(picture.data(), width, height, stack);
        REQUIRE(level_at(solve, 0, 0) == started_at + 3);
        REQUIRE(level_at(solve, width - 1, height - 1) == started_at);
    }
}

TEST_CASE("A painted region survives being run-length encoded", "[ChromaStack]")
{
    constexpr int        w = 9, h = 7;
    std::vector<uint8_t> painted(size_t(w) * size_t(h), 0);
    // A diagonal stroke rather than a rectangle: a stroke is the shape this representation was chosen
    // for, and the shape a list of rectangles cannot say.
    for (int i = 0; i < h; ++i)
        for (int d = 0; d < 2; ++d)
            painted[size_t(i) * size_t(w) + size_t(std::min(w - 1, i + d))] = 1;

    const ChromaMask encoded = chroma_mask_encode(painted.data(), w, h);
    REQUIRE(encoded.width == w);
    REQUIRE(encoded.height == h);
    REQUIRE_FALSE(encoded.empty());
    REQUIRE(chroma_mask_decode(encoded) == painted);
    // Shorter than the bitmap it came from, which is the whole reason a project file holds runs.
    REQUIRE(encoded.runs.size() < size_t(w) * size_t(h));

    SECTION("a region that starts at the first pixel still starts with an outside run")
    {
        std::vector<uint8_t> everything(size_t(w) * size_t(h), 1);
        const ChromaMask     full = chroma_mask_encode(everything.data(), w, h);
        REQUIRE(full.runs.size() == 2);
        REQUIRE(full.runs[0] == 0);
        REQUIRE(full.runs[1] == uint32_t(w * h));
        REQUIRE(chroma_mask_decode(full) == everything);
    }

    SECTION("nothing painted is no region at all")
    {
        const std::vector<uint8_t> nothing(size_t(w) * size_t(h), 0);
        REQUIRE(chroma_mask_encode(nothing.data(), w, h).empty());
    }
}

TEST_CASE("A back-lit stack darkens as the column gets thicker", "[ChromaStack]")
{
    // Front lit, a column's colour is what it reflects and the base behind it is opaque. Back lit, the
    // whole column is a filter over a lamp - the base included, because the base is the thinnest part
    // of such a plate and the brightest part of the picture - so every layer can only take light away.
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 1;
    stack.bands           = { band(filament(0, ColorRGB(1.f, 0.9f, 0.8f), 8.f), 6),
                              band(filament(1, ColorRGB(0.3f, 0.5f, 1.f), 8.f), 6) };
    stack.lighting        = ChromaLighting::BackLit;

    const std::vector<ColorRGB> palette = chroma_palette(stack);
    REQUIRE(palette.size() == size_t(stack.total_layers() + 1));
    // Entry 0 is the bare lamp - no filament at all - which only a plate with no base ever reaches.
    REQUIRE_THAT(luminance(palette.front()), WithinAbs(1.f, 1e-3f));

    int brightened = 0;
    for (size_t i = 1; i < palette.size(); ++i)
        if (luminance(palette[i]) >= luminance(palette[i - 1]))
            ++brightened;
    REQUIRE(brightened == 0);

    SECTION("the same stack lit from the front is the print it always was")
    {
        ChromaStack front = stack;
        front.lighting    = ChromaLighting::FrontLit;

        const std::vector<ColorRGB> lit = chroma_palette(front);
        REQUIRE(lit.size() == palette.size());
        // A front-lit column one layer tall is exactly the bottom filament's own colour.
        REQUIRE_THAT(luminance(lit[1]), WithinAbs(luminance(ColorRGB(1.f, 0.9f, 0.8f)), 1e-3f));
        // And it is not the back-lit picture: a thick column reflects more light and transmits less,
        // which is the inversion a caller has to know about before mapping tone to height.
        REQUIRE(luminance(lit.back()) > luminance(palette.back()));
    }

    SECTION("lit from both sides, the palette is the two palettes mixed")
    {
        ChromaStack both = stack;
        both.lighting    = ChromaLighting::BothLit;

        ChromaStack front = stack;
        front.lighting    = ChromaLighting::FrontLit;
        const std::vector<ColorRGB> reflected = chroma_palette(front);

        // All the way to one end is the back-lit print exactly, and all the way to the other is the
        // front-lit one: the mode has to be continuous with the two it sits between, or a user sliding
        // it would see the picture jump.
        both.lighting_balance = 1.f;
        const std::vector<ColorRGB> all_lamp = chroma_palette(both);
        both.lighting_balance = 0.f;
        const std::vector<ColorRGB> no_lamp = chroma_palette(both);
        REQUIRE(all_lamp.size() == palette.size());
        REQUIRE(no_lamp.size() == palette.size());
        for (size_t i = 0; i < palette.size(); ++i) {
            REQUIRE_THAT(luminance(all_lamp[i]), WithinAbs(luminance(palette[i]), 1e-3f));
            REQUIRE_THAT(luminance(no_lamp[i]), WithinAbs(luminance(reflected[i]), 1e-3f));
        }

        // And in between it is brighter than the darker of the two everywhere, which is the whole
        // point: room light landing on a lithophane lifts its shadows and is why a stack chosen
        // against the lamp alone washes out.
        both.lighting_balance            = 0.5f;
        const std::vector<ColorRGB> half = chroma_palette(both);
        int                         lifted = 0;
        for (size_t i = 1; i < half.size(); ++i)
            if (luminance(half[i]) > std::min(luminance(palette[i]), luminance(reflected[i])) + 1e-4f)
                ++lifted;
        REQUIRE(lifted == int(half.size()) - 1);
    }

    SECTION("the lamp is a light rather than another filament")
    {
        ChromaStack warm = stack;
        warm.led_color   = ColorRGB(1.f, 0.4f, 0.4f);
        const std::vector<ColorRGB> red = chroma_palette(warm);
        REQUIRE(red.size() == palette.size());
        REQUIRE(red[3].r() > red[3].b());

        ChromaStack bright   = stack;
        bright.led_intensity = 4.f;
        const std::vector<ColorRGB> blown = chroma_palette(bright);
        REQUIRE(blown.size() == palette.size());
        // A brighter lamp never makes a column darker, and the thinnest columns clamp: a print held up
        // to too strong a light loses its highlights, and the palette says so rather than hiding it.
        int dimmed = 0;
        for (size_t i = 0; i < blown.size(); ++i)
            if (luminance(blown[i]) < luminance(palette[i]) - 1e-4f)
                ++dimmed;
        REQUIRE(dimmed == 0);
        REQUIRE_THAT(luminance(blown.front()), WithinAbs(1.f, 1e-3f));
    }

    SECTION("and the solve matches against it unchanged")
    {
        // The palette is the only thing the solve reads, so a back-lit stack is solved by the same
        // code and has to reach a picture made of its own colours.
        const int            relief = stack.relief_layers();
        std::vector<uint8_t> rgb;
        for (int i = 0; i <= relief; ++i) {
            const ColorRGB &c = palette[size_t(stack.base_layers + i)];
            rgb.push_back(uint8_t(std::lround(c.r() * 255.f)));
            rgb.push_back(uint8_t(std::lround(c.g() * 255.f)));
            rgb.push_back(uint8_t(std::lround(c.b() * 255.f)));
        }

        const ChromaSolve solve = chroma_solve(rgb.data(), relief + 1, 1, stack);
        REQUIRE_FALSE(solve.empty());
        REQUIRE(solve.levels.size() == size_t(relief + 1));
        REQUIRE(solve.max_delta_e < 2.);
        REQUIRE(solve.grey.front() == 0);
    }
}

TEST_CASE("A stack's lighting and retouch survive the project file", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 7;
    stack.base_auto       = false;
    stack.dither          = true;
    stack.applied_ranges  = 42ull;
    stack.bands           = { band(filament(1, ColorRGB(0.8f, 0.2f, 0.1f), 6.f), 5), band(BLACK_TRANSLUCENT, 7) };
    stack.bands.front().filament.name = "Red PLA";
    stack.lighting                    = ChromaLighting::BackLit;
    stack.led_color                   = ColorRGB(1.f, 0.85f, 0.6f);
    stack.led_intensity               = 1.5f;

    const std::vector<uint8_t> painted = left_half_mask(8, 4, 3);
    ChromaRetouchEdit          flattened;
    flattened.region  = chroma_mask_encode(painted.data(), 8, 4);
    flattened.target  = ChromaRetouchEdit::Target::Band;
    flattened.value   = 1;
    flattened.enabled = false;
    ChromaRetouchEdit lifted;
    lifted.region       = chroma_mask_encode(painted.data(), 8, 4);
    lifted.target       = ChromaRetouchEdit::Target::Delta;
    lifted.value        = -2;
    stack.retouch.edits = { flattened, lifted };
    stack.lighting_balance = 0.25f;

    nlohmann::json json;
    chroma_stack_to_json(json, stack);
    ChromaStack back;
    chroma_stack_from_json(json, back);

    REQUIRE(back.bands.size() == 2);
    REQUIRE(back.base_layers == 7);
    REQUIRE_FALSE(back.base_auto);
    REQUIRE(back.dither);
    REQUIRE(back.applied_ranges == 42ull);
    REQUIRE(back.bands[0].filament.name == "Red PLA");
    REQUIRE(back.lighting == ChromaLighting::BackLit);
    // Colours come back as the bytes they are shown and printed as.
    REQUIRE_THAT(back.led_color.g(), WithinAbs(0.85f, 2.f / 255.f));
    REQUIRE_THAT(back.led_color.b(), WithinAbs(0.6f, 2.f / 255.f));
    REQUIRE_THAT(back.led_intensity, WithinRel(1.5f, 1e-4f));
    REQUIRE_THAT(back.lighting_balance, WithinRel(0.25f, 1e-4f));
    REQUIRE(back.retouch.edits.size() == 2);
    REQUIRE(back.retouch.edits[0].target == ChromaRetouchEdit::Target::Band);
    REQUIRE(back.retouch.edits[0].value == 1);
    REQUIRE_FALSE(back.retouch.edits[0].enabled);
    REQUIRE(back.retouch.edits[1].target == ChromaRetouchEdit::Target::Delta);
    REQUIRE(back.retouch.edits[1].value == -2);
    REQUIRE(back.retouch.edits[1].enabled);
    REQUIRE(chroma_mask_decode(back.retouch.edits[1].region) == chroma_mask_decode(lifted.region));

    SECTION("and a project written before any of it existed reads as the print it was")
    {
        // Only the keys the .3mf has always had. Absent means front lit and no edits, which is what
        // makes a saved project solve to the height map it was printing.
        nlohmann::json old;
        old["layer_height_mm"] = 0.1f;
        old["base_layers"]     = 4;
        old["bands"]           = nlohmann::json::array({ nlohmann::json{
            { "filament", 0 }, { "td_mm", 8.f }, { "color", nlohmann::json::array({ 0, 0, 0 }) }, { "layers", 6 } } });

        ChromaStack plain;
        chroma_stack_from_json(old, plain);
        REQUIRE(plain.bands.size() == 1);
        REQUIRE(plain.bands[0].layers == 6);
        REQUIRE(plain.lighting == ChromaLighting::FrontLit);
        REQUIRE(plain.retouch.empty());
        REQUIRE_THAT(plain.led_intensity, WithinRel(1.f, 1e-6f));
        REQUIRE_THAT(plain.lighting_balance, WithinRel(0.5f, 1e-6f));
    }

    SECTION("and it survives an undo step too")
    {
        // The same fields through cereal, which is what an undo snapshot and the clipboard are made
        // of. Appended rather than inserted, so an older stream still reads.
        std::stringstream stream;
        {
            cereal::BinaryOutputArchive out(stream);
            out(stack);
        }
        ChromaStack restored;
        {
            cereal::BinaryInputArchive in(stream);
            in(restored);
        }

        REQUIRE(restored.bands.size() == 2);
        REQUIRE(restored.lighting == ChromaLighting::BackLit);
        REQUIRE_THAT(restored.led_intensity, WithinRel(1.5f, 1e-6f));
        REQUIRE_THAT(restored.led_color.b(), WithinRel(0.6f, 1e-6f));
        REQUIRE_THAT(restored.lighting_balance, WithinRel(0.25f, 1e-6f));
        REQUIRE(restored.retouch.edits.size() == 2);
        REQUIRE(restored.retouch.edits[0].target == ChromaRetouchEdit::Target::Band);
        REQUIRE_FALSE(restored.retouch.edits[0].enabled);
        REQUIRE(restored.retouch.edits[1].region.runs == lifted.region.runs);
    }
}

TEST_CASE("A vivid corner of a picture can earn a band its size would not", "[ChromaStack]")
{
    // Fifteen pixels of near-neutral grey and one of saturated orange. Weighted by coverage alone the
    // orange is one sixteenth of the question, so the picker spends its filaments on the grey and the
    // one thing the picture is about prints as a smudge.
    std::vector<uint8_t> rgb;
    for (int i = 0; i < 15; ++i) {
        rgb.push_back(130);
        rgb.push_back(128);
        rgb.push_back(126);
    }
    rgb.push_back(240);
    rgb.push_back(110);
    rgb.push_back(20);

    const ChromaHistogram plain = chroma_histogram(rgb.data(), 16, 1);
    const ChromaHistogram vivid = chroma_histogram(rgb.data(), 16, 1, 192, CHROMA_VIVIDNESS_RECOMMENDED);
    REQUIRE(plain.bins.size() == 2);
    REQUIRE(vivid.bins.size() == 2);

    // The grey is still what most of the picture is, in both: this is a thumb on the scale, not a
    // different scale.
    REQUIRE(plain.bins[0].first.r() < plain.bins[1].first.r());
    REQUIRE(vivid.bins[0].first.r() < vivid.bins[1].first.r());
    REQUIRE(vivid.bins[1].second > plain.bins[1].second);
    REQUIRE(vivid.bins[0].second > vivid.bins[1].second);

    SECTION("the default is still coverage alone, which is what it always was")
    {
        REQUIRE_THAT(plain.bins[0].second, WithinAbs(15.f / 16.f, 1e-5f));
        REQUIRE_THAT(plain.bins[1].second, WithinAbs(1.f / 16.f, 1e-5f));
        REQUIRE_THAT(CHROMA_VIVIDNESS_DEFAULT, WithinAbs(0.f, 1e-6f));
    }

    SECTION("a picture of one colour is not moved by it at all")
    {
        // Every bin sits on the picture's own average, so every factor is 1 and the weighting is
        // coverage - which is the case that would otherwise divide by nothing.
        const std::vector<uint8_t> flat = flat_picture(ColorRGB(0.4f, 0.6f, 0.2f), 8, 4);
        const ChromaHistogram      one  = chroma_histogram(flat.data(), 8, 4, 192, CHROMA_VIVIDNESS_RECOMMENDED);
        REQUIRE(one.bins.size() == 1);
        REQUIRE_THAT(one.bins[0].second, WithinAbs(1.f, 1e-6f));
    }
}

TEST_CASE("An importance region says which part of the picture the stack is for", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.08f;
    stack.base_layers     = 2;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 8) };
    const std::vector<ColorRGB> palette = chroma_palette(stack);

    // Half a picture the stack prints exactly, half a saturated red it cannot reach at all.
    constexpr int        width = 8, height = 4;
    std::vector<uint8_t> picture;
    const ColorRGB      &reachable = palette[size_t(stack.base_layers + 5)];
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const bool left = x < width / 2;
            picture.push_back(left ? uint8_t(std::lround(reachable.r() * 255.f)) : 255);
            picture.push_back(left ? uint8_t(std::lround(reachable.g() * 255.f)) : 0);
            picture.push_back(left ? uint8_t(std::lround(reachable.b() * 255.f)) : 0);
        }

    const ChromaSolve plain = chroma_solve(picture.data(), width, height, stack);
    REQUIRE_FALSE(plain.empty());
    REQUIRE(plain.mean_delta_e > 1.);

    const std::vector<uint8_t> left  = left_half_mask(width, height, width / 2);
    std::vector<uint8_t>       right(size_t(width) * size_t(height), 0);
    for (size_t i = 0; i < right.size(); ++i)
        right[i] = left[i] == 0 ? 1 : 0;

    SECTION("marking the part it gets right brings the number down")
    {
        stack.importance.region = chroma_mask_encode(left.data(), width, height);
        stack.importance.weight = 4.f;
        const ChromaSolve solve = chroma_solve(picture.data(), width, height, stack);
        REQUIRE(solve.mean_delta_e < plain.mean_delta_e);
        // The worst pixel is still the worst pixel there is: an importance region says what to
        // optimise for, not what to stop looking at.
        REQUIRE_THAT(solve.max_delta_e, WithinRel(plain.max_delta_e, 1e-9));
        // And nothing about which colour a pixel got has changed: a weight cannot move a match.
        REQUIRE(solve.levels == plain.levels);
        REQUIRE(solve.delta == plain.delta);
    }

    SECTION("marking the part it gets wrong takes it up")
    {
        stack.importance.region = chroma_mask_encode(right.data(), width, height);
        stack.importance.weight = 4.f;
        REQUIRE(chroma_solve(picture.data(), width, height, stack).mean_delta_e > plain.mean_delta_e);
    }

    SECTION("a weight of one is no preference at all")
    {
        stack.importance.region = chroma_mask_encode(right.data(), width, height);
        stack.importance.weight = 1.f;
        REQUIRE(stack.importance.empty());
        REQUIRE_THAT(chroma_solve(picture.data(), width, height, stack).mean_delta_e,
                     WithinRel(plain.mean_delta_e, 1e-9));
    }

    SECTION("and the histogram counts the marked pixels for more")
    {
        ChromaImportance importance;
        importance.region = chroma_mask_encode(right.data(), width, height);
        importance.weight = 4.f;

        const ChromaHistogram flat   = chroma_histogram(picture.data(), width, height);
        const ChromaHistogram marked = chroma_histogram(picture.data(), width, height,
                                                        192, CHROMA_VIVIDNESS_DEFAULT, importance);
        REQUIRE(flat.bins.size() == 2);
        REQUIRE(marked.bins.size() == 2);

        // The red half is what was marked, so its share of the question goes from a half to four
        // fifths - and the colours themselves are untouched.
        const size_t red_flat   = flat.bins[0].first.r() > flat.bins[1].first.r() ? 0 : 1;
        const size_t red_marked = marked.bins[0].first.r() > marked.bins[1].first.r() ? 0 : 1;
        REQUIRE_THAT(flat.bins[red_flat].second, WithinAbs(0.5f, 1e-5f));
        REQUIRE_THAT(marked.bins[red_marked].second, WithinAbs(0.8f, 1e-5f));
    }
}

TEST_CASE("The filament search answers the same thing every time it is asked", "[ChromaStack]")
{
    // The search runs over its candidates in parallel. A search that came back with a different stack
    // from one run to the next would be worse than a slow one: a user would put a slider back where it
    // was and get a different picture.
    std::vector<uint8_t> rgb;
    const uint8_t        pixels[][3] = { { 220, 40, 30 }, { 250, 200, 40 }, { 30, 70, 160 },
                                         { 240, 240, 235 }, { 20, 110, 60 }, { 90, 60, 140 } };
    for (const auto &p : pixels)
        for (int i = 0; i < 4; ++i) {
            rgb.push_back(p[0]);
            rgb.push_back(p[1]);
            rgb.push_back(p[2]);
        }
    const ChromaHistogram hist = chroma_histogram(rgb.data(), 24, 1);

    const std::vector<ChromaFilament> available{
        WHITE_OPAQUE, BLACK_OPAQUE,
        filament(5, ColorRGB(0.86f, 0.16f, 0.12f), 4.f), filament(6, ColorRGB(0.98f, 0.78f, 0.16f), 5.f),
        filament(7, ColorRGB(0.12f, 0.27f, 0.63f), 4.f), filament(8, ColorRGB(0.16f, 0.59f, 0.24f), 4.f)
    };

    const ChromaStack first = chroma_auto_stack(hist, available, 4, 0.08f, 4, 24);
    REQUIRE(first.bands.size() == 4);
    for (int run = 0; run < 4; ++run) {
        const ChromaStack again = chroma_auto_stack(hist, available, 4, 0.08f, 4, 24);
        REQUIRE(again.bands.size() == first.bands.size());
        int differed = 0;
        for (size_t i = 0; i < again.bands.size(); ++i) {
            if (again.bands[i].filament.filament != first.bands[i].filament.filament)
                ++differed;
            if (again.bands[i].layers != first.bands[i].layers)
                ++differed;
        }
        REQUIRE(differed == 0);
    }

    SECTION("and so does the colour suggestion's hill climb")
    {
        const std::vector<ChromaSuggestion> once  = chroma_suggest_colors(hist, 3, available, 0.08f, 24);
        const std::vector<ChromaSuggestion> twice = chroma_suggest_colors(hist, 3, available, 0.08f, 24);
        REQUIRE(once.size() == twice.size());
        REQUIRE_FALSE(once.empty());
        for (size_t i = 0; i < once.size(); ++i) {
            REQUIRE_THAT(twice[i].color.r(), WithinAbs(once[i].color.r(), 1e-6f));
            REQUIRE_THAT(twice[i].color.g(), WithinAbs(once[i].color.g(), 1e-6f));
            REQUIRE_THAT(twice[i].color.b(), WithinAbs(once[i].color.b(), 1e-6f));
        }
    }
}

TEST_CASE("A plate printed face down prints the picture against the build plate", "[ChromaStack]")
{
    ChromaPlate plate;
    plate.generated   = true;
    plate.width_mm    = 60.f;
    plate.height_mm   = 40.f;
    plate.base_mm     = 2.4f; // the whole stack's thickness, since the relief is cut up into it
    plate.cell_mm     = 4.f;
    plate.orientation = ChromaOrientation::FaceDown;

    SECTION("the picture's face is the plate's bottom, and it is still the first triangles")
    {
        plate.frame.width_mm = 0.f;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE_FALSE(mesh.empty());
        REQUIRE(mesh.picture_triangles > 0);
        REQUIRE(is_closed(mesh.its));

        int above = 0;
        for (size_t i = 0; i < mesh.picture_triangles; ++i)
            for (int k = 0; k < 3; ++k)
                if (std::abs(mesh.its.vertices[size_t(mesh.its.indices[i][k])].z()) > 1e-4f)
                    ++above;
        REQUIRE(above == 0);

        const BoundingBoxf3 box = bounding_box(mesh.its);
        REQUIRE_THAT(box.min.z(), WithinAbs(0., 1e-5));
        REQUIRE_THAT(box.max.z(), WithinAbs(2.4, 1e-4));
        REQUIRE_THAT(box.size().x(), WithinAbs(60., 1e-3));
    }

    SECTION("the whole bottom is one flat plane and the frame shapes the back")
    {
        plate.frame.width_mm = 5.f;
        plate.frame.rise_mm  = 1.5f;
        plate.frame.profile  = ChromaFrame::Profile::Round;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE(is_closed(mesh.its));

        const BoundingBoxf3 box = bounding_box(mesh.its);
        REQUIRE_THAT(box.size().x(), WithinAbs(60. + 10., 1e-3));
        // Nothing below the picture, because there is nothing for it to stand on: the frame's rise is
        // on the back.
        REQUIRE_THAT(box.min.z(), WithinAbs(0., 1e-5));
        REQUIRE_THAT(box.max.z(), WithinAbs(2.4 + 1.5, 1e-4));
    }

    SECTION("and face up is the plate it has always been")
    {
        plate.orientation          = ChromaOrientation::FaceUp;
        plate.frame.width_mm       = 5.f;
        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE(is_closed(mesh.its));
        int below = 0;
        for (size_t i = 0; i < mesh.picture_triangles; ++i)
            for (int k = 0; k < 3; ++k)
                if (std::abs(mesh.its.vertices[size_t(mesh.its.indices[i][k])].z() - 2.4f) > 1e-4f)
                    ++below;
        REQUIRE(below == 0);
    }
}

TEST_CASE("A face-down stack swaps its filaments in the order they print", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 4;
    stack.bands           = { band(WHITE_OPAQUE, 2), band(BLACK_TRANSLUCENT, 3) };

    // Face up: the base prints in the bottom band's filament and the bands follow it.
    const std::vector<ChromaRange> up = chroma_ranges(stack, 0.);
    REQUIRE(up.size() == 2);
    REQUIRE(up[0].filament == WHITE_OPAQUE.filament);
    REQUIRE_THAT(up[0].z_max, WithinAbs(0.6, 1e-9));
    REQUIRE(up[1].filament == BLACK_TRANSLUCENT.filament);
    REQUIRE_THAT(up[1].z_max, WithinAbs(0.9, 1e-9));

    // Face down the light enters through the first layer, so the band it meets first has to print
    // first: the bands come out top-down and the base is printed last, on top of them.
    stack.plate.orientation          = ChromaOrientation::FaceDown;
    const std::vector<ChromaRange> down = chroma_ranges(stack, 0.);
    REQUIRE(down.size() == 2);
    REQUIRE(down[0].filament == BLACK_TRANSLUCENT.filament);
    REQUIRE_THAT(down[0].z_min, WithinAbs(0., 1e-9));
    REQUIRE_THAT(down[0].z_max, WithinAbs(0.3, 1e-9));
    // The base prints in the bottom band's filament, so it is the same range as that band rather than
    // a swap of its own - the face-down version of what the base does face up.
    REQUIRE(down[1].filament == WHITE_OPAQUE.filament);
    REQUIRE_THAT(down[1].z_min, WithinAbs(0.3, 1e-9));
    REQUIRE_THAT(down[1].z_max, WithinAbs(0.9, 1e-9));

    // Both ways up it is the same print height, which is what the plate has to be thick enough for.
    REQUIRE_THAT(stack.total_mm(), WithinAbs(0.9f, 1e-6f));

    SECTION("and the height map it is read with is the other way up too")
    {
        ChromaSolve solve;
        solve.width  = 2;
        solve.height = 1;
        solve.levels = { 4, 9 };
        solve.grey   = { 0, 255 };
        chroma_invert_relief(solve);
        REQUIRE(solve.grey[0] == 255);
        REQUIRE(solve.grey[1] == 0);
        // Only the height map: the colours are the same picture whichever way up it prints.
        REQUIRE(solve.levels[0] == 4);
    }
}

TEST_CASE("A frame can carry its own cross-section", "[ChromaStack]")
{
    // The gallery's frames are real mouldings, and a real moulding is not one of eight shapes. A
    // definition hands its section over as a polyline and it is swept as it stands.
    ChromaFrame frame;
    frame.width_mm       = 8.f;
    frame.profile_points = { Vec2f(0.f, 3.f), Vec2f(4.f, 3.f), Vec2f(4.f, 1.f), Vec2f(8.f, 0.f) };

    const std::vector<Vec2f> profile = chroma_frame_profile(frame, 1.f);
    REQUIRE(profile.size() == 4);
    // Heights are above the picture's surface in the definition and above z = 0 coming out, which is
    // what lets a definition be written without knowing how thick the plate's backing is.
    REQUIRE_THAT(profile[0].x(), WithinAbs(0.f, 1e-6f));
    REQUIRE_THAT(profile[0].y(), WithinAbs(4.f, 1e-6f));
    REQUIRE_THAT(profile[1].y(), WithinAbs(4.f, 1e-6f));
    // Two samples at the same inset are a vertical face, and that survives.
    REQUIRE_THAT(profile[2].x(), WithinAbs(4.f, 1e-6f));
    REQUIRE_THAT(profile[2].y(), WithinAbs(2.f, 1e-6f));
    // And it arrives exactly where the picture is.
    REQUIRE_THAT(profile.back().x(), WithinAbs(8.f, 1e-6f));
    REQUIRE_THAT(profile.back().y(), WithinAbs(1.f, 1e-6f));

    SECTION("a sample that goes backwards is pulled forward onto the one before it")
    {
        // An overhang is a thing the sweep cannot express - every sample is one ring, and the rings
        // have to march inwards - so it becomes the vertical face it can.
        frame.profile_points = { Vec2f(0.f, 2.f), Vec2f(6.f, 1.f), Vec2f(3.f, 0.5f), Vec2f(8.f, 0.f) };
        const std::vector<Vec2f> pulled = chroma_frame_profile(frame, 1.f);
        REQUIRE(pulled.size() == 4);
        REQUIRE_THAT(pulled[1].x(), WithinAbs(6.f, 1e-6f));
        REQUIRE_THAT(pulled[2].x(), WithinAbs(6.f, 1e-6f));
    }

    SECTION("a sample outside the frame's own band is brought back into it")
    {
        frame.profile_points = { Vec2f(-2.f, 2.f), Vec2f(40.f, 1.f), Vec2f(8.f, 0.f) };
        const std::vector<Vec2f> clamped = chroma_frame_profile(frame, 1.f);
        REQUIRE(clamped.size() == 3);
        for (const Vec2f &s : clamped) {
            REQUIRE(s.x() >= -1e-6f);
            REQUIRE(s.x() <= 8.f + 1e-6f);
            REQUIRE(s.y() >= 1.f - 1e-6f);
        }
    }

    SECTION("anything less than a section falls back to the enum, exactly as before")
    {
        ChromaFrame enum_only = frame;
        enum_only.profile_points.clear();
        enum_only.profile = ChromaFrame::Profile::Step;
        enum_only.rise_mm = 2.f;

        ChromaFrame one_sample     = enum_only;
        one_sample.profile_points = { Vec2f(0.f, 1.f) };

        const std::vector<Vec2f> a = chroma_frame_profile(enum_only, 1.f);
        const std::vector<Vec2f> b = chroma_frame_profile(one_sample, 1.f);
        REQUIRE(a.size() == b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            REQUIRE_THAT(b[i].x(), WithinAbs(a[i].x(), 1e-6f));
            REQUIRE_THAT(b[i].y(), WithinAbs(a[i].y(), 1e-6f));
        }
    }

    SECTION("and the plate is built from it")
    {
        ChromaPlate plate;
        plate.generated = true;
        plate.width_mm  = 50.f;
        plate.height_mm = 50.f;
        plate.base_mm   = 1.f;
        plate.cell_mm   = 5.f;
        plate.frame     = frame;

        const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
        REQUIRE(is_closed(mesh.its));
        REQUIRE(mesh.picture_triangles > 0);
        const BoundingBoxf3 box = bounding_box(mesh.its);
        REQUIRE_THAT(box.size().x(), WithinAbs(50. + 16., 1e-3));
        REQUIRE_THAT(box.max.z(), WithinAbs(1. + 3., 1e-4)); // the section's own rise, not rise_mm
    }
}

TEST_CASE("A rounded plate keeps its frame when the corner is tighter than the frame is wide",
          "[ChromaStack]")
{
    // The rings of the sweep are the same loop at different distances out, so they have to have the
    // same points. A corner radius smaller than the frame's width leaves the inner rings with no
    // corner to turn, and answering that with a plain rectangle changed the point count, which the
    // sweep reads as a torn shape and refuses: the frame disappeared.
    ChromaPlate plate;
    plate.generated      = true;
    plate.width_mm       = 60.f;
    plate.height_mm      = 40.f;
    plate.base_mm        = 1.f;
    plate.cell_mm        = 5.f;
    plate.frame.shape    = ChromaFrame::Shape::Rounded;
    plate.frame.corner_mm = 2.f;
    plate.frame.width_mm  = 6.f;
    plate.frame.rise_mm   = 2.f;
    plate.frame.profile   = ChromaFrame::Profile::Round;

    const ChromaPlateMesh mesh = chroma_plate_mesh(plate);
    REQUIRE(is_closed(mesh.its));
    REQUIRE(mesh.picture_triangles > 0);
    REQUIRE(mesh.picture_triangles < mesh.its.indices.size());

    const BoundingBoxf3 box = bounding_box(mesh.its);
    REQUIRE_THAT(box.size().x(), WithinAbs(60. + 12., 1e-3));
    REQUIRE_THAT(box.max.z(), WithinAbs(1. + 2., 1e-4)); // the frame is there, standing as proud as its rise
}

TEST_CASE("A plate and the frame it is wearing survive the project file", "[ChromaStack]")
{
    ChromaStack stack;
    stack.layer_height_mm = 0.1f;
    stack.base_layers     = 3;
    stack.bands           = { band(WHITE_OPAQUE, 4), band(BLACK_TRANSLUCENT, 6) };

    stack.plate.generated       = true;
    stack.plate.width_mm        = 120.f;
    stack.plate.height_mm       = 80.f;
    stack.plate.base_mm         = 1.4f;
    stack.plate.cell_mm         = 0.8f;
    stack.plate.orientation     = ChromaOrientation::FaceDown;
    stack.plate.frame.shape     = ChromaFrame::Shape::Rounded;
    stack.plate.frame.width_mm  = 9.f;
    stack.plate.frame.corner_mm = 3.f;
    stack.plate.frame.frame_id  = "gallery/ogee-walnut";
    stack.plate.frame.profile_points = { Vec2f(0.f, 2.5f), Vec2f(3.f, 2.5f), Vec2f(9.f, 0.f) };

    const std::vector<uint8_t> painted = left_half_mask(6, 4, 2);
    stack.importance.region            = chroma_mask_encode(painted.data(), 6, 4);
    stack.importance.weight            = 3.5f;

    nlohmann::json json;
    chroma_stack_to_json(json, stack);
    ChromaStack back;
    chroma_stack_from_json(json, back);

    REQUIRE(back.plate.generated);
    REQUIRE(back.plate.orientation == ChromaOrientation::FaceDown);
    REQUIRE_THAT(back.plate.width_mm, WithinRel(120.f, 1e-4f));
    REQUIRE_THAT(back.plate.base_mm, WithinRel(1.4f, 1e-4f));
    REQUIRE(back.plate.frame.shape == ChromaFrame::Shape::Rounded);
    REQUIRE(back.plate.frame.frame_id == "gallery/ogee-walnut");
    REQUIRE(back.plate.frame.profile_points.size() == 3);
    REQUIRE_THAT(back.plate.frame.profile_points[1].x(), WithinRel(3.f, 1e-4f));
    REQUIRE_THAT(back.plate.frame.profile_points[1].y(), WithinRel(2.5f, 1e-4f));
    REQUIRE_THAT(back.plate.frame.corner_mm, WithinRel(3.f, 1e-4f));
    REQUIRE(back.importance.region.runs == stack.importance.region.runs);
    REQUIRE_THAT(back.importance.weight, WithinRel(3.5f, 1e-4f));
    // The frame it reopens with is the frame it was saved wearing, not the nearest enum shape to it.
    REQUIRE(chroma_frame_profile(back.plate.frame, 1.f).size() == 3);

    SECTION("and a project saved before any plate was written comes back as the picture it was")
    {
        nlohmann::json old;
        old["layer_height_mm"] = 0.1f;
        old["bands"]           = nlohmann::json::array({ nlohmann::json{
            { "filament", 0 }, { "td_mm", 8.f }, { "color", nlohmann::json::array({ 0, 0, 0 }) }, { "layers", 6 } } });

        ChromaStack plain;
        chroma_stack_from_json(old, plain);
        REQUIRE_FALSE(plain.plate.generated);
        REQUIRE(plain.plate.orientation == ChromaOrientation::FaceUp);
        REQUIRE(plain.plate.frame.frame_id.empty());
        REQUIRE(plain.plate.frame.profile_points.empty());
        REQUIRE(plain.importance.empty());
    }

    SECTION("and an undo step keeps all of it too")
    {
        std::stringstream stream;
        {
            cereal::BinaryOutputArchive out(stream);
            out(stack);
        }
        ChromaStack restored;
        {
            cereal::BinaryInputArchive in(stream);
            in(restored);
        }
        REQUIRE(restored.plate.orientation == ChromaOrientation::FaceDown);
        REQUIRE(restored.plate.frame.frame_id == "gallery/ogee-walnut");
        REQUIRE(restored.plate.frame.profile_points.size() == 3);
        REQUIRE_THAT(restored.importance.weight, WithinRel(3.5f, 1e-6f));
        REQUIRE(restored.importance.region.runs == stack.importance.region.runs);
    }
}
