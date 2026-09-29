#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <unordered_map>

#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "ShortestPath.hpp"

#include <clipper2/clipper.h>

// #define CLIPPER_UTILS_DEBUG

#ifdef CLIPPER_UTILS_DEBUG
#include "SVG.hpp"
#endif /* CLIPPER_UTILS_DEBUG */

// Profiling support using the Shiny intrusive profiler
//#define CLIPPER_UTILS_PROFILE
#if defined(SLIC3R_PROFILE) && defined(CLIPPER_UTILS_PROFILE)
	#include <Shiny/Shiny.h>
	#define CLIPPERUTILS_PROFILE_FUNC() PROFILE_FUNC()
	#define CLIPPERUTILS_PROFILE_BLOCK(name) PROFILE_BLOCK(name)
#else
	#define CLIPPERUTILS_PROFILE_FUNC()
	#define CLIPPERUTILS_PROFILE_BLOCK(name)
#endif

namespace Slic3r {

#ifdef CLIPPER_UTILS_DEBUG
// For debugging the Clipper library, for providing bug reports to the Clipper author.
bool export_clipper_input_polygons_bin(const char *path, const ClipperLib::Paths &input_subject, const ClipperLib::Paths &input_clip)
{
    FILE *pfile = fopen(path, "wb");
    if (pfile == NULL)
        return false;

    uint32_t sz = uint32_t(input_subject.size());
    fwrite(&sz, 1, sizeof(sz), pfile);
    for (size_t i = 0; i < input_subject.size(); ++i) {
        const ClipperLib::Path &path = input_subject[i];
        sz = uint32_t(path.size());
        ::fwrite(&sz, 1, sizeof(sz), pfile);
        ::fwrite(path.data(), sizeof(ClipperLib::IntPoint), sz, pfile);
    }
    sz = uint32_t(input_clip.size());
    ::fwrite(&sz, 1, sizeof(sz), pfile);
    for (size_t i = 0; i < input_clip.size(); ++i) {
        const ClipperLib::Path &path = input_clip[i];
        sz = uint32_t(path.size());
        ::fwrite(&sz, 1, sizeof(sz), pfile);
        ::fwrite(path.data(), sizeof(ClipperLib::IntPoint), sz, pfile);
    }
    ::fclose(pfile);
    return true;

err:
    ::fclose(pfile);
    return false;
}
#endif /* CLIPPER_UTILS_DEBUG */

namespace ClipperUtils {
Points EmptyPathsProvider::s_empty_points;
Points SinglePathProvider::s_end;

// Clip source polygon to be used as a clipping polygon with a bouding box around the source (to be clipped) polygon.
// Useful as an optimization for expensive ClipperLib operations, for example when clipping source polygons one by one
// with a set of polygons covering the whole layer below.
template<typename PointsType> inline void clip_clipper_polygon_with_subject_bbox_templ(const PointsType &src, const BoundingBox &bbox, PointsType &out, const bool get_entire_polygons=false)
{
    using PointType = typename PointsType::value_type;

    out.clear();
    const size_t cnt = src.size();
    if (cnt < 3) return;

    enum class Side { Left = 1, Right = 2, Top = 4, Bottom = 8 };

    auto sides = [bbox](const PointType &p) {
        return int(p.x() < bbox.min.x()) * int(Side::Left) + int(p.x() > bbox.max.x()) * int(Side::Right) + int(p.y() < bbox.min.y()) * int(Side::Bottom) +
               int(p.y() > bbox.max.y()) * int(Side::Top);
    };

    int          sides_prev = sides(src.back());
    int          sides_this = sides(src.front());
    const size_t last       = cnt - 1;
    for (size_t i = 0; i < last; ++i) {
        int sides_next = sides(src[i + 1]);
        if ( // This point is inside. Take it.
            sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (sides_prev & sides_this & sides_next) == 0) {
            out.emplace_back(src[i]);
            sides_prev = sides_this;
        } else {
            // All the three points (this, prev, next) are outside at the same side.
            // Ignore this point.
        }
        sides_this = sides_next;
    }

    // Never produce just a single point output polygon.
    if (!out.empty()) {
        if (get_entire_polygons) {
            out=src;
        } else {
            if (int sides_next = sides(out.front());
            // The last point is inside. Take it.
            sides_this == 0 ||
            // Either this point is outside and previous or next is inside, or
            // the edge possibly cuts corner of the bounding box.
            (sides_prev & sides_this & sides_next) == 0)
            out.emplace_back(src.back());
        }
    }
}

void clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox, Points &out, const bool get_entire_polygons) { clip_clipper_polygon_with_subject_bbox_templ(src, bbox, out, get_entire_polygons); }
void clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox, ZPoints &out) { clip_clipper_polygon_with_subject_bbox_templ(src, bbox, out); }

template<typename PointsType> [[nodiscard]] PointsType clip_clipper_polygon_with_subject_bbox_templ(const PointsType &src, const BoundingBox &bbox)
{
    PointsType out;
    clip_clipper_polygon_with_subject_bbox(src, bbox, out);
    return out;
}

[[nodiscard]] Points  clip_clipper_polygon_with_subject_bbox(const Points &src, const BoundingBox &bbox) { return clip_clipper_polygon_with_subject_bbox_templ(src, bbox); }
[[nodiscard]] ZPoints clip_clipper_polygon_with_subject_bbox(const ZPoints &src, const BoundingBox &bbox) { return clip_clipper_polygon_with_subject_bbox_templ(src, bbox); }

void clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, Polygon &out) { 
    clip_clipper_polygon_with_subject_bbox(src.points, bbox, out.points);
}

[[nodiscard]] Polygon clip_clipper_polygon_with_subject_bbox(const Polygon &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygon out;
    clip_clipper_polygon_with_subject_bbox(src.points, bbox, out.points, get_entire_polygons);
    return out;
}

[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const Polygons &src, const BoundingBox &bbox)
{
    Polygons out;
    out.reserve(src.size());
    for (const Polygon &p : src) out.emplace_back(clip_clipper_polygon_with_subject_bbox(p, bbox));
    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) { return polygon.empty(); }), out.end());
    return out;
}
[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygon &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygons out;
    out.reserve(src.num_contours());
    out.emplace_back(clip_clipper_polygon_with_subject_bbox(src.contour, bbox, get_entire_polygons));
    for (const Polygon &p : src.holes) out.emplace_back(clip_clipper_polygon_with_subject_bbox(p, bbox, get_entire_polygons));
    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) { return polygon.empty(); }), out.end());
    return out;
}
[[nodiscard]] Polygons clip_clipper_polygons_with_subject_bbox(const ExPolygons &src, const BoundingBox &bbox, const bool get_entire_polygons)
{
    Polygons out;
    out.reserve(number_polygons(src));
    for (const ExPolygon &p : src) {
        Polygons temp = clip_clipper_polygons_with_subject_bbox(p, bbox, get_entire_polygons);
        out.insert(out.end(), temp.begin(), temp.end());
    }

    out.erase(std::remove_if(out.begin(), out.end(), [](const Polygon &polygon) {return polygon.empty(); }), out.end());
    return out;
}
}

static ExPolygons PolyTreeToExPolygons(ClipperLib::PolyTree &&polytree)
{
    struct Inner {
        static void PolyTreeToExPolygonsRecursive(ClipperLib::PolyNode &&polynode, ExPolygons *expolygons)
        {  
            size_t cnt = expolygons->size();
            expolygons->resize(cnt + 1);
            (*expolygons)[cnt].contour.points = std::move(polynode.Contour);
            (*expolygons)[cnt].holes.resize(polynode.ChildCount());
            for (int i = 0; i < polynode.ChildCount(); ++ i) {
                (*expolygons)[cnt].holes[i].points = std::move(polynode.Childs[i]->Contour);
                // Add outer polygons contained by (nested within) holes.
                for (int j = 0; j < polynode.Childs[i]->ChildCount(); ++ j)
                    PolyTreeToExPolygonsRecursive(std::move(*polynode.Childs[i]->Childs[j]), expolygons);
            }
        }

        static size_t PolyTreeCountExPolygons(const ClipperLib::PolyNode &polynode)
        {
            size_t cnt = 1;
            for (int i = 0; i < polynode.ChildCount(); ++ i) {
                for (int j = 0; j < polynode.Childs[i]->ChildCount(); ++ j)
                cnt += PolyTreeCountExPolygons(*polynode.Childs[i]->Childs[j]);
            }
            return cnt;
        }
    };

    ExPolygons retval;
    size_t cnt = 0;
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        cnt += Inner::PolyTreeCountExPolygons(*polytree.Childs[i]);
    retval.reserve(cnt);
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        Inner::PolyTreeToExPolygonsRecursive(std::move(*polytree.Childs[i]), &retval);
    return retval;
}

#if 0
// Global test.
bool has_duplicate_points(const ClipperLib::PolyTree &polytree)
{
    struct Helper {
        static void collect_points_recursive(const ClipperLib::PolyNode &polynode, ClipperLib::Path &out) {
            // For each hole of the current expolygon:
            out.insert(out.end(), polynode.Contour.begin(), polynode.Contour.end());
            for (int i = 0; i < polynode.ChildCount(); ++ i)
                collect_points_recursive(*polynode.Childs[i], out);
        }
    };
    ClipperLib::Path pts;
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        Helper::collect_points_recursive(*polytree.Childs[i], pts);
    return has_duplicate_points(std::move(pts));
}
#else
// Local test inside each of the contours.
bool has_duplicate_points(const ClipperLib::PolyTree &polytree)
{
    struct Helper {
        static bool has_duplicate_points_recursive(const ClipperLib::PolyNode &polynode) {
            if (has_duplicate_points(polynode.Contour))
                return true;
            for (int i = 0; i < polynode.ChildCount(); ++ i)
                if (has_duplicate_points_recursive(*polynode.Childs[i]))
                    return true;
            return false;
        }
    };
    ClipperLib::Path pts;
    for (int i = 0; i < polytree.ChildCount(); ++ i)
        if (Helper::has_duplicate_points_recursive(*polytree.Childs[i]))
            return true;
    return false;
}
#endif

// Clipper2 backend, the interface keeps Clipper1's enums.
namespace C2 = Clipper2Lib;

namespace {

C2::ClipType to_c2(ClipperLib::ClipType type)
{
    switch (type) {
    case ClipperLib::ctIntersection: return C2::ClipType::Intersection;
    case ClipperLib::ctUnion:        return C2::ClipType::Union;
    case ClipperLib::ctDifference:   return C2::ClipType::Difference;
    default:                         return C2::ClipType::Xor;
    }
}

C2::FillRule to_c2(ClipperLib::PolyFillType type)
{
    switch (type) {
    case ClipperLib::pftEvenOdd:  return C2::FillRule::EvenOdd;
    case ClipperLib::pftNonZero:  return C2::FillRule::NonZero;
    case ClipperLib::pftPositive: return C2::FillRule::Positive;
    default:                      return C2::FillRule::Negative;
    }
}

C2::JoinType to_c2(ClipperLib::JoinType type)
{
    switch (type) {
    case ClipperLib::jtSquare: return C2::JoinType::Square;
    case ClipperLib::jtRound:  return C2::JoinType::Round;
    default:                   return C2::JoinType::Miter;
    }
}

C2::EndType to_c2(ClipperLib::EndType type)
{
    switch (type) {
    case ClipperLib::etClosedPolygon: return C2::EndType::Polygon;
    case ClipperLib::etClosedLine:    return C2::EndType::Joined;
    case ClipperLib::etOpenSquare:    return C2::EndType::Square;
    case ClipperLib::etOpenRound:     return C2::EndType::Round;
    default:                          return C2::EndType::Butt;
    }
}

inline int64_t px(const Point &pt) { return pt.x(); }
inline int64_t py(const Point &pt) { return pt.y(); }
inline int64_t px(const C2::Point64 &pt) { return pt.x; }
inline int64_t py(const C2::Point64 &pt) { return pt.y; }

template<typename PathsProvider>
C2::Paths64 to_paths64(PathsProvider &&paths)
{
    C2::Paths64 out;
    out.reserve(paths.size());
    for (const Points &path : paths) {
        C2::Path64 &dst = out.emplace_back();
        dst.reserve(path.size());
        for (const Point &pt : path)
            dst.emplace_back(pt.x(), pt.y());
    }
    return out;
}

// Drops vertices closer than `shortest` to the last kept one, like Clipper1's ShortestEdgeLength.
template<class PathT>
bool append_offset_path(C2::Paths64 &out, const PathT &path, double shortest, C2::EndType end)
{
    int high = int(path.size()) - 1;
    if (high < 0)
        return false;
    const double shortest2 = shortest * shortest;
    auto same = [shortest2](const auto &a, const auto &b) {
        const double dx = double(px(a) - px(b));
        const double dy = double(py(a) - py(b));
        return shortest2 > 0. ? dx * dx + dy * dy < shortest2 : dx == 0. && dy == 0.;
    };
    if (end == C2::EndType::Polygon || end == C2::EndType::Joined)
        while (high > 0 && same(path[high], path[0]))
            -- high;
    C2::Path64 dst;
    dst.reserve(high + 1);
    dst.emplace_back(px(path[0]), py(path[0]));
    for (int i = 1, last = 0; i <= high; ++ i)
        if (! same(path[i], path[last])) {
            dst.emplace_back(px(path[i]), py(path[i]));
            last = i;
        }
    if (end == C2::EndType::Polygon && dst.size() < 3)
        return false;
    out.emplace_back(std::move(dst));
    return true;
}

inline Points c2_to_points(const C2::Path64 &path)
{
    Points out;
    out.reserve(path.size());
    for (const C2::Point64 &pt : path)
        out.emplace_back(pt.x, pt.y);
    return out;
}

Polygons c2_to_polygons(const C2::Paths64 &paths)
{
    Polygons out;
    out.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        out.emplace_back().points = c2_to_points(path);
    return out;
}

Polylines c2_to_polylines(const C2::Paths64 &paths)
{
    Polylines out;
    out.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        out.emplace_back(c2_to_points(path));
    return out;
}

size_t c2_count_expolygons(const C2::PolyPath64 &outer)
{
    size_t cnt = 1;
    for (const auto &hole : outer)
        for (const auto &island : *hole)
            cnt += c2_count_expolygons(*island);
    return cnt;
}

void c2_append_expolygons(const C2::PolyPath64 &outer, ExPolygons &out)
{
    ExPolygon &expoly = out.emplace_back();
    expoly.contour.points = c2_to_points(outer.Polygon());
    expoly.holes.reserve(outer.Count());
    for (const auto &hole : outer)
        expoly.holes.emplace_back().points = c2_to_points(hole->Polygon());
    // Islands inside the holes.
    for (const auto &hole : outer)
        for (const auto &island : *hole)
            c2_append_expolygons(*island, out);
}

ExPolygons c2_to_expolygons(const C2::PolyTree64 &tree)
{
    size_t cnt = 0;
    for (const auto &outer : tree)
        cnt += c2_count_expolygons(*outer);
    ExPolygons out;
    out.reserve(cnt);
    for (const auto &outer : tree)
        c2_append_expolygons(*outer, out);
    return out;
}

template<class TOut>
void c2_clip(C2::ClipType type, const C2::Paths64 &subject, const C2::Paths64 &clip, C2::FillRule fill, TOut &out)
{
    C2::Clipper64 clipper;
    // Clipper2 keeps collinear vertices by default, Clipper1 removed them.
    clipper.PreserveCollinear(false);
    clipper.AddSubject(subject);
    if (! clip.empty())
        clipper.AddClip(clip);
    clipper.Execute(type, fill, out);
}

Polylines c2_clip_open(C2::ClipType type, const C2::Paths64 &subject, const C2::Paths64 &clip)
{
    C2::Clipper64 clipper;
    clipper.PreserveCollinear(false);
    clipper.AddOpenSubject(subject);
    if (! clip.empty())
        clipper.AddClip(clip);
    C2::Paths64 closed, open;
    clipper.Execute(type, C2::FillRule::NonZero, closed, open);
    return c2_to_polylines(open);
}

// Clipper1 semantics: miter limit at least 2, arc tolerance (also for round caps) at most a quarter of the offset.
void c2_offset_setup(C2::ClipperOffset &co, float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    const double max_arc_tolerance = std::abs(delta) * 0.25;
    if (joinType == jtRound)
        co.ArcTolerance(miterLimit > 0. ? std::min(miterLimit, max_arc_tolerance) : 0.25);
    else {
        co.MiterLimit(std::max(miterLimit, 2.));
        co.ArcTolerance(std::min(0.25, max_arc_tolerance));
    }
}

// A single group offsets like Clipper1's path by path offset only if every CW path is a hole of a CCW path.
bool c2_offset_as_group(const C2::Paths64 &paths, const std::vector<double> &areas)
{
    std::vector<C2::Rect64> contours;
    for (size_t i = 0; i < paths.size(); ++ i)
        if (areas[i] > 0.)
            contours.emplace_back(C2::GetBounds(paths[i]));
    for (size_t i = 0; i < paths.size(); ++ i)
        if (areas[i] < 0.) {
            const C2::Rect64 hole = C2::GetBounds(paths[i]);
            if (std::none_of(contours.begin(), contours.end(), [&hole](const C2::Rect64 &r) {
                    return r.left < hole.left && r.right > hole.right && r.top < hole.top && r.bottom > hole.bottom;
                }))
                return false;
        }
    return true;
}

template<class TOut>
void c2_offset_closed_paths(C2::Paths64 &&paths, float delta, ClipperLib::JoinType joinType, double miterLimit, TOut &out)
{
    std::vector<double> areas;
    areas.reserve(paths.size());
    for (const C2::Path64 &path : paths)
        areas.emplace_back(C2::Area(path));
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    if (c2_offset_as_group(paths, areas)) {
        // Clipper2 would grow degenerate paths even for a negative offset.
        if (delta < 0. && std::all_of(areas.begin(), areas.end(), [](double a) { return a == 0.; }))
            return;
        co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
        co.Execute(delta, out);
        return;
    }
    C2::Paths64 offsetted, single;
    for (size_t i = 0; i < paths.size(); ++ i) {
        if (areas[i] == 0. && delta < 0.)
            continue;
        co.Clear();
        co.AddPath(paths[i], to_c2(joinType), C2::EndType::Polygon);
        // Clipper2 reverses a lone CW path, so the flipped sign shrinks it as a hole.
        co.Execute(areas[i] < 0. ? - delta : delta, single);
        append(offsetted, std::move(single));
    }
    c2_clip(C2::ClipType::Union, offsetted, {}, delta > 0. ? C2::FillRule::NonZero : C2::FillRule::Positive, out);
}

template<class PathsT, class TOut>
void c2_offset_closed(PathsT &&src, float delta, ClipperLib::JoinType joinType, double miterLimit, TOut &out)
{
    const double shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64  paths;
    paths.reserve(src.size());
    for (const auto &path : src)
        append_offset_path(paths, path, shortest, C2::EndType::Polygon);
    c2_offset_closed_paths(std::move(paths), delta, joinType, miterLimit, out);
}

template<class PathsProvider, class TOut>
void c2_offset2_closed(PathsProvider &&src, float delta1, float delta2, ClipperLib::JoinType joinType, double miterLimit, TOut &out)
{
    C2::Paths64 first;
    c2_offset_closed(std::forward<PathsProvider>(src), delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
}

inline const ExPolygon& expolygon_of(const ExPolygon &expoly) { return expoly; }
inline const ExPolygon& expolygon_of(const ExPolygon *expoly) { return *expoly; }
inline const ExPolygon& expolygon_of(const Surface &surface) { return surface.expolygon; }
inline const ExPolygon& expolygon_of(const Surface *surface) { return surface->expolygon; }

// Clipper1 ignored the orientation of expolygons, a single group needs CCW contours and CW holes.
template<class ExPolygonRange, class TOut>
void c2_offset_expolygons(const ExPolygonRange &src, float delta, ClipperLib::JoinType joinType, double miterLimit, TOut &out)
{
    const double shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64  paths;
    for (const auto &item : src) {
        const ExPolygon &expoly = expolygon_of(item);
        if (! append_offset_path(paths, expoly.contour.points, shortest, C2::EndType::Polygon))
            continue;
        if (const double area = C2::Area(paths.back()); area == 0.) {
            // A degenerate contour vanishes when shrunk and encloses no hole when grown.
            if (delta < 0.)
                paths.pop_back();
            continue;
        } else if (area < 0.)
            std::reverse(paths.back().begin(), paths.back().end());
        for (const Polygon &hole : expoly.holes)
            if (append_offset_path(paths, hole.points, shortest, C2::EndType::Polygon)) {
                if (const double area = C2::Area(paths.back()); area == 0.)
                    paths.pop_back();
                else if (area > 0.)
                    std::reverse(paths.back().begin(), paths.back().end());
            }
    }
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
    co.Execute(delta, out);
}

template<class ExPolygonRange>
Polygons c2_expolygons_offset(const ExPolygonRange &src, float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::Paths64 out;
    c2_offset_expolygons(src, delta, joinType, miterLimit, out);
    return c2_to_polygons(out);
}

template<class ExPolygonRange>
ExPolygons c2_expolygons_offset_ex(const ExPolygonRange &src, float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::PolyTree64 out;
    c2_offset_expolygons(src, delta, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

template<class PathsProvider>
Polygons c2_offset_lines(PathsProvider &&src, float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType endType)
{
    C2::Paths64 out;
    if (endType == ClipperLib::etClosedPolygon) {
        c2_offset_closed(std::forward<PathsProvider>(src), delta, joinType, miterLimit, out);
        return c2_to_polygons(out);
    }
    const C2::EndType end      = to_c2(endType);
    const double      shortest = std::abs(delta * ClipperOffsetShortestEdgeFactor);
    C2::Paths64       paths;
    paths.reserve(src.size());
    for (const Points &path : src)
        append_offset_path(paths, path, shortest, end);
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), end);
    co.Execute(delta, out);
    return c2_to_polygons(out);
}

template<class PathsProvider>
C2::Paths64 c2_clip_paths(PathsProvider &&clip, ApplySafetyOffset do_safety_offset)
{
    if (do_safety_offset == ApplySafetyOffset::No)
        return to_paths64(std::forward<PathsProvider>(clip));
    C2::Paths64 out;
    c2_offset_closed(std::forward<PathsProvider>(clip), ClipperSafetyOffset, DefaultJoinType, DefaultMiterLimit, out);
    return out;
}

} // namespace

Slic3r::Polygons offset(const Slic3r::Polygon &polygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::Paths64 paths;
    if (! append_offset_path(paths, polygon.points, std::abs(delta * ClipperOffsetShortestEdgeFactor), C2::EndType::Polygon))
        return {};
    const double area = C2::Area(paths.front());
    if (area == 0. && delta < 0.)
        return {};
    C2::ClipperOffset co;
    c2_offset_setup(co, delta, joinType, miterLimit);
    co.AddPaths(paths, to_c2(joinType), C2::EndType::Polygon);
    C2::Paths64 out;
    // A CW polygon shrinks as a hole and stays CW.
    co.Execute(area < 0. ? - delta : delta, out);
    return c2_to_polygons(out);
}

Slic3r::Polygons offset(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::Paths64 out;
    c2_offset_closed(ClipperUtils::PolygonsProvider(polygons), delta, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::ExPolygons offset_ex(const Slic3r::Polygons &polygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::PolyTree64 out;
    c2_offset_closed(ClipperUtils::PolygonsProvider(polygons), delta, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

Slic3r::Polygons offset(const Slic3r::Polyline &polyline, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type)
    { assert(delta > 0); return c2_offset_lines(ClipperUtils::SinglePathProvider(polyline.points), delta, joinType, miterLimit, end_type); }

Slic3r::Polygons offset(const Slic3r::Polyline3 &polyline, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type)
{
    assert(delta > 0);
    return c2_offset_lines(ClipperUtils::SinglePathProvider(polyline.to_polyline().points), delta, joinType, miterLimit, end_type);
}
Slic3r::Polygons offset(const Slic3r::Polylines &polylines, const float delta, ClipperLib::JoinType joinType, double miterLimit, ClipperLib::EndType end_type)
    { assert(delta > 0); return c2_offset_lines(ClipperUtils::PolylinesProvider(polylines), delta, joinType, miterLimit, end_type); }

Polygons contour_to_polygons(const Polygon &polygon, const float line_width, ClipperLib::JoinType join_type, double miter_limit)
    { assert(line_width > 1.f); return c2_offset_lines(ClipperUtils::SinglePathProvider(polygon.points), line_width / 2, join_type, miter_limit, ClipperLib::etClosedLine); }
Polygons contour_to_polygons(const Polygons &polygons, const float line_width, ClipperLib::JoinType join_type, double miter_limit)
    { assert(line_width > 1.f); return c2_offset_lines(ClipperUtils::PolygonsProvider(polygons), line_width / 2, join_type, miter_limit, ClipperLib::etClosedLine); }

Slic3r::Polygons offset(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(std::array<const ExPolygon*, 1>{ &expolygon }, delta, joinType, miterLimit); }
Slic3r::Polygons offset(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(expolygons, delta, joinType, miterLimit); }
Slic3r::Polygons offset(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(surfaces, delta, joinType, miterLimit); }
Slic3r::Polygons offset(const Slic3r::SurfacesPtr &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset(surfaces, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygon &expolygon, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(std::array<const ExPolygon*, 1>{ &expolygon }, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::ExPolygons &expolygons, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(expolygons, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::Surfaces &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(surfaces, delta, joinType, miterLimit); }
Slic3r::ExPolygons offset_ex(const Slic3r::SurfacesPtr &surfaces, const float delta, ClipperLib::JoinType joinType, double miterLimit)
    { return c2_expolygons_offset_ex(surfaces, delta, joinType, miterLimit); }

Polygons offset2(const ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::Paths64 first, out;
    c2_offset_expolygons(expolygons, delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
ExPolygons offset2_ex(const ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::Paths64    first;
    C2::PolyTree64 out;
    c2_offset_expolygons(expolygons, delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}
ExPolygons offset2_ex(const Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    C2::Paths64    first;
    C2::PolyTree64 out;
    c2_offset_expolygons(surfaces, delta1, joinType, miterLimit, first);
    c2_offset_closed(first, delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

// Offset outside, then inside produces morphological closing. All deltas should be positive.
Slic3r::Polygons closing(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::PolygonsProvider(polygons), delta1, - delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::ExPolygons closing_ex(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::PolyTree64 out;
    c2_offset2_closed(ClipperUtils::PolygonsProvider(polygons), delta1, - delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}
Slic3r::ExPolygons closing_ex(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::PolyTree64 out;
    c2_offset2_closed(ClipperUtils::SurfacesProvider(surfaces), delta1, - delta2, joinType, miterLimit, out);
    return c2_to_expolygons(out);
}

// Offset inside, then outside produces morphological opening. All deltas should be positive.
Slic3r::Polygons opening(const Slic3r::Polygons &polygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::PolygonsProvider(polygons), - delta1, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::Polygons opening(const Slic3r::ExPolygons &expolygons, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::ExPolygonsProvider(expolygons), - delta1, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}
Slic3r::Polygons opening(const Slic3r::Surfaces &surfaces, const float delta1, const float delta2, ClipperLib::JoinType joinType, double miterLimit)
{
    assert(delta1 > 0);
    assert(delta2 > 0);
    C2::Paths64 out;
    c2_offset2_closed(ClipperUtils::SurfacesProvider(surfaces), - delta1, delta2, joinType, miterLimit, out);
    return c2_to_polygons(out);
}

template<class TSubj, class TClip>
static inline Polygons _clipper(ClipperLib::ClipType clipType, TSubj &&subject, TClip &&clip, ApplySafetyOffset do_safety_offset)
{
    // Safety offset only allowed on intersection and difference.
    assert(do_safety_offset == ApplySafetyOffset::No || clipType != ClipperLib::ctUnion);
    C2::Paths64 out;
    c2_clip(to_c2(clipType), to_paths64(std::forward<TSubj>(subject)), c2_clip_paths(std::forward<TClip>(clip), do_safety_offset), C2::FillRule::NonZero, out);
    return c2_to_polygons(out);
}

Slic3r::Polygons diff(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset) 
    { return diff(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return diff_ex(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::ExPolygons diff_clipped(const Slic3r::ExPolygons & subject, const Slic3r::ExPolygons & clip, ApplySafetyOffset do_safety_offset)
{
    return diff_ex(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset);
}
Slic3r::Polygons diff(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons diff(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::Polygons intersection_clipped(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset) 
    { return intersection(subject, ClipperUtils::clip_clipper_polygons_with_subject_bbox(clip, get_extents(subject).inflated(SCALED_EPSILON)), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygon &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::Polygons intersection(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// BBS
Slic3r::Polygons intersection(const Slic3r::Polygons& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons clip_temp;
    clip_temp.push_back(clip);
    return intersection(subject, clip_temp, do_safety_offset);
}

Slic3r::Polygons union_(const Slic3r::Polygons &subject)
    { return _clipper(ClipperLib::ctUnion, ClipperUtils::PolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::Polygons union_(const Slic3r::ExPolygons &subject)
    { return _clipper(ClipperLib::ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::Polygons union_(const Slic3r::Polygons &subject, const ClipperLib::PolyFillType fillType)
{
    C2::Paths64 out;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, to_c2(fillType), out);
    return c2_to_polygons(out);
}

Slic3r::Polygons union_(const Slic3r::Polygons &subject, const Slic3r::Polygons &subject2)
    {
        // BBS
        Polygons polys = subject;
        for (const Polygon& poly : subject2)
            polys.push_back(poly);
        return union_(polys);
    }

// Clipper2 builds the PolyTree in one pass without Clipper1's slowdown on shared edges (#117).
template <typename TSubject, typename TClip>
static ExPolygons _clipper_ex(ClipperLib::ClipType clipType, TSubject &&subject,  TClip &&clip, ApplySafetyOffset do_safety_offset, ClipperLib::PolyFillType fill_type = ClipperLib::pftNonZero)
{
    assert(do_safety_offset == ApplySafetyOffset::No || clipType != ClipperLib::ctUnion);
    C2::PolyTree64 out;
    c2_clip(to_c2(clipType), to_paths64(std::forward<TSubject>(subject)), c2_clip_paths(std::forward<TClip>(clip), do_safety_offset), to_c2(fill_type), out);
    return c2_to_expolygons(out);
}

Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygon &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygon &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::SinglePathProvider(clip.points), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::ExPolygons &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons diff_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctDifference, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// BBS
inline Slic3r::ExPolygons diff_ex(const Slic3r::Polygon& subject, const Slic3r::Polygons& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons subject_temp;
    subject_temp.push_back(subject);

    return diff_ex(subject_temp, clip, do_safety_offset);
}

inline Slic3r::ExPolygons diff_ex(const Slic3r::Polygon& subject, const Slic3r::Polygon& clip, ApplySafetyOffset do_safety_offset)
{
    Slic3r::Polygons subject_temp;
    Slic3r::Polygons clip_temp;

    subject_temp.push_back(subject);
    clip_temp.push_back(clip);
    return diff_ex(subject_temp, clip_temp, do_safety_offset);
}

Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Polygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons& subject, const Slic3r::ExPolygon& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset);}
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygon& subject, const Slic3r::ExPolygons& clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset);}
Slic3r::ExPolygons intersection_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Polygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::PolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::Surfaces &subject, const Slic3r::Surfaces &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesProvider(subject), ClipperUtils::SurfacesProvider(clip), do_safety_offset); }
Slic3r::ExPolygons intersection_ex(const Slic3r::SurfacesPtr &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset)
    { return _clipper_ex(ClipperLib::ctIntersection, ClipperUtils::SurfacesPtrProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset); }
// May be used to "heal" unusual models (3DLabPrints etc.) by providing fill_type (pftEvenOdd, pftNonZero, pftPositive, pftNegative).
Slic3r::ExPolygons union_ex(const Slic3r::Polygons &subject, ClipperLib::PolyFillType fill_type)
    { return _clipper_ex(ClipperLib::ctUnion, ClipperUtils::PolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No, fill_type); }
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject)
    { return _clipper_ex(ClipperLib::ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons &subject, const Slic3r::Polygons &subject2)
    { return _clipper_ex(ClipperLib::ctUnion, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::PolygonsProvider(subject2), ApplySafetyOffset::No); }
Slic3r::ExPolygons union_ex(const Slic3r::Surfaces &subject)
    { return _clipper_ex(ClipperLib::ctUnion, ClipperUtils::SurfacesProvider(subject), ClipperUtils::EmptyPathsProvider(), ApplySafetyOffset::No); }

// BBS
Slic3r::ExPolygons union_ex(const Slic3r::ExPolygons& poly1, const Slic3r::ExPolygons& poly2, bool safety_offset_)
    {
    ExPolygons expolys = poly1;
    for (const ExPolygon& expoly : poly2)
        expolys.push_back(expoly);
    return union_ex(expolys);
}

Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygon &clip, ApplySafetyOffset do_safety_offset) {
    return _clipper_ex(ClipperLib::ctXor, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonProvider(clip), do_safety_offset);
}
Slic3r::ExPolygons xor_ex(const Slic3r::ExPolygons &subject, const Slic3r::ExPolygons &clip, ApplySafetyOffset do_safety_offset) {
    return _clipper_ex(ClipperLib::ctXor, ClipperUtils::ExPolygonsProvider(subject), ClipperUtils::ExPolygonsProvider(clip), do_safety_offset);
}

template<typename PathsProvider1, typename PathsProvider2>
Polylines _clipper_pl_open(ClipperLib::ClipType clipType, PathsProvider1 &&subject, PathsProvider2 &&clip)
    { return c2_clip_open(to_c2(clipType), to_paths64(std::forward<PathsProvider1>(subject)), to_paths64(std::forward<PathsProvider2>(clip))); }

// If the split_at_first_point() call above happens to split the polygon inside the clipping area
// we would get two consecutive polylines instead of a single one, so we go through them in order
// to recombine continuous polylines.
static void _clipper_pl_recombine(Polylines &polylines)
{
    for (size_t i = 0; i < polylines.size(); ++i) {
        for (size_t j = i+1; j < polylines.size(); ++j) {
            if (polylines[i].points.back() == polylines[j].points.front()) {
                /* If last point of i coincides with first point of j,
                   append points of j to i and delete j */
                polylines[i].points.insert(polylines[i].points.end(), polylines[j].points.begin()+1, polylines[j].points.end());
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.front() == polylines[j].points.back()) {
                /* If first point of i coincides with last point of j,
                   prepend points of j to i and delete j */
                polylines[i].points.insert(polylines[i].points.begin(), polylines[j].points.begin(), polylines[j].points.end()-1);
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.front() == polylines[j].points.front()) {
                /* Since Clipper does not preserve orientation of polylines, 
                   also check the case when first point of i coincides with first point of j. */
                polylines[j].reverse();
                polylines[i].points.insert(polylines[i].points.begin(), polylines[j].points.begin(), polylines[j].points.end()-1);
                polylines.erase(polylines.begin() + j);
                --j;
            } else if (polylines[i].points.back() == polylines[j].points.back()) {
                /* Since Clipper does not preserve orientation of polylines, 
                   also check the case when last point of i coincides with last point of j. */
                polylines[j].reverse();
                polylines[i].points.insert(polylines[i].points.end(), polylines[j].points.begin()+1, polylines[j].points.end());
                polylines.erase(polylines.begin() + j);
                --j;
            }
        }
    }
}

template<typename PathProvider1, typename PathProvider2>
Polylines _clipper_pl_closed(ClipperLib::ClipType clipType, PathProvider1 &&subject, PathProvider2 &&clip)
{
    // Transform input polygons into open paths.
    C2::Paths64 paths;
    paths.reserve(subject.size());
    for (const Points &poly : subject) {
        // Emplace polygon, duplicate the 1st point.
        C2::Path64 &path = paths.emplace_back();
        path.reserve(poly.size() + 1);
        for (const Point &pt : poly)
            path.emplace_back(pt.x(), pt.y());
        if (! poly.empty())
            path.emplace_back(poly.front().x(), poly.front().y());
    }
    // perform clipping
    Polylines retval = c2_clip_open(to_c2(clipType), paths, to_paths64(std::forward<PathProvider2>(clip)));
    _clipper_pl_recombine(retval);
    return retval;
}

Slic3r::Polylines diff_pl(const Slic3r::Polyline& subject, const Slic3r::Polygons& clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip)
    { return _clipper_pl_open(ClipperLib::ctDifference, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonsProvider(clip)); }
Slic3r::Polylines diff_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_closed(ClipperLib::ctDifference, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygon &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::SinglePathProvider(clip.points)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polyline &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygon &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polyline &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::SinglePathProvider(subject.points), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::PolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polylines &subject, const Slic3r::ExPolygons &clip)
    { return _clipper_pl_open(ClipperLib::ctIntersection, ClipperUtils::PolylinesProvider(subject), ClipperUtils::ExPolygonsProvider(clip)); }
Slic3r::Polylines intersection_pl(const Slic3r::Polygons &subject, const Slic3r::Polygons &clip)
    { return _clipper_pl_closed(ClipperLib::ctIntersection, ClipperUtils::PolygonsProvider(subject), ClipperUtils::PolygonsProvider(clip)); }

// Orca: Sort and orient open polyline fragments produced by clipping `source` with
// intersection_pl(), so that they run in the same order and direction as the source
// polyline. Clipping creates new endpoints at the clip boundary, but it keeps the
// interior source vertices intact, so a fragment's position on the source path is
// recovered exactly by looking its vertices up in the source. Fragments without any
// surviving source vertex lie on a single source segment, found by a nearest-segment
// search.
void restore_source_path_order(const Slic3r::Polyline &source, Slic3r::Polylines &fragments)
{
    const Points &src = source.points;
    if (src.size() < 2 || fragments.empty())
        return;

    std::unordered_map<Point, size_t, PointHash> source_index;
    source_index.reserve(src.size());
    for (size_t i = 0; i < src.size(); ++ i)
        source_index.emplace(src[i], i);

    // Sort key: index of the source vertex where the fragment starts, then the signed
    // offset of the fragment's start from that vertex, to order multiple fragments cut
    // from one long source segment.
    std::vector<std::pair<size_t, double>> keys(fragments.size());
    for (size_t n = 0; n < fragments.size(); ++ n) {
        Polyline    &pl    = fragments[n];
        const size_t npos  = size_t(-1);
        size_t       front = npos;
        size_t       back  = npos;
        for (const Point &pt : pl.points)
            if (auto it = source_index.find(pt); it != source_index.end()) {
                front = it->second;
                break;
            }
        for (auto i = pl.points.rbegin(); i != pl.points.rend(); ++ i)
            if (auto it = source_index.find(*i); it != source_index.end()) {
                back = it->second;
                break;
            }
        Vec2crd source_dir;
        if (front == npos) {
            // All vertices were created by clipping, thus the whole fragment lies on a
            // single source segment. Find that segment.
            double best = std::numeric_limits<double>::max();
            for (size_t i = 0; i + 1 < src.size(); ++ i)
                if (double d = Line::distance_to_squared(pl.first_point(), src[i], src[i + 1]); d < best) {
                    best  = d;
                    front = i;
                }
            back       = front;
            source_dir = src[front + 1] - src[front];
        } else
            source_dir = src[std::min(back + 1, src.size() - 1)] - src[front > 0 ? front - 1 : 0];
        if (front > back) {
            pl.reverse();
            std::swap(front, back);
        } else if (front == back &&
                   (pl.last_point() - pl.first_point()).cast<double>().dot(source_dir.cast<double>()) < 0.)
            pl.reverse();
        const Vec2crd seg = src[std::min(front + 1, src.size() - 1)] - src[front];
        keys[n] = { front, (pl.first_point() - src[front]).cast<double>().dot(seg.cast<double>()) };
    }

    std::vector<size_t> order(fragments.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&keys](size_t a, size_t b) { return keys[a] < keys[b]; });
    Polylines sorted;
    sorted.reserve(fragments.size());
    for (size_t n : order)
        sorted.emplace_back(std::move(fragments[n]));
    fragments = std::move(sorted);
}

Lines _clipper_ln(ClipperLib::ClipType clipType, const Lines &subject, const Polygons &clip)
{
    // convert Lines to Polylines
    Polylines polylines;
    polylines.reserve(subject.size());
    for (const Line &line : subject)
        polylines.emplace_back(Polyline(line.a, line.b));
    
    // perform operation
    polylines = _clipper_pl_open(clipType, ClipperUtils::PolylinesProvider(polylines), ClipperUtils::PolygonsProvider(clip));
    
    // convert Polylines to Lines
    Lines retval;
    for (Polylines::const_iterator polyline = polylines.begin(); polyline != polylines.end(); ++polyline)
        if (polyline->size() >= 2)
            //FIXME It may happen, that Clipper produced a polyline with more than 2 collinear points by clipping a single line with polygons. It is a very rare issue, but it happens, see GH #6933.
            retval.push_back({ polyline->front(), polyline->back() });
    return retval;
}

// Convert polygons / expolygons into ClipperLib::PolyTree using ClipperLib::pftEvenOdd, thus union will NOT be performed.
// If the contours are not intersecting, their orientation shall not be modified by union_pt().
template<typename PathsProvider>
static ClipperLib::PolyTree clipper1_union_pt(PathsProvider &&subject)
{
    ClipperLib::Clipper clipper;
    clipper.AddPaths(std::forward<PathsProvider>(subject), ClipperLib::ptSubject, true);
    ClipperLib::PolyTree out;
    clipper.Execute(ClipperLib::ctUnion, out, ClipperLib::pftEvenOdd, ClipperLib::pftEvenOdd);
    return out;
}
ClipperLib::PolyTree union_pt(const Polygons &subject)
{
    return clipper1_union_pt(ClipperUtils::PolygonsProvider(subject));
}
ClipperLib::PolyTree union_pt(const ExPolygons &subject)
{
    return clipper1_union_pt(ClipperUtils::ExPolygonsProvider(subject));
}

// Simple spatial ordering of Polynodes
ClipperLib::PolyNodes order_nodes(const ClipperLib::PolyNodes &nodes)
{
    // collect ordering points
    Points ordering_points;
    ordering_points.reserve(nodes.size());
    
    for (const ClipperLib::PolyNode *node : nodes)
        ordering_points.emplace_back(
            Point(node->Contour.front().x(), node->Contour.front().y()));

    // perform the ordering
    ClipperLib::PolyNodes ordered_nodes =
        chain_clipper_polynodes(ordering_points, nodes);

    return ordered_nodes;
}

static void traverse_pt_noholes(const ClipperLib::PolyNodes &nodes, Polygons *out)
{
    foreach_node<e_ordering::ON>(nodes, [&out](const ClipperLib::PolyNode *node) 
    {
        traverse_pt_noholes(node->Childs, out);
        out->emplace_back(node->Contour);
        if (node->IsHole()) out->back().reverse(); // ccw
    });
}

static void traverse_pt_outside_in(const C2::PolyPath64 &parent, Polygons *retval)
{
    // collect ordering points
    Points ordering_points;
    ordering_points.reserve(parent.Count());
    for (const auto &node : parent)
        ordering_points.emplace_back(node->Polygon().front().x, node->Polygon().front().y);

    // Perform the ordering, push results recursively.
    //FIXME pass the last point to chain_points?
    for (size_t idx : chain_points(ordering_points)) {
        const C2::PolyPath64 &node = *parent.Child(idx);
        retval->emplace_back().points = c2_to_points(node.Polygon());
        if (node.IsHole())
            // Orient a hole, which is clockwise oriented, to CCW.
            retval->back().reverse();
        // traverse the next depth
        traverse_pt_outside_in(node, retval);
    }
}

Polygons union_pt_chained_outside_in(const Polygons &subject)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::EvenOdd, tree);
    Polygons retval;
    traverse_pt_outside_in(tree, &retval);
    return retval;
}

// Clipper2 unions are strictly simple, like Clipper1's with StrictlySimple(true).
Polygons simplify_polygons(const Polygons &subject)
{
    C2::Paths64 out;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::NonZero, out);
    return c2_to_polygons(out);
}

ExPolygons simplify_polygons_ex(const Polygons &subject)
{
    C2::PolyTree64 out;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(subject)), {}, C2::FillRule::NonZero, out);
    return c2_to_expolygons(out);
}

Polygons top_level_islands(const Slic3r::Polygons &polygons)
{
    C2::PolyTree64 tree;
    c2_clip(C2::ClipType::Union, to_paths64(ClipperUtils::PolygonsProvider(polygons)), {}, C2::FillRule::EvenOdd, tree);
    // Convert only the top level islands to the output.
    Polygons out;
    out.reserve(tree.Count());
    for (const auto &island : tree)
        out.emplace_back().points = c2_to_points(island->Polygon());
    return out;
}

// Outer offset shall not split the input contour into multiples. It is expected, that the solution will be non empty and it will contain just a single polygon.
ClipperLib::Paths fix_after_outer_offset(
	const ClipperLib::Path 		&input, 
													// combination of default prameters to correspond to void ClipperOffset::Execute(Paths& solution, double delta)
													// to produce a CCW output contour from CCW input contour for a positive offset.
	ClipperLib::PolyFillType 	 filltype, 			// = ClipperLib::pftPositive
	bool 						 reverse_result)	// = false
{
  	ClipperLib::Paths solution;
  	if (! input.empty()) {
		ClipperLib::Clipper clipper;
	  	clipper.AddPath(input, ClipperLib::ptSubject, true);
		clipper.ReverseSolution(reverse_result);
		clipper.Execute(ClipperLib::ctUnion, solution, filltype, filltype);
	}
    return solution;
}

// Inner offset may split the source contour into multiple contours, but one resulting contour shall not lie inside the other.
ClipperLib::Paths fix_after_inner_offset(
	const ClipperLib::Path 		&input, 
													// combination of default prameters to correspond to void ClipperOffset::Execute(Paths& solution, double delta)
													// to produce a CCW output contour from CCW input contour for a negative offset.
	ClipperLib::PolyFillType 	 filltype, 			// = ClipperLib::pftNegative
	bool 						 reverse_result) 	// = true
{
  	ClipperLib::Paths solution;
  	if (! input.empty()) {
		ClipperLib::Clipper clipper;
		clipper.AddPath(input, ClipperLib::ptSubject, true);
		ClipperLib::IntRect r = clipper.GetBounds();
		r.left -= 10; r.top -= 10; r.right += 10; r.bottom += 10;
		if (filltype == ClipperLib::pftPositive)
			clipper.AddPath({ ClipperLib::IntPoint(r.left, r.bottom), ClipperLib::IntPoint(r.left, r.top), ClipperLib::IntPoint(r.right, r.top), ClipperLib::IntPoint(r.right, r.bottom) }, ClipperLib::ptSubject, true);
		else
			clipper.AddPath({ ClipperLib::IntPoint(r.left, r.bottom), ClipperLib::IntPoint(r.right, r.bottom), ClipperLib::IntPoint(r.right, r.top), ClipperLib::IntPoint(r.left, r.top) }, ClipperLib::ptSubject, true);
		clipper.ReverseSolution(reverse_result);
		clipper.Execute(ClipperLib::ctUnion, solution, filltype, filltype);
		if (! solution.empty())
			solution.erase(solution.begin());
	}
	return solution;
}

ClipperLib::Path mittered_offset_path_scaled(const Points &contour, const std::vector<float> &deltas, double miter_limit)
{
	assert(contour.size() == deltas.size());

#ifndef NDEBUG
	// Verify that the deltas are either all positive, or all negative.
	bool positive = false;
	bool negative = false;
	for (float delta : deltas)
		if (delta < 0.f)
			negative = true;
		else if (delta > 0.f)
			positive = true;
	assert(! (negative && positive));
#endif /* NDEBUG */

	ClipperLib::Path out;

	if (deltas.size() > 2)
	{
		out.reserve(contour.size() * 2);

		// Clamp miter limit to 2.
		miter_limit = (miter_limit > 2.) ? 2. / (miter_limit * miter_limit) : 0.5;
		
		// perpenduclar vector
		auto   perp = [](const Vec2d &v) -> Vec2d { return Vec2d(v.y(), - v.x()); };

		// Add a new point to the output, scale by CLIPPER_OFFSET_SCALE and round to ClipperLib::cInt.
		auto   add_offset_point = [&out](Vec2d pt) {
            pt += Vec2d(0.5 - (pt.x() < 0), 0.5 - (pt.y() < 0));
			out.emplace_back(ClipperLib::cInt(pt.x()), ClipperLib::cInt(pt.y()));
		};

		// Minimum edge length, squared.
		double lmin  = *std::max_element(deltas.begin(), deltas.end()) * ClipperOffsetShortestEdgeFactor;
		double l2min = lmin * lmin;
		// Minimum angle to consider two edges to be parallel.
		// Vojtech's estimate.
//		const double sin_min_parallel = EPSILON + 1. / double(CLIPPER_OFFSET_SCALE);
		// Implementation equal to Clipper.
		const double sin_min_parallel = 1.;

		// Find the last point further from pt by l2min.
		Vec2d  pt     = contour.front().cast<double>();
		size_t iprev  = contour.size() - 1;
		Vec2d  ptprev;
		for (; iprev > 0; -- iprev) {
			ptprev = contour[iprev].cast<double>();
			if ((ptprev - pt).squaredNorm() > l2min)
				break;
		}

		if (iprev != 0) {
			size_t ilast = iprev;
			// Normal to the (pt - ptprev) segment.
			Vec2d nprev = perp(pt - ptprev).normalized();
			for (size_t i = 0; ; ) {
				// Find the next point further from pt by l2min.
				size_t j = i + 1;
				Vec2d ptnext;
				for (; j <= ilast; ++ j) {
					ptnext = contour[j].cast<double>();
					double l2 = (ptnext - pt).squaredNorm();
					if (l2 > l2min)
						break;
				}
				if (j > ilast) {
					assert(i <= ilast);
					// If the last edge is too short, merge it with the previous edge.
					i = ilast;
					ptnext = contour.front().cast<double>();
				}

				// Normal to the (ptnext - pt) segment.
				Vec2d nnext  = perp(ptnext - pt).normalized();

				double delta  = deltas[i];
				double sin_a  = std::clamp(cross2(nprev, nnext), -1., 1.);
				double convex = sin_a * delta;
				if (convex <= - sin_min_parallel) {
					// Concave corner.
					add_offset_point(pt + nprev * delta);
					add_offset_point(pt);
					add_offset_point(pt + nnext * delta);
				} else {
					double dot = nprev.dot(nnext);
					if (convex < sin_min_parallel && dot > 0.) {
						// Nearly parallel.
						add_offset_point((nprev.dot(nnext) > 0.) ? (pt + nprev * delta) : pt);
					} else {
						// Convex corner, possibly extremely sharp if convex < sin_min_parallel.
						double r = 1. + dot;
					  	if (r >= miter_limit)
							add_offset_point(pt + (nprev + nnext) * (delta / r));
					  	else {
							double dx = std::tan(std::atan2(sin_a, dot) / 4.);
							Vec2d  newpt1 = pt + (nprev - perp(nprev) * dx) * delta;
							Vec2d  newpt2 = pt + (nnext + perp(nnext) * dx) * delta;
#ifndef NDEBUG
							Vec2d vedge = 0.5 * (newpt1 + newpt2) - pt;
							double dist_norm = vedge.norm();
							assert(std::abs(dist_norm - std::abs(delta)) < SCALED_EPSILON);
#endif /* NDEBUG */
							add_offset_point(newpt1);
							add_offset_point(newpt2);
					  	}
					}
				}

				if (i == ilast)
					break;

				ptprev = pt;
				nprev  = nnext;
				pt     = ptnext;
				i = j;
			}
		}
	}

#if 0
	{
		ClipperLib::Path polytmp(out);
		unscaleClipperPolygon(polytmp);
		Slic3r::Polygon offsetted(std::move(polytmp));
		BoundingBox bbox = get_extents(contour);
		bbox.merge(get_extents(offsetted));
		static int iRun = 0;
		SVG svg(debug_out_path("mittered_offset_path_scaled-%d.svg", iRun ++).c_str(), bbox);
		svg.draw_outline(Polygon(contour), "blue", scale_(0.01));
		svg.draw_outline(offsetted, "red", scale_(0.01));
		svg.draw(contour, "blue", scale_(0.03));
		svg.draw((Points)offsetted, "blue", scale_(0.03));
	}
#endif

	return out;
}

Polygons variable_offset_inner(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
	for (const std::vector<float> &ds : deltas)
		for (float delta : ds)
			assert(delta <= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_inner_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftNegative, true);
#ifndef NDEBUG	
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_outer_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftNegative, false));
#ifndef NDEBUG	
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ClipperLib::Paths output;
	if (holes.empty())
		output = std::move(contours);
	else {
		ClipperLib::Clipper clipper;
		clipper.Clear();
		clipper.AddPaths(contours, ClipperLib::ptSubject, true);
		clipper.AddPaths(holes, ClipperLib::ptClip, true);
		clipper.Execute(ClipperLib::ctDifference, output, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
	}

	return to_polygons(std::move(output));
}

Polygons variable_offset_outer(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta >= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_outer_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftPositive, false);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_inner_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftPositive, true));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ClipperLib::Paths output;
	if (holes.empty())
		output = std::move(contours);
	else {
		ClipperLib::Clipper clipper;
		clipper.Clear();
		clipper.AddPaths(contours, ClipperLib::ptSubject, true);
		clipper.AddPaths(holes, ClipperLib::ptClip, true);
		clipper.Execute(ClipperLib::ctDifference, output, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
	}

	return to_polygons(std::move(output));
}

ExPolygons variable_offset_outer_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta >= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_outer_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftPositive, false);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_inner_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftPositive, true));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ExPolygons output;
	if (holes.empty()) {
		output.reserve(contours.size());
		for (ClipperLib::Path &path : contours) 
			output.emplace_back(std::move(path));
	} else {
		ClipperLib::Clipper clipper;
		clipper.AddPaths(contours, ClipperLib::ptSubject, true);
		clipper.AddPaths(holes, ClipperLib::ptClip, true);
	    ClipperLib::PolyTree polytree;
		clipper.Execute(ClipperLib::ctDifference, polytree, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
	    output = PolyTreeToExPolygons(std::move(polytree));
	}

	return output;
}


ExPolygons variable_offset_inner_ex(const ExPolygon &expoly, const std::vector<std::vector<float>> &deltas, double miter_limit)
{
#ifndef NDEBUG
	// Verify that the deltas are all non positive.
	for (const std::vector<float>& ds : deltas)
		for (float delta : ds)
			assert(delta <= 0.);
	assert(expoly.holes.size() + 1 == deltas.size());
#endif /* NDEBUG */

	// 1) Offset the outer contour.
	ClipperLib::Paths contours = fix_after_inner_offset(mittered_offset_path_scaled(expoly.contour.points, deltas.front(), miter_limit), ClipperLib::pftNegative, true);
#ifndef NDEBUG
	for (auto &c : contours)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 2) Offset the holes one by one, collect the results.
	ClipperLib::Paths holes;
	holes.reserve(expoly.holes.size());
	for (const Polygon& hole : expoly.holes)
		append(holes, fix_after_outer_offset(mittered_offset_path_scaled(hole.points, deltas[1 + &hole - expoly.holes.data()], miter_limit), ClipperLib::pftNegative, false));
#ifndef NDEBUG
	for (auto &c : holes)
		assert(ClipperLib::Area(c) > 0.);
#endif /* NDEBUG */

	// 3) Subtract holes from the contours.
	ExPolygons output;
	if (holes.empty()) {
		output.reserve(contours.size());
		for (ClipperLib::Path &path : contours) 
			output.emplace_back(std::move(path));
	} else {
		ClipperLib::Clipper clipper;
		clipper.AddPaths(contours, ClipperLib::ptSubject, true);
		clipper.AddPaths(holes, ClipperLib::ptClip, true);
	    ClipperLib::PolyTree polytree;
		clipper.Execute(ClipperLib::ctDifference, polytree, ClipperLib::pftNonZero, ClipperLib::pftNonZero);
	    output = PolyTreeToExPolygons(std::move(polytree));
	}

	return output;
}

Pointfs make_counter_clockwise(const Pointfs& pointfs)
{
    Pointfs ps = pointfs;
    if (Polygon::new_scale(pointfs).is_clockwise()) {
        std::reverse(ps.begin(), ps.end());
    }

    return ps;
}

}
