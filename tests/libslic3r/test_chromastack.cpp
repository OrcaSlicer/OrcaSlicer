#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/ChromaStack.hpp"
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
