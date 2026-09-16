#pragma once

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

#include "ExtrusionEntity.hpp"

// Periodic feature recoloring.
//
// Each object can have patterns that print one feature with another filament in bands that repeat along Z.
// They are stored as a flat list of doubles in the `periodic_recolor_patterns` object setting.
//
// ToolOrdering::collect_periodic_recolor_extruders() adds the pattern filaments to the layers they print on, so
// the prime tower, flush volumes and nozzle grouping plan for them, and GCode::process_layer() prints the matching
// extrusions with them. Both build the same plan and ask first_matching_filament() for each extrusion's filament.
// The filament a pattern replaces, and the outer/inner wall split, come from LayerTools.
//
// Patterns work with By layer printing only; Print::validate rejects them for By object prints.

namespace Slic3r {

class ConfigBase;
class PrintObject;

// ---------------------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------------------

// Features a pattern can target, in the order the gizmo's Feature dropdown lists them. erExternalPerimeter also
// matches erOverhangPerimeter, so an outer wall keeps one color where it overhangs.
inline constexpr std::array<ExtrusionRole, 10> PERIODIC_RECOLOR_ROLES = {{
    erExternalPerimeter,
    erPerimeter,
    erInternalInfill,
    erSolidInfill,
    erTopSolidInfill,
    erBottomSurface,
    erBridgeInfill,
    erInternalBridgeInfill,
    erIroning,
    erGapFill,
}};

// Which point of a band sits on the mark it is placed at. The band grows from there, so changing
// its thickness never moves the marks. `h` below is band_vertical_height.
enum class PeriodicRecolorAlignment : int
{
    // Band spans [mark, mark + h]: the mark is the band's bottom.
    Bottom = 0,
    // Band spans [mark - h/2, mark + h/2]: the mark is the band's middle.
    Middle = 1,
    // Band spans [mark - h, mark]: the mark is the band's top.
    Top    = 2,
};

struct PeriodicRecolorPattern
{
    bool                   enabled              = true;
    // 1-based filament id, matching *_filament_id config.
    int                    filament             = 0;
    ExtrusionRole          role                 = erExternalPerimeter;
    // Heights in mm from the object's bottom (SlicingParameters::object_print_z_min), so a raft never shifts the
    // bands. Band N marks start + N * period, and only marks within [start, end] get a band.
    double                 start                = 0.;
    double                 end                  = 0.;
    double                 band_vertical_height = 0.;
    double                 period               = 0.;
    PeriodicRecolorAlignment alignment          = PeriodicRecolorAlignment::Top;

    // Whether the settings are usable; invalid patterns are ignored.
    bool is_valid(size_t num_filaments) const;
};

struct PeriodicRecolorPatterns
{
    std::vector<PeriodicRecolorPattern> patterns;

    static PeriodicRecolorPatterns from_doubles(const std::vector<double> &values);
    std::vector<double> to_doubles() const;

    // Appends the 0-based target filaments from enabled, valid patterns.
    void collect_filaments(size_t num_filaments, std::vector<unsigned int> &out_zero_based) const;
    // Updates the filaments after the 0-based filament `deleted` is removed: later ones shift down, and patterns on
    // the deleted one move to the 0-based `replacement`, or are disabled when it is -1.
    void delete_filament(size_t deleted, int replacement);
};

// ---------------------------------------------------------------------------------------
// Plan and layer rules
// ---------------------------------------------------------------------------------------

// The rules on one layer: which feature prints with which filament, in pattern order.
class PeriodicRecolorLayerRules
{
public:
    bool active() const { return !m_rules.empty(); }
    // The 0-based filament of the first pattern on this layer whose feature appears anywhere in `entity`, or -1.
    // A match recolors all of `entity`, so callers pass an unsortable collection whole, to keep its order, and a
    // sortable one child by child.
    int  first_matching_filament(const ExtrusionEntity &entity) const;

private:
    friend class PeriodicRecolorPlan;
    // One rule per covering pattern: {selected role, 0-based filament}, in pattern order. A rule equal to the
    // one before it is left out.
    std::vector<std::pair<ExtrusionRole, int>> m_rules;
};

// Which layers each pattern covers, for one object. Looked up by (print_z, height) rather than layer index, because
// ToolOrdering and GCode::process_layer reach layers by different routes.
class PeriodicRecolorPlan
{
public:
    // Builds the plan for a sliced object. ToolOrdering and GCode::process_layer both use it, so they agree on which
    // layers each band covers.
    static PeriodicRecolorPlan build(const PrintObject &object);

    bool empty() const { return m_layers.empty(); }
    // A layer the plan does not hold returns an empty rule set, which recolors nothing.
    const PeriodicRecolorLayerRules &rules_for(double print_z, double height) const;

private:
    struct Entry
    {
        double                    print_z = 0.; // layer top
        double                    height  = 0.; // layer height
        PeriodicRecolorLayerRules rules;
    };
    // In layer order: ascending by print_z, which rules_for() binary-searches.
    std::vector<Entry> m_layers;
};

// The patterns stored in a config, or none when the key is absent or empty.
PeriodicRecolorPatterns periodic_recolor_patterns_of(const ConfigBase &config);

// Appends the 0-based filaments this object's enabled, valid patterns name. It reads the settings, not the sliced
// layers, so it may list a filament that never prints; the checks that use it would rather have too many.
void periodic_recolor_append_targets(const PrintObject &object, std::vector<unsigned int> &out);

// The bands one pattern describes over an object of the given height, as [lo, hi) pairs in mm from the object's
// bottom, ignoring layers. The 3D preview draws these; the print can differ, since slicing rounds bands to whole
// layers. Returns at most `max_bands` bands.
std::vector<std::pair<double, double>> periodic_recolor_ideal_bands(const PeriodicRecolorPattern &pattern,
                                                                   double  object_height,
                                                                   size_t  max_bands);

} // namespace Slic3r
