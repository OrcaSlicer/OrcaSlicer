#include "TpmsField.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "../MarchingSquares.hpp"
#include "../Execution/ExecutionTBB.hpp"

namespace marchsq {
using namespace Slic3r;

struct TpmsField
{
    static constexpr float rsizef = 0.004f; // raster pixel size in mm (roughly point accuracy).
    const coord_t          rsize  = scaled(rsizef);
    const long             gsize;           // grid cell size in raster pixels.

    const TpmsEquation &tpms;
    Vec3d               frequency;
    Point               size;
    Point               offs;
    coordf_t            z;

    TpmsField(const TpmsEquation &tpms, const Vec3d &frequency, const BoundingBox &bbox, coordf_t z, double cell)
        : gsize(std::lround(cell / rsizef)), tpms(tpms), frequency(frequency), size(bbox.size()), offs(bbox.min), z(z)
    {}

    float get_scalar(const Coord &p) const
    {
        const Point pt = to_Point(p);
        return tpms(frequency.x() * unscaled(pt.x()), frequency.y() * unscaled(pt.y()), frequency.z() * z);
    }

    inline coord_t to_coord(long x) const { return x * rsize; }
    inline long    to_coordr(coord_t x) const { return x / rsize; }
    inline Point   to_Point(const Coord &p) const { return Point(to_coord(p.c) + offs.x(), to_coord(p.r) + offs.y()); }
};

template<> struct _RasterTraits<TpmsField>
{
    using ValueType = float;
    static float  get(const TpmsField &sf, size_t row, size_t col) { return sf.get_scalar(Coord(long(row), long(col))); }
    static size_t rows(const TpmsField &sf) { return sf.to_coordr(sf.size.y()); }
    static size_t cols(const TpmsField &sf) { return sf.to_coordr(sf.size.x()); }
};

} // namespace marchsq

namespace Slic3r {

Polylines make_tpms_polylines(const TpmsEquation &tpms, const Vec3d &frequency, const BoundingBox &bbox, coordf_t z, double cell)
{
    const marchsq::TpmsField raster(tpms, frequency, bbox, z, cell);
    // Within an eighth of a cell finer than the default one.
    const double tolerance = std::min(SPARSE_INFILL_RESOLUTION, cell / 8.) / SCALING_FACTOR;
    const std::vector<marchsq::Ring> rings = marchsq::execute_with_policy(ex_tbb, raster, 0.f, {raster.gsize, raster.gsize});
    Polylines polylines;
    polylines.reserve(rings.size());
    for (const marchsq::Ring &ring : rings) {
        Polyline &polyline = polylines.emplace_back();
        polyline.points.reserve(ring.size() + 1);
        for (const marchsq::Coord &crd : ring)
            polyline.points.emplace_back(raster.to_Point(crd));
        // The rings are polygons.
        polyline.points.push_back(polyline.points.front());
        polyline.simplify(tolerance);
    }
    return polylines;
}

BoundingBox tpms_sampling_bbox(BoundingBox bbox, double cell)
{
    const coord_t step = coord_t(std::lround(cell / SCALING_FACTOR));
    bbox.offset(step);
    bbox.merge(align_to_grid(bbox.min, Point(step, step)));
    return bbox;
}

} // namespace Slic3r
