#ifndef slic3r_FillMagma_hpp_
#define slic3r_FillMagma_hpp_

#include "FillBase.hpp"
#include "../Magma/MagmaTriangleCell.hpp"

namespace Slic3r {

namespace magma { class MagmaTubeMap; }

// Shared base for every Magma infill pattern (triangle today; rectilinear /
// square later). Holds the pattern-agnostic plumbing — the non-owning tube map
// pointer set by Fill.cpp and the self-crossing / no-sort overrides — so a new
// pattern only supplies its own line generation. Fill.cpp attaches the tube map
// by casting to FillMagmaBase*, which means any subclass gets it (no silent
// null-tube_map when a new pattern is added).
class FillMagmaBase : public Fill
{
public:
    // Pre-computed tube map (set by Fill.cpp, non-owning)
    const magma::MagmaTubeMap* tube_map = nullptr;

    // Magma patterns are self-crossing (multiple line families intersect).
    bool is_self_crossing() override { return true; }

    // Preserve connect_infill's merged ordering — the GCode generator's TSP
    // re-sort produces chaotic routing with many window-gap fragments.
    bool no_sort() const override { return true; }
};

// Magma Triangle infill pattern for vertical reinforcement
//
// Creates a triangle grid pattern with:
// - Circular spiral offset per layer for helical interlocking tubes
// - Staggered windows (gaps in shared walls) that create U-tube pairs
// - Proper cell sizing for injection nozzle requirements
//
// Generates lines directly from the TriangleLattice (not via multiline engine).
// Window gaps are built into line generation, not clipped after the fact.
class FillMagmaTriangle : public FillMagmaBase
{
public:
    Fill* clone() const override { return new FillMagmaTriangle(*this); }
    ~FillMagmaTriangle() override = default;

protected:
    // Fixed angle - pattern doesn't rotate between layers
    // (spiral interlock comes from circular translation offset)
    float _layer_angle(size_t idx) const override { return 0.f; }

    // Use origin (0,0) as reference point. Pattern grid is aligned to
    // world origin. Spiral offset handles layer-to-layer variation.
    std::pair<float, Point> _infill_direction(const Surface *surface) const override;

    // Generate triangle infill for one ExPolygon region.
    // Lines are generated directly from the TriangleLattice with window gaps
    // built in, then clipped to the polygon via intersection_pl and anchored
    // via chain_or_connect_infill.
    void _fill_surface_single(
        const FillParams &params,
        unsigned int thickness_layers,
        const std::pair<float, Point> &direction,
        ExPolygon expolygon,
        Polylines &polylines_out) override;
};

// Magma Rectilinear (square grid) infill pattern.
//
// Two perpendicular single-wall line families (one horizontal per row, one
// vertical per column) forming square cells. Seals more easily than the triangle
// (circumscribed/inscribed ratio sqrt2 ~= 1.41 vs 2.0) and prints faster (straight
// axis-aligned lines, 2 families instead of 3). Window gaps for U-tube pairs are
// added in a later step (#24); for now the squares are closed.
class FillMagmaRectilinear : public FillMagmaBase
{
public:
    Fill* clone() const override { return new FillMagmaRectilinear(*this); }
    ~FillMagmaRectilinear() override = default;

protected:
    float _layer_angle(size_t idx) const override { return 0.f; }
    std::pair<float, Point> _infill_direction(const Surface *surface) const override;
    void _fill_surface_single(
        const FillParams &params,
        unsigned int thickness_layers,
        const std::pair<float, Point> &direction,
        ExPolygon expolygon,
        Polylines &polylines_out) override;
};

} // namespace Slic3r

#endif // slic3r_FillMagma_hpp_
