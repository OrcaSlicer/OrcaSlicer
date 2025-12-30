#include "../ClipperUtils.hpp"
#include "../MarchingSquares.hpp"
#include "FillTpmsFK.hpp"
#include <cmath>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace marchsq {
using namespace Slic3r;

using coordr_t = long; // length type for (r, c) raster coordinates.
// Note that coordf_t, Pointfs, Point3f, etc all use double not float.
using Pointf = Vec2d; // (x, y) field point in coordf_t.

struct ScalarField
{
    static constexpr float gsizef = 0.40;                        // grid cell size in mm (roughly line segment length).
    static constexpr float rsizef = 0.004;                       // raster pixel size in mm (roughly point accuracy).
    const coord_t          rsize  = scaled(rsizef);              // raster pixel size in coord_t.
    const coordr_t         gsize  = std::round(gsizef / rsizef); // grid cell size in coordr_t.
    Point                  size;                                 // field size in coord_t.
    Point                  offs;                                 // field offset in coord_t.
    coordf_t               z;                                    // z offset as a float.
    float                  freq;                                 // field frequency in cycles per mm.
    float                  isoval = 0.0;                         // iso value threshold to use.

    explicit ScalarField(const BoundingBox bb, const coordf_t z = 0.0, const float period = 10.0)
        : size{bb.size()}, offs{bb.min}, z{z}, freq{float(2 * PI) / period}
    {}

    // Get the scalar field value at x,y,z in coordf_t coordinates.
    float get_scalar(coordf_t x, coordf_t y, coordf_t z) const
    {
        const float fx = freq * x;
        const float fy = freq * y;
        const float fz = freq * z;

        // Fischer - Koch S equation:
        // cos(2x)sin(y)cos(z) + cos(2y)sin(z)cos(x) + cos(2z)sin(x)cos(y) = 0
        return cosf(2 * fx) * sinf(fy) * cosf(fz) + cosf(2 * fy) * sinf(fz) * cosf(fx) + cosf(2 * fz) * sinf(fx) * cosf(fy);
    }

    // Get the scalar field value at a Coord for the current z value.
    float get_scalar(Coord p) const
    {
        Pointf pf = to_Pointf(p);
        return get_scalar(pf.x(), pf.y(), z);
    }

    // Convert between dimension scales.
    inline coord_t  to_coord(const coordr_t& x) const { return x * rsize; }
    inline coordr_t to_coordr(const coord_t& x) const { return x / rsize; }

    // Convert between point/coordinate systems, including translation.
    inline Point  to_Point(const Coord& p) const { return Point(to_coord(p.c) + offs.x(), to_coord(p.r) + offs.y()); }
    inline Coord  to_Coord(const Point& p) const { return Coord(to_coordr(p.y() - offs.y()), to_coordr(p.x() - offs.x())); }
    inline Pointf to_Pointf(const Point& p) const { return Pointf(unscaled(p.x()), unscaled(p.y())); }
    inline Pointf to_Pointf(const Coord& p) const { return to_Pointf(to_Point(p)); }
};

// Register ScalarField as a RasterType for MarchingSquares.
template<> struct _RasterTraits<ScalarField>
{
    // The type of pixel cell in the raster
    using ValueType = float;

    // Value at a given position
    static float get(const ScalarField& sf, size_t row, size_t col) { return sf.get_scalar(Coord(row, col)); }

    // Number of rows and cols of the raster
    static size_t rows(const ScalarField& sf) { return sf.to_coordr(sf.size.y()); }
    static size_t cols(const ScalarField& sf) { return sf.to_coordr(sf.size.x()); }
};

// Get the polylines for the scalar field. The tolerance is used for
// simplifying the polylines to remove redundant points. The default will
// only remove points on (almost) perfectly straight lines. Set to -1 to turn
// off simplifying entirely. Note tolerance is the max line deviation from
// simplifying and should be scaled.
Polylines get_polylines(const ScalarField& sf, const double tolerance = SCALED_EPSILON)
{
    std::vector<Ring> rings = execute_with_policy(ex_tbb, sf, sf.isoval, {sf.gsize, sf.gsize});

    Polylines polys;
    polys.reserve(rings.size());
    // size_t old_pts = 0, new_pts = 0;

    for (const Ring& ring : rings) {
        Polyline poly;
        Points&  pts = poly.points;
        pts.reserve(ring.size() + 1);
        for (const Coord& crd : ring)
            pts.emplace_back(sf.to_Point(crd));
        // MarchingSquare's rings are polygons, so add the first point to the end to make it a PolyLine.
        pts.push_back(pts.front());
        // old_pts += poly.points.size();
        //  Simplify within specified tolerance to reduce points.
        if (tolerance >= 0.0)
            poly.simplify(tolerance);
        // new_pts += poly.points.size();
        polys.emplace_back(poly);
    }
    // std::cerr << "MarchingSquares: poly.simplify(" << tolerance << ") reduced points from" <<
    //     old_pts << " to " << new_pts << " (" << 100*new_pts/old_pts << "%)\n";
    return polys;
}

} // namespace marchsq

namespace Slic3r {

using namespace std;

void FillTpmsFK::_fill_surface_single(const FillParams&              params,
                                      unsigned int                   thickness_layers,
                                      const std::pair<float, Point>& direction,
                                      ExPolygon                      expolygon,
                                      Polylines&                     polylines_out)
{
    auto infill_angle = float(this->angle + (CorrectionAngle * 2 * M_PI) / 360.);
    if (std::abs(infill_angle) >= EPSILON)
        expolygon.rotate(-infill_angle);

    float density_factor = std::min(0.9f, params.density);
    // Density (field period) adjusted to have a good %of weight.
    const float vari_T = 4.18f * spacing * params.multiline / density_factor;

    BoundingBox bbox = expolygon.contour.bounding_box();
    // Enlarge the bounding box by the multi-line width to avoid artifacts at the edges.
    bbox.offset(scale_((params.multiline + 1) * spacing));
    marchsq::ScalarField sf = marchsq::ScalarField(bbox, this->z, vari_T);
    // Get simplified lines using coarse tolerance of 0.1mm (this is infill).
    Polylines polylines = marchsq::get_polylines(sf, SCALED_SPARSE_INFILL_RESOLUTION);

    // Apply multiline offset if needed
    multiline_fill(polylines, params, spacing);

    // Prune the lines within the expolygon.
    polylines = intersection_pl(std::move(polylines), expolygon);

    if (!polylines.empty()) {
        // Remove very small bits, but be careful to not remove infill lines connecting thin walls!
        // The infill perimeter lines should be separated by around a single infill line width.
        const double minlength = scale_(0.8 * this->spacing);
        polylines.erase(std::remove_if(polylines.begin(), polylines.end(),
                                       [minlength](const Polyline& pl) { return pl.length() < minlength; }),
                        polylines.end());
    }

    if (!polylines.empty()) {
        // connect lines
        size_t polylines_out_first_idx = polylines_out.size();

        // chain_or_connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);
        // chain_infill not situable for this pattern due to internal "islands", this also affect performance a lot.
        connect_infill(std::move(polylines), expolygon, polylines_out, this->spacing, params);

        // new paths must be rotated back
        if (std::abs(infill_angle) >= EPSILON) {
            for (auto it = polylines_out.begin() + polylines_out_first_idx; it != polylines_out.end(); ++it)
                it->rotate(infill_angle);
        }
    }
}

} // namespace Slic3r
