#ifndef SRC_LIBSLIC3R_SHORTEDGECOLLAPSE_HPP_
#define SRC_LIBSLIC3R_SHORTEDGECOLLAPSE_HPP_

#include "libslic3r/TriangleMesh.hpp"

namespace Slic3r{

// Decimates the model by collapsing short edges. It starts with very small edges and gradually increases the collapsible length,
// until the target triangle count is reached (the algorithm will certainly undershoot the target count, result will have less triangles than target count)
//  The algorithm does not check for triangle flipping, disconnections, self intersections or any other degeneration that can appear during mesh processing.
void its_short_edge_collpase(indexed_triangle_set &mesh, size_t target_triangle_count);

}


#endif /* SRC_LIBSLIC3R_SHORTEDGECOLLAPSE_HPP_ */
