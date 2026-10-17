#include <catch2/catch_all.hpp>

#include "libslic3r/Fill/VoronoiInfill.hpp"
#include "libslic3r/Fill/VoronoiRouting.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <array>
#include <cmath>
#include <limits>
#include <set>

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

TEST_CASE("Voronoi layers share a complete reproducible cloud", "[FillVoronoi]")
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
    infill.set_z_range(Vec2d(0., 10.));
    const Segments first = segments(infill.fill_geometry(bounds, region, 0.45, 0.15, 0.1, 0.45, 0.).walls);
    REQUIRE_FALSE(provider->queries.empty());
    REQUIRE(first != segments(infill.fill_geometry(bounds, region, 0.45, 0.15, 0.2, 0.45, 0.).walls));
    const size_t queries = provider->queries.size();
    infill.fill_geometry(bounds, region, 0.45, 0.15, 8., 0.45, 0.);
    REQUIRE(provider->queries.size() == queries);
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

TEST_CASE("Voronoi finite empty clouds terminate without geometry", "[FillVoronoi]")
{
    auto provider = std::make_shared<FixedProvider>();
    VoronoiInfill infill(provider);
    const auto region = rectangle(-5., -5., 5., 5.);
    REQUIRE(infill.fill_geometry(region.contour.bounding_box(), region, 0.45, 0.2, 0., 0.45, 0.).walls.empty());
}

TEST_CASE("Random cloud rotation preserves points in a spherical region", "[FillVoronoi][Rotation]")
{
    const double angle = GENERATE(0.37, -1.2);
    const Vec2d center(4., -3.);
    const BoundingBoxf3 box(Vec3d(-16., -23., -20.), Vec3d(24., 17., 20.));
    const auto original = std::make_shared<Voronoi::PoissonPointCloud>();
    const auto rotated = std::make_shared<Voronoi::PoissonPointCloud>(angle, center);
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
    REQUIRE_THROWS_AS(Voronoi::PoissonPointCloud(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
}

TEST_CASE("Complete Voronoi clouds include distant exterior competitors", "[FillVoronoi]")
{
    FixedProvider provider;
    provider.sites = {Vec3d(-100., 0., -20.), Vec3d(100., 0., 20.),
                      Vec3d(0., 100., 0.), Vec3d(0., -100., 0.)};
    const auto bounds = rectangle(-5., -5., 5., 5.).contour.bounding_box();
    const BoundingBoxf3 domain(Vec3d(-5., -5., 0.), Vec3d(5., 5., 20.));
    const auto sites = Voronoi::complete_cloud(provider, domain, 1.);
    REQUIRE(sites.size() == provider.sites.size());
    for (double z : {0., 1., 20.})
        REQUIRE(segments(Voronoi::section(sites, bounds, z).walls) ==
                segments(Voronoi::section(provider.sites, bounds, z).walls));
}
