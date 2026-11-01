#include <catch2/catch_all.hpp>

#include "libslic3r/Arrange.hpp"
#include "libslic3r/ArrangeProgress.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include <chrono>
#include <cmath>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::arrangement;

namespace {
Polygon rectangle(double x, double y, double w, double h)
{
    return Polygon({Point(scale_(x), scale_(y)), Point(scale_(x + w), scale_(y)),
                    Point(scale_(x + w), scale_(y + h)), Point(scale_(x), scale_(y + h))});
}
ArrangePolygon part(Polygon p, int id)
{
    ArrangePolygon ap;
    ap.poly.contour = std::move(p);
    ap.itemid = id;
    ap.bed_idx = 0; // Callers assign a source plate; -1 tells stock Arrange to skip the item.
    ap.height = 10;
    return ap;
}
ArrangeParams settings()
{
    ArrangeParams p;
    p.use_sparrow = true;
    p.sparrow_time_limit_s = 0;
    p.parallel = false;
    p.progressind = {};
    return p;
}
const BoundingBox bed(Point(0, 0), Point(scale_(256.), scale_(256.)));
}

TEST_CASE("Arrange progress advances within a plate without reversing", "[arrange][progress]")
{
    ArrangeProgress progress;
    progress.begin_plate(0., 4.);
    REQUIRE(progress.advance(0., 8.) == 0.);
    const double halfway = progress.advance(4., 8.);
    REQUIRE(halfway == Approx(0.99 / 8.));
    REQUIRE(progress.advance(6., 8.) > halfway);
    const double previous = progress.advance(8., 8.);

    // Discover many more plates than expected; the percentage must not regress.
    progress.begin_plate(0.05, 20.);
    REQUIRE(progress.advance(0., 8.) == previous);
    const double later = progress.advance(4., 8.);
    REQUIRE(later > previous);
    // A lower real count (including a fallback restart) cannot lower the display.
    REQUIRE(progress.observe(0.) == later);
    REQUIRE(progress.advance(100., 8.) < 1.);
}

TEST_CASE("Arrange progress handles early stopping and exhausted budgets", "[arrange][progress]")
{
    ArrangeProgress progress;
    progress.begin_plate(0., 3.);
    const double early = progress.advance(1., 8.);
    progress.begin_plate(0., 3.);
    REQUIRE(progress.advance(0., 8.) == early);
    REQUIRE(progress.advance(1., 8.) > early);
    const double previous = progress.advance(2., 8.);
    REQUIRE(progress.advance(10., 0.) == previous);
    progress.begin_plate(0.8, 1.);
    REQUIRE(progress.advance(100., 8.) == Approx(0.99));
    REQUIRE(progress.observe(1.) == Approx(0.99));
}

TEST_CASE("Sparrow supplies fractional progress without an object callback", "[arrange][sparrow][progress]")
{
    if (!sparrow_available())
        return;
    ArrangePolygons items{part(rectangle(0, 0, 20, 20), 0)};
    auto params = settings();
    double previous = 0.;
    size_t calls = 0;
    params.progress_fraction = [&](double fraction, std::string) {
        CHECK(fraction >= previous);
        CHECK(fraction < 1.);
        previous = fraction;
        ++calls;
    };
    arrange(items, bed, params);
    REQUIRE(calls > 0);
    REQUIRE(items[0].is_arranged());
}

TEST_CASE("Sparrow reports timed progress during a plate search", "[arrange][sparrow][progress]")
{
    if (!sparrow_available())
        return;
    ArrangePolygons items;
    for (int i = 0; i < 40; ++i)
        items.push_back(part(rectangle(0, 0, 90, 90), i));
    auto params = settings();
    params.sparrow_time_limit_s = 8.;
    double previous = 0.;
    unsigned placed = 0;
    bool advanced_without_placement = false;
    params.progressind = [&](unsigned count, std::string) { placed = count; };
    params.progress_fraction = [&](double fraction, std::string) {
        CHECK(fraction >= previous);
        CHECK(fraction < 1.);
        if (placed == 0 && fraction > previous)
            advanced_without_placement = true;
        previous = fraction;
    };
    const auto start = std::chrono::steady_clock::now();
    params.stopcondition = [&] {
        return advanced_without_placement ||
            std::chrono::steady_clock::now() - start > std::chrono::seconds(2);
    };
    arrange(items, bed, params);
    REQUIRE(advanced_without_placement);
}

TEST_CASE("Arrange rescues near-bed-size parts", "[arrange][sparrow]")
{
    ArrangePolygons items{part(rectangle(0, 0, 255, 255), 0)};
    arrange(items, bed, settings());
    REQUIRE(items[0].bed_idx == 0);
    const auto placed = items[0].transformed_poly();
    REQUIRE(diff(Polygons{placed.contour}, Polygons{bed.polygon()}).empty());
}

TEST_CASE("Stock finishes what sparrow leaves out, around sparrow's layout", "[arrange][sparrow]")
{
    if (!sparrow_available())
        return;
    // At full spacing a 255 mm part does not fit sparrow's 256 mm bed; stock's near-bed-size
    // rescue reduces the spacing and places it. A 300 mm part fits nowhere.
    ArrangePolygons items{part(rectangle(0, 0, 20, 20), 0), part(rectangle(0, 0, 20, 20), 1), part(rectangle(0, 0, 255, 255), 2),
                          part(rectangle(0, 0, 300, 300), 3)};
    for (auto &ap : items)
        ap.bed_idx = UNARRANGED;
    auto params = settings();
    params.sparrow_only = true;
    arrange(items, bed, params);
    for (size_t i = 0; i < 3; ++i) {
        INFO("item " << i);
        REQUIRE(items[i].is_arranged());
    }
    REQUIRE(items[3].bed_idx == UNARRANGED);
    REQUIRE(items[2].bed_idx != items[0].bed_idx);
    REQUIRE(items[2].itemid == 2); // stock renumbers its own items; keep the caller's
    const auto placed = items[2].transformed_poly();
    REQUIRE(diff(Polygons{placed.contour}, Polygons{bed.polygon()}).empty());
}

TEST_CASE("A prime tower placeholder takes room only on multi-filament plates", "[arrange][sparrow]")
{
    if (!sparrow_available())
        return;
    // The placeholder leaves a 70 mm strip on plate 0, too small for the part.
    ArrangePolygon tower = part(rectangle(0, 0, 256, 186), 9);
    tower.is_virt_object = tower.is_wipe_tower = true;
    auto plate_of = [&](std::vector<int> filaments, bool always) {
        ArrangePolygons items{part(rectangle(0, 0, 100, 100), 0)};
        items[0].extrude_ids = filaments;
        items[0].bed_idx = UNARRANGED;
        auto params = settings();
        params.sparrow_only = true;
        params.prime_tower_always = always;
        arrange(items, ArrangePolygons{tower}, bed, params);
        return items[0].bed_idx;
    };
    REQUIRE(plate_of({1}, false) == 0);
    REQUIRE(plate_of({1, 2}, false) == 1);
    REQUIRE(plate_of({1}, true) == 1); // smooth timelapse prints a tower everywhere
}

TEST_CASE("Sparrow-only arrange leaves items untouched when sparrow declines", "[arrange][sparrow]")
{
    // Filament ids beyond the 64 the masks hold: sparrow declines.
    ArrangePolygons items{part(rectangle(0, 0, 20, 20), 0), part(rectangle(0, 0, 20, 20), 1)};
    items[0].extrude_ids = {65};
    items[1].translation = Vec2crd(scale_(50.), 0);
    const ArrangePolygons before = items;
    auto params = settings();
    params.sparrow_only = true;
    arrange(items, bed, params);
    for (size_t i = 0; i < items.size(); ++i) {
        REQUIRE(items[i].bed_idx == before[i].bed_idx);
        REQUIRE(items[i].translation == before[i].translation);
    }
}

TEST_CASE("Simplified outlines keep the full spacing", "[arrange][sparrow]")
{
    if (!sparrow_available())
        return;
    // A fine circle, like an outline at mesh resolution.
    Polygon circle;
    for (int i = 0; i < 720; ++i) {
        const double a = 2. * PI * i / 720;
        circle.points.emplace_back(scale_(20. * std::cos(a)), scale_(20. * std::sin(a)));
    }
    ArrangePolygons items;
    for (int i = 0; i < 20; ++i) {
        items.push_back(part(circle, i));
        items.back().inflation = scale_(1.5);
    }
    arrange(items, bed, settings());
    for (size_t i = 0; i < items.size(); ++i) {
        REQUIRE(items[i].is_arranged());
        for (size_t j = i + 1; j < items.size(); ++j)
            if (items[i].bed_idx == items[j].bed_idx) {
                // 1.5 mm each side, less float slack in the placements.
                const Polygons a = offset(items[i].transformed_poly().contour, float(scale_(1.49)));
                const Polygons b = offset(items[j].transformed_poly().contour, float(scale_(1.49)));
                REQUIRE(intersection(a, b).empty());
            }
    }
}

TEST_CASE("Arrange preserves material separation", "[arrange][sparrow]")
{
    ArrangePolygons items{part(rectangle(0, 0, 20, 20), 0), part(rectangle(0, 0, 20, 20), 1)};
    SECTION("Incompatible temperatures") {
        items[0].filament_temp_type = FilamentTempType::LowTemp;
        items[1].filament_temp_type = FilamentTempType::HighTemp;
    }
    auto params = settings();
    SECTION("One material per plate") {
        items[0].extrude_ids = {1};
        items[1].extrude_ids = {2};
        params.allow_multi_materials_on_same_plate = false;
    }
    arrange(items, bed, params);
    REQUIRE(items[0].is_arranged());
    REQUIRE(items[1].is_arranged());
    REQUIRE(items[0].bed_idx != items[1].bed_idx);
}

TEST_CASE("Arrange respects fixed objects' materials", "[arrange][sparrow]")
{
    ArrangePolygons items{part(rectangle(0, 0, 20, 20), 0)};
    ArrangePolygons fixed{part(rectangle(100, 100, 20, 20), 1)};
    fixed[0].bed_idx = 0;
    SECTION("Incompatible temperatures") {
        items[0].filament_temp_type = FilamentTempType::LowTemp;
        fixed[0].filament_temp_type = FilamentTempType::HighTemp;
    }
    ArrangePolygons expected = items;
    auto stock = settings();
    stock.use_sparrow = false;
    arrange(expected, fixed, bed, stock);
    arrange(items, fixed, bed, settings());
    // Stock may leave this item unplaced when the source plate contains only
    // fixed objects. Preserve that behavior instead of mixing incompatible material.
    REQUIRE(items[0].bed_idx == expected[0].bed_idx);
    REQUIRE(items[0].bed_idx != 0);
}

TEST_CASE("Stock fallback uses convex hulls without changing input contours", "[arrange][sparrow]")
{
    Polygon concave({Point(0, 0), Point(scale_(40.), scale_(0.)), Point(scale_(40.), scale_(10.)),
                     Point(scale_(10.), scale_(10.)), Point(scale_(10.), scale_(40.)), Point(scale_(0.), scale_(40.))});
    ArrangePolygons actual{part(concave, 0), part(concave, 1)};
    ArrangePolygons fixed{part(concave, 2)};
    fixed[0].bed_idx = 0;
    fixed[0].translation = Vec2crd(scale_(100.), scale_(100.));
    ArrangePolygons expected_fixed = fixed;
    expected_fixed[0].poly.contour = Geometry::convex_hull(concave.points);
    ArrangePolygons expected = actual;
    for (auto &ap : expected)
        ap.poly.contour = Geometry::convex_hull(ap.poly.contour.points);
    auto params = settings();
    // Mirrors ArrangeJob, which collects outlines for sparrow and arranges them with stock first.
    params.use_sparrow = false;
    arrange(actual, fixed, bed, params);
    arrange(expected, expected_fixed, bed, params);
    for (size_t i = 0; i < actual.size(); ++i) {
        REQUIRE(actual[i].is_arranged());
        REQUIRE(actual[i].bed_idx == expected[i].bed_idx);
        REQUIRE(actual[i].translation == expected[i].translation);
        REQUIRE(actual[i].rotation == Approx(expected[i].rotation));
        REQUIRE(actual[i].poly.contour.points == concave.points);
    }
}

TEST_CASE("Exclusion checks retain shallow mesh protrusions", "[arrange][sparrow]")
{
    // A 0.1 mm outward tip disappears under the former 0.2 mm simplification.
    std::vector<Vec3f> vertices{{0, 0, 1}, {20, 0, 1}, {20.1f, 10, 1}, {20, 20, 1}, {0, 20, 1}};
    for (size_t i = 0; i < 5; ++i)
        vertices.emplace_back(vertices[i].x(), vertices[i].y(), 2.f);
    std::vector<Vec3i> faces{{0, 2, 1}, {0, 3, 2}, {0, 4, 3},
                             {5, 6, 7}, {5, 7, 8}, {5, 8, 9}};
    for (int i = 0; i < 5; ++i) {
        int j = (i + 1) % 5;
        faces.emplace_back(i, j, j + 5);
        faces.emplace_back(i, j + 5, i + 5);
    }
    Model model;
    auto *object = model.add_object();
    const TriangleMesh mesh(vertices, faces);
    object->add_volume(mesh, false);
    auto *instance = object->add_instance();
    REQUIRE(instance->footprint_intersects({rectangle(20.04, 9, 1, 2)}, Transform3d::Identity()));
    REQUIRE_FALSE(instance->footprint_intersects({rectangle(20.2, 9, 1, 2)}, Transform3d::Identity()));
}

namespace {
ArrangePolygon ring_host(Polygon opening)
{
    auto host = part(rectangle(0, 0, 240, 240), 100);
    opening.make_clockwise();
    host.poly.holes.push_back(std::move(opening));
    host.translation = Vec2crd(scale_(8.), scale_(8.));
    return host;
}

Polygon world_opening(const ArrangePolygon &host)
{
    Polygon opening = host.poly.holes.front();
    opening.make_counter_clockwise();
    opening.rotate(host.rotation);
    opening.translate(host.translation.x(), host.translation.y());
    return opening;
}

void check_inside_opening(const ArrangePolygon &piece, const ArrangePolygon &host)
{
    const Polygon opening = world_opening(host);
    const Polygon placed = piece.transformed_poly().contour;
    REQUIRE(diff(Polygons{placed}, Polygons{opening}).empty());
    // Measure physical clearance independently of the packer's offset join style.
    // Miter-expanded square corners would incorrectly demand extra clearance
    // against curved or rotated boundaries. Both sides require at least 1 mm.
    auto distance_to_edge = [](const Point &point, const Point &a, const Point &b) {
        const Vec2d p(unscale<double>(point.x()), unscale<double>(point.y()));
        const Vec2d start(unscale<double>(a.x()), unscale<double>(a.y()));
        const Vec2d end(unscale<double>(b.x()), unscale<double>(b.y()));
        const Vec2d edge = end - start;
        const double t = edge.squaredNorm() > 0. ? std::clamp((p - start).dot(edge) / edge.squaredNorm(), 0., 1.) : 0.;
        return (p - start - t * edge).norm();
    };
    double clearance = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < placed.points.size(); ++i) {
        const auto &a = placed.points[i];
        const auto &b = placed.points[(i + 1) % placed.points.size()];
        for (size_t j = 0; j < opening.points.size(); ++j) {
            const auto &c = opening.points[j];
            const auto &d = opening.points[(j + 1) % opening.points.size()];
            clearance = std::min({clearance, distance_to_edge(a, c, d), distance_to_edge(b, c, d),
                                  distance_to_edge(c, a, b), distance_to_edge(d, a, b)});
        }
    }
    REQUIRE(clearance >= 1.98);
}
}

TEST_CASE("Sparrow fills a closed opening without moving its host", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    ArrangePolygons fixed{ring_host(rectangle(20, 20, 200, 200))};
    const auto original = fixed.front();
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0), part(rectangle(0, 0, 80, 80), 1)};
    auto params = settings();
    params.allow_rotations = false;
    arrange(pieces, fixed, bed, params);
    for (const auto &piece : pieces) {
        REQUIRE(piece.bed_idx == 0);
        REQUIRE(piece.rotation == 0.);
        check_inside_opening(piece, fixed.front());
    }
    REQUIRE(intersection(offset(pieces[0].transformed_poly().contour, scale_(0.99)),
                         offset(pieces[1].transformed_poly().contour, scale_(0.99))).empty());
    REQUIRE(fixed.front().translation == original.translation);
    REQUIRE(fixed.front().rotation == original.rotation);
    REQUIRE(fixed.front().poly.holes.front().points == original.poly.holes.front().points);
}

TEST_CASE("Sparrow cannot fill openings based on area alone", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    Polygon hole;
    SECTION("Large area but too narrow") { hole = rectangle(20, 20, 10, 200); }
    SECTION("Raw dimensions fit but spacing does not") { hole = rectangle(20, 20, 30, 30); }
    ArrangePolygons fixed{ring_host(hole)};
    ArrangePolygons pieces{part(rectangle(0, 0, 30, 30), 0)};
    arrange(pieces, fixed, bed, settings());
    REQUIRE(pieces.front().is_arranged());
    REQUIRE(pieces.front().bed_idx > 0);
}

TEST_CASE("Sparrow preserves an existing occupant of a closed opening", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    auto occupant = part(rectangle(28, 28, 200, 200), 101);
    ArrangePolygons fixed{ring_host(rectangle(20, 20, 200, 200)), occupant};
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0)};
    arrange(pieces, fixed, bed, settings());
    REQUIRE(pieces.front().is_arranged());
    REQUIRE(pieces.front().bed_idx > 0);
    REQUIRE(fixed[1].translation == occupant.translation);
    REQUIRE(fixed[1].poly.contour.points == occupant.poly.contour.points);
}

TEST_CASE("Sparrow transforms the opening with its host", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    auto host = part(rectangle(-80, -80, 160, 160), 100);
    auto hole = rectangle(-60, -60, 120, 120);
    hole.make_clockwise();
    host.poly.holes.push_back(hole);
    host.rotation = 0.4;
    host.translation = Vec2crd(scale_(128.), scale_(128.));
    ArrangePolygons fixed{host};
    ArrangePolygons pieces{part(rectangle(0, 0, 70, 70), 0)};
    arrange(pieces, fixed, bed, settings());
    REQUIRE(pieces.front().bed_idx == 0);
    check_inside_opening(pieces.front(), host);
    REQUIRE(fixed.front().translation == host.translation);
    REQUIRE(fixed.front().rotation == host.rotation);
}

TEST_CASE("Projected openings reach Arrange and close when covered by geometry", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    struct OutlineFlag {
        bool previous = use_true_outline.exchange(true);
        ~OutlineFlag() { use_true_outline.store(previous); }
    } flag;
    Model model;
    auto *object = model.add_object();
    auto add_box = [&](double x, double y, double w, double h) {
        auto mesh = make_cube(w, h, 10.);
        mesh.translate(x, y, 0.);
        object->add_volume(mesh, false);
    };
    add_box(0, 0, 200, 20);
    add_box(0, 180, 200, 20);
    add_box(0, 20, 20, 160);
    add_box(180, 20, 20, 160);
    auto *instance = object->add_instance();
    DynamicPrintConfig config;
    ArrangePolygon first;
    instance->get_arrange_polygon(&first, config);
    REQUIRE(first.poly.holes.size() == 1);
    REQUIRE(std::abs(first.poly.holes.front().area()) * SCALING_FACTOR * SCALING_FACTOR == Approx(160. * 160.));
    REQUIRE_FALSE(instance->footprint_intersects({rectangle(80, 80, 20, 20)}, Transform3d::Identity()));
    // A later volume change must invalidate the cached opening. Even a thin
    // ceiling would make the projected opening unsafe for another printed part.
    add_box(20, 20, 160, 160);
    ArrangePolygon covered;
    instance->get_arrange_polygon(&covered, config);
    REQUIRE(covered.poly.holes.empty());
    REQUIRE(instance->footprint_intersects({rectangle(80, 80, 20, 20)}, Transform3d::Identity()));
}

TEST_CASE("Cancelling before hole cleanup preserves the completed placement", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    ArrangePolygons fixed{ring_host(rectangle(20, 20, 200, 200))};
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0)};
    auto params = settings();
    bool cancelled = false;
    params.progressind = [&](unsigned count, std::string) {
        if (count == pieces.size()) cancelled = true;
    };
    params.stopcondition = [&] { return cancelled; };
    arrange(pieces, fixed, bed, params);
    REQUIRE(cancelled);
    REQUIRE(pieces.front().is_arranged());
    REQUIRE(pieces.front().bed_idx > 0);
}

TEST_CASE("Hole cleanup respects per-object spacing inflation", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    ArrangePolygons fixed{ring_host(rectangle(20, 20, 90, 90))};
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0)};
    fixed.front().inflation = scale_(6.);
    pieces.front().inflation = scale_(6.);
    arrange(pieces, fixed, bed, settings());
    REQUIRE(pieces.front().is_arranged());
    REQUIRE(pieces.front().bed_idx > 0);
}

TEST_CASE("A movable ring and its nested part finish on one plate", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    ArrangePolygons pieces{ring_host(rectangle(20, 20, 200, 200)), part(rectangle(0, 0, 80, 80), 0)};
    arrange(pieces, bed, settings());
    REQUIRE(pieces[0].bed_idx == 0);
    REQUIRE(pieces[1].bed_idx == 0);
    check_inside_opening(pieces[1], pieces[0]);
}

TEST_CASE("Hole cleanup avoids exclusions inside an opening", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    ArrangePolygons fixed{ring_host(rectangle(20, 20, 200, 200))};
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0)};
    auto params = settings();
    auto exclusion = part(rectangle(28, 28, 100, 100), 102);
    exclusion.is_virt_object = true;
    params.excluded_regions.push_back(exclusion);
    arrange(pieces, fixed, bed, params);
    REQUIRE(pieces.front().bed_idx == 0);
    check_inside_opening(pieces.front(), fixed.front());
    REQUIRE(intersection(offset(pieces.front().transformed_poly().contour, scale_(0.99)),
                         Polygons{exclusion.poly.contour}).empty());
}

TEST_CASE("Sparrow uses the curved opening of a circular ring", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    auto circle = [](double radius) {
        Points points;
        for (int i = 0; i < 96; ++i) {
            const double angle = i * 2. * M_PI / 96.;
            points.emplace_back(scale_(radius * std::cos(angle)), scale_(radius * std::sin(angle)));
        }
        return Polygon(points);
    };
    auto host = part(circle(120.), 100);
    auto hole = circle(100.);
    hole.make_clockwise();
    host.poly.holes.push_back(hole);
    host.translation = Vec2crd(scale_(128.), scale_(128.));
    ArrangePolygons fixed{host};
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0)};
    arrange(pieces, fixed, bed, settings());
    REQUIRE(pieces.front().bed_idx == 0);
    check_inside_opening(pieces.front(), host);
}

TEST_CASE("An off-bed fixed opening cannot pull a part outside the bed", "[arrange][sparrow][holes]")
{
    if (!sparrow_available()) return;
    auto host = part(rectangle(0, 0, 390, 240), 100);
    auto hole = rectangle(20, 20, 200, 200);
    hole.make_clockwise();
    host.poly.holes.push_back(hole);
    host.translation = Vec2crd(scale_(-150.), scale_(8.));
    ArrangePolygons fixed{host};
    ArrangePolygons pieces{part(rectangle(0, 0, 80, 80), 0)};
    arrange(pieces, fixed, bed, settings());
    REQUIRE(pieces.front().is_arranged());
    REQUIRE(pieces.front().bed_idx > 0);
    REQUIRE(diff(Polygons{pieces.front().transformed_poly().contour}, Polygons{bed.polygon()}).empty());
}

TEST_CASE("Nested rings get space before their smaller contents", "[arrange][sparrow][holes][nested]")
{
    if (!sparrow_available()) return;
    auto square_ring = [](double outer, double inner, int id) {
        auto ring = part(rectangle(0, 0, outer, outer), id);
        auto opening = rectangle((outer - inner) / 2., (outer - inner) / 2., inner, inner);
        opening.make_clockwise();
        ring.poly.holes.push_back(opening);
        return ring;
    };
    ArrangePolygons fixed{ring_host(rectangle(10, 10, 220, 220))};
    SECTION("Without a wipe tower") {}
    SECTION("With replicated empty wipe-tower placeholders") {
        for (int plate = 0; plate < 3; ++plate) {
            auto placeholder = part(rectangle(165, 238, 35, 0), 200 + plate);
            placeholder.is_virt_object = true;
            placeholder.is_wipe_tower = true;
            placeholder.bed_idx = plate;
            fixed.push_back(placeholder);
        }
    }
    // Reverse size order deliberately. The 100 mm square cannot fit beside either
    // inner ring, so all four objects share a plate only with three-level nesting.
    ArrangePolygons pieces{part(rectangle(0, 0, 100, 100), 0),
                           square_ring(160, 140, 1), square_ring(200, 180, 2)};
    auto params = settings();
    params.allow_rotations = false;
    arrange(pieces, fixed, bed, params);
    for (const auto &piece : pieces) REQUIRE(piece.bed_idx == 0);
    check_inside_opening(pieces[2], fixed[0]);
    check_inside_opening(pieces[1], pieces[2]);
    check_inside_opening(pieces[0], pieces[1]);
}

TEST_CASE("Arrange results are scored by plates used and parts left out", "[Arrange][Sparrow]")
{
    ArrangePolygons sel{part(rectangle(0, 0, 10, 10), 0), part(rectangle(0, 0, 10, 10), 1), part(rectangle(0, 0, 10, 10), 2)};
    sel[0].bed_idx = 0;
    sel[1].bed_idx = 2;
    sel[2].bed_idx = -1;
    ArrangePolygon tower = part(rectangle(0, 0, 5, 5), 3);
    tower.is_virt_object = true;
    tower.bed_idx = 7;
    sel.push_back(tower);

    SECTION("virtual items and unplaced parts do not take a plate") {
        CHECK(count_plates_used(sel, {}, false) == 2);
        CHECK(count_unplaced(sel, false) == 1);
    }
    SECTION("a plate occupied by a fixed part counts") {
        ArrangePolygons fixed{part(rectangle(0, 0, 10, 10), 9)};
        fixed[0].bed_idx = 5;
        CHECK(count_plates_used(sel, fixed, false) == 3);
    }
    SECTION("a plate arrange counts parts sent off the plate") {
        CHECK(count_plates_used(sel, {}, true) == 2); // bed 2 and the unplaced one
        CHECK(count_unplaced(sel, true) == 0);
    }
}

TEST_CASE("Outline to hull ratio shows the room hull packing wastes", "[Arrange][Sparrow]")
{
    SECTION("a convex part wastes nothing") {
        ArrangePolygons items{part(rectangle(0, 0, 10, 10), 0)};
        double outline = 0.;
        CHECK(outline_hull_ratio(items, &outline) == Catch::Approx(1.));
        CHECK(outline == Catch::Approx(scale_(10.) * scale_(10.)));
    }
    SECTION("an L shape leaves a quarter of its hull empty") {
        ArrangePolygon l = part(Polygon({Point(0, 0), Point(scale_(20), 0), Point(scale_(20), scale_(10)),
                                         Point(scale_(10), scale_(10)), Point(scale_(10), scale_(20)), Point(0, scale_(20))}), 0);
        CHECK(outline_hull_ratio({l}) == Catch::Approx(300. / 350.).margin(1e-6));
    }
    SECTION("nothing to measure is neutral") {
        CHECK(outline_hull_ratio({}) == 1.);
    }
}
