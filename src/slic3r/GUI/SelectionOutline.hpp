#pragma once

#include <array>
#include <vector>

namespace Slic3r {

class GLVolume;

namespace GUI {

struct Camera;
class GLModel;

// The outline of the selected volumes: a ring around them and lines along their inner edges, found in a mask
// of the volumes and drawn over the scene. See docs/HLSD/selection-outline.md.
class SelectionOutline
{
public:
    // Set in the stencil where the outlined volumes are drawn.
    static constexpr unsigned int StencilBit = 0x80;
    // Set in the stencil where the outline is drawn, which the occlusion pass leaves as it is.
    static constexpr unsigned int DrawnBit = 0x40;

    // Over the bound framebuffer, whose stencil and depth hold the scene the volumes were drawn into.
    void render(const std::vector<GLVolume*>& volumes, const Camera& camera, const std::array<float, 2>& z_range,
                const std::array<double, 4>& clipping_plane, float scale, GLModel& quad);
    // Frees the targets.
    void reset();

private:
    bool update_targets(unsigned int width, unsigned int height);

    unsigned int m_mask_fbo{ 0 };
    unsigned int m_mask_texture{ 0 };
    unsigned int m_depth_texture{ 0 };
    unsigned int m_edges_fbo{ 0 };
    unsigned int m_edges_texture{ 0 };
    std::array<unsigned int, 2> m_size{ { 0, 0 } };
    bool m_complete{ false };
    int m_stencil_bits{ -1 };
};

} // namespace GUI
} // namespace Slic3r
