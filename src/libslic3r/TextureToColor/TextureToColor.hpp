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

}  // namespace tex2color
}  // namespace Slic3r
