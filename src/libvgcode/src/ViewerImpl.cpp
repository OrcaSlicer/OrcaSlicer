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

#include <cstdint>
#include "../include/Types.hpp"
#include <array>
#include "../include/PathVertex.hpp"
#include "Bitset.hpp"
#include "../include/ColorPrint.hpp"
#include <cfloat>
#include <iterator>
#include <map>
#include <assert.h>
#include <optional>
#include <stdexcept>
#include <cstdio>
#include <string>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <type_traits>
#include <vector>
#include <utility>

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

// ORCA: fragment_outputs lists the outputs of a fragment shader that writes more than one, in location order
unsigned int init_shader(const std::string& shader_name, const char* vertex_shader, const char* fragment_shader,
                         const std::vector<const char*>& fragment_outputs = {})
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
#ifndef ENABLE_OPENGL_ES
    for (size_t i = 0; i < fragment_outputs.size(); ++i)
        glsafe(glBindFragDataLocation(shader_id, static_cast<GLuint>(i), fragment_outputs[i]));
#endif // ENABLE_OPENGL_ES
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

#ifndef ENABLE_OPENGL_ES
// ORCA: the opaque depth, then the accumulated colors of the translucent segments, see render_translucent().
// Past the units render_segments() and the shadow map take.
static constexpr int TRANSLUCENT_TEXTURE_UNIT = 5;

// ORCA: the source with a #define after its #version line
static std::string with_define(const char* source, const char* define)
{
    const std::string src(source);
    const size_t body = src.find('\n') + 1;
    return src.substr(0, body) + "#define " + define + "\n" + src.substr(body);
}
#endif // ENABLE_OPENGL_ES

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

void ViewerImpl::TextureData::set_positions(const std::vector<Vec4>& positions)
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
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_FLOAT, &positions[offset]));
            m_tex_ids[i].positions.second = w * h;
        }
        else {
            // the last row is only partially fitted with data, send it separately
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_FLOAT, nullptr));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RGBA, GL_FLOAT, &positions[offset]));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(remaining % w), 1, GL_RGBA, GL_FLOAT, &positions[offset + w * (h - 1)]));
            m_tex_ids[i].positions.second = w * (h - 1) + remaining % w;
        }
        m_positions_size += m_tex_ids[i].positions.second * sizeof(Vec4);

        remaining = (remaining > tex_capacity) ? remaining - tex_capacity: 0;
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_heights_widths_angles(const std::vector<Vec4>& heights_widths_angles)
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
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_FLOAT, &heights_widths_angles[offset]));
            m_tex_ids[i].heights_widths_angles.second = w * h;
        }
        else {
            // the last row is only partially fitted with data, send it separately
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_FLOAT, nullptr));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RGBA, GL_FLOAT, &heights_widths_angles[offset]));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(remaining % w), 1, GL_RGBA, GL_FLOAT, &heights_widths_angles[offset + w * (h - 1)]));
            m_tex_ids[i].heights_widths_angles.second = w * (h - 1) + remaining % w;
        }
        m_height_width_angle_size += m_tex_ids[i].heights_widths_angles.second * sizeof(Vec4);

        remaining = (remaining > tex_capacity) ? remaining - tex_capacity : 0;
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_colors(const std::vector<ColorWithAlpha>& colors)
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
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, &colors[offset]));
            m_tex_ids[i].colors.second = w * h;
        }
        else {
            // the last row is only partially fitted with data, send it separately
            glsafe(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h - 1), GL_RGBA, GL_UNSIGNED_BYTE, &colors[offset]));
            glsafe(glTexSubImage2D(GL_TEXTURE_2D, 0, 0, static_cast<GLsizei>(h - 1), static_cast<GLsizei>(remaining % w), 1, GL_RGBA, GL_UNSIGNED_BYTE, &colors[offset + w * (h - 1)]));
            m_tex_ids[i].colors.second = w * (h - 1) + remaining % w;
        }
        m_colors_size += m_tex_ids[i].colors.second * sizeof(ColorWithAlpha);

        remaining = (remaining > tex_capacity) ? remaining - tex_capacity : 0;
    }

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_bound_texture));
    glsafe(glPixelStorei(GL_UNPACK_ALIGNMENT, curr_unpack_alignment));
}

void ViewerImpl::TextureData::set_enabled_segments(const std::vector<uint32_t>& enabled_segments, const std::vector<ColorWithAlpha>& colors, bool sort_by_opacity)
{
    if (m_count == 0)
        return;

    for (TexIds& ids : m_tex_ids) {
        delete_textures(ids.enabled_segments.first);
        ids.enabled_segments.second = 0;
        ids.enabled_translucent_segments = 0;
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
            if (sort_by_opacity) {
                const size_t offset = curr_tex_id * tex_capacity;
                const auto translucent_begin = std::stable_partition(curr_segments.begin(), curr_segments.end(),
                    [&colors, offset](uint32_t s) { return colors[offset + s][3] == 255; });
                m_tex_ids[curr_tex_id].enabled_translucent_segments = static_cast<size_t>(std::distance(translucent_begin, curr_segments.end()));
            }
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

void ViewerImpl::SegmentsUniforms::init(unsigned int shader_id)
{
    view_matrix            = glGetUniformLocation(shader_id, "view_matrix");
    projection_matrix      = glGetUniformLocation(shader_id, "projection_matrix");
    camera_position        = glGetUniformLocation(shader_id, "camera_position");
    positions_tex          = glGetUniformLocation(shader_id, "position_tex");
    height_width_angle_tex = glGetUniformLocation(shader_id, "height_width_angle_tex");
    colors_tex             = glGetUniformLocation(shader_id, "color_tex");
    segment_index_tex      = glGetUniformLocation(shader_id, "segment_index_tex");
    reverse_order          = glGetUniformLocation(shader_id, "reverse_order");
    instances_count        = glGetUniformLocation(shader_id, "instances_count");
    first_instance         = glGetUniformLocation(shader_id, "first_instance");
    // ORCA: section view
    clipping_plane         = glGetUniformLocation(shader_id, "clipping_plane");
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
    m_segments_shader_id = init_shader("segments", Segments_Vertex_Shader, Segments_Fragment_Shader, { "fragment_color", "oit_reveal", "oit_front" });
    m_uni_segments_oit_pass_id        = glGetUniformLocation(m_segments_shader_id, "oit_pass");
    m_uni_segments_opaque_depth_id    = glGetUniformLocation(m_segments_shader_id, "opaque_depth");
    m_uni_segments_front_depth_id     = glGetUniformLocation(m_segments_shader_id, "front_depth");
    m_uni_segments_oit_depth_range_id = glGetUniformLocation(m_segments_shader_id, "oit_depth_range");
    m_translucent_composite_shader_id = init_shader("translucent_composite", Translucent_Composite_Vertex_Shader, Translucent_Composite_Fragment_Shader);
    m_uni_translucent_composite_accum_tex_id  = glGetUniformLocation(m_translucent_composite_shader_id, "accum_tex");
    m_uni_translucent_composite_reveal_tex_id = glGetUniformLocation(m_translucent_composite_shader_id, "reveal_tex");
    m_uni_translucent_composite_front_tex_id  = glGetUniformLocation(m_translucent_composite_shader_id, "front_tex");
    m_uni_translucent_composite_origin_id     = glGetUniformLocation(m_translucent_composite_shader_id, "origin");
    const std::string depth_vs = with_define(Segments_Vertex_Shader, "DEPTH_ONLY");
    const std::string depth_fs = with_define(Segments_Fragment_Shader, "DEPTH_ONLY");
    m_segments_depth_shader_id = init_shader("segments_depth", depth_vs.c_str(), depth_fs.c_str(), { "fragment_color", "oit_reveal", "oit_front" });
    m_uni_segments_depth.init(m_segments_depth_shader_id);
    m_uni_segments_depth_opaque_depth_id = glGetUniformLocation(m_segments_depth_shader_id, "opaque_depth");
#endif // ENABLE_OPENGL_ES

    m_uni_segments.init(m_segments_shader_id);
    // ORCA: realistic view
    m_uni_segments_shadow_map_id             = glGetUniformLocation(m_segments_shader_id, "shadow_map");
    m_uni_segments_shadow_light_vp_id        = glGetUniformLocation(m_segments_shader_id, "shadow_light_vp");
    m_uni_segments_shadow_intensity_id       = glGetUniformLocation(m_segments_shader_id, "shadow_intensity");
    m_uni_segments_shadow_map_texel_id       = glGetUniformLocation(m_segments_shader_id, "shadow_map_texel");
    m_uni_segments_exposure_id               = glGetUniformLocation(m_segments_shader_id, "exposure");
    m_uni_segments_saturation_id             = glGetUniformLocation(m_segments_shader_id, "saturation");
    m_uni_segments_light_top_dir_id          = glGetUniformLocation(m_segments_shader_id, "light_top_dir");
    m_uni_segments_gloss_id                  = glGetUniformLocation(m_segments_shader_id, "gloss_strength");
    m_uni_segments_layer_colors.init(m_segments_shader_id);
    glcheck();
    assert(m_uni_segments.view_matrix != -1 &&
           m_uni_segments.projection_matrix != -1 &&
           m_uni_segments.camera_position != -1 &&
           m_uni_segments.positions_tex != -1 &&
           m_uni_segments.height_width_angle_tex != -1 &&
           m_uni_segments.colors_tex != -1 &&
           m_uni_segments.segment_index_tex != -1 &&
           m_uni_segments.reverse_order != -1 &&
           m_uni_segments.instances_count != -1);

    // ORCA: realistic view
#ifdef ENABLE_OPENGL_ES
    m_segments_caster_shader_id = init_shader("segments_shadow_caster", Segments_Shadow_Caster_Vertex_Shader_ES, Segments_Shadow_Caster_Fragment_Shader_ES);
#else
    m_segments_caster_shader_id = init_shader("segments_shadow_caster", Segments_Shadow_Caster_Vertex_Shader, Segments_Shadow_Caster_Fragment_Shader);
#endif // ENABLE_OPENGL_ES
    m_uni_segments_caster.init(m_segments_caster_shader_id);
    // The caster pulls its vertices from gl_VertexID, but a core profile needs a vertex array bound to draw.
    glsafe(glGenVertexArrays(1, &m_segments_caster_vao_id));
    glcheck();

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
    // ORCA: section view
    m_uni_options_clipping_plane_id         = glGetUniformLocation(m_options_shader_id, "clipping_plane");
    m_uni_options_layer_colors.init(m_options_shader_id);
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
    if (m_segments_caster_shader_id != 0) {
        glsafe(glDeleteProgram(m_segments_caster_shader_id));
        m_segments_caster_shader_id = 0;
    }
    if (m_segments_caster_vao_id != 0) {
        glsafe(glDeleteVertexArrays(1, &m_segments_caster_vao_id));
        m_segments_caster_vao_id = 0;
    }
#ifndef ENABLE_OPENGL_ES
    if (m_translucent_composite_shader_id != 0) {
        glsafe(glDeleteProgram(m_translucent_composite_shader_id));
        m_translucent_composite_shader_id = 0;
    }
    if (m_segments_depth_shader_id != 0) {
        glsafe(glDeleteProgram(m_segments_depth_shader_id));
        m_segments_depth_shader_id = 0;
    }
#endif // ENABLE_OPENGL_ES
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
    // swap rather than clear: these are sized by the print, and a reset means the memory
    // should go back, not sit reserved until the next load
    std::vector<PathVertex>().swap(m_vertices);
    std::vector<ColorWithAlpha>().swap(m_vertices_colors);
    m_has_translucent_colors = false;
    m_enabled_translucent_segments_count = 0;
    m_extrusions_box = { Vec3{ 0.0f, 0.0f, 0.0f }, Vec3{ 0.0f, 0.0f, 0.0f } };
    for (std::vector<float>& times : m_layer_start_times)
        std::vector<float>().swap(times);
    std::vector<uint32_t>().swap(m_layer_first_vertex);
    m_layers_in_vertex_order = false;
    // BitSet::clear() only zeroes the bits, it keeps the blocks allocated; load() builds a new
    // bitset anyway and it is never read while m_vertices is empty
    m_valid_lines_bitset = BitSet<>();
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
    m_translucent_targets.release();
#endif // ENABLE_OPENGL_ES
}

// On some graphic cards texture buffers using GL_RGB32F format do not work, see:
// https://dev.prusa3d.com/browse/SPE-2411
// https://github.com/prusa3d/PrusaSlicer/issues/12908
// To let all drivers be happy, we use GL_RGBA32F format, so we need to add an extra float
// to position and heights_widths_angles vectors

// whether a line is drawn from vertex i to vertex i + 1
static bool is_line(const std::vector<PathVertex>& vertices, size_t i)
{
    return i + 1 < vertices.size() && vertices[i + 1].position != vertices[i].position &&
           vertices[i + 1].type == vertices[i].type && vertices[i].type != EMoveType::Seam;
}

// ORCA: whether two lines meet at vertex i, also across the vertex repeated in place that starts a path split
// where a value the preview colors by changes, see starts_path() in LibVGCodeWrapper.cpp
static bool is_joint(const std::vector<PathVertex>& vertices, size_t i)
{
    auto repeated = [&vertices](size_t j) {
        return j + 1 < vertices.size() && vertices[j + 1].position == vertices[j].position && vertices[j + 1].type == vertices[j].type;
    };
    const bool before = i > 0 && (is_line(vertices, i - 1) || (i > 1 && repeated(i - 1) && is_line(vertices, i - 2)));
    const bool after = is_line(vertices, i) || (repeated(i) && is_line(vertices, i + 1));
    return before && after;
}

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
        const bool this_line_valid = is_line(vertices, i);
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
            // ORCA: the last component, there to comply with GL_RGBA32F format, carries the layer the shaders grey and dim by,
            // plus 0.5 at the joint of two segments, which the shaders read as an int, see joint_a in Segments_Vertex_Shader
            Vec4 position = { v.position[0], v.position[1], v.position[2],
                              static_cast<float>(v.layer_id) + (is_joint(vertices, i) ? 0.5f : 0.0f) };
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
            
            // ORCA: Set bias for wipes and options to avoid z-fighting
            float bias = 0.0f;
            if (v.is_wipe())
                bias = 0.05f;
            else if (v.is_option())
                bias = 0.1f;

            // the last component is a dummy float to comply with GL_RGBA32F format
            // ORCA: Pass bias to shader
            heights_widths_angles->push_back({ height, width,
                std::atan2(prev_line[0] * this_line[1] - prev_line[1] * this_line[0], dot(prev_line, this_line)), bias });
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

    m_extrusions_box = get_bounding_box({ EMoveType::Extrude });
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

    // Populate layer_duration for each vertex from the accumulated layer times
    for (PathVertex& v : m_vertices) {
        v.layer_duration = m_layers.get_layer_time(m_settings.time_mode, static_cast<size_t>(v.layer_id));
    }

    // Index of the first vertex of each layer, walked back to front so that a layer with no
    // vertex of its own inherits the next layer's index and the array stays non-decreasing.
    if (!m_layers.empty()) {
        const uint32_t vertices_count = static_cast<uint32_t>(m_vertices.size());
        m_layer_first_vertex.assign(m_layers.count(), vertices_count);
        for (uint32_t i = vertices_count; i > 0; --i) {
            const uint32_t layer_id = m_vertices[i - 1].layer_id;
            if (layer_id < m_layer_first_vertex.size())
                m_layer_first_vertex[layer_id] = i - 1;
        }
        for (size_t i = m_layer_first_vertex.size() - 1; i > 0; --i)
            m_layer_first_vertex[i - 1] = std::min(m_layer_first_vertex[i - 1], m_layer_first_vertex[i]);
        m_layers_in_vertex_order = std::is_sorted(m_vertices.begin(), m_vertices.end(),
            [](const PathVertex& a, const PathVertex& b) { return a.layer_id < b.layer_id; });

        // the running time at each layer's first vertex, summed in vertex order so that
        // get_estimated_time_at() matches a full accumulation exactly
        std::array<float, TIME_MODES_COUNT> running{};
        for (std::vector<float>& times : m_layer_start_times)
            times.assign(m_layer_first_vertex.size(), 0.0f);
        size_t layer = 0;
        for (size_t i = 0; i <= m_vertices.size(); ++i) {
            for (; layer < m_layer_first_vertex.size() && m_layer_first_vertex[layer] == i; ++layer) {
                for (size_t j = 0; j < TIME_MODES_COUNT; ++j)
                    m_layer_start_times[j][layer] = running[j];
            }
            if (i < m_vertices.size()) {
                for (size_t j = 0; j < TIME_MODES_COUNT; ++j)
                    running[j] += m_vertices[i].times[j];
            }
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
    update_colors();
    update_enabled_entities();
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

    // ORCA: so that each pass of render() draws only its own segments, in print order
    m_enabled_translucent_segments_count = 0;
#ifdef ENABLE_OPENGL_ES
    m_texture_data.set_enabled_segments(enabled_segments, m_vertices_colors, m_has_translucent_colors);
    for (size_t i = 0; i < m_texture_data.get_count(); ++i)
        m_enabled_translucent_segments_count += m_texture_data.get_enabled_translucent_segments_count(i);
    m_texture_data.set_enabled_options(enabled_options);
#else
    if (m_has_translucent_colors) {
        const auto is_opaque = [this](uint32_t id) { return m_vertices_colors[id][3] == 255; };
        const auto translucent_begin = std::stable_partition(enabled_segments.begin(), enabled_segments.end(), is_opaque);
        m_enabled_translucent_segments_count = static_cast<size_t>(std::distance(translucent_begin, enabled_segments.end()));
    }

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

void ViewerImpl::update_colors_texture()
{
#if !defined(ENABLE_OPENGL_ES)
    if (m_colors_buf_id == 0)
        return;
#endif // ENABLE_OPENGL_ES

    // ORCA: the colors of the vertices alone; the layers greyed or dimmed around the one the sliders
    // show are left to the shaders, see set_layer_colors(), so that a slider step uploads nothing.
    #ifdef ENABLE_OPENGL_ES
        if (!m_vertices_colors.empty())
            // update gpu buffer for colors
            m_texture_data.set_colors(m_vertices_colors);
    #else
        m_colors_tex_size = m_vertices_colors.size() * sizeof(ColorWithAlpha);

        // update gpu buffer for colors
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, m_colors_buf_id));
        glsafe(glBufferData(GL_TEXTURE_BUFFER, m_colors_tex_size, m_vertices_colors.data(), GL_STATIC_DRAW));
        glsafe(glBindBuffer(GL_TEXTURE_BUFFER, 0));
    #endif // ENABLE_OPENGL_ES
}

void ViewerImpl::LayerColorsUniforms::init(unsigned int shader_id)
{
    lit_layers       = glGetUniformLocation(shader_id, "lit_layers");
    grey_below_layer = glGetUniformLocation(shader_id, "grey_below_layer");
    dim_brightness   = glGetUniformLocation(shader_id, "dim_brightness");
    kept_vertex      = glGetUniformLocation(shader_id, "kept_vertex");
}

void ViewerImpl::set_layer_colors(const LayerColorsUniforms& uni) const
{
    const size_t top_layer_id = m_settings.top_layer_only_view_range ? m_layers.get_view_range()[1] : 0;
    const bool color_top_layer_only = m_view_range.get_full()[1] != m_view_range.get_visible()[1];

    // ORCA: when dim_previous_layers is enabled, darken every layer (keeping its color) except the
    // one(s) the layer slider is being scrubbed to, so that only those are shown at full brightness.
    // A slider thumb marks a layer as inspected only once it is moved away from
    // its end of the print: the upper one while it is below the last layer (or while the moves
    // slider is not at the end of the layer), the lower one while it is above the first layer, so
    // trimming the print from the bottom lights up the lowest visible layer and using the slider as
    // a range lights up both ends. When neither thumb is moved the whole print is rendered normally.
    // Gated on top-layer-only mode, which the greying path below also keys off of, so that the moves
    // slider still animates normally across all layers when that mode is disabled.
    const Interval& layers_range = m_layers.get_view_range();
    const bool inspecting_top_layer = layers_range[1] + 1 < m_layers.count() || color_top_layer_only;
    const bool inspecting_bottom_layer = layers_range[0] > 0;
    const bool dim_previous_layers = m_settings.dim_previous_layers && m_settings.top_layer_only_view_range &&
                                     !m_layers.empty() && (inspecting_top_layer || inspecting_bottom_layer);

    // Without dimming, the layers below the top one go dark grey while the moves slider is short of
    // the end. Neither touches the inspected layers, nor the first vertex of a spiral vase layer.
    glsafe(glUniform2i(uni.lit_layers, dim_previous_layers && inspecting_bottom_layer ? static_cast<int>(layers_range[0]) : -1,
                                       dim_previous_layers && inspecting_top_layer ? static_cast<int>(layers_range[1]) : -1));
    glsafe(glUniform1i(uni.grey_below_layer, color_top_layer_only ? static_cast<int>(top_layer_id) : 0));
    glsafe(glUniform1f(uni.dim_brightness, dim_previous_layers ? m_settings.dim_previous_layers_brightness : -1.0f));
    glsafe(glUniform1i(uni.kept_vertex, m_settings.spiral_vase_mode ? static_cast<int>(m_view_range.get_enabled()[0]) : -1));
}

void ViewerImpl::update_colors()
{


    if (!m_used_extruders.empty()) {
        // ensure that the number of defined tool colors matches the max id of the used extruders 
        const size_t max_used_extruder_id = 1 + static_cast<size_t>(m_used_extruders.rbegin()->first);
        const size_t tool_colors_size = m_tool_colors.size();
        if (m_tool_colors.size() < max_used_extruder_id) {
            for (size_t i = 0; i < max_used_extruder_id - tool_colors_size; ++i) {
                m_tool_colors.push_back({ DUMMY_COLOR[0], DUMMY_COLOR[1], DUMMY_COLOR[2], 255 });
            }
        }
    }

    update_color_ranges();
    
    // Recalculate "normal" colors of all the vertices for current view settings.
    // If some part of the preview should be rendered in dark grey, it is taken
    // care of in the shaders, see set_layer_colors(). That is to avoid the need to recalculate
    // the "normal" color on every slider move.
    const bool had_translucent_colors = m_has_translucent_colors;
    m_has_translucent_colors = false;
    for (size_t i = 0; i < m_vertices.size(); ++i) {
        const Color color = get_vertex_color(m_vertices[i]);
        const uint8_t alpha = get_vertex_alpha(m_vertices[i]);
        m_vertices_colors[i] = { color[0], color[1], color[2], alpha };
        m_has_translucent_colors |= alpha < 255;
    }

    update_colors_texture();
    m_settings.update_colors = false;

    // ORCA: the enabled segments are sorted by opacity
    if (had_translucent_colors || m_has_translucent_colors)
        m_settings.update_enabled_entities = true;
}

void ViewerImpl::apply_pending_updates()
{
    if (m_settings.update_view_full_range)
        update_view_full_range();

    // ORCA: the colors first, which decide the order of the enabled segments
    if (m_settings.update_colors)
        update_colors();

    if (m_settings.update_enabled_entities)
        update_enabled_entities();
}

void ViewerImpl::render(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    apply_pending_updates();

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

void ViewerImpl::render_shadow_casters(const Mat4x4& view_matrix, const Mat4x4& projection_matrix, const Vec3& light_position)
{
    apply_pending_updates();

    // Only the extrusions and travels cast: the option markers are indicators, not material.
    m_rendering_shadow_casters = true;
    render_segments(view_matrix, projection_matrix, light_position);
    m_rendering_shadow_casters = false;
}

void ViewerImpl::set_shadow_map(int texture_unit, const Mat4x4& light_view_projection, float intensity, float texel_size)
{
    m_shadow_map_texture_unit = texture_unit;
    m_shadow_light_vp = light_view_projection;
    m_shadow_intensity = intensity;
    m_shadow_map_texel = texel_size;
}

void ViewerImpl::set_tone(float exposure, float saturation)
{
    m_exposure = exposure;
    m_saturation = saturation;
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
    // Update layer_duration for all vertices based on the new time mode
    for (PathVertex& v : m_vertices) {
        v.layer_duration = m_layers.get_layer_time(mode, static_cast<size_t>(v.layer_id));
    }
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
}

void ViewerImpl::toggle_top_layer_only_view_range()
{
    m_settings.top_layer_only_view_range = !m_settings.top_layer_only_view_range;
    update_view_full_range();
    m_view_range.set_visible(m_view_range.get_enabled());
    m_settings.update_enabled_entities = true;
}

// ORCA: enable/disable darkening of the layers the layer slider is not scrubbed to
void ViewerImpl::set_dim_previous_layers(bool value)
{
    if (m_settings.dim_previous_layers == value)
        return;
    m_settings.dim_previous_layers = value;
}

void ViewerImpl::set_translucent_filaments(bool value)
{
    if (m_settings.translucent_filaments == value)
        return;
    m_settings.translucent_filaments = value;
    m_settings.update_colors = true;
}

// ORCA: set how bright the darkened layers are rendered, 1.0 = unchanged, 0.0 = black
void ViewerImpl::set_dim_previous_layers_brightness(float value)
{
    value = std::clamp(value, 0.0f, 1.0f);
    if (m_settings.dim_previous_layers_brightness == value)
        return;
    m_settings.dim_previous_layers_brightness = value;
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
}

float ViewerImpl::get_estimated_time_at(size_t id) const
{
    const size_t mode = static_cast<size_t>(m_settings.time_mode);
    if (mode >= TIME_MODES_COUNT || id >= m_vertices.size())
        return 0.0f;
    size_t first = 0;
    float time = 0.0f;
    const size_t layer = static_cast<size_t>(m_vertices[id].layer_id);
    if (layer < m_layer_first_vertex.size() && m_layer_first_vertex[layer] <= id) {
        first = m_layer_first_vertex[layer];
        time = m_layer_start_times[mode][layer];
    }
    for (size_t i = first; i <= id; ++i)
        time += m_vertices[i].times[mode];
    return time;
}

Color ViewerImpl::get_vertex_color(const PathVertex& v) const
{
    if (v.type == EMoveType::Noop)
        return DUMMY_COLOR;

    if ((v.is_wipe() && (m_settings.view_type != EViewType::Speed && m_settings.view_type != EViewType::ActualSpeed && m_settings.view_type != EViewType::Acceleration && m_settings.view_type != EViewType::Jerk)) || v.is_option())
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
// ORCA: Add Pressure Advance visualization support
    case EViewType::PressureAdvance:
    {
        return v.is_travel() ? get_option_color(move_type_to_option(v.type)) : m_pressure_advance_range.get_color_at(v.pressure_advance);
    }
    // ORCA: Add Acceleration visualization support
    case EViewType::Acceleration:
    {
        return m_acceleration_range.get_color_at(v.acceleration);
    }
    // ORCA: Add Jerk visualization support
    case EViewType::Jerk:
    {
        return m_jerk_range.get_color_at(v.jerk);
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
        const ColorWithAlpha& color = m_tool_colors[v.extruder_id];
        return { color[0], color[1], color[2] };
    }
    case EViewType::Summary: // ORCA
    case EViewType::ColorPrint:
    {
        if (m_layers.layer_contains_colorprint_options(static_cast<size_t>(v.layer_id)))
            return DUMMY_COLOR;
        const ColorWithAlpha& color = m_color_print_colors[static_cast<size_t>(v.color_id) % m_color_print_colors.size()];
        return { color[0], color[1], color[2] };
    }
    default: { break; }
    }

    return DUMMY_COLOR;
}

// ORCA: only the extrusions colored by filament can be translucent.
uint8_t ViewerImpl::get_vertex_alpha(const PathVertex& v) const
{
    if (!m_settings.translucent_filaments || !v.is_extrusion())
        return 255;

    switch (m_settings.view_type)
    {
    case EViewType::Tool:
        return m_tool_colors[v.extruder_id][3];
    case EViewType::Summary:
    case EViewType::ColorPrint:
        return m_layers.layer_contains_colorprint_options(static_cast<size_t>(v.layer_id)) ? 255 :
            m_color_print_colors[static_cast<size_t>(v.color_id) % m_color_print_colors.size()][3];
    default:
        return 255;
    }
}

void ViewerImpl::set_tool_colors(const PaletteWithAlpha& colors)
{
    m_tool_colors = colors;
    m_settings.update_colors = true;
}

void ViewerImpl::set_color_print_colors(const PaletteWithAlpha& colors)
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
// ORCA: Add Pressure Advance visualization support
    case EViewType::PressureAdvance:          { return m_pressure_advance_range; }
    // ORCA: Add Acceleration visualization support
    case EViewType::Acceleration:             { return m_acceleration_range; }
    // ORCA: Add Jerk visualization support
    case EViewType::Jerk:                     { return m_jerk_range; }
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
// ORCA: Add Pressure Advance visualization support
    case EViewType::PressureAdvance:          { m_pressure_advance_range.set_palette(palette); break; }
    // ORCA: Add Acceleration visualization support
    case EViewType::Acceleration:             { m_acceleration_range.set_palette(palette);     break; }
    // ORCA: Add Jerk visualization support
    case EViewType::Jerk:                     { m_jerk_range.set_palette(palette);             break; }
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
    ret += STDVEC_MEMSIZE(m_vertices_colors, ColorWithAlpha);
    for (const std::vector<float>& times : m_layer_start_times)
        ret += STDVEC_MEMSIZE(times, float);
    ret += STDVEC_MEMSIZE(m_layer_first_vertex, uint32_t);
    ret += m_valid_lines_bitset.size_in_bytes_cpu();
    ret += m_height_range.size_in_bytes_cpu();
    ret += m_width_range.size_in_bytes_cpu();
    ret += m_speed_range.size_in_bytes_cpu();
    ret += m_actual_speed_range.size_in_bytes_cpu();
    ret += m_fan_speed_range.size_in_bytes_cpu();
    ret += m_temperature_range.size_in_bytes_cpu();
    // ORCA: Add Pressure Advance visualization support
    ret += m_pressure_advance_range.size_in_bytes_cpu();
    // ORCA: Add Acceleration visualization support
    ret += m_acceleration_range.size_in_bytes_cpu();
    // ORCA: Add Jerk visualization support
    ret += m_jerk_range.size_in_bytes_cpu();
    ret += m_volumetric_rate_range.size_in_bytes_cpu();
    ret += m_actual_volumetric_rate_range.size_in_bytes_cpu();
    for (size_t i = 0; i < COLOR_RANGE_TYPES_COUNT; ++i) {
        ret += m_layer_time_range[i].size_in_bytes_cpu();
    }
    ret += STDVEC_MEMSIZE(m_tool_colors, ColorWithAlpha);
    ret += STDVEC_MEMSIZE(m_color_print_colors, ColorWithAlpha);
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
    ret += m_translucent_targets.size_in_bytes();
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

    // every vertex before m_layer_first_vertex[layers_range[0]] has a smaller layer_id, so the loop
    // below would skip all of them anyway
    auto first_it = m_vertices.begin();
    if (layers_range[0] < m_layer_first_vertex.size())
        first_it += m_layer_first_vertex[layers_range[0]];
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
        // ORCA: skip straight to the layer above the range rather than walking there
        if (m_layers_in_vertex_order && layers_range[1] + 1 < m_layer_first_vertex.size())
            last_it = std::max(first_it, m_vertices.begin() + m_layer_first_vertex[layers_range[1] + 1]);
        else if (m_layers_in_vertex_order)
            last_it = m_vertices.end();
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
            const auto range_first_it = m_vertices.begin() + full_range[0];
            auto top_first_it = range_first_it;
            // ORCA: skip straight to the top layer rather than walking there
            if (m_layers_in_vertex_order)
                top_first_it = std::max(top_first_it, m_vertices.begin() + m_layer_first_vertex[layers_range[1]]);
            while (top_first_it != m_vertices.end() && (top_first_it->layer_id < layers_range[1] || !is_visible(*top_first_it, m_settings)))
                ++top_first_it;
            const bool shortened = top_first_it != range_first_it;
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
    // ORCA: Add Pressure Advance visualization support
    m_pressure_advance_range.reset();
    // ORCA: Add Acceleration visualization support
    m_acceleration_range.reset();
    // ORCA: Add Jerk visualization support
    m_jerk_range.reset();
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
            // ORCA: Add Pressure Advance visualization support
            if (v.pressure_advance >= 0.0f)
                m_pressure_advance_range.update(v.pressure_advance);
        }
        if ((v.is_travel() && m_settings.options_visibility[size_t(EOptionType::Travels)]) ||
            (v.is_wipe() && m_settings.options_visibility[size_t(EOptionType::Wipes)]) ||
             v.is_extrusion()) {
            m_speed_range.update(v.feedrate);
            m_actual_speed_range.update(v.actual_feedrate);
            // ORCA: Add Acceleration visualization support
            m_acceleration_range.update(v.acceleration);
            // ORCA: Add Jerk visualization support
            m_jerk_range.update(v.jerk);
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
    std::vector<Vec4> heights_widths_angles;
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

void ViewerImpl::render_segments(const Mat4x4& view_matrix, const Mat4x4& projection_matrix, const Vec3& camera_position, bool translucent)
{
    // ORCA: realistic view. The shadow caster pass draws depth only, as does the first order independent pass.
#ifndef ENABLE_OPENGL_ES
    const bool depth_only = m_translucent_pass == 1;
    const unsigned int shader_id = m_rendering_shadow_casters ? m_segments_caster_shader_id : depth_only ? m_segments_depth_shader_id : m_segments_shader_id;
    const SegmentsUniforms& uni = m_rendering_shadow_casters ? m_uni_segments_caster : depth_only ? m_uni_segments_depth : m_uni_segments;
#else
    const bool depth_only = false;
    const unsigned int shader_id = m_rendering_shadow_casters ? m_segments_caster_shader_id : m_segments_shader_id;
    const SegmentsUniforms& uni = m_rendering_shadow_casters ? m_uni_segments_caster : m_uni_segments;
#endif // ENABLE_OPENGL_ES
    if (shader_id == 0)
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

    glsafe(glUseProgram(shader_id));

    glsafe(glUniform1i(uni.positions_tex, 0));
    glsafe(glUniform1i(uni.height_width_angle_tex, 1));
    glsafe(glUniform1i(uni.colors_tex, 2));
    glsafe(glUniform1i(uni.segment_index_tex, 3));
    glsafe(glUniformMatrix4fv(uni.view_matrix, 1, GL_FALSE, view_matrix.data()));
    glsafe(glUniformMatrix4fv(uni.projection_matrix, 1, GL_FALSE, projection_matrix.data()));
    glsafe(glUniform3fv(uni.camera_position, 1, camera_position.data()));
    // ORCA: view_matrix(2,2) > 0 is looking down, where the last segments printed are the nearest.
    // Blending goes the other way, from the farthest.
    const bool reverse_order = (view_matrix[10] > 0.0f) != translucent;
    glsafe(glUniform1i(uni.reverse_order, reverse_order ? 1 : 0));
    glsafe(glUniform4fv(uni.clipping_plane, 1, m_clipping_plane.data()));
#ifndef ENABLE_OPENGL_ES
    if (depth_only)
        glsafe(glUniform1i(m_uni_segments_depth_opaque_depth_id, TRANSLUCENT_TEXTURE_UNIT));
#endif // ENABLE_OPENGL_ES
    if (!m_rendering_shadow_casters && !depth_only) {
        // ORCA: realistic view
        glsafe(glUniform1i(m_uni_segments_shadow_map_id, m_shadow_map_texture_unit));
        glsafe(glUniformMatrix4fv(m_uni_segments_shadow_light_vp_id, 1, GL_FALSE, m_shadow_light_vp.data()));
        glsafe(glUniform1f(m_uni_segments_shadow_intensity_id, m_shadow_intensity));
        glsafe(glUniform1f(m_uni_segments_shadow_map_texel_id, m_shadow_map_texel));
        glsafe(glUniform1f(m_uni_segments_exposure_id, m_exposure));
        glsafe(glUniform1f(m_uni_segments_saturation_id, m_saturation));
        glsafe(glUniform3fv(m_uni_segments_light_top_dir_id, 1, m_light_top_dir.data()));
        glsafe(glUniform1f(m_uni_segments_gloss_id, m_gloss ? 0.6f : 0.0f));
        set_layer_colors(m_uni_segments_layer_colors);
#ifndef ENABLE_OPENGL_ES
        glsafe(glUniform1i(m_uni_segments_oit_pass_id, m_translucent_pass));
        glsafe(glUniform1i(m_uni_segments_opaque_depth_id, TRANSLUCENT_TEXTURE_UNIT));
        glsafe(glUniform1i(m_uni_segments_front_depth_id, TRANSLUCENT_TEXTURE_UNIT + 1));
        glsafe(glUniform2fv(m_uni_segments_oit_depth_range_id, 1, m_oit_depth_range.data()));
#endif // ENABLE_OPENGL_ES
    }

    // ORCA: the translucent segments neither hide each other nor what is drawn after them. Drawn late in the
    // frame, they cannot count on the depth test the toolpaths left on, which hides them behind opaque ones.
    const bool curr_blend = glIsEnabled(GL_BLEND);
    const bool curr_depth_test = glIsEnabled(GL_DEPTH_TEST);
    std::array<int, 4> curr_blend_func = { 0, 0, 0, 0 };
    std::array<int, 2> curr_blend_equation = { 0, 0 };
    GLboolean curr_depth_mask = GL_TRUE;
    if (translucent) {
        glsafe(glEnable(GL_DEPTH_TEST));
        glsafe(glGetIntegerv(GL_BLEND_SRC_RGB, &curr_blend_func[0]));
        glsafe(glGetIntegerv(GL_BLEND_DST_RGB, &curr_blend_func[1]));
        glsafe(glGetIntegerv(GL_BLEND_SRC_ALPHA, &curr_blend_func[2]));
        glsafe(glGetIntegerv(GL_BLEND_DST_ALPHA, &curr_blend_func[3]));
        glsafe(glGetIntegerv(GL_BLEND_EQUATION_RGB, &curr_blend_equation[0]));
        glsafe(glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &curr_blend_equation[1]));
        glsafe(glGetBooleanv(GL_DEPTH_WRITEMASK, &curr_depth_mask));
        glsafe(glEnable(GL_BLEND));
        // premultiplied, the highlights are not scaled by the opacity; the nearest depth, then sums,
        // in the passes of render_translucent_segments()
        glsafe(glBlendEquation(m_translucent_pass == 1 ? GL_MIN : GL_FUNC_ADD));
        glsafe(glBlendFunc(GL_ONE, m_translucent_pass != 0 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA));
        glsafe(glDepthMask(GL_FALSE));
    }

    glsafe(glDisable(GL_CULL_FACE));

    // ORCA: of the count enabled segments, the last translucent_count are the translucent ones
    auto draw = [this, &uni, translucent](size_t count, size_t translucent_count) {
        const size_t first = translucent ? count - translucent_count : 0;
        if (!m_rendering_shadow_casters)
            count = translucent ? translucent_count : count - translucent_count;
        if (count == 0)
            return;
        glsafe(glUniform1i(uni.instances_count, static_cast<int>(count)));
        glsafe(glUniform1i(uni.first_instance, static_cast<int>(first)));
        if (!m_rendering_shadow_casters) {
            m_segment_template.render(count);
            return;
        }
        // ORCA: realistic view. 6 vertices per caster ribbon.
        int curr_vertex_array = 0;
        glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));
        glsafe(glBindVertexArray(m_segments_caster_vao_id));
        glsafe(glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(6 * count)));
        glsafe(glBindVertexArray(curr_vertex_array));
    };

#ifdef ENABLE_OPENGL_ES
    int curr_bound_texture = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_bound_texture));

    const size_t tex_count = m_texture_data.get_count();
    for (size_t n = 0; n < tex_count; ++n) {
        const size_t i = reverse_order ? tex_count - 1 - n : n;
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
        draw(count, m_texture_data.get_enabled_translucent_segments_count(i));
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
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, m_colors_buf_id));
    glsafe(glActiveTexture(GL_TEXTURE3));
    glsafe(glBindTexture(GL_TEXTURE_BUFFER, m_enabled_segments_tex_id));
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, m_enabled_segments_buf_id));

    draw(m_enabled_segments_count, m_enabled_translucent_segments_count);
#endif // ENABLE_OPENGL_ES

    if (curr_cull_face)
        glsafe(glEnable(GL_CULL_FACE));
    if (translucent) {
        if (!curr_depth_test)
            glsafe(glDisable(GL_DEPTH_TEST));
        glsafe(glDepthMask(curr_depth_mask));
        glsafe(glBlendEquationSeparate(curr_blend_equation[0], curr_blend_equation[1]));
        glsafe(glBlendFuncSeparate(curr_blend_func[0], curr_blend_func[1], curr_blend_func[2], curr_blend_func[3]));
        if (!curr_blend)
            glsafe(glDisable(GL_BLEND));
    }

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
    glsafe(glUniform4fv(m_uni_options_clipping_plane_id, 1, m_clipping_plane.data()));
    set_layer_colors(m_uni_options_layer_colors);

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
    glsafe(glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA8, m_colors_buf_id));
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

#ifndef ENABLE_OPENGL_ES
bool ViewerImpl::TranslucentTargets::update(int width, int height)
{
    if (unsupported || width <= 0 || height <= 0)
        return false;
    if (fbo != 0 && size[0] == width && size[1] == height)
        return true;

    release();
    size = { width, height };

    int curr_texture = 0;
    int curr_draw_fbo = 0;
    int curr_read_fbo = 0;
    glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_texture));
    glsafe(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &curr_draw_fbo));
    glsafe(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &curr_read_fbo));

    auto make_texture = [width, height](unsigned int& id, GLenum internal_format, GLenum format, GLenum type) {
        glsafe(glGenTextures(1, &id));
        glsafe(glBindTexture(GL_TEXTURE_2D, id));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
        glsafe(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
        glsafe(glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, nullptr));
    };
    const GLenum buffers[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };

    make_texture(depth_tex, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT);
    make_texture(accum_tex, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    make_texture(reveal_tex, GL_R16F, GL_RED, GL_FLOAT);
    make_texture(front_tex, GL_RGBA16F, GL_RGBA, GL_FLOAT);
    make_texture(front_depth_tex, GL_R32F, GL_RED, GL_FLOAT);
    glsafe(glGenFramebuffers(1, &front_depth_fbo));
    glsafe(glBindFramebuffer(GL_FRAMEBUFFER, front_depth_fbo));
    glsafe(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, front_depth_tex, 0));
    glsafe(glDrawBuffers(1, buffers));
    bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glsafe(glGenFramebuffers(1, &fbo));
    glsafe(glBindFramebuffer(GL_FRAMEBUFFER, fbo));
    glsafe(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, accum_tex, 0));
    glsafe(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, reveal_tex, 0));
    glsafe(glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, front_tex, 0));
    glsafe(glDrawBuffers(3, buffers));
    complete = complete && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    glsafe(glBindTexture(GL_TEXTURE_2D, curr_texture));
    glsafe(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, curr_draw_fbo));
    glsafe(glBindFramebuffer(GL_READ_FRAMEBUFFER, curr_read_fbo));

    if (!complete) {
        release();
        unsupported = true;
    }
    return complete;
}

void ViewerImpl::TranslucentTargets::release()
{
    for (unsigned int* id : { &fbo, &front_depth_fbo }) {
        if (*id != 0) {
            glsafe(glDeleteFramebuffers(1, id));
            *id = 0;
        }
    }
    delete_textures(accum_tex);
    delete_textures(reveal_tex);
    delete_textures(front_tex);
    delete_textures(depth_tex);
    delete_textures(front_depth_tex);
    size = { 0, 0 };
}

size_t ViewerImpl::TranslucentTargets::size_in_bytes() const
{
    // RGBA16F, R16F, RGBA16F, a 24 bit depth and R32F
    return static_cast<size_t>(size[0]) * static_cast<size_t>(size[1]) * (8 + 2 + 8 + 4 + 4);
}
#endif // ENABLE_OPENGL_ES

void ViewerImpl::render_translucent(const Mat4x4& view_matrix, const Mat4x4& projection_matrix)
{
    apply_pending_updates();
    if (m_enabled_translucent_segments_count == 0)
        return;

    const Mat4x4 inv_view_matrix = inverse(view_matrix);
    render_translucent_segments(view_matrix, projection_matrix, { inv_view_matrix[12], inv_view_matrix[13], inv_view_matrix[14] });
}

void ViewerImpl::render_translucent_segments(const Mat4x4& view_matrix, const Mat4x4& projection_matrix, const Vec3& camera_position)
{
#ifndef ENABLE_OPENGL_ES
    // ORCA: the order independent path, whose targets are as large as the canvas: the nearest translucent surface
    // over the weighted blended order independent transparency (McGuire and Bavoil, 2013) of the others. Blended in
    // print order, even one color shows the order, its surfaces being shaded apart.
    std::array<int, 4> viewport = { 0, 0, 0, 0 };
    int draw_fbo = 0;
    int read_fbo = 0;
    int samples = 0;
    glsafe(glGetIntegerv(GL_VIEWPORT, viewport.data()));
    glsafe(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo));
    glsafe(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo));
    glsafe(glGetIntegerv(GL_SAMPLES, &samples));
    // the depth of a multisampled framebuffer object cannot be copied into a texture
    if ((read_fbo != 0 && samples > 0) || !m_translucent_targets.update(viewport[2], viewport[3])) {
        render_segments(view_matrix, projection_matrix, camera_position, true);
        return;
    }
    const TranslucentTargets& targets = m_translucent_targets;
    const int width = viewport[2];
    const int height = viewport[3];

    int curr_active_texture = 0;
    std::array<int, 3> curr_textures = { 0, 0, 0 };
    glsafe(glGetIntegerv(GL_ACTIVE_TEXTURE, &curr_active_texture));
    for (int i = 0; i < 3; ++i) {
        glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT + i));
        glsafe(glGetIntegerv(GL_TEXTURE_BINDING_2D, &curr_textures[i]));
    }

    // the opaque scene, to hide the translucent segments behind it
    glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT));
    glsafe(glBindTexture(GL_TEXTURE_2D, targets.depth_tex));
    glsafe(glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, viewport[0], viewport[1], width, height));

    // the weights fall with the depth across the extrusions
    float near_z = FLT_MAX;
    float far_z = -FLT_MAX;
    for (int i = 0; i < 8; ++i) {
        const Vec3 p = { m_extrusions_box[i & 1][0], m_extrusions_box[(i >> 1) & 1][1], m_extrusions_box[(i >> 2) & 1][2] };
        const float z = -(view_matrix[2] * p[0] + view_matrix[6] * p[1] + view_matrix[10] * p[2] + view_matrix[14]);
        near_z = std::min(near_z, z);
        far_z = std::max(far_z, z);
    }
    m_oit_depth_range = { near_z, std::max(far_z, near_z + 1.0f) };

    // the depth of the nearest translucent surface, as the fragments see it, kept by GL_MIN
    const std::array<float, 4> zero = { 0.0f, 0.0f, 0.0f, 0.0f };
    const std::array<float, 4> one = { 1.0f, 1.0f, 1.0f, 1.0f };
    glsafe(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targets.front_depth_fbo));
    glsafe(glViewport(0, 0, width, height));
    glsafe(glClearBufferfv(GL_COLOR, 0, one.data()));
    m_translucent_pass = 1;
    render_segments(view_matrix, projection_matrix, camera_position, true);
    glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT + 1));
    glsafe(glBindTexture(GL_TEXTURE_2D, targets.front_depth_tex));

    // that surface apart, the others summed
    glsafe(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, targets.fbo));
    for (int i = 0; i < 3; ++i)
        glsafe(glClearBufferfv(GL_COLOR, i, zero.data()));
    m_translucent_pass = 2;
    render_segments(view_matrix, projection_matrix, camera_position, true);
    m_translucent_pass = 0;

    glsafe(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo));
    glsafe(glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo));
    glsafe(glViewport(viewport[0], viewport[1], width, height));

    // composite over the scene
    int curr_shader = 0;
    int curr_vertex_array = 0;
    std::array<int, 4> curr_blend_func = { 0, 0, 0, 0 };
    GLboolean curr_depth_mask = GL_TRUE;
    const bool curr_blend = glIsEnabled(GL_BLEND);
    const bool curr_depth_test = glIsEnabled(GL_DEPTH_TEST);
    glsafe(glGetIntegerv(GL_CURRENT_PROGRAM, &curr_shader));
    glsafe(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &curr_vertex_array));
    glsafe(glGetIntegerv(GL_BLEND_SRC_RGB, &curr_blend_func[0]));
    glsafe(glGetIntegerv(GL_BLEND_DST_RGB, &curr_blend_func[1]));
    glsafe(glGetIntegerv(GL_BLEND_SRC_ALPHA, &curr_blend_func[2]));
    glsafe(glGetIntegerv(GL_BLEND_DST_ALPHA, &curr_blend_func[3]));
    glsafe(glGetBooleanv(GL_DEPTH_WRITEMASK, &curr_depth_mask));

    glsafe(glUseProgram(m_translucent_composite_shader_id));
    glsafe(glUniform1i(m_uni_translucent_composite_accum_tex_id, TRANSLUCENT_TEXTURE_UNIT));
    glsafe(glUniform1i(m_uni_translucent_composite_reveal_tex_id, TRANSLUCENT_TEXTURE_UNIT + 1));
    glsafe(glUniform1i(m_uni_translucent_composite_front_tex_id, TRANSLUCENT_TEXTURE_UNIT + 2));
    glsafe(glUniform2i(m_uni_translucent_composite_origin_id, viewport[0], viewport[1]));
    glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT));
    glsafe(glBindTexture(GL_TEXTURE_2D, targets.accum_tex));
    glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT + 1));
    glsafe(glBindTexture(GL_TEXTURE_2D, targets.reveal_tex));
    glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT + 2));
    glsafe(glBindTexture(GL_TEXTURE_2D, targets.front_tex));
    glsafe(glEnable(GL_BLEND));
    glsafe(glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(glDisable(GL_DEPTH_TEST));
    glsafe(glDepthMask(GL_FALSE));
    glsafe(glBindVertexArray(m_segments_caster_vao_id));
    glsafe(glDrawArrays(GL_TRIANGLES, 0, 3));

    glsafe(glBindVertexArray(curr_vertex_array));
    glsafe(glDepthMask(curr_depth_mask));
    if (curr_depth_test)
        glsafe(glEnable(GL_DEPTH_TEST));
    glsafe(glBlendFuncSeparate(curr_blend_func[0], curr_blend_func[1], curr_blend_func[2], curr_blend_func[3]));
    if (!curr_blend)
        glsafe(glDisable(GL_BLEND));
    glsafe(glUseProgram(curr_shader));
    for (int i = 0; i < 3; ++i) {
        glsafe(glActiveTexture(GL_TEXTURE0 + TRANSLUCENT_TEXTURE_UNIT + i));
        glsafe(glBindTexture(GL_TEXTURE_2D, curr_textures[i]));
    }
    glsafe(glActiveTexture(curr_active_texture));
#else
    render_segments(view_matrix, projection_matrix, camera_position, true);
#endif // ENABLE_OPENGL_ES
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
