#ifndef slic3r_FillScales_hpp_
#define slic3r_FillScales_hpp_

#include "../libslic3r.h"

#include "FillBase.hpp"

namespace Slic3r {

// Orca: overlapping scales, the seigaiha ("blue sea waves") pattern. A decorative top and bottom
// surface pattern - it has no cross-layer structure, so it is not offered as a sparse infill.
//
// Discs of radius R sit on the lattice x = i * sqrt(3) * R + (j & 1) * sqrt(3) * R / 2, y = j * R / 2,
// and a disc is hidden by every disc in a lower row. Those proportions are forced, not chosen:
// the two discs directly in front of a disc are then exactly R from its centre, so they meet at
// that centre and no disc ever exposes a closed ring, while the lattice is still just dense enough
// to cover the plane. Painter's order over a full covering makes the visible parts of the discs an
// exact tiling of the plane, which is what fixes the density - see FillScales.cpp.
//
// Each visible part is drawn as m_arcs concentric arcs one line distance apart, so at 100% surface
// density the scales close up into a solid surface and lowering the density opens them out.
class FillScales : public Fill
{
public:
    explicit FillScales(size_t arcs) : m_arcs(arcs) {}
    ~FillScales() override = default;
    bool is_self_crossing() override { return false; }

protected:
    Fill* clone() const override { return new FillScales(*this); }
    void _fill_surface_single(
        const FillParams                &params,
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction,
        ExPolygon                        expolygon,
        Polylines                       &polylines_out) override;

    // Concentric arcs per scale. Also the scale radius, in units of the distance between
    // neighbouring lines, so the whole pattern scales with the density.
    size_t m_arcs;
};

} // namespace Slic3r

#endif // slic3r_FillScales_hpp_
