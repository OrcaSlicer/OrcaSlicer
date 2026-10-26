// convert_filament_colors lives in libslic3r_gui; this is the only suite that links it.
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstddef>

#include "libslic3r/Color.hpp"
#include "libvgcode/include/Types.hpp"
#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/LibVGCode/LibVGCodeWrapper.hpp"

using Catch::Matchers::WithinAbs;

TEST_CASE("Filament colors keep their opacity for the G-code preview", "[GCodePreviewColors]")
{
    const libvgcode::PaletteWithAlpha palette = libvgcode::convert_filament_colors({ "#3366CC", "#3366CC80", "not a color" });
    REQUIRE(palette.size() == 3);

    CHECK(palette[0][3] == 255);
    CHECK(palette[1][3] == 0x80);
    for (size_t i = 0; i < 3; ++i)
        CHECK(palette[1][i] == palette[0][i]);

    const libvgcode::ColorWithAlpha dummy{ libvgcode::DUMMY_COLOR[0], libvgcode::DUMMY_COLOR[1], libvgcode::DUMMY_COLOR[2], 255 };
    CHECK(palette[2] == dummy);
}

TEST_CASE("An almost clear filament keeps its color at the least opacity", "[GCodePreviewColors]")
{
    Slic3r::ColorRGBA clear;
    REQUIRE(Slic3r::decode_color("#3366CC10", clear));
    REQUIRE(clear.a() < FullyTransparentMaterialThreshold);
    const Slic3r::ColorRGBA drawn = adjust_color_for_rendering(clear);
    CHECK(drawn.r() == clear.r());
    CHECK(drawn.g() == clear.g());
    CHECK(drawn.b() == clear.b());
    CHECK(drawn.a() == FullyTransparentMaterialThreshold);

    const libvgcode::PaletteWithAlpha palette = libvgcode::convert_filament_colors({ "#3366CC10" });
    REQUIRE(palette.size() == 1);
    for (int i = 0; i < 3; ++i)
        CHECK(palette[0][i] == std::lround(255.0f * drawn[i]));
    CHECK(palette[0][3] == std::lround(255.0f * drawn.a()));
}

TEST_CASE("A translucent black filament is lifted to a grey that rises with its transparency", "[GCodePreviewColors]")
{
    const libvgcode::PaletteWithAlpha palette = libvgcode::convert_filament_colors({ "#000000", "#00000080", "#00000020" });
    REQUIRE(palette.size() == 3);
    for (const libvgcode::ColorWithAlpha& color : palette) {
        CHECK(color[1] == color[0]);
        CHECK(color[2] == color[0]);
    }
    CHECK(palette[0][3] == 255);
    CHECK(palette[1][3] == 0x80);
    CHECK(palette[0][0] < palette[1][0]);
    CHECK(palette[1][0] < palette[2][0]);
    CHECK(palette[2][0] < 160);
}

TEST_CASE("Preview filament colors convert back with their opacity", "[GCodePreviewColors]")
{
    const Slic3r::ColorRGBA color = libvgcode::convert(libvgcode::ColorWithAlpha{ 51, 102, 204, 128 });
    CHECK_THAT(color.r(), WithinAbs(51.0 / 255.0, 1e-6));
    CHECK_THAT(color.a(), WithinAbs(128.0 / 255.0, 1e-6));
}
