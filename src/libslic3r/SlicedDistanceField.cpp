#include "SlicedDistanceField.hpp"
#include "EdgeGrid.hpp"
#include "Polygon.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Slic3r {
namespace {
void report(const SlicedDistanceField::Progress &progress, SlicedDistanceField::Stage stage, double fraction)
{
    if (progress) progress(stage, fraction);
}
std::vector<SlicedDistanceField::Slice> merge_profiles(std::vector<SlicedDistanceField::Slice> slices)
{
    std::vector<SlicedDistanceField::Slice> merged;
    for (SlicedDistanceField::Slice slice : slices) {
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
} // namespace

SlicedDistanceField::SlicedDistanceField(std::vector<Slice> slices, const Progress &progress)
{
    report(progress, Stage::Distance, 0.);
    const double log_range = std::log1p(far_distance / distance_scale);
    // Decode once so batched interpolation needs no logarithms or exponentials.
    for (size_t i = 0; i < decoded.size(); ++i)
        decoded[i] = distance_scale * std::expm1(log_range * i / 255.);
    slices = merge_profiles(std::move(slices));
    BoundingBoxf3 bounds;
    for (const Slice &slice : slices)
        for (const ExPolygon &polygon : *slice.contours) {
            const BoundingBox box = polygon.contour.bounding_box();
            if (!box.defined) continue;
            bounds.merge(Vec3d(unscale<double>(box.min.x()), unscale<double>(box.min.y()), slice.bottom_z));
            bounds.merge(Vec3d(unscale<double>(box.max.x()), unscale<double>(box.max.y()), slice.top_z));
        }
    if (!bounds.defined) {

        report(progress, Stage::Complete, 1.);
        return;
    }
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
    std::vector<Slice> slabs;
    double previous_top = slices.front().bottom_z;
    for (const Slice &slice : slices) {
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
                report(progress, Stage::Distance,
                       ((direction == 1 ? 0. : 1.) + double(output) / size[2]) / 2.);
            }
        };
        for (size_t i = 0; i < slabs.size(); ++i) {
            const Slice &slice = slabs[direction == 1 ? i : slabs.size() - 1 - i];
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
            report(progress, Stage::Distance,
                   ((direction == 1 ? 0. : 1.) + double(output) / size[2]) / 2.);
        }
        accumulate(std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), true);
    }
    report(progress, Stage::Encoding, 0.);
    original.distance.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (i % 65536 == 0) report(progress, Stage::Encoding, double(i) / count);
        const double d = std::sqrt(distances[i]);
        original.distance[i] = uint8_t(std::lround(255. * std::clamp(std::log1p(d / distance_scale) / log_range, 0., 1.)));
    }
    report(progress, Stage::Complete, 1.);
}

SlicedDistanceField::Interpolation SlicedDistanceField::interpolation(const Vec3d &point) const
{
    Interpolation result;
    if (original.distance.empty()) return result;
    const Vec3d coordinate = (point - origin) / spacing;
    const Vec3d upper(double(size[0] - 1), double(size[1] - 1), double(size[2] - 1));
    const Vec3d clamped = coordinate.cwiseMax(Vec3d::Zero()).cwiseMin(upper);
    std::array<size_t, 3> low;
    Vec3d fraction;
    for (int axis = 0; axis < 3; ++axis) {
        low[axis] = std::min(size_t(std::floor(clamped[axis])), size[axis] - 2);
        fraction[axis] = clamped[axis] - low[axis];
    }
    size_t i = 0;
    for (size_t z = 0; z < 2; ++z)
        for (size_t y = 0; y < 2; ++y)
            for (size_t x = 0; x < 2; ++x, ++i) {
                result.indices[i] = ((low[2] + z) * size[1] + low[1] + y) * size[0] + low[0] + x;
                result.weights[i] = (x ? fraction.x() : 1. - fraction.x()) *
                    (y ? fraction.y() : 1. - fraction.y()) * (z ? fraction.z() : 1. - fraction.z());
            }
    result.outside = (coordinate - clamped).norm() * spacing;
    return result;
}

double SlicedDistanceField::sample(const Interpolation &where) const
{
    if (original.distance.empty()) return far_distance;
    double distance = 0.;
    for (size_t i = 0; i < 8; ++i) distance += node_value(where.indices[i]) * where.weights[i];
    return where.outside > 0. ? std::min(far_distance, std::hypot(std::max(0., distance), where.outside)) : distance;
}

std::vector<double> SlicedDistanceField::sample(const std::vector<Vec3d> &points) const
{
    std::vector<double> result;
    result.reserve(points.size());
    for (const Vec3d &point : points) result.push_back(sample(interpolation(point)));
    return result;
}

} // namespace Slic3r
