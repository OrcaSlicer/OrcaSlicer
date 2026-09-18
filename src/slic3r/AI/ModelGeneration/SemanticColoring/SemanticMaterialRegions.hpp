#pragma once

#include "SemanticColoring.hpp"

namespace Slic3r::AI::SemanticColoring {

// Refine the automatic layer using original surface/material continuity. Exact
// position welding is local to this operation; source geometry, colors and
// recognizer labels remain immutable. All targets come from existing reliable
// automatic assignments and must be members of the supplied physical palette.
void refine_material_patches(const MeshSnapshot&, const Analysis&,
                             const std::vector<Color>& palette,
                             const std::vector<Color>& portrait_card,
                             FaceColors& suggestions);

} // namespace Slic3r::AI::SemanticColoring
