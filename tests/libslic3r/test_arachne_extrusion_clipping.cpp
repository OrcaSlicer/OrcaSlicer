#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "libslic3r/Arachne/utils/ExtrusionLine.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ClipperZUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cstddef>
#include <map>

using namespace Slic3r;
using namespace Slic3r::ClipperZUtils;

namespace {
ZPaths diamond()
{
    return {{{-1000000, 0, 0}, {0, -1000000, 0}, {1000000, 0, 0}, {0, 1000000, 0}}};
}

void check_geometry(const ZPaths& actual, const ZPaths& raw)
{
    REQUIRE(actual.size() == raw.size());
    for (size_t p = 0; p < actual.size(); ++p) {
        REQUIRE(actual[p].size() == raw[p].size());
        for (size_t i = 0; i < actual[p].size(); ++i) {
            CHECK(actual[p][i].x() == raw[p][i].x());
            CHECK(actual[p][i].y() == raw[p][i].y());
        }
    }
}
}

TEST_CASE("Arachne clipping recovers widths at coincident vertices of a two-junction subject", "[ArachneExtrusionClipping][Arachne][Regression]")
{
    const bool reversed = GENERATE(false, true);
    const bool varying = GENERATE(false, true);
    const auto operation = GENERATE(ctIntersection, ctDifference);
    CAPTURE(reversed, varying, operation);
    ZPath subject{{-2000000, 0, 400000}, {2000000, 0, varying ? 800000 : 400000}};
    if (reversed)
        std::reverse(subject.begin(), subject.end());
    const auto clip = diamond();
    const auto raw = clip_zpaths(operation, {subject}, true, clip, {});
    const auto result = clip_extrusion(subject, clip, operation);
    check_geometry(result, raw);
    REQUIRE_FALSE(result.empty());
    const std::map<coord_t, coord_t> expected{{-2000000, 400000}, {-1000000, varying ? 500000 : 400000},
                                             {1000000, varying ? 700000 : 400000}, {2000000, varying ? 800000 : 400000}};
    for (const auto& path : result)
        for (const auto& point : path) {
            CAPTURE(point.x(), point.y(), point.z());
            REQUIRE(expected.count(point.x()) == 1);
            CHECK(point.z() > 0);
            CHECK(point.z() == expected.at(point.x()));
        }
}

TEST_CASE("Arachne clipping preserves ordinary constant-width crossings", "[ArachneExtrusionClipping][Arachne]")
{
    const bool reversed = GENERATE(false, true);
    const auto operation = GENERATE(ctIntersection, ctDifference);
    ZPath subject{{-2000000, 0, 400000}, {2000000, 0, 400000}};
    if (reversed)
        std::reverse(subject.begin(), subject.end());
    const ZPaths clip{{{-1000000, -1000000, 0}, {1000000, -1000000, 0},
                        {1000000, 1000000, 0}, {-1000000, 1000000, 0}}};
    const auto result = clip_extrusion(subject, clip, operation);
    check_geometry(result, clip_zpaths(operation, {subject}, true, clip, {}));
    REQUIRE_FALSE(result.empty());
    for (const auto& path : result)
        for (const auto& point : path)
            CHECK(point.z() == 400000);
}

TEST_CASE("Arachne clipping retains the existing multi-junction width repair", "[ArachneExtrusionClipping][Arachne]")
{
    const bool reversed = GENERATE(false, true);
    const auto operation = GENERATE(ctIntersection, ctDifference);
    ZPath subject{{-2000000, 0, 400000}, {0, 0, 400000}, {2000000, 0, 400000}};
    if (reversed)
        std::reverse(subject.begin(), subject.end());
    const auto clip = diamond();
    const auto result = clip_extrusion(subject, clip, operation);
    check_geometry(result, clip_zpaths(operation, {subject}, true, clip, {}));
    REQUIRE_FALSE(result.empty());
    for (const auto& path : result)
        for (const auto& point : path)
            CHECK(point.z() == 400000);
}

TEST_CASE("Arachne clipping leaves retained subject vertex metadata unchanged", "[ArachneExtrusionClipping][Arachne]")
{
    const bool reversed = GENERATE(false, true);
    ZPath subject{{-500000, 0, 400000}, {500000, 0, 800000}};
    if (reversed)
        std::reverse(subject.begin(), subject.end());
    const auto result = clip_extrusion(subject, diamond(), ctIntersection);
    check_geometry(result, {subject});
    REQUIRE(result.front().size() == subject.size());
    for (size_t i = 0; i < subject.size(); ++i)
        CHECK(result.front()[i].z() == subject[i].z());
}

TEST_CASE("Arachne clipping does not manufacture paths from empty subjects", "[ArachneExtrusionClipping][Arachne]")
{
    CHECK(clip_extrusion({}, diamond(), ctIntersection).empty());
    CHECK(clip_extrusion({{0, 0, 400000}}, diamond(), ctIntersection).empty());
    CHECK(clip_extrusion({{0, 0, 400000}, {0, 0, 400000}}, diamond(), ctIntersection).empty());
    CHECK(clip_extrusion({{0, 0, 400000}, {0, 0, 800000}}, diamond(), ctIntersection).empty());
}

TEST_CASE("Arachne clipping ignores duplicate coordinates when recovering a valid segment", "[ArachneExtrusionClipping][Arachne]")
{
    const bool reversed = GENERATE(false, true);
    const coord_t start_x = GENERATE(-2000000, -1000000);
    CAPTURE(reversed, start_x);
    ZPath subject{{start_x, 0, 400000}, {start_x, 0, 400000}, {2000000, 0, 400000}};
    if (reversed)
        std::reverse(subject.begin(), subject.end());
    const auto clip = diamond();
    const auto result = clip_extrusion(subject, clip, ctIntersection);
    check_geometry(result, clip_zpaths(ctIntersection, {subject}, true, clip, {}));
    REQUIRE_FALSE(result.empty());
    for (const auto& path : result)
        for (const auto& point : path)
            CHECK(point.z() == 400000);
}
