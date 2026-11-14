#pragma once

#include "CadFaceSelection.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r::GUI {

// Native primary pick. Whole-body picks may contain a solid, shell or compound. The geometry
// snapshot survives a model edit; its source indices do not identify the edited document.
struct CadShapeSelection
{
    std::string kind;
    int body_index{-1}, face_index{-1}, edge_index{-1}, vertex_index{-1};
    std::vector<int> edge_indices; // native Shift/Ctrl edge set; shape is the primary edge
    std::uint64_t selection_revision{0};
    std::array<std::array<double, 4>, 4> body_to_design{{
        {{1., 0., 0., 0.}}, {{0., 1., 0., 0.}}, {{0., 0., 1., 0.}}, {{0., 0., 0., 1.}}
    }};
    std::shared_ptr<CadShape> shape;
    std::optional<CadFaceSelection> face;

    bool same_geometry(const CadShapeSelection& other) const
    {
        // Rebuilds clear the native pick before indices can be reused. Do not compare copy
        // addresses: a repeated notification can describe the same native pick.
        return std::tie(kind, body_index, face_index, edge_index, vertex_index, edge_indices, body_to_design) ==
               std::tie(other.kind, other.body_index, other.face_index, other.edge_index, other.vertex_index,
                        other.edge_indices, other.body_to_design) &&
               ((!face && !other.face) || (face && other.face && face->same_geometry(*other.face)));
    }
};

} // namespace Slic3r::GUI
