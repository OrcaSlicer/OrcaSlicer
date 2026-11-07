#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <tuple>

namespace Slic3r::GUI {

// A value snapshot, with no references to the document or the OCCT shape. Indices expire when
// the selection is cleared or the CAD document is rebuilt; they are not persistent face IDs.
struct CadFaceSelection
{
    int body_index{-1};
    int face_index{-1};
    std::uint64_t selection_revision{0};
    std::array<double, 3> centroid{};
    std::optional<std::array<double, 3>> normal;
    double area{0.0};
    bool is_planar{false};
    // centroid/normal/area are in OCCT shape coordinates (mm, mm^2). This row-major affine
    // matrix maps shape points into the Design viewport, including a pending body move.
    std::array<std::array<double, 4>, 4> body_to_design{{
        {{1., 0., 0., 0.}}, {{0., 1., 0., 0.}}, {{0., 0., 1., 0.}}, {{0., 0., 0., 1.}}
    }};

    bool same_geometry(const CadFaceSelection& other) const
    {
        return std::tie(body_index, face_index, centroid, normal, area, is_planar, body_to_design) ==
               std::tie(other.body_index, other.face_index, other.centroid, other.normal,
                        other.area, other.is_planar, other.body_to_design);
    }
};

} // namespace Slic3r::GUI
