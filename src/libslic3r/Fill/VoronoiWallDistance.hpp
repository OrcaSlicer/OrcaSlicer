#pragma once

#include "../ExPolygon.hpp"
#include "../BoundingBox.hpp"
#include "VoronoiPointCloud.hpp"
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace Slic3r::Voronoi {

// References the object's complete sliced outlines, including holes. These
// outlive its fillers and are read-only while parallel infill is generated.
// Supply non-overlapping layer slabs in increasing Z order.
struct WallSlice {
    double bottom_z;
    double top_z;
    const ExPolygons *contours;
};

enum class WallDistanceStage { Distance, Encoding, SmoothXY, SmoothZ, Complete };
// Called synchronously during construction, including within long phases.
// The callback may throw to cancel construction; no partial table is cached.
using WallDistanceProgress = std::function<void(WallDistanceStage, double)>;

// Approximate signed distance for density control, assembled from every sliced
// slab using Orca's planar SDF. Original and Gaussian-smoothed fields each use
// one distance byte and one sign bit per node.
class CubicWallDistance {
public:
    explicit CubicWallDistance(std::vector<WallSlice> slices, double smoothing_sigma = 2.,
                               const WallDistanceProgress &progress = {});
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
                                                  const WallDistanceProgress &progress = {});
    void clear();
private:
    std::mutex m_mutex;
    std::map<double, std::shared_ptr<const CubicWallDistance>> m_tables;
    std::map<std::pair<double, double>, std::shared_ptr<const PointCloudProvider>> m_providers;
};

} // namespace Slic3r::Voronoi
