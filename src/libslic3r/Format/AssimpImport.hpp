#pragma once

#include <string>
#include <array>
#include <vector>

namespace Slic3r {

struct TexturedMesh;

bool load_assimp_textured_model(const std::string& path, TexturedMesh& out, std::string* error_message = nullptr,
                               std::vector<std::array<float, 4>>* raw_vertex_colors = nullptr);

} // namespace Slic3r
