#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <math.h>
#include <string>
#include <utility>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/EmbossShape.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/TextConfiguration.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

// One square letter. Its first two points are the bottom edge, left to right.
ExPolygons square(coord_t center_x, coord_t bottom_y, coord_t size)
{
    const coord_t half = size / 2;
    Polygon contour({Point(center_x - half, bottom_y), Point(center_x + half, bottom_y),
                     Point(center_x + half, bottom_y + size), Point(center_x - half, bottom_y + size)});
    return {ExPolygon(contour)};
}

ExPolygonsWithId letter(unsigned id, ExPolygons shape = {}) { return ExPolygonsWithId{id, std::move(shape)}; }

const Points &points_of(const ExPolygonsWithId &letter) { return letter.expoly.front().contour.points; }

// Middle of the letter's bottom edge, the point that rides on the line's circle.
Vec2d bottom_center(const ExPolygonsWithId &letter)
{
    const Points &pts = points_of(letter);
    return (pts[0].cast<double>() + pts[1].cast<double>()) / 2.;
}

// All contour points of the letters in order, to tell whether a text was moved at all.
Points all_points(const ExPolygonsWithIds &shapes)
{
    Points result;
    for (const ExPolygonsWithId &letter : shapes)
        for (const ExPolygon &expolygon : letter.expoly)
            result.insert(result.end(), expolygon.contour.points.begin(), expolygon.contour.points.end());
    return result;
}

// The font the app ships, empty when the resources folder is not next to the tests.
Emboss::FontFileWithCache bundled_font()
{
    const std::string path = std::string(PROFILES_DIR) + "/../fonts/HarmonyOS_Sans_SC_Regular.ttf";
    return Emboss::FontFileWithCache(Emboss::create_font_file(path.c_str()));
}

double area_of(const ExPolygons &shape)
{
    double sum = 0.;
    for (const ExPolygon &expolygon : shape)
        sum += expolygon.area();
    return sum;
}

} // namespace

TEST_CASE("A letter under the anchor is not moved by bending", "[Emboss]")
{
    ExPolygonsWithIds shapes{letter('a', square(0, 0, 100))};
    const Points      before = points_of(shapes.front());

    Emboss::bend_to_arc(shapes, 1000., 0., 200.);

    REQUIRE(points_of(shapes.front()) == before);
}

TEST_CASE("A bent letter keeps its shape and sits on the circle", "[Emboss]")
{
    // The letter is a quarter of the circle away from the anchor, so it ends up beside the centre, turned on its side.
    const double      radius = 2000. / M_PI;
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100))};

    Emboss::bend_to_arc(shapes, radius, 0., 200.);

    const BoundingBox bb = get_extents(shapes.front().expoly);
    CHECK_THAT(static_cast<double>(bb.min.x()), WithinAbs(637., 1.5));
    CHECK_THAT(static_cast<double>(bb.min.y()), WithinAbs(-687., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.x()), WithinAbs(737., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.y()), WithinAbs(-587., 1.5));
    CHECK_THAT(shapes.front().expoly.front().area(), WithinRel(10000., 0.01));
}

TEST_CASE("A negative radius bends the text the other way", "[Emboss]")
{
    const double      radius = -2000. / M_PI;
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100))};

    Emboss::bend_to_arc(shapes, radius, 0., 200.);

    // The text end rises and the letter's top points at the centre, which is now above the text.
    const BoundingBox bb = get_extents(shapes.front().expoly);
    CHECK_THAT(static_cast<double>(bb.min.x()), WithinAbs(537., 1.5));
    CHECK_THAT(static_cast<double>(bb.min.y()), WithinAbs(587., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.x()), WithinAbs(637., 1.5));
    CHECK_THAT(static_cast<double>(bb.max.y()), WithinAbs(687., 1.5));
}

TEST_CASE("Lines of bent text share one centre", "[Emboss]")
{
    const double      radius      = 1000.;
    const double      line_height = 200.;
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100)), letter(Emboss::ENTER_UNICODE),
                             letter('b', square(1000, -200, 100))};

    Emboss::bend_to_arc(shapes, radius, 0., line_height);

    const Vec2d center(0., -radius);
    CHECK_THAT((bottom_center(shapes[0]) - center).norm(), WithinAbs(radius, 1.5));
    CHECK_THAT((bottom_center(shapes[2]) - center).norm(), WithinAbs(radius - line_height, 1.5));
    // The inner line keeps its own spacing: 1000 units along a circle of 800 is a turn of 1.25 rad.
    CHECK_THAT(bottom_center(shapes[2]).x(), WithinAbs(759.2, 1.5));
    CHECK_THAT(bottom_center(shapes[2]).y(), WithinAbs(-747.7, 1.5));
}

TEST_CASE("The first baseline's height sets the line's radius", "[Emboss]")
{
    // A baseline 300 units under the anchor rides a circle of 700 inside the one of 1000 through the anchor.
    ExPolygonsWithIds shapes{letter('a', square(1000, -300, 100))};

    Emboss::bend_to_arc(shapes, 1000., -300., 200.);

    CHECK_THAT(bottom_center(shapes[0]).x(), WithinAbs(692.9, 1.5));
    CHECK_THAT(bottom_center(shapes[0]).y(), WithinAbs(-900.8, 1.5));
}

TEST_CASE("Text stays straight when a line would sit at or past the circle's centre", "[Emboss]")
{
    ExPolygonsWithIds shapes{letter('a', square(1000, 0, 100)), letter(Emboss::ENTER_UNICODE),
                             letter('b', square(1000, -200, 100))};
    const Points before = all_points(shapes);

    // The second line's baseline would be 100 units past the centre.
    Emboss::bend_to_arc(shapes, 100., 0., 200.);

    CHECK(all_points(shapes) == before);
}

TEST_CASE("Text stays straight when a line is longer than its circle", "[Emboss]")
{
    // The line spans 2100 units from the first letter's left edge to the last one's right edge,
    // 2000 between the letters' centres, and its baseline is 100 units under the anchor.
    ExPolygonsWithIds shapes{letter('a', square(-1000, -100, 100)), letter('b', square(1000, -100, 100))};
    const Points      before = all_points(shapes);

    // The line's circle has radius 330 and is 2073 units around; the circle through the anchor is 2702 around.
    Emboss::bend_to_arc(shapes, 430., -100., 200.);
    CHECK(all_points(shapes) == before);

    // The line's circle has radius 340 and is 2136 units around.
    Emboss::bend_to_arc(shapes, 440., -100., 200.);
    CHECK(all_points(shapes) != before);
}

TEST_CASE("Text stays straight when its letters would reach the circle's centre", "[Emboss]")
{
    // With a negative radius the tops point at the centre: the baseline is 50 units from it, the tops 50 past it.
    ExPolygonsWithIds shapes{letter('a', square(100, 0, 100))};
    const Points      before = all_points(shapes);

    Emboss::bend_to_arc(shapes, -50., 0., 200.);
    CHECK(all_points(shapes) == before);

    // With the baseline 160 units away the tops stay 60 short of the centre.
    Emboss::bend_to_arc(shapes, -160., 0., 200.);
    CHECK(all_points(shapes) != before);
}

TEST_CASE("Text stays straight when its letter tops do not fit around the inside of the circle", "[Emboss]")
{
    // The line spans 2100 units and its letters are 100 tall.
    ExPolygonsWithIds shapes{letter('a', square(-1000, 0, 100)), letter('b', square(1000, 0, 100))};
    const Points      before = all_points(shapes);

    // The baseline circle of radius 400 is 2513 units around, but the tops ride one of radius 300, 1885 around.
    Emboss::bend_to_arc(shapes, -400., 0., 200.);
    CHECK(all_points(shapes) == before);

    // The tops ride a circle of radius 350, 2199 units around.
    Emboss::bend_to_arc(shapes, -450., 0., 200.);
    CHECK(all_points(shapes) != before);
}

TEST_CASE("A radius that is not a number or too large to bend anything leaves the text straight", "[Emboss]")
{
    const double radius = GENERATE(std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                                   std::numeric_limits<double>::quiet_NaN(), 1e30, -1e30);
    ExPolygonsWithIds shapes{letter('a', square(-1000, 0, 100)), letter('b', square(1000, 0, 100))};
    const Points      before = all_points(shapes);

    Emboss::bend_to_arc(shapes, radius, 0., 200.);

    CHECK(all_points(shapes) == before);
}

TEST_CASE("Letters either side of the anchor mirror each other", "[Emboss]")
{
    ExPolygonsWithIds shapes{letter('a', square(-500, 0, 100)), letter(' '), letter('b', square(500, 0, 100))};

    Emboss::bend_to_arc(shapes, 1000., 0., 200.);

    const BoundingBox left  = get_extents(shapes[0].expoly);
    const BoundingBox right = get_extents(shapes[2].expoly);
    CHECK_THAT(static_cast<double>(left.min.x()), WithinAbs(-static_cast<double>(right.max.x()), 1.5));
    CHECK_THAT(static_cast<double>(left.min.y()), WithinAbs(static_cast<double>(right.min.y()), 1.5));
    // Both ends drop below where they were.
    CHECK(left.max.y() < 100);
}

TEST_CASE("Text with an arc radius keeps every letter's area", "[Emboss]")
{
    Emboss::FontFileWithCache font = bundled_font();
    if (!font.has_value())
        SKIP("the bundled font is not reachable");

    const FontProp straight_prop(10.f);
    FontProp       bent_prop(10.f);
    bent_prop.arc_radius = 20.f;

    const ExPolygonsWithIds straight = Emboss::text2vshapes(font, L"Radial", straight_prop);
    const ExPolygonsWithIds bent     = Emboss::text2vshapes(font, L"Radial", bent_prop);

    REQUIRE(straight.size() == 6);
    REQUIRE(bent.size() == straight.size());
    for (size_t i = 0; i < straight.size(); ++i) {
        INFO("letter " << i);
        CHECK_THAT(area_of(bent[i].expoly), WithinRel(area_of(straight[i].expoly), 0.01));
    }
    CHECK(get_extents(bent).size().y() > get_extents(straight).size().y());
}

TEST_CASE("Bent text stays in the band of its line around the circle", "[Emboss]")
{
    Emboss::FontFileWithCache font = bundled_font();
    if (!font.has_value())
        SKIP("the bundled font is not reachable");

    const float       radius_mm = GENERATE(20.f, -20.f);
    const FontProp    straight_prop(10.f);
    FontProp          bent_prop(10.f);
    bent_prop.arc_radius = radius_mm;

    const ExPolygonsWithIds straight = Emboss::text2vshapes(font, L"Radial", straight_prop);
    const ExPolygonsWithIds bent     = Emboss::text2vshapes(font, L"Radial", bent_prop);

    // A letter only turns about the circle's centre after sliding under the anchor,
    // so its points keep their height above the centre to within half a letter's width.
    const double      radius = radius_mm / Emboss::get_text_shape_scale(bent_prop, *font.font_file);
    const BoundingBox line   = get_extents(straight);
    coord_t           widest = 0;
    for (const ExPolygonsWithId &letter : straight)
        widest = std::max(widest, get_extents(letter.expoly).size().x());
    const double low  = std::min(std::abs(radius + line.min.y()), std::abs(radius + line.max.y()));
    const double high = std::hypot(std::max(std::abs(radius + line.min.y()), std::abs(radius + line.max.y())), widest / 2.);

    const Vec2d center(0., -radius);
    for (const ExPolygonsWithId &letter : bent)
        for (const ExPolygon &expolygon : letter.expoly)
            for (const Point &p : expolygon.contour.points) {
                const double distance = (p.cast<double>() - center).norm();
                REQUIRE(distance > low - 2.);
                REQUIRE(distance < high + 2.);
            }
}

TEST_CASE("An arc radius is ignored with per-glyph orientation", "[Emboss]")
{
    Emboss::FontFileWithCache font = bundled_font();
    if (!font.has_value())
        SKIP("the bundled font is not reachable");

    FontProp per_glyph_prop(10.f);
    per_glyph_prop.per_glyph = true;
    FontProp with_radius     = per_glyph_prop;
    with_radius.arc_radius   = 20.f;

    const ExPolygonsWithIds expected = Emboss::text2vshapes(font, L"Radial", per_glyph_prop);
    const ExPolygonsWithIds actual   = Emboss::text2vshapes(font, L"Radial", with_radius);

    REQUIRE(actual.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        INFO("letter " << i);
        CHECK(actual[i].expoly == expected[i].expoly);
    }
}

TEST_CASE("Text styles differ when only the arc radius differs", "[Emboss]")
{
    FontProp a;
    FontProp b;
    REQUIRE(a == b);

    a.arc_radius = 20.f;
    CHECK_FALSE(a == b);

    b.arc_radius = 20.f;
    CHECK(a == b);
}

TEST_CASE("Each line of bent text turns by the angle of its own baseline", "[Emboss]")
{
    Emboss::FontFileWithCache font = bundled_font();
    if (!font.has_value())
        SKIP("the bundled font is not reachable");

    // The alignment moves the baselines against the anchor and decides how far each letter is from it.
    const FontProp::Align align = GENERATE(FontProp::Align(FontProp::HorizontalAlign::left, FontProp::VerticalAlign::top),
                                           FontProp::Align(FontProp::HorizontalAlign::center, FontProp::VerticalAlign::center),
                                           FontProp::Align(FontProp::HorizontalAlign::right, FontProp::VerticalAlign::bottom));
    FontProp straight_prop(10.f);
    straight_prop.align = align;
    FontProp bent_prop   = straight_prop;
    bent_prop.arc_radius = 30.f;

    const ExPolygonsWithIds straight = Emboss::text2vshapes(font, L"HH\nHH", straight_prop);
    const ExPolygonsWithIds bent     = Emboss::text2vshapes(font, L"HH\nHH", bent_prop);
    REQUIRE(straight.size() == 5);
    REQUIRE(bent.size() == 5);

    const Emboss::FontFile &font_file = *font.font_file;
    const double    scale       = Emboss::get_text_shape_scale(bent_prop, font_file);
    const double    radius      = 30. / scale;
    const double    baseline    = Emboss::get_align_y_offset_in_mm(align.second, 2, font_file, bent_prop) / scale;
    const double    line_height = Emboss::get_line_height(font_file, bent_prop);

    // The last letter of the first line and of the second line; none of them sits on the anchor.
    const size_t letters[]   = {1, 4};
    const double baselines[] = {baseline, baseline - line_height};
    for (size_t i = 0; i < 2; ++i) {
        INFO("line " << i);
        const Points &before = points_of(straight[letters[i]]);
        const Points &after  = points_of(bent[letters[i]]);
        REQUIRE(after.size() == before.size());

        // A rigid turn shows on any edge of the letter; take the pair of points furthest apart.
        size_t far = 1;
        for (size_t j = 1; j < before.size(); ++j)
            if ((before[j] - before[0]).cast<double>().norm() > (before[far] - before[0]).cast<double>().norm())
                far = j;
        const Vec2d  edge_before = (before[far] - before[0]).cast<double>();
        const Vec2d  edge_after  = (after[far] - after[0]).cast<double>();
        const double turned      = std::atan2(edge_before.x() * edge_after.y() - edge_before.y() * edge_after.x(), edge_before.dot(edge_after));

        const double center_x = static_cast<double>(get_extents(straight[letters[i]].expoly).center().x());
        const double expected = center_x / (radius + baselines[i]);
        // Clockwise, so the angle is negative.
        CHECK_THAT(turned, WithinAbs(-expected, 1e-3));
    }
}
