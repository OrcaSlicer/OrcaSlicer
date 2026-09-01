#pragma once

#include "Callbacks.hpp"
#include "TriMesh.hpp"
#include "opencv2/core.hpp"
#include <functional>
#include <string>

namespace Slic3r { namespace tex2color {

enum class MeshRepairDecision {
    Ask,
    ImportWithoutRepair,
    RepairAndImport
};

using MeshRepairCallback = std::function<bool(const indexed_triangle_set& mesh,
                                              indexed_triangle_set&       repaired_mesh,
                                              std::function<void(const char* message, unsigned progress)> progress_callback,
                                              std::function<bool()> cancel_callback,
                                              std::string* error_message)>;

struct TextureToColorSettings {
    std::size_t target_colors_num = 4;  // 目标颜色数量, 为0时, 自适应计算; 否则计算指定数目的颜色聚类

    double smooth_weight = 0.5;  // 光顺权重, 范围[0, 1], 0表示不进行光顺, 1表示完全光顺

    // 当超采样迭代次数大于0时, 进行指定迭代次数的超采样; 否则, 自适应超采样
    std::size_t oversampling_iters = 0;  // 超采样迭代次数
    std::size_t oversampling_min_face_count = 10000;  // 自适应采样: 当face_count小于oversampling_min_face_count时, 进行超采样
    std::size_t oversampling_max_face_count = 1000000;  // 无论输入参数如何, 超采样后的面片数不能超过oversampling_max_face_count

    double max_color_distance = 25.0;  // 自适应聚类允许的最大簇内半径(CIEDE2000 ΔE)
    std::size_t max_cluster_k = 32;  // 自适应聚类的最大颜色数量上限

    MeshRepairDecision mesh_repair_decision = MeshRepairDecision::ImportWithoutRepair;

    // Set by TextureToColor when Ask is selected and mesh repair needs user confirmation.
    bool* mesh_repair_decision_required = nullptr;

    MeshRepairCallback mesh_repair_callback;
};

/**
 * @brief 将纹理贴图转换为网格面片颜色, 并通过聚类和光顺生成可用于多色打印的着色网格
 *
 * 基于纹理网格的UV坐标对纹理图像进行采样, 计算每个面片的颜色,
 * 然后对颜色进行聚类（K-Means或自适应）和区域光顺, 最终输出带颜色信息的网格
 *
 * @param[in]      texture_mesh       带有UV坐标的输入三角网格
 * @param[in]      uv_coords          每个面片的UV坐标, 大小等于面片数, 每个面片有三个UV坐标
 * @param[in]      texture            纹理图像
 * @param[out]     color_mesh         输出的着色网格
 * @param[out]     face_colors        输出的着色网格的面片颜色, 大小等于面片数, 颜色值为[R, G, B], 范围0~255
 * @param[in]      settings           算法参数, 包括目标颜色数量、光顺权重等
 * @param[in]      progress_callback  进度回调函数
 * @param[in]      cancel_callback    取消回调函数
 * @return 成功返回true, 输入数据无效(空网格、无UV、空纹理等)返回false
 */
bool TextureToColor(const TriMesh& texture_mesh, const std::vector<std::vector<Vec2f>>& uv_coords, const cv::Mat& texture, TriMesh& color_mesh,
                    std::vector<std::array<std::size_t, 3>>& face_colors, const TextureToColorSettings& settings = TextureToColorSettings(),
                    AlgoProgressCallback progress_callback = nullptr, AlgoCancelCallback cancel_callback = nullptr);

/**
 * @brief Turn pre-computed per-face colors into a clustered color mesh (no texture/UV).
 *
 * Used for OBJ vertex colors and MTL face colors, which bypass texture sampling.
 * Two routes are possible:
 * - Low-poly meshes carrying per-vertex colors: the vertex colors are quantized
 *   into a small palette and the mesh is geometrically split along cluster
 *   boundaries, reproducing the split topology of the legacy OBJ vertex-color
 *   import. Output colors are then exact cluster centers, so mesh repair,
 *   re-clustering and smoothing are skipped.
 * - Everything else: mesh repair, color clustering (K-Means or adaptive) and
 *   region smoothing, sharing the same pipeline as TextureToColor.
 *
 * @param[in]      mesh               Input triangle mesh
 * @param[in]      input_face_colors  Pre-computed per-face RGB colors [0..255]
 * @param[out]     out_mesh           Output mesh. Geometry is subdivided on the
 *                                    vertex-color route, and may still be replaced
 *                                    by mesh repair on the generic route.
 * @param[out]     out_face_colors    Output per-face colors, one entry per out_mesh face
 * @param[in]      settings           Algorithm parameters (target_colors_num, smooth_weight;
 *                                    oversampling_min_face_count doubles as the low-poly
 *                                    threshold for the vertex-color route)
 * @param[in]      progress_callback  Progress callback
 * @param[in]      cancel_callback    Cancel callback
 * @param[in]      vertex_colors      Optional per-vertex RGBA [0..1]. Must match
 *                                    mesh.vertices in size to enable the vertex-color
 *                                    route; otherwise it is ignored.
 * @return true on success, false on failure or cancellation
 */
bool ClusterAndSmooth(const TriMesh& mesh,
                      const std::vector<std::array<std::size_t, 3>>& input_face_colors,
                      TriMesh& out_mesh,
                      std::vector<std::array<std::size_t, 3>>& out_face_colors,
                      const TextureToColorSettings& settings = TextureToColorSettings(),
                      AlgoProgressCallback progress_callback = nullptr,
                      AlgoCancelCallback cancel_callback = nullptr,
                      const std::vector<std::array<float, 4>>& vertex_colors = {});

}  // namespace tex2color
}  // namespace Slic3r
