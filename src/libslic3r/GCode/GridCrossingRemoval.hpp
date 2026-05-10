// Grid-based crossing removal: spatial hash edges into grid cells,
// only test edges within same/adjacent cells. O(n) average case vs O(n²).

#ifndef slic3r_GridCrossingRemoval_hpp_
#define slic3r_GridCrossingRemoval_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

// Remove crossings from a path using spatial hashing. O(n) average case.
void tsp_remove_crossings_grid(std::vector<size_t>& path, const Points& centers);

} // namespace Slic3r

#endif /* slic3r_GridCrossingRemoval_hpp_ */
