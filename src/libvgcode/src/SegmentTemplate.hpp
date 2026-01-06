///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_SEGMENTTEMPLATE_HPP
#define VGCODE_SEGMENTTEMPLATE_HPP

#include <cstddef>

namespace libvgcode {

class SegmentTemplate
{
public:
    SegmentTemplate() = default;
    ~SegmentTemplate() { shutdown(); }
    SegmentTemplate(const SegmentTemplate& other) = delete;
    SegmentTemplate(SegmentTemplate&& other) = delete;
    SegmentTemplate& operator = (const SegmentTemplate& other) = delete;
    SegmentTemplate& operator = (SegmentTemplate&& other) = delete;

    //
    // Initialize gpu buffers.
    //
    void init();
    //
    // Release gpu buffers.
    //
    void shutdown();
    void render(size_t count);

    //
    // Return the size of the data sent to gpu, in bytes.
    //
    size_t size_in_bytes_gpu() const { return m_size_in_bytes_gpu; }

private:
    //
    // gpu buffers ids.
    //
    unsigned int m_vao_id{ 0 };
    unsigned int m_vbo_id{ 0 };
    //
    // Size of the data sent to gpu, in bytes.
    //
    size_t m_size_in_bytes_gpu{ 0 };
};

} // namespace libvgcode

#endif // VGCODE_SEGMENTTEMPLATE_HPP