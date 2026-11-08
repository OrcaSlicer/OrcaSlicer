#pragma once

#include <string>
#include <utility>
#include <vector>

#include "libslic3r/BedMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "GLModel.hpp"
#include "GLTexture.hpp"

namespace Slic3r {
namespace GUI {

// Draws the printer's probed bed mesh on the plate as a colour-coded height map: a diverging ramp
// (low = cool, on plane = neutral grey, high = warm) over the probed rectangle only, that
// rectangle's outline, and a mark per probe point. Adaptive (KAMP) meshes therefore show up as a
// patch where the last print was, and the rest of the plate stays plain rather than pretending
// to know its shape.
//
// The ramp follows the "preview_color_vision" preference, so the G-code preview and the mesh
// switch palettes together. One instance serves every plate; the caller passes the plate origin
// and the mesh is drawn in that plate's printer coordinates.
class BedMeshOverlay
{
public:
    void set_mesh(BedMesh mesh, const std::string& host);
    void clear();
    bool has_mesh() const { return !m_mesh.empty(); }
    const BedMesh& mesh() const { return m_mesh; }
    // Host the mesh was read from, so a printer change can tell whether it still applies.
    const std::string& host() const { return m_host; }
    // Half-width of the colour range in mm; symmetric about zero so the neutral colour always means "on plane".
    float range() const { return m_range; }

    // Draw at height z (world), offset by the plate origin. Expects depth testing enabled.
    void render(const Transform3d& view_matrix, const Transform3d& projection_matrix, const Vec3d& plate_origin, float z);
    // ImGui panel with the ramp and its extremes, the profile, and how much of `bed` (printable
    // area in printer coordinates) the mesh covers. Anchored with its right edge at `right` and
    // its top at `top` (below the main toolbar), clear of the object-info and notification
    // panels at the bottom; the caller moves `right` in when something else owns that edge.
    void render_legend(const BoundingBoxf& bed, float right, float top, float scale);

private:
    using Stops = std::vector<std::pair<float, ColorRGBA>>;

    void      update_palette();
    void      init_models(float z);
    void      init_ramp_texture();
    ColorRGBA color_at(float t) const;

    BedMesh     m_mesh;
    std::string m_host;
    // Statistics of m_mesh, computed once in set_mesh() rather than per frame.
    float       m_z_min{ 0.f };
    float       m_z_max{ 0.f };
    float       m_range{ 0.f };
    float       m_models_z{ 0.f };
    std::string m_color_vision;
    Stops       m_stops;
    GLModel     m_surface;
    GLModel     m_outline;
    GLModel     m_probe_marks;
    GLTexture   m_ramp;
    bool        m_models_dirty{ true };
    bool        m_ramp_dirty{ true };
};

} // namespace GUI
} // namespace Slic3r
