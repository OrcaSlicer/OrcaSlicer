// Hilbert curve ordering: map 2D positions to 1D Hilbert curve index, sort,
// then 2-opt and rotate to minimize closing edge.
// Excellent locality for clustered object layouts.

#ifndef slic3r_HilbertCurve_hpp_
#define slic3r_HilbertCurve_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Core algorithm: Hilbert curve ordering on a raw point set.
std::vector<size_t> hilbert_curve_core(const Points& centers);

// Chain instances of print objects by sorting on the Hilbert space-filling curve.
// Maps 2D coordinates to 1D Hilbert index, sorts, applies 2-opt, and rotates.
std::vector<const PrintInstance*> chain_print_object_instances_hilbert(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_hilbert(const Print& print);

} // namespace Slic3r

#endif /* slic3r_HilbertCurve_hpp_ */
