#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../libslic3r.h"
#include "../Config.hpp"
#include "../ExPolygon.hpp"
#include "../Point.hpp"
#include "../Polyline.hpp"
#include "../PrintConfig.hpp"
#include "FillBase.hpp"

namespace Slic3r {

class PrintObject;

// Target density of the bone infill by depth below the walls, before the variation of the irregularity.
struct BoneGradient
{
    // Densities as fractions.
    double           wall_density { 0.15 };
    double           core_density { 0.05 };
    double           falloff { 7. };
    // 0 for none.
    double           hollow_depth { 0. };
    BoneFalloffCurve curve { BoneFalloffCurve::SCurve };
    // How far the density has gone from the wall density to the core density at 20%, 40%, 60% and 80% of
    // the falloff depth, as fractions.
    std::array<double, 4> custom { 0.2, 0.4, 0.6, 0.8 };

    // Cells sparser than this are wider than most cores, so no cell fits there and the walls of the cells
    // around the core reach across it as spokes. The core is left hollow where the density falls below it.
    static constexpr double HollowBelowDensity = 0.03;

    // From the bone_* settings, the sparse infill density and the interior density of a print or region config.
    static BoneGradient          from_config(const ConfigBase &config);
    // The custom curve as stored in bone_falloff_custom: four percentages separated by commas. Anything else
    // reads as the linear curve.
    static std::array<double, 4> parse_custom(const std::string &serialized);
    static std::string           serialize_custom(const std::array<double, 4> &custom);

    // Share of the way from the wall density to the core density at t in [0, 1] of the falloff depth.
    double progress(double t) const;
    double density(double depth) const;
    // Depth below the walls from which the core is hollow: the hollow depth, or shallower where the density
    // falls below HollowBelowDensity. Infinity when there is no hollow core.
    double hollow_from() const;
};

// Everything that shapes the walls of the bone infill.
struct BoneParams
{
    BoneGradient gradient;
    double       line_width { 0.45 };
    // Depth of the inner edge of the walls below the surface.
    double       shell { 0.9 };
    double       irregularity { 1. };
    // Cells are this many times taller than wide.
    double       stretch { 1. };
    double       max_bridge { 3. };
    int          seed { 7 };

    // From the settings of a print or region config, with the infill line width and wall depth.
    static BoneParams from_config(const ConfigBase &config, double line_width, double shell);
};

// One layer of an object to fill with bone infill.
struct BoneLayer
{
    coordf_t          bottom;
    coordf_t          top;
    const ExPolygons *outline;
    // The region to fill.
    ExPolygons        sparse;
};

// Whether a region fills its sparse infill with the bone infill: the pattern is Bone and the density is neither
// 0 (no infill) nor 100% (solid).
bool uses_bone_infill(const PrintRegionConfig &config);

// The walls of the bone infill of every layer, bottom to top, for any stack of layers, so that a sample shape
// can be filled without a print.
std::vector<Polylines> generate_bone_walls(const std::vector<BoneLayer> &layers, const BoneParams &params,
                                           const std::function<void()> &throw_if_canceled);

// Depth below the surface of the deepest point of a stack of layers, on the same distance field as the infill. Only
// the outlines of the layers are used. The tuning dialog shows it to put the gradient in proportion to the part.
double bone_max_depth(const std::vector<BoneLayer> &layers, const std::function<void()> &throw_if_canceled);

// Bone infill: the walls of a 3D foam graded like cancellous bone. Cells are small just inside the
// walls and grow towards the core, which can be hollow like a medullary cavity. Every layer is a
// cross-section of the same 3D foam, so its walls tilt in every direction and brace the part
// laterally and in torsion as well as in Z. The walls of each layer are pruned so that each rests on
// the walls below, or bridges a short gap between supported points.
//
// The walls of all layers are generated once per object, because pruning goes up through the layers;
// FillBone then clips the walls of its layer to each surface.
class BoneInfill
{
public:
    // Built from the outlines and the sparse infill surfaces of the layers of the object, with the
    // settings of the first region using the bone infill.
    BoneInfill(const PrintObject &object, const std::function<void()> &throw_if_canceled);

    // Walls of the layer at print_z, in the coordinates of the object. Empty for other heights.
    const Polylines &walls(coordf_t print_z) const;

private:
    std::vector<coordf_t>  m_print_z;
    std::vector<Polylines> m_walls;
};

using BoneInfillPtr = std::unique_ptr<BoneInfill>;

class FillBone : public Fill
{
public:
    ~FillBone() override = default;
    bool is_self_crossing() override { return false; }

    const BoneInfill *bone_infill { nullptr };

protected:
    Fill *clone() const override { return new FillBone(*this); }

    void _fill_surface_single(const FillParams              &params,
                              unsigned int                   thickness_layers,
                              const std::pair<float, Point> &direction,
                              ExPolygon                      expolygon,
                              Polylines                     &polylines_out) override;
};

} // namespace Slic3r
