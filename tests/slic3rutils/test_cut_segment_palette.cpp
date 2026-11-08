#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/GUI/3DScene.hpp"

#include <algorithm>
#include <cmath>

// Declarative cut planes: the section colours. The Prepare tab's chunks, the Cut Planes preview and
// the G-code preview all colour a section by calling this one function with its print-order index
// and the section count, so its output is a cross-tab contract: section N of M must be the same
// colour in all of them. The root keeps the filament's colour and the later sections fade from it,
// so the part still reads as its filament and the fade reads as print order.

using Catch::Matchers::WithinAbs;
using Slic3r::ColorRGBA;

namespace {
float saturation(const ColorRGBA &c)
{
    const float mx = std::max({ c.r(), c.g(), c.b() });
    const float mn = std::min({ c.r(), c.g(), c.b() });
    return mx > 0.f ? (mx - mn) / mx : 0.f;
}
float brightness(const ColorRGBA &c) { return std::max({ c.r(), c.g(), c.b() }); }
float channel_distance(const ColorRGBA &a, const ColorRGBA &b)
{
    return std::max({ std::abs(a.r() - b.r()), std::abs(a.g() - b.g()), std::abs(a.b() - b.b()) });
}
// A saturated orange-red, well clear of the near-black colours adjust_color_for_rendering() lifts.
const ColorRGBA FILAMENT(0.9f, 0.2f, 0.1f, 1.f);
} // namespace

TEST_CASE("The root section is drawn in its filament's colour", "[cutplanes]")
{
    for (size_t count : { size_t(1), size_t(2), size_t(5) }) {
        const ColorRGBA c = cut_segment_color(0, count, FILAMENT);
        CHECK_THAT(c.r(), WithinAbs(FILAMENT.r(), 1e-4));
        CHECK_THAT(c.g(), WithinAbs(FILAMENT.g(), 1e-4));
        CHECK_THAT(c.b(), WithinAbs(FILAMENT.b(), 1e-4));
        CHECK(c.a() == 1.f);
    }
}

TEST_CASE("Later sections fade toward a quarter saturation in print order", "[cutplanes]")
{
    constexpr size_t N = 4;
    for (size_t i = 1; i < N; ++ i) {
        const ColorRGBA prev = cut_segment_color(i - 1, N, FILAMENT);
        const ColorRGBA c    = cut_segment_color(i, N, FILAMENT);
        CHECK(saturation(c) < saturation(prev));
        // Only the saturation moves: same brightness, and the hue's channel order is kept.
        CHECK_THAT(brightness(c), WithinAbs(brightness(FILAMENT), 1e-4));
        CHECK(c.r() > c.g());
        CHECK(c.g() > c.b());
        // Sections that meet are consecutively numbered far more often than not, so neighbours in
        // print order must be clearly, not marginally, distinguishable.
        CHECK(channel_distance(c, prev) > 0.1f);
    }
    CHECK_THAT(saturation(cut_segment_color(N - 1, N, FILAMENT)), WithinAbs(0.25, 1e-4));
}

TEST_CASE("A colourless filament fades in brightness instead", "[cutplanes]")
{
    // White, grey and black have no hue to desaturate, so the fade moves the brightness and the
    // sections stay grey.
    const ColorRGBA grey(0.8f, 0.8f, 0.8f, 1.f);
    constexpr size_t N = 3;
    for (size_t i = 1; i < N; ++ i) {
        const ColorRGBA c = cut_segment_color(i, N, grey);
        CHECK(brightness(c) < brightness(cut_segment_color(i - 1, N, grey)));
        CHECK_THAT(saturation(c), WithinAbs(0., 1e-4));
    }
}

TEST_CASE("Section colours are stateless and clamp past the last section", "[cutplanes]")
{
    // Unlike Plater::get_next_color_for_filament(), asking twice must give the same answer --
    // otherwise a section would change colour on every preview rebuild.
    for (size_t i = 0; i < 40; ++ i)
        CHECK(cut_segment_color(i, 40, FILAMENT) == cut_segment_color(i, 40, FILAMENT));
    CHECK(cut_segment_color(10, 4, FILAMENT) == cut_segment_color(3, 4, FILAMENT));
}
