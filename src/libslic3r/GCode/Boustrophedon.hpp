// Boustrophedon (snake-like) ordering: sort objects into rows by Y,
// traverse each row alternating direction for smooth back-and-forth path.

#ifndef slic3r_Boustrophedon_hpp_
#define slic3r_Boustrophedon_hpp_

#include "../libslic3r.h"
#include "../Point.hpp"

#include <vector>

namespace Slic3r {

class Print;
class PrintObject;
struct PrintInstance;

// Core algorithm: boustrophedon (snake) ordering on a raw point set.
std::vector<size_t> boustrophedon_core(const Points& centers);

// Chain instances using a boustrophedon (snake) pattern:
// sort into rows by Y, traverse each row alternating direction,
// then apply 2-opt and path rotation.
std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const std::vector<const PrintObject*>& print_objects, const Point* start_near);
std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const Print& print);

} // namespace Slic3r

#endif /* slic3r_Boustrophedon_hpp_ */
