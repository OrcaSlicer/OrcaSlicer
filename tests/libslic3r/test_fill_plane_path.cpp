#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include "libslic3r/Point.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Fill/FillBase.hpp"
#include <cstddef>
#include "libslic3r/Config.hpp"
#include <limits>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "libslic3r/Fill/FillPlanePath.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

constexpr double output_scale = 1'000'000.;

class TestableHilbertCurve : public FillHilbertCurve
{
public:
    Points generate_points(double resolution, double smooth_factor = 0., coord_t max_coordinate = 7)
    {
        InfillPolylineOutput output(output_scale);
        FillParams params;
        params.smooth_factor = smooth_factor;
        FillHilbertCurve::generate(0, 0, max_coordinate, max_coordinate, resolution, params, output);
        return std::move(output.result());
    }
};

class TestableOctagramSpiral : public FillOctagramSpiral
{
public:
    Points generate_points(double resolution, double smooth_factor = 0., coord_t max_coordinate = 7)
    {
        InfillPolylineOutput output(output_scale);
        FillParams params;
        params.smooth_factor = smooth_factor;
        FillOctagramSpiral::generate(-max_coordinate, -max_coordinate, max_coordinate, max_coordinate, resolution, params, output);
        return std::move(output.result());
    }
};

// Cosine of the sharpest turn of a path, 1 meaning it has no turn at all.
double sharpest_turn_cosine(const Points &points)
{
    double sharpest = 1.;
    for (size_t i = 1; i + 1 < points.size(); ++i) {
        const Vec2d incoming = (points[i] - points[i - 1]).cast<double>().normalized();
        const Vec2d outgoing = (points[i + 1] - points[i]).cast<double>().normalized();
        sharpest = std::min(sharpest, incoming.dot(outgoing));
    }
    return sharpest;
}

double path_length(const Points &points)
{
    double length = 0.;
    for (size_t i = 1; i < points.size(); ++i)
        length += (points[i] - points[i - 1]).cast<double>().norm();
    return length;
}

// The edges of the first rounded corner of the order 1 Hilbert curve, which leaves the leg x = 0 and joins the leg y = 1.
std::vector<double> first_corner_edges(const Points &points)
{
    std::vector<double> edges;
    for (size_t i = 1; i < points.size(); ++i) {
        if (points[i - 1].x() == 0 && points[i].x() == 0)
            continue;
        if (points[i - 1].y() == coord_t(output_scale) && points[i].y() == coord_t(output_scale))
            break;
        edges.push_back((points[i] - points[i - 1]).cast<double>().norm());
    }
    return edges;
}

} // namespace

TEST_CASE("Hilbert curve exposes a smoothing factor", "[FillPlanePath]")
{
    const ConfigOptionDef *factor_def = print_config_def.get("sparse_infill_smooth_factor");
    REQUIRE(factor_def != nullptr);
    REQUIRE(factor_def->type == coPercent);
    REQUIRE_THAT(factor_def->min, Catch::Matchers::WithinAbs(0., 1e-12));
    REQUIRE_THAT(factor_def->max, Catch::Matchers::WithinAbs(100., 1e-12));
    REQUIRE_THAT(factor_def->get_default_value<ConfigOptionPercent>()->value,
                 Catch::Matchers::WithinAbs(0., 1e-12));
}

TEST_CASE("Hilbert curve smoothing rounds right angle turns", "[FillPlanePath]")
{
    const Points sharp  = TestableHilbertCurve().generate_points(0.005);
    const Points smooth = TestableHilbertCurve().generate_points(0.005, 1.);

    REQUIRE(smooth.front() == sharp.front());
    REQUIRE(smooth.back() == sharp.back());
    REQUIRE(smooth.size() > sharp.size());

    bool has_turn = false;
    for (size_t i = 1; i < smooth.size(); ++i) {
        const Vec2d segment = (smooth[i] - smooth[i - 1]).cast<double>();
        REQUIRE(segment.squaredNorm() > 0.);
    }
    for (size_t i = 1; i + 1 < smooth.size(); ++i) {
        const Vec2d incoming = (smooth[i] - smooth[i - 1]).cast<double>();
        const Vec2d outgoing = (smooth[i + 1] - smooth[i]).cast<double>();
        const double cross   = incoming.x() * outgoing.y() - incoming.y() * outgoing.x();
        const double cosine  = incoming.dot(outgoing) / (incoming.norm() * outgoing.norm());
        has_turn |= std::abs(cross) > 0.;
        REQUIRE(cosine > 0.);
    }
    REQUIRE(has_turn);

    const coord_t upper_bound = coord_t(7 * output_scale);
    for (const Point &point : smooth) {
        REQUIRE(point.x() >= 0);
        REQUIRE(point.y() >= 0);
        REQUIRE(point.x() <= upper_bound);
        REQUIRE(point.y() <= upper_bound);
    }
}

TEST_CASE("Smoothed Hilbert curve honors path resolution", "[FillPlanePath]")
{
    const Points coarse = TestableHilbertCurve().generate_points(0.1, 1.);
    const Points fine   = TestableHilbertCurve().generate_points(0.001, 1.);

    REQUIRE(fine.size() > coarse.size());
    REQUIRE(fine.front() == coarse.front());
    REQUIRE(fine.back() == coarse.back());
}

TEST_CASE("Smoothed Hilbert corners turn by the same angle at every vertex", "[FillPlanePath]")
{
    // A planner slows down at a vertex by how much it turns, so a rounded corner spreads its turn evenly.
    const Points smooth = TestableHilbertCurve().generate_points(0.0035, 1., 1);
    std::vector<double> turns;
    for (size_t i = 1; i + 1 < smooth.size(); ++i) {
        const Vec2d  incoming = (smooth[i] - smooth[i - 1]).cast<double>().normalized();
        const Vec2d  outgoing = (smooth[i + 1] - smooth[i]).cast<double>().normalized();
        const double turn     = std::atan2(std::abs(cross2(incoming, outgoing)), incoming.dot(outgoing));
        if (turn > 1e-6)
            turns.push_back(turn);
    }
    REQUIRE(turns.size() > 2);
    const auto [sharpest, gentlest] = std::minmax_element(turns.begin(), turns.end());
    REQUIRE_THAT(*gentlest, Catch::Matchers::WithinRel(*sharpest, 0.02));
}

TEST_CASE("Hilbert smoothing eases into the turn", "[FillPlanePath]")
{
    // Every vertex turns by the same angle, so the edges are as long as the curve is gentle. The curvature starts
    // from zero at the legs, so the edges shorten away from them.
    const std::vector<double> edges = first_corner_edges(TestableHilbertCurve().generate_points(0.0002, 1., 1));
    REQUIRE(edges.size() >= 16);
    REQUIRE(edges[0] / edges[1] > 1.1);
}

TEST_CASE("Octagram spiral smoothing rounds the turns of the spiral", "[FillPlanePath]")
{
    const Points sharp  = TestableOctagramSpiral().generate_points(0.005);
    const Points smooth = TestableOctagramSpiral().generate_points(0.005, 1.);

    REQUIRE(smooth.size() > sharp.size());
    REQUIRE(smooth.front() == sharp.front());
    REQUIRE(smooth.back() == sharp.back());
    // The spiral alternates between 90 and 135 degree turns; both are rounded into gentle ones.
    REQUIRE(sharpest_turn_cosine(sharp) < -0.7);
    REQUIRE(sharpest_turn_cosine(smooth) > 0.9);

    for (size_t i = 1; i < smooth.size(); ++i)
        REQUIRE((smooth[i] - smooth[i - 1]).cast<double>().squaredNorm() > 0.);
}

TEST_CASE("Octagram spiral smooth factor controls corner curvature", "[FillPlanePath]")
{
    const Points sharp          = TestableOctagramSpiral().generate_points(0.005);
    const Points half_smooth    = TestableOctagramSpiral().generate_points(0.005, 0.5);
    const Points full_smooth    = TestableOctagramSpiral().generate_points(0.005, 1.);
    const Points invalid_factor = TestableOctagramSpiral().generate_points(
        0.005, std::numeric_limits<double>::quiet_NaN());

    REQUIRE(path_length(full_smooth) < path_length(half_smooth));
    REQUIRE(path_length(half_smooth) < path_length(sharp));
    REQUIRE(invalid_factor == sharp);
}

TEST_CASE("Hilbert curve smooth factor controls corner curvature", "[FillPlanePath]")
{
    const Points sharp       = TestableHilbertCurve().generate_points(0.005);
    const Points half_smooth = TestableHilbertCurve().generate_points(0.005, 0.5);
    const Points full_smooth = TestableHilbertCurve().generate_points(0.005, 1.);
    const Points invalid_factor = TestableHilbertCurve().generate_points(
        0.005, std::numeric_limits<double>::quiet_NaN());

    REQUIRE(full_smooth.front() == half_smooth.front());
    REQUIRE(full_smooth.back() == half_smooth.back());
    REQUIRE(path_length(full_smooth) < path_length(half_smooth));
    REQUIRE(invalid_factor == sharp);

    for (size_t i = 1; i < full_smooth.size(); ++i)
        REQUIRE((full_smooth[i] - full_smooth[i - 1]).squaredNorm() > 0);
}
