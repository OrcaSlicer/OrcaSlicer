#ifndef slic3r_GLModel_hpp_
#define slic3r_GLModel_hpp_

#include "libslic3r/Point.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include <vector>
#include <string>

struct indexed_triangle_set;

namespace Slic3r {

class TriangleMesh;
class Polygon;
using Polygons = std::vector<Polygon, PointsAllocator<Polygon>>;
class BuildVolume;

namespace GUI {

    class GLModel
    {
    public:
        struct Geometry
        {
            enum class EPrimitiveType : unsigned char
            {
                Points,
                Triangles,
                TriangleStrip,
                TriangleFan,
                Lines,
                LineStrip,
                LineLoop
            };

            enum class EVertexLayout : unsigned char
            {
                P2,   // position 2 floats
                P2T2, // position 2 floats + texture coords 2 floats
                P3,   // position 3 floats
                P3T2, // position 3 floats + texture coords 2 floats
                P3N3, // position 3 floats + normal 3 floats
                P3N3T2, // position 3 floats + normal 3 floats + texture coords 2 floats
                P4,   // position 4 floats
            };

            enum class EIndexType : unsigned char
            {
                UINT,   // unsigned int
                USHORT, // unsigned short
                UBYTE   // unsigned byte
            };

            struct Format
            {
                EPrimitiveType type{ EPrimitiveType::Triangles };
                EVertexLayout vertex_layout{ EVertexLayout::P3N3 };
            };

            Format format;
            std::vector<float> vertices;
            std::vector<unsigned int> indices;
            EIndexType index_type{ EIndexType::UINT };
            ColorRGBA color{ ColorRGBA::BLACK() };

            void reserve_vertices(size_t vertices_count) { vertices.reserve(vertices_count * vertex_stride_floats(format)); }
            void reserve_indices(size_t indices_count) { indices.reserve(indices_count); }

            void add_vertex(const Vec2f& position);                                              // EVertexLayout::P2
            void add_vertex(const Vec2f& position, const Vec2f& tex_coord);                      // EVertexLayout::P2T2
            void add_vertex(const Vec3f& position);                                              // EVertexLayout::P3
            void add_vertex(const Vec3f& position, const Vec2f& tex_coord);                      // EVertexLayout::P3T2
            void add_vertex(const Vec3f& position, const Vec3f& normal);                         // EVertexLayout::P3N3
            void add_vertex(const Vec3f& position, const Vec3f& normal, const Vec2f& tex_coord); // EVertexLayout::P3N3T2
            void add_vertex(const Vec4f& position);                                              // EVertexLayout::P4

            void set_vertex(size_t id, const Vec3f& position, const Vec3f& normal); // EVertexLayout::P3N3

            void set_index(size_t id, unsigned int index);

            void add_index(unsigned int id);
            void add_line(unsigned int id1, unsigned int id2);
            void add_triangle(unsigned int id1, unsigned int id2, unsigned int id3);

            Vec2f extract_position_2(size_t id) const;
            Vec3f extract_position_3(size_t id) const;
            Vec3f extract_normal_3(size_t id) const;
            Vec2f extract_tex_coord_2(size_t id) const;

            unsigned int extract_index(size_t id) const;

            void remove_vertex(size_t id);

            bool is_empty() const { return vertices_count() == 0 || indices_count() == 0; }

            size_t vertices_count() const { return vertices.size() / vertex_stride_floats(format); }
            size_t indices_count() const { return indices.size(); }

            size_t vertices_size_floats() const { return vertices.size(); }
            size_t vertices_size_bytes() const  { return vertices_size_floats() * sizeof(float); }
            size_t indices_size_bytes() const { return indices.size() * index_stride_bytes(*this); }

            indexed_triangle_set get_as_indexed_triangle_set() const;

            static size_t vertex_stride_floats(const Format& format);
            static size_t vertex_stride_bytes(const Format& format) { return vertex_stride_floats(format) * sizeof(float); }

            static size_t position_stride_floats(const Format& format);
            static size_t position_stride_bytes(const Format& format) { return position_stride_floats(format) * sizeof(float); }
            static size_t position_offset_floats(const Format& format);
            static size_t position_offset_bytes(const Format& format) { return position_offset_floats(format) * sizeof(float); }

            static size_t normal_stride_floats(const Format& format);
            static size_t normal_stride_bytes(const Format& format) { return normal_stride_floats(format) * sizeof(float); }
            static size_t normal_offset_floats(const Format& format);
            static size_t normal_offset_bytes(const Format& format) { return normal_offset_floats(format) * sizeof(float); }

            static size_t tex_coord_stride_floats(const Format& format);
            static size_t tex_coord_stride_bytes(const Format& format) { return tex_coord_stride_floats(format) * sizeof(float); }
            static size_t tex_coord_offset_floats(const Format& format);
            static size_t tex_coord_offset_bytes(const Format& format) { return tex_coord_offset_floats(format) * sizeof(float); }

            static size_t index_stride_bytes(const Geometry& data);

            static bool has_position(const Format& format);
            static bool has_normal(const Format& format);
            static bool has_tex_coord(const Format& format);
        };

        struct RenderData
        {
            Geometry geometry;
            unsigned int vao_id{ 0 };
            unsigned int vbo_id{ 0 };
            unsigned int ibo_id{ 0 };
            size_t vertices_count{ 0 };
            size_t indices_count{ 0 };
        };
    private:
        RenderData m_render_data;

        // By default the vertex and index buffers data are sent to gpu at the first call to render() method.
        // If you need to initialize a model from outside the main thread, so that a call to render() may happen
        // before the initialization is complete, use the methods:
        // disable_render()
        // ... do your initialization ...
        // enable_render()
        // to keep the data on cpu side until needed.
        bool m_render_disabled{ false };
        BoundingBoxf3 m_bounding_box;
        std::string m_filename;

    public:
        GLModel() = default;
        virtual ~GLModel() { reset(); }

        size_t vertices_count() const { return m_render_data.vertices_count > 0 ?
            m_render_data.vertices_count : m_render_data.geometry.vertices_count(); }
        size_t indices_count() const { return m_render_data.indices_count > 0 ?
            m_render_data.indices_count : m_render_data.geometry.indices_count(); }

        size_t vertices_size_floats() const { return vertices_count() * Geometry::vertex_stride_floats(m_render_data.geometry.format); }
        size_t vertices_size_bytes() const  { return vertices_size_floats() * sizeof(float); }

        size_t indices_size_bytes() const { return indices_count() * Geometry::index_stride_bytes(m_render_data.geometry); }

        const Geometry& get_geometry() const { return m_render_data.geometry; }

        void init_from(Geometry&& data);
        void init_from(const TriangleMesh& mesh);
        void init_from(const indexed_triangle_set& its);
        void init_from(const Polygons& polygons, float z);
        bool init_from_file(const std::string& filename);

        void set_color(const ColorRGBA& color) { m_render_data.geometry.color = color; }
        const ColorRGBA& get_color() const { return m_render_data.geometry.color; }

        void reset();
        void render();
        void render(const std::pair<size_t, size_t>& range);
        void render_instanced(unsigned int instances_vbo, unsigned int instances_count);

        bool is_initialized() const { return vertices_count() > 0 && indices_count() > 0; }
        bool is_empty() const { return m_render_data.geometry.is_empty(); }

        const BoundingBoxf3& get_bounding_box() const { return m_bounding_box; }
        const std::string& get_filename() const { return m_filename; }

        bool is_render_disabled() const { return m_render_disabled; }
        void enable_render() { m_render_disabled = false; }
        void disable_render() { m_render_disabled = true; }

        size_t cpu_memory_used() const {
            size_t ret = 0;
            if (!m_render_data.geometry.vertices.empty())
                ret += vertices_size_bytes();
            if (!m_render_data.geometry.indices.empty())
                ret += indices_size_bytes();
            return ret;
        }
        size_t gpu_memory_used() const {
            size_t ret = 0;
            if (m_render_data.geometry.vertices.empty())
                ret += vertices_size_bytes();
            if (m_render_data.geometry.indices.empty())
                ret += indices_size_bytes();
            return ret;
        }

    private:
        bool send_to_gpu();
    };
    bool contains(const BuildVolume& volume, const GLModel& model, bool ignore_bottom = true);

    // create an arrow with cylindrical stem and conical tip, with the given dimensions and resolution
    // the origin of the arrow is in the center of the stem cap
    // the arrow has its axis of symmetry along the Z axis and is pointing upward
    // used to render bed axes and sequential marker
    GLModel::Geometry stilized_arrow(unsigned int resolution, float tip_radius, float tip_height, float stem_radius, float stem_height);

    // create an arrow whose stem is a quarter of circle, with the given dimensions and resolution
    // the origin of the arrow is in the center of the circle
    // the arrow is contained in the 1st quadrant of the XY plane and is pointing counterclockwise
    // used to render sidebar hints for rotations
    GLModel::Geometry circular_arrow(unsigned int resolution, float radius, float tip_height, float tip_width, float stem_width, float thickness);

    // create an arrow with the given dimensions
    // the origin of the arrow is in the center of the stem cap
    // the arrow is contained in XY plane and has its main axis along the Y axis
    // used to render sidebar hints for position and scale
    GLModel::Geometry straight_arrow(float tip_width, float tip_height, float stem_width, float stem_height, float thickness);

    // create a diamond with the given resolution
    // the origin of the diamond is in its center
    // the diamond is contained into a box with size [1, 1, 1]
    GLModel::Geometry diamond(unsigned int resolution);

    // create a sphere with smooth normals
    // the origin of the sphere is in its center
    GLModel::Geometry smooth_sphere(unsigned int resolution, float radius);
    // create a cylinder with smooth normals
    // the axis of the cylinder is the Z axis
    // the origin of the cylinder is the center of its bottom cap face
    GLModel::Geometry smooth_cylinder(unsigned int resolution, float radius, float height);
    // create a torus with smooth normals
    // the axis of the torus is the Z axis
    // the origin of the torus is in its center
    GLModel::Geometry smooth_torus(unsigned int primary_resolution, unsigned int secondary_resolution, float radius, float thickness);

    GLModel::Geometry init_plane_data(const indexed_triangle_set &its, const std::vector<int> &triangle_indices,float normal_offset = 0.0f);
    GLModel::Geometry init_torus_data(unsigned int       primary_resolution,
                                      unsigned int       secondary_resolution,
                                      const Vec3f &      center,
                                      float              radius,
                                      float              thickness,
                                      const Vec3f &      model_axis,
                                      const Transform3f &world_trafo);
    } // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLModel_hpp_

