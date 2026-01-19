#include "GLSelectionRectangle.hpp"
#include "Camera.hpp"
#include "CameraUtils.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include <igl/project.h>

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {

    void GLSelectionRectangle::start_dragging(const Vec2d& mouse_position, EState state)
    {
        if (is_dragging() || (state == Off))
            return;

        m_state = state;
        m_start_corner = mouse_position;
        m_end_corner = mouse_position;
    }

    void GLSelectionRectangle::dragging(const Vec2d& mouse_position)
    {
        if (!is_dragging())
            return;

        m_end_corner = mouse_position;
    }

    std::vector<unsigned int> GLSelectionRectangle::contains(const std::vector<Vec3d>& points) const
    {
        std::vector<unsigned int> out;

        // bounding box created from the rectangle corners - will take care of order of the corners
        const BoundingBox rectangle(Points{ Point(m_start_corner.cast<coord_t>()), Point(m_end_corner.cast<coord_t>()) });

        // Iterate over all points and determine whether they're in the rectangle.
        const Camera &camera = wxGetApp().plater()->get_camera();
        Points points_2d = CameraUtils::project(camera, points);
        unsigned int size = static_cast<unsigned int>(points.size());
        for (unsigned int i = 0; i< size; ++i)
            if (rectangle.contains(points_2d[i]))
                out.push_back(i);

        return out;
    }

    void GLSelectionRectangle::stop_dragging()
    {
        if (is_dragging())
            m_state = Off;
    }

    void GLSelectionRectangle::render(const GLCanvas3D& canvas)
    {
        if (!is_dragging())
            return;

        const Size cnv_size = canvas.get_canvas_size();
        const float cnv_width = (float)cnv_size.get_width();
        const float cnv_height = (float)cnv_size.get_height();
        if (cnv_width == 0.0f || cnv_height == 0.0f)
            return;

        const float cnv_inv_width = 1.0f / cnv_width;
        const float cnv_inv_height = 1.0f / cnv_height;
        const float left = 2.0f * (get_left() * cnv_inv_width - 0.5f);
        const float right = 2.0f * (get_right() * cnv_inv_width - 0.5f);
        const float top = -2.0f * (get_top() * cnv_inv_height - 0.5f);
        const float bottom = -2.0f * (get_bottom() * cnv_inv_height - 0.5f);
        
#if !SLIC3R_OPENGL_ES
        if (!OpenGLManager::get_gl_info().is_core_profile())
            glsafe(::glLineWidth(1.5f));
#endif // !SLIC3R_OPENGL_ES

        glsafe(::glDisable(GL_DEPTH_TEST));

#if !SLIC3R_OPENGL_ES
        if (!OpenGLManager::get_gl_info().is_core_profile()) {
            glsafe(::glPushAttrib(GL_ENABLE_BIT));
            glsafe(::glLineStipple(4, 0xAAAA));
            glsafe(::glEnable(GL_LINE_STIPPLE));
        }
#endif // !SLIC3R_OPENGL_ES

#if SLIC3R_OPENGL_ES
        GLShaderProgram* shader = wxGetApp().get_shader("dashed_lines");
#else
        GLShaderProgram* shader = OpenGLManager::get_gl_info().is_core_profile() ? wxGetApp().get_shader("dashed_thick_lines") : wxGetApp().get_shader("flat");
#endif // SLIC3R_OPENGL_ES
        if (shader != nullptr) {
            shader->start_using();

            if (!m_rectangle.is_initialized() || !m_old_start_corner.isApprox(m_start_corner) || !m_old_end_corner.isApprox(m_end_corner)) {
                m_old_start_corner = m_start_corner;
                m_old_end_corner = m_end_corner;
                m_rectangle.reset();

                GLModel::Geometry init_data;
#if !SLIC3R_OPENGL_ES
                if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
                    init_data.format = { GLModel::Geometry::EPrimitiveType::Lines, GLModel::Geometry::EVertexLayout::P4 };
                    init_data.reserve_vertices(5);
                    init_data.reserve_indices(8);
#if !SLIC3R_OPENGL_ES
                }
                else {
                    init_data.format = { GLModel::Geometry::EPrimitiveType::LineLoop, GLModel::Geometry::EVertexLayout::P2 };
                    init_data.reserve_vertices(4);
                    init_data.reserve_indices(4);
                }
#endif // !SLIC3R_OPENGL_ES

                // vertices
#if !SLIC3R_OPENGL_ES
                if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
                    const float width = right - left;
                    const float height = top - bottom;
                    float perimeter = 0.0f;

                    init_data.add_vertex(Vec4f(left, bottom, 0.0f, perimeter));
                    perimeter += width;
                    init_data.add_vertex(Vec4f(right, bottom, 0.0f, perimeter));
                    perimeter += height;
                    init_data.add_vertex(Vec4f(right, top, 0.0f, perimeter));
                    perimeter += width;
                    init_data.add_vertex(Vec4f(left, top, 0.0f, perimeter));
                    perimeter += height;
                    init_data.add_vertex(Vec4f(left, bottom, 0.0f, perimeter));

                    // indices
                    init_data.add_line(0, 1);
                    init_data.add_line(1, 2);
                    init_data.add_line(2, 3);
                    init_data.add_line(3, 4);
#if !SLIC3R_OPENGL_ES
                }
                else {
                    init_data.add_vertex(Vec2f(left, bottom));
                    init_data.add_vertex(Vec2f(right, bottom));
                    init_data.add_vertex(Vec2f(right, top));
                    init_data.add_vertex(Vec2f(left, top));

                    // indices
                    init_data.add_index(0);
                    init_data.add_index(1);
                    init_data.add_index(2);
                    init_data.add_index(3);
                }
#endif // !SLIC3R_OPENGL_ES

                m_rectangle.init_from(std::move(init_data));
            }

            shader->set_uniform("view_model_matrix", Transform3d::Identity());
            shader->set_uniform("projection_matrix", Transform3d::Identity());
#if !SLIC3R_OPENGL_ES
            if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
                const std::array<int, 4>& viewport = wxGetApp().plater()->get_camera().get_viewport();
                shader->set_uniform("viewport_size", Vec2d(double(viewport[2]), double(viewport[3])));
                shader->set_uniform("width", 0.25f);
                shader->set_uniform("dash_size", 0.01f);
                shader->set_uniform("gap_size", 0.0075f);
#if !SLIC3R_OPENGL_ES
            }
#endif // !SLIC3R_OPENGL_ES

            m_rectangle.set_color(ColorRGBA::ORCA()); // ORCA: use orca color for selection rectangle
            m_rectangle.render();
            shader->stop_using();
        }

#if !SLIC3R_OPENGL_ES
        if (!OpenGLManager::get_gl_info().is_core_profile())
            glsafe(::glPopAttrib());
#endif // !SLIC3R_OPENGL_ES
    }

} // namespace GUI
} // namespace Slic3r
