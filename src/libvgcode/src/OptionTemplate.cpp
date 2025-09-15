///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "OptionTemplate.hpp"
#include "OpenGLUtils.hpp"
#include "Utils.hpp"

#include <vector>
#include <algorithm>
#include <cmath>

namespace libvgcode {

// Geometry:
// diamond with 'resolution' sides, centered at (0.0, 0.0, 0.0)
// height and width of the diamond are equal to 1.0
void OptionTemplate::init(uint8_t resolution)
{
    if (m_top_vao_id != 0)
        return;

    m_resolution = std::max<uint8_t>(resolution, 3);
    m_vertices_count = 2 + resolution;
    const float step = 2.0f * PI / float(m_resolution);

    //
    // top fan
    //
    std::vector<float> top_vertices;
    top_vertices.reserve(6 * m_vertices_count);
    add_vertex({ 0.0f, 0.0f, 0.5f }, { 0.0f, 0.0f, 1.0f }, top_vertices);
    for (uint8_t i = 0; i <= m_resolution; ++i) {
        const float ii = float(i) * step;
        const Vec3 pos = { 0.5f * std::cos(ii), 0.5f * std::sin(ii), 0.0f };
        const Vec3 norm = normalize(pos);
        add_vertex(pos, norm, top_vertices);
    }

    //
    // bottom fan
    //
    std::vector<float> bottom_vertices;
    bottom_vertices.reserve(6 * m_vertices_count);
    add_vertex({ 0.0f, 0.0f, -0.5f }, { 0.0f, 0.0f, -1.0f }, bottom_vertices);
    for (uint8_t i = 0; i <= m_resolution; ++i) {
        const float ii = -float(i) * step;
        const Vec3 pos = { 0.5f * std::cos(ii), 0.5f * std::sin(ii), 0.0f };
        const Vec3 norm = normalize(pos);
        add_vertex(pos, norm, bottom_vertices);
    }

    m_size_in_bytes_gpu += top_vertices.size() * sizeof(float);
    m_size_in_bytes_gpu += bottom_vertices.size() * sizeof(float);

    const size_t vertex_stride = 6 * sizeof(float);
    const size_t position_offset = 0;
    const size_t normal_offset = 3 * sizeof(float);

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));
    int curr_array_buffer;
    glsafe(glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &curr_array_buffer));

    glsafe(glGenVertexArrays(1, &m_top_vao_id));
    glsafe(glBindVertexArray(m_top_vao_id));
    glsafe(glGenBuffers(1, &m_top_vbo_id));
    glsafe(glBindBuffer(GL_ARRAY_BUFFER, m_top_vbo_id));
    glsafe(glBufferData(GL_ARRAY_BUFFER, top_vertices.size() * sizeof(float), top_vertices.data(), GL_STATIC_DRAW));
    glsafe(glEnableVertexAttribArray(0));
    glsafe(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertex_stride, (const void*)position_offset));
    glsafe(glEnableVertexAttribArray(1));
    glsafe(glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, vertex_stride, (const void*)normal_offset));

    glsafe(glGenVertexArrays(1, &m_bottom_vao_id));
    glsafe(glBindVertexArray(m_bottom_vao_id));
    glsafe(glGenBuffers(1, &m_bottom_vbo_id));
    glsafe(glBindBuffer(GL_ARRAY_BUFFER, m_bottom_vbo_id));
    glsafe(glBufferData(GL_ARRAY_BUFFER, bottom_vertices.size() * sizeof(float), bottom_vertices.data(), GL_STATIC_DRAW));
    glsafe(glEnableVertexAttribArray(0));
    glsafe(glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, vertex_stride, (const void*)position_offset));
    glsafe(glEnableVertexAttribArray(1));
    glsafe(glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, vertex_stride, (const void*)normal_offset));

    glsafe(glBindBuffer(GL_ARRAY_BUFFER, curr_array_buffer));
    glsafe(glBindVertexArray(curr_vertex_array));
}

void OptionTemplate::shutdown()
{
    if (m_bottom_vbo_id != 0) {
        glsafe(glDeleteBuffers(1, &m_bottom_vbo_id));
        m_bottom_vbo_id = 0;
    }
    if (m_bottom_vao_id != 0) {
        glsafe(glDeleteVertexArrays(1, &m_bottom_vao_id));
        m_bottom_vao_id = 0;
    }
    if (m_top_vbo_id != 0) {
        glsafe(glDeleteBuffers(1, &m_top_vbo_id));
        m_top_vbo_id = 0;
    }
    if (m_top_vao_id != 0) {
        glsafe(glDeleteVertexArrays(1, &m_top_vao_id));
        m_top_vao_id = 0;
    }

    m_size_in_bytes_gpu = 0;
}

void OptionTemplate::render(size_t count)
{
    if (m_top_vao_id == 0 || m_top_vbo_id == 0 || m_bottom_vao_id == 0 || m_bottom_vbo_id == 0 || count == 0)
        return;

    int curr_vertex_array;
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));

    glsafe(glBindVertexArray(m_top_vao_id));
    glsafe(glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(m_vertices_count), static_cast<GLsizei>(count)));
    glsafe(glBindVertexArray(m_bottom_vao_id));
    glsafe(glDrawArraysInstanced(GL_TRIANGLE_FAN, 0, static_cast<GLsizei>(m_vertices_count), static_cast<GLsizei>(count)));
    glsafe(glBindVertexArray(curr_vertex_array));
}

} // namespace libvgcode
