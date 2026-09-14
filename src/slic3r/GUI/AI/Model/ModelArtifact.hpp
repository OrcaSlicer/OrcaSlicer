#pragma once

#include <cassert>
#include <functional>
#include <map>
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include <boost/filesystem/path.hpp>

namespace Slic3r::AI {

// AI editing uses Z-up millimetres and sRGB colors. GLB files use glTF's
// Y-up metres and linear COLOR_0; conversion occurs only at this boundary.
bool load_model_artifact(const boost::filesystem::path& path, TriangleMesh& mesh,
                         ObjInfo& colors, std::string& error);
bool write_model_artifact(const boost::filesystem::path& path, const indexed_triangle_set& mesh,
                          const std::vector<RGBA>& colors, std::string& error);
bool is_model_artifact(const boost::filesystem::path& path);
std::string model_artifact_format(const boost::filesystem::path& path);
std::string model_artifact_sha256(const boost::filesystem::path& path);

} // namespace Slic3r::AI
