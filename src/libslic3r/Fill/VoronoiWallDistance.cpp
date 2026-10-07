#include "VoronoiWallDistance.hpp"
#include <functional>
#include <memory>
#include <mutex>
#include "../EdgeGrid.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"
#include "../libslic3r.h"
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

namespace Slic3r::Voronoi {
namespace {
void report(const WallDistanceProgress &progress, WallDistanceStage stage, double fraction)
{
    if (progress) progress(stage, fraction);
}
std::vector<WallSlice> merge_profiles(std::vector<WallSlice> slices)
{
    std::vector<WallSlice> merged;
    for (WallSlice slice : slices) {
        if (!merged.empty() && std::abs(merged.back().top_z - slice.bottom_z) < EPSILON)
            slice.bottom_z = merged.back().top_z;
        if (!merged.empty() && merged.back().top_z == slice.bottom_z &&
            *merged.back().contours == *slice.contours)
            merged.back().top_z = slice.top_z;
        else
            merged.push_back(slice);
    }
    return merged;
}
// Lower envelope of weighted squared distances to slab endpoints. Endpoints
// and queries advance in height; expired and dominated parabolas are removed.
// Breakpoints use output-grid indices, discarding candidates between samples.
class DistanceEnvelope {
    struct Parabola { double height; float cost; uint32_t start; };
    std::vector<Parabola> m_candidates;
    size_t m_first = 0;
public:
    double minimum(double height, size_t index)
    {
        while (m_first + 1 < m_candidates.size() && m_candidates[m_first + 1].start <= index)
            ++m_first;
        if (m_first == m_candidates.size()) return 1e6;
        const auto &p = m_candidates[m_first];
        return std::min(1e6, p.cost + (height - p.height) * (height - p.height));
    }
    void add(double height, float cost, double next, size_t index, size_t count, double step)
    {
        if (cost >= 1e6) return;
        minimum(next, index);
        if (m_first > 0 && m_first >= m_candidates.size() / 2) {
            m_candidates.erase(m_candidates.begin(), m_candidates.begin() + m_first);
            m_first = 0;
        }
        uint32_t start = uint32_t(index);
        while (m_candidates.size() > m_first) {
            const auto &p = m_candidates.back();
            if (height == p.height) {
                if (cost >= p.cost) return;
                m_candidates.pop_back();
                continue;
            }
            const double crossing = (height + p.height) / 2. + (double(cost) - p.cost) / (2. * (height - p.height));
            const double offset = std::max(0., std::ceil((crossing - next) / step));
            if (offset >= count - index) return;
            start = uint32_t(index + size_t(offset));
            if (start > p.start) break;
            m_candidates.pop_back();
        }
        if (m_candidates.empty()) m_first = 0;
        m_candidates.push_back({height, cost, start});
    }
};
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
    static constexpr double distance_scale = 6.;
    static constexpr double far_distance = 1000.;
    double spacing = 1.;
    Vec3d origin = Vec3d::Zero();
    std::array<size_t, 3> size {{0, 0, 0}};
    struct Field {
        std::vector<uint8_t> distance;
        std::vector<uint8_t> inside;
    };
    Field original, smoothed;
    std::array<double, 256> decoded;

    double value(const Field &field, size_t index) const
    {
        const double d = decoded[field.distance[index]];
        return field.inside[index / 8] & (uint8_t(1) << (index % 8)) ? -d : d;
    }

    explicit Table(std::vector<WallSlice> slices, double smoothing_sigma, const WallDistanceProgress &progress)
    {
        if (!std::isfinite(smoothing_sigma) || smoothing_sigma < 0.)
            throw std::invalid_argument("Invalid Voronoi smoothing sigma");
        report(progress, WallDistanceStage::Distance, 0.);
        const double log_range = std::log1p(far_distance / distance_scale);
        // Decode once so batched interpolation needs no logarithms or exponentials.
        for (size_t i = 0; i < decoded.size(); ++i)
            decoded[i] = distance_scale * std::expm1(log_range * i / 255.);
        slices = merge_profiles(std::move(slices));
        BoundingBoxf3 bounds;
        for (const WallSlice &slice : slices)
            for (const ExPolygon &polygon : *slice.contours) {
                const BoundingBox box = polygon.contour.bounding_box();
                if (!box.defined) continue;
                bounds.merge(Vec3d(unscale<double>(box.min.x()), unscale<double>(box.min.y()), slice.bottom_z));
                bounds.merge(Vec3d(unscale<double>(box.max.x()), unscale<double>(box.max.y()), slice.top_z));
            }
        if (!bounds.defined) { report(progress, WallDistanceStage::Complete, 1.); return; }
        // Keep a small exterior halo; smoothing extrapolates beyond it. Bound construction memory
        // for objects larger than an ordinary printer by coarsening uniformly.
        constexpr size_t max_nodes = 32 * 1024 * 1024;
        size_t count;
        for (;;) {
            origin = (bounds.min.array() / spacing).floor().matrix() * spacing - Vec3d::Constant(4.);
            const Vec3d upper = (bounds.max.array() / spacing).ceil().matrix() * spacing + Vec3d::Constant(4.);
            bool fits = true;
            count = 1;
            for (int axis = 0; axis < 3; ++axis) {
                size[axis] = size_t(std::ceil((upper[axis] - origin[axis]) / spacing)) + 1;
                if (size[axis] > max_nodes / count) { fits = false; break; }
                count *= size[axis];
            }
            if (fits) break;
            spacing *= 2.;
        }
        const size_t plane_size = size[0] * size[1];
        const float maximum = float(far_distance * far_distance);
        std::vector<float> distances(count, maximum);
        original.inside.assign((count + 7) / 8, 0);
        const BoundingBox xy_bounds(Point::new_scale(origin.x(), origin.y()),
                                     Point::new_scale(origin.x() + (size[0] - 1) * spacing,
                                                      origin.y() + (size[1] - 1) * spacing));
        // Include missing layers as air slabs. Both sweeps rasterize one profile
        // at a time, so no stack of planar SDFs is retained.
        const ExPolygons empty_contours;
        std::vector<WallSlice> slabs;
        double previous_top = slices.front().bottom_z;
        for (const WallSlice &slice : slices) {
            if (slice.bottom_z > previous_top)
                slabs.push_back({previous_top, slice.bottom_z, &empty_contours});
            slabs.push_back(slice);
            previous_top = slice.top_z;
        }
        std::vector<float> planar(plane_size);
        for (int direction : {1, -1}) {
            std::vector<DistanceEnvelope> to_solid(plane_size), to_air(plane_size);
            size_t output = 0;
            auto accumulate = [&](double until, double bottom, bool empty) {
                while (output < size[2]) {
                    const size_t z = direction == 1 ? output : size[2] - 1 - output;
                    const double height = direction * (origin.z() + z * spacing);
                    if (height > until) break;
                    const double physical_z = direction * height;
                    const double cap = std::max(0., std::min(physical_z - slices.front().bottom_z,
                                                           slices.back().top_z - physical_z));
                    for (size_t xy = 0; xy < plane_size; ++xy) {
                        const size_t index = z * plane_size + xy;
                        const uint8_t bit = uint8_t(1) << (index % 8);
                        const bool was_inside = original.inside[index / 8] & bit;
                        bool material = was_inside;
                        double s = to_solid[xy].minimum(height, output);
                        double a = std::min(cap * cap, to_air[xy].minimum(height, output));
                        if (height >= bottom) {
                            const double d = empty ? far_distance : planar[xy];
                            if (!empty) s = std::min(s, std::pow(std::max(0., d), 2));
                            a = std::min(a, std::pow(std::max(0., -d), 2));
                            if (!empty && d < 0.) {
                                material = true;
                                original.inside[index / 8] |= bit;
                            }
                        }
                        // A sign change in the second sweep is a shared slab
                        // boundary with air on its other side: its distance is zero.
                        const float distance = direction == -1 && material && !was_inside ? 0.f : float(material ? a : s);
                        distances[index] = std::min(distances[index], distance);
                    }
                    ++output;
                    report(progress, WallDistanceStage::Distance,
                           ((direction == 1 ? 0. : 1.) + double(output) / size[2]) / 2.);
                }
            };
            for (size_t i = 0; i < slabs.size(); ++i) {
                const WallSlice &slice = slabs[direction == 1 ? i : slabs.size() - 1 - i];
                const double bottom = direction == 1 ? slice.bottom_z : -slice.top_z;
                const double top = direction == 1 ? slice.top_z : -slice.bottom_z;
                const bool empty = std::none_of(slice.contours->begin(), slice.contours->end(),
                                               [](const ExPolygon &p) { return !p.contour.empty(); });
                if (!empty) {
                    EdgeGrid::Grid grid(xy_bounds);
                    grid.create(*slice.contours, scale_(spacing));
                    grid.calculate_sdf();
                    for (size_t y = 0; y < size[1]; ++y)
                        for (size_t x = 0; x < size[0]; ++x)
                            planar[y * size[0] + x] = float(grid.signed_distance_bilinear(
                                Point::new_scale(origin.x() + x * spacing, origin.y() + y * spacing)) * SCALING_FACTOR);
                }
                accumulate(top, bottom, empty);
                if (output < size[2]) {
                    const size_t next_z = direction == 1 ? output : size[2] - 1 - output;
                    const double next = direction * (origin.z() + next_z * spacing);
                    for (size_t xy = 0; xy < plane_size; ++xy) {
                        const double d = empty ? far_distance : planar[xy];
                        if (!empty) to_solid[xy].add(top, std::pow(std::max(0., d), 2), next, output, size[2], spacing);
                        to_air[xy].add(top, std::pow(std::max(0., -d), 2), next, output, size[2], spacing);
                    }
                }
                report(progress, WallDistanceStage::Distance,
                       ((direction == 1 ? 0. : 1.) + double(output) / size[2]) / 2.);
            }
            accumulate(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), true);
        }
        report(progress, WallDistanceStage::Encoding, 0.);
        original.distance.resize(count);
        for (size_t i = 0; i < count; ++i) {
            if (i % 65536 == 0) report(progress, WallDistanceStage::Encoding, double(i) / (2. * count));
            const double d = std::sqrt(distances[i]);
            original.distance[i] = uint8_t(std::lround(255. * std::clamp(std::log1p(d / distance_scale) / log_range, 0., 1.)));
        }
        // Release construction scratch before allocating the fixed-point field.
        std::vector<float>().swap(distances);
        std::vector<float>().swap(planar);
        if (smoothing_sigma == 0.) {
            report(progress, WallDistanceStage::Encoding, 1.);
            report(progress, WallDistanceStage::Complete, 1.);
            return;
        }
        std::vector<int16_t> values(count);
        for (size_t i = 0; i < count; ++i) {
            if (i % 65536 == 0) report(progress, WallDistanceStage::Encoding, (1. + double(i) / count) / 2.);
            values[i] = int16_t(std::lround(value(original, i) * distance_units));
        }
        report(progress, WallDistanceStage::Encoding, 1.);
        smooth_distances(values, size, spacing, smoothing_sigma, progress);
        smoothed.distance.resize(count);
        smoothed.inside.assign((count + 7) / 8, 0);
        for (size_t i = 0; i < count; ++i) {
            if (i % 65536 == 0) report(progress, WallDistanceStage::Complete, double(i) / count);
            const double d = std::abs(double(values[i])) / distance_units;
            smoothed.distance[i] = uint8_t(std::lround(255. * std::clamp(std::log1p(d / distance_scale) / log_range, 0., 1.)));
            if (values[i] < 0) smoothed.inside[i / 8] |= uint8_t(1) << (i % 8);
        }
        report(progress, WallDistanceStage::Complete, 1.);
    }
};

CubicWallDistance::CubicWallDistance(std::vector<WallSlice> slices, double smoothing_sigma, const WallDistanceProgress &progress)
    : m_table(std::make_shared<Table>(std::move(slices), smoothing_sigma, progress)) {}

std::vector<WallDistanceSample> CubicWallDistance::sample_fields(const std::vector<Vec3d> &points) const
{
    std::vector<WallDistanceSample> result(points.size(), {Table::far_distance, Table::far_distance});
    if (m_table->original.distance.empty()) return result;
    const Table &t = *m_table;
    const auto &smoothed = t.smoothed.distance.empty() ? t.original : t.smoothed;
    for (size_t i = 0; i < points.size(); ++i) {
        const Vec3d coordinate = (points[i] - t.origin) / t.spacing;
        const Vec3d upper(double(t.size[0] - 1), double(t.size[1] - 1), double(t.size[2] - 1));
        const Vec3d clamped = coordinate.cwiseMax(Vec3d::Zero()).cwiseMin(upper);
        std::array<size_t, 3> low;
        Vec3d fraction;
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(size_t(std::floor(clamped[axis])), t.size[axis] - 2);
            fraction[axis] = clamped[axis] - low[axis];
        }
        WallDistanceSample value {0., 0.};
        for (size_t z = 0; z < 2; ++z)
            for (size_t y = 0; y < 2; ++y)
                for (size_t x = 0; x < 2; ++x) {
                    const size_t index = ((low[2] + z) * t.size[1] + low[1] + y) * t.size[0] + low[0] + x;
                    const double weight = (x ? fraction.x() : 1. - fraction.x()) *
                        (y ? fraction.y() : 1. - fraction.y()) * (z ? fraction.z() : 1. - fraction.z());
                    value.distance += t.value(t.original, index) * weight;
                    value.smoothed += t.value(smoothed, index) * weight;
                }
        // Outside the table, extend positive distance rather than allocating
        // an unbounded halo for background control points.
        const double outside = (coordinate - clamped).norm() * t.spacing;
        auto extend = [outside](double d) {
            return outside > 0. ? std::min(Table::far_distance, std::hypot(std::max(0., d), outside)) : d;
        };
        result[i] = {extend(value.distance), extend(value.smoothed)};
    }
    return result;
}

std::vector<double> CubicWallDistance::sample(const std::vector<Vec3d> &points) const
{
    const auto fields = sample_fields(points);
    std::vector<double> result;
    result.reserve(fields.size());
    for (const auto &field : fields) result.push_back(field.distance);
    return result;
}

std::shared_ptr<const PointCloudProvider> WallDistanceCloudCache::get(const std::function<std::vector<WallSlice>()> &load_slices, double decay, double sigma,
                                                                   const WallDistanceProgress &progress)
{
    if (!std::isfinite(decay) || decay < 0. || !std::isfinite(sigma) || sigma < 0.)
        throw std::invalid_argument("Invalid Voronoi distance settings");
    std::lock_guard<std::mutex> lock(m_mutex);
    auto &provider = m_providers[{decay, sigma}];
    if (!provider) {
        if (decay == 0. && sigma == 0.) {
            provider = std::make_shared<PoissonPointCloud>();
        } else {
            auto &table = m_tables[sigma];
            if (!table) table = std::make_shared<CubicWallDistance>(load_slices(), sigma, progress);
            provider = std::make_shared<WallDistancePointCloud>(
                [table](const auto &points) { return table->sample_fields(points); }, decay);
        }
    }
    return provider;
}

void WallDistanceCloudCache::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_providers.clear();
    m_tables.clear();
}

} // namespace Slic3r::Voronoi
