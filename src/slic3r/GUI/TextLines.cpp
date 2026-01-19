#include "TextLines.hpp"

#include <GL/glew.h>

#include "libslic3r/Model.hpp"

#include "libslic3r/Emboss.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Tesselate.hpp"

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ExPolygonsIndex.hpp"
#include "libslic3r/ClipperUtils.hpp"

#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/3DScene.hpp"

using namespace Slic3r;
using namespace Slic3r::Emboss;
using namespace Slic3r::GUI;

namespace {
// Be careful it is not water tide and contain self intersections
// It is only for visualization purposes
indexed_triangle_set its_create_torus(const Slic3r::Polygon &polygon, float radius, size_t steps = 20)
{
    assert(!polygon.empty());
    if (polygon.empty())
        return {};

    size_t count = polygon.size();
    if (count < 3)
        return {};

    // convert and scale to float
    std::vector<Vec2f> points_d;
    points_d.reserve(count);
    for (const Point &point : polygon.points)
        points_d.push_back(unscale(point).cast<float>());

    // pre calculate normalized line directions
    auto calc_line_norm = [](const Vec2f &f, const Vec2f &s) -> Vec2f { return  (s - f).normalized(); };    
    std::vector<Vec2f> line_norm(points_d.size());
    for (size_t i = 0; i < count - 1; ++i)
        line_norm[i] = calc_line_norm(points_d[i], points_d[i + 1]);
    line_norm.back() = calc_line_norm(points_d.back(), points_d.front());
        
    // precalculate sinus and cosinus
    double angle_step = 2 * M_PI / steps;
    std::vector<std::pair<double, float>> sin_cos;
    sin_cos.reserve(steps);
    for (size_t s = 0; s < steps; ++s) {
        double angle = s * angle_step;
        sin_cos.emplace_back(
            radius * std::sin(angle), 
            static_cast<float>(radius * std::cos(angle))
        );
    }
    
    indexed_triangle_set sphere = its_make_sphere(radius, 2 * PI / steps);

    // create torus model along polygon path
    indexed_triangle_set model;
    model.vertices.reserve(2 * steps * count + sphere.vertices.size()*count);
    model.indices.reserve(2 * steps * count + sphere.indices.size()*count);

    const Vec2f *prev_prev_point_d = &points_d[count-2]; // one before back
    const Vec2f *prev_point_d = &points_d.back();

    auto calc_angle = [](const Vec2f &d0, const Vec2f &d1) {
        double dot = d0.dot(d1);
        double det = d0.x() * d1.y() - d0.y() * d1.x(); // Determinant
        return std::atan2(det, dot);                    // atan2(y, x) or atan2(sin, cos)
    };

    // opposit previos direction of line - for calculate angle
    Vec2f opposit_prev_dir = (*prev_prev_point_d) - (*prev_point_d);
    for (size_t i = 0; i < count; ++i) {

        const Vec2f & point_d = points_d[i];
        // line segment direction
        Vec2f dir = point_d - (*prev_point_d);

        double angle = calc_angle(opposit_prev_dir, dir);
        double allowed_preccission = 1e-6;
        if (angle >= (PI - allowed_preccission) || 
            angle <= (-PI + allowed_preccission))
            continue; // it is almost line

        // perpendicular direction to line
        Vec2d p_dir(dir.y(), -dir.x());
        p_dir.normalize(); // Should done with double preccission
        // p_dir is tube unit side vector
        // tube unit top vector is z direction

        // Tube
        int prev_index = model.vertices.size() + 2 * sin_cos.size() - 2;
        for (const auto &[s, c] : sin_cos) {
            Vec2f side = (s * p_dir).cast<float>();
            Vec2f xy0  = side + (*prev_point_d);
            Vec2f xy1 = side + point_d;
            model.vertices.emplace_back(xy0.x(), xy0.y(), c); // pointing of prev index
            model.vertices.emplace_back(xy1.x(), xy1.y(), c);

            // create triangle indices
            int f0 = prev_index;
            int s0 = f0 + 1;
            int f1 = model.vertices.size() - 2;
            int s1 = f1 + 1;
            prev_index = f1;
            model.indices.emplace_back(s0, f0, s1);
            model.indices.emplace_back(f1, s1, f0);
        }

        prev_prev_point_d = prev_point_d;
        prev_point_d = &point_d;
        opposit_prev_dir = -dir;
    }

    // sphere on each point
    for (Vec2f& p: points_d){
        indexed_triangle_set sphere_copy = sphere;
        its_translate(sphere_copy, Vec3f(p.x(), p.y(), 0.f));
        its_merge(model, sphere_copy);
    }

    return model;
}

// select closest contour for each line
TextLines select_closest_contour(const std::vector<Polygons> &line_contours) {
    TextLines result;
    result.reserve(line_contours.size());
    Vec2d zero(0., 0.);
    for (const Polygons &polygons : line_contours){
        if (polygons.empty()) {
            result.emplace_back();
            continue;
        }
        // Improve: use int values and polygons only
        // Slic3r::Polygons polygons = union_(polygons);
        // std::vector<Slic3r::Line> lines = to_lines(polygons);
        // AABBTreeIndirect::Tree<2, Point> tree;
        // size_t line_idx;
        // Point hit_point;
        // Point::Scalar distance = AABBTreeLines::squared_distance_to_indexed_lines(lines, tree, point, line_idx, hit_point);

        ExPolygons expolygons = union_ex(polygons);
        std::vector<Linef> linesf = to_linesf(expolygons);
        AABBTreeIndirect::Tree2d tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(linesf);

        size_t line_idx = 0;
        Vec2d  hit_point;
        // double distance = 
        AABBTreeLines::squared_distance_to_indexed_lines(linesf, tree, zero, line_idx, hit_point);

        // conversion between index of point and expolygon
        ExPolygonsIndices cvt(expolygons);
        ExPolygonsIndex index = cvt.cvt(static_cast<uint32_t>(line_idx));

        const Slic3r::Polygon& polygon = index.is_contour() ?
            expolygons[index.expolygons_index].contour :
            expolygons[index.expolygons_index].holes[index.hole_index()];

        Point hit_point_int = hit_point.cast<Point::coord_type>();
        TextLine tl{polygon, PolygonPoint{index.point_index, hit_point_int}};
        result.emplace_back(tl);
    }
    return result;
}

inline Eigen::AngleAxis<double> get_rotation() { return Eigen::AngleAxis(-M_PI_2, Vec3d::UnitX()); }

indexed_triangle_set create_its(const TextLines &lines, float radius) 
{
    indexed_triangle_set its;
    // create model from polygons
    for (const TextLine &line : lines) {
        const Slic3r::Polygon &polygon = line.polygon;
        if (polygon.empty()) continue;
        indexed_triangle_set line_its = its_create_torus(polygon, radius);
        auto transl = Eigen::Translation3d(0., line.y, 0.);
        Transform3d tr = transl * get_rotation();
        its_transform(line_its, tr);
        its_merge(its, line_its);
    }
    return its;
}

GLModel::Geometry create_geometry(const TextLines &lines, float radius, bool is_mirrored)
{
    indexed_triangle_set its = create_its(lines, radius);

    GLModel::Geometry geometry;
    geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GUI::GLModel::Geometry::EVertexLayout::P3};
    ColorRGBA color(.7f, .7f, .7f, .7f); // Transparent Gray
    geometry.color = color;

    geometry.reserve_vertices(its.vertices.size());
    for (Vec3f vertex : its.vertices)
        geometry.add_vertex(vertex);

    geometry.reserve_indices(its.indices.size() * 3);

    if (is_mirrored) {
        // change order of indices
        for (Vec3i32 t : its.indices)
            geometry.add_triangle(t[0], t[2], t[1]);
    } else {
        for (Vec3i32 t : its.indices)
            geometry.add_triangle(t[0], t[1], t[2]);
    }
    return geometry;    
}
} // namespace

void TextLinesModel::init(const Transform3d      &text_tr,
                          const ModelVolumePtrs  &volumes_to_slice,
                          /*const*/ Emboss::StyleManager &style_manager,
                          unsigned                count_lines)
{
    assert(style_manager.is_active_font());
    if (!style_manager.is_active_font())
        return;
    const auto &ffc = style_manager.get_font_file_with_cache();
    assert(ffc.has_value());
    if (!ffc.has_value())
        return;
    const auto &ff_ptr = ffc.font_file;
    assert(ff_ptr != nullptr);
    if (ff_ptr == nullptr)
        return;
    const FontFile &ff = *ff_ptr;
    const FontProp &fp = style_manager.get_font_prop();

    FontProp::VerticalAlign align = fp.align.second;

    double line_height_mm = calc_line_height_in_mm(ff, fp);
    assert(line_height_mm > 0);
    if (line_height_mm <= 0)
        return;

    m_model.reset();
    m_lines.clear();

    // size_in_mm .. contain volume scale and should be ascent value in mm 
    double line_offset = fp.size_in_mm * ascent_ratio_offset;
    double first_line_center = line_offset + get_align_y_offset_in_mm(align, count_lines, ff, fp);    
    std::vector<float> line_centers(count_lines);
    for (size_t i = 0; i < count_lines; ++i)
        line_centers[i] = static_cast<float>(first_line_center - i * line_height_mm);

    // contour transformation
    Transform3d c_trafo = text_tr * get_rotation();
    Transform3d c_trafo_inv = c_trafo.inverse();

    std::vector<Polygons> line_contours(count_lines);
    for (const ModelVolume *volume : volumes_to_slice) {
        MeshSlicingParams slicing_params;
        slicing_params.trafo = c_trafo_inv * volume->get_matrix();
        for (size_t i = 0; i < count_lines; ++i) {
            const Polygons polys = Slic3r::slice_mesh(volume->mesh().its, line_centers[i], slicing_params);
            if (polys.empty())
                continue;
            Polygons &contours = line_contours[i];
            contours.insert(contours.end(), polys.begin(), polys.end());
        }
    }

    // fix for text line out of object
    // When move text close to edge - line center could be out of object
    for (Polygons &contours: line_contours) {
        if (!contours.empty())
            continue;

        // use line center at zero, there should be some contour.
        float line_center = 0.f;
        for (const ModelVolume *volume : volumes_to_slice) {
            MeshSlicingParams slicing_params;
            slicing_params.trafo = c_trafo_inv * volume->get_matrix();
            const Polygons polys = Slic3r::slice_mesh(volume->mesh().its, line_center, slicing_params);
            if (polys.empty())
                continue;
            contours.insert(contours.end(), polys.begin(), polys.end());
        }
    }

    m_lines = select_closest_contour(line_contours);
    assert(m_lines.size() == count_lines);
    assert(line_centers.size() == count_lines);
    for (size_t i = 0; i < count_lines; ++i)
        m_lines[i].y = line_centers[i];

    bool is_mirrored = has_reflection(text_tr);
    float radius = static_cast<float>(line_height_mm / 20.);
    //*
    GLModel::Geometry geometry = create_geometry(m_lines, radius, is_mirrored);
    if (geometry.vertices_count() == 0 || geometry.indices_count() == 0)
        return;
    m_model.init_from(std::move(geometry));
    /*/
    // slower solution
    ColorRGBA color(.7f, .7f, .7f, .7f); // Transparent Gray
    m_model.set_color(color);
    m_model.init_from(create_its(m_lines));
    //*/
}

void TextLinesModel::render(const Transform3d &text_world)
{
    if (!m_model.is_initialized())
        return;

    GUI_App &app = wxGetApp();
    const GLShaderProgram *shader = app.get_shader("flat");
    if (shader == nullptr)
        return;

    const Camera &camera = app.plater()->get_camera();

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * text_world);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    bool is_depth_test = glIsEnabled(GL_DEPTH_TEST);
    if (!is_depth_test)
        glsafe(::glEnable(GL_DEPTH_TEST));

    bool is_blend = glIsEnabled(GL_BLEND);
    if (!is_blend)
        glsafe(::glEnable(GL_BLEND));
    // glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    m_model.render();

    if (!is_depth_test)
        glsafe(::glDisable(GL_DEPTH_TEST));
    if (!is_blend)
        glsafe(::glDisable(GL_BLEND));

    shader->stop_using();
}

double TextLinesModel::calc_line_height_in_mm(const Slic3r::Emboss::FontFile &ff, const FontProp &fp)
{
    int line_height = Slic3r::Emboss::get_line_height(ff, fp); // In shape size
    double scale = Slic3r::Emboss::get_text_shape_scale(fp, ff);
    return line_height * scale;
}
