#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "slic3r/GUI/AI/ModelGeneration/ModelPreviewPalette.hpp"
#include "slic3r/GUI/AI/ModelGeneration/PortraitColorPackMapping.hpp"
#include "slic3r/GUI/AI/Orca/FilamentColorPack.hpp"
#include <limits>

using namespace Slic3r::GUI::PreviewPalette;

TEST_CASE("Portrait card keeps skin and lips separate and recolors cool clothing", "[FilamentColorPack]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> source {{46.f/255,46.f/255,49.f/255}, {57.f/255,67.f/255,59.f/255},
        {92.f/255,91.f/255,94.f/255}, {176.f/255,80.f/255,69.f/255},
        {204.f/255,148.f/255,112.f/255}, {233.f/255,233.f/255,236.f/255}};
    const auto mapping = portrait_pack_mapping(source, card);
    REQUIRE(mapping.enabled);
    REQUIRE(mapping.mapping_colors == source);
    CHECK(mapping.target_colors == std::vector<Color>{card[1], card[4], card[5], card[3], card[0], card[2]});
    auto reversed = source; std::reverse(reversed.begin(), reversed.end());
    auto expected = mapping.target_colors; std::reverse(expected.begin(), expected.end());
    CHECK(portrait_pack_mapping(reversed, card).target_colors == expected);
    CHECK(portrait_pack_mapping({{.96f,.95f,.94f}}, card).target_colors == std::vector<Color>{card[2]});
    CHECK_FALSE(portrait_pack_mapping({}, card).enabled);
    CHECK_FALSE(portrait_pack_mapping(source, {}).enabled);
}

TEST_CASE("Portrait matching preserves locally painted filament colors on repeated application", "[FilamentColorPack]")
{
    const std::vector<Color> card {{247.f/255,226.f/255,218.f/255}, {40.f/255,38.f/255,41.f/255},
        {246.f/255,247.f/255,249.f/255}, {234.f/255,154.f/255,146.f/255},
        {102.f/255,140.f/255,182.f/255}, {149.f/255,139.f/255,134.f/255}};
    const std::vector<Color> edited {{.2f,.2f,.2f}, {92.f/255,91.f/255,94.f/255},
        {176.f/255,80.f/255,69.f/255}, card[4], {204.f/255,148.f/255,112.f/255},
        {233.f/255,233.f/255,236.f/255}};
    const auto mapped = portrait_pack_mapping(edited, card);
    CHECK(mapped.target_colors == std::vector<Color>{card[1], card[5], card[3], card[4], card[0], card[2]});
    CHECK(portrait_pack_mapping(mapped.target_colors, card).target_colors == mapped.target_colors);
    auto shuffled = card; std::reverse(shuffled.begin(), shuffled.end());
    CHECK(portrait_pack_mapping(shuffled, card).target_colors == shuffled);
    auto rounded = card[4]; rounded[1] = .54902f;
    CHECK(portrait_pack_mapping({rounded}, card).target_colors == std::vector<Color>{card[4]});
}

TEST_CASE("The portrait color pack preserves the supplied physical slot order", "[FilamentColorPack]")
{
    const auto pack = Slic3r::GUI::young_portrait_color_pack();
    REQUIRE(pack.valid());
    REQUIRE(pack.colors == std::vector<std::string>{"#F7E2DA", "#282629", "#F6F7F9", "#EA9A92", "#668CB6", "#958B86"});
    REQUIRE(pack.labels.size() == pack.colors.size());
}

TEST_CASE("Color packs reject incomplete and nonphysical color cards", "[FilamentColorPack]")
{
    auto pack = Slic3r::GUI::young_portrait_color_pack();
    SECTION("Incomplete slot metadata") { pack.labels.pop_back(); }
    SECTION("Non RGB color") { pack.colors[0] = "cmyk(5,14,14,0)"; }
    SECTION("Malformed channel") { pack.colors[0] = "#FFHHFF"; }
    SECTION("More than six physical colors") { pack.colors.push_back("#FFFFFF"); pack.labels.push_back("extra"); }
    SECTION("Unnamed card") { pack.name.clear(); }
    REQUIRE_FALSE(pack.valid());
}

TEST_CASE("Preview palettes stay bounded and deterministic for a color gradient", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned i = 0; i < 256; ++i) histogram.add((i << 16) | ((255-i) << 8) | (i/2), 1);
    const auto colors = histogram.palette();
    REQUIRE(colors.size() == 6);
    REQUIRE(histogram.palette() == colors);
    REQUIRE(histogram.palette(100).size() == 6);
    REQUIRE(histogram.palette(0).empty());
    for (const auto& color : colors) for (float c : color) {
        REQUIRE(std::isfinite(c)); REQUIRE(c >= 0); REQUIRE(c <= 1);
    }
}

TEST_CASE("Preview area weights are independent of repeated tessellation samples", "[ModelPreviewPalette]")
{
    Histogram coarse, dense;
    coarse.add(0xff0000, 90); coarse.add(0x0000ff, 10);
    dense.add(0xff0000, 90);
    for (int i = 0; i < 1000; ++i) dense.add(0x0000ff, .01);
    const auto first = coarse.palette(1), second = dense.palette(1);
    REQUIRE(distance(first[0], second[0]) < 1e-8f);
    REQUIRE(distance(to_lab(first[0]), to_lab({1,0,0})) < distance(to_lab(first[0]), to_lab({0,0,1})));
}

TEST_CASE("Preview palettes handle empty and monochrome inputs", "[ModelPreviewPalette]")
{
    Histogram histogram;
    REQUIRE(histogram.palette().empty());
    histogram.add(0xff0000, 0);
    histogram.add(0xff0000, -1);
    REQUIRE(histogram.palette().empty());
    histogram.add(0xe8e7ec, 10);
    const auto colors = histogram.palette();
    REQUIRE(colors.size() == 1);
    REQUIRE(distance(colors[0], {232.f/255,231.f/255,236.f/255}) < 1e-8f);
}

TEST_CASE("Preview perceptual conversion round trips primary and neutral colors", "[ModelPreviewPalette]")
{
    for (Color color : {Color{0,0,0}, Color{1,1,1}, Color{1,0,0}, Color{0,1,0}, Color{0,0,1}, Color{.8f,.5f,.3f}})
        REQUIRE(distance(to_rgb(to_lab(color)), color) < 1e-8f);
}

TEST_CASE("Preview Oklab coordinates match the published sRGB primary reference", "[ModelPreviewPalette]")
{
    using Catch::Matchers::WithinAbs;
    // Bjorn Ottosson's 2021 linear-sRGB matrices, including the sRGB transfer
    // function. An internally consistent but wrong forward/inverse pair must
    // not pass solely because it round-trips.
    const Color red = to_lab({1, 0, 0});
    REQUIRE_THAT(red[0], WithinAbs(.62795536, 1e-6));
    REQUIRE_THAT(red[1], WithinAbs(.22486306, 1e-6));
    REQUIRE_THAT(red[2], WithinAbs(.12584630, 1e-6));
    const Color gray = to_lab({.5f, .5f, .5f});
    REQUIRE_THAT(gray[0], WithinAbs(.59818073, 1e-6));
    REQUIRE_THAT(gray[1], WithinAbs(0, 1e-6));
    REQUIRE_THAT(gray[2], WithinAbs(0, 1e-6));
}

TEST_CASE("Preview nearest color preserves group assignment when only target colors change", "[ModelPreviewPalette]")
{
    const std::vector<Color> original {{.15f, .4f, .2f}, {.85f, .2f, .15f}, {.95f, .95f, .95f}};
    std::vector<Color> mapping;
    for (const Color color : original) mapping.push_back(to_lab(color));
    auto edited = original;
    edited[0] = {0, 0, 1};
    const size_t original_group = nearest_lab_index(to_lab({.16f, .41f, .21f}), mapping);
    REQUIRE(original_group == 0);
    REQUIRE(edited[original_group] == Color{0, 0, 1});
    REQUIRE(nearest_lab_index(to_lab({.84f, .21f, .15f}), mapping) == 1);
    // Identical/tied mapping centers use the first group on CPU and GPU.
    REQUIRE(nearest_lab_index(to_lab(original[0]), {mapping[0], mapping[0]}) == 0);
    REQUIRE(nearest_lab_index(to_lab(original[0]), {}) == 0);
}

TEST_CASE("Preview nearest colors use the same chroma priority across interpolated source samples", "[ModelPreviewPalette]")
{
    const std::vector<Color> centers {{.5f, 0, 0}, {.8f, .08f, 0}, {.2f, 0, -.12f}};
    // The shader interpolates source RGB before conversion and selects one
    // palette entry at each fragment; target colors must never be interpolated.
    for (int i = 0; i <= 100; ++i) {
        const float t = i / 100.f;
        const Color source = to_lab({t, 1 - t, .4f});
        size_t reference = 0;
        float best = std::numeric_limits<float>::max();
        for (size_t j = 0; j < centers.size(); ++j) {
            const float dl = (source[0] - centers[j][0]) * .35f;
            const float da = source[1] - centers[j][1], db = source[2] - centers[j][2];
            const float d = dl * dl + da * da + db * db;
            if (d < best) { best = d; reference = j; }
        }
        REQUIRE(nearest_lab_index(source, centers) == reference);
    }
}

TEST_CASE("Protected preview palettes keep supported minority hues among dominant gray shades", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    histogram.add(0x18392d, 300);
    histogram.add(0x915647, 150);
    histogram.add(0x0000ff, 1); // A tiny blue speck must not reserve a channel.
    const auto area = histogram.palette(6), protected_colors = histogram.palette(6, {}, true);
    auto error = [](const std::vector<Color>& palette, Color target) {
        float best = 100;
        for (const auto& color : palette) best = std::min(best, distance(to_lab(color), to_lab(target)));
        return best;
    };
    REQUIRE(protected_colors.size() == 6);
    for (Color target : {Color{24.f/255,57.f/255,45.f/255}, Color{145.f/255,86.f/255,71.f/255}}) {
        REQUIRE(error(protected_colors, target) < 1e-8f);
        REQUIRE(error(protected_colors, target) < error(area, target));
    }
    REQUIRE(error(protected_colors, {0,0,1}) > .001f);
    REQUIRE(histogram.hue_candidates().size() == 2);
    REQUIRE(histogram.palette(6, {}, true) == protected_colors);
}

TEST_CASE("Protected palettes separate saturated accents from a dominant muted hue", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    histogram.add(0x413231, 3000);
    histogram.add(0xd5a084, 2000);
    // The accent shares the muted material's hue but has distinct chroma.
    // Averaging both into one protected hue loses this supported small color.
    histogram.add(0xbe6961, 50);
    const Color accent {190.f/255, 105.f/255, 97.f/255};
    const auto colors = histogram.palette(6, {}, true);
    REQUIRE(colors.size() == 6);
    float accent_error = 100, muted_error = 100;
    for (const auto& color : colors) {
        accent_error = std::min(accent_error, distance(to_lab(color), to_lab(accent)));
        muted_error = std::min(muted_error, distance(to_lab(color), to_lab({65.f/255, 50.f/255, 49.f/255})));
    }
    REQUIRE_THAT(accent_error, Catch::Matchers::WithinAbs(0, 1e-8));
    REQUIRE(muted_error < .000225f);
    REQUIRE(histogram.palette(6, {}, true) == colors);
}

TEST_CASE("A saturated speck cannot reserve a protected color within a muted hue", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    histogram.add(0x413231, 3000);
    histogram.add(0xd5a084, 2000);
    histogram.add(0xbe6961, 2);
    const Color speck {190.f/255, 105.f/255, 97.f/255};
    float error = 100;
    for (const auto& color : histogram.palette(6, {}, true))
        error = std::min(error, distance(to_lab(color), to_lab(speck)));
    REQUIRE(error > .000225f);
}

TEST_CASE("Preview color locks retain exact values and order across reclustering", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned i = 0; i < 256; ++i) histogram.add((i << 16) | ((255-i) << 8) | i, i+1);
    const std::vector<Color> locked {{.713f,.297f,.631f}, {.128f,.923f,.317f}};
    for (bool protect : {false, true}) {
        const auto colors = histogram.palette(6, locked, protect);
        REQUIRE(colors.size() == 6);
        REQUIRE(colors[0] == locked[0]);
        REQUIRE(colors[1] == locked[1]);
    }
    REQUIRE(histogram.palette(1, locked, true).size() == 1);
    REQUIRE(histogram.palette(1, locked, true)[0] == locked[0]);
    REQUIRE(histogram.palette(2, locked, true) == locked);
}

TEST_CASE("Protected palettes retain muted materials and small saturated accents with neutral support", "[ModelPreviewPalette]")
{
    Histogram histogram;
    for (unsigned gray : {0u, 40u, 90u, 145u, 200u, 255u})
        histogram.add((gray << 16) | (gray << 8) | gray, 10000);
    // Muted material with C below .025, plus a strong accent covering less
    // than .15% of the surface. Neither is an isolated texture speck.
    histogram.add(0x39433b, 600);
    histogram.add(0xb35145, 42);
    histogram.add(0x0000ff, 1);
    const auto colors = histogram.palette(6, {}, true);
    REQUIRE(colors.size() == 6);
    for (Color target : {Color{57.f/255,67.f/255,59.f/255}, Color{179.f/255,81.f/255,69.f/255}}) {
        float best = 100;
        for (const auto& color : colors) best = std::min(best, distance(to_lab(color), to_lab(target)));
        REQUIRE(best < 1e-8f);
    }
    float darkest = 1, lightest = 0;
    for (const auto& color : colors) {
        darkest = std::min(darkest, to_lab(color)[0]);
        lightest = std::max(lightest, to_lab(color)[0]);
    }
    REQUIRE(darkest < .15f);
    REQUIRE(lightest > .9f);
    REQUIRE(histogram.hue_candidates().size() == 2);
}

TEST_CASE("Preview color protection handles empty data invalid locks and small channel counts", "[ModelPreviewPalette]")
{
    Histogram histogram;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const std::vector<Color> locks {{nan,0,0}, {-1,0,0}, {0,2,0}, {.25f,.5f,.75f}, {.25f,.5f,.75f}};
    histogram.add(0xffffff, std::numeric_limits<double>::infinity());
    histogram.add(0xffffff, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(histogram.palette(6, {}, true).empty());
    REQUIRE(histogram.hue_candidates().empty());
    REQUIRE(histogram.palette(6, locks, true).size() == 1);
    REQUIRE(histogram.palette(6, locks, true)[0] == locks[3]);
    REQUIRE(histogram.palette(0, locks, true).empty());
    histogram.add(0xeeeeee, 500);
    histogram.add(0x006030, 10);
    for (size_t channels : {size_t(1), size_t(2), size_t(3), size_t(6)}) {
        const auto colors = histogram.palette(channels, locks, true);
        REQUIRE(colors.size() <= channels);
        REQUIRE(colors[0] == locks[3]);
        for (const auto& color : colors) for (float c : color) {
            REQUIRE(std::isfinite(c)); REQUIRE(c >= 0); REQUIRE(c <= 1);
        }
    }
}
