#include "GLCanvas3D.hpp"

#include <glad/gl.h>
#include <imgui/imgui.h>
#include <Eigen/Core>
#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PathTracer.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Tesselate.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libvgcode/include/Types.hpp"

#include "3DScene.hpp"
#include "Camera.hpp"
#include "GCodeViewer.hpp"
#include "FrameProfiler.hpp"
#include "GLModel.hpp"
#include "GLShader.hpp"
#include "GLTexture.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ImGuiWrapper.hpp"
#include "OpenGLManager.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosManager.hpp"
#include "slic3r/GUI/MeshUtils.hpp"

namespace Slic3r {
namespace GUI {

namespace {

// Linear albedo of the bed where the path tracer sees it in reflections and indirect light; the
// bed itself is rasterized. Dark like the default plates, so that objects do not mirror a bright bed
// the viewer cannot see.
const std::array<float, 4> PLATE_ALBEDO = { 0.045f, 0.048f, 0.055f, 1.0f };

static const auto s_srgb_lut = []() {
    std::array<float, 256> lut;
    for (int i = 0; i < 256; ++i) {
        float c = float(i) / 255.0f;
        lut[size_t(i)] = (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
    }
    return lut;
}();

inline float srgb_to_linear(float c)
{
    int idx = std::clamp(int(std::lround(c * 255.0f)), 0, 255);
    return s_srgb_lut[size_t(idx)];
}

Vec4f to_linear_albedo(float r, float g, float b, float a = 1.0f)
{
    return Vec4f{srgb_to_linear(r), srgb_to_linear(g), srgb_to_linear(b), std::clamp(a, 0.0f, 1.0f)};
}

// Linear albedo of a filament color as the rasterizer shows it in Prepare, and libvgcode in Preview:
// near black lifted and a least opacity kept, see adjust_color_for_rendering().
Vec4f to_shown_albedo(const ColorRGBA& color, float alpha)
{
    const ColorRGBA shown = adjust_color_for_rendering(ColorRGBA(color.r(), color.g(), color.b(), alpha));
    return to_linear_albedo(shown.r(), shown.g(), shown.b(), shown.a());
}

Vec4f resolve_extruder_albedo(int extruder_id, const GLVolume* vol, const std::vector<ColorRGBA>& extruder_colors)
{
    float alpha = 1.0f;
    if (vol != nullptr)
        alpha = std::min(alpha, vol->color.a());
    if (!extruder_colors.empty() && extruder_id > 0) {
        const int idx = std::clamp(extruder_id - 1, 0, int(extruder_colors.size()) - 1);
        const ColorRGBA& c = extruder_colors[size_t(idx)];
        return to_shown_albedo(c, std::min(alpha, c.a()));
    }
    if (vol != nullptr)
        return to_shown_albedo(vol->color, alpha);
    if (!extruder_colors.empty()) {
        const ColorRGBA& c = extruder_colors.front();
        return to_shown_albedo(c, std::min(alpha, c.a()));
    }
    return to_linear_albedo(0.75f, 0.77f, 0.80f, alpha);
}

// Draws the [0, u] x [0, v] corner of a texture, its first row on top, over the whole viewport with
// the flat_texture shader, leaving the blend state set by the caller untouched. quad is the canvas'
// full-screen quad, texture coordinates [0, 1] from its bottom left corner: it is stretched so that
// the corner fills the viewport.
void render_pathtrace_fullscreen_texture(GLModel& quad, unsigned int tex_id, float u, float v)
{
    GLShaderProgram* shader = wxGetApp().get_shader("flat_texture");
    if (shader == nullptr || tex_id == 0 || !quad.is_initialized())
        return;

    Transform3d stretch = Transform3d::Identity();
    stretch.translate(Vec3d(1.0 / u - 1.0, 1.0 - 1.0 / v, 0.0));
    stretch.scale(Vec3d(1.0 / u, -1.0 / v, 1.0));

    // The vertical flip turns the quad's back to the viewer.
    const GLboolean cull_face = ::glIsEnabled(GL_CULL_FACE);
    glsafe(::glDisable(GL_CULL_FACE));
    glsafe(::glBindTexture(GL_TEXTURE_2D, (GLuint)tex_id));
    shader->start_using();
    shader->set_uniform("view_model_matrix", stretch);
    shader->set_uniform("projection_matrix", Transform3d::Identity());
    quad.render();
    shader->stop_using();
    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    if (cull_face)
        glsafe(::glEnable(GL_CULL_FACE));
}

// Triangles of a plate's surface, tagged as the bed: the path tracer only bounces light off it, the
// rasterizer draws it.
std::vector<PathTraceTriangle> plate_triangles_for_pathtrace(const PartPlate& plate)
{
    const ExPolygon plate_polygon(Polygon::new_scale(plate.get_shape()));
    const std::vector<Vec3d> plate_surface = triangulate_expolygon_3d(plate_polygon, -0.03f, NORMALS_UP);
    const Vec4f plate_marker{-2.0f, 0.42f, 0.48f, 1.0f};
    std::vector<PathTraceTriangle> triangles;
    triangles.reserve(plate_surface.size() / 3);
    for (size_t i = 0; i + 2 < plate_surface.size(); i += 3) {
        PathTraceTriangle triangle;
        triangle.a = plate_surface[i].cast<float>();
        triangle.b = plate_surface[i + 1].cast<float>();
        triangle.c = plate_surface[i + 2].cast<float>();
        triangle.albedo = plate_marker;
        triangles.push_back(std::move(triangle));
    }
    return triangles;
}

// Subsurface scattering family of a filament type: -1 for filled filaments (carbon, glass, wood,
// metal), which do not scatter, otherwise the sss mode the type scatters with, 0 if unknown.
int filament_sss_family(std::string filament_type)
{
    for (char& c : filament_type)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto contains_any = [&filament_type](std::initializer_list<const char*> tokens) {
        for (const char* t : tokens) {
            if (filament_type.find(t) != std::string::npos)
                return true;
        }
        return false;
    };
    if (contains_any({"CF", "GF", "CARBON", "GLASS", "WOOD", "METAL"}))
        return -1;
    if (contains_any({"PA", "NYLON", "PP", "PEBA", "WAX"}))
        return 3;
    if (contains_any({"PET", "PCT", "TPU", "TPE", "PVA", "BVOH", "PC", "PMMA"}))
        return 2;
    if (contains_any({"PLA", "PHA"}))
        return 1;
    return 0;
}

// Subsurface scattering family of each extruder's filament. Reads the preset bundle, so it must run
// on the GUI thread, once per scene build or color update rather than per triangle.
std::vector<int> build_extruder_sss_families()
{
    std::vector<int> families;
    if (const PresetBundle* bundle = wxGetApp().preset_bundle) {
        families.reserve(bundle->filament_presets.size());
        for (const std::string& preset_name : bundle->filament_presets) {
            const Preset* preset = bundle->filaments.find_preset(preset_name, true);
            const std::string type = preset != nullptr ? preset->config.get_filament_type() : std::string();
            families.push_back(filament_sss_family(type.empty() ? "PLA" : type));
        }
    }
    return families;
}

// SSS mode of a triangle: its extruder's filament family, unless the color is near black, which
// scatters nothing visible, or nearly opaque translucent, which scatters strongly.
int sss_mode_for_extruder(const std::vector<int>& families, size_t extruder_idx, float opacity, const Vec3f& linear_rgb)
{
    // An extruder without a preset falls back to PLA.
    const int family = extruder_idx < families.size() ? families[extruder_idx] : 1;
    const float luminance = 0.2126f * linear_rgb.x() + 0.7152f * linear_rgb.y() + 0.0722f * linear_rgb.z();
    if (family < 0 || luminance < 0.05f)
        return 0;
    if (opacity < 0.98f && opacity >= 0.85f)
        return 3;
    return family;
}

} // namespace

void GLCanvas3D::_stop_pathtrace_bvh_worker()
{
    if (m_pathtrace_bvh_cancel_token)
        m_pathtrace_bvh_cancel_token->store(true);
    if (m_pathtrace_bvh_future.valid())
        m_pathtrace_bvh_future.wait();
}

void GLCanvas3D::_release_pathtrace_gl_resources()
{
    if (m_pathtrace_gpu_node_buffer != 0 || m_pathtrace_gpu_triangle_buffer != 0 ||
        m_pathtrace_gpu_shading_buffer != 0 || m_pathtrace_gpu_instance_buffer != 0 ||
        m_pathtrace_gpu_tlas_buffer != 0 || m_pathtrace_gpu_normal_buffer != 0) {
        const unsigned int buffers[] = {
            m_pathtrace_gpu_node_buffer,
            m_pathtrace_gpu_triangle_buffer,
            m_pathtrace_gpu_shading_buffer,
            m_pathtrace_gpu_instance_buffer,
            m_pathtrace_gpu_tlas_buffer,
            m_pathtrace_gpu_normal_buffer
        };
        glsafe(::glDeleteBuffers(6, buffers));
        m_pathtrace_gpu_normal_buffer = 0;
        m_pathtrace_gpu_node_buffer = 0;
        m_pathtrace_gpu_triangle_buffer = 0;
        m_pathtrace_gpu_shading_buffer = 0;
        m_pathtrace_gpu_instance_buffer = 0;
        m_pathtrace_gpu_tlas_buffer = 0;
    }
    if (m_pathtrace_gpu_accumulation_image != 0) {
        glsafe(::glDeleteTextures(1, &m_pathtrace_gpu_accumulation_image));
        m_pathtrace_gpu_accumulation_image = 0;
    }
    if (m_pathtrace_gpu_display_image != 0) {
        glsafe(::glDeleteTextures(1, &m_pathtrace_gpu_display_image));
        m_pathtrace_gpu_display_image = 0;
    }
    if (m_pathtrace_gpu_trans_accumulation_image != 0) {
        glsafe(::glDeleteTextures(1, &m_pathtrace_gpu_trans_accumulation_image));
        m_pathtrace_gpu_trans_accumulation_image = 0;
    }
    if (m_pathtrace_gpu_trans_display_image != 0) {
        glsafe(::glDeleteTextures(1, &m_pathtrace_gpu_trans_display_image));
        m_pathtrace_gpu_trans_display_image = 0;
    }
    for (unsigned int* tex : { &m_pathtrace_gpu_aux_normal_depth_image, &m_pathtrace_gpu_aux_albedo_image, &m_pathtrace_gpu_moment_image,
                               &m_pathtrace_gpu_denoise_images[0], &m_pathtrace_gpu_denoise_images[1],
                               &m_pathtrace_gpu_denoise_images[2], &m_pathtrace_gpu_denoise_images[3],
                               &m_pathtrace_gpu_interactive_display_image, &m_pathtrace_gpu_interactive_trans_display_image,
                               &m_pathtrace_gpu_history_images[0], &m_pathtrace_gpu_history_images[1], &m_pathtrace_gpu_history_images[2] }) {
        if (*tex != 0) {
            glsafe(::glDeleteTextures(1, tex));
            *tex = 0;
        }
    }
    m_pathtrace_gpu_image_size = {0, 0};
    m_pathtrace_gpu_interactive_size = {0, 0};
    m_pathtrace_history.valid = false;
    if (m_pathtrace_gpu_timer_queries[0] != 0) {
        glsafe(::glDeleteQueries(GLsizei(m_pathtrace_gpu_timer_queries.size()), m_pathtrace_gpu_timer_queries.data()));
        m_pathtrace_gpu_timer_queries = {0, 0, 0, 0};
        m_pathtrace_gpu_timer_query_in_flight[0] = false;
        m_pathtrace_gpu_timer_query_in_flight[1] = false;
    }
}

void GLCanvas3D::_reset_pathtrace_accumulation()
{
    m_pathtrace_gpu_samples = 0;
    m_pathtrace_tile_cursor = 0;
    m_pathtrace_last_dispatched_tiles = 0;
}

bool GLCanvas3D::_is_pathtrace_enabled() const
{
    if (!wxGetApp().is_gl_version_greater_or_equal_to(4, 3))
        return false;
    const GLGizmosManager::EType gizmo = m_gizmos.get_current_type();
    if (gizmo == GLGizmosManager::Seam ||
        gizmo == GLGizmosManager::FdmSupports ||
        gizmo == GLGizmosManager::FuzzySkin ||
        gizmo == GLGizmosManager::MmSegmentation)
        return false;
    if (!_is_realistic_view_enabled())
        return false;
    const AppConfig* cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return false;
    return cfg->get(SETTING_OPENGL_SHADING_MODEL) == "pathtrace";
}

void GLCanvas3D::_render_pathtrace_fps_overlay(ImGuiWrapper& imgui) const
{
    if (m_pathtrace_gpu_display_image != 0) {
        const uint32_t display_spp = (m_pathtrace_gpu_samples <= 1) ? m_pathtrace_gpu_samples : (m_pathtrace_gpu_samples - 1);
        if (m_pathtrace_gpu_samples >= m_pathtrace_max_samples + 1u) {
            imgui.text(std::string("PT: GPU [Converged: ") + std::to_string(m_pathtrace_max_samples) + " spp]");
        } else if (m_pathtrace_is_interactive) {
            imgui.text(std::string("PT: GPU [Interactive: 1 spp]"));
        } else {
            imgui.text(std::string("PT: GPU [") + std::to_string(display_spp) + " / " + std::to_string(m_pathtrace_max_samples) + " spp]");
        }
        if (m_pathtrace_gpu_dispatch_full_res_ms > 0.0f || m_pathtrace_gpu_dispatch_interactive_ms > 0.0f) {
            char time_buf[128];
            snprintf(time_buf, sizeof(time_buf), "PT GPU: Full %.2f ms | Orbit %.2f ms",
                     m_pathtrace_gpu_dispatch_full_res_ms > 0.0f ? m_pathtrace_gpu_dispatch_full_res_ms : m_pathtrace_gpu_dispatch_time_ms,
                     m_pathtrace_gpu_dispatch_interactive_ms);
            imgui.text(std::string(time_buf));
            // Keep the width of the widest times, so that the overlay does not resize as they change.
            const float reserved = ImGui::CalcTextSize("PT GPU: Full 000.00 ms | Orbit 000.00 ms").x;
            const float used = ImGui::CalcTextSize(time_buf).x;
            if (used < reserved) {
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::Dummy(ImVec2(reserved - used, 0.0f));
            }
        }
    } else {
        imgui.text(std::string("PT: GPU [Initializing]"));
    }
}

GLCanvas3D::PathTraceSceneBuildResult GLCanvas3D::_build_pathtrace_scene_async(
    PathTraceBvhJob job,
    std::shared_ptr<std::atomic<bool>> cancel_token)
{
    PathTraceSceneBuildResult result;
    result.geom_signature = job.geom_signature;

    if (cancel_token && cancel_token->load(std::memory_order_relaxed))
        return {};

    if (job.is_preview) {
        if (job.preview_triangles.empty())
            return result;

        result.gpu_scene = build_path_trace_gpu_scene(job.preview_triangles, cancel_token.get());
        job.preview_triangles.clear();
        job.preview_triangles.shrink_to_fit();
        // Toolpaths shade with their own bead normals.
        result.gpu_scene.normals.clear();
        result.gpu_scene.normals.shrink_to_fit();

        if (cancel_token && cancel_token->load(std::memory_order_relaxed))
            return {};

        if (!job.source_tri_vertex_id.empty() && !result.gpu_scene.original_indices.empty()) {
            result.gpu_tri_vertex_id.resize(result.gpu_scene.triangles.size(), SIZE_MAX);
            for (size_t i = 0; i < result.gpu_scene.triangles.size(); ++i) {
                const size_t src_idx = size_t(result.gpu_scene.original_indices[i]);
                if (src_idx < job.source_tri_vertex_id.size()) {
                    result.gpu_tri_vertex_id[i] = job.source_tri_vertex_id[src_idx];
                    result.gpu_scene.triangles[i].a[3] = job.source_tri_tag_z[src_idx];
                    const uint32_t v_id = (job.source_tri_vertex_id[src_idx] == SIZE_MAX)
                        ? UINT32_MAX
                        : uint32_t(job.source_tri_vertex_id[src_idx]);
                    float v_id_f;
                    std::memcpy(&v_id_f, &v_id, sizeof(float));
                    result.gpu_scene.triangles[i].b[3] = v_id_f;
                    const float tri_opacity = (src_idx < job.source_tri_opacity.size()) ? job.source_tri_opacity[src_idx] : 1.0f;
                    result.gpu_scene.triangles[i].c[3] = tri_opacity;
                    result.gpu_scene.shadings[i].params[0] = job.source_tri_height[src_idx];
                    result.gpu_scene.shadings[i].params[1] = job.source_tri_role[src_idx];
                    result.gpu_scene.shadings[i].params[2] = (src_idx < job.source_tri_facet.size()) ? job.source_tri_facet[src_idx] : 0.0f;
                    result.gpu_scene.shadings[i].params[3] = (src_idx < job.source_tri_delta_angle.size()) ? job.source_tri_delta_angle[src_idx] : 0.0f;
                    if (job.source_tri_vertex_id[src_idx] == SIZE_MAX) {
                        result.gpu_scene.shadings[i].albedo = PLATE_ALBEDO;
                    } else {
                        result.gpu_scene.shadings[i].albedo[3] = tri_opacity;
                    }
                }
            }
        }

        if (!result.gpu_scene.nodes.empty()) {
            PathTraceVolumeBlas blas;
            blas.root_node = 0;
            blas.node_count = uint32_t(result.gpu_scene.nodes.size());
            blas.tri_offset = 0;
            blas.tri_count = uint32_t(result.gpu_scene.triangles.size());
            blas.local_bounds_min = Vec3f(result.gpu_scene.nodes[0].bounds_min[0], result.gpu_scene.nodes[0].bounds_min[1], result.gpu_scene.nodes[0].bounds_min[2]);
            blas.local_bounds_max = Vec3f(result.gpu_scene.nodes[0].bounds_max[0], result.gpu_scene.nodes[0].bounds_max[1], result.gpu_scene.nodes[0].bounds_max[2]);
            blas.object_idx = -1;
            blas.instance_idx = -1;
            blas.volume_idx = -1;
            blas.plate_idx = -1;
            result.volume_blas.push_back(blas);
        }
    } else {
        for (auto& grp_data : job.volume_groups) {
            if (cancel_token && cancel_token->load(std::memory_order_relaxed))
                return {};
            if (grp_data.triangles.empty())
                continue;

            PathTraceGpuScene local_scene = build_path_trace_gpu_scene(grp_data.triangles, cancel_token.get());
            grp_data.triangles.clear();
            grp_data.triangles.shrink_to_fit();
            if (local_scene.nodes.empty() || local_scene.triangles.empty())
                continue;

            const uint32_t cur_node_offset = uint32_t(result.gpu_scene.nodes.size());
            const uint32_t cur_tri_offset = uint32_t(result.gpu_scene.triangles.size());

            for (auto& node : local_scene.nodes) {
                if (node.is_leaf()) {
                    node.left_or_first += int32_t(cur_tri_offset);
                } else if (node.left_or_first >= 0) {
                    node.left_or_first += int32_t(cur_node_offset);
                }
            }
            for (auto& tri : local_scene.triangles) {
                tri.a[3] = -1.0f;
                float invalid_v_id_f;
                const uint32_t invalid_v_id = UINT32_MAX;
                std::memcpy(&invalid_v_id_f, &invalid_v_id, sizeof(float));
                tri.b[3] = invalid_v_id_f;
                tri.c[3] = std::clamp(tri.c[3], 0.0f, 1.0f); // preserve opacity from albedo
            }

            PathTraceVolumeBlas blas;
            blas.root_node = cur_node_offset;
            blas.node_count = uint32_t(local_scene.nodes.size());
            blas.tri_offset = cur_tri_offset;
            blas.tri_count = uint32_t(local_scene.triangles.size());
            blas.local_bounds_min = Vec3f(local_scene.nodes[0].bounds_min[0], local_scene.nodes[0].bounds_min[1], local_scene.nodes[0].bounds_min[2]);
            blas.local_bounds_max = Vec3f(local_scene.nodes[0].bounds_max[0], local_scene.nodes[0].bounds_max[1], local_scene.nodes[0].bounds_max[2]);
            blas.object_idx = grp_data.object_idx;
            blas.instance_idx = grp_data.instance_idx;
            blas.volume_idx = grp_data.volume_idx;
            blas.plate_idx = -1;
            blas.is_wipe_tower = grp_data.is_wipe_tower;
            blas.wipe_tower_plate_idx = grp_data.wipe_tower_plate_idx;
            result.volume_blas.push_back(blas);

            result.gpu_scene.nodes.insert(result.gpu_scene.nodes.end(), local_scene.nodes.begin(), local_scene.nodes.end());
            result.gpu_scene.triangles.insert(result.gpu_scene.triangles.end(), local_scene.triangles.begin(), local_scene.triangles.end());
            result.gpu_scene.shadings.insert(result.gpu_scene.shadings.end(), local_scene.shadings.begin(), local_scene.shadings.end());
            result.gpu_scene.normals.insert(result.gpu_scene.normals.end(), local_scene.normals.begin(), local_scene.normals.end());

            for (size_t i = 0; i < local_scene.triangles.size(); ++i) {
                const size_t orig_idx = size_t(local_scene.original_indices[i]);
                const size_t ext_id = (orig_idx < grp_data.triangle_extruder_ids.size()) ? grp_data.triangle_extruder_ids[orig_idx] : 0;
                result.gpu_tri_vertex_id.push_back(ext_id);
                const size_t sh_idx = cur_tri_offset + i;
                const size_t ext_idx = (ext_id > 0) ? (ext_id - 1) : 0;
                const Vec3f rgb{result.gpu_scene.shadings[sh_idx].albedo[0], result.gpu_scene.shadings[sh_idx].albedo[1], result.gpu_scene.shadings[sh_idx].albedo[2]};
                const float op = result.gpu_scene.shadings[sh_idx].albedo[3];
                const int sss_mode = sss_mode_for_extruder(job.extruder_sss_families, ext_idx, op, rgb);
                const float roughness = 0.35f;
                result.gpu_scene.shadings[sh_idx].params[1] = roughness + float(sss_mode) * 2.0f;
            }
        }
    }

    for (auto& plate_data : job.plate_groups) {
        if (cancel_token && cancel_token->load(std::memory_order_relaxed))
            return {};
        if (plate_data.triangles.empty())
            continue;

        PathTraceGpuScene plate_scene = build_path_trace_gpu_scene(plate_data.triangles, cancel_token.get());
        plate_data.triangles.clear();
        plate_data.triangles.shrink_to_fit();
        if (plate_scene.nodes.empty() || plate_scene.triangles.empty())
            continue;

        const uint32_t cur_node_offset = uint32_t(result.gpu_scene.nodes.size());
        const uint32_t cur_tri_offset = uint32_t(result.gpu_scene.triangles.size());
        for (auto& node : plate_scene.nodes) {
            if (node.is_leaf()) {
                node.left_or_first += int32_t(cur_tri_offset);
            } else if (node.left_or_first >= 0) {
                node.left_or_first += int32_t(cur_node_offset);
            }
        }
        for (auto& tri : plate_scene.triangles) {
            tri.a[3] = -2.0f;
            float invalid_v_id_f;
            const uint32_t invalid_v_id = UINT32_MAX;
            std::memcpy(&invalid_v_id_f, &invalid_v_id, sizeof(float));
            tri.b[3] = invalid_v_id_f;
            tri.c[3] = 1.0f;
        }
        for (auto& sh : plate_scene.shadings) {
            sh.albedo = PLATE_ALBEDO;
            sh.params[1] = 0.35f;
        }
        PathTraceVolumeBlas blas;
        blas.root_node = cur_node_offset;
        blas.node_count = uint32_t(plate_scene.nodes.size());
        blas.tri_offset = cur_tri_offset;
        blas.tri_count = uint32_t(plate_scene.triangles.size());
        blas.local_bounds_min = Vec3f(plate_scene.nodes[0].bounds_min[0], plate_scene.nodes[0].bounds_min[1], plate_scene.nodes[0].bounds_min[2]);
        blas.local_bounds_max = Vec3f(plate_scene.nodes[0].bounds_max[0], plate_scene.nodes[0].bounds_max[1], plate_scene.nodes[0].bounds_max[2]);
        blas.object_idx = -1;
        blas.instance_idx = -1;
        blas.volume_idx = -1;
        blas.plate_idx = plate_data.plate_idx;
        result.volume_blas.push_back(blas);

        result.gpu_scene.nodes.insert(result.gpu_scene.nodes.end(), plate_scene.nodes.begin(), plate_scene.nodes.end());
        result.gpu_scene.triangles.insert(result.gpu_scene.triangles.end(), plate_scene.triangles.begin(), plate_scene.triangles.end());
        result.gpu_scene.shadings.insert(result.gpu_scene.shadings.end(), plate_scene.shadings.begin(), plate_scene.shadings.end());
        if (!job.is_preview)
            result.gpu_scene.normals.insert(result.gpu_scene.normals.end(), plate_scene.normals.begin(), plate_scene.normals.end());
        result.gpu_tri_vertex_id.resize(result.gpu_scene.triangles.size(), SIZE_MAX);
    }

    return result;
}

void GLCanvas3D::_extract_preview_toolpaths_for_pathtrace(
    PathTraceBvhJob& job,
    int cfg_layer_profile,
    bool plate_visible,
    PartPlate* current_plate) const
{
    if (m_gcode_viewer.has_data() && m_gcode_viewer.get_vertices_count() > 1) {
        struct ToolpathSegment
        {
            Vec3f p0;
            Vec3f p1;
            float width;
            float height;
            size_t vertex_id;
            libvgcode::EGCodeExtrusionRole role;
            uint16_t extruder_id{0};
        };
        const libvgcode::EViewType view_type = m_gcode_viewer.get_view_type();
        const bool check_speed = (view_type == libvgcode::EViewType::Speed || view_type == libvgcode::EViewType::ActualSpeed);
        const bool check_temp  = (view_type == libvgcode::EViewType::Temperature);
        const bool check_fan   = (view_type == libvgcode::EViewType::FanSpeed);
        std::vector<ToolpathSegment> segments;
        const size_t total_vertices = m_gcode_viewer.get_vertices_count();
        segments.reserve(std::min<size_t>(500000, total_vertices * 2));
        for (size_t i = 0; i + 1 < total_vertices; ++i) {
            const libvgcode::PathVertex& curr = m_gcode_viewer.get_gcode_vertex_at(i);
            const libvgcode::PathVertex& next = m_gcode_viewer.get_gcode_vertex_at(i + 1);
            if (!curr.is_extrusion() || !next.is_extrusion() || next.width <= 0.0f || next.height <= 0.0f)
                continue;
            if (!m_gcode_viewer.is_extrusion_role_visible(next.role))
                continue;
            const float z_shift = 0.5f * next.height;
            const Vec3f p0{curr.position[0], curr.position[1], curr.position[2] - z_shift};
            const Vec3f p1{next.position[0], next.position[1], next.position[2] - z_shift};
            const Vec3f delta = p1 - p0;
            const float curr_len_sq = delta.squaredNorm();
            if (curr_len_sq < 1e-8f)
                continue;
            const float curr_len = std::sqrt(curr_len_sq);

            // Adaptive diagonal sub-splitting: 45-degree infill lines suffer severe AABB inflation,
            // causing large overlapping bounding boxes in the BVH. Limiting diagonal span length to 4-5mm
            // collapses empty AABB volume by up to 20x while leaving axis-aligned segments at up to 20mm.
            const float diag_factor = (curr_len_sq > 1e-6f)
                ? std::clamp(2.0f * std::abs(delta.x() * delta.y()) / curr_len_sq, 0.0f, 1.0f)
                : 0.0f;
            const float seg_max_len = 20.0f + (4.0f - 20.0f) * diag_factor;

            if (!segments.empty() && curr_len < seg_max_len) {
                ToolpathSegment& prev = segments.back();
                const libvgcode::PathVertex& prev_v = m_gcode_viewer.get_gcode_vertex_at(prev.vertex_id);
                if (prev_v.extruder_id == next.extruder_id &&
                    prev.role == next.role &&
                    std::abs(prev.width - next.width) < 1e-3f &&
                    std::abs(prev.height - next.height) < 1e-3f &&
                    (!check_speed || std::abs(prev_v.feedrate - next.feedrate) < 0.5f) &&
                    (!check_temp || std::abs(prev_v.temperature - next.temperature) < 0.5f) &&
                    (!check_fan || std::abs(prev_v.fan_speed - next.fan_speed) < 0.5f) &&
                    (prev.p1 - p0).squaredNorm() < 1e-6f) {
                    const Vec3f prev_vec = prev.p1 - prev.p0;
                    const float prev_len = prev_vec.norm();
                    if (prev_len > 1e-5f && (prev_len + curr_len) <= seg_max_len) {
                        const Vec3f prev_dir = prev_vec / prev_len;
                        const Vec3f curr_dir = delta / curr_len;
                        if (prev_dir.dot(curr_dir) > 0.99995f) {
                            const Vec3f total_dir = (p1 - prev.p0).normalized();
                            const float perp_dist_sq = ((prev.p1 - prev.p0) - (prev.p1 - prev.p0).dot(total_dir) * total_dir).squaredNorm();
                            if (perp_dist_sq < 1e-6f) {
                                prev.p1 = p1;
                                continue;
                            }
                        }
                    }
                }
            }
            if (curr_len > seg_max_len) {
                const int pieces = std::min(64, int(std::ceil(curr_len / seg_max_len)));
                const Vec3f step = delta * (1.0f / float(pieces));
                Vec3f sub_p0 = p0;
                for (int p = 0; p < pieces; ++p) {
                    const Vec3f sub_p1 = (p + 1 == pieces) ? p1 : (p0 + step * float(p + 1));
                    segments.push_back(ToolpathSegment{sub_p0, sub_p1, next.width, next.height, i + 1, next.role, uint16_t(next.extruder_id)});
                    sub_p0 = sub_p1;
                }
            } else {
                segments.push_back(ToolpathSegment{p0, p1, next.width, next.height, i + 1, next.role, uint16_t(next.extruder_id)});
            }
        }
        const size_t quads_per_seg = 4;
        job.preview_triangles.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_vertex_id.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_tag_z.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_height.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_role.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_facet.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_delta_angle.reserve(segments.size() * quads_per_seg * 2);
        job.source_tri_opacity.reserve(segments.size() * quads_per_seg * 2);

        const size_t num_segs = segments.size();
        std::vector<Vec3f> seg_dirs(num_segs, Vec3f{0.0f, 1.0f, 0.0f});
        std::vector<Vec3f> seg_rights(num_segs, Vec3f{1.0f, 0.0f, 0.0f});
        for (size_t s = 0; s < num_segs; ++s) {
            Vec3f d = segments[s].p1 - segments[s].p0;
            const float d_len = d.norm();
            if (d_len > 1e-5f)
                d /= d_len;
            else
                d = Vec3f{0.0f, 1.0f, 0.0f};
            seg_dirs[s] = d;
            Vec3f r = d.cross(Vec3f{0.0f, 0.0f, 1.0f});
            const float r_len = r.norm();
            if (r_len > 1e-4f)
                r /= r_len;
            else
                r = Vec3f{1.0f, 0.0f, 0.0f};
            seg_rights[s] = r;
        }

        std::vector<bool> has_joint(num_segs, false);
        std::vector<Vec3f> joint_w(num_segs, Vec3f{0.0f, 0.0f, 0.0f});
        std::vector<float> joint_delta_angle(num_segs, 0.0f);

        for (size_t s = 0; s + 1 < num_segs; ++s) {
            const ToolpathSegment& cur = segments[s];
            const ToolpathSegment& nxt = segments[s + 1];
            if (cur.role == nxt.role &&
                std::abs(cur.p1.z() - nxt.p0.z()) < 1e-3f &&
                (cur.p1 - nxt.p0).squaredNorm() < 1e-5f) {
                const float dot = seg_dirs[s].dot(seg_dirs[s + 1]);
                if (dot > -0.707f) {
                    has_joint[s] = true;
                    Vec3f miter = seg_rights[s] + seg_rights[s + 1];
                    const float m_len = miter.norm();
                    const float half_w = std::max(0.01f, cur.width * 0.5f);
                    if (m_len > 1e-4f) {
                        miter /= m_len;
                        const float cos_half = miter.dot(seg_rights[s]);
                        const float scale = (cos_half > 0.5f) ? std::min(1.4142f, 1.0f / cos_half) : 1.4142f;
                        joint_w[s] = miter * (half_w * scale);
                    } else {
                        joint_w[s] = seg_rights[s] * half_w;
                    }
                    if (dot > 0.866f) { // gentle curve <= 30 deg
                        const float cross_z = seg_dirs[s].x() * seg_dirs[s + 1].y() - seg_dirs[s].y() * seg_dirs[s + 1].x();
                        joint_delta_angle[s] = std::atan2(cross_z, dot);
                    }
                }
            }
        }

        for (size_t s = 0; s < num_segs; ++s) {
            const ToolpathSegment& seg = segments[s];
            const Vec3f& dir = seg_dirs[s];
            const Vec3f up = seg_rights[s].cross(dir);

            const float half_w = std::max(0.01f, seg.width * 0.5f);
            const float half_h = std::max(0.01f, seg.height * 0.5f);

            const bool conn_prev = (s > 0 && has_joint[s - 1]);
            const bool conn_next = (s + 1 < num_segs && has_joint[s]);

            const Vec3f w0 = conn_prev ? joint_w[s - 1] : (seg_rights[s] * half_w);
            const Vec3f w1 = conn_next ? joint_w[s]     : (seg_rights[s] * half_w);
            const Vec3f h0 = up * half_h;
            const Vec3f h1 = h0;

            const Vec3f r0 = seg.p0 + w0;
            const Vec3f r1 = seg.p1 + w1;
            const Vec3f l0 = seg.p0 - w0;
            const Vec3f l1 = seg.p1 - w1;
            const Vec3f t0 = seg.p0 + h0;
            const Vec3f t1 = seg.p1 + h1;
            const Vec3f b0 = seg.p0 - h0;
            const Vec3f b1 = seg.p1 - h1;

            const int r = int(seg.role);
            const bool is_external = (r == 2 || r == 3 || r == 10 || r == 11 || r == 12 || r == 13 || r == 14 || r == 17 || r == 18);

            float delta_angle = 0.0f;
            if (is_external) {
                if (conn_prev && conn_next)
                    delta_angle = 0.5f * (joint_delta_angle[s - 1] + joint_delta_angle[s]);
                else if (conn_prev)
                    delta_angle = joint_delta_angle[s - 1];
                else if (conn_next)
                    delta_angle = joint_delta_angle[s];
            }

            const Vec4f def_lin = to_linear_albedo(0.75f, 0.77f, 0.80f, 1.0f);
            float seg_opacity = 1.0f;
            Vec3f seg_rgb{def_lin.x(), def_lin.y(), def_lin.z()};
            if (seg.vertex_id != SIZE_MAX) {
                const libvgcode::PathVertex& v = m_gcode_viewer.get_gcode_vertex_at(seg.vertex_id);
                if (v.is_option()) {
                    const ColorRGBA opt_col = m_gcode_viewer.get_option_color(libvgcode::move_type_to_option(v.type));
                    const Vec4f lin = to_linear_albedo(opt_col.r(), opt_col.g(), opt_col.b(), 1.0f);
                    seg_rgb = {lin.x(), lin.y(), lin.z()};
                    seg_opacity = 1.0f;
                } else {
                    const libvgcode::Color c = m_gcode_viewer.get_gcode_vertex_color(v);
                    const uint8_t a = m_gcode_viewer.get_gcode_vertex_alpha(v);
                    const Vec4f lin = to_linear_albedo(float(c[0]) / 255.0f, float(c[1]) / 255.0f, float(c[2]) / 255.0f, float(a) / 255.0f);
                    seg_rgb = {lin.x(), lin.y(), lin.z()};
                    seg_opacity = lin.w();
                }
            }

            const float tag_z = seg.p0.z() + half_h;
            const float roughness = 0.35f;
            const size_t ext_idx = (seg.extruder_id > 0) ? size_t(seg.extruder_id - 1) : 0;
            const int sss_mode = sss_mode_for_extruder(job.extruder_sss_families, ext_idx, seg_opacity, seg_rgb);
            const float encoded_param1 = roughness + float(sss_mode) * 2.0f;

            const auto add_tri = [&](const Vec3f& a, const Vec3f& b, const Vec3f& c, float facet_kind, float cur_delta_angle) {
                PathTraceTriangle t;
                t.a = a; t.b = b; t.c = c;
                t.albedo = Vec4f{seg_rgb.x(), seg_rgb.y(), seg_rgb.z(), seg_opacity};
                job.preview_triangles.push_back(std::move(t));
                job.source_tri_vertex_id.push_back(seg.vertex_id);
                job.source_tri_tag_z.push_back(tag_z);
                job.source_tri_height.push_back(seg.height);
                job.source_tri_role.push_back(encoded_param1);
                job.source_tri_facet.push_back(facet_kind);
                job.source_tri_delta_angle.push_back(cur_delta_angle);
                job.source_tri_opacity.push_back(seg_opacity);
            };
            // The second triangle of a quad carries a quarter on its facet kind, so that the shader
            // tells its corners apart whatever order the BVH puts the triangles in.
            const auto add_quad = [&](const Vec3f& p00, const Vec3f& p10, const Vec3f& p11, const Vec3f& p01, float facet_kind, float cur_delta_angle = 0.0f) {
                add_tri(p00, p10, p11, facet_kind, cur_delta_angle);
                add_tri(p00, p11, p01, facet_kind + 0.25f, cur_delta_angle);
            };
            // Rounded end of a flat bead: a half disk fanned around its end point c, from c + w over
            // c + out to c - w, tagged with half on its facet kind for the shader to take the normal
            // from the radius.
            const auto add_cap = [&](const Vec3f& c, const Vec3f& w, const Vec3f& out, float facet_kind = 0.5f) {
                const Vec3f q1 = c + 0.5f * w + 0.8660254f * out;
                const Vec3f q2 = c - 0.5f * w + 0.8660254f * out;
                add_tri(c, c + w, q1, facet_kind, 0.0f);
                add_tri(c, q1, q2, facet_kind, 0.0f);
                add_tri(c, q2, c - w, facet_kind, 0.0f);
            };

            if (is_external) {
                add_quad(r0, r1, t1, t0, 1.0f, delta_angle);
                add_quad(t0, t1, l1, l0, 0.0f);
                add_quad(l0, l1, b1, b0, 2.0f);
                add_quad(b0, b1, r1, r0, 2.0f, delta_angle);
                if (!conn_prev) add_quad(r0, t0, l0, b0, 2.0f);
                if (!conn_next) add_quad(t1, r1, b1, l1, 2.0f);
            } else if (r == 4) {
                // Sparse internal infill: upper roof bevels visible from top and side (4 tris)
                add_quad(r0, r1, t1, t0, 2.0f);
                add_quad(t0, t1, l1, l0, 2.0f);
            } else if (r == 1 || r == 19) {
                // Inner perimeters: upper bevels forming top deck and side walls (4 tris)
                add_quad(r0, r1, t1, t0, 0.0f);
                add_quad(t0, t1, l1, l0, 0.0f);
            } else {
                // Solid infill, top/bottom decks, bridge, gap fill: horizontal ribbon (2 tris), at the
                // top of the layer like the crests of the walls around it, the segment running at mid height.
                // Only gentle bends are mitered: a miter skews the profile the shader rebuilds across
                // the ribbon. Ends and sharper turns are rounded, a half disk reaching half the width
                // past the point, so that a turn is the union of two rounded beads. Where those
                // overlap in one plane, a ray would hit either surface at random, each with its own
                // normal, and leave speckles: the ends lie a hair below the ribbons, and every other
                // ribbon a hair below its neighbors, so that one surface wins throughout.
                // Ironing smooths the top surface with lines that overlap each other many times: its
                // ribbons shade flat, carrying three quarters on their facet kind, and lie a hair above
                // the top surface they iron.
                const bool ironing = r == 7;
                const bool miter_prev = conn_prev && seg_dirs[s - 1].dot(dir) > 0.866f;
                const bool miter_next = conn_next && dir.dot(seg_dirs[s + 1]) > 0.866f;
                const Vec3f straight = seg_rights[s] * half_w;
                const Vec3f m0 = miter_prev ? w0 : straight;
                const Vec3f m1 = miter_next ? w1 : straight;
                const Vec3f lift = up * (half_h - ((s & 1) ? 1e-4f : 0.0f) + (ironing ? 1e-3f : 0.0f));
                const Vec3f p0 = seg.p0 + lift;
                const Vec3f p1 = seg.p1 + lift;
                if (ironing) {
                    add_tri(p0 + m0, p1 + m1, p1 - m1, 0.75f, 0.0f);
                    add_tri(p0 + m0, p1 - m1, p0 - m0, 0.75f, 0.0f);
                } else {
                    add_quad(p0 + m0, p1 + m1, p1 - m1, p0 - m0, 0.0f);
                }
                const Vec3f below = up * 2e-4f;
                const float cap_kind = ironing ? 0.75f : 0.5f;
                if (!miter_prev) add_cap(p0 - below, straight, -half_w * dir, cap_kind);
                if (!miter_next) add_cap(p1 - below, straight, half_w * dir, cap_kind);
            }
        }

        // Add option markers (seams) into the BVH scene so they are physically path traced
        if (m_gcode_viewer.is_option_visible(libvgcode::EOptionType::Seams)) {
            const ColorRGBA seam_col = m_gcode_viewer.get_option_color(libvgcode::EOptionType::Seams);
            const Vec4f seam_albedo = to_linear_albedo(seam_col.r(), seam_col.g(), seam_col.b(), 1.0f);

            for (size_t i = 0; i < total_vertices; ++i) {
                const libvgcode::PathVertex& v = m_gcode_viewer.get_gcode_vertex_at(i);
                if (v.type != libvgcode::EMoveType::Seam)
                    continue;

                const float h = (v.height > 0.01f ? v.height : 0.20f);
                const float w = (v.width > 0.01f ? v.width : 0.45f);
                const float half_h = 0.75f * h;
                const float r = 0.75f * w;
                const Vec3f center{v.position[0], v.position[1], v.position[2] - 0.5f * h};
                const Vec3f top = center + Vec3f(0.0f, 0.0f, half_h);
                const Vec3f bot = center - Vec3f(0.0f, 0.0f, half_h);
                const Vec3f pe  = center + Vec3f(r, 0.0f, 0.0f);
                const Vec3f pw  = center - Vec3f(r, 0.0f, 0.0f);
                const Vec3f pn  = center + Vec3f(0.0f, r, 0.0f);
                const Vec3f ps  = center - Vec3f(0.0f, r, 0.0f);

                const auto add_tri = [&](const Vec3f& pA, const Vec3f& pB, const Vec3f& pC) {
                    PathTraceTriangle tri;
                    tri.a = pA; tri.b = pB; tri.c = pC;
                    tri.albedo = seam_albedo;
                    job.preview_triangles.push_back(std::move(tri));
                    job.source_tri_vertex_id.push_back(i);
                    job.source_tri_tag_z.push_back(center.z());
                    job.source_tri_height.push_back(h);
                    job.source_tri_role.push_back(0.35f);
                    job.source_tri_facet.push_back(4.0f); // 4.0 for seam diamond
                    job.source_tri_delta_angle.push_back(0.0f);
                    job.source_tri_opacity.push_back(1.0f);
                };

                add_tri(top, pe, pn);
                add_tri(top, pn, pw);
                add_tri(top, pw, ps);
                add_tri(top, ps, pe);
                add_tri(bot, pn, pe);
                add_tri(bot, pw, pn);
                add_tri(bot, ps, pw);
                add_tri(bot, pe, ps);
            }
        }
    }

    if (!job.preview_triangles.empty() && plate_visible && current_plate != nullptr)
        job.plate_groups.push_back({0, plate_triangles_for_pathtrace(*current_plate)});
}

void GLCanvas3D::_extract_prepare_volumes_for_pathtrace(
    PathTraceBvhJob& job,
    const std::vector<ColorRGBA>& extruder_colors,
    bool plate_visible,
    PartPlateList& plate_list) const
{
    if (m_model == nullptr)
        return;

    struct VolumeGroup {
        int object_idx{-1};
        int instance_idx{-1};
        int volume_idx{-1};
        bool is_wipe_tower{false};
        int wipe_tower_plate_idx{-1};
        Transform3d instance_trafo{Transform3d::Identity()};
        Transform3d inv_instance_trafo{Transform3d::Identity()};
        std::vector<const GLVolume*> volumes;
    };

    std::vector<VolumeGroup> groups;
    std::map<std::pair<int, int>, size_t> instance_to_group;
    // The meshes shade as smoothly as the rasterized realistic view does.
    const bool smooth_normals = wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool(SETTING_OPENGL_PHONG_SMOOTH_NORMALS);

    // Modifiers and unprintable parts are no part of the print: the rasterizer draws them over the
    // path traced image, see "overlays" in _render_objects().
    for (const GLVolume* volume : m_volumes.volumes) {
        if (volume == nullptr || !volume->is_active || volume->is_modifier || !volume->printable)
            continue;

        if (volume->is_wipe_tower || volume->object_idx() < 0) {
            VolumeGroup grp;
            grp.object_idx = volume->object_idx();
            grp.instance_idx = volume->instance_idx();
            grp.volume_idx = volume->volume_idx();
            grp.is_wipe_tower = volume->is_wipe_tower;
            grp.wipe_tower_plate_idx = volume->is_wipe_tower ? (volume->object_idx() - 1000) : -1;
            grp.instance_trafo = volume->world_matrix();
            grp.inv_instance_trafo = grp.instance_trafo.inverse();
            grp.volumes.push_back(volume);
            groups.push_back(std::move(grp));
        } else {
            const std::pair<int, int> key{volume->object_idx(), volume->instance_idx()};
            auto it = instance_to_group.find(key);
            if (it == instance_to_group.end()) {
                VolumeGroup grp;
                grp.object_idx = key.first;
                grp.instance_idx = key.second;
                grp.volume_idx = volume->volume_idx();
                grp.is_wipe_tower = false;
                grp.wipe_tower_plate_idx = -1;
                grp.instance_trafo = volume->get_instance_transformation().get_matrix();
                grp.inv_instance_trafo = grp.instance_trafo.inverse();
                grp.volumes.push_back(volume);
                instance_to_group[key] = groups.size();
                groups.push_back(std::move(grp));
            } else {
                groups[it->second].volumes.push_back(volume);
            }
        }
    }

    auto extract_volume_triangles = [&](const GLVolume* volume, const VolumeGroup& grp, std::vector<PathTraceTriangle>& out_triangles, std::vector<uint32_t>& out_ext_ids) {
        if (volume == nullptr || !volume->is_active || volume->is_modifier || !volume->printable)
            return;
        const Transform3d local_trafo = grp.inv_instance_trafo * volume->world_matrix();
        if (volume->is_wipe_tower) {
            const auto* wt = dynamic_cast<const GLWipeTowerVolume*>(volume);
            if (wt != nullptr && !wt->model_per_colors.empty()) {
                const std::vector<ColorRGBA>& wt_colors = wt->get_colors();
                for (size_t c = 0; c < wt->model_per_colors.size(); ++c) {
                    const indexed_triangle_set its = wt->model_per_colors[c].get_geometry().get_as_indexed_triangle_set();
                    if (its.indices.empty())
                        continue;
                    Vec4f albedo{0.75f, 0.77f, 0.80f, 1.0f};
                    if (!wt_colors.empty()) {
                        const ColorRGBA& col = c < wt_colors.size() ? wt_colors[c] : wt_colors.front();
                        albedo = to_shown_albedo(col, col.a());
                    }
                    append_path_trace_triangles(out_triangles,
                                                its.vertices,
                                                its.indices,
                                                local_trafo,
                                                albedo,
                                                smooth_normals);
                    out_ext_ids.insert(out_ext_ids.end(), its.indices.size(), 0u);
                }
            } else if (volume->model.indices_count() > 0) {
                const indexed_triangle_set its = volume->model.get_geometry().get_as_indexed_triangle_set();
                if (!its.indices.empty()) {
                    append_path_trace_triangles(out_triangles,
                                                its.vertices,
                                                its.indices,
                                                local_trafo,
                                                Vec4f{0.75f, 0.77f, 0.80f, 1.0f},
                                                smooth_normals);
                    out_ext_ids.insert(out_ext_ids.end(), its.indices.size(), 0u);
                }
            }
        } else {
            const int object_idx = volume->object_idx();
            const int volume_idx = volume->volume_idx();
            if (object_idx < 0 || object_idx >= int(m_model->objects.size()))
                return;
            const ModelObject* object = m_model->objects[object_idx];
            if (object == nullptr || volume_idx < 0 || volume_idx >= int(object->volumes.size()))
                return;
            const ModelVolume* model_volume = object->volumes[volume_idx];
            if (model_volume == nullptr || model_volume->mesh().its.indices.empty())
                return;
            const int base_extruder = model_volume->extruder_id() > 0 ? model_volume->extruder_id() : volume->extruder_id;
            const uint32_t base_ext_id = (base_extruder > 0) ? uint32_t(base_extruder) : 1u;
            if (!model_volume->mmu_segmentation_facets.empty()) {
                std::vector<indexed_triangle_set> its_per_color;
                model_volume->mmu_segmentation_facets.get_facets(*model_volume, its_per_color);
                for (size_t color_idx = 0; color_idx < its_per_color.size(); ++color_idx) {
                    if (its_per_color[color_idx].indices.empty())
                        continue;
                    const uint32_t ext_id = (color_idx == 0) ? base_ext_id : uint32_t(color_idx);
                    append_path_trace_triangles(out_triangles,
                                                its_per_color[color_idx].vertices,
                                                its_per_color[color_idx].indices,
                                                local_trafo,
                                                resolve_extruder_albedo(int(ext_id), volume, extruder_colors),
                                                smooth_normals);
                    out_ext_ids.insert(out_ext_ids.end(), its_per_color[color_idx].indices.size(), ext_id);
                }
            } else {
                append_path_trace_triangles(out_triangles,
                                            model_volume->mesh().its.vertices,
                                            model_volume->mesh().its.indices,
                                            local_trafo,
                                            resolve_extruder_albedo(int(base_ext_id), volume, extruder_colors),
                                            smooth_normals);
                out_ext_ids.insert(out_ext_ids.end(), model_volume->mesh().its.indices.size(), base_ext_id);
            }
        }
    };

    for (const VolumeGroup& group : groups) {
        std::vector<PathTraceTriangle> local_triangles;
        std::vector<uint32_t> local_ext_ids;
        for (const GLVolume* volume : group.volumes)
            extract_volume_triangles(volume, group, local_triangles, local_ext_ids);

        if (!local_triangles.empty()) {
            job.volume_groups.push_back({group.object_idx, group.instance_idx, group.volume_idx, group.is_wipe_tower, group.wipe_tower_plate_idx, std::move(local_triangles), std::move(local_ext_ids)});
        }
    }

    if (!job.volume_groups.empty() && plate_visible) {
        for (size_t p_idx = 0; p_idx < plate_list.get_plate_count(); ++p_idx) {
            const PartPlate* plate = plate_list.get_plate(int(p_idx));
            if (plate == nullptr)
                continue;
            std::vector<PathTraceTriangle> plate_triangles = plate_triangles_for_pathtrace(*plate);
            if (!plate_triangles.empty())
                job.plate_groups.push_back({int(p_idx), std::move(plate_triangles)});
        }
    }
}

void GLCanvas3D::_update_pathtrace_instances_and_tlas(size_t transform_signature)
{
    m_pathtrace_gpu_instances.clear();
    for (PathTraceVolumeBlas& blas : m_pathtrace_volume_blas) {
        if (blas.node_count == 0 || blas.tri_count == 0)
            continue;
        PathTraceGpuInstance inst;
        inst.data[0] = blas.root_node;
        inst.data[1] = blas.node_count;
        inst.data[2] = blas.tri_offset;
        inst.data[3] = blas.tri_count;

        Transform3d world_trafo = Transform3d::Identity();
        const GLVolume* vol = nullptr;
        if (blas.is_wipe_tower) {
            for (const GLVolume* v : m_volumes.volumes) {
                if (v != nullptr && v->is_wipe_tower && (v->object_idx() - 1000) == blas.wipe_tower_plate_idx) {
                    vol = v;
                    break;
                }
            }
            if (vol == nullptr || !vol->is_active)
                continue;
            world_trafo = vol->world_matrix();
        } else if (blas.object_idx >= 0) {
            for (const GLVolume* v : m_volumes.volumes) {
                if (v != nullptr && v->object_idx() == blas.object_idx && v->instance_idx() == blas.instance_idx) {
                    vol = v;
                    break;
                }
            }
            if (vol == nullptr || !vol->is_active)
                continue;
            if (m_model != nullptr && vol->object_idx() >= 0 && vol->object_idx() < int(m_model->objects.size()))
                world_trafo = vol->get_instance_transformation().get_matrix();
            else
                world_trafo = vol->world_matrix();
        }
        const Transform3d inv_trafo = world_trafo.inverse();
        const Eigen::Matrix4f m_inv = inv_trafo.matrix().cast<float>();
        const bool is_identity = m_inv.isApprox(Eigen::Matrix4f::Identity(), 1e-5f);
        const Eigen::Matrix3f m_norm = m_inv.block<3, 3>(0, 0).transpose();

        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                inst.world_to_local[col * 4 + row] = m_inv(row, col);
            }
        }
        for (int col = 0; col < 3; ++col) {
            for (int row = 0; row < 3; ++row) {
                inst.normal_to_world[col * 4 + row] = m_norm(row, col);
            }
        }
        inst.normal_to_world[15] = 1.0f;

        Vec3f world_min = Vec3f::Constant(std::numeric_limits<float>::max());
        Vec3f world_max = Vec3f::Constant(std::numeric_limits<float>::lowest());
        for (int c = 0; c < 8; ++c) {
            Vec3f pt(
                (c & 1) ? blas.local_bounds_max.x() : blas.local_bounds_min.x(),
                (c & 2) ? blas.local_bounds_max.y() : blas.local_bounds_min.y(),
                (c & 4) ? blas.local_bounds_max.z() : blas.local_bounds_min.z()
            );
            Vec3f world_pt = (world_trafo * pt.cast<double>()).cast<float>();
            world_min = world_min.cwiseMin(world_pt);
            world_max = world_max.cwiseMax(world_pt);
        }
        inst.bounds_min = {world_min.x(), world_min.y(), world_min.z(), (blas.plate_idx >= 0) ? -2.0f : 1.0f};
        inst.bounds_max = {world_max.x(), world_max.y(), world_max.z(), is_identity ? 1.0f : 0.0f};
        m_pathtrace_gpu_instances.push_back(inst);
    }

    if (m_pathtrace_gpu_instance_buffer == 0)
        glsafe(::glGenBuffers(1, &m_pathtrace_gpu_instance_buffer));
    glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_instance_buffer));
    glsafe(::glBufferData(GL_SHADER_STORAGE_BUFFER,
                          m_pathtrace_gpu_instances.size() * sizeof(PathTraceGpuInstance),
                          m_pathtrace_gpu_instances.data(),
                          GL_DYNAMIC_DRAW));
    glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0));

    struct TlasBuildNode {
        Vec3f bounds_min{Vec3f::Constant(std::numeric_limits<float>::max())};
        Vec3f bounds_max{Vec3f::Constant(std::numeric_limits<float>::lowest())};
        int left{-1};
        int right{-1};
        int inst_idx{-1};
        int split_axis{0};
    };

    std::vector<TlasBuildNode> tlas_build_nodes;
    const size_t num_insts = m_pathtrace_gpu_instances.size();
    if (num_insts > 0) {
        std::vector<int> inst_indices(num_insts);
        std::iota(inst_indices.begin(), inst_indices.end(), 0);

        const auto compute_inst_centroid = [&](int idx) -> Vec3f {
            const auto& inst = m_pathtrace_gpu_instances[size_t(idx)];
            return Vec3f(0.5f * (inst.bounds_min[0] + inst.bounds_max[0]),
                         0.5f * (inst.bounds_min[1] + inst.bounds_max[1]),
                         0.5f * (inst.bounds_min[2] + inst.bounds_max[2]));
        };

        std::function<int(size_t, size_t)> build_tlas_node = [&](size_t first, size_t count) -> int {
            const int node_idx = int(tlas_build_nodes.size());
            tlas_build_nodes.emplace_back();

            Vec3f b_min = Vec3f::Constant(std::numeric_limits<float>::max());
            Vec3f b_max = Vec3f::Constant(std::numeric_limits<float>::lowest());
            Vec3f c_min = Vec3f::Constant(std::numeric_limits<float>::max());
            Vec3f c_max = Vec3f::Constant(std::numeric_limits<float>::lowest());

            for (size_t i = first; i < first + count; ++i) {
                const int inst_id = inst_indices[i];
                const auto& inst = m_pathtrace_gpu_instances[size_t(inst_id)];
                const Vec3f i_min(inst.bounds_min[0], inst.bounds_min[1], inst.bounds_min[2]);
                const Vec3f i_max(inst.bounds_max[0], inst.bounds_max[1], inst.bounds_max[2]);
                b_min = b_min.cwiseMin(i_min);
                b_max = b_max.cwiseMax(i_max);
                const Vec3f cent = 0.5f * (i_min + i_max);
                c_min = c_min.cwiseMin(cent);
                c_max = c_max.cwiseMax(cent);
            }

            tlas_build_nodes[node_idx].bounds_min = b_min;
            tlas_build_nodes[node_idx].bounds_max = b_max;

            if (count <= 1) {
                tlas_build_nodes[node_idx].inst_idx = inst_indices[first];
                return node_idx;
            }

            const Vec3f c_ext = c_max - c_min;
            int axis = 0;
            if (c_ext.y() > c_ext.x() && c_ext.y() >= c_ext.z())
                axis = 1;
            else if (c_ext.z() > c_ext.x() && c_ext.z() >= c_ext.y())
                axis = 2;

            tlas_build_nodes[node_idx].split_axis = axis;
            const size_t mid = first + count / 2;
            std::nth_element(inst_indices.begin() + first,
                             inst_indices.begin() + mid,
                             inst_indices.begin() + first + count,
                             [&](int a, int b) {
                                 return compute_inst_centroid(a)[axis] < compute_inst_centroid(b)[axis];
                             });

            const int left_id = build_tlas_node(first, mid - first);
            const int right_id = build_tlas_node(mid, count - (mid - first));
            tlas_build_nodes[node_idx].left = left_id;
            tlas_build_nodes[node_idx].right = right_id;
            return node_idx;
        };

        build_tlas_node(0, num_insts);
    }

    std::vector<PathTraceGpuNode> tlas_gpu_nodes;
    if (!tlas_build_nodes.empty()) {
        tlas_gpu_nodes.resize(2 * tlas_build_nodes.size() + 2);
        std::vector<int> new_indices(tlas_build_nodes.size(), -1);
        std::vector<int> queue;
        queue.reserve(tlas_build_nodes.size());
        queue.push_back(0);
        new_indices[0] = 0;
        size_t next_child_idx = 1;
        size_t head = 0;

        while (head < queue.size()) {
            const int u = queue[head++];
            const TlasBuildNode& src = tlas_build_nodes[u];
            const int dst_idx = new_indices[u];
            PathTraceGpuNode& dst = tlas_gpu_nodes[dst_idx];

            for (int a = 0; a < 3; ++a) {
                dst.bounds_min[a] = src.bounds_min[a];
                dst.bounds_max[a] = src.bounds_max[a];
            }

            if (src.inst_idx >= 0) {
                dst.left_or_first = int32_t(src.inst_idx);
                dst.count_and_axis = 1u;
            } else {
                const int left_dst = int(next_child_idx);
                const int right_dst = int(next_child_idx + 1);
                next_child_idx += 2;

                dst.left_or_first = left_dst;
                dst.count_and_axis = uint32_t(src.split_axis) << 30;

                if (src.left >= 0) {
                    new_indices[src.left] = left_dst;
                    queue.push_back(src.left);
                }
                if (src.right >= 0) {
                    new_indices[src.right] = right_dst;
                    queue.push_back(src.right);
                }
            }
        }
        tlas_gpu_nodes.resize(next_child_idx > 1 ? next_child_idx : 1);
    }

    if (m_pathtrace_gpu_tlas_buffer == 0)
        glsafe(::glGenBuffers(1, &m_pathtrace_gpu_tlas_buffer));
    glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_tlas_buffer));
    glsafe(::glBufferData(GL_SHADER_STORAGE_BUFFER,
                          tlas_gpu_nodes.size() * sizeof(PathTraceGpuNode),
                          tlas_gpu_nodes.data(),
                          GL_DYNAMIC_DRAW));
    glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0));

    m_pathtrace_transform_signature = transform_signature;
}

void GLCanvas3D::_render_pathtrace_composite()
{
    // The interactive image stays on screen until the first full resolution sample is complete.
    const bool show_interactive = m_pathtrace_gpu_samples < 2 && m_pathtrace_gpu_interactive_size[0] > 0;
    const unsigned int color_tex = show_interactive ? m_pathtrace_gpu_interactive_display_image : m_pathtrace_gpu_display_image;
    const unsigned int bed_tex = show_interactive ? m_pathtrace_gpu_interactive_trans_display_image : m_pathtrace_gpu_trans_display_image;
    const std::array<unsigned int, 2>& size = show_interactive ? m_pathtrace_gpu_interactive_size : m_pathtrace_gpu_image_size;
    const float u = float(size[0]) / float(std::max(1u, m_pathtrace_gpu_image_size[0]));
    const float v = float(size[1]) / float(std::max(1u, m_pathtrace_gpu_image_size[1]));

    glsafe(::glDisable(GL_DEPTH_TEST));
    // Pass 1: modulate the rasterized bed by the path traced irradiance ratio. The texture holds
    // half the ratio and GL_DST_COLOR, GL_SRC_COLOR yields 2 * src * dst, so the bed can be both
    // darkened (shadows) and brightened or tinted (indirect light). Destination alpha is kept.
    // GLTexture::render_texture() cannot be used for this pass: it sets its own blend function.
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFuncSeparate(GL_DST_COLOR, GL_SRC_COLOR, GL_ZERO, GL_ONE));
    render_pathtrace_fullscreen_texture(m_background, bed_tex, u, v);
    // Pass 2: path traced objects over the modulated bed.
    GLTexture::render_sub_texture(color_tex, -1.0f, 1.0f, -1.0f, 1.0f, { { 0.0f, v }, { u, v }, { u, 0.0f }, { 0.0f, 0.0f } });
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glEnable(GL_DEPTH_TEST));
}

void GLCanvas3D::_resolve_pathtrace_image(GLShaderProgram& shader, bool interactive, uint32_t width, uint32_t height, uint32_t samples)
{
    // Progressive images follow the variance of each pixel, which limits the filter by itself: it
    // blends what is still noisy and leaves converged edges and texture alone. Interactive frames do
    // not keep that variance and are filtered at full strength; with a single sample, the luminance
    // edge stop is wide enough to smooth isolated bright pixels. The bed term has no variance and
    // fades out as the samples converge.
    const float sqrt_samples = std::sqrt(float(std::max(1u, samples)));
    const float strength = interactive ? 1.0f : std::min(1.0f, 4.0f / sqrt_samples);
    // Steps 1, 2 and 4: a step of 8 leaves blocks around sharp highlights.
    const int passes = 3;

    const std::array<unsigned int, 4> inputs = { m_pathtrace_gpu_accumulation_image, m_pathtrace_gpu_trans_accumulation_image,
                                                 m_pathtrace_gpu_aux_normal_depth_image, m_pathtrace_gpu_aux_albedo_image };
    for (size_t i = 0; i < inputs.size(); ++i) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + GLenum(i)));
        glsafe(::glBindTexture(GL_TEXTURE_2D, inputs[i]));
    }
    glsafe(::glActiveTexture(GL_TEXTURE6));
    glsafe(::glBindTexture(GL_TEXTURE_2D, m_pathtrace_gpu_moment_image));
    glsafe(::glBindImageTexture(2, interactive ? m_pathtrace_gpu_interactive_display_image : m_pathtrace_gpu_display_image,
                                0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8));
    glsafe(::glBindImageTexture(3, interactive ? m_pathtrace_gpu_interactive_trans_display_image : m_pathtrace_gpu_trans_display_image,
                                0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8));

    shader.start_using();
    shader.set_uniform("accumulation_tex", 0);
    shader.set_uniform("bed_accumulation_tex", 1);
    shader.set_uniform("normal_depth_tex", 2);
    shader.set_uniform("albedo_tex", 3);
    shader.set_uniform("color_in_tex", 4);
    shader.set_uniform("bed_in_tex", 5);
    shader.set_uniform("moment_tex", 6);
    shader.set_uniform("use_variance", !interactive);
    shader.set_uniform("image_size", std::array<int, 2>{ int(width), int(height) });
    shader.set_uniform("strength", strength);
    shader.set_uniform("luminance_sigma", interactive && samples <= 1 ? 6.0f : 0.05f + 1.5f / sqrt_samples);
    for (int pass = 0; pass < passes; ++pass) {
        // Ping-pong: each pass writes one set of images and reads the other.
        const size_t out_set = size_t(pass % 2);
        const size_t in_set = 1 - out_set;
        glsafe(::glActiveTexture(GL_TEXTURE4));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_pathtrace_gpu_denoise_images[in_set]));
        glsafe(::glActiveTexture(GL_TEXTURE5));
        glsafe(::glBindTexture(GL_TEXTURE_2D, m_pathtrace_gpu_denoise_images[2 + in_set]));
        glsafe(::glBindImageTexture(0, m_pathtrace_gpu_denoise_images[out_set], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F));
        glsafe(::glBindImageTexture(1, m_pathtrace_gpu_denoise_images[2 + out_set], 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA16F));
        shader.set_uniform("step_size", passes == 1 ? 0 : (1 << pass));
        shader.set_uniform("first_pass", pass == 0);
        shader.set_uniform("final_pass", pass == passes - 1);
        glsafe(::glDispatchCompute((width + 7u) / 8u, (height + 7u) / 8u, 1));
        glsafe(::glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));
    }
    shader.stop_using();

    for (int unit = 6; unit >= 0; --unit) {
        glsafe(::glActiveTexture(GL_TEXTURE0 + GLenum(unit)));
        glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
    }
    for (GLuint unit = 0; unit < 4; ++unit)
        glsafe(::glBindImageTexture(unit, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, unit < 2 ? GL_RGBA16F : GL_RGBA8));
}

bool GLCanvas3D::_render_pathtraced_view(const Camera& source_camera, const Size& canvas_size)
{
    size_t geom_signature = 0;
    const auto combine_geom = [&geom_signature](size_t value) {
        geom_signature ^= value + size_t(0x9e3779b9) + (geom_signature << 6) + (geom_signature >> 2);
    };

    size_t transform_signature = 0;
    const auto combine_transform = [&transform_signature](size_t value) {
        transform_signature ^= value + size_t(0x9e3779b9) + (transform_signature << 6) + (transform_signature >> 2);
    };
    const auto hash_matrix_transform = [&combine_transform](const Transform3d& matrix) {
        const auto& coefficients = matrix.matrix();
        for (Eigen::Index i = 0; i < coefficients.size(); ++i)
            combine_transform(std::hash<double>{}(coefficients.data()[i]));
    };

    const std::vector<ColorRGBA> extruder_colors =
        (wxGetApp().plater() != nullptr) ? wxGetApp().plater()->get_extruders_colors() : std::vector<ColorRGBA>{};

    combine_geom(size_t(m_canvas_type));
    combine_geom(size_t(wxGetApp().app_config != nullptr && wxGetApp().app_config->get_bool(SETTING_OPENGL_PHONG_SMOOTH_NORMALS)));
    float preview_z_min = -1e10f;
    float preview_z_max = 1e10f;
    int preview_move_min = -1;
    int preview_move_max = -1;
    size_t preview_color_signature = 0;
    const auto combine_color = [&preview_color_signature](size_t value) {
        preview_color_signature ^= value + size_t(0x9e3779b9) + (preview_color_signature << 6) + (preview_color_signature >> 2);
    };
    for (const ColorRGBA& c : extruder_colors) {
        combine_color(std::hash<float>{}(c.r()));
        combine_color(std::hash<float>{}(c.g()));
        combine_color(std::hash<float>{}(c.b()));
        combine_color(std::hash<float>{}(c.a()));
    }
    if (m_model != nullptr && m_canvas_type == ECanvasType::CanvasView3D) {
        for (const GLVolume* volume : m_volumes.volumes) {
            if (volume == nullptr)
                continue;
            combine_color(std::hash<float>{}(volume->color.r()));
            combine_color(std::hash<float>{}(volume->color.g()));
            combine_color(std::hash<float>{}(volume->color.b()));
            combine_color(std::hash<float>{}(volume->color.a()));
        }
    }
    combine_color(size_t(m_gcode_viewer.is_translucent_filaments()));

    PartPlateList& plate_list = wxGetApp().plater()->get_partplate_list();
    PartPlate* current_plate = plate_list.get_curr_plate();
    const bool plate_visible = is_bed_visible() && current_plate != nullptr;
    combine_geom(size_t(plate_visible));
    if (plate_visible) {
        combine_geom(size_t(plate_list.get_plate_count()));
        if (m_canvas_type == ECanvasType::CanvasView3D) {
            for (size_t p_idx = 0; p_idx < plate_list.get_plate_count(); ++p_idx) {
                if (const PartPlate* pl = plate_list.get_plate(int(p_idx))) {
                    for (const Vec2d& point : pl->get_shape()) {
                        combine_geom(std::hash<double>{}(point.x()));
                        combine_geom(std::hash<double>{}(point.y()));
                    }
                }
            }
        } else {
            for (const Vec2d& point : current_plate->get_shape()) {
                combine_geom(std::hash<double>{}(point.x()));
                combine_geom(std::hash<double>{}(point.y()));
            }
        }
    }

    if (m_canvas_type == ECanvasType::CanvasPreview) {
        combine_color(size_t(m_gcode_viewer.get_view_type()));
        combine_geom(m_gcode_viewer.get_vertices_count());
        combine_geom(m_gcode_viewer.get_layers_count());
        const BoundingBoxf3& gcode_bb = m_gcode_viewer.get_paths_bounding_box();
        for (double v : {gcode_bb.min.x(), gcode_bb.min.y(), gcode_bb.min.z(), gcode_bb.max.x(), gcode_bb.max.y(), gcode_bb.max.z()})
            combine_geom(std::hash<double>{}(v));
        for (size_t r = 0; r < libvgcode::GCODE_EXTRUSION_ROLES_COUNT; ++r)
            combine_geom(size_t(m_gcode_viewer.is_extrusion_role_visible(libvgcode::EGCodeExtrusionRole(r))));
        combine_geom(size_t(m_gcode_viewer.is_option_visible(libvgcode::EOptionType::Seams)));
        if (m_gcode_viewer.has_data() && m_gcode_viewer.get_vertices_count() > 0) {
            const libvgcode::Interval& visible_range = m_gcode_viewer.get_gcode_view_visible_range();
            const libvgcode::Interval& full_range    = m_gcode_viewer.get_gcode_view_full_range();
            const uint32_t min_v = m_gcode_viewer.is_top_layer_only_view_range()
                ? uint32_t(full_range[0])
                : uint32_t(visible_range[0]);
            const uint32_t max_v = uint32_t(visible_range[1]);
            preview_move_min = int(min_v);
            preview_move_max = int(max_v);
            const std::vector<double> layer_zs = m_gcode_viewer.get_layers_zs();
            if (!layer_zs.empty()) {
                const auto& layers_range = m_gcode_viewer.get_layers_z_range();
                const size_t min_layer = std::min(layers_range[0], layers_range[1]);
                const size_t max_layer = std::max(layers_range[0], layers_range[1]);
                const size_t min_idx   = std::min<size_t>(min_layer, layer_zs.size() - 1);
                const size_t max_idx   = std::min<size_t>(max_layer, layer_zs.size() - 1);
                preview_z_min = (min_idx > 0) ? float(0.5 * (layer_zs[min_idx - 1] + layer_zs[min_idx])) : -1.0f;
                preview_z_max = (max_idx + 1 < layer_zs.size())
                    ? float(0.5 * (layer_zs[max_idx] + layer_zs[max_idx + 1]))
                    : float(layer_zs[max_idx] + 1.0);
            }
        }
    } else if (m_model != nullptr) {
        combine_geom(m_model->objects.size());
        for (const GLVolume* volume : m_volumes.volumes) {
            if (volume == nullptr)
                continue;
            combine_geom(volume->geometry_id.first);
            combine_geom(volume->geometry_id.second);
            combine_geom(volume->model.vertices_count());
            combine_geom(volume->model.indices_count());
            combine_geom(size_t(volume->is_active));
            combine_geom(size_t(volume->printable));
            combine_geom(size_t(volume->extruder_id));
            combine_geom(size_t(volume->is_wipe_tower));
            if (volume->is_wipe_tower) {
                const auto* wt = dynamic_cast<const GLWipeTowerVolume*>(volume);
                if (wt != nullptr) {
                    combine_geom(wt->model_per_colors.size());
                    for (const auto& m : wt->model_per_colors) {
                        combine_geom(m.vertices_count());
                        combine_geom(m.indices_count());
                    }
                    for (const auto& c : wt->get_colors()) {
                        combine_geom(std::hash<float>{}(c.r()));
                        combine_geom(std::hash<float>{}(c.g()));
                        combine_geom(std::hash<float>{}(c.b()));
                        combine_geom(std::hash<float>{}(c.a()));
                    }
                }
                const BoundingBoxf3& bb = volume->bounding_box();
                combine_geom(std::hash<double>{}(bb.min.x()));
                combine_geom(std::hash<double>{}(bb.min.y()));
                combine_geom(std::hash<double>{}(bb.min.z()));
                combine_geom(std::hash<double>{}(bb.max.x()));
                combine_geom(std::hash<double>{}(bb.max.y()));
                combine_geom(std::hash<double>{}(bb.max.z()));
            }
            if (volume->is_active && volume->object_idx() >= 0) {
                const Transform3d& part_trafo = volume->get_volume_transformation().get_matrix();
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) {
                        combine_geom(std::hash<double>{}(part_trafo.matrix()(r, c)));
                    }
                }
            }
            const int object_idx = volume->object_idx();
            const int volume_idx = volume->volume_idx();
            if (object_idx >= 0 && object_idx < int(m_model->objects.size()) && m_model->objects[object_idx] != nullptr &&
                volume_idx >= 0 && volume_idx < int(m_model->objects[object_idx]->volumes.size()) &&
                m_model->objects[object_idx]->volumes[volume_idx] != nullptr) {
                const ModelVolume* mv = m_model->objects[object_idx]->volumes[volume_idx];
                combine_geom(size_t(mv->extruder_id()));
                combine_geom(size_t(mv->mmu_segmentation_facets.timestamp()));
            }
        }
    }
    if (geom_signature == 0)
        geom_signature = 1;
    if (preview_color_signature == 0)
        preview_color_signature = 1;

    combine_transform(geom_signature);
    if (m_model != nullptr && m_canvas_type == ECanvasType::CanvasView3D) {
        for (const GLVolume* volume : m_volumes.volumes) {
            if (volume == nullptr || !volume->is_active)
                continue;
            hash_matrix_transform(volume->world_matrix());
        }
    }
    if (transform_signature == 0)
        transform_signature = 1;

    if (m_pathtrace_bvh_future.valid()) {
        if (m_pathtrace_bvh_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            PathTraceSceneBuildResult result = m_pathtrace_bvh_future.get();
            if (result.geom_signature == geom_signature) {
                m_pathtrace_gpu_scene = std::move(result.gpu_scene);
                m_pathtrace_volume_blas = std::move(result.volume_blas);
                m_pathtrace_gpu_tri_vertex_id = std::move(result.gpu_tri_vertex_id);
                m_pathtrace_geom_signature = geom_signature;
                m_pathtrace_preview_color_signature = 0;
                m_pathtrace_transform_signature = 0; // force instance rebuild
            }
            m_pathtrace_bvh_job_signature = 0;
        } else if (m_pathtrace_bvh_job_signature == geom_signature) {
            // Still building the BVH in background: fall back to rasterized view
            schedule_extra_frame(16);
            return false;
        } else {
            // Scene changed while previous build was running: cancel it
            if (m_pathtrace_bvh_cancel_token)
                m_pathtrace_bvh_cancel_token->store(true);
            schedule_extra_frame(16);
            return false;
        }
    }

    if (geom_signature != m_pathtrace_geom_signature) {
        if (m_pathtrace_bvh_future.valid()) {
            if (m_pathtrace_bvh_cancel_token)
                m_pathtrace_bvh_cancel_token->store(true);
            schedule_extra_frame(16);
            return false;
        }

        m_pathtrace_gpu_tri_vertex_id.clear();
        m_pathtrace_gpu_tri_vertex_id.shrink_to_fit();
        m_pathtrace_gpu_scene.triangles.clear();
        m_pathtrace_gpu_scene.triangles.shrink_to_fit();
        m_pathtrace_gpu_scene.shadings.clear();
        m_pathtrace_gpu_scene.shadings.shrink_to_fit();
        m_pathtrace_gpu_scene.normals.clear();
        m_pathtrace_gpu_scene.normals.shrink_to_fit();
        m_pathtrace_gpu_scene.nodes.clear();
        m_pathtrace_gpu_scene.nodes.shrink_to_fit();
        m_pathtrace_gpu_scene.original_indices.clear();
        m_pathtrace_gpu_scene.original_indices.shrink_to_fit();
        m_pathtrace_volume_blas.clear();
        m_pathtrace_volume_blas.shrink_to_fit();

        PathTraceBvhJob job;
        job.geom_signature = geom_signature;
        job.is_preview = (m_canvas_type == ECanvasType::CanvasPreview);
        job.extruder_sss_families = build_extruder_sss_families();

        if (m_canvas_type == ECanvasType::CanvasPreview) {
            _extract_preview_toolpaths_for_pathtrace(job, 0, plate_visible, current_plate);
        } else if (m_model != nullptr) {
            _extract_prepare_volumes_for_pathtrace(job, extruder_colors, plate_visible, plate_list);
        }

        auto cancel_token = std::make_shared<std::atomic<bool>>(false);
        m_pathtrace_bvh_cancel_token = cancel_token;
        m_pathtrace_bvh_job_signature = geom_signature;
        m_pathtrace_bvh_future = std::async(std::launch::async, [job = std::move(job), cancel_token]() mutable {
            return _build_pathtrace_scene_async(std::move(job), cancel_token);
        });

        schedule_extra_frame(16);
        return false;
    }

    if (m_pathtrace_volume_blas.empty())
        return false;

    if (!m_pathtrace_gpu_tri_vertex_id.empty() &&
        m_pathtrace_preview_color_signature != preview_color_signature) {
        const std::vector<int> sss_families = build_extruder_sss_families();
        bool any_opacity_changed = false;
        if (m_canvas_type == ECanvasType::CanvasPreview) {
            for (size_t i = 0; i < m_pathtrace_gpu_scene.triangles.size(); ++i) {
                const size_t v_id = m_pathtrace_gpu_tri_vertex_id[i];
                if (v_id == SIZE_MAX)
                    continue;
                const libvgcode::PathVertex& v = m_gcode_viewer.get_gcode_vertex_at(v_id);
                if (v.is_option()) {
                    const ColorRGBA opt_col = m_gcode_viewer.get_option_color(libvgcode::move_type_to_option(v.type));
                    const Vec4f lin_opt = to_linear_albedo(opt_col.r(), opt_col.g(), opt_col.b(), 1.0f);
                    m_pathtrace_gpu_scene.shadings[i].albedo = {lin_opt.x(), lin_opt.y(), lin_opt.z(), 1.0f};
                    if (std::abs(m_pathtrace_gpu_scene.triangles[i].c[3] - 1.0f) > 1e-4f) {
                        m_pathtrace_gpu_scene.triangles[i].c[3] = 1.0f;
                        any_opacity_changed = true;
                    }
                    continue;
                }
                const libvgcode::Color c = m_gcode_viewer.get_gcode_vertex_color(v);
                const uint8_t a = m_gcode_viewer.get_gcode_vertex_alpha(v);
                const float opacity = float(a) / 255.0f;
                const Vec3f albedo_rgb = {float(c[0]) / 255.0f, float(c[1]) / 255.0f, float(c[2]) / 255.0f};
                const Vec4f lin_albedo = to_linear_albedo(albedo_rgb.x(), albedo_rgb.y(), albedo_rgb.z(), opacity);
                m_pathtrace_gpu_scene.shadings[i].albedo = {lin_albedo.x(), lin_albedo.y(), lin_albedo.z(), lin_albedo.w()};
                const float roughness = 0.35f;
                const size_t ext_idx = (v.extruder_id > 0) ? size_t(v.extruder_id - 1) : 0;
                const int sss_mode = sss_mode_for_extruder(sss_families, ext_idx, lin_albedo.w(), Vec3f{lin_albedo.x(), lin_albedo.y(), lin_albedo.z()});
                m_pathtrace_gpu_scene.shadings[i].params[1] = roughness + float(sss_mode) * 2.0f;
                if (std::abs(m_pathtrace_gpu_scene.triangles[i].c[3] - lin_albedo.w()) > 1e-4f) {
                    m_pathtrace_gpu_scene.triangles[i].c[3] = lin_albedo.w();
                    any_opacity_changed = true;
                }
            }
        } else {
            for (size_t i = 0; i < m_pathtrace_gpu_scene.triangles.size(); ++i) {
                const size_t ext_id = m_pathtrace_gpu_tri_vertex_id[i];
                if (ext_id == 0 || ext_id == SIZE_MAX)
                    continue;
                const Vec4f lin_albedo = resolve_extruder_albedo(int(ext_id), nullptr, extruder_colors);
                m_pathtrace_gpu_scene.shadings[i].albedo = {lin_albedo.x(), lin_albedo.y(), lin_albedo.z(), lin_albedo.w()};
                const float roughness = 0.35f;
                const size_t ext_idx = (ext_id > 0) ? (ext_id - 1) : 0;
                const int sss_mode = sss_mode_for_extruder(sss_families, ext_idx, lin_albedo.w(), Vec3f{lin_albedo.x(), lin_albedo.y(), lin_albedo.z()});
                m_pathtrace_gpu_scene.shadings[i].params[1] = roughness + float(sss_mode) * 2.0f;
                if (std::abs(m_pathtrace_gpu_scene.triangles[i].c[3] - lin_albedo.w()) > 1e-4f) {
                    m_pathtrace_gpu_scene.triangles[i].c[3] = lin_albedo.w();
                    any_opacity_changed = true;
                }
            }
        }
        if (m_pathtrace_gpu_shading_buffer != 0 && m_pathtrace_gpu_scene_signature == m_pathtrace_geom_signature) {
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_shading_buffer));
            glsafe(::glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                     m_pathtrace_gpu_scene.shadings.size() * sizeof(PathTraceGpuShading),
                                     m_pathtrace_gpu_scene.shadings.data()));
            if (any_opacity_changed && m_pathtrace_gpu_triangle_buffer != 0) {
                glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_triangle_buffer));
                glsafe(::glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0,
                                         m_pathtrace_gpu_scene.triangles.size() * sizeof(PathTraceGpuTriangle),
                                         m_pathtrace_gpu_scene.triangles.data()));
            }
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0));
        }
        m_pathtrace_preview_color_signature = preview_color_signature;
    }

    size_t signature = transform_signature;
    const auto combine = [&signature](size_t value) {
        signature ^= value + size_t(0x9e3779b9) + (signature << 6) + (signature >> 2);
    };
    const auto hash_matrix = [&combine](const Transform3d& matrix) {
        const auto& coefficients = matrix.matrix();
        for (Eigen::Index i = 0; i < coefficients.size(); ++i)
            combine(std::hash<double>{}(coefficients.data()[i]));
    };
    combine(preview_color_signature);
    std::string pt_quality = "medium";
    if (const AppConfig* cfg = wxGetApp().app_config) {
        try {
            const std::string q = cfg->get(SETTING_OPENGL_PATHTRACE_QUALITY);
            if (!q.empty())
                pt_quality = q;
        } catch (...) {}
    }

    uint32_t cfg_max_samples = 128u;
    int cfg_bounces = 2;
    int max_trans_bounces = 8;
    int shadow_max_hits = 8;
    int quality_preset = 1;
    // GPU time per frame of the progressive passes, which leaves the rest of a 60 Hz frame to the
    // rasterizer and the UI, and the range of the interactive resolution. Dense close-ups of toolpaths
    // need the low end of that range to keep orbiting at the 8 ms target.
    m_pathtrace_tile_budget_ms = 10.0f;
    float min_orbit_scale = 0.18f;
    float max_orbit_scale = 0.65f;

    if (pt_quality == "low") {
        cfg_max_samples = 64u;
        cfg_bounces = 1;
        max_trans_bounces = 4;
        shadow_max_hits = 4;
        quality_preset = 0;
        m_pathtrace_tile_budget_ms = 8.0f;
        min_orbit_scale = 0.15f;
        max_orbit_scale = 0.50f;
    } else if (pt_quality == "high") {
        cfg_max_samples = 256u;
        cfg_bounces = 2;
        max_trans_bounces = 12;
        shadow_max_hits = 8;
        quality_preset = 2;
        m_pathtrace_tile_budget_ms = 12.0f;
        min_orbit_scale = 0.20f;
        max_orbit_scale = 0.75f;
    } else if (m_canvas_type == ECanvasType::CanvasPreview) {
        // Bounce rays start inside dense toolpaths and run incoherent, at several times the cost of
        // camera rays: on Medium the second bounce took a third of a toolpath sample, for half a
        // percent of its brightness. High keeps it.
        cfg_bounces = 1;
    }

    const bool glossy_toolpaths = wxGetApp().app_config == nullptr || wxGetApp().app_config->get_bool(SETTING_OPENGL_REALISTIC_GLOSS);
    combine(size_t(cfg_bounces));
    combine(size_t(quality_preset));
    combine(size_t(_shadow_mode()));
    combine(size_t(glossy_toolpaths));
    combine(std::hash<float>{}(preview_z_min));
    combine(std::hash<float>{}(preview_z_max));
    combine(std::hash<int>{}(preview_move_min));
    combine(std::hash<int>{}(preview_move_max));
    const ClippingPlane clip_plane = _get_volumes_clipping_plane();
    const bool clipping_active = clip_plane.is_active();
    combine(size_t(clipping_active));
    if (clipping_active) {
        for (double v : clip_plane.get_data())
            combine(std::hash<double>{}(v));
    }
    // The greying and dimming of the layers around the ones the sliders show, by the top of the layers.
    libvgcode::LayerShading layer_shading;
    std::array<float, 3> layer_shading_z{ -1e9f, -1e9f, -1e9f };
    if (m_canvas_type == ECanvasType::CanvasPreview) {
        layer_shading = m_gcode_viewer.get_layer_shading();
        const std::vector<double> layer_zs = m_gcode_viewer.get_layers_zs();
        const auto layer_top = [&layer_zs](int id) { return id >= 0 && size_t(id) < layer_zs.size() ? float(layer_zs[size_t(id)]) : -1e9f; };
        layer_shading_z = { layer_top(layer_shading.lit_layers[0]), layer_top(layer_shading.lit_layers[1]),
                            layer_shading.grey_below_layer > 0 ? layer_top(layer_shading.grey_below_layer) : -1e9f };
    }
    for (float z : layer_shading_z)
        combine(std::hash<float>{}(z));
    combine(std::hash<float>{}(layer_shading.dim_brightness));
    combine(std::hash<int>{}(layer_shading.kept_vertex));
    // The camera passing below the bed hides it, which no history may carry over.
    combine(size_t(!source_camera.is_looking_downward()));
    // Everything but the camera: the interactive history stays valid while only the camera moves.
    const size_t history_signature = signature;
    hash_matrix(source_camera.get_view_matrix());
    hash_matrix(source_camera.get_projection_matrix());
    combine(size_t(canvas_size.get_width()));
    combine(size_t(canvas_size.get_height()));
    if (signature == 0)
        signature = 1;

    const auto now_tp = std::chrono::steady_clock::now();
    if (signature != m_pathtrace_requested_signature) {
        if (m_pathtrace_requested_signature != 0)
            m_pathtrace_last_change_time = now_tp;
        m_pathtrace_requested_signature = signature;
        _reset_pathtrace_accumulation();
    }
    const bool recent_change = std::chrono::duration_cast<std::chrono::milliseconds>(now_tp - m_pathtrace_last_change_time).count() < 150;
    const bool interactive = m_camera_movement || m_mouse.dragging || m_gizmos.is_dragging() || recent_change;

    m_pathtrace_max_samples = cfg_max_samples;
    m_pathtrace_is_interactive = interactive;

    bool can_use_gpu = false;
    GLShaderProgram* compute_shader = nullptr;
    GLShaderProgram* denoise_shader = nullptr;
#if !SLIC3R_OPENGL_ES
    if (OpenGLManager::get_gl_info().is_version_greater_or_equal_to(4, 3)) {
        compute_shader = wxGetApp().get_shader("pathtrace_compute");
        denoise_shader = wxGetApp().get_shader("pathtrace_denoise");
        can_use_gpu = compute_shader != nullptr && denoise_shader != nullptr;
    }
#endif // !SLIC3R_OPENGL_ES

    if (can_use_gpu) {
        if (m_pathtrace_gpu_scene_signature != m_pathtrace_geom_signature) {
            if (m_pathtrace_gpu_node_buffer == 0)
                glsafe(::glGenBuffers(1, &m_pathtrace_gpu_node_buffer));
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_node_buffer));
            glsafe(::glBufferData(GL_SHADER_STORAGE_BUFFER,
                                  m_pathtrace_gpu_scene.nodes.size() * sizeof(PathTraceGpuNode),
                                  m_pathtrace_gpu_scene.nodes.data(),
                                  GL_STATIC_DRAW));

            if (m_pathtrace_gpu_triangle_buffer == 0)
                glsafe(::glGenBuffers(1, &m_pathtrace_gpu_triangle_buffer));
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_triangle_buffer));
            glsafe(::glBufferData(GL_SHADER_STORAGE_BUFFER,
                                  m_pathtrace_gpu_scene.triangles.size() * sizeof(PathTraceGpuTriangle),
                                  m_pathtrace_gpu_scene.triangles.data(),
                                  GL_DYNAMIC_DRAW));

            if (m_pathtrace_gpu_shading_buffer == 0)
                glsafe(::glGenBuffers(1, &m_pathtrace_gpu_shading_buffer));
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_shading_buffer));
            glsafe(::glBufferData(GL_SHADER_STORAGE_BUFFER,
                                  m_pathtrace_gpu_scene.shadings.size() * sizeof(PathTraceGpuShading),
                                  m_pathtrace_gpu_scene.shadings.data(),
                                  GL_DYNAMIC_DRAW));

            // Without shading normals the binding still needs a buffer, which the shader never reads.
            const PathTraceGpuNormals no_normals{};
            if (m_pathtrace_gpu_normal_buffer == 0)
                glsafe(::glGenBuffers(1, &m_pathtrace_gpu_normal_buffer));
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_pathtrace_gpu_normal_buffer));
            glsafe(::glBufferData(GL_SHADER_STORAGE_BUFFER,
                                  std::max<size_t>(1, m_pathtrace_gpu_scene.normals.size()) * sizeof(PathTraceGpuNormals),
                                  m_pathtrace_gpu_scene.normals.empty() ? no_normals.data() : m_pathtrace_gpu_scene.normals.front().data(),
                                  GL_STATIC_DRAW));
            glsafe(::glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0));

            m_pathtrace_gpu_scene_signature = m_pathtrace_geom_signature;
        }

        if (m_pathtrace_gpu_instances.empty() || m_pathtrace_transform_signature != transform_signature) {
            _update_pathtrace_instances_and_tlas(transform_signature);
        }

        if (m_pathtrace_gpu_instances.empty())
            return false;


        const float active_orbit_scale = std::round(m_pathtrace_dynamic_orbit_scale * 20.0f) * 0.05f;

        // Full render size, at most 1440 pixels on the long side, and the interactive size within it.
        const uint32_t canvas_width = uint32_t(std::max(1, canvas_size.get_width()));
        const uint32_t canvas_height = uint32_t(std::max(1, canvas_size.get_height()));
        const float full_scale = std::min(1.0f, 1440.0f / float(std::max(canvas_width, canvas_height)));
        const uint32_t full_width = std::max(1u, uint32_t(std::lround(float(canvas_width) * full_scale)));
        const uint32_t full_height = std::max(1u, uint32_t(std::lround(float(canvas_height) * full_scale)));
        const uint32_t interactive_width = std::clamp(uint32_t(std::lround(float(full_width) * active_orbit_scale)), 1u, full_width);
        const uint32_t interactive_height = std::clamp(uint32_t(std::lround(float(full_height) * active_orbit_scale)), 1u, full_height);

        // The images only follow the canvas size: switching between the interactive and the full
        // resolution passes reallocates nothing.
        if (m_pathtrace_gpu_accumulation_image == 0 ||
            m_pathtrace_gpu_image_size[0] != full_width ||
            m_pathtrace_gpu_image_size[1] != full_height) {
            const auto allocate_image = [full_width, full_height](unsigned int& tex, GLint internal_format, GLenum type, GLint filter) {
                if (tex == 0)
                    glsafe(::glGenTextures(1, &tex));
                glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
                glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter));
                glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter));
                glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
                glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
                glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, internal_format, GLsizei(full_width), GLsizei(full_height), 0, GL_RGBA, type, nullptr));
            };
            glsafe(::glActiveTexture(GL_TEXTURE0));
            allocate_image(m_pathtrace_gpu_accumulation_image, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_trans_accumulation_image, GL_RGBA32F, GL_FLOAT, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_aux_normal_depth_image, GL_RGBA16F, GL_FLOAT, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_aux_albedo_image, GL_RGBA8, GL_UNSIGNED_BYTE, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_moment_image, GL_R32F, GL_FLOAT, GL_NEAREST);
            for (unsigned int& tex : m_pathtrace_gpu_denoise_images)
                allocate_image(tex, GL_RGBA16F, GL_FLOAT, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_display_image, GL_RGBA8, GL_UNSIGNED_BYTE, GL_LINEAR);
            allocate_image(m_pathtrace_gpu_trans_display_image, GL_RGBA8, GL_UNSIGNED_BYTE, GL_LINEAR);
            allocate_image(m_pathtrace_gpu_interactive_display_image, GL_RGBA8, GL_UNSIGNED_BYTE, GL_LINEAR);
            allocate_image(m_pathtrace_gpu_interactive_trans_display_image, GL_RGBA8, GL_UNSIGNED_BYTE, GL_LINEAR);
            allocate_image(m_pathtrace_gpu_history_images[0], GL_RGBA32F, GL_FLOAT, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_history_images[1], GL_RGBA32F, GL_FLOAT, GL_NEAREST);
            allocate_image(m_pathtrace_gpu_history_images[2], GL_RGBA16F, GL_FLOAT, GL_NEAREST);
            glsafe(::glBindTexture(GL_TEXTURE_2D, 0));

            m_pathtrace_gpu_image_size = {full_width, full_height};
            m_pathtrace_gpu_interactive_size = {0, 0};
            m_pathtrace_history.valid = false;
            _reset_pathtrace_accumulation();
        }

        const uint32_t target_max_samples = interactive ? 1u : (cfg_max_samples + 1u);
        // Nothing on screen yet after a reset: start with the cheap interactive pass, never with a
        // full resolution frame in a single dispatch.
        const bool interactive_pass = interactive || m_pathtrace_gpu_samples == 0;

        if (!interactive && m_pathtrace_gpu_samples >= 1 && ImGui::IsAnyItemActive()) {
            schedule_extra_frame(16);
            _render_pathtrace_composite();
            return true;
        }

        // Sections of the frame profiler, so that the overlay splits the path tracing time.
        m_frame_profiler.mark("pt setup");
        bool resolve_interactive = false;
        bool resolve_full = false;
        bool interactive_history = false;
        if (m_pathtrace_gpu_samples < target_max_samples) {
            PathTraceCamera camera;
            camera.position = source_camera.get_position().cast<float>();
            camera.right = source_camera.get_dir_right().cast<float>();
            camera.up = source_camera.get_dir_up().cast<float>();
            camera.forward = source_camera.get_dir_forward().cast<float>();
            camera.orthographic = source_camera.get_type() == Camera::EType::Ortho;
            if (camera.orthographic)
                camera.orthographic_height = float(source_camera.get_near_height());
            else
                camera.vertical_fov_degrees = float(source_camera.get_fov());

            glsafe(::glBindImageTexture(0, m_pathtrace_gpu_accumulation_image, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F));
            glsafe(::glBindImageTexture(1, m_pathtrace_gpu_aux_normal_depth_image, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F));
            glsafe(::glBindImageTexture(2, m_pathtrace_gpu_trans_accumulation_image, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F));
            glsafe(::glBindImageTexture(3, m_pathtrace_gpu_aux_albedo_image, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8));
            glsafe(::glBindImageTexture(4, m_pathtrace_gpu_moment_image, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F));

            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_pathtrace_gpu_node_buffer));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_pathtrace_gpu_triangle_buffer));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, m_pathtrace_gpu_instance_buffer));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, m_pathtrace_gpu_shading_buffer));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, m_pathtrace_gpu_normal_buffer));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, m_pathtrace_gpu_tlas_buffer));

            const uint32_t render_width = interactive_pass ? interactive_width : full_width;
            const uint32_t render_height = interactive_pass ? interactive_height : full_height;
            const Vec3f camera_right = camera.right.normalized();
            const Vec3f camera_up = camera.up.normalized();
            const Vec3f camera_forward = camera.forward.normalized();
            compute_shader->start_using();
            compute_shader->set_uniform("image_size", std::array<int, 2>{int(render_width), int(render_height)});
            compute_shader->set_uniform("camera_position", camera.position);
            compute_shader->set_uniform("camera_right", camera_right);
            compute_shader->set_uniform("camera_up", camera_up);
            compute_shader->set_uniform("camera_forward", camera_forward);
            compute_shader->set_uniform("camera_half_fov",
                camera.orthographic ? 0.0f : std::tan(camera.vertical_fov_degrees * 3.14159265358979323846f / 360.0f));
            compute_shader->set_uniform("camera_orthographic_height", camera.orthographic_height);
            compute_shader->set_uniform("camera_orthographic", camera.orthographic);
            // The interactive pass runs the same path tracer as the progressive one, so that the image
            // keeps its look while orbiting, with one indirect bounce.
            compute_shader->set_uniform("max_bounces", interactive_pass ? std::min(cfg_bounces, 1) : cfg_bounces);
            compute_shader->set_uniform("z_min", preview_z_min);
            compute_shader->set_uniform("z_max", preview_z_max);
            compute_shader->set_uniform("move_min", preview_move_min);
            compute_shader->set_uniform("move_max", preview_move_max);
            compute_shader->set_uniform("clipping_plane", clipping_active
                ? std::array<float, 4>{float(clip_plane.get_data()[0]), float(clip_plane.get_data()[1]),
                                       float(clip_plane.get_data()[2]), float(clip_plane.get_data()[3])}
                : std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
            const Vec3f light_dir = _key_light_dir_world(source_camera.get_view_matrix().matrix().block<3, 3>(0, 0)).cast<float>();
            // The front light of the shaders, which turns with the camera.
            const Vec3f fill_dir = (0.6985074f * camera_right + 0.1397015f * camera_up - 0.6985074f * camera_forward).normalized();
            compute_shader->set_uniform("light_direction_uniform", light_dir);
            compute_shader->set_uniform("fill_direction_uniform", fill_dir);
            compute_shader->set_uniform("glossy_toolpaths", glossy_toolpaths);
            compute_shader->set_uniform("shadows_enabled", _shadow_mode() != EShadowMode::Off);
            compute_shader->set_uniform("layer_profile_mode", (m_canvas_type == ECanvasType::CanvasPreview) ? 1 : 0);
            compute_shader->set_uniform("is_preview_canvas", m_canvas_type == ECanvasType::CanvasPreview);
            compute_shader->set_uniform("bed_hidden", !source_camera.is_looking_downward());
            compute_shader->set_uniform("lit_layer_z", std::array<float, 2>{ layer_shading_z[0], layer_shading_z[1] });
            compute_shader->set_uniform("grey_below_z", layer_shading_z[2]);
            compute_shader->set_uniform("layer_dim", layer_shading.dim_brightness);
            compute_shader->set_uniform("kept_vertex", layer_shading.kept_vertex);
            compute_shader->set_uniform("instance_count", int(m_pathtrace_gpu_instances.size()));
            compute_shader->set_uniform("has_vertex_normals", m_pathtrace_gpu_scene.normals.size() == m_pathtrace_gpu_scene.triangles.size() &&
                                                              !m_pathtrace_gpu_scene.normals.empty());
            compute_shader->set_uniform("max_trans_bounces", max_trans_bounces);
            compute_shader->set_uniform("shadow_max_hits", shadow_max_hits);
            compute_shader->set_uniform("quality_preset", quality_preset);

            if (m_pathtrace_gpu_timer_queries[0] == 0) {
                glsafe(::glGenQueries(GLsizei(m_pathtrace_gpu_timer_queries.size()), m_pathtrace_gpu_timer_queries.data()));
                m_pathtrace_gpu_timer_query_in_flight[0] = false;
                m_pathtrace_gpu_timer_query_in_flight[1] = false;
            }

            // Read back the GPU time of earlier dispatches. The slot about to be reused was written
            // two frames ago: still unread, it means the GPU runs that far behind, and its time is
            // waited for rather than dropped. Dropped, it left the scheduler on a stale reading, and
            // the scheduler kept adding tiles while frames grew past 100 ms.
            const int read_query_idx = 1 - m_pathtrace_gpu_timer_query_idx;
            const int write_query_idx = m_pathtrace_gpu_timer_query_idx;
            for (const int slot : { write_query_idx, read_query_idx }) {
                if (!m_pathtrace_gpu_timer_query_in_flight[slot])
                    continue;
                GLint available = 1;
                if (slot == read_query_idx)
                    glsafe(::glGetQueryObjectiv(m_pathtrace_gpu_timer_queries[2 * slot + 1], GL_QUERY_RESULT_AVAILABLE, &available));
                if (!available)
                    continue;
                GLuint64 begin_ns = 0;
                GLuint64 end_ns = 0;
                glsafe(::glGetQueryObjectui64v(m_pathtrace_gpu_timer_queries[2 * slot], GL_QUERY_RESULT, &begin_ns));
                glsafe(::glGetQueryObjectui64v(m_pathtrace_gpu_timer_queries[2 * slot + 1], GL_QUERY_RESULT, &end_ns));
                const float elapsed_ms = float(double(end_ns > begin_ns ? end_ns - begin_ns : 0) * 1e-6);
                m_pathtrace_gpu_dispatch_time_ms = elapsed_ms;
                if (m_pathtrace_gpu_timer_is_interactive[slot]) {
                    m_pathtrace_gpu_dispatch_interactive_ms = elapsed_ms;
                    // The cost of an interactive frame follows its pixel count, so the scale that meets
                    // the 8 ms target follows from the one measured at once. It drops straight there,
                    // which takes one frame instead of a dozen when orbiting starts at a high scale, and
                    // grows back gradually.
                    if (elapsed_ms > 0.2f && full_width > 0) {
                        const float measured_scale = float(m_pathtrace_gpu_timer_res[slot][0]) / float(full_width);
                        const float ideal_scale = measured_scale * std::sqrt(8.0f / elapsed_ms);
                        const float next_scale = ideal_scale < m_pathtrace_dynamic_orbit_scale
                            ? ideal_scale
                            : m_pathtrace_dynamic_orbit_scale + 0.25f * (ideal_scale - m_pathtrace_dynamic_orbit_scale);
                        m_pathtrace_dynamic_orbit_scale = std::clamp(next_scale, min_orbit_scale, max_orbit_scale);
                    }
                } else {
                    m_pathtrace_gpu_dispatch_full_res_ms = elapsed_ms;
                    if (m_pathtrace_gpu_timer_tiles[slot] > 0)
                        m_pathtrace_gpu_ms_per_tile = elapsed_ms / float(m_pathtrace_gpu_timer_tiles[slot]);
                }
                m_pathtrace_gpu_timer_query_in_flight[slot] = false;
            }

            m_pathtrace_gpu_timer_is_interactive[write_query_idx] = interactive_pass;
            m_pathtrace_gpu_timer_res[write_query_idx] = {render_width, render_height};

            constexpr uint32_t PT_TILE_SIZE = 128u;

            if (interactive_pass) {
                // Reproject the previous interactive frame, unless the scene changed since.
                const bool use_history = m_pathtrace_history.valid && m_pathtrace_history.scene_signature == history_signature;
                interactive_history = use_history;
                const PathTraceCamera& history_camera = m_pathtrace_history.camera;
                compute_shader->set_uniform("history_valid", use_history);
                compute_shader->set_uniform("interactive_frame", int(++m_pathtrace_history.frame & 0xFFFFu));
                compute_shader->set_uniform("history_size", std::array<int, 2>{int(m_pathtrace_history.size[0]), int(m_pathtrace_history.size[1])});
                compute_shader->set_uniform("history_camera_position", history_camera.position);
                compute_shader->set_uniform("history_camera_right", history_camera.right);
                compute_shader->set_uniform("history_camera_up", history_camera.up);
                compute_shader->set_uniform("history_camera_forward", history_camera.forward);
                compute_shader->set_uniform("history_camera_half_fov",
                    history_camera.orthographic ? 0.0f : std::tan(history_camera.vertical_fov_degrees * 3.14159265358979323846f / 360.0f));
                compute_shader->set_uniform("history_camera_orthographic_height", history_camera.orthographic_height);
                compute_shader->set_uniform("history_camera_orthographic", history_camera.orthographic);
                compute_shader->set_uniform("history_accumulation_tex", 0);
                compute_shader->set_uniform("history_bed_tex", 1);
                compute_shader->set_uniform("history_normal_depth_tex", 2);
                for (size_t i = 0; i < m_pathtrace_gpu_history_images.size(); ++i) {
                    glsafe(::glActiveTexture(GL_TEXTURE0 + GLenum(i)));
                    glsafe(::glBindTexture(GL_TEXTURE_2D, m_pathtrace_gpu_history_images[i]));
                }
                glsafe(::glActiveTexture(GL_TEXTURE0));

                compute_shader->set_uniform("is_interactive_pass", true);
                compute_shader->set_uniform("tile_offset", std::array<int, 2>{0, 0});
                compute_shader->set_uniform("tile_size", std::array<int, 2>{int(render_width), int(render_height)});
                compute_shader->set_uniform("sample_start", 0);
                compute_shader->set_uniform("sample_count", 1);

                glsafe(::glQueryCounter(m_pathtrace_gpu_timer_queries[2 * write_query_idx], GL_TIMESTAMP));

                const GLuint groups_x = (render_width + 7u) / 8u;
                const GLuint groups_y = (render_height + 7u) / 8u;
                glsafe(::glDispatchCompute(groups_x, groups_y, 1));
                glsafe(::glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));


                m_pathtrace_gpu_samples = 1;
                m_pathtrace_tile_cursor = 0;
                m_pathtrace_last_dispatched_tiles = 0;
                m_pathtrace_tiles_per_frame = 4u;
                m_pathtrace_gpu_dispatch_full_res_ms = 0.0f;
                m_pathtrace_gpu_ms_per_tile = 0.0f;
                m_pathtrace_gpu_timer_tiles[write_query_idx] = 0;
                m_pathtrace_gpu_interactive_size = {render_width, render_height};
                resolve_interactive = true;

                for (int i = 2; i >= 0; --i) {
                    glsafe(::glActiveTexture(GL_TEXTURE0 + GLenum(i)));
                    glsafe(::glBindTexture(GL_TEXTURE_2D, 0));
                }
                // This frame becomes the history of the next one.
                glsafe(::glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT));
                const std::array<unsigned int, 3> history_sources = { m_pathtrace_gpu_accumulation_image, m_pathtrace_gpu_trans_accumulation_image,
                                                                       m_pathtrace_gpu_aux_normal_depth_image };
                for (size_t i = 0; i < history_sources.size(); ++i)
                    glsafe(::glCopyImageSubData(history_sources[i], GL_TEXTURE_2D, 0, 0, 0, 0,
                                                m_pathtrace_gpu_history_images[i], GL_TEXTURE_2D, 0, 0, 0, 0,
                                                GLsizei(render_width), GLsizei(render_height), 1));
                m_pathtrace_history.valid = true;
                m_pathtrace_history.scene_signature = history_signature;
                m_pathtrace_history.size = {render_width, render_height};
                m_pathtrace_history.camera = camera;
                m_pathtrace_history.camera.right = camera_right;
                m_pathtrace_history.camera.up = camera_up;
                m_pathtrace_history.camera.forward = camera_forward;
            } else {
                const uint32_t tiles_x = (render_width + PT_TILE_SIZE - 1u) / PT_TILE_SIZE;
                const uint32_t tiles_y = (render_height + PT_TILE_SIZE - 1u) / PT_TILE_SIZE;
                const uint32_t total_tiles = tiles_x * tiles_y;

                uint32_t stride = std::max(1u, uint32_t(std::lround(float(total_tiles) * 0.6180339887f)));
                while (std::gcd(stride, total_tiles) != 1u) {
                    ++stride;
                }

                if (m_pathtrace_gpu_ms_per_tile > 0.0f) {
                    // Grow smoothly, but cut back at once when over budget.
                    const uint32_t ideal_tiles = std::clamp(uint32_t(std::floor(m_pathtrace_tile_budget_ms / std::max(0.05f, m_pathtrace_gpu_ms_per_tile))), 1u, total_tiles);
                    m_pathtrace_tiles_per_frame = ideal_tiles < m_pathtrace_tiles_per_frame
                        ? ideal_tiles
                        : std::clamp(uint32_t(std::lround(0.7f * m_pathtrace_tiles_per_frame + 0.3f * ideal_tiles)), 1u, total_tiles);
                }

                compute_shader->set_uniform("is_interactive_pass", false);
                compute_shader->set_uniform("sample_start", int(m_pathtrace_gpu_samples));
                compute_shader->set_uniform("sample_count", 1);

                glsafe(::glQueryCounter(m_pathtrace_gpu_timer_queries[2 * write_query_idx], GL_TIMESTAMP));

                uint32_t count = 0;
                for (; count < m_pathtrace_tiles_per_frame && m_pathtrace_tile_cursor < total_tiles; ++count, ++m_pathtrace_tile_cursor) {
                    const uint32_t tile_idx = (m_pathtrace_tile_cursor * stride) % total_tiles;
                    const uint32_t tx = tile_idx % tiles_x;
                    const uint32_t ty = tile_idx / tiles_x;
                    const uint32_t ox = tx * PT_TILE_SIZE;
                    const uint32_t oy = ty * PT_TILE_SIZE;
                    const uint32_t tw = std::min(PT_TILE_SIZE, render_width - ox);
                    const uint32_t th = std::min(PT_TILE_SIZE, render_height - oy);

                    compute_shader->set_uniform("tile_offset", std::array<int, 2>{int(ox), int(oy)});
                    compute_shader->set_uniform("tile_size", std::array<int, 2>{int(tw), int(th)});
                    glsafe(::glDispatchCompute((tw + 7u) / 8u, (th + 7u) / 8u, 1));
                }

                m_pathtrace_last_dispatched_tiles = count;
                m_pathtrace_gpu_timer_tiles[write_query_idx] = count;

                glsafe(::glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT));

                if (m_pathtrace_tile_cursor >= total_tiles) {
                    m_pathtrace_tile_cursor = 0;
                    m_pathtrace_gpu_samples += 1;
                    // The display only takes whole samples, so it never shows tiles at different sample counts.
                    resolve_full = true;
                }
            }

            compute_shader->stop_using();

            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, 0));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, 0));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, 0));
            glsafe(::glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, 0));
            glsafe(::glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F));
            glsafe(::glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA16F));
            glsafe(::glBindImageTexture(2, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F));
            glsafe(::glBindImageTexture(3, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA8));
            glsafe(::glBindImageTexture(4, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32F));
            // The end timestamp goes after the images are unbound: right after the dispatches the
            // driver may write it before they have run, which made a 100 ms frame read as 6 ms.
            glsafe(::glQueryCounter(m_pathtrace_gpu_timer_queries[2 * write_query_idx + 1], GL_TIMESTAMP));
            m_pathtrace_gpu_timer_query_in_flight[write_query_idx] = true;
            m_pathtrace_gpu_timer_query_idx = read_query_idx;
        }

        m_frame_profiler.mark("pt trace");
        if (resolve_interactive)
            // Blended with its history, an interactive frame holds a few samples per pixel.
            _resolve_pathtrace_image(*denoise_shader, true, interactive_width, interactive_height, interactive_history ? 4u : 1u);
        else if (resolve_full)
            _resolve_pathtrace_image(*denoise_shader, false, full_width, full_height, m_pathtrace_gpu_samples - 1u);
        m_frame_profiler.mark("pt denoise");

        if (m_pathtrace_gpu_samples < target_max_samples) {
            schedule_extra_frame(1);
        } else if (recent_change) {
            schedule_extra_frame(60);
        }

        _render_pathtrace_composite();
        m_frame_profiler.mark("pt composite");
        return true;
    }
    return false;
}

} // namespace GUI
} // namespace Slic3r

