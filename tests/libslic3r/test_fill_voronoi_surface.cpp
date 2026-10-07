#include <catch2/catch_all.hpp>
#include <array>
#include <limits>
#include <memory>
#include <utility>

#include "libslic3r/Fill/FillVoronoi.hpp"
#include "libslic3r/Fill/VoronoiWallDistance.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Surface.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"

using namespace Slic3r;

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
    const auto ordered = filler.fill_paths(std::move(layer), params, start);
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
    const auto ordered = filler.fill_paths(std::move(layer), params, start);
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
    const auto layer = geometry.fill_layer(filler.bounding_box, inset.front(), filler.spacing,
                                          params.density, filler.z, params.flow.width(), params.flow.height(), height, params.flow.spacing());
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

TEST_CASE("Voronoi wall distribution reduces material through native extrusion", "[FillVoronoi]")
{
    const ExPolygons outlines {ExPolygon{Point::new_scale(-40., -40.), Point::new_scale(40., -40.),
                                        Point::new_scale(40., 40.), Point::new_scale(-40., 40.)}};
    auto field = std::make_shared<Voronoi::CubicWallDistance>(std::vector<Voronoi::WallSlice>{{-100., 100., &outlines}});
    FillVoronoi uniform, biased;
    const Voronoi::WallDistanceSamples distance = [field](const auto &points) { return field->sample_fields(points); };
    std::shared_ptr<const Voronoi::PointCloudProvider> cloud = std::make_shared<Voronoi::WallDistancePointCloud>(distance);
    biased.set_point_cloud(std::move(cloud));
    FillParams params;
    params.density = 0.2f;
    params.flow = Flow(0.45f, 0.2f, 0.4f);
    params.using_internal_flow = true;
    for (auto *filler : {&uniform, &biased}) {
        filler->set_bounding_box(outlines.front().contour.bounding_box());
        filler->spacing = params.flow.spacing();
        filler->angle = 0.f;
        filler->z = 4.;
        filler->layer_id = 20;
    }
    Surface surface(stInternal, outlines.front());
    ExtrusionEntityCollection original, thinned, repeat;
    uniform.fill_surface_extrusion(&surface, params, original.entities);
    biased.fill_surface_extrusion(&surface, params, thinned.entities);
    REQUIRE_FALSE(thinned.entities.empty());
    REQUIRE(thinned.total_volume() > 0.);
    REQUIRE(thinned.total_volume() < original.total_volume());
    biased.fill_surface_extrusion(&surface, params, repeat.entities);
    REQUIRE_THAT(repeat.total_volume(), Catch::Matchers::WithinAbs(thinned.total_volume(), EPSILON));
}
