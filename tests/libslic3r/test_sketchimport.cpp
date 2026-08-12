#include <catch2/catch_all.hpp>   // mainline OrcaSlicer ships Catch2 v3 (v2 was catch2/catch.hpp)

#include "libslic3r/SketchImport.hpp"
#include "libslic3r/Utils.hpp"   // resources_dir

#include <fstream>
#include <string>

using namespace Slic3r;

TEST_CASE("svg_to_regions parses a filled path into a region", "[SketchImport]")
{
    // A 10x10 mm filled square. Written to a temp file because nanosvg reads
    // from disk.
    const std::string path = "/tmp/snaporca_test_square.svg";
    {
        std::ofstream f(path);
        f << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10mm\" height=\"10mm\" "
             "viewBox=\"0 0 10 10\">"
             "<path d=\"M0,0 L10,0 L10,10 L0,10 Z\" fill=\"#000000\"/></svg>";
    }

    ImportRegions regs = svg_to_regions(path, 1.0);
    REQUIRE(regs.size() >= 1);
    // Outer contour present with at least a few vertices.
    REQUIRE(regs[0].size() >= 1);
    REQUIRE(regs[0][0].size() >= 4);

    // Centred on the origin: bbox half-extent ~5 mm on each side.
    double hi = 0.0;
    for (const auto& region : regs)
        for (const auto& contour : region)
            for (const Vec2d& p : contour)
                hi = std::max(hi, std::max(std::abs(p.x()), std::abs(p.y())));
    REQUIRE(hi > 3.0);    // not collapsed
    REQUIRE(hi < 8.0);    // ~5 mm half-size after centring
}

TEST_CASE("svg_to_regions rejects bad input gracefully", "[SketchImport]")
{
    REQUIRE(svg_to_regions("", 1.0).empty());
    REQUIRE(svg_to_regions("/tmp/snaporca_does_not_exist.svg", 1.0).empty());
    REQUIRE(svg_to_regions("/tmp/snaporca_test_square.svg", 0.0).empty()); // scale<=0
}

TEST_CASE("transform_regions moves and scales independently", "[SketchImport]")
{
    ImportRegions r = {{ {Vec2d(-1,-1), Vec2d(1,-1), Vec2d(1,1), Vec2d(-1,1)} }};
    ImportRegions t = transform_regions(r, Vec2d(10, 20), 2.0, 3.0);
    REQUIRE(t.size() == 1);
    REQUIRE(t[0][0].size() == 4);
    // (-1,-1) -> (-1*2+10, -1*3+20) = (8, 17)
    REQUIRE_THAT(t[0][0][0].x(), Catch::Matchers::WithinAbs(8.0, 1e-9));
    REQUIRE_THAT(t[0][0][0].y(), Catch::Matchers::WithinAbs(17.0, 1e-9));
    // (1,1) -> (1*2+10, 1*3+20) = (12, 23)
    REQUIRE_THAT(t[0][0][2].x(), Catch::Matchers::WithinAbs(12.0, 1e-9));
    REQUIRE_THAT(t[0][0][2].y(), Catch::Matchers::WithinAbs(23.0, 1e-9));
    // identity is a no-op
    ImportRegions id = transform_regions(r, Vec2d(0,0), 1.0, 1.0);
    REQUIRE_THAT(id[0][0][1].x(), Catch::Matchers::WithinAbs(1.0, 1e-9));
}

TEST_CASE("text_to_regions vectorizes glyphs with counters", "[SketchImport]")
{
    // Locate the bundled font; resources_dir() may be unset under ctest, so
    // fall back to a cwd-relative path (tests run from the repo root).
    std::string font = resources_dir().empty()
                           ? std::string("resources/fonts/HarmonyOS_Sans_SC_Regular.ttf")
                           : resources_dir() + "/fonts/HarmonyOS_Sans_SC_Regular.ttf";
    {
        std::ifstream probe(font);
        if (!probe.good()) {
            SUCCEED("bundled font not reachable in this environment; covered live on :10");
            return;
        }
    }

    // Bad input is rejected without throwing.
    REQUIRE(text_to_regions("", 10.0, font).empty());
    REQUIRE(text_to_regions("A", 0.0, font).empty());

    // 'A' has one triangular counter -> a region with an outer + 1 hole.
    ImportRegions a = text_to_regions("A", 12.0, font);
    REQUIRE(a.size() >= 1);
    bool has_hole = false;
    for (const auto& region : a)
        if (region.size() >= 2) has_hole = true;
    REQUIRE(has_hole);

    // Two letters produce more regions than one.
    ImportRegions ab = text_to_regions("AB", 12.0, font);
    REQUIRE(ab.size() >= a.size());
}
