#pragma once

#include "ExPolygon.hpp"
#include "Point.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace Slic3r {

// Approximate signed distance to completed slice slabs; negative inside.
// Slabs must be ordered, non-overlapping, and their contours valid during construction.
class SlicedDistanceField {
public:
    struct Slice { double bottom_z, top_z; const ExPolygons *contours; };
    enum class Stage { Distance, Encoding, Complete };
    using Progress = std::function<void(Stage, double)>;
    struct Interpolation {
        std::array<size_t, 8> indices {};
        std::array<double, 8> weights {};
        double outside = 0.;
    };
    static constexpr double far_distance = 1000.;
    SlicedDistanceField() = default;
    explicit SlicedDistanceField(std::vector<Slice> slices, const Progress &progress = {});
    double grid_spacing() const { return spacing; }
    const Vec3d &grid_origin() const { return origin; }
    const std::array<size_t, 3> &grid_size() const { return size; }
    size_t node_count() const { return original.distance.size(); }
    bool is_inside(size_t index) const { return original.inside[index / 8] & (uint8_t(1) << (index % 8)); }
    double node_value(size_t index) const {
        const double d = decoded[original.distance[index]];
        return is_inside(index) ? -d : d;
    }
    // Reuse the same interpolation weights for fields built on this grid.
    Interpolation interpolation(const Vec3d &point) const;
    double sample(const Interpolation &where) const;
    std::vector<double> sample(const std::vector<Vec3d> &points) const;
private:
    static constexpr double distance_scale = 6.;
    double spacing = 1.;
    Vec3d origin = Vec3d::Zero();
    std::array<size_t, 3> size {{0, 0, 0}};
    struct Field { std::vector<uint8_t> distance, inside; } original;
    std::array<double, 256> decoded {};
};

} // namespace Slic3r
