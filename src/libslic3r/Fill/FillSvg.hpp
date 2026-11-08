#ifndef slic3r_FillSvg_hpp_
#define slic3r_FillSvg_hpp_

#include "../libslic3r.h"
#include "../ExPolygon.hpp"
#include "../Polygon.hpp"
#include "../Polyline.hpp"
#include "FillBase.hpp"

#include <memory>
#include <string>

namespace Slic3r {

// Sparse infill from SVG line art: the drawing is one unit tile, repeated on the rectangular
// grid of its document box across every layer. What gets printed is single lines -- the outline
// of every filled shape and the centerline of every stroked path -- so "line art" is meant
// literally: stroke widths, colours and fill are not rendered, only followed.
//
// Four patterns share this Filler: Catfill, Sharkfill and Puppyfill (the bundled
// resources/infill/*.svg tessellations) and Custom (SVG), whose file is the region's
// sparse_infill_svg_path. The tile's printed size is
// set by sparse_infill_svg_scale; at the default of 0 (automatic) it is derived from the sparse
// infill density so that line length x line width / tile area == density, the same contract
// every other pattern honours.
//
// The tile is anchored to the slice-frame grid like Honeycomb, not to the fill region, so it is
// identical on every layer; infill_direction rotates it and the layer-to-layer alternation is
// disabled (the art should stack, not flip 90 degrees every layer).
struct SvgInfillTile
{
    // Art in scaled coordinates, tile origin at the document's lower-left corner, y up: the
    // filled regions (printed as their outline, or as centerlines in single-wall mode), closed
    // strokes and open strokes (both printed as they are).
    ExPolygons fills;
    Polygons   loops;
    Polylines  lines;
    // Document (tile) size in mm; the tiling pitch at scale 1.
    Vec2d     size { 0., 0. };
    // Total line length in mm at scale 1.
    double    length { 0. };

    bool empty() const { return (fills.empty() && loops.empty() && lines.empty()) || size.x() <= 0. || size.y() <= 0.; }
};

class FillSvg : public Fill
{
public:
    // Single-wall mode: a filled band narrower than this many line widths (at print scale) is a
    // stroke and prints along its centerline; anything wider is a shape and keeps its outline.
    static constexpr double kSingleWallMaxWidths = 6.;
    ~FillSvg() override = default;
    bool is_self_crossing() override { return true; }

    // Parse SVG text into a tile. Empty tile on parse failure or when nothing is drawn.
    static SvgInfillTile tile_from_svg_text(const std::string &svg_text);
    // The tile for a pattern: the bundled patterns load their resource, Custom (SVG) the given file.
    // Parsed tiles are cached process-wide, keyed by path and modification time.
    static std::shared_ptr<const SvgInfillTile> tile_for(InfillPattern pattern, const std::string &svg_path);
    // Uniform tile scale (mm per SVG mm) for the requested density and line spacing, or the
    // explicit scale when `scale_percent` > 0. `art_length` is the printed line length of one
    // tile at unit scale (the tile's outline length unless single-wall art is used).
    static double tile_scale(const SvgInfillTile &tile, double density, double line_spacing, double scale_percent,
                             double art_length = -1.);
    // The printable lines of one tile at unit scale: outlines of the fills plus the strokes, or,
    // with `single_wall_max_width_mm` > 0, the centerline (medial axis) of every filled band
    // narrower than that width at the given print scale while thicker regions keep their outline.
    // Cached per (tile, scale, width); the returned references stay valid for the process lifetime.
    struct Art
    {
        Polygons  loops;
        Polylines lines;
        double    length_mm() const;
    };
    static const Art &art(const std::shared_ptr<const SvgInfillTile> &tile, double scale, double single_wall_max_width_mm);
    static Art        make_art(const SvgInfillTile &tile, double scale, double single_wall_max_width_mm);
    // Stamp the art over `region_bbox` (scaled, in the frame the lines are generated in):
    // every tile touching the box, aligned to the absolute grid so consecutive layers match.
    static void stamp(const Art &art, const Vec2d &tile_size, double scale, const BoundingBox &region_bbox,
                      Polygons &loops_out, Polylines &lines_out);
    // Outline-mode art as polylines only (loops opened at their first vertex), for inspection.
    static Polylines tile_lines(const SvgInfillTile &tile, double scale, const BoundingBox &region_bbox);
    // Merge polylines whose ends meet within `tolerance` into continuous ones.
    static void join_at_endpoints(Polylines &polylines, coord_t tolerance);

protected:
    Fill* clone() const override { return new FillSvg(*this); }
    void _fill_surface_single(
        const FillParams                &params,
        unsigned int                     thickness_layers,
        const std::pair<float, Point>   &direction,
        ExPolygon                        expolygon,
        Polylines                       &polylines_out) override;
    // The art keeps its orientation from layer to layer.
    float _layer_angle(size_t idx) const override { return 0.f; }
};

} // namespace Slic3r

#endif // slic3r_FillSvg_hpp_
