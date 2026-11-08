#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <utility>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/PathTracer.hpp"
#include "libslic3r/Point.hpp"

namespace Slic3r {

class GLShaderProgram;

namespace GUI {

class GLCanvas3D;
class ImGuiWrapper;
class PartPlate;
class PartPlateList;
class Size;
struct Camera;

// Draws a canvas by GPU path tracing, see docs/HLSD/path-tracer.md. Everything of the path tracer lives
// here, and a friend of GLCanvas3D, it reads the volumes, toolpaths and view state of its canvas.
class PathTraceRenderer
{
public:
    explicit PathTraceRenderer(GLCanvas3D& canvas) : m_canvas(canvas) {}

    // Draws the scene over the rasterized bed, false if the rasterizer has to draw it instead.
    bool render(const Camera& source_camera, const Size& canvas_size);
    // Whether the image on screen is final, so that the scene cache may keep it.
    bool is_frame_final() const;
    void render_fps_overlay(ImGuiWrapper& imgui) const;
    void stop_worker();
    void release_gl_resources();
    // Frees the scene, so that it is built anew when next path traced. Without a current OpenGL context
    // its GL objects stay until this canvas next renders, which frees or overwrites them.
    void release_scene(bool gl_current = true);

private:
    struct QualitySettings {
        uint32_t cfg_max_samples;
        int cfg_bounces;
        int max_trans_bounces;
        int shadow_max_hits;
        int quality_preset;
        // GPU time per frame of the progressive passes, and the range of the interactive resolution.
        float tile_budget_ms;
        float min_orbit_scale;
        float max_orbit_scale;
    };

    GLCanvas3D& m_canvas;

    size_t m_requested_signature{0};
    PathTraceGpuScene m_gpu_scene;
    // Whether the uploaded scene has shading normals, which do not stay on the CPU.
    bool m_gpu_has_vertex_normals{false};
    std::vector<size_t> m_gpu_tri_vertex_id;
    size_t m_preview_color_signature{0};
    std::chrono::steady_clock::time_point m_last_change_time{};
    unsigned int m_gpu_node_buffer{0};
    unsigned int m_gpu_triangle_buffer{0};
    unsigned int m_gpu_shading_buffer{0};
    // Shading normals of the triangles, Prepare only: toolpaths have their own bead normals.
    unsigned int m_gpu_normal_buffer{0};
    unsigned int m_gpu_instance_buffer{0};
    unsigned int m_gpu_tlas_buffer{0};
    unsigned int m_gpu_accumulation_image{0};
    unsigned int m_gpu_display_image{0};
    unsigned int m_gpu_trans_accumulation_image{0};
    unsigned int m_gpu_trans_display_image{0};
    // Denoiser inputs (first surface normal and distance, albedo and class) and its ping-pong images
    // (color A, color B, bed A, bed B).
    unsigned int m_gpu_aux_normal_depth_image{0};
    unsigned int m_gpu_aux_albedo_image{0};
    // Sum of the squared luminance of the samples, for the variance the denoiser follows.
    unsigned int m_gpu_moment_image{0};
    std::array<unsigned int, 4> m_gpu_denoise_images{0, 0, 0, 0};
    // Display images of the reduced resolution interactive pass. They stay on screen until the first
    // full resolution sample is complete, so that releasing the mouse never stalls on a full frame.
    unsigned int m_gpu_interactive_display_image{0};
    unsigned int m_gpu_interactive_trans_display_image{0};
    // All path tracing images are allocated at the full render size; the interactive pass renders
    // into their top left corner, of this size.
    std::array<unsigned int, 2> m_gpu_interactive_size{0, 0};
    // The previous interactive frame, which the next one reprojects and blends with: copies of its
    // accumulation, bed term, first surfaces and their class, and the camera and size it was rendered
    // with. Valid while nothing but the camera changed since.
    std::array<unsigned int, 4> m_gpu_history_images{0, 0, 0, 0};
    struct History {
        bool valid{false};
        size_t scene_signature{0};
        std::array<unsigned int, 2> size{0, 0};
        PathTraceCamera camera;
        uint32_t frame{0};
    };
    History m_history;
    std::array<unsigned int, 2> m_gpu_image_size{0, 0};
    size_t m_gpu_scene_signature{0};
    size_t m_geom_signature{0};
    size_t m_transform_signature{0};
    struct VolumeBlas {
        uint32_t root_node{0};
        uint32_t node_count{0};
        uint32_t tri_offset{0};
        uint32_t tri_count{0};
        Vec3f local_bounds_min{Vec3f::Zero()};
        Vec3f local_bounds_max{Vec3f::Zero()};
        // The part this BLAS holds, for all the instances of its object, see pathtrace_part_key(). Object
        // indices go stale when an arrange or an undo reorders the objects of the model, which keeps
        // the meshes and so this BLAS. Zero for the toolpaths and the plates.
        std::pair<size_t, size_t> part_key{0, 0};
        int plate_idx{-1};
    };
    std::vector<VolumeBlas> m_volume_blas;
    std::vector<PathTraceGpuInstance> m_gpu_instances;
    uint32_t m_gpu_samples{0};
    uint32_t m_max_samples{2048};
    bool m_is_interactive{false};
    // Begin and end timestamps of the path tracing dispatches, double buffered. Timestamps, because a
    // GL_TIME_ELAPSED query around compute dispatches, like a timestamp right after them, can measure
    // only their submission.
    std::array<unsigned int, 4> m_gpu_timer_queries{0, 0, 0, 0};
    int m_gpu_timer_query_idx{0};
    bool m_gpu_timer_query_in_flight[2]{false, false};
    bool m_gpu_timer_is_interactive[2]{false, false};
    std::array<unsigned int, 2> m_gpu_timer_res[2]{{0, 0}, {0, 0}};
    // Tiles dispatched in each timer slot, and the GPU time per tile they measured.
    std::array<uint32_t, 2> m_gpu_timer_tiles{0, 0};
    float m_gpu_ms_per_tile{0.0f};
    float m_gpu_dispatch_time_ms{0.0f};
    float m_gpu_dispatch_full_res_ms{0.0f};
    float m_gpu_dispatch_interactive_ms{0.0f};
    uint32_t m_tile_cursor{ 0 };
    uint32_t m_tiles_per_frame{ 6 };
    // Starts at the lowest scale of the presets: nothing has been timed yet for the first dispatch, which
    // could otherwise stall a weak GPU.
    float    m_dynamic_orbit_scale{ 0.15f };
    struct SceneBuildResult {
        size_t geom_signature{0};
        size_t color_signature{0};
        PathTraceGpuScene gpu_scene;
        std::vector<VolumeBlas> volume_blas;
        std::vector<size_t> gpu_tri_vertex_id;
    };
    struct BvhJob {
        size_t geom_signature{0};
        // The colors the triangles are extracted with.
        size_t color_signature{0};
        bool is_preview{false};
        // Subsurface scattering family of each extruder's filament, read from the presets on the GUI thread.
        std::vector<int> extruder_sss_families;
        std::vector<PathTraceTriangle> preview_triangles;
        std::vector<size_t> source_tri_vertex_id;
        std::vector<float> source_tri_tag_z;
        std::vector<float> source_tri_height;
        std::vector<float> source_tri_role;
        std::vector<float> source_tri_facet;
        std::vector<float> source_tri_delta_angle;
        std::vector<float> source_tri_opacity;
        struct VolumeGroupTriangles {
            std::pair<size_t, size_t> part_key{0, 0};
            std::vector<PathTraceTriangle> triangles;
            std::vector<uint32_t> triangle_extruder_ids;
        };
        std::vector<VolumeGroupTriangles> volume_groups;
        struct PlateTriangles {
            int plate_idx{-1};
            std::vector<PathTraceTriangle> triangles;
        };
        std::vector<PlateTriangles> plate_groups;
    };
    static SceneBuildResult build_scene_async(BvhJob job, std::shared_ptr<std::atomic<bool>> cancel_token);
    std::future<SceneBuildResult> m_bvh_future;
    std::shared_ptr<std::atomic<bool>> m_bvh_cancel_token;
    size_t m_bvh_job_signature{0};

    // Marks everything on the GPU as never uploaded.
    void forget_uploads();
    void free_cpu_scene();
    void reset_accumulation();
    bool give_up_on_scene(size_t geom_signature, const char* reason);
    QualitySettings quality_settings() const;
    void extract_preview_toolpaths(BvhJob& job, bool plate_visible, PartPlate* current_plate) const;
    void extract_prepare_volumes(BvhJob& job, const std::vector<ColorRGBA>& extruder_colors, bool plate_visible, PartPlateList& plate_list) const;
    void update_scene_colors(const std::vector<ColorRGBA>& extruder_colors, const std::vector<int>& sss_families, size_t preview_color_signature);
    bool upload_scene(size_t geom_signature);
    bool allocate_images(uint32_t full_width, uint32_t full_height, size_t geom_signature);
    void update_instances_and_tlas(size_t transform_signature);
    void read_back_gpu_times(int write_query_idx, int read_query_idx, uint32_t full_width, float min_orbit_scale, float max_orbit_scale);
    // Resolves the accumulation of the last pass into the interactive or the full resolution display
    // images, through the denoiser.
    void resolve_image(GLShaderProgram& shader, bool interactive, uint32_t width, uint32_t height, uint32_t samples);
    // Composites the path traced display images over the rasterized bed.
    void render_composite();
};

} // namespace GUI
} // namespace Slic3r
