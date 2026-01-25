#include <GL/glew.h>

#include "3DScene.hpp"
#include "GLShader.hpp"
#include "GUI_App.hpp"
#include "GUI_Colors.hpp"
#include "Plater.hpp"
#include "BitmapCache.hpp"
#include "Camera.hpp"

#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <boost/log/trivial.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <Eigen/Dense>

#ifdef HAS_GLSAFE
void glAssertRecentCallImpl(const char* file_name, unsigned int line, const char* function_name)
{
#if defined(NDEBUG)
    // In release mode, only show OpenGL errors if sufficiently high loglevel.
    if (Slic3r::get_logging_level() < 5)
        return;
#endif // NDEBUG

    GLenum err = glGetError();
    if (err == GL_NO_ERROR)
        return;
    const char* sErr = 0;
    switch (err) {
    case GL_INVALID_ENUM:       sErr = "Invalid Enum";      break;
    case GL_INVALID_VALUE:      sErr = "Invalid Value";     break;
    // be aware that GL_INVALID_OPERATION is generated if glGetError is executed between the execution of glBegin and the corresponding execution of glEnd
    case GL_INVALID_OPERATION:  sErr = "Invalid Operation"; break;
    case GL_STACK_OVERFLOW:     sErr = "Stack Overflow";    break;
    case GL_STACK_UNDERFLOW:    sErr = "Stack Underflow";   break;
    case GL_OUT_OF_MEMORY:      sErr = "Out Of Memory";     break;
    default:                    sErr = "Unknown";           break;
    }
    BOOST_LOG_TRIVIAL(error) << "OpenGL error in " << file_name << ":" << line << ", function " << function_name << "() : " << (int)err << " - " << sErr;
    assert(false);
}
#endif // HAS_GLSAFE

float FullyTransparentMaterialThreshold  = 0.1f;
float FullTransparentModdifiedToFixAlpha = 0.3f;
// Be careful changing this value because it could break thumbnail color due to rounding error!
// The color rendering on BambuLab's "send to printer" screen relies on the assumption that this color can be accurately rendered by OpenGL,
// value like 0.18f could not because in C++ (int)(0.18f * 255) == 45 however in OpenGL it renders this as 46
// which breaks the `SelectMachineDialog::record_edge_pixels_data()` function!
float FULL_BLACK_THRESHOLD = 0.2f;

Slic3r::ColorRGBA adjust_color_for_rendering(const Slic3r::ColorRGBA &colors)
{
    if (colors.a() < FullyTransparentMaterialThreshold) { // completely transparent
        return {1, 1, 1, FullTransparentModdifiedToFixAlpha};
    }
    else if(colors.r() < FULL_BLACK_THRESHOLD && colors.g() < FULL_BLACK_THRESHOLD && colors.b() < FULL_BLACK_THRESHOLD) { // black
        return {FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, FULL_BLACK_THRESHOLD, colors.a()};
    }
    else
        return colors;
}

namespace Slic3r {


const float GLVolume::SinkingContours::HalfWidth = 0.25f;

void GLVolume::SinkingContours::render()
{
    update();

    GLShaderProgram* shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    const GUI::Camera& camera = GUI::wxGetApp().plater()->get_camera();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * Geometry::assemble_transform(m_shift));
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    m_model.render();
}

void GLVolume::SinkingContours::update()
{
    const int object_idx = m_parent.object_idx();
    const Model& model = GUI::wxGetApp().plater()->model();

    if (0 <= object_idx && object_idx < int(model.objects.size()) && m_parent.is_sinking() && !m_parent.is_below_printbed()) {
        const BoundingBoxf3& box = m_parent.transformed_convex_hull_bounding_box();
        if (!m_old_box.size().isApprox(box.size()) || m_old_box.min.z() != box.min.z()) {
            m_old_box = box;
            m_shift = Vec3d::Zero();

            const TriangleMesh& mesh = model.objects[object_idx]->volumes[m_parent.volume_idx()]->mesh();

            m_model.reset();
            GUI::GLModel::Geometry init_data;
            init_data.format = { GUI::GLModel::Geometry::EPrimitiveType::Triangles, GUI::GLModel::Geometry::EVertexLayout::P3 };
            init_data.color = ColorRGBA::WHITE();
            unsigned int vertices_counter = 0;
            MeshSlicingParams slicing_params;
            slicing_params.trafo = m_parent.world_matrix();
            const Polygons polygons = union_(slice_mesh(mesh.its, 0.0f, slicing_params));
            for (const ExPolygon& expoly : diff_ex(expand(polygons, float(scale_(HalfWidth))), shrink(polygons, float(scale_(HalfWidth))))) {
                const std::vector<Vec3d> triangulation = triangulate_expolygon_3d(expoly);
                init_data.reserve_vertices(init_data.vertices_count() + triangulation.size());
                init_data.reserve_indices(init_data.indices_count() + triangulation.size());
                for (const Vec3d& v : triangulation) {
                    init_data.add_vertex((Vec3f)(v.cast<float>() + 0.015f * Vec3f::UnitZ())); // add a small positive z to avoid z-fighting
                    ++vertices_counter;
                    if (vertices_counter % 3 == 0)
                        init_data.add_triangle(vertices_counter - 3, vertices_counter - 2, vertices_counter - 1);
                }
            }
            m_model.init_from(std::move(init_data));
        }
        else
            m_shift = box.center() - m_old_box.center();
    }
    else
        m_model.reset();
}

ColorRGBA GLVolume::DISABLED_COLOR    = ColorRGBA::DARK_GRAY();
ColorRGBA GLVolume::SLA_SUPPORT_COLOR = ColorRGBA::LIGHT_GRAY();
ColorRGBA GLVolume::SLA_PAD_COLOR     = { 0.0f, 0.2f, 0.0f, 1.0f };
// BBS
ColorRGBA GLVolume::NEUTRAL_COLOR     = { 0.8f, 0.8f, 0.8f, 1.0f };
ColorRGBA GLVolume::UNPRINTABLE_COLOR = { 0.0f, 0.0f, 0.0f, 0.5f };

ColorRGBA GLVolume::MODEL_MIDIFIER_COL   = {1.0f, 1.0f, 0.0f, 0.6f};
ColorRGBA GLVolume::MODEL_NEGTIVE_COL    = {0.3f, 0.3f, 0.3f, 0.4f};
ColorRGBA GLVolume::SUPPORT_ENFORCER_COL = {0.3f, 0.3f, 1.0f, 0.4f};
ColorRGBA GLVolume::SUPPORT_BLOCKER_COL  = {1.0f, 0.3f, 0.3f, 0.4f};

ColorRGBA GLVolume::MODEL_HIDDEN_COL  = {0.f, 0.f, 0.f, 0.3f};

std::array<ColorRGBA, 5> GLVolume::MODEL_COLOR = { {
    { 1.0f, 1.0f, 0.0f, 1.f },
    { 1.0f, 0.5f, 0.5f, 1.f },
    { 0.5f, 1.0f, 0.5f, 1.f },
    { 0.5f, 0.5f, 1.0f, 1.f },
    { 1.0f, 1.0f, 0.0f, 1.f }
} };

void GLVolume::update_render_colors()
{
    GLVolume::DISABLED_COLOR    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Model_Disable]);
    GLVolume::NEUTRAL_COLOR     = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Model_Neutral]);
    GLVolume::MODEL_COLOR[0]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Modifier]);
    GLVolume::MODEL_COLOR[1]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Negtive_Volume]);
    GLVolume::MODEL_COLOR[2]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Support_Enforcer]);
    GLVolume::MODEL_COLOR[3]    = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Support_Blocker]);
    GLVolume::UNPRINTABLE_COLOR = GUI::ImGuiWrapper::from_ImVec4(RenderColor::colors[RenderCol_Model_Unprintable]);

}

void GLVolume::load_render_colors()
{
    RenderColor::colors[RenderCol_Model_Disable]    = GUI::ImGuiWrapper::to_ImVec4(GLVolume::DISABLED_COLOR);
    RenderColor::colors[RenderCol_Model_Neutral]    = GUI::ImGuiWrapper::to_ImVec4(GLVolume::NEUTRAL_COLOR);
    RenderColor::colors[RenderCol_Modifier]         = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[0]);
    RenderColor::colors[RenderCol_Negtive_Volume]   = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[1]);
    RenderColor::colors[RenderCol_Support_Enforcer] = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[2]);
    RenderColor::colors[RenderCol_Support_Blocker]   = GUI::ImGuiWrapper::to_ImVec4(GLVolume::MODEL_COLOR[3]);
    RenderColor::colors[RenderCol_Model_Unprintable] = GUI::ImGuiWrapper::to_ImVec4(GLVolume::UNPRINTABLE_COLOR);
}

GLVolume::GLVolume(float r, float g, float b, float a)
    : m_sla_shift_z(0.0)
    , m_sinking_contours(*this)
    // geometry_id == 0 -> invalid
    , geometry_id(std::pair<size_t, size_t>(0, 0))
    , extruder_id(0)
    , selected(false)
    , disabled(false)
    , printable(true)
    , visible(true)
    , is_active(true)
    , zoom_to_volumes(true)
    , shader_outside_printer_detection_enabled(false)
    , is_outside(false)
    , partly_inside(false)
    , hover(HS_None)
    , is_modifier(false)
    , slice_error(false)
    , is_wipe_tower(false)
    , is_extrusion_path(false)
    , force_transparent(false)
    , force_native_color(false)
    , force_neutral_color(false)
    , force_sinking_contours(false)
    , picking(false)
    , tverts_range(0, size_t(-1))
{
    color = { r, g, b, a };
    set_render_color(color);
    mmuseg_ts = 0;
}


// BBS
float GLVolume::explosion_ratio = 1.0;
float GLVolume::last_explosion_ratio = 1.0;

void GLVolume::set_render_color()
{
    bool outside = is_outside || is_below_printbed();

    if (force_native_color || force_neutral_color) {
#ifdef ENABBLE_OUTSIDE_COLOR
        if (outside && shader_outside_printer_detection_enabled)
            set_render_color(OUTSIDE_COLOR);
        else {
#endif
            if (force_native_color)
                set_render_color(color);
            else
                set_render_color(NEUTRAL_COLOR);
#ifdef ENABLE_OUTSIDE_COLOR
        }
#endif
    }
    else {
        /* BBS
        if (hover == HS_Select)
            set_render_color(HOVER_SELECT_COLOR);
        else if (hover == HS_Deselect)
            set_render_color(HOVER_DESELECT_COLOR);
        else if (selected)
            set_render_color(outside ? SELECTED_OUTSIDE_COLOR : SELECTED_COLOR);
        else if (disabled)
        */
        if (disabled)
            set_render_color(DISABLED_COLOR);
#ifdef ENABLE_OUTSIDE_COLOR
        else if (is_outside && shader_outside_printer_detection_enabled)
            set_render_color(OUTSIDE_COLOR);
#endif
        else {
            //to make black not too hard too see
            ColorRGBA new_color = adjust_color_for_rendering(color);
            set_render_color(new_color);
        }
    }

    if (force_transparent) {
        if (color.a() < FullyTransparentMaterialThreshold) {
            render_color.a(FullTransparentModdifiedToFixAlpha);
        } else {
            render_color.a(color.a());
        }
    }

    //BBS set unprintable color
    if (!printable) {
        render_color = UNPRINTABLE_COLOR;
    }

    //BBS set invisible color
    if (!visible) {
        render_color = MODEL_HIDDEN_COL;
    }
}

ColorRGBA color_from_model_volume(const ModelVolume& model_volume)
{
    ColorRGBA color;
    if (model_volume.is_negative_volume())
        return GLVolume::MODEL_NEGTIVE_COL;
    else if (model_volume.is_modifier())
#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        return GLVolume::MODEL_MIDIFIER_COL;
#else
		color = { 0.2f, 1.0f, 0.2f, 1.0f };
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
    else if (model_volume.is_support_blocker())
        return GLVolume::SUPPORT_BLOCKER_COL;
    else if (model_volume.is_support_enforcer())
        return GLVolume::SUPPORT_ENFORCER_COL;
    return color;
}

Transform3d GLVolume::world_matrix() const
{
    Transform3d m = m_instance_transformation.get_matrix() * m_volume_transformation.get_matrix();
    Vec3d ofs2ass = m_offset_to_assembly * (GLVolume::explosion_ratio - 1.0);
    Vec3d volofs2obj = m_volume_transformation.get_offset() * (GLVolume::explosion_ratio - 1.0);

    m.translation()(2) += m_sla_shift_z;
    m.translate(ofs2ass + volofs2obj);
    return m;
}

bool GLVolume::is_left_handed() const
{
    const Vec3d &m1 = m_instance_transformation.get_mirror();
    const Vec3d &m2 = m_volume_transformation.get_mirror();
    return m1.x() * m1.y() * m1.z() * m2.x() * m2.y() * m2.z() < 0.;
}

const BoundingBoxf3& GLVolume::transformed_bounding_box() const
{
    if (!m_transformed_bounding_box.has_value() || last_explosion_ratio != explosion_ratio) {
        const BoundingBoxf3& box = bounding_box();
        assert(box.defined || box.min.x() >= box.max.x() || box.min.y() >= box.max.y() || box.min.z() >= box.max.z());
        std::optional<BoundingBoxf3>* trans_box = const_cast<std::optional<BoundingBoxf3>*>(&m_transformed_bounding_box);
        *trans_box = box.transformed(world_matrix());
        last_explosion_ratio = explosion_ratio;
    }
    return *m_transformed_bounding_box;
}

const BoundingBoxf3& GLVolume::transformed_convex_hull_bounding_box() const
{
    if (!m_transformed_convex_hull_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* trans_box = const_cast<std::optional<BoundingBoxf3>*>(&m_transformed_convex_hull_bounding_box);
        *trans_box = transformed_convex_hull_bounding_box(world_matrix());
    }
    return *m_transformed_convex_hull_bounding_box;
}

BoundingBoxf3 GLVolume::transformed_convex_hull_bounding_box(const Transform3d &trafo) const
{
	return (m_convex_hull && ! m_convex_hull->empty()) ?
		m_convex_hull->transformed_bounding_box(trafo) :
        bounding_box().transformed(trafo);
}

BoundingBoxf3 GLVolume::transformed_non_sinking_bounding_box(const Transform3d& trafo) const
{
    return GUI::wxGetApp().plater()->model().objects[object_idx()]->volumes[volume_idx()]->mesh().transformed_bounding_box(trafo, 0.0);
}

const BoundingBoxf3& GLVolume::transformed_non_sinking_bounding_box() const
{
    if (!m_transformed_non_sinking_bounding_box.has_value()) {
        std::optional<BoundingBoxf3>* trans_box = const_cast<std::optional<BoundingBoxf3>*>(&m_transformed_non_sinking_bounding_box);
        const Transform3d& trafo = world_matrix();
        *trans_box = transformed_non_sinking_bounding_box(trafo);
    }
    return *m_transformed_non_sinking_bounding_box;
}

void GLVolume::set_range(double min_z, double max_z)
{
    this->tverts_range.first = 0;
    this->tverts_range.second = this->model.indices_count();

    if (!this->print_zs.empty()) {
        // The Z layer range is specified.
        // First test whether the Z span of this object is not out of (min_z, max_z) completely.
        if (this->print_zs.front() > max_z || this->print_zs.back() < min_z)
            this->tverts_range.second = 0;
        else {
            // Then find the lowest layer to be displayed.
            size_t i = 0;
            for (; i < this->print_zs.size() && this->print_zs[i] < min_z; ++i);
            if (i == this->print_zs.size())
                // This shall not happen.
                this->tverts_range.second = 0;
            else {
                // Remember start of the layer.
                this->tverts_range.first = this->offsets[i];
                // Some layers are above $min_z. Which?
                for (; i < this->print_zs.size() && this->print_zs[i] <= max_z; ++i);
                if (i < this->print_zs.size())
                    this->tverts_range.second = this->offsets[i];
            }
        }
    }
}

void GLVolume::render()
{
    if (!is_active)
        return;

    GLShaderProgram *shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    ModelObjectPtrs &model_objects = GUI::wxGetApp().model().objects;
    std::vector<ColorRGBA> colors = GUI::wxGetApp().plater()->get_extruders_colors();

    simple_render(shader, model_objects, colors);
}

//BBS: add outline related logic
void GLVolume::render_with_outline(const GUI::Size& cnv_size)
{
    if (!is_active)
        return;

    GLShaderProgram *shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    ModelObjectPtrs &model_objects = GUI::wxGetApp().model().objects;
    std::vector<ColorRGBA> colors = GUI::wxGetApp().plater()->get_extruders_colors();

    const GUI::OpenGLManager::EFramebufferType framebuffers_type = GUI::OpenGLManager::get_framebuffers_type();
    if (framebuffers_type == GUI::OpenGLManager::EFramebufferType::Unknown) {
        // No supported, degrade to normal rendering
        simple_render(shader, model_objects, colors);
        return;
    }

    // 1st. render pass, render the model into a separate render target that has only depth buffer
    GLuint depth_fbo   = 0;
    GLuint depth_tex = 0;
    if (framebuffers_type == GUI::OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glGenFramebuffers(1, &depth_fbo));
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, depth_fbo));

        glActiveTexture(GL_TEXTURE0);
        glsafe(::glGenTextures(1, &depth_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, depth_tex));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, cnv_size.get_width(), cnv_size.get_height(), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr));

        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_tex, 0));
    } else {
        glsafe(::glGenFramebuffersEXT(1, &depth_fbo));
        glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, depth_fbo));

        glActiveTexture(GL_TEXTURE0);
        glsafe(::glGenTextures(1, &depth_tex));
        glsafe(::glBindTexture(GL_TEXTURE_2D, depth_tex));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
        glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
        glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, cnv_size.get_width(), cnv_size.get_height(), 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr));

        glsafe(::glFramebufferTexture2D(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_TEXTURE_2D, depth_tex, 0));
    }
    glsafe(::glClear(GL_DEPTH_BUFFER_BIT));
    if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
        model.render();
    else
        model.render(this->tverts_range);
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

    // 2nd. render pass, just a normal render with the depth buffer passed as a texture
    if (framebuffers_type == GUI::OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
    } else if (framebuffers_type == GUI::OpenGLManager::EFramebufferType::Ext) {
        glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0));
    }
    shader->set_uniform("is_outline", true);
    shader->set_uniform("screen_size", Vec2f{cnv_size.get_width(), cnv_size.get_height()});
    glActiveTexture(GL_TEXTURE0);
    glsafe(::glBindTexture(GL_TEXTURE_2D, depth_tex));
    shader->set_uniform("depth_tex", 0);
    simple_render(shader, model_objects, colors);

    // Some clean up to do
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    shader->set_uniform("is_outline", false);
    if (framebuffers_type == GUI::OpenGLManager::EFramebufferType::Arb) {
        glsafe(::glBindFramebuffer(GL_FRAMEBUFFER, 0));
        if (depth_fbo != 0)
            glsafe(::glDeleteFramebuffers(1, &depth_fbo));
    } else if (framebuffers_type == GUI::OpenGLManager::EFramebufferType::Ext) {
        glsafe(::glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0));
        if (depth_fbo != 0)
            glsafe(::glDeleteFramebuffersEXT(1, &depth_fbo));
    }
    if (depth_tex != 0)
        glsafe(::glDeleteTextures(1, &depth_tex));
}

//BBS add render for simple case
void GLVolume::simple_render(GLShaderProgram* shader, ModelObjectPtrs& model_objects, std::vector<ColorRGBA>& extruder_colors, bool ban_light)
{
    if (this->is_left_handed())
        glFrontFace(GL_CW);
    glsafe(::glCullFace(GL_BACK));

    bool color_volume = false;
    ModelObject* model_object = nullptr;
    ModelVolume* model_volume = nullptr;
    do {
        if ((!printable) || object_idx() >= model_objects.size())
            break;
        model_object = model_objects[object_idx()];

        if (volume_idx() >=  model_object->volumes.size())
            break;
        model_volume = model_object->volumes[volume_idx()];
        if (model_volume->mmu_segmentation_facets.empty())
            break;

        color_volume = true;
        if (model_volume->mmu_segmentation_facets.timestamp() != mmuseg_ts) {
            mmuseg_models.clear();
            std::vector<indexed_triangle_set> its_per_color;
            model_volume->mmu_segmentation_facets.get_facets(*model_volume, its_per_color);
            mmuseg_models.resize(its_per_color.size());
            for (int idx = 0; idx < its_per_color.size(); idx++) {
                mmuseg_models[idx].init_from(its_per_color[idx]);
            }

            mmuseg_ts = model_volume->mmu_segmentation_facets.timestamp();
        }
    } while (0);

    if (color_volume && !picking) {
        // when force_transparent, we need to keep the alpha
        if (force_native_color && render_color.is_transparent()) {
            for (auto &extruder_color : extruder_colors)
                extruder_color.a(render_color.a());
        }

        for (int idx = 0; idx < mmuseg_models.size(); idx++) {
            GUI::GLModel &m = mmuseg_models[idx];
            if (!m.is_initialized())
                continue;

            if (shader) {
                if (idx == 0) {
                    int extruder_id = model_volume->extruder_id();
                    //to make black not too hard too see
                    ColorRGBA new_color = adjust_color_for_rendering(extruder_colors[extruder_id - 1]);
                    if (ban_light) {
                        new_color[3] = (255 - (extruder_id - 1))/255.0f;
                    }
                    m.set_color(new_color);
                    // shader->set_uniform("uniform_color", new_color);
                }
                else {
                    if (idx <= extruder_colors.size()) {
                        //to make black not too hard too see
                        ColorRGBA new_color = adjust_color_for_rendering(extruder_colors[idx - 1]);
                        if (ban_light) {
                            new_color[3] = (255 - (idx - 1))/255.0f;
                        }
                        m.set_color(new_color);
                        // shader->set_uniform("uniform_color", new_color);
                    }
                    else {
                        //to make black not too hard too see
                        ColorRGBA new_color = adjust_color_for_rendering(extruder_colors[0]);
                        if (ban_light) {
                            new_color[3] = (255 - 0) / 255.0f;
                        }
                        m.set_color(new_color);
                        // shader->set_uniform("uniform_color", new_color);
                    }
                }
            }
            if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
                m.render();
            else
                m.render(this->tverts_range);
        }
    } else {
        if (tverts_range == std::make_pair<size_t, size_t>(0, -1))
            model.render();
        else
            model.render(this->tverts_range);
    }
    if (this->is_left_handed())
        glFrontFace(GL_CCW);
}

bool GLVolume::is_sla_support() const { return this->composite_id.volume_id == -int(slaposSupportTree); }
bool GLVolume::is_sla_pad() const { return this->composite_id.volume_id == -int(slaposPad); }

bool GLVolume::is_sinking() const
{
    if (is_modifier || GUI::wxGetApp().preset_bundle->printers.get_edited_preset().printer_technology() == ptSLA)
        return false;
    const BoundingBoxf3& box = transformed_convex_hull_bounding_box();
    return box.min.z() < SINKING_Z_THRESHOLD && box.max.z() >= SINKING_Z_THRESHOLD;
}

bool GLVolume::is_below_printbed() const
{
    return transformed_convex_hull_bounding_box().max.z() < 0.0;
}

void GLVolume::render_sinking_contours()
{
    m_sinking_contours.render();
}

GLWipeTowerVolume::GLWipeTowerVolume(const std::vector<ColorRGBA>& colors)
    : GLVolume()
{
    m_colors = colors;
}

void GLWipeTowerVolume::render()
{
    if (!is_active)
        return;

    if (m_colors.size() == 0 || m_colors.size() != model_per_colors.size())
        return;

    if (this->is_left_handed())
        glFrontFace(GL_CW);
    glsafe(::glCullFace(GL_BACK));

    for (int i = 0; i < m_colors.size(); i++) {
        if (!picking) {
            ColorRGBA new_color = adjust_color_for_rendering(m_colors[i]);
            this->model_per_colors[i].set_color(new_color);
        } else {
            this->model_per_colors[i].set_color(model.get_color());
        }
        this->model_per_colors[i].render();
    }
    
    if (this->is_left_handed())
        glFrontFace(GL_CCW);
}

bool GLWipeTowerVolume::IsTransparent() { 
    for (size_t i = 0; i < m_colors.size(); i++) {
        if (m_colors[i].is_transparent()) { 
            return true;
        }
    }
    return false; 
}

std::vector<int> GLVolumeCollection::load_object(
    const ModelObject       *model_object,
    int                      obj_idx,
    const std::vector<int>  &instance_idxs,
    const std::string       &color_by,
    bool 					 opengl_initialized,
    bool                    need_raycaster)
{
    std::vector<int> volumes_idx;
    for (int volume_idx = 0; volume_idx < int(model_object->volumes.size()); ++volume_idx)
        for (int instance_idx : instance_idxs)
            volumes_idx.emplace_back(this->GLVolumeCollection::load_object_volume(model_object, obj_idx, volume_idx, instance_idx, color_by, opengl_initialized, false, false, need_raycaster));
    return volumes_idx;
}


int GLVolumeCollection::load_object_volume(
    const ModelObject   *model_object,
    int                  obj_idx,
    int                  volume_idx,
    int                  instance_idx,
    const std::string   &color_by,
    bool 				 opengl_initialized,
    bool                 in_assemble_view,
    bool                 use_loaded_id,
    bool                 need_raycaster)
{
    const ModelVolume   *model_volume = model_object->volumes[volume_idx];
    const int            extruder_id  = model_volume->extruder_id();
    const ModelInstance *instance 	  = model_object->instances[instance_idx];
    auto color = GLVolume::MODEL_COLOR[((color_by == "volume") ? volume_idx : obj_idx) % 4];
    color.a(model_volume->is_model_part() ? 0.7f : 0.4f);

    std::shared_ptr<const TriangleMesh> mesh = model_volume->mesh_ptr();
    this->volumes.emplace_back(new GLVolume(color));
    GLVolume& v = *this->volumes.back();
    v.set_color(color_from_model_volume(*model_volume));
    v.name = model_volume->name;
	
#if ENABLE_SMOOTH_NORMALS
    v.model.init_from(mesh, true);
#else
    v.model.init_from(*mesh);
    if (need_raycaster) { v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(mesh); }
#endif // ENABLE_SMOOTH_NORMALS
    v.composite_id = GLVolume::CompositeID(obj_idx, volume_idx, instance_idx);

    if (model_volume->is_model_part())
    {
        // GLVolume will reference a convex hull from model_volume!
        v.set_convex_hull(model_volume->get_convex_hull_shared_ptr());
        if (extruder_id != -1)
            v.extruder_id = extruder_id;
    }
    v.is_modifier = !model_volume->is_model_part();
    v.shader_outside_printer_detection_enabled = model_volume->is_model_part();
    if (in_assemble_view) {
        v.set_instance_transformation(instance->get_assemble_transformation());
        v.set_offset_to_assembly(instance->get_offset_to_assembly());
    }
    else
        v.set_instance_transformation(instance->get_transformation());
    v.set_volume_transformation(model_volume->get_transformation());
    //use object's instance id
    if (use_loaded_id && (instance->loaded_id > 0))
        v.model_object_ID = instance->loaded_id;
    else
        v.model_object_ID = instance->id().id;

    return int(this->volumes.size() - 1);
}

// Load SLA auxiliary GLVolumes (for support trees or pad).
// This function produces volumes for multiple instances in a single shot,
// as some object specific mesh conversions may be expensive.
void GLVolumeCollection::load_object_auxiliary(
    const SLAPrintObject* print_object,
    int                             obj_idx,
    // pairs of <instance_idx, print_instance_idx>
    const std::vector<std::pair<size_t, size_t>>& instances,
    SLAPrintObjectStep              milestone,
    // Timestamp of the last change of the milestone
    size_t                          timestamp)
{
    assert(print_object->is_step_done(milestone));
    Transform3d  mesh_trafo_inv = print_object->trafo().inverse();
    // Get the support mesh.
    TriangleMesh mesh = print_object->get_mesh(milestone);
    mesh.transform(mesh_trafo_inv);
    // Convex hull is required for out of print bed detection.
    TriangleMesh convex_hull = mesh.convex_hull_3d();
    for (const std::pair<size_t, size_t>& instance_idx : instances) {
        const ModelInstance& model_instance = *print_object->model_object()->instances[instance_idx.first];
        this->volumes.emplace_back(new GLVolume((milestone == slaposPad) ? GLVolume::SLA_PAD_COLOR : GLVolume::SLA_SUPPORT_COLOR));
        GLVolume& v = *this->volumes.back();
#if ENABLE_SMOOTH_NORMALS
        v.model.init_from(mesh, true);
#else
        v.model.init_from(mesh);
        v.model.set_color((milestone == slaposPad) ? GLVolume::SLA_PAD_COLOR : GLVolume::SLA_SUPPORT_COLOR);
        v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<const TriangleMesh>(mesh));
#endif // ENABLE_SMOOTH_NORMALS
        v.composite_id = GLVolume::CompositeID(obj_idx, -int(milestone), (int)instance_idx.first);
        v.geometry_id = std::pair<size_t, size_t>(timestamp, model_instance.id().id);
        // Create a copy of the convex hull mesh for each instance. Use a move operator on the last instance.
        if (&instance_idx == &instances.back())
            v.set_convex_hull(std::move(convex_hull));
        else
            v.set_convex_hull(convex_hull);
        v.is_modifier = false;
        v.shader_outside_printer_detection_enabled = (milestone == slaposSupportTree);
        v.set_instance_transformation(model_instance.get_transformation());
        // Leave the volume transformation at identity.
        // v.set_volume_transformation(model_volume->get_transformation());
    }
}

int GLVolumeCollection::load_wipe_tower_preview(
    int obj_idx, float pos_x, float pos_y, float width, float depth, float height,
    float rotation_angle, bool size_unknown, float brim_width)
{
    int plate_idx = obj_idx - 1000;

    if (depth < 0.01f)
        return int(this->volumes.size() - 1);
    if (height == 0.0f)
        height = 0.1f;

    std::vector<ColorRGBA> extruder_colors = GUI::wxGetApp().plater()->get_extruders_colors();
    std::vector<ColorRGBA> colors;
    GUI::PartPlateList& ppl = GUI::wxGetApp().plater()->get_partplate_list();
    std::vector<int> plate_extruders = ppl.get_plate(plate_idx)->get_extruders(true);
    TriangleMesh wipe_tower_shell = make_cube(width, depth, height);
    for (int extruder_id : plate_extruders) {
        if (extruder_id <= extruder_colors.size())
            colors.push_back(extruder_colors[extruder_id - 1]);
        else
            colors.push_back(extruder_colors[0]);
    }

    // Orca: make it transparent
    for(auto& color : colors)
        color.a(0.66f);
    volumes.emplace_back(new GLWipeTowerVolume(colors));
    GLWipeTowerVolume& v = *dynamic_cast<GLWipeTowerVolume*>(volumes.back());
    v.model_per_colors.resize(colors.size());
    for (int i = 0; i < colors.size(); i++) {
        TriangleMesh color_part = make_cube(width, depth / colors.size(), height);
        color_part.translate({ 0.f, depth * i / colors.size(), 0. });
        v.model_per_colors[i].init_from(color_part);
    }
    v.model.init_from(wipe_tower_shell);
    v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<const TriangleMesh>(wipe_tower_shell));
    v.set_convex_hull(wipe_tower_shell);
    v.set_volume_offset(Vec3d(pos_x, pos_y, 0.0));
    v.set_volume_rotation(Vec3d(0., 0., (M_PI / 180.) * rotation_angle));
    v.composite_id = GLVolume::CompositeID(obj_idx, 0, 0);
    v.geometry_id.first = 0;
    v.geometry_id.second = wipe_tower_instance_id().id + (obj_idx - 1000);
    v.is_wipe_tower = true;
    v.shader_outside_printer_detection_enabled = !size_unknown;
    return int(volumes.size() - 1);
}

int GLVolumeCollection::load_real_wipe_tower_preview(
    int obj_idx, float pos_x, float pos_y, const TriangleMesh& wt_mesh,const TriangleMesh &brim_mesh,bool render_brim, float rotation_angle, bool size_unknown,  bool opengl_initialized)
{
    int plate_idx = obj_idx - 1000;
    if (wt_mesh.its.vertices.empty()) return int(this->volumes.size() - 1);

    std::vector<Slic3r::ColorRGBA> extruder_colors = GUI::wxGetApp().plater()->get_extruders_colors();
    GUI::PartPlateList               &ppl              = GUI::wxGetApp().plater()->get_partplate_list();
    std::vector<int>                  plate_extruders  = ppl.get_plate(plate_idx)->get_extruders(true);
    std::vector<Slic3r::ColorRGBA>    colors;
    if (!plate_extruders.empty()) {
        if (plate_extruders.front() <= extruder_colors.size())
            colors.push_back(extruder_colors[plate_extruders.front() - 1]);
        else
            colors.push_back(extruder_colors[0]);
    }
    if (colors.empty()) return int(this->volumes.size() - 1);
    volumes.emplace_back(new GLWipeTowerVolume({colors}));
    GLWipeTowerVolume &v = *dynamic_cast<GLWipeTowerVolume *>(volumes.back());
    auto mesh = wt_mesh;
    if (render_brim) {
        mesh.merge(brim_mesh);
    }
    if (!colors.empty()) {
        v.model_per_colors.resize(1);
        v.model_per_colors[0].init_from(mesh);
    }
    TriangleMesh wipe_tower_shell = mesh.convex_hull_3d();
    v.model.init_from(wipe_tower_shell);
    v.mesh_raycaster = std::make_unique<GUI::MeshRaycaster>(std::make_shared<const TriangleMesh>(wipe_tower_shell));
    v.set_convex_hull(wipe_tower_shell);
    v.set_volume_offset(Vec3d(pos_x, pos_y, 0.0));
    v.set_volume_rotation(Vec3d(0., 0., (M_PI / 180.) * rotation_angle));
    v.composite_id                             = GLVolume::CompositeID(obj_idx, 0, 0);
    v.geometry_id.first                        = 0;
    v.geometry_id.second                       = wipe_tower_instance_id().id + (obj_idx - 1000);
    v.is_wipe_tower                            = true;
    v.shader_outside_printer_detection_enabled = !size_unknown;
    return int(volumes.size() - 1);
}


GLVolume* GLVolumeCollection::new_toolpath_volume(const ColorRGBA& rgba)
{
    GLVolume* out = new_nontoolpath_volume(rgba);
    out->is_extrusion_path = true;
    return out;
}

GLVolume* GLVolumeCollection::new_nontoolpath_volume(const ColorRGBA& rgba)
{
    GLVolume* out = new GLVolume(rgba);
    out->is_extrusion_path = false;
    this->volumes.emplace_back(out);
    return out;
}

GLVolumeWithIdAndZList volumes_to_render(const GLVolumePtrs& volumes, GLVolumeCollection::ERenderType type, const Transform3d& view_matrix, std::function<bool(const GLVolume&)> filter_func)
{
    GLVolumeWithIdAndZList list;
    list.reserve(volumes.size());

    for (unsigned int i = 0; i < (unsigned int)volumes.size(); ++i) {
        GLVolume* volume = volumes[i];
        bool is_transparent = volume->render_color.is_transparent();
        auto tempGlwipeTowerVolume = dynamic_cast<GLWipeTowerVolume *>(volume);
        if (tempGlwipeTowerVolume) { 
            is_transparent = tempGlwipeTowerVolume->IsTransparent();
        }
        if (((type == GLVolumeCollection::ERenderType::Opaque && !is_transparent) || 
            (type == GLVolumeCollection::ERenderType::Transparent && is_transparent) ||
             type == GLVolumeCollection::ERenderType::All) &&
            (! filter_func || filter_func(*volume)))
            list.emplace_back(std::make_pair(volume, std::make_pair(i, 0.0)));
    }

    if (type == GLVolumeCollection::ERenderType::Transparent && list.size() > 1) {
        for (GLVolumeWithIdAndZ& volume : list) {
            volume.second.second = volume.first->bounding_box().transformed(view_matrix * volume.first->world_matrix()).max(2);
        }

        std::sort(list.begin(), list.end(),
            [](const GLVolumeWithIdAndZ& v1, const GLVolumeWithIdAndZ& v2) -> bool { return v1.second.second < v2.second.second; }
        );
    }
    else if (type == GLVolumeCollection::ERenderType::Opaque && list.size() > 1) {
        std::sort(list.begin(), list.end(),
            [](const GLVolumeWithIdAndZ& v1, const GLVolumeWithIdAndZ& v2) -> bool { return v1.first->selected && !v2.first->selected; }
        );
    }

    return list;
}

int GLVolumeCollection::get_selection_support_threshold_angle(bool &enable_support) const
{
    const DynamicPrintConfig& glb_cfg        = GUI::wxGetApp().preset_bundle->prints.get_edited_preset().config;
    enable_support =  glb_cfg.opt_bool("enable_support");
    int support_threshold_angle =  glb_cfg.opt_int("support_threshold_angle");
    return  support_threshold_angle ;
}

//BBS: add outline drawing logic
void GLVolumeCollection::render(GLVolumeCollection::ERenderType       type,
                                bool                                  disable_cullface,
                                const Transform3d &                   view_matrix,
                                const Transform3d&                    projection_matrix,
                                const GUI::Size&                      cnv_size,
                                std::function<bool(const GLVolume &)> filter_func,
                                bool                                  partly_inside_enable) const
{
    GLVolumeWithIdAndZList to_render = volumes_to_render(volumes, type, view_matrix, filter_func);
    if (to_render.empty())
        return;

    GLShaderProgram* shader = GUI::wxGetApp().get_current_shader();
    if (shader == nullptr)
        return;

    GLShaderProgram* sink_shader = GUI::wxGetApp().get_shader("flat");
#if SLIC3R_OPENGL_ES
    GLShaderProgram* edges_shader = GUI::wxGetApp().get_shader("dashed_lines");
#else
    GLShaderProgram* edges_shader = GUI::OpenGLManager::get_gl_info().is_core_profile() ? GUI::wxGetApp().get_shader("dashed_thick_lines") : GUI::wxGetApp().get_shader("flat");
#endif // SLIC3R_OPENGL_ES

    if (type == ERenderType::Transparent) {
        glsafe(::glEnable(GL_BLEND));
        glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    }

    glsafe(::glCullFace(GL_BACK));
    if (disable_cullface)
        glsafe(::glDisable(GL_CULL_FACE));

    for (GLVolumeWithIdAndZ& volume : to_render) {
#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        if (type == ERenderType::Transparent) {
            volume.first->force_transparent = true;
            //BOOST_LOG_TRIVIAL(info) << boost::format("transparent rendering...");
        }
        //else
        //    BOOST_LOG_TRIVIAL(info) << boost::format("opaque rendering...");
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        volume.first->set_render_color();
#if ENABLE_MODIFIERS_ALWAYS_TRANSPARENT
        if (type == ERenderType::Transparent)
            volume.first->force_transparent = false;
#endif // ENABLE_MODIFIERS_ALWAYS_TRANSPARENT

        // render sinking contours of non-hovered volumes
        shader->stop_using();
        if (sink_shader != nullptr) {
            sink_shader->start_using();
            if (m_show_sinking_contours) {
                if (volume.first->is_sinking() && !volume.first->is_below_printbed() &&
                    volume.first->hover == GLVolume::HS_None && !volume.first->force_sinking_contours) {
                    volume.first->render_sinking_contours();
                }
            }
            sink_shader->stop_using();
        }
        shader->start_using();

        if (!volume.first->model.is_initialized())
            shader->set_uniform("uniform_color", volume.first->render_color);
        shader->set_uniform("z_range", m_z_range);
        shader->set_uniform("clipping_plane", m_clipping_plane);
        shader->set_uniform("use_color_clip_plane", m_use_color_clip_plane);
        shader->set_uniform("color_clip_plane", m_color_clip_plane);
        shader->set_uniform("uniform_color_clip_plane_1", m_color_clip_plane_colors[0]);
        shader->set_uniform("uniform_color_clip_plane_2", m_color_clip_plane_colors[1]);
        //BOOST_LOG_TRIVIAL(info) << boost::format("set uniform_color to {%1%, %2%, %3%, %4%}, with_outline=%5%, selected %6%")
        //    %volume.first->render_color[0]%volume.first->render_color[1]%volume.first->render_color[2]%volume.first->render_color[3]
        //    %with_outline%volume.first->selected;

        //BBS set print_volume to render volume
        //shader->set_uniform("print_volume.type", static_cast<int>(m_render_volume.type));
        //shader->set_uniform("print_volume.xy_data", m_render_volume.data);
        //shader->set_uniform("print_volume.z_data", m_render_volume.zs);

        if (volume.first->partly_inside && partly_inside_enable) {
            //only partly inside volume need to be painted with boundary check
            shader->set_uniform("print_volume.type", static_cast<int>(m_print_volume.type));
            shader->set_uniform("print_volume.xy_data", m_print_volume.data);
            shader->set_uniform("print_volume.z_data", m_print_volume.zs);
        }
        else {
            //use -1 ad a invalid type
            shader->set_uniform("print_volume.type", -1);
        }
        
        bool  enable_support;
        int   support_threshold_angle = get_selection_support_threshold_angle(enable_support);
    
        float normal_z  = -::cos(Geometry::deg2rad((float) support_threshold_angle));
  
        shader->set_uniform("volume_world_matrix", volume.first->world_matrix());
        shader->set_uniform("slope.actived", m_slope.isGlobalActive && !volume.first->is_modifier && !volume.first->is_wipe_tower);
        shader->set_uniform("slope.volume_world_normal_matrix", static_cast<Matrix3f>(volume.first->world_matrix().matrix().block(0, 0, 3, 3).inverse().transpose().cast<float>()));
        shader->set_uniform("slope.normal_z", normal_z);

#if ENABLE_ENVIRONMENT_MAP
        unsigned int environment_texture_id = GUI::wxGetApp().plater()->get_environment_texture_id();
        bool use_environment_texture = environment_texture_id > 0 && GUI::wxGetApp().app_config->get("use_environment_map") == "1";
        shader->set_uniform("use_environment_tex", use_environment_texture);
        if (use_environment_texture)
            glsafe(::glBindTexture(GL_TEXTURE_2D, environment_texture_id));
#endif // ENABLE_ENVIRONMENT_MAP
        glcheck();

		auto red_color = ColorRGBA{1.0f, 0.0f, 0.0f, 1.0f};//slice_error
        volume.first->model.set_color(volume.first->slice_error ? red_color : volume.first->render_color);
        const Transform3d model_matrix = volume.first->world_matrix();
        shader->set_uniform("view_model_matrix", view_matrix * model_matrix);
        shader->set_uniform("projection_matrix", projection_matrix);
        const Matrix3d view_normal_matrix = view_matrix.matrix().block(0, 0, 3, 3) * model_matrix.matrix().block(0, 0, 3, 3).inverse().transpose();
        shader->set_uniform("view_normal_matrix", view_normal_matrix);
		//BBS: add outline related logic
        if (volume.first->selected && GUI::wxGetApp().show_outline())
            volume.first->render_with_outline(cnv_size);
        else
            volume.first->render();

#if ENABLE_ENVIRONMENT_MAP
        if (use_environment_texture)
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
#endif // ENABLE_ENVIRONMENT_MAP

        glsafe(::glBindBuffer(GL_ARRAY_BUFFER, 0));
        glsafe(::glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0));
    }

    if (m_show_sinking_contours) {
        shader->stop_using();
        if (sink_shader != nullptr) {
            sink_shader->start_using();
            for (GLVolumeWithIdAndZ& volume : to_render) {
                // render sinking contours of hovered/displaced volumes
                if (volume.first->is_sinking() && !volume.first->is_below_printbed() &&
                    (volume.first->hover != GLVolume::HS_None || volume.first->force_sinking_contours)) {
                    glsafe(::glDepthFunc(GL_ALWAYS));
                    volume.first->render_sinking_contours();
                    glsafe(::glDepthFunc(GL_LESS));
                }
            }
            sink_shader->start_using();
        }
        shader->start_using();
    }

    if (disable_cullface)
        glsafe(::glEnable(GL_CULL_FACE));

    if (type == ERenderType::Transparent)
        glsafe(::glDisable(GL_BLEND));
}

bool GLVolumeCollection::check_wipe_tower_outside_state(const Slic3r::BuildVolume &build_volume, int plate_id) const
{
    for (GLVolume *volume : this->volumes) {
        if (volume->is_wipe_tower) {
            int wipe_tower_plate_id = volume->composite_id.object_id - 1000;
            if (wipe_tower_plate_id != plate_id)
                continue;
            const std::vector<Vec2d>& printable_area = build_volume.printable_area();
            Polygon printable_poly = Polygon::new_scale(printable_area);

            // multi-extruder
            Polygons extruder_polys;
            const std::vector<std::vector<Vec2d>> & extruder_areas = build_volume.extruder_areas();
            if (!extruder_areas.empty()) {
                for (size_t i = 0; i < extruder_areas.size(); ++i) {
                    extruder_polys.emplace_back(Polygon::new_scale(extruder_areas[i]));
                }
                extruder_polys = union_(extruder_polys);
                if (extruder_polys.empty())
                    return false;

                printable_poly = extruder_polys[0];
            }

            const BoundingBoxf3 &bbox = volume->transformed_convex_hull_bounding_box();
            Polygon wipe_tower_polygon = bbox.polygon(true);

            Polygons diff_res = diff(wipe_tower_polygon, printable_poly);
            return diff_res.empty();
        }
    }
    return true;
}

bool GLVolumeCollection::check_outside_state(const BuildVolume &build_volume, ModelInstanceEPrintVolumeState *out_state, ObjectFilamentResults* object_results) const
{
    if (GUI::wxGetApp().plater() == NULL)
    {
        if (out_state != nullptr)
            *out_state = ModelInstancePVS_Inside;
        return false;
    }

    const Model&        model              = GUI::wxGetApp().plater()->model();
    auto                volume_below       = [](GLVolume& volume) -> bool
        { return volume.object_idx() != -1 && volume.volume_idx() != -1 && volume.is_below_printbed(); };
    // Volume is partially below the print bed, thus a pre-calculated convex hull cannot be used.
    auto                volume_sinking     = [](GLVolume& volume) -> bool
        { return volume.object_idx() != -1 && volume.volume_idx() != -1 && volume.is_sinking(); };
    // Cached bounding box of a volume above the print bed.
    auto                volume_bbox        = [volume_sinking](GLVolume& volume) -> BoundingBoxf3
        { return volume_sinking(volume) ? volume.transformed_non_sinking_bounding_box() : volume.transformed_convex_hull_bounding_box(); };
    // Cached 3D convex hull of a volume above the print bed.
    auto                volume_convex_mesh = [volume_sinking, &model](GLVolume& volume) -> const TriangleMesh&
        { return volume_sinking(volume) ? model.objects[volume.object_idx()]->volumes[volume.volume_idx()]->mesh() : *volume.convex_hull(); };

    ModelInstanceEPrintVolumeState overall_state = ModelInstancePVS_Fully_Outside;
    bool contained_min_one = false;

    //BBS: add instance judge logic, besides to original volume judge logic
    //std::map<int64_t, ModelInstanceEPrintVolumeState> model_state;

    GUI::PartPlate* curr_plate = GUI::wxGetApp().plater()->get_partplate_list().get_selected_plate();
    const Pointfs& pp_bed_shape = curr_plate->get_shape();
    BuildVolume plate_build_volume(pp_bed_shape, build_volume.printable_height(), build_volume.extruder_areas(), build_volume.extruder_heights());
    const std::vector<BoundingBoxf3>& exclude_areas = curr_plate->get_exclude_areas();

    std::map<ModelObject*, std::map<int, std::set<int>>> objects_unprintable_filaments;
    int extruder_count = build_volume.get_extruder_area_count();
    std::vector<std::set<int>> unprintable_filament_ids(extruder_count, std::set<int>());
    std::set<ModelObject*> partly_objects_set;
    const ModelObjectPtrs &model_objects = model.objects;
    for (GLVolume* volume : this->volumes)
    {
        std::vector<bool> inside_extruders;
        if (! volume->is_modifier && (volume->shader_outside_printer_detection_enabled || (! volume->is_wipe_tower && volume->composite_id.volume_id >= 0))) {
            BuildVolume::ObjectState state;
            if (volume_below(*volume))
                state = BuildVolume::ObjectState::Below;
            else {
                switch (plate_build_volume.type()) {
                case BuildVolume_Type::Rectangle: {
                    //FIXME this test does not evaluate collision of a build volume bounding box with non-convex objects.
                    const BoundingBoxf3& bb = volume_bbox(*volume);
                    state = plate_build_volume.volume_state_bbox(bb);
                    if ((state == BuildVolume::ObjectState::Inside) && (extruder_count > 1))
                    {
                        state = plate_build_volume.check_volume_bbox_state_with_extruder_areas(bb, inside_extruders);
                    }
                    break;
                }
                case BuildVolume_Type::Circle:
                case BuildVolume_Type::Convex:
                //FIXME doing test on convex hull until we learn to do test on non-convex polygons efficiently.
                case BuildVolume_Type::Custom:
                {
                    const indexed_triangle_set& convex_mesh_it = volume_convex_mesh(*volume).its;
                    const Transform3f trafo = volume->world_matrix().cast<float>();
                    state = plate_build_volume.object_state(convex_mesh_it, trafo, volume_sinking(*volume));
                    if ((state == BuildVolume::ObjectState::Inside) && (extruder_count > 1))
                    {
                        state = plate_build_volume.check_object_state_with_extruder_areas(convex_mesh_it, trafo, inside_extruders);
                    }
                    break;
                }
                default:
                    // Ignore, don't produce any collision.
                    state = BuildVolume::ObjectState::Inside;
                    break;
                }
                assert(state != BuildVolume::ObjectState::Below);

                if (state == BuildVolume::ObjectState::Limited) {
                    //unprintable_filament_ids.resize(inside_extruders.size());
                    ModelObject *model_object = model_objects[volume->object_idx()];
                    ModelVolume *model_volume = model_object->volumes[volume->volume_idx()];
                    for (size_t i = 0; i < inside_extruders.size(); ++i) {
                        if (!inside_extruders[i]) {
                            std::vector<int> filaments = model_volume->get_extruders();
                            unprintable_filament_ids[i].insert(filaments.begin(), filaments.end());
                            if (object_results) {
                                std::map<int, std::set<int>>& obj_extruder_filament_maps = objects_unprintable_filaments[model_object];
                                std::set<int>& obj_extruder_filaments = obj_extruder_filament_maps[i+1];
                                obj_extruder_filaments.insert(filaments.begin(), filaments.end());
                            }
                        }
                    }
                }
            }

            //int64_t comp_id = ((int64_t)volume->composite_id.object_id << 32) | ((int64_t)volume->composite_id.instance_id);
            volume->is_outside = (state != BuildVolume::ObjectState::Inside && state != BuildVolume::ObjectState::Limited);
            volume->partly_inside = (state == BuildVolume::ObjectState::Colliding);
            if (volume->printable) {
                if (state == BuildVolume::ObjectState::Colliding)
                {
                    overall_state = ModelInstancePVS_Partly_Outside;
                    partly_objects_set.emplace(model_objects[volume->object_idx()]);
                }
                else if ((state == BuildVolume::ObjectState::Limited) && (overall_state != ModelInstancePVS_Partly_Outside))
                    overall_state = ModelInstancePVS_Limited;
                else if ((state == BuildVolume::ObjectState::Inside) && (overall_state == ModelInstancePVS_Fully_Outside)) {
                    overall_state = ModelInstancePVS_Fully_Outside;
                }
                contained_min_one |= !volume->is_outside;
            }

            /*ModelInstanceEPrintVolumeState volume_state;
            //if (volume->is_outside && (plate_build_volume.bounding_volume().intersects(volume->bounding_box())))
            if (volume->is_outside && (state == BuildVolume::ObjectState::Colliding))
                volume_state = ModelInstancePVS_Partly_Outside;
            else if (volume->is_outside)
                volume_state = ModelInstancePVS_Fully_Outside;
            else
                volume_state = ModelInstancePVS_Inside;

            if (model_state.find(comp_id) != model_state.end())
            {
                if (model_state[comp_id] != ModelInstancePVS_Partly_Outside)
                {
                    if (volume_state == ModelInstancePVS_Partly_Outside)
                        model_state[comp_id] = ModelInstancePVS_Partly_Outside;
                    else if (model_state[comp_id] != volume_state)
                    {
                        model_state[comp_id] = ModelInstancePVS_Partly_Outside;
                    }
                }
            }
            else
            {
                model_state[comp_id] = volume_state;
            }

            if (model_state[comp_id] == ModelInstancePVS_Partly_Outside) {
                overall_state = ModelInstancePVS_Partly_Outside;
                BOOST_LOG_TRIVIAL(debug) << "instance includes " << volume->name << " is partially outside of bed";
            }*/
        }
    }

    std::vector<std::vector<int>> unprintable_filament_vec;
    for (const std::set<int>& filamnt_ids : unprintable_filament_ids) {
        unprintable_filament_vec.emplace_back(std::vector<int>(filamnt_ids.begin(), filamnt_ids.end()));
    }

    if (object_results && !partly_objects_set.empty()) {
        object_results->partly_outside_objects = std::vector<ModelObject*>(partly_objects_set.begin(), partly_objects_set.end());
    }

    //check per-object error for extruder areas
    if (object_results && (extruder_count > 1))
    {
        const auto& project_config = Slic3r::GUI::wxGetApp().preset_bundle->project_config;
        object_results->mode = curr_plate->get_real_filament_map_mode(project_config);
        if (object_results->mode < FilamentMapMode::fmmManual)
        {
            std::vector<int> conflict_filament_vector;
            for (int index = 0; index < extruder_count; index++ )
            {
                if (!unprintable_filament_vec[index].empty())
                {
                    std::sort (unprintable_filament_vec[index].begin(), unprintable_filament_vec[index].end());
                    if (index == 0)
                        conflict_filament_vector = unprintable_filament_vec[index];
                    else
                    {
                        std::vector<int> result_filaments;
                        //result_filaments.reserve(conflict_filaments.size());
                        std::set_intersection (conflict_filament_vector.begin(), conflict_filament_vector.end(), unprintable_filament_vec[index].begin(), unprintable_filament_vec[index].end(), insert_iterator<vector<int>>(result_filaments, result_filaments.begin()));
                        conflict_filament_vector = result_filaments;
                    }
                }
                else
                {
                    conflict_filament_vector.clear();
                    break;
                }
            }

            if (!conflict_filament_vector.empty())
            {
                std::set<int> conflict_filaments_set(conflict_filament_vector.begin(), conflict_filament_vector.end());
                object_results->filaments = conflict_filament_vector;

                for (auto& object_map: objects_unprintable_filaments)
                {
                    ModelObject *model_object = object_map.first;
                    std::map<int, std::set<int>>& obj_extruder_filament_maps = object_map.second;
                    std::set<int> obj_filaments_set;
                    ObjectFilamentInfo object_filament_info;
                    object_filament_info.object = model_object;

                    for (std::map<int, std::set<int>>::iterator extruder_map_iter = obj_extruder_filament_maps.begin(); extruder_map_iter != obj_extruder_filament_maps.end(); extruder_map_iter++ )
                    {
                        int extruder_id = extruder_map_iter->first;
                        std::set<int>& filaments_set = extruder_map_iter->second;

                        for (int filament: filaments_set)
                        {
                            if (conflict_filaments_set.find(filament) != conflict_filaments_set.end())
                            {
                                obj_filaments_set.emplace(filament);
                            }
                        }
                    }
                    if (!obj_filaments_set.empty()) {
                        object_filament_info.auto_filaments = std::vector<int>(obj_filaments_set.begin(), obj_filaments_set.end());
                        object_results->object_filaments.push_back(std::move(object_filament_info));
                    }
                }
            }
        }
        else
        {
            std::set<int> conflict_filaments_set;
            const auto& project_config = Slic3r::GUI::wxGetApp().preset_bundle->project_config;
            std::vector<int> filament_maps = curr_plate->get_real_filament_maps(project_config);
            for (auto& object_map: objects_unprintable_filaments)
            {
                ModelObject *model_object = object_map.first;
                std::map<int, std::set<int>>& obj_extruder_filament_maps = object_map.second;
                ObjectFilamentInfo object_filament_info;
                object_filament_info.object = model_object;

                for (std::map<int, std::set<int>>::iterator extruder_map_iter = obj_extruder_filament_maps.begin(); extruder_map_iter != obj_extruder_filament_maps.end(); extruder_map_iter++ )
                {
                    int extruder_id = extruder_map_iter->first;
                    std::set<int>& filaments_set = extruder_map_iter->second;

                    for (int filament: filaments_set)
                    {
                        if (filament_maps[filament - 1] == extruder_id)
                        {
                            object_filament_info.manual_filaments.emplace(filament, extruder_id);
                            object_results->filament_maps[filament] = extruder_id;
                            conflict_filaments_set.emplace(filament);
                        }
                    }
                }
                if (!object_filament_info.manual_filaments.empty())
                {
                    object_results->object_filaments.push_back(std::move(object_filament_info));
                }
            }
            if (!conflict_filaments_set.empty()) {
                object_results->filaments = std::vector<int>(conflict_filaments_set.begin(), conflict_filaments_set.end());
            }
        }
    }

    /*for (GLVolume* volume : this->volumes)
    {
        if (! volume->is_modifier && (volume->shader_outside_printer_detection_enabled || (! volume->is_wipe_tower && volume->composite_id.volume_id >= 0)))
        {
            int64_t comp_id = ((int64_t)volume->composite_id.object_id << 32) | ((int64_t)volume->composite_id.instance_id);
            if (model_state.find(comp_id) != model_state.end())
            {
                if (model_state[comp_id] == ModelInstancePVS_Partly_Outside) {
                    volume->partly_inside = true;
                }
                else
                    volume->partly_inside = false;
            }
        }
    }*/

    if (out_state != nullptr)
        *out_state = overall_state;

    return contained_min_one;
}

void GLVolumeCollection::reset_outside_state()
{
    for (GLVolume* volume : this->volumes)
    {
        if (volume != nullptr) {
            volume->is_outside = false;
            volume->partly_inside = false;
        }
    }
}

void GLVolumeCollection::update_colors_by_extruder(const DynamicPrintConfig *config, bool is_update_alpha)
{
    
    using ColorItem = std::pair<std::string, ColorRGBA>;
    std::vector<ColorItem> colors;

    if (static_cast<PrinterTechnology>(config->opt_int("printer_technology")) == ptSLA) {
        const std::string& txt_color = config->opt_string("material_colour").empty() ?
                                       print_config_def.get("material_colour")->get_default_value<ConfigOptionString>()->value :
                                       config->opt_string("material_colour");
        ColorRGBA rgba;
        if (decode_color(txt_color, rgba))
            colors.push_back({ txt_color, rgba });
    }
    else {
        const ConfigOptionStrings* filamemts_opt = dynamic_cast<const ConfigOptionStrings*>(config->option("filament_colour"));
        if (filamemts_opt == nullptr)
            return;

        size_t colors_count = (size_t)filamemts_opt->values.size();
        if (colors_count == 0)
            return;
        colors.resize(colors_count);

        for (unsigned int i = 0; i < colors_count; ++i) {
            ColorRGBA rgba;
            const std::string& fil_color = config->opt_string("filament_colour", i);
            if (decode_color(fil_color, rgba))
                colors[i] = { fil_color, rgba };
        }
    }

    for (GLVolume* volume : volumes) {
        if (volume == nullptr || volume->is_modifier || volume->is_wipe_tower || volume->volume_idx() < 0)
            continue;

        int extruder_id = volume->extruder_id - 1;
        if (extruder_id < 0 || (int)colors.size() <= extruder_id)
            extruder_id = 0;

        const ColorItem& color = colors[extruder_id];
        if (!color.first.empty()) {
            if (!is_update_alpha) {
                float old_a   = volume->color.a();
                volume->color = color.second;
                volume->color.a(old_a);
            } else {
                volume->color = color.second;
            }
        }
    }
}

void GLVolumeCollection::set_transparency(float alpha)
{
    for (GLVolume *volume : volumes) {
        if (volume == nullptr || volume->is_modifier || volume->is_wipe_tower || (volume->volume_idx() < 0))
            continue;

        volume->color.a(alpha);
    }
}

std::vector<double> GLVolumeCollection::get_current_print_zs(bool active_only) const
{
    // Collect layer top positions of all volumes.
    std::vector<double> print_zs;
    for (GLVolume *vol : this->volumes)
    {
        if (!active_only || vol->is_active)
            append(print_zs, vol->print_zs);
    }
    std::sort(print_zs.begin(), print_zs.end());

    // Replace intervals of layers with similar top positions with their average value.
    int n = int(print_zs.size());
    int k = 0;
    for (int i = 0; i < n;) {
        int j = i + 1;
        coordf_t zmax = print_zs[i] + EPSILON;
        for (; j < n && print_zs[j] <= zmax; ++ j) ;
        print_zs[k ++] = (j > i + 1) ? (0.5 * (print_zs[i] + print_zs[j - 1])) : print_zs[i];
        i = j;
    }
    if (k < n)
        print_zs.erase(print_zs.begin() + k, print_zs.end());

    return print_zs;
}

size_t GLVolumeCollection::cpu_memory_used() const
{
	size_t memsize = sizeof(*this) + this->volumes.capacity() * sizeof(GLVolume);
	for (const GLVolume *volume : this->volumes)
		memsize += volume->cpu_memory_used();
	return memsize;
}

size_t GLVolumeCollection::gpu_memory_used() const
{
	size_t memsize = 0;
	for (const GLVolume *volume : this->volumes)
		memsize += volume->gpu_memory_used();
	return memsize;
}

std::string GLVolumeCollection::log_memory_info() const
{
	return " (GLVolumeCollection RAM: " + format_memsize_MB(this->cpu_memory_used()) + " GPU: " + format_memsize_MB(this->gpu_memory_used()) + " Both: " + format_memsize_MB(this->gpu_memory_used()) + ")";
}

static void thick_lines_to_geometry(
    const Lines&               lines,
    const std::vector<double>& widths,
    const std::vector<double>& heights,
    bool                       closed,
    double                     top_z,
    GUI::GLModel::Geometry&    geometry)
{
    assert(!lines.empty());
    if (lines.empty())
        return;

    enum Direction : unsigned char
    {
        Left,
        Right,
        Top,
        Bottom
    };

    // right, left, top, bottom
    std::array<int, 4> idx_prev    = { -1, -1, -1, -1 };
    std::array<int, 4> idx_initial = { -1, -1, -1, -1 };

    double bottom_z_prev = 0.0;
    Vec2d  b1_prev(Vec2d::Zero());
    Vec2d  v_prev(Vec2d::Zero());
    double len_prev = 0.0;
    double width_initial = 0.0;
    double bottom_z_initial = 0.0;

    // loop once more in case of closed loops
    const size_t lines_end = closed ? (lines.size() + 1) : lines.size();
    for (size_t ii = 0; ii < lines_end; ++ii) {
        const size_t i = (ii == lines.size()) ? 0 : ii;
        const Line& line = lines[i];
        const double bottom_z = top_z - heights[i];
        const double middle_z = 0.5 * (top_z + bottom_z);
        const double width = widths[i];

        const bool is_first = (ii == 0);
        const bool is_last = (ii == lines_end - 1);
        const bool is_closing = closed && is_last;

        const Vec2d v = unscale(line.vector()).normalized();
        const double len = unscale<double>(line.length());

        const Vec2d a = unscale(line.a);
        const Vec2d b = unscale(line.b);
        Vec2d a1 = a;
        Vec2d a2 = a;
        Vec2d b1 = b;
        Vec2d b2 = b;
        {
            const double dist = 0.5 * width;  // scaled
            const double dx = dist * v.x();
            const double dy = dist * v.y();
            a1 += Vec2d(+dy, -dx);
            a2 += Vec2d(-dy, +dx);
            b1 += Vec2d(+dy, -dx);
            b2 += Vec2d(-dy, +dx);
        }

        // calculate new XY normals
        const Vec2d xy_right_normal = unscale(line.normal()).normalized();

        std::array<int, 4> idx_a = { 0, 0, 0, 0 };
        std::array<int, 4> idx_b = { 0, 0, 0, 0 };
        int idx_last = int(geometry.vertices_count());

        const bool bottom_z_different = bottom_z_prev != bottom_z;
        bottom_z_prev = bottom_z;

        if (!is_first && bottom_z_different) {
            // Found a change of the layer thickness -> Add a cap at the end of the previous segment.
            geometry.add_triangle(idx_b[Bottom], idx_b[Left], idx_b[Top]);
            geometry.add_triangle(idx_b[Bottom], idx_b[Top], idx_b[Right]);
        }

        // Share top / bottom vertices if possible.
        if (is_first) {
            idx_a[Top] = idx_last++;
            geometry.add_vertex(Vec3f(a.x(), a.y(), top_z), Vec3f(0.0f, 0.0f, 1.0f));
        }
        else
            idx_a[Top] = idx_prev[Top];

        if (is_first || bottom_z_different) {
            // Start of the 1st line segment or a change of the layer thickness while maintaining the print_z.
            idx_a[Bottom] = idx_last++;
            geometry.add_vertex(Vec3f(a.x(), a.y(), bottom_z), Vec3f(0.0f, 0.0f, -1.0f));
            idx_a[Left] = idx_last++;
            geometry.add_vertex(Vec3f(a2.x(), a2.y(), middle_z), Vec3f(-xy_right_normal.x(), -xy_right_normal.y(), 0.0f));
            idx_a[Right] = idx_last++;
            geometry.add_vertex(Vec3f(a1.x(), a1.y(), middle_z), Vec3f(xy_right_normal.x(), xy_right_normal.y(), 0.0f));
        }
        else
            idx_a[Bottom] = idx_prev[Bottom];

        if (is_first) {
            // Start of the 1st line segment.
            width_initial = width;
            bottom_z_initial = bottom_z;
            idx_initial = idx_a;
        }
        else {
            // Continuing a previous segment.
            // Share left / right vertices if possible.
            const double v_dot = v_prev.dot(v);
            // To reduce gpu memory usage, we try to reuse vertices
            // To reduce the visual artifacts, due to averaged normals, we allow to reuse vertices only when any of two adjacent edges 
            // is longer than a fixed threshold.
            // The following value is arbitrary, it comes from tests made on a bunch of models showing the visual artifacts
            const double len_threshold = 2.5;

            // Generate new vertices if the angle between adjacent edges is greater than 45 degrees or thresholds conditions are met
            const bool sharp = (v_dot < 0.707) || (len_prev > len_threshold) || (len > len_threshold);
            if (sharp) {
                if (!bottom_z_different) {
                    // Allocate new left / right points for the start of this segment as these points will receive their own normals to indicate a sharp turn.
                    idx_a[Right] = idx_last++;
                    geometry.add_vertex(Vec3f(a1.x(), a1.y(), middle_z), Vec3f(xy_right_normal.x(), xy_right_normal.y(), 0.0f));
                    idx_a[Left] = idx_last++;
                    geometry.add_vertex(Vec3f(a2.x(), a2.y(), middle_z), Vec3f(-xy_right_normal.x(), -xy_right_normal.y(), 0.0f));
                    if (cross2(v_prev, v) > 0.0) {
                        // Right turn. Fill in the right turn wedge.
                        geometry.add_triangle(idx_prev[Right], idx_a[Right], idx_prev[Top]);
                        geometry.add_triangle(idx_prev[Right], idx_prev[Bottom], idx_a[Right]);
                    }
                    else {
                        // Left turn. Fill in the left turn wedge.
                        geometry.add_triangle(idx_prev[Left], idx_prev[Top], idx_a[Left]);
                        geometry.add_triangle(idx_prev[Left], idx_a[Left], idx_prev[Bottom]);
                    }
                }
            }
            else {
                if (!bottom_z_different) {
                    // The two successive segments are nearly collinear.
                    idx_a[Left]  = idx_prev[Left];
                    idx_a[Right] = idx_prev[Right];
                }
            }
            if (is_closing) {
                if (!sharp) {
                    if (!bottom_z_different) {
                        // Closing a loop with smooth transition. Unify the closing left / right vertices.
                        geometry.set_vertex(idx_initial[Left], geometry.extract_position_3(idx_prev[Left]), geometry.extract_normal_3(idx_prev[Left]));
                        geometry.set_vertex(idx_initial[Right], geometry.extract_position_3(idx_prev[Right]), geometry.extract_normal_3(idx_prev[Right]));
                        geometry.remove_vertex(geometry.vertices_count() - 1);
                        geometry.remove_vertex(geometry.vertices_count() - 1);
                        // Replace the left / right vertex indices to point to the start of the loop.
                        const size_t indices_count = geometry.indices_count();
                        for (size_t u = indices_count - 24; u < indices_count; ++u) {
                            const unsigned int id = geometry.extract_index(u);
                            if (id == (unsigned int)idx_prev[Left])
                                geometry.set_index(u, (unsigned int)idx_initial[Left]);
                            else if (id == (unsigned int)idx_prev[Right])
                                geometry.set_index(u, (unsigned int)idx_initial[Right]);
                        }
                    }
                }
                // This is the last iteration, only required to solve the transition.
                break;
            }
        }

        // Only new allocate top / bottom vertices, if not closing a loop.
        if (is_closing)
            idx_b[Top] = idx_initial[Top];
        else {
            idx_b[Top] = idx_last++;
            geometry.add_vertex(Vec3f(b.x(), b.y(), top_z), Vec3f(0.0f, 0.0f, 1.0f));
        }

        if (is_closing && width == width_initial && bottom_z == bottom_z_initial)
            idx_b[Bottom] = idx_initial[Bottom];
        else {
            idx_b[Bottom] = idx_last++;
            geometry.add_vertex(Vec3f(b.x(), b.y(), bottom_z), Vec3f(0.0f, 0.0f, -1.0f));
        }
        // Generate new vertices for the end of this line segment.
        idx_b[Left] = idx_last++;
        geometry.add_vertex(Vec3f(b2.x(), b2.y(), middle_z), Vec3f(-xy_right_normal.x(), -xy_right_normal.y(), 0.0f));
        idx_b[Right] = idx_last++;
        geometry.add_vertex(Vec3f(b1.x(), b1.y(), middle_z), Vec3f(xy_right_normal.x(), xy_right_normal.y(), 0.0f));

        idx_prev = idx_b;
        bottom_z_prev = bottom_z;
        b1_prev = b1;
        v_prev = v;
        len_prev = len;

        if (bottom_z_different && (closed || (!is_first && !is_last))) {
            // Found a change of the layer thickness -> Add a cap at the beginning of this segment.
            geometry.add_triangle(idx_a[Bottom], idx_a[Right], idx_a[Top]);
            geometry.add_triangle(idx_a[Bottom], idx_a[Top], idx_a[Left]);
        }

        if (!closed) {
            // Terminate open paths with caps.
            if (is_first) {
                geometry.add_triangle(idx_a[Bottom], idx_a[Right], idx_a[Top]);
                geometry.add_triangle(idx_a[Bottom], idx_a[Top], idx_a[Left]);
            }
            // We don't use 'else' because both cases are true if we have only one line.
            if (is_last) {
                geometry.add_triangle(idx_b[Bottom], idx_b[Left], idx_b[Top]);
                geometry.add_triangle(idx_b[Bottom], idx_b[Top], idx_b[Right]);
            }
        }

        // Add quads for a straight hollow tube-like segment.
        // bottom-right face
        geometry.add_triangle(idx_a[Bottom], idx_b[Bottom], idx_b[Right]);
        geometry.add_triangle(idx_a[Bottom], idx_b[Right], idx_a[Right]);
        // top-right face
        geometry.add_triangle(idx_a[Right], idx_b[Right], idx_b[Top]);
        geometry.add_triangle(idx_a[Right], idx_b[Top], idx_a[Top]);
        // top-left face
        geometry.add_triangle(idx_a[Top], idx_b[Top], idx_b[Left]);
        geometry.add_triangle(idx_a[Top], idx_b[Left], idx_a[Left]);
        // bottom-left face
        geometry.add_triangle(idx_a[Left], idx_b[Left], idx_b[Bottom]);
        geometry.add_triangle(idx_a[Left], idx_b[Bottom], idx_a[Bottom]);
    }
}

// caller is responsible for supplying NO lines with zero length
static void thick_lines_to_geometry(
    const Lines3&              lines,
    const std::vector<double>& widths,
    const std::vector<double>& heights,
    bool                       closed,
    GUI::GLModel::Geometry&    geometry)
{
    assert(!lines.empty());
    if (lines.empty())
        return;

    enum Direction : unsigned char
    {
        Left,
        Right,
        Top,
        Bottom
    };

    // left, right, top, bottom
    std::array<int, 4> idx_prev    = { -1, -1, -1, -1 };
    std::array<int, 4> idx_initial = { -1, -1, -1, -1 };

    double z_prev = 0.0;
    double len_prev = 0.0;
    Vec3d  n_right_prev = Vec3d::Zero();
    Vec3d  n_top_prev = Vec3d::Zero();
    Vec3d  unit_v_prev = Vec3d::Zero();
    double width_initial = 0.0;

    // new vertices around the line endpoints
    // left, right, top, bottom
    std::array<Vec3d, 4> a = { Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero() };
    std::array<Vec3d, 4> b = { Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero(), Vec3d::Zero() };

    // loop once more in case of closed loops
    const size_t lines_end = closed ? (lines.size() + 1) : lines.size();
    for (size_t ii = 0; ii < lines_end; ++ii) {
        const size_t i = (ii == lines.size()) ? 0 : ii;

        const Line3& line = lines[i];
        const double height = heights[i];
        const double width = widths[i];

        const Vec3d unit_v = unscale(line.vector()).normalized();
        const double len = unscale<double>(line.length());

        Vec3d n_top = Vec3d::Zero();
        Vec3d n_right = Vec3d::Zero();

        if (line.a.x() == line.b.x() && line.a.y() == line.b.y()) {
            // vertical segment
            n_top = Vec3d::UnitY();
            n_right = Vec3d::UnitX();
            if (line.a.z() < line.b.z())
                n_right = -n_right;
        }
        else {
            // horizontal segment
            n_right = unit_v.cross(Vec3d::UnitZ()).normalized();
            n_top = n_right.cross(unit_v).normalized();
        }

        const Vec3d rl_displacement = 0.5 * width * n_right;
        const Vec3d tb_displacement = 0.5 * height * n_top;
        const Vec3d l_a = unscale(line.a);
        const Vec3d l_b = unscale(line.b);

        a[Right]  = l_a + rl_displacement;
        a[Left]   = l_a - rl_displacement;
        a[Top]    = l_a + tb_displacement;
        a[Bottom] = l_a - tb_displacement;
        b[Right]  = l_b + rl_displacement;
        b[Left]   = l_b - rl_displacement;
        b[Top]    = l_b + tb_displacement;
        b[Bottom] = l_b - tb_displacement;

        const Vec3d n_bottom = -n_top;
        const Vec3d n_left = -n_right;

        std::array<int, 4> idx_a = { 0, 0, 0, 0};
        std::array<int, 4> idx_b = { 0, 0, 0, 0 };
        int idx_last = int(geometry.vertices_count());

        const bool z_different = (z_prev != l_a.z());
        z_prev = l_b.z();

        // Share top / bottom vertices if possible.
        if (ii == 0) {
            idx_a[Top] = idx_last++;
            geometry.add_vertex((Vec3f)a[Top].cast<float>(), (Vec3f)n_top.cast<float>());
        }
        else
            idx_a[Top] = idx_prev[Top];

        if (ii == 0 || z_different) {
            // Start of the 1st line segment or a change of the layer thickness while maintaining the print_z.
            idx_a[Bottom] = idx_last++;
            geometry.add_vertex((Vec3f)a[Bottom].cast<float>(), (Vec3f)n_bottom.cast<float>());
            idx_a[Left] = idx_last++;
            geometry.add_vertex((Vec3f)a[Left].cast<float>(), (Vec3f)n_left.cast<float>());
            idx_a[Right] = idx_last++;
            geometry.add_vertex((Vec3f)a[Right].cast<float>(), (Vec3f)n_right.cast<float>());
        }
        else
            idx_a[Bottom] = idx_prev[Bottom];

        if (ii == 0) {
            // Start of the 1st line segment.
            width_initial = width;
            idx_initial =  idx_a;
        }
        else {
            // Continuing a previous segment.
            // Share left / right vertices if possible.
            const double v_dot = unit_v_prev.dot(unit_v);
            const bool is_right_turn = n_top_prev.dot(unit_v_prev.cross(unit_v)) > 0.0;

            // To reduce gpu memory usage, we try to reuse vertices
            // To reduce the visual artifacts, due to averaged normals, we allow to reuse vertices only when any of two adjacent edges 
            // is longer than a fixed threshold.
            // The following value is arbitrary, it comes from tests made on a bunch of models showing the visual artifacts
            const double len_threshold = 2.5;

            // Generate new vertices if the angle between adjacent edges is greater than 45 degrees or thresholds conditions are met
            const bool is_sharp = v_dot < 0.707 || len_prev > len_threshold || len > len_threshold;
            if (is_sharp) {
                // Allocate new left / right points for the start of this segment as these points will receive their own normals to indicate a sharp turn.
                idx_a[Right] = idx_last++;
                geometry.add_vertex((Vec3f)a[Right].cast<float>(), (Vec3f)n_right.cast<float>());
                idx_a[Left] = idx_last++;
                geometry.add_vertex((Vec3f)a[Left].cast<float>(), (Vec3f)n_left.cast<float>());

                if (is_right_turn) {
                    // Right turn. Fill in the right turn wedge.
                    geometry.add_triangle(idx_prev[Right], idx_a[Right], idx_prev[Top]);
                    geometry.add_triangle(idx_prev[Right], idx_prev[Bottom], idx_a[Right]);
                }
                else {
                    // Left turn. Fill in the left turn wedge.
                    geometry.add_triangle(idx_prev[Left], idx_prev[Top], idx_a[Left]);
                    geometry.add_triangle(idx_prev[Left], idx_a[Left], idx_prev[Bottom]);
                }
            }
            else {
                // The two successive segments are nearly collinear.
                idx_a[Left] = idx_prev[Left];
                idx_a[Right] = idx_prev[Right];
            }

            if (ii == lines.size()) {
                if (!is_sharp) {
                    // Closing a loop with smooth transition. Unify the closing left / right vertices.
                    geometry.set_vertex(idx_initial[Left], geometry.extract_position_3(idx_prev[Left]), geometry.extract_normal_3(idx_prev[Left]));
                    geometry.set_vertex(idx_initial[Right], geometry.extract_position_3(idx_prev[Right]), geometry.extract_normal_3(idx_prev[Right]));
                    geometry.remove_vertex(geometry.vertices_count() - 1);
                    geometry.remove_vertex(geometry.vertices_count() - 1);
                    // Replace the left / right vertex indices to point to the start of the loop.
                    const size_t indices_count = geometry.indices_count();
                    for (size_t u = indices_count - 24; u < indices_count; ++u) {
                        const unsigned int id = geometry.extract_index(u);
                        if (id == (unsigned int)idx_prev[Left])
                            geometry.set_index(u, (unsigned int)idx_initial[Left]);
                        else if (id == (unsigned int)idx_prev[Right])
                            geometry.set_index(u, (unsigned int)idx_initial[Right]);
                    }
                }

                // This is the last iteration, only required to solve the transition.
                break;
            }
        }

        // Only new allocate top / bottom vertices, if not closing a loop.
        if (closed && ii + 1 == lines.size())
            idx_b[Top] = idx_initial[Top];
        else {
            idx_b[Top] = idx_last++;
            geometry.add_vertex((Vec3f)b[Top].cast<float>(), (Vec3f)n_top.cast<float>());
        }

        if (closed && ii + 1 == lines.size() && width == width_initial)
            idx_b[Bottom] = idx_initial[Bottom];
        else {
            idx_b[Bottom] = idx_last++;
            geometry.add_vertex((Vec3f)b[Bottom].cast<float>(), (Vec3f)n_bottom.cast<float>());
        }

        // Generate new vertices for the end of this line segment.
        idx_b[Left] = idx_last++;
        geometry.add_vertex((Vec3f)b[Left].cast<float>(), (Vec3f)n_left.cast<float>());
        idx_b[Right] = idx_last++;
        geometry.add_vertex((Vec3f)b[Right].cast<float>(), (Vec3f)n_right.cast<float>());

        idx_prev = idx_b;
        n_right_prev = n_right;
        n_top_prev = n_top;
        unit_v_prev = unit_v;
        len_prev = len;

        if (!closed) {
            // Terminate open paths with caps.
            if (i == 0) {
                geometry.add_triangle(idx_a[Bottom], idx_a[Right], idx_a[Top]);
                geometry.add_triangle(idx_a[Bottom], idx_a[Top], idx_a[Left]);
            }

            // We don't use 'else' because both cases are true if we have only one line.
            if (i + 1 == lines.size()) {
                geometry.add_triangle(idx_b[Bottom], idx_b[Left], idx_b[Top]);
                geometry.add_triangle(idx_b[Bottom], idx_b[Top], idx_b[Right]);
            }
        }

        // Add quads for a straight hollow tube-like segment.
        // bottom-right face
        geometry.add_triangle(idx_a[Bottom], idx_b[Bottom], idx_b[Right]);
        geometry.add_triangle(idx_a[Bottom], idx_b[Right], idx_a[Right]);
        // top-right face
        geometry.add_triangle(idx_a[Right], idx_b[Right], idx_b[Top]);
        geometry.add_triangle(idx_a[Right], idx_b[Top], idx_a[Top]);
        // top-left face
        geometry.add_triangle(idx_a[Top], idx_b[Top], idx_b[Left]);
        geometry.add_triangle(idx_a[Top], idx_b[Left], idx_a[Left]);
        // bottom-left face
        geometry.add_triangle(idx_a[Left], idx_b[Left], idx_b[Bottom]);
        geometry.add_triangle(idx_a[Left], idx_b[Bottom], idx_a[Bottom]);
    }
}

void _3DScene::thick_lines_to_verts(
    const Lines&               lines,
    const std::vector<double>& widths,
    const std::vector<double>& heights,
    bool                       closed,
    double                     top_z,
    GUI::GLModel::Geometry&    geometry)
{
    thick_lines_to_geometry(lines, widths, heights, closed, top_z, geometry);
}

void _3DScene::thick_lines_to_verts(
    const Lines3&              lines,
    const std::vector<double>& widths,
    const std::vector<double>& heights,
    bool                       closed,
    GUI::GLModel::Geometry&    geometry)
{
    thick_lines_to_geometry(lines, widths, heights, closed, geometry);
}

// Fill in the qverts and tverts with quads and triangles for the extrusion_path.
void _3DScene::extrusionentity_to_verts(const ExtrusionPath& extrusion_path, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry)
{
    Polyline            polyline = extrusion_path.polyline;
    polyline.remove_duplicate_points();
    polyline.translate(copy);
    const Lines               lines = polyline.lines();
    std::vector<double> widths(lines.size(), extrusion_path.width);
    std::vector<double> heights(lines.size(), extrusion_path.height);
    thick_lines_to_verts(lines, widths, heights, false, print_z, geometry);
}

// Fill in the qverts and tverts with quads and triangles for the extrusion_loop.
void _3DScene::extrusionentity_to_verts(const ExtrusionLoop& extrusion_loop, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry)
{
    Lines               lines;
    std::vector<double> widths;
    std::vector<double> heights;
    for (const ExtrusionPath& extrusion_path : extrusion_loop.paths) {
        Polyline            polyline = extrusion_path.polyline;
        polyline.remove_duplicate_points();
        polyline.translate(copy);
        const Lines lines_this = polyline.lines();
        append(lines, lines_this);
        widths.insert(widths.end(), lines_this.size(), extrusion_path.width);
        heights.insert(heights.end(), lines_this.size(), extrusion_path.height);
    }
    thick_lines_to_verts(lines, widths, heights, true, print_z, geometry);
}

// Fill in the qverts and tverts with quads and triangles for the extrusion_multi_path.
void _3DScene::extrusionentity_to_verts(const ExtrusionMultiPath& extrusion_multi_path, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry)
{
    Lines               lines;
    std::vector<double> widths;
    std::vector<double> heights;
    for (const ExtrusionPath& extrusion_path : extrusion_multi_path.paths) {
        Polyline            polyline = extrusion_path.polyline;
        polyline.remove_duplicate_points();
        polyline.translate(copy);
        const Lines lines_this = polyline.lines();
        append(lines, lines_this);
        widths.insert(widths.end(), lines_this.size(), extrusion_path.width);
        heights.insert(heights.end(), lines_this.size(), extrusion_path.height);
    }
    thick_lines_to_verts(lines, widths, heights, false, print_z, geometry);
}

void _3DScene::extrusionentity_to_verts(const ExtrusionEntityCollection& extrusion_entity_collection, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry)
{
    for (const ExtrusionEntity* extrusion_entity : extrusion_entity_collection.entities)
        extrusionentity_to_verts(extrusion_entity, print_z, copy, geometry);
}

void _3DScene::extrusionentity_to_verts(const ExtrusionEntity* extrusion_entity, float print_z, const Point& copy, GUI::GLModel::Geometry& geometry)
{
    if (extrusion_entity != nullptr) {
        auto* extrusion_path = dynamic_cast<const ExtrusionPath*>(extrusion_entity);
        if (extrusion_path != nullptr)
            extrusionentity_to_verts(*extrusion_path, print_z, copy, geometry);
        else {
            auto* extrusion_loop = dynamic_cast<const ExtrusionLoop*>(extrusion_entity);
            if (extrusion_loop != nullptr)
                extrusionentity_to_verts(*extrusion_loop, print_z, copy, geometry);
            else {
                auto* extrusion_multi_path = dynamic_cast<const ExtrusionMultiPath*>(extrusion_entity);
                if (extrusion_multi_path != nullptr)
                    extrusionentity_to_verts(*extrusion_multi_path, print_z, copy, geometry);
                else {
                    auto* extrusion_entity_collection = dynamic_cast<const ExtrusionEntityCollection*>(extrusion_entity);
                    if (extrusion_entity_collection != nullptr)
                        extrusionentity_to_verts(*extrusion_entity_collection, print_z, copy, geometry);
                    else
                        throw Slic3r::RuntimeError("Unexpected extrusion_entity type in to_verts()");
                }
            }
        }
    }
}

} // namespace Slic3r
