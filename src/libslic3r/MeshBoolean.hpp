#ifndef libslic3r_MeshBoolean_hpp_
#define libslic3r_MeshBoolean_hpp_

#include <Eigen/Core>
#include "libslic3r/Point.hpp"
#include <memory>
#include <exception>
#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>

#include <libslic3r/TriangleMesh.hpp>
#include <Eigen/Geometry>

namespace Slic3r {

namespace MeshBoolean {

using EigenMesh = std::pair<Eigen::MatrixXd, Eigen::MatrixXi>;

TriangleMesh eigen_to_triangle_mesh(const EigenMesh &emesh);
EigenMesh triangle_mesh_to_eigen(const TriangleMesh &mesh);

void minus(EigenMesh &A, const EigenMesh &B);
void self_union(EigenMesh &A);
    
void minus(TriangleMesh& A, const TriangleMesh& B);
void self_union(TriangleMesh& mesh);

namespace cgal {

struct CGALMesh;
struct CGALMeshDeleter { void operator()(CGALMesh *ptr); };
using CGALMeshPtr = std::unique_ptr<CGALMesh, CGALMeshDeleter>;

CGALMeshPtr clone(const CGALMesh &m);

void save_CGALMesh(const std::string& fname, const CGALMesh& cgal_mesh);

CGALMeshPtr triangle_mesh_to_cgal(
    const std::vector<stl_vertex> &V,
    const std::vector<stl_triangle_vertex_indices> &F);

inline CGALMeshPtr triangle_mesh_to_cgal(const indexed_triangle_set &M)
{
    return triangle_mesh_to_cgal(M.vertices, M.indices);
}
inline CGALMeshPtr triangle_mesh_to_cgal(const TriangleMesh &M)
{
    return triangle_mesh_to_cgal(M.its);
}

TriangleMesh cgal_to_triangle_mesh(const CGALMesh &cgalmesh);
indexed_triangle_set cgal_to_indexed_triangle_set(const CGALMesh &cgalmesh);

// Do boolean mesh difference with CGAL bypassing igl.
void minus(TriangleMesh &A, const TriangleMesh &B);
void plus(TriangleMesh &A, const TriangleMesh &B);
void intersect(TriangleMesh &A, const TriangleMesh &B);

void minus(indexed_triangle_set &A, const indexed_triangle_set &B);
void plus(indexed_triangle_set &A, const indexed_triangle_set &B);
void intersect(indexed_triangle_set &A, const indexed_triangle_set &B);

void minus(CGALMesh &A, CGALMesh &B);
void plus(CGALMesh &A, CGALMesh &B);
void intersect(CGALMesh &A, CGALMesh &B);

bool does_self_intersect(const TriangleMesh &mesh);
bool does_self_intersect(const CGALMesh &mesh);

//BBS
std::vector<TriangleMesh> segment(const TriangleMesh& src, double smoothing_alpha = 0.5, int segment_number = 5);
TriangleMesh merge(std::vector<TriangleMesh> meshes);

bool does_bound_a_volume(const CGALMesh &mesh);
bool empty(const CGALMesh &mesh);

// Real UV unwrap of an open mesh patch via CGAL's LSCM (Least Squares Conformal Maps) surface
// parameterization. Returns one UV coordinate per input vertex (same indexing as `mesh.vertices`),
// or nullopt if `mesh` isn't a single topological disk -- LSCM needs exactly one connected
// component with exactly one boundary loop, true for a typical single brush stroke/patch but not
// guaranteed for multiple disconnected painted islands merged into one mesh.
std::optional<std::vector<Vec2f>> parameterize_lscm(const indexed_triangle_set &mesh);

// Isotropic remeshing (CGAL): rebuilds the mesh so its triangles are close to a uniform target edge
// length, splitting oversized triangles and collapsing undersized ones. Used to even out a model with
// wildly varying triangle sizes so texture displacement has a consistent vertex density to work with.
// Edges whose dihedral angle exceeds `sharp_angle_deg`, and any open border, are held fixed so hard
// features survive instead of being eroded by the relaxation pass; pass 0 to remesh everything.
// Returns the input unchanged if remeshing fails (e.g. a non-manifold or self-intersecting input).
// `n_relaxation_steps` is the number of tangential relaxation passes run inside each iteration. That
// relaxation is what actually evens out the triangle distribution - splitting and collapsing alone
// only bring edge *lengths* near the target, leaving the vertices wherever they happened to land. CGAL
// defaults it to 1, which on a few iterations is not enough to look uniform.
indexed_triangle_set remesh_isotropic(const indexed_triangle_set &mesh, double target_edge_length,
                                      unsigned n_iterations = 3, double sharp_angle_deg = 40.0,
                                      unsigned n_relaxation_steps = 1);
}

namespace mcut {
struct McutMesh;
struct McutMeshDeleter
{
    void operator()(McutMesh *ptr);
};
using McutMeshPtr = std::unique_ptr<McutMesh, McutMeshDeleter>;
bool empty(const McutMesh &mesh);

McutMeshPtr  triangle_mesh_to_mcut(const indexed_triangle_set &M);
TriangleMesh mcut_to_triangle_mesh(const McutMesh &mcutmesh);

// do boolean and save result to srcMesh
// return true if sucessful
bool do_boolean_single(McutMesh& srcMesh, const McutMesh& cutMesh, const std::string& boolean_opts);
// Returns true to stop a boolean between two MCUT dispatches.
using BooleanCancelCB = std::function<bool()>;
// Receives progress from 0 to 100, never decreasing.
using BooleanProgressCB = std::function<void(float)>;

// do boolean of mesh with multiple volumes and save result to srcMesh
// Both srcMesh and cutMesh may have multiple volumes.
// boolean_opts is "UNION", "A_NOT_B" or "INTERSECTION". Returns false and leaves srcMesh untouched
// for any other operation or when cancel_cb stops it. A dispatch mcut refuses leaves that part of srcMesh as it was,
// or for a union puts both meshes side by side.
bool do_boolean(McutMesh &srcMesh, const McutMesh &cutMesh, const std::string &boolean_opts,
                const BooleanCancelCB &cancel_cb = {}, const BooleanProgressCB &progress_cb = {});


// do boolean and convert result to TriangleMesh; dst_mesh gets nothing when do_boolean returns false or the result is empty
void make_boolean(const TriangleMesh &src_mesh, const TriangleMesh &cut_mesh, std::vector<TriangleMesh> &dst_mesh, const std::string &boolean_opts,
                  const BooleanCancelCB &cancel_cb = {}, const BooleanProgressCB &progress_cb = {});
} // namespace mcut

// Booleans over several meshes, all in one (world) coordinate system, built on mcut::make_boolean.
// A cancelled or failed operation returns std::nullopt; callers tell the two apart through their cancel_cb.
namespace ops {
// Orients a mesh for the operations below: flipped as a whole when its signed volume is negative,
// so an inside-out input doesn't invert the result. A cavity inside a positive mesh is kept as is.
void normalize_input(TriangleMesh &mesh);

std::optional<TriangleMesh> union_all(const std::vector<TriangleMesh> &meshes,
                                      const mcut::BooleanCancelCB &cancel_cb = {}, const mcut::BooleanProgressCB &progress_cb = {});
// Every mesh must take part; fails when the meshes don't share one common volume. When it fails, *failed tells
// whether that is because neither mcut nor CGAL could compute it, rather than an empty intersection.
std::optional<TriangleMesh> intersect_all(const std::vector<TriangleMesh> &meshes,
                                          const mcut::BooleanCancelCB &cancel_cb = {}, const mcut::BooleanProgressCB &progress_cb = {},
                                          bool *failed = nullptr);
// result[i] = a[i] - b[0] - b[1] - ...; one entry per a[i], std::nullopt where nothing is left or the cut failed.
// Returns no entries at all when cancelled.
std::vector<std::optional<TriangleMesh>> subtract_each(const std::vector<TriangleMesh> &a, const std::vector<TriangleMesh> &b,
                                                       const mcut::BooleanCancelCB &cancel_cb = {}, const mcut::BooleanProgressCB &progress_cb = {});
// result[i] = a[i] - (b[0] ∪ b[1] ∪ ...); one entry per a[i], as subtract_each. No entries when the union of b fails or when cancelled.
std::vector<std::optional<TriangleMesh>> subtract_union(const std::vector<TriangleMesh> &a, const std::vector<TriangleMesh> &b,
                                                        const mcut::BooleanCancelCB &cancel_cb = {}, const mcut::BooleanProgressCB &progress_cb = {});
// The volume an object prints: the union of its parts minus the union of its negative volumes.
std::optional<TriangleMesh> solid(const std::vector<TriangleMesh> &parts, const std::vector<TriangleMesh> &negatives,
                                  const mcut::BooleanCancelCB &cancel_cb = {}, const mcut::BooleanProgressCB &progress_cb = {});
} // namespace ops

} // namespace MeshBoolean
} // namespace Slic3r
#endif // libslic3r_MeshBoolean_hpp_
