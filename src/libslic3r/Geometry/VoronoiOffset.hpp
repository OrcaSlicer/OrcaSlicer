// Polygon offsetting using Voronoi diagram prodiced by boost::polygon.

#ifndef slic3r_VoronoiOffset_hpp_
#define slic3r_VoronoiOffset_hpp_

#include <boost/polygon/polygon.hpp>
#include <cmath>
#include <vector>

#include "libslic3r/libslic3r.h"
#include "Voronoi.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {

namespace Voronoi {

using VD = Slic3r::Geometry::VoronoiDiagram;

inline const Point& contour_point(const VD::cell_type &cell, const Line &line)
	{ return ((cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line.a : line.b); }
inline Point&       contour_point(const VD::cell_type &cell, Line &line)
	{ return ((cell.source_category() == boost::polygon::SOURCE_CATEGORY_SEGMENT_START_POINT) ? line.a : line.b); }

inline const Point& contour_point(const VD::cell_type &cell, const Lines &lines)
	{ return contour_point(cell, lines[cell.source_index()]); }
inline Point&       contour_point(const VD::cell_type &cell, Lines &lines)
	{ return contour_point(cell, lines[cell.source_index()]); }

inline Vec2d 		vertex_point(const VD::vertex_type &v) { return Vec2d(v.x(), v.y()); }
inline Vec2d 		vertex_point(const VD::vertex_type *v) { return Vec2d(v->x(), v->y()); }

// "Color" stored inside the boost::polygon Voronoi vertex.
enum class VertexCategory : unsigned char
{
	// Voronoi vertex is on the input contour.
	// VD::vertex_type stores coordinates in double, though the coordinates shall match exactly
	// with the coordinates of the input contour when converted to int32_t.
	OnContour,
	// Vertex is inside the CCW input contour, holes are respected.
	Inside,
	// Vertex is outside the CCW input contour, holes are respected.
	Outside,
	// Not known yet.
	Unknown,
};

// "Color" stored inside the boost::polygon Voronoi edge.
// The Voronoi edge as represented by boost::polygon Voronoi module is really a half-edge,
// the half-edges are classified based on the target vertex (VD::vertex_type::vertex1())
enum class EdgeCategory : unsigned char
{
	// This half-edge points onto the contour, this VD::edge_type::vertex1().color() is OnContour.
	PointsToContour,
	// This half-edge points inside, this VD::edge_type::vertex1().color() is Inside.
	PointsInside,
	// This half-edge points outside, this VD::edge_type::vertex1().color() is Outside.
	PointsOutside,
	// Not known yet.
	Unknown
};

// "Color" stored inside the boost::polygon Voronoi cell.
enum class CellCategory : unsigned char
{
	// This Voronoi cell is split by an input segment to two halves, one is inside, the other is outside.
	Boundary,
	// This Voronoi cell is completely inside.
	Inside,
	// This Voronoi cell is completely outside.
	Outside,
	// Not known yet.
	Unknown
};

inline VertexCategory 	vertex_category(const VD::vertex_type &v)
	{ return static_cast<VertexCategory>(v.color()); }
inline VertexCategory 	vertex_category(const VD::vertex_type *v)
	{ return static_cast<VertexCategory>(v->color()); }
inline void 		  	set_vertex_category(VD::vertex_type &v, VertexCategory c)
	{ v.color(static_cast<VD::vertex_type::color_type>(c)); }
inline void 		  	set_vertex_category(VD::vertex_type *v, VertexCategory c)
	{ v->color(static_cast<VD::vertex_type::color_type>(c)); }

inline EdgeCategory 	edge_category(const VD::edge_type &e)
	{ return static_cast<EdgeCategory>(e.color()); }
inline EdgeCategory 	edge_category(const VD::edge_type *e)
	{ return static_cast<EdgeCategory>(e->color()); }
inline void 			set_edge_category(VD::edge_type &e, EdgeCategory c)
	{ e.color(static_cast<VD::edge_type::color_type>(c)); }
inline void 			set_edge_category(VD::edge_type *e, EdgeCategory c)
	{ e->color(static_cast<VD::edge_type::color_type>(c)); }

inline CellCategory   	cell_category(const VD::cell_type &v)
	{ return static_cast<CellCategory>(v.color()); }
inline CellCategory   	cell_category(const VD::cell_type *v)
	{ return static_cast<CellCategory>(v->color()); }
inline void 		  	set_cell_category(const VD::cell_type &v, CellCategory c)
	{ v.color(static_cast<VD::cell_type::color_type>(c)); }
inline void 		  	set_cell_category(const VD::cell_type *v, CellCategory c)
	{ v->color(static_cast<VD::cell_type::color_type>(c)); }

// Mark the "Color" of VD vertices, edges and cells as Unknown.
void reset_inside_outside_annotations(VD &vd);

// Assign "Color" to VD vertices, edges and cells signifying whether the entity is inside or outside
// the input polygons defined by Lines.
void annotate_inside_outside(VD &vd, const Lines &lines);

// Returns a signed distance to Voronoi vertices from the input polygons.
// (negative distances inside, positive distances outside).
std::vector<double> signed_vertex_distances(const VD &vd, const Lines &lines);

static inline bool edge_offset_no_intersection(const Vec2d &intersection_point)
	{ return std::isnan(intersection_point.x()); }
static inline bool edge_offset_has_intersection(const Vec2d &intersection_point)
	{ return ! edge_offset_no_intersection(intersection_point); }
std::vector<Vec2d> edge_offset_contour_intersections(
	const VD &vd, const Lines &lines, const std::vector<double> &distances,
	double offset_distance);

std::vector<Vec2d> skeleton_edges_rough(
    const VD                    &vd,
    const Lines                 &lines,
    const double                 threshold_alpha);

Polygons offset(
    const Geometry::VoronoiDiagram  &vd,
    const Lines                     &lines,
    const std::vector<double>       &signed_vertex_distances,
    double                           offset_distance,
    double                           discretization_error);

// Offset a polygon or a set of polygons possibly with holes by traversing a Voronoi diagram.
// The input polygons are stored in lines and lines are referenced by vd.
// Outer curve will be extracted for a positive offset_distance,
// inner curve will be extracted for a negative offset_distance.
// Circular arches will be discretized to achieve discretization_error.
Polygons offset(
	const VD 		&vd, 
	const Lines 	&lines, 
	double 			 offset_distance, 
	double 			 discretization_error);

} // namespace Voronoi

} // namespace Slic3r

#endif // slic3r_VoronoiOffset_hpp_
