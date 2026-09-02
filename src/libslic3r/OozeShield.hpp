#ifndef slic3r_OozeShield_hpp_
#define slic3r_OozeShield_hpp_

#include "libslic3r.h"
#include "Polygon.hpp"
#include "ExtrusionEntity.hpp"

namespace Slic3r {

class Print;
class Flow;

// Cura-style contour-following ooze shield: a single-wall shroud offset from the
// model silhouette (not support) to wipe nozzle ooze after tool changes.
class OozeShield
{
public:
    // Per-object-layer shield polygons in print coordinates.
    static std::vector<Polygons> generate_layer_polygons(const Print &print);

    static void polygons_to_extrusion_entities(
        const Polygons          &polygons,
        ExtrusionEntityCollection &dst,
        const Flow              &flow);

    // Highest object-layer index that should emit a shield (last layer with a tool change).
    static size_t max_shield_layer(const Print &print);
};

} // namespace Slic3r

#endif // slic3r_OozeShield_hpp_
