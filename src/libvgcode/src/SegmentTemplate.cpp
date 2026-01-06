///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "SegmentTemplate.hpp"
#include "OpenGLUtils.hpp"

#include <cstdint>
#include <array>

namespace libvgcode {

//|     /1-------6\     |
//|    / |       | \    |
//|   2--0-------5--7   |
//|    \ |       | /    |
//|      3-------4      | 
static constexpr const std::array<uint8_t, 24> VERTEX_DATA = {
    0, 1, 2, // front spike
    0, 2, 3, // front spike
    0, 3, 4, // right/bottom body 
    0, 4, 5, // right/bottom body 
    0, 5, 6, // left/top body 
    0, 6, 1, // left/top body 
    5, 4, 7, // back spike
    5, 7, 6, // back spike
};

void SegmentTemplate::init()
{
    if (m_vao_id != 0)
        return;

    m_size_in_bytes_gpu += VERTEX_DATA.size() * sizeof(uint8_t);

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));
    int curr_array_buffer;
    glsafe(glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &curr_array_buffer));

    glsafe(glGenVertexArrays(1, &m_vao_id));
    glsafe(glBindVertexArray(m_vao_id));

    glsafe(glGenBuffers(1, &m_vbo_id));
    glsafe(glBindBuffer(GL_ARRAY_BUFFER, m_vbo_id));
    glsafe(glBufferData(GL_ARRAY_BUFFER, VERTEX_DATA.size() * sizeof(uint8_t), VERTEX_DATA.data(), GL_STATIC_DRAW));
    glsafe(glEnableVertexAttribArray(0));
#ifdef ENABLE_OPENGL_ES
    glsafe(glVertexAttribPointer(0, 1, GL_UNSIGNED_BYTE, GL_FALSE, 0, (const void*)0));
#else
    glsafe(glVertexAttribIPointer(0, 1, GL_UNSIGNED_BYTE, 0, (const void*)0));
#endif // ENABLE_OPENGL_ES

    glsafe(glBindBuffer(GL_ARRAY_BUFFER, curr_array_buffer));
    glsafe(glBindVertexArray(curr_vertex_array));
}

void SegmentTemplate::shutdown()
{
    if (m_vbo_id != 0) {
        glsafe(glDeleteBuffers(1, &m_vbo_id));
        m_vbo_id = 0;
    }
    if (m_vao_id != 0) {
        glsafe(glDeleteVertexArrays(1, &m_vao_id));
        m_vao_id = 0;
    }

    m_size_in_bytes_gpu = 0;
}

void SegmentTemplate::render(size_t count)
{
    if (m_vao_id == 0 || m_vbo_id == 0 || count == 0)
        return;

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));

    glsafe(glBindVertexArray(m_vao_id));
    glsafe(glDrawArraysInstanced(GL_TRIANGLES, 0, static_cast<GLsizei>(VERTEX_DATA.size()), static_cast<GLsizei>(count)));
    glsafe(glBindVertexArray(curr_vertex_array));
}

} // namespace libvgcode
