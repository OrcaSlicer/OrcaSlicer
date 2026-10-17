#pragma once

#include "../ExPolygon.hpp"
#include "../SlicedDistanceField.hpp"
#include "../BoundingBox.hpp"
#include "VoronoiPointCloud.hpp"
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <tuple>
#include <vector>

namespace Slic3r::Voronoi {

using WallSlice = SlicedDistanceField::Slice;

enum class WallDistanceStage { Distance, Encoding, SmoothXY, SmoothZ, MaterialDensity, Complete };
// Called synchronously during construction, including within long phases.
// The callback may throw to cancel construction; no partial table is cached.
using WallDistanceProgress = std::function<void(WallDistanceStage, double)>;

// Material density prepared from the sliced distance field, with Gaussian
// corner reinforcement, decay and optional modifier blending.
class CubicWallDistance {
public:
    explicit CubicWallDistance(std::vector<WallSlice> slices, double smoothing_sigma = 2.,
                               const WallDistanceProgress &progress = {}, double decay = 6., double material_density = 0.25);
    double grid_spacing() const;
    double maximum_density_ratio() const;
    std::vector<double> sample(const std::vector<Vec3d> &points) const;
    std::vector<WallDistanceSample> sample_fields(const std::vector<Vec3d> &points) const;
private:
    struct Table;
    std::shared_ptr<const Table> m_table;
};

// Owned by a PrintObject. Initialization is serialized; providers and distance
// data are immutable and can subsequently be used by parallel layer workers.
class WallDistanceCloudCache {
public:
    std::shared_ptr<const PointCloudProvider> get(const std::function<std::vector<WallSlice>()> &load_slices,
                                                  double decay = 6., double sigma = 8.,
                                                  const WallDistanceProgress &progress = {}, double angle = 0., double material_density = 0.25);
    void clear();
private:
    std::mutex m_mutex;
    std::map<std::tuple<double, double, double>, std::shared_ptr<const CubicWallDistance>> m_tables;
    std::map<std::tuple<double, double, double, double>, std::shared_ptr<const PointCloudProvider>> m_providers;
};

} // namespace Slic3r::Voronoi
