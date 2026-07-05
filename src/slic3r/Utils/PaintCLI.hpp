// PaintCLI.hpp — CLI paint-inspection primitives.
//
// Backs the --inspect-paint CLI action. Reads the per-facet enforcer /
// blocker / extruder / fuzzy-skin state that OrcaSlicer stores on every
// ModelVolume (supports, seam, MMU color, fuzzy-skin) and emits a
// structured JSON summary — facet count, surface area, and mesh-local
// bounding box per state — so CI / scripted / AI tooling can reason
// about existing paint on a .3mf without opening the GUI.
//
// Coordinates are mesh-local (each volume's own frame), matching the
// frame that the paint gizmos operate in.
#ifndef slic3r_PaintCLI_hpp_
#define slic3r_PaintCLI_hpp_

#include <iosfwd>
#include <string>

namespace Slic3r {
class Model;

namespace PaintCLI {

void inspect_to_json(const Model &model, const std::string &source_path,
                     std::ostream &out);

} // namespace PaintCLI
} // namespace Slic3r

#endif
