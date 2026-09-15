#pragma once

#include "libslic3r/Point.hpp"

#include <array>
#include <vector>

namespace Slic3r {
namespace GUI {

class GLModel;

// The last 3D scene pass, kept as a texture for frames that only rebuild the overlay.
class SceneCache
{
public:
    // Scene pass inputs that change without any frame request.
    struct Key
    {
        std::array<unsigned int, 2> size{ { 0, 0 } };
        Transform3d view_matrix{ Transform3d::Identity() };
        Transform3d projection_matrix{ Transform3d::Identity() };
        // Hovered volumes that draw their sinking contour, hovered plate icons, hovered gizmo grabber.
        std::vector<int> sinking_hover_volume_idxs;
        std::vector<int> hover_plate_icon_idxs;
        int gizmo_hover_id{ -1 };
        bool render_preview{ true };

        bool operator == (const Key& other) const {
            return size == other.size && render_preview == other.render_preview &&
                   gizmo_hover_id == other.gizmo_hover_id &&
                   sinking_hover_volume_idxs == other.sinking_hover_volume_idxs &&
                   hover_plate_icon_idxs == other.hover_plate_icon_idxs &&
                   view_matrix.isApprox(other.view_matrix) &&
                   projection_matrix.isApprox(other.projection_matrix);
        }
    };

    // Copies the bound read framebuffer, sized by key.size, and remembers key.
    void capture(Key key);
    bool matches(const Key& key) const { return m_valid && m_key == key; }
    // Draws the last capture over the whole viewport, on the given full screen quad.
    void render(GLModel& quad);
    void invalidate() { m_valid = false; }
    // Frees the texture.
    void reset();

private:
    unsigned int m_texture_id{ 0 };
    std::array<unsigned int, 2> m_texture_size{ { 0, 0 } };
    Key m_key;
    bool m_valid{ false };
};

} // namespace GUI
} // namespace Slic3r
