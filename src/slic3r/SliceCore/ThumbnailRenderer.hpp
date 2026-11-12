#pragma once

// ThumbnailRenderer.hpp
//
// Headless thumbnail/preview-image renderer for SliceCore (Tier-2 preview).
// Uses GLFW + GLAD + OpenGLManager + GLCanvas3D::render_thumbnail_framebuffer to
// render the model to an off-screen framebuffer and encode the result as PNG bytes.
//
// Only available in SLIC3R_GUI builds (same guard as OpenGLManager / GLCanvas3D).
// Non-GUI builds get a no-op stub that returns false immediately.
//
// Thread safety: all GL work is serialised behind a process-wide static mutex.
// GLFW and GLAD are process-global singletons; concurrent calls from the
// JobQueue worker threads are safe.

#include <string>
#include <vector>

namespace Slic3r {
class Model;
class DynamicPrintConfig;
} // namespace Slic3r

namespace Slic3r {
namespace SliceCore {

// Renders a top-down ortho thumbnail of the model to PNG bytes.
//
// Returns true on success and fills png_bytes with the raw PNG file data.
// On any GL/context/shader failure returns false, sets err, and leaves
// png_bytes unchanged.  The caller should treat failure as non-fatal.
//
// width / height: desired image dimensions in pixels (e.g. 512 x 512).
// The rendered image may be smaller if the GL driver caps framebuffer size.
//
// v1 note: renders the entire model (all objects on all plates) in a single
// pass using plate_id=0.  Per-plate rendering can be added in a future
// iteration when a PartPlateList can be passed in from the caller.
bool render_model_thumbnail(const Model              &model,
                            const DynamicPrintConfig &cfg,
                            int                       width,
                            int                       height,
                            std::vector<unsigned char> &png_bytes,
                            std::string               &err);

} // namespace SliceCore
} // namespace Slic3r
