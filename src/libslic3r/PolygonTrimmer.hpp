#ifndef slic3r_PolygonTrimmer_hpp_
#define slic3r_PolygonTrimmer_hpp_

#include "libslic3r.h"
#include <vector>
#include <string>
#include "Line.hpp"
#include "MultiPoint.hpp"
#include "Polyline.hpp"
#include "Polygon.hpp"

namespace Slic3r {

namespace EdgeGrid {
	class Grid;
}

struct TrimmedLoop
{
	Points          			points;
	// Number of points per segment. Empty if the loop is 
	std::vector<unsigned int> 	segments;

	bool 	is_trimmed() const { return ! segments.empty(); }
};

TrimmedLoop trim_loop(const Polygon &loop, const EdgeGrid::Grid &grid);
std::vector<TrimmedLoop> trim_loops(const Polygons &loops, const EdgeGrid::Grid &grid);

} // namespace Slic3r

#endif /* slic3r_PolygonTrimmer_hpp_ */
