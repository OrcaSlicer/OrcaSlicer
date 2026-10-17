#include <catch2/catch_all.hpp>
#include <array>
#include <algorithm>
#include <cmath>
#include <vector>
#include <limits>
#include <memory>
#include <optional>
#include "libslic3r/Fill/VoronoiPointCloud.hpp"
#include <utility>

#include "libslic3r/Fill/FillVoronoi.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/ShortestPath.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"

using namespace Slic3r;

namespace {
class FiniteWallSites : public Voronoi::PoissonPointCloud {
public:
    std::vector<Vec3d> points_in(const BoundingBoxf3 &box, double) const override
    {
        std::vector<Vec3d> result;
        for (double x : {-5., 5.}) {
            const Vec3d site(x, 0., 0.);
            if ((site.array() >= box.min.array()).all() && (site.array() < box.max.array()).all()) result.push_back(site);
        }
        return result;
    }
    std::optional<BoundingBoxf3> extent() const override
    {
        BoundingBoxf3 box;
        box.merge(Vec3d(-5., 0., 0.));
        box.merge(Vec3d(5., 0., 0.));
        return box;
    }
};
}

TEST_CASE("Voronoi schedules reversible chords and preserves fixed floor sweeps", "[FillVoronoi][Ordering]")
{
    const bool floors = GENERATE(false, true);
    std::vector<Voronoi::PathGroup> groups;
    std::vector<ExtrusionMultiPath> entities;
    for (int i = 0; i < 20; ++i) {
        const Point a(37 * i % 101, 53 * i % 103), b(61 * i % 107, 43 * i % 109);
        Polylines paths{Polyline(a, b)};
        const bool reversible = !floors || i % 3 != 0;
        if (!reversible) paths.push_back(Polyline(b + Point(0, 1), a + Point(0, 1)));
        groups.push_back({paths, reversible});
        ExtrusionMultiPath entity;
        for (const auto &line : paths) {
            ExtrusionPath path(erInternalInfill, 0.08, 0.45, 0.2);
            path.polyline = Polyline3(line);
            entity.paths.push_back(std::move(path));
        }
        entities.push_back(std::move(entity));
    }
    std::vector<ExtrusionEntity *> pointers;
    for (auto &entity : entities) pointers.push_back(&entity);
    const Point start(-10, -10);
    const auto expected = chain_extrusion_entities(pointers, &start);
    const auto actual = Voronoi::order_groups(groups, start);
    REQUIRE(actual.size() == expected.size());
    size_t reversed_floors = 0;
    for (size_t i = 0; i < actual.size(); ++i) {
        reversed_floors += expected[i].second && !groups[expected[i].first].reversible;
        auto paths = groups[expected[i].first].paths;
        if (expected[i].second && groups[expected[i].first].reversible) {
            std::reverse(paths.begin(), paths.end());
            for (auto &path : paths) path.reverse();
        }
        REQUIRE(actual[i].paths.size() == paths.size());
        for (size_t j = 0; j < paths.size(); ++j)
            REQUIRE(actual[i].paths[j].points == paths[j].points);
    }
    if (floors) REQUIRE(reversed_floors > 0);
}

TEST_CASE("Voronoi routes whole native floor zigzags from the previous path endpoint", "[FillVoronoi]")
{
    FillParams params;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    FillVoronoi filler;
    std::vector<Voronoi::Band> bands;
    for (double y : {100., 0., 50.})
        bands.push_back({ExPolygon{Point::new_scale(-2., y), Point::new_scale(0., y),
                                  Point::new_scale(0., y + 10.), Point::new_scale(-2., y + 10.)},
                         Vec2d(1., 0.), 0.});
    std::vector<Polylines> original;
    for (const auto &band : bands) original.push_back(filler.fill_band(band, params));
    const Point start = original[1].front().first_point();
    Voronoi::Layer layer;
    layer.bands = bands;
    const auto ordered = Voronoi::order_groups(filler.fill_paths(std::move(layer), params), start);
    REQUIRE(ordered.size() == original.size());
    for (size_t i = 0; i < ordered.size(); ++i) {
        const auto &expected = original[std::array<size_t, 3>{1, 2, 0}[i]];
        REQUIRE(ordered[i].paths.size() == expected.size());
        for (size_t j = 0; j < expected.size(); ++j)
            REQUIRE(ordered[i].paths[j].points == expected[j].points);
    }
    const auto travel = [&start](const std::vector<Polylines> &groups) {
        double distance = 0.;
        Point current = start;
        for (const auto &group : groups) {
            distance += (group.front().first_point() - current).cast<double>().norm();
            current = group.back().last_point();
        }
        return distance;
    };
    std::vector<Polylines> ordered_paths;
    for (const auto &group : ordered) ordered_paths.push_back(group.paths);
    REQUIRE(travel(ordered_paths) < travel(original));
}

TEST_CASE("Voronoi mixes native odd and even floor zigzags between wall trails", "[FillVoronoi]")
{
    const size_t rows = GENERATE(3, 4);
    FillParams params;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    const double width = rows * params.flow.spacing();
    Voronoi::Band band{ExPolygon{Point::new_scale(-width, -10.), Point::new_scale(0., -10.),
                                Point::new_scale(0., 10.), Point::new_scale(-width, 10.)}, Vec2d(1., 0.), 0.};
    FillVoronoi filler;
    const Polylines floor = filler.fill_band(band, params);
    REQUIRE(floor.size() == 1);
    const Point entry = floor.front().first_point(), exit = floor.back().last_point();
    REQUIRE((entry.y() == exit.y()) == (rows % 2 == 0));
    const Point start = entry + Point::new_scale(0., -50.);
    Voronoi::Layer layer;
    layer.bands.push_back(band);
    layer.walls = {Polyline(exit + Point::new_scale(0., 50.), exit), Polyline(start, entry)};
    const auto ordered = Voronoi::order_groups(filler.fill_paths(std::move(layer), params), start);
    REQUIRE(ordered.size() == 3);
    REQUIRE(ordered[0].reversible);
    REQUIRE_FALSE(ordered[1].reversible);
    REQUIRE(ordered[2].reversible);
    REQUIRE(ordered[0].paths.front().last_point() == entry);
    REQUIRE(ordered[1].paths.front().points == floor.front().points);
    REQUIRE(ordered[2].paths.front().first_point() == exit);
}

TEST_CASE("Voronoi fills the full width of narrow floor bands", "[FillVoronoi]")
{
    const double width = GENERATE(0.46, 0.6, 1., 2.2);
    const double height = GENERATE(0.12, 0.2, 0.3);
    FillParams params;
    params.flow = Flow(0.45f, float(height), 0.4f);
    const double rounded_width = std::ceil(width / params.flow.spacing()) * params.flow.spacing();
    const double supported_edge = width - 0.4;
    Voronoi::Band band{ExPolygon{Point::new_scale(supported_edge - rounded_width, -10.), Point::new_scale(supported_edge, -10.),
                                Point::new_scale(supported_edge, 10.), Point::new_scale(supported_edge - rounded_width, 10.)},
                       Vec2d(1., 0.), supported_edge};
    FillVoronoi filler;
    filler.set_bounding_box(BoundingBox(Point::new_scale(-20., -20.), Point::new_scale(20., 20.)));
    auto filled = filler.fill_band(band, params);
    REQUIRE_FALSE(filled.empty());
    REQUIRE(filled.size() == 1);
    REQUIRE(diff_pl(filled, band.polygon).empty());
    const Polyline &zigzag = filled.front();
    double previous_x = std::numeric_limits<double>::max();
    double previous_direction = 0.;
    size_t row_count = 0;
    Polyline first_row;
    for (size_t i = 1; i < zigzag.points.size(); ++i) {
        const Point &a = zigzag.points[i - 1], &b = zigzag.points[i];
        const double direction = unscale_(b.y() - a.y());
        if (std::abs(direction) < 19.)
            continue;
        REQUIRE(a.x() <= previous_x);
        if (row_count > 0) {
            REQUIRE_THAT(unscale_(previous_x - a.x()), Catch::Matchers::WithinAbs(params.flow.spacing(), 1e-5));
            REQUIRE(direction * previous_direction < 0.);
        } else {
            first_row = Polyline(a, b);
        }
        previous_x = a.x();
        previous_direction = direction;
        ++row_count;
    }
    REQUIRE(row_count == size_t(std::ceil(width / params.flow.spacing())));
    Voronoi::Band previous = band;
    previous.polygon.translate(Point::new_scale(width / 2., 0.));
    previous.row_origin += width / 2.;
    auto previous_fill = filler.fill_band(previous, params);
    ExtrusionEntityCollection prior_extrusion;
    extrusion_entities_append_paths(prior_extrusion.entities, std::move(previous_fill), erInternalInfill,
                                    params.flow.mm3_per_mm(), params.flow.width(), params.flow.height());
    ExtrusionPath first(erInternalInfill, params.flow.mm3_per_mm(), params.flow.width(), params.flow.height());
    first.polyline = Polyline3(first_row);
    const ExPolygons unsupported = diff_ex(first.polygons_covered_by_width(), prior_extrusion.polygons_covered_by_width());
    double unsupported_area = 0.;
    for (const auto &polygon : unsupported)
        unsupported_area += polygon.area();
    REQUIRE(unsupported_area / band.polygon.area() < 0.001);
    ExtrusionEntityCollection extrusion;
    extrusion_entities_append_paths(extrusion.entities, std::move(filled), erInternalInfill,
                                    params.flow.mm3_per_mm(), params.flow.width(), params.flow.height());
    const ExPolygons uncovered = diff_ex(ExPolygons{band.polygon}, extrusion.polygons_covered_by_width());
    double area = 0.;
    for (const auto &polygon : uncovered)
        area += polygon.area();
    REQUIRE(area / band.polygon.area() < 0.001);
}

TEST_CASE("Voronoi floor zigzags remain within bands containing holes", "[FillVoronoi]")
{
    FillParams params;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    const double pitch = params.flow.spacing();
    Voronoi::Band band{ExPolygon{Point::new_scale(-8. * pitch, -10.), Point::new_scale(0., -10.),
                                Point::new_scale(0., 10.), Point::new_scale(-8. * pitch, 10.)}, Vec2d(1., 0.), 0.};
    band.polygon.holes.emplace_back(Points{Point::new_scale(-6. * pitch, -2.), Point::new_scale(-6. * pitch, 2.),
                                           Point::new_scale(-2. * pitch, 2.), Point::new_scale(-2. * pitch, -2.)});
    FillVoronoi filler;
    const Polylines paths = filler.fill_band(band, params);
    REQUIRE_FALSE(paths.empty());
    REQUIRE(diff_pl(paths, band.polygon).empty());
    bool has_join = false;
    for (const Polyline &path : paths) {
        REQUIRE(path.first_point().x() >= path.last_point().x());
        if (path.points.size() > 3)
            has_join = true;
    }
    REQUIRE(has_join);
}

TEST_CASE("Voronoi fills shallow bands with continuous coverage and configured flow", "[FillVoronoi]")
{
    const double height = GENERATE(0.12, 0.2, 0.3);
    ExPolygon region{Point::new_scale(0., 0.), Point::new_scale(80., 0.),
                     Point::new_scale(80., 80.), Point::new_scale(0., 80.)};
    region.holes.emplace_back(Points{Point::new_scale(30., 30.), Point::new_scale(30., 50.),
                                    Point::new_scale(50., 50.), Point::new_scale(50., 30.)});
    FillParams params;
    params.density = 0.2f;
    params.pattern = ipVoronoi;
    params.flow = Flow(0.45f, float(height), 0.4f);
    params.extrusion_role = erInternalInfill;
    params.using_internal_flow = true;
    FillVoronoi filler;
    filler.set_bounding_box(region.contour.bounding_box());
    filler.layer_id = 30;
    filler.z = 6.;
    filler.angle = 0.f;
    filler.spacing = params.flow.spacing();
    Surface surface(stInternal, region);
    const Polylines paths = filler.fill_surface(&surface, params);
    REQUIRE_FALSE(paths.empty());
    REQUIRE(diff_pl(paths, region).empty());

    // Compare the deposited paths with the geometric bands. The real adapter
    // must fill their interiors, rather than merely tracing polygon edges.
    VoronoiInfill geometry;
    const ExPolygons inset = offset_ex(region, -0.5f * float(scale_(filler.spacing)));
    REQUIRE(inset.size() == 1);
    const auto layer = geometry.fill_geometry(filler.bounding_box, inset.front(), filler.spacing, params.density, filler.z, params.flow.width(), params.flow.height(), height, params.flow.spacing());
    REQUIRE_FALSE(layer.bands.empty());
    ExtrusionEntityCollection output;
    filler.fill_surface_extrusion(&surface, params, output.entities);
    REQUIRE(output.entities.size() == 1);
    const auto *ordered = dynamic_cast<const ExtrusionEntityCollection *>(output.entities.front());
    REQUIRE(ordered != nullptr);
    REQUIRE(ordered->no_sort);
    REQUIRE_FALSE(ordered->can_reverse());
    const Polygons deposited = output.polygons_covered_by_width();
    double band_area = 0., uncovered_area = 0.;
    for (const auto &band : layer.bands) {
        band_area += band.polygon.area();
        for (const ExPolygon &uncovered : diff_ex(ExPolygons{band.polygon}, deposited))
            uncovered_area += uncovered.area();
        // The long rows and their short contour joins both belong to this
        // floor; joins may run in a different direction from the rows.
        // Allow fixed-point rounding of vertices on diagonal clip boundaries.
        REQUIRE(diff_pl(filler.fill_band(band, params), offset_ex(band.polygon, float(SCALED_EPSILON))).empty());
    }
    // Clipped tapered tips can be narrower than a row; the rectangular
    // regression above requires complete coverage across the floor width.
    REQUIRE(uncovered_area / band_area < 0.02);
    const double deposited_volume = output.total_volume();
    const double region_volume = region.area() * SCALING_FACTOR * SCALING_FACTOR * height;
    REQUIRE_THAT(deposited_volume / region_volume, Catch::Matchers::WithinRel(double(params.density), 0.2));

    // Rows keep their order but may be printed from either end.
    const auto flattened = output.flatten();
    REQUIRE_FALSE(flattened.entities.empty());
    for (const ExtrusionEntity *entity : flattened.entities) {
        REQUIRE(entity->can_reverse());
        const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
        REQUIRE(path != nullptr);
        REQUIRE_THAT(double(path->width), Catch::Matchers::WithinAbs(double(params.flow.width()), 1e-6));
        REQUIRE_THAT(path->mm3_per_mm, Catch::Matchers::WithinAbs(params.flow.mm3_per_mm(), 1e-9));
    }
}

TEST_CASE("Voronoi uses native multiline outlines instead of cell loops", "[FillVoronoi][Multiline]")
{
    const int count = GENERATE(1, 2, 3, 4);
    FillParams params;
    params.multiline = count;
    params.density = 0.2f;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    params.using_internal_flow = true;
    params.anchor_length_max = 0.f;
    const ExPolygon region {Point::new_scale(-10., -10.), Point::new_scale(10., -10.),
                            Point::new_scale(10., 10.), Point::new_scale(-10., 10.)};
    FillVoronoi filler;
    filler.set_point_cloud(std::make_shared<FiniteWallSites>());
    filler.set_bounding_box(region.contour.bounding_box());
    filler.spacing = params.flow.spacing();
    filler.z = 0.;
    filler.angle = 0.f;
    const Surface surface(stInternal, region);
    auto expected = FillVoronoi();
    expected.set_point_cloud(std::make_shared<FiniteWallSites>());
    expected.set_bounding_box(region.contour.bounding_box());
    expected.spacing = filler.spacing;
    expected.z = 0.;
    expected.angle = 0.f;
    FillParams single = params;
    single.multiline = 1;
    Polylines native = expected.fill_surface(&surface, single);
    multiline_fill(native, params, float(params.flow.spacing()));
    const auto clip = offset_ex(region, -float(scale_(0.5 * filler.spacing)));
    native = intersection_pl(std::move(native), clip);
    const auto actual = filler.fill_surface(&surface, params);
    Polygons actual_area = offset(actual, float(scale_(0.5 * params.flow.width())), jtRound, 0.01, etOpenRound);
    Polygons native_area = offset(native, float(scale_(0.5 * params.flow.width())), jtRound, 0.01, etOpenRound);
    REQUIRE(diff(actual_area, native_area).empty());
    REQUIRE(diff(native_area, actual_area).empty());
}

TEST_CASE("Voronoi honours native boundary anchor lengths", "[FillVoronoi][Anchors]")
{
    const ExPolygon region {Point::new_scale(-10., -10.), Point::new_scale(10., -10.),
                            Point::new_scale(10., 10.), Point::new_scale(-10., 10.)};
    FillParams params;
    params.density = 0.2f;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    FillVoronoi filler;
    filler.set_point_cloud(std::make_shared<FiniteWallSites>());
    filler.set_bounding_box(region.contour.bounding_box());
    filler.spacing = params.flow.spacing();
    filler.z = 0.;
    filler.angle = 0.f;
    const Surface surface(stInternal, region);
    params.anchor_length_max = 0.f;
    const auto unanchored = filler.fill_surface(&surface, params);
    params.anchor_length = params.anchor_length_max = 2.f;
    const auto anchored = filler.fill_surface(&surface, params);
    double original_length = 0., anchored_length = 0.;
    for (const auto &path : unanchored) original_length += path.length();
    for (const auto &path : anchored) anchored_length += path.length();
    REQUIRE(anchored_length > original_length);
    REQUIRE(diff_pl(anchored, offset_ex(region, float(SCALED_EPSILON))).empty());
}

TEST_CASE("Floor rows use existing wall anchors without reversing their sweep", "[FillVoronoi][Floors][Anchors]")
{
    const bool both_ends = GENERATE(false, true);
    FillParams params;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    const ExPolygon floor {Point::new_scale(-2.5, -10.), Point::new_scale(0., -10.),
                           Point::new_scale(0., 10.), Point::new_scale(-2.5, 10.)};
    ExPolygons support {
        ExPolygon{Point::new_scale(-3., -10.8), Point::new_scale(1., -10.8), Point::new_scale(1., -9.99), Point::new_scale(-3., -9.99)}};
    if (both_ends) support.push_back(ExPolygon{Point::new_scale(-3., 9.99), Point::new_scale(1., 9.99),
                                              Point::new_scale(1., 10.8), Point::new_scale(-3., 10.8)});
    FillVoronoi filler;
    filler.set_point_cloud(std::make_shared<FiniteWallSites>());
    filler.set_bounding_box(floor.contour.bounding_box());
    filler.spacing = params.flow.spacing();
    filler.z = 0.;
    filler.angle = 0.f;
    params.density = 0.2f;
    params.anchor_length_max = 0.f;
    const Surface surface(stInternal, floor);
    filler.fill_surface(&surface, params);
    filler.set_floor_support(support);
    const auto paths = filler.fill_band({floor, Vec2d(1., 0.), 0.}, params);
    REQUIRE_FALSE(paths.empty());
    bool has_rows = false;
    for (const auto &path : paths) {
        REQUIRE(path.first_point().x() >= path.last_point().x());
        for (size_t i = 1; i < path.points.size(); ++i)
            if (std::abs(path.points[i].y() - path.points[i - 1].y()) > scale_(10.)) {
                for (const Point &p : {path.points[i - 1], path.points[i]}) {
                    REQUIRE(expolygons_contain(support, p) == (both_ends || p.y() < 0));
                    if (!both_ends && p.y() > 0) REQUIRE(p.y() <= scale_(10.));
                }
                has_rows = true;
            }
    }
    REQUIRE(has_rows);
}
