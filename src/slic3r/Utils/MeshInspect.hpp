#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace Slic3r {

class Model;

namespace MeshInspect {

// Writes the --inspect-mesh JSON for `model` to `out`: per object its bounding boxes and the faces
// it can be laid on, taken from lay_on_face_planes() so they are the faces the --ground-* options
// choose from, in the frame those options take. At most `max_planes` faces are listed per object,
// largest first. `source_paths` lists every input file; the CLI merges them into one model.
void inspect_to_json(const Model &model, const std::vector<std::string> &source_paths, std::ostream &out, size_t max_planes = 8);

} // namespace MeshInspect
} // namespace Slic3r
