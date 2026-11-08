#pragma once

#include <Eigen/Core>
#include <vector>
#include <admesh/stl.h>
#include <map>
#include <string>

namespace Slic3r {

struct SkeletonGraph
{
    std::vector<Eigen::Vector3d> nodes;
    std::vector<std::vector<int>> adj; // undirected adjacency

    int num_nodes() const { return (int) nodes.size(); }
    void add_node(const Eigen::Vector3d& p)
    {
        nodes.push_back(p);
        adj.emplace_back();
    }
    void add_edge(int u, int v)
    {
        adj[u].push_back(v);
        adj[v].push_back(u);
    }
    int degree(int n) const { return (int) adj[n].size(); }
};

struct SkeletonDAG
{
    std::vector<Eigen::Vector3d> nodes;
    std::vector<std::vector<int>> succ; // successors (directed forward)
    std::vector<std::vector<int>> pred; // predecessors
    int root = -1;
    std::vector<double> dist; // Dijkstra distance from root

    int num_nodes() const { return (int) nodes.size(); }
    void add_edge(int u, int v)
    {
        succ[u].push_back(v);
        pred[v].push_back(u);
    }
};

// medial_centering is CGAL's omega_P: how strongly contraction is biased toward the medial
// axis. 0 disables the term entirely (set_is_medially_centered(false)); CGAL's default is 0.2.
// It is a bias, not a constraint — MCF gives no guarantee the skeleton stays inside the solid,
// and measurably does not on a branched part.
SkeletonGraph skeletonize_mesh(const Eigen::MatrixXd& V,
                               const Eigen::MatrixXi& F,
                               double quality_speed_tradeoff = 0.1,
                               double min_edge_length_factor = 0.002,
                               double medial_centering       = 0.2);

// Voxel thinning skeleton using the Lee 1994 parallel 3D thinning algorithm.
// The mesh is rasterised into an N×N×N grid, thinned to a 1-voxel-wide
// skeleton, then converted to a SkeletonGraph.
// resolution: grid dimension; 64 is a reasonable default for interactive use.
SkeletonGraph skeletonize_mesh_voxel(const Eigen::MatrixXd& V,
                                     const Eigen::MatrixXi& F,
                                     int resolution = 64);

// Laplacian smoothing, with Taubin lambda/mu on components containing a cycle so a loop does
// not collapse. Pass V and F to constrain it: a step that would move a node out of the solid is
// shortened, and pinned if even a small step escapes. Smoothing is the largest single source of
// skeleton lying outside the material (7.1% -> 14.2% of length on a branched test part), and a
// cut plane sits at each edge MIDPOINT, so an edge in air is a cut centred on nothing.
void smooth_skeleton_graph(SkeletonGraph& G, int iterations = 10, float alpha = 0.5f,
                           bool fix_endpoints = true,
                           const Eigen::MatrixXd* V = nullptr, const Eigen::MatrixXi* F = nullptr);

int find_graph_center(const SkeletonGraph& G);

SkeletonDAG orient_skeleton_as_dag(const SkeletonGraph& G, int root = -1);

// Extends downward-running branch tips to the bed plane (z = z_min of the mesh).
//
// A degree-1 node is a candidate only if it lies within `band_mm` of the bed AND its own
// branch actually runs downward, which is what keeps a part lying on its side (whose tips
// sit at mid-height with horizontal branches) from being dragged to the bed. The target
// point is validated against the OUTER contour of the bed cross-section rather than
// against a material hit, so a tip sitting over a bore — where a downward ray hits
// nothing at all — still lands correctly.
//
// band_mm <= 0 selects an automatic band. Returns the number of nodes moved or inserted;
// *out_bed_z, if given, receives the bed plane used.
//
// What stands on the bed. Tips grounds downward-running branch tips (the behaviour above).
// Middle plants ONE root under the middle of the part instead: a duct lying across the bed, or
// a U standing on its bend, has no leg — its lowest point is mid-branch — and rooting it at an
// end tip grows every cut from one side. Auto picks Middle exactly when the part has no leg.
enum class BedRootMode : int { Auto = 0, Middle = 1, Tips = 2 };

// What snap_to_bed() decided, for plant_middle_root() to act on once the graph is final. The
// foot is planted AFTER resampling, or resampling would subdivide it into a column of
// horizontal cuts through the bottom of the part.
struct BedRootPlan
{
    bool            use_middle{false};
    Eigen::Vector3d centre{Eigen::Vector3d::Zero()}; // area centroid of the bed contact, on the bed
    double          bed_z{0.0};
    double          low_zone{0.0};  // a middle node may sit up to this far above the lowest one
    double          twig_len{0.0};  // a hanging tip branch shorter than this is MCF noise
    std::vector<std::vector<Eigen::Vector2d>> footprint; // outer bed-contact loops, world XY
    std::string     reason;
};

int snap_to_bed(SkeletonGraph& G, const Eigen::MatrixXd& V, const Eigen::MatrixXi& F,
                double band_mm = 0.0, double* out_bed_z = nullptr,
                BedRootMode mode = BedRootMode::Tips, BedRootPlan* plan = nullptr);

// Hangs a vertical foot edge down to the bed from the point of the low centreline that lies
// nearest, in plan view, to the centre of the bed contact -- splitting an edge there when that
// point is mid-edge, so a symmetric part gets a symmetric fan. Short tip branches hanging below the
// structure (MCF stubs, which would otherwise become competing roots and cuts) are pruned first.
// Returns the foot's node index, or -1 when nothing was planted.
// The foot is recognised again from geometry alone by skeleton_foot_top(), so a graph edited in
// the gizmo keeps its root without any side state.
int plant_middle_root(SkeletonGraph& G, const BedRootPlan& plan);

// If `root` is a planted foot, the centreline node it hangs from; else -1. A foot is a degree-1
// node whose single edge rises vertically into a node that carries at least two near-horizontal
// edges besides it -- a centreline running along the bed, not a leg standing on it. World frame.
int skeleton_foot_top(const std::vector<Eigen::Vector3d>& nodes, const std::vector<std::vector<int>>& adj,
                      int root);

// How far along the skeleton each node is from its root, and how far toward lying flat the cut
// there is still held: a cut's normal may tilt at most 90 deg - lean from +Z. Shared by the tangent
// ramp and the cut list so the planes that are skipped as the seat and the planes that are held agree.
//
// A tip root (a leg) starts at a lean of 90 -- its first plane is the bed plane -- and a foot root
// at `root_lean_deg`. Under a foot, every edge inside the SEAT is not cut at all: the middle chunk is
// whatever rests on the bed there. The seat reaches 1.1 x foot height x tan(lean) from the foot,
// which keeps the first plane on either side from meeting the other inside the part: planes tilted
// 90 - lean from +Z, `seat` either side of a centreline `h` above the bed, meet seat / tan(lean) =
// 1.1 h above it, i.e. clear of a pipe of radius h (steeper planes meet higher still).
struct SkeletonRootRamp
{
    std::vector<double> s;     // distance from the nearest root along the graph; for a foot, from its top
    std::vector<char>   foot;  // 1 when that nearest root is a foot
    std::vector<double> seat;  // that foot's seat radius (0 for a tip root)
    double              ramp{0.0};
    double              lean0_foot_deg{45.0};

    // Lean allowed for a cut whose plane sits at path distance s_mid, from a root of this kind.
    double lean_deg(double s_mid, bool from_foot, double seat_r) const;
};
SkeletonRootRamp skeleton_root_ramp(const std::vector<Eigen::Vector3d>& nodes,
                                    const std::vector<std::vector<int>>& adj,
                                    const std::vector<int>& roots,
                                    double root_lean_deg = 45.0, double root_ramp = 0.0);

// Genus of the largest surface component from the Euler characteristic, or -1 when the
// mesh is not a valid closed surface and the formula would be meaningless.
int mesh_genus(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F);

// A traced bore centreline: the axis of a pipe, a drilled hole or a blind pocket.
struct TunnelSpine
{
    std::vector<Eigen::Vector3d> points;   // ordered along the bore
    std::vector<double>          radius;   // outer cross-section radius at each point
    Eigen::Vector3d              axis{Eigen::Vector3d::UnitZ()}; // seed direction
    double                       depth{0.0};  // length of the hole-bearing run
    double                       width{0.0};  // width of the hole itself
    double aspect() const { return width > 1e-9 ? depth / width : 0.0; }
};

// Finds bores by cross-section hole persistence, then traces each one's centreline.
//
// Candidate directions are +Z plus the three PCA axes of the vertices. For each, the mesh
// is sliced at `station` intervals and the stations whose cross-section contains a nested
// contour (a hole) are recorded; the longest contiguous run is a bore candidate. A
// candidate is accepted when depth/width >= min_aspect — a 60 mm pipe with a 12 mm bore
// scores 5, a 6 mm plate with a 30 mm window scores 0.2 — which is what separates "follow
// the axis" from "follow the material loop". Pass min_aspect <= 0 to accept every hole.
//
// Detection is by cross-section, not by genus, so blind pockets (a cup, a drilled boss)
// are found too even though their surface is genus 0.
std::vector<TunnelSpine> find_tunnel_spines(const Eigen::MatrixXd& V,
                                            const Eigen::MatrixXi& F,
                                            double station,
                                            double min_aspect = 1.5,
                                            int    max_tunnels = 4);

// Replaces the skeleton inside each spine's corridor with the spine itself: nodes within
// the recorded radius of the polyline are dropped (for a plain pipe that is the entire
// mid-wall MCF loop), the spine is inserted as a node chain, and any branch left dangling
// is reconnected to its nearest spine node so side branches survive.
SkeletonGraph splice_tunnel_spines(const SkeletonGraph& G, const std::vector<TunnelSpine>& spines);

// Finds through-bores from the inner wall itself, so a bore that BENDS is followed.
//
// find_tunnel_spines() only sees a hole along a straight axis; a 90 deg elbow fails its gate
// in every direction and is skeletonised as a loop around its own wall. Here every face is
// classified by how much of its own hemisphere is open sky: a face lining a bore sees material
// almost everywhere. The connected inner-wall patches are taken, and a patch with exactly two
// boundary loops is a through-bore whatever its path or cross-section. A harmonic function on
// that patch (0 on one opening, 1 on the other) has level sets that ring the bore; the area
// centroid of each ring is a centreline point, spaced about `spacing` mm apart (<= 0 = auto).
//
// `min_aspect` gates on centreline length / mean bore diameter, so a washer or a windowed
// plate -- also a two-opening inner wall -- is left to the material-loop path; pass <= 0 to
// accept every bore. Each point's radius is the OUTER contour's reach at that ring, which is
// what splice_tunnel_spines() needs to swallow the mid-wall skeleton. Blind pockets (one
// loop) and branched manifolds (three or more) are reported in `note` and not traced.
std::vector<TunnelSpine> find_bore_spines_from_walls(const indexed_triangle_set& its,
                                                     double       spacing,
                                                     double       min_aspect  = 1.2,
                                                     int          max_tunnels = 4,
                                                     std::string* note        = nullptr);

// Extends every branch tip along its own direction until the material runs out.
//
// Mean curvature flow contracts the MATERIAL, and a branch tip has nothing beyond it to hold
// the contraction back, so the skeleton stops short of the geometry it describes. Measured on
// a symmetric trident whose prongs are 60 mm: MCF reached 47.3, 44.1 and 36.5 mm — 13 to 24 mm
// short, and asymmetrically on a symmetric part. Downstream that is not cosmetic; on a part
// shaped like a manifold it lifted two of three legs clear of the bed, so they never became
// DAG roots and the whole thing came out single-rooted with no convergence.
//
// The fix reuses the bore march: walking the outer contour's centroid until the section is
// gone is exactly "follow the centreline to the end of the part", and that code already
// creeps to within 0.15 mm of a pipe end. A tip already at the extremity fails on its first
// step and is left alone, and a branch too short to carry a direction (MCF returns a cone as
// two nodes 0.011 mm apart) is skipped rather than marched off into noise.
//
// max_extension <= 0 selects half the bounding-box diagonal as a safety cap; the march's own
// termination is what normally stops it. Returns the number of tips extended.
int extend_skeleton_tips(SkeletonGraph& G, const Eigen::MatrixXd& V, const Eigen::MatrixXi& F,
                         double max_extension = 0.0);

std::vector<int> leaf_first_traversal(const SkeletonDAG& dag);
std::vector<int> proximity_traversal(const SkeletonDAG& dag);
std::vector<int> bottom_first_traversal(const SkeletonDAG& dag);

Eigen::Vector3d node_normal(const SkeletonDAG& dag, int node, const Eigen::Vector3d& root_pos);

SkeletonGraph collapse_skeleton_graph(const SkeletonDAG& dag, float angle_thresh);

// Overload: merges nodes connected by edges shorter than edge_len_thresh,
// and optionally also by angle similarity (pass 0.f to disable angle check).
SkeletonGraph collapse_skeleton_graph(const SkeletonDAG& dag, float angle_thresh, float edge_len_thresh);

// DFS from the lowest-Z node to the highest-Z node to find the unique path
// through the skeleton tree, then applies Laplacian smoothing to the result.
// Pass V and F for the same reason smooth_skeleton_graph wants them: collapsing a loop leaves a
// single arc, and smoothing an arc chords it inward, so on a ring the spine walks straight out
// through the bore wall. Measured on a 4mm-wall ring (r 18..22, cut planes at edge midpoints):
// 53.4% of spine length outside the solid without the guard, 0% with it.
SkeletonGraph collapse_skeleton_to_spine(const SkeletonGraph& G, int smooth_iterations = 10, float smooth_alpha = 0.5f,
                                         const Eigen::MatrixXd* V = nullptr, const Eigen::MatrixXi* F = nullptr);

// Subdivides every edge longer than max_edge_len by inserting evenly-spaced
// intermediate nodes, restoring geometric density after collapse.
// Pass 0 or negative to skip.
SkeletonGraph resample_skeleton_graph(const SkeletonGraph& G, double max_edge_len);

// Uniform resampling along each skeleton branch at the given interval.
// Traces chains of degree-2 interior nodes, replaces them with evenly-spaced
// nodes, and preserves tip/junction positions exactly. Suitable for normalising
// the dense, uneven node distribution produced by raw MCF skeletonization.
SkeletonGraph resample_skeleton_uniform(const SkeletonGraph& G, double interval);

Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, 3, Eigen::RowMajor>> trianglemesh_to_eigen_vertices(const indexed_triangle_set& its);

Eigen::Map<const Eigen::Matrix<int, Eigen::Dynamic, 3, Eigen::RowMajor>> trianglemesh_to_eigen_faces(const indexed_triangle_set& its);

// Multi-root root finding: all degree-1 nodes in the bottom `bottom_pct` fraction of Z.
// When the graph carries a planted foot (skeleton_foot_top), only the feet and other tips
// lying ON the bed are roots: a stub merely near the bottom must not compete with the middle.
std::vector<int> find_skeleton_roots(const SkeletonGraph& G, double bottom_pct = 0.1);

// Multi-root DAG: overload taking a vector of roots (multi-source Dijkstra).
// Falls back to find_graph_center if roots is empty.
SkeletonDAG orient_skeleton_as_dag(const SkeletonGraph& G, const std::vector<int>& roots);

// Convergence node detection: any DAG node with in-degree >= 2.
// These are nodes where multiple distinct branches arrive, requiring a bisector cut plane.
std::vector<int> find_convergence_nodes(const SkeletonDAG& dag);

// Collapse flat base regions: edges with rise angle < angle_threshold_deg are "flat".
// Each connected flat-edge component is collapsed to a single representative node.
SkeletonGraph collapse_base_region(const SkeletonGraph& G, double angle_threshold_deg = 15.0);

// Collapse clusters of directly-connected junction (degree>=3) nodes into one.
SkeletonGraph collapse_nearby_junctions(const SkeletonGraph& G);

// Iteratively removes degree-1 leaf nodes whose single edge is shorter than
// min_len. Repeats until no more short leaves remain (cascade pruning).
SkeletonGraph prune_short_leaves(const SkeletonGraph& G, double min_len);

// Per-edge tangent smoothing result.
struct SkeletonTangents {
    std::vector<std::pair<int,int>>          edges;      // undirected (u < v), index-parallel to raw/smoothed
    std::vector<Eigen::Vector3d>             raw;        // DAG-directed raw unit tangents
    std::vector<Eigen::Vector3d>             smoothed;   // after W-blend + clearance fix
    std::map<std::pair<int,int>, int>        edge_index; // (min,max) → index into edges/raw/smoothed

    // Returns the smoothed tangent for edge (par→child); falls back to (0,0,1) if not found.
    Eigen::Vector3d get(int par, int child) const {
        auto key = std::make_pair(std::min(par, child), std::max(par, child));
        auto it  = edge_index.find(key);
        if (it == edge_index.end() || smoothed.empty())
            return Eigen::Vector3d(0, 0, 1);
        return smoothed[it->second];
    }
};

// Compute per-edge tangents with branch smoothing and clearance checking.
// W=0: keep raw tangents. W=1: fully interpolate between branch endpoint tangents.
// Near a root each normal's tilt from +Z is capped as skeleton_root_ramp() allows, the cap opening
// over `root_ramp` mm (0 = auto), so the planes fan out of the bed gradually instead of the first
// plane lying flat and the next one snapping straight to the spine.
SkeletonTangents compute_skeleton_tangents(
    const SkeletonGraph& G,
    const SkeletonDAG& dag,
    const std::vector<int>& roots,
    const std::vector<int>& convergence_nodes,
    double W = 0.5,
    double root_lean_deg = 45.0,
    double root_ramp = 0.0);

} // namespace Slic3r
