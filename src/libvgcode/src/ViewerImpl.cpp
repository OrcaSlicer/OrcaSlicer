///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak, Vojtěch Bubník @bubnikv, Oleksandra Iushchenko @YuSanka
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "ViewerImpl.hpp"
#include "../include/GCodeInputData.hpp"
#include "Shaders.hpp"
#include "ShadersES.hpp"
#include "OpenGLUtils.hpp"
#include "Utils.hpp"

#include <map>
#include <assert.h>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace libvgcode {

template<class T, class O = T>
using IntegerOnly = std::enable_if_t<std::is_integral<T>::value, O>;

// Rounding up.
// 1.5 is rounded to 2
// 1.49 is rounded to 1
// 0.5 is rounded to 1,
// 0.49 is rounded to 0
// -0.5 is rounded to 0,
// -0.51 is rounded to -1,
// -1.5 is rounded to -1.
// -1.51 is rounded to -2.
// If input is not a valid float (it is infinity NaN or if it does not fit)
// the float to int conversion produces a max int on Intel and +-max int on ARM.
template<typename I>
inline IntegerOnly<I, I> fast_round_up(double a)
{
    // Why does Java Math.round(0.49999999999999994) return 1?
    // https://stackoverflow.com/questions/9902968/why-does-math-round0-49999999999999994-return-1
    return a == 0.49999999999999994 ? I(0) : I(floor(a + 0.5));
}

// Round to a bin with minimum two digits resolution.
// Equivalent to conversion to string with sprintf(buf, "%.2g", value) and conversion back to float, but faster.
static float round_to_bin(const float value)
{
//    assert(value >= 0);
    constexpr float const scale[5]     = { 100.f,  1000.f,  10000.f,  100000.f,  1000000.f };
    constexpr float const invscale[5]  = { 0.01f,  0.001f,  0.0001f,  0.00001f,  0.000001f };
    constexpr float const threshold[5] = { 0.095f, 0.0095f, 0.00095f, 0.000095f, 0.0000095f };
    // Scaling factor, pointer to the tables above.
    int                   i = 0;
    // While the scaling factor is not yet large enough to get two integer digits after scaling and rounding:
    for (; value < threshold[i] && i < 4; ++i);
    // At least on MSVC std::round() calls a complex function, which is pretty expensive.
    // our fast_round_up is much cheaper and it could be inlined.
//    return std::round(value * scale[i]) * invscale[i];
    double a = value * scale[i];
    assert(std::abs(a) < double(std::numeric_limits<int64_t>::max()));
    return fast_round_up<int64_t>(a) * invscale[i];
}

static Mat4x4 inverse(const Mat4x4& m)
{
    // ref: https://stackoverflow.com/questions/1148309/inverting-a-4x4-matrix

    Mat4x4 inv;

    inv[0] = m[5] * m[10] * m[15] -
             m[5] * m[11] * m[14] -
             m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] -
             m[13] * m[7] * m[10];

    inv[4] = -m[4] * m[10] * m[15] +
             m[4] * m[11] * m[14] +
             m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] +
             m[12] * m[7] * m[10];

    inv[8] = m[4] * m[9] * m[15] -
             m[4] * m[11] * m[13] -
             m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] -
             m[12] * m[7] * m[9];

    inv[12] = -m[4] * m[9] * m[14] +
               m[4] * m[10] * m[13] +
               m[8] * m[5] * m[14] -
               m[8] * m[6] * m[13] -
               m[12] * m[5] * m[10] +
               m[12] * m[6] * m[9];

    inv[1] = -m[1] * m[10] * m[15] +
             m[1] * m[11] * m[14] +
             m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] +
             m[13] * m[3] * m[10];

    inv[5] = m[0] * m[10] * m[15] -
             m[0] * m[11] * m[14] -
             m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] -
             m[12] * m[3] * m[10];

    inv[9] = -m[0] * m[9] * m[15] +
             m[0] * m[11] * m[13] +
             m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] +
             m[12] * m[3] * m[9];

    inv[13] = m[0] * m[9] * m[14] -
             m[0] * m[10] * m[13] -
             m[8] * m[1] * m[14] +
             m[8] * m[2] * m[13] +
             m[12] * m[1] * m[10] -
             m[12] * m[2] * m[9];

    inv[2] = m[1] * m[6] * m[15] -
             m[1] * m[7] * m[14] -
             m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] -
             m[13] * m[3] * m[6];

    inv[6] = -m[0] * m[6] * m[15] +
             m[0] * m[7] * m[14] +
             m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] +
             m[12] * m[3] * m[6];

    inv[10] = m[0] * m[5] * m[15] -
              m[0] * m[7] * m[13] -
              m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] -
              m[12] * m[3] * m[5];

    inv[14] = -m[0] * m[5] * m[14] +
              m[0] * m[6] * m[13] +
              m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] +
              m[12] * m[2] * m[5];

    inv[3] = -m[1] * m[6] * m[11] +
             m[1] * m[7] * m[10] +
             m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] +
             m[9] * m[3] * m[6];

    inv[7] = m[0] * m[6] * m[11] -
             m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] -
             m[8] * m[3] * m[6];

    inv[11] = -m[0] * m[5] * m[11] +
             m[0] * m[7] * m[9] +
             m[4] * m[1] * m[11] -
             m[4] * m[3] * m[9] -
             m[8] * m[1] * m[7] +
             m[8] * m[3] * m[5];

    inv[15] = m[0] * m[5] * m[10] -
              m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] -
              m[8] * m[2] * m[5];

    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    assert(det != 0.0f);

    det = 1.0f / det;

    std::array<float, 16> ret = {};
    for (int i = 0; i < 16; ++i) {
        ret[i] = inv[i] * det;
    }

    return ret;
}

std::string check_shader(GLuint handle)
{
    std::string ret;
    GLint params;
    glsafe(glGetShaderiv(handle, GL_COMPILE_STATUS, &params));
    if (params == GL_FALSE) {
        glsafe(glGetShaderiv(handle, GL_INFO_LOG_LENGTH, &params));
        ret.resize(params);
        glsafe(glGetShaderInfoLog(handle, params, &params, ret.data()));
    }
    return ret;
}

std::string check_program(GLuint handle)
{
    std::string ret;
    GLint params;
    glsafe(glGetProgramiv(handle, GL_LINK_STATUS, &params));
    if (params == GL_FALSE) {
        glsafe(glGetProgramiv(handle, GL_INFO_LOG_LENGTH, &params));
        ret.resize(params);
        glsafe(glGetProgramInfoLog(handle, params, &params, ret.data()));
    }
    return ret;
}

unsigned int init_shader(const std::string& shader_name, const char* vertex_shader, const char* fragment_shader)
{
    const GLuint vs_id = glCreateShader(GL_VERTEX_SHADER);
    glcheck();
    glsafe(glShaderSource(vs_id, 1, &vertex_shader, nullptr));
    glsafe(glCompileShader(vs_id));
    std::string res = check_shader(vs_id);
    if (!res.empty()) {
        glsafe(glDeleteShader(vs_id));
        throw std::runtime_error("LibVGCode: Unable to compile vertex shader:\n" + shader_name + "\n" + res + "\n");
    }

    const GLuint fs_id = glCreateShader(GL_FRAGMENT_SHADER);
    glcheck();
    glsafe(glShaderSource(fs_id, 1, &fragment_shader, nullptr));
    glsafe(glCompileShader(fs_id));
    res = check_shader(fs_id);
    if (!res.empty()) {
        glsafe(glDeleteShader(vs_id));
        glsafe(glDeleteShader(fs_id));
        throw std::runtime_error("LibVGCode: Unable to compile fragment shader:\n" + shader_name + "\n" + res + "\n");
    }

    const GLuint shader_id = glCreateProgram();
    glcheck();
    glsafe(glAttachShader(shader_id, vs_id));
    glsafe(glAttachShader(shader_id, fs_id));
    glsafe(glLinkProgram(shader_id));
    res = check_program(shader_id);
    if (!res.empty()) {
        glsafe(glDetachShader(shader_id, vs_id));
        glsafe(glDetachShader(shader_id, fs_id));
        glsafe(glDeleteShader(vs_id));
        glsafe(glDeleteShader(fs_id));
        glsafe(glDeleteProgram(shader_id));
        throw std::runtime_error("LibVGCode: Unable to link shader program:\n" + shader_name + "\n" + res + "\n");
    }

    glsafe(glDetachShader(shader_id, vs_id));
    glsafe(glDetachShader(shader_id, fs_id));
    glsafe(glDeleteShader(vs_id));
    glsafe(glDeleteShader(fs_id));
    return shader_id;
}

static void delete_textures(unsigned int& id)
{
    if (id != 0) {
        glsafe(glDeleteTextures(1, &id));
        id = 0;
    }
}

static void delete_buffers(unsigned int& id)
{
    if (id != 0) {
        glsafe(glDeleteBuffers(1, &id));
        id = 0;
    }
}

static const std::array<Color, size_t(EGCodeExtrusionRole::COUNT)> DEFAULT_EXTRUSION_ROLES_COLORS = { {
    { 230, 179, 179 }, // None
    { 255, 230,  77 }, // Perimeter
    { 255, 125,  56 }, // ExternalPerimeter
    {  31,  31, 255 }, // OverhangPerimeter
    { 176,  48,  41 }, // InternalInfill
    { 150,  84, 204 }, // SolidInfill
    { 240,  64,  64 }, // TopSolidInfill
    { 255, 140, 105 }, // Ironing
    {  77, 128, 186 }, // BridgeInfill
    { 255, 255, 255 }, // GapFill
    {   0, 135, 110 }, // Skirt
    {   0, 255,   0 }, // SupportMaterial
    {   0, 128,   0 }, // SupportMaterialInterface
    { 179, 227, 171 }, // WipeTower
    {  94, 209, 148 },  // Custom
    // ORCA
    { 102,  92, 199 }, // BottomSurface
    {  77, 128, 186 }, // InternalBridgeInfill
    {   0,  59, 110 }, // Brim
    {   0,  64,   0 }, // SupportTransition
    { 128, 128, 128 }, // Mixed
} };

static const std::array<Color, size_t(EOptionType::COUNT)> DEFAULT_OPTIONS_COLORS{ {
    {  56,  72, 155 }, // Travels
    { 255, 255,   0 }, // Wipes
    { 205,  34, 214 }, // Retractions
    {  73, 173, 207 }, // Unretractions
    { 230, 230, 230 }, // Seams
    { 193, 190,  99 }, // ToolChanges
    { 218, 148, 139 }, // ColorChanges
    {  82, 240, 131 }, // PausePrints
    { 226, 210,  67 }  // CustomGCodes
} };

#ifdef ENABLE_OPENGL_ES
static std::pair<size_t, size_t> width_height(size_t count)
{
    std::pair<size_t, size_t> ret;
    ret.first = std::min(count, OpenGLWrapper::max_texture_size());
    size_t rows_count = count / ret.first;
    if (count > rows_count * ret.first)
        ++rows_count;
    ret.second = std::min(rows_count, OpenGLWrapper::max_texture_size());
    return ret;
}

void ViewerImpl::TextureData::init(size_t vertices_count)
{
    if (vertices_count == 0)
        return;

    m_width = std::min(vertices_count, OpenGLWrapper::max_texture_size());
    size_t rows_count = vertices_count / m_width;
    if (vertices_count > rows_count * m_width)
        ++rows_count;
    m_height = std::min(rows_count, OpenGLWrapper::max_texture_size());
    m_count = rows_count / m_height;
    if (rows_count > m_count * m_height)
        ++m_count;

    const std::pair<size_t, size_t> test = width_height(vertices_count);
    assert(test.first == m_width);
    assert(test.second == m_height);

    m_tex_ids = std::vector<TexIds>(m_count);
}

void ViewerImpl::TextureData::set_positions(const std::vector<Vec3>& positions)
{
    if (m_count == 0)
        return;

    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.positions.first);
        ids.positions.second = 0;
    }

    m_positions_size = 0;

    if (positions.empty())
        return;

    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));
    int curr_unpack_alignment = 0;
    glsafe(glGetIntegerv(GL_UNPACK_ALIGNMENT, &curr_unpack_alignment));

    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    const size_t tex_capacity = max_texture_capacity();
    size_t remaining = positions.size();
    for (size_t i = 0; i < m_count; ++i) {
        const auto [w, h] = width_height(std::min(remaining, tex_capacity));
        const size_t offset = i * tex_capacity;

        glsafe(glGenTextures(1, &m_tex_ids[i].positions.first));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_tex_ids[i].positions.first));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
        if (remaining >= tex_capacity) {
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGB, GL_FLOAT, &positions[offset]));
            m_tex_ids[i].positions.second = w * h;
        }
        else {
            // the last row is only partially fitted with data, send it separately
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGB, GL_FLOAT, nullptr));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RGB, GL_FLOAT, &positions[offset]));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(remaining % w), 1, GL_RGB, GL_FLOAT, &positions[offset + w * (h - 1)]));
            m_tex_ids[i].positions.second = w * (h - 1) + remaining % w;
        }
        m_positions_size += m_tex_ids[i].positions.second * sizeof(Vec3);

        remaining = (remaining > tex_capacity) ? remaining - tex_capacity: 0;
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_heights_widths_angles(const std::vector<Vec3>& heights_widths_angles)
{
    if (m_count == 0)
        return;

    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.heights_widths_angles.first);
        ids.heights_widths_angles.second = 0;
    }

    m_height_width_angle_size = 0;

    if (heights_widths_angles.empty())
        return;

    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));
    int curr_unpack_alignment = 0;
    glsafe(glGetIntegerv(GL_UNPACK_ALIGNMENT, &curr_unpack_alignment));

    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    const size_t tex_capacity = max_texture_capacity();
    size_t remaining = heights_widths_angles.size();
    for (size_t i = 0; i < m_count; ++i) {
        const auto [w, h] = width_height(std::min(remaining, tex_capacity));
        const size_t offset = i * tex_capacity;

        glsafe(glGenTextures(1, &m_tex_ids[i].heights_widths_angles.first));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_tex_ids[i].heights_widths_angles.first));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
        if (remaining >= tex_capacity) {
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGB, GL_FLOAT, &heights_widths_angles[offset]));
            m_tex_ids[i].heights_widths_angles.second = w * h;
        }
        else {
            // the last row is only partially fitted with data, send it separately
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGB, GL_FLOAT, nullptr));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RGB, GL_FLOAT, &heights_widths_angles[offset]));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(remaining % w), 1, GL_RGB, GL_FLOAT, &heights_widths_angles[offset + w * (h - 1)]));
            m_tex_ids[i].heights_widths_angles.second = w * (h - 1) + remaining % w;
        }
        m_height_width_angle_size += m_tex_ids[i].heights_widths_angles.second * sizeof(Vec3);

        remaining = (remaining > tex_capacity) ? remaining - tex_capacity : 0;
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_colors(const std::vector<float>& colors)
{
    if (m_count == 0)
        return;

    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.colors.first);
        ids.colors.second = 0;
    }

    m_colors_size = 0;

    if (colors.empty())
        return;

    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));
    int curr_unpack_alignment = 0;
    glsafe(glGetIntegerv(GL_UNPACK_ALIGNMENT, &curr_unpack_alignment));

    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    const size_t tex_capacity = max_texture_capacity();
    size_t remaining = colors.size();
    for (size_t i = 0; i < m_count; ++i) {
        const auto [w, h] = width_height(std::min(remaining, tex_capacity));
        const size_t offset = i * tex_capacity;

        glsafe(glGenTextures(1, &m_tex_ids[i].colors.first));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_tex_ids[i].colors.first));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
        if (remaining >= tex_capacity) {
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RED, GL_FLOAT, &colors[offset]));
            m_tex_ids[i].colors.second = w * h;
        }
        else {
            // the last row is only partially fitted with data, send it separately
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RED, GL_FLOAT, nullptr));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RED, GL_FLOAT, &colors[offset]));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(remaining % w), 1, GL_RED, GL_FLOAT, &colors[offset + w * (h - 1)]));
            m_tex_ids[i].colors.second = w * (h - 1) + remaining % w;
        }
        m_colors_size += m_tex_ids[i].colors.second * sizeof(float);

        remaining = (remaining > tex_capacity) ? remaining - tex_capacity : 0;
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_enabled_segments(const std::vector<uint32_t>& enabled_segments)
{
    if (m_count == 0)
        return;

    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.enabled_segments.first);
        ids.enabled_segments.second = 0;
    }

    m_enabled_segments_size = 0;

    if (enabled_segments.empty())
        return;

    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));
    int curr_unpack_alignment = 0;
    glsafe(glGetIntegerv(GL_UNPACK_ALIGNMENT, &curr_unpack_alignment));

    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    const size_t tex_capacity = max_texture_capacity();
    size_t curr_tex_id = 0;
    std::vector<uint32_t> curr_segments;
    for (size_t i = 0; i < enabled_segments.size(); ++i) {
        uint32_t seg = enabled_segments[i];
        const bool new_tex = static_cast<size_t>(seg) > (curr_tex_id + 1) * tex_capacity;
        if (!new_tex)
            curr_segments.push_back(seg - static_cast<uint32_t>(curr_tex_id * tex_capacity));
        if (i + 1 == enabled_segments.size() || new_tex) {
            const auto [w, h] = width_height(curr_segments.size());

            glsafe(glGenTextures(1, &m_tex_ids[curr_tex_id].enabled_segments.first));
            glsafe(glBindTexture(GL_TEXTURE_2D, m_tex_ids[curr_tex_id].enabled_segments.first));
            glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
            glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
            glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
            if (curr_segments.size() == tex_capacity) {
                glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, curr_segments.data()));
                m_tex_ids[curr_tex_id].enabled_segments.second = w * h;
            }
            else {
                glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));
                if (h == 1) {
                    glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, static_cast<GLsizei>(w), 1, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, curr_segments.data()));
                    m_tex_ids[curr_tex_id].enabled_segments.second = w;
                }
                else {
                    // the last row is only partially fitted with data, send it separately
                    glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RED_INTEGER, GL_UNSIGNED_INT, curr_segments.data()));
                    glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(curr_segments.size() % w), 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &curr_segments[w * (h - 1)]));
                    m_tex_ids[curr_tex_id].enabled_segments.second = w * (h - 1) + curr_segments.size() % w;
                }
            }
            m_enabled_segments_size += m_tex_ids[curr_tex_id].enabled_segments.second * sizeof(uint32_t);
            if (new_tex) {
                curr_segments.clear();
                ++curr_tex_id;
                curr_segments.push_back(seg - static_cast<uint32_t>(curr_tex_id * tex_capacity));
            }
        }
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_enabled_options(const std::vector<uint32_t>& enabled_options)
{
    if (m_count == 0)
        return;

    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.enabled_options.first);
        ids.enabled_options.second = 0;
    }

    m_enabled_options_size = 0;

    if (enabled_options.empty())
        return;

    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));
    int curr_unpack_alignment = 0;
    glsafe(glGetIntegerv(GL_UNPACK_ALIGNMENT, &curr_unpack_alignment));

    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, 1));

    const size_t tex_capacity = max_texture_capacity();
    size_t curr_tex_id = 0;
    std::vector<uint32_t> curr_options;
    for (size_t i = 0; i < enabled_options.size(); ++i) {
        uint32_t opt = enabled_options[i];
        const bool new_tex = static_cast<size_t>(opt) > (curr_tex_id + 1) * tex_capacity;
        if (!new_tex)
            curr_options.push_back(opt - static_cast<uint32_t>(curr_tex_id * tex_capacity));
        if (i + 1 == enabled_options.size() || new_tex) {
            const auto [w, h] = width_height(curr_options.size());

            glsafe(glGenTextures(1, &m_tex_ids[curr_tex_id].enabled_options.first));
            glsafe(glBindTexture(GL_TEXTURE_2D, m_tex_ids[curr_tex_id].enabled_options.first));
            glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
            glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
            glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
            if (curr_options.size() == tex_capacity) {
                glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, curr_options.data()));
                m_tex_ids[curr_tex_id].enabled_options.second = w * h;
            }
            else {
                glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr));
                if (h == 1) {
                    glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, static_cast<GLsizei>(w), 1, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, curr_options.data()));
                    m_tex_ids[curr_tex_id].enabled_options.second = w;
                }
                else {
                    // the last row is only partially fitted with data, send it separately
                    glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RED_INTEGER, GL_UNSIGNED_INT, curr_options.data()));
                    glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(curr_options.size() % w), 1, GL_RED_INTEGER, GL_UNSIGNED_INT, &curr_options[w * (h - 1)]));
                    m_tex_ids[curr_tex_id].enabled_options.second = w * (h - 1) + curr_options.size() % w;
                }
            }
            m_enabled_options_size += m_tex_ids[curr_tex_id].enabled_options.second * sizeof(uint32_t);
            if (new_tex) {
                curr_options.clear();
                ++curr_tex_id;
                curr_options.push_back(opt - static_cast<uint32_t>(curr_tex_id * tex_capacity));
            }
        }
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::reset()
{
    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.enabled_options.first);
        delete_textures(ids.enabled_segments.first);
        delete_textures(ids.colors.first);
        delete_textures(ids.heights_widths_angles.first);
        delete_textures(ids.positions.first);
    }
    m_tex_ids.clear();

    m_width = 0;
    m_height = 0;
    m_count = 0;

    m_positions_size = 0;
    m_height_width_angle_size = 0;
    m_colors_size = 0;
    m_enabled_segments_size = 0;
    m_enabled_options_size = 0;
}

std::pair<unsigned int, size_t> ViewerImpl::TextureData::get_positions_tex_id(size_t id) const
{
    assert(id < m_tex_ids.size());
    return m_tex_ids[id].positions;
}

std::pair<unsigned int, size_t> ViewerImpl::TextureData::get_heights_widths_angles_tex_id(size_t id) const
{
    assert(id < m_tex_ids.size());
    return m_tex_ids[id].heights_widths_angles;
}

std::pair<unsigned int, size_t> ViewerImpl::TextureData::get_colors_tex_id(size_t id) const
{
    assert(id < m_tex_ids.size());
    return m_tex_ids[id].colors;
}

std::pair<unsigned int, size_t> ViewerImpl::TextureData::get_enabled_segments_tex_id(size_t id) const
{
    assert(id < m_tex_ids.size());
    return m_tex_ids[id].enabled_segments;
}

std::pair<unsigned int, size_t> ViewerImpl::TextureData::get_enabled_options_tex_id(size_t id) const
{
    assert(id < m_tex_ids.size());
    return m_tex_ids[id].enabled_options;
}

size_t ViewerImpl::TextureData::get_enabled_segments_count() const
{
    size_t ret = 0;
    for (size_t i = 0; i < m_count; ++i) {
        ret += m_tex_ids[i].enabled_segments.second;
    }
    return ret;
}

size_t ViewerImpl::TextureData::get_enabled_options_count() const
{
    size_t ret = 0;
    for (size_t i = 0; i < m_count; ++i) {
        ret += m_tex_ids[i].enabled_options.second;
    }
    return ret;
}

size_t ViewerImpl::TextureData::get_used_gpu_memory() const
{
    size_t ret = 0;
    ret += m_positions_size;
    ret += m_height_width_angle_size;
    ret += m_colors_size;
    ret += m_enabled_segments_size;
    ret += m_enabled_options_size;
    return ret;
}
#endif // ENABLE_OPENGL_ES

ViewerImpl::ViewerImpl()
{
    reset_default_extrusion_roles_colors();
    reset_default_options_colors();
}

void ViewerImpl::init(const std::string& opengl_context_version)
{
    if (m_initialized)
        return;

    if (!OpenGLWrapper::load_opengl(opengl_context_version)) {
        if (OpenGLWrapper::is_valid_context())
            throw std::runtime_error("LibVGCode was unable to initialize the GLAD library.\n");
        else {
#ifdef ENABLE_OPENGL_ES
            throw std::runtime_error("LibVGCode requires an OpenGL ES context based on OpenGL ES 3.0 or higher.\n");
#else
            throw std::runtime_error("LibVGCode requires an OpenGL context based on OpenGL 3.2 or higher.\n");
#endif // ENABLE_OPENGL_ES
        }
    }

    // segments shader
#ifdef ENABLE_OPENGL_ES
    m_segments_shader_id = init_shader("segments", Segments_Vertex_Shader_ES, Segments_Fragment_Shader_ES);
#else
    m_segments_shader_id = init_shader("segments", Segments_Vertex_Shader, Segments_Fragment_Shader);
#endif // ENABLE_OPENGL_ES

    m_uni_segments_view_matrix_id            = glGetUniformLocation(m_segments_shader_id, "view_matrix");
    m_uni_segments_projection_matrix_id      = glGetUniformLocation(m_segments_shader_id, "projection_matrix");
    m_uni_segments_camera_position_id        = glGetUniformLocation(m_segments_shader_id, "camera_position");
    m_uni_segments_positions_tex_id          = glGetUniformLocation(m_segments_shader_id, "position_tex");
    m_uni_segments_height_width_angle_tex_id = glGetUniformLocation(m_segments_shader_id, "height_width_angle_tex");
    m_uni_segments_colors_tex_id             = glGetUniformLocation(m_segments_shader_id, "color_tex");
    m_uni_segments_segment_index_tex_id      = glGetUniformLocation(m_segments_shader_id, "segment_index_tex");
    glcheck();
    assert(m_uni_segments_view_matrix_id != -1 &&
           m_uni_segments_projection_matrix_id != -1 &&
           m_uni_segments_camera_position_id != -1 &&
           m_uni_segments_positions_tex_id != -1 &&
           m_uni_segments_height_width_angle_tex_id != -1 &&
           m_uni_segments_colors_tex_id != -1 &&
           m_uni_segments_segment_index_tex_id != -1);

    m_segment_template.init();

    // options shader
#ifdef ENABLE_OPENGL_ES
    m_options_shader_id = init_shader("options", Options_Vertex_Shader_ES, Options_Fragment_Shader_ES);
#else
    m_options_shader_id = init_shader("options", Options_Vertex_Shader, Options_Fragment_Shader);
#endif // ENABLE_OPENGL_ES

    m_uni_options_view_matrix_id            = glGetUniformLocation(m_options_shader_id, "view_matrix");
    m_uni_options_projection_matrix_id      = glGetUniformLocation(m_options_shader_id, "projection_matrix");
    m_uni_options_positions_tex_id          = glGetUniformLocation(m_options_shader_id, "position_tex");
    m_uni_options_height_width_angle_tex_id = glGetUniformLocation(m_options_shader_id, "height_width_angle_tex");
    m_uni_options_colors_tex_id             = glGetUniformLocation(m_options_shader_id, "color_tex");
    m_uni_options_segment_index_tex_id      = glGetUniformLocation(m_options_shader_id, "segment_index_tex");
    glcheck();
    assert(m_uni_options_view_matrix_id != -1 &&
           m_uni_options_projection_matrix_id != -1 &&
           m_uni_options_positions_tex_id != -1 &&
           m_uni_options_height_width_angle_tex_id != -1 &&
           m_uni_options_colors_tex_id != -1 &&
           m_uni_options_segment_index_tex_id != -1);

    m_option_template.init(16);

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    // cog marker shader
#ifdef ENABLE_OPENGL_ES
    m_cog_marker_shader_id = init_shader("cog_marker", Cog_Marker_Vertex_Shader_ES, Cog_Marker_Fragment_Shader_ES);
#else
    m_cog_marker_shader_id = init_shader("cog_marker", Cog_Marker_Vertex_Shader, Cog_Marker_Fragment_Shader);
#endif // ENABLE_OPENGL_ES

    m_uni_cog_marker_world_center_position = glGetUniformLocation(m_cog_marker_shader_id, "world_center_position");
    m_uni_cog_marker_scale_factor          = glGetUniformLocation(m_cog_marker_shader_id, "scale_factor");
    m_uni_cog_marker_view_matrix           = glGetUniformLocation(m_cog_marker_shader_id, "view_matrix");
    m_uni_cog_marker_projection_matrix     = glGetUniformLocation(m_cog_marker_shader_id, "projection_matrix");
    glcheck();
    assert(m_uni_cog_marker_world_center_position != -1 &&
           m_uni_cog_marker_scale_factor != -1 &&
           m_uni_cog_marker_view_matrix != -1 &&
           m_uni_cog_marker_projection_matrix != -1);

    m_cog_marker.init(32, 1.0f);

    // tool marker shader
#ifdef ENABLE_OPENGL_ES
    m_tool_marker_shader_id = init_shader("tool_marker", Tool_Marker_Vertex_Shader_ES, Tool_Marker_Fragment_Shader_ES);
#else
    m_tool_marker_shader_id = init_shader("tool_marker", Tool_Marker_Vertex_Shader, Tool_Marker_Fragment_Shader);
#endif // ENABLE_OPENGL_ES

    m_uni_tool_marker_world_origin      = glGetUniformLocation(m_tool_marker_shader_id, "world_origin");
    m_uni_tool_marker_scale_factor      = glGetUniformLocation(m_tool_marker_shader_id, "scale_factor");
    m_uni_tool_marker_view_matrix       = glGetUniformLocation(m_tool_marker_shader_id, "view_matrix");
    m_uni_tool_marker_projection_matrix = glGetUniformLocation(m_tool_marker_shader_id, "projection_matrix");
    m_uni_tool_marker_color_base        = glGetUniformLocation(m_tool_marker_shader_id, "color_base");

    glcheck();
    assert(m_uni_tool_marker_world_origin != -1 &&
           m_uni_tool_marker_scale_factor != -1 &&
           m_uni_tool_marker_view_matrix != -1 &&
           m_uni_tool_marker_projection_matrix != -1 &&
           m_uni_tool_marker_color_base != -1);

    m_tool_marker.init(32, 2.0f, 4.0f, 1.0f, 8.0f);
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

    m_initialized = true;
}

void ViewerImpl::shutdown()
{
    reset();
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    m_tool_marker.shutdown();
    m_cog_marker.shutdown();
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    m_option_template.shutdown();
    m_segment_template.shutdown();
    if (m_options_shader_id != 0) {
        glsafe(glDeleteProgram(m_options_shader_id));
        m_options_shader_id = 0;
    }
    if (m_segments_shader_id != 0) {
        glsafe(glDeleteProgram(m_segments_shader_id));
        m_segments_shader_id = 0;
    }
    m_initialized = false;
    OpenGLWrapper::unload_opengl();
}

void ViewerImpl::reset()
{
    m_layers.reset();
    m_view_range.reset();
    m_extrusion_roles.reset();
    m_options.clear();
    m_used_extruders.clear();
    m_total_time = { 0.0f, 0.0f };
    m_travels_time = { 0.0f, 0.0f };
    m_vertices.clear();
    m_vertices_colors.clear();
    m_valid_lines_bitset.clear();
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    m_cog_marker.reset();
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

#ifdef ENABLE_OPENGL_ES
    m_texture_data.reset();
#else
    m_enabled_segments_count = 0;
    m_enabled_options_count = 0;

    m_settings_used_for_ranges = std::nullopt;

    delete_textures(m_enabled_options_tex_id);
    delete_buffers(m_enabled_options_buf_id);
    delete_textures(m_enabled_segments_tex_id);
    delete_buffers(m_enabled_segments_buf_id);
    delete_textures(m_colors_tex_id);
    delete_buffers(m_colors_buf_id);
    delete_textures(m_heights_widths_angles_tex_id);
    delete_buffers(m_heights_widths_angles_buf_id);
    delete_textures(m_positions_tex_id);
    delete_buffers(m_positions_buf_id);
#endif // ENABLE_OPENGL_ES
}

// On some graphic cards texture buffers using GL_RGB32F format do not work, see:
// https://dev.prusa3d.com/browse/SPE-2411
// https://github.com/prusa3d/PrusaSlicer/issues/12908
// To let all drivers be happy, we use GL_RGBA32F format, so we need to add an extra (currently unused) float
// to position and heights_widths_angles vectors
using Vec4 = std::array<float, 4>;

static void extract_pos_and_or_hwa(const std::vector<PathVertex>& vertices, float travels_radius, float wipes_radius, BitSet<>& valid_lines_bitset,
    std::vector<Vec4>* positions = nullptr, std::vector<Vec4>* heights_widths_angles = nullptr, bool update_bitset = false) {
  static constexpr const Vec3 ZERO = { 0.0f, 0.0f, 0.0f };
    if (positions == nullptr && heights_widths_angles == nullptr)
        return;
    if (vertices.empty())
        return;
    if (travels_radius <= 0.0f || wipes_radius <= 0.0f)
        return;

    if (positions != nullptr)
        positions->reserve(vertices.size());
    if (heights_widths_angles != nullptr)
        heights_widths_angles->reserve(vertices.size());
    for (size_t i = 0; i < vertices.size(); ++i) {
        const PathVertex& v = vertices[i];
        const EMoveType move_type = v.type;
        const bool prev_line_valid = i > 0 && valid_lines_bitset[i - 1];
        const Vec3 prev_line = prev_line_valid ? v.position - vertices[i - 1].position : ZERO;
        const bool this_line_valid = i + 1 < vertices.size() &&
                                     vertices[i + 1].position != v.position &&
                                     vertices[i + 1].type == move_type &&
                                     move_type != EMoveType::Seam;
        const Vec3 this_line = this_line_valid ? vertices[i + 1].position - v.position : ZERO;

        if (this_line_valid) {
            // there is a valid path between point i and i+1.
        }
        else {
            // the connection is invalid, there should be no line rendered, ever
            if (update_bitset)
                valid_lines_bitset.reset(i);
        }
        
        if (positions != nullptr) {
            // the last component is a dummy float to comply with GL_RGBA32F format
            Vec4 position = { v.position[0], v.position[1], v.position[2], 0.0f };
            if (move_type == EMoveType::Extrude)
                // push down extrusion vertices by half height to render them at the right z
                position[2] -= 0.5f * v.height;
            positions->emplace_back(position);
        }

        if (heights_widths_angles != nullptr) {
            float height = 0.0f;
            float width = 0.0f;
            if (v.is_travel()) {
                height = travels_radius;
                width  = travels_radius;
            }
            else if (v.is_wipe()) {
                height = wipes_radius;
                width  = wipes_radius;
            }
            else {
                height = v.height;
                width = v.width;
            }
            // the last component is a dummy float to comply with GL_RGBA32F format
            heights_widths_angles->push_back({ height, width,
                std::atan2(prev_line[0] * this_line[1] - prev_line[1] * this_line[0], dot(prev_line, this_line)), 0.0f });
        }
    }
}

void ViewerImpl::load(GCodeInputData&& gcode_data)
{
    if (!m_initialized)
        return;

    if (gcode_data.vertices.empty())
        return;

    reset();

    m_vertices = std::move(gcode_data.vertices);
    m_tool_colors = std::move(gcode_data.tools_colors);
    m_color_print_colors = std::move(gcode_data.color_print_colors);
    m_vertices_colors.resize(m_vertices.size());

    m_settings.spiral_vase_mode = gcode_data.spiral_vase_mode;

    for (size_t i = 0; i < m_vertices.size(); ++i) {
        const PathVertex& v = m_vertices[i];

        m_layers.update(v, static_cast<uint32_t>(i));

        for (size_t j = 0; j < TIME_MODES_COUNT; ++j) {
            m_total_time[j] += v.times[j];
            if (v.type == EMoveType::Travel)
                m_travels_time[j] += v.times[j];
        }

        const EOptionType option_type = move_type_to_option(v.type);
        if (option_type != EOptionType::COUNT)
            m_options.emplace_back(option_type);

        if (v.type == EMoveType::Extrude) {
            m_extrusion_roles.add(v.role, v.times);

            auto estruder_it = m_used_extruders.find(v.extruder_id);
            if (estruder_it == m_used_extruders.end())
                estruder_it = m_used_extruders.insert({ v.extruder_id, std::vector<ColorPrint>() }).first;
            if (estruder_it->second.empty() || estruder_it->second.back().color_id != v.color_id) {
                const ColorPrint cp = { v.extruder_id, v.color_id, v.layer_id, m_total_time };
                estruder_it->second.emplace_back(cp);
            }
        }

        if (i > 0) {
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
            // updates calculation for center of gravity
            if (v.type == EMoveType::Extrude &&
                v.role != EGCodeExtrusionRole::Skirt &&
                v.role != EGCodeExtrusionRole::SupportMaterial &&
                v.role != EGCodeExtrusionRole::SupportMaterialInterface &&
                v.role != EGCodeExtrusionRole::WipeTower &&
                v.role != EGCodeExtrusionRole::Custom &&
                v.role != EGCodeExtrusionRole::Brim &&
                v.role != EGCodeExtrusionRole::SupportTransition) {
                m_cog_marker.update(0.5f * (v.position + m_vertices[i - 1].position), v.weight);
            }
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
        }
    }

    if (!m_layers.empty())
        m_layers.set_view_range(0, static_cast<uint32_t>(m_layers.count()) - 1);

    std::sort(m_options.begin(), m_options.end());
    m_options.erase(std::unique(m_options.begin(), m_options.end()), m_options.end());
    m_options.shrink_to_fit();

    // reset segments visibility bitset
    m_valid_lines_bitset = BitSet<>(m_vertices.size());
    m_valid_lines_bitset.setAll();

    if (m_settings.time_mode != ETimeMode::Normal && m_total_time[static_cast<size_t>(m_settings.time_mode)] == 0.0f)
        m_settings.time_mode = ETimeMode::Normal;

    // buffers to send to gpu
    // the last component is a dummy float to comply with GL_RGBA32F format
    std::vector<Vec4> positions;
    std::vector<Vec4> heights_widths_angles;
    positions.reserve(m_vertices.size());
    heights_widths_angles.reserve(m_vertices.size());
    extract_pos_and_or_hwa(m_vertices, m_travels_radius, m_wipes_radius, m_valid_lines_bitset, &positions, &heights_widths_angles, true);

    if (!positions.empty()) {
#ifdef ENABLE_OPENGL_ES
        m_texture_data.init(positions.size());
        // create and fill position textures
        m_texture_data.set_positions(positions);
        // create and fill height, width and angle textures
        m_texture_data.set_heights_widths_angles(heights_widths_angles);
#else
        m_positions_tex_size = positions.size() * sizeof(Vec3);
        m_height_width_angle_tex_size = heights_widths_angles.size() * sizeof(Vec3);

        int old_bound_texture = 0;
        glsafe(glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &old_bound_texture));

        // create and fill positions buffer
        glsafe(glGenBuffers(1, &m_positions_buf_id));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_positions_buf_id));
        glsafe(glBufferData(GL_TEXTURE_BUFFER, positions.size() * sizeof(Vec4), positions.data(), GL_STATIC_DRAW));
        glsafe(glGenTextures(1, &m_positions_tex_id));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_positions_tex_id));

        // create and fill height, width and angles buffer
        glsafe(glGenBuffers(1, &m_heights_widths_angles_buf_id));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_heights_widths_angles_buf_id));
        glsafe(glBufferData(GL_TEXTURE_BUFFER, heights_widths_angles.size() * sizeof(Vec4), heights_widths_angles.data(), GL_DYNAMIC_DRAW));
        glsafe(glGenTextures(1, &m_heights_widths_angles_tex_id));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_heights_widths_angles_tex_id));

        // create (but do not fill) colors buffer (data is set in update_colors())
        glsafe(glGenBuffers(1, &m_colors_buf_id));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_colors_buf_id));
        glsafe(glGenTextures(1, &m_colors_tex_id));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_colors_tex_id));

        // create (but do not fill) enabled segments buffer (data is set in update_enabled_entities())
        glsafe(glGenBuffers(1, &m_enabled_segments_buf_id));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_enabled_segments_buf_id));
        glsafe(glGenTextures(1, &m_enabled_segments_tex_id));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_enabled_segments_tex_id));

        // create (but do not fill) enabled options buffer (data is set in update_enabled_entities())
        glsafe(glGenBuffers(1, &m_enabled_options_buf_id));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_enabled_options_buf_id));
        glsafe(glGenTextures(1, &m_enabled_options_tex_id));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_enabled_options_tex_id));

        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, 0));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, old_bound_texture));
#endif // ENABLE_OPENGL_ES
    }

    update_view_full_range();
    m_view_range.set_visible(m_view_range.get_enabled());
    update_enabled_entities();
    update_colors();
}

void ViewerImpl::update_enabled_entities()
{
    if (m_vertices.empty())
        return;

    std::vector<uint32_t> enabled_segments;
    std::vector<uint32_t> enabled_options;
    Interval range = m_view_range.get_visible();

    // when top layer only visualization is enabled, we need to render
    // all the toolpaths in the other layers as grayed, so extend the range
    // to contain them
    if (m_settings.top_layer_only_view_range)
        range[0] = m_view_range.get_full()[0];

    // to show the options at the current tool marker position we need to extend the range by one extra step
    if (m_vertices[range[1]].is_option() && range[1] < static_cast<uint32_t>(m_vertices.size()) - 1)
        ++range[1];

    if (m_settings.spiral_vase_mode) {
        // when spiral vase mode is enabled and only one layer is shown, extend the range by one step
        const Interval& layers_range = m_layers.get_view_range();
        if (layers_range[0] > 0 && layers_range[0] == layers_range[1])
            --range[0];
    }

    for (size_t i = range[0]; i < range[1]; ++i) {
        const PathVertex& v = m_vertices[i];

        if (!m_valid_lines_bitset[i] && !v.is_option())
            continue;
        if (v.is_travel()) {
            if (!m_settings.options_visibility[size_t(EOptionType::Travels)])
                continue;
        }
        else if (v.is_wipe()) {
            if (!m_settings.options_visibility[size_t(EOptionType::Wipes)])
                continue;
        }
        else if (v.is_option()) {
            if (!m_settings.options_visibility[size_t(move_type_to_option(v.type))])
                continue;
        }
        else if (v.is_extrusion()) {
            if (!m_settings.extrusion_roles_visibility[size_t(v.role)])
                continue;
        }
        else
            continue;

        if (v.is_option())
            enabled_options.push_back(static_cast<uint32_t>(i));
        else
            enabled_segments.push_back(static_cast<uint32_t>(i));
    }

#ifdef ENABLE_OPENGL_ES
    m_texture_data.set_enabled_segments(enabled_segments);
    m_texture_data.set_enabled_options(enabled_options);
#else
    m_enabled_segments_count = enabled_segments.size();
    m_enabled_options_count = enabled_options.size();

    m_enabled_segments_tex_size = enabled_segments.size() * sizeof(uint32_t);
    m_enabled_options_tex_size = enabled_options.size() * sizeof(uint32_t);

    // update gpu buffer for enabled segments
    assert(m_enabled_segments_buf_id > 0);
    glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_enabled_segments_buf_id));
    if (!enabled_segments.empty())
        glsafe(glBufferData(GL_TEXTURE_BUFFER, enabled_segments.size() * sizeof(uint32_t), enabled_segments.data(), GL_STATIC_DRAW));
    else
        glsafe(glBufferData(GL_TEXTURE_BUFFER, 0, nullptr, GL_STATIC_DRAW));

    // update gpu buffer for enabled options
    assert(m_enabled_options_buf_id > 0);
    glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_enabled_options_buf_id));
    if (!enabled_options.empty())
        glsafe(glBufferData(GL_TEXTURE_BUFFER, enabled_options.size() * sizeof(uint32_t), enabled_options.data(), GL_STATIC_DRAW));
    else
        glsafe(glBufferData(GL_TEXTURE_BUFFER, 0, nullptr, GL_STATIC_DRAW));

    glsafe(glBindBuffer(GL_TEXTURE_BUFFER, 0));
#endif // ENABLE_OPENGL_ES

    m_settings.update_enabled_entities = false;
}

static float encode_color(const Color& color) {
    const int r = static_cast<int>(color[0]);
    const int g = static_cast<int>(color[1]);
    const int b = static_cast<int>(color[2]);
    const int i_color = r << 16 | g << 8 | b;
    return static_cast<float>(i_color);
}


void ViewerImpl::update_colors_texture()
{
#if !defined(ENABLE_OPENGL_ES)
    if (m_colors_buf_id == 0)
        return;
#endif // ENABLE_OPENGL_ES

    const size_t top_layer_id = m_settings.top_layer_only_view_range ? m_layers.get_view_range()[1] : 0;
    const bool color_top_layer_only = m_view_range.get_full()[1] != m_view_range.get_visible()[1];

    // Based on current settings and slider position, we might want to render some
    // vertices as dark grey. Use either that or the normal color (from the cache).
    std::vector<float> colors(m_vertices_colors.size());
    assert(colors.size() == m_vertices.size() && m_vertices_colors.size() == m_vertices.size());
    for (size_t i=0; i<m_vertices.size(); ++i)
        colors[i] = (color_top_layer_only && m_vertices[i].layer_id < top_layer_id &&
                    (!m_settings.spiral_vase_mode || i != m_view_range.get_enabled()[0])) ?
                    encode_color(DUMMY_COLOR) : m_vertices_colors[i];

    #ifdef ENABLE_OPENGL_ES
        if (!colors.empty())
            // update gpu buffer for colors
            m_texture_data.set_colors(colors);
    #else
        m_colors_tex_size = colors.size() * sizeof(float);

        // update gpu buffer for colors
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_colors_buf_id));
        glsafe(glBufferData(GL_TEXTURE_BUFFER, colors.size() * sizeof(float), colors.data(), GL_STATIC_DRAW));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, 0));
    #endif // ENABLE_OPENGL_ES
}


void ViewerImpl::update_colors()
{


    if (!m_used_extruders.empty()) {
        // ensure that the number of defined tool colors matches the max id of the used extruders 
        const size_t max_used_extruder_id = 1 + static_cast<size_t>(m_used_extruders.rbegin()->first);
        const size_t tool_colors_size = m_tool_colors.size();
        if (m_tool_colors.size() < max_used_extruder_id) {
            for (size_t i = 0; i < max_used_extruder_id - tool_colors_size; ++i) {
                m_tool_colors.emplace_back(DUMMY_COLOR);
            }
        }
    }

    update_color_ranges();
    
    // Recalculate "normal" colors of all the vertices for current view settings.
    // If some part of the preview should be rendered in dark grey, it is taken
    // care of in update_colors_texture. That is to avoid the need to recalculate
    // the "normal" color on every slider move.
    for (size_t i = 0; i < m_vertices.size(); ++i)
        m_vertices_colors[i] = encode_color(get_vertex_color(m_vertices[i]));
    
    update_colors_texture();
    m_settings.update_colors = false;
}

void ViewerImpl::render(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    if (m_settings.update_view_full_range)
        update_view_full_range();

    if (m_settings.update_enabled_entities)
        update_enabled_entities();

    if (m_settings.update_colors)
        update_colors();

    const Mat4x4 inv_view_matrix = inverse(view_matrix);
    const Vec3 camera_position = { inv_view_matrix[12], inv_view_matrix[13], inv_view_matrix[14] };
    render_segments(view_matrix, projection_matrix, camera_position);
    render_options(view_matrix, projection_matrix);

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    if (m_settings.options_visibility[size_t(EOptionType::ToolMarker)])
        render_tool_marker(view_matrix, projection_matrix);
    if (m_settings.options_visibility[size_t(EOptionType::CenterOfGravity)])
        render_cog_marker(view_matrix, projection_matrix);
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
}

void ViewerImpl::set_view_type(EViewType type)
{
    m_settings.view_type = type;
    m_settings.update_colors = true;
}

void ViewerImpl::set_time_mode(ETimeMode mode)
{
    m_settings.time_mode = mode;
    m_settings.update_colors = true;
}

void ViewerImpl::set_layers_view_range(Interval::value_type min, Interval::value_type max)
{
    min = std::clamp<Interval::value_type>(min, 0, m_layers.count() - 1);
    max = std::clamp<Interval::value_type>(max, 0, m_layers.count() - 1);
    m_layers.set_view_range(min, max);
    // force immediate update of the full range
    update_view_full_range();
    m_view_range.set_visible(m_view_range.get_enabled());
    m_settings.update_enabled_entities = true;
    //m_settings.update_colors = true;
    update_colors_texture();
}

void ViewerImpl::toggle_top_layer_only_view_range()
{
    m_settings.top_layer_only_view_range = !m_settings.top_layer_only_view_range;
    update_view_full_range();
    m_view_range.set_visible(m_view_range.get_enabled());
    m_settings.update_enabled_entities = true;
    //m_settings.update_colors = true;
    update_colors_texture();
}

std::vector<ETimeMode> ViewerImpl::get_time_modes() const
{
    std::vector<ETimeMode> ret;
    for (size_t i = 0; i < TIME_MODES_COUNT; ++i) {
        if (std::accumulate(m_vertices.begin(), m_vertices.end(), 0.0f,
            [i](float a, const PathVertex& v) { return a + v.times[i]; }) > 0.0f)
            ret.push_back(static_cast<ETimeMode>(i));
    }
    return ret;
}

std::vector<uint8_t> ViewerImpl::get_used_extruders_ids() const
{
    std::vector<uint8_t> ret;
    ret.reserve(m_used_extruders.size());
    for (const auto& [id, colors] : m_used_extruders) {
        ret.emplace_back(id);
    }
    return ret;
}

size_t ViewerImpl::get_color_prints_count(uint8_t extruder_id) const
{
    const auto it = m_used_extruders.find(extruder_id);
    return (it == m_used_extruders.end()) ? 0 : it->second.size();
}

std::vector<ColorPrint> ViewerImpl::get_color_prints(uint8_t extruder_id) const
{
    const auto it = m_used_extruders.find(extruder_id);
    return (it == m_used_extruders.end()) ? std::vector<ColorPrint>() : it->second;
}

AABox ViewerImpl::get_bounding_box(const std::vector<EMoveType>& types) const
{
    Vec3 min = { FLT_MAX, FLT_MAX, FLT_MAX };
    Vec3 max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const PathVertex& v : m_vertices) {
        if (std::find(types.begin(), types.end(), v.type) != types.end()) {
            for (int j = 0; j < 3; ++j) {
                min[j] = std::min(min[j], v.position[j]);
                max[j] = std::max(max[j], v.position[j]);
            }
        }
    }
    return { min, max };
}

AABox ViewerImpl::get_extrusion_bounding_box(const std::vector<EGCodeExtrusionRole>& roles) const
{
    Vec3 min = { FLT_MAX, FLT_MAX, FLT_MAX };
    Vec3 max = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const PathVertex& v : m_vertices) {
        if (v.is_extrusion() && std::find(roles.begin(), roles.end(), v.role) != roles.end()) {
            for (int j = 0; j < 3; ++j) {
              min[j] = std::min(min[j], v.position[j]);
              max[j] = std::max(max[j], v.position[j]);
            }
        }
    }
    return { min, max };
}

bool ViewerImpl::is_option_visible(EOptionType type) const
{
    return m_settings.options_visibility[size_t(type)];
}

void ViewerImpl::toggle_option_visibility(EOptionType type)
{
    m_settings.options_visibility[size_t(type)] = ! m_settings.options_visibility[size_t(type)];
    const Interval old_enabled_range = m_view_range.get_enabled();
    update_view_full_range();
    const Interval& new_enabled_range = m_view_range.get_enabled();
    if (old_enabled_range != new_enabled_range) {
        const Interval& visible_range = m_view_range.get_visible();
        if (old_enabled_range == visible_range)
            m_view_range.set_visible(new_enabled_range);
        else if (m_settings.top_layer_only_view_range && new_enabled_range[0] < visible_range[0])
            m_view_range.set_visible(new_enabled_range[0], visible_range[1]);
    }
    m_settings.update_enabled_entities = true;
    m_settings.update_colors = true;
}

bool ViewerImpl::is_extrusion_role_visible(EGCodeExtrusionRole role) const
{
    return m_settings.extrusion_roles_visibility[size_t(role)];
}

void ViewerImpl::toggle_extrusion_role_visibility(EGCodeExtrusionRole role)
{
    m_settings.extrusion_roles_visibility[size_t(role)] = ! m_settings.extrusion_roles_visibility[size_t(role)];
    update_view_full_range();
    m_settings.update_enabled_entities = true;
    m_settings.update_colors = true;
}

void ViewerImpl::set_view_visible_range(Interval::value_type min, Interval::value_type max)
{
    // force update of the full range, to avoid clamping the visible range with full old values
    // when calling m_view_range.set_visible()
    update_view_full_range();
    m_view_range.set_visible(min, max);
    update_enabled_entities();
    //m_settings.update_colors = true;
    update_colors_texture();
}

float ViewerImpl::get_estimated_time_at(size_t id) const
{
    return std::accumulate(m_vertices.begin(), m_vertices.begin() + id + 1, 0.0f, 
        [this](float a, const PathVertex& v) { return a + v.times[static_cast<size_t>(m_settings.time_mode)]; });
}

Color ViewerImpl::get_vertex_color(const PathVertex& v) const
{
    if (v.type == EMoveType::Noop)
        return DUMMY_COLOR;

    if ((v.is_wipe() && (m_settings.view_type != EViewType::Speed && m_settings.view_type != EViewType::ActualSpeed)) || v.is_option())
        return get_option_color(move_type_to_option(v.type));

    switch (m_settings.view_type)
    {
    case EViewType::FeatureType:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : get_extrusion_role_color(v.role);
    }
    case EViewType::Height:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_height_range.get_color_at(v.height);
    }
    case EViewType::Width:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_width_range.get_color_at(v.width);
    }
    case EViewType::Speed:
    {
        return m_speed_range.get_color_at(v.feedrate);
    }
    case EViewType::ActualSpeed:
    {
        return m_actual_speed_range.get_color_at(v.actual_feedrate);
    }
    case EViewType::FanSpeed:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_fan_speed_range.get_color_at(v.fan_speed);
    }
    case EViewType::Temperature:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_temperature_range.get_color_at(v.temperature);
    }
    case EViewType::VolumetricFlowRate:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_volumetric_rate_range.get_color_at(v.volumetric_rate());
    }
    case EViewType::ActualVolumetricFlowRate:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_actual_volumetric_rate_range.get_color_at(v.actual_volumetric_rate());
    }
    case EViewType::LayerTimeLinear:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) :
            m_layer_time_range[0].get_color_at(m_layers.get_layer_time(m_settings.time_mode, static_cast<size_t>(v.layer_id)));
    }
    case EViewType::LayerTimeLogarithmic:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) :
            m_layer_time_range[1].get_color_at(m_layers.get_layer_time(m_settings.time_mode, static_cast<size_t>(v.layer_id)));
    }
    case EViewType::Tool:
    {
        assert(static_cast<size_t>(v.extruder_id) < m_tool_colors.size());
        return m_tool_colors[v.extruder_id];
    }
    case EViewType::Summary: // ORCA
    case EViewType::ColorPrint:
    {
        return m_layers.layer_contains_colorprint_options(static_cast<size_t>(v.layer_id)) ? DUMMY_COLOR :
            m_color_print_colors[static_cast<size_t>(v.color_id) % m_color_print_colors.size()];
    }
    default: { break; }
    }

    return DUMMY_COLOR;
}

void ViewerImpl::set_tool_colors(const Palette& colors)
{
    m_tool_colors = colors;
    m_settings.update_colors = true;
}

void ViewerImpl::set_color_print_colors(const Palette& colors)
{
    m_color_print_colors = colors;
    m_settings.update_colors = true;
}

const Color& ViewerImpl::get_extrusion_role_color(EGCodeExtrusionRole role) const
{
    return m_extrusion_roles_colors[size_t(role)];
}

void ViewerImpl::set_extrusion_role_color(EGCodeExtrusionRole role, const Color& color)
{
    m_extrusion_roles_colors[size_t(role)] = color;
    m_settings.update_colors = true;
}

void ViewerImpl::reset_default_extrusion_roles_colors()
{
    m_extrusion_roles_colors = DEFAULT_EXTRUSION_ROLES_COLORS;
}

const Color& ViewerImpl::get_option_color(EOptionType type) const
{
    return m_options_colors[size_t(type)];
}

void ViewerImpl::set_option_color(EOptionType type, const Color& color)
{
    m_options_colors[size_t(type)] = color;
    m_settings.update_colors = true;
}

void ViewerImpl::reset_default_options_colors()
{
    m_options_colors = DEFAULT_OPTIONS_COLORS;
}

const ColorRange& ViewerImpl::get_color_range(EViewType type) const
{
    switch (type)
    {
    case EViewType::Height:                   { return m_height_range; }
    case EViewType::Width:                    { return m_width_range; }
    case EViewType::Speed:                    { return m_speed_range; }
    case EViewType::ActualSpeed:              { return m_actual_speed_range; }
    case EViewType::FanSpeed:                 { return m_fan_speed_range; }
    case EViewType::Temperature:              { return m_temperature_range; }
    case EViewType::VolumetricFlowRate:       { return m_volumetric_rate_range; }
    case EViewType::ActualVolumetricFlowRate: { return m_actual_volumetric_rate_range; }
    case EViewType::LayerTimeLinear:          { return m_layer_time_range[0]; }
    case EViewType::LayerTimeLogarithmic:     { return m_layer_time_range[1]; }
    default:                                  { return ColorRange::DUMMY_COLOR_RANGE; }
    }
}

void ViewerImpl::set_color_range_palette(EViewType type, const Palette& palette)
{
    switch (type)
    {
    case EViewType::Height:                   { m_height_range.set_palette(palette);          break; }
    case EViewType::Width:                    { m_width_range.set_palette(palette);           break; }
    case EViewType::Speed:                    { m_speed_range.set_palette(palette);           break; }
    case EViewType::ActualSpeed:              { m_actual_speed_range.set_palette(palette);    break; }
    case EViewType::FanSpeed:                 { m_fan_speed_range.set_palette(palette);       break; }
    case EViewType::Temperature:              { m_temperature_range.set_palette(palette);     break; }
    case EViewType::VolumetricFlowRate:       { m_volumetric_rate_range.set_palette(palette); break; }
    case EViewType::ActualVolumetricFlowRate: { m_actual_volumetric_rate_range.set_palette(palette); break; }
    case EViewType::LayerTimeLinear:          { m_layer_time_range[0].set_palette(palette);   break; }
    case EViewType::LayerTimeLogarithmic:     { m_layer_time_range[1].set_palette(palette);   break; }
    default:                                  { break; }
    }
    m_settings.update_colors = true;
}

void ViewerImpl::set_travels_radius(float radius)
{
    m_travels_radius = std::clamp(radius, MIN_TRAVELS_RADIUS_MM, MAX_TRAVELS_RADIUS_MM);
    update_heights_widths();
}

void ViewerImpl::set_wipes_radius(float radius)
{
    m_wipes_radius = std::clamp(radius, MIN_WIPES_RADIUS_MM, MAX_WIPES_RADIUS_MM);
    update_heights_widths();
}

size_t ViewerImpl::get_used_cpu_memory() const
{
    size_t ret = sizeof(*this);
    ret += m_layers.size_in_bytes_cpu();
    ret += STDVEC_MEMSIZE(m_options, EOptionType);
    ret += m_used_extruders.size() * sizeof(std::map<uint8_t, ColorPrint>::value_type);
    ret += sizeof(m_extrusion_roles_colors);
    ret += sizeof(m_options_colors);
    ret += STDVEC_MEMSIZE(m_vertices, PathVertex);
    ret += m_valid_lines_bitset.size_in_bytes_cpu();
    ret += m_height_range.size_in_bytes_cpu();
    ret += m_width_range.size_in_bytes_cpu();
    ret += m_speed_range.size_in_bytes_cpu();
    ret += m_actual_speed_range.size_in_bytes_cpu();
    ret += m_fan_speed_range.size_in_bytes_cpu();
    ret += m_temperature_range.size_in_bytes_cpu();
    ret += m_volumetric_rate_range.size_in_bytes_cpu();
    ret += m_actual_volumetric_rate_range.size_in_bytes_cpu();
    for (size_t i = 0; i < COLOR_RANGE_TYPES_COUNT; ++i) {
        ret += m_layer_time_range[i].size_in_bytes_cpu();
    }
    ret += STDVEC_MEMSIZE(m_tool_colors, Color);
    ret += STDVEC_MEMSIZE(m_color_print_colors, Color);
    return ret;
}

size_t ViewerImpl::get_used_gpu_memory() const
{
    size_t ret = 0;
    ret += m_segment_template.size_in_bytes_gpu();
    ret += m_option_template.size_in_bytes_gpu();
#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
    ret += m_tool_marker.size_in_bytes_gpu();
    ret += m_cog_marker.size_in_bytes_gpu();
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS
#ifdef ENABLE_OPENGL_ES
    ret += m_texture_data.get_used_gpu_memory();
#else
    ret += m_positions_tex_size;
    ret += m_height_width_angle_tex_size;
    ret += m_colors_tex_size;
    ret += m_enabled_segments_tex_size;
    ret += m_enabled_options_tex_size;
#endif // ENABLE_OPENGL_ES
    return ret;
}

static bool is_visible(const PathVertex& v, const Settings& settings)
{
    const EOptionType option_type = move_type_to_option(v.type);
    try
    {
        return (option_type == EOptionType::COUNT) ?
            (v.type == EMoveType::Extrude) ? settings.extrusion_roles_visibility[size_t(v.role)] : false :
            settings.options_visibility[size_t(option_type)];
    }
    catch (...)
    {
        return false;
    }
}

void ViewerImpl::update_view_full_range()
{
    const Interval& layers_range = m_layers.get_view_range();
    const bool travels_visible = m_settings.options_visibility[size_t(EOptionType::Travels)];
    const bool wipes_visible   = m_settings.options_visibility[size_t(EOptionType::Wipes)];

    auto first_it = m_vertices.begin();
    while (first_it != m_vertices.end() &&
           (first_it->layer_id < layers_range[0] || !is_visible(*first_it, m_settings))) {
        ++first_it;
    }

    // If the first vertex is an extrusion, add an extra step to properly detect the first segment
    if (first_it != m_vertices.begin() && first_it != m_vertices.end() && first_it->type == EMoveType::Extrude)
        --first_it;

    if (first_it == m_vertices.end())
        m_view_range.set_full(Range());
    else {
        if (travels_visible || wipes_visible) {
            // if the global range starts with a travel/wipe move, extend it to the travel/wipe start
            while (first_it != m_vertices.begin() &&
                   ((travels_visible && first_it->is_travel()) ||
                    (wipes_visible && first_it->is_wipe()))) {
                --first_it;
            }
        }

        auto last_it = first_it;
        while (last_it != m_vertices.end() && last_it->layer_id <= layers_range[1]) {
            ++last_it;
        }
        if (last_it != first_it)
            --last_it;

        // remove disabled trailing options, if any 
        auto rev_first_it = std::make_reverse_iterator(first_it);
        if (rev_first_it != m_vertices.rbegin())
            --rev_first_it;
        auto rev_last_it = std::make_reverse_iterator(last_it);
        if (rev_last_it != m_vertices.rbegin())
            --rev_last_it;

        bool reduced = false;
        while (rev_last_it != rev_first_it && !is_visible(*rev_last_it, m_settings)) {
            ++rev_last_it;
            reduced = true;
        }

        if (reduced && rev_last_it != m_vertices.rend())
            last_it = rev_last_it.base() - 1;

        if (travels_visible || wipes_visible) {
            // if the global range ends with a travel/wipe move, extend it to the travel/wipe end
            while (last_it != m_vertices.end() && last_it + 1 != m_vertices.end() &&
                   ((travels_visible && last_it->is_travel() && (last_it + 1)->is_travel()) ||
                    (wipes_visible && last_it->is_wipe() && (last_it + 1)->is_wipe()))) {
                  ++last_it;
            }
        }

        if (first_it != last_it)
            m_view_range.set_full(std::distance(m_vertices.begin(), first_it), std::distance(m_vertices.begin(), last_it));
        else
            m_view_range.set_full(Range());

        if (m_settings.top_layer_only_view_range) {
            const Interval& full_range = m_view_range.get_full();
            auto top_first_it = m_vertices.begin() + full_range[0];
            bool shortened = false;
            while (top_first_it != m_vertices.end() && (top_first_it->layer_id < layers_range[1] || !is_visible(*top_first_it, m_settings))) {
                ++top_first_it;
                shortened = true;
            }
            if (shortened)
                --top_first_it;

            // when spiral vase mode is enabled and only one layer is shown, extend the range by one step
            if (m_settings.spiral_vase_mode && layers_range[0] > 0 && layers_range[0] == layers_range[1])
                --top_first_it;
            m_view_range.set_enabled(std::distance(m_vertices.begin(), top_first_it), full_range[1]);
        }
        else
            m_view_range.set_enabled(m_view_range.get_full());
    }

    m_settings.update_view_full_range = false;
}

void ViewerImpl::update_color_ranges()
{
    // Color ranges do not need to be recalculated that often. If the following settings are the same
    // as last time, the current ranges are still valid. The recalculation is quite expensive.
    if (m_settings_used_for_ranges.has_value() &&
        m_settings.extrusion_roles_visibility == m_settings_used_for_ranges->extrusion_roles_visibility &&
        m_settings.options_visibility == m_settings_used_for_ranges->options_visibility)
        return;

    m_width_range.reset();
    m_height_range.reset();
    m_speed_range.reset();
    m_actual_speed_range.reset();
    m_fan_speed_range.reset();
    m_temperature_range.reset();
    m_volumetric_rate_range.reset();
    m_actual_volumetric_rate_range.reset();
    m_layer_time_range[0].reset(); // ColorRange::EType::Linear
    m_layer_time_range[1].reset(); // ColorRange::EType::Logarithmic

    for (size_t i = 0; i < m_vertices.size(); i++) {
        const PathVertex& v = m_vertices[i];
        if (v.is_extrusion()) {
            m_height_range.update(round_to_bin(v.height));
            if (!v.is_custom_gcode() || m_settings.extrusion_roles_visibility[size_t(EGCodeExtrusionRole::Custom)]) {
                m_width_range.update(round_to_bin(v.width));
                m_volumetric_rate_range.update(round_to_bin(v.volumetric_rate()));
                m_actual_volumetric_rate_range.update(round_to_bin(v.actual_volumetric_rate()));
            }
            m_fan_speed_range.update(round_to_bin(v.fan_speed));
            m_temperature_range.update(round_to_bin(v.temperature));
        }
        if ((v.is_travel() && m_settings.options_visibility[size_t(EOptionType::Travels)]) ||
            (v.is_wipe() && m_settings.options_visibility[size_t(EOptionType::Wipes)]) ||
             v.is_extrusion()) {
            m_speed_range.update(v.feedrate);
            m_actual_speed_range.update(v.actual_feedrate);
        }
    }

    const std::vector<float> times = m_layers.get_times(m_settings.time_mode);
    for (size_t i = 0; i < m_layer_time_range.size(); ++i) {
        for (float t : times) {
            m_layer_time_range[i].update(t);
        }
    }

    m_settings_used_for_ranges = m_settings;
}

void ViewerImpl::update_heights_widths()
{
#ifdef ENABLE_OPENGL_ES
    std::vector<Vec3> heights_widths_angles;
    heights_widths_angles.reserve(m_vertices.size());
    extract_pos_and_or_hwa(m_vertices, m_travels_radius, m_wipes_radius, m_valid_lines_bitset, nullptr, &heights_widths_angles);
    m_texture_data.set_heights_widths_angles(heights_widths_angles);
#else
    if (m_heights_widths_angles_buf_id == 0)
        return;

    glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_heights_widths_angles_buf_id));

    Vec3* buffer = static_cast<Vec3*>(glMapBuffer(GL_TEXTURE_BUFFER, GL_WRITE_ONLY));
    glcheck();

    for (size_t i = 0; i < m_vertices.size(); ++i) {
        const PathVertex& v = m_vertices[i];
        if (v.is_travel()) {
            buffer[i][0] = m_travels_radius;
            buffer[i][1] = m_travels_radius;
        }
        else if (v.is_wipe()) {
            buffer[i][0] = m_wipes_radius;
            buffer[i][1] = m_wipes_radius;
        }
    }

    glsafe(glUnmapBuffer(GL_TEXTURE_BUFFER));
    glsafe(glBindBuffer(GL_TEXTURE_BUFFER, 0));
#endif // ENABLE_OPENGL_ES
}

void ViewerImpl::render_segments(const Mat4x4& view_matrix, const Mat4x4& projection_matrix, const Vec3& camera_position)
{
    if (m_segments_shader_id == 0)
        return;

#ifdef ENABLE_OPENGL_ES
    if (m_texture_data.get_enabled_segments_count() == 0)
#else
    if (m_enabled_segments_count == 0)
#endif // ENABLE_OPENGL_ES
        return;

    int curr_active_texture = 0;
    glsafe(glGetIntegerv(GL_ACTIVE_TEXTURE, &curr_active_texture));
    int curr_shader;
    glsafe(glGetIntegerv(GL_CURRENT_PROGRAM, &curr_shader));
    const bool curr_cull_face = glIsEnabled(GL_CULL_FACE);
    glcheck();

    glsafe(glUseProgram(m_segments_shader_id));

    glsafe(glUniform1i(m_uni_segments_positions_tex_id, 0));
    glsafe(glUniform1i(m_uni_segments_height_width_angle_tex_id, 1));
    glsafe(glUniform1i(m_uni_segments_colors_tex_id, 2));
    glsafe(glUniform1i(m_uni_segments_segment_index_tex_id, 3));
    glsafe(glUniformMatrix4fv(m_uni_segments_view_matrix_id, 1, GL_FALSE, view_matrix.data()));
    glsafe(glUniformMatrix4fv(m_uni_segments_projection_matrix_id, 1, GL_FALSE, projection_matrix.data()));
    glsafe(glUniform3fv(m_uni_segments_camera_position_id, 1, camera_position.data()));

    glsafe(glDisable(GL_CULL_FACE));

#ifdef ENABLE_OPENGL_ES
    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));

    for (size_t i = 0; i < m_texture_data.get_count(); ++i) {
        const auto [id, count] = m_texture_data.get_enabled_segments_tex_id(i);
        if (count == 0)
            continue;
        glsafe(glActiveTexture(GL_TEXTURE0));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_texture_data.get_positions_tex_id(i).first));
        glsafe(glActiveTexture(GL_TEXTURE1));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_texture_data.get_heights_widths_angles_tex_id(i).first));
        glsafe(glActiveTexture(GL_TEXTURE2));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_texture_data.get_colors_tex_id(i).first));
        glsafe(glActiveTexture(GL_TEXTURE3));
        glsafe(glBindTexture(GL_TEXTURE_2D, id));
        m_segment_template.render(count);
    }
#else
    std::array<int, 4> curr_bound_texture = { 0, 0, 0, 0 };
    for (int i = 0; i < curr_bound_texture.size(); ++i) {
        glsafe(glActiveTexture(GL_TEXTURE0 + i));
        glsafe(glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &curr_bound_texture[i]));
        //assert(curr_bound_texture[i] == 0);
    }

    glsafe(glActiveTexture(GL_TEXTURE0));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_positions_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_positions_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE1));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_heights_widths_angles_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_heights_widths_angles_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE2));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_colors_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, m_colors_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE3));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_enabled_segments_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, m_enabled_segments_buf_id));

    m_segment_template.render(m_enabled_segments_count);
#endif // ENABLE_OPENGL_ES

    if (curr_cull_face)
        glsafe(glEnable(GL_CULL_FACE));

    glsafe(glUseProgram(curr_shader));
#ifdef ENABLE_OPENGL_ES
    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
#else
    for (int i = 0; i < curr_bound_texture.size(); ++i) {
        glsafe(glActiveTexture(GL_TEXTURE0 + i));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, curr_bound_texture[i]));
    }
#endif // ENABLE_OPENGL_ES
    glsafe(glActiveTexture(curr_active_texture));
}

void ViewerImpl::render_options(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    if (m_options_shader_id == 0)
        return;

#ifdef ENABLE_OPENGL_ES
    if (m_texture_data.get_enabled_options_count() == 0)
#else
    if (m_enabled_options_count == 0)
#endif // ENABLE_OPENGL_ES
        return;

    int curr_active_texture = 0;
    glsafe(glGetIntegerv(GL_ACTIVE_TEXTURE, &curr_active_texture));
    int curr_shader;
    glsafe(glGetIntegerv(GL_CURRENT_PROGRAM, &curr_shader));
    const bool curr_cull_face = glIsEnabled(GL_CULL_FACE);
    glcheck();

    glsafe(glUseProgram(m_options_shader_id));

    glsafe(glUniform1i(m_uni_options_positions_tex_id, 0));
    glsafe(glUniform1i(m_uni_options_height_width_angle_tex_id, 1));
    glsafe(glUniform1i(m_uni_options_colors_tex_id, 2));
    glsafe(glUniform1i(m_uni_options_segment_index_tex_id, 3));
    glsafe(glUniformMatrix4fv(m_uni_options_view_matrix_id, 1, GL_FALSE, view_matrix.data()));
    glsafe(glUniformMatrix4fv(m_uni_options_projection_matrix_id, 1, GL_FALSE, projection_matrix.data()));

    glsafe(glEnable(GL_CULL_FACE));

#ifdef ENABLE_OPENGL_ES
    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));

    for (size_t i = 0; i < m_texture_data.get_count(); ++i) {
        const auto [id, count] = m_texture_data.get_enabled_options_tex_id(i);
        if (count == 0)
            continue;
        glsafe(glActiveTexture(GL_TEXTURE0));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_texture_data.get_positions_tex_id(i).first));
        glsafe(glActiveTexture(GL_TEXTURE1));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_texture_data.get_heights_widths_angles_tex_id(i).first));
        glsafe(glActiveTexture(GL_TEXTURE2));
        glsafe(glBindTexture(GL_TEXTURE_2D, m_texture_data.get_colors_tex_id(i).first));
        glsafe(glActiveTexture(GL_TEXTURE3));
        glsafe(glBindTexture(GL_TEXTURE_2D, id));
        m_option_template.render(count);
    }
#else
    std::array<int, 4> curr_bound_texture = { 0, 0, 0, 0 };
    for (int i = 0; i < curr_bound_texture.size(); ++i) {
        glsafe(glActiveTexture(GL_TEXTURE0 + i));
        glsafe(glGetIntegerv(GL_TEXTURE_BINDING_BUFFER, &curr_bound_texture[i]));
        //assert(curr_bound_texture[i] == 0);
    }

    glsafe(glActiveTexture(GL_TEXTURE0));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_positions_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_positions_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE1));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_heights_widths_angles_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, m_heights_widths_angles_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE2));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_colors_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, m_colors_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE3));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_enabled_options_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, m_enabled_options_buf_id));

    m_option_template.render(m_enabled_options_count);
#endif // ENABLE_OPENGL_ES

    if (!curr_cull_face)
        glsafe(glDisable(GL_CULL_FACE));

    glsafe(glUseProgram(curr_shader));
#ifdef ENABLE_OPENGL_ES
    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
#else
    for (int i = 0; i < curr_bound_texture.size(); ++i) {
        glsafe(glActiveTexture(GL_TEXTURE0 + i));
        glsafe(glBindTexture(GL_TEXTURE_BUFFER, curr_bound_texture[i]));
    }
#endif // ENABLE_OPENGL_ES
    glsafe(glActiveTexture(curr_active_texture));
}

#if VGCODE_ENABLE_COG_AND_TOOL_MARKERS
void ViewerImpl::render_cog_marker(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    if (m_cog_marker_shader_id == 0)
        return;

    int curr_shader;
    glsafe(glGetIntegerv(GL_CURRENT_PROGRAM, &curr_shader));
    const bool curr_cull_face = glIsEnabled(GL_CULL_FACE);
    const bool curr_depth_test = glIsEnabled(GL_DEPTH_TEST);
    glcheck();

    glsafe(glEnable(GL_CULL_FACE));
    glsafe(glDisable(GL_DEPTH_TEST));

    glsafe(glUseProgram(m_cog_marker_shader_id));

    glsafe(glUniform3fv(m_uni_cog_marker_world_center_position, 1, m_cog_marker.get_position().data()));
    glsafe(glUniform1f(m_uni_cog_marker_scale_factor, m_cog_marker_scale_factor));
    glsafe(glUniformMatrix4fv(m_uni_cog_marker_view_matrix, 1, GL_FALSE, view_matrix.data()));
    glsafe(glUniformMatrix4fv(m_uni_cog_marker_projection_matrix, 1, GL_FALSE, projection_matrix.data()));

    m_cog_marker.render();

    if (curr_depth_test)
        glsafe(glEnable(GL_DEPTH_TEST));
    if (!curr_cull_face)
        glsafe(glDisable(GL_CULL_FACE));

    glsafe(glUseProgram(curr_shader));
}

void ViewerImpl::render_tool_marker(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    if (m_tool_marker_shader_id == 0)
        return;

    if (m_view_range.get_visible()[1] == m_view_range.get_enabled()[1])
        return;

    m_tool_marker.set_position(get_current_vertex().position);

    int curr_shader;
    glsafe(glGetIntegerv(GL_CURRENT_PROGRAM, &curr_shader));
    const bool curr_cull_face = glIsEnabled(GL_CULL_FACE);
    GLboolean curr_depth_mask;
    glsafe(glGetBooleanv(GL_DEPTH_WRITEMASK, &curr_depth_mask));
    const bool curr_blend = glIsEnabled(GL_BLEND);
    glcheck();
    int curr_blend_func;
    glsafe(glGetIntegerv(GL_BLEND_SRC_ALPHA, &curr_blend_func));

    glsafe(glDisable(GL_CULL_FACE));
    glsafe(glDepthMask(GL_FALSE));
    glsafe(glEnable(GL_BLEND));
    glsafe(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    glsafe(glUseProgram(m_tool_marker_shader_id));

    const Vec3& origin = m_tool_marker.get_position();
    const Vec3 offset = { 0.0f, 0.0f, m_tool_marker.get_offset_z() };
    const Vec3 position = origin + offset;
    glsafe(glUniform3fv(m_uni_tool_marker_world_origin, 1, position.data()));
    glsafe(glUniform1f(m_uni_tool_marker_scale_factor, m_tool_marker_scale_factor));
    glsafe(glUniformMatrix4fv(m_uni_tool_marker_view_matrix, 1, GL_FALSE, view_matrix.data()));
    glsafe(glUniformMatrix4fv(m_uni_tool_marker_projection_matrix, 1, GL_FALSE, projection_matrix.data()));
    const Color& color = m_tool_marker.get_color();
    glsafe(glUniform4f(m_uni_tool_marker_color_base, color[0], color[1], color[2], m_tool_marker.get_alpha()));

    m_tool_marker.render();

    glsafe(glBlendFunc(GL_SRC_ALPHA, curr_blend_func));
    if (!curr_blend)
        glsafe(glDisable(GL_BLEND));
    if (curr_depth_mask == GL_TRUE)
        glsafe(glDepthMask(GL_TRUE));
    if (curr_cull_face)
        glsafe(glEnable(GL_CULL_FACE));

    glsafe(glUseProgram(curr_shader));
}
#endif // VGCODE_ENABLE_COG_AND_TOOL_MARKERS

} // namespace libvgcode
