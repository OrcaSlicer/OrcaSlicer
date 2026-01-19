#include "Triangulation.hpp"
#include "IntersectionPoints.hpp"
#include <boost/next_prior.hpp>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>
#include <CGAL/Triangulation_vertex_base_with_info_2.h>
#include <CGAL/spatial_sort.h>

using namespace Slic3r;
namespace priv{
inline void insert_edges(Triangulation::HalfEdges &edges, uint32_t &offset, const Polygon &polygon, const Triangulation::Changes& changes) {
    const Points &pts = polygon.points;
    uint32_t size = static_cast<uint32_t>(pts.size());
    uint32_t last_index = offset + size - 1;
    uint32_t prev_index = changes[last_index];
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t index = changes[offset + i];
        // when duplicit points are neighbor
        if (prev_index == index) continue; 
        edges.push_back({prev_index, index});
        prev_index = index;
    }
    offset += size;
}

inline void insert_edges(Triangulation::HalfEdges &edges, uint32_t &offset, const Polygon &polygon) {
    const Points &pts = polygon.points;
    uint32_t size = static_cast<uint32_t>(pts.size());
    uint32_t prev_index = offset + size - 1;
    for (uint32_t i = 0; i < size; ++i) {
        uint32_t index = offset + i;
        edges.push_back({prev_index, index});
        prev_index = index;
    }
    offset += size;
}

inline bool has_bidirectional_constrained(
    const Triangulation::HalfEdges &constrained)
{
    for (const auto &c : constrained) {
        auto key = std::make_pair(c.second, c.first);
        auto it  = std::lower_bound(constrained.begin(), constrained.end(),
                                    key);
        if (it != constrained.end() && *it == key) return true;
    }
    return false;
}

inline bool is_unique(const Points &points) { 
    Points pts = points; // copy
    std::sort(pts.begin(), pts.end());
    auto it = std::adjacent_find(pts.begin(), pts.end());
    return it == pts.end();
}

inline bool has_self_intersection(
    const Points                   &points,
    const Triangulation::HalfEdges &constrained_half_edges)
{
    Lines lines;
    lines.reserve(constrained_half_edges.size());
    for (const auto &he : constrained_half_edges)
        lines.emplace_back(points[he.first], points[he.second]);
    return !get_intersections(lines).empty();
}

} // namespace priv

//#define VISUALIZE_TRIANGULATION
#ifdef VISUALIZE_TRIANGULATION
#include "admesh/stl.h" // indexed triangle set
static void visualize(const Points                 &points,
               const Triangulation::Indices &indices,
               const char                   *filename)
{
    // visualize
    indexed_triangle_set its;
    its.vertices.reserve(points.size());
    for (const Point &p : points) its.vertices.emplace_back(p.x(), p.y(), 0.);
    its.indices = indices;
    its_write_obj(its, filename);
}
#endif // VISUALIZE_TRIANGULATION

Triangulation::Indices Triangulation::triangulate(const Points    &points,
                                                  const HalfEdges &constrained_half_edges)
{
    assert(!points.empty());
    assert(!constrained_half_edges.empty());
    // constrained must be sorted
    assert(std::is_sorted(constrained_half_edges.begin(),
                          constrained_half_edges.end()));
    // check that there is no duplicit constrained edge
    assert(std::adjacent_find(constrained_half_edges.begin(), constrained_half_edges.end()) == constrained_half_edges.end());
    // edges can NOT contain bidirectional constrained
    assert(!priv::has_bidirectional_constrained(constrained_half_edges));
    // check that there is only unique poistion of points
    assert(priv::is_unique(points));
    assert(!priv::has_self_intersection(points, constrained_half_edges));
    // use cgal triangulation
    using K   = CGAL::Exact_predicates_inexact_constructions_kernel;
    using Vb  = CGAL::Triangulation_vertex_base_with_info_2<uint32_t, K>;
    using Fb  = CGAL::Constrained_triangulation_face_base_2<K>;
    using Tds = CGAL::Triangulation_data_structure_2<Vb, Fb>;
    using CDT = CGAL::Constrained_Delaunay_triangulation_2<K, Tds, CGAL::Exact_predicates_tag>;

    // construct a constrained triangulation
    CDT cdt;
    {
        std::vector<CDT::Vertex_handle> vertices_handle(points.size()); // for constriants
        using Point_with_ord = std::pair<CDT::Point, size_t>;
        using SearchTrait    = CGAL::Spatial_sort_traits_adapter_2
            <K, CGAL::First_of_pair_property_map<Point_with_ord> >;

        std::vector<Point_with_ord> cdt_points;
        cdt_points.reserve(points.size());
        size_t ord = 0;
        for (const auto &p : points)
            cdt_points.emplace_back(std::make_pair(CDT::Point{p.x(), p.y()}, ord++));
        
        SearchTrait st;
        CGAL::spatial_sort(cdt_points.begin(), cdt_points.end(), st);
        CDT::Face_handle f;
        for (const auto& p : cdt_points) {
            auto handle = cdt.insert(p.first, f);
            handle->info() = p.second;
            vertices_handle[p.second] = handle;
            f = handle->face();
        }

        // Constrain the triangulation.
        for (const HalfEdge &edge : constrained_half_edges)
            cdt.insert_constraint(vertices_handle[edge.first], vertices_handle[edge.second]);
    }

    auto faces = cdt.finite_face_handles();

    // Unmark constrained edges of outside faces.
    size_t num_faces = 0;
    for (CDT::Face_handle fh : faces) {
        for (int i = 0; i < 3; ++i) {
            if (!fh->is_constrained(i)) continue;
            auto key = std::make_pair(fh->vertex((i + 2) % 3)->info(), fh->vertex((i + 1) % 3)->info());
            auto it = std::lower_bound(constrained_half_edges.begin(), constrained_half_edges.end(), key);
            if (it == constrained_half_edges.end() || *it != key) continue;
            // This face contains a constrained edge and it is outside.
            for (int j = 0; j < 3; ++ j)
                fh->set_constraint(j, false);
            --num_faces;
            break;            
        }
        ++num_faces;
    }

    auto inside = [](CDT::Face_handle &fh) { 
        return fh->neighbor(0) != fh && 
               (fh->is_constrained(0) ||
                fh->is_constrained(1) ||
                fh->is_constrained(2)); 
    };

#ifdef VISUALIZE_TRIANGULATION
    std::vector<Vec3i32> indices2;
    indices2.reserve(num_faces);
    for (CDT::Face_handle fh : faces)
        if (inside(fh)) indices2.emplace_back(fh->vertex(0)->info(), fh->vertex(1)->info(), fh->vertex(2)->info());
    visualize(points, indices2, "C:/data/temp/triangulation_without_floodfill.obj");
#endif // VISUALIZE_TRIANGULATION

    // Propagate inside the constrained regions.
    std::vector<CDT::Face_handle> queue;
    queue.reserve(num_faces);
    for (CDT::Face_handle seed : faces){
        if (!inside(seed)) continue;    
        // Seed fill to neighbor faces.
        queue.emplace_back(seed);
        while (! queue.empty()) {
            CDT::Face_handle fh = queue.back();
            queue.pop_back();
            for (int i = 0; i < 3; ++i) {
                if (fh->is_constrained(i)) continue;
                // Propagate along this edge.
                fh->set_constraint(i, true);
                CDT::Face_handle nh = fh->neighbor(i);
                bool was_inside = inside(nh);
                // Mark the other side of this edge.
                nh->set_constraint(nh->index(fh), true);
                if (! was_inside)
                    queue.push_back(nh);
            }
        }
    }

    std::vector<Vec3i32> indices;
    indices.reserve(num_faces);
    for (CDT::Face_handle fh : faces)
        if (inside(fh))
            indices.emplace_back(fh->vertex(0)->info(), fh->vertex(1)->info(), fh->vertex(2)->info());

#ifdef VISUALIZE_TRIANGULATION
    visualize(points, indices, "C:/data/temp/triangulation.obj");
#endif // VISUALIZE_TRIANGULATION

    return indices;
}

Triangulation::Indices Triangulation::triangulate(const Polygon &polygon)
{
    const Points &pts = polygon.points;
    HalfEdges edges;
    edges.reserve(pts.size());
    uint32_t offset = 0;
    priv::insert_edges(edges, offset, polygon);
    std::sort(edges.begin(), edges.end());
    return triangulate(pts, edges);
}

Triangulation::Indices Triangulation::triangulate(const Polygons &polygons)
{
    size_t count = count_points(polygons);
    Points points;
    points.reserve(count);

    HalfEdges edges;
    edges.reserve(count);
    uint32_t  offset = 0;

    for (const Polygon &polygon : polygons) {
        Slic3r::append(points, polygon.points);
        priv::insert_edges(edges, offset, polygon);
    }

    std::sort(edges.begin(), edges.end());
    return triangulate(points, edges);
}

Triangulation::Indices Triangulation::triangulate(const ExPolygon &expolygon){
    ExPolygons expolys({expolygon}); 
    return triangulate(expolys);
}

Triangulation::Indices Triangulation::triangulate(const ExPolygons &expolygons){
    Points pts = to_points(expolygons);
    Points d_pts = collect_duplicates(pts);
    if (d_pts.empty()) return triangulate(expolygons, pts);

    Changes changes = create_changes(pts, d_pts);
    Indices indices = triangulate(expolygons, pts, changes);
    // reverse map for changes
    Changes changes2(changes.size(), std::numeric_limits<uint32_t>::max());
    for (size_t i = 0; i < changes.size(); ++i)
        changes2[changes[i]] = i;

    // convert indices into expolygons indicies
    for (Vec3i32 &t : indices) 
        for (size_t ti = 0; ti < 3; ti++) t[ti] = changes2[t[ti]];
    
    return indices;
}

Triangulation::Indices Triangulation::triangulate(const ExPolygons &expolygons, const Points &points)
{
    assert(count_points(expolygons) == points.size());
    // when contain duplicit coordinate in points will not work properly
    assert(collect_duplicates(points).empty());

    HalfEdges edges;
    edges.reserve(points.size());
    uint32_t offset = 0;
    for (const ExPolygon &expolygon : expolygons) {
        priv::insert_edges(edges, offset, expolygon.contour);
        for (const Polygon &hole : expolygon.holes)
            priv::insert_edges(edges, offset, hole);
    }
    std::sort(edges.begin(), edges.end());
    return triangulate(points, edges);
}

Triangulation::Indices Triangulation::triangulate(const ExPolygons &expolygons, const Points& points, const Changes& changes)
{
    assert(!points.empty());
    assert(count_points(expolygons) == points.size());
    assert(changes.size() == points.size());
    // IMPROVE: search from end and somehow distiquish that value is not a change
    uint32_t count_points = *std::max_element(changes.begin(), changes.end())+1;
    Points pts(count_points);
    for (size_t i = 0; i < changes.size(); i++)
        pts[changes[i]] = points[i];    

    HalfEdges edges;
    edges.reserve(points.size());
    uint32_t offset = 0;
    for (const ExPolygon &expolygon : expolygons) {
        priv::insert_edges(edges, offset, expolygon.contour, changes);
        for (const Polygon &hole : expolygon.holes)
            priv::insert_edges(edges, offset, hole, changes);
    }

    std::sort(edges.begin(), edges.end());
    return triangulate(pts, edges);
}

Triangulation::Changes Triangulation::create_changes(const Points &points, const Points &duplicits)
{
    assert(!duplicits.empty());
    assert(duplicits.size() < points.size()/2);
    std::vector<uint32_t> duplicit_indices(duplicits.size(), std::numeric_limits<uint32_t>::max());
    Changes changes; 
    changes.reserve(points.size());
    uint32_t index = 0;
    for (const Point &p: points) {
        auto it = std::lower_bound(duplicits.begin(), duplicits.end(), p);
        if (it == duplicits.end() || *it != p) { 
            changes.push_back(index);
            ++index;
            continue;
        }
        uint32_t &d_index = duplicit_indices[it - duplicits.begin()];
        if (d_index == std::numeric_limits<uint32_t>::max()) {
            d_index = index;
            changes.push_back(index);
            ++index;
        } else {
            changes.push_back(d_index);
        }
    }
    return changes;
}
