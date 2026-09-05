// MeshInspect.hpp — CLI mesh-inspection primitives.
//
// Backs the --inspect-mesh CLI action. Emits a structured JSON summary of
// the loaded Model — per-object mesh-local bbox, world bbox, and the top-N
// largest planar-face clusters ranked by area — so external tooling can
// reason about a model's geometry without opening the GUI. Useful for CI
// pipelines, scripted preprocessing, and AI-assisted orientation planning
// (the largest face's normal is almost always the natural "sit-on-the-bed"
// candidate).
#ifndef slic3r_MeshInspect_hpp_
#define slic3r_MeshInspect_hpp_

#include <iosfwd>
#include <string>

namespace Slic3r {
class Model;

namespace MeshInspect {

// Emit a structured JSON inspection of `model` to `out`:
//   - per-object mesh-local bbox + centroid,
//   - world bbox (via instance 0's transform) + instance_offset,
//   - top_n_clusters largest planar-face clusters, each with the
//     area-weighted mean normal, total area (mm²), and triangle count.
// Coordinates are mesh-local unless the field name says otherwise.
void inspect_to_json(const Model &model, const std::string &source_path,
                     std::ostream &out, int top_n_clusters = 8);

} // namespace MeshInspect
} // namespace Slic3r

#endif
