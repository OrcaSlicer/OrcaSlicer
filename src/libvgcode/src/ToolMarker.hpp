///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_TOOLMARKER_HPP
#define VGCODE_TOOLMARKER_HPP

#include "../include/Types.hpp"

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
#include <algorithm>

namespace libvgcode {

class ToolMarker
{
public:
    ToolMarker() = default;
    ~ToolMarker() { shutdown(); }
    ToolMarker(const ToolMarker& other) = delete;
    ToolMarker(ToolMarker&& other) = delete;
    ToolMarker& operator = (const ToolMarker& other) = delete;
    ToolMarker& operator = (ToolMarker&& other) = delete;

    //
    // Initialize gpu buffers.
    //
    void init(uint16_t resolution, float tip_radius, float tip_height, float stem_radius, float stem_height);
    //
    // Release gpu buffers.
    //
    void shutdown();
    void render();

    const Vec3& get_position() const { return m_position; }
    void set_position(const Vec3& position) { m_position = position; }

    float get_offset_z() const { return m_offset_z; }
    void set_offset_z(float offset_z) { m_offset_z = std::max(offset_z, 0.0f); }

    const Color& get_color() const { return m_color; }
    void set_color(const Color& color) { m_color = color; }

    float get_alpha() const { return m_alpha; }
    void set_alpha(float alpha) { m_alpha = std::clamp(alpha, 0.25f, 0.75f); }

    //
    // Return the size of the data sent to gpu, in bytes.
    //
    size_t size_in_bytes_gpu() const { return m_size_in_bytes_gpu; }

private:
    Vec3 m_position{ 0.0f, 0.0f, 0.0f };
    float m_offset_z{ 0.5f };
    Color m_color{ 255, 255, 255 };
    float m_alpha{ 0.5f };

    uint16_t m_indices_count{ 0 };
    //
    // gpu buffers ids.
    //
    unsigned int m_vao_id{ 0 };
    unsigned int m_vbo_id{ 0 };
    unsigned int m_ibo_id{ 0 };
    //
    // Size of the data sent to gpu, in bytes.
    //
    size_t m_size_in_bytes_gpu{ 0 };
};

} // namespace libvgcode

#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

#endif // VGCODE_TOOLMARKER_HPP