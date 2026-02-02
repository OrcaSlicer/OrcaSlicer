#include "libslic3r/libslic3r.h"
#include "GLModel.hpp"

#include "3DScene.hpp"
#include "GUI_App.hpp"
#include "GLShader.hpp"

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string/predicate.hpp>

#if ENABLE_SMOOTH_NORMALS
#include <igl/per_face_normals.h>
#include <igl/per_corner_normals.h>
#include <igl/per_vertex_normals.h>
#endif // ENABLE_SMOOTH_NORMALS

#include <GL/glew.h>

namespace Slic3r {
namespace GUI {

#if ENABLE_SMOOTH_NORMALS
static void smooth_normals_corner(const TriangleMesh& mesh, std::vector<stl_normal>& normals)
{
    using MapMatrixXfUnaligned = Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>>;
    using MapMatrixXiUnaligned = Eigen::Map<const Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>>;

    std::vector<Vec3f> face_normals = its_face_normals(mesh.its);

    Eigen::MatrixXd vertices = MapMatrixXfUnaligned(mesh.its.vertices.front().data(),
        Eigen::Index(mesh.its.vertices.size()), 3).cast<double>();
    Eigen::MatrixXi indices = MapMatrixXiUnaligned(mesh.its.indices.front().data(),
        Eigen::Index(mesh.its.indices.size()), 3);
    Eigen::MatrixXd in_normals = MapMatrixXfUnaligned(face_normals.front().data(),
        Eigen::Index(face_normals.size()), 3).cast<double>();
    Eigen::MatrixXd out_normals;

    igl::per_corner_normals(vertices, indices, in_normals, 1.0, out_normals);

    normals = std::vector<stl_normal>(mesh.its.vertices.size());
    for (size_t i = 0; i < mesh.its.indices.size(); ++i) {
        for (size_t j = 0; j < 3; ++j) {
            normals[mesh.its.indices[i][j]] = out_normals.row(i * 3 + j).cast<float>();
        }
    }
}
#endif // ENABLE_SMOOTH_NORMALS

void GLModel::Geometry::add_vertex(const Vec2f& position)
{
    assert(format.vertex_layout == EVertexLayout::P2);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
}

void GLModel::Geometry::add_vertex(const Vec2f& position, const Vec2f& tex_coord)
{
    assert(format.vertex_layout == EVertexLayout::P2T2);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
    vertices.emplace_back(tex_coord.x());
    vertices.emplace_back(tex_coord.y());
}

void GLModel::Geometry::add_vertex(const Vec3f& position)
{
    assert(format.vertex_layout == EVertexLayout::P3);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
    vertices.emplace_back(position.z());
}

void GLModel::Geometry::add_vertex(const Vec3f& position, const Vec2f& tex_coord)
{
    assert(format.vertex_layout == EVertexLayout::P3T2);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
    vertices.emplace_back(position.z());
    vertices.emplace_back(tex_coord.x());
    vertices.emplace_back(tex_coord.y());
}

void GLModel::Geometry::add_vertex(const Vec3f& position, const Vec3f& normal)
{
    assert(format.vertex_layout == EVertexLayout::P3N3);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
    vertices.emplace_back(position.z());
    vertices.emplace_back(normal.x());
    vertices.emplace_back(normal.y());
    vertices.emplace_back(normal.z());
}

void GLModel::Geometry::add_vertex(const Vec3f& position, const Vec3f& normal, const Vec2f& tex_coord)
{
    assert(format.vertex_layout == EVertexLayout::P3N3T2);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
    vertices.emplace_back(position.z());
    vertices.emplace_back(normal.x());
    vertices.emplace_back(normal.y());
    vertices.emplace_back(normal.z());
    vertices.emplace_back(tex_coord.x());
    vertices.emplace_back(tex_coord.y());
}

void GLModel::Geometry::add_vertex(const Vec4f& position)
{
    assert(format.vertex_layout == EVertexLayout::P4);
    vertices.emplace_back(position.x());
    vertices.emplace_back(position.y());
    vertices.emplace_back(position.z());
    vertices.emplace_back(position.w());
}

void GLModel::Geometry::add_index(unsigned int id)
{
    indices.emplace_back(id);
}

void GLModel::Geometry::add_line(unsigned int id1, unsigned int id2)
{
    indices.emplace_back(id1);
    indices.emplace_back(id2);
}

void GLModel::Geometry::add_triangle(unsigned int id1, unsigned int id2, unsigned int id3)
{
    indices.emplace_back(id1);
    indices.emplace_back(id2);
    indices.emplace_back(id3);
}

Vec2f GLModel::Geometry::extract_position_2(size_t id) const
{
    const size_t p_stride = position_stride_floats(format);
    if (p_stride != 2) {
        assert(false);
        return { FLT_MAX, FLT_MAX };
    }

    if (vertices_count() <= id) {
        assert(false);
        return { FLT_MAX, FLT_MAX };
    }

    const float* start = &vertices[id * vertex_stride_floats(format) + position_offset_floats(format)];
    return { *(start + 0), *(start + 1) };
}

Vec3f GLModel::Geometry::extract_position_3(size_t id) const
{
    const size_t p_stride = position_stride_floats(format);
    if (p_stride != 3) {
        assert(false);
        return { FLT_MAX, FLT_MAX, FLT_MAX };
    }

    if (vertices_count() <= id) {
        assert(false);
        return { FLT_MAX, FLT_MAX, FLT_MAX };
    }

    const float* start = &vertices[id * vertex_stride_floats(format) + position_offset_floats(format)];
    return { *(start + 0), *(start + 1), *(start + 2) };
}

Vec3f GLModel::Geometry::extract_normal_3(size_t id) const
{
    const size_t n_stride = normal_stride_floats(format);
    if (n_stride != 3) {
        assert(false);
        return { FLT_MAX, FLT_MAX, FLT_MAX };
    }

    if (vertices_count() <= id) {
        assert(false);
        return { FLT_MAX, FLT_MAX, FLT_MAX };
    }

    const float* start = &vertices[id * vertex_stride_floats(format) + normal_offset_floats(format)];
    return { *(start + 0), *(start + 1), *(start + 2) };
}

Vec2f GLModel::Geometry::extract_tex_coord_2(size_t id) const
{
    const size_t t_stride = tex_coord_stride_floats(format);
    if (t_stride != 2) {
        assert(false);
        return { FLT_MAX, FLT_MAX };
    }

    if (vertices_count() <= id) {
        assert(false);
        return { FLT_MAX, FLT_MAX };
    }

    const float* start = &vertices[id * vertex_stride_floats(format) + tex_coord_offset_floats(format)];
    return { *(start + 0), *(start + 1) };
}

void GLModel::Geometry::set_vertex(size_t id, const Vec3f& position, const Vec3f& normal)
{
    assert(format.vertex_layout == EVertexLayout::P3N3);
    assert(id < vertices_count());
    if (id < vertices_count()) {
        float* start = &vertices[id * vertex_stride_floats(format)];
        *(start + 0) = position.x();
        *(start + 1) = position.y();
        *(start + 2) = position.z();
        *(start + 3) = normal.x();
        *(start + 4) = normal.y();
        *(start + 5) = normal.z();
    }
}

void GLModel::Geometry::set_index(size_t id, unsigned int index)
{
    assert(id < indices_count());
    if (id < indices_count())
        indices[id] = index;
}

unsigned int GLModel::Geometry::extract_index(size_t id) const
{
    if (indices_count() <= id) {
        assert(false);
        return -1;
    }

    return indices[id];
}

void GLModel::Geometry::remove_vertex(size_t id)
{
    assert(id < vertices_count());
    if (id < vertices_count()) {
        const size_t stride = vertex_stride_floats(format);
        std::vector<float>::const_iterator it = vertices.begin() + id * stride;
        vertices.erase(it, it + stride);
    }
}

indexed_triangle_set GLModel::Geometry::get_as_indexed_triangle_set() const
{
    indexed_triangle_set its;
    its.vertices.reserve(vertices_count());
    for (size_t i = 0; i < vertices_count(); ++i) {
        its.vertices.emplace_back(extract_position_3(i));
    }
    its.indices.reserve(indices_count() / 3);
    for (size_t i = 0; i < indices_count() / 3; ++i) {
        const size_t tri_id = i * 3;
        its.indices.emplace_back(extract_index(tri_id), extract_index(tri_id + 1), extract_index(tri_id + 2));
    }
    return its;
}

size_t GLModel::Geometry::vertex_stride_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2:     { return 2; }
    case EVertexLayout::P2T2:   { return 4; }
    case EVertexLayout::P3:     { return 3; }
    case EVertexLayout::P3T2:   { return 5; }
    case EVertexLayout::P3N3:   { return 6; }
    case EVertexLayout::P3N3T2: { return 8; }
    case EVertexLayout::P4:     { return 4; }
    default:                    { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::position_stride_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2:
    case EVertexLayout::P2T2:   { return 2; }
    case EVertexLayout::P3:
    case EVertexLayout::P3T2:
    case EVertexLayout::P3N3:
    case EVertexLayout::P3N3T2: { return 3; }
    case EVertexLayout::P4:     { return 4; }
    default:                    { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::position_offset_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2:
    case EVertexLayout::P2T2:
    case EVertexLayout::P3:
    case EVertexLayout::P3T2:
    case EVertexLayout::P3N3:
    case EVertexLayout::P3N3T2:
    case EVertexLayout::P4:   { return 0; }
    default:                  { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::normal_stride_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P3N3:
    case EVertexLayout::P3N3T2: { return 3; }
    default:                    { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::normal_offset_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P3N3:
    case EVertexLayout::P3N3T2: { return 3; }
    default:                    { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::tex_coord_stride_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2T2:
    case EVertexLayout::P3T2:
    case EVertexLayout::P3N3T2: { return 2; }
    default:                    { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::tex_coord_offset_floats(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2T2:   { return 2; }
    case EVertexLayout::P3T2:   { return 3; }
    case EVertexLayout::P3N3T2: { return 6; }
    default:                    { assert(false); return 0; }
    };
}

size_t GLModel::Geometry::index_stride_bytes(const Geometry& data)
{
    switch (data.index_type)
    {
    case EIndexType::UINT:   { return sizeof(unsigned int); }
    case EIndexType::USHORT: { return sizeof(unsigned short); }
    case EIndexType::UBYTE:  { return sizeof(unsigned char); }
    default:                 { assert(false); return 0; }
    };
}

bool GLModel::Geometry::has_position(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2:
    case EVertexLayout::P2T2:
    case EVertexLayout::P3:
    case EVertexLayout::P3T2:
    case EVertexLayout::P3N3:
    case EVertexLayout::P3N3T2:
    case EVertexLayout::P4:   { return true; }
    default:                  { assert(false); return false; }
    };
}

bool GLModel::Geometry::has_normal(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2:
    case EVertexLayout::P2T2:
    case EVertexLayout::P3:
    case EVertexLayout::P3T2:
    case EVertexLayout::P4:     { return false; }
    case EVertexLayout::P3N3:
    case EVertexLayout::P3N3T2: { return true; }
    default:                    { assert(false); return false; }
    };
}

bool GLModel::Geometry::has_tex_coord(const Format& format)
{
    switch (format.vertex_layout)
    {
    case EVertexLayout::P2T2:
    case EVertexLayout::P3T2:
    case EVertexLayout::P3N3T2: { return true; }
    case EVertexLayout::P2:
    case EVertexLayout::P3:
    case EVertexLayout::P3N3:
    case EVertexLayout::P4:     { return false; }
    default:                    { assert(false); return false; }
    };
}

void GLModel::init_from(Geometry&& data)
{
    if (is_initialized()) {
        // call reset() if you want to reuse this model
        assert(false);
        return;
    }

    if (data.vertices.empty() || data.indices.empty()) {
        assert(false);
        return;
    }

    m_render_data.geometry = std::move(data);

    // update bounding box
    for (size_t i = 0; i < vertices_count(); ++i) {
        const size_t position_stride = Geometry::position_stride_floats(data.format);
        if (position_stride == 3)
            m_bounding_box.merge(m_render_data.geometry.extract_position_3(i).cast<double>());
        else if (position_stride == 2) {
            const Vec2f position = m_render_data.geometry.extract_position_2(i);
            m_bounding_box.merge(Vec3f(position.x(), position.y(), 0.0f).cast<double>());
        }
    }
}

void GLModel::init_from(const TriangleMesh& mesh)
{
    init_from(mesh.its);
}

void GLModel::init_from(const indexed_triangle_set& its)
{
    if (is_initialized()) {
        // call reset() if you want to reuse this model
        assert(false);
        return;
    }

    if (its.vertices.empty() || its.indices.empty()){
        assert(false);
        return;
    }

    Geometry& data = m_render_data.geometry;
    data.format = { Geometry::EPrimitiveType::Triangles, Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(3 * its.indices.size());
    data.reserve_indices(3 * its.indices.size());

    // vertices + indices
    unsigned int vertices_counter = 0;
    for (uint32_t i = 0; i < its.indices.size(); ++i) {
        const stl_triangle_vertex_indices face = its.indices[i];
        const stl_vertex                  vertex[3] = { its.vertices[face[0]], its.vertices[face[1]], its.vertices[face[2]] };
        const stl_vertex                  n = face_normal_normalized(vertex);
        for (size_t j = 0; j < 3; ++j) {
            data.add_vertex(vertex[j], n);
        }
        vertices_counter += 3;
        data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
    }

    // update bounding box
    for (size_t i = 0; i < vertices_count(); ++i) {
        m_bounding_box.merge(data.extract_position_3(i).cast<double>());
    }
}

void GLModel::init_from(const Polygons& polygons, float z)
{
    if (is_initialized()) {
        // call reset() if you want to reuse this model
        assert(false);
        return;
    }

    if (polygons.empty()) {
        assert(false);
        return;
    }

    Geometry& data = m_render_data.geometry;
    data.format = { Geometry::EPrimitiveType::Lines, Geometry::EVertexLayout::P3 };

    size_t segments_count = 0;
    for (const Polygon& polygon : polygons) {
        segments_count += polygon.points.size();
    }

    data.reserve_vertices(2 * segments_count);
    data.reserve_indices(2 * segments_count);

    // vertices + indices
    unsigned int vertices_counter = 0;
    for (const Polygon& poly : polygons) {
        for (size_t i = 0; i < poly.points.size(); ++i) {
            const Point& p0 = poly.points[i];
            const Point& p1 = (i == poly.points.size() - 1) ? poly.points.front() : poly.points[i + 1];
            data.add_vertex(Vec3f(unscale<float>(p0.x()), unscale<float>(p0.y()), z));
            data.add_vertex(Vec3f(unscale<float>(p1.x()), unscale<float>(p1.y()), z));
            vertices_counter += 2;
            data.add_line(vertices_counter - 2, vertices_counter - 1);
        }
    }

    // update bounding box
    for (size_t i = 0; i < vertices_count(); ++i) {
        m_bounding_box.merge(data.extract_position_3(i).cast<double>());
    }
}

bool GLModel::init_from_file(const std::string& filename)
{
    if (!boost::filesystem::exists(filename))
        return false;

    if (!boost::algorithm::iends_with(filename, ".stl"))
        return false;

    Model model;
    try {
        model = Model::read_from_file(filename);
    }
    catch (std::exception&) {
        return false;
    }

    init_from(model.mesh());

    m_filename = filename;

    return true;
}

void GLModel::reset()
{
    // release gpu memory
    if (m_render_data.ibo_id > 0) {
        glsafe(::glDeleteBuffers(1, &m_render_data.ibo_id));
        m_render_data.ibo_id = 0;
    }
    if (m_render_data.vbo_id > 0) {
        glsafe(::glDeleteBuffers(1, &m_render_data.vbo_id));
        m_render_data.vbo_id = 0;
    }
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        if (m_render_data.vao_id > 0) {
            glsafe(::glDeleteVertexArrays(1, &m_render_data.vao_id));
            m_render_data.vao_id = 0;
        }
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    m_render_data.vertices_count = 0;
    m_render_data.indices_count  = 0;
    m_render_data.geometry.vertices = std::vector<float>();
    m_render_data.geometry.indices  = std::vector<unsigned int>();
    m_bounding_box = BoundingBoxf3();
    m_filename = std::string();
}

static GLenum get_primitive_mode(const GLModel::Geometry::Format& format)
{
    switch (format.type)
    {
    case GLModel::Geometry::EPrimitiveType::Points:        { return GL_POINTS; }
    default:
    case GLModel::Geometry::EPrimitiveType::Triangles:     { return GL_TRIANGLES; }
    case GLModel::Geometry::EPrimitiveType::TriangleStrip: { return GL_TRIANGLE_STRIP; }
    case GLModel::Geometry::EPrimitiveType::TriangleFan:   { return GL_TRIANGLE_FAN; }
    case GLModel::Geometry::EPrimitiveType::Lines:         { return GL_LINES; }
    case GLModel::Geometry::EPrimitiveType::LineStrip:     { return GL_LINE_STRIP; }
    case GLModel::Geometry::EPrimitiveType::LineLoop:      { return GL_LINE_LOOP; }
    }
}

static GLenum get_index_type(const GLModel::Geometry& data)
{
    switch (data.index_type)
    {
    default:
    case GLModel::Geometry::EIndexType::UINT:   { return GL_UNSIGNED_INT; }
    case GLModel::Geometry::EIndexType::USHORT: { return GL_UNSIGNED_SHORT; }
    case GLModel::Geometry::EIndexType::UBYTE:  { return GL_UNSIGNED_BYTE; }
    }
}

void GLModel::render()
{
    render(std::make_pair<size_t, size_t>(0, indices_count()));
}

void GLModel::render(const std::pair<size_t, size_t>& range)
{
    if (m_render_disabled)
        return;

    if (range.second == range.first)
        return;

    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    // sends data to gpu if not done yet
    if (m_render_data.vbo_id == 0 || m_render_data.ibo_id == 0) {
        if (m_render_data.geometry.vertices_count() > 0 && m_render_data.geometry.indices_count() > 0 && !send_to_gpu())
            return;
    }

    const Geometry& data = m_render_data.geometry;

    const GLenum mode = get_primitive_mode(data.format);
    const GLenum index_type = get_index_type(data);

    const size_t vertex_stride_bytes = Geometry::vertex_stride_bytes(data.format);
    const bool position = Geometry::has_position(data.format);
    const bool normal = Geometry::has_normal(data.format);
    const bool tex_coord = Geometry::has_tex_coord(data.format);

#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(m_render_data.vao_id));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
    // the following binding is needed to set the vertex attributes
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_render_data.vbo_id));

    int position_id = -1;
    int normal_id = -1;
    int tex_coord_id = -1;

    if (position) {
        position_id = shader->get_attrib_location("v_position");
        if (position_id != -1) {
            glsafe(::glVertexAttribPointer(position_id, Geometry::position_stride_floats(data.format), GL_FLOAT, GL_FALSE, vertex_stride_bytes, (const void*)Geometry::position_offset_bytes(data.format)));
            glsafe(::glEnableVertexAttribArray(position_id));
        }
    }
    if (normal) {
        normal_id = shader->get_attrib_location("v_normal");
        if (normal_id != -1) {
            glsafe(::glVertexAttribPointer(normal_id, Geometry::normal_stride_floats(data.format), GL_FLOAT, GL_FALSE, vertex_stride_bytes, (const void*)Geometry::normal_offset_bytes(data.format)));
            glsafe(::glEnableVertexAttribArray(normal_id));
        }
    }
    if (tex_coord) {
        tex_coord_id = shader->get_attrib_location("v_tex_coord");
        if (tex_coord_id != -1) {
            glsafe(::glVertexAttribPointer(tex_coord_id, Geometry::tex_coord_stride_floats(data.format), GL_FLOAT, GL_FALSE, vertex_stride_bytes, (const void*)Geometry::tex_coord_offset_bytes(data.format)));
            glsafe(::glEnableVertexAttribArray(tex_coord_id));
        }
    }

    shader->set_uniform("uniform_color", data.color);

    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_render_data.ibo_id));
    glsafe(::glDrawElements(mode, range.second - range.first, index_type, (const void*)(range.first * Geometry::index_stride_bytes(data))));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

    if (tex_coord_id != -1)
        glsafe(::glDisableVertexAttribArray(tex_coord_id));
    if (normal_id != -1)
        glsafe(::glDisableVertexAttribArray(normal_id));
    if (position_id != -1)
        glsafe(::glDisableVertexAttribArray(position_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
}

void GLModel::render_instanced(unsigned int instances_vbo, unsigned int instances_count)
{
    if (instances_vbo == 0 || instances_count == 0)
        return;

    GLShaderProgram* shader = wxGetApp().get_current_shader();
    if (shader == nullptr || !boost::algorithm::iends_with(shader->get_name(), "_instanced"))
        return;

    // vertex attributes
    const GLint position_id = shader->get_attrib_location("v_position");
    const GLint normal_id   = shader->get_attrib_location("v_normal");
    if (position_id == -1 || normal_id == -1)
        return;

    // instance attributes
    const GLint offset_id = shader->get_attrib_location("i_offset");
    const GLint scales_id = shader->get_attrib_location("i_scales");
    if (offset_id == -1 || scales_id == -1)
        return;

    if (m_render_data.vbo_id == 0 || m_render_data.ibo_id == 0) {
        if (!send_to_gpu())
            return;
    }

#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(m_render_data.vao_id));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, instances_vbo));
    const size_t instance_stride = 5 * sizeof(float);
    glsafe(::glVertexAttribPointer(offset_id, 3, GL_FLOAT, GL_FALSE, instance_stride, (const void*)0));
    glsafe(::glEnableVertexAttribArray(offset_id));
    glsafe(::glVertexAttribDivisor(offset_id, 1));

    glsafe(::glVertexAttribPointer(scales_id, 2, GL_FLOAT, GL_FALSE, instance_stride, (const void*)(3 * sizeof(float))));
    glsafe(::glEnableVertexAttribArray(scales_id));
    glsafe(::glVertexAttribDivisor(scales_id, 1));

    const Geometry& data = m_render_data.geometry;

    const GLenum mode = get_primitive_mode(data.format);
    const GLenum index_type = get_index_type(data);

    const size_t vertex_stride_bytes = Geometry::vertex_stride_bytes(data.format);
    const bool position = Geometry::has_position(data.format);
    const bool normal   = Geometry::has_normal(data.format);

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_render_data.vbo_id));

    if (position) {
        glsafe(::glVertexAttribPointer(position_id, Geometry::position_stride_floats(data.format), GL_FLOAT, GL_FALSE, vertex_stride_bytes, (const void*)Geometry::position_offset_bytes(data.format)));
        glsafe(::glEnableVertexAttribArray(position_id));
    }

    if (normal) {
        glsafe(::glVertexAttribPointer(normal_id, Geometry::normal_stride_floats(data.format), GL_FLOAT, GL_FALSE, vertex_stride_bytes, (const void*)Geometry::normal_offset_bytes(data.format)));
        glsafe(::glEnableVertexAttribArray(normal_id));
    }

    shader->set_uniform("uniform_color", data.color);

    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_render_data.ibo_id));
    glsafe(::glDrawElementsInstanced(mode, indices_count(), index_type, (const void*)0, instances_count));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));

    if (normal)
        glsafe(::glDisableVertexAttribArray(normal_id));
    if (position)
        glsafe(::glDisableVertexAttribArray(position_id));

    glsafe(::glDisableVertexAttribArray(scales_id));
    glsafe(::glDisableVertexAttribArray(offset_id));

    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES
}

bool GLModel::send_to_gpu()
{
    if (m_render_data.vbo_id > 0 || m_render_data.ibo_id > 0) {
        assert(false);
        return false;
    }

    Geometry& data = m_render_data.geometry;
    if (data.vertices.empty() || data.indices.empty()) {
        assert(false);
        return false;
    }

#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glGenVertexArrays(1, &m_render_data.vao_id));
        glsafe(::glBindVertexArray(m_render_data.vao_id));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    // vertices
    glsafe(::glGenBuffers(1, &m_render_data.vbo_id));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, m_render_data.vbo_id));
    glsafe(::glBufferData(GL_ARRAY_BUFFER, data.vertices_size_bytes(), data.vertices.data(), GL_STATIC_DRAW));
    glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
    m_render_data.vertices_count = vertices_count();
    data.vertices = std::vector<float>();

    // indices
    glsafe(::glGenBuffers(1, &m_render_data.ibo_id));
    glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_render_data.ibo_id));
    const size_t indices_count = data.indices.size();
    if (m_render_data.vertices_count <= 256) {
        // convert indices to unsigned char to save gpu memory
        std::vector<unsigned char> reduced_indices(indices_count);
        for (size_t i = 0; i < indices_count; ++i) {
            reduced_indices[i] = (unsigned char)data.indices[i];
        }
        data.index_type = Geometry::EIndexType::UBYTE;
        glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices_count * sizeof(unsigned char), reduced_indices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }
    else if (m_render_data.vertices_count <= 65536) {
        // convert indices to unsigned short to save gpu memory
        std::vector<unsigned short> reduced_indices(indices_count);
        for (size_t i = 0; i < data.indices.size(); ++i) {
            reduced_indices[i] = (unsigned short)data.indices[i];
        }
        data.index_type = Geometry::EIndexType::USHORT;
        glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, indices_count * sizeof(unsigned short), reduced_indices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }
    else {
        data.index_type = Geometry::EIndexType::UINT;
        glsafe(::glBufferData(GL_ELEMENT_ARRAY_BUFFER, data.indices_size_bytes(), data.indices.data(), GL_STATIC_DRAW));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }
    m_render_data.indices_count = indices_count;
    data.indices = std::vector<unsigned int>();

#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_core_profile()) {
#endif // !SLIC3R_OPENGL_ES
        glsafe(::glBindVertexArray(0));
#if !SLIC3R_OPENGL_ES
    }
#endif // !SLIC3R_OPENGL_ES

    return true;
}

template<typename Fn>
inline bool all_vertices_inside(const GLModel::Geometry& geometry, Fn fn)
{
    const size_t position_stride_floats = geometry.position_stride_floats(geometry.format);
    const size_t position_offset_floats = geometry.position_offset_floats(geometry.format);
    assert(position_stride_floats == 3);
    if (geometry.vertices.empty() || position_stride_floats != 3)
        return false;

    for (auto it = geometry.vertices.begin(); it != geometry.vertices.end(); ) {
        it += position_offset_floats;
        if (!fn({ *it, *(it + 1), *(it + 2) }))
            return false;
        it += (geometry.vertex_stride_floats(geometry.format) - position_offset_floats - position_stride_floats);
    }
    return true;
}

bool contains(const BuildVolume& volume, const GLModel& model, bool ignore_bottom)
{
    static constexpr const double epsilon = BuildVolume::BedEpsilon;
    switch (volume.type()) {
    case BuildVolume_Type::Rectangle:
    {
        BoundingBox3Base<Vec3d> build_volume = volume.bounding_volume().inflated(epsilon);
        if (volume.printable_height() == 0.0)
            build_volume.max.z() = std::numeric_limits<double>::max();
        if (ignore_bottom)
            build_volume.min.z() = -std::numeric_limits<double>::max();
        const BoundingBoxf3& model_box = model.get_bounding_box();
        return build_volume.contains(model_box.min) && build_volume.contains(model_box.max);
    }
    case BuildVolume_Type::Circle:
    {
        const Geometry::Circled& circle = volume.circle();
        const Vec2f c = unscaled<float>(circle.center);
        const float r = unscaled<double>(circle.radius) + float(epsilon);
        const float r2 = sqr(r);
        return volume.printable_height() == 0.0 ?
            all_vertices_inside(model.get_geometry(), [c, r2](const Vec3f& p) { return (to_2d(p) - c).squaredNorm() <= r2; }) :

            all_vertices_inside(model.get_geometry(), [c, r2, z = volume.printable_height() + epsilon](const Vec3f& p) { return (to_2d(p) - c).squaredNorm() <= r2 && p.z() <= z; });
    }
    case BuildVolume_Type::Convex:
        //FIXME doing test on convex hull until we learn to do test on non-convex polygons efficiently.
    case BuildVolume_Type::Custom:
        return volume.printable_height() == 0.0 ?
            all_vertices_inside(model.get_geometry(), [&volume](const Vec3f& p) { return Geometry::inside_convex_polygon(volume.top_bottom_convex_hull_decomposition_bed(), to_2d(p).cast<double>()); }) :
            all_vertices_inside(model.get_geometry(), [&volume, z = volume.printable_height() + epsilon](const Vec3f& p) { return Geometry::inside_convex_polygon(volume.top_bottom_convex_hull_decomposition_bed(), to_2d(p).cast<double>()) && p.z() <= z; });
    default:
        return true;
    }
}

GLModel::Geometry stilized_arrow(unsigned int resolution, float tip_radius, float tip_height, float stem_radius, float stem_height)
{
    resolution = std::max<unsigned int>(4, resolution);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(6 * resolution + 2);
    data.reserve_indices(6 * resolution * 3);

    const float angle_step = 2.0f * float(PI) / float(resolution);
    std::vector<float> cosines(resolution);
    std::vector<float> sines(resolution);

    for (unsigned int i = 0; i < resolution; ++i) {
        const float angle = angle_step * float(i);
        cosines[i] = ::cos(angle);
        sines[i] = -::sin(angle);
    }

    const float total_height = tip_height + stem_height;

    // tip vertices/normals
    data.add_vertex(Vec3f(0.0f, 0.0f, total_height), (Vec3f)Vec3f::UnitZ());
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_vertex(Vec3f(tip_radius * sines[i], tip_radius * cosines[i], stem_height), Vec3f(sines[i], cosines[i], 0.0f));
    }

    // tip triangles
    for (unsigned int i = 0; i < resolution; ++i) {
        const unsigned int v3 = (i < resolution - 1) ? i + 2 : 1;
        data.add_triangle(0, i + 1, v3);
    }

    // tip cap outer perimeter vertices
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_vertex(Vec3f(tip_radius * sines[i], tip_radius * cosines[i], stem_height), (Vec3f)(-Vec3f::UnitZ()));
    }

    // tip cap inner perimeter vertices
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_vertex(Vec3f(stem_radius * sines[i], stem_radius * cosines[i], stem_height), (Vec3f)(-Vec3f::UnitZ()));
    }

    // tip cap triangles
    for (unsigned int i = 0; i < resolution; ++i) {
        const unsigned int v2 = (i < resolution - 1) ? i + resolution + 2 : resolution + 1;
        const unsigned int v3 = (i < resolution - 1) ? i + 2 * resolution + 2 : 2 * resolution + 1;
        data.add_triangle(i + resolution + 1, v3, v2);
        data.add_triangle(i + resolution + 1, i + 2 * resolution + 1, v3);
    }

    // stem bottom vertices
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_vertex(Vec3f(stem_radius * sines[i], stem_radius * cosines[i], stem_height), Vec3f(sines[i], cosines[i], 0.0f));
    }

    // stem top vertices
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_vertex(Vec3f(stem_radius * sines[i], stem_radius * cosines[i], 0.0f), Vec3f(sines[i], cosines[i], 0.0f));
    }

    // stem triangles
    for (unsigned int i = 0; i < resolution; ++i) {
        const unsigned int v2 = (i < resolution - 1) ? i + 3 * resolution + 2 : 3 * resolution + 1;
        const unsigned int v3 = (i < resolution - 1) ? i + 4 * resolution + 2 : 4 * resolution + 1;
        data.add_triangle(i + 3 * resolution + 1, v3, v2);
        data.add_triangle(i + 3 * resolution + 1, i + 4 * resolution + 1, v3);
    }

    // stem cap vertices
    data.add_vertex((Vec3f)Vec3f::Zero(), (Vec3f)(-Vec3f::UnitZ()));
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_vertex(Vec3f(stem_radius * sines[i], stem_radius * cosines[i], 0.0f), (Vec3f)(-Vec3f::UnitZ()));
    }

    // stem cap triangles
    for (unsigned int i = 0; i < resolution; ++i) {
        const unsigned int v3 = (i < resolution - 1) ? i + 5 * resolution + 3 : 5 * resolution + 2;
        data.add_triangle(5 * resolution + 1, v3, i + 5 * resolution + 2);
    }

    return data;
}

GLModel::Geometry circular_arrow(unsigned int resolution, float radius, float tip_height, float tip_width, float stem_width, float thickness)
{
    resolution = std::max<unsigned int>(2, resolution);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(8 * (resolution + 1) + 30);
    data.reserve_indices((8 * resolution + 16) * 3);

    const float half_thickness = 0.5f * thickness;
    const float half_stem_width = 0.5f * stem_width;
    const float half_tip_width = 0.5f * tip_width;

    const float outer_radius = radius + half_stem_width;
    const float inner_radius = radius - half_stem_width;
    const float step_angle = 0.5f * float(PI) / float(resolution);

    // tip
    // top face vertices
    data.add_vertex(Vec3f(0.0f, outer_radius, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(0.0f, radius + half_tip_width, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(-tip_height, radius, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(0.0f, radius - half_tip_width, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(0.0f, inner_radius, half_thickness), (Vec3f)Vec3f::UnitZ());

    // top face triangles
    data.add_triangle(0, 1, 2);
    data.add_triangle(0, 2, 4);
    data.add_triangle(4, 2, 3);

    // bottom face vertices
    data.add_vertex(Vec3f(0.0f, outer_radius, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(0.0f, radius + half_tip_width, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(-tip_height, radius, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(0.0f, radius - half_tip_width, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(0.0f, inner_radius, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));

    // bottom face triangles
    data.add_triangle(5, 7, 6);
    data.add_triangle(5, 9, 7);
    data.add_triangle(9, 8, 7);

    // side faces vertices
    data.add_vertex(Vec3f(0.0f, outer_radius, -half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(0.0f, radius + half_tip_width, -half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(0.0f, outer_radius, half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(0.0f, radius + half_tip_width, half_thickness), (Vec3f)Vec3f::UnitX());

    Vec3f normal(-half_tip_width, tip_height, 0.0f);
    normal.normalize();
    data.add_vertex(Vec3f(0.0f, radius + half_tip_width, -half_thickness), normal);
    data.add_vertex(Vec3f(-tip_height, radius, -half_thickness), normal);
    data.add_vertex(Vec3f(0.0f, radius + half_tip_width, half_thickness), normal);
    data.add_vertex(Vec3f(-tip_height, radius, half_thickness), normal);

    normal = { -half_tip_width, -tip_height, 0.0f };
    normal.normalize();
    data.add_vertex(Vec3f(-tip_height, radius, -half_thickness), normal);
    data.add_vertex(Vec3f(0.0f, radius - half_tip_width, -half_thickness), normal);
    data.add_vertex(Vec3f(-tip_height, radius, half_thickness), normal);
    data.add_vertex(Vec3f(0.0f, radius - half_tip_width, half_thickness), normal);

    data.add_vertex(Vec3f(0.0f, radius - half_tip_width, -half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(0.0f, inner_radius, -half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(0.0f, radius - half_tip_width, half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(0.0f, inner_radius, half_thickness), (Vec3f)Vec3f::UnitX());

    // side face triangles
    for (unsigned int i = 0; i < 4; ++i) {
        const unsigned int ii = i * 4;
        data.add_triangle(10 + ii, 11 + ii, 13 + ii);
        data.add_triangle(10 + ii, 13 + ii, 12 + ii);
    }

    // stem
    // top face vertices
    for (unsigned int i = 0; i <= resolution; ++i) {
        const float angle = float(i) * step_angle;
        data.add_vertex(Vec3f(inner_radius * ::sin(angle), inner_radius * ::cos(angle), half_thickness), (Vec3f)Vec3f::UnitZ());
    }

    for (unsigned int i = 0; i <= resolution; ++i) {
        const float angle = float(i) * step_angle;
        data.add_vertex(Vec3f(outer_radius * ::sin(angle), outer_radius * ::cos(angle), half_thickness), (Vec3f)Vec3f::UnitZ());
    }

    // top face triangles
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_triangle(26 + i, 27 + i, 27 + resolution + i);
        data.add_triangle(27 + i, 28 + resolution + i, 27 + resolution + i);
    }

    // bottom face vertices
    for (unsigned int i = 0; i <= resolution; ++i) {
        const float angle = float(i) * step_angle;
        data.add_vertex(Vec3f(inner_radius * ::sin(angle), inner_radius * ::cos(angle), -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    }

    for (unsigned int i = 0; i <= resolution; ++i) {
        const float angle = float(i) * step_angle;
        data.add_vertex(Vec3f(outer_radius * ::sin(angle), outer_radius * ::cos(angle), -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    }

    // bottom face triangles
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_triangle(28 + 2 * resolution + i, 29 + 3 * resolution + i, 29 + 2 * resolution + i);
        data.add_triangle(29 + 2 * resolution + i, 29 + 3 * resolution + i, 30 + 3 * resolution + i);
    }

    // side faces vertices and triangles
    for (unsigned int i = 0; i <= resolution; ++i) {
        const float angle = float(i) * step_angle;
        const float c = ::cos(angle);
        const float s = ::sin(angle);
        data.add_vertex(Vec3f(inner_radius * s, inner_radius * c, -half_thickness), Vec3f(-s, -c, 0.0f));
    }

    for (unsigned int i = 0; i <= resolution; ++i) {
        const float angle = float(i) * step_angle;
        const float c = ::cos(angle);
        const float s = ::sin(angle);
        data.add_vertex(Vec3f(inner_radius * s, inner_radius * c, half_thickness), Vec3f(-s, -c, 0.0f));
    }

    unsigned int first_id = 26 + 4 * (resolution + 1);
    for (unsigned int i = 0; i < resolution; ++i) {
        const unsigned int ii = first_id + i;
        data.add_triangle(ii, ii + 1, ii + resolution + 2);
        data.add_triangle(ii, ii + resolution + 2, ii + resolution + 1);
    }

    data.add_vertex(Vec3f(inner_radius, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(outer_radius, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(inner_radius, 0.0f, half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(outer_radius, 0.0f, half_thickness), (Vec3f)(-Vec3f::UnitY()));

    first_id = 26 + 6 * (resolution + 1);
    data.add_triangle(first_id, first_id + 1, first_id + 3);
    data.add_triangle(first_id, first_id + 3, first_id + 2);

    for (int i = resolution; i >= 0; --i) {
        const float angle = float(i) * step_angle;
        const float c = ::cos(angle);
        const float s = ::sin(angle);
        data.add_vertex(Vec3f(outer_radius * s, outer_radius * c, -half_thickness), Vec3f(s, c, 0.0f));
    }

    for (int i = resolution; i >= 0; --i) {
        const float angle = float(i) * step_angle;
        const float c = ::cos(angle);
        const float s = ::sin(angle);
        data.add_vertex(Vec3f(outer_radius * s, outer_radius * c, +half_thickness), Vec3f(s, c, 0.0f));
    }

    first_id = 30 + 6 * (resolution + 1);
    for (unsigned int i = 0; i < resolution; ++i) {
        const unsigned int ii = first_id + i;
        data.add_triangle(ii, ii + 1, ii + resolution + 2);
        data.add_triangle(ii, ii + resolution + 2, ii + resolution + 1);
    }

    return data;
}

GLModel::Geometry straight_arrow(float tip_width, float tip_height, float stem_width, float stem_height, float thickness)
{
    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(42);
    data.reserve_indices(72);

    const float half_thickness = 0.5f * thickness;
    const float half_stem_width = 0.5f * stem_width;
    const float half_tip_width = 0.5f * tip_width;
    const float total_height = tip_height + stem_height;

    // top face vertices
    data.add_vertex(Vec3f(half_stem_width, 0.0f, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(half_stem_width, stem_height, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(half_tip_width, stem_height, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(0.0f, total_height, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(-half_tip_width, stem_height, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(-half_stem_width, stem_height, half_thickness), (Vec3f)Vec3f::UnitZ());
    data.add_vertex(Vec3f(-half_stem_width, 0.0f, half_thickness), (Vec3f)Vec3f::UnitZ());

    // top face triangles
    data.add_triangle(0, 1, 6);
    data.add_triangle(6, 1, 5);
    data.add_triangle(4, 5, 3);
    data.add_triangle(5, 1, 3);
    data.add_triangle(1, 2, 3);

    // bottom face vertices
    data.add_vertex(Vec3f(half_stem_width, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(half_stem_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(half_tip_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(0.0f, total_height, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(-half_tip_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(-half_stem_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));
    data.add_vertex(Vec3f(-half_stem_width, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitZ()));

    // bottom face triangles
    data.add_triangle(7, 13, 8);
    data.add_triangle(13, 12, 8);
    data.add_triangle(12, 11, 10);
    data.add_triangle(8, 12, 10);
    data.add_triangle(9, 8, 10);

    // side faces vertices
    data.add_vertex(Vec3f(half_stem_width, 0.0f, -half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(half_stem_width, stem_height, -half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(half_stem_width, 0.0f, half_thickness), (Vec3f)Vec3f::UnitX());
    data.add_vertex(Vec3f(half_stem_width, stem_height, half_thickness), (Vec3f)Vec3f::UnitX());

    data.add_vertex(Vec3f(half_stem_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(half_tip_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(half_stem_width, stem_height, half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(half_tip_width, stem_height, half_thickness), (Vec3f)(-Vec3f::UnitY()));

    Vec3f normal(tip_height, half_tip_width, 0.0f);
    normal.normalize();
    data.add_vertex(Vec3f(half_tip_width, stem_height, -half_thickness), normal);
    data.add_vertex(Vec3f(0.0f, total_height, -half_thickness), normal);
    data.add_vertex(Vec3f(half_tip_width, stem_height, half_thickness), normal);
    data.add_vertex(Vec3f(0.0f, total_height, half_thickness), normal);

    normal = { -tip_height, half_tip_width, 0.0f };
    normal.normalize();
    data.add_vertex(Vec3f(0.0f, total_height, -half_thickness), normal);
    data.add_vertex(Vec3f(-half_tip_width, stem_height, -half_thickness), normal);
    data.add_vertex(Vec3f(0.0f, total_height, half_thickness), normal);
    data.add_vertex(Vec3f(-half_tip_width, stem_height, half_thickness), normal);

    data.add_vertex(Vec3f(-half_tip_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(-half_stem_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(-half_tip_width, stem_height, half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(-half_stem_width, stem_height, half_thickness), (Vec3f)(-Vec3f::UnitY()));

    data.add_vertex(Vec3f(-half_stem_width, stem_height, -half_thickness), (Vec3f)(-Vec3f::UnitX()));
    data.add_vertex(Vec3f(-half_stem_width, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitX()));
    data.add_vertex(Vec3f(-half_stem_width, stem_height, half_thickness), (Vec3f)(-Vec3f::UnitX()));
    data.add_vertex(Vec3f(-half_stem_width, 0.0f, half_thickness), (Vec3f)(-Vec3f::UnitX()));

    data.add_vertex(Vec3f(-half_stem_width, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(half_stem_width, 0.0f, -half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(-half_stem_width, 0.0f, half_thickness), (Vec3f)(-Vec3f::UnitY()));
    data.add_vertex(Vec3f(half_stem_width, 0.0f, half_thickness), (Vec3f)(-Vec3f::UnitY()));

    // side face triangles
    for (unsigned int i = 0; i < 7; ++i) {
        const unsigned int ii = i * 4;
        data.add_triangle(14 + ii, 15 + ii, 17 + ii);
        data.add_triangle(14 + ii, 17 + ii, 16 + ii);
    }

    return data;
}

GLModel::Geometry diamond(unsigned int resolution)
{
    resolution = std::max<unsigned int>(4, resolution);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(resolution + 2);
    data.reserve_indices((2 * (resolution + 1)) * 3);

    const float step = 2.0f * float(PI) / float(resolution);

    // vertices
    for (unsigned int i = 0; i < resolution; ++i) {
        const float ii = float(i) * step;
        const Vec3f p = { 0.5f * ::cos(ii), 0.5f * ::sin(ii), 0.0f };
        data.add_vertex(p, (Vec3f)p.normalized());
    }
    Vec3f p = { 0.0f, 0.0f, 0.5f };
    data.add_vertex(p, (Vec3f)p.normalized());
    p = { 0.0f, 0.0f, -0.5f };
    data.add_vertex(p, (Vec3f)p.normalized());

    // triangles
    // top
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_triangle(i + 0, i + 1, resolution);
    }
    data.add_triangle(resolution - 1, 0, resolution);

    // bottom
    for (unsigned int i = 0; i < resolution; ++i) {
        data.add_triangle(i + 0, resolution + 1, i + 1);
    }
    data.add_triangle(resolution - 1, resolution + 1, 0);

    return data;
}

GLModel::Geometry smooth_sphere(unsigned int resolution, float radius)
{
    resolution = std::max<unsigned int>(4, resolution);

    const unsigned int sectorCount = resolution;
    const unsigned int stackCount  = resolution;

    const float sectorStep = float(2.0 * M_PI / sectorCount);
    const float stackStep = float(M_PI / stackCount);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices((stackCount - 1) * sectorCount + 2);
    data.reserve_indices((2 * (stackCount - 1) * sectorCount) * 3);

    // vertices
    for (unsigned int i = 0; i <= stackCount; ++i) {
        // from pi/2 to -pi/2
        const double stackAngle = 0.5 * M_PI - stackStep * i;
        const double xy = double(radius) * ::cos(stackAngle);
        const double z = double(radius) * ::sin(stackAngle);
        if (i == 0 || i == stackCount) {
            const Vec3f v(float(xy), 0.0f, float(z));
            data.add_vertex(v, (Vec3f)v.normalized());
        }
        else {
            for (unsigned int j = 0; j < sectorCount; ++j) {
                // from 0 to 2pi
                const double sectorAngle = sectorStep * j;
                const Vec3f v(float(xy * std::cos(sectorAngle)), float(xy * std::sin(sectorAngle)), float(z));
                data.add_vertex(v, (Vec3f)v.normalized());
            }
        }
    }

    // triangles
    for (unsigned int i = 0; i < stackCount; ++i) {
        // Beginning of current stack.
        unsigned int k1 = (i == 0) ? 0 : (1 + (i - 1) * sectorCount);
        const unsigned int k1_first = k1;
        // Beginning of next stack.
        unsigned int k2 = (i == 0) ? 1 : (k1 + sectorCount);
        const unsigned int k2_first = k2;
        for (unsigned int j = 0; j < sectorCount; ++j) {
            // 2 triangles per sector excluding first and last stacks
            unsigned int k1_next = k1;
            unsigned int k2_next = k2;
            if (i != 0) {
                k1_next = (j + 1 == sectorCount) ? k1_first : (k1 + 1);
                data.add_triangle(k1, k2, k1_next);
            }
            if (i + 1 != stackCount) {
                k2_next = (j + 1 == sectorCount) ? k2_first : (k2 + 1);
                data.add_triangle(k1_next, k2, k2_next);
            }
            k1 = k1_next;
            k2 = k2_next;
        }
    }

    return data;
}

GLModel::Geometry smooth_cylinder(unsigned int resolution, float radius, float height)
{
    resolution = std::max<unsigned int>(4, resolution);

    const unsigned int sectorCount = resolution;
    const float sectorStep = 2.0f * float(M_PI) / float(sectorCount);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(sectorCount * 4 + 2);
    data.reserve_indices(sectorCount * 4 * 3);

    auto generate_vertices_on_circle = [sectorCount, sectorStep](float radius) {
        std::vector<Vec3f> ret;
        ret.reserve(sectorCount);
        for (unsigned int i = 0; i < sectorCount; ++i) {
            // from 0 to 2pi
            const float sectorAngle = sectorStep * i;
            ret.emplace_back(radius * std::cos(sectorAngle), radius * std::sin(sectorAngle), 0.0f);
        }
        return ret;
    };

    const std::vector<Vec3f> base_vertices = generate_vertices_on_circle(radius);
    const Vec3f h = height * Vec3f::UnitZ();

    // stem vertices
    for (unsigned int i = 0; i < sectorCount; ++i) {
        const Vec3f& v = base_vertices[i];
        const Vec3f n = v.normalized();
        data.add_vertex(v, n);
        data.add_vertex(v + h, n);
    }

    // stem triangles
    for (unsigned int i = 0; i < sectorCount; ++i) {
        unsigned int v1 = i * 2;
        unsigned int v2 = (i < sectorCount - 1) ? v1 + 2 : 0;
        unsigned int v3 = v2 + 1;
        unsigned int v4 = v1 + 1;
        data.add_triangle(v1, v2, v3);
        data.add_triangle(v1, v3, v4);
    }

    // bottom cap vertices
    Vec3f cap_center = Vec3f::Zero();
    unsigned int cap_center_id = data.vertices_count();
    Vec3f normal = -Vec3f::UnitZ();

    data.add_vertex(cap_center, normal);
    for (unsigned int i = 0; i < sectorCount; ++i) {
        data.add_vertex(base_vertices[i], normal);
    }

    // bottom cap triangles
    for (unsigned int i = 0; i < sectorCount; ++i) {
        data.add_triangle(cap_center_id, (i < sectorCount - 1) ? cap_center_id + i + 2 : cap_center_id + 1, cap_center_id + i + 1);
    }

    // top cap vertices
    cap_center += h;
    cap_center_id = data.vertices_count();
    normal = -normal;

    data.add_vertex(cap_center, normal);
    for (unsigned int i = 0; i < sectorCount; ++i) {
        data.add_vertex(base_vertices[i] + h, normal);
    }

    // top cap triangles
    for (unsigned int i = 0; i < sectorCount; ++i) {
        data.add_triangle(cap_center_id, cap_center_id + i + 1, (i < sectorCount - 1) ? cap_center_id + i + 2 : cap_center_id + 1);
    }

    return data;
}

GLModel::Geometry smooth_torus(unsigned int primary_resolution, unsigned int secondary_resolution, float radius, float thickness)
{
    const unsigned int torus_sector_count = std::max<unsigned int>(4, primary_resolution);
    const float torus_sector_step = 2.0f * float(M_PI) / float(torus_sector_count);
    const unsigned int section_sector_count = std::max<unsigned int>(4, secondary_resolution);
    const float section_sector_step = 2.0f * float(M_PI) / float(section_sector_count);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(torus_sector_count * section_sector_count);
    data.reserve_indices(torus_sector_count * section_sector_count * 2 * 3);

    // vertices
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const float section_angle = torus_sector_step * i;
        const float csa = std::cos(section_angle);
        const float ssa = std::sin(section_angle);
        const Vec3f section_center(radius * csa, radius * ssa, 0.0f);
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const float circle_angle = section_sector_step * j;
            const float thickness_xy = thickness * std::cos(circle_angle);
            const float thickness_z  = thickness * std::sin(circle_angle);
            const Vec3f v(thickness_xy * csa, thickness_xy * ssa, thickness_z);
            data.add_vertex(section_center + v, (Vec3f)v.normalized());
        }
    }

    // triangles
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const unsigned int ii = i * section_sector_count;
        const unsigned int ii_next = ((i + 1) % torus_sector_count) * section_sector_count;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const unsigned int j_next = (j + 1) % section_sector_count;
            const unsigned int i0 = ii + j;
            const unsigned int i1 = ii_next + j;
            const unsigned int i2 = ii_next + j_next;
            const unsigned int i3 = ii + j_next;
            data.add_triangle(i0, i1, i2);
            data.add_triangle(i0, i2, i3);
        }
    }

    return data;
}

GLModel::Geometry init_plane_data(const indexed_triangle_set& its, const std::vector<int>& triangle_indices, float normal_offset)
{
    GLModel::Geometry init_data;
    init_data.format = { GUI::GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    init_data.reserve_indices(3 * triangle_indices.size());
    init_data.reserve_vertices(3 * triangle_indices.size());
    unsigned int i = 0;
    for (int idx : triangle_indices) {
        Vec3f v0 = its.vertices[its.indices[idx][0]];
        Vec3f v1 = its.vertices[its.indices[idx][1]];
        Vec3f v2 = its.vertices[its.indices[idx][2]];
        const Vec3f n = (v1 - v0).cross(v2 - v0).normalized();
        if (std::abs(normal_offset) > 0.0) {
            v0 = v0 + n * normal_offset;
            v1 = v1 + n * normal_offset;
            v2 = v2 + n * normal_offset;
        }
        init_data.add_vertex(v0, n);
        init_data.add_vertex(v1, n);
        init_data.add_vertex(v2, n);
        init_data.add_triangle(i, i + 1, i + 2);
        i += 3;
    }

    return init_data;
}

GLModel::Geometry init_torus_data(unsigned int       primary_resolution,
                                  unsigned int       secondary_resolution,
                                  const Vec3f &      center,
                                  float              radius,
                                  float              thickness,
                                  const Vec3f &      model_axis,
                                  const Transform3f &world_trafo)
{
    const unsigned int torus_sector_count   = std::max<unsigned int>(4, primary_resolution);
    const unsigned int section_sector_count = std::max<unsigned int>(4, secondary_resolution);
    const float        torus_sector_step    = 2.0f * float(M_PI) / float(torus_sector_count);
    const float        section_sector_step  = 2.0f * float(M_PI) / float(section_sector_count);

    GLModel::Geometry data;
    data.format = { GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3 };
    data.reserve_vertices(torus_sector_count * section_sector_count);
    data.reserve_indices(torus_sector_count * section_sector_count * 2 * 3);

    // vertices
    const Transform3f local_to_world_matrix = world_trafo * Geometry::translation_transform(center.cast<double>()).cast<float>() *
                                              Eigen::Quaternion<float>::FromTwoVectors(Vec3f::UnitZ(), model_axis);
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const float section_angle = torus_sector_step * i;
        const Vec3f radius_dir(std::cos(section_angle), std::sin(section_angle), 0.0f);
        const Vec3f local_section_center = radius * radius_dir;
        const Vec3f world_section_center = local_to_world_matrix * local_section_center;
        const Vec3f local_section_normal = local_section_center.normalized().cross(Vec3f::UnitZ()).normalized();
        const Vec3f world_section_normal = (Vec3f) (local_to_world_matrix.matrix().block(0, 0, 3, 3) * local_section_normal).normalized();
        const Vec3f base_v               = thickness * radius_dir;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const Vec3f v = Eigen::AngleAxisf(section_sector_step * j, world_section_normal) * base_v;
            data.add_vertex(world_section_center + v, (Vec3f) v.normalized());
        }
    }

    // triangles
    for (unsigned int i = 0; i < torus_sector_count; ++i) {
        const unsigned int ii      = i * section_sector_count;
        const unsigned int ii_next = ((i + 1) % torus_sector_count) * section_sector_count;
        for (unsigned int j = 0; j < section_sector_count; ++j) {
            const unsigned int j_next = (j + 1) % section_sector_count;
            const unsigned int i0     = ii + j;
            const unsigned int i1     = ii_next + j;
            const unsigned int i2     = ii_next + j_next;
            const unsigned int i3     = ii + j_next;
            data.add_triangle(i0, i1, i2);
            data.add_triangle(i0, i2, i3);
        }
    }

    return data;
}
} // namespace GUI
} // namespace Slic3r
