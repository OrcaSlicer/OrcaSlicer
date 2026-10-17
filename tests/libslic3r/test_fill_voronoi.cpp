#include <catch2/catch_all.hpp>

#include "libslic3r/VoronoiCloudMethod.hpp"
#include "libslic3r/Fill/VoronoiInfill.hpp"
#include "libslic3r/Fill/VoronoiRouting.hpp"
#include "libslic3r/Fill/VoronoiWallDistance.hpp"
#include "libslic3r/Fill/VoronoiMaterialDensity.hpp"
#include "libslic3r/Fill/VoronoiLattice.hpp"
#include "libslic3r/Fill/VoronoiWallSnap.hpp"
#include "libslic3r/Fill/VoronoiRelaxation.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/EdgeGrid.hpp"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <vector>
#include <stdexcept>
#include <utility>

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
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, spacing, density, z, spacing, 0.).walls;
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
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, z, 0.45, 0.).walls;
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
    REQUIRE(infill.fill_geometry(bounds, region, 0.45, 0.2, 0., 0.45, 0.).walls.size() == 1);
    const Polylines middle = infill.fill_geometry(bounds, region, 0.45, 0.2, 10., 0.45, 0.).walls;
    REQUIRE(middle.size() == 2);
    for (const Polyline &path : middle)
        REQUIRE_THAT(std::abs(unscale_(path.first_point().x())), Catch::Matchers::WithinAbs(5., EPSILON));
    REQUIRE(infill.fill_geometry(bounds, region, 0.45, 0.2, 20., 0.45, 0.).walls.empty());
}

TEST_CASE("Voronoi slice walls separate the nearest three dimensional sites", "[FillVoronoi]")
{
    const double z = GENERATE(-3., 4., 12., 20.);
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-12., -8., -6.), Vec3d(12., -8., 2.), Vec3d(0., 14., 10.), Vec3d(-2., -1., 25.)};
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, z, 0.45, 0.).walls;
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
    const Segments first = segments(infill.fill_geometry(bounds, region, 0.45, 0.15, 0.1, 0.45, 0.).walls);
    REQUIRE_FALSE(provider->queries.empty());
    REQUIRE(first != segments(infill.fill_geometry(bounds, region, 0.45, 0.15, 0.2, 0.45, 0.).walls));
    infill.fill_geometry(bounds, region, 0.45, 0.15, 1000., 0.45, 0.);
    // Jumping past the active range starts locally, without ingesting the gap.
    REQUIRE(provider->queries.back().min.z() > 900.);
    REQUIRE(first == segments(infill.fill_geometry(bounds, region, 0.45, 0.15, 0.1, 0.45, 0.).walls));
}

TEST_CASE("Voronoi infill repeats at the same height across independently filled regions", "[FillVoronoi]")
{
    const ExPolygon whole = rectangle(-50., -50., 50., 50.);
    const ExPolygon part = rectangle(-20., -10., 30., 40.);
    const BoundingBox bounds = whole.contour.bounding_box();
    VoronoiInfill first, second;
    const Polylines full = first.fill_geometry(bounds, whole, 0.45, 0.15, 0., 0.45, 0.).walls;
    const Polylines partial = second.fill_geometry(bounds, part, 0.45, 0.15, 0., 0.45, 0.).walls;
    REQUIRE_FALSE(full.empty());
    REQUIRE(segments(full) == segments(second.fill_geometry(bounds, whole, 0.45, 0.15, 0., 0.45, 0.).walls));
    REQUIRE(segments(intersection_pl(full, part)) == segments(partial));
    // Changing a cached density and changing it back must not change the pattern.
    first.fill_geometry(bounds, whole, 0.45, 0.30, 0., 0.45, 0.);
    REQUIRE(segments(full) == segments(first.fill_geometry(bounds, whole, 0.45, 0.15, 0., 0.45, 0.).walls));
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
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.20, 0., 0.45, 0.).walls;
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
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), region, 0.45, density, 0., 0.45, 0.).walls.empty());
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), region, 0., 0.2, 0., 0., 0.).walls.empty());
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), region, std::numeric_limits<double>::denorm_min(), 0.2, 0., std::numeric_limits<double>::denorm_min(), 0.).walls.empty());
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), ExPolygon{}, 0.45, 0.2, 0., 0.45, 0.).walls.empty());
}

TEST_CASE("Voronoi bisectors reach the boundary for collinear sites", "[FillVoronoi]")
{
    const size_t count = GENERATE(size_t(2), size_t(3));
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    FixedSites infill;
    infill.sites = {Vec3d(-10., 0., 0.), Vec3d(10., 0., 0.)};
    if (count == 3)
        infill.sites.push_back(Vec3d(0., 0., 0.));
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, 0., 0.45, 0.).walls;
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
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, 0., 0.45, 0.).walls;
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
    const Polylines paths = infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, 0., 0.45, 0.).walls;
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
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, 0., 0.45, 0.).walls.empty());
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
    const auto below = infill.fill_geometry(bounds, region, 0.4, 0.2, 4.8, 0.45, 0.2);
    const auto current = infill.fill_geometry(bounds, region, 0.4, 0.2, 5., 0.45, 0.2);
    const auto above = infill.fill_geometry(bounds, region, 0.4, 0.2, 5.2, 0.45, 0.2);
    REQUIRE(path_length(current.walls) < SCALED_EPSILON);
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
    const auto below = infill.fill_geometry(bounds, region, spacing, 0.2, 5. - height, 0.45, height);
    const auto current = infill.fill_geometry(bounds, region, spacing, 0.2, 5., 0.45, height);
    const auto above = infill.fill_geometry(bounds, region, spacing, 0.2, 5. + height, 0.45, height);
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
    const auto layer = infill.fill_geometry(region.contour.bounding_box(), region, 0.4, 0.2, 10., width, 0.2);
    REQUIRE(layer.bands.empty());
    REQUIRE(layer.walls.size() == 1);
    // At the same height a narrower bead needs a band instead; cached geometry
    // must include extrusion dimensions, not just the layer Z coordinate.
    const auto narrow = infill.fill_geometry(region.contour.bounding_box(), region, 0.2, 0.2, 10., 0.3, 0.2);
    REQUIRE(path_length(narrow.walls) < SCALED_EPSILON);
    REQUIRE(narrow.bands.size() == 1);
}

TEST_CASE("Voronoi horizontal faces fill once and stop at neighboring cells", "[FillVoronoi]")
{
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    const BoundingBox bounds = region.contour.bounding_box();
    FixedSites infill;
    infill.sites = {Vec3d(0., 0., 0.), Vec3d(0., 0., 1.), Vec3d(4., 0., 0.5),
                    Vec3d(-4., 0., 0.5), Vec3d(0., 4., 0.5), Vec3d(0., -4., 0.5)};
    REQUIRE(infill.fill_geometry(bounds, region, 0.4, 0.2, 0.25, 0.45, 0.25).bands.empty());
    REQUIRE(infill.fill_geometry(bounds, region, 0.4, 0.2, 0.75, 0.45, 0.25).bands.empty());
    const auto floor = infill.fill_geometry(bounds, region, 0.4, 0.2, 0.5, 0.45, 0.25);
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
    const auto layer = infill.fill_geometry(region.contour.bounding_box(), region, 0.4, 0.2, 0., 0.45, 0.2);
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
    const auto layer = infill.fill_geometry(region.contour.bounding_box(), region, 0.4, density, 5., 0.45, height);
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
    infill.fill_geometry(bounds, region, 0.4, 0.2, 0.3, 0.6, 0.3, 0.2);
    REQUIRE_FALSE(provider->spacings.empty());
    const double initial_spacing = provider->spacings.front();
    infill.fill_geometry(bounds, region, 0.4, 0.2, 20., 0.45, 0.12, 0.2);
    for (double spacing : provider->spacings)
        REQUIRE_THAT(spacing, Catch::Matchers::WithinAbs(initial_spacing, EPSILON));
}

TEST_CASE("Voronoi overlapping bands have reproducible ownership across independent fills", "[FillVoronoi]")
{
    const ExPolygon region = rectangle(-20., -20., 20., 20.);
    const BoundingBox bounds = region.contour.bounding_box();
    VoronoiInfill first, second, unrelated;
    const auto expected = first.fill_geometry(bounds, region, 0.4, 0.2, 5., 0.45, 0.2);
    unrelated.fill_geometry(bounds, region, 0.4, 0.3, 13., 0.45, 0.3);
    const auto actual = second.fill_geometry(bounds, region, 0.4, 0.2, 5., 0.45, 0.2);
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
    std::set<size_t> seen;
    for (const auto &group : ordered) {
        const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto &expected) {
            return expected.front().first_point() == group.paths.front().first_point();
        });
        REQUIRE(found != groups.end());
        REQUIRE(seen.insert(size_t(found - groups.begin())).second);
        REQUIRE(group.paths.size() == found->size());
        for (size_t j = 0; j < group.paths.size(); ++j)
            REQUIRE(group.paths[j].points == (*found)[j].points);
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
    // A reversed proxy must still expand from the supported side.
    const auto only_floor = Voronoi::order_groups({{floor, false}}, floor_exit);
    REQUIRE(only_floor.size() == 1);
    REQUIRE(only_floor.front().paths.size() == floor.size());
    for (size_t i = 0; i < floor.size(); ++i)
        REQUIRE(only_floor.front().paths[i].points == floor[i].points);
}

TEST_CASE("Voronoi keeps supporting walls between horizontal floor faces", "[FillVoronoi]")
{
    FixedSites infill;
    for (int x = -1; x <= 4; ++x)
        for (int y = -1; y <= 4; ++y)
            for (int z = -1; z <= 2; ++z)
                infill.sites.emplace_back(20. * x, 20. * y, 20. * z);
    const ExPolygon region = rectangle(0., 0., 50., 50.);
    const auto layer = infill.fill_geometry(region.contour.bounding_box(), region, .4, .05, 10.1, .45, .2);
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
    const double angle = GENERATE(0., 0.37, -1.2);
    Voronoi::PoissonPointCloud provider(angle, Vec2d(2.5, -1.7));
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

TEST_CASE("Random cloud rotation preserves points and thinning in a spherical field", "[FillVoronoi][Rotation]")
{
    const double angle = GENERATE(0.37, -1.2);
    const Vec2d center(4., -3.);
    const BoundingBoxf3 box(Vec3d(-16., -23., -20.), Vec3d(24., 17., 20.));
    auto distance = [center](const std::vector<Vec3d> &points) {
        std::vector<Voronoi::WallDistanceSample> samples;
        for (const auto &p : points) {
            const double d = (p - Vec3d(center.x(), center.y(), 0.)).norm() - 10.;
            samples.push_back({d, Voronoi::WallDistancePointCloud::density_ratio(d, d)});
        }
        return samples;
    };
    const bool thinned = GENERATE(false, true);
    std::shared_ptr<const Voronoi::PointCloudProvider> original, rotated;
    if (thinned) {
        original = std::make_shared<Voronoi::WallDistancePointCloud>(distance, Voronoi::WallDistancePointCloud::density_ratio(0., 2.));
        rotated = std::make_shared<Voronoi::WallDistancePointCloud>(distance, Voronoi::WallDistancePointCloud::density_ratio(0., 2.), angle, center);
    } else {
        original = std::make_shared<Voronoi::PoissonPointCloud>();
        rotated = std::make_shared<Voronoi::PoissonPointCloud>(angle, center);
    }
    auto interior = [center](std::vector<Vec3d> sites) {
        sites.erase(std::remove_if(sites.begin(), sites.end(), [center](const auto &p) {
            return (p - Vec3d(center.x(), center.y(), 0.)).norm() >= 10.;
        }), sites.end());
        return sites;
    };
    const auto source = interior(original->points_in(box, 3.));
    const auto result = interior(rotated->points_in(box, 3.));
    REQUIRE_FALSE(source.empty());
    REQUIRE(result.size() == source.size());
    for (const auto &p : source) {
        const Vec2d offset = p.head<2>() - center;
        const Vec3d expected(center.x() + std::cos(angle) * offset.x() - std::sin(angle) * offset.y(),
                             center.y() + std::sin(angle) * offset.x() + std::cos(angle) * offset.y(), p.z());
        const auto nearest = std::min_element(result.begin(), result.end(), [&](const auto &a, const auto &b) {
            return (a - expected).squaredNorm() < (b - expected).squaredNorm();
        });
        REQUIRE_THAT((*nearest - expected).norm(), Catch::Matchers::WithinAbs(0., 1e-12));
    }
}

TEST_CASE("Wall-distance thinning samples rotated candidates in model coordinates", "[FillVoronoi][Rotation]")
{
    const BoundingBoxf3 box(Vec3d(-12., -10., -5.), Vec3d(12., 10., 5.));
    std::vector<Vec3d> sampled;
    Voronoi::WallDistancePointCloud rotated([&](const auto &points) {
        sampled = points;
        return std::vector<Voronoi::WallDistanceSample>(points.size(), {-1., 1.});
    }, Voronoi::WallDistancePointCloud::density_ratio(0., 2.), 0.37, Vec2d(2., -3.));
    const auto retained = rotated.points_in(box, 3.);
    const auto candidates = Voronoi::PoissonPointCloud(0.37, Vec2d(2., -3.)).points_in(
        box, 3. / Voronoi::WallDistancePointCloud::density_ratio(0., 2.));
    REQUIRE(sampled == candidates);
    REQUIRE_FALSE(retained.empty());
    const BoundingBoxf3 first(box.min, Vec3d(0., box.max.y(), box.max.z()));
    const BoundingBoxf3 second(Vec3d(0., box.min.y(), box.min.z()), box.max);
    auto parts = rotated.points_in(first, 3.);
    const auto rest = rotated.points_in(second, 3.);
    parts.insert(parts.end(), rest.begin(), rest.end());
    auto keys = [](const auto &points) {
        std::set<std::array<double, 3>> result;
        for (const auto &p : points) result.insert({p.x(), p.y(), p.z()});
        return result;
    };
    REQUIRE(keys(parts) == keys(retained));
    REQUIRE_THROWS_AS(Voronoi::PoissonPointCloud(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
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
        Voronoi::CellGeometry actual_mesh;
        actual_mesh.update(cloud.sites());
        const auto actual = actual_mesh.shallow_bands(bounds, z, width, height, row_spacing);
        Voronoi::CellGeometry expected_mesh;
        expected_mesh.update(provider->sites);
        const auto expected = expected_mesh.shallow_bands(bounds, z, width, height, row_spacing);
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
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, 0., 0.45, 0.).walls.empty());
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
    auto constant = [](const std::vector<Vec3d> &points) { return std::vector<Voronoi::WallDistanceSample>(points.size(), {-6., Voronoi::WallDistancePointCloud::density_ratio(-6., -6.)}); };
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

TEST_CASE("Voronoi wall cloud makes smaller cells near model walls", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-40., -40., 40., 40.)};
    auto field = std::make_shared<Voronoi::CubicWallDistance>(std::vector<Voronoi::WallSlice>{{-100., 100., &outlines}});
    auto provider = std::make_shared<Voronoi::WallDistancePointCloud>([field](const auto &points) { return field->sample_fields(points); });
    VoronoiInfill infill(provider);
    const auto bounds = outlines.front().contour.bounding_box();
    const auto walls = infill.fill_geometry(bounds, outlines.front(), 0.45, 0.25, 0., 0.45, 0.).walls;
    REQUIRE_FALSE(walls.empty());
    const auto near_wall = rectangle(30., -30., 38., 30.);
    const auto center = rectangle(-4., -30., 4., 30.);
    REQUIRE(path_length(intersection_pl(walls, near_wall)) > path_length(intersection_pl(walls, center)));
    REQUIRE(segments(infill.fill_geometry(bounds, outlines.front(), 0.45, 0.25, 5., 0.45, 0.).walls) != segments(walls));
    REQUIRE(segments(infill.fill_geometry(bounds, outlines.front(), 0.45, 0.25, 0., 0.45, 0.).walls) == segments(walls));
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

TEST_CASE("Voronoi Gaussian smoothing preserves flat-wall material density", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-30., -30., 30., 30.)};
    Voronoi::CubicWallDistance field({{0., 60., &outlines}});
    for (const auto &sample : field.sample_fields({Vec3d(0., 0., -2.), Vec3d(0., 0., 0.),
                Vec3d(0., 0., 2.), Vec3d(0., 0., 6.), Vec3d(28., 0., 30.)}))
        REQUIRE_THAT(sample.density, Catch::Matchers::WithinRel(
            Voronoi::WallDistancePointCloud::density_ratio(sample.distance, sample.distance), 0.04));
}

TEST_CASE("Voronoi Gaussian smoothing increases density at convex and concave corners", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-20., -20., 20., 20.)};
    Voronoi::CubicWallDistance convex({{0., 40., &outlines}});
    const auto outer = convex.sample_fields({Vec3d(14., 14., 20.), Vec3d(14., 0., 20.),
                                             Vec3d(14., 0., 34.), Vec3d(0., 14., 34.)});
    using Cloud = Voronoi::WallDistancePointCloud;
    REQUIRE(outer[0].density >
            Cloud::density_ratio(outer[0].distance, outer[0].distance) + 0.03);
    REQUIRE_THAT(outer[1].density, Catch::Matchers::WithinRel(Cloud::density_ratio(outer[1].distance, outer[1].distance), 0.04));
    REQUIRE_THAT(outer[2].density, Catch::Matchers::WithinRel(outer[0].density, 0.04));
    REQUIRE_THAT(outer[3].density, Catch::Matchers::WithinRel(outer[0].density, 0.04));
    ExPolygon hollow = rectangle(-30., -30., 30., 30.);
    hollow.holes.push_back(rectangle(-10., -10., 10., 10.).contour);
    hollow.holes.back().reverse();
    const ExPolygons hole {hollow};
    Voronoi::CubicWallDistance concave({{0., 40., &hole}});
    const auto inner = concave.sample_fields({Vec3d(14., 14., 20.)}).front();
    REQUIRE(inner.density >
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
    REQUIRE_THAT(field.sample_fields({center}).front().density,
                 Catch::Matchers::WithinRel(Voronoi::WallDistancePointCloud::density_ratio(field.sample({center}).front(), expected / weight_sum), 0.04));
}

TEST_CASE("Voronoi Gaussian smoothing remains planar when its radius exceeds the halo", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-50., -50., 50., 50.)};
    Voronoi::CubicWallDistance field({{0., 100., &outlines}}, 8.);
    const auto samples = field.sample_fields({Vec3d(0., 0., -2.), Vec3d(0., 0., 0.),
                                               Vec3d(0., 0., 4.), Vec3d(46., 0., 50.)});
    for (const auto &sample : samples)
        REQUIRE_THAT(sample.density, Catch::Matchers::WithinRel(Voronoi::WallDistancePointCloud::density_ratio(sample.distance, sample.distance), 0.06));
}

TEST_CASE("Voronoi Gaussian smoothing accepts zero width and rejects invalid widths", "[FillVoronoi]")
{
    const ExPolygons outlines {rectangle(-10., -10., 10., 10.)};
    Voronoi::CubicWallDistance field({{0., 20., &outlines}}, 0.);
    for (const auto &sample : field.sample_fields({Vec3d(0., 0., 10.), Vec3d(0., 0., -1.), Vec3d(9.5, 0., 10.)}))
        REQUIRE_THAT(sample.density, Catch::Matchers::WithinRel(Voronoi::WallDistancePointCloud::density_ratio(sample.distance, sample.distance), 0.04));
    REQUIRE_THROWS_AS(Voronoi::CubicWallDistance({{0., 20., &outlines}}, -1.), std::invalid_argument);
    REQUIRE_THROWS_AS(Voronoi::CubicWallDistance({{0., 20., &outlines}}, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}

TEST_CASE("Voronoi thinning samples prepared material density once per candidate", "[FillVoronoi]")
{
    size_t queried = 0;
    Voronoi::WallDistancePointCloud cloud([&](const auto &points) {
        queried += points.size();
        return std::vector<Voronoi::WallDistanceSample>(points.size(), {-6., Voronoi::WallDistancePointCloud::density_ratio(-6., -5.)});
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
    REQUIRE_THAT(count({-1., 1.}), Catch::Matchers::WithinRel(1., 0.1));
    REQUIRE_THAT(count({-2., Cloud::density_ratio(-2., -1.)}), Catch::Matchers::WithinRel(std::pow(Cloud::density_ratio(-2., -1.), 3), 0.1));
    REQUIRE_THAT(count({-1., maximum}), Catch::Matchers::WithinRel(std::pow(maximum, 3), 0.1));
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
    REQUIRE(loads == 2);
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

TEST_CASE("Point-cloud orientation uses the object centre and shares wall-distance tables", "[FillVoronoi][Rotation]")
{
    const auto method = GENERATE(VoronoiCloudMethod::Random, VoronoiCloudMethod::Fcc, VoronoiCloudMethod::Cubic);
    Voronoi::WallDistanceCloudCache cache;
    const ExPolygons outlines {rectangle(-2., -9., 10., 3.)};
    const auto slices = [&] { return std::vector<Voronoi::WallSlice>{{-6., 6., &outlines}}; };
    const double decay = GENERATE(0., 6.);
    const double angle = 0.37;
    const auto original = cache.get(slices, decay, 0., {}, 0., method);
    const auto rotated = cache.get(slices, decay, 0., [](auto stage, double) {
        if (stage == Voronoi::WallDistanceStage::Distance) throw std::runtime_error("Distances already cached");
    }, 0., method, 0, angle);
    REQUIRE(rotated != original);
    REQUIRE(rotated == cache.get(slices, decay, 0., {}, 0., method, 0, angle));
    REQUIRE(original == cache.get(slices, decay, 0., {}, 0., method, 0, 2. * std::acos(-1.)));
    const BoundingBoxf3 box(Vec3d(-20., -20., -10.), Vec3d(20., 20., 10.));
    if (decay == 0.) {
        if (method == VoronoiCloudMethod::Random)
            REQUIRE(rotated->points_in(box, 3.) == Voronoi::PoissonPointCloud(angle, Vec2d(4., -3.)).points_in(box, 3.));
        else
            REQUIRE(rotated->points_in(box, 3.) == Voronoi::LatticePointCloud(method, {}, 1., angle, Vec2d(4., -3.)).points_in(box, 3.));
    }
    REQUIRE(rotated->relative_density({Vec3d(4., -3., 0.)}) == original->relative_density({Vec3d(4., -3., 0.)}));
    const auto relaxed = cache.get(slices, decay, 0., {}, 3., method, 1, angle);
    REQUIRE(relaxed == cache.get(slices, decay, 0., {}, 3., method, 1, angle));
    REQUIRE(relaxed != cache.get(slices, decay, 0., {}, 3., method, 1));
}

TEST_CASE("Spring relaxation balances interior edges while holding the cloud boundary", "[FillVoronoi][Relaxation]")
{
    auto source = std::make_shared<FixedProvider>();
    for (double x : {-3., 3.})
        for (double y : {-3., 3.})
            for (double z : {-3., 3.}) source->sites.emplace_back(x, y, z);
    const Vec3d off_center(0.8, 0.4, 0.);
    source->sites.push_back(off_center);
    const BoundingBoxf3 all(Vec3d::Constant(-4.), Vec3d::Constant(4.));
    Voronoi::RelaxedPointCloud cloud(source, all, 2., 5);
    const auto sites = cloud.points_in(all, 2.);
    REQUIRE(sites.size() == source->sites.size());
    for (size_t i = 0; i < 8; ++i) REQUIRE(std::find(sites.begin(), sites.end(), source->sites[i]) != sites.end());
    const auto center = std::find_if(sites.begin(), sites.end(), [](const Vec3d &p) { return p.cwiseAbs().maxCoeff() < 3.; });
    REQUIRE(center != sites.end());
    REQUIRE(center->norm() < off_center.norm());
    REQUIRE((*center - off_center).norm() <= 5. * 0.1 * 2.);
    auto split = cloud.points_in(BoundingBoxf3(all.min, Vec3d(4., 4., 0.)), 2.);
    append(split, cloud.points_in(BoundingBoxf3(Vec3d(-4., -4., 0.), all.max), 2.));
    REQUIRE(split == sites);
    const Voronoi::RelaxedPointCloud rebuilt(source, all, 2., 5);
    REQUIRE(rebuilt.points_in(all, 2.) == sites);
}

TEST_CASE("Spring relaxation follows the provider's preferred local spacing", "[FillVoronoi][Relaxation]")
{
    class SpacedProvider : public FixedProvider {
    public:
        double target = 2.;
        std::vector<double> local_spacing(const std::vector<Vec3d> &points, double) const override
            { return std::vector<double>(points.size(), target); }
    };
    auto source = std::make_shared<SpacedProvider>();
    for (double x : {-3., 3.})
        for (double y : {-3., 3.})
            for (double z : {-3., 3.}) source->sites.emplace_back(x, y, z);
    const Vec3d initial(0.8, 0.4, 0.);
    source->sites.push_back(initial);
    const BoundingBoxf3 all(Vec3d::Constant(-4.), Vec3d::Constant(4.));
    const auto moved_center = [&](double target) {
        source->target = target;
        const Voronoi::RelaxedPointCloud cloud(source, all, 2., 1);
        const auto points = cloud.points_in(all, 2.);
        const auto center = std::find_if(points.begin(), points.end(), [](const Vec3d &p) { return p.cwiseAbs().maxCoeff() < 3.; });
        REQUIRE(center != points.end());
        return Vec3d(*center);
    };
    REQUIRE(moved_center(2.).norm() < initial.norm());
    REQUIRE(moved_center(16.).norm() > initial.norm());
}

TEST_CASE("Relaxation spacing retains the density falloff outside the model", "[FillVoronoi][Relaxation]")
{
    const Voronoi::WallDistancePointCloud source([](const std::vector<Vec3d> &points) {
        std::vector<Voronoi::WallDistanceSample> result;
        for (const auto &p : points) result.push_back({p.x(), Voronoi::WallDistancePointCloud::density_ratio(p.x(), p.x())});
        return result;
    });
    const auto spacing = source.local_spacing({Vec3d(-10., 0., 0.), Vec3d(10., 0., 0.), Vec3d(-1., 0., 0.)}, 4.);
    REQUIRE_THAT(spacing[0], Catch::Matchers::WithinAbs(spacing[1], EPSILON));
    REQUIRE(spacing[0] > spacing[2]);
    REQUIRE_THAT(spacing[2], Catch::Matchers::WithinAbs(4., EPSILON));
}

TEST_CASE("Zero relaxation retains the original provider without preparing an object cloud", "[FillVoronoi][Relaxation]")
{
    Voronoi::WallDistanceCloudCache cache;
    int loads = 0;
    const auto slices = [&] { ++loads; return std::vector<Voronoi::WallSlice>{}; };
    const auto original = cache.get(slices, 0., 0.);
    REQUIRE(cache.get(slices, 0., 0., {}, 4., VoronoiCloudMethod::Random, 0) == original);
    REQUIRE_FALSE(original->extent().has_value());
    REQUIRE(loads == 0);
}

TEST_CASE("Canceled point relaxation leaves the generated provider reusable", "[FillVoronoi][Relaxation]")
{
    const auto method = GENERATE(VoronoiCloudMethod::Random, VoronoiCloudMethod::Fcc, VoronoiCloudMethod::Cubic);
    const ExPolygons outlines {rectangle(0., 0., 12., 12.)};
    Voronoi::WallDistanceCloudCache cache;
    const auto slices = [&] { return std::vector<Voronoi::WallSlice>{{0., 12., &outlines}}; };
    const auto original = cache.get(slices, 0., 0., {}, 3., method);
    REQUIRE_THROWS_AS(cache.get(slices, 0., 0., [](Voronoi::WallDistanceStage stage, double fraction) {
        if (stage == Voronoi::WallDistanceStage::Relaxation && fraction > 0.1) throw std::runtime_error("Canceled");
    }, 3., method, 3), std::runtime_error);
    REQUIRE(original == cache.get(slices, 0., 0., {}, 3., method));
    const auto relaxed = cache.get(slices, 0., 0., {}, 3., method, 3);
    REQUIRE(relaxed == cache.get(slices, 0., 0., {}, 3., method, 3));
    REQUIRE(relaxed != original);
    REQUIRE(relaxed->extent()->defined);
}

TEST_CASE("FCC has twelve nearest neighbours and a vertical body diagonal", "[FillVoronoi][Fcc]")
{
    const double angle = GENERATE(0., 0.37);
    const Voronoi::LatticePointCloud cloud(VoronoiCloudMethod::Fcc, {}, 1., angle);
    const auto sites = cloud.points_in(BoundingBoxf3(Vec3d::Constant(-8.), Vec3d::Constant(8.)), 3.);
    std::vector<Vec3d> neighbours;
    for (const auto &p : sites)
        if (p.norm() > 1e-9) neighbours.push_back(p);
    std::sort(neighbours.begin(), neighbours.end(), [](const auto &a, const auto &b) { return a.norm() < b.norm(); });
    REQUIRE(neighbours.size() > 12);
    const double edge = std::pow(2., 1. / 6.) * 3.;
    int vertical_faces = 0;
    for (size_t i = 0; i < 12; ++i) {
        REQUIRE_THAT(neighbours[i].norm(), Catch::Matchers::WithinAbs(edge, 1e-9));
        const double cosine = std::abs(neighbours[i].z()) / edge;
        if (cosine < 1e-9) ++vertical_faces;
        else REQUIRE_THAT(cosine, Catch::Matchers::WithinAbs(std::sqrt(2. / 3.), 1e-9));
    }
    REQUIRE(vertical_faces == 6);
    REQUIRE(neighbours[12].norm() > edge * 1.1);
}

TEST_CASE("Cubic sites have six orthogonal neighbours and a vertical body diagonal", "[FillVoronoi][Lattice]")
{
    const double angle = GENERATE(0., 0.37);
    const Voronoi::LatticePointCloud cloud(VoronoiCloudMethod::Cubic, {}, 1., angle);
    const auto sites = cloud.points_in(BoundingBoxf3(Vec3d::Constant(-8.), Vec3d::Constant(8.)), 3.);
    std::vector<Vec3d> neighbours;
    for (const auto &p : sites)
        if (p.norm() > 1e-9) neighbours.push_back(p);
    std::sort(neighbours.begin(), neighbours.end(), [](const auto &a, const auto &b) { return a.norm() < b.norm(); });
    REQUIRE(neighbours.size() > 6);
    for (size_t i = 0; i < 6; ++i) {
        REQUIRE_THAT(neighbours[i].norm(), Catch::Matchers::WithinAbs(3., 1e-9));
        REQUIRE_THAT(std::abs(neighbours[i].z()) / 3., Catch::Matchers::WithinAbs(1. / std::sqrt(3.), 1e-9));
        for (size_t j = i + 1; j < 6; ++j) {
            const double cosine = neighbours[i].dot(neighbours[j]) / 9.;
            REQUIRE_THAT(cosine, Catch::Matchers::WithinAbs(cosine < -0.5 ? -1. : 0., 1e-9));
        }
    }
    REQUIRE_THAT(neighbours[6].norm(), Catch::Matchers::WithinAbs(3. * std::sqrt(2.), 1e-9));
}

TEST_CASE("FCC thinning follows cubic tetrahedra and coarser checkerboards", "[FillVoronoi][Fcc]")
{
    const double retention = GENERATE(0.75, 0.5, 0.25, 0.125, 0.015625);
    const double angle = GENERATE(0., 0.37);
    const double decay = 6., spacing = 3.;
    const double near = Voronoi::WallDistancePointCloud::near_wall_distance;
    const double maximum = Voronoi::WallDistancePointCloud::density_ratio(0., near, 0.);
    const double target = maximum * std::cbrt(retention);
    // Find a constant distance sample giving this retention through the public
    // density function, independently of its particular decay curve.
    double low = 0., high = 64.;
    for (int step = 0; step < 48; ++step) {
        const double depth = 0.5 * (low + high);
        if (Voronoi::WallDistancePointCloud::density_ratio(-std::max(near, depth), -depth, decay) > target)
            low = depth;
        else high = depth;
    }
    const double depth = 0.5 * (low + high);
    const auto samples = [=](const auto &points) {
        return std::vector<Voronoi::WallDistanceSample>(points.size(), {-std::max(near, depth), Voronoi::WallDistancePointCloud::density_ratio(-std::max(near, depth), -depth, decay)});
    };
    const auto saturated = [=](const auto &points) {
        return std::vector<Voronoi::WallDistanceSample>(points.size(), {0., Voronoi::WallDistancePointCloud::density_ratio(0., near, decay)});
    };
    const BoundingBoxf3 box(Vec3d::Constant(-12.), Vec3d::Constant(12.));
    const auto selected = Voronoi::LatticePointCloud(VoronoiCloudMethod::Fcc, samples, maximum, angle).points_in(box, spacing);
    const auto candidates = Voronoi::LatticePointCloud(VoronoiCloudMethod::Fcc, saturated, maximum, angle).points_in(box, spacing);
    Eigen::Matrix3d orientation;
    orientation.row(0) = Vec3d(1., -1., 0.).normalized();
    orientation.row(1) = Vec3d(1., 1., -2.).normalized();
    orientation.row(2) = Vec3d(1., 1., 1.).normalized();
    const double half_cube = std::pow(2., 1. / 6.) * spacing / maximum / std::sqrt(2.);
    const auto coordinates = [&](const Vec3d &p) {
        const Vec3d untilted = orientation.transpose() * Vec3d(std::cos(angle) * p.x() + std::sin(angle) * p.y(),
                                                             -std::sin(angle) * p.x() + std::cos(angle) * p.y(), p.z());
        return std::array<int64_t, 3>{std::llround(untilted.x() / half_cube), std::llround(untilted.y() / half_cube),
                                      std::llround(untilted.z() / half_cube)};
    };
    std::set<std::array<int64_t, 3>> actual, expected;
    for (const auto &p : selected) REQUIRE(actual.insert(coordinates(p)).second);
    for (const auto &p : candidates) {
        const auto q = coordinates(p);
        const bool u = q[0] % 2 != 0, v = q[1] % 2 != 0, w = q[2] % 2 != 0;
        const int64_t scale = retention == 0.015625 ? 4 : 2;
        const bool keep = retention == 0.75 ? !(u && v) : retention == 0.5 ? !u :
                          retention == 0.25 ? !u && !v && !w :
                          q[0] % scale == 0 && q[1] % scale == 0 && q[2] % scale == 0 &&
                          (q[0] + q[1] + q[2]) / scale % 2 == 0;
        if (keep) expected.insert(q);
    }
    REQUIRE_FALSE(expected.empty());
    REQUIRE(actual == expected);
}

TEST_CASE("Lattice thinning follows density and preserves partitioned queries", "[FillVoronoi][Lattice]")
{
    const auto method = GENERATE(VoronoiCloudMethod::Fcc, VoronoiCloudMethod::Cubic);
    const double decay = GENERATE(0., 6.);
    const double angle = GENERATE(0., 0.37);
    const double maximum = Voronoi::WallDistancePointCloud::density_ratio(0., 2., 0.);
    const auto sample = [decay](const std::vector<Vec3d> &points) {
        return std::vector<Voronoi::WallDistanceSample>(points.size(), {-6., Voronoi::WallDistancePointCloud::density_ratio(-6., -6., decay)});
    };
    const Voronoi::LatticePointCloud cloud(method, sample, maximum, angle);
    const BoundingBoxf3 box(Vec3d(-50.3, -49.7, -48.8), Vec3d(51.2, 50.4, 49.9));
    const auto whole = cloud.points_in(box, 3.);
    REQUIRE_FALSE(whole.empty());
    auto keys = [](const auto &points) {
        std::set<std::array<double, 3>> result;
        for (const auto &p : points) REQUIRE(result.insert({p.x(), p.y(), p.z()}).second);
        return result;
    };
    const int axis = GENERATE(0, 1, 2);
    BoundingBoxf3 first = box, second = box;
    first.max[axis] = second.min[axis] = 0.37;
    auto parts = cloud.points_in(first, 3.);
    append(parts, cloud.points_in(second, 3.));
    REQUIRE(keys(parts) == keys(whole));
    const auto candidates = Voronoi::LatticePointCloud(method, {}, 1., angle).points_in(box, 3. / maximum);
    const double probability = std::pow(Voronoi::WallDistancePointCloud::density_ratio(-6., -6., decay) / maximum, 3.);
    REQUIRE_THAT(double(whole.size()) / candidates.size(), Catch::Matchers::WithinRel(probability, 0.08));
    REQUIRE(cloud.points_in(box, 3.) == whole);
}

TEST_CASE("Boundary snapping moves nearby points on either side of a wall and preserves queries", "[FillVoronoi][WallSnap]")
{
    auto source = std::make_shared<FixedProvider>();
    source->sites = {Vec3d(-0.9, 0., 0.), Vec3d(0.9, 1., 0.), Vec3d(-1.4, 2., 0.),
                     Vec3d(1.6, 3., 0.), Vec3d(0., 4., 0.)};
    const auto raw_plane = [](const std::vector<Vec3d> &points) {
        std::vector<Voronoi::WallDistanceSample> result;
        for (const auto &p : points) result.push_back({p.x(), p.x() - 5.});
        return result;
    };
    const Voronoi::SnappedPointCloud cloud(source, raw_plane, 3., 1.);
    const BoundingBoxf3 box(Vec3d(-2., -1., -1.), Vec3d(2., 5., 1.));
    const auto sites = cloud.points_in(box, 3.);
    REQUIRE(sites.size() == source->sites.size());
    REQUIRE_THAT(sites[0].x(), Catch::Matchers::WithinAbs(0., 1e-12));
    REQUIRE_THAT(sites[1].x(), Catch::Matchers::WithinAbs(0., 1e-12));
    REQUIRE_THAT(sites[2].x(), Catch::Matchers::WithinAbs(0., 1e-12));
    REQUIRE(sites[3] == source->sites[3]);
    REQUIRE(sites[4] == source->sites[4]);
    REQUIRE(cloud.relative_density({Vec3d::Zero()}) == source->relative_density({Vec3d::Zero()}));
    const double split = GENERATE(0., 0.5);
    const auto left = cloud.points_in(BoundingBoxf3(box.min, Vec3d(split, 5., 1.)), 3.);
    const auto right = cloud.points_in(BoundingBoxf3(Vec3d(split, -1., -1.), box.max), 3.);
    REQUIRE(left.size() + right.size() == sites.size());
    for (const auto &p : sites) {
        const size_t occurrences = std::count(left.begin(), left.end(), p) + std::count(right.begin(), right.end(), p);
        REQUIRE(occurrences == 1);
    }
}

TEST_CASE("Boundary snapping caps displacement and leaves undefined normals unchanged", "[FillVoronoi][WallSnap]")
{
    auto source = std::make_shared<FixedProvider>();
    source->sites = {Vec3d(-2., 0., 0.), Vec3d(0., 0., 0.)};
    const bool flat = GENERATE(false, true);
    const auto field = [flat](const std::vector<Vec3d> &points) {
        std::vector<Voronoi::WallDistanceSample> result;
        for (const auto &p : points) {
            const double d = flat ? 0.5 : 0.1 * p.x();
            result.push_back({d, d});
        }
        return result;
    };
    const Voronoi::SnappedPointCloud cloud(source, field, 3., 1.);
    const auto sites = cloud.points_in(BoundingBoxf3(Vec3d::Constant(-4.), Vec3d::Constant(4.)), 3.);
    REQUIRE(sites.size() == 2);
    REQUIRE_THAT((sites[0] - source->sites[0]).norm(), Catch::Matchers::WithinAbs(flat ? 0. : 1.5, 1e-12));
    REQUIRE(sites[1] == source->sites[1]);
}

TEST_CASE("Shell tees share boundary edges and cycle all three pairings", "[FillVoronoi][Boundary]")
{
    const bool persistent_sites = GENERATE(false, true);
    const auto boundary = to_polylines(rectangle(0., -2., 2., 2.));
    std::set<size_t> unpaired;
    int turns = 0;
    for (size_t layer = 0; layer < 3; ++layer) {
        const double y = persistent_sites ? 0.11 * layer : 0.;
        Polylines walls {Polyline{Points{Point::new_scale(1., y), Point::new_scale(0., y)}}};
        const Points nodes = Voronoi::join_boundary(walls, boundary);
        const auto graph = Voronoi::build_wall_graph(walls);
        const size_t vertex = graph.indices.at(Point::new_scale(0., y));
        REQUIRE(graph.vertices[vertex].edges.size() == 3);
        std::vector<std::array<Vec3d, 2>> sites;
        if (persistent_sites) sites.assign(nodes.size(), {Vec3d(-1., 0., 0.), Vec3d(1., 0., 0.)});
        const auto connections = Voronoi::pair_junctions(graph, {}, layer, nodes, sites);
        for (size_t edge : graph.vertices[vertex].edges) {
            const size_t partner = connections[edge][graph.edges[edge][0] == vertex ? 0 : 1];
            if (partner == Voronoi::no_connection) unpaired.insert(edge);
            if (edge == 0 && partner != Voronoi::no_connection) ++turns;
        }
        double length = 0.;
        for (const auto &path : Voronoi::trace_trails(graph, connections, layer)) length += path.length();
        REQUIRE_THAT(unscale<double>(length), Catch::Matchers::WithinAbs(13., 1e-6));
    }
    REQUIRE(unpaired.size() == 3);
    REQUIRE(turns == 2);
}

TEST_CASE("Material density bytes preserve zero and sparse tails and reserve escape codes", "[FillVoronoi][MaterialDensity]")
{
    using namespace Voronoi;
    REQUIRE(encode_material_density(0.) == 0);
    REQUIRE(encode_material_density(1.) == 253);
    REQUIRE_THAT(decode_material_density(253), Catch::Matchers::WithinAbs(1., 1e-15));
    for (int code = 1; code <= 253; ++code)
        REQUIRE(encode_material_density(decode_material_density(uint8_t(code))) == code);
    for (double density : {0.0001, 0.0005, 0.0025, 0.05, 0.25, 0.8})
        REQUIRE_THAT(decode_material_density(encode_material_density(density)), Catch::Matchers::WithinRel(density, 0.022));
    REQUIRE(encode_material_density(1e-10) == 1);
    REQUIRE_THROWS_AS(decode_material_density(254), std::invalid_argument);
    REQUIRE_THROWS_AS(decode_material_density(255), std::invalid_argument);
    REQUIRE_THROWS_AS(encode_material_density(-0.1), std::invalid_argument);
    REQUIRE_THROWS_AS(encode_material_density(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}

TEST_CASE("The candidate density bound covers both sparse interiors and dense walls", "[FillVoronoi][MaterialDensity]")
{
    const ExPolygons model {rectangle(-40., -40., 40., 40.)};
    const double decay = GENERATE(2., 3.);
    const double sigma = GENERATE(0., 8.);
    const double density = 0.3;
    Voronoi::CubicWallDistance field({{-40., 40., &model}}, sigma, {}, decay, density);
    std::vector<Vec3d> points;
    for (int x = 0; x <= 40; ++x) points.emplace_back(x, 0., 0.);
    const auto samples = field.sample_fields(points);
    const double boost = Voronoi::WallDistancePointCloud::density_ratio(0., sigma == 0. ? 0. : 2.);
    const double expected = Voronoi::decode_material_density(Voronoi::encode_material_density(density * boost)) / density;
    REQUIRE_THAT(field.maximum_density_ratio(), Catch::Matchers::WithinRel(expected, 1e-12));
    REQUIRE(Voronoi::encode_material_density(density * samples.front().density) < 128);
    REQUIRE(Voronoi::encode_material_density(density * samples.back().density) >= 128);
    for (const auto &sample : samples)
        REQUIRE(field.maximum_density_ratio() >= sample.density - 1e-12);
}

TEST_CASE("Fused material density uses an inside-only smoothstep including modifier ceilings", "[FillVoronoi][MaterialDensity]")
{
    const ExPolygons model {rectangle(-20., -20., 20., 20.)};
    const Voronoi::DensityModifier modifier {3, 0.5, 4., {{8., 32., {rectangle(0., -20., 20., 20.)}}}};
    Voronoi::CubicWallDistance field({{0., 40., &model}}, 0., {}, 0., 0.1, {modifier}, 3);
    for (double depth : {-2., 0., 1., 2., 3., 4., 8.}) {
        const double t = std::clamp(depth / 4., 0., 1.);
        const double expected = 0.1 + 0.4 * t * t * (3. - 2. * t);
        const auto samples = field.sample_fields({Vec3d(depth, 0., 20.), Vec3d(10., 0., 8. + depth)});
        for (const auto &sample : samples)
            REQUIRE_THAT(0.1 * sample.density, Catch::Matchers::WithinRel(expected, 0.06));
    }
    REQUIRE(field.maximum_density_ratio() >= 5.);
    Voronoi::CubicWallDistance other_region({{0., 40., &model}}, 0., {}, 0., 0.1, {modifier}, 7);
    REQUIRE_THAT(other_region.sample_fields({Vec3d(10., 0., 20.)}).front().density,
                 Catch::Matchers::WithinRel(1., 0.022));
}

TEST_CASE("Fused modifiers compose material density in order", "[FillVoronoi][MaterialDensity]")
{
    const ExPolygons model {rectangle(-20., -20., 20., 20.)};
    Voronoi::DensityModifier first {3, 0.5, 0., {{0., 40., {rectangle(0., -20., 20., 20.)}}}};
    auto second = first;
    second.material_density = 0.2;
    Voronoi::CubicWallDistance ordered({{0., 40., &model}}, 0., {}, 0., 0.1, {first, second}, 3);
    const auto sample = ordered.sample_fields({Vec3d(10., 0., 20.)}).front();
    REQUIRE_THAT(sample.density * 0.1, Catch::Matchers::WithinRel(0.2, 0.022));
    first.blend_width = second.blend_width = 4.;
    Voronoi::CubicWallDistance blended({{0., 40., &model}}, 0., {}, 0., 0.1, {first, second}, 3);
    REQUIRE_THAT(blended.sample_fields({Vec3d(2., 0., 20.)}).front().density * 0.1,
                 Catch::Matchers::WithinRel(0.25, 0.04));
}

TEST_CASE("Fused point clouds accommodate higher modifier density and empty interiors", "[FillVoronoi][MaterialDensity]")
{
    const auto method = GENERATE(VoronoiCloudMethod::Random, VoronoiCloudMethod::Fcc, VoronoiCloudMethod::Cubic);
    const double target = GENERATE(0., 0.5);
    const ExPolygons model {rectangle(-20., -20., 20., 20.)};
    const Voronoi::DensityModifier modifier {3, target, 0., {{0., 40., {rectangle(0., -20., 20., 20.)}}}};
    Voronoi::WallDistanceCloudCache cache;
    const auto slices = [&] { return std::vector<Voronoi::WallSlice>{{0., 40., &model}}; };
    const auto field = cache.get(slices, 0., 0., {}, 0., method, 0, 0., false, 0.1, {modifier}, 3);
    const BoundingBoxf3 box(Vec3d(4., -5., 15.), Vec3d(14., 5., 25.));
    const auto points = field->points_in(box, 3.);
    if (target == 0.) REQUIRE(points.empty());
    else {
        const auto uniform = cache.get(slices, 0., 0., {}, 0., method)->points_in(box, 3.);
        REQUIRE(points.size() > 50 * uniform.size());
    }
    REQUIRE_THAT(field->relative_density({Vec3d(10., 0., 20.)}).front(), Catch::Matchers::WithinRel(target / 0.1, 0.022));
    const auto spacing = field->local_spacing({Vec3d(10., 0., 20.)}, 3.).front();
    if (target > 0.) REQUIRE_THAT(spacing, Catch::Matchers::WithinRel(3. * 0.1 / target, 0.022));
    const auto left = field->points_in(BoundingBoxf3(box.min, Vec3d(9., 5., 25.)), 3.);
    const auto right = field->points_in(BoundingBoxf3(Vec3d(9., -5., 15.), box.max), 3.);
    REQUIRE(left.size() + right.size() == points.size());
}

TEST_CASE("Modifier blend weights use smoothstep entirely inside the boundary", "[FillVoronoi][MaterialDensity]")
{
    REQUIRE_THAT(Voronoi::modifier_blend(-1., 4.), Catch::Matchers::WithinAbs(0., EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(0., 4.), Catch::Matchers::WithinAbs(0., EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(1., 4.), Catch::Matchers::WithinAbs(0.15625, EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(2., 4.), Catch::Matchers::WithinAbs(0.5, EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(3., 4.), Catch::Matchers::WithinAbs(0.84375, EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(4., 4.), Catch::Matchers::WithinAbs(1., EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(8., 4.), Catch::Matchers::WithinAbs(1., EPSILON));
    REQUIRE_THAT(Voronoi::modifier_blend(0., 0.), Catch::Matchers::WithinAbs(1., EPSILON));
}

TEST_CASE("Fused modifiers control wall decay and corner smoothing", "[FillVoronoi][MaterialDensity]")
{
    const double parent_sigma = GENERATE(0., 8.);
    const ExPolygons model {rectangle(-20., -20., 20., 20.)};
    Voronoi::DensityModifier modifier {3, 0.1, 0., {{0., 40., model}}};
    modifier.decay = 0.;
    modifier.smoothing_sigma = 0.;
    Voronoi::CubicWallDistance uniform({{0., 40., &model}}, parent_sigma, {}, 6., 0.1, {modifier}, 3);
    const auto center = uniform.sample_fields({Vec3d(0., 0., 20.)}).front();
    REQUIRE_THAT(center.density, Catch::Matchers::WithinRel(1., 0.022));
    const Vec3d corner(14., 14., 20.);
    const auto unadjusted = uniform.sample_fields({corner}).front();
    modifier.smoothing_sigma = 8.;
    Voronoi::CubicWallDistance smoothed({{0., 40., &model}}, parent_sigma, {}, 6., 0.1, {modifier}, 3);
    const auto adjusted = smoothed.sample_fields({corner}).front();
    REQUIRE(adjusted.density > unadjusted.density + 0.1);
    REQUIRE_THAT(adjusted.distance, Catch::Matchers::WithinAbs(unadjusted.distance, EPSILON));
}

TEST_CASE("Incremental Voronoi geometry agrees with fresh builds while the slab moves", "[FillVoronoi][Incremental]")
{
    const auto provider = std::make_shared<Voronoi::PoissonPointCloud>();
    const auto bounds = rectangle(-6., -6., 6., 6.).contour.bounding_box();
    Voronoi::CellGeometry mesh;
    size_t removed = 0;
    for (double z : {0.1, 1.1, 2.1, 3.1, 5.1, 8.1, 12.1, 0.1}) {
        const auto points = provider->points_in(BoundingBoxf3(Vec3d(-14., -14., z - 7.), Vec3d(14., 14., z + 7.)), 4.);
        const auto changed = mesh.update(points);
        removed += changed.second;
        const auto unchanged = mesh.update(points);
        REQUIRE(unchanged.first == 0);
        REQUIRE(unchanged.second == 0);
        const auto bands = mesh.shallow_bands(bounds, z, 0.45, 0.2, 0.4);
        Voronoi::CellGeometry reference_mesh;
        reference_mesh.update(points);
        const auto reference = reference_mesh.shallow_bands(bounds, z, 0.45, 0.2, 0.4);
        ExPolygons got, want;
        for (const auto &band : bands) got.push_back(band.polygon);
        for (const auto &band : reference) want.push_back(band.polygon);
        ExPolygons difference = diff_ex(got, want);
        append(difference, diff_ex(want, got));
        double area = 0.;
        for (const auto &polygon : difference) area += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
        REQUIRE(area < 0.0001);
    }
    REQUIRE(removed > 0);
}

TEST_CASE("Copied Voronoi networks own their storage through dimension changes", "[FillVoronoi][Incremental]")
{
    const auto bounds = rectangle(-10., -10., 10., 10.).contour.bounding_box();
    Voronoi::CellGeometry mesh;
    std::vector<Vec3d> points {Vec3d(-5., 0., 0.), Vec3d(5., 0., 0.), Vec3d(0., 5., 0.), Vec3d(0., 0., 10.)};
    const auto contours = [&](const Voronoi::CellGeometry &geometry) {
        std::vector<Points> result;
        for (const auto &band : geometry.shallow_bands(bounds, 4.9, 0.45, 0.2, 0.4))
            result.push_back(band.polygon.contour.points);
        return result;
    };
    mesh.update(points);
    Voronoi::CellGeometry copy = mesh, assigned;
    assigned = mesh;
    const auto expected = contours(copy);
    REQUIRE_FALSE(expected.empty());
    while (!points.empty()) {
        points.pop_back();
        mesh.update(points);
        Voronoi::CellGeometry fresh;
        fresh.update(points);
        REQUIRE(contours(mesh) == contours(fresh));
        REQUIRE(contours(copy) == expected);
        REQUIRE(contours(assigned) == expected);
    }
    REQUIRE(contours(mesh).empty());
}

TEST_CASE("Candidate density bounds include modifier settings and ignore other regions", "[FillVoronoi][MaterialDensity]")
{
    const ExPolygons model {rectangle(-10., -10., 10., 10.)};
    const double density = GENERATE(0., 0.7, 0.9);
    const double sigma = GENERATE(0., 8.);
    Voronoi::DensityModifier modifier {3, density, 0., {{0., 20., model}}};
    modifier.smoothing_sigma = sigma;
    const Voronoi::DensityModifier unrelated {7, 1., 0., {{0., 20., model}}};
    Voronoi::CubicWallDistance field({{0., 20., &model}}, 0., {}, 6., 0.1, {modifier, unrelated}, 3);
    const double boost = Voronoi::WallDistancePointCloud::density_ratio(0., sigma == 0. ? 0. : 2.);
    const double maximum = std::max(0.1, std::min(1., density * boost));
    const double expected = Voronoi::decode_material_density(Voronoi::encode_material_density(maximum)) / 0.1;
    REQUIRE_THAT(field.maximum_density_ratio(), Catch::Matchers::WithinRel(expected, 1e-12));
}
