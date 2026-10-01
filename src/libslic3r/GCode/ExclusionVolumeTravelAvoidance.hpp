#pragma once

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <map>
#include <vector>

namespace Slic3r {

class ExclusionVolumeTravelAvoidance
{
public:
    enum class Status : unsigned char
    {
        Unchanged,
        Rerouted,
        EndpointInside,
        Failed
    };

    enum class Detail : unsigned char
    {
        None,
        UnknownExtruder,
        NoActiveObstacles,
        NoIntersection,
        EndpointInside,
        SegmentOutsideBed,
        DetourFailed,
        FinalPathInvalid,
        IterationLimit
    };

    struct Result
    {
        Status status { Status::Unchanged };
        Detail detail { Detail::None };
        Polyline path;
        size_t active_obstacles { 0 };
        size_t iterations { 0 };

        bool rerouted() const { return status == Status::Rerouted; }
    };

    void init(const PrintConfig &config, const Vec3d &plate_origin);
    void clear();

    bool empty() const;

    // Input and output are scaled XY points in the active nozzle's generated
    // G-code coordinates before GCodeWriter subtracts the plate offset.
    Result route(const Polyline &travel, double start_z, double end_z, int extruder_id);

private:
    struct RoutingSpace
    {
        std::vector<BedExcludeRegion> regions;
        std::vector<BoundingBox>       region_bboxes;
        ExPolygons                     valid_bed;
        std::map<std::vector<size_t>, ExPolygons> obstacle_cache;
    };

    const ExPolygons *active_obstacles(
        RoutingSpace &space,
        double z_min,
        double z_max);

    std::vector<RoutingSpace> m_spaces;
    coord_t m_clearance { 0 };
};

} // namespace Slic3r
