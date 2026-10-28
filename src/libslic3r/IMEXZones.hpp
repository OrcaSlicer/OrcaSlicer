#pragma once

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// Where every IMEX bed zone, carriage collision strip and advisory margin band sits, in
// plate-local millimetres. Purely geometric: no rendering, no clipping against the bed
// outline, no GUI types. PartPlate turns the rectangles into GLModels (clipping each one
// to the bed polygon as it goes) and copies the boxes straight into the members that
// check_outside() consults, so this struct is the whole answer to "which boxes".
//
// Every list is empty and `primary_zone_box` unset when no parallel mode is in effect —
// IMEX off, Primary mode, a 1x1 tool grid, a mode whose tool roster is empty, or a mode
// whose Primary tool is not on the current grid (no `:P` marker, or a `:P` index left
// over from a wider grid). The whole layout hangs off the primary's cell, so without one
// there is nothing to divide and consumers must see the same "no zones" answer.
struct ImexZoneLayout
{
    // Physical T-index of the active mode's Primary tool; -1 when the layout is empty.
    int primary_head = -1;

    // Physical head -> XY center of that head's zone. One entry per tool the mode makes
    // active, Primary included. A Span tool has no entry: it shares the primary's zone
    // rather than owning one. Ghost placement offsets by center[head] - center[primary].
    //
    // EXCEPTION, read before using the X component. These centers are built from the head's
    // OWN grid cell, but an AGGREGATED representative's zone rectangle is pinned to the
    // primary's column and then expanded to a full-width row strip. So for an aggregated head
    // whose own column differs from the primary's, center[head].x names a column its painted
    // zone does not have, and `center[head] - center[primary]` carries a bogus X offset.
    // Reachable at imex_tools_per_gantry >= 3 when the primary-column tool on the aggregated
    // gantry is inactive (e.g. tpg=3, gantry_count=2, mode "0:P,1:S,4:M,5:M").
    // Both current consumers pin X to the primary's center for aggregated heads --
    // PartPlate::calc_imex_ghosts() via bed_x_center, GCodeViewer via sec_center_of() -- and a
    // new consumer must do the same until this is either pinned here or the aggregate flag is
    // exposed on the layout.
    std::map<int, Vec2d> head_zone_centers;

    // The clear printable area — the primary tool's zone. Objects go here and nowhere else.
    std::optional<BoundingBoxf> primary_zone_box;

    // Copy / Mirror zone rectangles, already expanded along whichever axis carries no
    // separation: secondaries differing from primary only by row give full-bed-width
    // strips, only by column give full-bed-height strips, and differing on both axes give
    // per-tool quadrants.
    std::vector<BoundingBoxf> copy_zones;
    std::vector<BoundingBoxf> mirror_zones;

    // `copy_zones` followed by `mirror_zones`, as full-height boxes for the placement
    // check. An object overlapping one of these is outside the primary zone.
    std::vector<BoundingBoxf3> secondary_zone_boxes;

    // Carriage danger strips lying just inside the primary zone's boundary, one per
    // boundary that faces an adjacent Mirror ZONE -- an aggregated gantry contributes the
    // one pinned zone it paints, not one per head -- in right / left / top / bottom order.
    // Width is the literal nozzle clearance for that axis. Copy tools never contribute:
    // they travel in the same direction as the primary and cannot close on it.
    std::vector<BoundingBoxf3> collision_zones;

    // Advisory (non-blocking) bands immediately inside each collision strip, present only
    // when imex_carriage_margin > 0. Same boundary order as `collision_zones`.
    std::vector<BoundingBoxf> margin_bands;
};

// Computes the IMEX zone layout for one plate.
//
// `printer_cfg`   — the edited printer preset's config. Read for is_imex, the tool grid
//                   (imex_gantry_count / imex_tools_per_gantry / imex_tool_layout), the
//                   mode roster (imex_mode_names / imex_mode_active_tools) and the strip
//                   widths (imex_nozzle_clearance_x / _y, imex_carriage_margin). A key absent
//                   from the config falls back to the value registered for that option in
//                   print_config_def, via the imex_cfg_* accessors.
// `plate_mode`    — the plate's own IMEX mode. Wins over the process preset unless it is
//                   `kImexPrimaryMode`.
// `process_mode`  — the process preset's `imex_parallel_mode`, used only as the fallback
//                   when `plate_mode` is `kImexPrimaryMode`. Pass "" when unavailable.
// `bed_extents`   — XY extents of the plate shape, in plate-local mm. Zones subdivide it.
//
// Zone sizing keys off the count of ACTIVE tools per axis rather than the grid dimensions,
// so a tool the mode leaves out donates its share of the bed to its active neighbours.
ImexZoneLayout compute_imex_zone_layout(const DynamicPrintConfig& printer_cfg,
                                        const std::string&        plate_mode,
                                        const std::string&        process_mode,
                                        const BoundingBoxf&       bed_extents);

// Every config input to a plate's IMEX zones and ghosts: what compute_imex_zone_layout() reads from
// the printer preset, plus the plate's resolved mode and imex_firmware_managed_zones, which decides
// whether the plate draws ghosts. The bed is left out because a plate rebuilds its zones in
// set_shape(). An option that moves a zone or a ghost must be added here, or they go stale silently.
struct ImexLayoutKey
{
    // Every other field stays at its default while imex is false.
    struct Scalars
    {
        bool   imex             = false;
        int    gantry_count     = 0;
        int    tools_per_gantry = 0;
        int    tool_layout      = -1;
        double clearance_x      = 0.0;
        double clearance_y      = 0.0;
        double carriage_margin  = 0.0;
        bool   firmware_managed = false;

        bool operator==(const Scalars& rhs) const
        {
            // A clearance field accepts "nan", so NaN equals itself here, or the key would change
            // every frame.
            auto same = [](double a, double b) { return a == b || (std::isnan(a) && std::isnan(b)); };
            return std::tie(imex, gantry_count, tools_per_gantry, tool_layout, firmware_managed) ==
                       std::tie(rhs.imex, rhs.gantry_count, rhs.tools_per_gantry, rhs.tool_layout, rhs.firmware_managed) &&
                   same(clearance_x, rhs.clearance_x) && same(clearance_y, rhs.clearance_y) &&
                   same(carriage_margin, rhs.carriage_margin);
        }
    };
    Scalars s;
    // The resolved mode and the printer's whole mode table. A name doesn't identify a roster: two
    // presets can give one name different rosters, and editing a roster keeps its name.
    std::string              mode;
    std::vector<std::string> mode_names;
    std::vector<std::string> mode_active_tools;
};

// Brings `key` up to date with `printer_cfg` and the plate's resolved `mode`, and returns
// whether it changed. A null `printer_cfg` reads as IMEX off. Plates and the G-code preview call
// this every frame, so it compares against the live config and copies only when something changed.
bool update_imex_layout_key(std::optional<ImexLayoutKey>& key, const DynamicPrintConfig* printer_cfg, const std::string& mode);

} // namespace Slic3r
