#include "FillSvg.hpp"

#include "../ClipperUtils.hpp"
#include "../NSVGUtils.hpp"
#include "InfillMeshTiling.hpp"
#include "../Surface.hpp"
#include "../Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <map>
#include <mutex>
#include <regex>
#include <string>
#include <tuple>

namespace Slic3r {

// The element whose id attribute is `id`: a self-closing tag, or the tag through its matching
// close tag (nested tags of the same name are counted). Found by plain string search, not a regex:
// the id comes from the file and may contain regex syntax, and a backtracking regex across a large
// element recurses once per character, which can overflow a slicing thread's stack.
static std::string svg_element_by_id(const std::string &text, const std::string &id)
{
    auto is_name_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    auto name_at = [&text, &is_name_char](size_t pos) {
        size_t end = pos;
        while (end < text.size() && is_name_char(text[end]))
            ++ end;
        return text.substr(pos, end - pos);
    };
    for (size_t at = text.find(id); at != std::string::npos && ! id.empty(); at = text.find(id, at + 1)) {
        // Must be the whole value of an id attribute: id="<id>", spaces allowed around '='.
        if (at == 0 || text[at - 1] != '"' || at + id.size() >= text.size() || text[at + id.size()] != '"')
            continue;
        size_t q = at - 1;
        while (q > 0 && std::isspace(static_cast<unsigned char>(text[q - 1])))
            -- q;
        if (q == 0 || text[q - 1] != '=')
            continue;
        -- q;
        while (q > 0 && std::isspace(static_cast<unsigned char>(text[q - 1])))
            -- q;
        if (q < 3 || text.compare(q - 2, 2, "id") != 0 || ! std::isspace(static_cast<unsigned char>(text[q - 3])))
            continue;
        // Inside a tag: the nearest '<' before it, with no '>' in between.
        const size_t start = text.rfind('<', q);
        if (start == std::string::npos || text.find('>', start) < at)
            continue;
        const std::string name = name_at(start + 1);
        const size_t open_end = text.find('>', at);
        if (name.empty() || open_end == std::string::npos)
            continue;
        if (text[open_end - 1] == '/')
            return text.substr(start, open_end + 1 - start);
        int    depth = 1;
        size_t pos   = open_end + 1;
        for (size_t lt = text.find('<', pos); lt != std::string::npos; lt = text.find('<', pos)) {
            const size_t gt = text.find('>', lt);
            if (gt == std::string::npos)
                break;
            pos = gt + 1;
            if (text[lt + 1] == '/' && name_at(lt + 2) == name) {
                if (-- depth == 0)
                    return text.substr(start, pos - start);
            } else if (name_at(lt + 1) == name && text[gt - 1] != '/')
                ++ depth;
        }
    }
    return {};
}

// nanosvg does not implement <use>: replace every <use href="#id" x y transform> with the
// referenced element wrapped in a <g> carrying the equivalent transform, so cloned art (the usual
// way a tessellation is authored) is parsed like inline art. Bounded, in case of reference cycles.
static std::string svg_expand_use_elements(std::string svg_text)
{
    static const std::regex use_re(R"re(<use\b([^>]*?)(?:/>|>\s*</use>))re", std::regex::icase);
    auto attr = [](const std::string &attrs, const char *name) -> std::string {
        const std::regex re(std::string(R"re((?:^|\s))re") + name + R"re(\s*=\s*"([^"]*)")re");
        std::smatch am;
        return std::regex_search(attrs, am, re) ? am[1].str() : std::string();
    };
    for (int depth = 0; depth < 8; ++ depth) {
        std::smatch m;
        if (! std::regex_search(svg_text, m, use_re))
            break;
        std::string out;
        std::string::const_iterator pos = svg_text.cbegin();
        bool any = false;
        while (std::regex_search(pos, svg_text.cend(), m, use_re)) {
            out.append(pos, m[0].first);
            const std::string attrs = m[1].str();
            std::string href = attr(attrs, "href");
            if (href.empty())
                href = attr(attrs, "xlink:href");
            std::string replacement; // empty: drop an unresolvable <use>
            if (href.size() > 1 && href[0] == '#') {
                const std::string id = href.substr(1);
                if (std::string element = svg_element_by_id(svg_text, id); ! element.empty()) {
                    element = std::regex_replace(element, std::regex(R"re(\sid\s*=\s*"[^"]*")re"), std::string(), std::regex_constants::format_first_only);
                    const std::string x = attr(attrs, "x"), y = attr(attrs, "y"), transform = attr(attrs, "transform");
                    std::string t = transform;
                    if (! x.empty() || ! y.empty())
                        t += " translate(" + (x.empty() ? std::string("0") : x) + " " + (y.empty() ? std::string("0") : y) + ")";
                    replacement = "<g" + (t.empty() ? std::string() : " transform=\"" + t + "\"") + ">" + element + "</g>";
                    any = true;
                }
            }
            out += replacement;
            pos = m[0].second;
        }
        out.append(pos, svg_text.cend());
        svg_text = std::move(out);
        if (! any)
            break;
    }
    return svg_text;
}

// An SVG whose art is a <pattern> definition (a repeat cell painted onto a background rect) is
// exactly a tile: nanosvg ignores patterns, so lift the first pattern's cell out into a document
// of its own size. Returns the original text when there is no usable pattern.
static std::string svg_pattern_cell_as_document(const std::string &svg_text)
{
    // Plain string search for the same reason as svg_element_by_id(); tag names compare
    // case-insensitively, as the regex this replaces did.
    std::string lower = svg_text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    size_t open = lower.find("<pattern");
    while (open != std::string::npos && open + 8 < lower.size() && ! std::isspace(static_cast<unsigned char>(lower[open + 8])) && lower[open + 8] != '>')
        open = lower.find("<pattern", open + 1);
    if (open == std::string::npos)
        return svg_text;
    const size_t open_end = svg_text.find('>', open);
    const size_t close    = open_end == std::string::npos ? std::string::npos : lower.find("</pattern>", open_end);
    if (close == std::string::npos)
        return svg_text;
    const std::string attrs = svg_text.substr(open + 8, open_end - open - 8);
    const std::string inner = svg_text.substr(open_end + 1, close - open_end - 1);
    auto attr = [&attrs](const char *name) -> std::string {
        const std::regex re(std::string(R"re((?:^|\s))re") + name + R"re(\s*=\s*"([^"]*)")re");
        std::smatch am;
        return std::regex_search(attrs, am, re) ? am[1].str() : std::string();
    };
    const std::string width = attr("width"), height = attr("height");
    if (width.empty() || height.empty() || inner.find('<') == std::string::npos)
        return svg_text;
    std::string doc = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + width + "\" height=\"" + height + "\"";
    if (const std::string view_box = attr("viewBox"); ! view_box.empty())
        doc += " viewBox=\"" + view_box + "\"";
    else
        doc += " viewBox=\"0 0 " + width + " " + height + "\"";
    doc += ">" + inner + "</svg>";
    return doc;
}

SvgInfillTile FillSvg::tile_from_svg_text(const std::string &svg_text_in)
{
    SvgInfillTile tile;
    if (svg_text_in.empty())
        return tile;
    const std::string svg_text = svg_pattern_cell_as_document(svg_expand_use_elements(svg_text_in));
    // Path coordinates resolved to mm. nanosvg scales the shapes to the requested units but
    // reports the document width/height in pixels, hence the explicit conversion below.
    constexpr float  dpi       = 96.f;
    constexpr double mm_per_px = 25.4 / double(dpi);
    NSVGimage_ptr image = nsvgParse(svg_text, "mm", dpi);
    if (image == nullptr)
        return tile;
    const double doc_width  = double(image->width) * mm_per_px;
    const double doc_height = double(image->height) * mm_per_px;

    // Chord tolerance of 0.05 mm: NSVGLineParams compares squared distances of the scaled points.
    constexpr double tolerance_mm = 0.05;
    NSVGLineParams params((tolerance_mm * tolerance_mm) / (SCALING_FACTOR * SCALING_FACTOR));

    ExPolygons fills;
    Polygons   loops;
    Polylines  lines;
    for (const NSVGshape *shape = image->shapes; shape != nullptr; shape = shape->next) {
        if (! (shape->flags & NSVG_FLAGS_VISIBLE))
            continue;
        const bool has_fill   = shape->fill.type != NSVG_PAINT_NONE;
        const bool has_stroke = shape->stroke.type != NSVG_PAINT_NONE;
        if (! has_fill && ! has_stroke)
            continue;
        if (has_fill && doc_width > 0. && doc_height > 0.) {
            // A filled shape spanning the whole document is a background, not art.
            const double w = double(shape->bounds[2] - shape->bounds[0]);
            const double h = double(shape->bounds[3] - shape->bounds[1]);
            if (w >= 0.98 * doc_width && h >= 0.98 * doc_height)
                continue;
        }
        LinesPath path = linearize_path(shape->paths, params);
        if (has_fill) {
            // A filled shape prints as its outline: resolve the fill rule first so overlapping
            // sub-paths (the usual way a silhouette with holes is drawn) become one region.
            Polygons polygons = std::move(path.polygons);
            for (const Polyline &pl : path.polylines)
                if (pl.points.size() >= 3)
                    polygons.emplace_back(pl.points);
            append(fills, union_ex(polygons,
                shape->fillRule == NSVG_FILLRULE_EVENODD ? pftEvenOdd : pftNonZero));
        } else {
            // Stroke only: the path itself is the line.
            for (Polygon &poly : path.polygons)
                if (poly.points.size() >= 3)
                    loops.emplace_back(std::move(poly));
                else if (poly.points.size() == 2)
                    lines.emplace_back(poly.points);
            for (Polyline &pl : path.polylines)
                if (pl.points.size() >= 2)
                    lines.emplace_back(std::move(pl));
        }
    }
    if (fills.empty() && loops.empty() && lines.empty())
        return tile;
    // (The clip below can empty everything too; checked again after it.)

    // The document box is the tile. nanosvg maps the viewBox onto [0, width] x [0, height]
    // (y down); NSVGLineParams flipped y, so the drawing sits in y <= 0 -- lift it so the tile
    // origin is the lower-left corner. Drawings without a document size tile on their bounds.
    double width  = doc_width;
    double height = doc_height;
    BoundingBox bounds;
    for (const ExPolygon &ep : fills)
        bounds.merge(get_extents(ep));
    for (const Polygon &poly : loops)
        bounds.merge(poly.bounding_box());
    for (const Polyline &pl : lines)
        bounds.merge(pl.bounding_box());
    Point shift(0, 0);
    if (width > EPSILON && height > EPSILON) {
        shift = Point(0, scaled<coord_t>(height));
    } else {
        width  = unscaled<double>(bounds.size().x());
        height = unscaled<double>(bounds.size().y());
        shift  = - bounds.min;
    }
    if (width <= EPSILON || height <= EPSILON)
        return tile;
    // The px round trip leaves float noise on sizes authored in mm; snap to 0.1 um.
    width  = std::round(width  * 1e4) * 1e-4;
    height = std::round(height * 1e4) * 1e-4;
    for (ExPolygon &ep : fills)
        ep.translate(shift);
    for (Polygon &poly : loops)
        poly.translate(shift);
    for (Polyline &pl : lines)
        pl.translate(shift);
    // Clip to the cell box, as a renderer clips a pattern to its cell: art authored to run past
    // the edge continues in the neighbouring tile and must not be drawn twice. Loops that cross
    // the edge become open lines.
    const Polygon cell_box({ Point(0, 0), Point(scaled<coord_t>(width), 0),
                             Point(scaled<coord_t>(width), scaled<coord_t>(height)), Point(0, scaled<coord_t>(height)) });
    const BoundingBox cell_bb = cell_box.bounding_box();
    fills = intersection_ex(fills, Polygons{ cell_box });
    Polygons kept_loops;
    Polygons crossing_loops;
    for (Polygon &poly : loops)
        (cell_bb.contains(poly.bounding_box().min) && cell_bb.contains(poly.bounding_box().max) ? kept_loops : crossing_loops).emplace_back(std::move(poly));
    loops = std::move(kept_loops);
    append(lines, intersection_pl(crossing_loops, Polygons{ cell_box }));
    lines = intersection_pl(lines, Polygons{ cell_box });
    double length = 0.;
    for (const ExPolygon &ep : fills)
        for (const Polygon &poly : to_polygons(ep))
            length += unscaled<double>(poly.length());
    for (const Polygon &poly : loops)
        length += unscaled<double>(poly.length());
    for (const Polyline &pl : lines)
        length += unscaled<double>(pl.length());
    if (fills.empty() && loops.empty() && lines.empty())
        return tile;
    tile.fills  = std::move(fills);
    tile.loops  = std::move(loops);
    tile.lines  = std::move(lines);
    tile.size   = Vec2d(width, height);
    tile.length = length;
    return tile;
}

std::shared_ptr<const SvgInfillTile> FillSvg::tile_for(InfillPattern pattern, const std::string &svg_path)
{
    struct CacheEntry
    {
        std::time_t                          mtime;
        std::shared_ptr<const SvgInfillTile> tile;
    };
    static std::mutex                        cache_mutex;
    static std::map<std::string, CacheEntry> cache;

    std::string path = svg_path;
    if (pattern == ipCatfill)
        path = (boost::filesystem::path(resources_dir()) / "infill" / "catfill.svg").string();
    else if (pattern == ipSharkfill)
        path = (boost::filesystem::path(resources_dir()) / "infill" / "sharkfill.svg").string();
    else if (pattern == ipPuppyfill)
        path = (boost::filesystem::path(resources_dir()) / "infill" / "puppyfill.svg").string();
    if (path.empty())
        return {};

    boost::system::error_code ec;
    const std::time_t mtime = boost::filesystem::last_write_time(path, ec);
    if (ec) {
        BOOST_LOG_TRIVIAL(warning) << "svg infill: cannot read " << path << ": " << ec.message();
        return {};
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (auto it = cache.find(path); it != cache.end() && it->second.mtime == mtime)
        return it->second.tile;
    std::unique_ptr<std::string> text = read_from_disk(path);
    auto tile = std::make_shared<const SvgInfillTile>(text ? tile_from_svg_text(*text) : SvgInfillTile());
    if (tile->empty())
        BOOST_LOG_TRIVIAL(warning) << "svg infill: " << path << " contains no printable lines";
    cache[path] = CacheEntry{ mtime, tile };
    return tile;
}

double FillSvg::tile_scale(const SvgInfillTile &tile, double density, double line_spacing, double scale_percent, double art_length)
{
    if (scale_percent > 0.)
        return 0.01 * scale_percent;
    // Line area fraction of a tile at scale s: (length * s) * width / (area * s^2).
    const double area   = tile.size.x() * tile.size.y();
    const double length = art_length >= 0. ? art_length : tile.length;
    if (length <= 0. || area <= 0. || density <= 0. || line_spacing <= 0.)
        return 1.;
    return std::max(0.05, length * line_spacing / (area * density));
}

double FillSvg::Art::length_mm() const
{
    double len = 0.;
    for (const Polygon &poly : loops)
        len += unscaled<double>(poly.length());
    for (const Polyline &pl : lines)
        len += unscaled<double>(pl.length());
    return len;
}

FillSvg::Art FillSvg::make_art(const SvgInfillTile &tile, double scale, double single_wall_max_width_mm)
{
    Art art;
    art.loops = tile.loops;
    art.lines = tile.lines;
    if (single_wall_max_width_mm <= 0. || scale <= 0.) {
        for (const ExPolygon &ep : tile.fills)
            append(art.loops, to_polygons(ep));
        return art;
    }
    // Thresholds in tile units: the art is scaled by `scale` when printed.
    const double max_width = scale_(single_wall_max_width_mm) / scale;
    const double min_width = 0.1 * max_width;
    for (const ExPolygon &ep : tile.fills) {
        // The parts too thin for two lines; what survives the closing keeps its outline.
        ExPolygons thick = offset2_ex(ExPolygons{ ep }, -float(max_width / 2.), float(max_width / 2.));
        ExPolygons thin  = opening_ex(diff_ex(ExPolygons{ ep }, offset_ex(thick, float(ClipperSafetyOffset))), float(min_width / 2.));
        for (const ExPolygon &t : thick)
            append(art.loops, to_polygons(t));
        for (ExPolygon &t : thin)
            t.medial_axis(min_width, max_width, &art.lines);
    }
    return art;
}

const FillSvg::Art &FillSvg::art(const std::shared_ptr<const SvgInfillTile> &tile, double scale, double single_wall_max_width_mm)
{
    struct Key
    {
        const SvgInfillTile *tile;
        long                 scale_key;
        long                 width_key;
        bool operator<(const Key &rhs) const { return std::tie(tile, scale_key, width_key) < std::tie(rhs.tile, rhs.scale_key, rhs.width_key); }
    };
    static std::mutex                                       cache_mutex;
    static std::map<Key, std::unique_ptr<Art>>              cache;
    static std::vector<std::shared_ptr<const SvgInfillTile>> keep_alive; // tiles referenced by the keys
    const Key key { tile.get(), std::lround(scale * 1e6), std::lround(single_wall_max_width_mm * 1e6) };
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto it = cache.find(key);
    if (it == cache.end()) {
        it = cache.emplace(key, std::make_unique<Art>(make_art(*tile, scale, single_wall_max_width_mm))).first;
        keep_alive.push_back(tile);
    }
    return *it->second;
}

void FillSvg::stamp(const Art &art, const Vec2d &tile_size, double scale, const BoundingBox &region_bbox,
                    Polygons &loops_out, Polylines &lines_out)
{
    if ((art.loops.empty() && art.lines.empty()) || scale <= 0. || ! region_bbox.defined)
        return;
    const Point pitch(coord_t(std::lround(tile_size.x() * scale / SCALING_FACTOR)),
                      coord_t(std::lround(tile_size.y() * scale / SCALING_FACTOR)));
    if (pitch.x() <= 0 || pitch.y() <= 0)
        return;
    auto scaled_copy = [scale](const Points &src, coord_t x, coord_t y) {
        Points out;
        out.reserve(src.size());
        for (const Point &p : src)
            out.emplace_back(coord_t(std::lround(double(p.x()) * scale)) + x,
                             coord_t(std::lround(double(p.y()) * scale)) + y);
        return out;
    };
    // First tile at or below the box's lower-left corner on the absolute grid.
    const Point origin = align_to_grid(region_bbox.min, pitch);
    for (coord_t x = origin.x(); x <= region_bbox.max.x(); x += pitch.x())
        for (coord_t y = origin.y(); y <= region_bbox.max.y(); y += pitch.y()) {
            for (const Polygon &poly : art.loops)
                loops_out.emplace_back(scaled_copy(poly.points, x, y));
            for (const Polyline &pl : art.lines)
                lines_out.emplace_back(scaled_copy(pl.points, x, y));
        }
}

Polylines FillSvg::tile_lines(const SvgInfillTile &tile, double scale, const BoundingBox &region_bbox)
{
    Polygons  loops;
    Polylines lines;
    stamp(make_art(tile, scale, 0.), tile.size, scale, region_bbox, loops, lines);
    for (const Polygon &poly : loops)
        lines.emplace_back(poly.split_at_first_point());
    return lines;
}

// Art clipped at a cell edge continues in the neighbouring tile: one continuous extrusion beats
// a travel per tile boundary.
void FillSvg::join_at_endpoints(Polylines &polylines, coord_t tolerance)
{
    join_polylines_at_endpoints(polylines, tolerance);
}

void FillSvg::_fill_surface_single(
    const FillParams                &params,
    unsigned int                     thickness_layers,
    const std::pair<float, Point>   &direction,
    ExPolygon                        expolygon,
    Polylines                       &polylines_out)
{
    const std::string svg_path = params.config != nullptr ? params.config->sparse_infill_svg_path.value : std::string();
    std::shared_ptr<const SvgInfillTile> tile = tile_for(params.pattern, svg_path);
    if (tile == nullptr || tile->empty())
        return;
    const double scale_percent = params.config != nullptr ? params.config->sparse_infill_svg_scale.value : 0.;
    double scale = tile_scale(*tile, params.density, this->spacing, scale_percent);
    // Single wall: bands up to kSingleWallMaxWidths line widths wide print along their centerline.
    const bool   single_wall = params.config != nullptr && params.config->sparse_infill_single_wall.value;
    const double single_wall_width = single_wall ? kSingleWallMaxWidths * this->spacing : 0.;
    const Art   *tile_art = &art(tile, scale, single_wall_width);
    if (single_wall && scale_percent <= 0.) {
        // The automatic scale must realize the density with the art that is actually printed,
        // which single wall roughly halves; one refinement pass is enough (the threshold only
        // moves with the scale, and the art rarely changes with it).
        scale    = tile_scale(*tile, params.density, this->spacing, 0., tile_art->length_mm());
        tile_art = &art(tile, scale, single_wall_width);
    }

    // Fill::_infill_direction adds 90 degrees for the line patterns; take it back so that an
    // infill direction of 0 prints the drawing upright.
    const float angle = direction.first - float(M_PI / 2.);
    // Generate axis-aligned in the rotated frame, then rotate the art into place.
    Polygon bb_polygon = expolygon.contour.bounding_box().polygon();
    bb_polygon.rotate(-angle);
    Polygons  loops;
    Polylines lines;
    stamp(*tile_art, tile->size, scale, bb_polygon.bounding_box(), loops, lines);
    for (Polygon &poly : loops)
        poly.rotate(angle);
    for (Polyline &pl : lines)
        pl.rotate(angle);

    // Closed loops go through the polygon clipper, which opens them at the region boundary and
    // re-joins fragments split at their first vertex; open lines through the polyline clipper.
    Polylines clipped = intersection_pl(loops, to_polygons(expolygon));
    append(clipped, intersection_pl(lines, expolygon));
    join_at_endpoints(clipped, scaled<coord_t>(0.05));
    const double min_length = scale_(0.5 * this->spacing);
    clipped.erase(std::remove_if(clipped.begin(), clipped.end(),
                                 [min_length](const Polyline &pl) { return pl.length() < min_length; }),
                  clipped.end());
    chain_or_connect_infill(std::move(clipped), expolygon, polylines_out, this->spacing, params);
}

} // namespace Slic3r
