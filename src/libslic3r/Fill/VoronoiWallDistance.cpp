#include "VoronoiWallDistance.hpp"
#include "VoronoiMaterialDensity.hpp"
#include "VoronoiLattice.hpp"
#include "VoronoiWallSnap.hpp"
#include "VoronoiRelaxation.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <array>
#include <utility>
#include <stdexcept>
#include <limits>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r::Voronoi {
namespace {
void report(const WallDistanceProgress &progress, WallDistanceStage stage, double fraction)
{
    if (progress) progress(stage, fraction);
}
// Fixed-point millimetres retain more precision than the logarithmic byte
// table. OpenCV supplies optimized separable filters for signed 16-bit images.
constexpr double distance_units = 16.;
constexpr int maximum_units = 16000;

cv::Mat extrapolate_border(const cv::Mat &source, int radius_x, int radius_y, int maximum_step)
{
    cv::Mat padded;
    cv::copyMakeBorder(source, padded, radius_y, radius_y, radius_x, radius_x,
                       cv::BORDER_REPLICATE | cv::BORDER_ISOLATED);
    // Linear outward extension preserves planar fields. Bound the slope by
    // the distance field's Lipschitz limit, and saturate at the table cutoff.
    for (int y = radius_y; radius_x > 0 && y < radius_y + source.rows; ++y) {
        int16_t *row = padded.ptr<int16_t>(y);
        const int left = row[radius_x], right = row[radius_x + source.cols - 1];
        const int left_step = std::clamp(left - int(row[radius_x + 1]), 0, maximum_step);
        const int right_step = std::clamp(right - int(row[radius_x + source.cols - 2]), 0, maximum_step);
        for (int k = 1; k <= radius_x; ++k) {
            row[radius_x - k] = int16_t(std::min(maximum_units, left + k * left_step));
            row[radius_x + source.cols - 1 + k] = int16_t(std::min(maximum_units, right + k * right_step));
        }
    }
    const int16_t *top = padded.ptr<int16_t>(radius_y), *next = padded.ptr<int16_t>(radius_y + 1);
    const int16_t *bottom = padded.ptr<int16_t>(radius_y + source.rows - 1);
    const int16_t *previous = padded.ptr<int16_t>(radius_y + source.rows - 2);
    for (int k = 1; k <= radius_y; ++k) {
        int16_t *above = padded.ptr<int16_t>(radius_y - k);
        int16_t *below = padded.ptr<int16_t>(radius_y + source.rows - 1 + k);
        for (int x = 0; x < padded.cols; ++x) {
            above[x] = int16_t(std::min(maximum_units, int(top[x]) + k * std::clamp(int(top[x]) - next[x], 0, maximum_step)));
            below[x] = int16_t(std::min(maximum_units, int(bottom[x]) + k * std::clamp(int(bottom[x]) - previous[x], 0, maximum_step)));
        }
    }
    return padded;
}

void smooth_distances(std::vector<int16_t> &values, const std::array<size_t, 3> &size, double spacing, double sigma,
                      const WallDistanceProgress &progress)
{
    if (sigma == 0.) return;
    report(progress, WallDistanceStage::SmoothXY, 0.);
    const double radius_voxels = std::ceil(3. * sigma / spacing);
    if (radius_voxels > (std::numeric_limits<int>::max() - 1) / 2)
        throw std::invalid_argument("Voronoi smoothing radius is too large");
    const int radius = int(radius_voxels);
    const cv::Mat kernel = cv::getGaussianKernel(2 * radius + 1, sigma / spacing, CV_32F);
    const int maximum_step = int(std::lround(std::min(double(maximum_units), spacing * distance_units)));
    const size_t plane_size = size[0] * size[1];
    // X and Y passes operate on one plane at a time. Only the filter's
    // temporary border is extrapolated; no additional geometry is rasterized.
    for (size_t z = 0; z < size[2]; ++z) {
        cv::Mat plane(int(size[1]), int(size[0]), CV_16SC1, values.data() + z * plane_size);
        cv::Mat padded = extrapolate_border(plane, radius, radius, maximum_step);
        cv::sepFilter2D(padded, padded, CV_16S, kernel, kernel);
        padded(cv::Rect(radius, radius, plane.cols, plane.rows)).copyTo(plane);
        report(progress, WallDistanceStage::SmoothXY, double(z + 1) / size[2]);
    }
    report(progress, WallDistanceStage::SmoothZ, 0.);
    // Treat XY positions as columns for the Z pass. Limit scratch storage by
    // filtering blocks of columns rather than padding an entire second volume.
    const size_t block_width = std::max(size_t(1), size_t(256 * 1024) / (size[2] + 2 * size_t(radius)));
    const cv::Mat identity = cv::Mat::ones(1, 1, CV_32F);
    for (size_t first = 0; first < plane_size; first += block_width) {
        cv::Mat columns(int(size[2]), int(std::min(block_width, plane_size - first)), CV_16SC1,
                        values.data() + first, plane_size * sizeof(int16_t));
        cv::Mat padded = extrapolate_border(columns, 0, radius, maximum_step);
        cv::sepFilter2D(padded, padded, CV_16S, identity, kernel);
        padded(cv::Rect(0, radius, columns.cols, columns.rows)).copyTo(columns);
        report(progress, WallDistanceStage::SmoothZ, double(std::min(first + block_width, plane_size)) / plane_size);
    }
}
} // namespace

struct CubicWallDistance::Table {
    SlicedDistanceField original;
    std::vector<uint8_t> material;
    double reference_density;
    double maximum_ratio;
    std::array<double, 254> decoded_material;
    explicit Table(std::vector<WallSlice> slices, double smoothing_sigma, const WallDistanceProgress &progress,
                   double decay, double material_density, const std::vector<DensityModifier> &modifiers, int region_id)
        : reference_density(material_density)
    {
        if (!std::isfinite(material_density) || material_density <= 0. || material_density > 1. ||
            !std::isfinite(decay) || decay < 0.) throw std::invalid_argument("Invalid material density settings");
        for (size_t i = 0; i < decoded_material.size(); ++i)
            decoded_material[i] = decode_material_density(uint8_t(i));
        if (!std::isfinite(smoothing_sigma) || smoothing_sigma < 0.)
            throw std::invalid_argument("Invalid Voronoi smoothing sigma");
        const auto density_bound = [&](double density, double sigma) {
            const double boost = WallDistancePointCloud::density_ratio(0., sigma == 0. ? 0. : 2.);
            return decode_material_density(encode_material_density(std::min(1., density * boost))) / reference_density;
        };
        maximum_ratio = std::max(0.01, density_bound(material_density, smoothing_sigma));
        report(progress, WallDistanceStage::Distance, 0.);
        original = SlicedDistanceField(std::move(slices), [&](auto stage, double fraction) {
            if (stage == SlicedDistanceField::Stage::Distance)
                report(progress, WallDistanceStage::Distance, fraction);
            else if (stage == SlicedDistanceField::Stage::Encoding)
                report(progress, WallDistanceStage::Encoding, fraction / 2.);
        });
        if (original.node_count() == 0) {
            report(progress, WallDistanceStage::Complete, 1.);
            return;
        }
        const auto &size = original.grid_size();
        const auto &origin = original.grid_origin();
        const double spacing = original.grid_spacing();
        const size_t count = original.node_count();
        std::vector<int16_t> values;
        if (smoothing_sigma != 0.) {
            values.resize(count);
            for (size_t i = 0; i < count; ++i) {
                if (i % 65536 == 0) report(progress, WallDistanceStage::Encoding, (1. + double(i) / count) / 2.);
                values[i] = int16_t(std::lround(original.node_value(i) * distance_units));
            }
            smooth_distances(values, size, spacing, smoothing_sigma, progress);
        }
        material.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (i % 65536 == 0) report(progress, WallDistanceStage::MaterialDensity, double(i) / count);
            const double raw = original.node_value(i);
            const double mean = values.empty() ? raw : double(values[i]) / distance_units;
            const double density = std::min(1., material_density * WallDistancePointCloud::density_ratio(raw, mean, decay));
            material[i] = encode_material_density(density);
        }
        size_t modifier_index = 0;
        for (const DensityModifier &modifier : modifiers) {
            if (modifier.region_id != region_id) continue;
            if (!std::isfinite(modifier.material_density) || modifier.material_density < 0. || modifier.material_density > 1. ||
                !std::isfinite(modifier.blend_width) || modifier.blend_width < 0.)
                throw std::invalid_argument("Invalid Voronoi density modifier");
            const double target_decay = modifier.decay.value_or(decay);
            const double target_sigma = modifier.smoothing_sigma.value_or(smoothing_sigma);
            if (!std::isfinite(target_decay) || target_decay < 0. || !std::isfinite(target_sigma) || target_sigma < 0.)
                throw std::invalid_argument("Invalid Voronoi modifier density settings");
            maximum_ratio = std::max(maximum_ratio, density_bound(modifier.material_density, target_sigma));
            std::vector<WallSlice> mask_slices;
            for (const auto &slice : modifier.slices)
                mask_slices.push_back({slice.bottom_z, slice.top_z, &slice.contours});
            // Use Orca's sliced modifier outline, including its top and bottom.
            SlicedDistanceField mask(std::move(mask_slices), [&](auto, double) {
                // Forward cancellation even during the modifier's distance construction.
                report(progress, WallDistanceStage::Modifiers, double(modifier_index) / modifiers.size());
            });
            std::vector<int16_t> modifier_values;
            const bool own_smoothing = target_sigma != smoothing_sigma;
            if (own_smoothing && target_sigma != 0.) {
                modifier_values.resize(count);
                for (size_t i = 0; i < count; ++i) {
                    if (i % 65536 == 0) report(progress, WallDistanceStage::Modifiers, double(modifier_index) / modifiers.size());
                    modifier_values[i] = int16_t(std::lround(original.node_value(i) * distance_units));
                }
                smooth_distances(modifier_values, size, spacing, target_sigma, [&](auto, double) {
                    report(progress, WallDistanceStage::Modifiers, double(modifier_index) / modifiers.size());
                });
            }
            const auto &target_values = own_smoothing ? modifier_values : values;
            for (size_t first = 0; first < count; first += 4096) {
                report(progress, WallDistanceStage::Modifiers, (modifier_index + double(first) / count) / modifiers.size());
                std::vector<Vec3d> points;
                const size_t last = std::min(count, first + 4096);
                points.reserve(last - first);
                for (size_t i = first; i < last; ++i)
                    points.push_back(origin + spacing * Vec3d(double(i % size[0]), double((i / size[0]) % size[1]), double(i / (size[0] * size[1]))));
                const auto depths = mask.sample(points);
                for (size_t j = 0; j < points.size(); ++j) {
                    const double blend = modifier_blend(-depths[j], modifier.blend_width);
                    if (blend == 0.) continue;
                    const size_t i = first + j;
                    const double raw = original.node_value(i);
                    const double mean = target_values.empty() ? raw : double(target_values[i]) / distance_units;
                    const double density = std::min(1., modifier.material_density * WallDistancePointCloud::density_ratio(raw, mean, target_decay));
                    material[i] = encode_material_density((1. - blend) * decoded_material[material[i]] + blend * density);
                }
            }
            ++modifier_index;
        }
        size_t inside_nodes = 0;
        double inside_min = 1., inside_max = 0.;
        for (size_t i = 0; i < count; ++i)
            if (original.is_inside(i)) {
                ++inside_nodes;
                const double density = decoded_material[material[i]];
                inside_min = std::min(inside_min, density);
                inside_max = std::max(inside_max, density);
            }
        BOOST_LOG_TRIVIAL(warning) << "Voronoi diagnostic: field decay=" << decay << " sigma=" << smoothing_sigma
            << " reference_density=" << reference_density << " grid=" << size[0] << ',' << size[1] << ',' << size[2]
            << " spacing=" << spacing << " origin=" << origin.transpose() << " inside_nodes=" << inside_nodes
            << " inside_density=" << inside_min << ',' << inside_max << " maximum_ratio=" << maximum_ratio;
        report(progress, WallDistanceStage::Complete, 1.);
    }
};

CubicWallDistance::CubicWallDistance(std::vector<WallSlice> slices, double smoothing_sigma, const WallDistanceProgress &progress, double decay, double material_density,
                                     const std::vector<DensityModifier> &modifiers, int region_id)
    : m_table(std::make_shared<Table>(std::move(slices), smoothing_sigma, progress, decay, material_density, modifiers, region_id)) {}

double CubicWallDistance::grid_spacing() const { return m_table->original.grid_spacing(); }
double CubicWallDistance::maximum_density_ratio() const { return m_table->maximum_ratio; }

std::vector<WallDistanceSample> CubicWallDistance::sample_fields(const std::vector<Vec3d> &points) const
{
    std::vector<WallDistanceSample> result(points.size(), {SlicedDistanceField::far_distance, 0.01});
    if (m_table->original.node_count() == 0) return result;
    const Table &t = *m_table;
    for (size_t i = 0; i < points.size(); ++i) {
        const auto where = t.original.interpolation(points[i]);
        double density = 0.;
        for (size_t j = 0; j < 8; ++j)
            density += t.decoded_material[t.material[where.indices[j]]] * where.weights[j] / t.reference_density;
        result[i] = {t.original.sample(where), where.outside > 0. ? std::max(0.01, density) : density};
    }
    return result;
}

std::vector<double> CubicWallDistance::sample(const std::vector<Vec3d> &points) const
{
    return m_table->original.sample(points);
}

std::shared_ptr<const PointCloudProvider> WallDistanceCloudCache::get(const std::function<std::vector<WallSlice>()> &load_slices,
                                                                   double decay, double sigma, const WallDistanceProgress &progress,
                                                                   double site_spacing, VoronoiCloudMethod method, int relaxation_iterations,
                                                                   double angle, bool snap, double material_density, const std::vector<DensityModifier> &modifiers, int region_id, double hull_threshold)
{
    if (!std::isfinite(site_spacing) || site_spacing < 0. || relaxation_iterations < 0 ||
        ((snap || relaxation_iterations > 0) && site_spacing == 0.))
        throw std::invalid_argument("Invalid Voronoi point preparation settings");
    if (!std::isfinite(material_density) || material_density <= 0. || material_density > 1. ||
        !std::isfinite(decay) || decay < 0. || !std::isfinite(sigma) || sigma < 0.)
        throw std::invalid_argument("Invalid Voronoi distance settings");
    if (!std::isfinite(angle)) throw std::invalid_argument("Invalid Voronoi cloud angle");
    angle = std::fmod(angle, 2. * std::acos(-1.));
    if (angle < 0.) angle += 2. * std::acos(-1.);
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto bounds = [&] {
        BoundingBoxf3 box;
        for (const auto &slice : load_slices())
            for (const auto &polygon : *slice.contours) {
                const auto xy = polygon.contour.bounding_box();
                if (!xy.defined) continue;
                box.merge(Vec3d(unscale<double>(xy.min.x()), unscale<double>(xy.min.y()), slice.bottom_z));
                box.merge(Vec3d(unscale<double>(xy.max.x()), unscale<double>(xy.max.y()), slice.top_z));
            }
        return box;
    };
    std::shared_ptr<const CubicWallDistance> table;
    WallDistanceSamples sample;
    const bool fused = std::any_of(modifiers.begin(), modifiers.end(), [region_id](const auto &m) { return m.region_id == region_id; });
    if (!std::isfinite(hull_threshold) || hull_threshold < 0. || hull_threshold > 1.)
        throw std::invalid_argument("Invalid Voronoi hull threshold");
    const bool cutoff = hull_threshold > 0.;
    if (decay != 0. || sigma != 0. || snap || fused || cutoff) {
        auto &cached = m_tables[{decay, sigma, material_density, region_id}];
        if (!cached) cached = std::make_shared<CubicWallDistance>(load_slices(), sigma, progress, decay, material_density, modifiers, region_id);
        table = cached;
        sample = [table](const auto &points) { return table->sample_fields(points); };
    }
    const double sampled_threshold = decode_material_density(encode_material_density(hull_threshold)) / material_density;
    auto &provider = m_providers[{decay, sigma, material_density, region_id, hull_threshold, 0., method, 0, angle, false}];
    if (!provider) {
        Vec2d center = Vec2d::Zero();
        if (angle != 0.) {
            const BoundingBoxf3 box = bounds();
            if (box.defined) center = box.center().head<2>();
        }
        const bool variable = decay != 0. || sigma != 0. || fused || cutoff;
        if (method != VoronoiCloudMethod::Random)
            provider = std::make_shared<LatticePointCloud>(method, variable ? sample : WallDistanceSamples{}, table ? table->maximum_density_ratio() : 1., angle, center, sampled_threshold);
        else if (variable)
            provider = std::make_shared<WallDistancePointCloud>(sample, table->maximum_density_ratio(), angle, center, sampled_threshold);
        else
            provider = std::make_shared<PoissonPointCloud>(angle, center);
    }
    std::shared_ptr<const PointCloudProvider> result = provider;
    if (relaxation_iterations > 0) {
        auto &relaxed = m_providers[{decay, sigma, material_density, region_id, hull_threshold, site_spacing, method, relaxation_iterations, angle, false}];
        if (!relaxed)
            relaxed = std::make_shared<RelaxedPointCloud>(provider, bounds(), site_spacing, relaxation_iterations,
                [&](double fraction) { report(progress, WallDistanceStage::Relaxation, fraction); });
        result = relaxed;
    }
    if (snap) {
        auto &snapped = m_providers[{decay, sigma, material_density, region_id, hull_threshold, site_spacing, method, relaxation_iterations, angle, true}];
        if (!snapped)
            snapped = std::make_shared<SnappedPointCloud>(result, sample, site_spacing, table->grid_spacing());
        result = snapped;
    }
    return result;
}

void WallDistanceCloudCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_providers.clear();
    m_tables.clear();
}

} // namespace Slic3r::Voronoi
