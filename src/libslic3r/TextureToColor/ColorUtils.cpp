#include "ColorUtils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <numeric>

#include <boost/next_prior.hpp>

#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/AABB_traits.h>
#include <CGAL/AABB_tree.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Point_3.h>
#include <CGAL/Polygon_mesh_processing/compute_normal.h>
#include <CGAL/Polygon_mesh_processing/intersection.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>
#include <CGAL/Polygon_mesh_processing/repair_self_intersections.h>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/mesh_segmentation.h>
#include <tbb/parallel_for.h>

#include "CgalUtils.hpp"
#include "libslic3r/AABBTreeIndirect.hpp"
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace tex2color {
namespace color_utils {

// #define DEBUG_FLAG

#ifndef M_PI
#define M_PI 3.1415926535897932
#endif

#ifndef EPSILON
#define EPSILON 1e-6
#endif

#ifndef DOUBLE_LIMITS
#define DOUBLE_LIMITS
#define Double_MAX std::numeric_limits<double>::max()
#define Double_MIN -std::numeric_limits<double>::max()
#endif  // !DOUBLE_LIMITS

namespace PMP = CGAL::Polygon_mesh_processing;

using cgalutils::CGALMesh;
using CGALKernel = cgalutils::Kernel;

static constexpr double TOPO_SMOOTH_WEIGHT_THRESHOLD = 0.3;

namespace detail {
template<typename Mesh>
double average_edge_length_impl(const Mesh& m) {
    double total = 0.0;
    size_t count = 0;
    for (auto e : m.edges()) {
        auto h = m.halfedge(e);
        auto p0 = m.point(m.source(h));
        auto p1 = m.point(m.target(h));
        total += std::sqrt(CGAL::squared_distance(p0, p1));
        ++count;
    }
    return count > 0 ? total / count : 1.0;
}
} // namespace detail

typedef CGAL::Aff_transformation_3<CGALKernel> Affine_transformation_3;
typedef boost::graph_traits<CGALMesh>::halfedge_descriptor halfedge_descriptor;
typedef boost::graph_traits<CGALMesh>::edge_descriptor edge_descriptor;
typedef boost::graph_traits<CGALMesh>::vertex_descriptor vertex_descriptor;
typedef CGAL::AABB_face_graph_triangle_primitive<CGALMesh> Primitive;
typedef CGAL::AABB_traits<CGALKernel, Primitive> Traits;
typedef CGAL::AABB_tree<Traits> Tree;
typedef CGALMesh::template Property_map<vertex_descriptor, CGALKernel::Vector_3> VNMap;

static inline ColorDouble convert_rgb_uint_to_rgb_double(const Color& color) {
    return ColorDouble{static_cast<double>(color[0]), static_cast<double>(color[1]), static_cast<double>(color[2])};
}

static void normalize(CGALKernel::Vector_3& vec) {
    double squared_length = vec.squared_length();
    if (squared_length > EPSILON) {
        vec /= sqrt(squared_length);
    }
}

static double get_angle_between_vectors(const CGALKernel::Vector_3& v1, const CGALKernel::Vector_3& v2) {
    CGALKernel::Vector_3 dir1{v1}, dir2{v2};
    normalize(dir1);
    normalize(dir2);
    double product_dot = dir1.x() * dir2.x() + dir1.y() * dir2.y() + dir1.z() * dir2.z();
    if (product_dot > 1.0 - EPSILON) {
        return 0.0;
    } else if (product_dot < -1.0 + EPSILON) {
        return 180.0;
    }
    return std::acos(product_dot) / M_PI * 180.0;
}

static void calc_face_normals(const CGALMesh& mesh, std::vector<Eigen::Vector3d>& face_normals) {
    std::size_t fcnt = mesh.number_of_faces();
    face_normals.resize(fcnt);
    for (auto face : mesh.faces()) {
        std::vector<CGALKernel::Point_3> points;
        for (auto vtx : mesh.vertices_around_face(mesh.halfedge(face))) {
            points.push_back(mesh.point(vtx));
        }
        Eigen::Vector3d pos1(points[0].x(), points[0].y(), points[0].z());
        Eigen::Vector3d pos2(points[1].x(), points[1].y(), points[1].z());
        Eigen::Vector3d pos3(points[2].x(), points[2].y(), points[2].z());
        face_normals[face] = (pos3 - pos2).cross(pos1 - pos2);
        face_normals[face].normalize();
    }
    return;
}

static bool check_and_repair_self_intersect(CGALMesh& mesh, bool* is_self_intersect_status = nullptr) {
    // true means the mesh is no self intersect now
    // false means the mesh is still self intersect
    auto is_self_intersect = PMP::does_self_intersect(mesh);
    if (is_self_intersect_status != nullptr) {
        *is_self_intersect_status = is_self_intersect;
    }
    if (is_self_intersect) {
        bool repair = PMP::experimental::remove_self_intersections(mesh);
        if (repair) {
            return true;
        } else {
            return false;
        }
    }
    return true;
}

static bool save_polylines(const std::string& file_name, const std::vector<std::vector<CGALKernel::Point_3>>& polylines) {
    std::vector<CGALKernel::Point_3> points;
    std::vector<std::pair<std::size_t, std::size_t>> lines;
    for (auto& polyline : polylines) {
        std::size_t begin_pt_idx = points.size();
        for (auto& pt : polyline) {
            points.push_back(pt);
        }
        for (std::size_t i = 1; i < polyline.size(); ++i) {
            lines.emplace_back(begin_pt_idx + i, begin_pt_idx + i + 1);  // obj is begin at 1
        }
    }
    std::ofstream output_file(file_name, std::ios::out);
    for (auto& point : points) {
        output_file << "v " << point[0] << " " << point[1] << " " << point[2] << "\n";
    }
    for (auto& line : lines) {
        output_file << "l " << line.first << " " << line.second << "\n";
    }
    output_file.close();
    return true;
}

static bool smooth_region_topo_boundary(CGALMesh& mesh, std::vector<std::size_t>& face_labels, std::size_t max_iters = 20) {
    // Topological smoothing: reassign face labels
    std::size_t iter = 0;
    while (iter < max_iters) {
        ++iter;
        bool flip_flag = false;
        for (auto face : mesh.faces()) {
            std::size_t same_label_count = 0;
            std::unordered_map<std::size_t, std::size_t> map_label_to_cnt;
            std::size_t max_adj_cnt = 0;
            std::size_t max_adj_label = face_labels[face];
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(face))) {
                if (adj_face == CGALMesh::null_face() || !mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {
                    continue;
                }
                if (face_labels[adj_face] == face_labels[face]) {
                    ++same_label_count;
                } else {
                    ++map_label_to_cnt[face_labels[adj_face]];
                    if (map_label_to_cnt[face_labels[adj_face]] > max_adj_cnt) {
                        max_adj_cnt = map_label_to_cnt[face_labels[adj_face]];
                        max_adj_label = face_labels[adj_face];
                    }
                }
            }
            if (max_adj_cnt > same_label_count) {
                face_labels[face] = max_adj_label;
                flip_flag = true;
            }
        }

        if (!flip_flag) {
            break;
        }
    }
    return true;
}

static bool smooth_region_geom_boundary(CGALMesh& mesh, std::vector<std::size_t>& face_labels, const SmoothParameters& smooth_parameters) {
    // 1. extract boundary vertices and make polines
    std::unordered_map<CGAL::SM_Vertex_index, std::size_t> map_vtx_to_degree;
    std::unordered_set<CGAL::SM_Edge_index> segment_boundary_edges;
    std::unordered_set<CGAL::SM_Edge_index> feature_edges;
    std::unordered_set<CGAL::SM_Vertex_index> feature_vertices;
    constexpr double feature_angle = 45;
    for (const auto& edge : mesh.edges()) {
        if (!mesh.is_valid(edge) || mesh.is_border(edge)) {
            continue;
        }
        auto source = mesh.source(mesh.halfedge(edge));
        auto target = mesh.target(mesh.halfedge(edge));
        auto face_1 = mesh.face(mesh.halfedge(edge));
        auto face_2 = mesh.face(mesh.opposite(mesh.halfedge(edge)));
        auto normal_1 = PMP::compute_face_normal(face_1, mesh);
        auto normal_2 = PMP::compute_face_normal(face_2, mesh);
        double angle = get_angle_between_vectors(normal_1, normal_2);
        if (angle > feature_angle) {
            feature_edges.insert(edge);
            feature_vertices.insert(source);
            feature_vertices.insert(target);
        }

        if (face_labels[face_1] == face_labels[face_2]) {
            continue;
        }

        segment_boundary_edges.insert(edge);
        ++map_vtx_to_degree[source];
        ++map_vtx_to_degree[target];
    }

    // 2. smooth each polyline
    std::vector<std::vector<CGAL::SM_Vertex_index>> polylines;
    std::unordered_set<CGAL::SM_Edge_index> visited_edges;

    std::function<void(std::vector<CGAL::SM_Vertex_index>&)> trace_polyline = [&](std::vector<CGAL::SM_Vertex_index>& polyline) -> void {
        if (polyline.empty()) {
            return;
        }
        CGAL::SM_Vertex_index curr_vtx = polyline.back();
        if (map_vtx_to_degree[curr_vtx] != 2) {
            return;
        }
        for (const auto& halfedge : mesh.halfedges_around_target(mesh.halfedge(curr_vtx))) {
            CGAL::SM_Edge_index edge = mesh.edge(halfedge);
            if (visited_edges.count(edge) || !segment_boundary_edges.count(edge)) {
                continue;
            }
            visited_edges.insert(edge);
            CGAL::SM_Vertex_index adj_vtx = mesh.source(halfedge);
            if (!map_vtx_to_degree.count(adj_vtx)) {
                continue;
            }
            polyline.push_back(adj_vtx);
            return trace_polyline(polyline);
        }
    };

    // 2.1. open polyline: from T nodes search other 2-degree nodes
    for (auto& [src_vtx, degree] : map_vtx_to_degree) {
        if (degree == 2) {
            continue;
        }
        for (auto& src_halfedge : mesh.halfedges_around_target(mesh.halfedge(src_vtx))) {
            CGAL::SM_Edge_index src_edge = mesh.edge(src_halfedge);
            if (visited_edges.count(src_edge) || !segment_boundary_edges.count(src_edge)) {
                continue;
            }
            visited_edges.insert(src_edge);
            CGAL::SM_Vertex_index adj_vtx = mesh.source(src_halfedge);
            if (!map_vtx_to_degree.count(adj_vtx)) {
                continue;
            }
            std::vector<CGAL::SM_Vertex_index> polyline{src_vtx, adj_vtx};
            trace_polyline(polyline);
            polylines.push_back(std::move(polyline));
        }
    }

    // 2.2. closed polylines
    for (auto edge : segment_boundary_edges) {
        if (visited_edges.count(edge)) {
            continue;
        }
        visited_edges.insert(edge);
        CGAL::SM_Halfedge_index halfedge = mesh.halfedge(edge);
        std::vector<CGAL::SM_Vertex_index> polyline{mesh.source(halfedge), mesh.target(halfedge)};
        trace_polyline(polyline);
        if (polyline.front() != polyline.back()) {
            std::cerr << "[Error]:  loop polyline but not closed!!!\n";
        }
        polylines.push_back(std::move(polyline));
    }

    // 3. smooth boundary
    Tree boundary_tree(mesh.faces().begin(), mesh.faces().end(), mesh);
    boundary_tree.accelerate_distance_queries();

    constexpr std::size_t max_iters = 5;
    const double smooth_weight = smooth_parameters.smooth_weight;  // Controls smoothing intensity; larger values produce smoother results. Range: 0.1~1.0.
    double origin_weight = std::max(1.0 - smooth_weight, 0.0);
    for (std::size_t iter = 0; iter < max_iters; ++iter) {
        for (const auto& polyline : polylines) {
            std::size_t pt_cnt = polyline.size();
            std::vector<CGALKernel::Point_3> points(pt_cnt);
            for (std::size_t pt_idx = 1; pt_idx + 1 < pt_cnt; ++pt_idx) {
                std::vector<CGALKernel::Point_3> pts{mesh.point(polyline[pt_idx - 1]), mesh.point(polyline[pt_idx + 1])};
                CGALKernel::Point_3 smooth_pt = CGAL::ORIGIN + ((CGAL::centroid(pts.begin(), pts.end()) - CGAL::ORIGIN) * smooth_weight +
                                                                (mesh.point(polyline[pt_idx]) - CGAL::ORIGIN) * origin_weight);
                points[pt_idx] = boundary_tree.closest_point(smooth_pt);
            }

            if (polyline.front() == polyline.back()) {
                if (feature_vertices.count(polyline.front())) {
                    continue;
                }
                std::vector<CGALKernel::Point_3> pts{mesh.point(polyline[1]), mesh.point(polyline[pt_cnt - 2])};
                CGALKernel::Point_3 smooth_pt = CGAL::ORIGIN + ((CGAL::centroid(pts.begin(), pts.end()) - CGAL::ORIGIN) * smooth_weight +
                                                                (mesh.point(polyline.front()) - CGAL::ORIGIN) * origin_weight);
                mesh.point(polyline.front()) = boundary_tree.closest_point(smooth_pt);
            }

            for (std::size_t pt_idx = 1; pt_idx + 1 < pt_cnt; ++pt_idx) {
                if (feature_vertices.count(polyline[pt_idx])) {
                    continue;
                }
                mesh.point(polyline[pt_idx]) = points[pt_idx];
            }
        }
    }

    for (auto& polyline : polylines) {
        for (auto& vtx : polyline) {
            mesh.point(vtx) = boundary_tree.closest_point(mesh.point(vtx));
        }
    }

    return true;
}

// HSV, XYZ, and LAB are only used internally for computing color differences, so they are declared in this cpp file only.
typedef std::array<double, 3> HSV;
typedef std::array<double, 3> XYZ;  // Intermediate space for converting between LAB and RGB
typedef std::array<double, 3> LAB;  // CIELAB was designed to match human visual perception; the standard method for perceptual color difference
// Common white points
const XYZ D65_WHITE = {0.95047, 1.0, 1.08883};

/**
 * @brief Convert an RGB color to the HSV color space.
 *
 * @param rgb Input RGB color [R, G, B], range 0~255.
 * @return HSV output [H, S, V], H in 0~360, S and V in 0~1.
 */
static HSV convert_rgb_to_hsv(const RGB& rgb) {
    // Normalize to [0, 1]
    double r = rgb[0] / 255.0;
    double g = rgb[1] / 255.0;
    double b = rgb[2] / 255.0;

    double max = std::max({r, g, b});
    double min = std::min({r, g, b});
    double delta = max - min;

    // Compute hue H
    double h = 0;
    if (delta == 0) {
        h = 0;  // Gray; hue is undefined
    } else {
        if (max == r) {
            h = 60.0 * fmod((g - b) / delta, 6.0);
        } else if (max == g) {
            h = 60.0 * ((b - r) / delta + 2.0);
        } else {  // max == b
            h = 60.0 * ((r - g) / delta + 4.0);
        }
        if (h < 0) {
            h += 360.0;
        }
    }

    // Compute saturation S
    double s = (max == 0) ? 0 : (delta / max);

    // Compute value V
    double v = max;

    return {h, s, v};
}

/**
 * @brief Convert an HSV color to the RGB color space.
 *
 * @param hsv Input HSV color [H, S, V], H in 0~360, S and V in 0~1.
 * @return RGB output [R, G, B], range 0~255.
 */
static RGB convert_hsv_to_rgb(const HSV& hsv) {
    double h = hsv[0];
    double s = hsv[1];
    double v = hsv[2];

    double c = v * s;
    double x = c * (1 - std::abs(fmod(h / 60.0, 2.0) - 1));
    double m = v - c;

    double r, g, b;

    if (h < 60) {
        r = c;
        g = x;
        b = 0;
    } else if (h < 120) {
        r = x;
        g = c;
        b = 0;
    } else if (h < 180) {
        r = 0;
        g = c;
        b = x;
    } else if (h < 240) {
        r = 0;
        g = x;
        b = c;
    } else if (h < 300) {
        r = x;
        g = 0;
        b = c;
    } else {
        r = c;
        g = 0;
        b = x;
    }

    return {static_cast<std::size_t>((r + m) * 255 + 0.5), static_cast<std::size_t>((g + m) * 255 + 0.5), static_cast<std::size_t>((b + m) * 255 + 0.5)};
}

static XYZ convert_rgb_to_xyz(const RGB& color_rgb) {
    ColorDouble rgb{static_cast<double>(color_rgb[0]), static_cast<double>(color_rgb[1]), static_cast<double>(color_rgb[2])};
    auto gammaCorrect = [](double v) -> double {
        v = v / 255.0;
        if (v > 0.04045) {
            return std::pow((v + 0.055) / 1.055, 2.4);
        } else {
            return v / 12.92;
        }
    };

    double r = gammaCorrect(rgb[0]);
    double g = gammaCorrect(rgb[1]);
    double b = gammaCorrect(rgb[2]);

    // sRGB to XYZ matrix
    return {r * 0.4124564 + g * 0.3575761 + b * 0.1804375, r * 0.2126729 + g * 0.7151522 + b * 0.0721750, r * 0.0193339 + g * 0.1191920 + b * 0.9503041};
}

// sRGB non-linear channel values [0,1] (double) -> linear light -> XYZ; equivalent to convert_rgb_to_xyz when v=n/255
static XYZ convert_srgb01_to_xyz(double rs, double gs, double bs) {
    auto gamma_correct = [](double v) -> double {
        v = std::clamp(v, 0.0, 1.0);
        return (v > 0.04045) ? std::pow((v + 0.055) / 1.055, 2.4) : (v / 12.92);
    };
    const double r = gamma_correct(rs);
    const double g = gamma_correct(gs);
    const double b = gamma_correct(bs);
    return {r * 0.4124564 + g * 0.3575761 + b * 0.1804375, r * 0.2126729 + g * 0.7151522 + b * 0.0721750, r * 0.0193339 + g * 0.1191920 + b * 0.9503041};
}

static LAB convert_xyz_to_lab(const XYZ& xyz) {
    auto f = [](double t) -> double {
        const double delta = 6.0 / 29.0;
        if (t > delta * delta * delta) {
            return std::cbrt(t);
        } else {
            return t / (3.0 * delta * delta) + 4.0 / 29.0;
        }
    };

    // D65 white point
    double xn = D65_WHITE[0], yn = D65_WHITE[1], zn = D65_WHITE[2];

    double fx = f(xyz[0] / xn);
    double fy = f(xyz[1] / yn);
    double fz = f(xyz[2] / zn);

    return {116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

// ColorDouble here represents sRGB [0,1]; see calc_rgb_color_difference_by_ciede2000_srgb01 header comment
static LAB convert_srgb01_to_lab(const ColorDouble& srgb01) {
    return convert_xyz_to_lab(convert_srgb01_to_xyz(srgb01[0], srgb01[1], srgb01[2]));
}

static LAB convert_rgb_to_lab(const RGB& rgb) {
    return convert_xyz_to_lab(convert_rgb_to_xyz(rgb));
}

static Color convert_lab_to_rgb(const LAB& lab) {
    // Lab → XYZ
    const double delta = 6.0 / 29.0;
    const double delta2x3 = 3.0 * delta * delta;

    double fy = (lab[0] + 16.0) / 116.0;
    double fx = lab[1] / 500.0 + fy;
    double fz = fy - lab[2] / 200.0;

    double x = D65_WHITE[0] * (fx > delta ? fx * fx * fx : delta2x3 * (fx - 4.0 / 29.0));
    double y = D65_WHITE[1] * (fy > delta ? fy * fy * fy : delta2x3 * (fy - 4.0 / 29.0));
    double z = D65_WHITE[2] * (fz > delta ? fz * fz * fz : delta2x3 * (fz - 4.0 / 29.0));

    // XYZ -> linear RGB (sRGB inverse matrix)
    double r_lin = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z;
    double g_lin = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z;
    double b_lin = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z;

    // linear RGB -> sRGB (inverse gamma correction)
    auto inverse_gamma = [](double v) -> double {
        v = std::max(v, 0.0);
        return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    };

    auto to_uint8 = [](double v) -> std::size_t { return static_cast<std::size_t>(std::clamp(std::round(v * 255.0), 0.0, 255.0)); };

    return {to_uint8(inverse_gamma(r_lin)), to_uint8(inverse_gamma(g_lin)), to_uint8(inverse_gamma(b_lin))};
}

/**
 * @brief CIEDE2000 color-difference computation.
 *
 * Currently the most accurate color-difference formula, recommended by CIE as the industry standard.
 *
 * @param lab1 LAB values of the first color.
 * @param lab2 LAB values of the second color.
 * @return Color difference (typically < 1.0 is imperceptible to the human eye).
 */
static double ciede2000(const std::array<double, 3>& lab1, const std::array<double, 3>& lab2) {
    // Parameters in the CIE L*C*h* formula
    double L1 = lab1[0], a1 = lab1[1], b1 = lab1[2];
    double L2 = lab2[0], a2 = lab2[1], b2 = lab2[2];

    // Compute C1 and C2
    double C1 = std::sqrt(a1 * a1 + b1 * b1);
    double C2 = std::sqrt(a2 * a2 + b2 * b2);
    double C_avg = (C1 + C2) / 2.0;

    // G factor (compensates for non-linearity in the mid-low chroma region)
    double C7 = C_avg * C_avg * C_avg * C_avg * C_avg * C_avg * C_avg;
    double G = 0.5 * (1.0 - std::sqrt(C7 / (C7 + 6103515625.0)));

    // a1' and a2'
    double a1_prime = (1.0 + G) * a1;
    double a2_prime = (1.0 + G) * a2;

    // C'1 and C'2
    double C1_prime = std::sqrt(a1_prime * a1_prime + b1 * b1);
    double C2_prime = std::sqrt(a2_prime * a2_prime + b2 * b2);
    double C_prime_avg = (C1_prime + C2_prime) / 2.0;

    // h'1 and h'2
    double h1_prime = std::atan2(b1, a1_prime);
    double h2_prime = std::atan2(b2, a2_prime);
    if (h1_prime < 0) {
        h1_prime += 2 * M_PI;
    }
    if (h2_prime < 0) {
        h2_prime += 2 * M_PI;
    }

    // Compute dh'
    double dh_prime;
    if (std::abs(h1_prime - h2_prime) <= M_PI) {
        dh_prime = h2_prime - h1_prime;
    } else if (h2_prime <= h1_prime) {
        dh_prime = h2_prime - h1_prime + 2 * M_PI;
    } else {
        dh_prime = h2_prime - h1_prime - 2 * M_PI;
    }

    // Compute dH'
    double dH_prime = 2.0 * std::sqrt(C1_prime * C2_prime) * std::sin(dh_prime / 2.0);

    // Compute dL'
    double dL_prime = L2 - L1;

    // Compute dC'
    double dC_prime = C2_prime - C1_prime;

    // Compute h_prime_avg
    double h_prime_avg;
    if (std::abs(h1_prime - h2_prime) > M_PI) {
        h_prime_avg = (h1_prime + h2_prime + 2 * M_PI) / 2.0;
    } else {
        h_prime_avg = (h1_prime + h2_prime) / 2.0;
    }

    // Compute T
    double T = 1.0 - 0.17 * std::cos(h_prime_avg - M_PI / 6.0) + 0.24 * std::cos(2.0 * h_prime_avg) + 0.32 * std::cos(3.0 * h_prime_avg + M_PI / 30.0) -
               0.20 * std::cos(4.0 * h_prime_avg - 3.0 * M_PI / 6.0);

    // Compute rotation term R_T = -R_C * sin(2*delta_theta), where delta_theta = 30 * exp(-((h_bar'-275)/25)^2)
    // h_prime_avg is in radians; convert to degrees for delta_theta; 2*delta_theta = 60 * exp(...), convert back to radians for sin
    double h_prime_avg_deg = h_prime_avg * 180.0 / M_PI;
    double C_prime_avg_7 = std::pow(C_prime_avg, 7);
    double R = -2.0 * std::sqrt(C_prime_avg_7 / (C_prime_avg_7 + 6103515625.0)) *
               std::sin((60.0 * M_PI / 180.0) * std::exp(-std::pow((h_prime_avg_deg - 275.0) / 25.0, 2)));

    // Compute SL, SC, SH
    double L_prime_avg = (L1 + L2) / 2.0;
    double SL = 1.0 + 0.015 * std::pow(L_prime_avg - 50.0, 2) / std::sqrt(20 + std::pow(L_prime_avg - 50.0, 2));
    double SC = 1.0 + 0.045 * C_prime_avg;
    double SH = 1.0 + 0.015 * C_prime_avg * T;

    // Final color difference
    double deltaE = std::sqrt(std::pow(dL_prime / SL, 2) + std::pow(dC_prime / SC, 2) + std::pow(dH_prime / SH, 2) + R * (dC_prime / SC) * (dH_prime / SH));

    return deltaE;
}

double calc_rgb_color_difference_by_ciede2000(const RGB& rgb1, const RGB& rgb2) {
    auto lab1 = convert_rgb_to_lab(rgb1);
    auto lab2 = convert_rgb_to_lab(rgb2);
    return ciede2000(lab1, lab2);
}

double calc_rgb_color_difference_by_ciede2000_srgb01(const ColorDouble& rgb1, const ColorDouble& rgb2) {
    const LAB lab1 = convert_srgb01_to_lab(rgb1);
    const LAB lab2 = convert_srgb01_to_lab(rgb2);
    return ciede2000(lab1, lab2);
}

// Working-space distance function type: inputs are two colors in the same space (RGB-double or Lab)
using WorkingDistFunc = double (*)(const ColorDouble&, const ColorDouble&);

// Farthest Point Sampling (FPS) initialization algorithm.
// The first center is the point nearest to the global centroid; subsequent centers are the points farthest from the existing set.
static std::vector<ColorDouble> farthest_point_sampling_init(const std::vector<ColorDouble>& working_colors, std::size_t k, WorkingDistFunc dist_func) {
    std::vector<ColorDouble> centers(k);

    // 1. Compute the global centroid and pick the nearest point as the first center
    ColorDouble centroid = {0.0, 0.0, 0.0};
    for (const auto& c : working_colors) {
        centroid[0] += c[0];
        centroid[1] += c[1];
        centroid[2] += c[2];
    }
    const auto n = static_cast<double>(working_colors.size());
    centroid[0] /= n;
    centroid[1] /= n;
    centroid[2] /= n;

    double best_dist = std::numeric_limits<double>::max();
    std::size_t first_idx = 0;
    for (std::size_t i = 0; i < working_colors.size(); ++i) {
        double d = dist_func(working_colors[i], centroid);
        if (d < best_dist) {
            best_dist = d;
            first_idx = i;
        }
    }
    centers[0] = working_colors[first_idx];

    // Minimum distance from each point to the already-selected center set
    std::vector<double> min_distances(working_colors.size(), std::numeric_limits<double>::max());

    // 2. Greedily select the remaining K-1 centers: pick the point with the largest min_distance each time
    for (std::size_t i = 1; i < k; ++i) {
        const ColorDouble& last_center = centers[i - 1];

        // Update each point's minimum distance with the newly added center
        double farthest_dist = -1.0;
        std::size_t farthest_idx = 0;
        for (std::size_t c_idx = 0; c_idx < working_colors.size(); ++c_idx) {
            double d = dist_func(working_colors[c_idx], last_center);
            if (d < min_distances[c_idx]) {
                min_distances[c_idx] = d;
            }
            if (min_distances[c_idx] > farthest_dist) {
                farthest_dist = min_distances[c_idx];
                farthest_idx = c_idx;
            }
        }

        centers[i] = working_colors[farthest_idx];
    }

    return centers;
}

bool remesh_mesh(TriMesh& bbs_mesh, std::vector<std::size_t>& face_labels, double target_edge_length_ratio) {
    if (face_labels.size() != bbs_mesh.indices.size()) {
        BOOST_LOG_TRIVIAL(warning) << "Input mesh face count does not match label count";
        return false;
    }

    // Back up face labels for recovery after remeshing.
    std::vector<std::size_t> face_labels_of_original_mesh(face_labels);

    CGALMesh cgal_mesh;
    cgalutils::convert_trimesh_to_cgal(bbs_mesh, cgal_mesh);

    // AABBTreeIndirect references vertices/faces externally, so snapshot the
    // pre-remesh geometry by moving it out of bbs_mesh (which is overwritten
    // below with the post-remesh mesh). std::move on std::vector is O(1).
    TriVertices old_vertices = std::move(bbs_mesh.vertices);
    TriFaces    old_indices  = std::move(bbs_mesh.indices);
    auto original_mesh_tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(old_vertices, old_indices);

    std::unordered_set<CGAL::SM_Edge_index> feature_edges;
    std::unordered_set<CGAL::SM_Vertex_index> feature_vertices;
    CGALMesh::Property_map<CGALMesh::Edge_index, bool> constrained_edges =
        cgal_mesh.add_property_map<CGALMesh::Edge_index, bool>("constrained_edges", false).first;
    CGALMesh::Property_map<CGALMesh::Vertex_index, bool> constrained_vertices =
        cgal_mesh.add_property_map<CGALMesh::Vertex_index, bool>("constrained_vertices", false).first;

    // An edge is considered a geometric feature edge if its dihedral angle is less than 135 degrees (loose threshold)
    constexpr double feature_angle = 135;

    auto is_feature_edge = [&](CGAL::SM_Edge_index edge) -> bool {
        if (cgal_mesh.is_border(edge)) {
            return true;
        }
        auto halfedge_1 = cgal_mesh.halfedge(edge);
        auto halfedge_2 = cgal_mesh.opposite(halfedge_1);
        auto face_1 = cgal_mesh.face(halfedge_1);
        auto face_2 = cgal_mesh.face(halfedge_2);
        if (face_labels[face_1] != face_labels[face_2]) {
            // Boundary between different color regions; treated as a feature edge
            return true;
        }
        // TODO: CGAL remeshing tends to crash when too many constrained edges are added; needs handling
        //auto normal_1 = PMP::compute_face_normal(face_1, cgal_mesh);
        //auto normal_2 = PMP::compute_face_normal(face_2, cgal_mesh);
        //double angle = 180 - get_angle_between_vectors(normal_1, normal_2);
        //BOOST_LOG_TRIVIAL(debug) << "end.\n";
        //return angle > feature_angle;
        return false;
    };

    for (auto edge : cgal_mesh.edges()) {
        if (is_feature_edge(edge)) {
            feature_edges.insert(edge);
            feature_vertices.insert(cgal_mesh.source(cgal_mesh.halfedge(edge)));
            feature_vertices.insert(cgal_mesh.target(cgal_mesh.halfedge(edge)));
            constrained_edges[edge] = true;
            constrained_vertices[cgal_mesh.source(cgal_mesh.halfedge(edge))] = true;
            constrained_vertices[cgal_mesh.target(cgal_mesh.halfedge(edge))] = true;
        }
    }

#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "remesh_mesh: feature_edges.size() = " << feature_edges.size() << ".\n";

    std::vector<std::vector<CGALKernel::Point_3>> polylines;
    for (auto edge : feature_edges) {
        auto src_vtx = cgal_mesh.source(cgal_mesh.halfedge(edge));
        auto trg_vtx = cgal_mesh.target(cgal_mesh.halfedge(edge));
        polylines.push_back({cgal_mesh.point(src_vtx), cgal_mesh.point(trg_vtx)});
    }
    save_polylines("ColorUtils_remesh_feature_lines.obj", polylines);
#endif  // DEBUG_FLAG

    std::size_t iters = 5;
#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "PMP::isotropic_remeshing start...\n";
#endif  // DEBUG_FLAG
    // TODO: CGAL remeshing preserves geometric boundaries but does not maintain face labels well; colors need to be recomputed
    PMP::isotropic_remeshing(cgal_mesh.faces(), target_edge_length_ratio * detail::average_edge_length_impl(cgal_mesh), cgal_mesh,
                             CGAL::parameters::number_of_iterations(iters)
                                 .protect_constraints(true)
                                 .edge_is_constrained_map(constrained_edges)
                                 .vertex_is_constrained_map(constrained_vertices)
                                 .collapse_constraints(true));
#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "PMP::isotropic_remeshing finshed...\n";
#endif  // DEBUG_FLAG
    if (PMP::does_self_intersect(cgal_mesh)) {
        PMP::experimental::remove_self_intersections(cgal_mesh);
    }

    bbs_mesh.clear();

    std::unordered_map<CGAL::SM_Vertex_index, std::size_t> map_cgal_vtx_to_bbs_vtx;
    TriVertices bbs_vertices;
    TriFaces bbs_faces;
    face_labels.clear();
    face_labels.reserve(cgal_mesh.number_of_faces());

    for (const auto& cgal_vtx : cgal_mesh.vertices()) {
        if (!cgal_mesh.is_valid(cgal_vtx) || cgal_mesh.is_removed(cgal_vtx) || cgal_mesh.is_isolated(cgal_vtx)) {
            continue;
        }
        if (!map_cgal_vtx_to_bbs_vtx.count(cgal_vtx)) {
            map_cgal_vtx_to_bbs_vtx[cgal_vtx] = bbs_vertices.size();
            const auto& cgal_point = cgal_mesh.point(cgal_vtx);
            bbs_vertices.emplace_back(TriVertex(cgal_point.x(), cgal_point.y(), cgal_point.z()));
        }
    }

    for (const auto& cgal_face : cgal_mesh.faces()) {
        if (!cgal_mesh.is_valid(cgal_face) || cgal_mesh.is_removed(cgal_face)) {
            continue;
        }
        TriFace bbs_face;
        std::size_t face_vid = 0;
        for (auto cgal_vtx : cgal_mesh.vertices_around_face(cgal_mesh.halfedge(cgal_face))) {
            do { if (!(map_cgal_vtx_to_bbs_vtx.count(cgal_vtx))) { BOOST_LOG_TRIVIAL(warning) << "CGAL mesh contains a face with an invalid vertex"; return false; } } while(0);
            bbs_face[face_vid] = map_cgal_vtx_to_bbs_vtx[cgal_vtx];
            ++face_vid;
        }
        bbs_faces.push_back(std::move(bbs_face));
    }

    bbs_mesh = TriMesh(bbs_faces, bbs_vertices);

    face_labels.resize(bbs_mesh.indices.size());
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, bbs_mesh.indices.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            auto& face = bbs_mesh.indices[fid];
            Vec3f face_centroid = (bbs_mesh.vertices[face[0]] + bbs_mesh.vertices[face[1]] + bbs_mesh.vertices[face[2]]) / 3.0;
            size_t hit_idx = 0;
            Vec3f  closest;
            AABBTreeIndirect::squared_distance_to_indexed_triangle_set(
                old_vertices, old_indices, original_mesh_tree, face_centroid, hit_idx, closest);
            face_labels[fid] = face_labels_of_original_mesh[hit_idx];
        }
    });

    return true;
}

bool is_closed(const TriMesh& bbs_mesh) {
    CGALMesh cgal_mesh;
    cgalutils::convert_trimesh_to_cgal(bbs_mesh, cgal_mesh);

#ifdef DEBUG_FLAG
    std::size_t border_edges_count = 0;
    std::size_t edges_count = 0;
    for (auto edge : cgal_mesh.edges()) {
        if (cgal_mesh.is_border(edge)) {
            ++border_edges_count;
        }
        ++edges_count;
    }

    BOOST_LOG_TRIVIAL(debug) << "border edges count = " << border_edges_count << "\n";
    BOOST_LOG_TRIVIAL(debug) << "edges count = " << edges_count << "\n";

    std::size_t border_faces_count = 0;
    std::size_t faces_count = 0;
    for (auto face : cgal_mesh.faces()) {
        for (auto halfedge : cgal_mesh.halfedges_around_face(cgal_mesh.halfedge(face))) {
            auto edge = cgal_mesh.edge(halfedge);
            if (cgal_mesh.is_border(edge)) {
                ++border_faces_count;
                break;
            }
        }
        ++faces_count;
    }

    BOOST_LOG_TRIVIAL(debug) << "border faces count = " << border_faces_count << "\n";
    BOOST_LOG_TRIVIAL(debug) << "faces count = " << faces_count << "\n";

    BOOST_LOG_TRIVIAL(debug) << "vertices count = " << cgal_mesh.number_of_vertices() << "\n";
    std::size_t num_of_components = 0;
    std::unordered_set<CGAL::SM_Face_index> visited_faces;
    for (auto src_face : cgal_mesh.faces()) {
        if (visited_faces.count(src_face)) {
            continue;
        }
        ++num_of_components;
        std::queue<CGAL::SM_Face_index> que;
        que.push(src_face);
        visited_faces.insert(src_face);
        while (!que.empty()) {
            auto curr_face = que.front();
            que.pop();
            for (auto adj_face : cgal_mesh.faces_around_face(cgal_mesh.halfedge(curr_face))) {
                if (!cgal_mesh.is_valid(adj_face) || cgal_mesh.is_removed(adj_face) || visited_faces.count(adj_face)) {
                    continue;
                }
                que.push(adj_face);
                visited_faces.insert(adj_face);
            }
        }
    }
    BOOST_LOG_TRIVIAL(debug) << "components count = " << num_of_components << "\n";
#endif  // DEBUG_FLAG

    return CGAL::is_closed(cgal_mesh);
}

static bool smooth_region_labels(TriMesh& tri_mesh, std::vector<std::size_t>& face_labels, const SmoothParameters& smooth_parameters) {
    CGALMesh mesh;
    cgalutils::convert_trimesh_to_cgal(tri_mesh, mesh);

    if (mesh.number_of_faces() != face_labels.size()) {
        BOOST_LOG_TRIVIAL(warning) << "Face count does not match label count";
        return false;
    }

    if (smooth_parameters.smooth_weight >= TOPO_SMOOTH_WEIGHT_THRESHOLD) {
        // Topological smoothing: reassign face labels.
        smooth_region_topo_boundary(mesh, face_labels);
    }

    if (smooth_parameters.smooth_weight > EPSILON) {
        // Geometric smoothing: smooth polylines and project back onto the original mesh.
        smooth_region_geom_boundary(mesh, face_labels, smooth_parameters);
    }

    tri_mesh = cgalutils::cgal_to_trimesh(mesh);

    return true;
}

bool smooth_region(TriMesh& tri_mesh, std::vector<std::array<std::size_t, 3>>& face_colors, const SmoothParameters& smooth_parameters) {
    // Convert colors to labels
    std::size_t label_next = 0;
    std::vector<std::size_t> face_labels;
    face_labels.reserve(face_colors.size());
    std::map<std::array<std::size_t, 3>, std::size_t> map_color_to_label;
    std::unordered_map<std::size_t, std::array<std::size_t, 3>> map_label_to_color;
    for (auto& color : face_colors) {
        if (!map_color_to_label.count(color)) {
            map_color_to_label[color] = label_next;
            map_label_to_color[label_next] = color;
            ++label_next;
        }
        face_labels.push_back(map_color_to_label[color]);
    }

    if (!smooth_region_labels(tri_mesh, face_labels, smooth_parameters))
        return false;

    // Convert labels back to colors
    for (std::size_t fid = 0; fid < face_labels.size(); ++fid) {
        face_colors[fid] = map_label_to_color[face_labels[fid]];
    }

    return true;
}

bool smooth_region(TriMesh& tri_mesh, std::vector<std::size_t>& face_labels, const SmoothParameters& smooth_parameters) {
    return smooth_region_labels(tri_mesh, face_labels, smooth_parameters);
}

// Compute the squared Euclidean distance between two colors (RGB vectors)
double calc_rgb_color_difference_by_squared_rgb_double(const ColorDouble& c1, const ColorDouble& c2) {
    double dr = c1[0] - c2[0];
    double dg = c1[1] - c2[1];
    double db = c1[2] - c2[2];
    return dr * dr + dg * dg + db * db;
}

// Compute the squared Euclidean distance between two colors (RGB vectors)
double calc_rgb_color_difference_by_squared_rgb(const RGB& c1, const RGB& c2) {
    auto c1d = convert_rgb_uint_to_rgb_double(c1);
    auto c2d = convert_rgb_uint_to_rgb_double(c2);
    return calc_rgb_color_difference_by_squared_rgb_double(c1d, c2d);
}

// K-Means core: run FPS initialization + assign/update iterations in working space, return cluster centers
static std::vector<ColorDouble> kmeans_core(const std::vector<ColorDouble>& working_colors, std::size_t k, std::size_t max_iter, WorkingDistFunc dist_func,
                                            const std::function<bool()>& cancel_cb = nullptr) {
    std::vector<ColorDouble> centers = farthest_point_sampling_init(working_colors, k, dist_func);
    std::vector<std::size_t> assignments(working_colors.size());

    for (std::size_t iter = 0; iter < max_iter; ++iter) {
        if (cancel_cb && cancel_cb()) return centers;
        std::atomic<bool> changed(false);

        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, working_colors.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (std::size_t i = range.begin(); i < range.end(); ++i) {
                double min_dist = std::numeric_limits<double>::max();
                std::size_t best_cluster = 0;

                for (std::size_t j = 0; j < k; ++j) {
                    double d = dist_func(working_colors[i], centers[j]);
                    if (d < min_dist) {
                        min_dist = d;
                        best_cluster = j;
                    }
                }

                if (assignments[i] != best_cluster) {
                    changed.store(true, std::memory_order_relaxed);
                    assignments[i] = best_cluster;
                }
            }
        });

        if (!changed.load()) {
            break;
        }

        std::vector<ColorDouble> new_centers(k, {0.0, 0.0, 0.0});
        std::vector<std::size_t> counts(k, 0);

        for (std::size_t i = 0; i < working_colors.size(); ++i) {
            std::size_t cluster_id = assignments[i];
            ++counts[cluster_id];
            new_centers[cluster_id][0] += working_colors[i][0];
            new_centers[cluster_id][1] += working_colors[i][1];
            new_centers[cluster_id][2] += working_colors[i][2];
        }

        for (std::size_t i = 0; i < k; ++i) {
            if (counts[i] == 0) {
                double max_min_dist = -1.0;
                std::size_t best_idx = 0;
                for (std::size_t p = 0; p < working_colors.size(); ++p) {
                    double nearest = std::numeric_limits<double>::max();
                    for (std::size_t c = 0; c < k; ++c) {
                        if (c == i || counts[c] == 0) {
                            continue;
                        }
                        double d = dist_func(working_colors[p], centers[c]);
                        if (d < nearest) {
                            nearest = d;
                        }
                    }
                    if (nearest > max_min_dist) {
                        max_min_dist = nearest;
                        best_idx = p;
                    }
                }
                centers[i] = working_colors[best_idx];
            } else {
                centers[i][0] = new_centers[i][0] / counts[i];
                centers[i][1] = new_centers[i][1] / counts[i];
                centers[i][2] = new_centers[i][2] / counts[i];
            }
        }
    }

    return centers;
}

// K-Means clustering algorithm
std::vector<Color> cluster_k_means(const std::vector<Color>& colors, const ClusterParameters& cluster_parameters) {
    std::size_t k = cluster_parameters.cluster_k;
    std::size_t max_iter = cluster_parameters.max_iter;

    if (k == 0 || colors.empty()) {
        return {};
    }

    const bool use_lab = (cluster_parameters.color_difference_method != ColorDifferenceMethod::RGB);
    WorkingDistFunc working_dist_func = use_lab ? ciede2000 : calc_rgb_color_difference_by_squared_rgb_double;

    // ==========================================
    // 1. Preprocessing: deduplicate + pre-convert to working space
    // ==========================================
    std::vector<Color> unique_colors = colors;
    std::sort(unique_colors.begin(), unique_colors.end());
    auto last = std::unique(unique_colors.begin(), unique_colors.end());
    unique_colors.erase(last, unique_colors.end());

#ifdef DEBUG_FLAG
    BOOST_LOG_TRIVIAL(debug) << "Input colors: " << colors.size() << ", Unique colors: " << unique_colors.size();
#endif  // DEBUG_FLAG

    if (unique_colors.size() < k) {
        BOOST_LOG_TRIVIAL(warning) << "Unique color count (" << unique_colors.size() << ") is less than target K (" << k << "). Adjusting K.";
        k = unique_colors.size();
        if (k == 0) {
            return {};
        }
        return unique_colors;
    }

    std::vector<ColorDouble> working_colors(unique_colors.size());
    for (std::size_t i = 0; i < unique_colors.size(); ++i) {
        if (use_lab) {
            working_colors[i] = convert_rgb_to_lab(unique_colors[i]);
        } else {
            working_colors[i] = {static_cast<double>(unique_colors[i][0]), static_cast<double>(unique_colors[i][1]), static_cast<double>(unique_colors[i][2])};
        }
    }

    // ==========================================
    // 2. K-Means clustering
    // ==========================================
    auto centers = kmeans_core(working_colors, k, max_iter, working_dist_func, cluster_parameters.cancel_callback);

    // ==========================================
    // 3. Output: convert from working space back to RGB
    // ==========================================
    std::vector<Color> result(k);
    for (std::size_t i = 0; i < k; ++i) {
        if (use_lab) {
            result[i] = convert_lab_to_rgb(centers[i]);
        } else {
            result[i] = {static_cast<std::size_t>(std::round(centers[i][0])), static_cast<std::size_t>(std::round(centers[i][1])),
                         static_cast<std::size_t>(std::round(centers[i][2]))};
        }
    }

    return result;
}

std::vector<Color> cluster_to_specified_colors(const std::vector<Color>& colors, const std::vector<Color>& specified_colors) {
    std::vector<Color> cluster_colors = colors;
    std::vector<ColorDouble> specified_double_colors;
    specified_double_colors.reserve(specified_colors.size());
    for (auto& color : specified_colors) {
        specified_double_colors.push_back(ColorDouble{static_cast<double>(color[0]), static_cast<double>(color[1]), static_cast<double>(color[2])});
    }

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, cluster_colors.size()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t i = range.begin(); i < range.end(); ++i) {
            ColorDouble p_color{static_cast<double>(cluster_colors[i][0]), static_cast<double>(cluster_colors[i][1]),
                                static_cast<double>(cluster_colors[i][2])};

            double min_dist = std::numeric_limits<double>::max();
            std::size_t best_cluster = 0;

            for (std::size_t j = 0; j < specified_double_colors.size(); ++j) {
                double d = calc_rgb_color_difference_by_squared_rgb_double(p_color, specified_double_colors[j]);
                if (d < min_dist) {
                    min_dist = d;
                    best_cluster = j;
                }
            }

            cluster_colors[i] = specified_colors[best_cluster];
        }
    });

    return cluster_colors;
}

// Color PCA struct
struct ColorPCA {
    std::size_t color_idx;  // Index of the original color in ColorList
    double pca_value;  // Projection value onto the first principal component

    ColorPCA(std::size_t c_idx, double pca_val)
        : color_idx(c_idx),
          pca_value(pca_val) {}

    // Comparison operator (ascending order)
    bool operator<(const ColorPCA& other) const { return pca_value < other.pca_value; }

    // Equality check
    bool operator==(const ColorPCA& other) const { return color_idx == other.color_idx; }
};

[[maybe_unused]] static std::vector<ColorPCA> sort_colors_by_pca(const std::vector<Color>& colors) {
    const std::size_t n = colors.size();

    if (n == 0) {
        return {};
    }

    if (n == 1) {
        return {{0, 0.0}};
    }

    // Step 1: Data preprocessing - normalize to [0, 1]
    Eigen::MatrixXd data(n, 3);

    for (std::size_t i = 0; i < n; ++i) {
        data(i, 0) = static_cast<double>(colors[i][0]) / 255.0;  // R
        data(i, 1) = static_cast<double>(colors[i][1]) / 255.0;  // G
        data(i, 2) = static_cast<double>(colors[i][2]) / 255.0;  // B
    }

    // Step 2: Compute mean and center the data
    Eigen::RowVector3d mean = data.colwise().mean();
    Eigen::MatrixXd centered = data.rowwise() - mean;

    // Step 3: Compute covariance matrix (3x3)
    Eigen::Matrix3d cov = (centered.adjoint() * centered) / static_cast<double>(n - 1);

    // Step 4: Eigenvalue decomposition
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(cov);

    if (solver.info() != Eigen::Success) {
#ifdef DEBUG_FLAG
        std::cerr << "PCA: Eigenvalue decomposition failed" << std::endl;
#endif  // DEBUG_FLAG
        // Fallback: return an approximate result sorted by luminance.
        // Luminance is a key perceptual feature; convert RGB to grayscale (L = 0.299R + 0.587G + 0.114B) and sort in ascending order.
        std::vector<ColorPCA> result;
        result.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            double luminance = 0.299 * colors[i][0] + 0.587 * colors[i][1] + 0.114 * colors[i][2];
            result.push_back({i, luminance});
        }
        std::sort(result.begin(), result.end());
        return result;
    }

    // Get eigenvalues and eigenvectors (sorted by eigenvalue in descending order)
    Eigen::Vector3d eigenvalues = solver.eigenvalues();
    Eigen::Matrix3d eigenvectors = solver.eigenvectors();

    // Step 5: Find the eigenvector corresponding to the largest eigenvalue (first principal component)
    Eigen::MatrixXd::Index max_eigenvalue_idx;
    eigenvalues.maxCoeff(&max_eigenvalue_idx);

    Eigen::Vector3d first_principal_component = eigenvectors.col(max_eigenvalue_idx);

    // Step 6: Project centered data onto the first principal component
    Eigen::VectorXd projections = centered * first_principal_component;

    // Step 7: Build result and sort
    std::vector<ColorPCA> result;
    result.reserve(n);

    for (std::size_t i = 0; i < n; ++i) {
        result.push_back({i, projections(i)});
    }

    std::sort(result.begin(), result.end());

    return result;
}

std::vector<Color> cluster_adaptive(const std::vector<Color>& colors, const ClusterParameters& cluster_parameters) {
    if (colors.empty()) {
        return {};
    }

    const double max_color_distance = cluster_parameters.max_color_distance;
    const std::size_t max_iter = cluster_parameters.max_iter;
    const bool use_lab = (cluster_parameters.color_difference_method != ColorDifferenceMethod::RGB);
    WorkingDistFunc working_dist_func = use_lab ? ciede2000 : calc_rgb_color_difference_by_squared_rgb_double;

    // ==========================================
    // 1. Deduplicate + convert to working space
    // ==========================================
    std::vector<Color> unique_colors = colors;
    std::sort(unique_colors.begin(), unique_colors.end());
    auto last = std::unique(unique_colors.begin(), unique_colors.end());
    unique_colors.erase(last, unique_colors.end());

    BOOST_LOG_TRIVIAL(debug) << "cluster_adaptive: colors=" << colors.size()
                             << " unique=" << unique_colors.size()
                             << " max_color_distance=" << max_color_distance;

    if (unique_colors.size() <= 1) {
        return unique_colors;
    }

    std::vector<ColorDouble> working_colors(unique_colors.size());
    for (std::size_t i = 0; i < unique_colors.size(); ++i) {
        if (use_lab) {
            working_colors[i] = convert_rgb_to_lab(unique_colors[i]);
        } else {
            working_colors[i] = {static_cast<double>(unique_colors[i][0]), static_cast<double>(unique_colors[i][1]), static_cast<double>(unique_colors[i][2])};
        }
    }

    // ==========================================
    // 2. Binary search k: find the smallest k where P99 radius <= max_color_distance
    // ==========================================
    const std::size_t max_k = cluster_parameters.max_cluster_k;
    std::size_t lo = 1;
    std::size_t hi = std::min<std::size_t>(max_k, unique_colors.size());
    std::size_t best_k = 0;
    std::vector<ColorDouble> best_centers;

    constexpr double kRadiusPercentile = 0.99;

    auto calc_max_radius = [&](const std::vector<ColorDouble>& centers) -> double {
        std::vector<double> distances(working_colors.size());
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, working_colors.size()), [&](const tbb::blocked_range<size_t>& range) {
            for (std::size_t i = range.begin(); i < range.end(); ++i) {
                double min_dist = std::numeric_limits<double>::max();
                for (const auto& center : centers) {
                    double d = working_dist_func(working_colors[i], center);
                    if (d < min_dist) {
                        min_dist = d;
                    }
                }
                distances[i] = min_dist;
            }
        });
        if (distances.empty()) {
            return 0.0;
        }
        std::size_t idx = std::min<std::size_t>(static_cast<std::size_t>(distances.size() * kRadiusPercentile), distances.size() - 1);
        std::nth_element(distances.begin(), distances.begin() + idx, distances.end());
        return distances[idx];
    };

    const auto& cancel_cb = cluster_parameters.cancel_callback;

    while (lo <= hi) {
        if (cancel_cb && cancel_cb()) return {};
        std::size_t mid = lo + (hi - lo) / 2;
        auto centers = kmeans_core(working_colors, mid, max_iter, working_dist_func, cancel_cb);
        if (cancel_cb && cancel_cb()) return {};
        double max_radius = calc_max_radius(centers);

        BOOST_LOG_TRIVIAL(debug) << "cluster_adaptive binary search: k=" << mid << " max_radius=" << max_radius;

        if (max_radius <= max_color_distance) {
            best_k = mid;
            best_centers = std::move(centers);
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }

    if (best_k == 0) {
        best_k = std::min<std::size_t>(max_k, unique_colors.size());
        best_centers = kmeans_core(working_colors, best_k, max_iter, working_dist_func, cancel_cb);
        BOOST_LOG_TRIVIAL(warning) << "cluster_adaptive: binary search found no k satisfying max_radius<="
                                   << max_color_distance << ", fallback to k=" << best_k;
    }

    BOOST_LOG_TRIVIAL(debug) << "cluster_adaptive: best_k=" << best_k;

    // ==========================================
    // 3. Convert centers back to RGB
    // ==========================================
    std::vector<Color> result(best_k);
    for (std::size_t i = 0; i < best_k; ++i) {
        if (use_lab) {
            result[i] = convert_lab_to_rgb(best_centers[i]);
        } else {
            result[i] = {static_cast<std::size_t>(std::round(best_centers[i][0])), static_cast<std::size_t>(std::round(best_centers[i][1])),
                         static_cast<std::size_t>(std::round(best_centers[i][2]))};
        }
    }

    return result;
}

static std::vector<std::vector<CGAL::SM_Face_index>> get_connected_face_groups(const CGALMesh& mesh) {
    std::vector<std::vector<CGAL::SM_Face_index>> face_groups;
    std::unordered_set<CGAL::SM_Face_index> visited_faces;
    for (auto src_face : mesh.faces()) {
        if (visited_faces.count(src_face)) {
            continue;
        }
        std::vector<CGAL::SM_Face_index> face_group;
        std::queue<CGAL::SM_Face_index> que;
        que.push(src_face);
        visited_faces.insert(src_face);
        while (!que.empty()) {
            auto curr_face = que.front();
            que.pop();
            face_group.push_back(curr_face);
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(curr_face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face) || visited_faces.count(adj_face)) {
                    continue;
                }
                que.push(adj_face);
                visited_faces.insert(adj_face);
            }
        }
        face_groups.push_back(std::move(face_group));
    }
    return face_groups;
}

bool get_components(const TriMesh& bbs_mesh, const std::vector<Vec2f>& bbs_vertex_uvs, std::vector<TriMesh>& component_meshes,
                    std::vector<std::vector<Vec2f>>& component_vertex_uvs) {
    component_meshes.clear();
    component_vertex_uvs.clear();

    if (bbs_mesh.vertices.size() != bbs_vertex_uvs.size()) {
        BOOST_LOG_TRIVIAL(warning) << "Input mesh vertex count does not match texture coordinate count";
        return false;
    }

    CGALMesh cgal_mesh;
    std::vector<Vec2f> cgal_vertex_uvs;
    if (!cgalutils::convert_trimesh_to_cgal(bbs_mesh, bbs_vertex_uvs, cgal_mesh, cgal_vertex_uvs)) {
        BOOST_LOG_TRIVIAL(warning) << "Mesh conversion failed";
        return false;
    }

    auto face_groups = get_connected_face_groups(cgal_mesh);

    for (const auto& faces : face_groups) {
        std::unordered_map<CGAL::SM_Vertex_index, std::size_t> map_cgal_vtx_to_bbs_vtx;
        TriVertices bbs_vertices;
        TriFaces bbs_faces;
        std::vector<Vec2f> bbs_vertex_uvs;

        for (auto cgal_face : faces) {
            TriFace bbs_face;
            std::size_t face_vid = 0;
            for (auto cgal_vtx : cgal_mesh.vertices_around_face(cgal_mesh.halfedge(cgal_face))) {
                if (!map_cgal_vtx_to_bbs_vtx.count(cgal_vtx)) {
                    map_cgal_vtx_to_bbs_vtx[cgal_vtx] = bbs_vertices.size();
                    const auto& cgal_point = cgal_mesh.point(cgal_vtx);
                    bbs_vertices.push_back(TriVertex(cgal_point.x(), cgal_point.y(), cgal_point.z()));
                    bbs_vertex_uvs.push_back(cgal_vertex_uvs[cgal_vtx]);
                }
                bbs_face[face_vid] = map_cgal_vtx_to_bbs_vtx[cgal_vtx];
                ++face_vid;
            }
            bbs_faces.push_back(std::move(bbs_face));
        }

        component_meshes.push_back(TriMesh(bbs_faces, bbs_vertices));
        component_vertex_uvs.push_back(std::move(bbs_vertex_uvs));
    }

    return true;
}

bool calc_nearest_color_id(const std::vector<RGB>& colors, const RGB& color, std::size_t& nearest_color_id) {
    if (colors.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No color list provided";
        return false;
    }
    double min_dist = std::numeric_limits<double>::max();
    nearest_color_id = 0;
    for (std::size_t i = 0; i < colors.size(); ++i) {
        double dist = calc_rgb_color_difference_by_ciede2000(colors[i], color);
        if (dist < min_dist) {
            min_dist = dist;
            nearest_color_id = i;
        }
    }
    return true;
}

bool mesh_cluster(const TriMesh& bbs_mesh, const std::vector<RGB>& cluster_centers, std::vector<RGB>& map_face_to_rgb,
                  std::vector<std::size_t>& map_face_to_cluster_id) {
    if (cluster_centers.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "No cluster centers provided";
        return false;
    }

    CGALMesh mesh;
    cgalutils::convert_trimesh_to_cgal(bbs_mesh, mesh);
    if (mesh.number_of_faces() != bbs_mesh.indices.size()) {
        BOOST_LOG_TRIVIAL(warning) << "CGAL mesh face count does not match BBS mesh face count";
        return false;
    }
    if (mesh.number_of_faces() != map_face_to_rgb.size()) {
        BOOST_LOG_TRIVIAL(warning) << "CGAL mesh face count does not match RGB count";
        return false;
    }

    if (cluster_centers.size() == 1) {
        std::fill(map_face_to_rgb.begin(), map_face_to_rgb.end(), cluster_centers[0]);
#ifdef DEBUG_FLAG
        BOOST_LOG_TRIVIAL(debug) << "input cluster centers' size is 1, so we set all face RGB as same with it and return.\n";
#endif
        return true;
    }

    std::vector<double> map_face_to_area(mesh.number_of_faces());
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, mesh.number_of_faces()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            CGAL::SM_Face_index face(fid);
            map_face_to_area[fid] = std::max(PMP::face_area(face, mesh), EPSILON);
        }
    });

    // Step 1: Identify faces that definitely belong to a cluster center.
    // A face is definitively assigned when dist1 * absolute_difference_times < dist2 (nearest vs. second-nearest center).
    constexpr double absolute_difference_times = 1.5;
    // dE <= 1.0: imperceptible to the human eye, high-precision color matching
    // dE <= 2.0: slight difference, noticeable by experts; printing / image processing standard
    // dE <= 3.0: noticeable by ordinary observers; general quality control
    constexpr double difference_epsilon = 3.0;
    constexpr std::size_t invalid_cluster_id = std::numeric_limits<std::size_t>::max();
    map_face_to_cluster_id.resize(mesh.number_of_faces(), invalid_cluster_id);
    std::vector<std::vector<std::pair<double, std::size_t>>> map_face_to_dists(mesh.number_of_faces(),
                                                                               std::vector<std::pair<double, std::size_t>>(cluster_centers.size()));
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, mesh.number_of_faces()), [&](const tbb::blocked_range<size_t>& range) {
        for (std::size_t fid = range.begin(); fid < range.end(); ++fid) {
            CGAL::SM_Face_index face(fid);
            std::vector<std::pair<double, std::size_t>>& dist_and_cid_vec = map_face_to_dists[fid];
            for (std::size_t cluster_id = 0; cluster_id < cluster_centers.size(); ++cluster_id) {
                double dist = calc_rgb_color_difference_by_ciede2000(map_face_to_rgb[fid], cluster_centers[cluster_id]);
                //dist_and_cid_vec.emplace_back(dist, cluster_id);
                dist_and_cid_vec[cluster_id] = std::pair<double, std::size_t>{dist, cluster_id};
            }
            std::sort(dist_and_cid_vec.begin(), dist_and_cid_vec.end());
            if (dist_and_cid_vec[0].first < difference_epsilon || dist_and_cid_vec[0].first * absolute_difference_times < dist_and_cid_vec[1].first) {
                map_face_to_cluster_id[fid] = dist_and_cid_vec[0].second;
            }
        }
    });

    std::unordered_set<std::size_t> unclusted_fids;
    for (std::size_t fid = 0; fid < mesh.number_of_faces(); ++fid) {
        if (map_face_to_cluster_id[fid] == invalid_cluster_id) {
            unclusted_fids.insert(fid);
        }
    }

    auto convert_unclustered_to_clustered = [&](const std::unordered_set<std::size_t>& iter_clustered_fids) -> bool {
        if (iter_clustered_fids.empty()) {
            return false;
        }
        for (auto& fid : iter_clustered_fids) {
            unclusted_fids.erase(fid);
        }
        return true;
    };

    // Step 2: Flood. Use faces computed in the previous step as seeds and propagate outward.
    while (!unclusted_fids.empty()) {
        bool changed = false;
        std::unordered_set<std::size_t> iter_clustered_fids;
        // If an uncolored face has an adjacent color whose count exceeds the sum of all other colors, assign that color
        for (auto fid : unclusted_fids) {
            CGAL::SM_Face_index face(fid);
            std::size_t count = 0;
            std::unordered_map<std::size_t, std::size_t> map_cluster_id_to_count;
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {
                    continue;
                }
                ++count;
                ++map_cluster_id_to_count[map_face_to_cluster_id[adj_face]];
            }
            for (auto& [cluster_id, cnt] : map_cluster_id_to_count) {
                if (cluster_id != invalid_cluster_id && cnt * 2 > count) {
                    map_face_to_cluster_id[fid] = cluster_id;
                    iter_clustered_fids.insert(fid);
                    break;
                }
            }
        }
        changed = convert_unclustered_to_clustered(iter_clustered_fids) || changed;
        iter_clustered_fids.clear();

        // If a face's nearest cluster center (by color distance) happens to have an adjacent face, assign that color too
        for (auto fid : unclusted_fids) {
            CGAL::SM_Face_index face(fid);
            std::unordered_set<std::size_t> adj_cluster_ids;
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {
                    continue;
                }
                if (map_face_to_cluster_id[adj_face] == invalid_cluster_id) {
                    continue;
                }
                adj_cluster_ids.insert(map_face_to_cluster_id[adj_face]);
            }
            if (adj_cluster_ids.count(map_face_to_dists[fid].front().second)) {
                map_face_to_cluster_id[fid] = map_face_to_dists[fid].front().second;
                iter_clustered_fids.insert(fid);
            }
        }
        changed = convert_unclustered_to_clustered(iter_clustered_fids) || changed;
        iter_clustered_fids.clear();

        // If no face colors were modified in this iteration, stop
        if (!changed) {
            break;
        }
    }

    // Step 3: Handle remaining unclustered faces (run after the above operations complete)
    constexpr bool use_average_color = true;
    for (auto src_fid : std::vector<std::size_t>(unclusted_fids.begin(), unclusted_fids.end())) {
        if (!unclusted_fids.count(src_fid)) {
            continue;
        }
        // Compute connected unclustered faces
        std::queue<std::size_t> que;
        std::unordered_set<std::size_t> connected_unclustered_faces;
        que.push(src_fid);
        connected_unclustered_faces.insert(src_fid);
        double sum_r = 0, sum_g = 0, sum_b = 0;
        double sum_area = 0.0;
        std::unordered_map<std::size_t, double> map_cluster_id_to_adj_area;
        while (!que.empty()) {
            auto curr_fid = que.front();
            que.pop();
            sum_r += map_face_to_area[curr_fid] * map_face_to_rgb[curr_fid][0];
            sum_g += map_face_to_area[curr_fid] * map_face_to_rgb[curr_fid][1];
            sum_b += map_face_to_area[curr_fid] * map_face_to_rgb[curr_fid][2];
            sum_area += map_face_to_area[curr_fid];
            CGAL::SM_Face_index curr_face(curr_fid);
            for (auto adj_face : mesh.faces_around_face(mesh.halfedge(curr_face))) {
                if (!mesh.is_valid(adj_face) || mesh.is_removed(adj_face)) {  // Invalid face
                    continue;
                }
                if (map_face_to_cluster_id[adj_face] != invalid_cluster_id) {
                    map_cluster_id_to_adj_area[map_face_to_cluster_id[adj_face]] += map_face_to_area[adj_face];
                } else {
                    if (!connected_unclustered_faces.count(adj_face)) {  // Already clustered or already recorded
                        que.push(adj_face);
                        connected_unclustered_faces.insert(adj_face);
                    }
                }
            }
        }
        std::size_t matched_cluster_id = invalid_cluster_id;
        if (use_average_color) {
            // Use average color
            RGB average_color{static_cast<std::size_t>(sum_r / sum_area), static_cast<std::size_t>(sum_g / sum_area),
                              static_cast<std::size_t>(sum_b / sum_area)};
            double min_dist = std::numeric_limits<double>::max();
            for (auto& [cluster_id, area] : map_cluster_id_to_adj_area) {
                double dist = calc_rgb_color_difference_by_ciede2000(average_color, cluster_centers[cluster_id]);
                if (dist < min_dist) {
                    min_dist = dist;
                    matched_cluster_id = cluster_id;
                }
            }
        } else {
            // Use adjacent area
            double adj_max_area = 0.0;
            for (auto& [cluster_id, area] : map_cluster_id_to_adj_area) {
                if (area > adj_max_area) {
                    adj_max_area = area;
                    matched_cluster_id = cluster_id;
                }
            }
        }
        for (auto fid : connected_unclustered_faces) {
            map_face_to_cluster_id[fid] = matched_cluster_id;
            unclusted_fids.erase(fid);
        }
    }

    // Convert cluster center IDs to colors
    for (std::size_t fid = 0; fid < mesh.number_of_faces(); ++fid) {
        if (map_face_to_cluster_id[fid] == invalid_cluster_id) {
            map_face_to_rgb[fid] = cluster_centers[0];
            map_face_to_cluster_id[fid] = 0;  // Prevent out-of-bounds errors when using cluster_id later
        } else {
            map_face_to_rgb[fid] = cluster_centers[map_face_to_cluster_id[fid]];
        }
    }

    return true;
}

}  // namespace color_utils

}  // namespace tex2color
}  // namespace Slic3r
