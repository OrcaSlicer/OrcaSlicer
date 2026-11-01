#include "libslic3r/libslic3r.h"
#include "SelectionOutline.hpp"

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "OpenGLManager.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/Point.hpp"

#include <glad/gl.h>
#include <boost/log/trivial.hpp>
#include <vector>
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <utility>

namespace Slic3r {
namespace GUI {

// Widths on the model in mm, kept between two widths in pixels at a scale of 1.
static constexpr float                RING_WIDTH_MM      = 0.35f;
static constexpr std::array<float, 2> RING_WIDTH_PX      = { 2.5f, 5.0f };
static constexpr float                LINE_HALF_WIDTH_MM = 0.09f;
static constexpr std::array<float, 2> LINE_HALF_WIDTH_PX = { 0.6f, 1.2f };
static constexpr float LINE_OPACITY    = 0.75f;
// The farthest the outline shaders search.
static constexpr int MAX_REACH = 15;

static const ColorRGB OUTLINE_COLOR = ColorRGB::WHITE();

void SelectionOutline::render(const std::vector<GLVolume*>& volumes, const Camera& camera, const std::array<float, 2>& z_range,
                              const std::array<double, 4>& clipping_plane, float scale, GLModel& quad)
{
    if (volumes.empty() || OpenGLManager::get_framebuffers_type() != OpenGLManager::EFramebufferType::Arb)
        return;
    GLShaderProgram* mask_shader  = wxGetApp().get_shader("outline_mask");
    GLShaderProgram* edges_shader = wxGetApp().get_shader("outline_edges");
    GLShaderProgram* shader       = wxGetApp().get_shader("outline");
    if (mask_shader == nullptr || edges_shader == nullptr || shader == nullptr)
        return;

    const std::array<int, 4>& viewport = camera.get_viewport();
    const int width  = viewport[2];
    const int height = viewport[3];
    if (width <= 0 || height <= 0 || !update_targets(width, height))
        return;
    if (m_stencil_bits < 0)
        m_stencil_bits = OpenGLManager::stencil_bits();

    const Transform3d& view_matrix       = camera.get_view_matrix();
    const Transform3d& projection_matrix = camera.get_projection_matrix();
    const Matrix4d&    projection        = projection_matrix.matrix();
    // The passes reach as far as the widest lines.
    const float ring_width = RING_WIDTH_PX[1] * scale;
    const float line_width = LINE_HALF_WIDTH_PX[1] * scale;
    // Wide enough to place the silhouette between texels at the outer edge of the ring.
    const float ring_sigma = 0.5f * (ring_width + 0.5f);
    const int   ring_reach = std::min(MAX_REACH, int(std::ceil(ring_width + 1.5f)));
    const int   line_reach = std::min(MAX_REACH - 1, int(std::ceil(line_width + 0.5f)) + 1);
    // A pixel spans 2 / (P11 * height) of view space, at unit depth for a perspective camera.
    const float pixel_size   = float(2.0 / (projection(1, 1) * height));
    const Vec2f pixel_size_coef = camera.get_type() == Camera::EType::Ortho ? Vec2f(0.0f, pixel_size) : Vec2f(pixel_size, 0.0f);

    // Screen box of the volumes, the whole screen when one reaches behind the camera.
    std::array<int, 4> box = { 0, 0, width, height };
    {
        const Matrix4d view_projection = projection * view_matrix.matrix();
        Vec2d lo(DBL_MAX, DBL_MAX);
        Vec2d hi(-DBL_MAX, -DBL_MAX);
        bool behind = false;
        for (const GLVolume* volume : volumes) {
            const BoundingBoxf3& bb = volume->transformed_bounding_box();
            for (int i = 0; i < 8 && !behind; ++i) {
                const Vec4d c = view_projection * Vec4d((i & 1) ? bb.max.x() : bb.min.x(), (i & 2) ? bb.max.y() : bb.min.y(),
                                                        (i & 4) ? bb.max.z() : bb.min.z(), 1.0);
                behind = c.w() <= 0.0;
                lo = lo.cwiseMin(c.head<2>() / c.w());
                hi = hi.cwiseMax(c.head<2>() / c.w());
            }
        }
        if (!behind)
            box = { int(std::floor((lo.x() * 0.5 + 0.5) * width)), int(std::floor((lo.y() * 0.5 + 0.5) * height)),
                    int(std::ceil((hi.x() * 0.5 + 0.5) * width)), int(std::ceil((hi.y() * 0.5 + 0.5) * height)) };
    }
    auto scissor = [&box, width, height](int pad) {
        const int x0 = std::clamp(box[0] - pad, 0, width);
        const int y0 = std::clamp(box[1] - pad, 0, height);
        const int x1 = std::clamp(box[2] + pad, 0, width);
        const int y1 = std::clamp(box[3] + pad, 0, height);
        glsafe(::glScissor(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)));
    };
    // Each pass reads the one before it this far beyond the pixels it writes.
    const int composite_pad = int(std::ceil(ring_width)) + 2;
    const int edges_pad     = composite_pad + std::max(ring_reach, line_reach + 1) + 1;
    const int mask_pad      = edges_pad + std::max(ring_reach, 2) + 1;

    GLint prev_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
    GLfloat clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    glsafe(::glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color));
    const GLboolean blend = ::glIsEnabled(GL_BLEND);
    const GLboolean cull  = ::glIsEnabled(GL_CULL_FACE);

    // The mask: normal, part and the distances to the triangle edges of the nearest selected surface.
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_mask_fbo));
    glsafe(::glEnable(GL_SCISSOR_TEST));
    scissor(mask_pad);
    glsafe(::glClearColor(0.0f, 0.0f, 0.0f, 0.0f));
    glsafe(::glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
    glsafe(::glDisable(GL_BLEND));
    glsafe(::glDisable(GL_CULL_FACE));
    mask_shader->start_using();
    mask_shader->set_uniform("projection_matrix", projection_matrix);
    mask_shader->set_uniform("z_range", z_range);
    mask_shader->set_uniform("clipping_plane", clipping_plane);
    for (size_t i = 0; i < volumes.size(); ++i) {
        GLVolume& volume = *volumes[i];
        const Transform3d model_matrix = volume.world_matrix();
        mask_shader->set_uniform("volume_world_matrix", model_matrix);
        mask_shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        mask_shader->set_uniform("view_normal_matrix", Matrix3d(view_matrix.matrix().block(0, 0, 3, 3) *
                                                                model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose()));
        mask_shader->set_uniform("part_id", float(i % 65535 + 1));
        if (volume.tverts_range == std::make_pair<size_t, size_t>(0, -1))
            volume.model.render(mask_shader);
        else
            volume.model.render(volume.tverts_range, mask_shader);
    }
    mask_shader->stop_using();

    glsafe(::glActiveTexture(GL_TEXTURE0));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_mask_texture));
    glsafe(::glActiveTexture(GL_TEXTURE1));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_depth_texture));

    // The edges: inner ones between neighboring texels, and the nearest covered texel along each row.
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_edges_fbo));
    glsafe(::glDisable(GL_DEPTH_TEST));
    scissor(edges_pad);
    edges_shader->start_using();
    edges_shader->set_uniform("mask_tex", 0);
    edges_shader->set_uniform("depth_tex", 1);
    edges_shader->set_uniform("tex_size", Vec2f(float(width), float(height)));
    edges_shader->set_uniform("inv_projection_matrix", Matrix4d(projection.inverse()));
    edges_shader->set_uniform("pixel_size_coef", pixel_size_coef);
    edges_shader->set_uniform("ring_reach", ring_reach);
    edges_shader->set_uniform("ring_sigma", ring_sigma);
    quad.render();
    edges_shader->stop_using();

    glsafe(::glActiveTexture(GL_TEXTURE2));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_edges_texture));

    // Over the scene, depth tested so that what is in front of the selection hides its outline.
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo)));
    scissor(composite_pad);
    glsafe(::glEnable(GL_DEPTH_TEST));
    glsafe(::glDepthFunc(GL_LEQUAL));
    glsafe(::glDepthMask(GL_FALSE));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    const bool use_stencil = uses_stencil();
    if (use_stencil) {
        glsafe(::glEnable(GL_STENCIL_TEST));
        glsafe(::glStencilMask(DrawnBit));
        glsafe(::glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE));
    }
    shader->start_using();
    shader->set_uniform("mask_tex", 0);
    shader->set_uniform("depth_tex", 1);
    shader->set_uniform("edges_tex", 2);
    shader->set_uniform("tex_size", Vec2f(float(width), float(height)));
    shader->set_uniform("use_stencil", use_stencil);
    shader->set_uniform("ring_width", std::array<float, 3>{ RING_WIDTH_PX[0] * scale, ring_width, RING_WIDTH_MM });
    shader->set_uniform("ring_reach", ring_reach);
    shader->set_uniform("ring_sigma", ring_sigma);
    shader->set_uniform("line_width", std::array<float, 3>{ LINE_HALF_WIDTH_PX[0] * scale, line_width, LINE_HALF_WIDTH_MM });
    shader->set_uniform("line_reach", line_reach);
    shader->set_uniform("line_opacity", LINE_OPACITY);
    shader->set_uniform("outline_color", OUTLINE_COLOR);
    shader->set_uniform("depth_coef", std::array<float, 4>{ float(projection(2, 2)), float(projection(2, 3)),
                                                            float(projection(3, 2)), float(projection(3, 3)) });
    shader->set_uniform("pixel_size_coef", pixel_size_coef);
    // The ring off the samples of the selection, the lines on them, both marking DrawnBit where they draw.
    if (use_stencil)
        glsafe(::glStencilFunc(GL_NOTEQUAL, StencilBit | DrawnBit, StencilBit));
    shader->set_uniform("inner", false);
    quad.render();
    if (use_stencil)
        glsafe(::glStencilFunc(GL_EQUAL, StencilBit | DrawnBit, StencilBit));
    shader->set_uniform("inner", true);
    quad.render();
    shader->stop_using();

    if (use_stencil) {
        glsafe(::glDisable(GL_STENCIL_TEST));
        glsafe(::glStencilMask(0xFF));
        glsafe(::glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP));
    }
    glsafe(::glDisable(GL_SCISSOR_TEST));
    glsafe(::glDepthFunc(GL_LESS));
    glsafe(::glDepthMask(GL_TRUE));
    if (!blend)
        glsafe(::glDisable(GL_BLEND));
    if (cull)
        glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]));
    for (GLenum unit : { GL_TEXTURE2, GL_TEXTURE1, GL_TEXTURE0 }) {
        glsafe(::glActiveTexture(unit));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    }
}

void SelectionOutline::reset()
{
    for (unsigned int* id : { &m_mask_texture, &m_depth_texture, &m_edges_texture }) {
        if (*id != 0) {
            glsafe(::glDeleteTextures(1, id));
            *id = 0;
        }
    }
    for (unsigned int* id : { &m_mask_fbo, &m_edges_fbo }) {
        if (*id != 0) {
            glsafe(::glDeleteFramebuffers(1, id));
            *id = 0;
        }
    }
    m_size = { { 0, 0 } };
    m_complete = false;
    m_stencil_bits = -1;
}

bool SelectionOutline::update_targets(unsigned int width, unsigned int height)
{
    if (m_mask_fbo != 0 && m_size[0] == width && m_size[1] == height)
        return m_complete;

    auto allocate = [width, height](unsigned int& id, GLint format, GLenum data_format, GLenum type) {
        if (id == 0) {
            glsafe(::glGenTextures(1, &id));
            glsafe(::glBindTexture(GL_TEXTURE_2D, id));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
            glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        }
        else
            glsafe(::glBindTexture(GL_TEXTURE_2D, id));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, format, width, height, 0, data_format, type, nullptr));
    };
    allocate(m_mask_texture, GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT);
    allocate(m_depth_texture, wxGetApp().is_gl_version_greater_or_equal_to(3, 0) ? GL_DEPTH_COMPONENT32F : GL_DEPTH_COMPONENT24,
             GL_DEPTH_COMPONENT, GL_FLOAT);
    allocate(m_edges_texture, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    GLint prev_fbo = 0;
    glsafe(::glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo));
    if (m_mask_fbo == 0)
        glsafe(::glGenFramebuffers(1, &m_mask_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_mask_fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_mask_texture, 0));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_depth_texture, 0));
    m_complete = ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (m_edges_fbo == 0)
        glsafe(::glGenFramebuffers(1, &m_edges_fbo));
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, m_edges_fbo));
    glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_edges_texture, 0));
    m_complete = m_complete && ::glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo)));

    m_size = { { width, height } };
    if (!m_complete)
        BOOST_LOG_TRIVIAL(error) << "SelectionOutline: incomplete framebuffers, the selection is drawn without its outline";
    return m_complete;
}

} // namespace GUI
} // namespace Slic3r
