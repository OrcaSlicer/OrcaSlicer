#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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

double path_length(const Points &points)
{
    double length = 0.;
    for (size_t i = 1; i < points.size(); ++i)
        length += (points[i] - points[i - 1]).cast<double>().norm();
    return length;
}

double discrete_curvature_at(const Points &points, const Point &point)
{
    const auto point_it = std::find(points.begin(), points.end(), point);
    REQUIRE(point_it != points.end());
    const size_t point_idx = size_t(std::distance(points.begin(), point_it));
    REQUIRE(point_idx > 0);
    REQUIRE(point_idx + 1 < points.size());

    const Vec2d incoming = (points[point_idx] - points[point_idx - 1]).cast<double>() / output_scale;
    const Vec2d outgoing = (points[point_idx + 1] - points[point_idx]).cast<double>() / output_scale;
    const Vec2d chord     = incoming + outgoing;
    const double cross   = std::abs(incoming.x() * outgoing.y() - incoming.y() * outgoing.x());
    return 2. * cross / (incoming.norm() * outgoing.norm() * chord.norm());
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

TEST_CASE("Smoothed Hilbert corners use a uniform subdivision depth", "[FillPlanePath]")
{
    const Points smooth = TestableHilbertCurve().generate_points(0.0035, 1., 1);
    const Point  curve_entry(0, coord_t(0.5 * output_scale));
    const Point  curve_exit(coord_t(0.5 * output_scale), coord_t(output_scale));

    const auto entry_it = std::find(smooth.begin(), smooth.end(), curve_entry);
    REQUIRE(entry_it != smooth.end());
    const auto exit_it = std::find(entry_it, smooth.end(), curve_exit);
    REQUIRE(exit_it != smooth.end());

    const size_t segment_count = size_t(std::distance(entry_it, exit_it));
    REQUIRE(segment_count > 1);
    REQUIRE((segment_count & (segment_count - 1)) == 0);

    double previous_length = (entry_it[1] - entry_it[0]).cast<double>().norm();
    REQUIRE(previous_length > 0.);
    double max_length_ratio = 1.;
    for (size_t segment = 1; segment < segment_count; ++segment) {
        const double current_length = (entry_it[segment + 1] - entry_it[segment]).cast<double>().norm();
        REQUIRE(current_length > 0.);
        max_length_ratio = std::max(max_length_ratio,
                                    std::max(current_length / previous_length, previous_length / current_length));
        previous_length = current_length;
    }
    REQUIRE(max_length_ratio < 1.5);
}

TEST_CASE("Hilbert smoothing joins straight segments with continuous curvature", "[FillPlanePath]")
{
    const Points coarse = TestableHilbertCurve().generate_points(0.005, 0.5, 1);
    const Points fine   = TestableHilbertCurve().generate_points(0.0001, 0.5, 1);
    const Point  first_curve_entry(0, coord_t(0.75 * output_scale));

    const double coarse_entry_curvature = discrete_curvature_at(coarse, first_curve_entry);
    const double fine_entry_curvature   = discrete_curvature_at(fine, first_curve_entry);
    REQUIRE(coarse_entry_curvature > 0.);
    REQUIRE(fine_entry_curvature < 0.25 * coarse_entry_curvature);
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
