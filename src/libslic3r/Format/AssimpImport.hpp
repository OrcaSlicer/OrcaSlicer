#pragma once

#include <string>

namespace Slic3r {

struct TexturedMesh;

bool load_assimp_textured_model(const std::string& path, TexturedMesh& out, std::string* error_message = nullptr);

} // namespace Slic3r
