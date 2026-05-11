// Boustrophedon (snake-like) ordering: sort objects into rows by Y,
// traverse each row alternating direction for smooth back-and-forth path.

#include "Boustrophedon.hpp"
#include "TSPPostProcessing.hpp"

namespace Slic3r {

// Core algorithm: boustrophedon (snake) ordering on a raw point set.
std::vector<size_t> boustrophedon_core(const Points& centers)
{
    if (centers.empty()) return {};

    // Row-based serpentine traversal.
    std::vector<size_t> path = row_serpentine_path(centers);

    // Post-processing: 2-opt, crossing removal, rotation.
    tsp_2opt_improve(path, centers);
    tsp_remove_crossings(path, centers);
    tsp_rotate_minimize_closing(path, centers);

    return path;
}

// Production wrapper.
std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const std::vector<const PrintObject*>& print_objects, const Point* start_near)
{
    return chain_instances_with_core(print_objects, start_near, boustrophedon_core);
}

std::vector<const PrintInstance*> chain_print_object_instances_boustrophedon(const Print& print)
{
    return chain_print_object_instances_boustrophedon(print.objects().vector(), nullptr);
}

} // namespace Slic3r
