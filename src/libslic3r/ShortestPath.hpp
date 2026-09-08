#ifndef slic3r_ShortestPath_hpp_
#define slic3r_ShortestPath_hpp_

#include "libslic3r.h"
#include "ExtrusionEntity.hpp"
#include "Point.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace Slic3r {

	namespace ClipperLib {
		class PolyNode;
		using PolyNodes = std::vector<PolyNode*, PointsAllocator<PolyNode*>>;
	}

std::vector<size_t> 				 chain_points(const Points &points, Point *start_near = nullptr);
std::vector<size_t> 				 chain_expolygons(const ExPolygons &input_exploy);

std::vector<std::pair<size_t, bool>> chain_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const Point *start_near = nullptr);
void                                 reorder_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const std::vector<std::pair<size_t, bool>> &chain);
void                                 chain_and_reorder_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const Point &start_near);
void                                 chain_and_reorder_extrusion_entities(std::vector<ExtrusionEntity*> &entities, const Point *start_near = nullptr);

std::vector<std::pair<size_t, bool>> chain_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const Point *start_near = nullptr);
void                                 reorder_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, std::vector<std::pair<size_t, bool>> &chain);
void                                 chain_and_reorder_extrusion_paths(std::vector<ExtrusionPath> &extrusion_paths, const Point *start_near = nullptr);

// Result of an exact single-chain ordering of extrusion entities for zero-travel continuous printing.
// Unlike chain_extrusion_entities(), which minimizes travel but tolerates gaps between segments,
// this ordering only succeeds if every entity connects to the next one end-to-start with no gap,
// i.e. the whole set forms a single continuous trace (an Eulerian trail, open or closed).
struct ExactChainResult {
    // (index into the input entities, true if the entity must be reversed).
    std::vector<std::pair<size_t, bool>> order;
    Point start;   // First point of the chain.
    Point end;     // Last point of the chain. Coincides with start when closed.
    bool  closed = false; // True if the chain forms a closed loop (start == end).
};

// Order extrusion entities into a single continuous trace with zero travel:
// the start point of each successive entity must coincide (within SCALED_EPSILON) with
// the end point of the previous one. Never creates any connecting segments.
// Returns std::nullopt if the entities cannot form such a trace
// (more than 2 odd-degree endpoints, disconnected graph, missing endpoints,
// or a required reversal forbidden by can_reverse()).
// If preferred_start is given, the chain starts from the valid endpoint closest to it.
std::optional<ExactChainResult>  chain_extrusion_entities_exact(const std::vector<ExtrusionEntity*> &entities, const Point *preferred_start = nullptr);

Polylines 							 chain_polylines(Polylines &&src, const Point *start_near = nullptr);
inline Polylines 					 chain_polylines(const Polylines& src, const Point* start_near = nullptr) { Polylines tmp(src); return chain_polylines(std::move(tmp), start_near); }
template<typename T> inline void reorder_by_shortest_traverse(std::vector<T> &polylines_out)
{
    Points start_point;
    start_point.reserve(polylines_out.size());
    for (const T& contour : polylines_out) start_point.push_back(contour.points.front());

    std::vector<Points::size_type> order = chain_points(start_point);

    std::vector<T> Temp = polylines_out;
    polylines_out.erase(polylines_out.begin(), polylines_out.end());

    for (size_t i:order) polylines_out.emplace_back(std::move(Temp[i]));
}

ClipperLib::PolyNodes				 chain_clipper_polynodes(const Points &points, const ClipperLib::PolyNodes &items);

// Chain instances of print objects by an approximate shortest path.
// Returns pairs of PrintObject idx and instance of that PrintObject.
class Print;
struct PrintInstance;
// BBS
class PrintObject;
std::vector<const PrintInstance*> chain_print_object_instances(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> 	 chain_print_object_instances(const Print &print);

// Chain lines into polylines.
Polylines 							 chain_lines(const std::vector<Line> &lines, const double point_distance_epsilon);

} // namespace Slic3r

#endif /* slic3r_ShortestPath_hpp_ */
