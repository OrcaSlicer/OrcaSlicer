#pragma once

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polyline.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <optional>
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
    Result route(const Polyline &travel, double start_z, double end_z, int extruder_id) const;

private:
    struct RoutingSpace
    {
        std::vector<BedExcludeRegion> regions;
        Polygon bed_shape;
    };

    struct ActiveObstacles
    {
        ExPolygons obstacles;
        ExPolygons valid_bed;
    };

    std::optional<ActiveObstacles> active_obstacles(
        const RoutingSpace &space,
        double z_min,
        double z_max) const;

    std::vector<RoutingSpace> m_spaces;
    coord_t m_clearance { 0 };
};

} // namespace Slic3r
