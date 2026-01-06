#include "Clipper2Utils.hpp"
#include "libslic3r.h"
#include "clipper2/clipper.h"

namespace Slic3r {

//BBS: FIXME
Slic3r::Polylines Paths64_to_polylines(const Clipper2Lib::Paths64& in)
{
    Slic3r::Polylines out;
    out.reserve(in.size());
    for (const Clipper2Lib::Path64& path64 : in) {
        Slic3r::Points points;
        points.reserve(path64.size());
        for (const Clipper2Lib::Point64& point64 : path64)
            points.emplace_back(std::move(Slic3r::Point(point64.x, point64.y)));
        out.emplace_back(std::move(Slic3r::Polyline(points)));
    }
    return out;
}

//BBS: FIXME
template <typename Container>
Clipper2Lib::Paths64 Slic3rPoints_to_Paths64(const Container& in)
{
    Clipper2Lib::Paths64 out;
    out.reserve(in.size());
    for (const auto& item : in) {
        Clipper2Lib::Path64 path;
        path.reserve(item.size());
        for (const Slic3r::Point& point : item.points)
            path.emplace_back(std::move(Clipper2Lib::Point64(point.x(), point.y())));
        out.emplace_back(std::move(path));
    }
    return out;
}

Clipper2Lib::Paths64 Slic3rPolylines_to_Paths64(const Polylines& in)
{
    return Slic3rPoints_to_Paths64(in);
}

Points Path64ToPoints(const Clipper2Lib::Path64& path64)
{
    Points points;
    points.reserve(path64.size());
    for (const Clipper2Lib::Point64 &point64 : path64) points.emplace_back(std::move(Slic3r::Point(point64.x, point64.y)));
    return points;
}

static ExPolygons PolyTreeToExPolygons(Clipper2Lib::PolyTree64 &&polytree)
{
    struct Inner
    {
        static void PolyTreeToExPolygonsRecursive(Clipper2Lib::PolyTree64 &&polynode, ExPolygons *expolygons)
        {
            size_t cnt = expolygons->size();
            expolygons->resize(cnt + 1);
            (*expolygons)[cnt].contour.points = Path64ToPoints(polynode.Polygon());

            (*expolygons)[cnt].holes.resize(polynode.Count());
            for (int i = 0; i < polynode.Count(); ++i) {
                (*expolygons)[cnt].holes[i].points = Path64ToPoints(polynode[i]->Polygon());
                // Add outer polygons contained by (nested within) holes.
                for (int j = 0; j < polynode[i]->Count(); ++j) PolyTreeToExPolygonsRecursive(std::move(*polynode[i]->Child(j)), expolygons);
            }
        }

        static size_t PolyTreeCountExPolygons(const Clipper2Lib::PolyPath64& polynode)
        {
            size_t cnt = 1;
            for (size_t i = 0; i < polynode.Count(); ++i) {
                for (size_t j = 0; j < polynode.Child(i)->Count(); ++j) cnt += PolyTreeCountExPolygons(*polynode.Child(i)->Child(j));
            }
            return cnt;
        }
    };

    ExPolygons retval;
    size_t     cnt = 0;
    for (int i = 0; i < polytree.Count(); ++i) cnt += Inner::PolyTreeCountExPolygons(*polytree[i]);
    retval.reserve(cnt);
    for (int i = 0; i < polytree.Count(); ++i) Inner::PolyTreeToExPolygonsRecursive(std::move(*polytree[i]), &retval);
    return retval;
}

void SimplifyPolyTree(const Clipper2Lib::PolyPath64 &polytree, double epsilon, Clipper2Lib::PolyPath64 &result)
{
    for (const auto &child : polytree) {
        Clipper2Lib::PolyPath64 *newchild = result.AddChild(Clipper2Lib::SimplifyPath(child->Polygon(), epsilon));
        SimplifyPolyTree(*child, epsilon, *newchild);
    }
}

Clipper2Lib::Paths64 Slic3rPolygons_to_Paths64(const Polygons &in)
{
    Clipper2Lib::Paths64 out;
    out.reserve(in.size());
    for (const Polygon &poly : in) {
        Clipper2Lib::Path64 path;
        path.reserve(poly.points.size());
        for (const Slic3r::Point &point : poly.points) path.emplace_back(std::move(Clipper2Lib::Point64(point.x(), point.y())));
        out.emplace_back(std::move(path));
    }
    return out;
}

Clipper2Lib::Paths64 Slic3rExPolygons_to_Paths64(const ExPolygons& in)
{
    Clipper2Lib::Paths64 out;
    out.reserve(in.size());
    for (const ExPolygon& expolygon : in) {
        for (size_t i = 0; i < expolygon.num_contours(); i++) {
            const auto         &poly = expolygon.contour_or_hole(i);
            Clipper2Lib::Path64 path;
            path.reserve(poly.points.size());
            for (const Slic3r::Point &point : poly.points) path.emplace_back(std::move(Clipper2Lib::Point64(point.x(), point.y())));
            out.emplace_back(std::move(path));
        }
    }
    return out;
}

Polylines _clipper2_pl_open(Clipper2Lib::ClipType clipType, const Slic3r::Polylines& subject, const Slic3r::Polygons& clip)
{
    Clipper2Lib::Clipper64 c;
    c.AddOpenSubject(Slic3rPoints_to_Paths64(subject));
    c.AddClip(Slic3rPoints_to_Paths64(clip));

    Clipper2Lib::ClipType ct = clipType;
    Clipper2Lib::FillRule fr = Clipper2Lib::FillRule::NonZero;
    Clipper2Lib::Paths64 solution, solution_open;
    c.Execute(ct, fr, solution, solution_open);

    Slic3r::Polylines out;
    out.reserve(solution.size() + solution_open.size());
    polylines_append(out, std::move(Paths64_to_polylines(solution)));
    polylines_append(out, std::move(Paths64_to_polylines(solution_open)));

    return out;
}

Slic3r::Polylines intersection_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip)
    { return _clipper2_pl_open(Clipper2Lib::ClipType::Intersection, subject, clip); }
Slic3r::Polylines  diff_pl_2(const Slic3r::Polylines& subject, const Slic3r::Polygons& clip)
    { return _clipper2_pl_open(Clipper2Lib::ClipType::Difference, subject, clip); }

ExPolygons union_ex_2(const Polygons& polygons)
{
    Clipper2Lib::Clipper64 c;
    c.AddSubject(Slic3rPolygons_to_Paths64(polygons));

    Clipper2Lib::ClipType ct = Clipper2Lib::ClipType::Union;
    Clipper2Lib::FillRule fr = Clipper2Lib::FillRule::NonZero;
    Clipper2Lib::PolyTree64 solution;
    c.Execute(ct, fr, solution);

    ExPolygons results = PolyTreeToExPolygons(std::move(solution));

    return results;
}

ExPolygons union_ex_2(const ExPolygons &expolygons)
{
    Clipper2Lib::Clipper64 c;
    c.AddSubject(Slic3rExPolygons_to_Paths64(expolygons));

    Clipper2Lib::ClipType   ct = Clipper2Lib::ClipType::Union;
    Clipper2Lib::FillRule   fr = Clipper2Lib::FillRule::NonZero;
    Clipper2Lib::PolyTree64 solution;
    c.Execute(ct, fr, solution);

    ExPolygons results = PolyTreeToExPolygons(std::move(solution));

    return results;
}

// 对 ExPolygons 进行偏移
ExPolygons offset_ex_2(const ExPolygons &expolygons, double delta)
{
    Clipper2Lib::Paths64 subject = Slic3rExPolygons_to_Paths64(expolygons);
    Clipper2Lib::ClipperOffset offsetter;
    offsetter.AddPaths(subject, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon);
    Clipper2Lib::PolyPath64 polytree;
    offsetter.Execute(delta, polytree);
    ExPolygons results = PolyTreeToExPolygons(std::move(polytree));

    return results;
}

ExPolygons offset2_ex_2(const ExPolygons& expolygons, double delta1, double delta2)
{
    // 1st offset
    Clipper2Lib::Paths64       subject = Slic3rExPolygons_to_Paths64(expolygons);
    Clipper2Lib::ClipperOffset offsetter;
    offsetter.AddPaths(subject, Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon);
    Clipper2Lib::PolyPath64 polytree;
    offsetter.Execute(delta1, polytree);

    // simplify the result
    Clipper2Lib::PolyPath64 polytree2;
    SimplifyPolyTree(polytree, SCALED_EPSILON, polytree2);

    // 2nd offset
    offsetter.Clear();
    offsetter.AddPaths(Clipper2Lib::PolyTreeToPaths64(polytree2), Clipper2Lib::JoinType::Round, Clipper2Lib::EndType::Polygon);
    polytree.Clear();
    offsetter.Execute(delta2, polytree);

    // convert back to expolygons
    ExPolygons results = PolyTreeToExPolygons(std::move(polytree));

    return results;
}

}