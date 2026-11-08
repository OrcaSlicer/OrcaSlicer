#include "SkeletonBuild.hpp"
#include "QuadricEdgeCollapse.hpp"
#include "TriangleMeshSlicer.hpp"

#include <algorithm>
#include <boost/log/trivial.hpp>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <queue>
#include <set>
#include <sstream>

namespace Slic3r {

// ── cut ordering ──────────────────────────────────────────────────────────────

std::vector<std::pair<int,int>> build_skeleton_cut_edges(const SkeletonDAG& dag, double root_lean_deg)
{
    const int n = dag.num_nodes();
    if (n == 0)
        return {};

    // The seat under a planted foot: cuts there are dropped (see skeleton_root_ramp()).
    std::vector<std::vector<int>> adj(n);
    std::vector<int>              sources;
    for (int u = 0; u < n; ++u) {
        for (int v : dag.succ[u]) {
            adj[u].push_back(v);
            adj[v].push_back(u);
        }
        if (dag.pred[u].empty())
            sources.push_back(u);
    }
    const SkeletonRootRamp ramp = skeleton_root_ramp(dag.nodes, adj, sources, root_lean_deg);
    const auto in_seat = [&ramp](int u, int v) {
        const int par = ramp.s[u] <= ramp.s[v] ? u : v;
        return ramp.foot[par] && std::isfinite(ramp.s[u]) && std::isfinite(ramp.s[v]) &&
               0.5 * (ramp.s[u] + ramp.s[v]) < ramp.seat[par];
    };

    // One BFS queue per DAG root, round-robin interleaved so parallel branches zigzag.
    std::vector<std::queue<int>> qs;
    std::vector<bool> visited(n, false);
    for (int i = 0; i < n; ++i) {
        if (dag.pred[i].empty()) {
            qs.emplace_back();
            qs.back().push(i);
            visited[i] = true;
        }
    }
    if (qs.empty())
        return {};

    std::vector<std::pair<int,int>> edges;
    edges.reserve(std::max(0, n - 1));

    bool any = true;
    while (any) {
        any = false;
        for (auto& q : qs) {
            if (q.empty())
                continue;
            any = true;
            const int u = q.front();
            q.pop();
            for (int v : dag.succ[u]) {
                if (!visited[v]) {
                    visited[v] = true;
                    if (!in_seat(u, v))
                        edges.emplace_back(u, v);
                    q.push(v);
                }
            }
        }
    }
    return edges;
}

// ── topology from a centreline ────────────────────────────────────────────────

SkeletonTopology rebuild_skeleton_topology(const SkeletonGraph& local, const Transform3d& world,
                                           const indexed_triangle_set& its_local, const SkeletonParams& p)
{
    SkeletonTopology t;
    if (local.num_nodes() == 0)
        return t;

    const bool    world_is_identity = world.matrix().isApprox(Transform3d::Identity().matrix());
    SkeletonGraph G                 = local;
    if (!world_is_identity)
        for (auto& n : G.nodes)
            n = world * n;

    t.roots             = find_skeleton_roots(G);
    t.dag               = orient_skeleton_as_dag(G, t.roots);
    t.convergence_nodes = p.correct_convergence ? find_convergence_nodes(t.dag) : std::vector<int>{};
    t.tangents          = compute_skeleton_tangents(G, t.dag, t.roots, t.convergence_nodes, (double) p.tangent_W,
                                                    (double) p.root_lean, (double) p.root_ramp);
    t.cut_edges         = build_skeleton_cut_edges(t.dag, (double) p.root_lean);
    for (int r : t.roots)
        if (skeleton_foot_top(G.nodes, G.adj, r) >= 0) {
            t.foot_node = r;
            break;
        }

    // Back to the object's local frame. Node positions take the full inverse; tangents take the
    // inverse of the linear part only, because the consumer multiplies them by world.linear()
    // again (perform_skeleton_cut: `world.linear() * m_skel_tangents.get(...)`), so storing a
    // world-space tangent here would apply the rotation twice.
    if (!world_is_identity) {
        t.dag.nodes                   = local.nodes;
        const Eigen::Matrix3d inv_lin = world.linear().inverse();
        for (auto* ts : {&t.tangents.smoothed, &t.tangents.raw})
            for (auto& v : *ts) {
                const Vec3d  w = inv_lin * v;
                const double l = w.norm();
                v = (l > 1e-12) ? Vec3d(w / l) : Vec3d::UnitZ();
            }
    }
    // Last, and in the object-local frame: these are the planes perform_skeleton_cut() authors.
    if (p.uncross_planes)
        t.planes_reaimed = uncross_skeleton_cut_planes(its_local, t.dag, t.cut_edges, t.tangents,
                                                       (double) p.plane_gap, &t.max_reaim_deg);

    // How far each finished plane tilts from the bed normal, for the dump and the corpus checks.
    t.cut_tilt_deg.reserve(t.cut_edges.size());
    for (const auto& [par, child] : t.cut_edges) {
        const Vec3d n = (world.linear() * t.tangents.get(par, child)).normalized();
        t.cut_tilt_deg.push_back(std::acos(std::clamp(n.z(), -1.0, 1.0)) * 180.0 / M_PI);
    }
    return t;
}

// ── cut planes that do not cross inside the part ──────────────────────────────

SkeletonCutPlane skeleton_edge_cut_plane(const SkeletonDAG& dag, const SkeletonTangents& tangents,
                                         int par, int child)
{
    return { (dag.nodes[par] + dag.nodes[child]) * 0.5, tangents.get(par, child).normalized() };
}

// Outer contour, in 3D, of the cut face plane (p, n) makes through the part at p: the cross-section
// region whose contour encloses p, else the nearest. Empty when the plane misses the part. The
// contour alone is enough for the crossing test, because a linear function's extremes over a
// region with holes lie on its outer contour.
static std::vector<Vec3d> own_cut_face(const indexed_triangle_set& mesh, const Vec3d& p, const Vec3d& n)
{
    const Eigen::Quaterniond to_z = Eigen::Quaterniond::FromTwoVectors(n, Vec3d::UnitZ());
    MeshSlicingParams params;
    params.trafo.rotate(to_z);
    const double z = n.dot(p);
    // The single-plane slice_mesh() builds edge ids only for the faces the plane crosses; the
    // slice_mesh_ex() path rebuilds them for the whole mesh on every call, which made this pass
    // cost seconds on a 65k-face intake (one slice per plane plus ~10 per re-aimed one).
    const Polygons polys = slice_mesh(mesh, float(z), params);
    if (polys.empty())
        return {};

    // The outer contour around p is the LARGEST loop enclosing it: a bore's centreline is also
    // enclosed by the bore's own loop. Containment ignores winding, so a mirrored mesh is fine.
    const Vec3d    pr  = to_z * p;
    const Point    pp  = Point::new_scale(pr.x(), pr.y());
    const Polygon *own = nullptr;
    const auto     enclose = [&polys, &own](const Point& q) {
        for (const Polygon& poly : polys)
            if (poly.contains(q) && (own == nullptr || std::abs(poly.area()) > std::abs(own->area())))
                own = &poly;
    };
    enclose(pp);
    if (own == nullptr) {
        // p fell just outside every loop (a centreline grazing the wall): take the nearest loop,
        // then whatever encloses it.
        double best = std::numeric_limits<double>::max();
        for (const Polygon& poly : polys)
            if (!poly.points.empty())
                if (const double d = poly.distance_to(pp); d < best) {
                    best = d;
                    own  = &poly;
                }
        if (own == nullptr)
            return {};
        enclose(own->points.front());
    }
    const Eigen::Matrix3d back = to_z.conjugate().toRotationMatrix();
    std::vector<Vec3d>    out;
    out.reserve(own->points.size());
    for (const Point& pt : own->points) {
        const Vec2d u = unscale(pt);
        out.push_back(back * Vec3d(u.x(), u.y(), z));
    }
    return out;
}

int uncross_skeleton_cut_planes(const indexed_triangle_set& mesh, const SkeletonDAG& dag,
                                const std::vector<std::pair<int,int>>& cut_edges,
                                SkeletonTangents& tangents, double min_gap, double* max_turn_deg)
{
    if (max_turn_deg)
        *max_turn_deg = 0.;
    const int n_cuts = (int) cut_edges.size();
    if (n_cuts == 0 || mesh.indices.empty())
        return 0;
    if (n_cuts > 64) {
        // Over the segmentation's 64-plane wall the cut is refused outright, so there is nothing to
        // protect -- and this pass is what costs a minute on a dense branched part (measured 65 s on
        // the 137k-face Intake_manifold, 89 cuts). It runs again once edits bring the count down.
        std::cout << "[uncross_cut_planes] skipped: " << n_cuts << " cuts is over the 64-plane limit\n";
        return 0;
    }

    // A cut's predecessor is the cut that ends at its parent node. cut_edges is a BFS tree, so each
    // node is the child of at most one cut, and root-first, so a predecessor always has the lower
    // index. Only a chain continuation is paired with its predecessor: where the path forks, the
    // branches leave the junction sideways or even backwards and the predecessor is not "the material
    // behind this plane" -- swinging a branch towards the trunk's normal measured up to 152 deg on
    // Intake_manifold, which is not a correction. Sibling crossings stay a known gap.
    std::vector<int> cut_into(dag.num_nodes(), -1);
    std::vector<int> cuts_from(dag.num_nodes(), 0);
    for (int k = 0; k < n_cuts; ++k) {
        cut_into[cut_edges[k].second] = k;
        ++cuts_from[cut_edges[k].first];
    }
    std::vector<int> pred(n_cuts, -1), succ(n_cuts, -1);
    for (int k = 0; k < n_cuts; ++k)
        if (const int par = cut_edges[k].first; cut_into[par] >= 0 && cuts_from[par] == 1) {
            pred[k]              = cut_into[par];
            succ[cut_into[par]]  = k;
        }

    std::vector<Vec3d>              point(n_cuts), tangent(n_cuts);
    std::vector<std::vector<Vec3d>> tangent_face(n_cuts);
    for (int k = 0; k < n_cuts; ++k) {
        const SkeletonCutPlane pl = skeleton_edge_cut_plane(dag, tangents, cut_edges[k].first, cut_edges[k].second);
        point[k]        = pl.point;
        tangent[k]      = pl.normal;
        tangent_face[k] = own_cut_face(mesh, pl.point, pl.normal);
    }

    // Cut b's face wholly behind cut f's plane, and f's face wholly in front of b's, by min_gap.
    const auto clear = [&point, min_gap](int b, const Vec3d& nb, const std::vector<Vec3d>& fb,
                                         int f, const Vec3d& nf, const std::vector<Vec3d>& ff) {
        const double df = nf.dot(point[f]);
        for (const Vec3d& x : fb)
            if (nf.dot(x) - df > -min_gap)
                return false;
        const double db = nb.dot(point[b]);
        for (const Vec3d& x : ff)
            if (nb.dot(x) - db < min_gap)
                return false;
        return true;
    };

    // One greedy pass over the chains. Forward, each cut is checked against its already-final
    // PREDECESSOR and may only lag it; backward, against its already-final SUCCESSOR and may only
    // lead it. A crossing cut swings from the neighbour's normal (t = 0, parallel, which can never
    // cross) towards its own current normal (t = 1) and keeps the furthest swing that is still
    // clear -- kept even if the spine advanced less than min_gap there, as the best on offer.
    const auto sweep = [&](std::vector<Vec3d>& n, std::vector<std::vector<Vec3d>>& face, bool forward) {
        for (int s = 0; s < n_cuts; ++s) {
            const int k = forward ? s : n_cuts - 1 - s;
            const int o = forward ? pred[k] : succ[k];
            if (o < 0 || face[o].empty() || face[k].empty())
                continue;
            const auto ok = [&](const Vec3d& nk, const std::vector<Vec3d>& fk) {
                return forward ? clear(o, n[o], face[o], k, nk, fk) : clear(k, nk, fk, o, n[o], face[o]);
            };
            if (ok(n[k], face[k]))
                continue;
            const Vec3d  from  = n[o];
            const Vec3d  to    = n[k];
            const double angle = std::acos(std::clamp(from.dot(to), -1., 1.));
            if (angle > M_PI - 1e-3)
                continue; // the spine doubles back on itself here; no swing direction is defined
            const auto swing = [&](double t) -> Vec3d {
                // Separate returns rather than a conditional: normalized() yields an aligned
                // Eigen::Matrix<double,3,1> while `to` is Vec3d (Eigen::DontAlign). Each
                // converts to the other, so clang rejects the conditional as ambiguous. Each
                // return converts to the lambda's Vec3d on its own.
                if (angle < 1e-9)
                    return to;
                return Vec3d((std::sin((1. - t) * angle) * from + std::sin(t * angle) * to) /
                             std::sin(angle))
                    .normalized();
            };
            double             lo = 0., hi = 1.;
            Vec3d              best_n    = from;
            std::vector<Vec3d> best_face = own_cut_face(mesh, point[k], from);
            // 10 halvings put the swing within angle/1024 of the limit: < 0.1 deg for a 90 deg bend.
            for (int it = 0; it < 10; ++it) {
                const double       t  = 0.5 * (lo + hi);
                const Vec3d        nt = swing(t);
                std::vector<Vec3d> ft = own_cut_face(mesh, point[k], nt);
                if (!ft.empty() && ok(nt, ft)) {
                    lo        = t;
                    best_n    = nt;
                    best_face = std::move(ft);
                } else
                    hi = t;
            }
            n[k]    = best_n;
            face[k] = std::move(best_face);
        }
    };

    // Either pass alone parks the whole correction on ONE side of the trouble: forward, the planes
    // after a tight bend lag behind it; backward, the planes before it turn early. Averaging the two
    // spreads it over both sides and halves the worst turn (driver_intake: 14.1 deg on 16 planes
    // either way, 7.1 deg on 27 averaged, still 0 crossings). For a bound on the turn per step the
    // average of two clear schedules is itself clear; a final forward pass repairs whatever the
    // real, face-dependent constraint disagrees with.
    std::vector<Vec3d>              fwd = tangent, bwd = tangent;
    std::vector<std::vector<Vec3d>> fwd_face = tangent_face, bwd_face = tangent_face;
    sweep(fwd, fwd_face, true);
    sweep(bwd, bwd_face, false);
    std::vector<Vec3d>              normal(n_cuts);
    std::vector<std::vector<Vec3d>> face(n_cuts);
    for (int k = 0; k < n_cuts; ++k) {
        if (fwd[k] == tangent[k] && bwd[k] == tangent[k]) {
            normal[k] = tangent[k];
            face[k]   = tangent_face[k];
            continue;
        }
        const Vec3d sum = fwd[k] + bwd[k];
        normal[k]       = sum.norm() > 1e-6 ? Vec3d(sum.normalized()) : fwd[k];
        face[k]         = own_cut_face(mesh, point[k], normal[k]);
    }
    sweep(normal, face, true);

    int    changed = 0;
    double worst   = 0.;
    for (int k = 0; k < n_cuts; ++k) {
        if (normal[k] == tangent[k])
            continue;
        const auto [par, child] = cut_edges[k];
        const auto slot = tangents.edge_index.find({std::min(par, child), std::max(par, child)});
        if (slot == tangents.edge_index.end() || slot->second >= (int) tangents.smoothed.size())
            continue; // get() falls back to +Z for an unknown edge; nothing here to re-aim
        tangents.smoothed[slot->second] = normal[k];
        ++changed;
        worst = std::max(worst, std::acos(std::clamp(normal[k].dot(tangent[k]), -1., 1.)) * 180. / M_PI);
    }
    if (max_turn_deg)
        *max_turn_deg = worst;
    std::cout << "[uncross_cut_planes] " << changed << " / " << n_cuts
              << " cut plane(s) turned off the tangent so no two neighbours cross inside the part"
              << " (largest turn " << worst << " deg, gap " << min_gap << " mm)\n";
    return changed;
}

int skeleton_cycle_count(const SkeletonGraph& G)
{
    const int n = G.num_nodes();
    if (n == 0)
        return 0;

    int n_edges = 0;
    for (const auto& a : G.adj)
        n_edges += (int) a.size();
    n_edges /= 2;

    // connected components
    std::vector<bool> seen(n, false);
    int comps = 0;
    for (int i = 0; i < n; ++i) {
        if (seen[i])
            continue;
        ++comps;
        std::queue<int> q;
        q.push(i);
        seen[i] = true;
        while (!q.empty()) {
            const int u = q.front();
            q.pop();
            for (int v : G.adj[u])
                if (!seen[v]) { seen[v] = true; q.push(v); }
        }
    }
    return n_edges - n + comps;
}

// ── the pipeline ──────────────────────────────────────────────────────────────

SkeletonResult build_skeleton(const indexed_triangle_set& its_local,
                              const SkeletonParams&       p,
                              const Transform3d&          world,
                              const SkeletonProgress*     progress)
{
    SkeletonResult r;
    if (its_local.vertices.empty() || its_local.indices.empty()) {
        r.note = "empty mesh";
        return r;
    }

    // Work in world space; see the header note. Only the vertices need moving — face indices and
    // winding are unchanged for a non-mirroring transform, and a mirroring one is handled by the
    // outward-orientation fix inside build_cgal_mesh().
    const bool           world_is_identity = world.matrix().isApprox(Transform3d::Identity().matrix());
    indexed_triangle_set its_world;
    if (!world_is_identity) {
        its_world = its_local;
        for (auto& v : its_world.vertices)
            v = (world * v.cast<double>()).cast<float>();
    }
    const indexed_triangle_set& its = world_is_identity ? its_local : its_world;

    // Phase timings. Skeletonization cost scales steeply with face count and there are three
    // candidates for a slow run (skeletonize / bore tracing / the rest), so the log says which.
    using Clock = std::chrono::steady_clock;
    const auto  t_start = Clock::now();
    auto        t_mark  = t_start;
    // Stage boundaries double as the progress and cancellation checkpoints — the stages
    // themselves are single uninterruptible CGAL calls, so this is as fine-grained as it gets.
    int pct = 0;
    auto lap = [&t_mark, &pct, progress](const char* what, int pct_after = -1) {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - t_mark).count();
        std::cout << "[skeleton timing] " << what << ": " << (int) ms << " ms\n";
        t_mark = now;
        if (pct_after >= 0)
            pct = pct_after;
        if (progress && progress->report)
            progress->report(pct, what);
        return ms;
    };
    auto canceled = [progress]() { return progress && progress->canceled && progress->canceled(); };

    // ── decimate ──
    // MCF cost grows steeply with triangle count while the skeleton it produces is a coarse
    // 1D curve, so density past a few thousand faces buys nothing. Measured on a 42908-face
    // intake: minutes at full density. The decimated copy is used for skeletonization only —
    // bore tracing and bed snapping still slice the ORIGINAL mesh, so accuracy is unaffected.
    const indexed_triangle_set* mesh_for_skel = &its;
    indexed_triangle_set        decimated;
    if (p.decimate_faces > 0 && (int) its.indices.size() > p.decimate_faces) {
        decimated = its;
        uint32_t target = (uint32_t) p.decimate_faces;
        try {
            its_quadric_edge_collapse(decimated, target);
            if (!decimated.indices.empty() && !decimated.vertices.empty()) {
                std::cout << "[skeleton] decimated " << its.indices.size() << " -> "
                          << decimated.indices.size() << " faces for skeletonization\n";
                mesh_for_skel = &decimated;
            }
        } catch (const std::exception& e) {
            std::cout << "[skeleton] decimation failed (" << e.what()
                      << "); skeletonizing at full density\n";
        }
        lap("decimate", 10);
    }

    auto V_f = trianglemesh_to_eigen_vertices(its);
    auto F   = trianglemesh_to_eigen_faces(its);
    Eigen::MatrixXd V = V_f.cast<double>();

    auto Vs_f = trianglemesh_to_eigen_vertices(*mesh_for_skel);
    auto Fs   = trianglemesh_to_eigen_faces(*mesh_for_skel);
    Eigen::MatrixXd Vs = Vs_f.cast<double>();

    const Eigen::Vector3d bb_min = V.colwise().minCoeff().transpose();
    const Eigen::Vector3d bb_max = V.colwise().maxCoeff().transpose();
    const double diag = (bb_max - bb_min).norm();

    r.genus = mesh_genus(Vs, Fs);

    // MCF needs a closed, valid surface (CGAL crashes on others: a reported macOS crash in the mean curvature
    // flow); mesh_genus() checks exactly that and gives -1 otherwise. Such a mesh takes the voxel skeleton.
    const bool mcf = p.method == 0 && r.genus >= 0;
    if (p.method == 0 && !mcf)
        BOOST_LOG_TRIVIAL(warning) << "skeleton: the mesh is not a closed surface; using the voxel skeleton";
    if (mcf)
        r.graph = skeletonize_mesh(Vs, Fs, (double) p.quality_speed, (double) p.min_edge_factor,
                                   (double) p.medial_centering);
    else
        r.graph = skeletonize_mesh_voxel(Vs, Fs, p.voxel_resolution);
    lap(mcf ? "skeletonize (MCF)" : "skeletonize (voxel)", 55);
    if (canceled()) { r.note = "canceled"; return r; }

    r.cycles_raw = skeleton_cycle_count(r.graph);

    // ── graph cleaning ──
    // These exist to tame MCF noise, so they run BEFORE a traced bore centreline is
    // spliced in: Laplacian smoothing pulls a curve toward its chord, and the traced
    // centreline of a bend is already exact — smoothing it would only flatten the arc.
    if (p.resample_edge_len > 0.f)
        r.graph = resample_skeleton_uniform(r.graph, (double) p.resample_edge_len);
    // F is an Eigen::Map over the mesh's index buffer. It binds to the const& parameters
    // elsewhere by way of a temporary, but a pointer needs a real MatrixXi to point at.
    const Eigen::MatrixXi F_mat = F;
    smooth_skeleton_graph(r.graph, p.smooth_iterations, p.smooth_alpha, true, &V, &F_mat);
    if (p.collapse_junctions)
        r.graph = collapse_nearby_junctions(r.graph);
    if (p.prune_leaves > 0.f)
        r.graph = prune_short_leaves(r.graph, (double) p.prune_leaves);
    lap("clean", 60);
    if (canceled()) { r.note = "canceled"; return r; }

    // ── hollow geometry: trace bores and replace the wall skeleton with the axis ──
    if (p.tunnel_fill != TunnelFill::Off) {
        const double station = p.tunnel_station > 0.f ? (double) p.tunnel_station
                                                      : std::max(0.5, diag / 120.0);
        const double aspect  = p.tunnel_fill == TunnelFill::On ? 0.0 : (double) p.tunnel_min_aspect;
        // The inner wall first: it follows a bore round a bend and takes any cross-section. The
        // straight-axis scanner stays as the fallback for blind pockets and for a mesh whose inner
        // wall cannot be told from its outside.
        std::string wall_note;
        r.tunnels = find_bore_spines_from_walls(its, p.resample_edge_len > 0.f ? (double) p.resample_edge_len : 0.0,
                                                aspect, 4, &wall_note);
        const bool from_walls = !r.tunnels.empty();
        if (!from_walls)
            r.tunnels = find_tunnel_spines(V, F, station, aspect);
        if (!r.tunnels.empty()) {
            r.graph = splice_tunnel_spines(r.graph, r.tunnels);
            std::ostringstream os;
            os << r.tunnels.size() << " bore(s) traced" << (from_walls ? " from the inner wall" : " by axis scan") << ":";
            for (const auto& t : r.tunnels)
                os << " [depth=" << t.depth << " width=" << t.width
                   << " aspect=" << t.aspect() << " pts=" << t.points.size() << "]";
            r.note = os.str();
        } else {
            r.note = "no bore accepted";
        }
        lap("bore tracing", 85);
        if (canceled()) { r.note = "canceled"; return r; }
    } else {
        r.note = "tunnel handling off";
    }

    // ── bed ──
    double      bed_z = 0.0;
    BedRootPlan bed_plan;
    r.snapped_nodes = snap_to_bed(r.graph, V, F, (double) p.bed_snap_band, &bed_z, p.bed_root, &bed_plan);
    r.bed_z     = bed_z;
    r.root_mode = (bed_plan.use_middle ? "middle" : "tips") +
                  (bed_plan.reason.empty() ? std::string() : " (" + bed_plan.reason + ")");
    lap("bed snap", 92);
    if (canceled()) { r.note = "canceled"; return r; }

    // ── extremities ──
    // Undo MCF's tip retraction — but only at the ends the bed snap did not already settle,
    // which is why this runs AFTER it and not before.
    //
    // The two stages reach an end differently on purpose. snap_to_bed drops a tip onto the bed
    // plane, so the branch arrives perpendicular to it and the first cut is flat: the base
    // chunk sits square and the print starts on a clean vertical. extend_skeleton_tips follows
    // the cross-section centroid, which is the right thing in free space but drifts sideways
    // against a face you want to start flat on — run first, it walked the trunk tip of a
    // trident down AND 7 mm sideways, leaving a kinked, diagonal start where there had been a
    // straight one.
    //
    // Ordered this way each stage owns the ends it is good at: the bed snap grounds feet
    // straight, and extension reaches the tips that have no bed to snap to (a trident's prongs
    // stop 13-24 mm short of the top, where nothing else can help them). A tip already sitting
    // on the bed fails on its first step here and is left alone.
    //
    // Still after splicing either way, so tips the bore corridor is about to delete are never
    // extended first.
    {
        const int n_ext = extend_skeleton_tips(r.graph, V, F);
        if (n_ext > 0)
            lap("extend tips", 94);
    }

    // Normalise node spacing again: the traced centreline comes in at the march step
    // (sub-millimetre), and cut planes land at edge midpoints, so segment height is edge
    // length. Arc-length resampling preserves the curve, unlike the smoothing above.
    if (p.resample_edge_len > 0.f)
        r.graph = resample_skeleton_uniform(r.graph, (double) p.resample_edge_len);

    r.dag = orient_skeleton_as_dag(r.graph, find_skeleton_roots(r.graph));
    if (p.collapse_to_spine) {
        r.graph = collapse_skeleton_to_spine(r.graph, p.smooth_iterations, p.smooth_alpha, &V, &F_mat);
        r.dag   = orient_skeleton_as_dag(r.graph, find_skeleton_roots(r.graph));
    }
    if (p.resample_edge_len > 0.f) {
        r.graph = resample_skeleton_graph(r.graph, (double) p.resample_edge_len);
        r.dag   = orient_skeleton_as_dag(r.graph, find_skeleton_roots(r.graph));
    }

    // ── middle root ──
    // After the last resample, so the foot stays ONE vertical edge (it is never cut; a subdivided
    // foot would put a stack of flat planes through the bottom of the part).
    if (bed_plan.use_middle && plant_middle_root(r.graph, bed_plan) < 0)
        r.root_mode = "tips (middle planned, but no foot could be planted)";

    r.cycles_final = skeleton_cycle_count(r.graph);

    // Back to the object's local frame, then everything derived from the graph -- through the same
    // function the gizmo uses after an edit, so both agree on the root and the planes.
    if (!world_is_identity) {
        const Transform3d inv = world.inverse();
        for (auto& n : r.graph.nodes)
            n = inv * n;
        for (auto& sp : r.tunnels)
            for (auto& q : sp.points)
                q = inv * q;
    }
    {
        SkeletonTopology t  = rebuild_skeleton_topology(r.graph, world, its_local, p);
        r.dag               = std::move(t.dag);
        r.tangents          = std::move(t.tangents);
        r.roots             = std::move(t.roots);
        r.convergence_nodes = std::move(t.convergence_nodes);
        r.cut_edges         = std::move(t.cut_edges);
        r.cut_tilt_deg      = std::move(t.cut_tilt_deg);
        r.foot_node         = t.foot_node;
        r.planes_reaimed    = t.planes_reaimed;
        r.max_reaim_deg     = t.max_reaim_deg;
    }
    lap("dag + tangents", 100);
    r.build_ms = std::chrono::duration<double, std::milli>(Clock::now() - t_start).count();
    std::cout << "[skeleton timing] TOTAL: " << (int) r.build_ms << " ms\n";
    return r;
}

// ── parameter string ──────────────────────────────────────────────────────────

bool parse_skeleton_params(const std::string& spec, SkeletonParams& out, std::string& error)
{
    auto trim = [](std::string s) {
        const auto b = s.find_first_not_of(" \t");
        const auto e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };

    std::stringstream ss(spec);
    std::string item;
    while (std::getline(ss, item, ';')) {
        item = trim(item);
        if (item.empty())
            continue;
        const auto eq = item.find('=');
        if (eq == std::string::npos) {
            error = "expected key=value, got '" + item + "'";
            return false;
        }
        // Case-folded so `tangent_W` and `tangent_w` both land: the struct field is spelled
        // with a capital W and the key with a lowercase one, and that mismatch silently
        // failed every run of a sweep that was measuring whether the control did anything.
        std::string k = trim(item.substr(0, eq));
        std::transform(k.begin(), k.end(), k.begin(),
                       [](unsigned char c) { return (char) std::tolower(c); });
        const std::string v = trim(item.substr(eq + 1));
        try {
            if      (k == "method")             out.method             = std::stoi(v);
            else if (k == "quality_speed")      out.quality_speed      = std::stof(v);
            else if (k == "min_edge_factor")    out.min_edge_factor    = std::stof(v);
            else if (k == "medial_centering")   out.medial_centering   = std::stof(v);
            else if (k == "voxel_resolution")   out.voxel_resolution   = std::stoi(v);
            else if (k == "resample")           out.resample_edge_len  = std::stof(v);
            else if (k == "smooth_iterations")  out.smooth_iterations  = std::stoi(v);
            else if (k == "smooth_alpha")       out.smooth_alpha       = std::stof(v);
            else if (k == "collapse_junctions") out.collapse_junctions = (std::stoi(v) != 0);
            else if (k == "collapse_to_spine")  out.collapse_to_spine  = (std::stoi(v) != 0);
            else if (k == "prune_leaves")       out.prune_leaves       = std::stof(v);
            else if (k == "decimate")         out.decimate_faces     = std::stoi(v);
            else if (k == "test_rot") {
                float a = 0.f, b = 0.f, cc = 0.f;
                if (std::sscanf(v.c_str(), "%f,%f,%f", &a, &b, &cc) != 3) {
                    error = "test_rot needs three comma-separated degrees, got '" + v + "'";
                    return false;
                }
                out.test_rot = Vec3f(a, b, cc);
            }
            else if (k == "tangent_w")          out.tangent_W          = std::stof(v);
            else if (k == "correct_convergence")out.correct_convergence= (std::stoi(v) != 0);
            else if (k == "uncross_planes")     out.uncross_planes     = (std::stoi(v) != 0);
            else if (k == "plane_gap")          out.plane_gap          = std::stof(v);
            else if (k == "tunnel_min_aspect")  out.tunnel_min_aspect  = std::stof(v);
            else if (k == "tunnel_station")     out.tunnel_station     = std::stof(v);
            else if (k == "bed_snap_band")      out.bed_snap_band      = std::stof(v);
            else if (k == "root_lean")          out.root_lean          = std::stof(v);
            else if (k == "root_ramp")          out.root_ramp          = std::stof(v);
            else if (k == "bed_root") {
                if      (v == "auto"   || v == "0") out.bed_root = BedRootMode::Auto;
                else if (v == "middle" || v == "1") out.bed_root = BedRootMode::Middle;
                else if (v == "tips"   || v == "2") out.bed_root = BedRootMode::Tips;
                else { error = "bed_root must be auto|middle|tips"; return false; }
            }
            else if (k == "tunnel_fill") {
                if      (v == "off"  || v == "0") out.tunnel_fill = TunnelFill::Off;
                else if (v == "on"   || v == "2") out.tunnel_fill = TunnelFill::On;
                else if (v == "threshold" || v == "1") out.tunnel_fill = TunnelFill::Threshold;
                else { error = "tunnel_fill must be off|threshold|on"; return false; }
            }
            else { error = "unknown skeleton parameter '" + k + "'"; return false; }
        } catch (const std::exception&) {
            error = "bad value for '" + k + "': '" + v + "'";
            return false;
        }
    }
    return true;
}

// ── dumps ─────────────────────────────────────────────────────────────────────

bool write_skeleton_obj(const SkeletonResult& r, const std::string& path)
{
    std::ofstream f(path);
    if (!f)
        return false;
    f << "# skeleton dump: " << r.graph.num_nodes() << " nodes\n";
    for (const auto& n : r.graph.nodes)
        f << "v " << n.x() << ' ' << n.y() << ' ' << n.z() << '\n';
    for (int u = 0; u < r.graph.num_nodes(); ++u)
        for (int v : r.graph.adj[u])
            if (v > u)
                f << "l " << (u + 1) << ' ' << (v + 1) << '\n';
    return true;
}

static void json_vec(std::ostream& f, const Eigen::Vector3d& v)
{
    f << '[' << v.x() << ',' << v.y() << ',' << v.z() << ']';
}

bool write_skeleton_json(const SkeletonResult& r, const std::string& path, const std::string& source_name)
{
    std::ofstream f(path);
    if (!f)
        return false;
    f.precision(9);

    f << "{\n  \"source\": \"" << source_name << "\",\n";
    f << "  \"genus\": " << r.genus << ",\n";
    f << "  \"cycles_raw\": " << r.cycles_raw << ",\n";
    f << "  \"cycles_final\": " << r.cycles_final << ",\n";
    f << "  \"bed_z\": " << r.bed_z << ",\n";
    f << "  \"snapped_nodes\": " << r.snapped_nodes << ",\n";
    f << "  \"note\": \"" << r.note << "\",\n";
    f << "  \"planes_reaimed\": " << r.planes_reaimed << ",\n";
    f << "  \"max_reaim_deg\": " << r.max_reaim_deg << ",\n";
    f << "  \"root_mode\": \"" << r.root_mode << "\",\n";
    f << "  \"foot_node\": " << r.foot_node << ",\n";
    f << "  \"cut_tilt_deg\": [";
    for (size_t i = 0; i < r.cut_tilt_deg.size(); ++i)
        f << (i ? "," : "") << r.cut_tilt_deg[i];
    f << "],\n";

    f << "  \"tunnels\": [\n";
    for (size_t i = 0; i < r.tunnels.size(); ++i) {
        const auto& t = r.tunnels[i];
        f << "    {\"depth\": " << t.depth << ", \"width\": " << t.width
          << ", \"aspect\": " << t.aspect() << ", \"axis\": ";
        json_vec(f, t.axis);
        f << ", \"points\": [";
        for (size_t k = 0; k < t.points.size(); ++k) {
            if (k) f << ',';
            json_vec(f, t.points[k]);
        }
        f << "]}" << (i + 1 < r.tunnels.size() ? "," : "") << '\n';
    }
    f << "  ],\n";

    f << "  \"nodes\": [";
    for (int i = 0; i < r.graph.num_nodes(); ++i) {
        if (i) f << ',';
        json_vec(f, r.graph.nodes[i]);
    }
    f << "],\n";

    f << "  \"edges\": [";
    bool first = true;
    for (int u = 0; u < r.graph.num_nodes(); ++u)
        for (int v : r.graph.adj[u])
            if (v > u) {
                if (!first) f << ',';
                first = false;
                f << '[' << u << ',' << v << ']';
            }
    f << "],\n";

    auto int_list = [&f](const char* key, const std::vector<int>& v, bool last = false) {
        f << "  \"" << key << "\": [";
        for (size_t i = 0; i < v.size(); ++i) { if (i) f << ','; f << v[i]; }
        f << ']' << (last ? "\n" : ",\n");
    };
    int_list("roots", r.roots);
    int_list("convergence_nodes", r.convergence_nodes);

    f << "  \"cut_edges\": [";
    for (size_t i = 0; i < r.cut_edges.size(); ++i) {
        if (i) f << ',';
        f << '[' << r.cut_edges[i].first << ',' << r.cut_edges[i].second << ']';
    }
    f << "],\n";

    // per cut edge: the plane normal that will be used, in the same order as cut_edges
    f << "  \"cut_normals\": [";
    for (size_t i = 0; i < r.cut_edges.size(); ++i) {
        if (i) f << ',';
        json_vec(f, r.tangents.get(r.cut_edges[i].first, r.cut_edges[i].second));
    }
    f << "]\n}\n";
    return true;
}

} // namespace Slic3r
