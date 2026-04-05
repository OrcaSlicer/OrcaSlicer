#ifndef slic3r_ZoneBoundary_ZoneInterior_hpp_
#define slic3r_ZoneBoundary_ZoneInterior_hpp_

#include <string>
#include <libslic3r/SLA/Interior.hpp>

namespace Slic3r {

class TriangleMesh;

namespace zone_boundary {

// Apply constrained mean curvature smoothing to the interior boundary.
// Smooths stair-step artifacts while ensuring shell thickness is maintained.
// original_mesh: the original mesh (for computing thickness constraint)
// iterations: number of smoothing iterations (default 5)
void smooth_interior(sla::Interior &interior, const TriangleMesh &original_mesh, int iterations = 5);

// Filter out thin inner zone sections using morphological reconstruction.
// Removes regions where the interior is thinner than min_width in any direction.
// This eliminates small disconnected islands and thin protrusions that would
// create unusable infill zones.
// Algorithm: erode to find thick core, over-dilate, intersect with original
// to preserve exact surface detail in thick regions.
// min_width: minimum thickness in mm (0 to disable)
void filter_thin_interior(sla::Interior &interior, double min_width);

// Export interior mesh to STL for debugging.
// stage_name: descriptive name for this processing stage (e.g., "1_initial", "2_filtered")
// object_id: ID of the PrintObject for unique filenames
// Returns true if export was successful, false if mesh is empty or export failed.
bool debug_export_interior(const sla::Interior &interior, const std::string &stage_name, int object_id = 0);

} // namespace zone_boundary
} // namespace Slic3r

#endif // slic3r_ZoneBoundary_ZoneInterior_hpp_
