#include <catch2/catch_all.hpp>

#include "libslic3r/Fill/VoronoiInfill.hpp"
#include "libslic3r/Fill/VoronoiRouting.hpp"
#include "libslic3r/Fill/VoronoiWallDistance.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <vector>
#include <stdexcept>

using namespace Slic3r;

namespace {

ExPolygon rectangle(double x0, double y0, double x1, double y1)
{
    return ExPolygon{Point::new_scale(x0, y0), Point::new_scale(x1, y0),
                     Point::new_scale(x1, y1), Point::new_scale(x0, y1)};
}

double path_length(const Polylines &paths)
{
    double length = 0.;
    for (const Polyline &path : paths)
        for (size_t i = 1; i < path.points.size(); ++i)
            length += (path.points[i].cast<double>() - path.points[i - 1].cast<double>()).norm();
    return length;
}

using Segments = std::set<std::pair<Point, Point>>;

Segments segments(const Polylines &paths)
{
    Segments result;
    for (const Polyline &path : paths) {
        REQUIRE(path.points.size() >= 2);
        for (size_t i = 1; i < path.points.size(); ++i) {
            Point a = path.points[i - 1], b = path.points[i];
            REQUIRE(a != b);
            if (b < a)
                std::swap(a, b);
            REQUIRE(result.emplace(a, b).second);
        }
    }
    return result;
}

class FixedProvider : public Voronoi::PoissonPointCloud
{
public:
    std::vector<Vec3d> sites;
    std::vector<Vec3d> points_in(const BoundingBoxf3 &bounds, double) const override
    {
        std::vector<Vec3d> result;
        for (const Vec3d &site : sites)
            if ((site.array() >= bounds.min.array()).all() && (site.array() < bounds.max.array()).all())
                result.push_back(site);
        return result;
    }
    std::optional<BoundingBoxf3> extent() const override
    {
        BoundingBoxf3 box;
        for (const Vec3d &site : sites) box.merge(site);
        return box;
    }
};

class FixedSites : public VoronoiInfill
{
public:
    FixedSites() : FixedSites(std::make_shared<FixedProvider>()) {}
    std::vector<Vec3d> &sites;
private:
    explicit FixedSites(std::shared_ptr<FixedProvider> provider) : VoronoiInfill(provider), sites(provider->sites) {}
};
using UniformSites = Voronoi::PoissonPointCloud;

} // namespace

TEST_CASE("Voronoi infill uses the requested material density", "[FillVoronoi]")
{
    const double density = GENERATE(0.05, 0.10, 0.20, 0.40);
    const double z = GENERATE(0., 13.);
    const double spacing = 0.45;
    const ExPolygon region = rectangle(0., 0., 200., 200.);
    VoronoiInfill infill;
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, spacing, density, z);
    REQUIRE_FALSE(paths.empty());
    const double actual = path_length(paths) * scale_(spacing) / region.area();
    REQUIRE_THAT(actual, Catch::Matchers::WithinRel(density, 0.10));
    segments(paths); // Every shared wall is printed once.
}

TEST_CASE("Voronoi faces move with height according to three dimensional distance", "[FillVoronoi]")
{
    const double z = GENERATE(0., 10., 20.);
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-10., 0., 0.), Vec3d(10., 0., 20.)};
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, z);
    REQUIRE(paths.size() == 1);
    // The 3D bisector is x+z=10. Its section moves left as the layer rises.
    REQUIRE_THAT(unscale_(paths.front().first_point().x()), Catch::Matchers::WithinAbs(10. - z, EPSILON));
    REQUIRE_THAT(unscale_(paths.front().last_point().x()), Catch::Matchers::WithinAbs(10. - z, EPSILON));
}

TEST_CASE("Voronoi cells appear and disappear as the slice passes their height", "[FillVoronoi]")
{
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-10., 0., 0.), Vec3d(10., 0., 0.), Vec3d(0., 0., 20.)};
    const BoundingBox bounds = region.contour.bounding_box();
    REQUIRE(infill.fill(bounds, region, 0.45, 0.2, 0.).size() == 1);
    const Polylines middle = infill.fill(bounds, region, 0.45, 0.2, 10.);
    REQUIRE(middle.size() == 2);
    for (const Polyline &path : middle)
        REQUIRE_THAT(std::abs(unscale_(path.first_point().x())), Catch::Matchers::WithinAbs(5., EPSILON));
    REQUIRE(infill.fill(bounds, region, 0.45, 0.2, 20.).empty());
}

TEST_CASE("Voronoi slice walls separate the nearest three dimensional sites", "[FillVoronoi]")
{
    const double z = GENERATE(-3., 4., 12., 20.);
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-12., -8., -6.), Vec3d(12., -8., 2.), Vec3d(0., 14., 10.), Vec3d(-2., -1., 25.)};
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, z);
    REQUIRE_FALSE(paths.empty());
    segments(paths);
    for (const Polyline &path : paths) {
        for (size_t i = 1; i < path.points.size(); ++i) {
            const Vec2d xy = (path.points[i - 1].cast<double>() + path.points[i].cast<double>()) * (0.5 * SCALING_FACTOR);
            const Vec3d midpoint(xy.x(), xy.y(), z);
            std::vector<double> distances;
            for (const Vec3d &site : infill.sites)
                distances.push_back((site - midpoint).squaredNorm());
            std::sort(distances.begin(), distances.end());
            REQUIRE_THAT(distances[1], Catch::Matchers::WithinAbs(distances[0], EPSILON));
        }
    }
}

TEST_CASE("Voronoi layers change while a moving point band remains reproducible", "[FillVoronoi]")
{
    class CountingProvider : public Voronoi::PoissonPointCloud {
    public:
        mutable std::vector<BoundingBoxf3> queries;
        std::vector<Vec3d> points_in(const BoundingBoxf3 &bounds, double spacing) const override
        {
            queries.push_back(bounds);
            return PoissonPointCloud::points_in(bounds, spacing);
        }
    };
    const ExPolygon region = rectangle(-50., -50., 50., 50.);
    const BoundingBox bounds = region.contour.bounding_box();
    auto provider = std::make_shared<CountingProvider>();
    VoronoiInfill infill(provider);
    const Segments first = segments(infill.fill(bounds, region, 0.45, 0.15, 0.1));
    REQUIRE_FALSE(provider->queries.empty());
    REQUIRE(first != segments(infill.fill(bounds, region, 0.45, 0.15, 0.2)));
    infill.fill(bounds, region, 0.45, 0.15, 1000.);
    // Jumping past the active range starts locally, without ingesting the gap.
    REQUIRE(provider->queries.back().min.z() > 900.);
    REQUIRE(first == segments(infill.fill(bounds, region, 0.45, 0.15, 0.1)));
}

TEST_CASE("Voronoi infill repeats at the same height across independently filled regions", "[FillVoronoi]")
{
    const ExPolygon whole = rectangle(-50., -50., 50., 50.);
    const ExPolygon part = rectangle(-20., -10., 30., 40.);
    const BoundingBox bounds = whole.contour.bounding_box();
    VoronoiInfill first, second;
    const Polylines full = first.fill(bounds, whole, 0.45, 0.15, 0.);
    const Polylines partial = second.fill(bounds, part, 0.45, 0.15, 0.);
    REQUIRE_FALSE(full.empty());
    REQUIRE(segments(full) == segments(second.fill(bounds, whole, 0.45, 0.15, 0.)));
    REQUIRE(segments(intersection_pl(full, part)) == segments(partial));
    // Changing a cached density and changing it back must not change the pattern.
    first.fill(bounds, whole, 0.45, 0.30, 0.);
    REQUIRE(segments(full) == segments(first.fill(bounds, whole, 0.45, 0.15, 0.)));
}

TEST_CASE("Voronoi infill stays inside concave surfaces and outside holes", "[FillVoronoi]")
{
    ExPolygon region{Point::new_scale(0., 0.), Point::new_scale(80., 0.),
                     Point::new_scale(80., 30.), Point::new_scale(30., 30.),
                     Point::new_scale(30., 80.), Point::new_scale(0., 80.)};
    Polygon hole = rectangle(5., 5., 20., 20.).contour;
    hole.reverse();
    region.holes.push_back(std::move(hole));
    VoronoiInfill infill;
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, 0.45, 0.20, 0.);
    REQUIRE_FALSE(paths.empty());
    REQUIRE(path_length(diff_pl(paths, region)) < SCALED_EPSILON);
    segments(paths);
}

TEST_CASE("Voronoi infill handles empty and invalid inputs", "[FillVoronoi]")
{
    const double density = GENERATE(0., -0.1, std::numeric_limits<double>::denorm_min(),
                                    std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN());
    const ExPolygon region = rectangle(0., 0., 10., 10.);
    VoronoiInfill infill;
    REQUIRE(infill.fill(region.contour.bounding_box(), region, 0.45, density, 0.).empty());
    REQUIRE(infill.fill(region.contour.bounding_box(), region, 0., 0.2, 0.).empty());
    REQUIRE(infill.fill(region.contour.bounding_box(), region, std::numeric_limits<double>::denorm_min(), 0.2, 0.).empty());
    REQUIRE(infill.fill(region.contour.bounding_box(), ExPolygon{}, 0.45, 0.2, 0.).empty());
}

TEST_CASE("Voronoi bisectors reach the boundary for collinear sites", "[FillVoronoi]")
{
    const size_t count = GENERATE(size_t(2), size_t(3));
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-10., 0., 0.), Vec3d(10., 0., 0.)};
    if (count == 3)
        infill.sites.push_back(Vec3d(0., 0., 0.));
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, 0.);
    REQUIRE(paths.size() == count - 1);
    REQUIRE_THAT(path_length(paths), Catch::Matchers::WithinAbs(scale_(40. * (count - 1)), SCALED_EPSILON));
    for (const Polyline &path : paths) {
        REQUIRE(path.first_point().x() == path.last_point().x());
        REQUIRE(std::abs(path.first_point().y()) == scale_(20.));
        REQUIRE(std::abs(path.last_point().y()) == scale_(20.));
    }
}

TEST_CASE("Voronoi rays and cocircular sites produce unique connected walls", "[FillVoronoi]")
{
    const size_t count = GENERATE(size_t(3), size_t(4));
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-10., -10., 0.), Vec3d(10., -10., 0.), Vec3d(10., 10., 0.)};
    if (count == 4)
        infill.sites.push_back(Vec3d(-10., 10., 0.));
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, 0.);
    REQUIRE(segments(paths).size() == count);
    REQUIRE(path_length(diff_pl(paths, region)) < SCALED_EPSILON);
    // Each wall separates its two nearest sites, including the infinite rays.
    for (const Polyline &path : paths) {
        for (size_t i = 1; i < path.points.size(); ++i) {
            const Vec2d xy = (path.points[i - 1].cast<double>() + path.points[i].cast<double>()) * (0.5 * SCALING_FACTOR);
            const Vec3d midpoint(xy.x(), xy.y(), 0.);
            std::vector<double> distances;
            for (const Vec3d &site : infill.sites)
                distances.push_back((site - midpoint).squaredNorm());
            std::sort(distances.begin(), distances.end());
            REQUIRE_THAT(distances[1], Catch::Matchers::WithinAbs(distances[0], EPSILON));
        }
    }
}

TEST_CASE("Voronoi infill accepts duplicate sites and large translated coordinates", "[FillVoronoi]")
{
    const double translation = GENERATE(-5000., 0., 5000.);
    const double separation = GENERATE(10., 10000.);
    const ExPolygon region = rectangle(translation - 20., -20., translation + 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(translation - separation, 0., 0.), Vec3d(translation + separation, 0., 0.),
                   Vec3d(translation - separation, 0., 0.)};
    const Polylines paths = infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, 0.);
    REQUIRE(paths.size() == 1);
    REQUIRE(paths.front().first_point().x() == scale_(translation));
    REQUIRE(paths.front().last_point().x() == scale_(translation));
    REQUIRE_THAT(path_length(paths), Catch::Matchers::WithinAbs(scale_(40.), SCALED_EPSILON));
}

TEST_CASE("Voronoi infill produces no walls with fewer than two distinct sites", "[FillVoronoi]")
{
    const size_t count = GENERATE(size_t(0), size_t(1), size_t(2));
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites.assign(count, Vec3d(0., 0., 0.));
    REQUIRE(infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, 0.).empty());
}

TEST_CASE("Voronoi sites are uniformly distributed in three dimensions", "[FillVoronoi]")
{
    const BoundingBoxf3 bounds(Vec3d(-100., -100., -100.), Vec3d(100., 100., 100.));
    UniformSites sampler;
    const std::vector<Vec3d> sites = sampler.points_in(bounds, 10.);
    REQUIRE_THAT(double(sites.size()), Catch::Matchers::WithinRel(8000., 0.05));
    std::array<size_t, 64> bins{};
    for (const Vec3d &p : sites) {
        REQUIRE(bounds.contains(p));
        const size_t x = size_t((p.x() + 100.) / 50.);
        const size_t y = size_t((p.y() + 100.) / 50.);
        const size_t z = size_t((p.z() + 100.) / 50.);
        ++bins[z * 16 + y * 4 + x];
    }
    double chi_squared = 0.;
    const double expected = double(sites.size()) / bins.size();
    for (size_t bin : bins)
        chi_squared += std::pow(double(bin) - expected, 2.) / expected;
    REQUIRE(chi_squared < 110.); // 64 uniform bins, 63 degrees of freedom.
}

TEST_CASE("Voronoi shallow face bands overlap adjacent layers by half", "[FillVoronoi]")
{
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    const BoundingBox bounds = region.contour.bounding_box();
    FixedSites infill;
    // Bisector x+5*z=25 moves one millimeter per 0.2 mm layer.
    infill.sites = {Vec3d(-1., 0., 0.), Vec3d(1., 0., 10.)};
    const auto below = infill.fill_layer(bounds, region, 0.4, 0.2, 4.8, 0.45, 0.2);
    const auto current = infill.fill_layer(bounds, region, 0.4, 0.2, 5., 0.45, 0.2);
    const auto above = infill.fill_layer(bounds, region, 0.4, 0.2, 5.2, 0.45, 0.2);
    REQUIRE(current.walls.empty());
    REQUIRE(current.bands.size() == 1);
    REQUIRE(below.bands.size() == 1);
    REQUIRE(above.bands.size() == 1);
    const ExPolygons polygon {current.bands.front().polygon};
    const double area = current.bands.front().polygon.area();
    for (const auto *neighbor : {&below, &above}) {
        const ExPolygons overlap = intersection_ex(polygon, ExPolygons{neighbor->bands.front().polygon});
        REQUIRE(overlap.size() == 1);
        REQUIRE_THAT(overlap.front().area() / area, Catch::Matchers::WithinAbs(0.5, 1e-6));
    }
    REQUIRE(current.bands.front().supported_side.x() > 0.);
}

TEST_CASE("Voronoi rounds shallow floors outward to whole rows with at least half layer overlap", "[FillVoronoi]")
{
    const double shift = GENERATE(0.23, 0.3, 0.5, 1.07);
    const double offset = GENERATE(0., 37., -63.);
    const double height = 0.2, spacing = 0.4070796327;
    const ExPolygon region = rectangle(offset - 20., -20., offset + 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(offset - 1., 0., 5. - shift / height), Vec3d(offset + 1., 0., 5. + shift / height)};
    const auto bounds = region.contour.bounding_box();
    const auto below = infill.fill_layer(bounds, region, spacing, 0.2, 5. - height, 0.45, height);
    const auto current = infill.fill_layer(bounds, region, spacing, 0.2, 5., 0.45, height);
    const auto above = infill.fill_layer(bounds, region, spacing, 0.2, 5. + height, 0.45, height);
    REQUIRE(current.bands.size() == 1);
    const auto &band = current.bands.front();
    const double rounded_width = std::ceil(2. * shift / spacing) * spacing;
    REQUIRE_THAT(unscale_(band.polygon.contour.bounding_box().max.x()), Catch::Matchers::WithinAbs(offset + shift, 1e-5));
    REQUIRE_THAT(unscale_(band.polygon.contour.bounding_box().min.x()),
                 Catch::Matchers::WithinAbs(offset + shift - rounded_width, 1e-5));
    REQUIRE_THAT(band.row_origin, Catch::Matchers::WithinAbs(offset + shift, 1e-9));
    for (const auto *neighbor : {&below, &above}) {
        REQUIRE(neighbor->bands.size() == 1);
        const ExPolygons overlap = intersection_ex(ExPolygons{band.polygon}, ExPolygons{neighbor->bands.front().polygon});
        REQUIRE(overlap.size() == 1);
        REQUIRE(overlap.front().area() / band.polygon.area() >= 0.5 - 1e-6);
    }
}

TEST_CASE("Voronoi faces retain a single line when bead overlap is sufficient", "[FillVoronoi]")
{
    const double width = GENERATE(0.45, 0.8);
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-10., 0., 0.), Vec3d(10., 0., 20.)};
    const auto layer = infill.fill_layer(region.contour.bounding_box(), region, 0.4, 0.2, 10., width, 0.2);
    REQUIRE(layer.bands.empty());
    REQUIRE(layer.walls.size() == 1);
    // At the same height a narrower bead needs a band instead; cached geometry
    // must include extrusion dimensions, not just the layer Z coordinate.
    const auto narrow = infill.fill_layer(region.contour.bounding_box(), region, 0.2, 0.2, 10., 0.3, 0.2);
    REQUIRE(narrow.walls.empty());
    REQUIRE(narrow.bands.size() == 1);
}

TEST_CASE("Voronoi horizontal faces fill once and stop at neighboring cells", "[FillVoronoi]")
{
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    const BoundingBox bounds = region.contour.bounding_box();
    FixedSites infill;
    infill.sites = {Vec3d(0., 0., 0.), Vec3d(0., 0., 1.), Vec3d(4., 0., 0.5),
                    Vec3d(-4., 0., 0.5), Vec3d(0., 4., 0.5), Vec3d(0., -4., 0.5)};
    REQUIRE(infill.fill_layer(bounds, region, 0.4, 0.2, 0.25, 0.45, 0.25).bands.empty());
    REQUIRE(infill.fill_layer(bounds, region, 0.4, 0.2, 0.75, 0.45, 0.25).bands.empty());
    const auto floor = infill.fill_layer(bounds, region, 0.4, 0.2, 0.5, 0.45, 0.25);
    REQUIRE(floor.bands.size() == 1);
    REQUIRE_THAT(floor.bands.front().polygon.area() / (scale_(1.) * scale_(1.)),
                 Catch::Matchers::WithinAbs(3.9375 * 3.9375, 1e-5));
}

TEST_CASE("Voronoi bands respect holes and do not duplicate walls or polygon areas", "[FillVoronoi]")
{
    ExPolygon region = rectangle(-20., -20., 20., 20.);
    Polygon hole = rectangle(-2., -2., 2., 2.).contour;
    hole.reverse();
    region.holes.push_back(hole);
    FixedSites infill;
    infill.sites = {Vec3d(-1., 0., -10.), Vec3d(1., 0., 10.), Vec3d(0., -1., -10.), Vec3d(0., 1., 10.)};
    const auto layer = infill.fill_layer(region.contour.bounding_box(), region, 0.4, 0.2, 0., 0.45, 0.2);
    REQUIRE_FALSE(layer.bands.empty());
    ExPolygons covered;
    for (const auto &band : layer.bands) {
        REQUIRE(diff_ex(ExPolygons{band.polygon}, ExPolygons{region}).empty());
        REQUIRE(intersection_ex(ExPolygons{band.polygon}, covered).empty());
        covered.push_back(band.polygon);
    }
    // Supporting walls on floor boundaries remain printable. Only band
    // interiors beyond the fixed-point cleanup tolerance exclude walls.
    ExPolygons interiors;
    for (const auto &band : layer.bands)
        append(interiors, offset_ex(band.polygon, -float(scale_(0.00001))));
    REQUIRE(intersection_pl(layer.walls, interiors).empty());
}

TEST_CASE("Voronoi shallow face compensation preserves statistical material density", "[FillVoronoi]")
{
    const double height = GENERATE(0.12, 0.2, 0.3);
    const double density = GENERATE(0.1, 0.2);
    const ExPolygon region = rectangle(0., 0., 100., 100.);
    VoronoiInfill infill;
    const auto layer = infill.fill_layer(region.contour.bounding_box(), region, 0.4, density, 5., 0.45, height);
    double material = path_length(layer.walls) * scale_(0.4);
    for (const auto &band : layer.bands)
        material += band.polygon.area();
    REQUIRE_FALSE(layer.bands.empty());
    REQUIRE_THAT(material / region.area(), Catch::Matchers::WithinRel(density, 0.15));
}

TEST_CASE("Voronoi site placement stays fixed when extrusion dimensions change", "[FillVoronoi]")
{
    class RecordingProvider : public Voronoi::PoissonPointCloud {
    public:
        mutable std::vector<double> spacings;
        std::vector<Vec3d> points_in(const BoundingBoxf3 &bounds, double spacing) const override
        {
            spacings.push_back(spacing);
            return Voronoi::PoissonPointCloud::points_in(bounds, spacing);
        }
    };
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    auto provider = std::make_shared<RecordingProvider>();
    VoronoiInfill infill(provider);
    const BoundingBox bounds = region.contour.bounding_box();
    infill.fill_layer(bounds, region, 0.4, 0.2, 0.3, 0.6, 0.3, 0.2);
    REQUIRE_FALSE(provider->spacings.empty());
    const double initial_spacing = provider->spacings.front();
    infill.fill_layer(bounds, region, 0.4, 0.2, 20., 0.45, 0.12, 0.2);
    for (double spacing : provider->spacings)
        REQUIRE_THAT(spacing, Catch::Matchers::WithinAbs(initial_spacing, EPSILON));
}

TEST_CASE("Voronoi overlapping bands have reproducible ownership across independent fills", "[FillVoronoi]")
{
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    const BoundingBox bounds = region.contour.bounding_box();
    VoronoiInfill first, second, unrelated;
    const auto expected = first.fill_layer(bounds, region, 0.4, 0.2, 5., 0.45, 0.2);
    unrelated.fill_layer(bounds, region, 0.4, 0.3, 13., 0.45, 0.3);
    const auto actual = second.fill_layer(bounds, region, 0.4, 0.2, 5., 0.45, 0.2);
    REQUIRE_FALSE(expected.bands.empty());
    REQUIRE(actual.bands.size() == expected.bands.size());
    for (size_t i = 0; i < expected.bands.size(); ++i) {
        REQUIRE_THAT((expected.bands[i].supported_side - actual.bands[i].supported_side).norm(),
                     Catch::Matchers::WithinAbs(0., EPSILON));
        ExPolygons difference = diff_ex(ExPolygons{expected.bands[i].polygon}, ExPolygons{actual.bands[i].polygon});
        append(difference, diff_ex(ExPolygons{actual.bands[i].polygon}, ExPolygons{expected.bands[i].polygon}));
        double area = 0.;
        for (const auto &polygon : difference)
            area += polygon.area();
        REQUIRE(area < scale_(EPSILON) * scale_(1.));
    }
}

TEST_CASE("Voronoi snaps short edges without transitive vertex drift", "[FillVoronoi]")
{
    const Polylines walls {Polyline(Points{Point(0, 0), Point(6, 0), Point(12, 0), Point(0, 100)})};
    const auto graph = Voronoi::build_wall_graph(walls, 8);
    REQUIRE(graph.indices.at(Point(6, 0)) == graph.indices.at(Point(0, 0)));
    REQUIRE(graph.indices.at(Point(12, 0)) != graph.indices.at(Point(0, 0)));
    REQUIRE(graph.edges.size() == 2);
    for (const auto &entry : graph.indices)
        REQUIRE((entry.first.cast<double>() - graph.vertices[entry.second].point.cast<double>()).norm() <= 8.);
}

TEST_CASE("Voronoi prints straight through rounded four way and six way crossings", "[FillVoronoi]")
{
    const size_t degree = GENERATE(size_t(4), size_t(6));
    const bool rounded = GENERATE(false, true);
    Polylines walls;
    for (size_t i = 0; i < degree; ++i) {
        const double angle = 2. * M_PI * double(i) / double(degree);
        const Point center = rounded && i % 2 ? Point(3, 0) : Point(0, 0);
        walls.emplace_back(Points{center, Point::new_scale(20. * std::cos(angle), 20. * std::sin(angle))});
    }
    if (rounded) walls.emplace_back(Points{Point(0, 0), Point(3, 0)});
    const auto graph = Voronoi::build_wall_graph(walls);
    const auto connections = Voronoi::pair_junctions(graph, {}, 0);
    const auto trails = Voronoi::trace_trails(graph, connections, 0);
    REQUIRE(trails.size() == degree / 2);
    REQUIRE(segments(trails).size() == degree);
    for (const Polyline &trail : trails) {
        REQUIRE(trail.points.size() == 3);
        const Vec2d a = (trail.points[0] - trail.points[1]).cast<double>();
        const Vec2d b = (trail.points[2] - trail.points[1]).cast<double>();
        REQUIRE(a.dot(b) / (a.norm() * b.norm()) < -0.99999);
    }
}

TEST_CASE("Voronoi cycles actual three way connections using persistent generating sites", "[FillVoronoi]")
{
    const BoundingBox bounds(Point::new_scale(-20., -20.), Point::new_scale(20., 20.));
    std::vector<Vec3d> sites {Vec3d(0., 2., 0.), Vec3d(-std::sqrt(3.), -1., 0.), Vec3d(std::sqrt(3.), -1., 0.)};
    const bool rounded_metadata = GENERATE(false, true);
    const bool duplicate_site = GENERATE(false, true);
    if (duplicate_site) sites.push_back(sites.front());
    std::set<Point> unpaired;
    Segments expected;
    for (size_t layer_id = 0; layer_id < 3; ++layer_id) {
        // Reordered point insertion and a changing Z plane must not change
        // which physical branch occupies each position in the three-layer cycle.
        std::rotate(sites.begin(), sites.begin() + 1, sites.end());
        auto section = Voronoi::section(sites, bounds, .2 * layer_id);
        const Point junction_point = section.junctions.front().point;
        if (rounded_metadata) section.junctions.front().point += Point(1, 0);
        const auto graph = Voronoi::build_wall_graph(section.walls);
        const auto connections = Voronoi::pair_junctions(graph, section.junctions, layer_id);
        auto trails = Voronoi::trace_trails(graph, connections, layer_id);
        REQUIRE(trails.size() == 2);
        if (layer_id == 0) expected = segments(trails);
        REQUIRE(segments(trails) == expected);
        REQUIRE(section.junctions.size() == 1);
        const size_t center = graph.indices.at(junction_point);
        size_t open = 0;
        for (size_t e : graph.vertices[center].edges) {
            const size_t side = graph.edges[e][0] == center ? 0 : 1;
            if (connections[e][side] == Voronoi::no_connection) {
                ++open;
                unpaired.insert(graph.vertices[graph.edges[e][1 - side]].point);
            }
        }
        REQUIRE(open == 1);
        // Test actual traced paths, not only the intended pairing table.
        size_t endpoints_at_center = 0;
        for (const auto &trail : trails) {
            endpoints_at_center += trail.first_point() == graph.vertices[center].point;
            endpoints_at_center += trail.last_point() == graph.vertices[center].point;
        }
        REQUIRE(endpoints_at_center == 1);
    }
    REQUIRE(unpaired.size() == 3);
}

TEST_CASE("Voronoi trail ordering chooses the nearest endpoint and preserves all walls", "[FillVoronoi]")
{
    Polylines trails {Polyline(Point(20, 0), Point(10, 0)), Polyline(Point(22, 0), Point(30, 0)),
                      Polyline(Point(-100, 0), Point(-90, 0))};
    const auto expected = segments(trails);
    const auto ordered = Voronoi::order_trails(std::move(trails), Point(0, 0));
    REQUIRE(ordered.size() == 3);
    REQUIRE(ordered[0].first_point() == Point(10, 0));
    REQUIRE(ordered[1].first_point() == Point(22, 0));
    REQUIRE(segments(ordered) == expected);
    REQUIRE(Voronoi::order_trails({}, Point(0, 0)).empty());
}

TEST_CASE("Voronoi tracing keeps parallel edges and changes closed trail starts", "[FillVoronoi]")
{
    const Polylines walls {Polyline(Points{Point(0, 0), Point(100, 0)}),
                          Polyline(Points{Point(0, 0), Point(100, 0)})};
    const auto graph = Voronoi::build_wall_graph(walls);
    const auto connections = Voronoi::pair_junctions(graph, {}, 0);
    const auto first = Voronoi::trace_trails(graph, connections, 0);
    const auto second = Voronoi::trace_trails(graph, connections, 1);
    REQUIRE(first.size() == 1);
    REQUIRE(second.size() == 1);
    REQUIRE(first.front().points.size() == 3);
    REQUIRE(first.front().first_point() != second.front().first_point());
    REQUIRE(first.front().first_point() == first.front().last_point());
    REQUIRE_THAT(path_length(first), Catch::Matchers::WithinAbs(200., 1e-9));
    REQUIRE_THAT(path_length(second), Catch::Matchers::WithinAbs(200., 1e-9));
}

TEST_CASE("Voronoi orders whole floor groups without reversing their supported sweep", "[FillVoronoi]")
{
    const std::vector<Polylines> groups {
        {Polyline(Point(100, 0), Point(1, 0))},
        {Polyline(Point(10, 0), Point(20, 0)), Polyline(Point(21, 0), Point(90, 0))},
        {Polyline(Point(200, 0), Point(210, 0))}
    };
    std::vector<Voronoi::PathGroup> input {{{}, false}};
    for (const auto &group : groups) input.push_back({group, false});
    const auto ordered = Voronoi::order_groups(std::move(input), Point(0, 0));
    REQUIRE(ordered.size() == 3);
    // The first group's exit is closest but cannot be used as its entry.
    // The next choice uses the last fragment's exit, not the first's.
    for (size_t i = 0; i < ordered.size(); ++i) {
        const auto &expected = groups[std::array<size_t, 3>{1, 0, 2}[i]];
        REQUIRE(ordered[i].paths.size() == expected.size());
        for (size_t j = 0; j < expected.size(); ++j)
            REQUIRE(ordered[i].paths[j].points == expected[j].points);
    }
    REQUIRE(Voronoi::order_groups({}, Point(0, 0)).empty());
}

TEST_CASE("Voronoi mixes floor groups and reversible wall trails using their actual exits", "[FillVoronoi]")
{
    const Point floor_exit = GENERATE(Point(90, 10), Point(10, 10));
    const Polylines floor {Polyline(Point(10, 1), Point(80, 0)), Polyline(Point(80, 1), floor_exit)};
    const Polyline wall(Point(0, 0), Point(10, 0));
    const Polyline next_wall(floor_exit + Point(20, 0), floor_exit);
    std::vector<Voronoi::PathGroup> input {{{next_wall}, true}, {floor, false}, {{wall}, true}};
    const auto ordered = Voronoi::order_groups(input, Point(0, 0));
    REQUIRE(ordered.size() == 3);
    REQUIRE(ordered[0].reversible);
    REQUIRE_FALSE(ordered[1].reversible);
    REQUIRE(ordered[2].reversible);
    REQUIRE(ordered[0].paths.front().points == wall.points);
    REQUIRE(ordered[1].paths.size() == floor.size());
    for (size_t i = 0; i < floor.size(); ++i)
        REQUIRE(ordered[1].paths[i].points == floor[i].points);
    REQUIRE(ordered[2].paths.front().first_point() == floor_exit);
    REQUIRE(ordered[2].paths.front().last_point() == next_wall.first_point());
    // A floor can also be the first group; walls have no blanket priority.
    const auto floor_first = Voronoi::order_groups(input, Point(10, 1));
    REQUIRE_FALSE(floor_first.front().reversible);
}

TEST_CASE("Voronoi keeps supporting walls between horizontal floor faces", "[FillVoronoi]")
{
    FixedSites infill;
    for (int x = -1; x <= 4; ++x)
        for (int y = -1; y <= 4; ++y)
            for (int z = -1; z <= 2; ++z)
                infill.sites.emplace_back(20. * x, 20. * y, 20. * z);
    const ExPolygon region = rectangle(0., 0., 50., 50.);
    const auto layer = infill.fill_layer(region.contour.bounding_box(), region, .4, .05, 10.1, .45, .2);
    REQUIRE(layer.bands.size() > 1);
    for (int axis = 0; axis < 2; ++axis)
        for (double position : {10., 30.})
            for (double along : {1., 20., 40., 49.}) {
                bool covered = false;
                for (const Polyline &wall : layer.walls)
                    for (size_t i = 1; i < wall.points.size(); ++i) {
                        const Point &a = wall.points[i - 1], &b = wall.points[i];
                        if (std::abs(a[axis] - scale_(position)) <= 1 && std::abs(b[axis] - scale_(position)) <= 1 &&
                            scale_(along) >= std::min(a[1 - axis], b[1 - axis]) &&
                            scale_(along) <= std::max(a[1 - axis], b[1 - axis]))
                            covered = true;
                    }
                REQUIRE(covered);
            }
}

TEST_CASE("Voronoi point queries preserve the field when partitioned", "[FillVoronoi]")
{
    Voronoi::PoissonPointCloud provider;
    const BoundingBoxf3 whole(Vec3d(-13.2, -8.1, -9.4), Vec3d(14.7, 12.6, 11.3));
    auto keys = [](const std::vector<Vec3d> &points) {
        std::set<std::array<double, 3>> result;
        for (const Vec3d &p : points)
            REQUIRE(result.insert({p.x(), p.y(), p.z()}).second);
        return result;
    };
    const auto expected = keys(provider.points_in(whole, 3.));
    REQUIRE_FALSE(expected.empty());
    const int axis = GENERATE(0, 1, 2);
    BoundingBoxf3 first = whole, second = whole;
    first.max[axis] = second.min[axis] = 0.37;
    auto points = provider.points_in(first, 3.);
    auto rest = provider.points_in(second, 3.);
    points.insert(points.end(), rest.begin(), rest.end());
    REQUIRE(keys(points) == expected);
}

TEST_CASE("Voronoi coverage includes boundary corners without internal nodes", "[FillVoronoi]")
{
    const auto bounds = rectangle(-2., -3., 2., 3.).contour.bounding_box();
    const auto section = Voronoi::section({Vec3d(0., 0., 4.)}, bounds, 0.);
    REQUIRE(section.walls.empty());
    REQUIRE_THAT(section.covering_radius, Catch::Matchers::WithinAbs(std::sqrt(29.), EPSILON));
    REQUIRE_FALSE(std::isfinite(Voronoi::section({}, bounds, 0.).covering_radius));
}

TEST_CASE("Voronoi coverage bounds nearest distance throughout clipped cells", "[FillVoronoi]")
{
    const auto bounds = rectangle(-7., -5., 9., 8.).contour.bounding_box();
    const std::vector<Vec3d> sites {Vec3d(-5., -3., -2.), Vec3d(6., -2., 3.),
                                  Vec3d(-3., 6., 1.), Vec3d(5., 7., -4.)};
    const double z = GENERATE(-3., 0., 5.);
    const auto section = Voronoi::section(sites, bounds, z);
    REQUIRE_FALSE(section.walls.empty());
    for (double x = -7.; x <= 9.; x += 0.25)
        for (double y = -5.; y <= 8.; y += 0.25) {
            double nearest = std::numeric_limits<double>::infinity();
            for (const auto &site : sites)
                nearest = std::min(nearest, (Vec3d(x, y, z) - site).norm());
            REQUIRE(nearest <= section.covering_radius + EPSILON);
        }
}

TEST_CASE("Voronoi adaptive coverage crosses gaps and preserves an exterior field", "[FillVoronoi]")
{
    auto provider = std::make_shared<FixedProvider>();
    provider->sites = {Vec3d(-100., 0., -20.), Vec3d(100., 0., 20.),
                       Vec3d(0., 100., 0.), Vec3d(0., -100., 0.)};
    Voronoi::ActivePointCloud cloud(provider);
    const auto bounds = rectangle(-5., -5., 5., 5.).contour.bounding_box();
    for (double z : {0., 1., 20., 0.}) {
        Voronoi::Section actual;
        REQUIRE(cloud.update(bounds, z, 0.5, 1., [&](const auto &sites) {
            actual = Voronoi::section(sites, bounds, z);
            return actual.covering_radius;
        }));
        const auto expected = Voronoi::section(provider->sites, bounds, z);
        REQUIRE(segments(actual.walls) == segments(expected.walls));
        REQUIRE_THAT(actual.covering_radius, Catch::Matchers::WithinAbs(expected.covering_radius, EPSILON));
    }
}

TEST_CASE("Voronoi retirement preserves future slices and rounded floors", "[FillVoronoi]")
{
    auto provider = std::make_shared<FixedProvider>();
    // Structured sites include near-horizontal faces and points well below the slice.
    for (double z = -20.; z <= 40.; z += 4.)
        for (double x : {-6., 0., 6.})
            for (double y : {-6., 0., 6.})
                provider->sites.emplace_back(x + 0.03 * z, y, z);
    Voronoi::ActivePointCloud cloud(provider);
    const auto bounds = rectangle(-4., -4., 4., 4.).contour.bounding_box();
    const double width = 0.45, height = 0.2, row_spacing = 0.4;
    const double margin = height * (1. + 2. * row_spacing / width);
    for (double z : {0., 2., 4., 10., 20., 0.}) {
        REQUIRE(cloud.update(bounds, z, margin, 3., [&](const auto &sites) {
            return Voronoi::section(sites, bounds, z).covering_radius;
        }));
        REQUIRE(cloud.sites().size() < provider->sites.size());
        REQUIRE(segments(Voronoi::section(cloud.sites(), bounds, z).walls) ==
                segments(Voronoi::section(provider->sites, bounds, z).walls));
        const auto actual = Voronoi::shallow_bands(cloud.sites(), bounds, z, width, height, row_spacing);
        const auto expected = Voronoi::shallow_bands(provider->sites, bounds, z, width, height, row_spacing);
        ExPolygons actual_polygons, expected_polygons;
        for (const auto &band : actual) actual_polygons.push_back(band.polygon);
        for (const auto &band : expected) expected_polygons.push_back(band.polygon);
        REQUIRE(diff_ex(actual_polygons, expected_polygons).empty());
        REQUIRE(diff_ex(expected_polygons, actual_polygons).empty());
        for (const auto &site : cloud.sites())
            REQUIRE(site.z() >= cloud.retired_below());
    }
}

TEST_CASE("Voronoi finite empty clouds terminate without geometry", "[FillVoronoi]")
{
    auto provider = std::make_shared<FixedProvider>();
    VoronoiInfill infill(provider);
    const auto region = rectangle(-5., -5., 5., 5.);
    REQUIRE(infill.fill(region.contour.bounding_box(), region, 0.45, 0.2, 0.).empty());
}

TEST_CASE("Voronoi wall distance favors walls and both corner signs", "[FillVoronoi]")
{
    using Cloud = Voronoi::WallDistancePointCloud;
    REQUIRE(Cloud::density_ratio(-1., -1.) > Cloud::density_ratio(-15., -15.));
    REQUIRE(Cloud::density_ratio(-4., -3.) > Cloud::density_ratio(-4., -4.));
    REQUIRE(Cloud::density_ratio(-4., -5.) > Cloud::density_ratio(-4., -4.));
    REQUIRE_THAT(Cloud::density_ratio(-1000., -1000.), Catch::Matchers::WithinAbs(0.01, EPSILON));
    REQUIRE_THAT(Cloud::density_ratio(0., 0.), Catch::Matchers::WithinAbs(1., EPSILON));
    REQUIRE_THAT(Cloud::density_ratio(-6., -6.), Catch::Matchers::WithinAbs(0.01 + 0.99 * std::exp(-4. / 6.), EPSILON));
    REQUIRE(Cloud::density_ratio(-50., -50.) < 0.02);
    REQUIRE(Cloud::density_ratio(-1000., -1000.) > 0.);
    REQUIRE_THAT(Cloud::density_ratio(-5., -5.), Catch::Matchers::WithinAbs(Cloud::density_ratio(5., 5.), EPSILON));
}

TEST_CASE("Voronoi wall thinning preserves reproducibility and site intensity", "[FillVoronoi]")
{
    auto constant = [](const std::vector<Vec3d> &points) { return std::vector<Voronoi::WallDistanceSample>(points.size(), {-6., -6.}); };
    Voronoi::PoissonPointCloud uniform;
    Voronoi::WallDistancePointCloud thinned(constant);
    const BoundingBoxf3 box(Vec3d(-20., -20., -20.), Vec3d(20., 20., 20.));
    auto keys = [](const std::vector<Vec3d> &points) {
        std::set<std::array<double, 3>> result;
        for (const Vec3d &p : points) REQUIRE(result.insert({p.x(), p.y(), p.z()}).second);
        return result;
    };
    const double maximum_ratio = Voronoi::WallDistancePointCloud::density_ratio(0., 2.);
    const auto source = keys(uniform.points_in(box, 1. / maximum_ratio));
    const auto expected = keys(thinned.points_in(box, 1.));
    REQUIRE_FALSE(expected.empty());
    REQUIRE_THAT(double(expected.size()) / source.size(), Catch::Matchers::WithinRel(std::pow(Voronoi::WallDistancePointCloud::density_ratio(-6., -6.) / maximum_ratio, 3), 0.15));
    for (const auto &point : expected) REQUIRE(source.count(point) == 1);
    auto first = box, second = box;
    first.max.z() = second.min.z() = 0.37;
    auto points = thinned.points_in(second, 1.);
    auto rest = thinned.points_in(first, 1.);
    points.insert(points.end(), rest.begin(), rest.end());
    REQUIRE(keys(points) == expected);
}

TEST_CASE("Voronoi sliced distances include holes and use each point's height", "[FillVoronoi]")
{
    ExPolygon lower = rectangle(-20., -20., 20., 20.);
    lower.holes.push_back(rectangle(-3., -3., 3., 3.).contour);
    lower.holes.back().reverse();
    const ExPolygons low {lower}, high {rectangle(-10., -10., 10., 10.)};
    Voronoi::CubicWallDistance field({{-5., 5., &low}, {5., 15., &high}});
    const auto values = field.sample({Vec3d(19., 0., 0.), Vec3d(0., 0., 0.),
                                      Vec3d(4., 0., 0.), Vec3d(9., 0., 10.), Vec3d(11., 0., 10.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE(values[1] > 0.);
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[4], Catch::Matchers::WithinAbs(1., 0.2));
    const auto again = field.sample({Vec3d(11., 0., 10.), Vec3d(19., 0., 0.)});
    REQUIRE_THAT(again[0], Catch::Matchers::WithinAbs(values[4], EPSILON));
    REQUIRE_THAT(again[1], Catch::Matchers::WithinAbs(values[0], EPSILON));
}

TEST_CASE("Voronoi wall cloud makes smaller cells near model walls", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-40., -40., 40., 40.)};
    auto field = std::make_shared<Voronoi::CubicWallDistance>(std::vector<Voronoi::WallSlice>{{-100., 100., &outlines}});
    auto provider = std::make_shared<Voronoi::WallDistancePointCloud>([field](const auto &points) { return field->sample_fields(points); });
    VoronoiInfill infill(provider);
    const auto bounds = outlines.front().contour.bounding_box();
    const auto walls = infill.fill(bounds, outlines.front(), 0.45, 0.25, 0.);
    REQUIRE_FALSE(walls.empty());
    const auto near_wall = rectangle(30., -30., 38., 30.);
    const auto center = rectangle(-4., -30., 4., 30.);
    REQUIRE(path_length(intersection_pl(walls, near_wall)) > path_length(intersection_pl(walls, center)));
    REQUIRE(segments(infill.fill(bounds, outlines.front(), 0.45, 0.25, 5.)) != segments(walls));
    REQUIRE(segments(infill.fill(bounds, outlines.front(), 0.45, 0.25, 0.)) == segments(walls));
}

TEST_CASE("Voronoi wall distances include floors and roofs without internal layer caps", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    Voronoi::CubicWallDistance field({{0., 10., &outlines}, {10., 20., &outlines}});
    const auto values = field.sample({Vec3d(0., 0., 1.), Vec3d(0., 0., 19.),
                                      Vec3d(0., 0., 10.), Vec3d(0., 0., -2.),
                                      Vec3d(0., 0., 22.), Vec3d(21., 0., 22.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-10., 0.2));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(2., 0.2));
    REQUIRE_THAT(values[4], Catch::Matchers::WithinAbs(2., 0.2));
    REQUIRE_THAT(values[5], Catch::Matchers::WithinAbs(std::sqrt(5.), 0.2));
    Voronoi::CubicWallDistance rounded({{0., 0.3, &outlines}, {0.1 + 0.2, 0.6, &outlines}});
    REQUIRE(std::abs(rounded.sample({Vec3d(0., 0., 0.3)}).front()) <= 2.);
}

TEST_CASE("Voronoi wall distances include local ledges and cavity floors and ceilings", "[FillVoronoi]")
{
    const ExPolygons wide {rectangle(-20., -20., 20., 20.)};
    const ExPolygons narrow {rectangle(-10., -10., 10., 10.)};
    Voronoi::CubicWallDistance ledge({{0., 10., &wide}, {10., 20., &narrow}});
    const auto values = ledge.sample({Vec3d(15., 0., 9.), Vec3d(15., 0., 11.), Vec3d(0., 0., 10.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(1., 0.2));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-10., 0.2));

    ExPolygon hollow = wide.front();
    hollow.holes.push_back(narrow.front().contour);
    hollow.holes.back().reverse();
    const ExPolygons middle {hollow};
    Voronoi::CubicWallDistance cavity({{0., 10., &wide}, {10., 20., &middle}, {20., 30., &wide}});
    const auto cap = cavity.sample({Vec3d(0., 0., 9.), Vec3d(0., 0., 11.),
                                   Vec3d(0., 0., 19.), Vec3d(0., 0., 21.)});
    REQUIRE_THAT(cap[0], Catch::Matchers::WithinAbs(-1., 0.2));
    REQUIRE_THAT(cap[1], Catch::Matchers::WithinAbs(1., 0.2));
    REQUIRE_THAT(cap[2], Catch::Matchers::WithinAbs(1., 0.2));
    REQUIRE_THAT(cap[3], Catch::Matchers::WithinAbs(-1., 0.2));
}

TEST_CASE("Voronoi wall clouds become denser near both floors and roofs", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-50., -50., 50., 50.)};
    auto field = std::make_shared<Voronoi::CubicWallDistance>(std::vector<Voronoi::WallSlice>{{0., 50., &outlines}});
    Voronoi::WallDistancePointCloud cloud([field](const auto &points) { return field->sample_fields(points); });
    const BoundingBoxf3 bottom(Vec3d(-10., -10., 0.), Vec3d(10., 10., 5.));
    const BoundingBoxf3 center(Vec3d(-10., -10., 22.5), Vec3d(10., 10., 27.5));
    const BoundingBoxf3 top(Vec3d(-10., -10., 45.), Vec3d(10., 10., 50.));
    const auto floor_points = cloud.points_in(bottom, 1.);
    const auto center_points = cloud.points_in(center, 1.);
    const auto roof_points = cloud.points_in(top, 1.);
    REQUIRE(floor_points.size() > 3 * center_points.size());
    REQUIRE(roof_points.size() > 3 * center_points.size());
    REQUIRE(cloud.points_in(bottom, 1.) == floor_points);
}

TEST_CASE("Voronoi cloud setup is reused across parallel layers and resets on reslicing", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    std::atomic<size_t> loads {0};
    const auto slices = [&] {
        ++loads;
        return std::vector<Voronoi::WallSlice>{{0., 20., &outlines}};
    };
    Voronoi::WallDistanceCloudCache cache;
    std::vector<std::future<std::shared_ptr<const Voronoi::PointCloudProvider>>> workers;
    for (int i = 0; i < 8; ++i)
        workers.push_back(std::async(std::launch::async, [&] { return cache.get(slices); }));
    const auto cloud = workers.front().get();
    for (size_t i = 1; i < workers.size(); ++i) REQUIRE(workers[i].get() == cloud);
    REQUIRE(cache.get(slices) == cloud);
    REQUIRE(loads == 1);
    const BoundingBoxf3 region(Vec3d(-10., -10., 0.), Vec3d(10., 10., 3.));
    const auto points = cloud->points_in(region, 4.);
    REQUIRE_FALSE(points.empty());
    cache.clear();
    const auto regenerated = cache.get(slices);
    REQUIRE(regenerated != cloud);
    REQUIRE(loads == 2);
    REQUIRE(regenerated->points_in(region, 4.) == points);
}

TEST_CASE("Voronoi cubic distances preserve a fixed component beside changing profiles", "[FillVoronoi]")
{
    std::vector<ExPolygons> profiles(20);
    std::vector<Voronoi::WallSlice> slices;
    for (size_t i = 0; i < profiles.size(); ++i) {
        profiles[i].push_back(rectangle(-100., -50., 0., 50.));
        const double z = (i + 0.5) * 5.;
        const double radius = std::sqrt(2500. - (z - 50.) * (z - 50.));
        Polygon circle;
        for (int n = 0; n < 32; ++n) {
            const double angle = 2. * M_PI * n / 32.;
            circle.points.push_back(Point::new_scale(50. + radius * std::cos(angle), radius * std::sin(angle)));
        }
        profiles[i].push_back(ExPolygon(circle));
        slices.push_back({i * 5., (i + 1) * 5., &profiles[i]});
    }
    Voronoi::CubicWallDistance field(slices);
    std::vector<Vec3d> points;
    for (double x : {-99., -50., -1.})
        for (double y : {-49., 0., 49.})
            for (double z : {1., 50., 99.}) points.emplace_back(x, y, z);
    const auto distances = field.sample(points);
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec3d q = (points[i] - Vec3d(-50., 0., 50.)).cwiseAbs() - Vec3d(50., 50., 50.);
        REQUIRE_THAT(distances[i], Catch::Matchers::WithinAbs(q.maxCoeff(), 0.7));
    }
    const ExPolygons lower {rectangle(-20., -20., 20., 20.)}, upper {rectangle(-10., -10., 10., 10.)};
    Voronoi::CubicWallDistance ledge({{0., 10., &lower}, {10., 20., &upper}});
    REQUIRE_THAT(ledge.sample({Vec3d(9., 0., 9.)}).front(), Catch::Matchers::WithinAbs(-std::sqrt(2.), 0.2));
}

TEST_CASE("Voronoi distances preserve long diagonal walls and caps", "[FillVoronoi]")
{
    const ExPolygons outlines {ExPolygon{Point::new_scale(0., -100.), Point::new_scale(100., 0.),
                                         Point::new_scale(0., 100.), Point::new_scale(-100., 0.)}};
    Voronoi::CubicWallDistance field({{0., 100., &outlines}});
    const auto values = field.sample({Vec3d(30., 20., 50.), Vec3d(40., 30., 1.),
                                      Vec3d(40., 30., 99.), Vec3d(70., 60., 50.),
                                      Vec3d(103., 0., 50.), Vec3d(0., 0., -2.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-50. / std::sqrt(2.), 0.5));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(-1., 0.3));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-1., 0.3));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(30. / std::sqrt(2.), 0.3));
    REQUIRE_THAT(values[4], Catch::Matchers::WithinAbs(3., 0.3));
    REQUIRE_THAT(values[5], Catch::Matchers::WithinAbs(2., 0.3));
}

TEST_CASE("Voronoi cubic distances include walls floors holes and deep interiors", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-40., -40., 40., 40.)};
    Voronoi::CubicWallDistance table({{0., 40., &outlines}, {40., 80., &outlines}});
    const auto values = table.sample({Vec3d(0., 0., 40.), Vec3d(0., 0., 1.),
                                      Vec3d(0., 0., 79.), Vec3d(42., 0., 40.), Vec3d(100., 0., 40.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(-40., 0.5));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(-1., 0.15));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-1., 0.15));
    REQUIRE(values[3] > 0.);
    REQUIRE(values[4] > 32.);
    ExPolygon hollow = outlines.front();
    hollow.holes.push_back(rectangle(-10., -10., 10., 10.).contour);
    hollow.holes.back().reverse();
    const ExPolygons middle {hollow};
    Voronoi::CubicWallDistance cavity({{0., 10., &outlines}, {10., 20., &middle}, {20., 30., &outlines}});
    REQUIRE_THAT(cavity.sample({Vec3d(0., 0., 15.)}).front(), Catch::Matchers::WithinAbs(5., 0.2));
    REQUIRE_THAT(cavity.sample({Vec3d(0., 0., 5.)}).front(), Catch::Matchers::WithinAbs(-5., 0.2));
}

TEST_CASE("Voronoi logarithmic distances retain zero crossings before density saturation", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    Voronoi::CubicWallDistance table({{0., 20., &outlines}});
    const auto values = table.sample({Vec3d(0., 0., -0.25), Vec3d(0., 0., 0.),
                                      Vec3d(0., 0., 0.25), Vec3d(19.5, 0., 10.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(0.25, 0.03));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(0., EPSILON));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(-0.25, 0.03));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(-0.5, 0.06));
    for (double distance : values)
        REQUIRE_THAT(Voronoi::WallDistancePointCloud::density_ratio(distance, distance),
                     Catch::Matchers::WithinAbs(1., EPSILON));
}

TEST_CASE("Voronoi logarithmic distances retain distant slices and saturate at one metre", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-4., -4., 4., 4.)};
    Voronoi::CubicWallDistance table({{0., 10., &outlines}, {2210., 2220., &outlines}});
    const auto values = table.sample({Vec3d(0., 0., 50.), Vec3d(0., 0., 1000.),
                                      Vec3d(0., 0., 1110.), Vec3d(0., 0., 2170.)});
    REQUIRE_THAT(values[0], Catch::Matchers::WithinAbs(40., 0.5));
    REQUIRE_THAT(values[1], Catch::Matchers::WithinAbs(990., 11.));
    REQUIRE_THAT(values[2], Catch::Matchers::WithinAbs(1000., EPSILON));
    REQUIRE_THAT(values[3], Catch::Matchers::WithinAbs(40., 0.5));
}

TEST_CASE("Voronoi cubic distances retain slices between output planes", "[FillVoronoi]")
{
    const ExPolygons solid {rectangle(-20., -20., 20., 20.)};
    ExPolygon hollow = solid.front();
    hollow.holes.push_back(rectangle(-10., -10., 10., 10.).contour);
    hollow.holes.back().reverse();
    const ExPolygons cavity {hollow};
    Voronoi::CubicWallDistance table({{0., 10.2, &solid}, {10.2, 10.3, &cavity}, {10.3, 20., &solid}});
    const auto values = table.sample({Vec3d(0., 0., 9.), Vec3d(0., 0., 12.), Vec3d(15., 0., 9.)});
    REQUIRE(std::abs(values[0]) <= 2.2);
    REQUIRE(std::abs(values[1]) <= 2.2);
    REQUIRE(std::abs(values[2]) > 4.);
    const ExPolygons empty;
    Voronoi::CubicWallDistance gap({{0., 10.2, &solid}, {10.2, 10.3, &empty}, {10.3, 20., &solid}});
    REQUIRE(std::abs(gap.sample({Vec3d(15., 0., 9.)}).front()) <= 2.2);
    Voronoi::CubicWallDistance missing({{0., 10.2, &solid}, {10.3, 20., &solid}});
    REQUIRE(gap.sample({Vec3d(15., 0., 9.)}) == missing.sample({Vec3d(15., 0., 9.)}));
}

TEST_CASE("Voronoi distance sweeps match all slab minima across changing thin profiles", "[FillVoronoi]")
{
    std::vector<ExPolygons> profiles(40);
    std::vector<Voronoi::WallSlice> slices;
    double height = 0.;
    for (size_t i = 0; i < profiles.size(); ++i) {
        const double radius = 5. + (i * 7 % 13);
        profiles[i].push_back(rectangle(-radius, -18., radius, 18.));
        if (i % 3 == 0) {
            profiles[i].front().holes.push_back(rectangle(-2., -3., 2., 3.).contour);
            profiles[i].front().holes.back().reverse();
        }
        const double thickness = i % 2 == 0 ? 0.17 : 1.31;
        slices.push_back({height, height + thickness, &profiles[i]});
        height += thickness;
        if (i == 20) height += 0.23;
    }
    Voronoi::CubicWallDistance field(slices, 0.);
    const BoundingBox bounds(Point::new_scale(-21., -22.), Point::new_scale(21., 22.));
    std::vector<Vec3d> points;
    for (double z = -2.; z <= std::ceil(height) + 2.; z += 1.)
        for (double x = -19.; x <= 19.; x += 1.) points.emplace_back(x, 0., z);
    std::vector<double> solid(points.size(), 1e6), air(points.size());
    std::vector<bool> inside(points.size(), false);
    for (size_t i = 0; i < points.size(); ++i) {
        const double dz = std::max(0., std::min(points[i].z(), height - points[i].z()));
        air[i] = dz * dz;
    }
    double previous_top = 0.;
    for (const auto &slice : slices) {
        EdgeGrid::Grid grid(bounds);
        grid.create(*slice.contours, scale_(1.));
        grid.calculate_sdf();
        for (size_t i = 0; i < points.size(); ++i) {
            const double z = points[i].z();
            if (slice.bottom_z > previous_top) {
                const double gap = std::max({0., previous_top - z, z - slice.bottom_z});
                air[i] = std::min(air[i], gap * gap);
            }
            const float sdf = float(grid.signed_distance_bilinear(Point::new_scale(points[i].x(), points[i].y())) * SCALING_FACTOR);
            const double dz = std::max({0., slice.bottom_z - z, z - slice.top_z});
            solid[i] = std::min(solid[i], dz * dz + std::pow(std::max(0.f, sdf), 2));
            air[i] = std::min(air[i], dz * dz + std::pow(std::max(0.f, -sdf), 2));
            if (dz == 0. && sdf < 0.) inside[i] = true;
        }
        previous_top = slice.top_z;
    }
    const auto actual = field.sample(points);
    for (size_t i = 0; i < points.size(); ++i) {
        const double distance = std::sqrt(inside[i] ? air[i] : solid[i]);
        const double code = std::round(255. * std::log1p(distance / 6.) / std::log1p(1000. / 6.));
        const double expected = (inside[i] ? -6. : 6.) * std::expm1(code * std::log1p(1000. / 6.) / 255.);
        REQUIRE_THAT(actual[i], Catch::Matchers::WithinAbs(expected, 1e-6));
    }
}

TEST_CASE("Voronoi Gaussian smoothing preserves planar walls and their zero crossing", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-30., -30., 30., 30.)};
    Voronoi::CubicWallDistance field({{0., 60., &outlines}});
    const auto samples = field.sample_fields({Vec3d(0., 0., -2.), Vec3d(0., 0., 0.),
                                               Vec3d(0., 0., 2.), Vec3d(0., 0., 6.),
                                               Vec3d(28., 0., 30.)});
    for (const auto &sample : samples)
        REQUIRE_THAT(sample.smoothed, Catch::Matchers::WithinAbs(sample.distance, 0.15));
    REQUIRE_THAT(samples[1].smoothed, Catch::Matchers::WithinAbs(0., 0.06));
}

TEST_CASE("Voronoi Gaussian smoothing increases density at convex and concave corners", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    Voronoi::CubicWallDistance convex({{0., 40., &outlines}});
    const auto outer = convex.sample_fields({Vec3d(14., 14., 20.), Vec3d(14., 0., 20.),
                                             Vec3d(14., 0., 34.), Vec3d(0., 14., 34.)});
    using Cloud = Voronoi::WallDistancePointCloud;
    REQUIRE(Cloud::density_ratio(outer[0].distance, outer[0].smoothed) >
            Cloud::density_ratio(outer[0].distance, outer[0].distance) + 0.03);
    REQUIRE_THAT(outer[1].smoothed, Catch::Matchers::WithinAbs(outer[1].distance, 0.15));
    REQUIRE_THAT(outer[2].smoothed, Catch::Matchers::WithinAbs(outer[0].smoothed, 0.15));
    REQUIRE_THAT(outer[3].smoothed, Catch::Matchers::WithinAbs(outer[0].smoothed, 0.15));
    ExPolygon hollow = rectangle(-30., -30., 30., 30.);
    hollow.holes.push_back(rectangle(-10., -10., 10., 10.).contour);
    hollow.holes.back().reverse();
    const ExPolygons hole {hollow};
    Voronoi::CubicWallDistance concave({{0., 40., &hole}});
    const auto inner = concave.sample_fields({Vec3d(14., 14., 20.)}).front();
    REQUIRE(Cloud::density_ratio(inner.distance, inner.smoothed) >
            Cloud::density_ratio(inner.distance, inner.distance) + 0.01);
}

TEST_CASE("Voronoi separable smoothing matches a three dimensional Gaussian", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    Voronoi::CubicWallDistance field({{0., 40., &outlines}});
    const Vec3d center(14., 14., 34.);
    std::vector<Vec3d> points;
    std::vector<double> weights;
    for (int x = -6; x <= 6; ++x)
        for (int y = -6; y <= 6; ++y)
            for (int z = -6; z <= 6; ++z) {
                points.push_back(center + Vec3d(x, y, z));
                weights.push_back(std::exp(-(x * x + y * y + z * z) / 8.));
            }
    const auto distances = field.sample(points);
    double expected = 0., weight_sum = 0.;
    for (size_t i = 0; i < distances.size(); ++i) {
        expected += weights[i] * distances[i];
        weight_sum += weights[i];
    }
    REQUIRE_THAT(field.sample_fields({center}).front().smoothed,
                 Catch::Matchers::WithinAbs(expected / weight_sum, 0.15));
}

TEST_CASE("Voronoi Gaussian smoothing remains planar when its radius exceeds the halo", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-50., -50., 50., 50.)};
    Voronoi::CubicWallDistance field({{0., 100., &outlines}}, 8.);
    const auto samples = field.sample_fields({Vec3d(0., 0., -2.), Vec3d(0., 0., 0.),
                                               Vec3d(0., 0., 4.), Vec3d(46., 0., 50.)});
    for (const auto &sample : samples)
        REQUIRE_THAT(sample.smoothed, Catch::Matchers::WithinAbs(sample.distance, 0.3));
}

TEST_CASE("Voronoi Gaussian smoothing accepts zero width and rejects invalid widths", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-10., -10., 10., 10.)};
    Voronoi::CubicWallDistance field({{0., 20., &outlines}}, 0.);
    for (const auto &sample : field.sample_fields({Vec3d(0., 0., 10.), Vec3d(0., 0., -1.), Vec3d(9.5, 0., 10.)}))
        REQUIRE_THAT(sample.smoothed, Catch::Matchers::WithinAbs(sample.distance, EPSILON));
    REQUIRE_THROWS_AS(Voronoi::CubicWallDistance({{0., 20., &outlines}}, -1.), std::invalid_argument);
    REQUIRE_THROWS_AS(Voronoi::CubicWallDistance({{0., 20., &outlines}}, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}

TEST_CASE("Voronoi wall density samples each candidate once using both fields", "[FillVoronoi]")
{
    size_t queried = 0;
    Voronoi::WallDistancePointCloud cloud([&](const auto &points) {
        queried += points.size();
        return std::vector<Voronoi::WallDistanceSample>(points.size(), {-6., -5.});
    });
    const BoundingBoxf3 box(Vec3d(-10., -10., -10.), Vec3d(10., 10., 10.));
    const auto candidates = Voronoi::PoissonPointCloud().points_in(box, 3. / Voronoi::WallDistancePointCloud::density_ratio(0., 2.));
    cloud.points_in(box, 3.);
    REQUIRE(queried == candidates.size());
}

TEST_CASE("Voronoi cubic density saturates near walls and in the deep interior", "[FillVoronoi]")
{
    using Cloud = Voronoi::WallDistancePointCloud;
    REQUIRE_THAT(Cloud::density_ratio(2., 2.), Catch::Matchers::WithinAbs(1., EPSILON));
    REQUIRE_THAT(Cloud::density_ratio(-2., -2.), Catch::Matchers::WithinAbs(1., EPSILON));
    REQUIRE_THAT(Cloud::density_ratio(32., 32.), Catch::Matchers::WithinAbs(0.01, EPSILON));
    REQUIRE(Cloud::density_ratio(8., 8.) > Cloud::density_ratio(16., 16.));
}

TEST_CASE("Voronoi corners boost density above the setting without boosting flat walls", "[FillVoronoi]")
{
    using Cloud = Voronoi::WallDistancePointCloud;
    for (double distance : {0., 1., 2., -1., -2.})
        REQUIRE_THAT(Cloud::density_ratio(distance, distance), Catch::Matchers::WithinAbs(1., EPSILON));
    const double maximum = 0.01 + 0.99 * std::exp(2. / 6.);
    REQUIRE_THAT(Cloud::density_ratio(-2., -1.), Catch::Matchers::WithinAbs(0.01 + 0.99 * std::exp(1. / 6.), EPSILON));
    REQUIRE_THAT(Cloud::density_ratio(-2., -3.), Catch::Matchers::WithinAbs(Cloud::density_ratio(-2., -1.), EPSILON));
    REQUIRE_THAT(Cloud::density_ratio(-1., -20.), Catch::Matchers::WithinAbs(maximum, EPSILON));

    const BoundingBoxf3 box(Vec3d(-20., -20., -20.), Vec3d(20., 20., 20.));
    const auto uniform = Voronoi::PoissonPointCloud().points_in(box, 2.);
    auto count = [&](Voronoi::WallDistanceSample sample) {
        Cloud cloud([sample](const auto &points) { return std::vector<Voronoi::WallDistanceSample>(points.size(), sample); });
        return double(cloud.points_in(box, 2.).size()) / uniform.size();
    };
    REQUIRE_THAT(count({-1., -1.}), Catch::Matchers::WithinRel(1., 0.1));
    REQUIRE_THAT(count({-2., -1.}), Catch::Matchers::WithinRel(std::pow(Cloud::density_ratio(-2., -1.), 3), 0.1));
    REQUIRE_THAT(count({-1., -20.}), Catch::Matchers::WithinRel(std::pow(maximum, 3), 0.1));
}

TEST_CASE("Voronoi zero decay keeps a uniform base and permits corner boosts", "[FillVoronoi]")
{
    using Cloud = Voronoi::WallDistancePointCloud;
    for (double depth : {0., 2., 12., 1000.}) {
        REQUIRE_THAT(Cloud::density_ratio(-depth, -depth, 0.), Catch::Matchers::WithinAbs(1., EPSILON));
        REQUIRE(Cloud::density_ratio(-depth, -depth + 1., 0.) > 1.);
    }
    REQUIRE(Cloud::density_ratio(-12., -12., 12.) > Cloud::density_ratio(-12., -12., 6.));
    const ExPolygons outlines {rectangle(-10., -10., 10., 10.)};
    Voronoi::WallDistanceCloudCache cache;
    size_t loads = 0;
    const auto slices = [&] { ++loads; return std::vector<Voronoi::WallSlice>{{0., 20., &outlines}}; };
    const auto plain = cache.get(slices, 0., 0.);
    const BoundingBoxf3 box(Vec3d(-5., -5., 0.), Vec3d(5., 5., 10.));
    REQUIRE(plain->points_in(box, 2.) == Voronoi::PoissonPointCloud().points_in(box, 2.));
    REQUIRE(cache.get(slices, 0., 0.) == plain);
    REQUIRE(loads == 0);
    REQUIRE(cache.get(slices, 6., 0.) != plain);
    REQUIRE(cache.get(slices, 12., 0.) != plain);
    REQUIRE(loads == 1);
    REQUIRE_THROWS_AS(Cloud({}, -1.), std::invalid_argument);
}

TEST_CASE("Voronoi distance construction reports progress through completion", "[FillVoronoi]")
{
    const ExPolygons outlines{rectangle(0., 0., 20., 20.)};
    const double sigma = GENERATE(0., 2.);
    auto previous_stage = Voronoi::WallDistanceStage::Distance;
    double previous_fraction = 0.;
    bool saw_smoothing = false;
    Voronoi::CubicWallDistance field({{0., 20., &outlines}}, sigma,
        [&](Voronoi::WallDistanceStage stage, double fraction) {
            REQUIRE(int(stage) >= int(previous_stage));
            REQUIRE(fraction >= 0.);
            REQUIRE(fraction <= 1.);
            if (stage == previous_stage) REQUIRE(fraction >= previous_fraction);
            previous_stage = stage;
            previous_fraction = fraction;
            if (stage == Voronoi::WallDistanceStage::SmoothXY || stage == Voronoi::WallDistanceStage::SmoothZ)
                saw_smoothing = true;
        });
    REQUIRE(previous_stage == Voronoi::WallDistanceStage::Complete);
    REQUIRE_THAT(previous_fraction, Catch::Matchers::WithinAbs(1., 1e-12));
    REQUIRE(saw_smoothing == (sigma > 0.));
    REQUIRE(field.sample({Vec3d(10., 10., 10.)}).front() < 0.);
}

TEST_CASE("Canceled Voronoi smoothing leaves the cache usable", "[FillVoronoi]")
{
    const ExPolygons outlines{rectangle(0., 0., 20., 20.)};
    Voronoi::WallDistanceCloudCache cache;
    const auto slices = [&] { return std::vector<Voronoi::WallSlice>{{0., 20., &outlines}}; };
    REQUIRE_THROWS_AS(cache.get(slices, 6., 2.,
        [](Voronoi::WallDistanceStage stage, double) {
            if (stage == Voronoi::WallDistanceStage::SmoothXY) throw std::runtime_error("cancel");
        }), std::runtime_error);
    bool completed = false;
    const auto provider = cache.get(slices, 6., 2.,
        [&](Voronoi::WallDistanceStage stage, double fraction) {
            if (stage == Voronoi::WallDistanceStage::Complete && fraction == 1.) completed = true;
        });
    REQUIRE(completed);
    REQUIRE(provider != nullptr);
    REQUIRE(cache.get(slices, 6., 2.,
        [](auto, double) { throw std::runtime_error("cached tables must not rebuild"); }) == provider);
}
