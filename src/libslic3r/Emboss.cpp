#include <numeric>
#include "Emboss.hpp"
#include <stdio.h>
#include <numeric>
#include <cstdlib>
#include <boost/nowide/convert.hpp>
#include <boost/log/trivial.hpp>
#include <ClipperUtils.hpp> // union_ex + for boldness(polygon extend(offset))
#include "IntersectionPoints.hpp"

#define STB_TRUETYPE_IMPLEMENTATION // force following include to generate implementation
#include "imgui/imstb_truetype.h" // stbtt_fontinfo
#include "Utils.hpp" // ScopeGuard

#include <Triangulation.hpp> // CGAL project
#include "libslic3r.h"

// to heal shape
#include "ExPolygonsIndex.hpp"
#include "libslic3r/AABBTreeLines.hpp" // search structure for found close points
#include "libslic3r/Line.hpp"
#include "libslic3r/BoundingBox.hpp"

// Experimentaly suggested ration of font ascent by multiple fonts
// to get approx center of normal text line
const double ASCENT_CENTER = 1/3.; // 0.5 is above small letter

// every glyph's shape point is divided by SHAPE_SCALE - increase precission of fixed point value
// stored in fonts (to be able represents curve by sequence of lines)
static constexpr double SHAPE_SCALE = 0.001; // SCALING_FACTOR promile is fine enough
static unsigned MAX_HEAL_ITERATION_OF_TEXT = 10;

using namespace Slic3r;
using namespace Emboss;
using fontinfo_opt = std::optional<stbtt_fontinfo>;

// NOTE: approach to heal shape by Clipper::Closing is not working

// functionality to remove all spikes from shape
// Potentionaly useable for eliminate spike in layer
//#define REMOVE_SPIKES

// function to remove useless islands and holes
// #define REMOVE_SMALL_ISLANDS
#ifdef REMOVE_SMALL_ISLANDS
namespace { void remove_small_islands(ExPolygons &shape, double minimal_area);}
#endif //REMOVE_SMALL_ISLANDS

//#define VISUALIZE_HEAL
#ifdef VISUALIZE_HEAL
namespace {
// for debug purpose only
// NOTE: check scale when store svg !!
#include "libslic3r/SVG.hpp" // for visualize_heal
static std::string visualize_heal_svg_filepath = "C:/data/temp/heal.svg";
void               visualize_heal(const std::string &svg_filepath, const ExPolygons &expolygons)
{
    Points      pts = to_points(expolygons);
    BoundingBox bb(pts);
    // double svg_scale = SHAPE_SCALE / unscale<double>(1.);
    //  bb.scale(svg_scale);
    SVG svg(svg_filepath, bb);
    svg.draw(expolygons);

    Points duplicits  = collect_duplicates(pts);
    int    black_size = std::max(bb.size().x(), bb.size().y()) / 20;
    svg.draw(duplicits, "black", black_size);

    Slic3r::IntersectionsLines intersections_f = get_intersections(expolygons);
    Points                     intersections   = get_unique_intersections(intersections_f);
    svg.draw(intersections, "red", black_size * 1.2);
}
} // namespace
#endif // VISUALIZE_HEAL

// do not expose out of this file stbtt_ data types
namespace{
using Polygon = Slic3r::Polygon;
bool is_valid(const FontFile &font, unsigned int index);
fontinfo_opt load_font_info(const unsigned char *data, unsigned int index = 0);
std::optional<Glyph> get_glyph(const stbtt_fontinfo &font_info, int unicode_letter, float flatness);

// take glyph from cache
const Glyph* get_glyph(int unicode, const FontFile &font, const FontProp &font_prop, 
        Glyphs &cache, fontinfo_opt &font_info_opt);

// scale and convert float to int coordinate
Point to_point(const stbtt__point &point);

// bad is contour smaller than 3 points
void remove_bad(Polygons &polygons);
void remove_bad(ExPolygons &expolygons);

// Try to remove self intersection by subtracting rect 2x2 px
ExPolygon create_bounding_rect(const ExPolygons &shape);

// Heal duplicates points and self intersections
bool heal_dupl_inter(ExPolygons &shape, unsigned max_iteration);

const Points pts_2x2({Point(0, 0), Point(1, 0), Point(1, 1), Point(0, 1)});
const Points pts_3x3({Point(-1, -1), Point(1, -1), Point(1, 1), Point(-1, 1)});

struct SpikeDesc
{
    // cosinus of max spike angle
    double cos_angle; // speed up to skip acos

    // Half of Wanted bevel size
    double half_bevel; 

    /// <summary>
    /// Calculate spike description
    /// </summary>
    /// <param name="bevel_size">Size of spike width after cut of the tip, has to be grater than 2.5</param>
    /// <param name="pixel_spike_length">When spike has same or more pixels with width less than 1 pixel</param>
    SpikeDesc(double bevel_size, double pixel_spike_length = 6):         
        // create min angle given by spike_length
        // Use it as minimal height of 1 pixel base spike
        cos_angle(std::fabs(std::cos(
            /*angle*/ 2. * std::atan2(pixel_spike_length, .5)
        ))),

        // When remove spike this angle is set.
        // Value must be grater than min_angle
        half_bevel(bevel_size / 2)
    {}
};

// return TRUE when remove point. It could create polygon with 2 points.
bool remove_when_spike(Polygon &polygon, size_t index, const SpikeDesc &spike_desc);
void remove_spikes_in_duplicates(ExPolygons &expolygons, const Points &duplicates);

#ifdef REMOVE_SPIKES
// Remove long sharp corners aka spikes 
// by adding points to bevel tip of spikes - Not printable parts
// Try to not modify long sides of spike and add points on it's side
void remove_spikes(Polygon &polygon, const SpikeDesc &spike_desc);
void remove_spikes(Polygons &polygons, const SpikeDesc &spike_desc);
void remove_spikes(ExPolygons &expolygons, const SpikeDesc &spike_desc);
#endif

// spike ... very sharp corner - when not removed cause iteration of heal process
// index ... index of duplicit point in polygon
bool remove_when_spike(Slic3r::Polygon &polygon, size_t index, const SpikeDesc &spike_desc) {

    std::optional<Point> add;
    bool do_erase = false;
    Points &pts = polygon.points;
    {
        size_t  pts_size = pts.size();
        if (pts_size < 3)
            return false;

        const Point &a = (index == 0) ? pts.back() : pts[index - 1];
        const Point &b = pts[index];
        const Point &c = (index == (pts_size - 1)) ? pts.front() : pts[index + 1];

        // calc sides
        Vec2d ba = (a - b).cast<double>();
        Vec2d bc = (c - b).cast<double>();

        double dot_product = ba.dot(bc);

        // sqrt together after multiplication save one sqrt
        double ba_size_sq = ba.squaredNorm();
        double bc_size_sq = bc.squaredNorm();
        double norm       = sqrt(ba_size_sq * bc_size_sq);
        double cos_angle  = dot_product / norm;

        // small angle are around 1 --> cos(0) = 1
        if (cos_angle < spike_desc.cos_angle)
            return false; // not a spike

        // has to be in range <-1, 1>
        // Due to preccission of floating point number could be sligtly out of range
        if (cos_angle > 1.)
            cos_angle = 1.;
        // if (cos_angle < -1.)
        //     cos_angle = -1.;

        // Current Spike angle
        double angle          = acos(cos_angle);
        double wanted_size    = spike_desc.half_bevel / cos(angle / 2.);
        double wanted_size_sq = wanted_size * wanted_size;

        bool is_ba_short = ba_size_sq < wanted_size_sq;
        bool is_bc_short = bc_size_sq < wanted_size_sq;

        auto a_side = [&b, &ba, &ba_size_sq, &wanted_size]() -> Point {
            Vec2d ba_norm = ba / sqrt(ba_size_sq);
            return b + (wanted_size * ba_norm).cast<coord_t>();
        };
        auto c_side = [&b, &bc, &bc_size_sq, &wanted_size]() -> Point {
            Vec2d bc_norm = bc / sqrt(bc_size_sq);
            return b + (wanted_size * bc_norm).cast<coord_t>();
        };

        if (is_ba_short && is_bc_short) {
            // remove short spike
            do_erase = true;
        } else if (is_ba_short) {
            // move point B on C-side
            pts[index] = c_side();
        } else if (is_bc_short) {
            // move point B on A-side
            pts[index] = a_side();
        } else {
            // move point B on C-side and add point on A-side(left - before)
            pts[index] = c_side();
            add = a_side();
            if (*add == pts[index]) {
                // should be very rare, when SpikeDesc has small base
                // will be fixed by remove B point
                add.reset();
                do_erase = true;
            }
        }
    }
    if (do_erase) {
        pts.erase(pts.begin() + index);
        return true;
    }
    if (add.has_value())
        pts.insert(pts.begin() + index, *add);
    return false;
}

void remove_spikes_in_duplicates(ExPolygons &expolygons, const Points &duplicates) { 
    if (duplicates.empty())
        return;
    auto check = [](Slic3r::Polygon &polygon, const Point &d) -> bool {
        double spike_bevel = 1 / SHAPE_SCALE;
        double spike_length = 5.;
        const static SpikeDesc sd(spike_bevel, spike_length);
        Points& pts = polygon.points;
        bool exist_remove = false;
        for (size_t i = 0; i < pts.size(); i++) {
            if (pts[i] != d)
                continue;
            exist_remove |= remove_when_spike(polygon, i, sd);
        }
        return exist_remove && pts.size() < 3;
    };

    bool exist_remove = false;
    for (ExPolygon &expolygon : expolygons) {
        BoundingBox bb(to_points(expolygon.contour));
        for (const Point &d : duplicates) {
            if (!bb.contains(d))
                continue;
            exist_remove |= check(expolygon.contour, d);
            for (Polygon &hole : expolygon.holes)
                exist_remove |= check(hole, d);
        }
    }

    if (exist_remove)
        remove_bad(expolygons);
}

bool is_valid(const FontFile &font, unsigned int index) {
    if (font.data == nullptr) return false;
    if (font.data->empty()) return false;
    if (index >= font.infos.size()) return false;
    return true;
}

fontinfo_opt load_font_info(
    const unsigned char *data, unsigned int index)
{
    int font_offset = stbtt_GetFontOffsetForIndex(data, index);
    if (font_offset < 0) {
        assert(false);
        // "Font index(" << index << ") doesn't exist.";
        return {};        
    }
    stbtt_fontinfo font_info;
    if (stbtt_InitFont(&font_info, data, font_offset) == 0) {
        // Can't initialize font.
        assert(false);
        return {};
    }
    return font_info;
}

void remove_bad(Polygons &polygons) {
    polygons.erase(
        std::remove_if(polygons.begin(), polygons.end(), 
            [](const Polygon &p) { return p.size() < 3; }), 
        polygons.end());
}

void remove_bad(ExPolygons &expolygons) {
    expolygons.erase(
        std::remove_if(expolygons.begin(), expolygons.end(), 
            [](const ExPolygon &p) { return p.contour.size() < 3; }),
        expolygons.end());

    for (ExPolygon &expolygon : expolygons)
         remove_bad(expolygon.holes);
}
} // end namespace

bool Emboss::divide_segments_for_close_point(ExPolygons &expolygons, double distance)
{
    if (expolygons.empty()) return false;
    if (distance < 0.) return false;

    // ExPolygons can't contain same neigbours
    remove_same_neighbor(expolygons);

    // IMPROVE: use int(insted of double) lines and tree
    const ExPolygonsIndices ids(expolygons);
    const std::vector<Linef> lines = Slic3r::to_linesf(expolygons, ids.get_count());
    AABBTreeIndirect::Tree<2, double> tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(lines);
    using Div = std::pair<Point, size_t>;
    std::vector<Div> divs;
    size_t point_index = 0;
    auto check_points = [&divs, &point_index, &lines, &tree, &distance, &ids, &expolygons](const Points &pts) {
        for (const Point &p : pts) {
            Vec2d p_d = p.cast<double>();
            std::vector<size_t> close_lines = AABBTreeLines::all_lines_in_radius(lines, tree, p_d, distance);
            for (size_t index : close_lines) {
                // skip point neighbour lines indices
                if (index == point_index) continue;
                if (&p != &pts.front()) { 
                    if (index == point_index - 1) continue;
                } else if (index == (pts.size()-1)) continue;

                // do not doubled side point of segment
                const ExPolygonsIndex id = ids.cvt(index);
                const ExPolygon &expoly = expolygons[id.expolygons_index];
                const Polygon &poly = id.is_contour() ? expoly.contour : expoly.holes[id.hole_index()];
                const Points &poly_pts = poly.points;
                const Point &line_a = poly_pts[id.point_index];
                const Point &line_b = (!ids.is_last_point(id)) ? poly_pts[id.point_index + 1] : poly_pts.front();
                assert(line_a == lines[index].a.cast<coord_t>());
                assert(line_b == lines[index].b.cast<coord_t>());
                if (p == line_a || p == line_b) continue;

                divs.emplace_back(p, index);
            }
            ++point_index;
        }
    };
    for (const ExPolygon &expoly : expolygons) { 
        check_points(expoly.contour.points);
        for (const Polygon &hole : expoly.holes) 
            check_points(hole.points);
    }

    // check if exist division
    if (divs.empty()) return false;

    // sort from biggest index to zero
    // to be able add points and not interupt indices
    std::sort(divs.begin(), divs.end(), 
        [](const Div &d1, const Div &d2) { return d1.second > d2.second; });
    
    auto it = divs.begin();
    // divide close line
    while (it != divs.end()) {
        // colect division of a line segmen
        size_t index = it->second;
        auto it2 = it+1;
        while (it2 != divs.end() && it2->second == index) ++it2;

        ExPolygonsIndex id = ids.cvt(index);
        ExPolygon &expoly = expolygons[id.expolygons_index];
        Polygon &poly = id.is_contour() ? expoly.contour : expoly.holes[id.hole_index()];
        Points &pts = poly.points;        
        size_t count = it2 - it;

        // add points into polygon to divide in place of near point
        if (count == 1) {
            pts.insert(pts.begin() + id.point_index + 1, it->first);
            ++it;
        } else {
            // collect points to add into polygon
            Points points;
            points.reserve(count);
            for (; it < it2; ++it) 
                points.push_back(it->first);            

            // need sort by line direction
            const Linef &line = lines[index];
            Vec2d        dir  = line.b - line.a;
            // select mayorit direction
            int axis  = (abs(dir.x()) > abs(dir.y())) ? 0 : 1;
            using Fnc = std::function<bool(const Point &, const Point &)>;
            Fnc fnc   = (dir[axis] < 0) ? Fnc([axis](const Point &p1, const Point &p2) { return p1[axis] > p2[axis]; }) :
                                          Fnc([axis](const Point &p1, const Point &p2) { return p1[axis] < p2[axis]; }) ;
            std::sort(points.begin(), points.end(), fnc);

            // use only unique points
            points.erase(std::unique(points.begin(), points.end()), points.end());

            // divide line by adding points into polygon
            pts.insert(pts.begin() + id.point_index + 1,
                points.begin(), points.end());
        }
        assert(it == it2);
    }
    return true;
}

HealedExPolygons Emboss::heal_polygons(const Polygons &shape, bool is_non_zero, unsigned int max_iteration)
{
    const double clean_distance = 1.415; // little grater than sqrt(2)
    ClipperLib::PolyFillType fill_type = is_non_zero ? 
        ClipperLib::pftNonZero : ClipperLib::pftEvenOdd;

    // When edit this code check that font 'ALIENATE.TTF' and glyph 'i' still work
    // fix of self intersections
    // http://www.angusj.com/delphi/clipper/documentation/Docs/Units/ClipperLib/Functions/SimplifyPolygon.htm
    ClipperLib::Paths paths = ClipperLib::SimplifyPolygons(ClipperUtils::PolygonsProvider(shape), fill_type);
    ClipperLib::CleanPolygons(paths, clean_distance);
    Polygons polygons = to_polygons(paths);
    polygons.erase(std::remove_if(polygons.begin(), polygons.end(), 
        [](const Polygon &p) { return p.size() < 3; }), polygons.end());
    
    if (polygons.empty())
        return {{}, false};

    // Do not remove all duplicates but do it better way
    // Overlap all duplicit points by rectangle 3x3
    Points duplicits = collect_duplicates(to_points(polygons));
    if (!duplicits.empty()) {
        polygons.reserve(polygons.size() + duplicits.size());
        for (const Point &p : duplicits) {
            Polygon rect_3x3(pts_3x3);
            rect_3x3.translate(p);
            polygons.push_back(rect_3x3);
        }
    }
    ExPolygons res = Slic3r::union_ex(polygons, fill_type);
    bool is_healed = heal_expolygons(res, max_iteration);
    return {res, is_healed};
}


bool Emboss::heal_expolygons(ExPolygons &shape, unsigned max_iteration)
{
    return ::heal_dupl_inter(shape, max_iteration);
}

namespace {

Points get_unique_intersections(const Slic3r::IntersectionsLines &intersections)
{
    Points result;
    if (intersections.empty())
        return result;

    // convert intersections into Points
    result.reserve(intersections.size());
    std::transform(intersections.begin(), intersections.end(), std::back_inserter(result),
        [](const Slic3r::IntersectionLines &i) { return Point(
            std::floor(i.intersection.x()), 
            std::floor(i.intersection.y())); 
        });
    // intersections should be unique poits
    std::sort(result.begin(), result.end());
    auto it = std::unique(result.begin(), result.end());
    result.erase(it, result.end());
    return result;
}

Polygons get_holes_with_points(const Polygons &holes, const Points &points)
{
    Polygons result;
    for (const Slic3r::Polygon &hole : holes)
        for (const Point &p : points)
            for (const Point &h : hole)
                if (p == h) {
                    result.push_back(hole);
                    break;
                }
    return result;
}

/// <summary>
/// Fill holes which create duplicits or intersections
/// When healing hole creates trouble in shape again try to heal by an union instead of diff_ex
/// </summary>
/// <param name="holes">Holes which was substracted from shape previous</param>
/// <param name="duplicates">Current duplicates in shape</param>
/// <param name="intersections">Current intersections in shape</param>
/// <param name="shape">Partialy healed shape[could be modified]</param>
/// <returns>True when modify shape otherwise False</returns>
bool fill_trouble_holes(const Polygons &holes, const Points &duplicates, const Points &intersections, ExPolygons &shape)
{
    if (holes.empty())
        return false;
    if (duplicates.empty() && intersections.empty())
        return false;

    Polygons fill = get_holes_with_points(holes, duplicates);
    append(fill, get_holes_with_points(holes, intersections));
    if (fill.empty())
        return false;

    shape = union_ex(shape, fill);
    return true;
}

// extend functionality from Points.cpp --> collect_duplicates
// with address of duplicated points
struct Duplicate {
    Point point;
    std::vector<uint32_t> indices;
};
using Duplicates = std::vector<Duplicate>;
Duplicates collect_duplicit_indices(const ExPolygons &expoly)
{
    Points pts = to_points(expoly);

    // initialize original index locations
    std::vector<uint32_t> idx(pts.size());
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), 
        [&pts](uint32_t i1, uint32_t i2) { return pts[i1] < pts[i2]; });

    Duplicates result;
    const Point *prev = &pts[idx.front()];
    for (size_t i = 1; i < idx.size(); ++i) {
        uint32_t index = idx[i];
        const Point *act = &pts[index];
        if (*prev == *act) {
            // duplicit point
            if (!result.empty() && result.back().point == *act) {
                // more than 2 points with same coordinate
                result.back().indices.push_back(index);
            } else {
                uint32_t prev_index = idx[i-1];
                result.push_back({*act, {prev_index, index}});
            }
            continue;
        }
        prev = act;
    }
    return result;
}

Points get_points(const Duplicates& duplicate_indices)
{
    Points result;
    if (duplicate_indices.empty())
        return result;

    // convert intersections into Points
    result.reserve(duplicate_indices.size());
    std::transform(duplicate_indices.begin(), duplicate_indices.end(), std::back_inserter(result), 
        [](const Duplicate &d) { return d.point; });
    return result;
}

bool heal_dupl_inter(ExPolygons &shape, unsigned max_iteration)
{    
    if (shape.empty()) return true;
    remove_same_neighbor(shape);

    // create loop permanent memory
    Polygons holes;
    while (--max_iteration) {        
        Duplicates duplicate_indices = collect_duplicit_indices(shape);
        //Points duplicates = collect_duplicates(to_points(shape));
        IntersectionsLines intersections = get_intersections(shape);
                
        // Check whether shape is already healed
        if (intersections.empty() && duplicate_indices.empty())
            return true;

        Points duplicate_points = get_points(duplicate_indices);
        Points intersection_points = get_unique_intersections(intersections);

        if (fill_trouble_holes(holes, duplicate_points, intersection_points, shape)) {
            holes.clear(); 
            continue;
        } 

        holes.clear();
        holes.reserve(intersections.size() + duplicate_points.size());

        remove_spikes_in_duplicates(shape, duplicate_points);

        // Fix self intersection in result by subtracting hole 2x2
        for (const Point &p : intersection_points) {
            Polygon hole(pts_2x2);
            hole.translate(p);
            holes.push_back(hole);
        }

        // Fix duplicit points by hole 3x3 around duplicit point
        for (const Point &p : duplicate_points) {
            Polygon hole(pts_3x3);
            hole.translate(p);
            holes.push_back(hole);
        }

        shape = Slic3r::diff_ex(shape, holes, ApplySafetyOffset::No);
        // ApplySafetyOffset::Yes is incompatible with function fill_trouble_holes
    }

    // Create partialy healed output
    Duplicates duplicates = collect_duplicit_indices(shape);
    IntersectionsLines intersections = get_intersections(shape);
    if (duplicates.empty() && intersections.empty()){
        // healed in the last loop
        return true;
    }
    
    #ifdef VISUALIZE_HEAL
    visualize_heal(visualize_heal_svg_filepath, shape);
    #endif // VISUALIZE_HEAL

    assert(false); // Can not heal this shape
    // investigate how to heal better way

    ExPolygonsIndices ei(shape);
    std::vector<bool> is_healed(shape.size(), {true});
    for (const Duplicate &duplicate : duplicates){
        for (uint32_t i : duplicate.indices)
            is_healed[ei.cvt(i).expolygons_index] = false;
    }
    for (const IntersectionLines &intersection : intersections) {
        is_healed[ei.cvt(intersection.line_index1).expolygons_index] = false;
        is_healed[ei.cvt(intersection.line_index2).expolygons_index] = false;
    }

    for (size_t shape_index = 0; shape_index < shape.size(); shape_index++) {
        if (!is_healed[shape_index]) {
            // exchange non healed expoly with bb rect
            ExPolygon &expoly = shape[shape_index];
            expoly = create_bounding_rect({expoly});
        }
    }
    return false;
}

ExPolygon create_bounding_rect(const ExPolygons &shape) {
    BoundingBox bb   = get_extents(shape);
    Point       size = bb.size();
    if (size.x() < 10)
        bb.max.x() += 10;
    if (size.y() < 10)
        bb.max.y() += 10;

    Polygon rect({// CCW
        bb.min,
        {bb.max.x(), bb.min.y()},
        bb.max,
        {bb.min.x(), bb.max.y()}});

    Point   offset = bb.size() * 0.1;
    Polygon hole({// CW
        bb.min + offset,
        {bb.min.x() + offset.x(), bb.max.y() - offset.y()},
        bb.max - offset,
        {bb.max.x() - offset.x(), bb.min.y() + offset.y()}});

    return ExPolygon(rect, hole);
}

#ifdef REMOVE_SMALL_ISLANDS
void remove_small_islands(ExPolygons &expolygons, double minimal_area) {
    if (expolygons.empty())
        return;

    // remove small expolygons contours
    auto expoly_it = std::remove_if(expolygons.begin(), expolygons.end(), 
        [&minimal_area](const ExPolygon &p) { return p.contour.area() < minimal_area; });
    expolygons.erase(expoly_it, expolygons.end());

    // remove small holes in expolygons
    for (ExPolygon &expoly : expolygons) {
        Polygons& holes = expoly.holes;
        auto it = std::remove_if(holes.begin(), holes.end(), 
            [&minimal_area](const Polygon &p) { return -p.area() < minimal_area; });
        holes.erase(it, holes.end());
    }
}
#endif // REMOVE_SMALL_ISLANDS

std::optional<Glyph> get_glyph(const stbtt_fontinfo &font_info, int unicode_letter, float flatness)
{
    int glyph_index = stbtt_FindGlyphIndex(&font_info, unicode_letter);
    if (glyph_index == 0) {
        //wchar_t wchar = static_cast<wchar_t>(unicode_letter); 
        //<< "Character unicode letter ("
        //<< "decimal value = " << std::dec << unicode_letter << ", "
        //<< "hexadecimal value = U+" << std::hex << unicode_letter << std::dec << ", "
        //<< "wchar value = " << wchar
        //<< ") is NOT defined inside of the font. \n";
        return {};
    }

    Glyph glyph;
    stbtt_GetGlyphHMetrics(&font_info, glyph_index, &glyph.advance_width, &glyph.left_side_bearing);

    stbtt_vertex *vertices;
    int num_verts = stbtt_GetGlyphShape(&font_info, glyph_index, &vertices);
    if (num_verts <= 0) return glyph; // no shape
    ScopeGuard sg1([&vertices]() { free(vertices); });

    int *contour_lengths = NULL;
    int  num_countour_int = 0;
    stbtt__point *points = stbtt_FlattenCurves(vertices, num_verts,
        flatness, &contour_lengths, &num_countour_int, font_info.userdata);
    if (!points) return glyph; // no valid flattening
    ScopeGuard sg2([&contour_lengths, &points]() {
        free(contour_lengths); 
        free(points); 
    });

    size_t   num_contour = static_cast<size_t>(num_countour_int);
    Polygons glyph_polygons;
    glyph_polygons.reserve(num_contour);
    size_t pi = 0; // point index
    for (size_t ci = 0; ci < num_contour; ++ci) {
        int length = contour_lengths[ci];
        // check minimal length for triangle
        if (length < 4) {
            // weird font
            pi+=length;
            continue;
        }
        // last point is first point
        --length;
        Points pts;
        pts.reserve(length);
        for (int i = 0; i < length; ++i) 
            pts.emplace_back(to_point(points[pi++]));
        
        // last point is first point --> closed contour
        assert(pts.front() == to_point(points[pi]));
        ++pi;

        // change outer cw to ccw and inner ccw to cw order
        std::reverse(pts.begin(), pts.end());
        glyph_polygons.emplace_back(pts);
    }
    if (!glyph_polygons.empty()) {
        unsigned max_iteration = 10;
        // TrueTypeFonts use non zero winding number
        // https://docs.microsoft.com/en-us/typography/opentype/spec/ttch01
        // https://developer.apple.com/fonts/TrueType-Reference-Manual/RM01/Chap1.html
        bool is_non_zero = true;
        glyph.shape = Emboss::heal_polygons(glyph_polygons, is_non_zero, max_iteration);
    }
    return glyph;
}

const Glyph* get_glyph(
    int              unicode,
    const FontFile & font,
    const FontProp & font_prop,
    Glyphs &         cache,
    fontinfo_opt &font_info_opt)
{
    // TODO: Use resolution by printer configuration, or add it into FontProp
    const float RESOLUTION = 0.0125f; // [in mm]
    auto glyph_item = cache.find(unicode);
    if (glyph_item != cache.end()) return &glyph_item->second;

    unsigned int font_index = font_prop.collection_number.value_or(0);
    if (!is_valid(font, font_index)) return nullptr;

    if (!font_info_opt.has_value()) {
        
        font_info_opt  = load_font_info(font.data->data(), font_index);
        // can load font info?
        if (!font_info_opt.has_value()) return nullptr;
    }

    float flatness = font.infos[font_index].ascent * RESOLUTION / font_prop.size_in_mm;

    // Fix for very small flatness because it create huge amount of points from curve
    if (flatness < RESOLUTION) flatness = RESOLUTION;

    std::optional<Glyph> glyph_opt = get_glyph(*font_info_opt, unicode, flatness);

    // IMPROVE: multiple loadig glyph without data
    // has definition inside of font?
    if (!glyph_opt.has_value()) return nullptr;

    Glyph &glyph = *glyph_opt;
    if (font_prop.char_gap.has_value()) 
        glyph.advance_width += *font_prop.char_gap;

    // scale glyph size
    glyph.advance_width = static_cast<int>(glyph.advance_width / SHAPE_SCALE);
    glyph.left_side_bearing = static_cast<int>(glyph.left_side_bearing / SHAPE_SCALE);

    if (!glyph.shape.empty()) {
        if (font_prop.boldness.has_value()) {
            float delta = static_cast<float>(*font_prop.boldness / SHAPE_SCALE / font_prop.size_in_mm);
            glyph.shape = Slic3r::union_ex(offset_ex(glyph.shape, delta));
        }
        if (font_prop.skew.has_value()) {
            double ratio = *font_prop.skew;
            auto skew = [&ratio](Polygon &polygon) {
                for (Slic3r::Point &p : polygon.points)
                    p.x() += static_cast<Point::coord_type>(std::round(p.y() * ratio));
            };
            for (ExPolygon &expolygon : glyph.shape) {
                skew(expolygon.contour);
                for (Polygon &hole : expolygon.holes) skew(hole);
            }
        }
    }
    auto [it, success] = cache.try_emplace(unicode, std::move(glyph));
    assert(success);
    return &it->second;
}

Point to_point(const stbtt__point &point) {
    return Point(static_cast<int>(std::round(point.x / SHAPE_SCALE)),
                 static_cast<int>(std::round(point.y / SHAPE_SCALE)));
}

} // namespace

#ifdef _WIN32
#include <windows.h>
#include <wingdi.h>
#include <windef.h>
#include <WinUser.h>

namespace {
EmbossStyle create_style(const std::wstring& name, const std::wstring& path) {
    return { boost::nowide::narrow(name.c_str()),
             boost::nowide::narrow(path.c_str()),
             EmbossStyle::Type::file_path, FontProp() };
}
} // namespace

// Get system font file path
std::optional<std::wstring> Emboss::get_font_path(const std::wstring &font_face_name)
{
//    static const LPWSTR fontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    static const LPCWSTR fontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    HKEY hKey;
    LONG result;

    // Open Windows font registry key
    result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, fontRegistryPath, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) return {};    

    DWORD maxValueNameSize, maxValueDataSize;
    result = RegQueryInfoKey(hKey, 0, 0, 0, 0, 0, 0, 0, &maxValueNameSize, &maxValueDataSize, 0, 0);
    if (result != ERROR_SUCCESS) return {};

    DWORD valueIndex = 0;
    LPWSTR valueName = new WCHAR[maxValueNameSize];
    LPBYTE valueData = new BYTE[maxValueDataSize];
    DWORD valueNameSize, valueDataSize, valueType;
    std::wstring wsFontFile;

    // Look for a matching font name
    do {
        wsFontFile.clear();
        valueDataSize = maxValueDataSize;
        valueNameSize = maxValueNameSize;

        result = RegEnumValue(hKey, valueIndex, valueName, &valueNameSize, 0, &valueType, valueData, &valueDataSize);

        valueIndex++;
        if (result != ERROR_SUCCESS || valueType != REG_SZ) {
            continue;
        }

        std::wstring wsValueName(valueName, valueNameSize);

        // Found a match
        if (_wcsnicmp(font_face_name.c_str(), wsValueName.c_str(), font_face_name.length()) == 0) {

            wsFontFile.assign((LPWSTR)valueData, valueDataSize);
            break;
        }
    }while (result != ERROR_NO_MORE_ITEMS);

    delete[] valueName;
    delete[] valueData;

    RegCloseKey(hKey);

    if (wsFontFile.empty()) return {};
    
    // Build full font file path
    WCHAR winDir[MAX_PATH];
    GetWindowsDirectory(winDir, MAX_PATH);

    std::wstringstream ss;
    ss << winDir << "\\Fonts\\" << wsFontFile;
    wsFontFile = ss.str();

    return wsFontFile;
}

EmbossStyles Emboss::get_font_list()
{
    //EmbossStyles list1 = get_font_list_by_enumeration();
    //EmbossStyles list2 = get_font_list_by_register();
    //EmbossStyles list3 = get_font_list_by_folder();
    return get_font_list_by_register();
}

EmbossStyles Emboss::get_font_list_by_register() {
//    static const LPWSTR fontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    static const LPCWSTR fontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    HKEY hKey;
    LONG result;

    // Open Windows font registry key
    result = RegOpenKeyEx(HKEY_LOCAL_MACHINE, fontRegistryPath, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        assert(false);
        //std::wcerr << L"Can not Open register key (" << fontRegistryPath << ")" 
        //    << L", function 'RegOpenKeyEx' return code: " << result <<  std::endl;
        return {}; 
    }

    DWORD maxValueNameSize, maxValueDataSize;
    result = RegQueryInfoKey(hKey, 0, 0, 0, 0, 0, 0, 0, &maxValueNameSize,
                             &maxValueDataSize, 0, 0);
    if (result != ERROR_SUCCESS) {
        assert(false);
        // Can not earn query key, function 'RegQueryInfoKey' return code: result
        return {}; 
    }

    // Build full font file path
    WCHAR winDir[MAX_PATH];
    GetWindowsDirectory(winDir, MAX_PATH);
    std::wstring font_path = std::wstring(winDir) + L"\\Fonts\\";

    EmbossStyles font_list;
    DWORD    valueIndex = 0;
    // Look for a matching font name
    LPWSTR font_name = new WCHAR[maxValueNameSize];
    LPBYTE fileTTF_name = new BYTE[maxValueDataSize];
    DWORD  font_name_size, fileTTF_name_size, valueType;
    do {
        fileTTF_name_size = maxValueDataSize;
        font_name_size = maxValueNameSize;

        result = RegEnumValue(hKey, valueIndex, font_name, &font_name_size, 0,
                              &valueType, fileTTF_name, &fileTTF_name_size);
        valueIndex++;
        if (result != ERROR_SUCCESS || valueType != REG_SZ) continue;
        std::wstring font_name_w(font_name, font_name_size);
        std::wstring file_name_w((LPWSTR) fileTTF_name, fileTTF_name_size);
        std::wstring path_w = font_path + file_name_w;

        // filtrate .fon from lists
        size_t pos = font_name_w.rfind(L" (TrueType)");
        if (pos >= font_name_w.size()) continue;
        // remove TrueType text from name
        font_name_w = std::wstring(font_name_w, 0, pos);
        font_list.emplace_back(create_style(font_name_w, path_w));
    } while (result != ERROR_NO_MORE_ITEMS);
    delete[] font_name;
    delete[] fileTTF_name;

    RegCloseKey(hKey);
    return font_list;
}

// TODO: Fix global function
bool CALLBACK EnumFamCallBack(LPLOGFONT       lplf,
                              LPNEWTEXTMETRIC lpntm,
                              DWORD           FontType,
                              LPVOID          aFontList)
{
    std::vector<std::wstring> *fontList =
        (std::vector<std::wstring> *) (aFontList);
    if (FontType & TRUETYPE_FONTTYPE) {
        std::wstring name = lplf->lfFaceName;
        fontList->push_back(name);
    }
    return true;
    // UNREFERENCED_PARAMETER(lplf);
    UNREFERENCED_PARAMETER(lpntm);
}

EmbossStyles Emboss::get_font_list_by_enumeration() {   

    HDC                       hDC = GetDC(NULL);
    std::vector<std::wstring> font_names;
    EnumFontFamilies(hDC, (LPCTSTR) NULL, (FONTENUMPROC) EnumFamCallBack,
                     (LPARAM) &font_names);

    EmbossStyles font_list;
    for (const std::wstring &font_name : font_names) {
        font_list.emplace_back(create_style(font_name, L""));
    }    
    return font_list;
}

EmbossStyles Emboss::get_font_list_by_folder() {
    EmbossStyles result;
    WCHAR winDir[MAX_PATH];
    UINT winDir_size = GetWindowsDirectory(winDir, MAX_PATH);
    std::wstring search_dir = std::wstring(winDir, winDir_size) + L"\\Fonts\\";
    WIN32_FIND_DATA fd;
    HANDLE          hFind;
    // By https://en.wikipedia.org/wiki/TrueType has also suffix .tte
    std::vector<std::wstring> suffixes = {L"*.ttf", L"*.ttc", L"*.tte"};
    for (const std::wstring &suffix : suffixes) {
        hFind = ::FindFirstFile((search_dir + suffix).c_str(), &fd);
        if (hFind == INVALID_HANDLE_VALUE) continue;
        do {
            // skip folder . and ..
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring file_name(fd.cFileName);
            // TODO: find font name instead of filename
            result.emplace_back(create_style(file_name, search_dir + file_name));
        } while (::FindNextFile(hFind, &fd));
        ::FindClose(hFind);
    }
    return result;
}

#else
EmbossStyles Emboss::get_font_list() { 
    // not implemented
    return {}; 
}

std::optional<std::wstring> Emboss::get_font_path(const std::wstring &font_face_name){
    // not implemented
    return {};
}
#endif

std::unique_ptr<FontFile> Emboss::create_font_file(
    std::unique_ptr<std::vector<unsigned char>> data)
{
    int collection_size = stbtt_GetNumberOfFonts(data->data());
    // at least one font must be inside collection
    if (collection_size < 1) {
        assert(false);
        // There is no font collection inside font data
        return nullptr;
    }

    unsigned int c_size = static_cast<unsigned int>(collection_size);
    std::vector<FontFile::Info> infos;
    infos.reserve(c_size);
    for (unsigned int i = 0; i < c_size; ++i) {
        auto font_info = load_font_info(data->data(), i);
        if (!font_info.has_value()) return nullptr;

        const stbtt_fontinfo *info = &(*font_info);
        // load information about line gap
        int ascent, descent, linegap;
        stbtt_GetFontVMetrics(info, &ascent, &descent, &linegap);

        float pixels       = 1000.; // value is irelevant
        float em_pixels    = stbtt_ScaleForMappingEmToPixels(info, pixels);
        int   units_per_em = static_cast<int>(std::round(pixels / em_pixels));

        infos.emplace_back(FontFile::Info{ascent, descent, linegap, units_per_em});
    }
    return std::make_unique<FontFile>(std::move(data), std::move(infos));
}

std::unique_ptr<FontFile> Emboss::create_font_file(const char *file_path)
{
    FILE *file = std::fopen(file_path, "rb");
    if (file == nullptr) {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "Couldn't open " << file_path << " for reading.";
        return nullptr;
    }
    ScopeGuard sg([&file]() { std::fclose(file); });

    // find size of file
    if (fseek(file, 0L, SEEK_END) != 0) {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "Couldn't fseek file " << file_path << " for size measure.";
        return nullptr;
    }
    size_t size = ftell(file);
    if (size == 0) {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "Size of font file is zero. Can't read.";
        return nullptr;    
    }
    rewind(file);
    auto buffer = std::make_unique<std::vector<unsigned char>>(size);
    size_t count_loaded_bytes = fread((void *) &buffer->front(), 1, size, file);
    if (count_loaded_bytes != size) {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "Different loaded(from file) data size.";
        return nullptr;
    }
    return create_font_file(std::move(buffer));
}


#ifdef _WIN32
static bool load_hfont(void* hfont, DWORD &dwTable, DWORD &dwOffset, size_t& size, HDC hdc = nullptr){
    bool del_hdc = false;
    if (hdc == nullptr) { 
        del_hdc = true;
        hdc = ::CreateCompatibleDC(NULL);
        if (hdc == NULL) return false;
    }
    
    // To retrieve the data from the beginning of the file for TrueType
    // Collection files specify 'ttcf' (0x66637474).
    dwTable  = 0x66637474;
    dwOffset = 0;

    ::SelectObject(hdc, hfont);
    size = ::GetFontData(hdc, dwTable, dwOffset, NULL, 0);
    if (size == GDI_ERROR) {
        // HFONT is NOT TTC(collection)
        dwTable = 0;
        size    = ::GetFontData(hdc, dwTable, dwOffset, NULL, 0);
    }

    if (size == 0 || size == GDI_ERROR) {
        if (del_hdc) ::DeleteDC(hdc);
        return false;
    }
    return true;
}

void *Emboss::can_load(void *hfont)
{
    DWORD dwTable=0, dwOffset=0;
    size_t size = 0;
    if (!load_hfont(hfont, dwTable, dwOffset, size)) return nullptr;
    return hfont;
}

std::unique_ptr<FontFile> Emboss::create_font_file(void *hfont)
{
    HDC hdc = ::CreateCompatibleDC(NULL);
    if (hdc == NULL) {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "Can't create HDC by CreateCompatibleDC(NULL).";
        return nullptr;
    }

    DWORD dwTable=0,dwOffset = 0;
    size_t size;
    if (!load_hfont(hfont, dwTable, dwOffset, size, hdc)) {
        ::DeleteDC(hdc);
        return nullptr;
    }
    auto buffer = std::make_unique<std::vector<unsigned char>>(size);
    size_t loaded_size = ::GetFontData(hdc, dwTable, dwOffset, buffer->data(), size);
    ::DeleteDC(hdc);
    if (size != loaded_size) {
        assert(false);
        BOOST_LOG_TRIVIAL(error) << "Different loaded(from HFONT) data size.";
        return nullptr;    
    }
    return create_font_file(std::move(buffer));
}
#endif // _WIN32

std::optional<Glyph> Emboss::letter2glyph(const FontFile &font,
                                                  unsigned int    font_index,
                                                  int             letter,
                                                  float           flatness)
{
    if (!is_valid(font, font_index)) return {};
    auto font_info_opt = load_font_info(font.data->data(), font_index);
    if (!font_info_opt.has_value()) return {};
    return get_glyph(*font_info_opt, letter, flatness);
}

const FontFile::Info &Emboss::get_font_info(const FontFile &font, const FontProp &prop)
{
    unsigned int font_index = prop.collection_number.value_or(0);
    assert(is_valid(font, font_index));
    return font.infos[font_index];
}

int Emboss::get_line_height(const FontFile &font, const FontProp &prop) {
    const FontFile::Info &info = get_font_info(font, prop);
    int line_height = info.ascent - info.descent + info.linegap;
    line_height += prop.line_gap.value_or(0);
    return static_cast<int>(line_height / SHAPE_SCALE);
}

namespace {
ExPolygons letter2shapes(
    wchar_t letter, Point &cursor, FontFileWithCache &font_with_cache, const FontProp &font_prop, fontinfo_opt& font_info_cache)
{
    assert(font_with_cache.has_value());
    if (!font_with_cache.has_value())
        return {};

    Glyphs &cache = *font_with_cache.cache;
    const FontFile &font  = *font_with_cache.font_file;

    if (letter == '\n') {
        cursor.x() = 0;
        // 2d shape has opposit direction of y
        cursor.y() -= get_line_height(font, font_prop);
        return {};
    }
    if (letter == '\t') {
        // '\t' = 4*space => same as imgui
        const int count_spaces = 4;
        const Glyph *space = get_glyph(int(' '), font, font_prop, cache, font_info_cache);
        if (space == nullptr)
            return {};
        cursor.x() += count_spaces * space->advance_width;
        return {};
    }
    if (letter == '\r')
        return {};

    int unicode = static_cast<int>(letter);
    auto it = cache.find(unicode);

    // Create glyph from font file and cache it
    const Glyph *glyph_ptr = (it != cache.end()) ? &it->second : get_glyph(unicode, font, font_prop, cache, font_info_cache);
    if (glyph_ptr == nullptr)
        return {};

    // move glyph to cursor position
    ExPolygons expolygons = glyph_ptr->shape; // copy
    for (ExPolygon &expolygon : expolygons)
        expolygon.translate(cursor);

    cursor.x() += glyph_ptr->advance_width;
    return expolygons;
}

// Check cancel every X letters in text
// Lower number - too much checks(slows down)
// Higher number - slows down response on cancelation
const int CANCEL_CHECK = 10;
} // namespace

namespace {
HealedExPolygons union_with_delta(const ExPolygonsWithIds &shapes, float delta, unsigned max_heal_iteration)
{
    // unify to one expolygons
    ExPolygons expolygons;
    for (const ExPolygonsWithId &shape : shapes) {
        if (shape.expoly.empty())
            continue;
        expolygons_append(expolygons, offset_ex(shape.expoly, delta));
    }
    ExPolygons result = union_ex(expolygons);
    result            = offset_ex(result, -delta);
    bool is_healed    = heal_expolygons(result, max_heal_iteration);
    return {result, is_healed};
}
} // namespace

ExPolygons Slic3r::union_with_delta(EmbossShape &shape, float delta, unsigned max_heal_iteration) 
{
    if (!shape.final_shape.expolygons.empty())
        return shape.final_shape;

    shape.final_shape = ::union_with_delta(shape.shapes_with_ids, delta, max_heal_iteration);
    for (const ExPolygonsWithId &e : shape.shapes_with_ids)
        if (!e.is_healed)
            shape.final_shape.is_healed = false;
    return shape.final_shape.expolygons;
}

void Slic3r::translate(ExPolygonsWithIds &expolygons_with_ids, const Point &p)
{
    for (ExPolygonsWithId &expolygons_with_id : expolygons_with_ids)
        translate(expolygons_with_id.expoly, p);
}

BoundingBox Slic3r::get_extents(const ExPolygonsWithIds &expolygons_with_ids)
{
    BoundingBox bb;
    for (const ExPolygonsWithId &expolygons_with_id : expolygons_with_ids)        
        bb.merge(get_extents(expolygons_with_id.expoly));
    return bb;
}

void Slic3r::center(ExPolygonsWithIds &e)
{
    BoundingBox bb = get_extents(e);
    translate(e, -bb.center());
}

HealedExPolygons Emboss::text2shapes(FontFileWithCache &font_with_cache, const char *text, const FontProp &font_prop, const std::function<bool()>& was_canceled)
{
    std::wstring text_w = boost::nowide::widen(text);
    ExPolygonsWithIds vshapes = text2vshapes(font_with_cache, text_w, font_prop, was_canceled);

    float delta = static_cast<float>(1. / SHAPE_SCALE);
    return ::union_with_delta(vshapes, delta, MAX_HEAL_ITERATION_OF_TEXT);
}

namespace {
/// <summary>
/// Align shape against pivot
/// </summary>
/// <param name="shapes">Shapes to align
/// Prerequisities: shapes are aligned left top</param>
/// <param name="text">To detect end of lines - to be able horizontal center the line</param>
/// <param name="prop">Containe Horizontal and vertical alignment</param>
/// <param name="font">Needed for scale and font size</param>
void align_shape(ExPolygonsWithIds &shapes, const std::wstring &text, const FontProp &prop, const FontFile &font);
}

ExPolygonsWithIds Emboss::text2vshapes(FontFileWithCache &font_with_cache, const std::wstring& text, const FontProp &font_prop, const std::function<bool()>& was_canceled){
    assert(font_with_cache.has_value());
    const FontFile &font = *font_with_cache.font_file;
    unsigned int font_index = font_prop.collection_number.value_or(0);
    if (!is_valid(font, font_index))
        return {};

    unsigned counter = 0;
    Point cursor(0, 0);

    fontinfo_opt font_info_cache;  
    ExPolygonsWithIds result;
    result.reserve(text.size());
    for (wchar_t letter : text) {
        if (++counter == CANCEL_CHECK) {
            counter = 0;
            if (was_canceled())
                return {};
        }
        unsigned id = static_cast<unsigned>(letter);
        result.push_back({id, letter2shapes(letter, cursor, font_with_cache, font_prop, font_info_cache)});
    }

    align_shape(result, text, font_prop, font);
    return result;
}

#include <boost/range/adaptor/reversed.hpp>
unsigned Emboss::get_count_lines(const std::wstring& ws)
{
    if (ws.empty())
        return 0;

    unsigned count = 1;
    for (wchar_t wc : ws)
        if (wc == '\n')
            ++count;
    return count;

    // unsigned prev_count = 0;
    // for (wchar_t wc : ws)
    //     if (wc == '\n')
    //         ++prev_count;
    //     else
    //         break;
    //
    // unsigned post_count = 0;
    // for (wchar_t wc : boost::adaptors::reverse(ws))
    //     if (wc == '\n')
    //         ++post_count;
    //     else
    //         break;
    //return count - prev_count - post_count;
}

unsigned Emboss::get_count_lines(const std::string &text)
{
    std::wstring ws = boost::nowide::widen(text.c_str());
    return get_count_lines(ws);
}

unsigned Emboss::get_count_lines(const ExPolygonsWithIds &shapes) {
    if (shapes.empty())
        return 0; // no glyphs
    unsigned result = 1; // one line is minimum
    for (const ExPolygonsWithId &shape_id : shapes)
        if (shape_id.id == ENTER_UNICODE)
            ++result;
    return result;
}

void Emboss::apply_transformation(const std::optional<float>& angle, const std::optional<float>& distance, Transform3d &transformation) {
    if (angle.has_value()) {
        double angle_z = *angle;
        transformation *= Eigen::AngleAxisd(angle_z, Vec3d::UnitZ());
    }
    if (distance.has_value()) {
        Vec3d translate = Vec3d::UnitZ() * (*distance);
        transformation.translate(translate);
    }
}

bool Emboss::is_italic(const FontFile &font, unsigned int font_index)
{
    if (font_index >= font.infos.size()) return false;
    fontinfo_opt font_info_opt = load_font_info(font.data->data(), font_index);

    if (!font_info_opt.has_value()) return false;
    stbtt_fontinfo *info = &(*font_info_opt);

    // https://docs.microsoft.com/cs-cz/typography/opentype/spec/name
    // https://developer.apple.com/fonts/TrueType-Reference-Manual/RM06/Chap6name.html
    // 2 ==> Style / Subfamily name
    int name_id = 2;
    int length;
    const char* value = stbtt_GetFontNameString(info, &length,
                                               STBTT_PLATFORM_ID_MICROSOFT,
                                               STBTT_MS_EID_UNICODE_BMP,
                                               STBTT_MS_LANG_ENGLISH,                            
                                               name_id);

    // value is big endian utf-16 i need extract only normal chars
    std::string value_str;
    value_str.reserve(length / 2);
    for (int i = 1; i < length; i += 2)
        value_str.push_back(value[i]);

    // lower case
    std::transform(value_str.begin(), value_str.end(), value_str.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const std::vector<std::string> italics({"italic", "oblique"});
    for (const std::string &it : italics) { 
        if (value_str.find(it) != std::string::npos) { 
            return true; 
        }
    }
    return false; 
}

std::string Emboss::create_range_text(const std::string &text,
                                      const FontFile    &font,
                                      unsigned int       font_index,
                                      bool              *exist_unknown)
{
    if (!is_valid(font, font_index)) return {};
            
    std::wstring ws = boost::nowide::widen(text);

    // need remove symbols not contained in font
    std::sort(ws.begin(), ws.end());

    auto font_info_opt = load_font_info(font.data->data(), 0);
    if (!font_info_opt.has_value()) return {};
    const stbtt_fontinfo *font_info = &(*font_info_opt);

    if (exist_unknown != nullptr) *exist_unknown = false;
    int prev_unicode = -1;
    ws.erase(std::remove_if(ws.begin(), ws.end(),
        [&prev_unicode, font_info, exist_unknown](wchar_t wc) -> bool {
            int unicode = static_cast<int>(wc);

            // skip white spaces
            if (unicode == '\n' || 
                unicode == '\r' || 
                unicode == '\t') return true;

            // is duplicit?
            if (prev_unicode == unicode) return true;
            prev_unicode = unicode;

            // can find in font?
            bool is_unknown = !stbtt_FindGlyphIndex(font_info, unicode);
            if (is_unknown && exist_unknown != nullptr)
                *exist_unknown = true;
            return is_unknown;
        }), ws.end());

    return boost::nowide::narrow(ws);
}

double Emboss::get_text_shape_scale(const FontProp &fp, const FontFile &ff)
{
    const FontFile::Info &info = get_font_info(ff, fp);
    double scale  = fp.size_in_mm / (double) info.unit_per_em;
    // Shape is scaled for store point coordinate as integer
    return scale * SHAPE_SCALE;
}

namespace {

void add_quad(uint32_t              i1,
              uint32_t              i2,
              indexed_triangle_set &result,
              uint32_t              count_point)
{
    // bottom indices
    uint32_t i1_ = i1 + count_point;
    uint32_t i2_ = i2 + count_point;
    result.indices.emplace_back(i2, i2_, i1);
    result.indices.emplace_back(i1_, i1, i2_);
};

indexed_triangle_set polygons2model_unique(
    const ExPolygons          &shape2d,
    const IProjection &projection,
    const Points              &points)
{
    // CW order of triangle indices
    std::vector<Vec3i32> shape_triangles=Triangulation::triangulate(shape2d, points);
    uint32_t           count_point     = points.size();

    indexed_triangle_set result;
    result.vertices.reserve(2 * count_point);
    std::vector<Vec3f> &front_points = result.vertices; // alias
    std::vector<Vec3f>  back_points;
    back_points.reserve(count_point);

    for (const Point &p : points) {
        auto p2 = projection.create_front_back(p);
        front_points.push_back(p2.first.cast<float>());
        back_points.push_back(p2.second.cast<float>());
    }    
    
    // insert back points, front are already in
    result.vertices.insert(result.vertices.end(),
                           std::make_move_iterator(back_points.begin()),
                           std::make_move_iterator(back_points.end()));
    result.indices.reserve(shape_triangles.size() * 2 + points.size() * 2);
    // top triangles - change to CCW
    for (const Vec3i32 &t : shape_triangles)
        result.indices.emplace_back(t.x(), t.z(), t.y());
    // bottom triangles - use CW
    for (const Vec3i32 &t : shape_triangles)
        result.indices.emplace_back(t.x() + count_point, 
                                    t.y() + count_point,
                                    t.z() + count_point);

    // quads around - zig zag by triangles
    size_t polygon_offset = 0;
    auto add_quads = [&polygon_offset,&result, &count_point]
    (const Polygon& polygon) {
        uint32_t polygon_points = polygon.points.size();
        // previous index
        uint32_t prev = polygon_offset + polygon_points - 1;
        for (uint32_t p = 0; p < polygon_points; ++p) { 
            uint32_t index = polygon_offset + p;
            add_quad(prev, index, result, count_point);
            prev = index;
        }
        polygon_offset += polygon_points;
    };

    for (const ExPolygon &expolygon : shape2d) {
        add_quads(expolygon.contour);
        for (const Polygon &hole : expolygon.holes) add_quads(hole);
    }   

    return result;
}

indexed_triangle_set polygons2model_duplicit(
    const ExPolygons          &shape2d,
    const IProjection &projection,
    const Points              &points,
    const Points              &duplicits)
{
    // CW order of triangle indices
    std::vector<uint32_t> changes = Triangulation::create_changes(points, duplicits);
    std::vector<Vec3i32> shape_triangles = Triangulation::triangulate(shape2d, points, changes);
    uint32_t count_point = *std::max_element(changes.begin(), changes.end()) + 1;

    indexed_triangle_set result;
    result.vertices.reserve(2 * count_point);
    std::vector<Vec3f> &front_points = result.vertices;
    std::vector<Vec3f>  back_points;
    back_points.reserve(count_point);

    uint32_t max_index = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < changes.size(); ++i) { 
        uint32_t index = changes[i];
        if (max_index != std::numeric_limits<uint32_t>::max() &&
            index <= max_index) continue; // duplicit point
        assert(index == max_index + 1);
        assert(front_points.size() == index);
        assert(back_points.size() == index);
        max_index = index;
        const Point &p = points[i];
        auto p2 = projection.create_front_back(p);
        front_points.push_back(p2.first.cast<float>());
        back_points.push_back(p2.second.cast<float>());
    }
    assert(max_index+1 == count_point);    
    
    // insert back points, front are already in
    result.vertices.insert(result.vertices.end(),
                           std::make_move_iterator(back_points.begin()),
                           std::make_move_iterator(back_points.end()));

    result.indices.reserve(shape_triangles.size() * 2 + points.size() * 2);
    // top triangles - change to CCW
    for (const Vec3i32 &t : shape_triangles)
        result.indices.emplace_back(t.x(), t.z(), t.y());
    // bottom triangles - use CW
    for (const Vec3i32 &t : shape_triangles)
        result.indices.emplace_back(t.x() + count_point, t.y() + count_point,
                                    t.z() + count_point);

    // quads around - zig zag by triangles
    size_t polygon_offset = 0;
    auto add_quads = [&polygon_offset, &result, count_point, &changes]
    (const Polygon &polygon) {
        uint32_t polygon_points = polygon.points.size();
        // previous index
        uint32_t prev = changes[polygon_offset + polygon_points - 1];
        for (uint32_t p = 0; p < polygon_points; ++p) {
            uint32_t index = changes[polygon_offset + p];
            if (prev == index) continue;
            add_quad(prev, index, result, count_point);
            prev = index;
        }
        polygon_offset += polygon_points;
    };

    for (const ExPolygon &expolygon : shape2d) {
        add_quads(expolygon.contour);
        for (const Polygon &hole : expolygon.holes) add_quads(hole);
    }
    return result;
}
} // namespace

indexed_triangle_set Emboss::polygons2model(const ExPolygons &shape2d,
                                            const IProjection &projection)
{
    Points points = to_points(shape2d);    
    Points duplicits = collect_duplicates(points);
    return (duplicits.empty()) ?
        polygons2model_unique(shape2d, projection, points) :
        polygons2model_duplicit(shape2d, projection, points, duplicits);
}

std::pair<Vec3d, Vec3d> Emboss::ProjectZ::create_front_back(const Point &p) const
{
    Vec3d front(p.x(), p.y(), 0.);
    return std::make_pair(front, project(front));
}

Vec3d Emboss::ProjectZ::project(const Vec3d &point) const 
{
    Vec3d res = point; // copy
    res.z() = m_depth;
    return res;
}

std::optional<Vec2d> Emboss::ProjectZ::unproject(const Vec3d &p, double *depth) const {
    return Vec2d(p.x(), p.y());
}


Vec3d Emboss::suggest_up(const Vec3d normal, double up_limit) 
{
    // Normal must be 1
    assert(is_approx(normal.squaredNorm(), 1.));

    // wanted up direction of result
    Vec3d wanted_up_side = 
        (std::fabs(normal.z()) > up_limit)?
        Vec3d::UnitY() : Vec3d::UnitZ();

    // create perpendicular unit vector to surface triangle normal vector
    // lay on surface of triangle and define up vector for text
    Vec3d wanted_up_dir = normal.cross(wanted_up_side).cross(normal);
    // normal3d is NOT perpendicular to normal_up_dir
    wanted_up_dir.normalize();

    return wanted_up_dir;
}

std::optional<float> Emboss::calc_up(const Transform3d &tr, double up_limit)
{
    auto tr_linear = tr.linear().eval();
    // z base of transformation ( tr * UnitZ )
    Vec3d normal = tr_linear.col(2);
    // scaled matrix has base with different size
    normal.normalize();
    Vec3d suggested = suggest_up(normal, up_limit);
    assert(is_approx(suggested.squaredNorm(), 1.));

    Vec3d up = tr_linear.col(1); // tr * UnitY()
    up.normalize();    
    Matrix3d m;
    m.row(0) = up;
    m.row(1) = suggested;
    m.row(2) = normal;
    double det = m.determinant();
    double dot = suggested.dot(up);
    double res = -atan2(det, dot);
    if (is_approx(res, 0.))
        return {};
    return res;
}

Transform3d Emboss::create_transformation_onto_surface(const Vec3d &position,
                                                       const Vec3d &normal,
                                                       double       up_limit)
{
    // is normalized ?
    assert(is_approx(normal.squaredNorm(), 1.));

    // up and emboss direction for generated model
    Vec3d up_dir     = Vec3d::UnitY();
    Vec3d emboss_dir = Vec3d::UnitZ();

    // after cast from float it needs to be normalized again
    Vec3d wanted_up_dir = suggest_up(normal, up_limit);

    // perpendicular to emboss vector of text and normal
    Vec3d axis_view;
    double angle_view;
    if (normal == -Vec3d::UnitZ()) {
        // text_emboss_dir has opposit direction to wanted_emboss_dir
        axis_view = Vec3d::UnitY();
        angle_view = M_PI;
    } else {
        axis_view = emboss_dir.cross(normal);
        angle_view = std::acos(emboss_dir.dot(normal)); // in rad
        axis_view.normalize();
    }

    Eigen::AngleAxis view_rot(angle_view, axis_view);
    Vec3d wanterd_up_rotated = view_rot.matrix().inverse() * wanted_up_dir;
    wanterd_up_rotated.normalize();
    double angle_up = std::acos(up_dir.dot(wanterd_up_rotated));

    Vec3d text_view = up_dir.cross(wanterd_up_rotated);
    Vec3d diff_view  = emboss_dir - text_view;
    if (std::fabs(diff_view.x()) > 1. ||
        std::fabs(diff_view.y()) > 1. ||
        std::fabs(diff_view.z()) > 1.) // oposit direction
        angle_up *= -1.;

    Eigen::AngleAxis up_rot(angle_up, emboss_dir);

    Transform3d transform = Transform3d::Identity();
    transform.translate(position);
    transform.rotate(view_rot);
    transform.rotate(up_rot);
    return transform;
}


// OrthoProject

std::pair<Vec3d, Vec3d> Emboss::OrthoProject::create_front_back(const Point &p) const {
    Vec3d front(p.x(), p.y(), 0.);
    Vec3d front_tr = m_matrix * front;
    return std::make_pair(front_tr, project(front_tr));
}

Vec3d Emboss::OrthoProject::project(const Vec3d &point) const
{
    return point + m_direction;
}

std::optional<Vec2d> Emboss::OrthoProject::unproject(const Vec3d &p, double *depth) const
{
    Vec3d pp = m_matrix_inv * p;
    if (depth != nullptr) *depth = pp.z();
    return Vec2d(pp.x(), pp.y());
}

// sample slice
namespace {

// using coor2 = int64_t;
using Coord2 = double;
using P2     = Eigen::Matrix<Coord2, 2, 1, Eigen::DontAlign>;

bool point_in_distance(const Coord2 &distance_sq, PolygonPoint &polygon_point, const size_t &i, const Slic3r::Polygon &polygon, bool is_first, bool is_reverse = false)
{
    size_t s  = polygon.size();
    size_t ii = (i + polygon_point.index) % s;

    // second point of line
    const Point &p = polygon[ii];
    Point p_d = p - polygon_point.point;

    P2 p_d2 = p_d.cast<Coord2>();
    Coord2 p_distance_sq = p_d2.squaredNorm();
    if (p_distance_sq < distance_sq)
        return false;

    // found line
    if (is_first) {
        // on same line
        // center also lay on line
        // new point is distance moved from point by direction
        polygon_point.point += p_d * sqrt(distance_sq / p_distance_sq);
        return true;
    }

    // line cross circle

    // start point of line
    size_t ii2          = (is_reverse) ? (ii + 1) % s : (ii + s - 1) % s;
    polygon_point.index = (is_reverse) ? ii : ii2;
    const Point &p2 = polygon[ii2];

    Point line_dir  = p2 - p;
    P2    line_dir2 = line_dir.cast<Coord2>();

    Coord2 a = line_dir2.dot(line_dir2);
    Coord2 b = 2 * p_d2.dot(line_dir2);
    Coord2 c = p_d2.dot(p_d2) - distance_sq;

    double discriminant = b * b - 4 * a * c;
    if (discriminant < 0) {
        assert(false);
        // no intersection
        polygon_point.point = p;
        return true;
    }

    // ray didn't totally miss sphere,
    // so there is a solution to
    // the equation.
    discriminant = sqrt(discriminant);

    // either solution may be on or off the ray so need to test both
    // t1 is always the smaller value, because BOTH discriminant and
    // a are nonnegative.
    double t1 = (-b - discriminant) / (2 * a);
    double t2 = (-b + discriminant) / (2 * a);

    double t = std::min(t1, t2);
    if (t < 0. || t > 1.) {
        // Bad intersection
        assert(false);
        polygon_point.point = p;
        return true;
    }

    polygon_point.point = p + (t * line_dir2).cast<Point::coord_type>();
    return true;
}

void point_in_distance(int32_t distance, PolygonPoint &p, const Slic3r::Polygon &polygon)
{
    Coord2 distance_sq = static_cast<Coord2>(distance) * distance;
    bool is_first = true;
    for (size_t i = 1; i < polygon.size(); ++i) {
        if (point_in_distance(distance_sq, p, i, polygon, is_first))
            return;
        is_first = false;
    }
    // There is not point on polygon with this distance
}

void point_in_reverse_distance(int32_t distance, PolygonPoint &p, const Slic3r::Polygon &polygon)
{
    Coord2 distance_sq = static_cast<Coord2>(distance) * distance;
    bool is_first = true;
    bool is_reverse = true;
    for (size_t i = polygon.size(); i > 0; --i) {
        if (point_in_distance(distance_sq, p, i, polygon, is_first, is_reverse))
            return;
        is_first = false;
    }
    // There is not point on polygon with this distance
}
} // namespace

// calculate rotation, need copy of polygon point
double Emboss::calculate_angle(int32_t distance, PolygonPoint polygon_point, const Polygon &polygon)
{
    PolygonPoint polygon_point2 = polygon_point; // copy
    point_in_distance(distance, polygon_point, polygon);
    point_in_reverse_distance(distance, polygon_point2, polygon);

    Point surface_dir = polygon_point2.point - polygon_point.point;
    Point norm(-surface_dir.y(), surface_dir.x());
    Vec2d norm_d = norm.cast<double>();
    //norm_d.normalize();
    return std::atan2(norm_d.y(), norm_d.x());
}

std::vector<double> Emboss::calculate_angles(int32_t distance, const PolygonPoints& polygon_points, const Polygon &polygon)
{
    std::vector<double> result;
    result.reserve(polygon_points.size());
    for(const PolygonPoint& pp: polygon_points)
        result.emplace_back(calculate_angle(distance, pp, polygon));
    return result;
}

PolygonPoints Emboss::sample_slice(const TextLine &slice, const BoundingBoxes &bbs, double scale)
{
    // find BB in center of line
    size_t first_right_index = 0;
    for (const BoundingBox &bb : bbs)
        if (!bb.defined) // white char do not have bb
            continue;
        else if (bb.min.x() < 0)
            ++first_right_index;
        else 
            break;

    PolygonPoints samples(bbs.size());
    int32_t shapes_x_cursor = 0;

    PolygonPoint cursor = slice.start; //copy

    auto create_sample = [&] //polygon_cursor, &polygon_line_index, &line_bbs, &shapes_x_cursor, &shape_scale, &em_2_polygon, &line, &offsets]
    (const BoundingBox &bb, bool is_reverse) {
        if (!bb.defined)
            return cursor;
        Point   letter_center  = bb.center();
        int32_t shape_distance = shapes_x_cursor - letter_center.x();
        shapes_x_cursor        = letter_center.x();
        double  distance_mm    = shape_distance * scale;
        int32_t distance_polygon = static_cast<int32_t>(std::round(scale_(distance_mm)));
        if (is_reverse)
            point_in_distance(distance_polygon, cursor, slice.polygon);
        else
            point_in_reverse_distance(distance_polygon, cursor, slice.polygon);
        return cursor;
    };

    // calc transformation for letters on the Right side from center
    bool is_reverse = true;
    for (size_t index = first_right_index; index < bbs.size(); ++index)
        samples[index] = create_sample(bbs[index], is_reverse);

    // calc transformation for letters on the Left side from center
    if (first_right_index < bbs.size()) {
        shapes_x_cursor = bbs[first_right_index].center().x();
        cursor          = samples[first_right_index];
    }else{
        // only left side exists
        shapes_x_cursor = 0;
        cursor = slice.start; // copy    
    }
    is_reverse = false;
    for (size_t index_plus_one = first_right_index; index_plus_one > 0; --index_plus_one) {
        size_t index = index_plus_one - 1;
        samples[index] = create_sample(bbs[index], is_reverse);
    }
    return samples;
}

namespace {
float get_align_y_offset(FontProp::VerticalAlign align, unsigned count_lines, const FontFile &ff, const FontProp &fp)
{
    assert(count_lines != 0);
    int line_height = get_line_height(ff, fp);
    int ascent = get_font_info(ff, fp).ascent / SHAPE_SCALE;
    float line_center = static_cast<float>(std::round(ascent * ASCENT_CENTER));

    // direction of Y in 2d is from top to bottom
    // zero is on base line of first line
    switch (align) {
    case FontProp::VerticalAlign::bottom: return line_height * (count_lines - 1);
    case FontProp::VerticalAlign::top: return -ascent;
    case FontProp::VerticalAlign::center: 
    default: 
        return -line_center + line_height * (count_lines - 1) / 2.;
    }
}

int32_t get_align_x_offset(FontProp::HorizontalAlign align, const BoundingBox &shape_bb, const BoundingBox &line_bb)
{
    switch (align) {
    case FontProp::HorizontalAlign::right: return -shape_bb.max.x() + (shape_bb.size().x() - line_bb.size().x());
    case FontProp::HorizontalAlign::center: return -shape_bb.center().x() + (shape_bb.size().x() - line_bb.size().x()) / 2;
    case FontProp::HorizontalAlign::left: // no change
    default: break;
    }
    return 0;
}

void align_shape(ExPolygonsWithIds &shapes, const std::wstring &text, const FontProp &prop, const FontFile &font)
{
    // Shapes have to match letters in text
    assert(shapes.size() == text.length());

    unsigned count_lines = get_count_lines(text);
    int y_offset = get_align_y_offset(prop.align.second, count_lines, font, prop);

    // Speed up for left aligned text
    if (prop.align.first == FontProp::HorizontalAlign::left){
        // already horizontaly aligned
        for (ExPolygonsWithId& shape : shapes)
            for (ExPolygon &s : shape.expoly)
                s.translate(Point(0, y_offset));
        return;
    }

    BoundingBox shape_bb;
    for (const ExPolygonsWithId& shape: shapes)
        shape_bb.merge(get_extents(shape.expoly));

    auto get_line_bb = [&](size_t j) {
        BoundingBox line_bb;
        for (; j < text.length() && text[j] != '\n'; ++j)
            line_bb.merge(get_extents(shapes[j].expoly));
        return line_bb;
    };

    // Align x line by line
    Point offset(
        get_align_x_offset(prop.align.first, shape_bb, get_line_bb(0)), 
        y_offset);
    for (size_t i = 0; i < shapes.size(); ++i) {
        wchar_t letter = text[i];
        if (letter == '\n'){
            offset.x() = get_align_x_offset(prop.align.first, shape_bb, get_line_bb(i + 1));
            continue;
        }
        ExPolygons &shape = shapes[i].expoly;
        for (ExPolygon &s : shape)
            s.translate(offset);
    }
}
} // namespace

double Emboss::get_align_y_offset_in_mm(FontProp::VerticalAlign align, unsigned count_lines, const FontFile &ff, const FontProp &fp){
    float offset_in_font_point = get_align_y_offset(align, count_lines, ff, fp);
    double scale = get_text_shape_scale(fp, ff);
    return scale * offset_in_font_point;
}

#ifdef REMOVE_SPIKES
#include <Geometry.hpp>
void remove_spikes(Polygon &polygon, const SpikeDesc &spike_desc)
{
    enum class Type {
        add, // Move with point B on A-side and add new point on C-side
        move, // Only move with point B
        erase // left only points A and C without move 
    };
    struct SpikeHeal
    {
        Type   type;
        size_t index;
        Point  b;
        Point  add;
    };
    using SpikeHeals = std::vector<SpikeHeal>;
    SpikeHeals heals;

    size_t count = polygon.size();
    if (count < 3)
        return;

    const Point *ptr_a = &polygon[count - 2];
    const Point *ptr_b = &polygon[count - 1];
    for (const Point &c : polygon) {
        const Point &a = *ptr_a;
        const Point &b = *ptr_b;
        ScopeGuard sg([&ptr_a, &ptr_b, &c]() {
            // prepare for next loop
            ptr_a = ptr_b;
            ptr_b = &c;
        });

        // calc sides
        Point ba = a - b;
        Point bc = c - b;

        Vec2d ba_f = ba.cast<double>();
        Vec2d bc_f = bc.cast<double>();
        double dot_product = ba_f.dot(bc_f);

        // sqrt together after multiplication save one sqrt
        double ba_size_sq = ba_f.squaredNorm();
        double bc_size_sq = bc_f.squaredNorm();
        double norm = sqrt(ba_size_sq * bc_size_sq);
        double cos_angle = dot_product / norm;

        // small angle are around 1 --> cos(0) = 1
        if (cos_angle < spike_desc.cos_angle)
            continue;

        SpikeHeal heal;
        heal.index = &b - &polygon.points.front();

        // has to be in range <-1, 1>
        // Due to preccission of floating point number could be sligtly out of range
        if (cos_angle > 1.)
            cos_angle = 1.;
        if (cos_angle < -1.)
            cos_angle = -1.;

        // Current Spike angle
        double angle = acos(cos_angle);
        double wanted_size = spike_desc.half_bevel / cos(angle / 2.);
        double wanted_size_sq = wanted_size * wanted_size;

        bool is_ba_short = ba_size_sq < wanted_size_sq;
        bool is_bc_short = bc_size_sq < wanted_size_sq;
        auto a_side = [&b, &ba_f, &ba_size_sq, &wanted_size]() {
            Vec2d ba_norm = ba_f / sqrt(ba_size_sq);
            return b + (wanted_size * ba_norm).cast<coord_t>();
        };
        auto c_side = [&b, &bc_f, &bc_size_sq, &wanted_size]() {
            Vec2d bc_norm = bc_f / sqrt(bc_size_sq);
            return b + (wanted_size * bc_norm).cast<coord_t>();
        };
        if (is_ba_short && is_bc_short) {
            // remove short spike
            heal.type = Type::erase;
        } else if (is_ba_short){
            // move point B on C-side
            heal.type = Type::move;
            heal.b    = c_side();
        } else if (is_bc_short) {
            // move point B on A-side
            heal.type = Type::move;
            heal.b    = a_side();
        } else {
            // move point B on A-side and add point on C-side
            heal.type = Type::add;
            heal.b    = a_side();
            heal.add  = c_side();           
        }
        heals.push_back(heal);
    }

    if (heals.empty())
        return;

    // sort index from high to low
    if (heals.front().index == (count - 1))
        std::rotate(heals.begin(), heals.begin()+1, heals.end());
    std::reverse(heals.begin(), heals.end());

    int extend = 0;
    int curr_extend = 0;
    for (const SpikeHeal &h : heals)
        switch (h.type) {
        case Type::add:
            ++curr_extend;
            if (extend < curr_extend)
                extend = curr_extend;
            break;
        case Type::erase:
            --curr_extend;
        }

    Points &pts = polygon.points;
    if (extend > 0)
        pts.reserve(pts.size() + extend);

    for (const SpikeHeal &h : heals) {
        switch (h.type) {
        case Type::add:
            pts[h.index] = h.b;
            pts.insert(pts.begin() + h.index+1, h.add);
            break;
        case Type::erase:
            pts.erase(pts.begin() + h.index);
            break;
        case Type::move:
            pts[h.index] = h.b; 
            break;
        default: break;
        }
    }
}

void remove_spikes(Polygons &polygons, const SpikeDesc &spike_desc)
{
    for (Polygon &polygon : polygons)
        remove_spikes(polygon, spike_desc);
    remove_bad(polygons);
}

void remove_spikes(ExPolygons &expolygons, const SpikeDesc &spike_desc)
{
    for (ExPolygon &expolygon : expolygons) {
        remove_spikes(expolygon.contour, spike_desc);
        remove_spikes(expolygon.holes, spike_desc);    
    }
    remove_bad(expolygons);
}

#endif // REMOVE_SPIKES
