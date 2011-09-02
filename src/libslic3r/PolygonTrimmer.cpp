#include "PolygonTrimmer.hpp"
#include "EdgeGrid.hpp"
#include "Geometry.hpp"

namespace Slic3r {

TrimmedLoop trim_loop(const Polygon &loop, const EdgeGrid::Grid &grid)
{
	assert(! loop.empty());
	assert(loop.size() >= 2);

	TrimmedLoop out;

	if (loop.size() >= 2) {

		struct Visitor {
			Visitor(const EdgeGrid::Grid &grid, const Slic3r::Point *pt_prev, const Slic3r::Point *pt_this) : grid(grid), pt_prev(pt_prev), pt_this(pt_this) {}

			bool operator()(coord_t iy, coord_t ix) {
				// Called with a row and colum of the grid cell, which is intersected by a line.
				auto cell_data_range = grid.cell_data_range(iy, ix);
				for (auto it_contour_and_segment = cell_data_range.first; it_contour_and_segment != cell_data_range.second; ++ it_contour_and_segment) {
					// End points of the line segment and their vector.
					auto segment = grid.segment(*it_contour_and_segment);
					if (Geometry::segments_intersect(segment.first, segment.second, *pt_prev, *pt_this)) {
						// The two segments intersect. Add them to the output.
					}
				}
				// Continue traversing the grid along the edge.
				return true;
			}

			const EdgeGrid::Grid &grid;
			const Slic3r::Point  *pt_this;
			const Slic3r::Point  *pt_prev;
		} visitor(grid, &loop.points.back(), nullptr);

		for (const Point &pt_this : loop.points) {
			visitor.pt_this = &pt_this;
			grid.visit_cells_intersecting_line(*visitor.pt_prev, pt_this, visitor);
			visitor.pt_prev = &pt_this;
		}
	}

	return out;
}

std::vector<TrimmedLoop> trim_loops(const Polygons &loops, const EdgeGrid::Grid &grid)
{
	std::vector<TrimmedLoop> out;
	out.reserve(loops.size());
	for (const Polygon &loop : loops)
		out.emplace_back(trim_loop(loop, grid));
	return out;
}

}
