#include "Skeleton.hpp"
#include "TriangleMesh.hpp"

#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/Polygon_mesh_slicer.h>
#include <CGAL/Polygon_mesh_processing/polygon_soup_to_polygon_mesh.h>
#include <CGAL/Polygon_mesh_processing/repair_polygon_soup.h>
#include <CGAL/Polygon_mesh_processing/orientation.h>
#include <CGAL/Polygon_mesh_processing/connected_components.h>
#include <CGAL/extract_mean_curvature_flow_skeleton.h>
#include <CGAL/boost/graph/helpers.h>
#include <CGAL/boost/graph/Euler_operations.h>
#include <CGAL/Polygon_mesh_processing/clip.h>
#include <CGAL/Polygon_mesh_processing/corefinement.h>
#include <CGAL/Polygon_mesh_processing/repair_degeneracies.h>
#include <CGAL/Polygon_mesh_processing/repair.h>
#include <CGAL/Polygon_mesh_processing/stitch_borders.h>
#include <CGAL/Polygon_mesh_processing/refine.h>
#include <CGAL/AABB_tree.h>
#include <CGAL/AABB_traits_3.h>
#include <CGAL/AABB_face_graph_triangle_primitive.h>
#include <CGAL/Constrained_Delaunay_triangulation_2.h>

#include <boost/graph/adjacency_list.hpp>

#include <future>
#include <thread>
#include <variant>

#include <Eigen/SVD>
#include <Eigen/Sparse>
#include <tbb/parallel_for.h>
#include "AABBTreeIndirect.hpp"
#include <functional>
#include <optional>
#include <sstream>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace Slic3r {

using K         = CGAL::Exact_predicates_inexact_constructions_kernel;
using Point_3   = K::Point_3;
using Plane_3   = K::Plane_3;
using Mesh      = CGAL::Surface_mesh<Point_3>;
using Primitive = CGAL::AABB_face_graph_triangle_primitive<Mesh>;
using Traits    = CGAL::AABB_traits_3<K, Primitive>;
using Tree      = CGAL::AABB_tree<Traits>;
namespace PMP   = CGAL::Polygon_mesh_processing;

static Mesh build_cgal_mesh(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F)
{
    std::vector<Point_3> pts;
    pts.reserve(V.rows());
    for (int i = 0; i < V.rows(); ++i)
        pts.emplace_back(V(i, 0), V(i, 1), V(i, 2));

    std::vector<std::vector<std::size_t>> polys;
    polys.reserve(F.rows());
    for (int i = 0; i < F.rows(); ++i)
        polys.push_back({(std::size_t) F(i, 0), (std::size_t) F(i, 1), (std::size_t) F(i, 2)});

    PMP::repair_polygon_soup(pts, polys);
    Mesh mesh;
    PMP::polygon_soup_to_polygon_mesh(pts, polys, mesh);
    if (!PMP::is_outward_oriented(mesh))
        PMP::reverse_face_orientations(mesh);
    return mesh;
}

// Canonical mesh for every stage of the pipeline: repaired, outward oriented, and reduced
// to its largest surface component. That last step is what makes a sealed hollow body
// (two shells) behave like the solid it encloses — the inner shell is simply dropped. It
// used to happen only inside skeletonize_mesh, so snap_to_bed and voxelize saw the inner
// wall and disagreed with the skeleton about where the surface was.
static Mesh build_prepared_mesh(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F, int* out_dropped = nullptr)
{
    Mesh mesh = build_cgal_mesh(V, F);
    const std::size_t ncc = PMP::keep_largest_connected_components(mesh, 1);
    mesh.collect_garbage();
    if (out_dropped)
        *out_dropped = (int) ncc;
    return mesh;
}

int mesh_genus(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F)
{
    if (V.rows() == 0 || F.rows() == 0)
        return -1;
    Mesh mesh = build_prepared_mesh(V, F);
    if (!CGAL::is_closed(mesh) || !CGAL::is_valid_polygon_mesh(mesh))
        return -1;
    const long nv  = (long) mesh.number_of_vertices();
    const long ne  = (long) mesh.number_of_edges();
    const long nf  = (long) mesh.number_of_faces();
    const long chi = nv - ne + nf;
    if (((2 - chi) % 2) != 0)
        return -1; // not a single orientable closed surface — genus is not defined here
    return (int) ((2 - chi) / 2);
}

// ── MCF skeletonization ───────────────────────────────────────────────────────

SkeletonGraph skeletonize_mesh(const Eigen::MatrixXd& V,
                               const Eigen::MatrixXi& F,
                               double quality_speed_tradeoff,
                               double min_edge_length_factor,
                               double medial_centering)
{
    using Mcfskel  = CGAL::Mean_curvature_flow_skeletonization<Mesh>;
    using Skeleton = Mcfskel::Skeleton;

    // MCF requires a closed, single-component manifold mesh.
    int ncc = 0;
    Mesh mesh = build_prepared_mesh(V, F, &ncc);
    if (ncc > 1)
        std::cerr << "[mcf] Warning: mesh had " << ncc << " components; kept the largest.\n";

    std::cout << "[mcf] Running MCF skeletonization"
              << "  quality_speed=" << quality_speed_tradeoff << "  min_edge_factor=" << min_edge_length_factor
              << "  medial=" << medial_centering << "…\n";

    Mcfskel mcfs(mesh);

    mcfs.set_quality_speed_tradeoff(quality_speed_tradeoff);

    // Medial centring (CGAL's omega_P, default 0.2, and the is_medially_centered flag that
    // gates it). Left at the default until now, which is worth knowing: MCF is a CONTRACTION,
    // not a medial-axis computation, and nothing in it constrains the result to the solid. It
    // moves each vertex toward the average of its neighbours, and across a concave feature —
    // the notch between two branches — that average lies in the void. Measured on a solid,
    // watertight, genus-0 trident: raw MCF put 18 of its 166 nodes OUTSIDE the material, 8.5 mm
    // of 145 mm of skeleton length, before any cleaning stage touched it.
    //
    // omega_P biases contraction toward the medial axis, so it is the one knob that attacks
    // that at source rather than repairing it downstream. It is a bias, not a constraint —
    // there is still no containment guarantee — and CGAL notes it can slow convergence, so it
    // is exposed as a parameter rather than pinned to a value.
    if (medial_centering > 0.0) {
        mcfs.set_is_medially_centered(true);
        mcfs.set_medially_centered_speed_tradeoff(medial_centering);
    } else {
        mcfs.set_is_medially_centered(false);
    }

    CGAL::Bbox_3 bb = CGAL::bbox_3(mesh.points().begin(), mesh.points().end());
    double diag = std::sqrt(std::pow(bb.xmax() - bb.xmin(), 2) + std::pow(bb.ymax() - bb.ymin(), 2) + std::pow(bb.zmax() - bb.zmin(), 2));
    mcfs.set_min_edge_length(min_edge_length_factor * diag);

    Skeleton skel;
    mcfs(skel);

    // Convert boost skeleton graph → SkeletonGraph (world-space positions).
    SkeletonGraph G_skel;
    std::unordered_map<std::size_t, int> vd_to_idx;
    for (auto vd : CGAL::make_range(boost::vertices(skel))) {
        vd_to_idx[(std::size_t) vd] = G_skel.num_nodes();
        const auto& p               = skel[vd].point;
        G_skel.add_node(Eigen::Vector3d(p.x(), p.y(), p.z()));
    }
    for (auto ed : CGAL::make_range(boost::edges(skel))) {
        int u = vd_to_idx[(std::size_t) boost::source(ed, skel)];
        int v = vd_to_idx[(std::size_t) boost::target(ed, skel)];
        G_skel.add_edge(u, v);
    }

    std::cout << "[mcf] Skeleton: " << G_skel.num_nodes() << " nodes, ";
    int ne = 0;
    for (auto& a : G_skel.adj)
        ne += (int) a.size();
    std::cout << ne / 2 << " edges.\n";
    return G_skel;
}

// Weighted Dijkstra (Euclidean edge lengths) from src.
static std::vector<double> dijkstra(const SkeletonGraph& G, int src)
{
    int n = G.num_nodes();
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    dist[src] = 0.0;

    using PD = std::pair<double, int>;
    std::priority_queue<PD, std::vector<PD>, std::greater<PD>> pq;
    pq.push({0.0, src});

    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u])
            continue;
        for (int v : G.adj[u]) {
            double w  = (G.nodes[u] - G.nodes[v]).norm();
            double nd = d + w;
            if (nd < dist[v]) {
                dist[v] = nd;
                pq.push({nd, v});
            }
        }
    }
    return dist;
}

static double min_dist_to_set(const std::vector<Eigen::Vector3d>& queries, const std::vector<Eigen::Vector3d>& dataset)
{
    double best = std::numeric_limits<double>::infinity();
    for (auto& q : queries)
        for (auto& d : dataset) {
            double dist = (q - d).norm();
            if (dist < best)
                best = dist;
        }
    return best;
}

// ── DAG orientation ───────────────────────────────────────────────────────────

int find_graph_center(const SkeletonGraph& G)
{
    int n = G.num_nodes();
    if (n == 0)
        return -1;

    std::vector<int> endpoints;
    for (int i = 0; i < n; ++i)
        if (G.degree(i) == 1)
            endpoints.push_back(i);
    if (endpoints.empty())
        for (int i = 0; i < n; ++i)
            endpoints.push_back(i);

    std::vector<double> max_dist(n, 0.0);
    for (int ep : endpoints) {
        auto d = dijkstra(G, ep);
        for (int i = 0; i < n; ++i)
            if (d[i] > max_dist[i])
                max_dist[i] = d[i];
    }
    return (int) (std::min_element(max_dist.begin(), max_dist.end()) - max_dist.begin());
}

SkeletonDAG orient_skeleton_as_dag(const SkeletonGraph& G, int root)
{
    int n = G.num_nodes();

    SkeletonDAG dag;
    dag.nodes = G.nodes;
    dag.succ.resize(n);
    dag.pred.resize(n);
    if (n == 0)
        return dag;

    if (root < 0)
        root = find_graph_center(G);
    dag.root = root;
    dag.dist = dijkstra(G, root);

    // Dijkstra shortest-path tree: for each non-root node v, its unique parent
    // is the neighbour u with the smallest dist[u] that is strictly closer to
    // root than v.  Tiebreak by node index so the choice is deterministic.
    // This guarantees one parent per node (no cycles, no false leaves from
    // loop spurs) while respecting the continuous edge weights that BFS ignores.
    for (int v = 0; v < n; ++v) {
        if (v == root)
            continue;
        int par     = -1;
        double best = std::numeric_limits<double>::infinity();
        for (int u : G.adj[v]) {
            bool qualifies = (dag.dist[u] < dag.dist[v]) || (dag.dist[u] == dag.dist[v] && u < v);
            if (!qualifies)
                continue;
            if (dag.dist[u] < best || (dag.dist[u] == best && u < par)) {
                best = dag.dist[u];
                par  = u;
            }
        }
        if (par != -1)
            dag.add_edge(par, v);
    }
    // ── DAG diagnostic dump ──────────────────────────────────────────────────
    {
        int n_nodes = dag.num_nodes();
        int n_edges = 0;
        for (int i = 0; i < n_nodes; ++i)
            n_edges += (int) dag.succ[i].size();

        // count structural anomalies
        int n_roots   = 0; // nodes with no predecessors
        int n_leaves  = 0; // nodes with no successors
        int n_multi_p = 0; // nodes with >1 predecessor (would break tree structure)
        for (int i = 0; i < n_nodes; ++i) {
            if (dag.pred[i].empty())
                ++n_roots;
            if (dag.succ[i].empty())
                ++n_leaves;
            if ((int) dag.pred[i].size() > 1)
                ++n_multi_p;
        }

        std::cout << "\n=== SkeletonDAG diagnostic ===\n"
                  << "  nodes     : " << n_nodes << "\n"
                  << "  edges     : " << n_edges << "\n"
                  << "  root      : " << dag.root << "\n"
                  << "  sources   : " << n_roots << " (nodes with no predecessor)\n"
                  << "  leaves    : " << n_leaves << " (nodes with no successor)\n"
                  << "  multi-par : " << n_multi_p << " (nodes with >1 predecessor — bad for a tree)\n\n";

        for (int i = 0; i < n_nodes; ++i) {
            const auto& pos = dag.nodes[i];
            std::cout << "  node " << i << (i == dag.root ? " [ROOT]" : "") << (dag.succ[i].empty() ? " [LEAF]" : "") << "\n"
                      << "    pos  : (" << pos.x() << ", " << pos.y() << ", " << pos.z() << ")\n"
                      << "    dist : " << dag.dist[i] << "\n"
                      << "    pred : [";
            for (int j = 0; j < (int) dag.pred[i].size(); ++j) {
                if (j)
                    std::cout << ", ";
                std::cout << dag.pred[i][j];
            }
            std::cout << "]\n"
                      << "    succ : [";
            for (int j = 0; j < (int) dag.succ[i].size(); ++j) {
                if (j)
                    std::cout << ", ";
                std::cout << dag.succ[i][j];
            }
            std::cout << "]\n";
        }
        std::cout << "=== end DAG diagnostic ===\n\n";
    }
    return dag;
}

std::vector<int> leaf_first_traversal(const SkeletonDAG& dag)
{
    // Multi-source BFS from all leaf nodes simultaneously, propagating toward
    // the root.  A node is enqueued once all of its children have been visited.
    // This alternates between branches at each level (leaves from branch A and
    // branch B are both in the initial queue), unlike reversing a root-seeded
    // topological sort which clusters nodes by branch.
    int n = dag.num_nodes();

    std::vector<int> unvisited_succ(n);
    for (int u = 0; u < n; ++u)
        unvisited_succ[u] = (int) dag.succ[u].size();

    std::queue<int> q;
    for (int i = 0; i < n; ++i)
        if (unvisited_succ[i] == 0)
            q.push(i); // seed from all leaves

    std::vector<int> order;
    order.reserve(n);
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        order.push_back(u);
        for (int p : dag.pred[u])
            if (--unvisited_succ[p] == 0)
                q.push(p);
    }
    return order;
}

std::vector<int> bottom_first_traversal(const SkeletonDAG& dag)
{
    int n = dag.num_nodes();
    std::vector<int> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return dag.nodes[a][2] < dag.nodes[b][2]; });
    return idx;
}

std::vector<int> ground_up_bfs(const SkeletonDAG& dag)
{
    // BFS from the DAG root outward via succ edges.  When the root is the
    // lowest-Z node (bed contact), this visits edges in ground-up order:
    // root-level branches first, then their children, etc.
    std::vector<int> order;
    if (dag.root < 0 || dag.num_nodes() == 0)
        return order;
    order.reserve(dag.num_nodes());
    std::queue<int> q;
    q.push(dag.root);
    while (!q.empty()) {
        int u = q.front();
        q.pop();
        order.push_back(u);
        for (int v : dag.succ[u])
            q.push(v);
    }
    return order;
}

std::vector<int> proximity_traversal(const SkeletonDAG& dag)
{
    int n = dag.num_nodes();
    if (n == 0)
        return {};

    std::vector<bool> visited(n, false);
    std::vector<int> order;
    order.reserve(n);

    // Seed from root; fall back to node 0 if root is unset.
    int seed = (dag.root >= 0 && dag.root < n) ? dag.root : 0;

    auto visit = [&](int idx) {
        visited[idx] = true;
        order.push_back(idx);
    };

    visit(seed);

    while ((int) order.size() < n) {
        const auto& last = dag.nodes[order.back()];
        int best         = -1;
        double best_d    = std::numeric_limits<double>::infinity();
        for (int i = 0; i < n; ++i) {
            if (visited[i])
                continue;
            double d = (dag.nodes[i] - last).squaredNorm();
            if (d < best_d) {
                best_d = d;
                best   = i;
            }
        }
        visit(best);
    }
    return order;
}

Eigen::Vector3d node_normal(const SkeletonDAG& dag, int node, const Eigen::Vector3d& root_pos)
{
    const Eigen::Vector3d& pt = dag.nodes[node];
    std::vector<Eigen::Vector3d> dirs;
    for (int nb : dag.pred[node]) {
        Eigen::Vector3d d = dag.nodes[nb] - pt;
        double len        = d.norm();
        if (len > 1e-6)
            dirs.push_back(d / len);
    }
    for (int nb : dag.succ[node]) {
        Eigen::Vector3d d = dag.nodes[nb] - pt;
        double len        = d.norm();
        if (len > 1e-6)
            dirs.push_back(d / len);
    }

    if (dirs.empty())
        return Eigen::Vector3d(0, 0, 1);

    // Single branch — normal is the branch direction itself.
    if (dirs.size() == 1) {
        Eigen::Vector3d n = dirs[0];
        if (n.dot(pt - root_pos) < 0) n = -n;
        return n;
    }

    // Build the 3×3 structure tensor M = Σᵢ dᵢ dᵢᵀ.
    // SelfAdjointEigenSolver returns eigenvalues in ascending order:
    //   col(0) = min eigenvector  — axis most ⊥ to all branches = junction plane normal
    //   col(2) = max eigenvector  — axis of maximum spread       = principal branch axis
    //
    // For a planar junction (Y, T, etc.) the junction plane normal is unique and
    // consistent across all arms, solving the ambiguity that SVD on the raw direction
    // matrix has for symmetric configurations (e.g. 120°/120°/120°).
    //
    // For a collinear / 180° junction the two smallest eigenvalues are degenerate
    // (no unique plane normal exists), so we fall back to the principal branch axis.
    Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
    for (const auto& d : dirs)
        M += d * d.transpose();

    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(M);
    const auto& vals = eig.eigenvalues();   // ascending: λ₀ ≤ λ₁ ≤ λ₂
    const auto& vecs = eig.eigenvectors();

    Eigen::Vector3d normal;
    // If λ_min is negligible relative to λ_max the junction is collinear/degenerate:
    // the "junction plane" is ill-defined, so use the principal branch axis instead.
    const double kDegenerateRatio = 0.1;
    if (vals(0) < kDegenerateRatio * vals(2))
        normal = vecs.col(2); // principal axis (max eigenvector)
    else
        normal = vecs.col(0); // junction plane normal (min eigenvector)

    if (normal.dot(pt - root_pos) < 0)
        normal = -normal;
    double nrm = normal.norm();
    return (nrm > 1e-8) ? (normal / nrm) : Eigen::Vector3d(0, 0, 1);
}

// Returns the halfedge of face fid whose supporting segment is nearest to p.
// Sets *out_t to the parameter value along [source, target] so callers can
// tell whether p is near an endpoint (t≈0 or t≈1) vs in the edge interior.
static Mesh::Halfedge_index find_halfedge_on(const Mesh& mesh, Mesh::Face_index fid, const Point_3& p, double& out_t)
{
    Mesh::Halfedge_index best;
    double best_dist = std::numeric_limits<double>::infinity();
    out_t            = 0.5;
    for (auto h : CGAL::halfedges_around_face(mesh.halfedge(fid), mesh)) {
        const Point_3& a = mesh.point(mesh.source(h));
        const Point_3& b = mesh.point(mesh.target(h));
        K::Vector_3 ab   = b - a;
        double ab_sq     = CGAL::to_double(ab.squared_length());
        if (ab_sq < 1e-20)
            continue;
        double t     = CGAL::to_double((p - a) * ab) / ab_sq;
        t            = std::max(0.0, std::min(1.0, t));
        Point_3 proj = a + t * ab;
        double d     = CGAL::to_double(CGAL::squared_distance(p, proj));
        if (d < best_dist) {
            best_dist = d;
            best      = h;
            out_t     = t;
        }
    }
    return best;
}

// ── Plane cross sections ──────────────────────────────────────────────────────
//
// Everything downstream of here — bore detection, centreline tracing and bed snapping —
// works from 2D cross sections rather than from raycasts. That is the whole reason hollow
// geometry works: the axis of a pipe sits in air, so a ray cast from it hits nothing,
// while the cross section still knows perfectly well where the material is.

using Slicer = CGAL::Polygon_mesh_slicer<Mesh, K>;

struct SecLoop
{
    std::vector<Eigen::Vector2d> pts;
    double          area{0.0};                            // signed
    Eigen::Vector2d centroid{Eigen::Vector2d::Zero()};    // area centroid
    double          max_r{0.0};                           // max |pt - centroid|
    int             depth{0};                             // 0 = outer boundary, 1 = hole, ...
};

struct Section
{
    Eigen::Vector3d origin{Eigen::Vector3d::Zero()};
    Eigen::Vector3d u{Eigen::Vector3d::UnitX()};
    Eigen::Vector3d w{Eigen::Vector3d::UnitY()};
    Eigen::Vector3d n{Eigen::Vector3d::UnitZ()};
    std::vector<SecLoop> loops;

    bool empty() const { return loops.empty(); }
    Eigen::Vector3d to3d(const Eigen::Vector2d& q) const { return origin + q.x() * u + q.y() * w; }
    Eigen::Vector2d to2d(const Eigen::Vector3d& p) const
    {
        const Eigen::Vector3d d = p - origin;
        return Eigen::Vector2d(d.dot(u), d.dot(w));
    }
};

static void plane_basis(const Eigen::Vector3d& n, Eigen::Vector3d& u, Eigen::Vector3d& w)
{
    const Eigen::Vector3d a = (std::abs(n.z()) < 0.9) ? Eigen::Vector3d::UnitZ() : Eigen::Vector3d::UnitX();
    u = n.cross(a).normalized();
    w = n.cross(u);
}

static bool point_in_loop(const SecLoop& L, const Eigen::Vector2d& p)
{
    bool inside = false;
    const int m = (int) L.pts.size();
    for (int i = 0, j = m - 1; i < m; j = i++) {
        const Eigen::Vector2d& a = L.pts[i];
        const Eigen::Vector2d& b = L.pts[j];
        if (((a.y() > p.y()) != (b.y() > p.y())) &&
            (p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()))
            inside = !inside;
    }
    return inside;
}

// Cross section of `mesh` by the plane through p with unit normal n, in plane coordinates
// centred on p (so p itself is the 2D origin).
static Section slice_section(Slicer& slicer, const Eigen::Vector3d& p, const Eigen::Vector3d& n)
{
    Section s;
    s.n      = n;
    s.origin = p;
    plane_basis(n, s.u, s.w);

    std::vector<std::vector<Point_3>> polys;
    try {
        slicer(Plane_3(Point_3(p.x(), p.y(), p.z()), K::Vector_3(n.x(), n.y(), n.z())),
               std::back_inserter(polys));
    } catch (const std::exception&) {
        return s; // degenerate plane / mesh — treat as no section rather than propagating
    }

    for (auto& poly : polys) {
        if (poly.size() > 1 && CGAL::squared_distance(poly.front(), poly.back()) < 1e-14)
            poly.pop_back(); // closed polyline repeats its first point
        if (poly.size() < 3)
            continue;

        SecLoop L;
        L.pts.reserve(poly.size());
        for (const Point_3& q : poly) {
            const Eigen::Vector3d d(CGAL::to_double(q.x()) - p.x(),
                                    CGAL::to_double(q.y()) - p.y(),
                                    CGAL::to_double(q.z()) - p.z());
            L.pts.emplace_back(d.dot(s.u), d.dot(s.w));
        }

        double a2 = 0.0;
        Eigen::Vector2d c(0.0, 0.0);
        const int m = (int) L.pts.size();
        for (int i = 0; i < m; ++i) {
            const Eigen::Vector2d& q0 = L.pts[i];
            const Eigen::Vector2d& q1 = L.pts[(i + 1) % m];
            const double cr = q0.x() * q1.y() - q1.x() * q0.y();
            a2 += cr;
            c += cr * (q0 + q1);
        }
        L.area = 0.5 * a2;
        if (std::abs(L.area) < 1e-9)
            continue; // sliver from a tangent plane
        L.centroid = (std::abs(a2) > 1e-12) ? Eigen::Vector2d(c / (3.0 * a2)) : L.pts[0];
        for (const auto& q : L.pts)
            L.max_r = std::max(L.max_r, (q - L.centroid).norm());
        s.loops.push_back(std::move(L));
    }

    for (size_t i = 0; i < s.loops.size(); ++i)
        for (size_t j = 0; j < s.loops.size(); ++j)
            if (i != j && point_in_loop(s.loops[j], s.loops[i].pts[0]))
                ++s.loops[i].depth;
    return s;
}

// Index of the outer (depth-0) loop containing q, else the largest outer loop, else -1.
static int outer_loop_at(const Section& s, const Eigen::Vector2d& q, bool* contained = nullptr)
{
    int    best      = -1;
    double best_area = -1.0;
    if (contained)
        *contained = false;
    for (size_t i = 0; i < s.loops.size(); ++i) {
        if (s.loops[i].depth != 0)
            continue;
        if (point_in_loop(s.loops[i], q)) {
            if (contained)
                *contained = true;
            return (int) i;
        }
        const double a = std::abs(s.loops[i].area);
        if (a > best_area) {
            best_area = a;
            best      = (int) i;
        }
    }
    return best;
}

void smooth_skeleton_graph(SkeletonGraph& G, int iterations, float alpha, bool fix_endpoints,
                           const Eigen::MatrixXd* V, const Eigen::MatrixXi* F)
{
    const int n = G.num_nodes();
    std::vector<Eigen::Vector3d> pos(G.nodes);

    // Optional containment: never let a smoothing step push a node out of the solid.
    //
    // Laplacian smoothing moves a node toward the average of its neighbours, and at a branch
    // junction that average lies across the void between the arms — so smoothing rounds the
    // corner straight through air. Measured on a solid genus-0 trident, it was the single
    // largest source of skeleton outside the material: 7.1% of the length before this stage,
    // 14.2% after it. That matters beyond appearances, because cut planes are placed at EDGE
    // MIDPOINTS, so an edge lying in air is a cut plane centred on nothing.
    //
    // The step is shortened rather than refused outright: halving a few times keeps most of the
    // smoothing where a full step would have escaped, and only pins the node when even a small
    // step leaves the solid. Containment is the same test snap_to_bed and the bore march use —
    // inside a depth-0 loop of the horizontal section — so a bore axis still counts as inside
    // and hollow parts behave exactly as before.
    std::unique_ptr<Mesh>   guard_mesh;
    std::unique_ptr<Slicer> guard;
    if (V != nullptr && F != nullptr && V->rows() > 0 && F->rows() > 0) {
        guard_mesh = std::make_unique<Mesh>(build_prepared_mesh(*V, *F));
        if (CGAL::is_closed(*guard_mesh))
            guard = std::make_unique<Slicer>(*guard_mesh);
    }
    auto inside_solid = [&](const Eigen::Vector3d& p) {
        if (!guard)
            return true;
        const Section s = slice_section(*guard, p, Eigen::Vector3d::UnitZ());
        if (s.empty())
            return false;
        bool in = false;
        outer_loop_at(s, s.to2d(p), &in);
        return in;
    };

    // Laplacian smoothing shrinks. On a tree the pinned degree-1 tips bound that, but a
    // component containing a CYCLE has nothing holding the cycle out and it collapses toward
    // its own centroid without limit. Measured on the torus corpus mesh (ideal loop radius
    // 25, tube radius 6): r = 22.9 at 0 iterations, 20.6 at 10, and 15.9 at 40 — by then the
    // skeleton has left the material entirely. The MCF skeleton of a washer or a windowed
    // plate behaves the same way.
    //
    // Those components get Taubin's λ/μ pass instead: a positive step followed by a slightly
    // larger negative one, with the pass-band edge at k≈0.1, which removes noise without net
    // shrinkage. Keyed on "the component has a cycle" rather than "has no tips", because a
    // loop with one stray MCF tip hanging off it is still a loop that will collapse — that is
    // exactly what the torus produces. Acyclic components keep today's plain Laplacian
    // behaviour, so no existing tree-skeleton model changes.
    std::vector<char> taubin(n, 0);
    {
        std::vector<char> seen(n, 0);
        for (int s = 0; s < n; ++s) {
            if (seen[s] || G.adj[s].empty())
                continue;
            std::vector<int> comp;
            std::queue<int>  q;
            q.push(s);
            seen[s] = 1;
            int half_edges = 0;
            while (!q.empty()) {
                const int u = q.front();
                q.pop();
                comp.push_back(u);
                half_edges += (int) G.adj[u].size();
                for (int v : G.adj[u])
                    if (!seen[v]) { seen[v] = 1; q.push(v); }
            }
            if (half_edges / 2 >= (int) comp.size()) // |E| >= |V| within the component
                for (int u : comp)
                    taubin[u] = 1;
        }
    }
    // 1/λ + 1/μ = 0.1  =>  μ < 0. λ is capped at 0.45 for stability: the amplification factor
    // (1-λk)(1-μk) must stay within ±1 for graph-Laplacian eigenvalues k up to 2, which fails
    // once λ approaches 1.
    const double lambda = std::min<double>(alpha, 0.45);
    const double mu     = (lambda > 1e-9) ? 1.0 / (0.1 - 1.0 / lambda) : 0.0;

    for (int iter = 0; iter < iterations; ++iter) {
        std::vector<Eigen::Vector3d> np(pos);
        for (int node = 0; node < n; ++node) {
            const auto& nbs = G.adj[node];
            if (nbs.empty())
                continue;
            if (fix_endpoints && (int) nbs.size() == 1)
                continue;
            Eigen::Vector3d c = Eigen::Vector3d::Zero();
            for (int nb : nbs)
                c += pos[nb];
            c /= (double) nbs.size();
            const double step = taubin[node] ? ((iter % 2 == 0) ? lambda : mu) : (double) alpha;
            Eigen::Vector3d cand = (1.0 - step) * pos[node] + step * c;
            if (guard && !inside_solid(cand)) {
                // Back off toward where the node already is. Four halvings take a 0.5 step to
                // 0.03; below that the smoothing is not worth the escape, so keep the node.
                double s = step;
                bool   ok = false;
                for (int k = 0; k < 4 && !ok; ++k) {
                    s *= 0.5;
                    cand = (1.0 - s) * pos[node] + s * c;
                    ok   = inside_solid(cand);
                }
                if (!ok)
                    cand = pos[node];
            }
            np[node] = cand;
        }
        pos = np;
    }
    G.nodes = pos;
}

// ── snap_to_bed ───────────────────────────────────────────────────────────────

// The tip branches of a graph: from each degree-1 node through degree-2 nodes to the first node
// that is not degree 2 (a junction, or the far tip of a plain chain).
namespace {
struct TipBranch
{
    int              tip{-1};
    int              end{-1};
    double           len{0.0};
    std::vector<int> path; // tip first, `end` excluded
};
}

static std::vector<TipBranch> tip_branches(const SkeletonGraph& G)
{
    std::vector<TipBranch> out;
    const int n = G.num_nodes();
    for (int i = 0; i < n; ++i) {
        if (G.degree(i) != 1)
            continue;
        TipBranch b;
        b.tip = i;
        b.path.push_back(i);
        int prev = i, cur = G.adj[i][0];
        for (int guard = 0; guard <= n; ++guard) {
            b.len += (G.nodes[cur] - G.nodes[prev]).norm();
            if (G.degree(cur) != 2 || cur == i)
                break;
            b.path.push_back(cur);
            const int next = G.adj[cur][0] == prev ? G.adj[cur][1] : G.adj[cur][0];
            prev = cur;
            cur  = next;
        }
        b.end = cur;
        out.push_back(std::move(b));
    }
    return out;
}

// Short tip branches that hang BELOW the junction they leave: MCF stubs where the part sits on
// the bed. On Intake_manifold stood up as a U they were the root -- a 3.6 mm stub under the
// plenum junction got extended to the bed and every cut grew diagonally out of it.
static std::vector<char> hanging_twigs(const SkeletonGraph& G, const std::vector<TipBranch>& branches,
                                       double twig_len)
{
    std::vector<char> hanging(G.num_nodes(), 0);
    for (const TipBranch& b : branches)
        if (G.degree(b.end) >= 3 && G.nodes[b.end].z() > G.nodes[b.tip].z() && b.len < twig_len)
            for (int k : b.path)
                hanging[k] = 1;
    return hanging;
}

static bool point_in_poly2(const std::vector<Eigen::Vector2d>& poly, const Eigen::Vector2d& p)
{
    bool inside = false;
    const int m = (int) poly.size();
    for (int i = 0, j = m - 1; i < m; j = i++) {
        const Eigen::Vector2d& a = poly[i];
        const Eigen::Vector2d& b = poly[j];
        if (((a.y() > p.y()) != (b.y() > p.y())) &&
            (p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()))
            inside = !inside;
    }
    return inside;
}

// Plan-view distance from p to the bed contact, 0 when over it. Not a containment test: a duct
// lying on the bed, or a U on its bend, touches it along a hairline, and the centreline above that
// line is "over the contact" for every purpose here.
static double footprint_gap(const BedRootPlan& plan, const Eigen::Vector3d& p)
{
    const Eigen::Vector2d q    = p.head<2>();
    double                best = std::numeric_limits<double>::max();
    for (const auto& loop : plan.footprint) {
        if (point_in_poly2(loop, q))
            return 0.0;
        for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
            const Eigen::Vector2d ab = loop[i] - loop[j];
            const double          l2 = ab.squaredNorm();
            const double          t  = l2 > 1e-18 ? std::clamp((q - loop[j]).dot(ab) / l2, 0.0, 1.0) : 0.0;
            best = std::min(best, (q - (loop[j] + t * ab)).norm());
        }
    }
    return best;
}

// How far beside the contact a foot may land: a centreline wanders off the exact line of contact
// by a fraction of the pipe's radius, never by the part's size.
static double footprint_slack(const BedRootPlan& plan)
{
    return std::max(2.0, 0.5 * plan.low_zone);
}

int snap_to_bed(SkeletonGraph& G, const Eigen::MatrixXd& V, const Eigen::MatrixXi& F,
                double band_mm, double* out_bed_z, BedRootMode mode, BedRootPlan* plan)
{
    if (plan)
        *plan = BedRootPlan{};
    const int n = G.num_nodes();
    const double bed_z = (V.rows() > 0) ? V.col(2).minCoeff() : 0.0;
    if (out_bed_z)
        *out_bed_z = bed_z;
    if (n == 0 || V.rows() == 0)
        return 0;

    const double z_ext = V.col(2).maxCoeff() - bed_z;
    const double band  = band_mm > 0.0 ? band_mm : std::max(5.0, 0.15 * z_ext);
    const double mesh_diag =
        (Eigen::Vector3d(V.colwise().maxCoeff().transpose()) -
         Eigen::Vector3d(V.colwise().minCoeff().transpose())).norm();

    Mesh   mesh = build_prepared_mesh(V, F);
    Slicer slicer(mesh);

    // The object's footprint. A target point has to be inside this outer contour — note
    // "inside the contour", not "on material": for a pipe the axis lands in the bore.
    const double eps = std::max(1e-3, z_ext * 1e-4);
    const Section bed_sec = slice_section(slicer, Eigen::Vector3d(0.0, 0.0, bed_z + eps),
                                          Eigen::Vector3d::UnitZ());

    auto footprint_target = [&](const Eigen::Vector3d& tip, const Eigen::Vector3d& dir,
                                double dz, Eigen::Vector3d& target) {
        if (bed_sec.empty())
            return false;
        // First choice: continue along the branch until it reaches the bed plane.
        if (dir.z() < -1e-6) {
            Eigen::Vector3d t = tip + dir * (dz / -dir.z());
            t.z()             = bed_z;
            bool in = false;
            outer_loop_at(bed_sec, bed_sec.to2d(t), &in);
            if (in) { target = t; return true; }
        }
        // Second choice: straight down.
        {
            Eigen::Vector3d t(tip.x(), tip.y(), bed_z);
            bool in = false;
            outer_loop_at(bed_sec, bed_sec.to2d(t), &in);
            if (in) { target = t; return true; }
        }
        // Last resort: the centroid of the footprint loop nearest the tip in XY.
        int    best = -1;
        double bd   = std::numeric_limits<double>::max();
        for (size_t i = 0; i < bed_sec.loops.size(); ++i) {
            if (bed_sec.loops[i].depth != 0)
                continue;
            const Eigen::Vector3d c = bed_sec.to3d(bed_sec.loops[i].centroid);
            const double d = (c.head<2>() - tip.head<2>()).squaredNorm();
            if (d < bd) { bd = d; best = (int) i; }
        }
        if (best < 0)
            return false;
        target = bed_sec.to3d(bed_sec.loops[best].centroid);
        target.z() = bed_z;
        return true;
    };

    // ── Middle or tips ──
    // Decided before anything moves. A LEG is a tip whose own branch runs down and drops well
    // below the junction (or far end) it leaves: a pipe standing up, a trident on its prongs.
    // Those keep the tip roots below. A part with no leg -- a duct lying across the bed, a U on
    // its bend, a manifold whose runners all rise from the plenum -- touches the bed somewhere
    // mid-branch, and that is where its one root belongs.
    if (plan && mode != BedRootMode::Tips && !bed_sec.empty()) {
        const std::vector<TipBranch> branches = tip_branches(G);
        const double leg_drop = std::max(band, 0.25 * z_ext);
        const double twig_len = 0.25 * mesh_diag;
        int legs = 0;
        for (const TipBranch& b : branches) {
            const Eigen::Vector3d d  = G.nodes[b.tip] - G.nodes[G.adj[b.tip][0]];
            const double          dn = d.norm();
            if (dn > 1e-9 && d.z() / dn <= -0.5 && G.nodes[b.end].z() - G.nodes[b.tip].z() >= leg_drop)
                ++legs;
        }
        const std::vector<char> hanging = hanging_twigs(G, branches, twig_len);

        if (legs == 0 || mode == BedRootMode::Middle) {
            // The middle of what rests on the bed: area centroid of the outer contact loops.
            Eigen::Vector3d c  = Eigen::Vector3d::Zero();
            double          ca = 0.0;
            for (const SecLoop& L : bed_sec.loops)
                if (L.depth == 0) {
                    const double a = std::abs(L.area);
                    c += a * bed_sec.to3d(L.centroid);
                    ca += a;
                }
            if (ca > 0.0)
                c /= ca;

            bool any_struct = false;
            for (int i = 0; i < n; ++i)
                any_struct |= !hanging[i];
            double z_low = std::numeric_limits<double>::max();
            for (int i = 0; i < n; ++i)
                if (!hanging[i] || !any_struct)
                    z_low = std::min(z_low, G.nodes[i].z());
            BedRootPlan cand;
            cand.centre   = Eigen::Vector3d(c.x(), c.y(), bed_z);
            cand.bed_z    = bed_z;
            cand.low_zone = std::max(band, 0.15 * z_ext);
            cand.twig_len = twig_len;
            for (const SecLoop& L : bed_sec.loops)
                if (L.depth == 0) {
                    std::vector<Eigen::Vector2d> loop;
                    loop.reserve(L.pts.size());
                    for (const Eigen::Vector2d& q : L.pts)
                        loop.push_back(bed_sec.to3d(q).head<2>());
                    cand.footprint.push_back(std::move(loop));
                }

            // Feasible when some low structural node stands over the contact. The exact point is
            // chosen by plant_middle_root() on the final graph.
            int    best = -1;
            double bd   = std::numeric_limits<double>::max();
            for (int i = 0; i < n; ++i) {
                if ((hanging[i] && any_struct) || G.nodes[i].z() > z_low + cand.low_zone ||
                    footprint_gap(cand, G.nodes[i]) > footprint_slack(cand))
                    continue;
                const double d = (G.nodes[i].head<2>() - c.head<2>()).norm();
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0) {
                *plan            = std::move(cand);
                plan->use_middle = true;
                std::ostringstream os;
                os << (mode == BedRootMode::Middle ? "middle (forced)" : "middle: no leg") << ", nearest node "
                   << bd << " mm from the footprint centre at z=" << G.nodes[best].z();
                plan->reason = os.str();
                std::cout << "[snap_to_bed] root goes under the " << plan->reason << "; tips left alone.\n";
                return 0;
            }
            plan->reason = "no low centreline node over the footprint; using tips";
            std::cout << "[snap_to_bed] " << plan->reason << "\n";
        } else {
            std::ostringstream os;
            os << "tips: " << legs << " leg(s)";
            plan->reason = os.str();
        }
    }

    int moved = 0;
    for (int i = 0; i < n; ++i) {
        if (G.degree(i) != 1)
            continue;
        const Eigen::Vector3d pos = G.nodes[i];
        const double dz = pos.z() - bed_z;
        if (dz <= 1e-6 || dz > band)
            continue;

        // The tip's own branch has to run downward. This is what keeps a part lying on its
        // side — tips at mid height, branches horizontal — off the bed.
        Eigen::Vector3d d  = pos - G.nodes[G.adj[i][0]];
        const double    dn = d.norm();
        if (dn < 1e-9)
            continue;
        d /= dn;
        if (d.z() > -0.5)
            continue;

        Eigen::Vector3d target;
        if (!footprint_target(pos, d, dz, target))
            continue;
        G.nodes[i] = target;
        ++moved;
    }

    // Ground EVERY leg that belongs on the bed, not just the ones already within the band.
    //
    // MCF retracts a branch tip well short of the material it describes — measured on a
    // symmetric trident with 60 mm prongs, the skeleton reached only 47.3 / 44.1 / 36.5 mm,
    // and asymmetrically at that. On a part shaped like a real manifold, several runners
    // standing on the bed and merging into a plenum, that retraction lifts most of the legs
    // clear of the bed: two of three ended at z = 16.1 and z = 26.0. The band above is far too
    // tight to catch them, and the fallback below only ever grounded the single LOWEST tip, so
    // the part came out with ONE root instead of three — no convergence, and a cut order that
    // walks up and over the other legs rather than treating them as feet.
    //
    // The guard that makes this safe is a material check, not a distance one. footprint_target
    // only asks that the landing point lie inside the bed contour, which an arm hanging in free
    // air over the footprint also satisfies; requiring the whole run down to stay inside the
    // part is what separates a retracted leg from a genuine overhang.
    auto path_inside = [&](const Eigen::Vector3d& from, const Eigen::Vector3d& to) {
        const int kSamples = 8;
        for (int s = 1; s < kSamples; ++s) {
            const Eigen::Vector3d q = from + (double(s) / kSamples) * (to - from);
            const Section sec = slice_section(slicer, q, Eigen::Vector3d::UnitZ());
            if (sec.empty())
                return false;
            bool in = false;
            outer_loop_at(sec, sec.to2d(q), &in);
            if (!in)
                return false;
        }
        return true;
    };

    for (int i = 0, n_before = n; i < n_before; ++i) {
        if (G.degree(i) != 1)
            continue;
        const Eigen::Vector3d pos = G.nodes[i];
        const double dz = pos.z() - bed_z;
        if (dz <= band)          // already on the bed, or handled by the band pass above
            continue;
        Eigen::Vector3d d  = pos - G.nodes[G.adj[i][0]];
        const double    dn = d.norm();
        if (dn < 1e-9)
            continue;
        d /= dn;
        if (d.z() > -0.5)        // same direction test: a sideways tip is not a foot
            continue;
        Eigen::Vector3d target;
        if (!footprint_target(pos, d, dz, target))
            continue;
        if (!path_inside(pos, target))
            continue;
        const int idx = G.num_nodes();
        G.add_node(target);
        G.add_edge(i, idx);
        ++moved;
        std::cout << "[snap_to_bed] extended retracted leg at z=" << pos.z()
                  << " down to the bed.\n";
    }

    if (moved == 0) {
        // No tip was close enough to the bed. Extend the lowest downward-running tip, if
        // there is one, so the DAG still gets a root on the bed.
        int low = -1;
        for (int i = 0; i < n; ++i) {
            if (G.degree(i) != 1)
                continue;
            if (low < 0 || G.nodes[i].z() < G.nodes[low].z())
                low = i;
        }
        if (low >= 0 && G.nodes[low].z() - bed_z > 1e-6) {
            Eigen::Vector3d d  = G.nodes[low] - G.nodes[G.adj[low][0]];
            const double    dn = d.norm();
            if (dn > 1e-9) {
                d /= dn;
                Eigen::Vector3d target;
                // Whether the tip's own direction can be trusted depends on whether the skeleton
                // represents the part at all.
                //
                // A bare `d.z() < 0` is too weak: the traced centreline of a pipe lying on its
                // side ends with a slope of about -0.006 (0.03 mm of droop over a 5 mm step),
                // which passes "is it negative", so the fallback grew a spurious vertical branch
                // to the bed and elected it root — on a part that must not snap at all.
                //
                // But requiring a real downward run is too strong when skeletonization has
                // degenerated: MCF returns a cone as TWO nodes 0.011 mm apart sharing one nearly
                // horizontal edge. That carries no direction to test, and refusing to ground it
                // leaves the DAG with no root at the bed. So: if the skeleton spans a decent
                // fraction of the part it is trusted and must earn the snap; if it is a degenerate
                // blob, grounding it is the only useful thing left to do.
                double skel_ext = 0.0;
                for (int a = 0; a < n; ++a)
                    for (int b = a + 1; b < n; ++b)
                        skel_ext = std::max(skel_ext, (G.nodes[a] - G.nodes[b]).norm());
                const bool degenerate  = skel_ext < 0.2 * mesh_diag;
                const bool runs_down   = d.z() <= -0.5;
                if (degenerate)
                    std::cout << "[snap_to_bed] Skeleton spans only " << skel_ext << "mm of a "
                              << mesh_diag << "mm part — too degenerate to have a direction;"
                              << " grounding it anyway.\n";
                if ((runs_down || degenerate) &&
                    footprint_target(G.nodes[low], degenerate ? Eigen::Vector3d(0, 0, -1) : d,
                                     G.nodes[low].z() - bed_z, target)) {
                    const int idx = G.num_nodes();
                    G.add_node(target);
                    G.add_edge(low, idx);
                    moved = 1;
                    std::cout << "[snap_to_bed] No tip within the band; extended tip " << low
                              << " to the bed with a new node.\n";
                }
            }
        }
    }

    std::cout << "[snap_to_bed] bed_z=" << bed_z << " band=" << band
              << "mm — snapped " << moved << " tip(s).\n";
    return moved;
}

// ── middle root ───────────────────────────────────────────────────────────────

int plant_middle_root(SkeletonGraph& G, const BedRootPlan& plan)
{
    if (!plan.use_middle || G.num_nodes() == 0)
        return -1;

    // Drop the stubs hanging under the structure. They are found again on the final graph rather
    // than carried over from the plan: resampling renumbered everything since.
    {
        const std::vector<char> hanging = hanging_twigs(G, tip_branches(G), plan.twig_len);
        int n_drop = 0;
        for (char h : hanging)
            n_drop += h;
        if (n_drop > 0 && n_drop < G.num_nodes()) {
            std::vector<int> remap(G.num_nodes(), -1);
            SkeletonGraph    kept;
            for (int i = 0; i < G.num_nodes(); ++i)
                if (!hanging[i]) {
                    remap[i] = kept.num_nodes();
                    kept.add_node(G.nodes[i]);
                }
            for (int u = 0; u < G.num_nodes(); ++u)
                for (int v : G.adj[u])
                    if (u < v && remap[u] >= 0 && remap[v] >= 0)
                        kept.add_edge(remap[u], remap[v]);
            G = std::move(kept);
            std::cout << "[plant_middle_root] pruned " << n_drop << " node(s) of stubs hanging below the centreline.\n";
        }
    }

    // The point to hang the foot from: on the low part of the centreline, over the contact, and
    // nearest the contact's centre in plan view. Mid-edge points split their edge, unless they sit
    // within a fifth of it from an end, where the end node serves.
    const Eigen::Vector2d c2    = plan.centre.head<2>();
    const double          slack = footprint_slack(plan);
    double                z_low = std::numeric_limits<double>::max();
    for (const Eigen::Vector3d& p : G.nodes)
        z_low = std::min(z_low, p.z());
    const auto usable = [&](const Eigen::Vector3d& p) {
        return p.z() - plan.bed_z >= 1e-3 && footprint_gap(plan, p) <= slack;
    };
    int    bu = -1, bv = -1;
    double bt = 0.0, bd = std::numeric_limits<double>::max();
    for (int u = 0; u < G.num_nodes(); ++u) {
        if (G.adj[u].empty() && G.nodes[u].z() <= z_low + plan.low_zone && usable(G.nodes[u])) {
            const double d = (G.nodes[u].head<2>() - c2).norm();
            if (d < bd) { bd = d; bu = u; bv = -1; bt = 0.0; }
        }
        for (int v : G.adj[u]) {
            if (v < u || std::min(G.nodes[u].z(), G.nodes[v].z()) > z_low + plan.low_zone)
                continue;
            const Eigen::Vector3d& a  = G.nodes[u];
            const Eigen::Vector3d& b  = G.nodes[v];
            const Eigen::Vector2d  ab = (b - a).head<2>();
            const double           l2 = ab.squaredNorm();
            const double           t  = l2 > 1e-12 ? std::clamp((c2 - a.head<2>()).dot(ab) / l2, 0.0, 1.0) : 0.0;
            const Eigen::Vector3d  p  = a + t * (b - a);
            if (!usable(p))
                continue;
            const double d = (p.head<2>() - c2).norm();
            if (d < bd - 1e-9) { bd = d; bu = u; bv = v; bt = t; }
        }
    }
    if (bu < 0) {
        std::cout << "[plant_middle_root] no low centreline point over the bed contact; no foot planted.\n";
        return -1;
    }
    int m = bu;
    if (bv >= 0) {
        if (bt >= 0.8)
            m = bv;
        else if (bt > 0.2) {
            m = G.num_nodes();
            G.add_node(G.nodes[bu] + bt * (G.nodes[bv] - G.nodes[bu]));
            auto& au = G.adj[bu];
            auto& av = G.adj[bv];
            std::replace(au.begin(), au.end(), bv, m);
            std::replace(av.begin(), av.end(), bu, m);
            G.adj[m] = {bu, bv};
        }
    }

    const int foot = G.num_nodes();
    G.add_node(Eigen::Vector3d(G.nodes[m].x(), G.nodes[m].y(), plan.bed_z));
    G.add_edge(m, foot);
    std::cout << "[plant_middle_root] foot under node " << m << " at (" << G.nodes[m].x() << ", "
              << G.nodes[m].y() << "), " << (G.nodes[m].z() - plan.bed_z) << " mm above the bed.\n";
    return foot;
}

int skeleton_foot_top(const std::vector<Eigen::Vector3d>& nodes, const std::vector<std::vector<int>>& adj,
                      int root)
{
    if (root < 0 || root >= (int) adj.size() || adj[root].size() != 1)
        return -1;
    const int             top = adj[root][0];
    const Eigen::Vector3d d   = nodes[top] - nodes[root];
    const double          len = d.norm();
    if (len < 1e-9 || d.z() / len < 0.999 || adj[top].size() < 3)
        return -1;
    int level = 0;
    for (int o : adj[top]) {
        if (o == root)
            continue;
        const Eigen::Vector3d e  = nodes[o] - nodes[top];
        const double          el = e.norm();
        if (el > 1e-9 && std::abs(e.z()) / el < 0.8)
            ++level;
    }
    return level >= 2 ? top : -1;
}

double SkeletonRootRamp::lean_deg(double s_mid, bool from_foot, double seat_r) const
{
    const double l0 = from_foot ? lean0_foot_deg : 90.0;
    if (ramp <= 0.0)
        return 0.0;
    const double x = (s_mid - (from_foot ? seat_r : 0.0)) / ramp;
    if (x <= 0.0)
        return l0;
    if (x >= 1.0)
        return 0.0;
    return l0 * (1.0 - x * x * (3.0 - 2.0 * x));
}

SkeletonRootRamp skeleton_root_ramp(const std::vector<Eigen::Vector3d>& nodes,
                                    const std::vector<std::vector<int>>& adj,
                                    const std::vector<int>& roots,
                                    double root_lean_deg, double root_ramp)
{
    SkeletonRootRamp R;
    const int n = (int) nodes.size();
    R.lean0_foot_deg = std::clamp(root_lean_deg, 0.0, 90.0);
    R.s.assign(n, std::numeric_limits<double>::infinity());
    R.foot.assign(n, 0);
    R.seat.assign(n, 0.0);
    if (n == 0)
        return R;

    using QE = std::pair<double, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
    // The flatter the first planes, the lower they meet over the middle: planes tilted 90 - lean from
    // +Z, `seat` either side, meet seat / tan(lean) above the centreline -- so 1.1 h needs a seat of
    // 1.1 h tan(lean). (Vertical planes, lean 0, never meet; flat ones, lean 90, need the clamp below.)
    const double tan_lean = std::tan(std::min(R.lean0_foot_deg, 89.9) * M_PI / 180.0);
    for (int r : roots) {
        if (r < 0 || r >= n)
            continue;
        const int top = skeleton_foot_top(nodes, adj, r);
        if (top >= 0) {
            const double h    = (nodes[top] - nodes[r]).norm();
            const double seat = 1.1 * h * tan_lean;
            for (int k : {r, top}) {
                R.s[k]    = 0.0;
                R.foot[k] = 1;
                R.seat[k] = seat;
                pq.push({0.0, k});
            }
        } else {
            R.s[r] = 0.0;
            pq.push({0.0, r});
        }
    }
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > R.s[u])
            continue;
        for (int v : adj[u]) {
            const double nd = d + (nodes[v] - nodes[u]).norm();
            if (nd < R.s[v]) {
                R.s[v]    = nd;
                R.foot[v] = R.foot[u];
                R.seat[v] = R.seat[u];
                pq.push({nd, v});
            }
        }
    }

    double max_s = 0.0, edge_sum = 0.0;
    int    edge_n = 0;
    for (int u = 0; u < n; ++u) {
        if (std::isfinite(R.s[u]))
            max_s = std::max(max_s, R.s[u]);
        for (int v : adj[u])
            if (u < v) {
                edge_sum += (nodes[v] - nodes[u]).norm();
                ++edge_n;
            }
    }
    const double mean_edge = edge_n > 0 ? edge_sum / edge_n : 0.0;
    R.ramp = root_ramp > 0.0 ? root_ramp : std::max(3.0 * mean_edge, 0.3 * max_s);
    // A seat swallowing most of the part would leave nothing to cut: a very shallow lean asks for
    // that, and is better served by a smaller seat that crosses near the top than by no cuts.
    for (double& seat : R.seat)
        seat = std::min(seat, 0.4 * max_s);
    return R;
}

SkeletonGraph collapse_skeleton_graph(const SkeletonDAG& dag, float thresh)
{
    int n = dag.num_nodes();
    if (n == 0)
        return {};

    const double kCosThresh = std::cos(thresh * M_PI / 180.0);

    Eigen::Vector3d root_pos = (dag.root >= 0 && dag.root < n) ? dag.nodes[dag.root] : dag.nodes[0];

    std::vector<Eigen::Vector3d> normals(n);
    for (int i = 0; i < n; ++i)
        normals[i] = node_normal(dag, i, root_pos);

    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) -> int { return parent[x] == x ? x : parent[x] = find(parent[x]); };

    for (int u = 0; u < n; ++u) {
        if (dag.succ[u].empty())
            continue; // preserve leaf nodes
        auto try_merge = [&](int v) {
            if (dag.succ[v].empty())
                return; // preserve leaf nodes
            if (std::abs(normals[u].dot(normals[v])) >= kCosThresh)
                parent[find(u)] = find(v);
        };
        for (int v : dag.succ[u])
            try_merge(v);
        for (int v : dag.pred[u])
            try_merge(v);
    }

    // One new node per component at the centroid of its members.
    std::unordered_map<int, int> comp_to_new;
    std::vector<Eigen::Vector3d> new_pos;
    std::vector<int> cnt;

    for (int i = 0; i < n; ++i) {
        int r               = find(i);
        auto [it, inserted] = comp_to_new.emplace(r, (int) new_pos.size());
        if (inserted) {
            new_pos.emplace_back(Eigen::Vector3d::Zero());
            cnt.push_back(0);
        }
        int ni = it->second;
        new_pos[ni] += dag.nodes[i];
        cnt[ni]++;
    }
    for (int i = 0; i < (int) new_pos.size(); ++i)
        new_pos[i] /= cnt[i];

    SkeletonGraph G;
    for (auto& p : new_pos)
        G.add_node(p);

    // Edges between different components (deduplicated).
    std::set<std::pair<int, int>> seen;
    for (int u = 0; u < n; ++u) {
        int nu = comp_to_new[find(u)];
        for (int v : dag.succ[u]) {
            int nv = comp_to_new[find(v)];
            if (nu == nv)
                continue;
            auto e = std::make_pair(std::min(nu, nv), std::max(nu, nv));
            if (seen.insert(e).second)
                G.add_edge(nu, nv);
        }
    }

    std::cout << "[collapse_skeleton] " << n << " → " << G.num_nodes() << " nodes  (angle_thresh=" << thresh << "°)\n";
    return G;
}

SkeletonGraph collapse_skeleton_graph(const SkeletonDAG& dag, float angle_thresh, float edge_len_thresh)
{
    int n = dag.num_nodes();
    if (n == 0)
        return {};

    const double kCosThresh = (angle_thresh > 0.f) ? std::cos(angle_thresh * M_PI / 180.0) : 2.0; // >1 disables angle check
    const double kLenThresh = (double) edge_len_thresh;

    Eigen::Vector3d root_pos = (dag.root >= 0 && dag.root < n) ? dag.nodes[dag.root] : dag.nodes[0];

    std::vector<Eigen::Vector3d> normals(n);
    if (angle_thresh > 0.f)
        for (int i = 0; i < n; ++i)
            normals[i] = node_normal(dag, i, root_pos);

    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) -> int { return parent[x] == x ? x : parent[x] = find(parent[x]); };

    for (int u = 0; u < n; ++u) {
        auto try_merge = [&](int v) {
            // Never collapse both endpoints if both are leaves — keeps tips intact.
            if (dag.succ[u].empty() && dag.succ[v].empty())
                return;

            double edge_len = (dag.nodes[u] - dag.nodes[v]).norm();
            bool by_length  = (kLenThresh > 0.0 && edge_len < kLenThresh);
            bool by_angle   = (angle_thresh > 0.f && !dag.succ[u].empty() && !dag.succ[v].empty() &&
                               std::abs(normals[u].dot(normals[v])) >= kCosThresh);

            if (by_length || by_angle)
                parent[find(u)] = find(v);
        };
        for (int v : dag.succ[u])
            try_merge(v);
        for (int v : dag.pred[u])
            try_merge(v);
    }

    std::unordered_map<int, int> comp_to_new;
    std::vector<Eigen::Vector3d> new_pos;
    std::vector<int>             cnt;
    std::vector<bool>            comp_has_endpoint;
    std::vector<Eigen::Vector3d> comp_endpoint_pos;

    for (int i = 0; i < n; ++i) {
        int r               = find(i);
        auto [it, inserted] = comp_to_new.emplace(r, (int) new_pos.size());
        if (inserted) {
            new_pos.emplace_back(Eigen::Vector3d::Zero());
            cnt.push_back(0);
            comp_has_endpoint.push_back(false);
            comp_endpoint_pos.emplace_back(Eigen::Vector3d::Zero());
        }
        int ni = it->second;
        new_pos[ni] += dag.nodes[i];
        cnt[ni]++;
        // Preserve the exact position of any topological endpoint (leaf or root)
        // so that snap_to_bed's bed-contact position is not diluted by averaging.
        if (dag.succ[i].empty() || dag.pred[i].empty()) {
            comp_has_endpoint[ni] = true;
            comp_endpoint_pos[ni] = dag.nodes[i];
        }
    }
    for (int i = 0; i < (int) new_pos.size(); ++i)
        new_pos[i] = comp_has_endpoint[i] ? comp_endpoint_pos[i] : new_pos[i] / cnt[i];

    SkeletonGraph G;
    for (auto& p : new_pos)
        G.add_node(p);

    std::set<std::pair<int, int>> seen;
    for (int u = 0; u < n; ++u) {
        int nu = comp_to_new[find(u)];
        for (int v : dag.succ[u]) {
            int nv = comp_to_new[find(v)];
            if (nu == nv)
                continue;
            auto e = std::make_pair(std::min(nu, nv), std::max(nu, nv));
            if (seen.insert(e).second)
                G.add_edge(nu, nv);
        }
    }

    std::cout << "[collapse_skeleton] " << n << " → " << G.num_nodes()
              << " nodes  (angle_thresh=" << angle_thresh << "°  edge_len_thresh=" << edge_len_thresh << "mm)\n";
    return G;
}

SkeletonGraph collapse_skeleton_to_spine(const SkeletonGraph& G, int smooth_iterations, float smooth_alpha,
                                         const Eigen::MatrixXd* V, const Eigen::MatrixXi* F)
{
    int n = G.num_nodes();
    if (n == 0 || n == 1)
        return G;

    // Source = lowest-Z node, destination = highest-Z node.
    int src = 0, dst = 0;
    for (int i = 1; i < n; ++i) {
        if (G.nodes[i].z() < G.nodes[src].z()) src = i;
        if (G.nodes[i].z() > G.nodes[dst].z()) dst = i;
    }
    if (src == dst)
        return G;

    // Dijkstra from src for shortest (by edge length) path to dst.
    // DFS would find the first path, not the shortest; the skeleton has cycles
    // so multiple paths exist and the shortest one is not guaranteed by DFS.
    using PD = std::pair<double, int>;
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    std::vector<int>    prev(n, -1);
    std::priority_queue<PD, std::vector<PD>, std::greater<PD>> pq;
    dist[src] = 0.0;
    pq.push({0.0, src});
    while (!pq.empty()) {
        auto [d, u] = pq.top(); pq.pop();
        if (d > dist[u]) continue;
        for (int v : G.adj[u]) {
            double nd = d + (G.nodes[v] - G.nodes[u]).norm();
            if (nd < dist[v]) {
                dist[v] = nd;
                prev[v] = u;
                pq.push({nd, v});
            }
        }
    }

    if (dist[dst] == std::numeric_limits<double>::infinity())
        return G;

    // Reconstruct path by walking prev[] from dst back to src.
    std::vector<int> path;
    for (int cur = dst; cur != -1; cur = prev[cur])
        path.push_back(cur);
    std::reverse(path.begin(), path.end());

    if (path.empty() || path.front() != src)
        return G;

    // Build spine chain from the path.
    SkeletonGraph spine;
    for (int idx : path)
        spine.add_node(G.nodes[idx]);
    for (int i = 0; i + 1 < (int) path.size(); ++i)
        spine.add_edge(i, i + 1);

    // Laplacian smoothing on the spine; endpoints are fixed.
    //
    // ... but NOT when the source was a loop. smooth_skeleton_graph protects cyclic components
    // with Taubin's no-net-shrink pass, keyed on |E| >= |V| within the component. Extracting a
    // spine turns a cycle into a PATH, which drops out of that branch and gets plain Laplacian
    // instead -- so a ring loses exactly the protection its cycle had, and the arc chords inward.
    // The containment guard does not catch it either: outer_loop_at only tests depth-0 loops, so
    // a node that has moved into the bore still reads as "inside" (deliberately, so a traced bore
    // axis counts as inside). Measured on a 4 mm-wall ring, r 18..22: the spine reached r 17.45
    // with 53.4% of its length in the bore; on the OD50/ID30 corpus washer, r 13.08 and 82.9%.
    // Cut planes sit at edge MIDPOINTS, so that is a run of cuts centred on nothing.
    //
    // A loop's MCF spine is already a clean arc and does not need smoothing, and resample runs
    // right after this in build_skeleton() either way. Acyclic sources keep today's behaviour.
    const bool source_has_cycle = [&G]() {
        size_t half_edges = 0, live_nodes = 0;
        for (int i = 0; i < G.num_nodes(); ++i) {
            half_edges += G.adj[i].size();
            if (!G.adj[i].empty())
                ++live_nodes;
        }
        // Whole-graph rather than per-component, which is the conservative direction: a tree that
        // shares a graph with a loop skips smoothing rather than being shrunk through a wall.
        return live_nodes > 0 && half_edges / 2 >= live_nodes;
    }();

    if (!source_has_cycle)
        smooth_skeleton_graph(spine, smooth_iterations, smooth_alpha, /*fix_endpoints=*/true, V, F);

    std::cout << "[collapse_to_spine] min-Z→max-Z DFS path: " << path.size() << " nodes"
              << (source_has_cycle ? " (cyclic source: smoothing skipped)" : "") << "\n";
    return spine;
}

SkeletonGraph resample_skeleton_graph(const SkeletonGraph& G, double max_edge_len)
{
    int n = G.num_nodes();
    if (n == 0 || max_edge_len <= 0.0)
        return G;

    SkeletonGraph R;
    // Copy original nodes at the same indices so adj references stay valid.
    std::vector<int> old_to_new(n);
    for (int i = 0; i < n; ++i) {
        old_to_new[i] = R.num_nodes();
        R.add_node(G.nodes[i]);
    }

    // Process each undirected edge once (u < v).
    for (int u = 0; u < n; ++u) {
        for (int v : G.adj[u]) {
            if (v <= u)
                continue;
            const double len  = (G.nodes[v] - G.nodes[u]).norm();
            const int    segs = std::max(1, (int) std::ceil(len / max_edge_len));
            int prev          = old_to_new[u];
            for (int s = 1; s < segs; ++s) {
                const double t = (double) s / (double) segs;
                const int    cur = R.num_nodes();
                R.add_node((1.0 - t) * G.nodes[u] + t * G.nodes[v]);
                R.add_edge(prev, cur);
                prev = cur;
            }
            R.add_edge(prev, old_to_new[v]);
        }
    }

    std::cout << "[resample_skeleton] " << n << " → " << R.num_nodes()
              << " nodes  (max_edge_len=" << max_edge_len << "mm)\n";
    return R;
}

SkeletonGraph resample_skeleton_uniform(const SkeletonGraph& G, double interval)
{
    int n = G.num_nodes();
    if (n == 0 || interval <= 0.0)
        return G;

    // Anchor nodes are tips (degree 1) and junctions (degree >= 3).
    // Degree-2 nodes are interior points on branches and will be replaced.
    //
    // A component of nothing but degree-2 nodes — a closed loop, which is precisely the MCF
    // skeleton of a torus, a washer or a windowed plate — has no anchor at all, so the branch
    // tracing below never starts on it and the whole component used to be dropped without a
    // word. (For a shape whose skeleton is a single loop that meant an EMPTY graph: zero
    // nodes, zero cuts.) One node per such component is promoted to a pseudo-anchor, which
    // both starts the trace and terminates it once it comes back around.
    std::vector<char> pseudo(n, 0);
    {
        std::vector<char> seen(n, 0);
        for (int s = 0; s < n; ++s) {
            if (seen[s] || G.adj[s].empty())
                continue;
            std::vector<int> comp;
            std::queue<int>  q;
            q.push(s);
            seen[s] = 1;
            bool has_anchor = false;
            while (!q.empty()) {
                const int u = q.front();
                q.pop();
                comp.push_back(u);
                if (G.degree(u) != 2)
                    has_anchor = true;
                for (int v : G.adj[u])
                    if (!seen[v]) { seen[v] = 1; q.push(v); }
            }
            if (!has_anchor && !comp.empty())
                pseudo[*std::min_element(comp.begin(), comp.end())] = 1;
        }
    }
    auto is_anchor = [&](int i) { return G.degree(i) != 2 || pseudo[i]; };

    SkeletonGraph R;
    std::vector<int> old_to_new(n, -1);
    for (int i = 0; i < n; ++i) {
        if (is_anchor(i)) {
            old_to_new[i] = R.num_nodes();
            R.add_node(G.nodes[i]);
        }
    }

    // Trace each branch: a chain from one anchor through degree-2 interior nodes to another anchor.
    // Process each undirected edge once.
    std::set<std::pair<int, int>> done_edges;
    auto edge_key = [](int a, int b) { return std::make_pair(std::min(a, b), std::max(a, b)); };

    auto sample_chain = [&](const std::vector<Eigen::Vector3d>& chain,
                            const std::vector<double>& arc,
                            double d) -> Eigen::Vector3d {
        int m = (int) chain.size();
        auto it  = std::lower_bound(arc.begin(), arc.end(), d);
        int  idx = (int) (it - arc.begin());
        if (idx >= m) idx = m - 1;
        if (idx == 0) return chain[0];
        double span = arc[idx] - arc[idx - 1];
        double t    = (span > 1e-12) ? (d - arc[idx - 1]) / span : 0.0;
        return (1.0 - t) * chain[idx - 1] + t * chain[idx];
    };

    for (int start = 0; start < n; ++start) {
        if (!is_anchor(start))
            continue;
        for (int nb : G.adj[start]) {
            if (done_edges.count(edge_key(start, nb)))
                continue;

            // Collect chain: start -> [interior degree-2 nodes] -> end_anchor.
            std::vector<Eigen::Vector3d> chain;
            chain.push_back(G.nodes[start]);

            int prev = start;
            int cur  = nb;
            done_edges.insert(edge_key(prev, cur));

            while (!is_anchor(cur)) {
                chain.push_back(G.nodes[cur]);
                int next = -1;
                for (int v : G.adj[cur]) {
                    if (v != prev) { next = v; break; }
                }
                if (next == -1) { cur = -1; break; }
                done_edges.insert(edge_key(cur, next));
                prev = cur;
                cur  = next;
            }
            if (cur == -1)
                continue;
            chain.push_back(G.nodes[cur]);

            int end_anchor = cur;

            // Compute cumulative arc lengths.
            int m = (int) chain.size();
            std::vector<double> arc(m, 0.0);
            for (int i = 1; i < m; ++i)
                arc[i] = arc[i - 1] + (chain[i] - chain[i - 1]).norm();
            double total = arc.back();

            if (total < 1e-12) {
                R.add_edge(old_to_new[start], old_to_new[end_anchor]);
                continue;
            }

            // Resample interior at uniform spacing; always preserve both endpoints.
            int n_segs  = std::max(1, (int) std::ceil(total / interval));
            double step = total / n_segs;

            int prev_new = old_to_new[start];
            for (int s = 1; s < n_segs; ++s) {
                Eigen::Vector3d pt  = sample_chain(chain, arc, s * step);
                int             cur_new = R.num_nodes();
                R.add_node(pt);
                R.add_edge(prev_new, cur_new);
                prev_new = cur_new;
            }
            R.add_edge(prev_new, old_to_new[end_anchor]);
        }
    }

    std::cout << "[resample_skeleton_uniform] " << n << " → " << R.num_nodes()
              << " nodes  (interval=" << interval << "mm)\n";
    return R;
}

Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::RowMajor>> trianglemesh_to_eigen_vertices(const indexed_triangle_set& its)
{ return {its.vertices[0].data(), (Eigen::Index) its.vertices.size(), 3}; }

Eigen::Map<const Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::RowMajor>> trianglemesh_to_eigen_faces(const indexed_triangle_set& its)
{ return {its.indices[0].data(), (Eigen::Index) its.indices.size(), 3}; }

// ── Multi-source Dijkstra ─────────────────────────────────────────────────────

// Like dijkstra() but initialises dist=0 for every node in `sources` simultaneously.
static std::vector<double> multi_source_dijkstra(const SkeletonGraph& G, const std::vector<int>& sources)
{
    int n = G.num_nodes();
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());

    using PD = std::pair<double, int>;
    std::priority_queue<PD, std::vector<PD>, std::greater<PD>> pq;

    for (int s : sources) {
        dist[s] = 0.0;
        pq.push({0.0, s});
    }

    while (!pq.empty()) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u])
            continue;
        for (int v : G.adj[u]) {
            double w  = (G.nodes[u] - G.nodes[v]).norm();
            double nd = d + w;
            if (nd < dist[v]) {
                dist[v] = nd;
                pq.push({nd, v});
            }
        }
    }
    return dist;
}

// ── find_skeleton_roots ───────────────────────────────────────────────────────

std::vector<int> find_skeleton_roots(const SkeletonGraph& G, double bottom_pct)
{
    int n = G.num_nodes();
    if (n == 0)
        return {};

    double z_min = std::numeric_limits<double>::infinity();
    double z_max = -std::numeric_limits<double>::infinity();
    for (const auto& p : G.nodes) {
        z_min = std::min(z_min, p.z());
        z_max = std::max(z_max, p.z());
    }

    double z_thresh = z_min + bottom_pct * (z_max - z_min);

    std::vector<int> roots;

    // A planted foot is THE root: only the feet and any tip lying on the bed itself stand on it.
    for (int i = 0; i < n; ++i)
        if (skeleton_foot_top(G.nodes, G.adj, i) >= 0)
            roots.push_back(i);
    if (!roots.empty()) {
        const double on_bed = z_min + std::max(0.05, 1e-3 * (z_max - z_min));
        for (int i = 0; i < n; ++i)
            if (G.degree(i) == 1 && G.nodes[i].z() <= on_bed &&
                std::find(roots.begin(), roots.end(), i) == roots.end())
                roots.push_back(i);
        std::cout << "[find_skeleton_roots] " << roots.size() << " root(s): planted foot under the middle.\n";
        return roots;
    }
    for (int i = 0; i < n; ++i)
        if (G.degree(i) == 1 && G.nodes[i].z() <= z_thresh)
            roots.push_back(i);

    if (roots.empty()) {
        // Fallback: return the single lowest-Z node.
        int lowest = 0;
        for (int i = 1; i < n; ++i)
            if (G.nodes[i].z() < G.nodes[lowest].z())
                lowest = i;
        roots.push_back(lowest);
        std::cout << "[find_skeleton_roots] No degree-1 nodes in bottom " << bottom_pct * 100.0
                  << "% — falling back to lowest-Z node " << lowest << ".\n";
    } else {
        std::cout << "[find_skeleton_roots] Found " << roots.size() << " root(s) in bottom "
                  << bottom_pct * 100.0 << "% of Z range.\n";
    }

    return roots;
}

// ── orient_skeleton_as_dag (multi-root overload) ──────────────────────────────

SkeletonDAG orient_skeleton_as_dag(const SkeletonGraph& G, const std::vector<int>& roots)
{
    if (roots.empty())
        return orient_skeleton_as_dag(G, find_graph_center(G));

    int n = G.num_nodes();

    SkeletonDAG dag;
    dag.nodes = G.nodes;
    dag.succ.resize(n);
    dag.pred.resize(n);
    dag.dist.assign(n, std::numeric_limits<double>::infinity());
    if (n == 0)
        return dag;

    dag.root = roots[0];

    // Multi-source Dijkstra from all roots using physical 3D edge lengths.
    // Dijkstra's shortest-path tree is acyclic by construction; consistent
    // distance ordering then orients ALL original edges (not just tree edges),
    // avoiding the conflicting directions that BFS hop-count ties can cause.
    using PairDI = std::pair<double, int>;
    std::priority_queue<PairDI, std::vector<PairDI>, std::greater<PairDI>> pq;

    for (int r : roots) {
        dag.dist[r] = 0.0;
        pq.push({0.0, r});
    }

    while (!pq.empty()) {
        auto [d, u] = pq.top(); pq.pop();
        if (d > dag.dist[u])
            continue;
        for (int v : G.adj[u]) {
            double w  = (G.nodes[v] - G.nodes[u]).norm();
            double nd = d + w;
            if (nd < dag.dist[v]) {
                dag.dist[v] = nd;
                pq.push({nd, v});
            }
        }
    }

    // Orient every undirected edge: smaller dist → larger dist; tie-break by index.
    for (int u = 0; u < n; ++u) {
        for (int v : G.adj[u]) {
            if (u >= v) continue; // process each edge once
            if (dag.dist[u] < dag.dist[v] || (dag.dist[u] == dag.dist[v] && u < v))
                dag.add_edge(u, v);
            else
                dag.add_edge(v, u);
        }
    }

    {
        int n_edges = 0;
        for (int i = 0; i < n; ++i) n_edges += (int)dag.succ[i].size();
        int n_sources = 0, n_leaves = 0, n_multi_p = 0;
        for (int i = 0; i < n; ++i) {
            if (dag.pred[i].empty()) ++n_sources;
            if (dag.succ[i].empty()) ++n_leaves;
            if ((int)dag.pred[i].size() > 1) ++n_multi_p;
        }
        std::cout << "\n=== SkeletonDAG diagnostic (multi-root Dijkstra) ===\n"
                  << "  nodes     : " << n << "\n"
                  << "  edges     : " << n_edges << "\n"
                  << "  root(s)   : " << roots.size() << " (primary=" << dag.root << ")\n"
                  << "  sources   : " << n_sources << "\n"
                  << "  leaves    : " << n_leaves << "\n"
                  << "  multi-par : " << n_multi_p << "\n"
                  << "=== end DAG diagnostic ===\n\n";
    }

    return dag;
}

// ── find_convergence_nodes ────────────────────────────────────────────────────

// A convergence node is any node with DAG in-degree >= 2: multiple branches
// from different parents arrive here, requiring a bisector cut plane.
std::vector<int> find_convergence_nodes(const SkeletonDAG& dag)
{
    std::vector<int> result;
    int n = dag.num_nodes();
    for (int i = 0; i < n; ++i)
        if ((int)dag.pred[i].size() >= 2)
            result.push_back(i);
    std::cout << "[find_convergence_nodes] Found " << result.size() << " convergence node(s).\n";
    return result;
}

// ── collapse_base_region ──────────────────────────────────────────────────────

SkeletonGraph collapse_base_region(const SkeletonGraph& G, double angle_threshold_deg)
{
    int n = G.num_nodes();
    if (n == 0)
        return G;

    const double threshold_rad = angle_threshold_deg * M_PI / 180.0;

    // 1. Classify every undirected edge as flat.
    auto is_flat = [&](int u, int v) -> bool {
        const auto& pu = G.nodes[u];
        const auto& pv = G.nodes[v];
        double len = (pu - pv).norm();
        if (len < 1e-12)
            return true; // zero-length edge treated as flat
        double dz    = std::abs(pu.z() - pv.z());
        double angle = std::asin(std::min(1.0, dz / len));
        return angle < threshold_rad;
    };

    // 2. Union-Find on nodes touched by flat edges.
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) -> int {
        return parent[x] == x ? x : parent[x] = find(parent[x]);
    };

    for (int u = 0; u < n; ++u) {
        for (int v : G.adj[u]) {
            if (v > u && is_flat(u, v))
                parent[find(u)] = find(v);
        }
    }

    // Group nodes by component root.
    std::unordered_map<int, std::vector<int>> comp_members;
    for (int i = 0; i < n; ++i)
        comp_members[find(i)].push_back(i);

    // 3. For each component with >=2 nodes: find boundary nodes.
    // Boundary = members with at least one non-flat edge to a node outside the component.
    // Build: comp_root -> representative position and new node index.
    std::unordered_map<int, int>            comp_to_new;   // comp_root -> new node idx
    std::unordered_map<int, Eigen::Vector3d> comp_pos;     // comp_root -> position
    std::vector<Eigen::Vector3d>             new_nodes;
    std::vector<int>                         old_to_new(n, -1);

    for (auto& [comp_root, members] : comp_members) {
        if ((int) members.size() < 2) {
            // Singleton: keep as-is.
            int ni = (int) new_nodes.size();
            comp_to_new[comp_root] = ni;
            old_to_new[members[0]] = ni;
            new_nodes.push_back(G.nodes[members[0]]);
            continue;
        }

        // Find boundary nodes.
        std::set<int> member_set(members.begin(), members.end());
        std::vector<int> boundary;
        for (int m : members) {
            for (int nb : G.adj[m]) {
                if (!member_set.count(nb) && !is_flat(m, nb)) {
                    boundary.push_back(m);
                    break;
                }
            }
        }
        if (boundary.empty())
            boundary = members; // fallback: use all

        // Representative = centroid of boundary nodes.
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int b : boundary)
            centroid += G.nodes[b];
        centroid /= (double) boundary.size();

        int ni = (int) new_nodes.size();
        comp_to_new[comp_root] = ni;
        for (int m : members)
            old_to_new[m] = ni;
        new_nodes.push_back(centroid);
    }

    // 4. Build new SkeletonGraph.
    SkeletonGraph R;
    for (const auto& p : new_nodes)
        R.add_node(p);

    std::set<std::pair<int,int>> seen;
    for (int u = 0; u < n; ++u) {
        int nu = old_to_new[u];
        for (int v : G.adj[u]) {
            if (v <= u) continue;
            int nv = old_to_new[v];
            if (nu == nv) continue;
            auto e = std::make_pair(std::min(nu, nv), std::max(nu, nv));
            if (seen.insert(e).second)
                R.add_edge(nu, nv);
        }
    }

    std::cout << "[collapse_base_region] " << n << " → " << R.num_nodes()
              << " nodes  (angle_threshold=" << angle_threshold_deg << "°)\n";
    return R;
}

// ── collapse_nearby_junctions ─────────────────────────────────────────────────

SkeletonGraph collapse_nearby_junctions(const SkeletonGraph& G)
{
    int n = G.num_nodes();
    if (n == 0)
        return G;

    // 1. Find all junction nodes (degree >= 3).
    std::vector<bool> is_junction(n, false);
    for (int i = 0; i < n; ++i)
        if (G.degree(i) >= 3)
            is_junction[i] = true;

    // 2. Union-Find on junctions connected by direct edges.
    std::vector<int> parent(n);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) -> int {
        return parent[x] == x ? x : parent[x] = find(parent[x]);
    };

    for (int u = 0; u < n; ++u) {
        if (!is_junction[u]) continue;
        for (int v : G.adj[u]) {
            if (v > u && is_junction[v])
                parent[find(u)] = find(v);
        }
    }

    // 3. Group junctions by cluster.
    std::unordered_map<int, std::vector<int>> clusters;
    for (int i = 0; i < n; ++i)
        if (is_junction[i])
            clusters[find(i)].push_back(i);

    // Build merge_map: eliminated junction -> representative junction.
    std::unordered_map<int, int> merge_map; // old node -> representative old node
    for (int i = 0; i < n; ++i)
        merge_map[i] = i; // identity by default

    std::unordered_map<int, Eigen::Vector3d> rep_pos; // representative old node -> new position

    int n_collapsed = 0;
    for (auto& [cluster_root, members] : clusters) {
        if ((int) members.size() < 2) continue;

        // Representative = highest-degree member (tiebreak: smallest index).
        int rep = members[0];
        for (int m : members)
            if (G.degree(m) > G.degree(rep) || (G.degree(m) == G.degree(rep) && m < rep))
                rep = m;

        // Centroid position.
        Eigen::Vector3d centroid = Eigen::Vector3d::Zero();
        for (int m : members)
            centroid += G.nodes[m];
        centroid /= (double) members.size();
        rep_pos[rep] = centroid;

        for (int m : members) {
            merge_map[m] = rep;
            if (m != rep) ++n_collapsed;
        }
    }

    // 4. Build new graph: assign new indices, skipping eliminated nodes.
    std::vector<int> old_to_new(n, -1);
    std::vector<Eigen::Vector3d> new_nodes;

    for (int i = 0; i < n; ++i) {
        int rep = merge_map[i];
        if (rep != i) continue; // eliminated
        int ni = (int) new_nodes.size();
        old_to_new[i] = ni;
        if (rep_pos.count(i))
            new_nodes.push_back(rep_pos[i]);
        else
            new_nodes.push_back(G.nodes[i]);
    }

    SkeletonGraph R;
    for (const auto& p : new_nodes)
        R.add_node(p);

    std::set<std::pair<int,int>> seen;
    for (int u = 0; u < n; ++u) {
        int nu = old_to_new[merge_map[u]];
        for (int v : G.adj[u]) {
            if (v < u) continue;
            int nv = old_to_new[merge_map[v]];
            if (nu == nv) continue;
            auto e = std::make_pair(std::min(nu, nv), std::max(nu, nv));
            if (seen.insert(e).second)
                R.add_edge(nu, nv);
        }
    }

    std::cout << "[collapse_nearby_junctions] Collapsed " << n_collapsed
              << " junction node(s). " << n << " → " << R.num_nodes() << " nodes.\n";
    return R;
}

// ── prune_short_leaves ────────────────────────────────────────────────────────

SkeletonGraph prune_short_leaves(const SkeletonGraph& G, double min_len)
{
    int n = G.num_nodes();
    std::vector<bool> removed(n, false);

    bool any = true;
    while (any) {
        any = false;
        for (int i = 0; i < n; ++i) {
            if (removed[i]) continue;
            int eff_deg = 0, nb = -1;
            for (int v : G.adj[i])
                if (!removed[v]) { ++eff_deg; nb = v; }
            if (eff_deg == 1 && nb >= 0 && (G.nodes[i] - G.nodes[nb]).norm() < min_len) {
                removed[i] = true;
                any = true;
            }
        }
    }

    std::vector<int> old_to_new(n, -1);
    SkeletonGraph R;
    for (int i = 0; i < n; ++i) {
        if (!removed[i]) {
            old_to_new[i] = R.num_nodes();
            R.add_node(G.nodes[i]);
        }
    }
    for (int u = 0; u < n; ++u) {
        if (removed[u]) continue;
        for (int v : G.adj[u]) {
            if (v > u && !removed[v])
                R.add_edge(old_to_new[u], old_to_new[v]);
        }
    }

    int n_pruned = 0;
    for (bool r : removed) if (r) ++n_pruned;
    if (n_pruned > 0)
        printf("[prune_short_leaves] Removed %d leaf node(s). %d → %d\n", n_pruned, n, R.num_nodes());
    return R;
}

// ── compute_skeleton_tangents ─────────────────────────────────────────────────

SkeletonTangents compute_skeleton_tangents(
    const SkeletonGraph& G,
    const SkeletonDAG&   dag,
    const std::vector<int>& roots,
    const std::vector<int>& convergence_nodes,
    double W,
    double root_lean_deg,
    double root_ramp)
{
    int n = G.num_nodes();
    SkeletonTangents result;
    if (n == 0)
        return result;

    // ── Build indexed edge list (u < v) ──────────────────────────────────────
    for (int u = 0; u < n; ++u) {
        for (int v : G.adj[u]) {
            if (v <= u) continue;
            int ei = (int) result.edges.size();
            result.edges.emplace_back(u, v);
            result.edge_index[{u, v}] = ei;
        }
    }
    int n_edges = (int) result.edges.size();
    auto& edge_index = result.edge_index;

    // ── Compute raw DAG-directed tangents ────────────────────────────────────
    // Build succ set for fast lookup.
    std::vector<std::set<int>> succ_set(n);
    for (int u = 0; u < n; ++u)
        for (int s : dag.succ[u])
            succ_set[u].insert(s);

    result.raw.resize(n_edges);
    for (int ei = 0; ei < n_edges; ++ei) {
        auto [u, v] = result.edges[ei];
        Eigen::Vector3d dir;
        if (succ_set[u].count(v))
            dir = G.nodes[v] - G.nodes[u]; // u->v is DAG forward
        else
            dir = G.nodes[u] - G.nodes[v]; // v->u is DAG forward; edge is u<v stored, tangent points from v to u
        double len = dir.norm();
        result.raw[ei] = (len > 1e-12) ? Eigen::Vector3d(dir / len) : Eigen::Vector3d(0, 0, 1);
    }

    result.smoothed = result.raw;

    // ── Anchor sets ──────────────────────────────────────────────────────────
    std::set<int> root_set(roots.begin(), roots.end());
    std::set<int> conv_set(convergence_nodes.begin(), convergence_nodes.end());

    std::vector<int> tips, junctions;
    for (int i = 0; i < n; ++i) {
        int deg = G.degree(i);
        if (deg == 1 && !root_set.count(i))
            tips.push_back(i);
        else if (deg >= 3)
            junctions.push_back(i);
    }

    std::set<int> branch_stops;
    for (int r : roots)       branch_stops.insert(r);
    for (int t : tips)        branch_stops.insert(t);
    for (int c : conv_set)    branch_stops.insert(c);
    for (int j : junctions)   branch_stops.insert(j);

    // ── Branch finding ────────────────────────────────────────────────────────
    // For each anchor, walk along each neighbour direction through degree-2
    // interior nodes until hitting another anchor.  Each branch is a vector<int>
    // of node indices from one anchor to the other (inclusive).
    struct Branch {
        std::vector<int> path;
        int a, b;
        bool a_conv, b_conv;
        Eigen::Vector3d t_a, t_b; // anchor tangents
    };

    auto get_edge_index = [&](int u, int v) -> int {
        auto key = std::make_pair(std::min(u,v), std::max(u,v));
        auto it  = edge_index.find(key);
        return (it != edge_index.end()) ? it->second : -1;
    };

    // Anchor tangent for non-convergence anchors.
    // Conv nodes are handled separately via conv_bisectors computed below.
    auto anchor_tangent = [&](int a, int next_node) -> Eigen::Vector3d {
        if (root_set.count(a))
            return Eigen::Vector3d::UnitZ();
        Eigen::Vector3d d = G.nodes[next_node] - G.nodes[a];
        double len = d.norm();
        return (len > 1e-12) ? Eigen::Vector3d(d / len) : Eigen::Vector3d(0, 0, 1);
    };

    // ── Pass 1: collect convergence branches (no smoothing yet) ──────────────
    // Exit filter uses DAG dist comparison — robust for horizontal branches where
    // Z-height is unreliable and pred_set can be inconsistent with the BFS-based
    // convergence detection (paths of different physical length reach the same hop
    // count convergence node, so Dijkstra may orient one edge "backwards").
    std::set<std::pair<int,int>> visited_edges;
    std::vector<Branch> conv_branches;

    for (int start : branch_stops) {
        for (int nb : G.adj[start]) {
            auto ek = std::make_pair(std::min(start, nb), std::max(start, nb));
            if (visited_edges.count(ek)) continue;

            std::vector<int> path;
            path.push_back(start);

            int prev = start;
            int cur  = nb;
            visited_edges.insert(ek);

            while (!branch_stops.count(cur) && G.degree(cur) == 2) {
                path.push_back(cur);
                int next = -1;
                for (int v : G.adj[cur]) {
                    if (v != prev) { next = v; break; }
                }
                if (next == -1) { cur = -1; break; }
                auto ekey = std::make_pair(std::min(cur, next), std::max(cur, next));
                visited_edges.insert(ekey);
                prev = cur;
                cur  = next;
            }
            if (cur == -1) continue;
            path.push_back(cur);

            int a = path.front();
            int b = path.back();
            bool a_conv = conv_set.count(a) > 0;
            bool b_conv = conv_set.count(b) > 0;

            if (!a_conv && !b_conv) continue;

            // Exit filter: a branch is an "exit" if its non-convergence endpoint is
            // farther from the roots (higher DAG dist) than the convergence endpoint.
            if (a_conv && !b_conv && dag.dist[b] > dag.dist[a]) continue;
            if (b_conv && !a_conv && dag.dist[a] > dag.dist[b]) continue;

            conv_branches.push_back({path, a, b, a_conv, b_conv, {}, {}});
        }
    }

    // ── Pass 2: compute per-convergence-node bisectors ───────────────────────
    // Sum contributions from every walk-confirmed incoming branch, which avoids
    // the DAG topology ambiguity entirely (we know each collected branch is incoming).
    // Flip if the result points downward (negative Z) — that would be unprintable.
    std::map<int, Eigen::Vector3d> conv_bisectors;
    for (const Branch& br : conv_branches) {
        int np = (int)br.path.size();
        auto add = [&](int c, int step) {
            Eigen::Vector3d d = G.nodes[c] - G.nodes[step];
            double len = d.norm();
            if (len > 1e-12) conv_bisectors[c] += d / len;
        };
        if (br.a_conv) add(br.a, np > 1 ? br.path[1]      : br.b);
        if (br.b_conv) add(br.b, np > 1 ? br.path[np - 2] : br.a);
    }
    for (auto& [c, sum] : conv_bisectors) {
        double slen = sum.norm();
        Eigen::Vector3d bis = (slen > 1e-12) ? Eigen::Vector3d(sum / slen) : Eigen::Vector3d(0, 0, 1);
        if (bis.z() < 0.0) bis = -bis; // flip downward bisectors — unprintable
        sum = bis;
    }

    // ── Pass 3: smooth each convergence branch toward its bisector ───────────
    for (Branch& br : conv_branches) {
        int np    = (int)br.path.size();
        int n_seg = np - 1;

        Eigen::Vector3d ta = br.a_conv ? conv_bisectors[br.a]
                                       : anchor_tangent(br.a, np > 1 ? br.path[1]      : br.b);
        Eigen::Vector3d tb = br.b_conv ? conv_bisectors[br.b]
                                       : anchor_tangent(br.b, np > 1 ? br.path[np - 2] : br.a);
        br.t_a = ta;
        br.t_b = tb;

        for (int i = 0; i < n_seg; ++i) {
            int ei = get_edge_index(br.path[i], br.path[i+1]);
            if (ei < 0) continue;
            bool conv_adj = (br.a_conv && i == 0) || (br.b_conv && i == n_seg - 1);
            if (conv_adj) {
                result.smoothed[ei] = br.a_conv ? ta : tb;
            } else {
                double frac = (n_seg > 1) ? (double)i / (double)(n_seg - 1) : 0.5;
                Eigen::Vector3d target  = (1.0 - frac) * ta + frac * tb;
                Eigen::Vector3d blended = (1.0 - W) * result.raw[ei] + W * target;
                double blen = blended.norm();
                result.smoothed[ei] = (blen > 1e-12) ? blended / blen : result.raw[ei];
            }
        }

        // Debug output.
        const Eigen::Vector3d& bisector = br.a_conv ? ta : tb;
        printf("[conv_branch] %s%d -> %s%d  (%d edges)  bisector=[%.3f,%.3f,%.3f]\n",
            br.a_conv ? "C" : "", br.a, br.b_conv ? "C" : "", br.b, n_seg,
            bisector.x(), bisector.y(), bisector.z());
        printf("  seg  nodes       raw                      smoothed\n");
        for (int i = 0; i < n_seg; ++i) {
            int ei = get_edge_index(br.path[i], br.path[i+1]);
            if (ei < 0) continue;
            const Eigen::Vector3d& r = result.raw[ei];
            const Eigen::Vector3d& s = result.smoothed[ei];
            bool conv_adj = (br.a_conv && i == 0) || (br.b_conv && i == n_seg - 1);
            printf("  [%2d] %3d->%3d   [%6.3f %6.3f %6.3f]  [%6.3f %6.3f %6.3f]%s\n",
                i, br.path[i], br.path[i+1],
                r.x(), r.y(), r.z(), s.x(), s.y(), s.z(),
                conv_adj ? "  <-- pinned" : "");
        }
    }

    // (Root-adjacent edges used to be forced to UnitZ here and every other edge left alone, so the
    // first plane lay flat and the next snapped straight to the spine: 0 -> 71 -> 39 -> 91 deg of
    // tilt out of the root of Intake_manifold. The lean toward +Z is now a ramp, applied last,
    // below.)

    // ── Rodrigues rotation helper ─────────────────────────────────────────────
    auto rodrigues = [](const Eigen::Vector3d& v, const Eigen::Vector3d& k, double theta) -> Eigen::Vector3d {
        return v * std::cos(theta) + k.cross(v) * std::sin(theta) + k * (k.dot(v)) * (1.0 - std::cos(theta));
    };

    // ── Clearance check at each convergence node ──────────────────────────────
    std::set<int> adjusted_endpoints;
    int n_violations = 0;
    int n_adjustments = 0;

    for (int c : convergence_nodes) {
        // Collect all adjacent edge indices for the clearance check.
        std::vector<int> adj_edges;
        for (int nb : G.adj[c]) {
            int ei = get_edge_index(c, nb);
            if (ei >= 0)
                adj_edges.push_back(ei);
        }
        if ((int) adj_edges.size() < 2) continue;

        // Local copy of tangents for cascade updates within same node.
        std::vector<Eigen::Vector3d> local_t;
        local_t.reserve(adj_edges.size());
        for (int ei : adj_edges)
            local_t.push_back(result.smoothed[ei]);

        for (int i = 0; i < (int) adj_edges.size(); ++i) {
            for (int j = i + 1; j < (int) adj_edges.size(); ++j) {
                double dot = local_t[i].dot(local_t[j]);
                dot = std::max(-1.0, std::min(1.0, dot));
                if (dot >= 0.0) continue; // no violation

                ++n_violations;
                double alpha = std::acos(dot);          // angle between tangents
                double delta = (alpha - M_PI / 2.0) / 2.0; // half the excess over 90°

                Eigen::Vector3d axis = local_t[i].cross(local_t[j]);
                double axis_len = axis.norm();
                if (axis_len < 1e-8) {
                    axis = local_t[i].cross(Eigen::Vector3d::UnitZ());
                    axis_len = axis.norm();
                    if (axis_len < 1e-8)
                        axis = Eigen::Vector3d::UnitX();
                    else
                        axis /= axis_len;
                } else {
                    axis /= axis_len;
                }

                Eigen::Vector3d ti_new = rodrigues(local_t[i], axis, +delta);
                Eigen::Vector3d tj_new = rodrigues(local_t[j], axis, -delta);

                double ti_len = ti_new.norm();
                double tj_len = tj_new.norm();
                if (ti_len > 1e-12) ti_new /= ti_len;
                if (tj_len > 1e-12) tj_new /= tj_len;

                if ((ti_new - result.smoothed[adj_edges[i]]).norm() > 1e-8) {
                    adjusted_endpoints.insert(adj_edges[i]);
                    ++n_adjustments;
                }
                if ((tj_new - result.smoothed[adj_edges[j]]).norm() > 1e-8) {
                    adjusted_endpoints.insert(adj_edges[j]);
                    ++n_adjustments;
                }

                local_t[i] = ti_new;
                local_t[j] = tj_new;
            }
        }

        // Write adjusted tangents back.
        for (int idx = 0; idx < (int) adj_edges.size(); ++idx)
            result.smoothed[adj_edges[idx]] = local_t[idx];
    }

    // ── Pass 2: re-smooth branches whose convergence endpoint was adjusted ────
    int n_resmoothed = 0;
    for (auto& br : conv_branches) {
        int n_path = (int) br.path.size();
        int n_seg  = n_path - 1;
        if (n_seg == 0) continue;

        // Check if the convergence endpoint edge was adjusted.
        bool a_adj = false, b_adj = false;
        if (br.a_conv) {
            int ei = get_edge_index(br.path[0], br.path[1 < n_path ? 1 : 0]);
            if (ei >= 0 && adjusted_endpoints.count(ei))
                a_adj = true;
        }
        if (br.b_conv) {
            int ei = get_edge_index(br.path[n_path - 2 >= 0 ? n_path - 2 : 0], br.path[n_path - 1]);
            if (ei >= 0 && adjusted_endpoints.count(ei))
                b_adj = true;
        }
        if (!a_adj && !b_adj) continue;

        // Retrieve updated endpoint tangents.
        Eigen::Vector3d ta = br.t_a, tb = br.t_b;
        if (a_adj) {
            int ei = get_edge_index(br.path[0], br.path[1 < n_path ? 1 : 0]);
            if (ei >= 0) ta = result.smoothed[ei];
        }
        if (b_adj) {
            int ei = get_edge_index(br.path[n_path - 2 >= 0 ? n_path - 2 : 0], br.path[n_path - 1]);
            if (ei >= 0) tb = result.smoothed[ei];
        }

        for (int i = 0; i < n_seg; ++i) {
            int ei = get_edge_index(br.path[i], br.path[i+1]);
            if (ei < 0) continue;
            bool conv_adj = (br.a_conv && i == 0) || (br.b_conv && i == n_seg - 1);
            if (conv_adj) {
                result.smoothed[ei] = br.a_conv ? ta : tb;
            } else {
                double frac = (n_seg > 1) ? (double) i / (double)(n_seg - 1) : 0.5;
                Eigen::Vector3d target = (1.0 - frac) * ta + frac * tb;
                Eigen::Vector3d blended = (1.0 - W) * result.raw[ei] + W * target;
                double blen = blended.norm();
                result.smoothed[ei] = (blen > 1e-12) ? blended / blen : result.raw[ei];
            }
        }
        ++n_resmoothed;
    }

    // ── Lean out of the bed, gradually ────────────────────────────────────────
    // Near a root a normal may tilt at most 90 deg - lean from +Z, the lean fading over the ramp.
    // At a leg's root the lean is 90 deg, so the first plane is the bed plane exactly as before;
    // under a planted foot it is root_lean_deg, held across the seat. A CAP on the tilt, not a
    // fixed turn toward +Z: on a bend the spine already rises at the seat's edge (68 deg tilt
    // 11 mm out on a U of radius 30), and turning that a further 45 deg laid the first planes 23
    // deg off flat -- they met inside the top wall over the middle, which the seat is sized to
    // prevent for planes at 90 - lean. Last, so the convergence smoothing above cannot undo it --
    // except that a root edge running INTO a convergence node keeps its bisector, as the old UnitZ
    // enforcement did.
    int n_leaned = 0;
    {
        const SkeletonRootRamp R = skeleton_root_ramp(G.nodes, G.adj, roots, root_lean_deg, root_ramp);
        for (int ei = 0; ei < n_edges; ++ei) {
            const auto [u, v] = result.edges[ei];
            if (!std::isfinite(R.s[u]) || !std::isfinite(R.s[v]))
                continue;
            const int par = R.s[u] <= R.s[v] ? u : v;
            const int ch  = par == u ? v : u;
            if (R.s[par] == 0.0 && conv_set.count(ch))
                continue;
            // From a leg, measured at the cut's parent node, so the root edge -- parent at distance 0 --
            // stays exactly the bed plane, as it always was. Under a foot, at the cut's own plane, the
            // same measure the seat is skipped by.
            const bool   from_foot = R.foot[par] != 0;
            const double s_eval    = from_foot ? 0.5 * (R.s[u] + R.s[v]) : R.s[par];
            const double lean      = R.lean_deg(s_eval, from_foot, R.seat[par]) * M_PI / 180.0;
            if (lean <= 1e-9)
                continue;
            const Eigen::Vector3d t   = result.smoothed[ei];
            const double          ang = std::acos(std::clamp(t.z(), -1.0, 1.0));
            const double          cap = 0.5 * M_PI - lean; // largest tilt from +Z allowed here
            if (ang <= cap + 1e-9)
                continue;
            Eigen::Vector3d k  = t.cross(Eigen::Vector3d::UnitZ());
            const double    kn = k.norm();
            k = kn > 1e-12 ? Eigen::Vector3d(k / kn) : Eigen::Vector3d::UnitX();
            Eigen::Vector3d nt = rodrigues(t, k, ang - cap);
            const double    nl = nt.norm();
            if (nl > 1e-12) {
                result.smoothed[ei] = nt / nl;
                ++n_leaned;
            }
        }
    }

    std::cout << "[compute_skeleton_tangents] " << n_leaned << " normals leaned toward the bed normal near a root.\n";
    std::cout << "[compute_skeleton_tangents] " << n_edges << " edges  "
              << conv_branches.size() << " convergence branches  "
              << n_violations << " clearance violations  "
              << n_adjustments << " endpoint adjustments  "
              << n_resmoothed << " branches re-smoothed.\n";

    return result;
}

// ── Voxel thinning skeleton (Lee 1994) ───────────────────────────────────────

namespace {

// Euler-number contribution table for 26-connectivity 3D thinning.
// One entry per 2×2×2 sub-cube configuration (256 entries).
// Removing a voxel is Euler-invariant iff the sum over all 8 octants equals 0.
// Source: Homann (2007), adapting Lee, Kashyap & Chu (1994).
static const int8_t kEulerLUT[256] = {
     0,  1,  1,  0,  1,  0, -2, -1,  1, -2,  0, -1,  0, -1, -1,  0,
     1,  0, -2, -1, -2, -1, -1, -2, -1, -2, -3, -2, -1, -2,  0, -1,
     1, -2,  0, -1, -2, -1, -3, -2, -2, -3, -1, -2, -1, -2, -2, -1,
     0, -1, -1, -2, -3, -2, -2, -3, -2, -3, -2, -1,  0, -1, -1,  0,
     1, -2, -2, -3,  0, -1, -1, -2, -2, -3, -3, -2, -1, -2, -2, -1,
    -2, -3, -3, -2, -1, -2,  0, -1, -3, -2, -2, -1, -2, -1, -1,  0,
    -2, -1, -3, -2, -3, -2, -2, -1, -3, -2, -2, -1, -2, -1, -1,  0,
    -1, -2, -2, -1, -2, -1, -1,  0, -2, -1, -1,  0, -1,  0,  0,  1,
     1, -2, -2, -3, -2, -3, -3, -2,  0, -1, -1, -2, -1, -2, -2, -1,
    -2, -3, -3, -2, -3, -2, -2, -1, -1, -2, -2, -1, -2, -1, -1,  0,
    -2, -3, -1, -2, -3, -2, -2, -1, -1, -2,  0, -1, -2, -1, -1,  0,
    -3, -2, -2, -1, -2, -1, -1,  0, -2, -1, -1,  0, -1,  0,  0,  1,
     0, -1, -1, -2, -1, -2, -2, -1, -1, -2, -2, -1, -2, -3, -1, -2,
    -1, -2, -2, -1, -2, -1, -1,  0, -2, -1, -1,  0, -1,  0,  0,  1,
    -1, -2, -2, -1, -2, -1, -1,  0, -2, -1, -1,  0, -1,  0,  0,  1,
     0, -1, -1,  0, -1,  0,  0,  1, -1,  0,  0,  1,  0,  1,  1,  0
};

struct VoxGrid {
    int nx = 0, ny = 0, nz = 0;
    double ox = 0, oy = 0, oz = 0, vs = 1.0;
    std::vector<uint8_t> d;

    void init(int x, int y, int z) {
        nx = x; ny = y; nz = z;
        d.assign((size_t)x * y * z, 0);
    }
    bool valid(int x, int y, int z) const {
        return (unsigned)x < (unsigned)nx &&
               (unsigned)y < (unsigned)ny &&
               (unsigned)z < (unsigned)nz;
    }
    uint8_t  get(int x, int y, int z) const { return valid(x,y,z) ? d[(size_t)z*ny*nx + y*nx + x] : 0; }
    uint8_t& ref(int x, int y, int z)       { return d[(size_t)z*ny*nx + y*nx + x]; }
};

// Sum EulerLUT contributions over all 8 octants with the center voxel absent.
// Returns 0 if removing the voxel is Euler-invariant.
static int euler_char(const VoxGrid& g, int x, int y, int z)
{
    int sum = 0;
    for (int dz : {-1, 1})
    for (int dy : {-1, 1})
    for (int dx : {-1, 1}) {
        int idx = 0;
        // bit 0 = center voxel = 0 (removed); bits 1..7 = the other 7 corners.
        if (g.get(x+dx, y,    z   )) idx |= 2;
        if (g.get(x,    y+dy, z   )) idx |= 4;
        if (g.get(x+dx, y+dy, z   )) idx |= 8;
        if (g.get(x,    y,    z+dz)) idx |= 16;
        if (g.get(x+dx, y,    z+dz)) idx |= 32;
        if (g.get(x,    y+dy, z+dz)) idx |= 64;
        if (g.get(x+dx, y+dy, z+dz)) idx |= 128;
        sum += kEulerLUT[idx];
    }
    return sum;
}

// BFS in the 3×3×3 local neighbourhood (excluding center) to check that all
// solid 26-neighbours form a single 26-connected component when the center is absent.
static bool single_26_component(const VoxGrid& g, int x, int y, int z)
{
    uint8_t local[27] = {};
    int first = -1, fg_count = 0;
    for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        int li = (dz+1)*9 + (dy+1)*3 + (dx+1);
        if (li == 13) continue; // center = absent
        if (g.get(x+dx, y+dy, z+dz)) {
            local[li] = 1;
            if (first < 0) first = li;
            ++fg_count;
        }
    }
    if (fg_count == 0) return false;
    if (fg_count == 1) return true;

    uint8_t vis[27] = {};
    int stk[26]; int top = 0;
    vis[first] = 1; stk[top++] = first;
    int reached = 1;
    while (top > 0) {
        int cur = stk[--top];
        int cz = cur/9, cy = (cur%9)/3, cx = cur%3;
        for (int nz_ = std::max(0,cz-1); nz_ <= std::min(2,cz+1); ++nz_)
        for (int ny_ = std::max(0,cy-1); ny_ <= std::min(2,cy+1); ++ny_)
        for (int nx_ = std::max(0,cx-1); nx_ <= std::min(2,cx+1); ++nx_) {
            int ni = nz_*9 + ny_*3 + nx_;
            if (ni == 13 || vis[ni] || !local[ni]) continue;
            vis[ni] = 1; stk[top++] = ni;
            ++reached;
        }
    }
    return reached == fg_count;
}

static bool is_endpoint(const VoxGrid& g, int x, int y, int z)
{
    int cnt = 0;
    for (int dz = -1; dz <= 1 && cnt < 2; ++dz)
    for (int dy = -1; dy <= 1 && cnt < 2; ++dy)
    for (int dx = -1; dx <= 1 && cnt < 2; ++dx) {
        if ((dx|dy|dz) == 0) continue;
        if (g.get(x+dx, y+dy, z+dz)) ++cnt;
    }
    return cnt < 2;
}

// A voxel is a "simple point" if removing it is both Euler-invariant and
// preserves the 26-connectivity of its solid neighbours.
// Endpoints (< 2 solid 26-neighbours) are never deleted to preserve tips.
static bool is_simple(const VoxGrid& g, int x, int y, int z)
{
    if (is_endpoint(g, x, y, z)) return false;
    return euler_char(g, x, y, z) == 0 && single_26_component(g, x, y, z);
}

// Six border-detection directions for the Lee 1994 sub-iterations.
static const int kThinDirs[6][3] = {
    { 0,-1, 0}, { 0, 1, 0},
    { 0, 0,-1}, { 0, 0, 1},
    { 1, 0, 0}, {-1, 0, 0}
};

static void thin_3d(VoxGrid& g)
{
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& dir : kThinDirs) {
            std::vector<int> to_del;
            for (int z = 1; z < g.nz-1; ++z)
            for (int y = 1; y < g.ny-1; ++y)
            for (int x = 1; x < g.nx-1; ++x) {
                if (!g.ref(x,y,z)) continue;
                if (g.get(x+dir[0], y+dir[1], z+dir[2])) continue; // not a border in this dir
                if (is_simple(g, x, y, z))
                    to_del.push_back(z*(g.ny*g.nx) + y*g.nx + x);
            }
            for (int idx : to_del) g.d[idx] = 0;
            if (!to_del.empty()) changed = true;
        }
    }
}

// Voxelise mesh using ray casting in +Z for each (xi,yi) column.
static VoxGrid voxelize(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F, int res)
{
    using Ray_3     = K::Ray_3;
    using Vector_3  = K::Vector_3;

    Mesh mesh = build_cgal_mesh(V, F);
    Tree tree;
    tree.insert(faces(mesh).first, faces(mesh).second, mesh);
    tree.build();

    double xlo = V.col(0).minCoeff(), xhi = V.col(0).maxCoeff();
    double ylo = V.col(1).minCoeff(), yhi = V.col(1).maxCoeff();
    double zlo = V.col(2).minCoeff(), zhi = V.col(2).maxCoeff();
    double span = std::max({xhi-xlo, yhi-ylo, zhi-zlo});
    double vs   = span / (double)res;
    double pad  = 2.0 * vs;

    double ox = xlo - pad, oy = ylo - pad, oz = zlo - pad;
    int nx = (int)std::ceil((xhi - ox) / vs) + 3;
    int ny = (int)std::ceil((yhi - oy) / vs) + 3;
    int nz = (int)std::ceil((zhi - oz) / vs) + 3;

    VoxGrid g; g.init(nx, ny, nz);
    g.ox = ox; g.oy = oy; g.oz = oz; g.vs = vs;

    using IsectType = Tree::Intersection_and_primitive_id<Ray_3>::Type;
    for (int yi = 0; yi < ny; ++yi)
    for (int xi = 0; xi < nx; ++xi) {
        double wx = ox + (xi + 0.5) * vs;
        double wy = oy + (yi + 0.5) * vs;

        Ray_3 ray(Point_3(wx, wy, oz - vs), Vector_3(0, 0, 1));
        std::vector<IsectType> hits;
        tree.all_intersections(ray, std::back_inserter(hits));

        std::vector<double> zs;
        zs.reserve(hits.size());
        for (auto& h : hits)
            if (const Point_3* pt = std::get_if<Point_3>(&h.first))   // CGAL 6: std::variant
                zs.push_back(pt->z());
        if (zs.empty()) continue;

        std::sort(zs.begin(), zs.end());
        // Deduplicate near-coincident hits (triangle edge/vertex intersections).
        zs.erase(std::unique(zs.begin(), zs.end(), [vs](double a, double b) {
            return std::abs(a - b) < vs * 0.05;
        }), zs.end());
        if (zs.size() % 2 != 0) continue; // parity error — degenerate geometry, skip

        // Parity fill: voxels between each entry/exit pair are solid.
        for (int h = 0; h + 1 < (int)zs.size(); h += 2) {
            int zi0 = std::max(0, (int)std::floor((zs[h]   - oz) / vs));
            int zi1 = std::min(nz-1, (int)std::ceil ((zs[h+1] - oz) / vs));
            for (int zi = zi0; zi <= zi1; ++zi)
                g.ref(xi, yi, zi) = 1;
        }
    }
    return g;
}

// Convert a thinned voxel grid to a SkeletonGraph.
// Each solid voxel becomes a node; 26-connected solid pairs become edges.
static SkeletonGraph voxels_to_graph(const VoxGrid& g)
{
    const size_t NXY = (size_t)g.nx * g.ny;
    std::vector<int> vox_idx(g.d.size(), -1);
    SkeletonGraph G;

    for (int z = 0; z < g.nz; ++z)
    for (int y = 0; y < g.ny; ++y)
    for (int x = 0; x < g.nx; ++x) {
        size_t i = (size_t)z*NXY + y*g.nx + x;
        if (!g.d[i]) continue;
        vox_idx[i] = G.num_nodes();
        G.add_node(Eigen::Vector3d(
            g.ox + (x + 0.5) * g.vs,
            g.oy + (y + 0.5) * g.vs,
            g.oz + (z + 0.5) * g.vs));
    }

    std::set<std::pair<int,int>> seen;
    for (int z = 0; z < g.nz; ++z)
    for (int y = 0; y < g.ny; ++y)
    for (int x = 0; x < g.nx; ++x) {
        int u = vox_idx[(size_t)z*NXY + y*g.nx + x];
        if (u < 0) continue;
        for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if ((dx|dy|dz) == 0) continue;
            if (!g.valid(x+dx, y+dy, z+dz)) continue;
            int v = vox_idx[(size_t)(z+dz)*NXY + (y+dy)*g.nx + (x+dx)];
            if (v < 0 || v <= u) continue;
            if (seen.emplace(u, v).second)
                G.add_edge(u, v);
        }
    }
    return G;
}

} // anonymous namespace

SkeletonGraph skeletonize_mesh_voxel(const Eigen::MatrixXd& V,
                                     const Eigen::MatrixXi& F,
                                     int resolution)
{
    std::cout << "[voxel] Voxelising at resolution " << resolution << "...\n";
    VoxGrid g = voxelize(V, F, resolution);

    int solid_before = 0;
    for (uint8_t v : g.d) if (v) ++solid_before;
    std::cout << "[voxel] " << solid_before << " solid voxels ("
              << g.nx << "×" << g.ny << "×" << g.nz << " grid, vs=" << g.vs << "mm)\n";

    std::cout << "[voxel] Thinning (Lee 1994)...\n";
    thin_3d(g);

    int solid_after = 0;
    for (uint8_t v : g.d) if (v) ++solid_after;
    std::cout << "[voxel] After thinning: " << solid_after << " skeleton voxels\n";

    SkeletonGraph G = voxels_to_graph(g);
    std::cout << "[voxel] Graph: " << G.num_nodes() << " nodes\n";
    return G;
}

// ── Bore detection and centreline tracing ─────────────────────────────────────
//
// The problem: mean curvature flow contracts the *material*, so an open tube — which is
// topologically a solid torus — collapses to a loop around the middle of its wall and the
// axial structure is lost entirely. No amount of graph cleaning recovers it, because the
// information was never in the skeleton. The axis has to be measured from the geometry.
//
// Cross sections give it directly. A bore shows up as a nested contour (a hole) that
// persists over many stations along some direction; its depth-to-width ratio is what
// distinguishes a pipe (deep and narrow — follow the axis) from a washer or a windowed
// plate (shallow and wide — follow the material loop). Detection is by cross section and
// not by genus, so blind pockets are found too even though their surface is genus 0.

// Distance from q to the polyline, plus the index of the nearest vertex and — optionally — how
// far past a terminal vertex q projects. The distance alone cannot tell "beside the polyline"
// from "beyond its end", because both reduce to the distance to the same endpoint; callers that
// treat the polyline as a corridor need to know which, or the corridor silently becomes a capsule
// with a hemispherical cap on each end.
static double point_polyline_dist(const std::vector<Eigen::Vector3d>& pts,
                                 const Eigen::Vector3d& q,
                                 int* nearest_vertex = nullptr,
                                 double* overhang = nullptr)
{
    double best = std::numeric_limits<double>::max();
    int    best_i = -1;
    for (size_t i = 0; i < pts.size(); ++i) {
        const double d = (pts[i] - q).norm();
        if (d < best) { best = d; best_i = (int) i; }
    }
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Eigen::Vector3d ab = pts[i + 1] - pts[i];
        const double len2 = ab.squaredNorm();
        if (len2 < 1e-18)
            continue;
        double t = (q - pts[i]).dot(ab) / len2;
        t = std::max(0.0, std::min(1.0, t));
        const double d = (pts[i] + t * ab - q).norm();
        if (d < best) {
            best   = d;
            best_i = (int) (t < 0.5 ? i : i + 1);
        }
    }
    if (nearest_vertex)
        *nearest_vertex = best_i;
    if (overhang) {
        // Only a query whose closest approach is a terminal vertex can be past the end at all;
        // anything else projects onto the interior and overhangs by nothing.
        double ov = 0.0;
        if (pts.size() >= 2) {
            const size_t L = pts.size() - 1;
            if (best_i == 0) {
                const Eigen::Vector3d d0 = pts[0] - pts[1];
                if (d0.squaredNorm() > 1e-18)
                    ov = std::max(0.0, (q - pts[0]).dot(d0.normalized()));
            } else if (best_i == (int) L) {
                const Eigen::Vector3d dn = pts[L] - pts[L - 1];
                if (dn.squaredNorm() > 1e-18)
                    ov = std::max(0.0, (q - pts[L]).dot(dn.normalized()));
            }
        }
        *overhang = ov;
    }
    return best;
}

// +Z plus the three PCA axes, deduplicated. A bore skew to all four is missed, which is no
// worse than the previous behaviour of missing every bore.
static std::vector<Eigen::Vector3d> candidate_axes(const Eigen::MatrixXd& V)
{
    std::vector<Eigen::Vector3d> dirs{Eigen::Vector3d::UnitZ()};
    if (V.rows() < 4)
        return dirs;

    const Eigen::Vector3d mean = V.colwise().mean().transpose();
    const Eigen::MatrixXd C    = V.rowwise() - mean.transpose();
    const Eigen::Matrix3d cov  = C.transpose() * C;
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(cov);
    if (es.info() != Eigen::Success)
        return dirs;

    for (int k = 2; k >= 0; --k) { // largest eigenvalue first
        Eigen::Vector3d v = es.eigenvectors().col(k);
        if (v.norm() < 1e-9)
            continue;
        v.normalize();
        if (v.z() < 0.0)
            v = -v;
        bool dup = false;
        for (const auto& e : dirs)
            if (std::abs(e.dot(v)) > 0.98) { dup = true; break; }
        if (!dup)
            dirs.push_back(v);
    }
    return dirs;
}

// Walk the centreline from `seed`, one direction per call. Each step places the next plane
// by a short fixed-point iteration: the plane normal is the chord from the previous point,
// and the next point is the area centroid of the outer contour in that plane. Following
// the chord rather than a fixed axis is what lets the trace go round a bend.
static void march_bore_one_way(Slicer& slicer,
                              const Eigen::Vector3d& seed,
                              const Eigen::Vector3d& dir0,
                              double h,
                              int    max_steps,
                              std::vector<Eigen::Vector3d>& pts,
                              std::vector<double>& rad,
                              const char* tag = "tunnel")
{
    Eigen::Vector3d p = seed, dir = dir0.normalized();
    const char* stop = "max_steps";
    // Adaptive step. A coarse step is what keeps forward progress ahead of the cross-section
    // centroid's lateral wander through a bend, but it also leaves the trace up to h short of the
    // far end of the bore — which showed up as the test pipes reaching 58.78 mm of 60 instead of
    // 59.4. So: step at h, and on a failed step halve and retry rather than giving up, growing
    // back toward h after each success. The march then runs coarse through the run and creeps the
    // last fraction of a millimetre to the end.
    const double h_min = std::min(0.15, h);
    double hh = h;

    // Curvature anticipation. Stepping along the CURRENT direction assumes the bore is locally
    // straight; through a bend that prediction lands progressively further outside the tube, and
    // once the predicted point leaves the outer contour the step fails. So carry the turn
    // observed over the last advance and pre-rotate the prediction by it, which tracks a constant
    // curvature bend instead of lagging it.
    //
    // Two things have to hold for that to work, and neither did.
    //
    // The anticipation has to be a turn RATE (rad/mm), not a per-step angle. It is applied over
    // whatever step length the next attempt uses, and a failed step halves that length — so
    // carrying an angle across unscaled over-rotates by exactly the factor the step shrank, and
    // refining makes the prediction worse rather than better. Measured on a real intake: six
    // halvings from 9.45 mm down to 0.15 mm all failing at the same corner.
    //
    // And the step itself has to shrink through the bend, because no pre-rotation tracks an arc
    // whose heading turns 27 deg over one step — which is what that intake does where its runner
    // meets the plenum. Both fall out of one number: hold the heading change per step to
    // kTurnBudget by capping hh at kTurnBudget / turn_rate. Away from a bend turn_rate is ~0, the
    // cap is inactive, and the march runs at the coarse step as before.
    auto rodrigues = [](const Eigen::Vector3d& v, const Eigen::Vector3d& k, double th) {
        return v * std::cos(th) + k.cross(v) * std::sin(th) + k * k.dot(v) * (1.0 - std::cos(th));
    };
    const double kTurnBudget = 12.0 * M_PI / 180.0; // heading change allowed over one step
    const double kTurnCap    = 45.0 * M_PI / 180.0; // hard limit on one pre-rotation
    // A safety net, not a shape control. Where a runner opens into a plenum the centreline
    // genuinely swings, and measuring it at 60 deg cost more coverage than the spikes it caught
    // (58% of the intake within 5 mm of centre, against 70% with no cap at all). Set above any
    // turn a real centreline makes over one step, so it only catches a jump to another feature.
    const double kAdvTurnCap = 100.0 * M_PI / 180.0; // hard limit on one accepted advance
    Eigen::Vector3d turn_axis = Eigen::Vector3d::UnitZ();
    double          turn_rate = 0.0; // radians per mm, from the previous advance
    int             recentred = 0;   // steps rescued by re-centring onto the nearest contour

    for (int step = 0; step < max_steps; ++step) {
        if (turn_rate > 1e-9)
            hh = std::max(h_min, std::min(hh, kTurnBudget / turn_rate));
        Eigen::Vector3d pdir = dir;
        // Cap the anticipation as well: a noisy centroid must not be able to spin the heading.
        const double a = std::min(turn_rate * hh, kTurnCap);
        if (a > 1e-4)
            pdir = rodrigues(dir, turn_axis, a).normalized();
        Eigen::Vector3d c = p + pdir * hh;
        double r  = 0.0;
        bool   ok = false;
        for (int it = 0; it < 6; ++it) {
            Eigen::Vector3d nd = c - p;
            const double nl = nd.norm();
            if (nl < 1e-9) { ok = false; stop = "plane normal degenerate"; break; }
            nd /= nl;
            const Section s = slice_section(slicer, c, nd);
            if (s.empty()) { ok = false; stop = "empty section (left the part)"; break; }
            bool contained = false;
            int  li = outer_loop_at(s, Eigen::Vector2d::Zero(), &contained);
            if (li < 0) { ok = false; stop = "no outer loop in section"; break; }
            if (!contained) {
                // The predicted point is off the material — which is not by itself the end of
                // the road. Where a bore opens into a wider volume (a runner into a plenum, a
                // tube into a flange) the outer contour's centroid lurches sideways within one
                // step, so a prediction made from the previous heading lands in air even though
                // the part continues. Treating that as the end is what stopped a real intake at
                // its plenum with a third of the mesh still ahead of it.
                //
                // So re-centre onto the nearest depth-0 loop rather than giving up. The bound is
                // what keeps this honest: a correction of more than the local corridor reach is
                // not a lurch, it is the march jumping to a different limb, and it must stop.
                double best = std::numeric_limits<double>::max();
                int    best_i = -1;
                for (size_t k = 0; k < s.loops.size(); ++k) {
                    if (s.loops[k].depth != 0)
                        continue;
                    const double d2 = s.loops[k].centroid.squaredNorm();
                    if (d2 < best) { best = d2; best_i = (int) k; }
                }
                if (best_i < 0) { ok = false; stop = "no outer loop in section"; break; }
                // Bound the correction by the STEP, not by the corridor reach. The reach is the
                // outer contour's full extent — ~50 mm on a real intake — and a leash that long
                // lets the march teleport clean across the part and back, which showed up as a
                // 13 mm zigzag in the middle of an otherwise exact spine. A centreline cannot
                // move sideways faster than it moves forward, so the step is the honest bound;
                // when it is too tight the retry below halves the step and tries again, and the
                // correction the shorter step needs shrinks with it.
                const double bound = 1.5 * hh;
                if ((s.to3d(s.loops[best_i].centroid) - c).norm() > bound) {
                    ok = false;
                    stop = "off the material (nearest contour too far to be the same feature)";
                    break;
                }
                li = best_i;
                ++recentred;
            }
            // Damp the fixed-point update. Taking the centroid outright is a Newton step with no
            // control on overshoot: through a section that is widening — a runner opening into a
            // plenum — it lands past the true centre, the next section is then cut at a skewed
            // angle, and the iteration rings instead of settling. Measured on a real intake that
            // put a 14 mm spike with a 119 deg turn in the middle of an otherwise exact spine.
            // Half steps converge to the same fixed point without the ringing; the iteration
            // budget below is raised to pay for the slower approach.
            const Eigen::Vector3d c_new = c + 0.5 * (s.to3d(s.loops[li].centroid) - c);
            const bool converged = (c_new - c).norm() < 0.01 * hh;
            c = c_new;
            // Corridor radius: the outer contour's full reach. Do NOT clamp this toward an
            // equivalent-area radius — for a thin-walled tube the wall skeleton sits close to
            // the outer surface and an area-based clamp leaves it outside the corridor, so the
            // wall loop survives the splice and the bore gets two skeletons.
            r  = s.loops[li].max_r;
            ok = true;
            if (converged)
                break; // a straight run converges on the first step; do not pay for two more
        }
        if (!ok) {
            // Near the end of the bore, not necessarily the end of the road: refine and retry.
            if (hh > h_min) { hh = std::max(h_min, hh * 0.5); continue; }
            break;
        }
        const Eigen::Vector3d d = c - p;
        const double dl = d.norm();
        // Forward progress, not total displacement. The centroid of a cross section wanders
        // sideways as the bore bends, and on a wide bore stepped at a small h that lateral
        // component dominates |d| — so a total-displacement test reads a healthy step through a
        // bend as a collapse. Measured: this is what stalled a real intake at 18% of its run.
        const double fwd = d.dot(dir);
        if (fwd < 0.25 * hh) {
            if (hh > h_min) { hh = std::max(h_min, hh * 0.5); continue; }
            stop = (dl < 0.25 * hh) ? "step collapsed onto the previous point"
                                    : "no forward progress (centroid moved sideways only)";
            break;
        }
        // Bound the accepted ADVANCE, not just the prediction. `c` is the section centroid, and
        // the fixed-point iteration above can drag it a long way from the point that was
        // predicted — so bounding the prediction alone bounds nothing. Measured on a real
        // intake: two consecutive 9.45 mm steps landing 14.4 and 15.5 mm away with the heading
        // swinging 119 deg and then 97 deg, a spike in the middle of an otherwise exact spine
        // that would have become two wildly tilted cut planes through the part.
        //
        // A step that turns that hard has not followed a centreline, it has jumped to another
        // region of the section. Halve and try again: a shorter step cuts a section more like
        // the last one, so the centroid moves less and the turn comes back inside the cap.
        // The first step of a march is exempt — its heading is the seed axis, not a measurement.
        if (pts.size() >= 1) {
            const double adv_turn = std::acos(std::max(-1.0, std::min(1.0, (d / dl).dot(dir))));
            if (adv_turn > kAdvTurnCap) {
                if (hh > h_min) { hh = std::max(h_min, hh * 0.5); continue; }
                stop = "step jumped (section centroid left the centreline)";
                break;
            }
        }
        // Damp the direction update. Re-aiming straight down the new chord lets one noisy
        // centroid throw the plane off, and the next section is then cut at a skewed angle,
        // which feeds back.
        //
        // The weight is the march's inertia, and it trades bend-following against drift.
        //
        // 0.3 was tried and reverted. It scored better on a station metric that measures the
        // distance from each section centroid, taken along the part's dominant axis, to the
        // skeleton — but that metric assumes the centreline stays roughly parallel to that axis
        // and therefore PENALISES a duct that correctly turns to meet the bed. On a real intake
        // it preferred a march that gave up 20 mm early and let the bed-snap finish with a
        // straight chord ~33 deg off vertical, over one that tracked the duct down to a
        // near-vertical tail (y pinned within 0.8 mm while z fell from 22 to 3). The second is
        // the right answer and produces the horizontal cuts a base chunk wants; the number said
        // otherwise because it was measuring along the wrong axis.
        const Eigen::Vector3d blended = (0.5 * dir + 0.5 * (d / dl));
        const double bl = blended.norm();
        const Eigen::Vector3d new_dir = (bl > 1e-12) ? Eigen::Vector3d(blended / bl)
                                                     : Eigen::Vector3d(d / dl);
        // Record how hard the heading actually turned, per millimetre travelled, to anticipate
        // and to size the next step. Normalising by the advance is what makes the number
        // comparable across a step length that changes underneath it.
        {
            const Eigen::Vector3d ax = dir.cross(new_dir);
            const double          s  = ax.norm();
            if (s > 1e-9 && dl > 1e-9) {
                turn_axis = ax / s;
                turn_rate = std::atan2(s, dir.dot(new_dir)) / dl;
            } else {
                turn_rate = 0.0;
            }
        }
        dir = new_dir;
        p   = c;
        pts.push_back(p);
        rad.push_back(r);
        // Ease back toward the coarse step after a success rather than doubling: the curvature
        // cap at the top of the loop re-clamps it anyway, and a gentler recovery stops the march
        // oscillating between "too coarse, fail" and "halve, succeed" all the way round a bend.
        hh = std::min(h, hh * 1.5);
    }
    std::cout << "[" << tag << "]   march stopped after " << pts.size() << " step(s): " << stop
              << "  at (" << p.x() << ", " << p.y() << ", " << p.z() << ")"
              << " dir (" << dir.x() << ", " << dir.y() << ", " << dir.z() << ")"
              << " step " << hh << "mm (coarse " << h << ")"
              << " recentred " << recentred;
    if (pts.size() >= 3) {
        // How hard was the path turning when it gave up? A bend that outruns the direction
        // update is the difference between "the bore genuinely ended" and "the march fell off
        // the outside of a corner", and the two need opposite fixes.
        const Eigen::Vector3d d1 = (pts[pts.size() - 1] - pts[pts.size() - 2]).normalized();
        const Eigen::Vector3d d2 = (pts[pts.size() - 2] - pts[pts.size() - 3]).normalized();
        const double turn = std::acos(std::max(-1.0, std::min(1.0, d1.dot(d2)))) * 180.0 / M_PI;
        std::cout << "  last turn " << turn << " deg/step";
    }
    std::cout << "\n";
}

int extend_skeleton_tips(SkeletonGraph& G, const Eigen::MatrixXd& V, const Eigen::MatrixXi& F,
                         double max_extension)
{
    const int n = G.num_nodes();
    if (n == 0 || V.rows() == 0 || F.rows() == 0)
        return 0;

    Mesh mesh = build_prepared_mesh(V, F);
    if (!CGAL::is_closed(mesh))
        return 0;
    Slicer slicer(mesh);

    const Eigen::Vector3d bb_min = V.colwise().minCoeff().transpose();
    const Eigen::Vector3d bb_max = V.colwise().maxCoeff().transpose();
    const double diag = (bb_max - bb_min).norm();
    const double cap  = max_extension > 0.0 ? max_extension : 0.5 * diag;

    // Snapshot the tips: the loop below adds nodes, and a freshly added tip is already at the
    // extremity — marching it again would only walk back over the same ground.
    std::vector<int> tips;
    for (int i = 0; i < n; ++i)
        if (G.degree(i) == 1)
            tips.push_back(i);

    int extended = 0;
    for (int t : tips) {
        Eigen::Vector3d d  = G.nodes[t] - G.nodes[G.adj[t][0]];
        const double    dn = d.norm();
        // A degenerate branch carries no direction to march along. MCF returns a cone as two
        // nodes 0.011 mm apart sharing one edge; marching off in whatever direction that noise
        // happens to point is worse than leaving the tip where it is.
        if (dn < 0.1)
            continue;
        d /= dn;

        // Step from the LOCAL section rather than a global constant: a 6 mm prong and a 60 mm
        // trunk want different steps, and the march's own adaptive halving covers the rest.
        double h = 0.0;
        {
            const Section s = slice_section(slicer, G.nodes[t], d);
            bool      contained = false;
            const int li = s.empty() ? -1 : outer_loop_at(s, Eigen::Vector2d::Zero(), &contained);
            if (li >= 0)
                h = 0.5 * std::sqrt(std::abs(s.loops[li].area) / M_PI);
        }
        h = std::max(0.5, std::min(h > 0.0 ? h : 2.0, 5.0));

        std::vector<Eigen::Vector3d> pts;
        std::vector<double>          rad;
        march_bore_one_way(slicer, G.nodes[t], d, h,
                           std::min(400, (int) std::ceil(cap / 0.15) + 20), pts, rad, "extend_tips");
        if (pts.empty())
            continue;

        int    prev  = t;
        double run   = 0.0;
        int    added = 0;
        for (const Eigen::Vector3d& p : pts) {
            // Stop if the march turns back on itself. A tip march that doubles back is
            // describing the body the tip came from, not the extremity beyond it.
            if ((p - G.nodes[t]).dot(d) <= 0.0)
                break;
            const double seg = (p - G.nodes[prev]).norm();
            if (run + seg > cap)
                break;
            run += seg;
            const int idx = G.num_nodes();
            G.add_node(p);
            G.add_edge(prev, idx);
            prev = idx;
            ++added;
        }
        if (added > 0) {
            ++extended;
            std::cout << "[extend_tips] tip at (" << G.nodes[t].x() << ", " << G.nodes[t].y()
                      << ", " << G.nodes[t].z() << ") extended " << run << "mm with " << added
                      << " node(s), step " << h << "mm\n";
        }
    }
    if (extended > 0)
        std::cout << "[extend_tips] extended " << extended << " / " << tips.size() << " tip(s).\n";
    return extended;
}

std::vector<TunnelSpine> find_tunnel_spines(const Eigen::MatrixXd& V,
                                            const Eigen::MatrixXi& F,
                                            double station,
                                            double min_aspect,
                                            int    max_tunnels)
{
    std::vector<TunnelSpine> out;
    if (V.rows() == 0 || F.rows() == 0 || station <= 0.0)
        return out;

    Mesh mesh = build_prepared_mesh(V, F);
    if (!CGAL::is_closed(mesh))
        return out;
    Slicer slicer(mesh);

    const Eigen::Vector3d bb_min = V.colwise().minCoeff().transpose();
    const Eigen::Vector3d bb_max = V.colwise().maxCoeff().transpose();
    const Eigen::Vector3d mean   = V.colwise().mean().transpose();
    const double          diag   = (bb_max - bb_min).norm();

    struct Cand { Eigen::Vector3d seed, axis; double depth{0.0}, width{0.0}; };
    std::vector<Cand> cands;

    for (const Eigen::Vector3d& dir : candidate_axes(V)) {
        const Eigen::VectorXd t = V * dir;
        const double lo = t.minCoeff(), hi = t.maxCoeff();
        const double span = hi - lo;
        if (span < 1e-6)
            continue;

        int n_st = (int) std::ceil(span / station);
        n_st     = std::max(4, std::min(n_st, 400));
        const double step = span / n_st;
        const Eigen::Vector3d base = mean - mean.dot(dir) * dir;

        std::vector<char>            has(n_st, 0);
        std::vector<Eigen::Vector3d> hole_c(n_st, Eigen::Vector3d::Zero());
        std::vector<double>          hole_w(n_st, 0.0);

        for (int k = 0; k < n_st; ++k) {
            const Eigen::Vector3d p = base + (lo + step * (k + 0.5)) * dir;
            const Section s = slice_section(slicer, p, dir);
            int    best = -1;
            double best_area = 0.0;
            for (size_t i = 0; i < s.loops.size(); ++i)
                if (s.loops[i].depth == 1 && std::abs(s.loops[i].area) > best_area) {
                    best_area = std::abs(s.loops[i].area);
                    best      = (int) i;
                }
            if (best < 0)
                continue;
            has[k]    = 1;
            hole_c[k] = s.to3d(s.loops[best].centroid);
            hole_w[k] = 2.0 * std::sqrt(best_area / M_PI);
        }

        int run_b = -1, run_e = -1, cur_b = -1;
        for (int k = 0; k <= n_st; ++k) {
            const bool on = (k < n_st && has[k] != 0);
            if (on && cur_b < 0)
                cur_b = k;
            if (!on && cur_b >= 0) {
                if (run_b < 0 || (k - cur_b) > (run_e - run_b)) { run_b = cur_b; run_e = k; }
                cur_b = -1;
            }
        }
        if (run_b < 0 || (run_e - run_b) < 2)
            continue;

        std::vector<double> ws(hole_w.begin() + run_b, hole_w.begin() + run_e);
        std::nth_element(ws.begin(), ws.begin() + ws.size() / 2, ws.end());

        Cand c;
        c.axis  = dir;
        c.seed  = hole_c[(run_b + run_e) / 2];
        c.depth = (run_e - run_b) * step;
        c.width = ws[ws.size() / 2];
        cands.push_back(c);

        std::cout << "[tunnel] dir (" << dir.x() << ", " << dir.y() << ", " << dir.z()
                  << ")  depth=" << c.depth << "mm  width=" << c.width
                  << "mm  aspect=" << (c.width > 1e-9 ? c.depth / c.width : 0.0) << "\n";
    }

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        const double aa = a.width > 1e-9 ? a.depth / a.width : 0.0;
        const double bb = b.width > 1e-9 ? b.depth / b.width : 0.0;
        return aa > bb;
    });

    for (const Cand& c : cands) {
        if ((int) out.size() >= max_tunnels)
            break;
        const double aspect = c.width > 1e-9 ? c.depth / c.width : 0.0;
        if (min_aspect > 0.0 && aspect < min_aspect) {
            std::cout << "[tunnel] rejected: aspect " << aspect << " < " << min_aspect
                      << " — treating the hole as a feature to follow, not a bore.\n";
            continue;
        }

        // Step scaled to the BORE, not to the whole mesh. `station` is the detection spacing and
        // comes from the bounding-box diagonal; using it for the march gave 1.58 mm steps through
        // a 63 mm bore, so tiny that the cross-section centroid's sideways wander through a bend
        // exceeded the forward step and the march stalled at 18% of the run. Forward progress has
        // to dominate that wander, which means h must scale with the bore width — while still
        // leaving enough samples along the run to follow a bend (~8 minimum).
        const double h = std::max(0.5, std::min(0.15 * c.width, c.depth / 8.0));
        // Budget the steps from the march's FLOOR, not from its coarse step. The step is adaptive
        // and shrinks through a bend, so sizing the budget from h let a curvature-limited march
        // exhaust it part way along the run and report that as the end of the bore.
        const int max_steps = std::min(4000, (int) std::ceil(diag / std::min(0.15, h)) + 20);

        std::vector<Eigen::Vector3d> fwd, bwd;
        std::vector<double>          rf, rb;
        march_bore_one_way(slicer, c.seed, c.axis, h, max_steps, fwd, rf);
        march_bore_one_way(slicer, c.seed, -c.axis, h, max_steps, bwd, rb);

        TunnelSpine sp;
        sp.axis  = c.axis;
        sp.depth = c.depth;
        sp.width = c.width;
        for (int i = (int) bwd.size() - 1; i >= 0; --i) {
            sp.points.push_back(bwd[i]);
            sp.radius.push_back(rb[i]);
        }
        sp.points.push_back(c.seed);
        sp.radius.push_back(!rf.empty() ? rf.front() : (!rb.empty() ? rb.front() : c.width));
        for (size_t i = 0; i < fwd.size(); ++i) {
            sp.points.push_back(fwd[i]);
            sp.radius.push_back(rf[i]);
        }
        if (sp.points.size() < 3)
            continue;

        // Duplicate rejection, on the traced result rather than on the seed. Two candidate
        // directions routinely find the SAME bore — on the 90 deg elbow the +Z candidate and
        // the PCA diagonal both do, and their axes are 45 deg apart, so no axis-similarity
        // test can catch it. Splicing both produced two overlapping spines, two DAG roots and
        // twice the cuts. Comparing the actual polylines is direction-agnostic, and it still
        // keeps genuinely crossing bores, which overlap only near their intersection.
        bool dup = false;
        for (const auto& acc : out) {
            int inside = 0;
            for (size_t i = 0; i < sp.points.size(); ++i) {
                int idx = -1;
                const double d = point_polyline_dist(acc.points, sp.points[i], &idx);
                const double r = (idx >= 0 && idx < (int) acc.radius.size()) ? acc.radius[idx] : 0.0;
                if (d <= r)
                    ++inside;
            }
            if (inside > (int) (0.6 * sp.points.size())) {
                std::cout << "[tunnel] discarded duplicate trace of an already-found bore ("
                          << inside << "/" << sp.points.size() << " points overlap)\n";
                dup = true;
                break;
            }
        }
        if (dup)
            continue;

        double len = 0.0;
        for (size_t i = 0; i + 1 < sp.points.size(); ++i)
            len += (sp.points[i + 1] - sp.points[i]).norm();
        // A trace that stalls must not be trusted. The splice below deletes every existing node
        // inside the spine's corridor, so a stub is not merely useless — it is destructive:
        // measured on a real intake, a 15.8 mm trace of an 86.7 mm run wiped all 53 wall nodes
        // and left a 6-node skeleton covering a fraction of the part. If the march did not get
        // most of the way along the run it claimed, discard it and keep the wall skeleton.
        // Coverage measured along the SEED AXIS, not as arc length. `c.depth` is how far the hole
        // persists along a straight axis, so on a bent tube the centreline arc is legitimately
        // longer than it — comparing arc length against it reported "107% of the run" for a trace
        // that had fallen out of the bend less than two thirds of the way along. Projecting the
        // span onto the seed axis compares like with like.
        double lo = 0.0, hi = 0.0;
        for (size_t i = 0; i < sp.points.size(); ++i) {
            const double t = (sp.points[i] - c.seed).dot(c.axis);
            lo = (i == 0) ? t : std::min(lo, t);
            hi = (i == 0) ? t : std::max(hi, t);
        }
        const double covered = (c.depth > 1e-9) ? (hi - lo) / c.depth : 0.0;
        if (covered < 0.5) {
            std::cout << "[tunnel] discarded stalled trace: " << len << "mm of a " << c.depth
                      << "mm run (" << (int) (covered * 100.0) << "%) — keeping the wall skeleton"
                      << " rather than letting a stub delete it.\n";
            continue;
        }

        std::cout << "[tunnel] traced centreline: " << sp.points.size() << " points, "
                  << len << "mm long, step " << h << "mm (" << (int) (covered * 100.0)
                  << "% of the run)\n";
        out.push_back(std::move(sp));
    }
    return out;
}

SkeletonGraph splice_tunnel_spines(const SkeletonGraph& G, const std::vector<TunnelSpine>& spines)
{
    if (spines.empty())
        return G;

    const int n = G.num_nodes();

    // One node spacing at each end of a spine. Enough overhang to swallow the wall ring around
    // the terminal node, not enough to reach into geometry the trace never covered.
    auto end_margin = [](const TunnelSpine& sp) {
        double m = 0.0;
        for (size_t i = 0; i + 1 < sp.points.size(); ++i)
            m = std::max(m, (sp.points[i + 1] - sp.points[i]).norm());
        return m;
    };

    // Which spine's corridor, if any, swallows each existing node. For a plain pipe this is
    // every node of the mid-wall MCF loop, so the wall skeleton disappears entirely.
    //
    // The corridor is capped longitudinally as well as laterally. Distance to the polyline
    // degenerates to distance to an endpoint once a query is past the end, so a lateral test on
    // its own makes the corridor a capsule with a hemispherical cap of radius r — and r is the
    // outer contour's full reach, 49-55 mm on a real intake. Where a trace stops short, that cap
    // deleted the wall skeleton covering exactly the geometry the trace failed to reach, so the
    // region ended up with no skeleton at all instead of falling back to the one it had.
    std::vector<int> owner(n, -1);
    int saved_by_cap = 0;
    for (int i = 0; i < n; ++i) {
        for (size_t s = 0; s < spines.size(); ++s) {
            int    idx  = -1;
            double over = 0.0;
            const double d = point_polyline_dist(spines[s].points, G.nodes[i], &idx, &over);
            const double r = (idx >= 0 && idx < (int) spines[s].radius.size()) ? spines[s].radius[idx] : 0.0;
            if (d > r * 1.02)
                continue;
            if (over > end_margin(spines[s])) { ++saved_by_cap; continue; }
            owner[i] = (int) s;
            break;
        }
    }

    SkeletonGraph R;
    std::vector<int> old_to_new(n, -1);
    for (int i = 0; i < n; ++i)
        if (owner[i] < 0) {
            old_to_new[i] = R.num_nodes();
            R.add_node(G.nodes[i]);
        }

    std::vector<std::vector<int>> spine_nodes(spines.size());
    for (size_t s = 0; s < spines.size(); ++s)
        for (const auto& p : spines[s].points) {
            spine_nodes[s].push_back(R.num_nodes());
            R.add_node(p);
        }

    std::set<std::pair<int,int>> added;
    auto add_unique = [&](int a, int b) {
        if (a < 0 || b < 0 || a == b)
            return;
        if (added.insert(std::make_pair(std::min(a, b), std::max(a, b))).second)
            R.add_edge(a, b);
    };

    for (size_t s = 0; s < spines.size(); ++s)
        for (size_t k = 1; k < spine_nodes[s].size(); ++k)
            add_unique(spine_nodes[s][k - 1], spine_nodes[s][k]);

    auto nearest_spine_node = [&](int s, const Eigen::Vector3d& q) {
        int    best = spine_nodes[s].empty() ? -1 : spine_nodes[s].front();
        double bd   = std::numeric_limits<double>::max();
        for (size_t k = 0; k < spines[s].points.size(); ++k) {
            const double d = (spines[s].points[k] - q).squaredNorm();
            if (d < bd) { bd = d; best = spine_nodes[s][k]; }
        }
        return best;
    };

    int reattached = 0;
    for (int u = 0; u < n; ++u)
        for (int v : G.adj[u]) {
            if (v <= u)
                continue;
            const int ou = owner[u], ov = owner[v];
            if (ou < 0 && ov < 0)
                add_unique(old_to_new[u], old_to_new[v]);
            else if (ou < 0 && ov >= 0) {
                add_unique(old_to_new[u], nearest_spine_node(ov, G.nodes[u]));
                ++reattached;
            } else if (ou >= 0 && ov < 0) {
                add_unique(nearest_spine_node(ou, G.nodes[v]), old_to_new[v]);
                ++reattached;
            }
            // both swallowed by a corridor: the edge is part of the wall skeleton, drop it
        }

    int dropped = 0;
    for (int i = 0; i < n; ++i)
        if (owner[i] >= 0)
            ++dropped;
    std::cout << "[tunnel] spliced " << spines.size() << " spine(s): dropped " << dropped
              << " / " << n << " wall nodes (" << saved_by_cap
              << " kept: laterally inside a corridor but past its end), reattached " << reattached
              << " branch edge(s), graph now " << R.num_nodes() << " nodes.\n";
    return R;
}

// ── Bores from the inner wall ─────────────────────────────────────────────────
//
// find_tunnel_spines() finds a bore by slicing along a few straight axes and counting the
// stations that show a hole, so it is blind to a bore that bends: a 90 deg elbow shows a hole
// along +Z for one leg only and along the PCA axes for neither. Measured on a real intake duct:
// four candidates, best aspect 1.12 against a gate of 1.2, none accepted, and the part was
// skeletonised as a loop around its own wall with the root at a random place on it.
//
// The inner wall knows where the bore is. A face lining a bore sees material in most of its own
// hemisphere; a face on the outside sees sky. Classify every face by a few rays, take the
// connected inner-wall patches, and a patch with two boundary loops IS a through-bore, however
// it bends. A harmonic function on the patch, 0 on one opening and 1 on the other, has level
// sets that ring the bore, and the area centroid of each ring is a centreline point. No
// marching, no step size, nothing to stall. Measured on six real intake parts, 0.4-1.5 s each
// in the Python prototype: every centreline point sits 0.80-0.97 of the local inradius from the
// nearest wall, median 0.95-0.98 (1.0 = exactly centred).
std::vector<TunnelSpine> find_bore_spines_from_walls(const indexed_triangle_set& its,
                                                     double       spacing,
                                                     double       min_aspect,
                                                     int          max_tunnels,
                                                     std::string* note)
{
    std::vector<TunnelSpine> out;
    std::ostringstream       log;
    const size_t nf = its.indices.size();
    const size_t nv = its.vertices.size();
    if (nf < 4 || nv < 4)
        return out;

    auto vert = [&its](int i) { return Eigen::Vector3d(its.vertices[i].cast<double>()); };

    std::vector<Eigen::Vector3d> fn(nf), fc(nf);
    std::vector<double>          fa(nf);
    double          total_area = 0.0;
    Eigen::Vector3d bb_min = vert(0), bb_max = vert(0);
    for (size_t i = 0; i < nv; ++i) {
        bb_min = bb_min.cwiseMin(vert((int) i));
        bb_max = bb_max.cwiseMax(vert((int) i));
    }
    const double diag = (bb_max - bb_min).norm();
    for (size_t f = 0; f < nf; ++f) {
        const Eigen::Vector3d a = vert(its.indices[f][0]), b = vert(its.indices[f][1]), c = vert(its.indices[f][2]);
        const Eigen::Vector3d n = (b - a).cross(c - a);
        const double          l = n.norm();
        fa[f] = 0.5 * l;
        total_area += fa[f];
        fn[f] = l > 1e-18 ? Eigen::Vector3d(n / l) : Eigen::Vector3d::UnitZ();
        fc[f] = (a + b + c) / 3.0;
    }

    // 1. How much of each face's hemisphere is open. A fixed cosine-weighted pattern (van der
    //    Corput), so the same mesh always classifies the same way.
    const auto tree = AABBTreeIndirect::build_aabb_tree_over_indexed_triangle_set(its.vertices, its.indices);
    const int  K    = nf > 60000 ? 12 : 24;
    std::vector<Eigen::Vector3d> pat(K);
    for (int i = 0; i < K; ++i) {
        const double u = (i + 0.5) / K;
        double v = 0.0;
        for (unsigned bit = (unsigned) i, f = 0; bit; bit >>= 1, ++f)
            if (bit & 1u)
                v += std::ldexp(1.0, -(int) f - 1);
        const double th = 2.0 * M_PI * v, r = std::sqrt(u);
        pat[i] = Eigen::Vector3d(r * std::cos(th), r * std::sin(th), std::sqrt(std::max(0.0, 1.0 - u)));
    }
    std::vector<float> escape(nf, 1.f);
    const double       push = 1e-4 * diag;
    tbb::parallel_for(tbb::blocked_range<size_t>(0, nf, 256), [&](const tbb::blocked_range<size_t>& range) {
        for (size_t f = range.begin(); f < range.end(); ++f) {
            const Eigen::Vector3d& n = fn[f];
            const Eigen::Vector3d  a = std::abs(n.x()) < 0.9 ? Eigen::Vector3d::UnitX() : Eigen::Vector3d::UnitY();
            const Eigen::Vector3d  t = n.cross(a).normalized();
            const Eigen::Vector3d  b = n.cross(t);
            const Eigen::Vector3d  o = fc[f] + push * n;
            int hits = 0;
            for (int i = 0; i < K; ++i) {
                const Eigen::Vector3d d = pat[i].x() * t + pat[i].y() * b + pat[i].z() * n;
                igl::Hit<float>       hit;
                if (AABBTreeIndirect::intersect_ray_first_hit(its.vertices, its.indices, tree, o, d, hit))
                    ++hits;
            }
            escape[f] = 1.f - float(hits) / float(K);
        }
    });

    // 2. Face adjacency across shared edges.
    struct EdgeRef { uint64_t key; int face; };
    std::vector<EdgeRef> er;
    er.reserve(3 * nf);
    for (size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            uint32_t a = (uint32_t) its.indices[f][k], b = (uint32_t) its.indices[f][(k + 1) % 3];
            if (a > b)
                std::swap(a, b);
            er.push_back({(uint64_t(a) << 32) | b, (int) f});
        }
    std::sort(er.begin(), er.end(), [](const EdgeRef& x, const EdgeRef& y) { return x.key < y.key; });
    std::vector<std::vector<int>> fadj(nf);
    for (size_t i = 0; i < er.size();) {
        size_t j = i;
        while (j < er.size() && er[j].key == er[i].key)
            ++j;
        for (size_t p = i; p < j; ++p)
            for (size_t q = i; q < j; ++q)
                if (p != q)
                    fadj[er[p].face].push_back(er[q].face);
        i = j;
    }

    // 3. Inner = mostly enclosed. Two majority passes clear the speckle at fillets and openings.
    std::vector<char> inner(nf);
    for (size_t f = 0; f < nf; ++f)
        inner[f] = escape[f] < 0.5f;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<char> next(nf);
        for (size_t f = 0; f < nf; ++f) {
            int yes = inner[f], tot = 1;
            for (int g : fadj[f]) { yes += inner[g]; ++tot; }
            next[f] = (2 * yes > tot);
        }
        inner.swap(next);
    }

    // 4. Connected inner-wall patches, largest area first.
    std::vector<int> comp(nf, -1);
    int ncomp = 0;
    for (size_t f = 0; f < nf; ++f) {
        if (!inner[f] || comp[f] >= 0)
            continue;
        std::vector<int> stack{(int) f};
        comp[f] = ncomp;
        while (!stack.empty()) {
            const int u = stack.back();
            stack.pop_back();
            for (int g : fadj[u])
                if (inner[g] && comp[g] < 0) { comp[g] = ncomp; stack.push_back(g); }
        }
        ++ncomp;
    }
    std::vector<double> carea(ncomp, 0.0);
    for (size_t f = 0; f < nf; ++f)
        if (comp[f] >= 0)
            carea[comp[f]] += fa[f];
    std::vector<int> order(ncomp);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&carea](int a, int b) { return carea[a] > carea[b]; });
    size_t n_inner = 0;
    for (size_t f = 0; f < nf; ++f) n_inner += inner[f] ? 1 : 0;
    log << "[bore] " << n_inner << " / " << nf << " faces line an enclosed space, in " << ncomp << " patch(es)\n";

    // The full mesh's cross section at each ring gives the corridor radius the splice needs.
    std::optional<Slicer> slicer;
    Mesh                  pmesh;
    {
        Eigen::MatrixXd V(nv, 3);
        Eigen::MatrixXi F(nf, 3);
        for (size_t i = 0; i < nv; ++i) V.row(i) = vert((int) i).transpose();
        for (size_t f = 0; f < nf; ++f) F.row(f) = Eigen::RowVector3i(its.indices[f][0], its.indices[f][1], its.indices[f][2]);
        pmesh = build_prepared_mesh(V, F);
        if (CGAL::is_closed(pmesh))
            slicer.emplace(pmesh);
    }

    struct Ring { Eigen::Vector3d c, n; double inr{0.0}, maxr{0.0}; };
    // Plane by PCA, points ordered by angle in it, shoelace area centroid. The mean of the ring
    // points would do for a circle but is pulled toward wherever the mesh is dense (fillets).
    auto fit_ring = [](const std::vector<Eigen::Vector3d>& P, Ring& R) -> bool {
        if (P.size() < 3)
            return false;
        Eigen::Vector3d mu = Eigen::Vector3d::Zero();
        for (const auto& p : P) mu += p;
        mu /= (double) P.size();
        Eigen::Matrix3d C = Eigen::Matrix3d::Zero();
        for (const auto& p : P) { const Eigen::Vector3d d = p - mu; C += d * d.transpose(); }
        Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(C);
        if (es.info() != Eigen::Success)
            return false;
        const Eigen::Vector3d n = es.eigenvectors().col(0), u = es.eigenvectors().col(2);
        const Eigen::Vector3d v = n.cross(u);
        std::vector<std::pair<double, int>> ang;
        ang.reserve(P.size());
        for (size_t i = 0; i < P.size(); ++i) {
            const Eigen::Vector3d d = P[i] - mu;
            ang.emplace_back(std::atan2(d.dot(v), d.dot(u)), (int) i);
        }
        std::sort(ang.begin(), ang.end());
        double a2 = 0.0, cx = 0.0, cy = 0.0;
        for (size_t i = 0; i < ang.size(); ++i) {
            const Eigen::Vector3d d0 = P[ang[i].second] - mu, d1 = P[ang[(i + 1) % ang.size()].second] - mu;
            const double x0 = d0.dot(u), y0 = d0.dot(v), x1 = d1.dot(u), y1 = d1.dot(v);
            const double cr = x0 * y1 - x1 * y0;
            a2 += cr;
            cx += cr * (x0 + x1);
            cy += cr * (y0 + y1);
        }
        if (std::abs(a2) < 1e-9)
            return false;
        R.c    = mu + (cx / (3.0 * a2)) * u + (cy / (3.0 * a2)) * v;
        R.n    = n;
        R.inr  = std::numeric_limits<double>::max();
        R.maxr = 0.0;
        for (const auto& p : P) {
            const double d = (p - R.c).norm();
            R.inr  = std::min(R.inr, d);
            R.maxr = std::max(R.maxr, d);
        }
        return true;
    };
    auto chain_length = [](const std::vector<Eigen::Vector3d>& pts) {
        double l = 0.0;
        for (size_t i = 0; i + 1 < pts.size(); ++i) l += (pts[i + 1] - pts[i]).norm();
        return l;
    };

    for (int ci : order) {
        if ((int) out.size() >= max_tunnels)
            break;
        if (carea[ci] < 0.05 * total_area)
            break;

        std::vector<int> faces;
        for (size_t f = 0; f < nf; ++f)
            if (comp[f] == ci)
                faces.push_back((int) f);
        std::vector<int> gl(nv, -1), lv;
        for (int f : faces)
            for (int k = 0; k < 3; ++k) {
                const int g = its.indices[f][k];
                if (gl[g] < 0) { gl[g] = (int) lv.size(); lv.push_back(g); }
            }
        const int m = (int) lv.size();

        // Patch edges; those with one incident patch face are the boundary.
        std::vector<uint64_t> ek;
        ek.reserve(3 * faces.size());
        for (int f : faces)
            for (int k = 0; k < 3; ++k) {
                uint32_t a = (uint32_t) gl[its.indices[f][k]], b = (uint32_t) gl[its.indices[f][(k + 1) % 3]];
                if (a > b) std::swap(a, b);
                ek.push_back((uint64_t(a) << 32) | b);
            }
        std::sort(ek.begin(), ek.end());
        std::vector<std::pair<int, int>> edges, bedges;
        for (size_t i = 0; i < ek.size();) {
            size_t j = i;
            while (j < ek.size() && ek[j] == ek[i]) ++j;
            const std::pair<int, int> e((int) (ek[i] >> 32), (int) (ek[i] & 0xffffffffu));
            edges.push_back(e);
            if (j - i == 1) bedges.push_back(e);
            i = j;
        }
        std::vector<int> uf(m);
        std::iota(uf.begin(), uf.end(), 0);
        std::function<int(int)> find = [&](int x) { return uf[x] == x ? x : uf[x] = find(uf[x]); };
        for (const auto& e : bedges) uf[find(e.first)] = find(e.second);
        std::vector<int> loop_of(m, -1);
        std::map<int, int> loop_id;
        for (const auto& e : bedges)
            for (int x : {e.first, e.second}) {
                const int r = find(x);
                auto it = loop_id.find(r);
                if (it == loop_id.end()) it = loop_id.emplace(r, (int) loop_id.size()).first;
                loop_of[x] = it->second;
            }
        const int nloops = (int) loop_id.size();
        std::vector<int> loop_size(nloops, 0);
        for (int x = 0; x < m; ++x) if (loop_of[x] >= 0) ++loop_size[loop_of[x]];
        log << "[bore] patch: " << faces.size() << " faces, " << (int) (100.0 * carea[ci] / total_area)
            << "% of the surface, " << nloops << " opening(s)";
        for (int s : loop_size) log << " " << s;
        log << "\n";
        if (nloops != 2 || loop_size[0] < 6 || loop_size[1] < 6) {
            log << "[bore]   " << (nloops < 2 ? "a blind pocket" : "a branched space") << " -- not traced here\n";
            continue;
        }

        // Harmonic field, 0 on one opening and 1 on the other. Cotangent weights, clamped at zero
        // and with a floor so an all-obtuse vertex cannot make the system singular.
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(12 * faces.size());
        std::vector<double> dw(m, 0.0);
        for (int f : faces) {
            int             li[3];
            Eigen::Vector3d P[3];
            for (int k = 0; k < 3; ++k) { li[k] = gl[its.indices[f][k]]; P[k] = vert(its.indices[f][k]); }
            for (int k = 0; k < 3; ++k) {
                const int i = li[k], j = li[(k + 1) % 3], o = (k + 2) % 3;
                const Eigen::Vector3d e1 = P[k] - P[o], e2 = P[(k + 1) % 3] - P[o];
                double w = e1.dot(e2) / std::max(e1.cross(e2).norm(), 1e-12);
                w = 0.5 * std::max(0.0, w) + 1e-6;
                trip.emplace_back(i, j, -w);
                trip.emplace_back(j, i, -w);
                dw[i] += w;
                dw[j] += w;
            }
        }
        for (int i = 0; i < m; ++i) trip.emplace_back(i, i, dw[i]);
        Eigen::SparseMatrix<double> L(m, m);
        L.setFromTriplets(trip.begin(), trip.end());
        Eigen::VectorXd  phi = Eigen::VectorXd::Zero(m);
        std::vector<int> fidx(m, -1), flist;
        for (int x = 0; x < m; ++x) {
            if (loop_of[x] == 1) phi[x] = 1.0;
            if (loop_of[x] < 0) { fidx[x] = (int) flist.size(); flist.push_back(x); }
        }
        if (!flist.empty()) {
            const int nfree = (int) flist.size();
            std::vector<Eigen::Triplet<double>> tff;
            Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nfree);
            for (int k = 0; k < L.outerSize(); ++k)
                for (Eigen::SparseMatrix<double>::InnerIterator it(L, k); it; ++it) {
                    const int r = (int) it.row(), c = (int) it.col();
                    if (fidx[r] < 0) continue;
                    if (fidx[c] >= 0) tff.emplace_back(fidx[r], fidx[c], it.value());
                    else rhs[fidx[r]] -= it.value() * phi[c];
                }
            Eigen::SparseMatrix<double> Lff(nfree, nfree);
            Lff.setFromTriplets(tff.begin(), tff.end());
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(Lff);
            if (solver.info() != Eigen::Success) { log << "[bore]   harmonic factorisation failed -- skipped\n"; continue; }
            const Eigen::VectorXd x = solver.solve(rhs);
            if (solver.info() != Eigen::Success) { log << "[bore]   harmonic solve failed -- skipped\n"; continue; }
            for (int i = 0; i < nfree; ++i) phi[flist[i]] = x[i];
        }

        // Level-set rings at t_k = (k + 0.5) / nl: every patch edge contributes one point per
        // level it crosses.
        auto ring_points = [&](int nl, std::vector<std::vector<Eigen::Vector3d>>& rings) {
            rings.assign(nl, {});
            for (auto e : edges) {
                int a = e.first, b = e.second;
                double pa = phi[a], pb = phi[b];
                if (pa > pb) { std::swap(pa, pb); std::swap(a, b); }
                if (pb - pa < 1e-15) continue;
                const int k0 = std::max(0, (int) std::ceil(pa * nl - 0.5)), k1 = std::min(nl - 1, (int) std::floor(pb * nl - 0.5));
                const Eigen::Vector3d A = vert(lv[a]), B = vert(lv[b]);
                for (int k = k0; k <= k1; ++k) {
                    const double t = (k + 0.5) / nl;
                    if (t <= pa || t > pb) continue;
                    rings[k].push_back(A + ((t - pa) / (pb - pa)) * (B - A));
                }
            }
        };
        std::vector<Eigen::Vector3d> loopA, loopB;
        for (int x = 0; x < m; ++x) {
            if (loop_of[x] == 0) loopA.push_back(vert(lv[x]));
            else if (loop_of[x] == 1) loopB.push_back(vert(lv[x]));
        }
        Ring rA, rB;
        if (!fit_ring(loopA, rA) || !fit_ring(loopB, rB)) { log << "[bore]   degenerate opening -- skipped\n"; continue; }

        std::vector<std::vector<Eigen::Vector3d>> rings;
        std::vector<Eigen::Vector3d>              centres;
        auto build_chain = [&](int nl, std::vector<Ring>& chain) {
            ring_points(nl, rings);
            chain.clear();
            chain.push_back(rA);
            for (const auto& rp : rings) { Ring R; if (fit_ring(rp, R)) chain.push_back(R); }
            chain.push_back(rB);
            centres.clear();
            for (const auto& R : chain) centres.push_back(R.c);
        };
        std::vector<Ring> chain;
        build_chain(40, chain);
        double len = chain_length(centres);
        const double sp = spacing > 0.0 ? spacing : std::max(0.5, len / 60.0);
        build_chain(std::max(8, std::min(400, (int) std::lround(len / sp))), chain);
        len = chain_length(centres);
        double mean_d = 0.0;
        for (const auto& R : chain) mean_d += 2.0 * R.inr;
        mean_d /= (double) chain.size();
        const double aspect = mean_d > 1e-9 ? len / mean_d : 0.0;
        log << "[bore]   through-bore: centreline " << len << " mm over " << chain.size() << " rings, mean bore "
            << mean_d << " mm, aspect " << aspect << "\n";
        if (min_aspect > 0.0 && aspect < min_aspect) {
            log << "[bore]   rejected: aspect " << aspect << " < " << min_aspect << " -- a ring, not a pipe; following the material instead\n";
            continue;
        }

        TunnelSpine spn;
        spn.axis  = (rB.c - rA.c).norm() > 1e-9 ? Eigen::Vector3d((rB.c - rA.c).normalized()) : Eigen::Vector3d::UnitZ();
        spn.depth = len;
        spn.width = mean_d;
        for (size_t i = 0; i < chain.size(); ++i) {
            const Ring& R = chain[i];
            // Corridor radius from the OUTER contour here, not the inner ring: the wall skeleton the
            // splice has to swallow sits mid-wall, outside the ring.
            double rr = 1.5 * R.maxr;
            if (slicer) {
                const Section s = slice_section(*slicer, R.c, R.n);
                bool      contained = false;
                const int li        = s.empty() ? -1 : outer_loop_at(s, Eigen::Vector2d::Zero(), &contained);
                if (li >= 0 && contained)
                    rr = s.loops[li].max_r;
            }
            spn.points.push_back(R.c);
            spn.radius.push_back(rr);
        }
        out.push_back(std::move(spn));
    }

    std::cout << log.str();
    if (note)
        *note = log.str();
    return out;
}

} // namespace Slic3r
