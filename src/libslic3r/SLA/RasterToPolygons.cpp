#include "RasterToPolygons.hpp"

#include "AGGRaster.hpp"
#include "libslic3r/MarchingSquares.hpp"
#include "MTUtils.hpp"
#include "ClipperUtils.hpp"

namespace marchsq {

// Specialize this struct to register a raster type for the Marching squares alg
template<> struct _RasterTraits<Slic3r::sla::RasterGrayscaleAA> {
    using Rst = Slic3r::sla::RasterGrayscaleAA;

    // The type of pixel cell in the raster
    using ValueType = uint8_t;

    // Value at a given position
    static uint8_t get(const Rst &rst, size_t row, size_t col) { return rst.read_pixel(col, row); }

    // Number of rows and cols of the raster
    static size_t rows(const Rst &rst) { return rst.resolution().height_px; }
    static size_t cols(const Rst &rst) { return rst.resolution().width_px; }
};

} // namespace Slic3r::marchsq

namespace Slic3r { namespace sla {

template<class Fn> void foreach_vertex(ExPolygon &poly, Fn &&fn)
{
    for (auto &p : poly.contour.points) fn(p);
    for (auto &h : poly.holes)
        for (auto &p : h.points) fn(p);
}

ExPolygons raster_to_polygons(const RasterGrayscaleAA &rst, Vec2i32 windowsize)
{
    size_t rows = rst.resolution().height_px, cols = rst.resolution().width_px;

    if (rows < 2 || cols < 2) return {};

    Polygons polys;
    long w_rows = std::max(1l, long(windowsize.y()));
    long w_cols = std::max(1l, long(windowsize.x()));

    std::vector<marchsq::Ring> rings =
        marchsq::execute(rst, 128, {w_rows, w_cols});

    polys.reserve(rings.size());

    auto pxd = rst.pixel_dimensions();

    for (const marchsq::Ring &ring : rings) {
        Polygon poly; Points &pts = poly.points;
        pts.reserve(ring.size());

        for (const marchsq::Coord &crd : ring)
            pts.emplace_back(scaled(crd.c * pxd.w_mm), scaled(crd.r * pxd.h_mm));

        polys.emplace_back(poly);
    }

    // reverse the raster transformations
    ExPolygons unioned = union_ex(polys);
    coord_t width = scaled(cols * pxd.h_mm), height = scaled(rows * pxd.w_mm);

    auto tr = rst.trafo();
    for (ExPolygon &expoly : unioned) {
        if (tr.mirror_y)
            foreach_vertex(expoly, [height](Point &p) {p.y() = height - p.y(); });

        if (tr.mirror_x)
            foreach_vertex(expoly, [width](Point &p) {p.x() = width - p.x(); });

        expoly.translate(-tr.center_x, -tr.center_y);

        if (tr.flipXY)
            foreach_vertex(expoly, [](Point &p) { std::swap(p.x(), p.y()); });

        if ((tr.mirror_x + tr.mirror_y + tr.flipXY) % 2) {
            expoly.contour.reverse();
            for (auto &h : expoly.holes) h.reverse();
        }
    }

    return unioned;
}

}} // namespace Slic3r
