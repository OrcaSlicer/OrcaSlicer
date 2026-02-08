///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_COGMARKER_HPP
#define VGCODE_COGMARKER_HPP

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS

namespace libvgcode {

class CogMarker
{
public:
    CogMarker() = default;
    ~CogMarker() { shutdown(); }
    CogMarker(const CogMarker& other) = delete;
    CogMarker(CogMarker&& other) = delete;
    CogMarker& operator = (const CogMarker& other) = delete;
    CogMarker& operator = (CogMarker&& other) = delete;

    //
    // Initialize gpu buffers
    //
    void init(uint8_t resolution, float radius);
    //
    // Release gpu buffers
    //
    void shutdown();
    //
    // Render the marker
    //
    void render();
    //
    // Update values used to calculate the center of gravity
    //
    void update(const Vec3& position, float mass);
    //
    // Reset values used to calculate the center of gravity
    //
    void reset();
    //
    // Return the calculated center of gravity position
    //
    Vec3 get_position() const;
    //
    // Return the size of the data sent to gpu, in bytes.
    //
    size_t size_in_bytes_gpu() const { return m_size_in_bytes_gpu; }

private:
    //
    // Values used to calculate the center of gravity
    //
    float m_total_mass{ 0.0f };
    Vec3 m_total_position{ 0.0f, 0.0f, 0.0f };
    //
    // The count of indices stored into the ibo buffer.
    //
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

#endif // VGCODE_COGMARKER_HPP