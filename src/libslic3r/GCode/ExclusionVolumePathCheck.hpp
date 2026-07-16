#pragma once

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace Slic3r {

enum class ExclusionVolumeMotionType : unsigned char
{
    Travel,
    Extrude,
    Other
};

struct ExclusionVolumePathHit
{
    size_t                    move_id { 0 };
    unsigned int              gcode_id { 0 };
    ExclusionVolumeMotionType motion_type { ExclusionVolumeMotionType::Other };
    unsigned char             source_move_type { 0 };
    int                       extruder_id { -1 };
    size_t                    region_id { 0 };
    Vec3d                     from { Vec3d::Zero() };
    Vec3d                     to { Vec3d::Zero() };
    bool                      used_unknown_z { false };
};

struct ExclusionVolumePathCheckResult
{
    bool has_any_conflict { false };
    bool has_travel_conflict { false };
    bool has_extrusion_conflict { false };
    bool has_other_motion_conflict { false };
    std::optional<ExclusionVolumePathHit> first_hit;
};

// Checks G-code coordinates as they are parsed. Exclusion regions are expressed
// in nozzle/model space, so each candidate physical extruder offset is applied
// before testing a motion. This keeps the checker independent of preview
// vertices, whose extruder_id field represents a filament rather than a nozzle.
class ExclusionVolumePathChecker
{
public:
    void configure(const PrintConfig &config);
    void clear();

    bool configured() const { return m_configured; }
    const ExclusionVolumePathCheckResult &result() const { return m_result; }

    void check_motion(
        const Vec3d &from,
        const Vec3d &to,
        bool start_xy_known,
        bool end_xy_known,
        bool start_z_known,
        bool end_z_known,
        int active_extruder_id,
        ExclusionVolumeMotionType motion_type,
        unsigned char source_move_type,
        unsigned int gcode_id,
        size_t move_id);

private:
    struct PreparedRegion
    {
        BedExcludeRegion region;
        BoundingBox bbox;
        AABBTreeLines::LinesDistancer<Line> edge_tree;
    };

    bool segment_intersects_region(
        const Vec3d &from,
        const Vec3d &to,
        bool z_known,
        const PreparedRegion &region) const;

    bool m_configured { false };
    std::vector<Vec2d> m_extruder_offsets;
    std::vector<std::vector<PreparedRegion>> m_regions_by_extruder;
    ExclusionVolumePathCheckResult m_result;
};

} // namespace Slic3r
