#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include <boost/filesystem/path.hpp>
#include <functional>
#include <memory>
#include <vector>

namespace Slic3r::AI {

struct GlbGeometrySource;

// This is a deliberately bounded geometry-only path. It accepts one static
// triangle primitive used by one node, dense float positions/optional normals,
// and invertible node transforms. It validates the source accessor mapping
// against the editor's complete mesh before any edited artifact is written.
// The checkpoint may throw the caller's cancellation exception.
std::shared_ptr<GlbGeometrySource> read_glb_geometry_source(
    const boost::filesystem::path& source, const indexed_triangle_set& editor_mesh,
    const std::function<void()>& checkpoint = {});

// Writes a new GLB version, retaining the original binary payload and JSON
// material/texture/node data. New position and affected-normal accessors are
// appended; topology, UVs, images and colors are never converted or resampled.
// Unsupported input, a changed source, or cancellation leaves no destination.
void write_glb_geometry_edit(const GlbGeometrySource& source,
    const boost::filesystem::path& destination, const indexed_triangle_set& edited_mesh,
    const std::vector<size_t>& selected_faces, const std::function<void()>& checkpoint = {});

} // namespace Slic3r::AI
