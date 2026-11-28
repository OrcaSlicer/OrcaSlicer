#include "Exception.hpp"
#include "MeshBoolean.hpp"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TryCatchSignal.hpp"
#include "libslic3r/format.hpp"
#include <Eigen/Core>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <utility>
#include <igl/MeshBooleanType.h>
#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh/Surface_mesh.h>
#include <string>
#include <fstream>
#include <array>
#include <vector>
#include "libslic3r/Point.hpp"
#include <memory>
#include <CGAL/boost/graph/graph_traits_Surface_mesh.h>
#include <CGAL/boost/graph/helpers.h>
#include <boost/property_map/property_map.hpp>
#include <CGAL/Named_function_parameters.h>
#include <exception>
#include <optional>
#include <CGAL/Polygon_mesh_processing/measure.h>
#include <csignal>
#include <CGAL/Polygon_mesh_processing/self_intersections.h>
#include <iterator>
#include <CGAL/Polygon_mesh_processing/triangulate_hole.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <cstdint>
#include <map>
#include <set>
#include <algorithm>
#undef PI

#include <boost/next_prior.hpp>
#include "boost/log/trivial.hpp"
// Include igl first. It defines "L" macro which then clashes with our localization
#include <igl/copyleft/cgal/mesh_boolean.h>
#undef L

// CGAL headers
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Exact_integer.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Cartesian_converter.h>
#include <CGAL/Polygon_mesh_processing/orient_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/remesh.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
// BBS: for segment
#include <CGAL/mesh_segmentation.h>
#include <CGAL/property_map.h>
#include <CGAL/boost/graph/copy_face_graph.h>
#include <CGAL/boost/graph/Face_filtered_graph.h>
// For parameterize_lscm()
#include <CGAL/boost/graph/border.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/Polygon_mesh_processing/detect_features.h>
#include <CGAL/Surface_mesh_parameterization/Error_code.h>
#include <CGAL/Surface_mesh_parameterization/LSCM_parameterizer_3.h>
#include <CGAL/Surface_mesh_parameterization/parameterize.h>
// BBS: for boolean using mcut
#include "mcut/include/mcut/mcut.h"

namespace Slic3r {
namespace MeshBoolean {

using MapMatrixXfUnaligned = Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>>;
using MapMatrixXiUnaligned = Eigen::Map<const Eigen::Matrix<int,   Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor | Eigen::DontAlign>>;

TriangleMesh eigen_to_triangle_mesh(const EigenMesh &emesh)
{
    auto &VC = emesh.first; auto &FC = emesh.second;

    indexed_triangle_set its;
    its.vertices.reserve(size_t(VC.rows()));
    its.indices.reserve(size_t(FC.rows()));

    for (Eigen::Index i = 0; i < VC.rows(); ++i)
        its.vertices.emplace_back(VC.row(i).cast<float>());

    for (Eigen::Index i = 0; i < FC.rows(); ++i)
        its.indices.emplace_back(FC.row(i));

    return TriangleMesh { std::move(its) };
}

EigenMesh triangle_mesh_to_eigen(const TriangleMesh &mesh)
{
    EigenMesh emesh;
    emesh.first = MapMatrixXfUnaligned(mesh.its.vertices.front().data(),
                                       Eigen::Index(mesh.its.vertices.size()),
                                       3).cast<double>();

    emesh.second = MapMatrixXiUnaligned(mesh.its.indices.front().data(),
                                        Eigen::Index(mesh.its.indices.size()),
                                        3);
    return emesh;
}

void minus(EigenMesh &A, const EigenMesh &B)
{
    auto &[VA, FA] = A;
    auto &[VB, FB] = B;

    Eigen::MatrixXd VC;
    Eigen::MatrixXi FC;
    igl::MeshBooleanType boolean_type(igl::MESH_BOOLEAN_TYPE_MINUS);
    igl::copyleft::cgal::mesh_boolean(VA, FA, VB, FB, boolean_type, VC, FC);

    VA = std::move(VC); FA = std::move(FC);
}

void minus(TriangleMesh& A, const TriangleMesh& B)
{
    EigenMesh eA = triangle_mesh_to_eigen(A);
    minus(eA, triangle_mesh_to_eigen(B));
    A = eigen_to_triangle_mesh(eA);
}

void self_union(EigenMesh &A)
{
    EigenMesh result;
    auto &[V, F] = A;
    auto &[VC, FC] = result;

    igl::MeshBooleanType boolean_type(igl::MESH_BOOLEAN_TYPE_UNION);
    igl::copyleft::cgal::mesh_boolean(V, F, Eigen::MatrixXd(), Eigen::MatrixXi(), boolean_type, VC, FC);

    A = std::move(result);
}

void self_union(TriangleMesh& mesh)
{
    auto eM = triangle_mesh_to_eigen(mesh);
    self_union(eM);
    mesh = eigen_to_triangle_mesh(eM);
}

namespace cgal {

namespace CGALProc    = CGAL::Polygon_mesh_processing;
namespace CGALParams  = CGAL::parameters;

using EpecKernel = CGAL::Exact_predicates_exact_constructions_kernel;
using EpicKernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using _EpicMesh = CGAL::Surface_mesh<EpicKernel::Point_3>;
using _EpecMesh = CGAL::Surface_mesh<EpecKernel::Point_3>;

struct CGALMesh {
    _EpicMesh m;
    CGALMesh() = default;
    CGALMesh(const _EpicMesh& _m) :m(_m) {}
};

void save_CGALMesh(const std::string& fname, const CGALMesh& cgal_mesh) {
    std::ofstream os(fname);
    os << cgal_mesh.m;
    os.close();
}
// /////////////////////////////////////////////////////////////////////////////
// Converions from and to CGAL mesh
// /////////////////////////////////////////////////////////////////////////////

template<class _Mesh> void triangle_mesh_to_cgal(const TriangleMesh& M, _Mesh& out)
{
    using Index3 = std::array<size_t, 3>;

    if (M.empty()) return;

    std::vector<typename _Mesh::Point> points;
    std::vector<Index3> indices;
    points.reserve(M.its.vertices.size());
    indices.reserve(M.its.indices.size());
    for (auto& v : M.its.vertices) points.emplace_back(v.x(), v.y(), v.z());
    for (auto& _f : M.its.indices) {
        auto f = _f.cast<size_t>();
        indices.emplace_back(Index3{ f(0), f(1), f(2) });
    }

    CGALProc::orient_polygon_soup(points, indices);
    CGALProc::polygon_soup_to_polygon_mesh(points, indices, out);

    // Number the faces because 'orient_to_bound_a_volume' needs a face <--> index map
    unsigned index = 0;
    for (auto face : out.faces()) face = CGAL::SM_Face_index(index++);

    if (CGAL::is_closed(out))
        CGALProc::orient_to_bound_a_volume(out);
    else
        throw Slic3r::RuntimeError("Mesh not watertight");
}

template<class _Mesh>
void triangle_mesh_to_cgal(const std::vector<stl_vertex> &                 V,
                           const std::vector<stl_triangle_vertex_indices> &F,
                           _Mesh &out)
{
    if (F.empty()) return;

    size_t vertices_count = V.size();
    size_t edges_count    = (F.size()* 3) / 2;
    size_t faces_count    = F.size();
    out.reserve(vertices_count, edges_count, faces_count);

    for (auto &v : V)
        out.add_vertex(typename _Mesh::Point{v.x(), v.y(), v.z()});

    using VI = typename _Mesh::Vertex_index;
    for (auto &f : F)
        out.add_face(VI(f(0)), VI(f(1)), VI(f(2)));
}

inline Vec3f to_vec3f(const _EpicMesh::Point& v)
{
    return { float(v.x()), float(v.y()), float(v.z()) };
}

inline Vec3f to_vec3f(const _EpecMesh::Point& v)
{
    CGAL::Cartesian_converter<EpecKernel, EpicKernel> cvt;
    auto iv = cvt(v);
    return { float(iv.x()), float(iv.y()), float(iv.z()) };
}

template<class _Mesh>
indexed_triangle_set cgal_to_indexed_triangle_set(const _Mesh &cgalmesh)
{
    indexed_triangle_set its;
    its.vertices.reserve(cgalmesh.num_vertices());
    its.indices.reserve(cgalmesh.num_faces());

    const auto &faces = cgalmesh.faces();
    const auto &vertices = cgalmesh.vertices();
    int vsize = int(vertices.size());

    for (const auto &vi : vertices) {
        auto &v = cgalmesh.point(vi); // Don't ask...
        its.vertices.emplace_back(to_vec3f(v));
    }

    for (const auto &face : faces) {
        auto vtc = cgalmesh.vertices_around_face(cgalmesh.halfedge(face));

        int i = 0;
        Vec3i32 facet;
        for (auto v : vtc) {
            int iv = v;
            if (i > 2 || iv < 0 || iv >= vsize) { i = 0; break; }
            facet(i++) = iv;
        }

        if (i == 3)
            its.indices.emplace_back(facet);
    }

    return its;
}

template<class _Mesh> TriangleMesh cgal_to_triangle_mesh(const _Mesh &cgalmesh)
{
    indexed_triangle_set its = cgal_to_indexed_triangle_set(cgalmesh);
    return TriangleMesh(std::move(its));
}

std::unique_ptr<CGALMesh, CGALMeshDeleter>
triangle_mesh_to_cgal(const std::vector<stl_vertex> &V,
                      const std::vector<stl_triangle_vertex_indices> &F)
{
    std::unique_ptr<CGALMesh, CGALMeshDeleter> out(new CGALMesh{});
    triangle_mesh_to_cgal(V, F, out->m);
    // CGAL's volume predicate dereferences an incident halfedge for every vertex.
    // Remove isolated vertices and compact their indices for conversion back to an ITS.
    if (CGALProc::remove_isolated_vertices(out->m) != 0)
        out->m.collect_garbage();
    return out;
}

TriangleMesh cgal_to_triangle_mesh(const CGALMesh &cgalmesh)
{
    return TriangleMesh{cgal_to_indexed_triangle_set(cgalmesh.m)};
}

indexed_triangle_set cgal_to_indexed_triangle_set(const CGALMesh &cgalmesh)
{
    return cgal_to_indexed_triangle_set(cgalmesh.m);
}

// /////////////////////////////////////////////////////////////////////////////
// Isotropic remeshing
// /////////////////////////////////////////////////////////////////////////////

indexed_triangle_set remesh_isotropic(const indexed_triangle_set &mesh, double target_edge_length,
                                      unsigned n_iterations, double sharp_angle_deg,
                                      unsigned n_relaxation_steps)
{
    if (mesh.indices.empty() || target_edge_length <= 0.0)
        return mesh;

    _EpicMesh cgal_mesh;
    triangle_mesh_to_cgal(mesh.vertices, mesh.indices, cgal_mesh);
    if (cgal_mesh.is_empty() || cgal_mesh.number_of_faces() == 0)
        return mesh;

    // Surface_mesh::add_face() refuses any face that would make the mesh non-manifold and returns a
    // null descriptor instead. Remeshing a mesh that silently lost faces that way produces holes in
    // the output, so bail out and let the caller report it rather than hand back a punctured model.
    if (cgal_mesh.number_of_faces() != mesh.indices.size())
        return mesh;

    using edge_descriptor = boost::graph_traits<_EpicMesh>::edge_descriptor;
    try {
        // Sharp edges and open borders are pinned before remeshing. Without that, the tangential
        // relaxation pass slides vertices along the surface and rounds every hard feature off - a
        // cube comes back with wobbly, eroded edges, which is the most visible way "remeshing does
        // not work properly". protect_constraints() forbids splitting or collapsing them, but it
        // requires each constrained edge to already be shorter than 4/3 * target, hence the split
        // first (passing the map so the halves inherit the constraint). This mirrors CGAL's own
        // isotropic_remeshing example.
        auto ecm = cgal_mesh.add_property_map<edge_descriptor, bool>("e:is_constrained", false).first;
        if (sharp_angle_deg > 0.0)
            CGALProc::detect_sharp_edges(cgal_mesh, sharp_angle_deg, ecm);
        for (edge_descriptor e : edges(cgal_mesh)) {
            const auto h = halfedge(e, cgal_mesh);
            if (is_border(h, cgal_mesh) || is_border(opposite(h, cgal_mesh), cgal_mesh))
                put(ecm, e, true);
        }

        std::vector<edge_descriptor> constrained;
        for (edge_descriptor e : edges(cgal_mesh))
            if (get(ecm, e))
                constrained.push_back(e);
        if (!constrained.empty())
            CGALProc::split_long_edges(constrained, target_edge_length, cgal_mesh,
                                       CGALParams::edge_is_constrained_map(ecm));

        CGALProc::isotropic_remeshing(faces(cgal_mesh), target_edge_length, cgal_mesh,
                                      CGALParams::number_of_iterations(n_iterations)
                                          .number_of_relaxation_steps(n_relaxation_steps)
                                          .edge_is_constrained_map(ecm)
                                          .protect_constraints(true));
    } catch (const std::exception &) {
        return mesh; // CGAL throws on some non-manifold / degenerate inputs; leave the mesh untouched
    }
    if (cgal_mesh.number_of_faces() == 0)
        return mesh;

    // isotropic_remeshing edits in place, and its edge collapses only *mark* vertices and faces as
    // removed - the underlying arrays keep the holes until the garbage is collected. That matters
    // because cgal_to_indexed_triangle_set() numbers its output vertices by iteration order (which
    // skips removed slots) while reading each face's corner as the raw integer value of the vertex
    // descriptor (which does not). Past the first collapse the two disagree, so every triangle
    // points at the wrong vertices, and any descriptor beyond the live vertex count is dropped
    // together with its triangle. Compacting first makes descriptor == iteration order again.
    cgal_mesh.collect_garbage();
    return cgal_to_indexed_triangle_set(cgal_mesh);
}

// /////////////////////////////////////////////////////////////////////////////
// UV parameterization
// /////////////////////////////////////////////////////////////////////////////

std::optional<std::vector<Vec2f>> parameterize_lscm(const indexed_triangle_set &mesh)
{
    namespace SMP = CGAL::Surface_mesh_parameterization;

    if (mesh.indices.empty())
        return std::nullopt;

    _EpicMesh cgal_mesh;
    triangle_mesh_to_cgal(mesh.vertices, mesh.indices, cgal_mesh);

    using vertex_descriptor   = boost::graph_traits<_EpicMesh>::vertex_descriptor;
    using halfedge_descriptor = boost::graph_traits<_EpicMesh>::halfedge_descriptor;

    // LSCM assumes a single topological disk: one connected component, one boundary loop. A patch
    // with several disconnected painted islands, or with a hole in it, doesn't qualify -- bail out
    // rather than silently parameterizing just one arbitrary piece of it.
    {
        std::vector<std::size_t> component_id(num_faces(cgal_mesh));
        const std::size_t        num_components = CGAL::Polygon_mesh_processing::connected_components(
            cgal_mesh, CGAL::make_property_map(component_id));
        if (num_components != 1)
            return std::nullopt;
    }

    const halfedge_descriptor border = CGAL::Polygon_mesh_processing::longest_border(cgal_mesh).first;
    if (border == halfedge_descriptor())
        return std::nullopt; // no boundary at all -- a closed patch, which isn't a disk either

    // ...and exactly one boundary loop. One connected component is not enough on its own: a patch with
    // a hole in it (paint a ring, or erase the middle of a stroke) is a single component with two
    // loops, and LSCM will happily "parameterize" it into an overlapping, folded-over chart rather
    // than fail. Walk the border halfedges and check every one of them belongs to the longest loop.
    {
        std::size_t border_halfedges = 0;
        for (halfedge_descriptor h : halfedges(cgal_mesh))
            if (is_border(h, cgal_mesh))
                ++border_halfedges;
        std::size_t loop_length = 0;
        halfedge_descriptor h = border;
        do {
            ++loop_length;
            h = next(h, cgal_mesh);
        } while (h != border && loop_length <= border_halfedges);
        if (loop_length != border_halfedges)
            return std::nullopt; // more than one boundary loop -- not a topological disk
    }

    using Point_2 = EpicKernel::Point_2;
    using UV_pmap = _EpicMesh::Property_map<vertex_descriptor, Point_2>;
    UV_pmap uv_map = cgal_mesh.add_property_map<vertex_descriptor, Point_2>("h:uv", Point_2(0, 0)).first;

    using Parameterizer = SMP::LSCM_parameterizer_3<_EpicMesh>;
    const SMP::Error_code err = SMP::parameterize(cgal_mesh, Parameterizer(), border, uv_map);
    if (err != SMP::OK)
        return std::nullopt;

    // triangle_mesh_to_cgal() adds vertices in the exact same order as mesh.vertices (see above),
    // and Surface_mesh assigns indices sequentially on insertion into a fresh mesh, so a
    // vertex_descriptor's index here is guaranteed to match the original input vertex index --
    // the same assumption cgal_to_indexed_triangle_set() above already relies on.
    std::vector<Vec2f> result(mesh.vertices.size(), Vec2f::Zero());
    for (vertex_descriptor vd : vertices(cgal_mesh)) {
        const std::size_t idx = std::size_t(vd);
        if (idx < result.size()) {
            const Point_2 &uv = uv_map[vd];
            result[idx]        = Vec2f(float(uv.x()), float(uv.y()));
        }
    }
    return result;
}

// /////////////////////////////////////////////////////////////////////////////
// Boolean operations for CGAL meshes
// /////////////////////////////////////////////////////////////////////////////

static bool _cgal_diff(CGALMesh &A, CGALMesh &B, CGALMesh &R)
{
    const auto &p = CGALParams::throw_on_self_intersection(true);
    return CGALProc::corefine_and_compute_difference(A.m, B.m, R.m, p, p);
}

static bool _cgal_union(CGALMesh &A, CGALMesh &B, CGALMesh &R)
{
    const auto &p = CGALParams::throw_on_self_intersection(true);
    return CGALProc::corefine_and_compute_union(A.m, B.m, R.m, p, p);
}

static bool _cgal_intersection(CGALMesh &A, CGALMesh &B, CGALMesh &R)
{
    const auto &p = CGALParams::throw_on_self_intersection(true);
    return CGALProc::corefine_and_compute_intersection(A.m, B.m, R.m, p, p);
}

template<class Op> void _cgal_do(Op &&op, CGALMesh &A, CGALMesh &B)
{
    bool success = false;
    bool hw_fail = false;
    try {
        CGALMesh result;
        try_catch_signal({SIGSEGV, SIGFPE}, [&success, &A, &B, &result, &op] {
            success = op(A, B, result);
        }, [&] { hw_fail = true; });
        A = std::move(result);      // In-place operation does not work
    } catch (...) {
        success = false;
    }

    if (hw_fail)
        throw Slic3r::HardCrash("CGAL mesh boolean operation crashed.");

    if (! success)
        throw Slic3r::RuntimeError("CGAL mesh boolean operation failed.");
}

void minus(CGALMesh &A, CGALMesh &B) { _cgal_do(_cgal_diff, A, B); }
void plus(CGALMesh &A, CGALMesh &B) { _cgal_do(_cgal_union, A, B); }
void intersect(CGALMesh &A, CGALMesh &B) { _cgal_do(_cgal_intersection, A, B); }
bool does_self_intersect(const CGALMesh &mesh) { return CGALProc::does_self_intersect(mesh.m); }
// BBS
void segment(CGALMesh& src, std::vector<CGALMesh>& dst, double smoothing_alpha = 0.5, int segment_number=5)
{
    typedef boost::graph_traits<_EpicMesh>::face_descriptor face_descriptor;
    typedef _EpicMesh::Property_map<face_descriptor, double> Facet_double_map;
    typedef CGAL::Face_filtered_graph<_EpicMesh> Filtered_graph;

    _EpicMesh mesh = src.m;
    Facet_double_map sdf_property_map;

    sdf_property_map = mesh.add_property_map<face_descriptor, double>("f:sdf").first;

    CGAL::sdf_values(mesh, sdf_property_map);

    // create a property-map for segment-ids
    typedef _EpicMesh::Property_map<face_descriptor, std::size_t> Facet_int_map;
    Facet_int_map segment_property_map = mesh.add_property_map<face_descriptor, std::size_t>("f:sid").first;;
    // segment the mesh using default parameters for number of levels, and smoothing lambda
    // Any other scalar values can be used instead of using SDF values computed using the CGAL function
    std::size_t number_of_segments = CGAL::segmentation_from_sdf_values(mesh, sdf_property_map, segment_property_map, segment_number, smoothing_alpha);
    //print area of each segment and then put it in a Mesh and print it in an OFF file
    Filtered_graph segment_mesh(mesh);
    _EpicMesh mesh_merged;
    for (std::size_t id = 0; id < number_of_segments; ++id)
    {
        segment_mesh.set_selected_faces(id, segment_property_map);
        //std::cout << "Segment " << id << "'s area is : " << CGAL::Polygon_mesh_processing::area(segment_mesh) << std::endl;
        _EpicMesh out;
        CGAL::copy_face_graph(segment_mesh, out);

        // save_CGALMesh("out.off", out);

        // fill holes
        typedef boost::graph_traits<_EpicMesh>::halfedge_descriptor      halfedge_descriptor;
        typedef boost::graph_traits<_EpicMesh>::vertex_descriptor        vertex_descriptor;
        std::vector<halfedge_descriptor> border_cycles;
        CGAL::extract_boundary_cycles(out, std::back_inserter(border_cycles));
        for (halfedge_descriptor h : border_cycles)
        {
            std::vector<face_descriptor>  patch_facets;
#if 0
            std::vector<vertex_descriptor> patch_vertices;
            CGAL::Polygon_mesh_processing::triangulate_and_refine_hole(out, h, std::back_inserter(patch_facets),
                std::back_inserter(patch_vertices));
            std::cout << "* Number of facets in constructed patch: " << patch_facets.size() << std::endl;
            std::cout << "  Number of vertices in constructed patch: " << patch_vertices.size() << std::endl;
#else
            CGAL::Polygon_mesh_processing::triangulate_hole(out, h, CGAL::parameters::default_values().face_output_iterator(std::back_inserter(patch_facets)));
#endif
        }

        //if (id > 2) {
        //    mesh_merged.join(out);
        //}
        //else
        {
            dst.emplace_back(CGALMesh(out));
        }
    }
    //if (mesh_merged.is_empty() == false) {
    //    CGAL::Polygon_mesh_processing::stitch_borders(mesh_merged);
    //    dst.emplace_back(std::move(CGALMesh(mesh_merged)));
    //}
}

std::vector<TriangleMesh> segment(const TriangleMesh& src, double smoothing_alpha, int segment_number)
{
    CGALMesh in_cgal_mesh;
    MeshBoolean::cgal::triangle_mesh_to_cgal(src, in_cgal_mesh.m);
    std::vector<CGALMesh> out_cgal_meshes;
    segment(in_cgal_mesh, out_cgal_meshes, smoothing_alpha, segment_number);

    std::vector<TriangleMesh> out_meshes;
    for (auto& outf_cgal_mesh: out_cgal_meshes)
    {
        out_meshes.emplace_back(cgal_to_triangle_mesh(outf_cgal_mesh.m));
    }

    return out_meshes;
}

void merge(std::vector<_EpicMesh>& srcs, _EpicMesh& dst)
{
    _EpicMesh mesh_merged;
    for (size_t i = 0; i < srcs.size(); i++)
    {
        mesh_merged.join(srcs[i]);
    }
    if (mesh_merged.is_empty() == false) {
        CGAL::Polygon_mesh_processing::stitch_borders(mesh_merged);
        dst = std::move(mesh_merged);
    }
}

TriangleMesh merge(std::vector<TriangleMesh> meshes)
{
    std::vector<_EpicMesh> srcs(meshes.size());
    for (size_t i = 0; i < meshes.size(); i++)
    {
        MeshBoolean::cgal::triangle_mesh_to_cgal(meshes[i], srcs[i]);
    }
    _EpicMesh dst;
    merge(srcs, dst);
    return cgal_to_triangle_mesh(dst);
}

template<class Op> void _mesh_boolean_do(Op &&op, indexed_triangle_set &A, const indexed_triangle_set &B)
{
    CGALMesh meshA;
    CGALMesh meshB;
    triangle_mesh_to_cgal(A.vertices, A.indices, meshA.m);
    triangle_mesh_to_cgal(B.vertices, B.indices, meshB.m);

    _cgal_do(op, meshA, meshB);

    A = cgal_to_indexed_triangle_set(meshA.m);
}

template<class Op> void _mesh_boolean_do(Op &&op, TriangleMesh &A, const TriangleMesh &B)
{
    CGALMesh meshA;
    CGALMesh meshB;
    triangle_mesh_to_cgal(A.its.vertices, A.its.indices, meshA.m);
    triangle_mesh_to_cgal(B.its.vertices, B.its.indices, meshB.m);

    _cgal_do(op, meshA, meshB);

    A = cgal_to_triangle_mesh(meshA);
}

void minus(TriangleMesh &A, const TriangleMesh &B)
{
    _mesh_boolean_do(_cgal_diff, A, B);
}

void plus(TriangleMesh &A, const TriangleMesh &B)
{
    _mesh_boolean_do(_cgal_union, A, B);
}

void intersect(TriangleMesh &A, const TriangleMesh &B)
{
    _mesh_boolean_do(_cgal_intersection, A, B);
}

void minus(indexed_triangle_set &A, const indexed_triangle_set &B)
{
    _mesh_boolean_do(_cgal_diff, A, B);
}

void plus(indexed_triangle_set &A, const indexed_triangle_set &B)
{
    _mesh_boolean_do(_cgal_union, A, B);
}

void intersect(indexed_triangle_set &A, const indexed_triangle_set &B)
{
    _mesh_boolean_do(_cgal_intersection, A, B);
}

bool does_self_intersect(const TriangleMesh &mesh)
{
    CGALMesh cgalm;
    triangle_mesh_to_cgal(mesh.its.vertices, mesh.its.indices, cgalm.m);
    return CGALProc::does_self_intersect(cgalm.m);
}

void CGALMeshDeleter::operator()(CGALMesh *ptr) { delete ptr; }

bool does_bound_a_volume(const CGALMesh &mesh)
{
    return CGAL::is_closed(mesh.m) && CGALProc::does_bound_a_volume(mesh.m);
}

bool empty(const CGALMesh &mesh)
{
    return mesh.m.is_empty();
}

CGALMeshPtr clone(const CGALMesh &m)
{
    return CGALMeshPtr{new CGALMesh{m}};
}

} // namespace cgal


namespace mcut {
/* BBS: MusangKing
 * mcut mesh array format for Boolean Opts calculation
 */
struct McutMesh
{
    // variables for mesh data in a format suited for mcut
    std::vector<uint32_t> faceSizesArray;
    std::vector<uint32_t> faceIndicesArray;
    std::vector<double>   vertexCoordsArray;
};
void McutMeshDeleter::operator()(McutMesh *ptr) { delete ptr; }

bool empty(const McutMesh &mesh) { return mesh.vertexCoordsArray.empty() || mesh.faceIndicesArray.empty(); }
void triangle_mesh_to_mcut(const TriangleMesh &src_mesh, McutMesh &srcMesh, const Transform3d &src_nm = Transform3d::Identity())
{
    // vertices precision convention and copy
    srcMesh.vertexCoordsArray.reserve(src_mesh.its.vertices.size() * 3);
    for (int i = 0; i < src_mesh.its.vertices.size(); ++i) {
        const Vec3d v = src_nm * src_mesh.its.vertices[i].cast<double>();
        srcMesh.vertexCoordsArray.push_back(v[0]);
        srcMesh.vertexCoordsArray.push_back(v[1]);
        srcMesh.vertexCoordsArray.push_back(v[2]);
    }

    // faces copy
    srcMesh.faceIndicesArray.reserve(src_mesh.its.indices.size() * 3);
    srcMesh.faceSizesArray.reserve(src_mesh.its.indices.size());
    for (int i = 0; i < src_mesh.its.indices.size(); ++i) {
        const int &f0 = src_mesh.its.indices[i][0];
        const int &f1 = src_mesh.its.indices[i][1];
        const int &f2 = src_mesh.its.indices[i][2];
        srcMesh.faceIndicesArray.push_back(f0);
        srcMesh.faceIndicesArray.push_back(f1);
        srcMesh.faceIndicesArray.push_back(f2);

        srcMesh.faceSizesArray.push_back((uint32_t) 3);
    }
}

McutMeshPtr triangle_mesh_to_mcut(const indexed_triangle_set &M)
{
    std::unique_ptr<McutMesh, McutMeshDeleter> out(new McutMesh{});
    TriangleMesh                               trimesh(M);
    triangle_mesh_to_mcut(trimesh, *out.get());
    return out;
}

TriangleMesh mcut_to_triangle_mesh(const McutMesh &mcutmesh)
{
    uint32_t ccVertexCount = mcutmesh.vertexCoordsArray.size() / 3;
    auto    &ccVertices    = mcutmesh.vertexCoordsArray;
    auto    &ccFaceIndices = mcutmesh.faceIndicesArray;
    auto    &faceSizes     = mcutmesh.faceSizesArray;
    uint32_t ccFaceCount   = faceSizes.size();
    // rearrange vertices/faces and save into result mesh
    std::vector<Vec3f> vertices(ccVertexCount);
    for (uint32_t i = 0; i < ccVertexCount; i++) {
        vertices[i][0] = (float) ccVertices[(uint64_t) i * 3 + 0];
        vertices[i][1] = (float) ccVertices[(uint64_t) i * 3 + 1];
        vertices[i][2] = (float) ccVertices[(uint64_t) i * 3 + 2];
    }

    // output faces
    int faceVertexOffsetBase = 0;

    // for each face in CC
    std::vector<Vec3i32> faces(ccFaceCount);
    for (uint32_t f = 0; f < ccFaceCount; ++f) {
        int faceSize = faceSizes.at(f);

        // for each vertex in face
        for (int v = 0; v < faceSize; v++) { faces[f][v] = ccFaceIndices[(uint64_t) faceVertexOffsetBase + v]; }
        faceVertexOffsetBase += faceSize;
    }

    TriangleMesh out(vertices, faces);
    return out;
}

void merge_mcut_meshes(McutMesh& src, const McutMesh& cut) {
    indexed_triangle_set all_its;
    TriangleMesh tri_src = mcut_to_triangle_mesh(src);
    TriangleMesh tri_cut = mcut_to_triangle_mesh(cut);
    its_merge(all_its, tri_src.its);
    its_merge(all_its, tri_cut.its);
    src = *triangle_mesh_to_mcut(all_its);
    }

MCAPI_ATTR void MCAPI_CALL mcDebugOutput(McDebugSource source,
    McDebugType type,
    unsigned int id,
    McDebugSeverity severity,
    size_t length,
    const char* message,
    const void* userParam)
{
    BOOST_LOG_TRIVIAL(debug)<<Slic3r::format("mcut mcDebugOutput message ( %d ): %s ", id, message);

    switch (source) {
        case MC_DEBUG_SOURCE_API:
            BOOST_LOG_TRIVIAL(debug)<<("Source: API");
            break;
        case MC_DEBUG_SOURCE_KERNEL:
            BOOST_LOG_TRIVIAL(debug)<<("Source: Kernel");
            break;
        }

    switch (type) {
        case MC_DEBUG_TYPE_ERROR:
            BOOST_LOG_TRIVIAL(debug)<<("Type: Error");
            break;
        case MC_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            BOOST_LOG_TRIVIAL(debug)<<("Type: Deprecated Behaviour");
            break;
        case MC_DEBUG_TYPE_OTHER:
            BOOST_LOG_TRIVIAL(debug)<<("Type: Other");
            break;
        }

    switch (severity) {
        case MC_DEBUG_SEVERITY_HIGH:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: high");
            break;
        case MC_DEBUG_SEVERITY_MEDIUM:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: medium");
            break;
        case MC_DEBUG_SEVERITY_LOW:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: low");
            break;
        case MC_DEBUG_SEVERITY_NOTIFICATION:
            BOOST_LOG_TRIVIAL(debug)<<("Severity: notification");
            break;
        }
}


bool do_boolean_single(McutMesh &srcMesh, const McutMesh &cutMesh, const std::string &boolean_opts)
{
    // create context
    McContext context = MC_NULL_HANDLE;
    McResult  err     = mcCreateContext(&context, 0);
    // add debug callback according to https://cutdigital.github.io/mcut.site/tutorials/debugging/
    mcDebugMessageCallback(context, mcDebugOutput, nullptr);
    mcDebugMessageControl(
        context,
        MC_DEBUG_SOURCE_ALL,
        MC_DEBUG_TYPE_ERROR,
        MC_DEBUG_SEVERITY_MEDIUM,
        true);
    // We can either let MCUT compute all possible meshes (including patches etc.), or we can
    // constrain the library to compute exactly the boolean op mesh we want. This 'constrained' case
    // is done with the following flags.
    // NOTE#1: you can extend these flags by bitwise ORing with additional flags (see `McDispatchFlags' in mcut.h)
    // NOTE#2: below order of columns MATTERS
    const std::map<std::string, McFlags> booleanOpts = {
        {"A_NOT_B", MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE},
        {"B_NOT_A", MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW},
        {"UNION", MC_DISPATCH_FILTER_FRAGMENT_SEALING_OUTSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_ABOVE},
        {"INTERSECTION", MC_DISPATCH_FILTER_FRAGMENT_SEALING_INSIDE | MC_DISPATCH_FILTER_FRAGMENT_LOCATION_BELOW},
    };

    std::map<std::string, McFlags>::const_iterator it          = booleanOpts.find(boolean_opts);
    McFlags                                        boolOpFlags = it->second;

    if (srcMesh.vertexCoordsArray.empty() && (boolean_opts == "UNION" || boolean_opts == "B_NOT_A")) {
        srcMesh = cutMesh;
        mcReleaseContext(context);
        return true;
    }

    err = mcDispatch(context,
                     MC_DISPATCH_VERTEX_ARRAY_DOUBLE |          // vertices are in array of doubles
                         MC_DISPATCH_ENFORCE_GENERAL_POSITION | // perturb if necessary
                         boolOpFlags,                           // filter flags which specify the type of output we want
                     // source mesh
                     reinterpret_cast<const void *>(srcMesh.vertexCoordsArray.data()), reinterpret_cast<const uint32_t *>(srcMesh.faceIndicesArray.data()),
                     srcMesh.faceSizesArray.data(), static_cast<uint32_t>(srcMesh.vertexCoordsArray.size() / 3), static_cast<uint32_t>(srcMesh.faceSizesArray.size()),
                     // cut mesh
                     reinterpret_cast<const void *>(cutMesh.vertexCoordsArray.data()), cutMesh.faceIndicesArray.data(), cutMesh.faceSizesArray.data(),
                     static_cast<uint32_t>(cutMesh.vertexCoordsArray.size() / 3), static_cast<uint32_t>(cutMesh.faceSizesArray.size()));
    if (err != MC_NO_ERROR) {
        BOOST_LOG_TRIVIAL(debug) << "MCUT mcDispatch fails! err=" << err;
        mcReleaseContext(context);
        if (boolean_opts == "UNION") {
            merge_mcut_meshes(srcMesh, cutMesh);
            return true;
        }
        return false;
    }

    // query the number of available connected component
    uint32_t numConnComps;
    err = mcGetConnectedComponents(context, MC_CONNECTED_COMPONENT_TYPE_FRAGMENT, 0, NULL, &numConnComps);
    if (err != MC_NO_ERROR || numConnComps==0) {
        BOOST_LOG_TRIVIAL(debug) << "MCUT mcGetConnectedComponents fails! err=" << err << ", numConnComps" << numConnComps;
        mcReleaseContext(context);
        if (numConnComps == 0 && boolean_opts == "UNION") {
            merge_mcut_meshes(srcMesh, cutMesh);
            return true;
        }
        return false;
    }

    std::vector<McConnectedComponent> connectedComponents(numConnComps, MC_NULL_HANDLE);
    err = mcGetConnectedComponents(context, MC_CONNECTED_COMPONENT_TYPE_FRAGMENT, (uint32_t) connectedComponents.size(), connectedComponents.data(), NULL);

    McutMesh outMesh;
    int N_vertices = 0;
    // traversal of all connected components
    for (int n = 0; n < numConnComps; ++n) {
        // query the data of each connected component from MCUT
        McConnectedComponent connComp = connectedComponents[n];

        // query the vertices
        McSize numBytes                  = 0;
        err                               = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, 0, NULL, &numBytes);
        uint32_t            ccVertexCount = (uint32_t) (numBytes / (sizeof(double) * 3));
        std::vector<double> ccVertices((uint64_t) ccVertexCount * 3u, 0);
        err = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_VERTEX_DOUBLE, numBytes, (void *) ccVertices.data(), NULL);

        // query the faces
        numBytes = 0;
        err      = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, 0, NULL, &numBytes);
        std::vector<uint32_t> ccFaceIndices(numBytes / sizeof(uint32_t), 0);
        err = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_FACE_TRIANGULATION, numBytes, ccFaceIndices.data(), NULL);
        std::vector<uint32_t> faceSizes(ccFaceIndices.size() / 3, 3);

        const uint32_t ccFaceCount = static_cast<uint32_t>(faceSizes.size());

        // Here we show, how to know when connected components, pertain particular boolean operations.
        McPatchLocation patchLocation = (McPatchLocation) 0;
        err                           = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_PATCH_LOCATION, sizeof(McPatchLocation), &patchLocation, NULL);

        McFragmentLocation fragmentLocation = (McFragmentLocation) 0;
        err = mcGetConnectedComponentData(context, connComp, MC_CONNECTED_COMPONENT_DATA_FRAGMENT_LOCATION, sizeof(McFragmentLocation), &fragmentLocation, NULL);

        outMesh.vertexCoordsArray.insert(outMesh.vertexCoordsArray.end(), ccVertices.begin(), ccVertices.end());

        // add offset to face index
        for (size_t i = 0; i < ccFaceIndices.size(); i++) {
            ccFaceIndices[i] += N_vertices;
        }

        int faceVertexOffsetBase = 0;

        // for each face in CC
        std::vector<Vec3i32> faces(ccFaceCount);
        for (uint32_t f = 0; f < ccFaceCount; ++f) {
            bool reverseWindingOrder = (fragmentLocation == MC_FRAGMENT_LOCATION_BELOW) && (patchLocation == MC_PATCH_LOCATION_OUTSIDE);
            int  faceSize            = faceSizes.at(f);
            if (reverseWindingOrder) {
                std::vector<uint32_t> faceIndex(faceSize);
                // for each vertex in face
                for (int v = faceSize - 1; v >= 0; v--) { faceIndex[v] = ccFaceIndices[(uint64_t) faceVertexOffsetBase + v]; }
                std::copy(faceIndex.begin(), faceIndex.end(), ccFaceIndices.begin() + faceVertexOffsetBase);
            }
            faceVertexOffsetBase += faceSize;
        }

        outMesh.faceIndicesArray.insert(outMesh.faceIndicesArray.end(), ccFaceIndices.begin(), ccFaceIndices.end());
        outMesh.faceSizesArray.insert(outMesh.faceSizesArray.end(), faceSizes.begin(), faceSizes.end());

        N_vertices += ccVertexCount;
    }

    // free connected component data
    err = mcReleaseConnectedComponents(context, 0, NULL);
    // destroy context
    err = mcReleaseContext(context);

    srcMesh = outMesh;

    return true;
}

bool do_boolean(McutMesh& srcMesh, const McutMesh& cutMesh, const std::string& boolean_opts,
                const BooleanCancelCB& cancel_cb, const BooleanProgressCB& progress_cb)
{
    if (boolean_opts != "A_NOT_B" && boolean_opts != "UNION" && boolean_opts != "INTERSECTION")
        return false;

    const auto canceled = [&cancel_cb]() { return cancel_cb && cancel_cb(); };
    const auto report   = [&progress_cb](size_t done, size_t total) {
        if (progress_cb)
            progress_cb(total == 0 ? 100.f : 100.f * float(done) / float(total));
    };

    TriangleMesh tri_src = mcut_to_triangle_mesh(srcMesh);
    std::vector<indexed_triangle_set> src_parts = its_split(tri_src.its);

    TriangleMesh tri_cut = mcut_to_triangle_mesh(cutMesh);
    std::vector<indexed_triangle_set> cut_parts = its_split(tri_cut.its);

    if (src_parts.empty() && boolean_opts == "UNION") {
        srcMesh = cutMesh;
        report(1, 1);
        return true;
    }
    if (cut_parts.empty()) {
        report(1, 1);
        return true;
    }

    // when src mesh has multiple connected components, mcut refuses to work.
    // But we can force it to work by spliting the src mesh into disconnected components,
    // and do booleans seperately, then merge all the results.
    // Cancellation is checked between dispatches only: a running dispatch can't be interrupted.
    indexed_triangle_set all_its;
    if (boolean_opts == "A_NOT_B") {
        // Each cut can leave the source with several disconnected components, which mcut rejects
        // in the next dispatch, so re-split after every cut part (e.g. each letter of a text).
        std::vector<indexed_triangle_set> parts = std::move(src_parts);
        for (size_t j = 0; j < cut_parts.size(); j++) {
            auto cut_part = triangle_mesh_to_mcut(cut_parts[j]);
            std::vector<indexed_triangle_set> next_parts;
            for (indexed_triangle_set &part : parts) {
                if (canceled())
                    return false;
                auto src_part = triangle_mesh_to_mcut(part);
                if (do_boolean_single(*src_part, *cut_part, boolean_opts)) {
                    TriangleMesh tri_part = mcut_to_triangle_mesh(*src_part);
                    std::vector<indexed_triangle_set> pieces = its_split(tri_part.its);
                    std::move(pieces.begin(), pieces.end(), std::back_inserter(next_parts));
                } else
                    next_parts.emplace_back(std::move(part));
            }
            parts = std::move(next_parts);
            report(j + 1, cut_parts.size());
        }
        for (const indexed_triangle_set &part : parts)
            its_merge(all_its, part);
    }
    else if (boolean_opts == "UNION") {
        const size_t total = src_parts.size() * cut_parts.size();
        for (size_t i = 0; i < src_parts.size(); i++) {
            auto src_part = triangle_mesh_to_mcut(src_parts[i]);
            for (size_t j = 0; j < cut_parts.size(); j++) {
                if (canceled())
                    return false;
                auto cut_part = triangle_mesh_to_mcut(cut_parts[j]);
                do_boolean_single(*src_part, *cut_part, boolean_opts);
                report(i * cut_parts.size() + j + 1, total);
            }
            TriangleMesh tri_part = mcut_to_triangle_mesh(*src_part);
            its_merge(all_its, tri_part.its);
        }
    }
    else {
        const size_t total = src_parts.size() * cut_parts.size();
        for (size_t i = 0; i < src_parts.size(); i++) {
            for (size_t j = 0; j < cut_parts.size(); j++) {
                if (canceled())
                    return false;
                auto src_part = triangle_mesh_to_mcut(src_parts[i]);
                auto cut_part = triangle_mesh_to_mcut(cut_parts[j]);
                bool success = do_boolean_single(*src_part, *cut_part, boolean_opts);
                if (success) {
                    TriangleMesh tri_part = mcut_to_triangle_mesh(*src_part);
                    its_merge(all_its, tri_part.its);
                }
                report(i * cut_parts.size() + j + 1, total);
            }
        }
    }
    srcMesh = *triangle_mesh_to_mcut(all_its);
    return true;
}

void make_boolean(const TriangleMesh &src_mesh, const TriangleMesh &cut_mesh, std::vector<TriangleMesh> &dst_mesh, const std::string &boolean_opts,
                  const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    McutMesh srcMesh, cutMesh;
    triangle_mesh_to_mcut(src_mesh, srcMesh);
    triangle_mesh_to_mcut(cut_mesh, cutMesh);
    if (!do_boolean(srcMesh, cutMesh, boolean_opts, cancel_cb, progress_cb))
        return;
    TriangleMesh tri_src = mcut_to_triangle_mesh(srcMesh);
    if (!tri_src.empty())
        dst_mesh.push_back(std::move(tri_src));
}

} // namespace mcut

namespace ops {
namespace {

using mcut::BooleanCancelCB;
using mcut::BooleanProgressCB;

// Spreads the 0-100 progress of a sequence of steps over 0-100 and never reports a lower value,
// even when a step is retried.
class ProgressSteps
{
public:
    ProgressSteps(const BooleanProgressCB &progress_cb, size_t steps) : m_progress_cb(progress_cb), m_steps(std::max<size_t>(steps, 1)) {}

    // Maps 0-100 onto steps [first, first + count).
    BooleanProgressCB steps(size_t first, size_t count)
    {
        if (!m_progress_cb)
            return {};
        return [this, first, count](float p) { report(100.f * (float(first) + float(count) * std::clamp(p, 0.f, 100.f) / 100.f) / float(m_steps)); };
    }
    BooleanProgressCB step(size_t step) { return steps(step, 1); }
    void done() { report(100.f); }

private:
    void report(float p)
    {
        if (m_progress_cb && p > m_last) {
            m_last = p;
            m_progress_cb(p);
        }
    }

    const BooleanProgressCB &m_progress_cb;
    size_t                   m_steps;
    float                    m_last{-1.f};
};

bool canceled(const BooleanCancelCB &cancel_cb) { return cancel_cb && cancel_cb(); }

void clean_result(TriangleMesh &mesh, bool merge_vertices)
{
    its_remove_degenerate_faces(mesh.its);
    if (merge_vertices) {
        its_merge_vertices(mesh.its, true);
        its_remove_degenerate_faces(mesh.its, true);
    }
    its_compactify_vertices(mesh.its, true);
    mesh = TriangleMesh(std::move(mesh.its));
}

std::optional<TriangleMesh> mcut_boolean(const TriangleMesh &a, const TriangleMesh &b, const std::string &op,
                                         const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    std::vector<TriangleMesh> result;
    mcut::make_boolean(a, b, result, op, cancel_cb, progress_cb);
    if (result.empty() || result.front().empty())
        return std::nullopt;
    clean_result(result.front(), op == "INTERSECTION");
    return std::move(result.front());
}

bool boxes_overlap(const TriangleMesh &a, const TriangleMesh &b)
{
    BoundingBoxf3 ba = a.bounding_box(), bb = b.bounding_box();
    if (!ba.defined || !bb.defined)
        return true;
    ba.offset(0.05);
    bb.offset(0.05);
    return ba.intersects(bb);
}

std::vector<TriangleMesh> shells_of(const TriangleMesh &mesh)
{
    std::vector<TriangleMesh> shells;
    for (indexed_triangle_set &its : its_split(mesh.its))
        shells.emplace_back(std::move(its));
    return shells;
}

TriangleMesh merged(const std::vector<TriangleMesh> &meshes)
{
    indexed_triangle_set its;
    for (const TriangleMesh &mesh : meshes)
        its_merge(its, mesh.its);
    return TriangleMesh(std::move(its));
}

// Whether the solid shell `inner` lies strictly inside the solid shell `outer`. mcut leaves a shell alone both when
// the surfaces don't cross and when it refuses a mesh it can't handle, so every vertex of `inner` must test inside,
// clear of the surface of `outer`; in doubt the answer is no and the caller keeps the shell as it is.
bool lies_inside(const TriangleMesh &inner, const TriangleMesh &outer)
{
    if (inner.its.vertices.empty() || outer.its.indices.empty() || its_volume(inner.its) <= 0.f || its_volume(outer.its) <= 0.f)
        return false;
    const BoundingBoxf3 inner_box = inner.bounding_box(), outer_box = outer.bounding_box();
    if (!outer_box.contains(inner_box.min) || !outer_box.contains(inner_box.max))
        return false;
    const AABBMesh tree(outer.its);
    // Nearer than this to the surface of `outer` counts as touching it.
    const double clearance = 1e-4 * outer_box.size().norm();
    // An oblique ray, so it doesn't run along a face or an edge of a box-like mesh.
    const Vec3d direction = Vec3d(0.3, 0.5, 0.8).normalized();
    return std::all_of(inner.its.vertices.begin(), inner.its.vertices.end(), [&](const Vec3f &vertex) {
        const Vec3d                point = vertex.cast<double>();
        const AABBMesh::hit_result hit   = tree.query_ray_hit(point, direction);
        return hit.is_inside() && hit.distance() > clearance && tree.squared_distance(point) > clearance * clearance;
    });
}

size_t solid_shell_count(const TriangleMesh &mesh)
{
    const std::vector<indexed_triangle_set> shells = its_split(mesh.its);
    return size_t(std::count_if(shells.begin(), shells.end(), [](const indexed_triangle_set &its) { return its_volume(its) > 0.f; }));
}

// Whether CGAL can take the mesh safely, as the CSG path checks before its booleans.
bool cgal_can_take(const TriangleMesh &mesh)
{
    const cgal::CGALMeshPtr cgal_mesh = cgal::triangle_mesh_to_cgal(mesh.its);
    return cgal_mesh && cgal::does_bound_a_volume(*cgal_mesh) && !cgal::does_self_intersect(*cgal_mesh);
}

// `a` op `b` through CGAL, on meshes it can take safely. False when it can't take them or fails; otherwise `a`
// holds the result, which may be empty.
template<class Op> bool cgal_boolean(TriangleMesh &a, TriangleMesh b, Op op)
{
    // The in-place CGAL conversion keeps isolated vertices, so drop them first.
    its_compactify_vertices(a.its);
    its_compactify_vertices(b.its);
    if (!cgal_can_take(a) || !cgal_can_take(b))
        return false;
    try {
        op(a, b);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "Mesh boolean: CGAL operation failed: " << e.what();
        return false;
    }
    if (!a.empty())
        clean_result(a, true);
    return true;
}

// Whether a cavity (an inward shell) of one of the meshes meets a solid shell other than the one around it. mcut
// sees each shell on its own, which gets a cavity right only while nothing else reaches into it.
bool cavity_reached(const std::vector<const TriangleMesh *> &meshes)
{
    std::vector<BoundingBoxf3> solids, cavities;
    for (const TriangleMesh *mesh : meshes)
        for (const indexed_triangle_set &its : its_split(mesh->its)) {
            BoundingBoxf3 box = bounding_box(its);
            box.offset(0.05);
            (its_volume(its) < 0.f ? cavities : solids).push_back(box);
        }
    return std::any_of(cavities.begin(), cavities.end(), [&solids](const BoundingBoxf3 &cavity) {
        return std::count_if(solids.begin(), solids.end(), [&cavity](const BoundingBoxf3 &solid) { return solid.intersects(cavity); }) > 1;
    });
}

const auto cgal_minus     = [](TriangleMesh &a, const TriangleMesh &b) { cgal::minus(a, b); };
const auto cgal_plus      = [](TriangleMesh &a, const TriangleMesh &b) { cgal::plus(a, b); };
const auto cgal_intersect = [](TriangleMesh &a, const TriangleMesh &b) { cgal::intersect(a, b); };

// `a` minus `cutter`, shell by shell. mcut leaves a shell whose surface the cutter doesn't cross, so a shell left
// as it was that lies inside the cutter is dropped. When a cavity is reached, CGAL takes the whole pair first.
// Nothing when nothing is left or when cancelled.
std::optional<TriangleMesh> subtract_pair(const TriangleMesh &a, const TriangleMesh &cutter,
                                          const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    if (canceled(cancel_cb))
        return std::nullopt;
    if (cavity_reached({&a, &cutter})) {
        if (TriangleMesh result = a; cgal_boolean(result, cutter, cgal_minus)) {
            if (result.empty() || canceled(cancel_cb))
                return std::nullopt;
            if (progress_cb)
                progress_cb(100.f);
            return result;
        }
    }

    const std::vector<TriangleMesh> shells = shells_of(a);
    std::vector<TriangleMesh>       kept;
    ProgressSteps                   progress(progress_cb, shells.size());
    for (size_t i = 0; i < shells.size(); ++i) {
        const TriangleMesh &shell = shells[i];
        if (canceled(cancel_cb))
            return std::nullopt;
        if (!boxes_overlap(shell, cutter)) {
            kept.push_back(shell);
            continue;
        }
        std::optional<TriangleMesh> cut = mcut_boolean(shell, cutter, "A_NOT_B", cancel_cb, progress.step(i));
        if (canceled(cancel_cb))
            return std::nullopt;
        if (!cut)
            continue;
        const bool untouched = cut->its.indices.size() == shell.its.indices.size() &&
                               std::abs(its_volume(cut->its) - its_volume(shell.its)) <= 1e-4f * std::abs(its_volume(shell.its));
        if (untouched && lies_inside(shell, cutter))
            continue;
        kept.push_back(std::move(*cut));
    }
    if (kept.empty())
        return std::nullopt;
    progress.done();
    return merged(kept);
}

// `a` intersected with `b`, shell pair by shell pair. mcut returns nothing for a shell lying wholly inside the
// other, so such a shell is kept as it is. When a cavity is reached, CGAL takes the whole pair first; when nothing
// comes out of mcut, CGAL tries the whole meshes, if it can take them. `failed` is set when the answer is not a
// clean "no common volume": CGAL could not run or threw.
std::optional<TriangleMesh> intersect_pair(const TriangleMesh &a, const TriangleMesh &b,
                                           const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb, bool &failed)
{
    if (canceled(cancel_cb))
        return std::nullopt;
    if (cavity_reached({&a, &b})) {
        if (TriangleMesh result = a; cgal_boolean(result, b, cgal_intersect)) {
            if (result.empty() || canceled(cancel_cb))
                return std::nullopt;
            if (progress_cb)
                progress_cb(100.f);
            return result;
        }
    }

    const std::vector<TriangleMesh> shells_a = shells_of(a), shells_b = shells_of(b);
    std::vector<TriangleMesh>       pieces;
    ProgressSteps                   progress(progress_cb, shells_a.size() * shells_b.size());
    bool                            any_overlap = false;
    for (size_t i = 0; i < shells_a.size(); ++i)
        for (size_t j = 0; j < shells_b.size(); ++j) {
            if (canceled(cancel_cb))
                return std::nullopt;
            if (!boxes_overlap(shells_a[i], shells_b[j]))
                continue;
            any_overlap = true;
            std::optional<TriangleMesh> piece = mcut_boolean(shells_a[i], shells_b[j], "INTERSECTION", cancel_cb,
                                                             progress.step(i * shells_b.size() + j));
            if (canceled(cancel_cb))
                return std::nullopt;
            if (piece)
                pieces.push_back(std::move(*piece));
            else if (lies_inside(shells_a[i], shells_b[j]))
                pieces.push_back(shells_a[i]);
            else if (lies_inside(shells_b[j], shells_a[i]))
                pieces.push_back(shells_b[j]);
        }
    if (!pieces.empty()) {
        TriangleMesh result = merged(pieces);
        clean_result(result, true);
        progress.done();
        return result;
    }
    // No two shells come near each other: known to be empty.
    if (!any_overlap)
        return std::nullopt;

    // mcut gives up on some touching or coplanar inputs that CGAL handles.
    if (canceled(cancel_cb))
        return std::nullopt;
    TriangleMesh result = a;
    if (!cgal_boolean(result, b, cgal_intersect)) {
        failed = true;
        return std::nullopt;
    }
    if (result.empty())
        return std::nullopt;
    progress.done();
    return result;
}

} // namespace

void normalize_input(TriangleMesh &mesh)
{
    if (its_volume(mesh.its) < 0.f)
        mesh.flip_triangles();
}

std::optional<TriangleMesh> union_all(const std::vector<TriangleMesh> &meshes, const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    // An mcut union appends the cutter to every source shell it doesn't reach, so the union is built from single
    // shells: overlapping pairs are joined until no pair joins, and the rest is put side by side once. A shell inside
    // another is absorbed by it. Cavities (inward shells) are kept as they are, unless one is reached: then CGAL
    // joins the whole meshes first. A single mesh always takes the shell path, which joins its own shells.
    if (meshes.empty() || canceled(cancel_cb))
        return std::nullopt;
    std::vector<const TriangleMesh *> inputs;
    for (const TriangleMesh &mesh : meshes)
        inputs.push_back(&mesh);
    if (meshes.size() > 1 && cavity_reached(inputs)) {
        TriangleMesh result = meshes.front();
        bool         joined = true;
        for (size_t i = 1; i < meshes.size() && joined; ++i) {
            if (canceled(cancel_cb))
                return std::nullopt;
            joined = cgal_boolean(result, meshes[i], cgal_plus);
        }
        if (canceled(cancel_cb))
            return std::nullopt;
        if (joined && !result.empty()) {
            if (progress_cb)
                progress_cb(100.f);
            return result;
        }
    }

    std::vector<TriangleMesh> shells, cavities;
    for (const TriangleMesh &mesh : meshes)
        for (TriangleMesh &shell : shells_of(mesh))
            (its_volume(shell.its) < 0.f ? cavities : shells).push_back(std::move(shell));
    if (shells.empty())
        return std::nullopt;

    ProgressSteps       progress(progress_cb, shells.size() - 1);
    size_t              joins = 0;
    // Overlapping pairs already tried and not joined, by shell id; a joined shell gets a new id.
    std::vector<size_t> ids(shells.size());
    for (size_t i = 0; i < ids.size(); ++i)
        ids[i] = i;
    size_t                                next_id = ids.size();
    std::set<std::pair<size_t, size_t>>   tried;
    for (bool joined_any = true; joined_any;) {
        joined_any = false;
        for (size_t i = 0; i < shells.size() && !joined_any; ++i) {
            if (canceled(cancel_cb))
                return std::nullopt;
            for (size_t j = i + 1; j < shells.size() && !joined_any; ++j) {
                if (!boxes_overlap(shells[i], shells[j]) || !tried.insert({ids[i], ids[j]}).second)
                    continue;
                std::optional<TriangleMesh> joined = mcut_boolean(shells[i], shells[j], "UNION", cancel_cb, progress.step(joins));
                if (canceled(cancel_cb))
                    return std::nullopt;
                if (!joined || solid_shell_count(*joined) != 1) {
                    if (lies_inside(shells[j], shells[i]))
                        joined = shells[i];
                    else if (lies_inside(shells[i], shells[j]))
                        joined = shells[j];
                    else
                        continue;
                }
                shells[i] = std::move(*joined);
                ids[i]    = next_id++;
                shells.erase(shells.begin() + std::ptrdiff_t(j));
                ids.erase(ids.begin() + std::ptrdiff_t(j));
                ++joins;
                joined_any = true;
            }
        }
    }
    shells.insert(shells.end(), cavities.begin(), cavities.end());
    TriangleMesh result = merged(shells);
    clean_result(result, false);
    progress.done();
    return result;
}

std::optional<TriangleMesh> intersect_all(const std::vector<TriangleMesh> &meshes, const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb,
                                          bool *failed)
{
    if (failed != nullptr)
        *failed = false;
    bool any_failed = false;
    if (meshes.size() < 2) {
        if (meshes.empty())
            return std::nullopt;
        if (progress_cb)
            progress_cb(100.f);
        return meshes.front();
    }

    // All meshes must be connected through overlapping bounding boxes, or there is no common volume.
    const size_t        n = meshes.size();
    std::vector<size_t> parent(n);
    for (size_t i = 0; i < n; ++i)
        parent[i] = i;
    const auto find = [&parent](size_t x) {
        while (parent[x] != x)
            x = parent[x];
        return x;
    };
    std::vector<size_t> degree(n, 0);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (boxes_overlap(meshes[i], meshes[j])) {
                parent[find(i)] = find(j);
                ++degree[i];
                ++degree[j];
            }
    for (size_t i = 1; i < n; ++i)
        if (find(i) != find(0))
            return std::nullopt;

    // An mcut or CGAL step can fail on one pair yet succeed in another order, so try the
    // best-connected seeds first and, at each step, every remaining operand before giving up.
    std::vector<size_t> seeds(n);
    for (size_t i = 0; i < n; ++i)
        seeds[i] = i;
    std::stable_sort(seeds.begin(), seeds.end(), [&degree](size_t a, size_t b) { return degree[a] > degree[b]; });

    ProgressSteps progress(progress_cb, n - 1);
    for (size_t seed : seeds) {
        if (canceled(cancel_cb))
            return std::nullopt;
        TriangleMesh        acc = meshes[seed];
        std::vector<size_t> remaining;
        for (size_t i = 0; i < n; ++i)
            if (i != seed)
                remaining.push_back(i);

        size_t step = 0;
        while (!remaining.empty()) {
            if (canceled(cancel_cb))
                return std::nullopt;
            std::stable_sort(remaining.begin(), remaining.end(), [&](size_t a, size_t b) {
                return boxes_overlap(acc, meshes[a]) && !boxes_overlap(acc, meshes[b]);
            });
            bool progressed = false;
            for (size_t i = 0; i < remaining.size(); ++i) {
                if (std::optional<TriangleMesh> next = intersect_pair(acc, meshes[remaining[i]], cancel_cb, progress.step(step), any_failed)) {
                    acc = std::move(*next);
                    remaining.erase(remaining.begin() + std::ptrdiff_t(i));
                    ++step;
                    progressed = true;
                    break;
                }
                if (canceled(cancel_cb))
                    return std::nullopt;
            }
            if (!progressed)
                break;
        }
        if (remaining.empty() && !acc.empty()) {
            progress.done();
            return acc;
        }
    }
    if (failed != nullptr)
        *failed = any_failed;
    return std::nullopt;
}

std::vector<std::optional<TriangleMesh>> subtract_each(const std::vector<TriangleMesh> &a, const std::vector<TriangleMesh> &b,
                                                       const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    std::vector<std::optional<TriangleMesh>> results(a.size());
    ProgressSteps                            progress(progress_cb, a.size() * b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        std::optional<TriangleMesh> acc = a[i];
        for (size_t j = 0; j < b.size() && acc; ++j)
            acc = subtract_pair(*acc, b[j], cancel_cb, progress.step(i * b.size() + j));
        if (canceled(cancel_cb))
            return {};
        results[i] = std::move(acc);
    }
    progress.done();
    return results;
}

std::vector<std::optional<TriangleMesh>> subtract_union(const std::vector<TriangleMesh> &a, const std::vector<TriangleMesh> &b,
                                                        const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    const size_t  union_steps = b.empty() ? 0 : b.size() - 1;
    ProgressSteps progress(progress_cb, union_steps + a.size());
    std::optional<TriangleMesh> cutter = union_all(b, cancel_cb, progress.steps(0, union_steps));
    if (!cutter)
        return {};
    std::vector<std::optional<TriangleMesh>> results(a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        results[i] = subtract_pair(a[i], *cutter, cancel_cb, progress.step(union_steps + i));
        if (canceled(cancel_cb))
            return {};
    }
    progress.done();
    return results;
}

std::optional<TriangleMesh> solid(const std::vector<TriangleMesh> &parts, const std::vector<TriangleMesh> &negatives,
                                  const BooleanCancelCB &cancel_cb, const BooleanProgressCB &progress_cb)
{
    ProgressSteps progress(progress_cb, negatives.empty() ? 1 : 2);
    std::optional<TriangleMesh> result = union_all(parts, cancel_cb, progress.step(0));
    if (result && !negatives.empty()) {
        std::vector<std::optional<TriangleMesh>> cut = subtract_union({*result}, negatives, cancel_cb, progress.step(1));
        result = cut.empty() ? std::nullopt : std::move(cut.front());
    }
    if (result)
        progress.done();
    return result;
}

} // namespace ops


} // namespace MeshBoolean
} // namespace Slic3r
