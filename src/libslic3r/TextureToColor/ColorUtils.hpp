#pragma once

#include "Callbacks.hpp"
#include "TriMesh.hpp"

namespace Slic3r { namespace tex2color {

namespace color_utils {
struct ClusterParameters;

typedef std::array<std::size_t, 3> Color;  // RGB: [R, G, B] 0~255
typedef std::vector<Color> ColorList;
typedef std::array<double, 3> ColorDouble;
typedef std::array<std::size_t, 3> RGB;

// Function pointer type that points to a specific color-difference function based on the chosen method.
using DistanceFunction = double (*)(const Color&, const Color&);

// Color space used for computing color differences.
enum struct ColorDifferenceMethod : std::size_t {
    RGB = 0,  // Simplest and fastest
    Lab = 1   // Most perceptually accurate
};

struct ClusterParameters {
    ColorDifferenceMethod color_difference_method = ColorDifferenceMethod::Lab;  // Method for measuring color difference; Lab is the most accurate

    double max_color_distance = 25;  // Max intra-cluster radius (CIEDE2000 dE) for adaptive clustering; ignored by the fixed-K algorithm

    std::size_t cluster_k = 10;  // Target number of cluster centers; ignored by the adaptive algorithm

    std::size_t max_cluster_k = 32;  // Max cluster count upper bound for adaptive algorithm

    std::size_t max_iter = 50;  // Maximum number of iterations

    std::function<bool()> cancel_callback;  // Optional cancellation check; returns true when the caller requests abort
};

struct SmoothParameters {
    double smooth_weight = 0.5;  // Controls smoothing intensity; larger values produce smoother results. Range: [0.0, 1.0]
};

/**
 * @brief Compute the squared Euclidean distance between two RGB colors.
 *
 * @param[in]      rgb1    First RGB color [R, G, B], range 0~255.
 * @param[in]      rgb2    Second RGB color [R, G, B], range 0~255.
 * @return Squared Euclidean distance: (R1-R2)^2 + (G1-G2)^2 + (B1-B2)^2.
 */
double calc_rgb_color_difference_by_squared_rgb(const RGB& rgb1, const RGB& rgb2);

/**
 * @brief Compute the squared Euclidean distance between two RGB colors (double precision).
 *
 * @param[in]      c1      First RGB color [R, G, B], as double.
 * @param[in]      c2      Second RGB color [R, G, B], as double.
 * @return Squared Euclidean distance: (R1-R2)^2 + (G1-G2)^2 + (B1-B2)^2.
 */
double calc_rgb_color_difference_by_squared_rgb_double(const ColorDouble& c1, const ColorDouble& c2);

/**
 * @brief Compute the CIEDE2000 color difference between two RGB colors.
 *
 * Currently the most accurate color-difference formula, recommended by CIE as the industry standard.
 * - dE <= 1.0: imperceptible to the human eye, high-precision color matching.
 * - dE <= 2.0: slight difference, noticeable by experts; printing / image processing standard.
 * - dE <= 3.0: noticeable by ordinary observers; general quality control.
 *
 * @param[in]      rgb1    First RGB color [R, G, B], range 0~255.
 * @param[in]      rgb2    Second RGB color [R, G, B], range 0~255.
 * @return CIEDE2000 color difference; smaller values indicate more similar colors.
 */
double calc_rgb_color_difference_by_ciede2000(const RGB& rgb1, const RGB& rgb2);

/**
 * @brief Compute the CIEDE2000 color difference between two sRGB colors (double precision, non-linear channels in [0,1]).
 *
 * Uses the same XYZ/Lab/dE00 pipeline as calc_rgb_color_difference_by_ciede2000 but without uint8
 * quantization or the intermediate x255 conversion; suitable for bisection, color blending, and other
 * iterative scenarios. Note: ColorDouble here represents [R,G,B] in [0,1], which differs from the
 * 0~255 scale used by other interfaces in this file. Callers should follow the naming convention.
 *
 * @param[in] rgb1 rgb2  sRGB non-linear channel values, recommended range [0,1].
 */
double calc_rgb_color_difference_by_ciede2000_srgb01(const ColorDouble& rgb1, const ColorDouble& rgb2);

/**
 * @brief K-Means clustering algorithm that minimizes the sum of squared errors.
 *
 * Uses K-Means++ initialization to iteratively find the optimal cluster centers.
 *
 * @param[in]      colors                Input color list.
 * @param[in]      cluster_parameters    Clustering parameters including cluster count, max iterations, color-difference method, etc.
 * @return List of cluster-center colors whose size equals cluster_parameters.cluster_k.
 */
std::vector<Color> cluster_k_means(const std::vector<Color>& colors, const ClusterParameters& cluster_parameters);

/**
 * @brief Adaptive K-Means clustering that determines an appropriate number of clusters under a max color-distance constraint.
 *
 * Automatically finds the optimal cluster count via binary search so that max_color_distance is satisfied.
 *
 * @param[in]      colors                Input color list.
 * @param[in]      cluster_parameters    Clustering parameters; cluster_k is ignored and determined automatically.
 * @return List of cluster-center colors whose count is determined by the algorithm based on max_color_distance.
 */
std::vector<Color> cluster_adaptive(const std::vector<Color>& colors, const ClusterParameters& cluster_parameters);

/**
 * @brief Cluster a color list to a set of specified cluster centers.
 *
 * For each input color, find the nearest specified cluster center and replace it.
 *
 * @param[in]      colors                Input color list.
 * @param[in]      specified_colors      Specified cluster-center colors.
 * @return Clustered color list where each color is replaced by its nearest center.
 */
std::vector<Color> cluster_to_specified_colors(const std::vector<Color>& colors, const std::vector<Color>& specified_colors);

/**
 * @brief Remesh the mesh while preserving color boundaries.
 *
 * Performs isotropic remeshing while protecting color boundaries. Edges whose two adjacent
 * faces have different colors are marked as feature edges and will not be modified.
 *
 * @param[in,out]  mesh                      Input mesh; modified in-place after remeshing.
 * @param[in,out]  face_labels               Face color labels; updated to match the new mesh.
 * @param[in]      target_edge_length_ratio  Ratio of target average edge length to input average edge length; >1 simplifies, <1 refines.
 * @return true on success, false on failure.
 */
bool remesh_mesh(TriMesh& mesh, std::vector<std::size_t>& face_labels, double target_edge_length_ratio);

/**
 * @brief Check whether the mesh is closed (watertight).
 *
 * A mesh is closed if it has no boundary edges, i.e. every edge is shared by exactly two faces.
 *
 * @param[in]      tri_mesh    Input mesh.
 * @return true if the mesh is closed, false if it has boundary edges.
 */
bool is_closed(const TriMesh& tri_mesh);

/**
 * @brief Smooth region boundaries (RGB color labels).
 *
 * Applies topological smoothing (label reassignment) and geometric smoothing (boundary vertex relocation).
 *
 * @param[in,out]  tri_mesh              Input mesh; modified in-place after smoothing.
 * @param[in,out]  face_labels           Face color labels (RGB format); updated after smoothing.
 * @param[in]      smooth_parameters     Smoothing control parameters.
 * @return true on success, false on failure.
 */
bool smooth_region(TriMesh& tri_mesh, std::vector<std::array<std::size_t, 3>>& face_labels, const SmoothParameters& smooth_parameters = SmoothParameters());

/**
 * @brief Smooth region boundaries (integer labels).
 *
 * Applies topological smoothing (label reassignment) and geometric smoothing (boundary vertex relocation).
 *
 * @param[in,out]  tri_mesh              Input mesh; modified in-place after smoothing.
 * @param[in,out]  face_labels           Integer face labels; updated after smoothing.
 * @param[in]      smooth_parameters     Smoothing control parameters.
 * @return true on success, false on failure.
 */
bool smooth_region(TriMesh& tri_mesh, std::vector<std::size_t>& face_labels, const SmoothParameters& smooth_parameters = SmoothParameters());

/**
 * @brief Split the mesh into connected components.
 *
 * Based on face connectivity, the mesh is split into independent components, each forming a
 * standalone mesh. Texture coordinates for each component are preserved.
 *
 * @param[in]      mesh                    Input mesh.
 * @param[in]      vertex_uvs              Vertex texture coordinates.
 * @param[out]     component_meshes        Output list of component meshes.
 * @param[out]     component_vertex_uvs    Output list of texture coordinates per component.
 * @return true on success, false on failure.
 */
bool get_components(const TriMesh& mesh, const std::vector<Vec2f>& vertex_uvs, std::vector<TriMesh>& component_meshes,
                    std::vector<std::vector<Vec2f>>& component_vertex_uvs);

/**
 * @brief Find the ID of the nearest color in a color list to a given color.
 *
 * @param[in]      colors                Color list.
 * @param[in]      color                 Target color.
 * @param[out]     nearest_color_id      ID of the nearest color found.
 * @return true on success, false on failure.
 */
bool calc_nearest_color_id(const std::vector<RGB>& colors, const RGB& color, std::size_t& nearest_color_id);

/**
 * @brief Cluster mesh face colors based on given cluster centers.
 *
 * @param[in]          mesh                    Input mesh.
 * @param[in]          cluster_centers         Cluster-center RGB colors.
 * @param[in, out]     map_face_to_rgb         RGB color per face; updated to the nearest cluster center after clustering.
 * @param[out]         map_face_to_cluster_id  Cluster-center ID per face; updated to the nearest cluster center ID.
 * @return true on success, false on failure.
 */
bool mesh_cluster(const TriMesh& mesh, const std::vector<RGB>& cluster_centers, std::vector<RGB>& map_face_to_rgb,
                  std::vector<std::size_t>& map_face_to_cluster_id);

}  // namespace color_utils

}  // namespace tex2color
}  // namespace Slic3r
