#pragma once

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Point.hpp"
#include <libvgcode/include/GCodeInputData.hpp>
#include <libvgcode/include/Types.hpp>

#include <cstdint>
#include <vector>

namespace libvgcode { class Viewer; }

namespace Slic3r::GUI {

struct PreviewTriangleVertex
{
    Vec3f position{Vec3f::Zero()};
    Vec3f normal{Vec3f::Zero()};
    Vec2f uv{Vec2f::Zero()};
};

struct PreviewTriangleGroup
{
    uint32_t first_index{0};
    uint32_t index_count{0};
    uint16_t material_slot{0};
    uint8_t  extrusion_role{0};
    uint8_t  extruder_id{0};
    uint8_t  colour_id{0};
    uint32_t layer_id{0};
    libvgcode::Color color{};
};

struct PreviewTriangleMesh
{
    std::vector<PreviewTriangleVertex> vertices;
    std::vector<uint32_t>              indices;
    std::vector<PreviewTriangleGroup>  groups;
    BoundingBoxf3                      bounds;
};

enum class PreviewMeshScope : uint8_t { Visible, Complete };

// Builds the same cap/corner topology used by Orca's toolpath OBJ export from the already
// normalized libvgcode viewer. No G-code parsing or independent path tessellation occurs here.
PreviewTriangleMesh build_preview_triangle_mesh(const libvgcode::Viewer& viewer, PreviewMeshScope scope);
// Raw input data has no viewer range and is therefore always complete.
PreviewTriangleMesh build_preview_triangle_mesh(const libvgcode::GCodeInputData& data);

} // namespace Slic3r::GUI
