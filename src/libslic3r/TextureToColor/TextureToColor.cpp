#include "TextureToColor.hpp"

#include <tbb/parallel_for.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <set>

#include <boost/next_prior.hpp>
#include "CgalUtils.hpp"
#include "ColorUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include <filesystem>
#include <fstream>
#include "Repair.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace tex2color {

using namespace color_utils;

// #define OUTPUT_TEST_RESULT

static void SaveToOFF(const std::string& path, const TriMesh& mesh, const std::vector<RGB>& face_colors)
{
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream ofs(path);
    if (!ofs.is_open()) {
        BOOST_LOG_TRIVIAL(warning) << "SaveToOFF: failed to open " << path;
        return;
    }

    const auto& vertices = mesh.vertices;
    const auto& faces = mesh.indices;

    ofs << "OFF\n";
    ofs << vertices.size() << " " << faces.size() << " 0\n";

    for (const auto& v : vertices) {
        ofs << v.x() << " " << v.y() << " " << v.z() << "\n";
    }

    for (std::size_t i = 0; i < faces.size(); ++i) {
        const auto& f = faces[i];
        ofs << "3 " << f[0] << " " << f[1] << " " << f[2];
        if (i < face_colors.size()) {
            ofs << " " << face_colors[i][0] / 255.0
                << " " << face_colors[i][1] / 255.0
                << " " << face_colors[i][2] / 255.0
                << " 1.0";
        }
        ofs << "\n";
    }
}

static std::vector<std::size_t> count_cluster_label_usage(const std::vector<std::size_t>& face_labels, std::size_t cluster_count)
{
    std::vector<std::size_t> usage(cluster_count, 0);
    for (std::size_t label : face_labels) {
        if (label < cluster_count) {
            ++usage[label];
        }
    }
    return usage;
}

static bool discard_unused_cluster_centers(std::vector<RGB>& cluster_centers, std::vector<std::size_t>& face_labels, const char* stage_name)
{
    if (cluster_centers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cannot discard unused cluster centers at " << stage_name
                                   << ", no cluster center is available.";
        return false;
    }

    const std::vector<std::size_t> usage = count_cluster_label_usage(face_labels, cluster_centers.size());
    std::vector<std::size_t> label_remap(cluster_centers.size(), std::numeric_limits<std::size_t>::max());
    std::vector<RGB> used_cluster_centers;
    used_cluster_centers.reserve(cluster_centers.size());

    for (std::size_t cluster_id = 0; cluster_id < cluster_centers.size(); ++cluster_id) {
        if (usage[cluster_id] == 0) {
            continue;
        }
        label_remap[cluster_id] = used_cluster_centers.size();
        used_cluster_centers.push_back(cluster_centers[cluster_id]);
    }

    if (used_cluster_centers.size() == cluster_centers.size()) {
        return true;
    }
    if (used_cluster_centers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cannot discard unused cluster centers at " << stage_name
                                   << ", no face uses any valid cluster center.";
        return false;
    }

    for (std::size_t& label : face_labels) {
        if (label >= label_remap.size() || label_remap[label] == std::numeric_limits<std::size_t>::max()) {
            BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cannot remap cluster label " << label
                                       << " at " << stage_name << ".";
            return false;
        }
        label = label_remap[label];
    }

    BOOST_LOG_TRIVIAL(debug) << "TextureToColor: discarded " << (cluster_centers.size() - used_cluster_centers.size())
                             << " unused adaptive cluster centers at " << stage_name << ".";
    cluster_centers = std::move(used_cluster_centers);
    return true;
}

static bool ensure_all_cluster_centers_used(const std::vector<RGB>& source_face_colors, const std::vector<RGB>& cluster_centers,
                                           std::vector<std::size_t>& face_labels, const char* stage_name)
{
    if (source_face_colors.size() != face_labels.size()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cannot preserve cluster colors at " << stage_name
                                   << ", face color count does not match label count.";
        return false;
    }
    if (cluster_centers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cannot preserve cluster colors at " << stage_name
                                   << ", no cluster center is available.";
        return false;
    }
    if (cluster_centers.size() > face_labels.size()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cannot use all cluster centers at " << stage_name
                                   << ", centers=" << cluster_centers.size() << " faces=" << face_labels.size() << ".";
        return false;
    }

    std::vector<std::size_t> usage = count_cluster_label_usage(face_labels, cluster_centers.size());
    std::size_t missing_count = 0;
    for (std::size_t cluster_id = 0; cluster_id < usage.size(); ++cluster_id) {
        if (usage[cluster_id] != 0) {
            continue;
        }
        ++missing_count;

        double best_cost = std::numeric_limits<double>::max();
        std::size_t best_face_id = std::numeric_limits<std::size_t>::max();
        std::size_t best_old_cluster_id = std::numeric_limits<std::size_t>::max();

        for (std::size_t fid = 0; fid < face_labels.size(); ++fid) {
            const std::size_t old_cluster_id = face_labels[fid];
            if (old_cluster_id >= cluster_centers.size() || usage[old_cluster_id] <= 1) {
                continue;
            }

            const double old_dist = calc_rgb_color_difference_by_ciede2000(source_face_colors[fid], cluster_centers[old_cluster_id]);
            const double new_dist = calc_rgb_color_difference_by_ciede2000(source_face_colors[fid], cluster_centers[cluster_id]);
            const double cost = new_dist - old_dist;
            if (cost < best_cost) {
                best_cost = cost;
                best_face_id = fid;
                best_old_cluster_id = old_cluster_id;
            }
        }

        if (best_face_id == std::numeric_limits<std::size_t>::max()) {
            BOOST_LOG_TRIVIAL(warning) << "TextureToColor: failed to assign a seed face for unused cluster " << cluster_id
                                       << " at " << stage_name << ".";
            continue;
        }

        face_labels[best_face_id] = cluster_id;
        --usage[best_old_cluster_id];
        ++usage[cluster_id];
    }

    if (missing_count > 0) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: reassigned seed faces for " << missing_count
                                 << " unused cluster centers at " << stage_name << ".";
    }

    for (std::size_t count : usage) {
        if (count == 0) {
            return false;
        }
    }
    return true;
}

// Bilinear interpolation texture sampling; sub-pixel precision avoids nearest-neighbor aliasing
static RGB get_pixel_color(float u, float v, const cv::Mat& texture) {
    u = u - std::floor(u);
    v = v - std::floor(v);

    // glTF UV convention: (0,0) = top-left, v increases downward
    float fx = u * (texture.cols - 1);
    float fy = v * (texture.rows - 1);

    int x0 = std::clamp(static_cast<int>(fx), 0, texture.cols - 1);
    int y0 = std::clamp(static_cast<int>(fy), 0, texture.rows - 1);
    int x1 = std::min(x0 + 1, texture.cols - 1);
    int y1 = std::min(y0 + 1, texture.rows - 1);

    float wx = fx - x0;
    float wy = fy - y0;

    const int ch = texture.channels();
    auto sample = [&](int row, int col) -> std::array<float, 3> {
        const uchar* ptr = texture.data + row * texture.step[0] + col * ch;
        return {static_cast<float>(ptr[2]), static_cast<float>(ptr[1]), static_cast<float>(ptr[0])};
    };

    auto c00 = sample(y0, x0);
    auto c10 = sample(y0, x1);
    auto c01 = sample(y1, x0);
    auto c11 = sample(y1, x1);

    // Bilinear blend: lerp(lerp(c00,c10,wx), lerp(c01,c11,wx), wy)
    RGB color;
    for (int i = 0; i < 3; ++i) {
        float top = c00[i] * (1.0f - wx) + c10[i] * wx;
        float bot = c01[i] * (1.0f - wx) + c11[i] * wx;
        color[i] = static_cast<std::size_t>(std::clamp(top * (1.0f - wy) + bot * wy, 0.0f, 255.0f));
    }
    return color;
}

// 7-point triangular Gaussian quadrature barycentric coordinates and weights (precision sufficient for capturing texture detail within faces)
static constexpr std::array<std::array<float, 3>, 7> GAUSS_TRI_BARY = {{
    {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f},
    {0.059715871f, 0.470142064f, 0.470142064f},
    {0.470142064f, 0.059715871f, 0.470142064f},
    {0.470142064f, 0.470142064f, 0.059715871f},
    {0.797426985f, 0.101286507f, 0.101286507f},
    {0.101286507f, 0.797426985f, 0.101286507f},
    {0.101286507f, 0.101286507f, 0.797426985f},
}};
static constexpr std::array<float, 7> GAUSS_TRI_WEIGHT = {0.225f, 0.132394152f, 0.132394152f, 0.132394152f, 0.125939181f, 0.125939181f, 0.125939181f};
static_assert(
    []() constexpr {
        float sum = 0.0f;
        for (auto w : GAUSS_TRI_WEIGHT) {
            sum += w;
        }
        return sum > 0.999f && sum < 1.001f;
    }(),
    "Sum of Gaussian quadrature weights must be 1.0");

// Multi-point Gaussian quadrature sampling on a single face; returns weighted average color.
// GAUSS_TRI_WEIGHT sums to 1.0 (Hammer quadrature formula), no normalization needed.
static RGB sample_face_color(const std::array<Vec2f, 3>& uvs, const cv::Mat& texture) {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    for (int k = 0; k < 7; ++k) {
        float u = GAUSS_TRI_BARY[k][0] * uvs[0].x() + GAUSS_TRI_BARY[k][1] * uvs[1].x() + GAUSS_TRI_BARY[k][2] * uvs[2].x();
        float v = GAUSS_TRI_BARY[k][0] * uvs[0].y() + GAUSS_TRI_BARY[k][1] * uvs[1].y() + GAUSS_TRI_BARY[k][2] * uvs[2].y();
        RGB c = get_pixel_color(u, v, texture);
        float w = GAUSS_TRI_WEIGHT[k];
        r += w * c[0];
        g += w * c[1];
        b += w * c[2];
    }
    return RGB{static_cast<std::size_t>(std::clamp(r, 0.0f, 255.0f)), static_cast<std::size_t>(std::clamp(g, 0.0f, 255.0f)),
               static_cast<std::size_t>(std::clamp(b, 0.0f, 255.0f))};
}

// Use array<Vec2f,3> instead of vector<Vec2f> for UV storage to avoid per-face heap allocations at million-face scale
using FaceUVArray = std::array<Vec2f, 3>;

static bool linear_subdivision(TriMesh& mesh, std::vector<FaceUVArray>& uv_coords, const std::function<void(int)>& sub_progress = nullptr) {
    const auto& original_vertices = mesh.vertices;
    const auto& original_faces = mesh.indices;
    TriVertices sub_vertices = mesh.vertices;
    sub_vertices.reserve(original_vertices.size() + original_faces.size() * 3);
    TriFaces sub_faces;
    std::vector<FaceUVArray> sub_uv_coords;

    // Single-level flat map with edge key encoding replaces nested unordered_map;
    // merges two vertex indices into a single uint64_t to reduce hash lookups and indirection.
    if (original_vertices.size() >= (1ULL << 32)) [[unlikely]] {
        BOOST_LOG_TRIVIAL(warning) << "[boundary] " << __FUNCTION__ << " vertex_count=" << original_vertices.size() << " exceeds 32-bit edge_key encoding range, skipping subdivision";
        return false;
    }
    auto edge_key = [](std::size_t a, std::size_t b) -> uint64_t {
        return a < b ? ((static_cast<uint64_t>(a) << 32) | b) : ((static_cast<uint64_t>(b) << 32) | a);
    };
    std::unordered_map<uint64_t, std::size_t> map_edge_to_sub_vtx;
    map_edge_to_sub_vtx.reserve(original_faces.size() * 3 / 2);

    for (const auto& face : original_faces) {
        for (std::size_t i = 0; i < 3; ++i) {
            std::size_t vtx_1 = face[i];
            std::size_t vtx_2 = face[(i + 1) % 3];
            uint64_t key = edge_key(vtx_1, vtx_2);
            if (map_edge_to_sub_vtx.count(key) > 0) {
                continue;
            }
            TriVertex edge_vtx = (original_vertices[vtx_1] + original_vertices[vtx_2]) * 0.5;
            map_edge_to_sub_vtx[key] = sub_vertices.size();
            sub_vertices.push_back(edge_vtx);
        }
    }
    if (sub_progress) {
        sub_progress(50);
    }

    // Subdivide faces and their UVs: each original face splits into 4 sub-faces (parallel writes, no contention)
    const std::size_t N = original_faces.size();
    sub_faces.resize(N * 4);
    sub_uv_coords.resize(N * 4);
    std::atomic<bool> has_missing_edge{false};

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, N), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            const std::size_t base = fid * 4;
            const auto& face = original_faces[fid];
            std::size_t vtx_0 = face[0];
            std::size_t vtx_1 = face[1];
            std::size_t vtx_2 = face[2];

            auto it01 = map_edge_to_sub_vtx.find(edge_key(vtx_0, vtx_1));
            auto it12 = map_edge_to_sub_vtx.find(edge_key(vtx_1, vtx_2));
            auto it20 = map_edge_to_sub_vtx.find(edge_key(vtx_2, vtx_0));
            if (it01 == map_edge_to_sub_vtx.end() || it12 == map_edge_to_sub_vtx.end() || it20 == map_edge_to_sub_vtx.end()) [[unlikely]] {
                has_missing_edge.store(true, std::memory_order_relaxed);
                Vec3i32 degen(vtx_0, vtx_0, vtx_0);
                FaceUVArray degen_uv = {uv_coords[fid][0], uv_coords[fid][0], uv_coords[fid][0]};
                for (int k = 0; k < 4; ++k) {
                    sub_faces[base + k] = degen;
                    sub_uv_coords[base + k] = degen_uv;
                }
                continue;
            }
            std::size_t e01 = it01->second;
            std::size_t e12 = it12->second;
            std::size_t e20 = it20->second;

            const Vec2f& uv0 = uv_coords[fid][0];
            const Vec2f& uv1 = uv_coords[fid][1];
            const Vec2f& uv2 = uv_coords[fid][2];
            Vec2f uv_e01 = (uv0 + uv1) * 0.5f;
            Vec2f uv_e12 = (uv1 + uv2) * 0.5f;
            Vec2f uv_e20 = (uv2 + uv0) * 0.5f;

            sub_faces[base + 0] = Vec3i32(vtx_0, e01, e20);
            sub_uv_coords[base + 0] = {uv0, uv_e01, uv_e20};

            sub_faces[base + 1] = Vec3i32(e01, vtx_1, e12);
            sub_uv_coords[base + 1] = {uv_e01, uv1, uv_e12};

            sub_faces[base + 2] = Vec3i32(e01, e12, e20);
            sub_uv_coords[base + 2] = {uv_e01, uv_e12, uv_e20};

            sub_faces[base + 3] = Vec3i32(e20, e12, vtx_2);
            sub_uv_coords[base + 3] = {uv_e20, uv_e12, uv2};
        }
    });
    // Remove degenerate triangles (three identical vertices) to avoid impacting downstream SDF / Remesh steps
    if (has_missing_edge.load(std::memory_order_relaxed)) {
        std::size_t write_idx = 0;
        for (std::size_t i = 0; i < sub_faces.size(); ++i) {
            if (sub_faces[i][0] == sub_faces[i][1] && sub_faces[i][1] == sub_faces[i][2]) {
                continue;
            }
            if (write_idx != i) {
                sub_faces[write_idx] = sub_faces[i];
                sub_uv_coords[write_idx] = sub_uv_coords[i];
            }
            ++write_idx;
        }
        BOOST_LOG_TRIVIAL(warning) << "[warning] linear_subdivision has missing edge vertex, removed " << (sub_faces.size() - write_idx) << " degenerate triangles";
        sub_faces.resize(write_idx);
        sub_uv_coords.resize(write_idx);
    }

    if (sub_progress) {
        sub_progress(100);
    }
    BOOST_LOG_TRIVIAL(debug) << "TextureToColor: input faces count = " << mesh.indices.size() << ".";
    mesh = TriMesh(sub_faces, sub_vertices);
    uv_coords = std::move(sub_uv_coords);
    BOOST_LOG_TRIVIAL(debug) << "TextureToColor: output faces count = " << mesh.indices.size() << ".";
    return true;
}

bool TextureToColor(const TriMesh& texture_mesh, const std::vector<std::vector<Vec2f>>& texture_mesh_uv_coords, const cv::Mat& texture, TriMesh& color_mesh,
                    std::vector<std::array<std::size_t, 3>>& face_colors, const TextureToColorSettings& settings, AlgoProgressCallback progress_callback,
                    AlgoCancelCallback cancel_callback) {
    auto report = [&](int pct, const char* msg) {
        if (progress_callback) {
            progress_callback({pct, msg});
        }
    };
    auto sub_report = [&](int sub_pct, int range_start, int range_end, const char* msg) {
        int pct = range_start + sub_pct * (range_end - range_start) / 100;
        report(pct, msg);
    };
    auto cancelled = [&]() -> bool {
        if (cancel_callback && cancel_callback()) {
            BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << " cancelled";
            return true;
        }
        return false;
    };

    color_mesh.clear();
    face_colors.clear();

    report(0, "Initializing");
    if (cancelled()) {
        return false;
    }

    if (texture_mesh.indices.size() == 0) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: texture mesh has no faces.";
        return false;
    }
    if (texture.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: texture is empty.";
        return false;
    }
    if (texture.channels() < 3) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: texture must have at least 3 channels, got " << texture.channels();
        return false;
    }
    if (texture_mesh_uv_coords.size() != texture_mesh.indices.size()) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: uv_coords size is not equal to texture mesh faces size.";
        return false;
    }
    for (std::size_t fid = 0; fid < texture_mesh.indices.size(); ++fid) {
        if (texture_mesh_uv_coords[fid].size() != 3) {
            BOOST_LOG_TRIVIAL(debug) << "TextureToColor: uv_coords of single face size is not equal to 3.";
            return false;
        }
    }
    color_mesh = texture_mesh;

    using Clock = std::chrono::high_resolution_clock;
    const auto t_total_start = Clock::now();
    auto t_step = t_total_start;
    auto lap = [&](const char* step_name) {
        auto now = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - t_step).count();
        BOOST_LOG_TRIVIAL(debug) << "[timing] " << step_name << ": " << ms << "ms"
                        << " faces=" << color_mesh.facets_count();
        t_step = now;
    };

    report(5, "Oversampling");
    if (cancelled()) {
        return false;
    }

    // Step 1: Oversampling (subdivision while propagating UVs)
    // Convert external vector<vector<Vec2f>> to internal vector<array<Vec2f,3>> to eliminate inner-level heap allocations
    std::vector<FaceUVArray> color_mesh_uv_coords(texture_mesh_uv_coords.size());
    for (std::size_t i = 0; i < texture_mesh_uv_coords.size(); ++i) {
        color_mesh_uv_coords[i] = {texture_mesh_uv_coords[i][0], texture_mesh_uv_coords[i][1], texture_mesh_uv_coords[i][2]};
    }
    {
        // Estimate total iterations and map each iteration's sub-progress to the [5, 25] range
        size_t estimated_iters = 0;
        if (settings.oversampling_iters > 0) {
            estimated_iters = settings.oversampling_iters;
        } else {
            size_t fc = color_mesh.facets_count();
            while (fc < settings.oversampling_min_face_count) {
                fc *= 4;
                ++estimated_iters;
            }
            if (estimated_iters == 0) {
                estimated_iters = 1;
            }
        }

        auto make_iter_progress = [&](size_t iter) {
            return [&, iter, estimated_iters](int pct) {
                int iter_start = static_cast<int>(iter * 100 / estimated_iters);
                int iter_end = static_cast<int>((iter + 1) * 100 / estimated_iters);
                int sub_pct = iter_start + pct * (iter_end - iter_start) / 100;
                sub_report(sub_pct, 5, 25, "Oversampling");
            };
        };

        if (settings.oversampling_iters > 0) {
            for (size_t i = 0; i < settings.oversampling_iters && color_mesh.facets_count() * 4.0 < settings.oversampling_max_face_count; ++i) {
                if (cancelled()) return false;
                linear_subdivision(color_mesh, color_mesh_uv_coords, make_iter_progress(i));
            }
        } else {
            size_t iter = 0;
            while (color_mesh.facets_count() < settings.oversampling_min_face_count) {
                if (cancelled()) return false;
                linear_subdivision(color_mesh, color_mesh_uv_coords, make_iter_progress(iter++));
            }
        }
    }

    lap("Oversampling");

    face_colors.resize(color_mesh.indices.size());

    report(25, "Computing face colors");
    if (cancelled()) {
        return false;
    }

    // Step 2: Compute each face's color (7-point Gaussian quadrature + bilinear interpolation sampling)
    {
        std::atomic<size_t> done_faces{0};
        std::atomic<bool> cancel_requested{false};
        const size_t total_faces = color_mesh.indices.size();
        const size_t report_interval = std::max<size_t>(total_faces / 20, 1);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, total_faces), [&](const tbb::blocked_range<size_t>& range) {
            for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
                if (cancel_requested.load(std::memory_order_relaxed)) return;
                face_colors[fid] = sample_face_color(color_mesh_uv_coords[fid], texture);
                size_t cnt = done_faces.fetch_add(1, std::memory_order_relaxed) + 1;
                if (cnt % report_interval == 0) {
                    if (cancelled()) { cancel_requested.store(true, std::memory_order_relaxed); return; }
                    sub_report(static_cast<int>(cnt * 100 / total_faces), 25, 40, "Computing face colors");
                }
            }
        });
        if (cancel_requested.load() || cancelled()) return false;
    }
    lap("Computing face colors");
#ifdef OUTPUT_TEST_RESULT
    SaveToOFF("texture_to_color_0_initialize.off", color_mesh, face_colors);
#endif

    report(40, "Repairing mesh");
    if (cancelled()) {
        return false;
    }

    // Sub-stage timing helper for the "Repairing mesh" outer lap. Logs each
    // sub-phase under a [timing][Repairing mesh] prefix so that regressions in
    // mesh inspection, RepairMesh, AABB resampling, etc. can be attributed
    // to a specific sub-stage without changing the outer lap structure.
    auto sub_lap = [&](const char* sub_name, Clock::time_point t0) {
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        BOOST_LOG_TRIVIAL(debug) << "[timing][Repairing mesh] " << sub_name << ": " << ms << "ms";
    };

    // Step 3: Repair mesh
    // Many textured models have non-manifold, non-closed, or other issues that need to be fixed beforehand
    auto resample_repaired_mesh = [&](TriMesh&& repaired_mesh) -> bool {
        // AABBTreeIndirect references vertices/faces externally, so snapshot the
        // pre-repair geometry by moving them out of color_mesh before it gets
        // overwritten with the repaired mesh below. std::move on std::vector is
        // O(1) (pointer adoption), no element copy.
        const auto t_aabb = Clock::now();
        TriVertices old_vertices = std::move(color_mesh.vertices);
        TriFaces    old_indices  = std::move(color_mesh.indices);
        auto before_repair_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(old_vertices, old_indices);
        sub_lap("resample.aabb_build", t_aabb);

        color_mesh = std::move(repaired_mesh);

        const auto t_is_closed = Clock::now();
        if (is_closed(color_mesh)) {
            BOOST_LOG_TRIVIAL(debug) << "TextureToColor: repaired mesh is closed.";
        } else {
            BOOST_LOG_TRIVIAL(debug) << "TextureToColor: repaired mesh is open.";
        }
        sub_lap("resample.is_closed", t_is_closed);

        // New faces after repair inherit old face colors via centroid nearest-neighbor lookup.
        // Since the mesh barely changes after repair, resampling via centroid nearest-neighbor is sufficient.
        const auto t_resample = Clock::now();
        std::vector<RGB> new_face_colors(color_mesh.facets_count());
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, color_mesh.facets_count()), [&](const tbb::blocked_range<size_t>& range) {
            for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
                const auto& face = color_mesh.indices[fid];
                Vec3f center = (color_mesh.vertices[face[0]] + color_mesh.vertices[face[1]] + color_mesh.vertices[face[2]]) / 3.0f;
                size_t hit_idx = 0;
                Vec3f  closest;
                AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                    old_vertices, old_indices, before_repair_tree, center, hit_idx, closest);
                new_face_colors[fid] = face_colors[hit_idx];
            }
        });
        face_colors = std::move(new_face_colors);
        sub_lap("resample.parallel_nearest", t_resample);
        return true;
    };

    auto repair_and_resample_mesh = [&]() -> bool {
        std::shared_ptr<TriMesh> repaired_mesh;
        const auto t_repair = Clock::now();
        bool success = RepairMesh(color_mesh, repaired_mesh);
        sub_lap("RepairMesh", t_repair);
        if (success == false) {
            BOOST_LOG_TRIVIAL(debug) << "TextureToColor: repair mesh failed.";
            return false;
        }
        if (cancelled()) return false;
        return resample_repaired_mesh(std::move(*repaired_mesh));
    };

    {
        const auto t_stats = Clock::now();
        TriangleMesh stats_mesh(static_cast<const indexed_triangle_set&>(color_mesh));
        const auto& stats = stats_mesh.stats();
        sub_lap("stats_check", t_stats);
        // Orca's TriangleMeshStats defines manifold() as open_edges == 0 and does not track
        // non-manifold edges/vertices separately, so BBS's "!manifold() || has_open_edges()"
        // collapses to this single test and the extra counters drop out of the log.
        if (!stats.manifold()) {
            BOOST_LOG_TRIVIAL(info) << "TextureToColor: mesh has non-manifold geometry or open boundaries, open_edges="
                                    << stats.open_edges;
            if (settings.mesh_repair_decision == MeshRepairDecision::Ask) {
                if (settings.mesh_repair_decision_required)
                    *settings.mesh_repair_decision_required = true;
                return false;
            }
            if (settings.mesh_repair_decision == MeshRepairDecision::RepairAndImport) {
                indexed_triangle_set repaired_its;
                std::string repair_error;
                const auto t_win3d = Clock::now();
                bool repaired = settings.mesh_repair_callback && settings.mesh_repair_callback(static_cast<const indexed_triangle_set&>(color_mesh), repaired_its,
                    [&](const char* message, unsigned percent) {
                        sub_report(static_cast<int>(percent), 40, 60, message ? message : "Repairing mesh");
                    },
                    [&]() { return cancelled(); }, &repair_error);
                sub_lap("windows_3d_repair", t_win3d);
                if (repaired) {
                    if (cancelled()) return false;
                    BOOST_LOG_TRIVIAL(info) << "TextureToColor: Windows 3D mesh repair finished.";
                    if (!resample_repaired_mesh(TriMesh(std::move(repaired_its))))
                        return false;
                } else {
                    BOOST_LOG_TRIVIAL(warning) << "TextureToColor: Windows 3D mesh repair failed: " << repair_error;
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << "TextureToColor: importing mesh without Windows 3D repair.";
            }
        }
    }

    const auto t_halfedge = Clock::now();
    const bool halfedge_ok = cgalutils::is_mesh_halfedge_compatible(color_mesh);
    sub_lap("is_mesh_halfedge_compatible", t_halfedge);
    if (!halfedge_ok && repair_and_resample_mesh() == false) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: repair and resample mesh failed.";
        return false;
    }
    lap("Repairing mesh");
#ifdef OUTPUT_TEST_RESULT
    SaveToOFF("texture_to_color_1_repair.off", color_mesh, face_colors);
#endif

    report(65, "Color clustering");
    if (cancelled()) {
        return false;
    }

    // Step 5: Color clustering
    std::vector<RGB> cluster_centers;
    std::vector<RGB> clustered_face_colors = face_colors;
    std::vector<std::size_t> clustered_face_labels(face_colors.size());
    const bool adaptive_cluster = settings.target_colors_num == 0;

    // Compute cluster centers
    if (adaptive_cluster) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: use cluster adaptive method.";
        ClusterParameters para;
        para.max_color_distance = settings.max_color_distance;
        para.max_cluster_k = settings.max_cluster_k;
        para.cancel_callback = cancel_callback ? [&]() { return cancel_callback(); } : std::function<bool()>{};
        cluster_centers = cluster_adaptive(face_colors, para);
        if (cancelled()) return false;
    } else {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: use cluster k-means method.";
        ClusterParameters para;
        para.cluster_k = settings.target_colors_num;
        para.cancel_callback = cancel_callback ? [&]() { return cancel_callback(); } : std::function<bool()>{};
        cluster_centers = cluster_k_means(face_colors, para);
        if (cancelled()) return false;
    }
    BOOST_LOG_TRIVIAL(debug) << "TextureToColor: the k is " << cluster_centers.size() << ".";
    if (cluster_centers.empty()) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: no cluster center generated.";
        return false;
    }
    const std::set<RGB> unique_cluster_centers(cluster_centers.begin(), cluster_centers.end());
    if (unique_cluster_centers.size() != cluster_centers.size()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: cluster centers contain duplicated RGB values, unique exported colors may be fewer than centers.";
    }

    report(70, "Assigning cluster labels");
    if (cancelled()) {
        return false;
    }

    // Assign each face's color to the nearest cluster center
    constexpr bool use_simple_cluster = true;  // Complex algorithm is still being optimized; use simple assignment for now
    if (use_simple_cluster) {
        std::atomic<size_t> done_cluster{0};
        std::atomic<bool> cancel_requested{false};
        const size_t total_cluster = color_mesh.indices.size();
        const size_t cluster_interval = std::max<size_t>(total_cluster / 20, 1);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, total_cluster), [&](const tbb::blocked_range<size_t>& range) {
            for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
                if (cancel_requested.load(std::memory_order_relaxed)) return;
                auto& face_color = face_colors[fid];
                auto nearest_color_id = std::numeric_limits<std::size_t>::max();
                bool success = calc_nearest_color_id(cluster_centers, face_color, nearest_color_id);
                if (success == false) {
                    BOOST_LOG_TRIVIAL(debug) << "TextureToColor: calc nearest color id failed.";
                    continue;
                }
                clustered_face_labels[fid] = nearest_color_id;
                clustered_face_colors[fid] = cluster_centers[nearest_color_id];
                size_t cnt = done_cluster.fetch_add(1, std::memory_order_relaxed) + 1;
                if (cnt % cluster_interval == 0) {
                    if (cancelled()) { cancel_requested.store(true, std::memory_order_relaxed); return; }
                    sub_report(static_cast<int>(cnt * 100 / total_cluster), 70, 85, "Assigning cluster labels");
                }
            }
        });
        if (cancel_requested.load() || cancelled()) return false;
    } else {
        bool success = mesh_cluster(color_mesh, cluster_centers, clustered_face_colors, clustered_face_labels);
        if (success == false) {
            BOOST_LOG_TRIVIAL(debug) << "TextureToColor: mesh cluster failed.";
            return false;
        }
    }
    if (adaptive_cluster) {
        if (!discard_unused_cluster_centers(cluster_centers, clustered_face_labels, "cluster assignment")) {
            return false;
        }
    } else {
        ensure_all_cluster_centers_used(face_colors, cluster_centers, clustered_face_labels, "cluster assignment");
    }
    lap("Color clustering & labeling");
#ifdef OUTPUT_TEST_RESULT
    for (std::size_t i = 0; i < clustered_face_colors.size(); ++i) {
        clustered_face_colors[i] = cluster_centers[clustered_face_labels[i]];
    }
    SaveToOFF("texture_to_color_3_cluster.off", color_mesh, clustered_face_colors);
#endif

    report(85, "Smoothing colors");
    if (cancelled()) {
        return false;
    }

    // Step 6: Post-process colors
    SmoothParameters smooth_parameters;
    smooth_parameters.smooth_weight = settings.smooth_weight;
    if (!smooth_region(color_mesh, clustered_face_labels, smooth_parameters)) {
        BOOST_LOG_TRIVIAL(debug) << "TextureToColor: smooth region failed.";
        return false;
    }
    BOOST_LOG_TRIVIAL(debug) << "TextureToColor: smooth region success.";
    if (adaptive_cluster) {
        if (!discard_unused_cluster_centers(cluster_centers, clustered_face_labels, "color smoothing")) {
            return false;
        }
    } else {
        ensure_all_cluster_centers_used(face_colors, cluster_centers, clustered_face_labels, "color smoothing");
    }
    report(95, "Updating face colors");
    if (cancelled()) {
        return false;
    }
    for (std::size_t i = 0; i < clustered_face_colors.size(); ++i) {
        clustered_face_colors[i] = cluster_centers[clustered_face_labels[i]];
    }
    const std::set<RGB> unique_exported_colors(clustered_face_colors.begin(), clustered_face_colors.end());
    if (unique_exported_colors.size() < cluster_centers.size()) {
        BOOST_LOG_TRIVIAL(warning) << "TextureToColor: final exported unique colors (" << unique_exported_colors.size()
                                   << ") are fewer than cluster centers (" << cluster_centers.size()
                                   << "), likely due to duplicate centers or unsatisfied seed assignment.";
    }
#ifdef OUTPUT_TEST_RESULT
    SaveToOFF("texture_to_color_4_smooth.off", color_mesh, clustered_face_colors);
#endif

    face_colors = std::move(clustered_face_colors);
    lap("Smoothing colors");
    double total_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_total_start).count();
    BOOST_LOG_TRIVIAL(debug) << "[timing] TextureToColor total: " << total_ms << "ms"
                    << " faces=" << color_mesh.facets_count();
    report(100, "Completed");
    return true;
}

}  // namespace tex2color
}  // namespace Slic3r
