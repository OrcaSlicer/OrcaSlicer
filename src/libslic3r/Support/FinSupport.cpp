#include "FinSupport.hpp"
#include "SupportCommon.hpp"

#include "../Fill/Fill.hpp"
#include "../AABBTreeLines.hpp"
#include "../AABBTreeIndirect.hpp"
#include "../Flow.hpp"
#include "../Geometry/ConvexHull.hpp"
#include "../Geometry/MedialAxis.hpp"
#include "../Layer.hpp"
#include "../Model.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../SVG.hpp"
#include "../TriangleMesh.hpp"
#include "../Utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <boost/log/trivial.hpp>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {
FinSupportDimensions fin_support_dimensions(const PrintObject &object, const PrintRegion &region)
{
    const double reference = region.flow(object, frExternalPerimeter, object.config().layer_height.value).width();
    return {object.config().fin_support_contact_width.get_abs_value(reference),
            object.config().fin_support_thickness.get_abs_value(reference)};
}

namespace {

double cross_2d(const Vec2d &a, const Vec2d &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

struct PaintedSegment {
    Vec2d a;
    Vec2d b;
    Vec2d normal;
    Vec3d surface_normal;
    Vec3d macro_normal = Vec3d::Zero();
    Vec2d face_center = Vec2d::Zero();
    double face_span = 0;
    size_t island = std::numeric_limits<size_t>::max();
};

struct PaintedLayer {
    std::array<std::vector<PaintedSegment>, 3> sections; // Middle, bottom and top of the layer.
};

enum class PaintedIntentKind {
    point,
    line,
    branched
};

struct PaintedIntentSurfacePoint {
    Vec3d point = Vec3d::Zero();
    size_t face = std::numeric_limits<size_t>::max();
};

struct PaintedIntentSource {
    size_t patch = 0;
    size_t chart = 0;
    Point point;
};

struct PaintedIntentSample {
    Vec2d chart = Vec2d::Zero();
    PaintedIntentSurfacePoint surface;
    double width = 0;
    bool junction = false;
    Geometry::MedialAxis::WidthMeasurement measurement;
    PaintedIntentSurfacePoint measurement_surface;
    PaintedIntentSource source;

    Vec2d xy() const { return surface.point.head<2>(); }
    double half_width_xy() const {
        return std::max(Vec2d(measurement_surface.point.head<2>() - surface.point.head<2>()).norm(), EPSILON);
    }
};

struct PaintedIntentStroke {
    // Surface samples are immutable input, never reconstructed from the XY fit.
    std::vector<Vec3d> points;
    std::vector<PaintedIntentSample> samples;
    std::array<Vec2d, 2> xy{Vec2d::Zero(), Vec2d::Zero()};
    double start_z = 0;
    double source_min_z = 0; // Source extent, not a wall-follow stop height.
    Vec3d center = Vec3d::Zero();
    Vec3d anchor = Vec3d::Zero();
    Vec3d macro_normal = Vec3d::Zero();
    double length = 0;
    double width = 0;
    bool anchor_is_contact_top = false;
    bool spans_macro_crease = false;
    bool starts_at_root = false;
    // Endpoint nodes first, followed by any junctions inside the finite segment.
    std::vector<size_t> nodes;
    // Head attachment host (or tail when there is no head attachment), used by
    // point alternatives and sibling provenance.
    // Seed ownership is independent. All connections, including tail merges,
    // are represented by nodes and their main edges.
    std::optional<size_t> parent_edge;
    // A fitted child may meet the interior of its parent rather than one of
    // the parent's graph endpoints. Keep that exact fitted connection instead
    // of reconstructing ownership from coincident endpoints later.
    std::optional<Vec2d> parent_point;
};

struct PaintedIntentNode {
    Vec2d point = Vec2d::Zero();
    double surface_z = 0;
    std::vector<size_t> edges;
    std::optional<PaintedIntentSource> source;
    std::optional<size_t> main_edge;
};

struct PaintedIntentGraph {
    PaintedIntentKind kind = PaintedIntentKind::point;
    Vec3d center = Vec3d::Zero();
    Vec3d macro_normal = Vec3d::Zero();
    std::vector<PaintedIntentNode> nodes;
    std::vector<PaintedIntentStroke> strokes;
};

struct PaintedIsland {
    double area = 0;
    Vec3d center = Vec3d::Zero();
    PaintedIntentGraph intent;
};

struct PaintedSides {
    std::vector<PaintedLayer> layers;
    std::vector<PaintedIsland> islands;
    size_t persistent_crease_edges = 0;
    size_t joined_painted_islands = 0;
    size_t unreliable_intent_charts = 0;
    size_t pruned_intent_strokes = 0;
    size_t raw_intent_paths = 0;
    size_t logical_intent_paths = 0;
    size_t joined_intent_paths = 0;
    size_t intent_junctions = 0;
    size_t paired_intent_junctions = 0;
    size_t pruned_intent_branches = 0;
    size_t blue_intent_groups = 0;
    size_t connector_blue_intent_groups = 0;
    size_t discarded_blue_intent_groups = 0;
    size_t point_blue_intent_charts = 0;
    size_t trimmed_intent_samples = 0;
    size_t coarsened_intent_samples = 0;
    size_t junction_split_intent_strokes = 0;
    size_t blue_intent_roots = 0;
    size_t merging_intent_strokes = 0;
    size_t branch_fit_candidates = 0;
    size_t branch_fit_rejects = 0;
    size_t branch_fit_continuations = 0;
    size_t branch_fit_ties = 0;
    size_t branch_parent_stops = 0;
    size_t branch_connection_misses = 0;
    size_t branch_cycle_cancellations = 0;
    size_t fit_split_intent_strokes = 0;
    size_t crease_intent_strokes = 0;
    size_t macro_intent_patches = 0;
};

struct FinDemand {
    Vec2d direction;
    double initial;
    double remaining;
    bool blocked = false;
};

struct FinShapeLayer {
    Vec2d center = Vec2d::Zero();
    double risk = 0;
    std::vector<FinDemand> demand;
};

struct FinShapeAnalysis {
    double base_area = 0;
    double bed_z = 0;
    double peak_risk = 0;
    bool has_base_edges = false;
    std::vector<FinShapeLayer> layers;
};

enum class FinSurfaceNormalPolicy {
    local_non_upward,
    painted_region
};

struct FinCandidate {
    Vec2d point;
    Vec2d normal; // A: load-bearing sheet direction.
    Vec2d contact_normal; // B: direction from the painted wall to the sheet.
    size_t top_layer;
    FinSupportDimensions dimensions;
    double expansion_slope;
    FinSurfaceNormalPolicy surface_normals = FinSurfaceNormalPolicy::local_non_upward;
    bool requires_painted_contact = true;
    Vec3d macro_normal = Vec3d::Zero();
    // Source painted-island identity for candidate ownership and overlap
    // comparisons. Contact authorization is selected independently above.
    std::optional<size_t> wall_follow_island;
    // Optional model-facing limit on A, in world XY. A shared tail junction
    // supplies the same limit as a free tail; source Z does not stop tracing.
    std::optional<Vec2d> axis_limit;
    // Resolved XY sheet junctions, independent of the source stroke endpoints
    // and their Z range. Correction may split one source junction into several.
    struct IntentNode {
        size_t node = std::numeric_limits<size_t>::max();
        Vec2d point = Vec2d::Zero();
        std::optional<size_t> main_edge;
    };
    struct IntentRoot {
        size_t node;
        Vec2d point;
        Vec2d outgoing; // Oriented actual A axis, from the root into this member.
    };
    std::optional<size_t> intent_island;
    std::optional<size_t> intent_edge;
    std::optional<size_t> intent_parent_edge;
    std::vector<IntentNode> intent_nodes;
    std::optional<IntentRoot> intent_root;
    bool intent_line = false;
    std::optional<Vec2d> intent_parent_point;
    // Immutable source identity for sibling cavity filling, even after a
    // corrected junction splits or its connection is detached.
    std::optional<std::pair<size_t, size_t>> source_parent_junction; // Parent edge, node.
};

struct FinCandidateProposal {
    FinCandidate primary;
    std::optional<FinCandidate> point_fallback;
};

// Only the original persistent-crease graph uses endpoint recovery. Ordinary
// XY fits create explicit nodes and ownership while solving and never reroot.
void rebuild_painted_intent_nodes(PaintedIntentGraph &intent)
{
    constexpr double endpoint_tolerance_squared = 1e-12;
    intent.nodes.clear();
    for (size_t edge_id = 0; edge_id < intent.strokes.size(); ++edge_id) {
        PaintedIntentStroke &edge = intent.strokes[edge_id];
        edge.nodes = {std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max()};
        if (edge.points.size() < 2)
            continue;
        const std::array<Vec3d, 2> endpoints{edge.points.front(), edge.points.back()};
        for (size_t side = 0; side < endpoints.size(); ++side) {
            auto node = std::find_if(intent.nodes.begin(), intent.nodes.end(), [&](const PaintedIntentNode &item) {
                return (item.point - endpoints[side].head<2>()).squaredNorm() <= endpoint_tolerance_squared &&
                       std::abs(item.surface_z - endpoints[side].z()) <= 1e-6;
            });
            if (node == intent.nodes.end()) {
                intent.nodes.push_back({endpoints[side].head<2>(), endpoints[side].z(), {edge_id}, std::nullopt});
                edge.nodes[side] = intent.nodes.size() - 1;
            } else {
                edge.nodes[side] = size_t(std::distance(intent.nodes.begin(), node));
                node->edges.push_back(edge_id);
            }
        }
    }
}

void root_painted_intent_graph(PaintedIntentGraph &intent)
{
    const size_t no_edge = std::numeric_limits<size_t>::max();
    if (intent.nodes.empty() || intent.strokes.empty())
        return;

    std::vector<bool> component_node(intent.nodes.size(), false);
    std::vector<bool> component_edge(intent.strokes.size(), false);
    std::vector<bool> visited_edge(intent.strokes.size(), false);
    for (size_t component_seed = 0; component_seed < intent.nodes.size(); ++component_seed) {
        if (component_node[component_seed] || intent.nodes[component_seed].edges.empty())
            continue;
        std::vector<size_t> nodes{component_seed};
        component_node[component_seed] = true;
        for (size_t cursor = 0; cursor < nodes.size(); ++cursor) {
            const size_t node_id = nodes[cursor];
            for (const size_t edge_id : intent.nodes[node_id].edges) {
                if (edge_id >= intent.strokes.size())
                    continue;
                component_edge[edge_id] = true;
                const PaintedIntentStroke &edge = intent.strokes[edge_id];
                for (const size_t endpoint : edge.nodes)
                    if (endpoint < intent.nodes.size() && !component_node[endpoint]) {
                        component_node[endpoint] = true;
                        nodes.push_back(endpoint);
                    }
            }
        }

        const size_t root_node = *std::max_element(nodes.begin(), nodes.end(), [&](size_t a, size_t b) {
            return intent.nodes[a].surface_z < intent.nodes[b].surface_z;
        });
        size_t root_edge = no_edge;
        for (const size_t edge_id : intent.nodes[root_node].edges) {
            if (!component_edge[edge_id])
                continue;
            if (root_edge == no_edge || intent.strokes[edge_id].length > intent.strokes[root_edge].length)
                root_edge = edge_id;
        }

        struct Visit {
            size_t node;
            size_t incoming;
        };
        std::vector<Visit> pending{{root_node, no_edge}};
        while (!pending.empty()) {
            const Visit visit = pending.back();
            pending.pop_back();
            std::vector<size_t> outgoing;
            for (const size_t edge_id : intent.nodes[visit.node].edges)
                if (edge_id < intent.strokes.size() && component_edge[edge_id] && !visited_edge[edge_id])
                    outgoing.push_back(edge_id);
            if (visit.incoming == no_edge && root_edge != no_edge) {
                const auto root = std::find(outgoing.begin(), outgoing.end(), root_edge);
                if (root != outgoing.end())
                    std::iter_swap(outgoing.begin(), root);
            }
            for (size_t i = 0; i < outgoing.size(); ++i) {
                const size_t edge_id = outgoing[i];
                PaintedIntentStroke &edge = intent.strokes[edge_id];
                if (edge.nodes[1] == visit.node) {
                    std::swap(edge.nodes[0], edge.nodes[1]);
                    std::reverse(edge.points.begin(), edge.points.end());
                    std::swap(edge.xy[0], edge.xy[1]);
                }
                if (edge.nodes[0] != visit.node)
                    continue;
                edge.starts_at_root = visit.incoming == no_edge;
                const size_t parent = visit.incoming != no_edge ? visit.incoming :
                    (i > 0 && root_edge != no_edge ? root_edge : no_edge);
                if (parent != no_edge && !edge.parent_edge.has_value()) {
                    edge.parent_edge = parent;
                    if (edge.nodes[0] < intent.nodes.size())
                        edge.parent_point = intent.nodes[edge.nodes[0]].point;
                    intent.nodes[edge.nodes[0]].main_edge = parent;
                }
                visited_edge[edge_id] = true;
                if (edge.nodes[1] < intent.nodes.size())
                    pending.push_back({edge.nodes[1], edge_id});
            }
        }
    }
}

enum class FinReject {
    top_depth, top_wall, wall_facing, projection_intrusion, body_depth, bed_collision, model_collision,
    missing_wall, missing_paint, blocked_contact, contact_contour, contact_span, contact_normal,
    contact_axis, contact_turn, empty_neck, top_band, topology, thin_body, no_outline, seed_region, overlap,
    direction, paint_empty, paint_facing, paint_parallel, paint_distance,
    paint_endpoint, count
};

struct FinDiagnostics {
    struct Failure {
        size_t count = 0;
        double first_z = 0;
        double first_value = 0;
    };
    std::array<Failure, size_t(FinReject::count)> failures{};

    bool reject(FinReject reason, double z, double value = 0)
    {
        Failure &failure = failures[size_t(reason)];
        if (failure.count++ == 0) {
            failure.first_z = z;
            failure.first_value = value;
        }
        return false;
    }

    void merge(const FinDiagnostics &other)
    {
        for (size_t i = 0; i < failures.size(); ++i) {
            if (failures[i].count == 0)
                failures[i] = other.failures[i];
            else
                failures[i].count += other.failures[i].count;
        }
    }

    void log(const char *stage, const PrintObject &object, bool compact = false) const
    {
        static constexpr std::array<const char *, size_t(FinReject::count)> names{
            "top-depth", "top-wall", "wall-facing", "projection-intrusion", "body-depth", "bed-collision", "model-collision",
            "contact-no-wall", "contact-no-paint", "contact-blocked", "contact-contour", "contact-span", "contact-normal",
            "contact-axis", "contact-turn", "empty-neck", "top-band", "topology", "thin-body", "no-outline", "seed-region", "fin-overlap",
            "no-direction", "paint-empty", "paint-facing", "paint-parallel",
            "paint-distance", "paint-endpoint"};
        if (compact) {
            std::ostringstream summary;
            bool has_failures = false;
            for (size_t i = 0; i < failures.size(); ++i) {
                if (failures[i].count == 0)
                    continue;
                if (has_failures)
                    summary << ';';
                summary << names[i]
                    << "(count=" << failures[i].count
                    << ",first_z=" << failures[i].first_z
                    << ",value=" << failures[i].first_value << ')';
                has_failures = true;
            }
            if (has_failures)
                BOOST_LOG_TRIVIAL(info)
                    << "Fin support object=" << object.model_object()->name
                    << " stage=" << stage << " rejects=" << summary.str();
            return;
        }
        for (size_t i = 0; i < failures.size(); ++i) {
            if (failures[i].count == 0)
                continue;
            BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                << " stage=" << stage << " reject=" << names[i]
                << " count=" << failures[i].count << " first_z=" << failures[i].first_z
                << " value=" << failures[i].first_value;
        }
    }
};

enum class ProjectionStop { bed, no_downward, absorbed, outside, clipped, count };

struct FinBodyAxis {
    Vec2d outer;
    Vec2d inner; // A1, before the analytical printable-cap inset.
    Vec2d direction;
    Vec2d cap_normal;
    std::optional<size_t> intent_edge;
    bool outer_junction = false;
    bool inner_junction = false; // Root extension or reached tail junction; no model-facing cap.
};

struct TracedFin {
    FinCandidate seed;
    FinDiagnostics diagnostics;
    size_t planned_contact_bands = 0;
    size_t accepted_contact_bands = 0;
    size_t seed_contact_bands = 0;
    size_t suppressed_contact_bands = 0;
    size_t contact_projection_attempts = 0;
    size_t contact_projection_layers = 0;
    size_t contact_projection_max_layers = 0;
    size_t contact_projection_zero_layers = 0;
    size_t contact_projection_exposed_layers = 0;
    size_t contact_projection_retained_layers = 0;
    std::array<size_t, size_t(ProjectionStop::count)> contact_projection_stops{};
    size_t intent_component_size = 1;
    size_t intent_root_count = 0;
    std::optional<size_t> wall_detach_layer;
    bool detached_at_axis_limit = false;
    std::vector<Vec2d> boundary; // One actual outer-contour position per layer, from the bed upward.
    std::vector<Vec2d> wall_normals;
    // Painted Shape resolves the finite contact axis independently from the
    // fin plane. Both vectors point from the model toward the fin.
    std::vector<Vec2d> contact_directions;
    std::vector<Vec2d> body_cap_normals;
    std::vector<bool> wall_present;
    std::vector<Vec2d> body_start;
    std::vector<Vec2d> tips;
    std::vector<bool> contacts;
    // Each network member retains its fixed A, analytical cap and ownership.
    // A junction rounds only that endpoint; the root's outer end stays flat.
    std::vector<std::vector<FinBodyAxis>> body_axes;
    // Contact polygons are center regions. Keep them separate from the body so
    // a composite path can rebuild its envelope without retaining the
    // individual rectangular segment bodies.
    std::vector<ExPolygons> contact_centers;
    std::vector<ExPolygons> areas;
    BoundingBoxes area_bounds;
    std::vector<ExPolygons> centers;
    std::vector<Polylines> outlines;
};

Point scaled_point(const Vec2d &p) { return Point::new_scale(p.x(), p.y()); }

Polygon fin_bed_shape(const PrintObject &object)
{
    Polygon bed = get_bed_shape_with_excluded_area(object.print()->config());
    if (!object.instances().empty())
        bed.translate(-object.instances().front().shift_without_plate_offset());
    return bed;
}

constexpr double min_fin_contact_alignment = 0.86602540378443864676; // cos(30 degrees).
constexpr double fin_direction_merge_alignment = 0.96592582628906828675; // cos(15 degrees).
constexpr double min_fin_macro_horizontal = 0.25881904510252076235; // sin(15 degrees).
constexpr double fin_crease_alignment = 0.70710678118654752440; // cos(45 degrees).
constexpr double fin_intent_fragment_alignment = 0.86602540378443864676; // cos(30 degrees).
constexpr double fin_intent_strong_continuation_alignment = 0.86602540378443864676; // cos(30 degrees).
constexpr double fin_intent_weak_continuation_alignment = 0.5; // cos(60 degrees).
constexpr double fin_intent_continuation_margin = 0.1;
constexpr double fin_intent_coarsening_width_ratio = 1.;
constexpr double fin_blue_group_alignment = 0.96592582628906828675; // cos(15 degrees).

// Keep vertices on simple paths between distinct width-qualified groups.
// Shared source junctions are separate graph vertices, so a short spur touching
// two long groups at their one common junction is still only a dangling spur.
std::vector<bool> connected_intent_groups(const std::vector<std::vector<size_t>> &graph,
                                        const std::vector<bool> &qualified)
{
    assert(graph.size() == qualified.size());
    // Represent each biconnected block by one extra vertex. This forest lets
    // leaf pruning discard dangling cycles as well as ordinary short branches.
    // A block on a qualifying connection retains all its original routes;
    // no arbitrary shortest path is substituted for the painted connectivity.
    std::vector<std::vector<size_t>> forest(graph.size());
    std::vector<size_t> discovered(graph.size(), 0), low(graph.size(), 0);
    std::vector<size_t> member_block(graph.size(), std::numeric_limits<size_t>::max());
    std::vector<std::pair<size_t, size_t>> pending_edges;
    size_t order = 0;
    std::function<void(size_t, size_t)> visit = [&](size_t node, size_t parent) {
        discovered[node] = low[node] = ++order;
        for (const size_t next : graph[node]) {
            if (next == parent)
                continue;
            if (discovered[next] == 0) {
                pending_edges.emplace_back(node, next);
                visit(next, node);
                low[node] = std::min(low[node], low[next]);
                if (low[next] < discovered[node])
                    continue;
                const size_t block = forest.size();
                forest.emplace_back();
                while (true) {
                    const auto edge = pending_edges.back();
                    pending_edges.pop_back();
                    for (const size_t member : {edge.first, edge.second}) {
                        if (member_block[member] == block)
                            continue;
                        member_block[member] = block;
                        forest[member].push_back(block);
                        forest[block].push_back(member);
                    }
                    if (edge.first == node && edge.second == next)
                        break;
                }
            } else if (discovered[next] < discovered[node]) {
                pending_edges.emplace_back(node, next);
                low[node] = std::min(low[node], discovered[next]);
            }
        }
    };
    for (size_t node = 0; node < graph.size(); ++node)
        if (discovered[node] == 0)
            visit(node, graph.size());

    std::vector<size_t> degree(forest.size());
    std::vector<bool> removed(forest.size(), false);
    std::queue<size_t> leaves;
    auto is_qualified = [&](size_t node) { return node < qualified.size() && qualified[node]; };
    for (size_t node = 0; node < forest.size(); ++node) {
        degree[node] = forest[node].size();
        if (degree[node] <= 1 && !is_qualified(node))
            leaves.push(node);
    }
    while (!leaves.empty()) {
        const size_t node = leaves.front();
        leaves.pop();
        if (removed[node])
            continue;
        removed[node] = true;
        for (const size_t next : forest[node]) {
            if (removed[next])
                continue;
            if (--degree[next] <= 1 && !is_qualified(next))
                leaves.push(next);
        }
    }
    std::vector<bool> retained = qualified;
    for (size_t block = graph.size(); block < forest.size(); ++block)
        if (!removed[block])
            for (const size_t member : forest[block])
                retained[member] = true;
    return retained;
}

struct CoarseIntentPath {
    Points points;
    std::vector<coordf_t> widths;
    Geometry::MedialAxis::WidthMeasurements measurements;
    std::vector<bool> junctions;
    bool boundary_front = false;
    bool boundary_back = false;
    bool closed = false;
};

struct CoarseIntentSkeleton {
    struct PruneCheck {
        Polyline path;
        size_t retained_edges = 0;
    };
    std::vector<CoarseIntentPath> paths;
    Polylines protected_paths;
    Polylines pruned_paths;
    Polylines checked_retained_paths;
    Polylines unchecked_paths;
    std::vector<PruneCheck> prune_checks;
    size_t joined_paths = 0;
    size_t junctions = 0;
    size_t paired_junctions = 0;
    size_t pruned_branches = 0;
};

// Geometry::MedialAxis deliberately returns polylines rather than its internal
// Voronoi graph. Rebuild only the coarse connectivity needed to distinguish one
// brush stroke from short medial-axis spurs. Protect each connected graph's
// global diameter before pruning its local leaves, so repeated pruning cannot
// consume the intended centerline. Every segment remains in scaled integer
// coordinates. Exact shared vertices recover explicit junctions; a later
// local-width join reconnects numerical fragments only when their endpoint
// tangents form one continuous painted stroke.
CoarseIntentSkeleton coarse_intent_skeleton(const ThickPolylines &polylines,
                                             const std::vector<Geometry::MedialAxis::WidthMeasurements> &measurements,
                                             const ExPolygon &painted_area,
                                             double feature_scale, bool collect_prune_diagnostics)
{
    constexpr size_t no_edge = std::numeric_limits<size_t>::max();
    struct Node {
        Point point;
        std::vector<size_t> edges;
    };
    struct Edge {
        std::array<size_t, 2> nodes;
        std::array<coordf_t, 2> widths;
        std::array<Geometry::MedialAxis::WidthMeasurement, 2> measurements;
        bool active = true;
        bool protected_axis = false;
    };

    CoarseIntentSkeleton result;
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::unordered_map<Point, size_t, PointHash> node_by_point;
    auto node_id = [&](const Point &point) {
        const auto [it, inserted] = node_by_point.emplace(point, nodes.size());
        if (inserted)
            nodes.push_back({point, {}});
        return it->second;
    };
    assert(measurements.size() == polylines.size());
    for (size_t polyline_id = 0; polyline_id < polylines.size(); ++polyline_id) {
        const ThickPolyline &polyline = polylines[polyline_id];
        const auto &sources = measurements[polyline_id];
        assert(sources.size() == polyline.width.size());
        if (polyline.points.size() < 2 ||
            polyline.width.size() < 2 * (polyline.points.size() - 1))
            continue;
        for (size_t i = 1; i < polyline.points.size(); ++i) {
            const size_t from = node_id(polyline.points[i - 1]);
            const size_t to = node_id(polyline.points[i]);
            if (from == to)
                continue;
            const size_t edge_id = edges.size();
            edges.push_back({{from, to}, {polyline.width[2 * i - 2], polyline.width[2 * i - 1]},
                             {sources[2 * i - 2], sources[2 * i - 1]}, true, false});
            nodes[from].edges.push_back(edge_id);
            nodes[to].edges.push_back(edge_id);
        }
    }

    auto active_degree = [&](size_t node) {
        return size_t(std::count_if(nodes[node].edges.begin(), nodes[node].edges.end(),
            [&](size_t edge) { return edges[edge].active; }));
    };
    auto side_at = [&](size_t edge, size_t node) { return edges[edge].nodes[0] == node ? size_t(0) : size_t(1); };
    auto other_node = [&](size_t edge, size_t node) { return edges[edge].nodes[1 - side_at(edge, node)]; };
    auto edge_length = [&](size_t edge) {
        return (unscale(nodes[edges[edge].nodes[1]].point) -
                unscale(nodes[edges[edge].nodes[0]].point)).norm();
    };

    // Preserve one global axis before pruning local branches. Two Dijkstra
    // sweeps recover the exact diameter of a tree and a stable main route for
    // the rare cyclic component. Point classification belongs to the later
    // blue-group filter, after cap trimming and coarsening.
    std::vector<bool> component_seen(nodes.size(), false);
    for (size_t seed = 0; seed < nodes.size(); ++seed) {
        if (component_seen[seed] || active_degree(seed) == 0)
            continue;
        std::vector<size_t> component_nodes;
        std::queue<size_t> pending_nodes;
        pending_nodes.push(seed);
        component_seen[seed] = true;
        while (!pending_nodes.empty()) {
            const size_t node = pending_nodes.front();
            pending_nodes.pop();
            component_nodes.push_back(node);
            for (const size_t edge : nodes[node].edges) {
                if (!edges[edge].active)
                    continue;
                const size_t next = other_node(edge, node);
                if (!component_seen[next]) {
                    component_seen[next] = true;
                    pending_nodes.push(next);
                }
            }
        }

        auto farthest_from = [&](size_t start, std::vector<size_t> *predecessor) {
            using QueueItem = std::pair<double, size_t>;
            std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
            std::vector<double> distance(nodes.size(), std::numeric_limits<double>::infinity());
            if (predecessor != nullptr)
                predecessor->assign(nodes.size(), no_edge);
            distance[start] = 0.;
            queue.emplace(0., start);
            while (!queue.empty()) {
                const auto [current_distance, node] = queue.top();
                queue.pop();
                if (current_distance > distance[node] + EPSILON)
                    continue;
                for (const size_t edge : nodes[node].edges) {
                    if (!edges[edge].active)
                        continue;
                    const size_t next = other_node(edge, node);
                    const double next_distance = current_distance + edge_length(edge);
                    if (next_distance + EPSILON >= distance[next])
                        continue;
                    distance[next] = next_distance;
                    if (predecessor != nullptr)
                        (*predecessor)[next] = edge;
                    queue.emplace(next_distance, next);
                }
            }
            size_t farthest = start;
            for (const size_t node : component_nodes)
                if (distance[node] > distance[farthest])
                    farthest = node;
            return std::make_pair(farthest, distance[farthest]);
        };

        const size_t first = farthest_from(component_nodes.front(), nullptr).first;
        std::vector<size_t> predecessor;
        const size_t second = farthest_from(first, &predecessor).first;
        for (size_t node = second; node != first;) {
            const size_t edge = predecessor[node];
            if (edge == no_edge)
                break;
            edges[edge].protected_axis = true;
            result.protected_paths.emplace_back(nodes[edges[edge].nodes[0]].point,
                                                nodes[edges[edge].nodes[1]].point);
            node = other_node(edge, node);
        }
    }

    // Repeatedly remove only side branches shorter than their length-weighted
    // mean width. A single broad junction sample must not erase an otherwise
    // significant stroke. The protected diameter can become a leaf as
    // neighboring noise disappears but is never consumed.
    std::vector<std::vector<size_t>> checked_branches;
    std::vector<bool> checked_edges(collect_prune_diagnostics ? edges.size() : 0, false);
    std::queue<size_t> leaves;
    for (size_t node = 0; node < nodes.size(); ++node)
        if (active_degree(node) == 1)
            leaves.push(node);
    while (!leaves.empty()) {
        const size_t leaf = leaves.front();
        leaves.pop();
        if (active_degree(leaf) != 1)
            continue;

        std::vector<size_t> branch;
        size_t current = leaf;
        size_t incoming = no_edge;
        size_t stop = no_edge;
        double length = 0.;
        double width_moment = 0.;
        while (true) {
            size_t next_edge = no_edge;
            for (const size_t edge : nodes[current].edges)
                if (edges[edge].active && edge != incoming) {
                    next_edge = edge;
                    break;
                }
            if (next_edge == no_edge || edges[next_edge].protected_axis) {
                stop = current;
                break;
            }
            branch.push_back(next_edge);
            const double segment_length = edge_length(next_edge);
            length += segment_length;
            width_moment += 0.5 * segment_length *
                (unscale<double>(edges[next_edge].widths[0]) +
                 unscale<double>(edges[next_edge].widths[1]));
            const size_t next = other_node(next_edge, current);
            if (active_degree(next) != 2) {
                stop = next;
                break;
            }
            current = next;
            incoming = next_edge;
        }
        const double average_width = length > EPSILON ? width_moment / length : 0.;
        if (collect_prune_diagnostics && !branch.empty()) {
            CoarseIntentSkeleton::PruneCheck check;
            size_t node = leaf;
            check.path.points.push_back(nodes[node].point);
            for (const size_t edge : branch) {
                checked_edges[edge] = true;
                node = other_node(edge, node);
                check.path.points.push_back(nodes[node].point);
            }
            result.prune_checks.push_back(std::move(check));
            checked_branches.push_back(branch);
        }
        if (branch.empty() || length + EPSILON >= std::max(feature_scale, average_width))
            continue;
        for (const size_t edge : branch) {
            edges[edge].active = false;
            result.pruned_paths.emplace_back(nodes[edges[edge].nodes[0]].point,
                                             nodes[edges[edge].nodes[1]].point);
        }
        ++result.pruned_branches;
        if (stop != no_edge && active_degree(stop) == 1)
            leaves.push(stop);
    }

    // Capture final edge states separately from the check history: pruning a
    // neighbor may expose the same surviving branch to another check later.
    if (collect_prune_diagnostics) {
        for (size_t check = 0; check < checked_branches.size(); ++check)
            for (const size_t edge : checked_branches[check])
                result.prune_checks[check].retained_edges += edges[edge].active ? 1 : 0;
        for (size_t edge = 0; edge < edges.size(); ++edge) {
            if (!edges[edge].active || edges[edge].protected_axis)
                continue;
            Polylines &paths = checked_edges[edge] ? result.checked_retained_paths : result.unchecked_paths;
            paths.emplace_back(nodes[edges[edge].nodes[0]].point, nodes[edges[edge].nodes[1]].point);
        }
    }

    // Pair the most nearly opposite macro branches at a junction. Direction is
    // measured over one painted-width scale instead of one raw Voronoi edge.
    // A bend within 30 degrees is always a continuation. A bend up to 60
    // degrees is accepted only when it is clearly straighter than the next
    // possible pair, so a symmetric Y remains three independent branches.
    std::vector<std::array<size_t, 2>> continuation(edges.size(), {no_edge, no_edge});
    auto pair_edges = [&](size_t node, size_t first, size_t second) {
        continuation[first][side_at(first, node)] = second;
        continuation[second][side_at(second, node)] = first;
    };
    auto junction_direction = [&](size_t node, size_t first_edge) {
        const size_t first_side = side_at(first_edge, node);
        const double smoothing_length = std::max(
            feature_scale, unscale<double>(edges[first_edge].widths[first_side]));
        size_t current_node = other_node(first_edge, node);
        size_t previous_edge = first_edge;
        double walked = edge_length(first_edge);
        while (walked < smoothing_length && active_degree(current_node) == 2) {
            size_t next_edge = no_edge;
            for (const size_t edge : nodes[current_node].edges)
                if (edges[edge].active && edge != previous_edge) {
                    next_edge = edge;
                    break;
                }
            if (next_edge == no_edge)
                break;
            walked += edge_length(next_edge);
            current_node = other_node(next_edge, current_node);
            previous_edge = next_edge;
        }
        Vec2d direction = unscale(nodes[current_node].point) - unscale(nodes[node].point);
        if (direction.norm() > EPSILON)
            direction.normalize();
        return direction;
    };
    for (size_t node = 0; node < nodes.size(); ++node) {
        std::vector<size_t> incident;
        for (const size_t edge : nodes[node].edges)
            if (edges[edge].active)
                incident.push_back(edge);
        if (incident.size() == 2) {
            pair_edges(node, incident[0], incident[1]);
            continue;
        }
        if (incident.size() < 3)
            continue;
        ++result.junctions;
        std::vector<bool> paired(incident.size(), false);
        bool paired_at_node = false;
        while (true) {
            size_t unpaired_count = 0;
            for (const bool value : paired)
                unpaired_count += !value;
            if (unpaired_count == 2) {
                size_t first = no_edge;
                size_t second = no_edge;
                for (size_t i = 0; i < incident.size(); ++i)
                    if (!paired[i]) {
                        if (first == no_edge)
                            first = i;
                        else
                            second = i;
                    }
                pair_edges(node, incident[first], incident[second]);
                paired[first] = true;
                paired[second] = true;
                paired_at_node = true;
                break;
            }
            double best_alignment = -1.;
            double second_alignment = -1.;
            size_t best_first = no_edge;
            size_t best_second = no_edge;
            for (size_t i = 0; i < incident.size(); ++i) {
                if (paired[i])
                    continue;
                const Vec2d first_direction = junction_direction(node, incident[i]);
                if (first_direction.norm() <= EPSILON)
                    continue;
                for (size_t j = i + 1; j < incident.size(); ++j) {
                    if (paired[j])
                        continue;
                    const Vec2d second_direction = junction_direction(node, incident[j]);
                    if (second_direction.norm() <= EPSILON)
                        continue;
                    const double alignment = -first_direction.dot(second_direction);
                    if (alignment > best_alignment) {
                        second_alignment = best_alignment;
                        best_alignment = alignment;
                        best_first = i;
                        best_second = j;
                    } else if (alignment > second_alignment)
                        second_alignment = alignment;
                }
            }
            const bool strong_continuation =
                best_alignment >= fin_intent_strong_continuation_alignment;
            const bool distinct_continuation =
                best_alignment >= fin_intent_weak_continuation_alignment &&
                best_alignment - second_alignment >= fin_intent_continuation_margin;
            if (best_first == no_edge || (!strong_continuation && !distinct_continuation))
                break;
            paired[best_first] = true;
            paired[best_second] = true;
            pair_edges(node, incident[best_first], incident[best_second]);
            paired_at_node = true;
        }
        result.paired_junctions += paired_at_node;
    }

    std::vector<bool> visited(edges.size(), false);
    auto trace = [&](size_t first_edge, size_t first_node) {
        CoarseIntentPath path;
        size_t edge = first_edge;
        size_t node = first_node;
        size_t last_node = first_node;
        path.boundary_front = active_degree(first_node) == 1;
        const size_t first_side = side_at(edge, node);
        path.points.push_back(nodes[node].point);
        path.widths.push_back(edges[edge].widths[first_side]);
        path.measurements.push_back(edges[edge].measurements[first_side]);
        path.junctions.push_back(active_degree(node) >= 3);
        while (edges[edge].active && !visited[edge]) {
            visited[edge] = true;
            const size_t enter_side = side_at(edge, node);
            const size_t exit_side = 1 - enter_side;
            const size_t exit_node = edges[edge].nodes[exit_side];
            last_node = exit_node;
            path.points.push_back(nodes[exit_node].point);
            path.widths.push_back(edges[edge].widths[exit_side]);
            path.measurements.push_back(edges[edge].measurements[exit_side]);
            path.junctions.push_back(active_degree(exit_node) >= 3);
            const size_t next = continuation[edge][exit_side];
            if (next == no_edge || visited[next])
                break;
            const size_t next_side = side_at(next, exit_node);
            if (edges[next].widths[next_side] < path.widths.back()) {
                path.widths.back() = edges[next].widths[next_side];
                path.measurements.back() = edges[next].measurements[next_side];
            }
            node = exit_node;
            edge = next;
        }
        path.boundary_back = active_degree(last_node) == 1;
        path.closed = path.points.size() > 2 && path.points.front() == path.points.back();
        return path;
    };
    for (size_t edge = 0; edge < edges.size(); ++edge) {
        if (!edges[edge].active || visited[edge])
            continue;
        if (continuation[edge][0] == no_edge)
            result.paths.push_back(trace(edge, edges[edge].nodes[0]));
        else if (continuation[edge][1] == no_edge)
            result.paths.push_back(trace(edge, edges[edge].nodes[1]));
    }
    for (size_t edge = 0; edge < edges.size(); ++edge)
        if (edges[edge].active && !visited[edge])
            result.paths.push_back(trace(edge, edges[edge].nodes[0]));

    // The extractor may return neighboring portions of one stroke as separate
    // polylines without a shared endpoint. Join only compatible
    // open ends, within their local painted width, across a connector wholly
    // contained by the same painted area. This retains separate branches and
    // avoids a global proximity merge across a fold or hole.
    struct Join {
        size_t first = no_edge;
        size_t second = no_edge;
        size_t first_side = 0;
        size_t second_side = 0;
        double score = std::numeric_limits<double>::infinity();
    };
    auto endpoint_tangent = [feature_scale](const CoarseIntentPath &path, size_t side, bool leaving) {
        const size_t endpoint = side == 0 ? 0 : path.points.size() - 1;
        size_t neighbor = endpoint;
        double walked = 0;
        const double smoothing_length = 0.5 * std::max(
            feature_scale, unscale<double>(path.widths[endpoint]));
        while ((side == 0 ? neighbor + 1 < path.points.size() : neighbor > 0) &&
               walked < smoothing_length) {
            const size_t next = side == 0 ? neighbor + 1 : neighbor - 1;
            walked += (unscale(path.points[next]) - unscale(path.points[neighbor])).norm();
            neighbor = next;
        }
        Vec2d tangent = leaving ? unscale(path.points[endpoint]) - unscale(path.points[neighbor]) :
                                  unscale(path.points[neighbor]) - unscale(path.points[endpoint]);
        if (tangent.norm() > EPSILON)
            tangent.normalize();
        return tangent;
    };
    while (true) {
        Join best;
        for (size_t first = 0; first < result.paths.size(); ++first) {
            const CoarseIntentPath &a = result.paths[first];
            if (a.closed || a.points.size() < 2 || a.widths.size() != a.points.size() ||
                a.junctions.size() != a.points.size())
                continue;
            for (size_t second = first + 1; second < result.paths.size(); ++second) {
                const CoarseIntentPath &b = result.paths[second];
                if (b.closed || b.points.size() < 2 || b.widths.size() != b.points.size() ||
                    b.junctions.size() != b.points.size())
                    continue;
                for (size_t first_side = 0; first_side < 2; ++first_side) {
                    const Vec2d first_point = unscale(a.points[first_side == 0 ? 0 : a.points.size() - 1]);
                    const Vec2d first_tangent = endpoint_tangent(a, first_side, true);
                    if (first_tangent.norm() <= EPSILON)
                        continue;
                    for (size_t second_side = 0; second_side < 2; ++second_side) {
                        const Vec2d second_point = unscale(b.points[second_side == 0 ? 0 : b.points.size() - 1]);
                        const Vec2d second_tangent = endpoint_tangent(b, second_side, false);
                        if (second_tangent.norm() <= EPSILON ||
                            first_tangent.dot(second_tangent) < fin_intent_fragment_alignment)
                            continue;
                        const Vec2d connector = second_point - first_point;
                        const double gap = connector.norm();
                        const double first_width = unscale<double>(a.widths[first_side == 0 ? 0 : a.widths.size() - 1]);
                        const double second_width = unscale<double>(b.widths[second_side == 0 ? 0 : b.widths.size() - 1]);
                        const double join_scale = std::max(feature_scale, 0.5 * (first_width + second_width));
                        const double maximum_gap = 1.5 * join_scale;
                        if (gap > maximum_gap + EPSILON)
                            continue;
                        double connector_alignment = first_tangent.dot(second_tangent);
                        if (gap > EPSILON) {
                            const Vec2d connector_direction = connector / gap;
                            if (gap > 0.5 * join_scale)
                                connector_alignment = std::min(connector_alignment,
                                    std::min(first_tangent.dot(connector_direction),
                                             second_tangent.dot(connector_direction)));
                            if (connector_alignment < fin_intent_fragment_alignment ||
                                !painted_area.contains(Line(scaled_point(first_point), scaled_point(second_point))))
                                continue;
                        }
                        const double score = gap / std::max(maximum_gap, EPSILON) +
                                             1. - connector_alignment;
                        if (score < best.score)
                            best = {first, second, first_side, second_side, score};
                    }
                }
            }
        }
        if (best.first == no_edge)
            break;

        CoarseIntentPath &first = result.paths[best.first];
        CoarseIntentPath &second = result.paths[best.second];
        if (best.first_side == 0) {
            std::reverse(first.points.begin(), first.points.end());
            std::reverse(first.widths.begin(), first.widths.end());
            std::reverse(first.measurements.begin(), first.measurements.end());
            std::reverse(first.junctions.begin(), first.junctions.end());
            std::swap(first.boundary_front, first.boundary_back);
        }
        if (best.second_side == 1) {
            std::reverse(second.points.begin(), second.points.end());
            std::reverse(second.widths.begin(), second.widths.end());
            std::reverse(second.measurements.begin(), second.measurements.end());
            std::reverse(second.junctions.begin(), second.junctions.end());
            std::swap(second.boundary_front, second.boundary_back);
        }
        const bool shared_endpoint = first.points.back() == second.points.front();
        if (shared_endpoint) {
            if (second.widths.front() < first.widths.back()) {
                first.widths.back() = second.widths.front();
                first.measurements.back() = second.measurements.front();
            }
            first.junctions.back() = first.junctions.back() || second.junctions.front();
            first.points.insert(first.points.end(), second.points.begin() + 1, second.points.end());
            first.widths.insert(first.widths.end(), second.widths.begin() + 1, second.widths.end());
            first.measurements.insert(first.measurements.end(), second.measurements.begin() + 1,
                                      second.measurements.end());
            first.junctions.insert(first.junctions.end(), second.junctions.begin() + 1,
                                   second.junctions.end());
        } else {
            first.points.insert(first.points.end(), second.points.begin(), second.points.end());
            first.widths.insert(first.widths.end(), second.widths.begin(), second.widths.end());
            first.measurements.insert(first.measurements.end(), second.measurements.begin(),
                                      second.measurements.end());
            first.junctions.insert(first.junctions.end(), second.junctions.begin(),
                                   second.junctions.end());
        }
        first.boundary_back = second.boundary_back;
        result.paths.erase(result.paths.begin() + best.second);
        ++result.joined_paths;
    }
    return result;
}

bool fin_contact_aligned(const Vec2d &body_direction, const Vec2d &contact_direction,
                         double minimum_alignment = min_fin_contact_alignment)
{
    return body_direction.dot(contact_direction) >= minimum_alignment - 1e-9;
}

struct SectionMoments {
    double area = 0;
    Vec2d first = Vec2d::Zero();
    Eigen::Matrix2d second = Eigen::Matrix2d::Zero();
};

// Area and first moments in world XY; second moments about a nearby origin.
// Polygon edge integrals preserve area weighting and subtract holes exactly.
SectionMoments area_moments(const ExPolygons &slices, const Vec2d &origin = Vec2d::Zero())
{
    SectionMoments result;
    auto add = [&](const Polygon &polygon, double sign) {
        const double signed_area = polygon.area();
        const double weight = sign * std::abs(signed_area) * SCALING_FACTOR * SCALING_FACTOR;
        if (weight == 0)
            return;
        result.area += weight;
        Vec2d local_first = Vec2d::Zero();
        Eigen::Matrix2d second = Eigen::Matrix2d::Zero();
        for (size_t i = 0; i < polygon.points.size(); ++i) {
            const Vec2d a = unscale(polygon.points[i]) - origin;
            const Vec2d b = unscale(polygon.points[(i + 1) % polygon.points.size()]) - origin;
            const double cross = a.x() * b.y() - b.x() * a.y();
            local_first += cross * (a + b) / 6;
            second(0, 0) += cross * (a.x() * a.x() + a.x() * b.x() + b.x() * b.x()) / 12;
            second(1, 1) += cross * (a.y() * a.y() + a.y() * b.y() + b.y() * b.y()) / 12;
            second(0, 1) += cross * (2 * a.x() * a.y() + a.x() * b.y() +
                                      b.x() * a.y() + 2 * b.x() * b.y()) / 24;
        }
        second(1, 0) = second(0, 1);
        const double orientation = signed_area > 0 ? sign : -sign;
        result.first += weight * origin + orientation * local_first;
        result.second += orientation * second;
    };
    for (const ExPolygon &slice : slices) {
        add(slice.contour, 1);
        for (const Polygon &hole : slice.holes)
            add(hole, -1);
    }
    return result;
}

// Analyze the complete printed prefix once. Candidate selectors may use the
// resulting directions without inheriting Simulated's candidate placement.
FinShapeAnalysis analyze_fin_shape(const PrintObject &object, double line_width)
{
    FinShapeAnalysis result;
    result.layers.resize(object.layer_count());
    if (object.layer_count() == 0)
        return result;
    const Layer &first = *object.get_layer(0);
    const SectionMoments base = area_moments(first.lslices);
    result.base_area = base.area;
    result.bed_z = first.print_z - first.height;
    if (base.area <= EPSILON * EPSILON)
        return result;

    const Vec2d base_center = base.first / base.area;
    const Eigen::Matrix2d base_distribution = area_moments(first.lslices, base_center).second / base.area;
    const double base_scale = std::sqrt(base.area);
    struct BaseEdge { Vec2d normal; double limit; double initial_margin; };
    Points base_points;
    for (const ExPolygon &slice : first.lslices)
        base_points.insert(base_points.end(), slice.contour.points.begin(), slice.contour.points.end());
    const Polygon base_hull = Geometry::convex_hull(base_points);
    std::vector<BaseEdge> base_edges;
    for (size_t i = 0; i < base_hull.points.size(); ++i) {
        const Vec2d a = unscale(base_hull.points[i]);
        const Vec2d edge = unscale(base_hull.points[(i + 1) % base_hull.points.size()]) - a;
        if (edge.norm() <= EPSILON)
            continue;
        const Vec2d normal = Vec2d(edge.y(), -edge.x()).normalized();
        double limit = -std::numeric_limits<double>::infinity();
        for (const Point &point : base_hull.points)
            limit = std::max(limit, unscale(point).dot(normal));
        base_edges.push_back({normal, limit, limit - base_center.dot(normal)});
    }
    result.has_base_edges = !base_edges.empty();
    if (!result.has_base_edges)
        return result;

    auto add_demand = [](std::vector<FinDemand> &demands, const Vec2d &direction, double value) {
        if (value <= EPSILON)
            return;
        for (FinDemand &item : demands)
            if (item.direction.dot(direction) >= fin_direction_merge_alignment) {
                item.initial = item.remaining = std::max(item.initial, value);
                return;
            }
        demands.push_back({direction, value, value});
    };
    Vec2d volume_moment = Vec2d::Zero();
    double volume = 0;
    for (size_t i = 0; i < object.layer_count(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        const Layer &layer = *object.get_layer(int(i));
        const SectionMoments section = area_moments(layer.lslices, base_center);
        const double area = section.area;
        volume += area * layer.height;
        volume_moment += section.first * layer.height;
        FinShapeLayer &state = result.layers[i];
        state.center = volume > EPSILON ? Vec2d(volume_moment / volume) : base_center;
        const double height = layer.print_z - result.bed_z;
        const double eccentricity = (state.center - base_center).norm();
        state.risk = height / base_scale * (1 + eccentricity / base_scale);
        result.peak_risk = std::max(result.peak_risk, state.risk);
        if (state.risk <= 0.8)
            continue;
        const double excess = state.risk / 0.8 - 1;
        const double height_need = excess / (1 + excess);
        for (const BaseEdge &edge : base_edges) {
            const double margin = edge.limit - state.center.dot(edge.normal);
            const double need = height_need * edge.initial_margin / std::max(margin, 0.5 * line_width);
            add_demand(state.demand, edge.normal, need);
        }
        const Vec2d offset = state.center - base_center;
        if (offset.norm() > EPSILON)
            add_demand(state.demand, offset.normalized(), height_need * (1 + offset.norm() / base_scale));
        if (area <= EPSILON * EPSILON)
            continue;
        const Vec2d section_center = section.first / area;
        const Vec2d section_offset = section_center - base_center;
        if (section_offset.norm() > EPSILON)
            add_demand(state.demand, section_offset.normalized(),
                       height_need * (1 + section_offset.norm() / base_scale));
        const Eigen::Matrix2d distribution = section.second / area - section_offset * section_offset.transpose();
        const double difference = distribution(0, 0) - distribution(1, 1);
        const double angle = 0.5 * std::atan2(2 * distribution(0, 1), difference);
        const Vec2d axis(std::cos(angle), std::sin(angle));
        const Vec2d cross(-axis.y(), axis.x());
        for (const Vec2d &direction : {axis, cross}) {
            const double floor = line_width * line_width / 12.;
            const double baseline = std::max(0., direction.dot(base_distribution * direction));
            const double spread = std::max(0., direction.dot(distribution * direction));
            const double growth = std::max(0., 0.5 * std::log((spread + floor) / (baseline + floor)));
            if (growth > 1e-6) {
                add_demand(state.demand, direction, height_need * (1 + growth));
                add_demand(state.demand, -direction, height_need * (1 + growth));
            }
        }
    }
    return result;
}

bool inside_model(const Layer &layer, const Vec2d &p)
{
    const Point point = scaled_point(p);
    return std::any_of(layer.lslices.begin(), layer.lslices.end(),
                       [&point](const ExPolygon &poly) { return poly.contains(point, false); });
}

// The paint is stored on 3D mesh facets. Slice the actual side facets here: the usual
// support-enforcer projection loses the side position and normal needed by a fin.
PaintedSides painted_sides(const PrintObject &object, bool split_painted_islands)
{
    PaintedSides result;
    result.layers.resize(object.layer_count());
    const auto layers = object.layers();
    const double print_z_offset = object.slicing_parameters().object_print_z_min;
    const double line_width = support_material_flow(&object).width();
    const double contact_width = object.config().fin_support_contact_width.get_abs_value(line_width);
    const double feature_scale = std::max(3 * line_width, contact_width + line_width);
    const bool export_intent_svg = split_painted_islands && get_logging_level() >= 3;
    static std::atomic_size_t next_intent_svg_run{0};
    const size_t intent_svg_run = export_intent_svg ? next_intent_svg_run.fetch_add(1) : 0;
    size_t intent_svg_chart = 0;
    size_t intent_svg_files = 0;
    size_t intent_svg_no_line_files = 0;
    std::string first_intent_svg_path;
    std::string first_intent_svg_no_line_path;
    for (const ModelVolume *volume : object.model_object()->volumes) {
        if (!volume->is_model_part())
            continue;
        indexed_triangle_set facets = volume->supported_facets.get_facets_strict(*volume, EnforcerBlockerType::ENFORCER);
        if (split_painted_islands)
            its_merge_vertices(facets, false);
        const Transform3f transform = (object.trafo_centered() * volume->get_matrix()).cast<float>();
        struct FaceGeometry {
            std::array<Vec3d, 3> vertices;
            Vec3d center = Vec3d::Zero();
            Vec3d normal = Vec3d::Zero();
            double area = 0;
        };
        std::vector<FaceGeometry> geometry(facets.indices.size());
        const double orientation = transform.linear().determinant() < 0 ? -1. : 1.;
        for (size_t face_id = 0; face_id < facets.indices.size(); ++face_id) {
            FaceGeometry &face = geometry[face_id];
            for (int i = 0; i < 3; ++i)
                face.vertices[i] = (transform * facets.vertices[facets.indices[face_id](i)]).cast<double>();
            const Vec3d cross = (face.vertices[1] - face.vertices[0]).cross(face.vertices[2] - face.vertices[0]);
            const double twice_area = cross.norm();
            if (twice_area <= 1e-12)
                continue;
            face.area = 0.5 * twice_area;
            face.center = (face.vertices[0] + face.vertices[1] + face.vertices[2]) / 3.;
            face.normal = orientation * cross / twice_area;
        }

        struct PersistentCreasePath {
            std::vector<Vec3d> points;
            Vec3d macro_normal = Vec3d::Zero();
            double length = 0;
            size_t face = std::numeric_limits<size_t>::max();
        };
        std::vector<PersistentCreasePath> persistent_creases;
        std::vector<Vec3d> macro_normals(geometry.size(), Vec3d::Zero());
        std::vector<Vec3i32> neighbors;
        std::vector<std::array<bool, 3>> barriers(facets.indices.size());
        std::vector<std::vector<size_t>> painted_proximity_neighbors(facets.indices.size());
        if (split_painted_islands && !facets.indices.empty()) {
            neighbors = its_face_neighbors(facets);
            struct SharpEdge {
                size_t face;
                int edge;
                size_t neighbor;
                int neighbor_edge;
                int a;
                int b;
                double length;
            };
            std::vector<SharpEdge> sharp_edges;
            for (size_t face_id = 0; face_id < facets.indices.size(); ++face_id) {
                if (geometry[face_id].area <= 0)
                    continue;
                for (int edge_id = 0; edge_id < 3; ++edge_id) {
                    const int neighbor = neighbors[face_id](edge_id);
                    if (neighbor < 0 || size_t(neighbor) <= face_id || geometry[size_t(neighbor)].area <= 0 ||
                        geometry[face_id].normal.dot(geometry[size_t(neighbor)].normal) >= fin_crease_alignment)
                        continue;
                    int neighbor_edge = -1;
                    for (int i = 0; i < 3; ++i)
                        if (neighbors[size_t(neighbor)](i) == int(face_id)) {
                            neighbor_edge = i;
                            break;
                        }
                    if (neighbor_edge < 0)
                        continue;
                    const Vec2i32 edge = its_triangle_edge(facets.indices[face_id], edge_id);
                    const double length = (geometry[face_id].vertices[(edge_id + 1) % 3] -
                                           geometry[face_id].vertices[edge_id]).norm();
                    sharp_edges.push_back({face_id, edge_id, size_t(neighbor), neighbor_edge,
                                           edge(0), edge(1), length});
                }
            }
            std::vector<std::vector<size_t>> vertex_edges(facets.vertices.size());
            for (size_t edge_id = 0; edge_id < sharp_edges.size(); ++edge_id) {
                vertex_edges[size_t(sharp_edges[edge_id].a)].push_back(edge_id);
                vertex_edges[size_t(sharp_edges[edge_id].b)].push_back(edge_id);
            }
            std::vector<bool> visited_edges(sharp_edges.size(), false);
            std::vector<size_t> pending_edges;
            for (size_t seed = 0; seed < sharp_edges.size(); ++seed) {
                if (visited_edges[seed])
                    continue;
                std::vector<size_t> component;
                double component_length = 0;
                pending_edges.push_back(seed);
                visited_edges[seed] = true;
                while (!pending_edges.empty()) {
                    const size_t edge_id = pending_edges.back();
                    pending_edges.pop_back();
                    component.push_back(edge_id);
                    component_length += sharp_edges[edge_id].length;
                    for (int vertex : {sharp_edges[edge_id].a, sharp_edges[edge_id].b})
                        for (const size_t next : vertex_edges[size_t(vertex)])
                            if (!visited_edges[next]) {
                                visited_edges[next] = true;
                                pending_edges.push_back(next);
                            }
                }
                if (component_length + EPSILON < feature_scale)
                    continue;
                result.persistent_crease_edges += component.size();
                for (const size_t edge_id : component) {
                    const SharpEdge &edge = sharp_edges[edge_id];
                    barriers[edge.face][edge.edge] = true;
                    barriers[edge.neighbor][edge.neighbor_edge] = true;
                }

                std::vector<std::pair<int, size_t>> component_vertices;
                for (const size_t edge_id : component)
                    for (const int vertex : {sharp_edges[edge_id].a, sharp_edges[edge_id].b}) {
                        auto item = std::find_if(component_vertices.begin(), component_vertices.end(),
                            [vertex](const auto &candidate) { return candidate.first == vertex; });
                        if (item == component_vertices.end())
                            component_vertices.emplace_back(vertex, 1);
                        else
                            ++item->second;
                    }
                std::vector<bool> path_edge_visited(sharp_edges.size(), false);
                auto trace_crease = [&](int start_vertex, size_t first_edge) {
                    PersistentCreasePath path;
                    path.face = sharp_edges[first_edge].face;
                    path.points.push_back((transform * facets.vertices[size_t(start_vertex)]).cast<double>());
                    int vertex = start_vertex;
                    size_t edge_id = first_edge;
                    Vec3d path_normal_sum = Vec3d::Zero();
                    while (!path_edge_visited[edge_id]) {
                        path_edge_visited[edge_id] = true;
                        const SharpEdge &edge = sharp_edges[edge_id];
                        const int next_vertex = edge.a == vertex ? edge.b : edge.a;
                        path.points.push_back((transform * facets.vertices[size_t(next_vertex)]).cast<double>());
                        path.length += edge.length;
                        path_normal_sum += 0.5 * edge.length *
                            (geometry[edge.face].normal + geometry[edge.neighbor].normal);
                        const auto degree = std::find_if(component_vertices.begin(), component_vertices.end(),
                            [next_vertex](const auto &candidate) { return candidate.first == next_vertex; });
                        if (degree == component_vertices.end() || degree->second != 2)
                            break;
                        size_t next_edge = sharp_edges.size();
                        for (const size_t candidate : vertex_edges[size_t(next_vertex)])
                            if (!path_edge_visited[candidate] &&
                                std::find(component.begin(), component.end(), candidate) != component.end()) {
                                next_edge = candidate;
                                break;
                            }
                        if (next_edge == sharp_edges.size())
                            break;
                        vertex = next_vertex;
                        edge_id = next_edge;
                    }
                    if (path_normal_sum.squaredNorm() > EPSILON * EPSILON)
                        path.macro_normal = path_normal_sum.normalized();
                    if (path.points.size() >= 2)
                        persistent_creases.push_back(std::move(path));
                };
                for (const auto &[vertex, degree] : component_vertices) {
                    if (degree == 2)
                        continue;
                    for (const size_t edge_id : vertex_edges[size_t(vertex)])
                        if (!path_edge_visited[edge_id] &&
                            std::find(component.begin(), component.end(), edge_id) != component.end())
                            trace_crease(vertex, edge_id);
                }
                for (const size_t edge_id : component)
                    if (!path_edge_visited[edge_id])
                        trace_crease(sharp_edges[edge_id].a, edge_id);
            }

            using QueueItem = std::pair<double, size_t>;
            std::vector<double> distance(geometry.size(), 0.);
            std::vector<size_t> distance_stamp(geometry.size(), 0);
            for (size_t seed = 0; seed < geometry.size(); ++seed) {
                if ((seed & 63) == 0 && object.print()->canceled())
                    throw CanceledException();
                if (geometry[seed].area <= 0)
                    continue;
                const size_t stamp = seed + 1;
                std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
                distance[seed] = 0;
                distance_stamp[seed] = stamp;
                queue.emplace(0., seed);
                Vec3d normal_sum = Vec3d::Zero();
                while (!queue.empty()) {
                    const auto [current_distance, face_id] = queue.top();
                    queue.pop();
                    if (distance_stamp[face_id] != stamp || current_distance != distance[face_id] ||
                        current_distance > feature_scale)
                        continue;
                    const double radius = current_distance / feature_scale;
                    normal_sum += geometry[face_id].area * std::exp(-0.5 * radius * radius) * geometry[face_id].normal;
                    for (int edge_id = 0; edge_id < 3; ++edge_id) {
                        const int neighbor = neighbors[face_id](edge_id);
                        if (neighbor < 0 || barriers[face_id][edge_id])
                            continue;
                        const size_t next = size_t(neighbor);
                        const double next_distance = current_distance +
                            (geometry[next].center - geometry[face_id].center).norm();
                        if (next_distance <= feature_scale &&
                            (distance_stamp[next] != stamp || next_distance < distance[next])) {
                            distance[next] = next_distance;
                            distance_stamp[next] = stamp;
                            queue.emplace(next_distance, next);
                        }
                    }
                }
                macro_normals[seed] = normal_sum.squaredNorm() > EPSILON * EPSILON ?
                    normal_sum.normalized() : geometry[seed].normal;
            }
        } else {
            for (size_t face_id = 0; face_id < geometry.size(); ++face_id)
                macro_normals[face_id] = geometry[face_id].normal;
        }

        auto build_intent_graph = [&](const std::vector<size_t> &faces, const Vec3d &center,
                                      double island_area) {
            if (object.print()->canceled())
                throw CanceledException();
            PaintedIntentGraph intent;
            intent.center = center;
            Vec3d normal_sum = Vec3d::Zero();
            Vec3d macro_normal_sum = Vec3d::Zero();
            double surface_area = 0;
            for (const size_t face_id : faces) {
                const FaceGeometry &face = geometry[face_id];
                normal_sum += face.area * face.normal;
                macro_normal_sum += face.area * macro_normals[face_id];
                surface_area += face.area;
            }
            if (macro_normal_sum.squaredNorm() > EPSILON * EPSILON)
                intent.macro_normal = macro_normal_sum.normalized();
            else if (normal_sum.squaredNorm() > EPSILON * EPSILON)
                intent.macro_normal = normal_sum.normalized();
            auto append_persistent_crease_strokes = [&] {
                for (const PersistentCreasePath &crease : persistent_creases) {
                    if (crease.face == std::numeric_limits<size_t>::max() ||
                        std::find(faces.begin(), faces.end(), crease.face) == faces.end() ||
                        crease.length + EPSILON < 2 * feature_scale ||
                        2 * crease.length * feature_scale + EPSILON < 0.2 * island_area)
                        continue;
                    Vec3d first_moment = Vec3d::Zero();
                    for (size_t i = 1; i < crease.points.size(); ++i) {
                        const double length = (crease.points[i] - crease.points[i - 1]).norm();
                        first_moment += 0.5 * length * (crease.points[i] + crease.points[i - 1]);
                    }
                    const Vec3d crease_center = first_moment / crease.length;
                    Vec3d crease_tangent = crease.points.back() - crease.points.front();
                    if (crease_tangent.norm() <= EPSILON)
                        continue;
                    PaintedIntentStroke stroke;
                    stroke.points = crease.points;
                    stroke.xy = {crease.points.front().head<2>(), crease.points.back().head<2>()};
                    stroke.center = crease_center;
                    stroke.macro_normal = crease.macro_normal;
                    stroke.length = crease.length;
                    stroke.width = line_width;
                    auto top = std::max_element(stroke.points.begin(), stroke.points.end(),
                        [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
                    const auto bottom = std::min_element(stroke.points.begin(), stroke.points.end(),
                        [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
                    stroke.start_z = top->z();
                    stroke.source_min_z = bottom->z();
                    const double z_span = stroke.start_z - stroke.source_min_z;
                    stroke.anchor_is_contact_top = z_span >= std::max(feature_scale, 0.5 * stroke.length);
                    stroke.anchor = stroke.anchor_is_contact_top ? *top : stroke.center;
                    stroke.spans_macro_crease = true;
                    intent.strokes.push_back(std::move(stroke));
                    ++result.crease_intent_strokes;
                }
            };
            auto finish_intent = [&] {
                if (intent.strokes.empty())
                    return;
                double total_length = 0;
                Vec3d first_moment = Vec3d::Zero();
                for (const PaintedIntentStroke &stroke : intent.strokes) {
                    total_length += stroke.length;
                    first_moment += stroke.length * stroke.center;
                }
                intent.center = first_moment / total_length;
                intent.kind = intent.strokes.size() == 1 ? PaintedIntentKind::line : PaintedIntentKind::branched;
                rebuild_painted_intent_nodes(intent);
                root_painted_intent_graph(intent);
            };
            append_persistent_crease_strokes();
            finish_intent();
            if (surface_area <= EPSILON || normal_sum.norm() <= 0.25 * surface_area) {
                ++result.unreliable_intent_charts;
                return intent;
            }

            const Vec3d chart_normal = normal_sum.normalized();
            const Vec3d reference = std::abs(chart_normal.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX();
            const Vec3d chart_u = reference.cross(chart_normal).normalized();
            const Vec3d chart_v = chart_normal.cross(chart_u).normalized();
            struct ProjectedFace {
                size_t face = 0;
                std::array<Vec2d, 3> points;
            };
            std::vector<ProjectedFace> projected_faces;
            Polygons triangles;
            double projected_area = 0;
            for (const size_t face_id : faces) {
                const FaceGeometry &face = geometry[face_id];
                ProjectedFace projected;
                projected.face = face_id;
                for (int i = 0; i < 3; ++i) {
                    const Vec3d relative = face.vertices[i] - center;
                    projected.points[i] = Vec2d(relative.dot(chart_u), relative.dot(chart_v));
                }
                const double twice_area = (projected.points[1] - projected.points[0]).x() *
                                              (projected.points[2] - projected.points[0]).y() -
                                          (projected.points[1] - projected.points[0]).y() *
                                              (projected.points[2] - projected.points[0]).x();
                if (std::abs(twice_area) <= 1e-12)
                    continue;
                Polygon triangle{scaled_point(projected.points[0]), scaled_point(projected.points[1]),
                                 scaled_point(projected.points[2])};
                triangle.make_counter_clockwise();
                triangles.push_back(std::move(triangle));
                projected_faces.push_back(std::move(projected));
                projected_area += 0.5 * std::abs(twice_area);
            }
            if (triangles.empty() || projected_area <= EPSILON) {
                ++result.unreliable_intent_charts;
                return intent;
            }

            const ExPolygons projected_chart = union_ex(triangles);
            double chart_area = 0;
            for (const ExPolygon &area : projected_chart)
                chart_area += std::abs(area.area()) * SCALING_FACTOR * SCALING_FACTOR;
            // Strong overlap indicates that one orthographic chart folded the
            // surface onto itself. Keep point intent rather than inventing a line.
            if (chart_area < 0.7 * projected_area) {
                ++result.unreliable_intent_charts;
                return intent;
            }
            const ExPolygons chart = closing_ex(
                projected_chart, float(scale_(0.25 * feature_scale)));

            using SurfacePoint = PaintedIntentSurfacePoint;
            auto to_surface = [&](const Vec2d &point) {
                SurfacePoint best{center, std::numeric_limits<size_t>::max()};
                double best_violation = std::numeric_limits<double>::infinity();
                for (const ProjectedFace &projected : projected_faces) {
                    const Vec2d ab = projected.points[1] - projected.points[0];
                    const Vec2d ac = projected.points[2] - projected.points[0];
                    const Vec2d ap = point - projected.points[0];
                    const double denominator = ab.x() * ac.y() - ab.y() * ac.x();
                    if (std::abs(denominator) <= 1e-12)
                        continue;
                    double b = (ap.x() * ac.y() - ap.y() * ac.x()) / denominator;
                    double c = (ab.x() * ap.y() - ab.y() * ap.x()) / denominator;
                    double a = 1 - b - c;
                    const double violation = std::max(0., -a) + std::max(0., -b) + std::max(0., -c);
                    if (violation >= best_violation)
                        continue;
                    best_violation = violation;
                    a = std::max(0., a);
                    b = std::max(0., b);
                    c = std::max(0., c);
                    const double total = a + b + c;
                    if (total <= EPSILON)
                        continue;
                    const FaceGeometry &face = geometry[projected.face];
                    best.point = (a * face.vertices[0] + b * face.vertices[1] + c * face.vertices[2]) / total;
                    best.face = projected.face;
                    if (violation <= 1e-8)
                        break;
                }
                return best;
            };

            using IntentSample = PaintedIntentSample;

            // Reduce the raw medial-axis polyline to the painted-width scale
            // before interpreting its direction. Endpoints stay fixed. While a
            // short edge remains, remove the adjacent interior vertex whose
            // replacement chord has the least maximum error over every raw
            // sample it covers. Sharp corners and broad curves therefore
            // outlive straight tessellation and small brush wobble.
            auto coarsen_intent_path = [&](std::vector<IntentSample> samples,
                                           double minimum_length,
                                           const ExPolygon &painted_area) {
                if (samples.size() <= 2 || minimum_length <= EPSILON)
                    return samples;

                const size_t no_vertex = std::numeric_limits<size_t>::max();
                std::vector<size_t> previous(samples.size(), no_vertex);
                std::vector<size_t> next(samples.size(), no_vertex);
                std::vector<bool> active(samples.size(), true);
                std::vector<bool> direction_witness(samples.size(), false);
                for (size_t i = 0; i < samples.size(); ++i) {
                    if (i > 0)
                        previous[i] = i - 1;
                    if (i + 1 < samples.size())
                        next[i] = i + 1;
                }

                // A junction coordinate alone does not describe the direction
                // of an incident branch. Preserve one sample about one local
                // painted width away on each side, so forward child fitting
                // retains the local direction near the source junction.
                for (size_t junction = 0; junction < samples.size(); ++junction) {
                    if (!samples[junction].junction)
                        continue;
                    direction_witness[junction] = true;
                    const double witness_distance = std::max(
                        minimum_length, samples[junction].width);
                    size_t before = junction;
                    double walked = 0.;
                    while (before > 0 && walked < witness_distance) {
                        walked += (samples[before].chart - samples[before - 1].chart).norm();
                        --before;
                    }
                    direction_witness[before] = true;
                    size_t after = junction;
                    walked = 0.;
                    while (after + 1 < samples.size() && walked < witness_distance) {
                        walked += (samples[after + 1].chart - samples[after].chart).norm();
                        ++after;
                    }
                    direction_witness[after] = true;
                }

                using QueueItem = std::tuple<double, size_t, size_t, size_t>;
                std::priority_queue<QueueItem, std::vector<QueueItem>, std::greater<QueueItem>> queue;
                auto enqueue = [&](size_t vertex) {
                    if (vertex == 0 || vertex + 1 == samples.size() || !active[vertex] ||
                        samples[vertex].junction || direction_witness[vertex])
                        return;
                    const size_t before = previous[vertex];
                    const size_t after = next[vertex];
                    if (before == no_vertex || after == no_vertex)
                        return;
                    const double before_length =
                        (samples[vertex].chart - samples[before].chart).norm();
                    const double after_length =
                        (samples[after].chart - samples[vertex].chart).norm();
                    if (before_length + EPSILON >= minimum_length &&
                        after_length + EPSILON >= minimum_length)
                        return;
                    const Vec2d chord = samples[after].chart - samples[before].chart;
                    const double chord_length = chord.norm();
                    if (chord_length <= EPSILON ||
                        !painted_area.contains(Line(scaled_point(samples[before].chart),
                                                    scaled_point(samples[after].chart))))
                        return;
                    const Vec2d direction = chord / chord_length;
                    double error = 0.;
                    // Measure the replacement chord against every original
                    // sample it would cover. Looking only at the currently
                    // removed vertex lets a broad curve disappear through a
                    // series of individually cheap removals.
                    for (size_t sample = before + 1; sample < after; ++sample) {
                        const Vec2d from_before = samples[sample].chart - samples[before].chart;
                        const double position = std::clamp(
                            direction.dot(from_before), 0., chord_length);
                        error = std::max(
                            error, (from_before - position * direction).norm());
                    }
                    queue.emplace(error, vertex, before, after);
                };
                for (size_t i = 1; i + 1 < samples.size(); ++i)
                    enqueue(i);

                while (!queue.empty()) {
                    const auto [error, vertex, before, after] = queue.top();
                    queue.pop();
                    (void) error;
                    if (!active[vertex] || previous[vertex] != before || next[vertex] != after)
                        continue;
                    const double before_length =
                        (samples[vertex].chart - samples[before].chart).norm();
                    const double after_length =
                        (samples[after].chart - samples[vertex].chart).norm();
                    if (before_length + EPSILON >= minimum_length &&
                        after_length + EPSILON >= minimum_length)
                        continue;
                    active[vertex] = false;
                    next[before] = after;
                    previous[after] = before;
                    enqueue(before);
                    enqueue(after);
                }

                std::vector<IntentSample> result;
                result.reserve(samples.size());
                for (size_t vertex = 0; vertex != no_vertex; vertex = next[vertex])
                    if (active[vertex])
                        result.push_back(std::move(samples[vertex]));
                return result;
            };

            // Transfer retained blue samples to world XY, preserving measured
            // half-widths and source surface heights independently of green fits.
            double contracted_length = 0;
            Vec3d contracted_first_moment = Vec3d::Zero();
            size_t diagnostic_chart = 0;
            for (const ExPolygon &area : chart) {
                const size_t chart_id = diagnostic_chart++;
                const Vec2d chart_size = unscale(get_extents(area).size());
                ThickPolylines skeleton;
                // Keep narrow medial-axis branches until their connectivity is
                // known. Width is an unsuitable proxy for whether a branch is
                // brush jitter or a deliberate narrow stroke.
                Geometry::MedialAxis medial_axis(
                    0., scale_(chart_size.norm() + feature_scale), area);
                std::vector<Geometry::MedialAxis::WidthMeasurements> measurements;
                medial_axis.build(&skeleton, &measurements);
                result.raw_intent_paths += skeleton.size();
                CoarseIntentSkeleton coarse = coarse_intent_skeleton(
                    skeleton, measurements, area, feature_scale, export_intent_svg);
                result.intent_junctions += coarse.junctions;
                result.paired_intent_junctions += coarse.paired_junctions;
                result.pruned_intent_branches += coarse.pruned_branches;
                result.pruned_intent_strokes += coarse.pruned_branches;
                result.joined_intent_paths += coarse.joined_paths;
                result.logical_intent_paths += coarse.paths.size();
                Polylines debug_raw_paths, debug_protected_paths, debug_pruned_paths, debug_painted_paths;
                if (export_intent_svg) {
                    auto project_paths = [&](const auto &paths) {
                        Polylines projected;
                        for (const auto &path : paths) {
                            Polyline line;
                            for (const Point &point : path.points)
                                line.points.push_back(scaled_point(to_surface(unscale(point)).point.head<2>()));
                            projected.push_back(std::move(line));
                        }
                        return projected;
                    };
                    debug_raw_paths = project_paths(skeleton);
                    debug_protected_paths = project_paths(coarse.protected_paths);
                    debug_pruned_paths = project_paths(coarse.pruned_paths);
                    Polylines boundaries{Polyline(area.contour.points)};
                    for (const Polygon &hole : area.holes)
                        boundaries.emplace_back(hole.points);
                    for (Polyline &boundary : boundaries)
                        if (!boundary.points.empty())
                            boundary.points.push_back(boundary.points.front());
                    debug_painted_paths = project_paths(boundaries);
                }
                Polylines debug_logical_paths;
                Lines debug_retained_lines;
                struct IntentPath {
                    std::vector<IntentSample> samples;
                    std::array<size_t, 2> nodes;
                };
                std::vector<IntentPath> skeleton_paths;
                struct BlueNode {
                    IntentSample sample;
                };
                std::vector<BlueNode> blue_nodes;
                std::unordered_map<Point, size_t, PointHash> blue_node_by_point;
                auto blue_node = [&](const IntentSample &sample) {
                    const auto [it, inserted] = blue_node_by_point.emplace(sample.source.point, blue_nodes.size());
                    if (inserted)
                        blue_nodes.push_back({sample});
                    return it->second;
                };
                std::vector<std::vector<IntentSample>> blue_paths;
                for (const CoarseIntentPath &path : coarse.paths) {
                    // A closed skeleton has no stable user-drawn line endpoint.
                    // Its area moment remains available as the graph's point alternative.
                    if (path.closed || path.points.size() < 2 ||
                        path.widths.size() != path.points.size() ||
                        path.measurements.size() != path.points.size() ||
                        path.junctions.size() != path.points.size())
                        continue;
                    std::vector<IntentSample> samples;
                    samples.reserve(path.points.size());
                    for (size_t point_id = 0; point_id < path.points.size(); ++point_id) {
                        const Vec2d chart_point = unscale(path.points[point_id]);
                        const SurfacePoint surface_point = to_surface(chart_point);
                        if (surface_point.face == std::numeric_limits<size_t>::max())
                            continue;
                        const auto &measurement = path.measurements[point_id];
                        const SurfacePoint measurement_surface = to_surface(measurement.point * SCALING_FACTOR);
                        if (measurement_surface.face == std::numeric_limits<size_t>::max())
                            continue;
                        IntentSample sample{chart_point, surface_point, unscale<double>(path.widths[point_id]),
                            path.junctions[point_id], measurement, measurement_surface,
                            {faces.front(), chart_id, path.points[point_id]}};
                        // Only exact shared chart samples may coalesce. XY coincidence
                        // never removes a sample or its independent surface height.
                        if (!samples.empty() && sample.source.point == samples.back().source.point) {
                            const bool junction = samples.back().junction || sample.junction;
                            if (sample.width < samples.back().width)
                                samples.back() = std::move(sample);
                            samples.back().junction = junction;
                        } else
                            samples.push_back(std::move(sample));
                    }
                    if (samples.size() < 2)
                        continue;

                    // Trim half of the path's average painted width only from
                    // degree-one graph ends. A junction end remains fixed, so
                    // branches continue to meet at the recovered graph node.
                    std::vector<double> arc_length(samples.size(), 0.);
                    double width_moment = 0;
                    for (size_t i = 1; i < samples.size(); ++i) {
                        const double length =
                            (samples[i].chart - samples[i - 1].chart).norm();
                        arc_length[i] = arc_length[i - 1] + length;
                        width_moment += 0.5 * length *
                            (samples[i - 1].width + samples[i].width);
                    }
                    const double original_length = arc_length.back();
                    if (original_length <= EPSILON)
                        continue;
                    const double original_average_width = width_moment / original_length;
                    double front_trim = path.boundary_front ? 0.5 * original_average_width : 0.;
                    double back_trim = path.boundary_back ? 0.5 * original_average_width : 0.;
                    const double requested_trim = front_trim + back_trim;
                    if (requested_trim > 0.8 * original_length) {
                        const double trim_scale = 0.8 * original_length / requested_trim;
                        front_trim *= trim_scale;
                        back_trim *= trim_scale;
                    }
                    size_t core_begin = 0;
                    size_t core_end = samples.size() - 1;
                    while (core_begin + 1 < core_end &&
                           arc_length[core_begin] + EPSILON < front_trim)
                        ++core_begin;
                    const double back_limit = original_length - back_trim;
                    while (core_end > core_begin + 1 &&
                           arc_length[core_end] > back_limit + EPSILON)
                        --core_end;
                    if (core_begin > 0 || core_end + 1 < samples.size()) {
                        result.trimmed_intent_samples += core_begin + samples.size() - core_end - 1;
                        samples = std::vector<IntentSample>(samples.begin() + core_begin,
                                                            samples.begin() + core_end + 1);
                    }
                    if (samples.size() < 2)
                        continue;

                    double raw_trimmed_length = 0.;
                    double raw_trimmed_width_moment = 0.;
                    for (size_t i = 1; i < samples.size(); ++i) {
                        const double length =
                            (samples[i].surface.point - samples[i - 1].surface.point).norm();
                        raw_trimmed_length += length;
                        raw_trimmed_width_moment += 0.5 * length *
                            (samples[i - 1].width + samples[i].width);
                    }
                    if (raw_trimmed_length <= EPSILON)
                        continue;
                    const double raw_trimmed_average_width =
                        raw_trimmed_width_moment / raw_trimmed_length;
                    const size_t sample_count_before_coarsening = samples.size();
                    samples = coarsen_intent_path(
                        std::move(samples),
                        fin_intent_coarsening_width_ratio * raw_trimmed_average_width,
                        area);
                    result.coarsened_intent_samples +=
                        sample_count_before_coarsening - samples.size();
                    if (samples.size() < 2)
                        continue;
                    double weighted_length = 0;
                    Vec3d first_moment = Vec3d::Zero();
                    for (size_t i = 1; i < samples.size(); ++i) {
                        const double length =
                            (samples[i].surface.point - samples[i - 1].surface.point).norm();
                        weighted_length += length;
                        first_moment += 0.5 * length *
                            (samples[i].surface.point + samples[i - 1].surface.point);
                    }
                    if (weighted_length <= EPSILON)
                        continue;
                    contracted_length += weighted_length;
                    contracted_first_moment += first_moment;

                    blue_paths.push_back(std::move(samples));
                }

                // Group the actual coarsened blue edges before surface-Z orientation.
                // Only exact source vertices connect; neither XY coincidence nor
                // proximity creates an additional connection here.
                struct BlueEdge {
                    size_t path;
                    size_t sample;
                    std::array<size_t, 2> nodes;
                    Vec2d direction;
                    double length;
                };
                struct BlueGroup {
                    std::vector<size_t> edges;
                    double length = 0.;
                    double width_moment = 0.;
                    bool retained = false;
                };
                std::vector<BlueEdge> blue_edges;
                std::vector<std::vector<size_t>> path_edges(blue_paths.size());
                for (size_t path = 0; path < blue_paths.size(); ++path) {
                    const auto &samples = blue_paths[path];
                    for (const IntentSample &sample : samples)
                        blue_node(sample);
                    for (size_t i = 1; i < samples.size(); ++i) {
                        const Vec2d delta = samples[i].chart - samples[i - 1].chart;
                        const double length = delta.norm();
                        // Distinct scaled chart vertices have a positive distance.
                        // Do not use XY length: a vertical surface can project to a point.
                        assert(length > 0.);
                        path_edges[path].push_back(blue_edges.size());
                        blue_edges.push_back({path, i - 1,
                            {blue_node(samples[i - 1]), blue_node(samples[i])}, delta / length, length});
                    }
                }
                std::vector<std::vector<size_t>> incident_blue_edges(blue_nodes.size());
                for (size_t edge = 0; edge < blue_edges.size(); ++edge)
                    for (const size_t node : blue_edges[edge].nodes)
                        incident_blue_edges[node].push_back(edge);
                const size_t no_blue_group = std::numeric_limits<size_t>::max();
                std::vector<size_t> edge_group(blue_edges.size(), no_blue_group);
                std::vector<BlueGroup> blue_groups;
                for (size_t seed = 0; seed < blue_edges.size(); ++seed) {
                    if (edge_group[seed] != no_blue_group)
                        continue;
                    const size_t group_id = blue_groups.size();
                    BlueGroup group;
                    auto append_edge = [&](size_t edge) {
                        edge_group[edge] = group_id;
                        group.edges.push_back(edge);
                        const BlueEdge &source = blue_edges[edge];
                        const auto &samples = blue_paths[source.path];
                        group.length += source.length;
                        group.width_moment += 0.5 * source.length *
                            (samples[source.sample].width + samples[source.sample + 1].width);
                    };
                    auto extend = [&](size_t edge, size_t node) {
                        while (true) {
                            const BlueEdge &incoming = blue_edges[edge];
                            const Vec2d direction = node == incoming.nodes[1] ?
                                incoming.direction : Vec2d(-incoming.direction);
                            size_t best = no_blue_group;
                            double best_alignment = -1.;
                            for (const size_t candidate : incident_blue_edges[node]) {
                                if (edge_group[candidate] != no_blue_group)
                                    continue;
                                const BlueEdge &outgoing = blue_edges[candidate];
                                const Vec2d next_direction = node == outgoing.nodes[0] ?
                                    outgoing.direction : Vec2d(-outgoing.direction);
                                const double alignment = direction.dot(next_direction);
                                // Strict improvement preserves source-list order on ties.
                                if (alignment >= fin_blue_group_alignment && alignment > best_alignment) {
                                    best = candidate;
                                    best_alignment = alignment;
                                }
                            }
                            if (best == no_blue_group)
                                break;
                            append_edge(best);
                            const BlueEdge &next = blue_edges[best];
                            node = next.nodes[next.nodes[0] == node ? 1 : 0];
                            edge = best;
                        }
                    };
                    append_edge(seed);
                    // Extend both ends so a source-list seed inside a smooth run
                    // does not artificially split that run into two groups.
                    extend(seed, blue_edges[seed].nodes[1]);
                    extend(seed, blue_edges[seed].nodes[0]);
                    blue_groups.push_back(std::move(group));
                }
                size_t longest_blue_group = no_blue_group;
                for (size_t group = 0; group < blue_groups.size(); ++group)
                    if (longest_blue_group == no_blue_group ||
                        blue_groups[group].length > blue_groups[longest_blue_group].length)
                        longest_blue_group = group;
                double blue_group_width = 0.;
                bool blue_point_fallback = false;
                if (longest_blue_group != no_blue_group) {
                    const BlueGroup &longest = blue_groups[longest_blue_group];
                    blue_group_width = longest.width_moment / longest.length;
                    blue_point_fallback = longest.length < blue_group_width;
                    result.point_blue_intent_charts += blue_point_fallback;
                }
                for (BlueGroup &group : blue_groups) {
                    // One common measured full width, without a feature-scale floor,
                    // extra cap subtraction, chord simplification or per-group threshold.
                    group.retained = !blue_point_fallback && group.length >= blue_group_width;
                }
                size_t connector_groups = 0;
                if (!blue_point_fallback && std::count_if(blue_groups.begin(), blue_groups.end(),
                        [](const BlueGroup &group) { return group.retained; }) >= 2) {
                    // Bipartite connectivity: groups connect through exact source
                    // vertices, including junctions inside a group, not just its ends.
                    // Do not turn all groups touching one junction into a clique.
                    std::vector<std::vector<size_t>> group_graph(blue_groups.size());
                    for (const auto &incident : incident_blue_edges) {
                        std::vector<size_t> groups;
                        for (const size_t edge : incident)
                            groups.push_back(edge_group[edge]);
                        std::sort(groups.begin(), groups.end());
                        groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
                        if (groups.size() < 2)
                            continue;
                        const size_t junction = group_graph.size();
                        group_graph.push_back(groups);
                        for (const size_t group : groups)
                            group_graph[group].push_back(junction);
                    }
                    std::vector<bool> qualified(group_graph.size(), false);
                    for (size_t group = 0; group < blue_groups.size(); ++group)
                        qualified[group] = blue_groups[group].retained;
                    const std::vector<bool> retained = connected_intent_groups(group_graph, qualified);
                    for (size_t group = 0; group < blue_groups.size(); ++group) {
                        connector_groups += retained[group] && !qualified[group];
                        blue_groups[group].retained = retained[group];
                    }
                }
                result.blue_intent_groups += blue_groups.size();
                result.connector_blue_intent_groups += connector_groups;
                for (const BlueGroup &group : blue_groups)
                    result.discarded_blue_intent_groups += !group.retained;

                // Keep the source sample order and measurements. Removed runs must
                // split paths rather than introducing chords across the missing edges.
                std::vector<std::vector<IntentSample>> retained_blue_paths;
                for (size_t path = 0; path < blue_paths.size(); ++path) {
                    const auto &samples = blue_paths[path];
                    size_t begin = 0;
                    for (size_t i = 0; i < path_edges[path].size(); ++i) {
                        if (blue_groups[edge_group[path_edges[path][i]]].retained)
                            continue;
                        if (i > begin)
                            retained_blue_paths.emplace_back(samples.begin() + begin, samples.begin() + i + 1);
                        begin = i + 1;
                    }
                    if (begin + 1 < samples.size())
                        retained_blue_paths.emplace_back(samples.begin() + begin, samples.end());
                }

                // Rebuild downstream nodes solely from retained samples. A shared
                // vertex must not inherit its measurement from a discarded path.
                blue_nodes.clear();
                blue_node_by_point.clear();
                for (const auto &samples : retained_blue_paths) {
                    auto append_path = [&](std::vector<IntentSample> oriented) {
                        if (oriented.size() < 2)
                            return;
                        const size_t first = blue_node(oriented.front());
                        const size_t last = blue_node(oriented.back());
                        if (export_intent_svg) {
                            Polyline line;
                            for (const IntentSample &sample : oriented)
                                line.points.push_back(scaled_point(sample.xy()));
                            debug_logical_paths.push_back(std::move(line));
                        }
                        skeleton_paths.push_back({std::move(oriented), {first, last}});
                    };
                    auto append_span = [&](const std::vector<IntentSample> &span) {
                        // Orient every retained blue edge by surface Z, with source
                        // list order breaking ties. Split wherever that orientation
                        // changes; neither a global maximum nor a later DFS may
                        // reverse another high end into an incoming branch.
                        for (const IntentSample &sample : span)
                            blue_node(sample);
                        auto forward = [&](size_t i) {
                            const double a = span[i - 1].surface.point.z();
                            const double b = span[i].surface.point.z();
                            return a != b ? a > b :
                                blue_node(span[i - 1]) < blue_node(span[i]);
                        };
                        size_t begin = 0;
                        bool direction = forward(1);
                        auto emit = [&](size_t end) {
                            std::vector<IntentSample> part(span.begin() + begin, span.begin() + end + 1);
                            if (!direction)
                                std::reverse(part.begin(), part.end());
                            append_path(std::move(part));
                        };
                        for (size_t i = 2; i < span.size(); ++i) {
                            if (forward(i) == direction)
                                continue;
                            emit(i - 1);
                            begin = i - 1;
                            direction = forward(i);
                        }
                        emit(span.size() - 1);
                    };

                    // Split at each source junction, including it in both adjacent groups.
                    // Source identity does not constrain the eventual XY junction position.
                    size_t span_begin = 0;
                    for (size_t i = 1; i + 1 < samples.size(); ++i) {
                        if (!samples[i].junction)
                            continue;
                        append_span(std::vector<IntentSample>(samples.begin() + span_begin,
                                                              samples.begin() + i + 1));
                        span_begin = i;
                        ++result.junction_split_intent_strokes;
                    }
                    append_span(std::vector<IntentSample>(samples.begin() + span_begin, samples.end()));
                }

                struct Fit {
                    Vec2d origin = Vec2d::Zero();
                    Vec2d direction = Vec2d::Zero();
                    double error = 0;

                    Vec2d project(const Vec2d &p) const {
                        return origin + direction * direction.dot(p - origin);
                    }
                };
                auto fit_samples = [](const std::vector<IntentSample> &samples,
                                      const std::optional<Vec2d> &fixed) {
                    assert(!samples.empty());
                    Fit fit;
                    std::vector<double> weights(samples.size(), 0.);
                    for (size_t i = 1; i < samples.size(); ++i) {
                        const double half_length = 0.5 * (samples[i].xy() - samples[i - 1].xy()).norm();
                        weights[i - 1] += half_length;
                        weights[i] += half_length;
                    }
                    const double total = std::accumulate(weights.begin(), weights.end(), 0.);
                    Vec2d center = samples.front().xy();
                    if (total > 0.) {
                        center.setZero();
                        for (size_t i = 0; i < samples.size(); ++i)
                            center += (weights[i] / total) * samples[i].xy();
                    }
                    fit.origin = fixed ? *fixed : center;
                    Eigen::Matrix2d covariance = Eigen::Matrix2d::Zero();
                    if (total > 0.) {
                        for (size_t i = 0; i < samples.size(); ++i) {
                            const Vec2d delta = samples[i].xy() - fit.origin;
                            covariance += (weights[i] / total) * delta * delta.transpose();
                        }
                        const double angle = 0.5 * std::atan2(2. * covariance(0, 1),
                                                             covariance(0, 0) - covariance(1, 1));
                        fit.direction = Vec2d(std::cos(angle), std::sin(angle));
                    } else if (fixed) {
                        // A fixed anchor is a constraint, not an extra weighted sample.
                        // One sample (or coincident samples) determines this initial ray.
                        const Vec2d delta = samples.front().xy() - *fixed;
                        if (delta.squaredNorm() > 0.)
                            fit.direction = delta.normalized();
                    }
                    const Vec2d order = samples.back().xy() -
                        (fixed ? *fixed : samples.front().xy());
                    if (fit.direction.dot(order) < 0.)
                        fit.direction = -fit.direction;
                    for (const IntentSample &sample : samples) {
                        // Zero-weight samples still take part in the acceptance test.
                        const double error = (sample.xy() - fit.project(sample.xy())).norm();
                        fit.error = std::max(fit.error, error / sample.half_width_xy());
                    }
                    return fit;
                };

                const size_t no_edge = std::numeric_limits<size_t>::max();
                // Finish the directed blue graph before any green fitting. Equal-Z
                // edges also count as incoming, so horizontal chains have one source.
                std::vector<std::vector<size_t>> outgoing(blue_nodes.size());
                std::vector<size_t> incoming(blue_nodes.size(), 0);
                for (size_t edge = 0; edge < skeleton_paths.size(); ++edge) {
                    const IntentPath &path = skeleton_paths[edge];
                    outgoing[path.nodes[0]].push_back(edge);
                    ++incoming[path.nodes[1]];
                }
                std::vector<size_t> roots;
                for (size_t node = 0; node < blue_nodes.size(); ++node)
                    if (incoming[node] == 0 && !outgoing[node].empty())
                        roots.push_back(node);
                result.blue_intent_roots += roots.size();

                struct Attachment {
                    size_t edge;
                    size_t side; // Free child head or merging upstream tail.
                };
                struct Branch {
                    size_t source_node = 0;
                    std::vector<size_t> mains;
                    bool continuation = false;
                    std::vector<Attachment> children;
                    bool at_start = false;
                };
                std::vector<Branch> branches;
                struct State {
                    std::optional<Vec2d> fixed;
                    // Boundary retains surface semantics without joining the fit samples.
                    std::optional<IntentSample> boundary;
                    std::vector<IntentSample> samples;
                    Fit fit;
                    // The eventual committed segment supplies each reserved host slot.
                    std::vector<std::pair<size_t, size_t>> hosts;
                    std::optional<size_t> head_attachment;
                };
                const size_t first_stroke = intent.strokes.size();
                auto add_node = [&](const Vec2d &xy, const IntentSample &sample, size_t edge) {
                    const size_t node = intent.nodes.size();
                    intent.nodes.push_back({xy, sample.surface.point.z(), {edge}, sample.source});
                    return node;
                };
                auto commit = [&](const State &state, size_t previous, bool root_start) {
                    assert(!state.samples.empty());
                    const size_t edge = intent.strokes.size();
                    PaintedIntentStroke stroke;
                    stroke.samples = state.samples;
                    stroke.starts_at_root = root_start;
                    if (state.boundary)
                        stroke.points.push_back(state.boundary->surface.point);
                    for (const IntentSample &sample : state.samples)
                        stroke.points.push_back(sample.surface.point);
                    stroke.xy = {state.fixed ? *state.fixed : state.fit.project(state.samples.front().xy()),
                                 state.fit.project(state.samples.back().xy())};
                    stroke.macro_normal = intent.macro_normal;
                    const auto top = std::max_element(stroke.points.begin(), stroke.points.end(),
                        [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
                    const auto bottom = std::min_element(stroke.points.begin(), stroke.points.end(),
                        [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
                    stroke.start_z = top->z();
                    stroke.source_min_z = bottom->z();
                    // Original surface measurements describe represented area and seed
                    // heights. Neither is recomputed from the finished planar segment.
                    double width_moment = 0.;
                    Vec3d moment = Vec3d::Zero();
                    double previous_width = state.boundary ? state.boundary->width : state.samples.front().width;
                    Vec3d previous_point = stroke.points.front();
                    for (const IntentSample &sample : state.samples) {
                        const double length = (sample.surface.point - previous_point).norm();
                        stroke.length += length;
                        width_moment += 0.5 * length * (previous_width + sample.width);
                        moment += 0.5 * length * (previous_point + sample.surface.point);
                        previous_width = sample.width;
                        previous_point = sample.surface.point;
                    }
                    stroke.center = stroke.length > 0. ? Vec3d(moment / stroke.length) : stroke.points.front();
                    stroke.width = stroke.length > 0. ? width_moment / stroke.length : state.samples.front().width;
                    stroke.anchor_is_contact_top = root_start || previous != no_edge ||
                        stroke.start_z - stroke.source_min_z >= std::max(feature_scale, 0.5 * stroke.length);
                    stroke.anchor = stroke.anchor_is_contact_top ? *top : stroke.center;
                    const IntentSample &first = state.boundary ? *state.boundary : state.samples.front();
                    if (previous != no_edge) {
                        const size_t node = intent.strokes[previous].nodes[1];
                        stroke.nodes.push_back(node);
                        intent.nodes[node].edges.push_back(edge);
                        intent.nodes[node].main_edge = previous;
                        stroke.parent_edge = previous;
                        stroke.parent_point = stroke.xy[0];
                    } else
                        stroke.nodes.push_back(add_node(stroke.xy[0], first, edge));
                    stroke.nodes.push_back(add_node(stroke.xy[1], state.samples.back(), edge));
                    intent.strokes.push_back(std::move(stroke));
                    for (const auto &[branch, slot] : state.hosts)
                        branches[branch].mains[slot] = edge;
                    if (state.head_attachment)
                        branches[*state.head_attachment].children.push_back({edge, 0});
                    return edge;
                };

                struct Work {
                    size_t path = 0;
                    size_t next = 0;
                    size_t previous = std::numeric_limits<size_t>::max();
                    bool root_start = false;
                    State state;
                };
                std::queue<Work> ready;
                std::vector<std::vector<Work>> arrivals(blue_nodes.size());
                std::vector<bool> scheduled(skeleton_paths.size(), false);
                auto schedule = [&](Work work) {
                    assert(!scheduled[work.path]);
                    scheduled[work.path] = true;
                    ready.push(std::move(work));
                };
                auto finish = [&](Work &work) {
                    work.previous = commit(work.state, work.previous, work.root_start);
                    work.root_start = false;
                    const Vec2d tail = intent.strokes[work.previous].xy[1];
                    const IntentSample boundary = work.state.samples.back();
                    work.state = State{};
                    work.state.fixed = tail;
                    work.state.boundary = boundary;
                };
                for (const size_t root : roots) {
                    const size_t branch = branches.size();
                    branches.push_back({root, {no_edge}, false, {}, true});
                    for (size_t i = 0; i < outgoing[root].size(); ++i) {
                        Work work;
                        work.path = outgoing[root][i];
                        work.root_start = true;
                        work.state.fixed = skeleton_paths[work.path].samples.front().xy();
                        if (i == 0)
                            work.state.hosts.emplace_back(branch, 0);
                        else
                            work.state.head_attachment = branch;
                        schedule(std::move(work));
                    }
                }
                while (!ready.empty()) {
                    if (object.print()->canceled())
                        throw CanceledException();
                    Work work = std::move(ready.front());
                    ready.pop();
                    const IntentPath &path = skeleton_paths[work.path];
                    while (work.next < path.samples.size()) {
                        State candidate = work.state;
                        candidate.samples.push_back(path.samples[work.next]);
                        candidate.fit = fit_samples(candidate.samples, candidate.fixed);
                        if (candidate.fit.error <= 1.) {
                            work.state = std::move(candidate);
                            ++work.next;
                        } else {
                            // Confirm the accepted state and retry this sample. A
                            // generated endpoint never becomes a blue fit sample.
                            assert(!work.state.samples.empty());
                            finish(work);
                            ++result.fit_split_intent_strokes;
                        }
                    }
                    const size_t node = path.nodes[1];
                    auto &upstream = arrivals[node];
                    upstream.push_back(std::move(work));
                    if (upstream.size() < incoming[node])
                        continue;
                    assert(upstream.size() == incoming[node]);
                    // Queue arrival time must not affect list-order tie breaking.
                    std::sort(upstream.begin(), upstream.end(), [](const Work &a, const Work &b) {
                        return a.path < b.path;
                    });
                    const auto &downstream = outgoing[node];
                    if (upstream.size() == 1 && downstream.empty()) {
                        finish(upstream.front());
                        upstream.clear();
                        continue;
                    }
                    if (upstream.size() == 1 && downstream.size() == 1 &&
                        !blue_nodes[node].sample.junction) {
                        Work next = std::move(upstream.front());
                        next.path = downstream.front();
                        next.next = 1;
                        schedule(std::move(next));
                        upstream.clear();
                        continue;
                    }

                    size_t window = 2;
                    for (const size_t edge : downstream)
                        window = std::min(window, skeleton_paths[edge].samples.size() - 1);
                    struct Pair {
                        size_t input;
                        size_t output;
                        Fit fit;
                    };
                    std::vector<Pair> pairs;
                    for (size_t input = 0; input < upstream.size(); ++input) {
                        for (size_t output = 0; output < downstream.size(); ++output) {
                            const State &state = upstream[input].state;
                            auto samples = state.samples;
                            const auto &tail = skeleton_paths[downstream[output]].samples;
                            samples.insert(samples.end(), tail.begin() + 1, tail.begin() + 1 + window);
                            const Fit fit = fit_samples(samples, state.fixed);
                            ++result.branch_fit_candidates;
                            if (fit.error > 1.) {
                                ++result.branch_fit_rejects;
                                continue;
                            }
                            pairs.push_back({input, output, fit});
                        }
                    }
                    // A contested child cannot continue either incoming line.
                    // Resolve ties before consuming any input in the greedy match.
                    std::vector<bool> tied(downstream.size(), false);
                    for (size_t output = 0; output < downstream.size(); ++output) {
                        double best = std::numeric_limits<double>::infinity();
                        for (const Pair &pair : pairs)
                            if (pair.output == output)
                                best = std::min(best, pair.fit.error);
                        size_t equivalent = 0;
                        for (const Pair &pair : pairs)
                            equivalent += pair.output == output && std::abs(pair.fit.error - best) <= EPSILON;
                        tied[output] = equivalent > 1;
                        result.branch_fit_ties += tied[output];
                    }
                    std::vector<bool> used_input(upstream.size(), false);
                    std::vector<bool> used_output(downstream.size(), false);
                    std::vector<size_t> selected;
                    while (selected.size() < 2) {
                        size_t best = no_edge;
                        for (size_t i = 0; i < pairs.size(); ++i) {
                            const Pair &pair = pairs[i];
                            if (tied[pair.output] || used_input[pair.input] || used_output[pair.output])
                                continue;
                            if (best == no_edge || pair.fit.error < pairs[best].fit.error)
                                best = i;
                        }
                        if (best == no_edge)
                            break;
                        selected.push_back(best);
                        used_input[pairs[best].input] = true;
                        used_output[pairs[best].output] = true;
                    }
                    // Hosts retain source-list order, independently of which score
                    // was selected first, for coincident first-hit intersections.
                    std::sort(selected.begin(), selected.end(), [&](size_t a, size_t b) {
                        return pairs[a].input < pairs[b].input;
                    });
                    const size_t branch = branches.size();
                    branches.push_back({node, std::vector<size_t>(std::max(size_t(1), selected.size()), no_edge),
                                        !selected.empty(), {}, false});
                    result.branch_fit_continuations += selected.size();
                    result.branch_parent_stops += selected.empty() && !downstream.empty();
                    for (size_t input = 0; input < upstream.size(); ++input) {
                        if (used_input[input])
                            continue;
                        Work &stopped = upstream[input];
                        if (selected.empty() && input == 0)
                            stopped.state.hosts.emplace_back(branch, 0);
                        finish(stopped);
                        if (!selected.empty() || input != 0) {
                            branches[branch].children.push_back({stopped.previous, 1});
                            ++result.merging_intent_strokes;
                        }
                    }
                    for (size_t slot = 0; slot < selected.size(); ++slot) {
                        const Pair &pair = pairs[selected[slot]];
                        Work next = std::move(upstream[pair.input]);
                        next.path = downstream[pair.output];
                        next.next = 1 + window;
                        const auto &tail = skeleton_paths[next.path].samples;
                        next.state.samples.insert(next.state.samples.end(), tail.begin() + 1,
                                                  tail.begin() + 1 + window);
                        next.state.fit = pair.fit;
                        next.state.hosts.emplace_back(branch, slot);
                        schedule(std::move(next));
                    }
                    for (size_t output = 0; output < downstream.size(); ++output) {
                        if (used_output[output])
                            continue;
                        Work child;
                        child.path = downstream[output];
                        child.state.head_attachment = branch;
                        schedule(std::move(child));
                    }
                    upstream.clear();
                }

                assert(std::all_of(scheduled.begin(), scheduled.end(), [](bool value) { return value; }));
                assert(std::all_of(arrivals.begin(), arrivals.end(),
                    [](const auto &pending) { return pending.empty(); }));

                // Endpoint owners must finish before a segment supplies an
                // internal junction. Committed edge IDs no longer imply this order
                // because all incoming states now wait at each source junction.
                std::vector<std::vector<size_t>> endpoint_owners(intent.strokes.size());
                for (size_t branch = 0; branch < branches.size(); ++branch)
                    for (const Attachment &child : branches[branch].children)
                        endpoint_owners[child.edge].push_back(branch);
                std::vector<std::vector<size_t>> dependents(branches.size());
                for (size_t branch = 0; branch < branches.size(); ++branch) {
                    // A stopped segment's confirmed tail (or a root's fixed
                    // head) is not moved by another branch. Its other endpoint
                    // need not settle before this fixed junction is attached.
                    if (!branches[branch].continuation ||
                        (branches[branch].mains.size() == 1 && branches[branch].children.empty()))
                        continue;
                    std::vector<size_t> owners;
                    for (const size_t main : branches[branch].mains) {
                        assert(main != no_edge);
                        for (const size_t owner : endpoint_owners[main])
                            if (owner != branch)
                                owners.push_back(owner);
                    }
                    std::sort(owners.begin(), owners.end());
                    owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
                    for (const size_t owner : owners)
                        dependents[owner].push_back(branch);
                }

                // Only strongly connected groups have mutually dependent
                // endpoint edits. Cancel those edits before moving anything;
                // downstream groups outside the cycle still attach normally.
                std::vector<size_t> discovered(branches.size(), no_edge), low(branches.size());
                std::vector<size_t> stack;
                std::vector<bool> on_stack(branches.size(), false), cyclic(branches.size(), false);
                size_t order = 0;
                std::function<void(size_t)> find_cycles = [&](size_t branch) {
                    discovered[branch] = low[branch] = order++;
                    stack.push_back(branch);
                    on_stack[branch] = true;
                    for (const size_t next : dependents[branch]) {
                        if (discovered[next] == no_edge) {
                            find_cycles(next);
                            low[branch] = std::min(low[branch], low[next]);
                        } else if (on_stack[next])
                            low[branch] = std::min(low[branch], discovered[next]);
                    }
                    if (low[branch] != discovered[branch])
                        return;
                    const bool cycle = stack.back() != branch;
                    size_t member;
                    do {
                        member = stack.back();
                        stack.pop_back();
                        on_stack[member] = false;
                        cyclic[member] = cycle;
                    } while (member != branch);
                };
                for (size_t branch = 0; branch < branches.size(); ++branch)
                    if (discovered[branch] == no_edge)
                        find_cycles(branch);
                for (size_t branch = 0; branch < branches.size(); ++branch) {
                    if (!cyclic[branch])
                        continue;
                    branches[branch].children.clear();
                    dependents[branch].clear();
                    ++result.branch_cycle_cancellations;
                }
                // Canceled endpoint edits no longer delay another junction.
                // Internal crossings still wait for any remaining external edits.
                std::vector<size_t> dependencies(branches.size(), 0);
                for (const auto &next : dependents)
                    for (const size_t branch : next)
                        ++dependencies[branch];
                std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> attachments_ready;
                for (size_t branch = 0; branch < branches.size(); ++branch)
                    if (dependencies[branch] == 0)
                        attachments_ready.push(branch);

                struct RayHit {
                    Vec2d point;
                    double distance;
                };
                auto intersect_host = [&](const Vec2d &origin, const Vec2d &direction,
                                          const PaintedIntentStroke &host) -> std::optional<RayHit> {
                    const Vec2d chord = host.xy[1] - host.xy[0];
                    const double length = chord.norm();
                    if (length == 0.)
                        return std::nullopt;
                    const Vec2d along = chord / length;
                    const double denominator = cross_2d(direction, along);
                    if (std::abs(denominator) <= 1e-12)
                        return std::nullopt;
                    const double distance = cross_2d(host.xy[0] - origin, along) / denominator;
                    if (distance < -EPSILON)
                        return std::nullopt;
                    const Vec2d point = origin + distance * direction;
                    const double position = (point - host.xy[0]).dot(along);
                    if (position < -EPSILON || position > length + EPSILON)
                        return std::nullopt;
                    return RayHit{point, std::max(0., distance)};
                };
                auto set_contact_top = [](PaintedIntentStroke &stroke) {
                    stroke.anchor = *std::max_element(stroke.points.begin(), stroke.points.end(),
                        [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
                    stroke.anchor_is_contact_top = true;
                };
                auto attach = [&](const Attachment &attachment, size_t node, size_t main) {
                    PaintedIntentStroke &child = intent.strokes[attachment.edge];
                    auto &old_edges = intent.nodes[child.nodes[attachment.side]].edges;
                    old_edges.erase(std::remove(old_edges.begin(), old_edges.end(), attachment.edge), old_edges.end());
                    child.nodes[attachment.side] = node;
                    intent.nodes[node].edges.push_back(attachment.edge);
                    child.xy[attachment.side] = intent.nodes[node].point;
                    if (attachment.side == 0 || !child.parent_edge) {
                        child.parent_edge = main;
                        child.parent_point = intent.nodes[node].point;
                    }
                    set_contact_top(child);
                };
                // Foldbacks are deliberately not detected by this XY solver.
                while (!attachments_ready.empty()) {
                    if (object.print()->canceled())
                        throw CanceledException();
                    const size_t branch_id = attachments_ready.top();
                    attachments_ready.pop();
                    const Branch &branch = branches[branch_id];
                    const IntentSample &source = blue_nodes[branch.source_node].sample;
                    const size_t first_main = branch.mains.front();
                    PaintedIntentStroke &first = intent.strokes[first_main];
                    if (!branch.continuation) {
                        if (!branch.children.empty()) {
                            const size_t node = first.nodes[branch.at_start ? 0 : 1];
                            intent.nodes[node].source = source.source;
                            intent.nodes[node].main_edge = first_main;
                            set_contact_top(first);
                            for (const Attachment &child : branch.children)
                                attach(child, node, first_main);
                        }
                    } else {
                        std::optional<Vec2d> crossing;
                        if (branch.mains.size() == 2) {
                            const Vec2d chord = first.xy[1] - first.xy[0];
                            const double length = chord.norm();
                            if (length > 0.) {
                                const auto hit = intersect_host(first.xy[0], chord / length,
                                                                intent.strokes[branch.mains[1]]);
                                if (hit && hit->distance <= length + EPSILON)
                                    crossing = hit->point;
                            }
                        }
                        if (crossing) {
                            // Both main segments stay whole. Only their common
                            // node is added, after their external endpoints settle.
                            const size_t node = add_node(*crossing, source, first_main);
                            intent.nodes[node].main_edge = first_main;
                            for (size_t slot = 0; slot < branch.mains.size(); ++slot) {
                                const size_t main = branch.mains[slot];
                                intent.strokes[main].nodes.push_back(node);
                                if (slot != 0)
                                    intent.nodes[node].edges.push_back(main);
                                set_contact_top(intent.strokes[main]);
                            }
                            for (const Attachment &child : branch.children)
                                attach(child, node, first_main);
                        } else {
                            // First finite hit assigns each open endpoint to a
                            // host. Collapse intersections separately on each host.
                            std::vector<std::vector<Attachment>> groups(branch.mains.size());
                            std::vector<Vec2d> sums(branch.mains.size(), Vec2d::Zero());
                            for (const Attachment &attachment : branch.children) {
                                const PaintedIntentStroke &child = intent.strokes[attachment.edge];
                                const Vec2d origin = child.xy[1 - attachment.side];
                                const Vec2d ray = child.xy[attachment.side] - origin;
                                const double length = ray.norm();
                                size_t chosen = no_edge;
                                std::optional<RayHit> nearest;
                                if (length > 0.) {
                                    for (size_t slot = 0; slot < branch.mains.size(); ++slot) {
                                        const auto hit = intersect_host(origin, ray / length,
                                            intent.strokes[branch.mains[slot]]);
                                        if (hit && (!nearest || hit->distance < nearest->distance)) {
                                            nearest = hit;
                                            chosen = slot;
                                        }
                                    }
                                }
                                if (!nearest) {
                                    ++result.branch_connection_misses;
                                    continue;
                                }
                                groups[chosen].push_back(attachment);
                                sums[chosen] += nearest->point;
                            }
                            for (size_t slot = 0; slot < branch.mains.size(); ++slot) {
                                if (groups[slot].empty())
                                    continue;
                                const size_t main = branch.mains[slot];
                                PaintedIntentStroke &host = intent.strokes[main];
                                const Vec2d joint = sums[slot] / double(groups[slot].size());
                                const Vec2d chord = host.xy[1] - host.xy[0];
                                const double length = chord.norm();
                                const double position = length > 0. ?
                                    (joint - host.xy[0]).dot(chord / length) : -1.;
                                if (length == 0. || position < -EPSILON || position > length + EPSILON) {
                                    result.branch_connection_misses += groups[slot].size();
                                    continue;
                                }
                                const size_t node = add_node(joint, source, main);
                                intent.nodes[node].main_edge = main;
                                host.nodes.push_back(node);
                                set_contact_top(host);
                                for (const Attachment &child : groups[slot])
                                    attach(child, node, main);
                            }
                        }
                    }
                    for (const size_t dependent : dependents[branch_id])
                        if (--dependencies[dependent] == 0)
                            attachments_ready.push(dependent);
                }
                assert(std::all_of(dependencies.begin(), dependencies.end(),
                    [](size_t count) { return count == 0; }));

                if (export_intent_svg) {
                    // All overlays are world XY. Never map solved green geometry
                    // back through the surface chart just for diagnostics.
                    for (size_t edge = first_stroke; edge < intent.strokes.size(); ++edge)
                        debug_retained_lines.emplace_back(scaled_point(intent.strokes[edge].xy[0]),
                                                          scaled_point(intent.strokes[edge].xy[1]));
                    const size_t svg_chart = intent_svg_chart++;
                    const std::string path = debug_out_path(
                        "fin-support/painted-shape-skeleton-%d-%d-r%d-l%d-k%d.svg",
                        int(intent_svg_run), int(svg_chart), int(skeleton.size()),
                        int(coarse.paths.size()), int(debug_retained_lines.size()));
                    BoundingBox bounds = get_extents(debug_painted_paths);
                    for (const Line &line : debug_retained_lines) {
                        bounds.merge(line.a);
                        bounds.merge(line.b);
                    }
                    SVG svg(path, bounds, scale_(1.5));
                    svg.add_comment("world XY: gray=paint boundary; orange=raw skeleton; cyan=protected axis; "
                                    "red=pruned branches; blue=retained source paths; green=final segments");
                    svg.draw(debug_painted_paths, "#606060", scale_(0.04));
                    svg.draw(debug_raw_paths, "#e69f00", scale_(0.025));
                    svg.draw(debug_protected_paths, "#56b4e9", scale_(0.055));
                    svg.draw(debug_pruned_paths, "#d55e00", scale_(0.06));
                    svg.draw(debug_logical_paths, "#0072b2", scale_(0.045));
                    svg.draw(debug_retained_lines, "#009e73", scale_(0.08));
                    svg.Close();

                    const std::string group_path = debug_out_path(
                        "fin-support/painted-shape-blue-groups-%d-%d.svg", int(intent_svg_run), int(svg_chart));
                    SVG group_svg(group_path, bounds, scale_(1.5));
                    if (group_svg.is_opened()) {
                        group_svg.add_comment("world XY display; measurements and turns use surface-chart space; "
                                              "blue=width-qualified; cyan=retained longest group; "
                                              "orange=short connector; pink=discarded; "
                                              "G labels identify source groups within this chart");
                        group_svg.draw(debug_painted_paths, "#606060", scale_(0.04));
                    }
                    for (size_t id = 0; id < blue_groups.size(); ++id) {
                        const BlueGroup &group = blue_groups[id];
                        const std::string label = "G" + std::to_string(id);
                        if (group_svg.is_opened()) {
                            const char *color = !group.retained ? "#e377c2" :
                                id == longest_blue_group ? "#56b4e9" :
                                group.length < blue_group_width ? "#e69f00" : "#0072b2";
                            Lines lines;
                            for (const size_t edge_id : group.edges) {
                                const BlueEdge &edge = blue_edges[edge_id];
                                const auto &samples = blue_paths[edge.path];
                                lines.emplace_back(scaled_point(samples[edge.sample].xy()),
                                                   scaled_point(samples[edge.sample + 1].xy()));
                            }
                            group_svg.draw(lines, color, scale_(0.06));
                            const BlueEdge &seed = blue_edges[group.edges.front()];
                            const auto &samples = blue_paths[seed.path];
                            group_svg.draw_text(scaled_point(0.5 *
                                (samples[seed.sample].xy() + samples[seed.sample + 1].xy())),
                                label.c_str(), color, 1);
                        }
                    }
                    BOOST_LOG_TRIVIAL(info)
                        << "Fin support object=" << object.model_object()->name
                        << " stage=painted-shape-blue-groups run=" << intent_svg_run
                        << " chart=" << svg_chart << " groups=" << blue_groups.size()
                        << " longest_group=" << (longest_blue_group == no_blue_group ? "none" :
                            "G" + std::to_string(longest_blue_group))
                        << " threshold_mm=" << blue_group_width << " max_turn_deg=15"
                        << " connector_groups=" << connector_groups
                        << " point_fallback=" << blue_point_fallback
                        << " svg_opened=" << group_svg.is_opened() << " svg=" << group_path;
                    if (group_svg.is_opened())
                        group_svg.Close();

                    // Enlarge only the diagnostic display so branch labels are
                    // readable; source geometry remains unchanged.
                    constexpr double diagnostic_scale = 4.;
                    auto project_prune_path = [&](const Polyline &source) {
                        Polyline projected;
                        for (const Point &point : source.points)
                            projected.points.push_back(scaled_point(
                                diagnostic_scale * to_surface(unscale(point)).point.head<2>()));
                        return projected;
                    };
                    Polylines prune_boundaries = debug_painted_paths;
                    for (Polyline &boundary : prune_boundaries)
                        boundary.scale(diagnostic_scale);
                    const BoundingBox prune_bounds = get_extents(prune_boundaries);
                    const std::string prune_path = debug_out_path(
                        "fin-support/painted-shape-pruning-%d-%d.svg", int(intent_svg_run), int(svg_chart));
                    SVG prune_svg(prune_path, prune_bounds, scale_(2.));
                    if (prune_svg.is_opened()) {
                        prune_svg.add_comment("world XY, display scale=4; gray=first-pass removed; "
                                              "cyan=protected; purple=checked and retained; red=not leaf-checked; "
                                              "B=first-pass leaf check");
                        prune_svg.draw(prune_boundaries, "#606060", scale_(0.08));
                        auto draw_prune_paths = [&](const Polylines &paths, const char *color) {
                            for (const Polyline &line : paths)
                                prune_svg.draw(project_prune_path(line), color, scale_(0.12));
                        };
                        draw_prune_paths(coarse.pruned_paths, "#c0c0c0");
                        draw_prune_paths(coarse.protected_paths, "#56b4e9");
                        draw_prune_paths(coarse.checked_retained_paths, "#8e44ad");
                        draw_prune_paths(coarse.unchecked_paths, "#d62728");
                    }
                    auto draw_prune_label = [&](const Polyline &source, const std::string &label,
                                                const char *color, double fraction) {
                        if (prune_svg.is_opened()) {
                            const Polyline projected = project_prune_path(source);
                            Polyline first_half, second_half;
                            projected.split_at_length(fraction * projected.length(), &first_half, &second_half);
                            prune_svg.draw_text(first_half.last_point(), label.c_str(), color, 1);
                        }
                    };
                    for (size_t id = 0; id < coarse.prune_checks.size(); ++id) {
                        const auto &check = coarse.prune_checks[id];
                        const std::string label = "B" + std::to_string(id);
                        draw_prune_label(check.path, label, check.retained_edges > 0 ? "#8e44ad" : "#909090", 0.5);
                    }
                    BOOST_LOG_TRIVIAL(info)
                        << "Fin support object=" << object.model_object()->name
                        << " stage=painted-shape-pruning run=" << intent_svg_run << " chart=" << svg_chart
                        << " checks=" << coarse.prune_checks.size()
                        << " removed_edges=" << coarse.pruned_paths.size()
                        << " protected_edges=" << coarse.protected_paths.size()
                        << " checked_retained_edges=" << coarse.checked_retained_paths.size()
                        << " unchecked_edges=" << coarse.unchecked_paths.size()
                        << " svg_opened=" << prune_svg.is_opened() << " svg=" << prune_path;
                    if (prune_svg.is_opened())
                        prune_svg.Close();
                    if (first_intent_svg_path.empty())
                        first_intent_svg_path = path;
                    if (debug_retained_lines.empty()) {
                        ++intent_svg_no_line_files;
                        if (first_intent_svg_no_line_path.empty())
                            first_intent_svg_no_line_path = path;
                    }
                    ++intent_svg_files;
                }

            }
            if (intent.strokes.empty()) {
                if (contracted_length > EPSILON)
                    intent.center = contracted_first_moment / contracted_length;
                return intent;
            }

            // Every ordinary fit built in this chart belongs to the same
            // macro patch. Keep one projection frame for the complete patch;
            // redistributing nearby faces per fitted fragment would make a
            // continuous green chain acquire discontinuous normals.
            for (PaintedIntentStroke &stroke : intent.strokes)
                if (!stroke.spans_macro_crease)
                    stroke.macro_normal = intent.macro_normal;
            double total_length = 0;
            Vec3d first_moment = Vec3d::Zero();
            for (const PaintedIntentStroke &stroke : intent.strokes) {
                total_length += stroke.length;
                first_moment += stroke.length * stroke.center;
            }
            if (total_length > 0.)
                intent.center = first_moment / total_length;
            intent.kind = intent.strokes.size() == 1 ? PaintedIntentKind::line : PaintedIntentKind::branched;
            return intent;
        };

        auto append_island = [&](const std::vector<size_t> &faces) {
            const size_t island_id = result.islands.size();
            PaintedIsland island;
            Vec3d first_moment = Vec3d::Zero();
            for (const size_t face_id : faces) {
                const FaceGeometry &face = geometry[face_id];
                if (face.area <= 0)
                    continue;
                const std::array<Vec3d, 3> &v = face.vertices;
                const Vec3d &normal3 = face.normal;
                Vec2d normal = normal3.head<2>();
                if (split_painted_islands || normal.norm() >= 0.5) {
                    island.area += face.area;
                    first_moment += face.area * face.center;
                }
                if (normal.norm() <= EPSILON || (!split_painted_islands && normal.norm() < 0.5))
                    continue;
                normal.normalize();
                const double zmin = std::min({v[0].z(), v[1].z(), v[2].z()});
                const double zmax = std::max({v[0].z(), v[1].z(), v[2].z()});
                auto layer_it = std::lower_bound(layers.begin(), layers.end(), zmin,
                    [print_z_offset](const Layer *layer, double z) { return layer->print_z - print_z_offset < z; });
                for (; layer_it != layers.end() && (*layer_it)->print_z - print_z_offset - (*layer_it)->height <= zmax; ++layer_it) {
                    const Layer &layer = **layer_it;
                    // Sample just inside the bead's vertical extent to avoid choosing
                    // the neighboring face solely because it shares a horizontal edge.
                    const double top_z = layer.print_z - print_z_offset;
                    const double bottom_z = top_z - layer.height;
                    const double inset = std::min(1e-6, 0.5 * layer.height);
                    // Z anti-aliasing can move slice_z away from the layer center.
                    // Wall lookup uses that slice, but bead limits use physical Z.
                    const std::array<double, 3> zs{layer.slice_z, bottom_z + inset, top_z - inset};
                    for (size_t section = 0; section < zs.size(); ++section) {
                        const double z = zs[section];
                        if (z < zmin || z > zmax)
                            continue;
                        std::vector<Vec2d> intersections;
                        for (int i = 0; i < 3; ++i) {
                            const Vec3d &a = v[i], &b = v[(i + 1) % 3];
                            if (std::abs(b.z() - a.z()) < 1e-6 || z < std::min(a.z(), b.z()) || z > std::max(a.z(), b.z()))
                                continue;
                            const Vec2d p = a.head<2>() + (z - a.z()) / (b.z() - a.z()) * (b - a).head<2>();
                            if (std::none_of(intersections.begin(), intersections.end(),
                                             [&p](const Vec2d &q) { return (p - q).squaredNorm() < 1e-8; }))
                                intersections.push_back(p);
                        }
                        if (intersections.size() < 2)
                            continue;
                        const Vec2d mid = 0.5 * (intersections[0] + intersections[1]);
                        Vec2d section_normal = normal;
                        if (inside_model(layer, mid + 0.6 * section_normal))
                            section_normal = -section_normal;
                        if (inside_model(layer, mid + 0.6 * section_normal))
                            continue;
                        Vec3d outward_normal = normal3;
                        if (normal3.head<2>().dot(section_normal) < 0)
                            outward_normal = -outward_normal;
                        Vec3d macro_normal = macro_normals[face_id];
                        if (macro_normal.head<2>().dot(section_normal) < 0)
                            macro_normal = -macro_normal;
                        PaintedSegment segment{intersections[0], intersections[1], section_normal, outward_normal};
                        if (split_painted_islands)
                            segment.macro_normal = macro_normal;
                        segment.island = island_id;
                        result.layers[size_t(layer_it - layers.begin())].sections[section].push_back(std::move(segment));
                    }
                }
            }
            if (island.area > 1e-12) {
                island.center = first_moment / island.area;
                if (split_painted_islands) {
                    island.intent.center = island.center;
                    Vec3d island_macro_sum = Vec3d::Zero();
                    for (const size_t face_id : faces)
                        island_macro_sum += geometry[face_id].area * macro_normals[face_id];
                    if (island_macro_sum.squaredNorm() > EPSILON * EPSILON)
                        island.intent.macro_normal = island_macro_sum.normalized();

                    std::vector<bool> in_island(geometry.size(), false);
                    std::vector<bool> visited_patch(geometry.size(), false);
                    for (const size_t face_id : faces)
                        in_island[face_id] = true;
                    std::vector<size_t> pending_patch;
                    for (const size_t seed : faces) {
                        if (visited_patch[seed])
                            continue;
                        std::vector<size_t> patch_faces;
                        pending_patch.push_back(seed);
                        visited_patch[seed] = true;
                        while (!pending_patch.empty()) {
                            const size_t face_id = pending_patch.back();
                            pending_patch.pop_back();
                            patch_faces.push_back(face_id);
                            for (int edge_id = 0; edge_id < 3; ++edge_id) {
                                const int neighbor = neighbors[face_id](edge_id);
                                if (neighbor < 0 || barriers[face_id][edge_id] ||
                                    !in_island[size_t(neighbor)] || visited_patch[size_t(neighbor)])
                                    continue;
                                visited_patch[size_t(neighbor)] = true;
                                pending_patch.push_back(size_t(neighbor));
                            }
                            for (const size_t neighbor : painted_proximity_neighbors[face_id])
                                if (in_island[neighbor] && !visited_patch[neighbor]) {
                                    visited_patch[neighbor] = true;
                                    pending_patch.push_back(neighbor);
                                }
                        }
                        double patch_area = 0;
                        Vec3d patch_first_moment = Vec3d::Zero();
                        for (const size_t face_id : patch_faces) {
                            patch_area += geometry[face_id].area;
                            patch_first_moment += geometry[face_id].area * geometry[face_id].center;
                        }
                        if (patch_area <= EPSILON)
                            continue;
                        ++result.macro_intent_patches;
                        PaintedIntentGraph patch_intent = build_intent_graph(
                            patch_faces, patch_first_moment / patch_area, island.area);
                        const size_t edge_offset = island.intent.strokes.size();
                        const size_t node_offset = island.intent.nodes.size();
                        for (PaintedIntentNode &node : patch_intent.nodes) {
                            for (size_t &edge : node.edges)
                                edge += edge_offset;
                            if (node.main_edge)
                                *node.main_edge += edge_offset;
                            island.intent.nodes.push_back(std::move(node));
                        }
                        for (PaintedIntentStroke &stroke : patch_intent.strokes) {
                            for (size_t &node : stroke.nodes)
                                node += node_offset;
                            if (stroke.parent_edge)
                                *stroke.parent_edge += edge_offset;
                            island.intent.strokes.push_back(std::move(stroke));
                        }
                    }
                    if (!island.intent.strokes.empty()) {
                        double total_length = 0;
                        Vec3d intent_first_moment = Vec3d::Zero();
                        for (const PaintedIntentStroke &stroke : island.intent.strokes) {
                            total_length += stroke.length;
                            intent_first_moment += stroke.length * stroke.center;
                        }
                        if (total_length > 0.)
                            island.intent.center = intent_first_moment / total_length;
                        island.intent.kind = island.intent.strokes.size() == 1 ?
                            PaintedIntentKind::line : PaintedIntentKind::branched;
                    }
                } else {
                    island.intent.center = island.center;
                }
                result.islands.push_back(island);
            }
        };
        if (split_painted_islands) {
            // First retain exact mesh connectivity, including regions which meet
            // only at a subdivision vertex.
            std::vector<std::vector<size_t>> vertex_faces(facets.vertices.size());
            for (size_t face_id = 0; face_id < facets.indices.size(); ++face_id)
                for (int vertex : facets.indices[face_id])
                    vertex_faces[size_t(vertex)].push_back(face_id);
            std::vector<bool> visited(facets.indices.size(), false);
            std::vector<size_t> pending;
            std::vector<std::vector<size_t>> components;
            for (size_t seed = 0; seed < facets.indices.size(); ++seed) {
                if (visited[seed])
                    continue;
                std::vector<size_t> component;
                pending.push_back(seed);
                visited[seed] = true;
                while (!pending.empty()) {
                    const size_t face_id = pending.back();
                    pending.pop_back();
                    component.push_back(face_id);
                    for (int vertex : facets.indices[face_id])
                        for (const size_t neighbor : vertex_faces[size_t(vertex)])
                            if (!visited[neighbor]) {
                                visited[neighbor] = true;
                                pending.push_back(neighbor);
                            }
                }
                components.push_back(std::move(component));
            }

            // Strict painted facets can contain sub-triangle cracks along the
            // boundary of consecutive brush stamps. A gap smaller than half the
            // printable feature scale cannot represent a separate fin contact;
            // join such components before interpreting their shape. Reject
            // opposing normals so nearby opposite walls of a thin part remain
            // separate islands.
            struct ComponentGeometry {
                Vec3d minimum = Vec3d::Constant(std::numeric_limits<double>::infinity());
                Vec3d maximum = Vec3d::Constant(-std::numeric_limits<double>::infinity());
                Vec3d normal_sum = Vec3d::Zero();
            };
            std::vector<ComponentGeometry> component_geometry(components.size());
            for (size_t component_id = 0; component_id < components.size(); ++component_id)
                for (const size_t face_id : components[component_id]) {
                    component_geometry[component_id].normal_sum += geometry[face_id].area * geometry[face_id].normal;
                    for (const Vec3d &point : geometry[face_id].vertices) {
                        component_geometry[component_id].minimum =
                            component_geometry[component_id].minimum.cwiseMin(point);
                        component_geometry[component_id].maximum =
                            component_geometry[component_id].maximum.cwiseMax(point);
                    }
                }
            std::vector<size_t> parent(components.size());
            for (size_t component_id = 0; component_id < parent.size(); ++component_id)
                parent[component_id] = component_id;
            auto root = [&parent](size_t component_id) {
                while (parent[component_id] != component_id) {
                    parent[component_id] = parent[parent[component_id]];
                    component_id = parent[component_id];
                }
                return component_id;
            };
            const double join_distance = 0.5 * feature_scale;
            const double join_distance_squared = join_distance * join_distance;
            for (size_t first = 0; first < components.size(); ++first) {
                for (size_t second = first + 1; second < components.size(); ++second) {
                    const Vec3d &first_normal = component_geometry[first].normal_sum;
                    const Vec3d &second_normal = component_geometry[second].normal_sum;
                    if (first_normal.norm() > EPSILON && second_normal.norm() > EPSILON &&
                        first_normal.dot(second_normal) < -0.5 * first_normal.norm() * second_normal.norm())
                        continue;
                    Vec3d separation = Vec3d::Zero();
                    for (int axis = 0; axis < 3; ++axis)
                        separation[axis] = std::max({0.,
                            component_geometry[first].minimum[axis] - component_geometry[second].maximum[axis],
                            component_geometry[second].minimum[axis] - component_geometry[first].maximum[axis]});
                    if (separation.squaredNorm() > join_distance_squared)
                        continue;
                    bool components_near = false;
                    size_t patch_first = std::numeric_limits<size_t>::max();
                    size_t patch_second = std::numeric_limits<size_t>::max();
                    for (const size_t first_face : components[first]) {
                        for (const size_t second_face : components[second]) {
                            bool faces_near = false;
                            for (const Vec3d &first_point : geometry[first_face].vertices)
                                for (const Vec3d &second_point : geometry[second_face].vertices)
                                    if ((first_point - second_point).squaredNorm() <= join_distance_squared) {
                                        faces_near = true;
                                        break;
                                    }
                            if (!faces_near)
                                continue;
                            components_near = true;
                            if (macro_normals[first_face].dot(macro_normals[second_face]) >=
                                fin_crease_alignment) {
                                patch_first = first_face;
                                patch_second = second_face;
                                break;
                            }
                        }
                        if (patch_first != std::numeric_limits<size_t>::max())
                            break;
                    }
                    if (components_near) {
                        parent[root(second)] = root(first);
                        if (patch_first != std::numeric_limits<size_t>::max()) {
                            painted_proximity_neighbors[patch_first].push_back(patch_second);
                            painted_proximity_neighbors[patch_second].push_back(patch_first);
                        }
                    }
                }
            }
            std::vector<std::vector<size_t>> joined_components(components.size());
            for (size_t component_id = 0; component_id < components.size(); ++component_id) {
                std::vector<size_t> &joined = joined_components[root(component_id)];
                joined.insert(joined.end(), components[component_id].begin(), components[component_id].end());
            }
            size_t joined_count = 0;
            for (const std::vector<size_t> &component : joined_components)
                if (!component.empty()) {
                    ++joined_count;
                    append_island(component);
                }
            result.joined_painted_islands += components.size() - joined_count;
        } else {
            std::vector<size_t> faces(facets.indices.size());
            for (size_t i = 0; i < faces.size(); ++i)
                faces[i] = i;
            append_island(faces);
        }
    }
    if (intent_svg_files > 0)
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-skeleton-svg files=" << intent_svg_files
            << " no_usable_line_files=" << intent_svg_no_line_files
            << " first_file=" << first_intent_svg_path
            << " first_no_usable_line_file=" << first_intent_svg_no_line_path;
    return result;
}

bool clear_fin_line(const Layer &layer, const Polygon &bed, const Vec2d &from, const Vec2d &to,
                    double width, FinDiagnostics &diagnostics)
{
    const Vec2d direction = to - from;
    if (direction.squaredNorm() < 1e-8)
        return diagnostics.reject(FinReject::body_depth, layer.print_z, direction.norm());
    const Vec2d side(-direction.y(), direction.x());
    const Vec2d half_width = 0.5 * width * side.normalized();
    const int samples = std::max(2, int(std::ceil((to - from).norm() / std::max(width, 0.4))));
    for (int i = 0; i <= samples; ++i) {
        const Vec2d p = from + double(i) / samples * (to - from);
        if (!bed.contains(scaled_point(p + half_width)) || !bed.contains(scaled_point(p - half_width)))
            return diagnostics.reject(FinReject::bed_collision, layer.print_z);
        if (inside_model(layer, p) || inside_model(layer, p + half_width) || inside_model(layer, p - half_width))
            return diagnostics.reject(FinReject::model_collision, layer.print_z);
    }
    return true;
}

struct OuterContourLocation {
    size_t slice = std::numeric_limits<size_t>::max();
};

bool projected_outer_contour(const Layer &layer, const Vec2d &origin, const Vec2d &normal,
                             double outer_distance, Vec2d &nearest, Vec2d *surface_normal = nullptr,
                             OuterContourLocation *location = nullptr)
{
    double best_distance = -std::numeric_limits<double>::infinity();
    bool found = false;
    for (size_t slice_id = 0; slice_id < layer.lslices.size(); ++slice_id) {
        const ExPolygon &slice = layer.lslices[slice_id];
        const Points &contour = slice.contour.points;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            const double denominator = normal.x() * edge.y() - normal.y() * edge.x();
            if (std::abs(denominator) < 1e-12)
                continue;
            const Vec2d delta = a - origin;
            const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
            const double edge_fraction = (delta.x() * normal.y() - delta.y() * normal.x()) / denominator;
            if (edge_fraction < 0 || edge_fraction > 1 || distance >= outer_distance)
                continue;
            const Vec2d point = origin + distance * normal;
            if (distance > best_distance && !inside_model(layer, point + 0.6 * normal)) {
                nearest = point;
                best_distance = distance;
                found = true;
                if (location != nullptr)
                    *location = {slice_id};
                if (surface_normal != nullptr) {
                    *surface_normal = Vec2d(edge.y(), -edge.x()).normalized();
                    if (surface_normal->dot(normal) < 0)
                        *surface_normal = -*surface_normal;
                }
            }
        }
    }
    return found;
}

struct ContourRayHit {
    Vec2d point = Vec2d::Zero();
    Vec2d normal = Vec2d::Zero();
    double distance = 0.;
};

std::optional<ContourRayHit> ray_contour_hit(const Polygon &contour, const Vec2d &origin,
                                             const Vec2d &direction, double maximum_distance,
                                             const Vec2d &expected_outward)
{
    std::optional<ContourRayHit> result;
    const double orientation = contour.area() >= 0 ? 1. : -1.;
    for (size_t edge_id = 0; edge_id < contour.points.size(); ++edge_id) {
        const Vec2d a = unscale(contour.points[edge_id]);
        const Vec2d b = unscale(contour.points[(edge_id + 1) % contour.points.size()]);
        const Vec2d edge = b - a;
        if (edge.squaredNorm() <= EPSILON * EPSILON)
            continue;
        const double denominator = cross_2d(direction, edge);
        if (std::abs(denominator) <= 1e-12)
            continue;
        const Vec2d delta = a - origin;
        const double distance = cross_2d(delta, edge) / denominator;
        const double position = cross_2d(delta, direction) / denominator;
        if (distance < -1e-9 || distance > maximum_distance + 1e-9 ||
            position < -1e-9 || position > 1 + 1e-9 ||
            (result.has_value() && distance >= result->distance))
            continue;
        Vec2d normal = orientation * Vec2d(edge.y(), -edge.x()).normalized();
        if (normal.dot(expected_outward) <= 0.)
            continue;
        result = ContourRayHit{origin + std::max(0., distance) * direction,
                               normal, std::max(0., distance)};
    }
    return result;
}

struct ClosestContourPoint {
    Vec2d point = Vec2d::Zero();
    Vec2d normal = Vec2d::Zero();
    double distance = std::numeric_limits<double>::infinity();
};

std::optional<ClosestContourPoint> closest_contour_point(const Polygon &contour, const Vec2d &origin)
{
    std::optional<ClosestContourPoint> result;
    const double orientation = contour.area() >= 0 ? 1. : -1.;
    for (size_t edge_id = 0; edge_id < contour.points.size(); ++edge_id) {
        const Vec2d a = unscale(contour.points[edge_id]);
        const Vec2d b = unscale(contour.points[(edge_id + 1) % contour.points.size()]);
        const Vec2d edge = b - a;
        if (edge.squaredNorm() <= EPSILON * EPSILON)
            continue;
        const double position = std::clamp((origin - a).dot(edge) / edge.squaredNorm(), 0., 1.);
        const Vec2d point = a + position * edge;
        const double distance = (origin - point).norm();
        const Vec2d normal = orientation * Vec2d(edge.y(), -edge.x()).normalized();
        if ((result.has_value() && distance > result->distance + 1e-9) ||
            (result.has_value() && std::abs(distance - result->distance) <= 1e-9 &&
             normal.dot(origin - point) <= result->normal.dot(origin - result->point)))
            continue;
        result = ClosestContourPoint{point, normal, distance};
    }
    return result;
}

struct PaintedContactAxis {
    Vec2d body_start = Vec2d::Zero();
    Vec2d clearance_normal = Vec2d::Zero();
    bool contact_valid = false;
    Vec2d contact_point = Vec2d::Zero();
    Vec2d contact_direction = Vec2d::Zero();
    Vec2d surface_normal = Vec2d::Zero();
};

struct PaintedLayerGeometryCache {
    double line_width = 0.;
    // All three values have exact, explicit provenance: the current layer's
    // lslices, the object bed, the layer support width, the configured XY gap,
    // and round joins. They are shared only inside one Painted Shape run.
    ExPolygons model_gap;
    ExPolygons final_model_clearance;
    ExPolygons bed_center;
};

std::optional<ContourRayHit> painted_clearance_hit(const Layer &layer, const Vec2d &origin,
                                                 const Vec2d &fin_direction, double outer_distance,
                                                 double gap, double line_width,
                                                 const ExPolygons *cached_clearance)
{
    const ExPolygons computed_clearance = cached_clearance == nullptr ?
        (gap > EPSILON ? offset_ex(
            layer.lslices, float(scale_(gap)),
            ClipperLib::jtRound, DefaultMiterLimit) : layer.lslices) :
        ExPolygons{};
    const ExPolygons &clearance = cached_clearance != nullptr ?
        *cached_clearance : computed_clearance;
    const double probe = std::max(1e-4, 0.01 * line_width);
    auto inside_clearance = [&](const Vec2d &point) {
        const Point scaled = scaled_point(point);
        return std::any_of(clearance.begin(), clearance.end(),
            [&](const ExPolygon &area) { return area.contains(scaled, false); });
    };

    std::optional<ContourRayHit> clearance_hit;
    auto visit_clearance = [&](const Polygon &polygon) {
        const double orientation = polygon.area() >= 0 ? 1. : -1.;
        for (size_t edge_id = 0; edge_id < polygon.points.size(); ++edge_id) {
            const Vec2d a = unscale(polygon.points[edge_id]);
            const Vec2d b = unscale(polygon.points[(edge_id + 1) % polygon.points.size()]);
            const Vec2d edge = b - a;
            if (edge.squaredNorm() <= EPSILON * EPSILON)
                continue;
            const double denominator = cross_2d(fin_direction, edge);
            if (std::abs(denominator) <= 1e-12)
                continue;
            const Vec2d delta = a - origin;
            const double distance = cross_2d(delta, edge) / denominator;
            const double position = cross_2d(delta, fin_direction) / denominator;
            if (distance >= outer_distance ||
                position < -1e-9 || position > 1 + 1e-9 ||
                (clearance_hit.has_value() && distance <= clearance_hit->distance))
                continue;
            const Vec2d point = origin + distance * fin_direction;
            if (!inside_clearance(point - probe * fin_direction) ||
                inside_clearance(point + probe * fin_direction))
                continue;
            Vec2d normal = orientation * Vec2d(edge.y(), -edge.x()).normalized();
            if (normal.dot(fin_direction) < 0.)
                normal = -normal;
            clearance_hit = ContourRayHit{point, normal, distance};
        }
    };
    for (const ExPolygon &area : clearance) {
        visit_clearance(area.contour);
        for (const Polygon &hole : area.holes)
            visit_clearance(hole);
    }
    return clearance_hit;
}

std::optional<PaintedContactAxis> painted_contact_axis(const Layer &layer,
                                                       const OuterContourLocation &wall_location,
                                                       const Vec2d &fin_direction,
                                                       const ContourRayHit &clearance_hit,
                                                       double gap, double line_width)
{
    if (wall_location.slice >= layer.lslices.size())
        return std::nullopt;

    PaintedContactAxis result;
    result.body_start = clearance_hit.point;
    result.clearance_normal = clearance_hit.normal;
    const Polygon &host = layer.lslices[wall_location.slice].contour;
    const std::optional<ClosestContourPoint> closest =
        closest_contour_point(host, result.body_start);
    if (!closest.has_value())
        return result;

    Vec2d nearest_direction = result.body_start - closest->point;
    if (nearest_direction.norm() <= EPSILON)
        nearest_direction = result.clearance_normal;
    else
        nearest_direction.normalize();
    if (nearest_direction.dot(fin_direction) <= 0.)
        return result;

    const double maximum_distance = gap / min_fin_contact_alignment + line_width;
    if (closest->distance > maximum_distance + EPSILON)
        return result;

    Vec2d contact_direction = nearest_direction;
    const double alignment = std::clamp(fin_direction.dot(nearest_direction), -1., 1.);
    if (alignment < min_fin_contact_alignment) {
        const double sign = cross_2d(fin_direction, nearest_direction) < 0. ? -1. : 1.;
        const double sine = sign * std::sqrt(std::max(0., 1. -
            min_fin_contact_alignment * min_fin_contact_alignment));
        contact_direction = Vec2d(
            min_fin_contact_alignment * fin_direction.x() - sine * fin_direction.y(),
            sine * fin_direction.x() + min_fin_contact_alignment * fin_direction.y());
    }

    if (alignment >= min_fin_contact_alignment) {
        result.contact_point = closest->point;
        result.surface_normal = closest->normal;
    } else {
        const std::optional<ContourRayHit> hit = ray_contour_hit(
            host, result.body_start, -contact_direction, maximum_distance, contact_direction);
        if (!hit.has_value())
            return result;
        result.contact_point = hit->point;
        result.surface_normal = hit->normal;
    }
    result.contact_direction = contact_direction;
    result.contact_valid = true;
    return result;
}

Polygon foot_area(const Vec2d &start, const Vec2d &tip, const Vec2d &normal, double thickness, double width,
                  double expansion)
{
    const double envelope = expansion;
    const double half_width = 0.5 * thickness + envelope;
    const Vec2d tangent(-normal.y(), normal.x());
    const Vec2d inner = start - 0.5 * width * normal;
    const Vec2d outer = tip + envelope * normal;
    return Polygon{scaled_point(inner - half_width * tangent),
                   scaled_point(outer - half_width * tangent),
                   scaled_point(outer + half_width * tangent),
                   scaled_point(inner + half_width * tangent)};
}

bool trace_fin(const PrintObject &object, const Polygon &bed, const FinCandidate &candidate,
               double top_depth, double gap, double thickness, double line_width, TracedFin &fin,
               const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    if (candidate.axis_limit && std::abs(cross_2d(
            *candidate.axis_limit - candidate.point, candidate.normal)) > EPSILON)
        return fin.diagnostics.reject(FinReject::direction,
            object.get_layer(int(candidate.top_layer))->print_z);
    if (top_depth - gap < 2 * line_width)
        return fin.diagnostics.reject(FinReject::top_depth, object.get_layer(int(candidate.top_layer))->print_z,
                                      top_depth - gap);
    fin.boundary.resize(candidate.top_layer + 1);
    fin.wall_normals.assign(candidate.top_layer + 1, candidate.contact_normal);
    fin.contact_directions.assign(candidate.top_layer + 1, candidate.normal);
    fin.body_cap_normals.assign(candidate.top_layer + 1, candidate.normal);
    fin.wall_present.assign(candidate.top_layer + 1, false);
    fin.body_start.resize(candidate.top_layer + 1);
    fin.tips.resize(candidate.top_layer + 1);
    const double top_z = object.get_layer(int(candidate.top_layer))->slice_z;
    Vec2d target = candidate.point;
    const Vec2d side(-candidate.normal.y(), candidate.normal.x());
    // A fixes the sheet plane through the seed. Its contour ray selects the
    // physical host; Painted Shape resolves the round-clearance endpoint and
    // finite contact axis separately instead of projecting a wall normal onto A.
    double projected_inner = std::numeric_limits<double>::infinity();
    bool attached_to_wall = true;
    for (size_t remaining = candidate.top_layer + 1; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        if (!attached_to_wall) {
            fin.boundary[i] = fin.boundary[i + 1];
            fin.wall_normals[i] = fin.wall_normals[i + 1];
            fin.contact_directions[i] = fin.contact_directions[i + 1];
            fin.body_cap_normals[i] = fin.body_cap_normals[i + 1];
            fin.wall_present[i] = false;
            fin.body_start[i] = fin.body_start[i + 1];
            fin.tips[i] = fin.tips[i + 1];
            if (!clear_fin_line(layer, bed, fin.body_start[i], fin.tips[i], thickness, fin.diagnostics))
                return false;
            continue;
        }
        const double outer_distance = top_depth + (top_z - layer.slice_z) * candidate.expansion_slope;
        fin.tips[i] = candidate.point + outer_distance * candidate.normal;
        Vec2d point = target;
        Vec2d surface_normal = candidate.contact_normal;
        OuterContourLocation wall_location;
        const bool have_surface = projected_outer_contour(layer, candidate.point, candidate.normal,
                                                         outer_distance, point, &surface_normal,
                                                         &wall_location);
        if (i == candidate.top_layer && (!have_surface || (point - candidate.point).norm() > line_width))
            return fin.diagnostics.reject(FinReject::top_wall, layer.print_z, (point - candidate.point).norm());
        if (!candidate.requires_painted_contact) {
            const ExPolygons *cached_clearance =
                geometry_cache != nullptr && i < geometry_cache->size() ?
                    &(*geometry_cache)[i].model_gap : nullptr;
            // Query the gap boundary independently of model contact. In
            // particular, losing the wall must not hide an XY stopping point.
            const auto clearance = painted_clearance_hit(
                layer, candidate.point, candidate.normal, outer_distance,
                gap, line_width, cached_clearance);
            const std::optional<PaintedContactAxis> axis = have_surface && clearance ?
                painted_contact_axis(layer, wall_location, candidate.normal,
                                     *clearance, gap, line_width) : std::nullopt;
            if (i == candidate.top_layer &&
                (!axis.has_value() || !axis->contact_valid))
                return fin.diagnostics.reject(FinReject::contact_axis, layer.print_z);

            Vec2d from = clearance ? clearance->point :
                Vec2d(candidate.point + projected_inner * candidate.normal);
            if (clearance)
                fin.body_cap_normals[i] = clearance->normal;
            const bool at_limit = candidate.axis_limit &&
                (!clearance || (from - *candidate.axis_limit).dot(candidate.normal) <= 0.);
            if (at_limit) {
                // Clamp this layer to the actual endpoint, not the previous
                // layer's cap. A gap boundary farther outward always wins.
                from = *candidate.axis_limit;
                fin.body_cap_normals[i] = candidate.normal;
                attached_to_wall = false;
                fin.wall_detach_layer = i;
                fin.detached_at_axis_limit = true;
            }
            const double inner = (from - candidate.point).dot(candidate.normal);
            if (i < candidate.top_layer && inner > projected_inner + 0.5 * line_width)
                return fin.diagnostics.reject(
                    FinReject::projection_intrusion, layer.print_z, inner - projected_inner);
            projected_inner = std::min(projected_inner, inner);
            fin.wall_present[i] = !at_limit && axis && axis->contact_valid;
            if (fin.wall_present[i]) {
                fin.boundary[i] = axis->contact_point;
                fin.wall_normals[i] = axis->surface_normal;
                fin.contact_directions[i] = axis->contact_direction;
            } else {
                fin.boundary[i] = point;
                fin.wall_normals[i] = surface_normal;
                if (!at_limit)
                    fin.diagnostics.reject(FinReject::contact_axis, layer.print_z);
            }

            const double depth = (fin.tips[i] - from).dot(candidate.normal);
            if (depth < 2 * line_width)
                return fin.diagnostics.reject(FinReject::body_depth, layer.print_z, depth);
            if (!clear_fin_line(layer, bed, from, fin.tips[i], thickness, fin.diagnostics))
                return false;
            fin.body_start[i] = from;
            if (have_surface)
                target = point;
            continue;
        }
        const double alignment = surface_normal.dot(candidate.normal);
        if (have_surface && alignment <= EPSILON)
            return fin.diagnostics.reject(FinReject::wall_facing, layer.print_z, alignment);
        const double surface_inner = have_surface ? (point - candidate.point).dot(candidate.normal) +
            (gap + 0.5 * thickness * std::abs(side.dot(surface_normal))) / alignment : projected_inner;
        // Project the existing sheet downward, extend its inner edge to the wall, and clip model clearance.
        // A lower wall may not remove the load-bearing vertical projection of the upper sheet.
        if (i < candidate.top_layer && surface_inner > projected_inner + 0.5 * line_width)
            return fin.diagnostics.reject(FinReject::projection_intrusion, layer.print_z, surface_inner - projected_inner);
        projected_inner = std::min(projected_inner, surface_inner);
        const Vec2d from = candidate.point + std::max(projected_inner, surface_inner) * candidate.normal;
        const double depth = (fin.tips[i] - from).dot(candidate.normal);
        if (depth < 2 * line_width)
            return fin.diagnostics.reject(FinReject::body_depth, layer.print_z, depth);
        if (!clear_fin_line(layer, bed, from, fin.tips[i], thickness, fin.diagnostics))
            return false;
        fin.boundary[i] = point;
        fin.wall_normals[i] = surface_normal;
        fin.contact_directions[i] = candidate.normal;
        fin.body_cap_normals[i] = candidate.normal;
        fin.wall_present[i] = have_surface;
        fin.body_start[i] = from;
        target = point;
    }

    return true;
}

Polygon fin_rectangle(const Vec2d &inner, const Vec2d &outer, const Vec2d &normal, double thickness)
{
    const Vec2d side = 0.5 * thickness * Vec2d(-normal.y(), normal.x());
    return Polygon{scaled_point(inner - side), scaled_point(outer - side),
                   scaled_point(outer + side), scaled_point(inner + side)};
}

std::optional<std::pair<Vec2d, Vec2d>> fin_cap_segment(
    const Vec2d &center, const Vec2d &body_direction, const Vec2d &cap_normal,
    double half_width, double center_inset = 0.)
{
    const Vec2d body_side(-body_direction.y(), body_direction.x());
    const Vec2d cap_side(-cap_normal.y(), cap_normal.x());
    const double denominator = cross_2d(body_direction, cap_side);
    if (denominator <= EPSILON)
        return std::nullopt;
    const Vec2d cap_center = center + center_inset * cap_normal;
    auto intersection = [&](double side) {
        const Vec2d ray = center + side * half_width * body_side;
        const double distance = cross_2d(cap_center - ray, cap_side) / denominator;
        return ray + distance * body_direction;
    };
    return std::make_pair(intersection(-1.), intersection(1.));
}

Polygon capped_fin_body(const Vec2d &inner, const Vec2d &outer,
                        const Vec2d &body_direction, const Vec2d &cap_normal,
                        double thickness)
{
    const auto cap = fin_cap_segment(inner, body_direction, cap_normal, 0.5 * thickness);
    if (!cap.has_value())
        return {};
    const Vec2d side = 0.5 * thickness * Vec2d(-body_direction.y(), body_direction.x());
    return Polygon{scaled_point(cap->first), scaled_point(outer - side),
                   scaled_point(outer + side), scaled_point(cap->second)};
}

Polygon capped_fin_center(const Vec2d &inner, const Vec2d &outer,
                          const Vec2d &body_direction, const Vec2d &cap_normal,
                          double thickness, double line_width)
{
    const double center_thickness = thickness - line_width;
    if (center_thickness <= EPSILON)
        return {};
    const auto cap = fin_cap_segment(
        inner, body_direction, cap_normal,
        0.5 * center_thickness, 0.5 * line_width);
    if (!cap.has_value())
        return {};
    // W is the physical outer end. The center boundary stops half a bead
    // earlier so expanding the complete center region recovers W exactly.
    const Vec2d center_outer = outer - 0.5 * line_width * body_direction;
    const Vec2d side = 0.5 * center_thickness *
        Vec2d(-body_direction.y(), body_direction.x());
    return Polygon{scaled_point(cap->first), scaled_point(center_outer - side),
                   scaled_point(center_outer + side), scaled_point(cap->second)};
}

Polygon fin_contact_center_polygon(const Vec2d &first, const Vec2d &second,
                                   std::pair<Vec2d, Vec2d> cap)
{
    const double direct = (first - cap.first).squaredNorm() +
                          (second - cap.second).squaredNorm();
    const double crossed = (first - cap.second).squaredNorm() +
                           (second - cap.first).squaredNorm();
    if (crossed < direct)
        std::swap(cap.first, cap.second);
    if ((first - second).squaredNorm() <= EPSILON * EPSILON)
        return Polygon{scaled_point(first), scaled_point(cap.first),
                       scaled_point(cap.second)};
    return Polygon{scaled_point(first), scaled_point(cap.first),
                   scaled_point(cap.second), scaled_point(second)};
}

std::optional<std::pair<Vec2d, Vec2d>> analytical_fin_center_cap(
    const TracedFin &fin, size_t layer_id, double thickness, double line_width)
{
    // A1 and A define the member's longitudinal frame. The local clearance
    // normal defines only the closing line; never recover either endpoint from
    // an already offset or boolean-combined polygon.
    return fin_cap_segment(
        fin.body_start[layer_id], fin.seed.normal,
        fin.body_cap_normals[layer_id],
        0.5 * (thickness - line_width), 0.5 * line_width);
}

Polygon fin_contact_polygon(const Vec2d &front, const Vec2d &body_start,
                            const Vec2d &body_direction, const Vec2d &body_cap_normal,
                            const Vec2d &surface_normal, double contact_width,
                            double thickness, double line_width)
{
    const Vec2d tangent(-surface_normal.y(), surface_normal.x());
    const auto cap = fin_cap_segment(
        body_start, body_direction, body_cap_normal,
        0.5 * (thickness - line_width), 0.5 * line_width);
    if (!cap.has_value())
        return {};
    const Vec2d first = front - 0.5 * contact_width * tangent;
    const Vec2d second = contact_width > 0 ?
        front + 0.5 * contact_width * tangent : front;
    const Vec2d first_rear = cap->first + line_width * body_direction;
    const Vec2d second_rear = cap->second + line_width * body_direction;
    Points points{scaled_point(first), scaled_point(cap->first),
                  scaled_point(first_rear), scaled_point(second_rear),
                  scaled_point(cap->second)};
    if (contact_width > 0)
        points.push_back(scaled_point(second));
    return Geometry::convex_hull(std::move(points));
}

ExPolygons build_fin_contact_section(const Layer &layer, const Polygon &bed, const TracedFin &fin,
                                     size_t layer_id, const Vec2d &wall,
                                     const Vec2d &surface_normal,
                                     double contact_width, double thickness, double line_width,
                                     double *failure_stage = nullptr,
                                     const ExPolygons *cached_bed_center = nullptr)
{
    if (failure_stage != nullptr)
        *failure_stage = 1.;
    const Vec2d front = wall + 0.5 * line_width * surface_normal;
    Polygon neck;
    if (!fin.seed.requires_painted_contact) {
        const std::optional<std::pair<Vec2d, Vec2d>> cap = analytical_fin_center_cap(
            fin, layer_id, thickness, line_width);
        if (!cap.has_value())
            return {};
        const Vec2d tangent(-surface_normal.y(), surface_normal.x());
        const Vec2d first = front - 0.5 * contact_width * tangent;
        const Vec2d second = contact_width > 0. ?
            front + 0.5 * contact_width * tangent : front;
        neck = fin_contact_center_polygon(first, second, *cap);
    } else {
        neck = fin_contact_polygon(
            front, fin.body_start[layer_id], fin.seed.normal,
            fin.body_cap_normals[layer_id], surface_normal,
            contact_width, thickness, line_width);
    }
    if (neck.points.size() < 3) {
        if (failure_stage != nullptr)
            *failure_stage = 2.;
        return {};
    }
    const ExPolygons computed_bed_center = cached_bed_center == nullptr ?
        offset_ex(bed, -float(scale_(0.5 * line_width))) : ExPolygons{};
    const ExPolygons &bed_center = cached_bed_center != nullptr ?
        *cached_bed_center : computed_bed_center;
    ExPolygons result;
    if (!fin.seed.requires_painted_contact) {
        // Painted Shape builds the contact in extrusion-center space. Keep the
        // analytical Y/U-to-cap polygon intact here: clipping it against the
        // half-bead model offset before it becomes the final gap exemption can
        // remove its shared cap edge, leaving separate center components which
        // only reconnect after physical-width expansion. The completed layer
        // applies the one contact-and-transition-exempted model-clearance mask.
        result = intersection_ex(Polygons{neck}, bed_center);
    } else {
        const ExPolygons clearance = offset_ex(
            layer.lslices, float(scale_(0.5 * line_width)),
            DefaultJoinType, DefaultMiterLimit);
        result = intersection_ex(diff_ex(neck, clearance), bed_center);
    }
    if (failure_stage != nullptr)
        *failure_stage = result.empty() ? 3. : 0.;
    return result;
}

struct FinContactSlice {
    Vec2d wall = Vec2d::Zero();
    Vec2d wall_normal = Vec2d::Zero();
    Vec2d first = Vec2d::Zero();
    Vec2d second = Vec2d::Zero();
    Vec3d surface_normal = Vec3d::Zero(); // Simple/Simulated projection only.
};

std::optional<FinContactSlice> painted_layer_contact(const Layer &layer, const Vec2d &a1,
                                                    double contact_width, double line_width)
{
    std::optional<ClosestContourPoint> nearest;
    auto visit = [&](const Polygon &contour) {
        const auto candidate = closest_contour_point(contour, a1);
        if (candidate && (!nearest || candidate->distance < nearest->distance))
            nearest = candidate;
    };
    for (const ExPolygon &slice : layer.lslices) {
        visit(slice.contour);
        for (const Polygon &hole : slice.holes)
            visit(hole);
    }
    if (!nearest)
        return std::nullopt;
    const Vec2d axis = a1 - nearest->point;
    if (!axis.allFinite() || axis.norm() <= EPSILON)
        return std::nullopt;
    FinContactSlice contact;
    contact.wall = nearest->point;
    contact.wall_normal = axis.normalized();
    const Vec2d front = contact.wall + 0.5 * line_width * contact.wall_normal;
    const Vec2d tangent(-contact.wall_normal.y(), contact.wall_normal.x());
    contact.first = front - 0.5 * contact_width * tangent;
    contact.second = front + 0.5 * contact_width * tangent;
    return contact;
}

struct FinContactBand {
    size_t begin;
    size_t end;
};

struct ProjectionClip {
    ExPolygons model;
    ExPolygons bed;
    bool ready = false;
};

struct ProjectionResult {
    size_t layers = 0;
    size_t exposed_layers = 0;
    bool complete = true;
    ProjectionStop stop = ProjectionStop::bed;
};

enum class FinExtensionState { pending, materializing, complete, invalid };
enum class FinExtensionLayerState { inactive, contact, materialized, absorbed, invalid };

struct FinCenterExtension {
    FinContactBand band{0, 0};
    FinExtensionState state = FinExtensionState::pending;
    // Geometry and completion state remain tied to this member's A ray until
    // every required layer has either materialized or been absorbed by its
    // analytically capped body.
    std::vector<ExPolygons> layers;
    std::vector<FinExtensionLayerState> layer_states;
};

struct FinComponentCenterGeometry {
    size_t source = std::numeric_limits<size_t>::max();
    bool complete = false;
    // Member-local body centers are prepared once and retained until the
    // intent graph is assembled. A standalone member reuses them directly;
    // a compound line uses them for projection absorption while constructing
    // its connected centerline envelope once at the component barrier.
    std::vector<ExPolygons> body_centers;
    // Extensions retain their member identity until the compound completion
    // barrier separates support transitions from actual contact sections.
    std::vector<FinCenterExtension> extensions;
};

struct ContactProjectionRay {
    Vec2d direction;
    double slope;
};

std::optional<ContactProjectionRay> contact_projection_ray(const Vec3d &surface_normal,
                                                            const Vec2d &fin_direction, double angle)
{
    // Simple and Simulated retain the original A/Z projection. Painted Shape
    // clips each local contact section against a fixed inclined plane.
    const double outward = surface_normal.head<2>().dot(fin_direction);
    if (outward <= EPSILON)
        return std::nullopt;
    const double radians = angle * M_PI / 180.;
    const double radial = outward * std::cos(radians) + surface_normal.z() * std::sin(radians);
    const double downward = outward * std::sin(radians) - surface_normal.z() * std::cos(radians);
    if (downward <= 1e-12)
        return std::nullopt;
    return ContactProjectionRay{fin_direction, radial / downward};
}

ExPolygons fin_body_area(const Layer &layer, const TracedFin &fin, size_t layer_id,
                         double thickness, double gap)
{
    const Polygon body = capped_fin_body(
        fin.body_start[layer_id], fin.tips[layer_id], fin.seed.normal,
        fin.body_cap_normals[layer_id], thickness);
    if (body.points.size() < 3)
        return {};
    const ExPolygons clearance = offset_ex(
        layer.lslices, float(scale_(gap)),
        fin.seed.requires_painted_contact ? DefaultJoinType : ClipperLib::jtRound,
        DefaultMiterLimit);
    return diff_ex(body, clearance);
}

ExPolygons fin_body_center_area(const Polygon &bed, const TracedFin &fin, size_t layer_id,
                                double thickness, double line_width,
                                const ExPolygons *cached_bed_center = nullptr)
{
    const Polygon center = capped_fin_center(
        fin.body_start[layer_id], fin.tips[layer_id], fin.seed.normal,
        fin.body_cap_normals[layer_id], thickness, line_width);
    if (center.points.size() < 3)
        return {};
    // Painted Shape keeps the analytically capped body intact while contacts
    // and downward transitions are solved. Layer finalization subtracts the
    // contact-and-transition-exempted model-clearance mask from the complete
    // support center.
    const ExPolygons computed_bed_center = cached_bed_center == nullptr ?
        offset_ex(bed, -float(scale_(0.5 * line_width))) : ExPolygons{};
    return intersection_ex(
        Polygons{center}, cached_bed_center != nullptr ?
            *cached_bed_center : computed_bed_center);
}

void build_fin_body_section(const PrintObject &object, const TracedFin &fin, size_t layer_id,
                            const Polygon &bed, double thickness, double gap,
                            ExPolygons &body, ExPolygons &body_center,
                            bool materialize_painted_body = true,
                            const ExPolygons *cached_bed_center = nullptr)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object, float(layer.height)).width();
    if (!fin.seed.requires_painted_contact) {
        body_center = fin_body_center_area(
            bed, fin, layer_id, thickness, width, cached_bed_center);
        if (layer_id == 0 && !body_center.empty()) {
            ExPolygons foot = offset_ex(
                body_center,
                float(scale_(object.config().raft_first_layer_expansion.value)),
                jtRound, double(scale_(0.005)));
            foot = intersection_ex(
                foot, cached_bed_center != nullptr ?
                    *cached_bed_center :
                    offset_ex(bed, -float(scale_(0.5 * width))));
            expolygons_append(body_center, std::move(foot));
            body_center = union_ex(body_center);
        }
        if (!materialize_painted_body)
            return;
        body = offset_ex(body_center, float(scale_(0.5 * width)),
                         jtRound, double(scale_(0.005)));
        return;
    }
    body = intersection_ex(fin_body_area(layer, fin, layer_id, thickness, gap), ExPolygons{ExPolygon(bed)});
    if (layer_id == 0) {
        ExPolygons foot = diff_ex(intersection_ex(
            Polygons{foot_area(fin.body_start.front(), fin.tips.front(), fin.seed.normal, thickness, width,
                               object.config().raft_first_layer_expansion.value)},
            offset_ex(bed, -float(scale_(0.5 * width)))),
            offset_ex(layer.lslices, float(scale_(gap + 0.5 * width)),
                      fin.seed.requires_painted_contact ? DefaultJoinType : ClipperLib::jtRound,
                      DefaultMiterLimit));
        expolygons_append(body, std::move(foot));
        body = union_ex(body);
    }
    body_center = offset_ex(body, -float(scale_(0.5 * width)));
}

ProjectionResult backfill_legacy_contact_projection(const PrintObject &object, const TracedFin &fin, size_t bottom,
                                                    const Polygon &bed, double thickness,
                                                    double contact_width, double gap, const Vec3d &surface_normal,
                                                    const std::vector<ExPolygons> &body_centers,
                                                    std::vector<ExPolygons> &contacts,
                                                    std::vector<ProjectionClip> &clips,
                                                    std::vector<ExPolygons> *projection_copy = nullptr)
{
    ProjectionResult result;
    const Vec2d &body_normal = fin.seed.normal;
    const auto ray = contact_projection_ray(surface_normal, body_normal,
        object.config().fin_support_contact_projection_angle.value);
    if (!ray) {
        result.stop = ProjectionStop::no_downward;
        return result;
    }
    const double surface_horizontal = surface_normal.head<2>().norm();
    if (surface_horizontal <= EPSILON) {
        result.stop = ProjectionStop::no_downward;
        return result;
    }
    const Vec2d contact_normal = surface_normal.head<2>() / surface_horizontal;
    const Vec2d &projection_direction = ray->direction;
    const double slope = ray->slope;
    // Project just the front segment (a point for zero length). Its endpoints
    // retain the surface orientation while its center descends in the fin plane
    // and connects directly to the current lower layer's inner body corners.
    const Layer &contact_layer = *object.get_layer(int(bottom));
    const double contact_line_width = support_material_flow(&object, float(contact_layer.height)).width();
    Vec2d front = fin.boundary[bottom] + 0.5 * contact_line_width * contact_normal;
    const Vec2d initial_front = front;
    double advance = 0;
    const double neck_depth = (fin.body_start[bottom] - fin.boundary[bottom]).dot(projection_direction);
    for (size_t remaining = bottom; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        const double width = support_material_flow(&object, float(layer.height)).width();
        const double dz = object.get_layer(int(i + 1))->slice_z - layer.slice_z;
        front += dz * slope * projection_direction;
        advance += dz * slope;
        // A projection only carries load; actual contacts come from this
        // layer's own band, including when another band's projection overlaps it.
        const double clearance = std::min(gap, 0.25 * width);
        const Vec2d &body_cap_normal = fin.body_cap_normals[i];
        const Vec2d cap_center = fin.body_start[i] + 0.5 * width * body_cap_normal;
        const double fraction = std::clamp(advance / std::max(neck_depth, EPSILON), 0., 1.);
        const double rear_span = (thickness - width) *
            std::abs(body_cap_normal.dot(contact_normal));
        const double projected_length = contact_width - std::max(0., contact_width - rear_span) * fraction;
        const Vec2d side = 0.5 * projected_length * Vec2d(-contact_normal.y(), contact_normal.x());
        const Point a = scaled_point(front - side), b = scaled_point(front + side);
        if (std::any_of(body_centers[i].begin(), body_centers[i].end(), [&a, &b](const ExPolygon &body) {
            return a == b ? body.contains(a) : body.contains(Line(a, b));
        })) {
            result.stop = ProjectionStop::absorbed;
            break;
        }
        const Vec2d body_side = 0.5 * (thickness - width) * Vec2d(-body_normal.y(), body_normal.x());
        const double outer_limit = std::max(cap_center.dot(projection_direction),
                                           (fin.tips[i] - 0.5 * width * body_normal).dot(projection_direction)) +
                                   std::abs(body_side.dot(projection_direction));
        if (front.dot(projection_direction) > outer_limit + EPSILON) {
            result.stop = ProjectionStop::outside;
            break;
        }
        ProjectionClip &clip = clips[i];
        if (!clip.ready) {
            clip.model = offset_ex(
                layer.lslices, float(scale_(clearance + 0.5 * width)),
                fin.seed.requires_painted_contact ? DefaultJoinType : ClipperLib::jtRound,
                DefaultMiterLimit);
            clip.bed = offset_ex(bed, -float(scale_(0.5 * width)));
            clip.ready = true;
        }
        // The front retains the local surface orientation while its center
        // advances along A. The original neck ends on the ray-derived body cap;
        // beyond that cap, projection must fit the lower body center area.
        const Polygon neck = fin_contact_polygon(
            initial_front, fin.body_start[i], body_normal, body_cap_normal, contact_normal,
            contact_width, thickness, width);
        if (neck.points.size() < 3) {
            result.stop = ProjectionStop::clipped;
            break;
        }
        const Vec2d tangent(-body_normal.y(), body_normal.x());
        double inner = std::numeric_limits<double>::infinity();
        double side_min = std::numeric_limits<double>::infinity();
        double side_max = -std::numeric_limits<double>::infinity();
        for (const Point &point : neck.points) {
            const Vec2d p = unscale(point);
            inner = std::min(inner, p.dot(body_normal));
            side_min = std::min(side_min, p.dot(tangent));
            side_max = std::max(side_max, p.dot(tangent));
        }
        const Vec2d side_center = 0.5 * (side_min + side_max) * tangent;
        const Polygon neck_limit = fin_rectangle((inner - width) * body_normal + side_center,
            (cap_center.dot(body_normal) + width) * body_normal + side_center,
            body_normal, side_max - side_min + 2 * width);
        ExPolygons allowed = intersection_ex(Polygons{neck}, Polygons{neck_limit});
        expolygons_append(allowed, body_centers[i]);
        allowed = union_ex(allowed);
        ExPolygons projection = intersection_ex(diff_ex(fin_contact_polygon(
            front, fin.body_start[i], body_normal, body_cap_normal, contact_normal,
            projected_length, thickness, width),
            clip.model), clip.bed);
        projection = intersection_ex(projection, allowed);
        if (projection.empty()) {
            result.stop = ProjectionStop::clipped;
            break;
        }
        if (diff_ex(projection, body_centers[i]).empty()) {
            result.stop = ProjectionStop::absorbed;
            break;
        }
        if (projection_copy != nullptr)
            expolygons_append((*projection_copy)[i], projection);
        expolygons_append(contacts[i], std::move(projection));
        ++result.layers;
        ++result.exposed_layers;
    }
    return result;
}

ProjectionResult backfill_painted_contact_projection(
    const PrintObject &object, const TracedFin &fin, size_t bottom,
    const Polygon &bed, double thickness, double contact_width,
    const FinContactSlice &bottom_contact,
    const std::vector<ExPolygons> &exposure_body_centers,
    const std::vector<ExPolygons> &absorption_body_centers,
    std::vector<ExPolygons> &contacts, std::vector<ProjectionClip> &clips,
    std::vector<ExPolygons> *projection_copy = nullptr,
    FinCenterExtension *extension = nullptr,
    const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    ProjectionResult result;
    auto invalidate = [&]() {
        result.complete = false;
        result.stop = ProjectionStop::clipped;
        if (extension != nullptr)
            extension->state = FinExtensionState::invalid;
        return result;
    };
    if (extension != nullptr)
        extension->state = FinExtensionState::materializing;

    const double angle = object.config().fin_support_contact_projection_angle.value * M_PI / 180.;
    // Rotate the lowest horizontal plane about its contact tangent by 90-angle.
    // Its retained half-space is cos(angle)*axis.dot(P.xy-X) +
    // sin(angle)*(P.z-X.z) >= 0. The hinge and plane never change per layer.
    const Vec2d axis = bottom_contact.wall_normal;
    const Vec2d tangent(-axis.y(), axis.x());
    const Vec2d origin = bottom_contact.wall;
    const double bottom_z = object.get_layer(int(bottom))->slice_z;
    const double horizontal_factor = std::cos(angle);
    const double vertical_factor = std::sin(angle);
    std::vector<ExPolygons> planned(bottom);
    std::vector<bool> planned_exposed(bottom, false);
    for (size_t remaining = bottom; remaining > 0; --remaining) {
        if (object.print()->canceled())
            throw CanceledException();
        const size_t layer_id = remaining - 1;
        const Layer &layer = *object.get_layer(int(layer_id));
        const double width = support_material_flow(&object, float(layer.height)).width();
        const auto contact = painted_layer_contact(layer, fin.body_start[layer_id], contact_width, width);
        const auto cap = analytical_fin_center_cap(fin, layer_id, thickness, width);
        if (!contact || !cap) {
            return invalidate();
        }
        const Polygon transition = fin_contact_center_polygon(contact->first, contact->second, *cap);
        if (transition.points.size() < 3)
            return invalidate();

        // Bound a half-plane mask by this layer's own polygon, not by an
        // arbitrary global extent. Classify before dividing so 0/90 degrees
        // and planes already past the polygon need no distant coordinates.
        double min_s = std::numeric_limits<double>::infinity();
        double max_s = -min_s;
        double min_t = min_s;
        double max_t = -min_s;
        for (const Point &point : transition.points) {
            const Vec2d delta = unscale(point) - origin;
            min_s = std::min(min_s, delta.dot(axis));
            max_s = std::max(max_s, delta.dot(axis));
            min_t = std::min(min_t, delta.dot(tangent));
            max_t = std::max(max_t, delta.dot(tangent));
        }
        const double drop = bottom_z - layer.slice_z;
        const double cut = vertical_factor * drop;
        ExPolygons projection;
        if (horizontal_factor * max_s <= cut) {
            continue;
        }
        if (horizontal_factor * min_s >= cut) {
            projection = union_ex(Polygons{transition});
        } else {
            const double s = cut / horizontal_factor;
            const Polygon retained{
                scaled_point(origin + s * axis + (min_t - EPSILON) * tangent),
                scaled_point(origin + (max_s + EPSILON) * axis + (min_t - EPSILON) * tangent),
                scaled_point(origin + (max_s + EPSILON) * axis + (max_t + EPSILON) * tangent),
                scaled_point(origin + s * axis + (max_t + EPSILON) * tangent)};
            projection = intersection_ex(Polygons{transition}, Polygons{retained});
        }
        ProjectionClip &clip = clips[layer_id];
        const ExPolygons *printable_bed = nullptr;
        if (geometry_cache != nullptr && layer_id < geometry_cache->size()) {
            printable_bed = &(*geometry_cache)[layer_id].bed_center;
        } else {
            if (!clip.ready)
                clip.bed = offset_ex(bed, -float(scale_(0.5 * width)));
            clip.ready = true;
            printable_bed = &clip.bed;
        }
        projection = intersection_ex(projection, *printable_bed);
        if (projection.empty())
            continue;
        if (diff_ex(projection, absorption_body_centers[layer_id]).empty()) {
            if (extension != nullptr)
                extension->layer_states[layer_id] = FinExtensionLayerState::absorbed;
            continue;
        }
        planned_exposed[layer_id] = !diff_ex(projection, exposure_body_centers[layer_id]).empty();
        planned[layer_id] = std::move(projection);
        if (extension != nullptr)
            extension->layer_states[layer_id] = FinExtensionLayerState::materialized;
    }
    // Keep publication atomic: a failed section must not leave a partial band.
    for (size_t layer_id = 0; layer_id < bottom; ++layer_id) {
        if (planned[layer_id].empty())
            continue;
        if (projection_copy != nullptr)
            expolygons_append((*projection_copy)[layer_id], planned[layer_id]);
        if (extension != nullptr)
            expolygons_append(extension->layers[layer_id], planned[layer_id]);
        else
            expolygons_append(contacts[layer_id], std::move(planned[layer_id]));
        ++result.layers;
        result.exposed_layers += planned_exposed[layer_id];
    }
    if (extension != nullptr)
        extension->state = FinExtensionState::complete;
    result.stop = ProjectionStop::bed;
    return result;
}

ProjectionResult backfill_contact_projection(
    const PrintObject &object, const TracedFin &fin, size_t bottom,
    const Polygon &bed, double thickness, double contact_width, double gap,
    const FinContactSlice &bottom_contact,
    const std::vector<ExPolygons> &exposure_body_centers,
    const std::vector<ExPolygons> &absorption_body_centers,
    std::vector<ExPolygons> &contacts, std::vector<ProjectionClip> &clips,
    std::vector<ExPolygons> *projection_copy = nullptr,
    FinCenterExtension *extension = nullptr,
    const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    if (fin.seed.requires_painted_contact)
        return backfill_legacy_contact_projection(
            object, fin, bottom, bed, thickness, contact_width, gap,
            bottom_contact.surface_normal, absorption_body_centers, contacts, clips,
            projection_copy);
    return backfill_painted_contact_projection(
        object, fin, bottom, bed, thickness, contact_width, bottom_contact,
        exposure_body_centers, absorption_body_centers,
        contacts, clips, projection_copy, extension, geometry_cache);
}

bool closed_outline(const ExPolygons &area, const ExPolygons &outer_centerlines,
                    const Vec2d &normal, double inset, double line_width, Polyline &outline)
{
    // The complete bead, not just its centerline, must fit inside the sheet.
    if (offset_ex(area, -float(scale_(inset + 0.5 * line_width - EPSILON))).empty())
        return false;
    // The outer contour is the union of body and projected contact center areas.
    // Eroding a short combined neck again can disconnect it at its merge layer.
    ExPolygons centerlines = outer_centerlines.empty() ? offset_ex(area, -float(scale_(inset))) : outer_centerlines;
    if (centerlines.size() != 1 || !centerlines.front().holes.empty())
        return false;
    Polygon &contour = centerlines.front().contour;
    if (!contour.is_valid())
        return false;
    auto start = std::max_element(contour.points.begin(), contour.points.end(), [&normal](const Point &a, const Point &b) {
        return unscale(a).dot(normal) < unscale(b).dot(normal);
    });
    std::rotate(contour.points.begin(), start, contour.points.end());
    outline = contour.split_at_first_point();
    return true;
}

size_t append_closed_outlines(const ExPolygons &area, const ExPolygons &outer_centerlines,
                              const Vec2d &normal, double inset, double line_width,
                              Polylines &outlines)
{
    if (offset_ex(area, -float(scale_(inset + 0.5 * line_width - EPSILON))).empty())
        return 0;
    ExPolygons centerlines = outer_centerlines.empty() ?
        offset_ex(area, -float(scale_(inset))) : outer_centerlines;
    Polylines generated;
    auto append = [&](Polygon contour) {
        if (!contour.is_valid())
            return false;
        auto start = std::max_element(contour.points.begin(), contour.points.end(),
            [&normal](const Point &a, const Point &b) {
                return unscale(a).dot(normal) < unscale(b).dot(normal);
            });
        std::rotate(contour.points.begin(), start, contour.points.end());
        generated.push_back(contour.split_at_first_point());
        return true;
    };
    for (const ExPolygon &centerline : centerlines) {
        if (!append(centerline.contour))
            return 0;
        for (const Polygon &hole : centerline.holes)
            if (!append(hole))
                return 0;
    }
    const size_t count = generated.size();
    for (Polyline &outline : generated)
        outlines.push_back(std::move(outline));
    return count;
}

const PaintedSegment *painted_contact(const std::vector<PaintedSegment> &segments, const Vec2d &point,
                                     const Vec2d &normal, double width,
                                     size_t painted_island = std::numeric_limits<size_t>::max())
{
    const PaintedSegment *nearest = nullptr;
    double nearest_distance = width + EPSILON;
    for (const PaintedSegment &segment : segments) {
        if ((painted_island != std::numeric_limits<size_t>::max() && segment.island != painted_island) ||
            segment.normal.dot(normal) <= 0)
            continue;
        // Follow the contact plane rather than projecting perpendicularly onto a
        // facet: those directions differ on curved surfaces and at other Zs.
        const Vec2d edge = segment.b - segment.a;
        const double denominator = normal.x() * edge.y() - normal.y() * edge.x();
        if (std::abs(denominator) < 1e-12)
            continue;
        const Vec2d delta = segment.a - point;
        const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
        const double fraction = (delta.x() * normal.y() - delta.y() * normal.x()) / denominator;
        if (fraction < -1e-7 || fraction > 1 + 1e-7 || std::abs(distance) > nearest_distance)
            continue;
        nearest = &segment;
        nearest_distance = std::abs(distance);
    }
    return nearest;
}

// Diagnose a failed lookup without changing its acceptance rules. This extra
// scan runs only on failure and reports the condition hiding the painted cut.
void diagnose_paint_lookup(const std::vector<PaintedSegment> &segments, const Vec2d &point,
                           const Vec2d &normal, double z, FinDiagnostics &diagnostics,
                           size_t painted_island = std::numeric_limits<size_t>::max())
{
    const bool has_paint = std::any_of(segments.begin(), segments.end(), [painted_island](const PaintedSegment &segment) {
        return painted_island == std::numeric_limits<size_t>::max() || segment.island == painted_island;
    });
    if (!has_paint) {
        diagnostics.reject(FinReject::paint_empty, z);
        return;
    }
    bool facing = false, nonparallel = false;
    double nearest = std::numeric_limits<double>::infinity();
    double overrun = std::numeric_limits<double>::infinity();
    for (const PaintedSegment &segment : segments) {
        if ((painted_island != std::numeric_limits<size_t>::max() && segment.island != painted_island) ||
            segment.normal.dot(normal) <= 0)
            continue;
        facing = true;
        const Vec2d edge = segment.b - segment.a;
        const double denominator = normal.x() * edge.y() - normal.y() * edge.x();
        if (std::abs(denominator) < 1e-12)
            continue;
        nonparallel = true;
        const Vec2d delta = segment.a - point;
        const double fraction = (delta.x() * normal.y() - delta.y() * normal.x()) / denominator;
        if (fraction < -1e-7 || fraction > 1 + 1e-7) {
            overrun = std::min(overrun, std::abs(fraction - std::clamp(fraction, 0., 1.)) * edge.norm());
            continue;
        }
        const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
        nearest = std::min(nearest, std::abs(distance));
    }
    if (!facing)
        diagnostics.reject(FinReject::paint_facing, z);
    else if (!nonparallel)
        diagnostics.reject(FinReject::paint_parallel, z);
    else if (std::isfinite(nearest))
        diagnostics.reject(FinReject::paint_distance, z, nearest);
    else
        diagnostics.reject(FinReject::paint_endpoint, z, overrun);
}

bool contact_contour_samples(const Layer &layer, const Vec2d &point, const Vec2d &normal,
                             double contact_width, double line_width, std::vector<Vec2d> &walls,
                             FinDiagnostics *diagnostics = nullptr)
{
    auto reject = [&](FinReject reason, double value = 0) {
        if (diagnostics != nullptr)
            diagnostics->reject(reason, layer.print_z, value);
        return false;
    };
    const Vec2d tangent(-normal.y(), normal.x());
    // Both bead edges must land on the allowed wall, not just its mathematical centerline.
    const double bead_span = contact_width + line_width;
    const int samples = std::max(2, int(std::ceil(bead_span / line_width)));
    walls.clear();
    walls.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        const Vec2d edge = point + (double(i) / samples - 0.5) * bead_span * tangent;
        Vec2d wall = edge;
        if (!projected_outer_contour(layer, edge, normal, 2 * line_width, wall))
            return reject(FinReject::contact_contour, -1);
        if ((wall - edge).norm() > line_width)
            return reject(FinReject::contact_contour, (wall - edge).norm());
        walls.push_back(wall);
    }
    return true;
}

bool painted_contact_width(const Layer &layer, const PaintedLayer &painted,
                           const Vec2d &point, const Vec2d &normal, double contact_width, double line_width,
                           Vec3d *surface_normal = nullptr, FinDiagnostics *diagnostics = nullptr,
                           bool check_top_section = true, const Vec3d *macro_normal = nullptr,
                           size_t painted_island = std::numeric_limits<size_t>::max())
{
    auto reject = [&](FinReject reason, double value = 0) {
        if (diagnostics != nullptr)
            diagnostics->reject(reason, layer.print_z, value);
        return false;
    };
    std::vector<Vec2d> walls;
    if (!contact_contour_samples(layer, point, normal, contact_width, line_width, walls, diagnostics))
        return false;
    Vec3d normal_sum = Vec3d::Zero();
    for (const Vec2d &wall : walls) {
        const size_t section_count = check_top_section ? painted.sections.size() : painted.sections.size() - 1;
        for (size_t section = 0; section < section_count; ++section) {
            const PaintedSegment *surface = painted_contact(
                painted.sections[section], wall, normal, line_width, painted_island);
            if (surface == nullptr) {
                if (diagnostics != nullptr)
                    diagnose_paint_lookup(painted.sections[section], wall, normal, layer.print_z,
                                          *diagnostics, painted_island);
                return reject(FinReject::contact_span, double(section));
            }
            // Simple and Simulated retain their local non-upward rule. Painted
            // Shape validates the same paint span but obtains tilt from its
            // crease-aware neighborhood instead of one tessellated facet.
            if (macro_normal == nullptr && surface->surface_normal.z() <= 0)
                normal_sum += surface->surface_normal;
        }
    }
    if (macro_normal != nullptr) {
        const double horizontal = macro_normal->head<2>().norm();
        if (horizontal < min_fin_macro_horizontal)
            return reject(FinReject::contact_normal, horizontal);
        if (surface_normal != nullptr) {
            const double slope = macro_normal->z() / horizontal;
            *surface_normal = Vec3d(normal.x(), normal.y(), slope).normalized();
        }
    } else {
        if (normal_sum.squaredNorm() <= EPSILON * EPSILON)
            return reject(FinReject::contact_normal);
        if (surface_normal != nullptr)
            *surface_normal = normal_sum.normalized();
    }
    return true;
}

bool validate_fin_contact(const Layer &layer, const PaintedLayer &painted, const Polygons *blockers,
                          bool wall_present, const Vec2d &wall, const Vec2d &fin_normal,
                          double contact_width, double line_width, Vec3d &surface_normal,
                          FinDiagnostics &diagnostics, bool check_top_section = true,
                          const Vec3d *macro_normal = nullptr,
                          size_t painted_island = std::numeric_limits<size_t>::max())
{
    if (!wall_present)
        return diagnostics.reject(FinReject::missing_wall, layer.print_z);
    const PaintedSegment *surface = painted_contact(
        painted.sections[0], wall, fin_normal, line_width, painted_island);
    if (surface == nullptr) {
        diagnose_paint_lookup(painted.sections[0], wall, fin_normal, layer.print_z,
                              diagnostics, painted_island);
        return diagnostics.reject(FinReject::missing_paint, layer.print_z);
    }
    if (blockers != nullptr && contains(*blockers, scaled_point(wall)))
        return diagnostics.reject(FinReject::blocked_contact, layer.print_z);
    if (!painted_contact_width(layer, painted, wall, surface->normal, contact_width,
                               line_width, &surface_normal, &diagnostics, check_top_section,
                               macro_normal, painted_island))
        return false;
    const Vec2d contact_normal = surface_normal.head<2>().normalized();
    if (!fin_contact_aligned(fin_normal, contact_normal)) {
        const double alignment = fin_normal.dot(contact_normal);
        return diagnostics.reject(FinReject::contact_turn, layer.print_z,
            std::acos(std::clamp(alignment, -1., 1.)) * 180 / M_PI);
    }
    return true;
}

// Painted Shape treats paint as an intent source. Once its seed has been
// selected from that painted island, each layer resolves its own shortest
// contact axis against the actual model wall. The complete bead need
// not remain inside the brush mask.
bool validate_model_fin_contact(const Layer &layer, const Polygons *blockers,
                                bool wall_present, const Vec2d &wall,
                                const Vec2d &wall_normal,
                                double contact_width, double line_width,
                                FinDiagnostics &diagnostics)
{
    if (!wall_present)
        return diagnostics.reject(FinReject::missing_wall, layer.print_z);
    if (blockers != nullptr && contains(*blockers, scaled_point(wall)))
        return diagnostics.reject(FinReject::blocked_contact, layer.print_z);
    std::vector<Vec2d> walls;
    if (!contact_contour_samples(layer, wall, wall_normal, contact_width,
                                 line_width, walls, &diagnostics))
        return false;
    return true;
}

bool finalize_legacy_fin_geometry(const PrintObject &object, TracedFin &fin,
                                  std::vector<ExPolygons> &bodies,
                                  const std::vector<ExPolygons> &body_centers,
                                  std::vector<ExPolygons> &contact_sections,
                                  std::vector<ExPolygons> &projections)
{
    const size_t count = bodies.size();
    fin.areas.resize(count);
    fin.centers.resize(count);
    fin.contact_centers.resize(count);
    fin.contact_projection_retained_layers = 0;
    for (size_t i = 0; i < count; ++i) {
        const Layer &layer = *object.get_layer(int(i));
        const double width = support_material_flow(&object, float(layer.height)).width();
        contact_sections[i] = union_ex(contact_sections[i]);
        projections[i] = union_ex(projections[i]);
        fin.contact_centers[i] = contact_sections[i];
        expolygons_append(fin.contact_centers[i], projections[i]);
        fin.contact_centers[i] = union_ex(fin.contact_centers[i]);

        ExPolygons support_centers = body_centers[i];
        expolygons_append(support_centers, projections[i]);
        expolygons_append(support_centers, contact_sections[i]);
        support_centers = union_ex(support_centers);
        fin.centers[i] = std::move(support_centers);
        if (!projections[i].empty()) {
            const ExPolygons missing_projection = diff_ex(projections[i], fin.centers[i]);
            if (missing_projection.empty())
                ++fin.contact_projection_retained_layers;
        }
        const bool joined_centers = fin.centers[i].size() == 1 &&
            fin.centers[i].front().holes.empty();
        expolygons_append(bodies[i], offset_ex(
            fin.contact_centers[i], float(scale_(0.5 * width)),
            jtRound, double(scale_(0.005))));
        ExPolygons area = union_ex(bodies[i]);
        const unsigned topology =
            (area.size() != 1 ? 1u : 0u) |
            (area.size() == 1 && !area.front().holes.empty() ? 2u : 0u) |
            (fin.centers[i].size() != 1 ? 4u : 0u) |
            (fin.centers[i].size() == 1 && !fin.centers[i].front().holes.empty() ? 8u : 0u) |
            (fin.centers[i].size() == 1 && !fin.centers[i].front().contour.is_valid() ? 16u : 0u) |
            (!joined_centers ? 32u : 0u);
        if (topology != 0) {
            return fin.diagnostics.reject(
                FinReject::topology, object.get_layer(int(i))->print_z, topology);
        }
        if (offset_ex(area, -float(scale_(width - EPSILON))).empty())
            return fin.diagnostics.reject(FinReject::thin_body, object.get_layer(int(i))->print_z);
        fin.areas[i] = std::move(area);
    }
    return true;
}

std::vector<FinContactBand> periodic_fin_contact_bands(const PrintObject &object, const TracedFin &fin)
{
    const PrintObjectConfig &config = object.config();
    // Convert the physical layer bottom to the model frame; surface samples
    // and the contact-band height comparisons below use slice_z.
    const double bed_z = object.get_layer(0)->print_z - object.get_layer(0)->height -
        object.slicing_parameters().object_print_z_min;
    const double first_top = bed_z + config.fin_support_contact_start_height.value;
    const double seed_top = object.get_layer(int(fin.seed.top_layer))->slice_z;
    const double height = config.fin_support_contact_height.value;
    const double spacing = config.fin_support_contact_spacing.value;
    const size_t count = fin.seed.top_layer + 1;
    std::vector<FinContactBand> result;
    size_t layer_id = 0;
    for (size_t band_id = 0;; ++band_id) {
        const double top = first_top + double(band_id) * spacing;
        if (top >= seed_top - 1e-9)
            break;
        const double bottom = top - height;
        while (layer_id < count && object.get_layer(int(layer_id))->slice_z <= bottom + 1e-9)
            ++layer_id;
        const size_t begin = layer_id;
        while (layer_id < count && object.get_layer(int(layer_id))->slice_z <= top + 1e-9)
            ++layer_id;
        if (begin < layer_id)
            result.push_back({begin, layer_id});
    }
    return result;
}

FinContactBand seed_fin_contact_band(const PrintObject &object, const TracedFin &fin)
{
    const size_t end = fin.seed.top_layer + 1;
    const double top = object.get_layer(int(fin.seed.top_layer))->slice_z;
    const double bottom = top - object.config().fin_support_contact_height.value;
    size_t begin = 0;
    while (begin < end && object.get_layer(int(begin))->slice_z <= bottom + 1e-9)
        ++begin;
    return {begin, end};
}

bool generate_fin_geometry(const PrintObject &object, TracedFin &fin, const Polygon &bed,
                           const std::vector<PaintedLayer> &painted,
                           const std::vector<Polygons> &blockers,
                           bool include_seed_contact = true,
                           FinComponentCenterGeometry *component_geometry = nullptr,
                           const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    const PrintObjectConfig &config = object.config();
    const double thickness = fin.seed.dimensions.thickness;
    const double contact_width = fin.seed.dimensions.contact_width;
    const double gap = config.support_object_xy_distance.value;
    const size_t count = fin.boundary.size();
    const bool staged_painted =
        component_geometry != nullptr && !fin.seed.requires_painted_contact;
    std::vector<ExPolygons> bodies(count), body_centers(count);
    std::vector<ExPolygons> contact_sections(count), projections(count);
    std::vector<ProjectionClip> clips(count);
    if (component_geometry != nullptr) {
        component_geometry->complete = false;
        component_geometry->body_centers.clear();
        component_geometry->extensions.clear();
    }
    auto record_projection = [&](const ProjectionResult &projection) {
        ++fin.contact_projection_attempts;
        fin.contact_projection_layers += projection.layers;
        fin.contact_projection_max_layers = std::max(fin.contact_projection_max_layers, projection.layers);
        fin.contact_projection_zero_layers += projection.layers == 0;
        fin.contact_projection_exposed_layers += projection.exposed_layers;
        ++fin.contact_projection_stops[size_t(projection.stop)];
    };
    fin.contacts.assign(count, false);
    fin.body_axes.resize(count);
    for (size_t i = 0; i < count; ++i) {
        const double line_width = support_material_flow(
            &object, float(object.get_layer(int(i))->height)).width();
        const Vec2d outer_center = !fin.seed.requires_painted_contact ?
            fin.tips[i] - 0.5 * line_width * fin.seed.normal : fin.tips[i];
        fin.body_axes[i].push_back({outer_center, fin.body_start[i],
            fin.seed.normal, fin.body_cap_normals[i], fin.seed.intent_edge, false});
        const ExPolygons *cached_bed_center =
            geometry_cache != nullptr && i < geometry_cache->size() ?
                &(*geometry_cache)[i].bed_center : nullptr;
        build_fin_body_section(
            object, fin, i, bed, thickness, gap, bodies[i], body_centers[i],
            component_geometry == nullptr || fin.seed.requires_painted_contact,
            cached_bed_center);
    }
    // Body/cap state continues vertically after reaching the XY limit, but model
    // contact does not. Clip before resolving walls so that a periodic band
    // cannot reacquire a surface below the stop and move its projection origin.
    auto clip_contact_band = [&](FinContactBand band) {
        if (!fin.seed.requires_painted_contact && fin.wall_detach_layer) {
            const size_t begin = std::max(band.begin, *fin.wall_detach_layer + 1);
            band.begin = std::min(begin, band.end);
        }
        return band;
    };
    // 0: unauthorized or invalid band, 1: complete band, -1: fatal contact geometry.
    auto prepare_band = [&](const FinContactBand &band,
                            std::vector<ExPolygons> &sections,
                            std::vector<FinContactSlice> &contact_slices) {
        if (band.begin >= band.end)
            return 0;
        ++fin.planned_contact_bands;
        contact_slices.resize(band.end - band.begin);
        for (size_t i = band.begin; i < band.end; ++i) {
            const Layer &layer = *object.get_layer(int(i));
            const double line_width = support_material_flow(&object, float(layer.height)).width();
            const Polygons *layer_blockers = i < blockers.size() ? &blockers[i] : nullptr;
            bool valid = false;
            Vec2d wall = fin.boundary[i];
            Vec2d wall_normal = fin.wall_normals[i];
            bool wall_present = fin.wall_present[i];
            FinContactSlice &contact = contact_slices[i - band.begin];
            if (fin.seed.requires_painted_contact) {
                const Vec3d *macro_normal =
                    fin.seed.surface_normals == FinSurfaceNormalPolicy::painted_region ?
                    &fin.seed.macro_normal : nullptr;
                valid = validate_fin_contact(
                    layer, painted[i], layer_blockers, fin.wall_present[i],
                    fin.boundary[i], fin.seed.normal, contact_width, line_width,
                    contact.surface_normal, fin.diagnostics, i != fin.seed.top_layer,
                    macro_normal,
                    fin.seed.wall_follow_island.value_or(std::numeric_limits<size_t>::max()));
            } else {
                const auto local = painted_layer_contact(layer, fin.body_start[i], contact_width, line_width);
                if (!local) {
                    fin.diagnostics.reject(FinReject::contact_axis, layer.print_z);
                    return 0;
                }
                wall = local->wall;
                wall_normal = local->wall_normal;
                contact = *local;
                // Painted Shape accepts A at candidate selection. Validate the
                // actual contact geometry without another direction-angle veto.
                valid = validate_model_fin_contact(
                    layer, layer_blockers, wall_present, wall,
                    wall_normal, contact_width, line_width, fin.diagnostics);
            }
            if (!valid)
                return 0;
            if (fin.seed.requires_painted_contact) {
                contact.wall = wall;
                contact.wall_normal = contact.surface_normal.head<2>().normalized();
                const Vec2d front = wall + 0.5 * line_width * contact.wall_normal;
                const Vec2d tangent(-contact.wall_normal.y(), contact.wall_normal.x());
                contact.first = front - 0.5 * contact_width * tangent;
                contact.second = front + 0.5 * contact_width * tangent;
            }
        }
        sections.resize(band.end - band.begin);
        for (size_t i = band.begin; i < band.end; ++i) {
            const Layer &layer = *object.get_layer(int(i));
            const double line_width = support_material_flow(&object, float(layer.height)).width();
            const FinContactSlice &contact = contact_slices[i - band.begin];
            double failure_stage = 0.;
            sections[i - band.begin] = build_fin_contact_section(
                layer, bed, fin, i, contact.wall, contact.wall_normal,
                contact_width, thickness, line_width, &failure_stage,
                geometry_cache != nullptr && i < geometry_cache->size() ?
                    &(*geometry_cache)[i].bed_center : nullptr);
            if (sections[i - band.begin].empty()) {
                fin.diagnostics.reject(FinReject::empty_neck, layer.print_z, failure_stage);
                return -1;
            }
        }
        return 1;
    };
    auto append_band = [&](const FinContactBand &band, std::vector<ExPolygons> &sections,
                           std::vector<ExPolygons> *copy) -> std::optional<size_t> {
        ++fin.accepted_contact_bands;
        std::optional<size_t> extension_id;
        if (component_geometry != nullptr) {
            extension_id = component_geometry->extensions.size();
            FinCenterExtension &extension = component_geometry->extensions.emplace_back();
            extension.band = band;
            extension.layers.resize(count);
            extension.layer_states.assign(count, FinExtensionLayerState::inactive);
        }
        for (size_t i = band.begin; i < band.end; ++i) {
            fin.contacts[i] = true;
            if (copy != nullptr)
                (*copy)[i].insert((*copy)[i].end(), sections[i - band.begin].begin(),
                                  sections[i - band.begin].end());
            if (extension_id.has_value()) {
                FinCenterExtension &extension =
                    component_geometry->extensions[*extension_id];
                extension.layers[i] = sections[i - band.begin];
                extension.layer_states[i] = FinExtensionLayerState::contact;
            }
            if (!staged_painted)
                expolygons_append(
                    contact_sections[i],
                    std::move(sections[i - band.begin]));
        }
        return extension_id;
    };

    // The seed contact is independent of the periodic bed-anchored schedule.
    // Build it first so its actual downward projection can suppress redundant
    // periodic contacts below it without regenerating either band.
    std::vector<ExPolygons> seed_reach(count);
    if (include_seed_contact) {
        const FinContactBand seed_band = clip_contact_band(seed_fin_contact_band(object, fin));
        std::vector<ExPolygons> seed_sections;
        std::vector<FinContactSlice> seed_contacts;
        const int seed_status = prepare_band(seed_band, seed_sections, seed_contacts);
        if (seed_status < 0)
            return false;
        if (seed_status == 0)
            return fin.diagnostics.reject(FinReject::top_band,
                                          object.get_layer(int(fin.seed.top_layer))->print_z);
        const std::optional<size_t> extension_id =
            append_band(seed_band, seed_sections, &seed_reach);
        ++fin.seed_contact_bands;
        if (seed_band.begin > 0) {
            FinCenterExtension *extension = extension_id.has_value() ?
                &component_geometry->extensions[*extension_id] : nullptr;
            const ProjectionResult projection = backfill_contact_projection(
                object, fin, seed_band.begin, bed, thickness, contact_width, gap,
                seed_contacts.front(), body_centers, body_centers,
                projections, clips, &seed_reach, extension, geometry_cache);
            record_projection(projection);
            if (!projection.complete)
                return false;
        }
        if (extension_id.has_value() && seed_band.begin == 0)
            component_geometry->extensions[*extension_id].state = FinExtensionState::complete;
    }

    const std::vector<FinContactBand> periodic_bands = periodic_fin_contact_bands(object, fin);
    for (auto band_it = periodic_bands.rbegin(); band_it != periodic_bands.rend(); ++band_it) {
        const FinContactBand band = clip_contact_band(*band_it);
        std::vector<ExPolygons> sections;
        std::vector<FinContactSlice> band_contacts;
        const int status = prepare_band(band, sections, band_contacts);
        if (status < 0)
            return false;
        if (status == 0)
            continue;
        const size_t top_layer = band.end - 1;
        const Layer &layer = *object.get_layer(int(top_layer));
        const double line_width = support_material_flow(&object, float(layer.height)).width();
        const bool reaches_seed = !seed_reach[top_layer].empty() && !intersection_ex(
            offset_ex(sections.back(), float(scale_(0.5 * line_width))),
            offset_ex(seed_reach[top_layer], float(scale_(0.5 * line_width)))).empty();
        if (reaches_seed) {
            --fin.planned_contact_bands;
            ++fin.suppressed_contact_bands;
            continue;
        }
        const std::optional<size_t> extension_id =
            append_band(band, sections, nullptr);
        if (band.begin > 0) {
            FinCenterExtension *extension = extension_id.has_value() ?
                &component_geometry->extensions[*extension_id] : nullptr;
            const ProjectionResult projection = backfill_contact_projection(
                object, fin, band.begin, bed, thickness, contact_width, gap,
                band_contacts.front(), body_centers, body_centers,
                projections, clips, nullptr, extension, geometry_cache);
            record_projection(projection);
            if (!projection.complete)
                return false;
        }
        if (extension_id.has_value() && band.begin == 0)
            component_geometry->extensions[*extension_id].state = FinExtensionState::complete;
    }
    if (include_seed_contact && !fin.contacts.back())
        return fin.diagnostics.reject(FinReject::top_band, object.get_layer(int(fin.seed.top_layer))->print_z);

    if (component_geometry != nullptr) {
        component_geometry->complete = std::all_of(
            component_geometry->extensions.begin(), component_geometry->extensions.end(),
            [](const FinCenterExtension &extension) {
                return extension.state == FinExtensionState::complete;
            });
        if (!component_geometry->complete)
            return false;
        if (!fin.seed.requires_painted_contact) {
            component_geometry->body_centers = std::move(body_centers);
            return true;
        }
        component_geometry->body_centers = body_centers;
    }

    return finalize_legacy_fin_geometry(
        object, fin, bodies, body_centers, contact_sections, projections);
}

bool generate_fin_toolpaths(const PrintObject &object, TracedFin &fin)
{
    const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
    fin.outlines.resize(fin.areas.size());
    // The bed layer is generated from the union of all fin feet.
    for (size_t i = 1; i < fin.areas.size(); ++i) {
        const Layer &layer = *object.get_layer(int(i));
        const Flow flow = support_material_flow(&object, float(layer.height));
        for (int wall = 0; wall < wall_count; ++wall) {
            Polyline outline;
            if (!closed_outline(fin.areas[i], wall == 0 ? fin.centers[i] : ExPolygons{}, fin.seed.normal,
                                0.5 * flow.width() + wall * flow.spacing(), flow.width(), outline))
                break;
            fin.outlines[i].push_back(std::move(outline));
        }
        if (fin.outlines[i].empty())
            return fin.diagnostics.reject(FinReject::no_outline, layer.print_z);
    }
    return true;
}

void generate_fin_foot_toolpaths(const PrintObject &object, const std::vector<TracedFin> &fins,
                                Polylines &paths, Polygons &fill)
{
    ExPolygons areas, centers;
    for (const TracedFin &fin : fins) {
        expolygons_append(areas, fin.areas.front());
        if (!fin.centers.empty())
            expolygons_append(centers, fin.centers.front());
    }
    areas = union_ex(areas);
    const Layer &layer = *object.get_layer(0);
    const Flow flow = support_material_flow(&object, float(layer.height));
    // Erode the merged footprint, not each foot separately: this removes
    // internal seams even where individual center contours do not overlap.
    // Keep the saved contact centers so short fronts and zero-length tips survive.
    expolygons_append(centers, offset_ex(areas, -float(scale_(0.5 * flow.width()))));
    centers = union_ex(centers);
    const Lines model_edges = to_lines(layer.lslices);
    auto clearance = [&model_edges](const Point &point) {
        double distance = std::numeric_limits<double>::infinity();
        for (const Line &edge : model_edges)
            distance = std::min(distance, edge.distance_to_squared(point));
        return distance;
    };
    const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
    for (int wall = 0; wall < wall_count; ++wall) {
        const double inset = 0.5 * flow.width() + wall * flow.spacing();
        if (offset_ex(areas, -float(scale_(inset + 0.5 * flow.width() - EPSILON))).empty())
            break;
        const ExPolygons loops = wall == 0 ? centers : offset_ex(areas, -float(scale_(inset)));
        for (Polygon contour : to_polygons(loops)) {
            if (!contour.is_valid())
                continue;
            auto start = std::max_element(contour.points.begin(), contour.points.end(),
                [&clearance](const Point &a, const Point &b) { return clearance(a) < clearance(b); });
            std::rotate(contour.points.begin(), start, contour.points.end());
            paths.push_back(contour.split_at_first_point());
        }
    }
    // Reserve the emitted walls once for the whole bed layer before support infill.
    fill = to_polygons(diff_ex(offset_ex(areas, -float(scale_(0.5 * flow.width()))),
                              offset(paths, float(scale_(0.5 * flow.spacing())))));
}

bool has_contact_normal(const PaintedLayer &painted)
{
    return std::any_of(painted.sections.begin(), painted.sections.end(), [](const auto &section) {
        return std::any_of(section.begin(), section.end(), [](const PaintedSegment &segment) {
            return segment.surface_normal.z() <= 0;
        });
    });
}

// Join coplanar painted cuts before scanning their XY angular intervals.
// Mesh triangulation must not split one usable interval into facet fragments.
std::vector<PaintedSegment> simulated_candidate_runs(const PaintedLayer &painted)
{
    std::vector<PaintedSegment> segments;
    if (!has_contact_normal(painted))
        return segments;
    // FinCandidate XY is defined at the model slice height. Keep candidate
    // discovery on the same section as layer.lslices and final wall lookup.
    for (PaintedSegment segment : painted.sections[0]) {
        const Vec2d tangent(-segment.normal.y(), segment.normal.x());
        if (segment.a.dot(tangent) > segment.b.dot(tangent))
            std::swap(segment.a, segment.b);
        segments.push_back(std::move(segment));
    }
    std::sort(segments.begin(), segments.end(), [](const PaintedSegment &a, const PaintedSegment &b) {
        const double angle_a = std::atan2(a.normal.y(), a.normal.x());
        const double angle_b = std::atan2(b.normal.y(), b.normal.x());
        if (angle_a != angle_b)
            return angle_a < angle_b;
        const double offset_a = a.a.dot(a.normal), offset_b = b.a.dot(b.normal);
        if (offset_a != offset_b)
            return offset_a < offset_b;
        const Vec2d tangent(-a.normal.y(), a.normal.x());
        return a.a.dot(tangent) < b.a.dot(tangent);
    });
    std::vector<PaintedSegment> runs;
    for (const PaintedSegment &segment : segments) {
        if (!runs.empty()) {
            PaintedSegment &run = runs.back();
            const Vec2d tangent(-run.normal.y(), run.normal.x());
            if ((run.normal - segment.normal).squaredNorm() < 1e-10 &&
                std::abs((segment.a - run.a).dot(run.normal)) < 1e-4 &&
                segment.a.dot(tangent) <= run.b.dot(tangent) + 1e-4) {
                if (segment.b.dot(tangent) > run.b.dot(tangent))
                    run.b = segment.b;
                continue;
            }
        }
        runs.push_back(segment);
    }
    return runs;
}

// Rank locations on a painted run against the entire sliced face. Painting
// limits where a seed is allowed; it does not define the face's center.
void locate_fin_face(const Layer &layer, PaintedSegment &run, double tolerance)
{
    const Vec2d tangent(-run.normal.y(), run.normal.x());
    const double run_min = std::min(run.a.dot(tangent), run.b.dot(tangent));
    const double run_max = std::max(run.a.dot(tangent), run.b.dot(tangent));
    double best_span = 0;
    double best_midpoint = 0.5 * (run_min + run_max);
    for (const ExPolygon &slice : layer.lslices) {
        const Points &contour = slice.contour.points;
        double face_min = std::numeric_limits<double>::infinity();
        double face_max = -std::numeric_limits<double>::infinity();
        bool touches_run = false;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            if (edge.norm() <= EPSILON ||
                std::abs(edge.normalized().dot(tangent)) < fin_direction_merge_alignment ||
                std::abs((a - run.a).dot(run.normal)) > tolerance ||
                std::abs((b - run.a).dot(run.normal)) > tolerance)
                continue;
            const double low = std::min(a.dot(tangent), b.dot(tangent));
            const double high = std::max(a.dot(tangent), b.dot(tangent));
            touches_run |= high >= run_min - tolerance && low <= run_max + tolerance;
            face_min = std::min(face_min, low);
            face_max = std::max(face_max, high);
        }
        if (touches_run && face_max - face_min > best_span) {
            best_span = face_max - face_min;
            best_midpoint = 0.5 * (face_min + face_max);
        }
    }
    run.face_span = std::max(best_span, run_max - run_min);
    run.face_center = run.a + (best_midpoint - run.a.dot(tangent)) * tangent;
}

// Intersect a forward XY ray with a painted cut. A determines orientation;
// the intersection supplies the seed's XY and the cut's layer supplies Z.
bool fin_ray_contact(const Vec2d &origin, const Vec2d &direction,
                     const Vec2d &a, const Vec2d &b, Vec2d &point)
{
    const Vec2d edge = b - a;
    const double denominator = direction.x() * edge.y() - direction.y() * edge.x();
    if (std::abs(denominator) <= EPSILON)
        return false;
    const Vec2d delta = a - origin;
    const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
    const double fraction = (delta.x() * direction.y() - delta.y() * direction.x()) / denominator;
    if (distance <= EPSILON || fraction < -1e-9 || fraction > 1 + 1e-9)
        return false;
    point = origin + distance * direction;
    return true;
}

struct FinPlan {
    FinCandidate seed;
    FinCandidate initial_seed;
    bool active = true;
    double farthest_along_sheet = 0;
    std::vector<FinCandidate> tops;
};

// A's plane stays fixed while its top follows the painted wall. Moving along A
// changes depth, not the sheet plane or its previously established contacts.
bool fin_line_contact(const Vec2d &origin, const Vec2d &direction,
                      const PaintedSegment &run, Vec2d &point)
{
    const Vec2d edge = run.b - run.a;
    const double denominator = direction.x() * edge.y() - direction.y() * edge.x();
    if (std::abs(denominator) <= EPSILON)
        return false;
    const Vec2d delta = run.a - origin;
    const double fraction = (delta.x() * direction.y() - delta.y() * direction.x()) / denominator;
    if (fraction < -1e-9 || fraction > 1 + 1e-9)
        return false;
    const double distance = (delta.x() * edge.y() - delta.y() * edge.x()) / denominator;
    point = origin + distance * direction;
    return true;
}

const PrintRegion *fin_wall_region(const Layer &layer, const Vec2d &wall, const Vec2d &normal, double width)
{
    const Point inside = scaled_point(wall - 0.25 * width * normal);
    for (const LayerRegion *region : layer.regions())
        if (std::any_of(region->slices.surfaces.begin(), region->slices.surfaces.end(),
            [&inside](const Surface &surface) { return surface.expolygon.contains(inside); }))
            return &region->region();
    return nullptr;
}

bool fin_seed_contact(const PrintObject &object, const PaintedLayer &painted,
                      const std::vector<Polygons> &blockers, size_t layer_id,
                      const Vec2d &point, const Vec2d &direction, const PaintedSegment &run,
                      FinSupportDimensions &dimensions, Vec3d &normal, FinDiagnostics &diagnostics)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object).width();
    Vec2d wall = point;
    if (!projected_outer_contour(layer, point, direction, 2 * width, wall) || (wall - point).norm() > width)
        return diagnostics.reject(FinReject::top_wall, layer.print_z);
    const PrintRegion *region = fin_wall_region(layer, wall, run.normal, width);
    if (region == nullptr)
        return diagnostics.reject(FinReject::seed_region, layer.print_z);
    dimensions = fin_support_dimensions(object, *region);
    const Polygons *layer_blockers = layer_id < blockers.size() ? &blockers[layer_id] : nullptr;
    return validate_fin_contact(layer, painted, layer_blockers, true, wall, direction,
                                dimensions.contact_width, width, normal, diagnostics, false);
}

bool select_simulated_fin_candidate(const PrintObject &object, const std::vector<PaintedLayer> &painted,
                                    const std::vector<Polygons> &blockers,
                                    const std::vector<PaintedSegment> &runs,
                                    size_t layer_id, const Vec2d &origin, const Vec2d &desired, double risk,
                                    const std::vector<FinPlan> &plans,
                                    const std::vector<FinCandidate> &rejected,
                                    FinDiagnostics &diagnostics, FinCandidate &result)
{
    struct Site { Vec2d point; Vec2d direction; const PaintedSegment *run; double alignment; double position_error; };
    std::vector<Site> sites;
    const double width = support_material_flow(&object).width();
    for (const PaintedSegment &run : runs) {
        const Vec2d edge = run.b - run.a;
        if (edge.norm() <= EPSILON)
            continue;
        // Choose A once for the painted run. Using centroid-to-site for every
        // alternative makes sheets on one flat face fan out radially.
        const double turn = std::atan2(desired.x() * run.normal.y() - desired.y() * run.normal.x(),
                                       desired.dot(run.normal));
        if (std::abs(turn) > M_PI / 3. + 1e-9)
            continue;
        const double correction = std::copysign(std::max(0., std::abs(turn) - M_PI / 6.), turn);
        const Vec2d direction(std::cos(correction) * desired.x() - std::sin(correction) * desired.y(),
                              std::sin(correction) * desired.x() + std::cos(correction) * desired.y());
        const double alignment = direction.dot(desired);
        std::vector<Vec2d> points;
        Vec2d direct;
        if (fin_ray_contact(origin, direction, run.a, run.b, direct))
            points.push_back(direct);
        const double inset = std::min(0.5, 0.5 * width / edge.norm());
        const double middle = std::clamp((run.face_center - run.a).dot(edge) / edge.squaredNorm(),
                                          inset, 1 - inset);
        points.push_back(run.a + middle * edge);
        // Other sites let the same layer add another sheet after the nearest
        // one has reduced demand. These are alternatives, not direction bins.
        for (double fraction : {0.25, 0.5, 0.75})
            points.push_back(run.a + fraction * edge);
        for (const Vec2d &point : points) {
            if ((point - origin).dot(direction) > EPSILON &&
                alignment >= min_fin_contact_alignment - 1e-9) {
                const Vec2d side(-direction.y(), direction.x());
                const Vec2d face_side(-run.normal.y(), run.normal.x());
                const double face_weight = std::clamp(run.face_span /
                    (2 * object.config().fin_support_max_spacing.value), 0., 1.);
                const double position_error = (1 - face_weight) * std::abs((point - origin).dot(side)) +
                    face_weight * std::abs((point - run.face_center).dot(face_side));
                sites.push_back({point, direction, &run, alignment, position_error});
            }
        }
    }
    std::stable_sort(sites.begin(), sites.end(), [](const Site &a, const Site &b) {
        if (a.alignment != b.alignment)
            return a.alignment > b.alignment;
        return a.position_error < b.position_error;
    });
    for (const Site &site : sites) {
        if (object.print()->canceled())
            throw CanceledException();
        if (std::any_of(plans.begin(), plans.end(), [&](const FinPlan &plan) {
                const Vec2d side(-plan.seed.normal.y(), plan.seed.normal.x());
                const Vec2d delta = site.point - plan.seed.point;
                const double spacing = std::max(0.5 * (plan.seed.dimensions.thickness + width),
                                                std::min(object.config().fin_support_max_spacing.value,
                                                         0.5 * (site.run->b - site.run->a).norm()));
                return plan.seed.normal.dot(site.direction) >= fin_direction_merge_alignment &&
                       plan.seed.contact_normal.dot(site.run->normal) >= fin_direction_merge_alignment &&
                       std::abs(delta.dot(side)) < spacing &&
                       std::abs(delta.dot(plan.seed.normal)) < object.config().fin_support_depth.value + width;
            }) || std::any_of(rejected.begin(), rejected.end(), [&](const FinCandidate &failed) {
                return failed.top_layer == layer_id && (failed.point - site.point).norm() < width &&
                       failed.normal.dot(site.direction) > min_fin_contact_alignment;
            }))
            continue;
        FinSupportDimensions dimensions;
        Vec3d surface_normal;
        if (!fin_seed_contact(object, painted[layer_id], blockers, layer_id, site.point, site.direction,
                              *site.run, dimensions, surface_normal, diagnostics))
            continue;
        const Vec2d contact_normal = surface_normal.head<2>().normalized();
        const double lean = -surface_normal.z() / surface_normal.head<2>().norm() *
                            std::max(0., contact_normal.dot(site.direction));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        result = {site.point, site.direction, contact_normal, layer_id, dimensions,
                  object.config().fin_support_expansion_factor.value * risk * weight * weight};
        return true;
    }
    return diagnostics.reject(FinReject::direction, object.get_layer(int(layer_id))->print_z);
}

bool extend_fin_plan(const PrintObject &object, const std::vector<PaintedLayer> &painted,
                     const std::vector<Polygons> &blockers, const std::vector<PaintedSegment> &runs,
                     size_t layer_id, double risk, FinPlan &plan, FinDiagnostics &diagnostics)
{
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object).width();
    for (const PaintedSegment &run : runs) {
        Vec2d point;
        if (!fin_line_contact(plan.seed.point, plan.seed.normal, run, point) ||
            (point - plan.seed.point).norm() > std::max(2 * width, 4 * layer.height) ||
            point.dot(plan.seed.normal) < plan.farthest_along_sheet - 0.5 * width)
            continue;
        FinSupportDimensions dimensions;
        Vec3d surface_normal;
        if (!fin_seed_contact(object, painted[layer_id], blockers, layer_id, point, plan.seed.normal,
                              run, dimensions, surface_normal, diagnostics))
            continue;
        const Vec2d normal = surface_normal.head<2>().normalized();
        const double lean = -surface_normal.z() / surface_normal.head<2>().norm() *
                            std::max(0., normal.dot(plan.seed.normal));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        plan.farthest_along_sheet = std::max(plan.farthest_along_sheet, point.dot(plan.seed.normal));
        plan.seed.point = point;
        plan.seed.top_layer = layer_id;
        plan.seed.contact_normal = normal;
        plan.seed.expansion_slope = object.config().fin_support_expansion_factor.value * risk * weight * weight;
        plan.tops.push_back(plan.seed);
        return true;
    }
    return false;
}

// A planning score. Exact contact capacity is still measured from the completed
// polygons, but no polygon is rebuilt on every layer of the upward sweep.
double planned_fin_capacity(const PrintObject &object, const FinPlan &plan, size_t layer_id, double bed_z)
{
    const double top = object.get_layer(int(plan.seed.top_layer))->print_z - bed_z;
    const double current = object.get_layer(int(layer_id))->print_z - bed_z;
    const double neck = object.config().support_object_xy_distance.value + 0.5 * plan.seed.dimensions.thickness;
    return std::pow(top / std::max(current, EPSILON), 2) * plan.seed.dimensions.thickness / 1.2 /
           (1 + neck / object.config().fin_support_max_spacing.value);
}

// A taller fin may replace a nearby lower one only if it preserves the lower
// contact schedule and at least the lower fin's reach at the bed.
bool fin_supersedes(const PrintObject &object, const TracedFin &higher, const TracedFin &lower)
{
    if (higher.seed.top_layer <= lower.seed.top_layer ||
        higher.seed.wall_follow_island != lower.seed.wall_follow_island ||
        higher.seed.axis_limit.has_value() != lower.seed.axis_limit.has_value() ||
        (higher.seed.axis_limit &&
         (*higher.seed.axis_limit - *lower.seed.axis_limit).norm() > EPSILON) ||
        higher.seed.intent_island != lower.seed.intent_island ||
        higher.seed.intent_edge != lower.seed.intent_edge ||
        higher.seed.intent_parent_edge != lower.seed.intent_parent_edge ||
        higher.seed.intent_line != lower.seed.intent_line ||
        higher.seed.surface_normals != lower.seed.surface_normals ||
        higher.seed.requires_painted_contact != lower.seed.requires_painted_contact ||
        higher.seed.normal.dot(lower.seed.normal) < fin_direction_merge_alignment ||
        higher.seed.contact_normal.dot(lower.seed.contact_normal) < min_fin_contact_alignment ||
        higher.seed.dimensions.thickness + EPSILON < lower.seed.dimensions.thickness)
        return false;
    const Vec2d side(-lower.seed.normal.y(), lower.seed.normal.x());
    const double separation = std::abs((higher.seed.point - lower.seed.point).dot(side));
    if (separation > std::min(object.config().fin_support_max_spacing.value,
                              4 * lower.seed.dimensions.thickness) ||
        higher.tips.front().dot(lower.seed.normal) + support_material_flow(&object).width() <
            lower.tips.front().dot(lower.seed.normal))
        return false;
    auto contact_bands = [&object](const TracedFin &fin) {
        std::vector<double> centers;
        for (size_t i = 0; i < fin.contacts.size();) {
            if (!fin.contacts[i]) {
                ++i;
                continue;
            }
            const size_t first = i;
            while (i < fin.contacts.size() && fin.contacts[i])
                ++i;
            centers.push_back(0.5 * (object.get_layer(int(first))->print_z +
                                     object.get_layer(int(i - 1))->print_z));
        }
        return centers;
    };
    const double contact_window = object.config().fin_support_contact_spacing.value;
    const std::vector<double> lower_bands = contact_bands(lower);
    const std::vector<double> higher_bands = contact_bands(higher);
    size_t j = 0;
    for (double z : lower_bands) {
        while (j < higher_bands.size() && higher_bands[j] < z - contact_window)
            ++j;
        if (j == higher_bands.size() || higher_bands[j] > z)
            return false;
        ++j;
    }
    return true;
}

bool overlaps_fin(const TracedFin &candidate, const std::vector<TracedFin> &existing,
                   size_t &overlap_layer, const std::vector<size_t> &superseded = {})
{
    for (size_t fin_id = 0; fin_id < existing.size(); ++fin_id) {
        if (std::find(superseded.begin(), superseded.end(), fin_id) != superseded.end())
            continue;
        const TracedFin &fin = existing[fin_id];
        const size_t count = std::min(candidate.areas.size(), fin.areas.size());
        // Feet are deliberately united on the bed. Above it, overlapping
        // geometry would emit duplicate or crossing extrusion paths.
        for (size_t i = 1; i < count; ++i) {
            if (!candidate.area_bounds[i].overlap(fin.area_bounds[i]))
                continue;
            const ExPolygons overlap = intersection_ex(candidate.areas[i], fin.areas[i]);
            if (std::any_of(overlap.begin(), overlap.end(), [](const ExPolygon &area) {
                    return area.area() * SCALING_FACTOR * SCALING_FACTOR > EPSILON * EPSILON;
                })) {
                overlap_layer = i;
                return true;
            }
        }
    }
    return false;
}

struct FinPlacement {
    std::vector<size_t> superseded;
    std::optional<size_t> overlap_layer;
};

FinPlacement evaluate_fin_placement(const PrintObject &object, const TracedFin &candidate,
                                    const std::vector<TracedFin> &existing)
{
    FinPlacement result;
    for (size_t i = 0; i < existing.size(); ++i)
        if (fin_supersedes(object, candidate, existing[i]))
            result.superseded.push_back(i);
    size_t overlap_layer = 0;
    if (overlaps_fin(candidate, existing, overlap_layer, result.superseded))
        result.overlap_layer = overlap_layer;
    return result;
}

void cache_fin_bounds(TracedFin &fin)
{
    fin.area_bounds.clear();
    fin.area_bounds.reserve(fin.areas.size());
    for (const ExPolygons &area : fin.areas)
        fin.area_bounds.push_back(get_extents(area));
}

bool realize_fin_candidate(const PrintObject &object, const FinCandidate &candidate,
                           const Polygon &bed, const std::vector<PaintedLayer> &painted,
                           const std::vector<Polygons> &blockers, double line_width,
                           TracedFin &fin, bool include_seed_contact = true,
                           FinComponentCenterGeometry *component_geometry = nullptr,
                           const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    fin = TracedFin{};
    fin.seed = candidate;
    if (!trace_fin(object, bed, candidate, object.config().fin_support_depth.value,
                   object.config().support_object_xy_distance.value,
                   candidate.dimensions.thickness, line_width, fin,
                   geometry_cache) ||
        !generate_fin_geometry(object, fin, bed, painted, blockers,
                               include_seed_contact, component_geometry,
                               geometry_cache))
        return false;
    if (!fin.areas.empty())
        cache_fin_bounds(fin);
    return true;
}

struct SimpleFinSpan {
    size_t run;
    double begin;
    double end;
};

struct SimpleFinAnchor {
    double position;
    double score;
};

struct SimpleFinContour {
    std::vector<Linef> edges;
    std::vector<Vec2d> normals;
    std::vector<double> offsets;
    double length = 0.;
};

struct SimpleFinRunLocation {
    size_t contour;
    double begin;
    double end;
    double contact_span;
};

struct SimpleCandidateLayer {
    std::vector<SimpleFinContour> contours;
    std::vector<PaintedSegment> runs;
    std::vector<SimpleFinRunLocation> run_locations;
    std::vector<SimpleFinSpan> seed_ranges;
    std::vector<std::vector<SimpleFinAnchor>> anchors;
    size_t painted_cuts = 0;
    size_t matched_intervals = 0;
    size_t joined_intervals = 0;
    size_t short_contact_intervals = 0;
    double longest_painted_cut = 0.;
    double painted_extent = 0.;
    double best_contact_line = 0.;
    double best_required_contact = 0.;
    double best_contact_slack = -std::numeric_limits<double>::infinity();
};

struct SimpleFinFeature {
    Vec2d point;
    Vec2d normal;
    double score;
};

// Paint grants permission, but candidate coordinates must live on the actual
// sliced outer contour rather than on a facet cut spanning a convex envelope.
SimpleCandidateLayer build_simple_candidate_space(const PrintObject &object, const Layer &layer,
                                                  const std::vector<PaintedSegment> &painted_cuts,
                                                  double tolerance)
{
    SimpleCandidateLayer result;
    result.painted_cuts = painted_cuts.size();
    if (painted_cuts.empty())
        return result;
    std::vector<Linef> edges;
    std::vector<Vec2d> normals;
    std::vector<SimpleFinRunLocation> edge_locations;
    for (const ExPolygon &slice : layer.lslices) {
        const size_t contour_id = result.contours.size();
        SimpleFinContour &outline = result.contours.emplace_back();
        const Points &contour = slice.contour.points;
        const double orientation = slice.contour.area() >= 0 ? 1. : -1.;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            if (edge.squaredNorm() <= EPSILON * EPSILON)
                continue;
            edges.emplace_back(a, b);
            const Vec2d normal = orientation * Vec2d(edge.y(), -edge.x()).normalized();
            normals.push_back(normal);
            edge_locations.push_back({contour_id, outline.length, outline.length + edge.norm()});
            outline.edges.emplace_back(a, b);
            outline.normals.push_back(normal);
            outline.offsets.push_back(outline.length);
            outline.length += edge.norm();
        }
    }
    struct EdgeInterval {
        double begin;
        double end;
        Vec3d surface_normal;
        Vec3d macro_normal;
    };
    std::vector<std::vector<EdgeInterval>> allowed(edges.size());
    const auto edge_tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(edges);
    Vec2d painted_min = Vec2d::Constant(std::numeric_limits<double>::infinity());
    Vec2d painted_max = Vec2d::Constant(-std::numeric_limits<double>::infinity());
    auto clip = [](double value, double change, double low, double high, double &begin, double &end) {
        if (std::abs(change) <= EPSILON)
            return value >= low && value <= high;
        double first = (low - value) / change;
        double last = (high - value) / change;
        if (first > last)
            std::swap(first, last);
        begin = std::max(begin, first);
        end = std::min(end, last);
        return end > begin + 1e-9;
    };
    for (const PaintedSegment &run : painted_cuts) {
        painted_min = painted_min.cwiseMin(run.a).cwiseMin(run.b);
        painted_max = painted_max.cwiseMax(run.a).cwiseMax(run.b);
        const Vec2d direction = run.b - run.a;
        const double length_squared = direction.squaredNorm();
        result.longest_painted_cut = std::max(result.longest_painted_cut, std::sqrt(length_squared));
        if (length_squared <= EPSILON * EPSILON)
            continue;
        AABBTreeIndirect::Tree<2, double>::BoundingBox query;
        query.extend(run.a);
        query.extend(run.b);
        query.min() -= Vec2d::Constant(tolerance);
        query.max() += Vec2d::Constant(tolerance);
        AABBTreeIndirect::traverse(edge_tree, AABBTreeIndirect::intersecting(query), [&](const auto &node) {
                if (normals[node.idx].dot(run.normal) < min_fin_contact_alignment)
                    return true;
                const Vec2d &a = edges[node.idx].a;
                const Vec2d &b = edges[node.idx].b;
                const Vec2d edge = b - a;
                double edge_begin = 0., edge_end = 1.;
                if (clip((a - run.a).dot(run.normal), edge.dot(run.normal),
                         -tolerance, tolerance, edge_begin, edge_end) &&
                    clip((a - run.a).dot(direction) / length_squared,
                         edge.dot(direction) / length_squared, 0., 1., edge_begin, edge_end))
                    allowed[node.idx].push_back({edge_begin, edge_end, run.surface_normal, run.macro_normal});
                return true;
            });
    }
    result.painted_extent = (painted_max - painted_min).norm();
    for (size_t i = 0; i < edges.size(); ++i) {
        auto &intervals = allowed[i];
        std::sort(intervals.begin(), intervals.end(), [](const EdgeInterval &a, const EdgeInterval &b) {
            return a.begin < b.begin;
        });
        for (size_t j = 0; j < intervals.size();) {
            const double begin = intervals[j].begin;
            double end = intervals[j].end;
            const Vec3d surface_normal = intervals[j].surface_normal;
            const Vec3d macro_normal = intervals[j].macro_normal;
            while (++j < intervals.size() && intervals[j].begin <= end + 1e-5 &&
                   (macro_normal.squaredNorm() <= EPSILON * EPSILON ||
                    intervals[j].macro_normal.squaredNorm() <= EPSILON * EPSILON ||
                    macro_normal.dot(intervals[j].macro_normal) >= fin_crease_alignment))
                end = std::max(end, intervals[j].end);
            if ((end - begin) * edges[i].length() <= EPSILON)
                continue;
            ++result.matched_intervals;
            const Vec2d edge = edges[i].b - edges[i].a;
            const Vec2d midpoint = edges[i].a + 0.5 * (begin + end) * edge;
            const PrintRegion *region = fin_wall_region(layer, midpoint, normals[i], tolerance);
            const FinSupportDimensions dimensions = fin_support_dimensions(object,
                region != nullptr ? *region : object.printing_region(0));
            PaintedSegment run{edges[i].a + begin * edge, edges[i].a + end * edge,
                               normals[i], surface_normal};
            run.macro_normal = macro_normal;
            result.runs.push_back(std::move(run));
            result.run_locations.push_back({edge_locations[i].contour,
                edge_locations[i].begin + begin * edge.norm(),
                edge_locations[i].begin + end * edge.norm(), dimensions.contact_width + tolerance});
        }
    }
    // The contact may slide across consecutive painted edges of one island.
    // Split at a turn that moves the straight contact beyond half a line width.
    constexpr double joined_gap = 1e-3;
    for (size_t contour_id = 0; contour_id < result.contours.size(); ++contour_id) {
        std::vector<size_t> runs;
        for (size_t i = 0; i < result.run_locations.size(); ++i)
            if (result.run_locations[i].contour == contour_id)
                runs.push_back(i);
        if (runs.empty())
            continue;
        const double perimeter = result.contours[contour_id].length;
        for (size_t pos = 0; pos < runs.size(); ++pos) {
            const size_t run_id = runs[pos];
            const SimpleFinRunLocation &location = result.run_locations[run_id];
            const double max_turn = std::min(M_PI / 12.,
                std::asin(std::min(1., tolerance / location.contact_span)));
            const double joined_normal_dot = std::cos(max_turn);
            double line_begin = location.begin, line_end = location.end;
            size_t current = pos;
            double shift = 0.;
            for (size_t step = 1; step < runs.size(); ++step) {
                const size_t previous = (current + runs.size() - 1) % runs.size();
                if (previous > current)
                    shift -= perimeter;
                const SimpleFinRunLocation &other = result.run_locations[runs[previous]];
                if (std::abs(other.end + shift - line_begin) > joined_gap ||
                    result.runs[runs[previous]].normal.dot(result.runs[runs[current]].normal) < joined_normal_dot ||
                    result.runs[runs[previous]].normal.dot(result.runs[run_id].normal) < joined_normal_dot)
                    break;
                line_begin = other.begin + shift;
                current = previous;
            }
            current = pos;
            shift = 0.;
            for (size_t step = 1; step < runs.size(); ++step) {
                const size_t next = (current + 1) % runs.size();
                if (next < current)
                    shift += perimeter;
                const SimpleFinRunLocation &other = result.run_locations[runs[next]];
                if (std::abs(other.begin + shift - line_end) > joined_gap ||
                    result.runs[runs[next]].normal.dot(result.runs[runs[current]].normal) < joined_normal_dot ||
                    result.runs[runs[next]].normal.dot(result.runs[run_id].normal) < joined_normal_dot)
                    break;
                line_end = other.end + shift;
                current = next;
            }
            result.joined_intervals += line_begin < location.begin - 1e-9 ||
                                       line_end > location.end + 1e-9;
            const double line_length = line_end - line_begin;
            const double slack = line_length - location.contact_span;
            if (slack > result.best_contact_slack) {
                result.best_contact_slack = slack;
                result.best_contact_line = line_length;
                result.best_required_contact = location.contact_span;
            }
            const double half_contact = 0.5 * location.contact_span;
            const double begin = std::max(location.begin, line_begin + half_contact);
            const double end = std::min(location.end, line_end - half_contact);
            if (end <= begin + 1e-9) {
                ++result.short_contact_intervals;
                continue;
            }
            const double length = location.end - location.begin;
            result.seed_ranges.push_back({run_id, (begin - location.begin) / length,
                                          (end - location.begin) / length});
        }
    }
    return result;
}

// Paint only grants permission to attach. Rank sites using the complete model
// contour, so a paint boundary or a triangulated facet cannot become a corner.
std::vector<SimpleFinFeature> simple_fin_features(const Layer &layer, double width, double spacing)
{
    std::vector<SimpleFinFeature> features;
    for (const ExPolygon &slice : layer.lslices) {
        const Polygon &contour = slice.contour;
        const double perimeter = unscale<double>(contour.length());
        const double context = std::max(2 * width, std::min(0.5 * spacing, 0.05 * perimeter));
        Polygon oriented = contour;
        if (oriented.area() < 0)
            std::reverse(oriented.points.begin(), oriented.points.end());
        for (const Polygon &smoothed : oriented.simplify(scale_(std::max(width, 0.25 * context)))) {
            const Points &points = smoothed.points;
            if (points.size() < 3)
                continue;
            const double orientation = smoothed.area() >= 0 ? 1. : -1.;
            for (size_t i = 0; i < points.size(); ++i) {
                const Vec2d before = unscale(points[(i + points.size() - 1) % points.size()]);
                const Vec2d at = unscale(points[i]);
                const Vec2d after = unscale(points[(i + 1) % points.size()]);
                const Vec2d incoming = at - before, outgoing = after - at;
                const double first = incoming.norm(), second = outgoing.norm();
                if (first <= EPSILON || second <= EPSILON)
                    continue;
                const double cross = orientation * (incoming.x() * outgoing.y() - incoming.y() * outgoing.x());
                const double turn = std::atan2(cross, incoming.dot(outgoing));
                if (turn >= M_PI / 6.) {
                    const double size = std::min(1., std::min(first, second) / context);
                    const double score = 1.3 + size * turn / (M_PI / 2.);
                    features.push_back({at, orientation * Vec2d(incoming.y(), -incoming.x()) / first, score});
                    features.push_back({at, orientation * Vec2d(outgoing.y(), -outgoing.x()) / second, score});
                }
                if (second >= context)
                    features.push_back({0.5 * (at + after),
                                        orientation * Vec2d(outgoing.y(), -outgoing.x()) / second,
                                        1. + std::min(1., second / (4 * context))});
            }
        }
    }
    return features;
}

void prepare_simple_fin_anchors(SimpleCandidateLayer &layer, const Layer &model_layer,
                                double width, double spacing)
{
    if (!layer.anchors.empty())
        return;
    layer.anchors.resize(layer.runs.size());
    const double tolerance = std::max(width, 0.05 * spacing);
    std::vector<Linef> runs;
    runs.reserve(layer.runs.size());
    for (const PaintedSegment &run : layer.runs)
        runs.emplace_back(run.a, run.b);
    const auto run_tree = AABBTreeLines::build_aabb_tree_over_indexed_lines(runs);
    for (const SimpleFinFeature &feature : simple_fin_features(model_layer, width, spacing)) {
        AABBTreeIndirect::Tree<2, double>::BoundingBox query;
        query.extend(feature.point - Vec2d::Constant(tolerance));
        query.extend(feature.point + Vec2d::Constant(tolerance));
        AABBTreeIndirect::traverse(run_tree, AABBTreeIndirect::intersecting(query), [&](const auto &node) {
            const size_t i = node.idx;
            const PaintedSegment &run = layer.runs[i];
            if (feature.normal.dot(run.normal) < min_fin_contact_alignment)
                return true;
            const Vec2d edge = run.b - run.a;
            if (edge.squaredNorm() <= EPSILON * EPSILON)
                return true;
            const double position = (feature.point - run.a).dot(edge) / edge.squaredNorm();
            if (position < 0. || position > 1.)
                return true;
            const double distance = (feature.point - run.a - position * edge).norm();
            if (distance <= tolerance)
                layer.anchors[i].push_back({position, feature.score - 0.2 * distance / tolerance});
            return true;
        });
    }
}

struct SimpleFinSite {
    Vec2d point;
    size_t layer;
    size_t run;
};

SimpleFinSite select_simple_fin_site(const SimpleCandidateLayer &layer, size_t layer_id, double width, double spacing)
{
    SimpleFinSite best{Vec2d::Zero(), layer_id, 0};
    double best_score = -1;
    for (const SimpleFinSpan &span : layer.seed_ranges) {
        const PaintedSegment &run = layer.runs[span.run];
        const Vec2d edge = run.b - run.a;
        const double length = edge.norm();
        const double remaining = (span.end - span.begin) * length;
        // The fallback midpoint belongs to the remaining paint interval, but
        // an unpainted model corner never gains priority from that boundary.
        const double fallback = 0.25 + 0.25 * std::min(1., remaining / spacing);
        if (fallback > best_score) {
            best = {run.a + 0.5 * (span.begin + span.end) * edge, layer_id, span.run};
            best_score = fallback;
        }
        for (const SimpleFinAnchor &anchor : layer.anchors[span.run]) {
            if (anchor.position < span.begin || anchor.position > span.end || anchor.score <= best_score)
                continue;
            const double inset = std::min(width / length, 0.5 * (span.end - span.begin));
            const double position = std::clamp(anchor.position, span.begin + inset, span.end - inset);
            best = {run.a + position * edge, layer_id, span.run};
            best_score = anchor.score;
        }
    }
    return best;
}

// Subtract the unavailable range connected to the wall intersection. Nearby
// edges on another island or across a fold do not belong to this range.
std::optional<Vec2d> subtract_simple_fin_contour_range(SimpleCandidateLayer &layer, const Vec2d &center,
                                                        const Vec2d &facing, double radius, double max_anchor_distance,
                                                        std::optional<size_t> required_contour = std::nullopt)
{
    size_t contour_id = 0, edge_id = 0;
    double fraction = 0., nearest_squared = std::min(radius * radius,
                                                    max_anchor_distance * max_anchor_distance);
    bool found = false;
    for (size_t c = 0; c < layer.contours.size(); ++c) {
        if (required_contour && c != *required_contour)
            continue;
        const SimpleFinContour &contour = layer.contours[c];
        for (size_t e = 0; e < contour.edges.size(); ++e) {
            if (contour.normals[e].dot(facing) <= 0.)
                continue;
            const Vec2d edge = contour.edges[e].b - contour.edges[e].a;
            const double t = std::clamp((center - contour.edges[e].a).dot(edge) / edge.squaredNorm(), 0., 1.);
            const double distance_squared = (center - contour.edges[e].a - t * edge).squaredNorm();
            if (distance_squared < nearest_squared) {
                contour_id = c;
                edge_id = e;
                fraction = t;
                nearest_squared = distance_squared;
                found = true;
            }
        }
    }
    if (!found)
        return std::nullopt;

    const SimpleFinContour &contour = layer.contours[contour_id];
    const Vec2d edge = contour.edges[edge_id].b - contour.edges[edge_id].a;
    const Vec2d intersection = contour.edges[edge_id].a + fraction * edge;
    const double reach = std::sqrt(radius * radius - nearest_squared);
    const double position = contour.offsets[edge_id] + fraction * contour.edges[edge_id].length();
    auto boundary = [&](int direction) {
        size_t edge = edge_id;
        double offset = position;
        double remaining = reach;
        for (size_t visited = 0; visited < contour.edges.size(); ++visited) {
            if (contour.normals[edge].dot(facing) <= 0.)
                break;
            const double edge_begin = contour.offsets[edge];
            const double edge_end = edge_begin + contour.edges[edge].length();
            const double available = direction > 0 ? edge_end - offset : offset - edge_begin;
            const double step = std::min(remaining, available);
            offset += direction * step;
            remaining -= step;
            if (remaining <= 1e-9)
                break;
            edge = direction > 0 ? (edge + 1) % contour.edges.size() :
                                   (edge + contour.edges.size() - 1) % contour.edges.size();
            offset = direction > 0 ? contour.offsets[edge] :
                                     contour.offsets[edge] + contour.edges[edge].length();
        }
        return position + direction * (reach - remaining);
    };
    const double unavailable_begin = boundary(-1);
    const double unavailable_end = boundary(1);
    std::vector<SimpleFinSpan> remaining;
    remaining.reserve(layer.seed_ranges.size());
    for (const SimpleFinSpan &span : layer.seed_ranges) {
        const SimpleFinRunLocation &location = layer.run_locations[span.run];
        if (location.contour != contour_id) {
            remaining.push_back(span);
            continue;
        }
        const double length = location.end - location.begin;
        std::vector<std::pair<double, double>> pieces{{location.begin + span.begin * length,
                                                        location.begin + span.end * length}};
        for (int wrap = -1; wrap <= 1; ++wrap) {
            const double begin = unavailable_begin + wrap * contour.length;
            const double end = unavailable_end + wrap * contour.length;
            std::vector<std::pair<double, double>> next;
            for (const auto &piece : pieces) {
                if (begin > piece.first + 1e-9)
                    next.emplace_back(piece.first, std::min(begin, piece.second));
                if (end < piece.second - 1e-9)
                    next.emplace_back(std::max(end, piece.first), piece.second);
            }
            pieces = std::move(next);
            if (pieces.empty())
                break;
        }
        for (const auto &piece : pieces)
            remaining.push_back({span.run, (piece.first - location.begin) / length,
                                 (piece.second - location.begin) / length});
    }
    layer.seed_ranges = std::move(remaining);
    return intersection;
}

void subtract_simple_fin_projection(const PrintObject &object, std::vector<SimpleCandidateLayer> &layers,
                                    const FinCandidate &seed, size_t seed_run,
                                    double top_depth, double expansion_slope, double radius, double width)
{
    // The seed already lies on the sliced contour. Remove its local interval
    // before projecting to lower layers, even if no lower wall is found.
    subtract_simple_fin_contour_range(layers[seed.top_layer], seed.point, seed.normal, radius, 1e-4,
                                      layers[seed.top_layer].run_locations[seed_run].contour);

    // Keep the last wall position when the ray has no intersection, as the fin
    // geometry does. Snap each fallback to a nearby contour before continuing.
    Vec2d wall = seed.point;
    const double top_z = object.get_layer(int(seed.top_layer))->slice_z;
    for (size_t remaining = seed.top_layer; remaining > 1; --remaining) {
        const size_t i = remaining - 1;
        const Layer &model_layer = *object.get_layer(int(i));
        const double distance = top_depth + (top_z - model_layer.slice_z) * expansion_slope;
        const bool found_wall = projected_outer_contour(model_layer, seed.point, seed.normal, distance, wall);
        if (found_wall) {
            if (!layers[i].seed_ranges.empty())
                subtract_simple_fin_contour_range(layers[i], wall, seed.normal, radius, 1e-4);
        } else if (const auto fallback = subtract_simple_fin_contour_range(layers[i], wall, seed.normal, radius, width)) {
            wall = *fallback;
        }
    }
}

struct FinEmitStats {
    size_t merged_outline_failures = 0;
    size_t merged_outline_fallbacks = 0;
    size_t merged_outline_splits = 0;
};

FinEmitStats emit_fin_support(PrintObject &object, const std::vector<TracedFin> &fins,
                              bool merge_intersecting = false)
{
    FinEmitStats stats;
    size_t support_id = 0;
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        Polylines paths;
        Polygons feet;
        if (layer_id == 0) {
            generate_fin_foot_toolpaths(object, fins, paths, feet);
        } else if (merge_intersecting) {
            ExPolygons areas, centers;
            for (const TracedFin &fin : fins) {
                if (layer_id > fin.seed.top_layer)
                    continue;
                expolygons_append(areas, fin.areas[layer_id]);
                expolygons_append(centers, fin.centers[layer_id]);
            }
            areas = union_ex(areas);
            centers = union_ex(centers);
            const Flow flow = support_material_flow(&object, float(layer.height));
            const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
            for (const ExPolygon &area : areas) {
                Vec2d normal = Vec2d::UnitX();
                for (const TracedFin &fin : fins) {
                    if (layer_id <= fin.seed.top_layer && !intersection_ex(
                            ExPolygons{area}, fin.areas[layer_id]).empty()) {
                        normal = fin.seed.normal;
                        break;
                    }
                }
                const ExPolygons component_centers = intersection_ex(centers, ExPolygons{area});
                for (int wall = 0; wall < wall_count; ++wall) {
                    const double inset = 0.5 * flow.width() + wall * flow.spacing();
                    size_t generated = append_closed_outlines(
                        ExPolygons{area}, wall == 0 ? component_centers : ExPolygons{},
                        normal, inset, flow.width(), paths);
                    if (generated == 0 && wall == 0)
                        generated = append_closed_outlines(
                            ExPolygons{area}, ExPolygons{}, normal, inset, flow.width(), paths);
                    if (generated == 0) {
                        const size_t before = paths.size();
                        for (const TracedFin &fin : fins) {
                            if (layer_id > fin.seed.top_layer || intersection_ex(
                                    ExPolygons{area}, fin.areas[layer_id]).empty())
                                continue;
                            const ExPolygons member_areas = intersection_ex(fin.areas[layer_id], ExPolygons{area});
                            append_closed_outlines(member_areas,
                                wall == 0 ? intersection_ex(fin.centers[layer_id], ExPolygons{area}) : ExPolygons{},
                                fin.seed.normal, inset, flow.width(), paths);
                        }
                        generated = paths.size() - before;
                        stats.merged_outline_fallbacks += generated > 0;
                    }
                    if (generated == 0) {
                        ++stats.merged_outline_failures;
                        break;
                    }
                    stats.merged_outline_splits += generated > 1 ? generated - 1 : 0;
                }
            }
        } else {
            for (const TracedFin &fin : fins) {
                if (layer_id > fin.seed.top_layer)
                    continue;
                paths.insert(paths.end(), fin.outlines[layer_id].begin(), fin.outlines[layer_id].end());
            }
        }
        if (paths.empty() && feet.empty())
            continue;
        SupportLayer *support_layer = object.add_support_layer(int(support_id++), 0, layer.height, layer.print_z);
        support_layer->support_fills.no_sort = true;
        const Flow flow = support_material_flow(&object, float(layer.height));
        // Keep a closed oriented path: generic ExtrusionLoop emission relocates its seam to the last nozzle position.
        extrusion_entities_append_paths(support_layer->support_fills.entities, std::move(paths), erSupportMaterial,
                                        flow.mm3_per_mm(), flow.width(), flow.height(), false);
        if (!feet.empty()) {
            std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
            filler->set_bounding_box(get_extents(feet));
            filler->spacing = flow.spacing();
            filler->layer_id = 0;
            const SupportParameters support_params(object);
            fill_expolygons_with_sheath_generate_paths(support_layer->support_fills.entities, feet,
                filler.get(), 1.f, erSupportMaterial, flow, support_params, false, false);
        }
        support_layer->support_islands = union_ex(
            support_layer->support_fills.polygons_covered_by_width(float(SCALED_EPSILON)));
    }
    return stats;
}

std::vector<FinCandidate> select_simple_fin_candidates(const PrintObject &object,
                                                       const std::vector<PaintedLayer> &painted)
{
    const PrintObjectConfig &config = object.config();
    const double width = support_material_flow(&object).width();
    const double placement_distance = config.fin_support_max_spacing.value;

    std::vector<SimpleCandidateLayer> layers(painted.size());
    double painted_top_z = 0.;
    size_t painted_top_layer = 0;
    for (size_t i = 1; i < painted.size(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        if (!painted[i].sections[0].empty()) {
            painted_top_z = object.get_layer(int(i))->print_z;
            painted_top_layer = i;
        }
        // lslices are sampled at slice_z; compare paint at the same height.
        // A cut infinitesimally below print_z may collapse against a top edge.
        // Match every painted facet cut separately. The contour intervals below
        // are sorted and united, regardless of triangle order or tiny normal differences.
        layers[i] = build_simple_candidate_space(
            object, *object.get_layer(int(i)), painted[i].sections[0], width);
    }

    std::vector<FinCandidate> candidates;
    size_t highest = layers.size() - 1;
    while (highest > 0) {
        if (object.print()->canceled())
            throw CanceledException();
        while (highest > 0 && layers[highest].seed_ranges.empty())
            --highest;
        if (highest == 0)
            break;
        SimpleCandidateLayer &layer = layers[highest];
        prepare_simple_fin_anchors(layer, *object.get_layer(int(highest)), width, placement_distance);
        const SimpleFinSite site = select_simple_fin_site(layer, highest, width, placement_distance);
        const PaintedSegment &run = layer.runs[site.run];
        const Vec3d &surface_normal = run.surface_normal;
        const double horizontal = surface_normal.head<2>().norm();
        if (horizontal <= EPSILON) {
            layer.seed_ranges.erase(std::remove_if(layer.seed_ranges.begin(), layer.seed_ranges.end(),
                [&site](const SimpleFinSpan &span) { return span.run == site.run; }), layer.seed_ranges.end());
            continue;
        }
        const Vec2d contact_normal = surface_normal.head<2>() / horizontal;
        const Layer &model_layer = *object.get_layer(int(highest));
        const PrintRegion *region = fin_wall_region(model_layer, site.point, run.normal, width);
        const FinSupportDimensions dimensions = fin_support_dimensions(object,
            region != nullptr ? *region : object.printing_region(0));
        const double lean = -surface_normal.z() / horizontal *
                            std::max(0., contact_normal.dot(run.normal));
        const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
        const FinCandidate candidate{site.point, run.normal, contact_normal, highest, dimensions,
            config.fin_support_expansion_factor.value * weight * weight};
        candidates.push_back(candidate);
        subtract_simple_fin_projection(object, layers, candidate, site.run,
                                       config.fin_support_depth.value, candidate.expansion_slope,
                                       placement_distance, width);
    }

    const double candidate_top_z = candidates.empty() ? 0. :
        object.get_layer(int(candidates.front().top_layer))->print_z;
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=simple-candidates candidates=" << candidates.size()
        << " painted_top_z=" << painted_top_z << " candidate_top_z=" << candidate_top_z
        << " top_paint_sections=" << painted[painted_top_layer].sections[0].size()
        << " top_painted_cuts=" << layers[painted_top_layer].painted_cuts
        << " top_matched_intervals=" << layers[painted_top_layer].matched_intervals
        << " top_joined_intervals=" << layers[painted_top_layer].joined_intervals
        << " top_short_contact_intervals=" << layers[painted_top_layer].short_contact_intervals
        << " top_initial_seed_intervals=" << (layers[painted_top_layer].matched_intervals -
                                               layers[painted_top_layer].short_contact_intervals)
        << " top_slice_z=" << object.get_layer(int(painted_top_layer))->slice_z
        << " top_longest_painted_cut_mm=" << layers[painted_top_layer].longest_painted_cut
        << " top_painted_extent_mm=" << layers[painted_top_layer].painted_extent
        << " top_best_contact_line_mm=" << layers[painted_top_layer].best_contact_line
        << " top_required_contact_mm=" << layers[painted_top_layer].best_required_contact
        << " top_best_contact_slack_mm=" << (layers[painted_top_layer].matched_intervals > 0 ?
                                             layers[painted_top_layer].best_contact_slack : 0.);
    return candidates;
}

struct PointFinDirection {
    Vec2d direction;
    double demand = 0;
};

bool nearest_outer_contour_point(const Layer &layer, const Vec2d &origin, const Vec2d &facing,
                                 double max_distance, Vec2d &point)
{
    double best_distance_squared = max_distance * max_distance;
    bool found = false;
    for (const ExPolygon &slice : layer.lslices) {
        const Points &contour = slice.contour.points;
        const double orientation = slice.contour.area() >= 0 ? 1. : -1.;
        for (size_t i = 0; i < contour.size(); ++i) {
            const Vec2d a = unscale(contour[i]);
            const Vec2d b = unscale(contour[(i + 1) % contour.size()]);
            const Vec2d edge = b - a;
            const double length_squared = edge.squaredNorm();
            if (length_squared <= EPSILON * EPSILON)
                continue;
            const Vec2d normal = orientation * Vec2d(edge.y(), -edge.x()).normalized();
            if (normal.dot(facing) <= 0.)
                continue;
            const double position = std::clamp((origin - a).dot(edge) / length_squared, 0., 1.);
            const Vec2d candidate = a + position * edge;
            const double distance_squared = (candidate - origin).squaredNorm();
            if (distance_squared <= best_distance_squared) {
                best_distance_squared = distance_squared;
                point = candidate;
                found = true;
            }
        }
    }
    return found;
}

std::optional<Vec2d> fin_plane_contour_intersection(const Vec2d &origin, const Vec2d &direction,
                                                    const Vec2d &a, const Vec2d &b)
{
    auto cross = [](const Vec2d &lhs, const Vec2d &rhs) {
        return lhs.x() * rhs.y() - lhs.y() * rhs.x();
    };
    const Vec2d edge = b - a;
    const double denominator = cross(edge, direction);
    if (std::abs(denominator) <= 1e-10) {
        if (std::abs(cross(a - origin, direction)) > 1e-7)
            return std::nullopt;
        const double length_squared = edge.squaredNorm();
        const double position = length_squared > EPSILON * EPSILON ?
            std::clamp((origin - a).dot(edge) / length_squared, 0., 1.) : 0.;
        return a + position * edge;
    }
    const double position = cross(origin - a, direction) / denominator;
    if (position < -1e-9 || position > 1 + 1e-9)
        return std::nullopt;
    return a + std::clamp(position, 0., 1.) * edge;
}

// Resolve a vertical sheet direction without moving the painted intent target.
// Whole-model demand may turn the sheet by at most 30 degrees from its wall.
PointFinDirection painted_shape_fin_direction(const Vec2d &wall_normal, const FinShapeLayer *shape)
{
    PointFinDirection best{wall_normal, 0};
    if (shape == nullptr)
        return best;
    for (const FinDemand &need : shape->demand) {
        const double turn = std::atan2(need.direction.x() * wall_normal.y() -
                                       need.direction.y() * wall_normal.x(),
                                       need.direction.dot(wall_normal));
        if (std::abs(turn) > M_PI / 3. + 1e-9)
            continue;
        const double correction = std::copysign(std::max(0., std::abs(turn) - M_PI / 6.), turn);
        const Vec2d direction(std::cos(correction) * need.direction.x() -
                              std::sin(correction) * need.direction.y(),
                              std::sin(correction) * need.direction.x() +
                              std::cos(correction) * need.direction.y());
        const double score = need.initial * std::max(0., direction.dot(need.direction));
        if (score > best.demand)
            best = {direction, score};
    }
    return best;
}

std::vector<FinCandidateProposal> select_painted_shape_fin_candidates(const PrintObject &object,
                                                                      const PaintedSides &painted_sides,
                                                                      const FinShapeAnalysis &shape)
{
    struct PointSite {
        bool found = false;
        Vec2d point = Vec2d::Zero();
        Vec2d direction = Vec2d::Zero();
        PaintedSegment run;
        size_t layer = 0;
        double distance_squared = std::numeric_limits<double>::infinity();
        double planar_distance_squared = std::numeric_limits<double>::infinity();
        double height_error = std::numeric_limits<double>::infinity();
        double demand = 0;
        bool macro_side = false;
        bool follows_intent = false;
        bool projects_down = false;
    };

    const PrintObjectConfig &config = object.config();
    const double width = support_material_flow(&object).width();
    const double minimum_painted_line_alignment =
        std::cos(config.fin_support_painted_shape_line_angle.value * M_PI / 180.);
    const double local_selection_radius = std::max(
        3 * width, config.fin_support_contact_width.get_abs_value(width) + width);
    auto better_site = [local_selection_radius, width](const PointSite &candidate, const PointSite &current) {
        if (!current.found)
            return true;
        if (candidate.macro_side != current.macro_side)
            return candidate.macro_side;
        const size_t candidate_height_band = size_t(candidate.height_error / width);
        const size_t current_height_band = size_t(current.height_error / width);
        if (candidate_height_band != current_height_band)
            return candidate_height_band < current_height_band;
        if (candidate.projects_down != current.projects_down)
            return candidate.projects_down;
        const size_t candidate_distance_band = size_t(
            std::sqrt(candidate.planar_distance_squared) / local_selection_radius);
        const size_t current_distance_band = size_t(
            std::sqrt(current.planar_distance_squared) / local_selection_radius);
        if (candidate_distance_band != current_distance_band)
            return candidate_distance_band < current_distance_band;
        if (candidate.follows_intent != current.follows_intent)
            return candidate.follows_intent;
        if (std::abs(candidate.demand - current.demand) > 1e-9)
            return candidate.demand > current.demand;
        return candidate.distance_squared < current.distance_squared;
    };
    auto better_contact_top_site = [local_selection_radius](const PointSite &candidate,
                                                            const PointSite &current) {
        if (!current.found)
            return true;
        if (std::abs(candidate.height_error - current.height_error) > 1e-9)
            return candidate.height_error < current.height_error;
        const size_t candidate_distance_band = size_t(
            std::sqrt(candidate.planar_distance_squared) / local_selection_radius);
        const size_t current_distance_band = size_t(
            std::sqrt(current.planar_distance_squared) / local_selection_radius);
        if (candidate_distance_band != current_distance_band)
            return candidate_distance_band < current_distance_band;
        if (candidate.projects_down != current.projects_down)
            return candidate.projects_down;
        if (candidate.follows_intent != current.follows_intent)
            return candidate.follows_intent;
        return candidate.distance_squared < current.distance_squared;
    };

    auto resolve_direction = [](const Vec2d &macro_direction, const FinShapeLayer *shape,
                                const Vec2d &intent_direction, bool direction_is_fixed) {
        if (direction_is_fixed)
            return std::make_pair(PointFinDirection{intent_direction, 0.}, true);
        if (macro_direction.squaredNorm() <= EPSILON * EPSILON)
            return std::make_pair(PointFinDirection{Vec2d::Zero(), 0.}, false);
        return std::make_pair(painted_shape_fin_direction(macro_direction, shape), false);
    };

    std::vector<FinCandidateProposal> proposals;
    size_t too_small = 0;
    size_t without_contour = 0;
    size_t contact_top_clamped = 0;
    size_t geometry_fallback = 0;
    size_t shifted_to_contact_top = 0;
    size_t direction_adjusted = 0;
    size_t intent_direction_used = 0;
    size_t point_intents = 0;
    size_t line_intents = 0;
    size_t branched_intents = 0;
    size_t intent_strokes = 0;
    size_t line_plane_targets = 0;
    size_t macro_plane_targets = 0;
    size_t line_to_point_fallbacks = 0;
    size_t line_plane_alignment_rejects = 0;
    size_t line_plane_missing_macro = 0;
    size_t axis_limited_candidates = 0;
    size_t plane_intersection_misses = 0;
    size_t contact_span_site_rejects = 0;
    size_t resolved_line_candidates = 0;
    size_t resolved_point_candidates = 0;
    double smallest_area_slack = std::numeric_limits<double>::infinity();
    double max_center_distance = 0.;
    proposals.reserve(painted_sides.islands.size());
    for (size_t island_id = 0; island_id < painted_sides.islands.size(); ++island_id) {
        if (object.print()->canceled())
            throw CanceledException();
        const PaintedIsland &island = painted_sides.islands[island_id];
        point_intents += island.intent.kind == PaintedIntentKind::point;
        line_intents += island.intent.kind == PaintedIntentKind::line;
        branched_intents += island.intent.kind == PaintedIntentKind::branched;
        intent_strokes += island.intent.strokes.size();
        std::vector<std::vector<PaintedSegment>> contour_runs(painted_sides.layers.size());
        for (size_t layer_id = 1; layer_id < painted_sides.layers.size(); ++layer_id) {
            const Layer &model_layer = *object.get_layer(int(layer_id));
            std::vector<PaintedSegment> island_runs;
            for (const PaintedSegment &run : painted_sides.layers[layer_id].sections[0]) {
                if (run.island == island_id)
                    island_runs.push_back(run);
            }
            SimpleCandidateLayer layer = build_simple_candidate_space(object, model_layer, island_runs, width);
            contour_runs[layer_id] = std::move(layer.runs);
        }

        struct IntentTarget {
            Vec3d point;
            Vec2d macro_direction = Vec2d::Zero();
            Vec2d direction = Vec2d::Zero();
            double area = 0;
            bool point_is_contact_top = false;
            bool direction_is_fixed = false;
            bool intersect_plane = false;
            double minimum_candidate_alignment = min_fin_contact_alignment;
            std::optional<Vec2d> axis_limit;
            std::optional<size_t> edge;
            std::optional<size_t> parent_edge;
            std::vector<FinCandidate::IntentNode> nodes;
            bool line = false;
            std::optional<Vec2d> parent_point;
            Vec3d macro_normal = Vec3d::Zero();
            std::optional<Vec2d> plane_origin;
            std::optional<FinCandidate::IntentRoot> root;
            std::optional<std::pair<size_t, size_t>> source_parent_junction;
        };
        struct IntentSource {
            IntentTarget point;
            std::optional<IntentTarget> line;
            Vec2d axis_end = Vec2d::Zero();
            bool corrected = false;
        };
        std::vector<IntentSource> sources;
        if (island.intent.kind == PaintedIntentKind::point || island.intent.strokes.empty()) {
            Vec2d macro_direction = island.intent.macro_normal.head<2>();
            if (macro_direction.norm() >= min_fin_macro_horizontal)
                macro_direction.normalize();
            else
                macro_direction = Vec2d::Zero();
            IntentTarget point{island.intent.center, macro_direction, Vec2d::Zero(), island.area,
                               false, false, false, min_fin_contact_alignment};
            point.macro_normal = island.intent.macro_normal;
            sources.push_back({std::move(point), std::nullopt});
        } else {
            sources.reserve(island.intent.strokes.size());
            auto point_target = [&](const PaintedIntentStroke &stroke, size_t edge_id) {
                const double represented_area = stroke.length * std::max(stroke.width, width);
                Vec2d macro_direction = stroke.macro_normal.head<2>();
                if (macro_direction.norm() >= min_fin_macro_horizontal)
                    macro_direction.normalize();
                else {
                    macro_direction = island.intent.macro_normal.head<2>();
                    if (macro_direction.norm() >= min_fin_macro_horizontal)
                        macro_direction.normalize();
                    else
                        macro_direction = Vec2d::Zero();
                }
                IntentTarget target{stroke.center, macro_direction, Vec2d::Zero(),
                    std::min(island.area, represented_area), false, false, false,
                    min_fin_contact_alignment};
                target.edge = edge_id;
                target.parent_edge = stroke.parent_edge;
                if (stroke.parent_point.has_value())
                    target.parent_point = Vec2d(*stroke.parent_point);
                target.macro_normal = stroke.macro_normal;
                for (const size_t node_id : stroke.nodes)
                    if (node_id < island.intent.nodes.size())
                        target.nodes.push_back({node_id, Vec2d(island.intent.nodes[node_id].point),
                                                island.intent.nodes[node_id].main_edge});
                return target;
            };
            auto line_target = [&](const PaintedIntentStroke &stroke, size_t edge_id) -> std::optional<IntentTarget> {
                if (stroke.points.empty())
                    return std::nullopt;
                Vec2d macro_direction = stroke.macro_normal.head<2>();
                if (macro_direction.norm() >= min_fin_macro_horizontal)
                    macro_direction.normalize();
                else
                    macro_direction = Vec2d::Zero();
                // This comparison is entirely in the XY ground projection: A
                // is the endpoint chord below and macro_direction is normalized
                // Npatch.xy. Keep A unnormalized while measuring whether its
                // endpoint is distinguishable from the equal-radius endpoint on
                // Npatch.xy. A deviation within one complete painted width is
                // drawing noise, so that edge uses the macro plane.
                Vec2d axis_direction =
                    (stroke.xy[1] - stroke.xy[0]);
                const double stroke_area = stroke.length * std::max(stroke.width, width);
                const double target_area = std::min(island.area, stroke_area);
                IntentTarget target{stroke.anchor, macro_direction, Vec2d::Zero(), target_area,
                                    stroke.anchor_is_contact_top, true, !stroke.spans_macro_crease,
                                    stroke.spans_macro_crease ? EPSILON : minimum_painted_line_alignment};
                if (stroke.anchor_is_contact_top)
                    target.point.z() = stroke.start_z;
                target.edge = edge_id;
                target.parent_edge = stroke.parent_edge;
                if (stroke.parent_point.has_value())
                    target.parent_point = Vec2d(*stroke.parent_point);
                target.line = true;
                target.macro_normal = stroke.macro_normal;
                target.plane_origin = Vec2d(stroke.xy[0]);
                for (size_t side = 0; side < stroke.nodes.size(); ++side) {
                    const size_t node_id = stroke.nodes[side];
                    if (node_id >= island.intent.nodes.size())
                        continue;
                    target.nodes.push_back({node_id, Vec2d(island.intent.nodes[node_id].point),
                                                island.intent.nodes[node_id].main_edge});
                }
                if (macro_direction.squaredNorm() <= EPSILON * EPSILON) {
                    ++line_plane_missing_macro;
                    return std::nullopt;
                }
                auto set_direction = [&](const Vec2d &direction) {
                    target.direction = direction;
                    if (stroke.starts_at_root && !stroke.nodes.empty()) {
                        const Vec2d chord = stroke.xy[1] - stroke.xy[0];
                        const double sign = chord.norm() <= EPSILON || chord.dot(direction) < 0. ? -1. : 1.;
                        target.root = FinCandidate::IntentRoot{stroke.nodes.front(), stroke.xy[0], sign * direction};
                    }
                };
                if (axis_direction.dot(macro_direction) < 0.)
                    axis_direction = -axis_direction;
                const double projected_length = axis_direction.norm();
                const double endpoint_deviation = projected_length <= EPSILON ? 0. :
                    (axis_direction - projected_length * macro_direction).norm();
                if (endpoint_deviation <= stroke.width + EPSILON) {
                    set_direction(macro_direction);
                    ++macro_plane_targets;
                    return target;
                }
                axis_direction /= projected_length;
                set_direction(axis_direction);
                ++line_plane_targets;
                return target;
            };
            // Skeleton edges carry both alternatives. No point is emitted until
            // this resolver has evaluated the line plane and its physical host.
            for (size_t edge_id = 0; edge_id < island.intent.strokes.size(); ++edge_id) {
                const PaintedIntentStroke &stroke = island.intent.strokes[edge_id];
                IntentSource source{point_target(stroke, edge_id), line_target(stroke, edge_id)};
                if (stroke.parent_edge && *stroke.parent_edge < island.intent.strokes.size()) {
                    const PaintedIntentStroke &parent = island.intent.strokes[*stroke.parent_edge];
                    for (size_t node : stroke.nodes) {
                        if (node >= island.intent.nodes.size() ||
                            std::find(parent.nodes.begin(), parent.nodes.end(), node) == parent.nodes.end())
                            continue;
                        source.point.source_parent_junction = std::make_pair(*stroke.parent_edge, node);
                        if (source.line)
                            source.line->source_parent_junction = source.point.source_parent_junction;
                        break;
                    }
                }
                if (source.line.has_value()) {
                    const Vec2d chord = (stroke.xy[1] - stroke.xy[0]);
                    const double orientation = chord.dot(source.line->direction) < 0. ? -1. : 1.;
                    source.axis_end = *source.line->plane_origin +
                        orientation * chord.norm() * source.line->direction;
                    source.line->axis_limit = source.axis_end;
                    source.corrected = (source.axis_end - stroke.xy[1]).norm() > EPSILON;
                }
                sources.push_back(std::move(source));
            }

            // Freeze all corrected axes before moving any junction. A neighbor's
            // new endpoint must not change another edge's correction decision.
            // The finite target is the rotated intent segment, not a traced
            // body cap or tip. Heights remain the member's own contact range.
            size_t next_node = island.intent.nodes.size();
            for (size_t child_id = 0; child_id < sources.size(); ++child_id) {
                IntentSource &child = sources[child_id];
                if (!child.line)
                    continue;
                const auto original_nodes = child.line->nodes;
                for (const FinCandidate::IntentNode &old_node : original_nodes) {
                    if (!old_node.main_edge || *old_node.main_edge == child_id ||
                        *old_node.main_edge >= sources.size())
                        continue;
                    const size_t parent_id = *old_node.main_edge;
                    IntentSource &parent = sources[parent_id];
                    if (!parent.line || (!child.corrected && !parent.corrected))
                        continue;
                    IntentTarget &child_line = *child.line;
                    IntentTarget &parent_line = *parent.line;
                    const Vec2d origin = *child_line.plane_origin;
                    const PaintedIntentStroke &stroke = island.intent.strokes[child_id];
                    const bool tail = stroke.nodes.size() > 1 && stroke.nodes[1] == old_node.node;
                    const Vec2d ray_origin = tail ? origin : child.axis_end;
                    Vec2d ray = tail ? Vec2d(child.axis_end - origin) : Vec2d(origin - child.axis_end);
                    if (ray.norm() <= EPSILON)
                        ray = child_line.direction;
                    else
                        ray.normalize();
                    std::optional<Vec2d> joint;
                    // Rotation about a common root keeps that exact point, even
                    // when the two corrected axes coincide or have zero XY span.
                    if (child_line.root && (origin - old_node.point).norm() <= EPSILON &&
                        (*parent_line.plane_origin - old_node.point).norm() <= EPSILON)
                        joint = old_node.point;
                    else
                        joint = fin_plane_contour_intersection(
                            origin, child_line.direction, *parent_line.plane_origin, parent.axis_end);
                    auto remove_node = [&](IntentTarget &target) {
                        target.nodes.erase(std::remove_if(target.nodes.begin(), target.nodes.end(),
                            [&](const FinCandidate::IntentNode &node) { return node.node == old_node.node; }),
                            target.nodes.end());
                    };
                    remove_node(child_line);
                    remove_node(child.point);
                    const bool first_attachment = child_line.parent_edge == old_node.main_edge &&
                        child_line.parent_point && (*child_line.parent_point - old_node.point).norm() <= EPSILON;
                    if (!joint || (*joint - ray_origin).dot(ray) < -EPSILON) {
                        if (first_attachment) {
                            child_line.parent_edge.reset();
                            child_line.parent_point.reset();
                            child.point.parent_edge.reset();
                            child.point.parent_point.reset();
                        }
                        continue;
                    }
                    if (first_attachment) {
                        child_line.parent_point = *joint;
                        child.point.parent_point = *joint;
                    }
                    const auto existing = std::find_if(parent_line.nodes.begin(), parent_line.nodes.end(),
                        [&](const FinCandidate::IntentNode &node) {
                            return node.main_edge == parent_id && (node.point - *joint).norm() <= EPSILON;
                        });
                    const FinCandidate::IntentNode node = existing != parent_line.nodes.end() ? *existing :
                        FinCandidate::IntentNode{next_node++, *joint, parent_id};
                    if (existing == parent_line.nodes.end()) {
                        parent_line.nodes.push_back(node);
                        parent.point.nodes.push_back(node);
                    }
                    child_line.nodes.push_back(node);
                    child.point.nodes.push_back(node);
                    if (tail)
                        child_line.axis_limit = *joint;
                }
            }
        }

        auto resolve_target = [&](const IntentTarget &target) -> std::optional<FinCandidate> {
            const Vec2d center = target.point.head<2>();
            const Vec2d plane_origin = target.plane_origin.value_or(center);
            PointSite center_site;
            for (size_t layer_id = 1; layer_id < contour_runs.size(); ++layer_id) {
                const Layer &model_layer = *object.get_layer(int(layer_id));
                const double dz = model_layer.slice_z - target.point.z();
                // Compare macro faces near this skeleton feature before
                // committing the intent to one actual sliced contour point.
                for (const PaintedSegment &run : contour_runs[layer_id]) {
                    const Vec2d edge = run.b - run.a;
                    if (edge.squaredNorm() <= EPSILON * EPSILON)
                        continue;
                    Vec2d point = Vec2d::Zero();
                    double position = 0;
                    if (target.intersect_plane) {
                        const std::optional<Vec2d> intersection =
                            fin_plane_contour_intersection(plane_origin, target.direction, run.a, run.b);
                        if (!intersection.has_value()) {
                            ++plane_intersection_misses;
                            continue;
                        }
                        point = *intersection;
                        position = std::clamp((point - run.a).dot(edge) / edge.squaredNorm(), 0., 1.);
                    } else {
                        position = std::clamp((center - run.a).dot(edge) / edge.squaredNorm(), 0., 1.);
                        point = run.a + position * edge;
                    }
                    const PrintRegion *region = fin_wall_region(model_layer, point, run.normal, width);
                    const FinSupportDimensions dimensions = fin_support_dimensions(
                        object, region != nullptr ? *region : object.printing_region(0));
                    const double contact_span = dimensions.contact_width + width;
                    if (!target.intersect_plane && edge.norm() >= contact_span) {
                        const double margin = 0.5 * contact_span / edge.norm();
                        position = std::clamp(position, margin, 1 - margin);
                        point = run.a + position * edge;
                    }
                    const double layer_width = support_material_flow(&object, float(model_layer.height)).width();
                    std::vector<Vec2d> contact_walls;
                    if (!contact_contour_samples(model_layer, point, run.normal, dimensions.contact_width,
                                                 layer_width, contact_walls)) {
                        ++contact_span_site_rejects;
                        continue;
                    }
                    PointSite site;
                    site.found = true;
                    site.point = point;
                    site.run = run;
                    site.layer = layer_id;
                    site.distance_squared = (point - center).squaredNorm() + dz * dz;
                    site.planar_distance_squared = (point - center).squaredNorm();
                    site.height_error = std::abs(dz);
                    const double macro_horizontal = run.macro_normal.head<2>().norm();
                    const Vec2d macro_xy = macro_horizontal > EPSILON ?
                        run.macro_normal.head<2>() / macro_horizontal : run.normal;
                    const FinShapeLayer *shape_layer = layer_id < shape.layers.size() ? &shape.layers[layer_id] : nullptr;
                    const auto [resolved, follows_intent] = resolve_direction(
                        target.macro_direction, shape_layer, target.direction, target.direction_is_fixed);
                    site.direction = resolved.direction;
                    site.demand = resolved.demand;
                    site.follows_intent = follows_intent;
                    site.macro_side = macro_horizontal >= min_fin_macro_horizontal &&
                        macro_xy.dot(run.normal) >= min_fin_contact_alignment &&
                        resolved.direction.squaredNorm() > EPSILON * EPSILON &&
                        fin_contact_aligned(resolved.direction, macro_xy,
                                            target.minimum_candidate_alignment);
                    // Painted projection measures downward from horizontal
                    // in each X-A1 vertical section.
                    site.projects_down = site.macro_side && layer_id > 0;
                    // A graph endpoint is a hard height anchor shared by all
                    // incident fits. Ignore sites which cannot host that fixed
                    // plane, then choose the closest height before any load or
                    // projection preference so adjacent segments keep one node.
                    if (target.point_is_contact_top && !site.macro_side)
                        continue;
                    if ((target.point_is_contact_top ? better_contact_top_site(site, center_site) :
                                                       better_site(site, center_site)))
                        center_site = std::move(site);
                }
            }
            if (!center_site.found) {
                ++without_contour;
                return std::nullopt;
            }

            // FinCandidate is anchored at the top middle of its contact. Move
            // a region center upward by half a contact height. A vertical
            // stroke's upper endpoint already represents that contact top.
            const double target_z = target.point.z() + (target.point_is_contact_top ? 0. :
                0.5 * config.fin_support_contact_height.value);
            size_t target_layer = center_site.layer;
            double target_error = std::abs(object.get_layer(int(target_layer))->slice_z - target_z);
            for (size_t layer_id = center_site.layer + 1; layer_id < painted_sides.layers.size(); ++layer_id) {
                const double error = std::abs(object.get_layer(int(layer_id))->slice_z - target_z);
                if (error > target_error)
                    break;
                target_error = error;
                target_layer = layer_id;
            }

            PointSite best = center_site;
            for (size_t layer_id = center_site.layer + 1; layer_id <= target_layer; ++layer_id) {
                const Layer &previous_layer = *object.get_layer(int(layer_id - 1));
                const Layer &model_layer = *object.get_layer(int(layer_id));
                const double dz = model_layer.slice_z - previous_layer.slice_z;
                const double horizontal = best.run.macro_normal.head<2>().norm();
                Vec2d predicted = best.point;
                if (horizontal > EPSILON)
                    predicted -= best.run.macro_normal.z() / horizontal * dz * best.run.normal;
                const double continuity = std::sqrt(3.) * std::abs(dz) + 2 * width;
                const PaintedSegment *next_run = nullptr;
                Vec2d next_point = Vec2d::Zero();
                double next_distance_squared = continuity * continuity;
                for (const PaintedSegment &run : contour_runs[layer_id]) {
                    if (run.normal.dot(best.run.normal) <= 0. ||
                        run.macro_normal.dot(best.run.macro_normal) < fin_crease_alignment)
                        continue;
                    const Vec2d edge = run.b - run.a;
                    if (edge.squaredNorm() <= EPSILON * EPSILON)
                        continue;
                    Vec2d point = Vec2d::Zero();
                    if (target.intersect_plane) {
                        const std::optional<Vec2d> intersection =
                            fin_plane_contour_intersection(plane_origin, target.direction, run.a, run.b);
                        if (!intersection.has_value())
                            continue;
                        point = *intersection;
                    } else {
                        const double position =
                            std::clamp((predicted - run.a).dot(edge) / edge.squaredNorm(), 0., 1.);
                        point = run.a + position * edge;
                    }
                    const PrintRegion *region = fin_wall_region(model_layer, point, run.normal, width);
                    const FinSupportDimensions dimensions = fin_support_dimensions(
                        object, region != nullptr ? *region : object.printing_region(0));
                    const double layer_width = support_material_flow(&object, float(model_layer.height)).width();
                    std::vector<Vec2d> contact_walls;
                    if (!contact_contour_samples(model_layer, point, run.normal, dimensions.contact_width,
                                                 layer_width, contact_walls)) {
                        ++contact_span_site_rejects;
                        continue;
                    }
                    const double distance_squared = (point - predicted).squaredNorm();
                    if (distance_squared <= next_distance_squared) {
                        next_distance_squared = distance_squared;
                        next_point = point;
                        next_run = &run;
                    }
                }
                if (next_run != nullptr) {
                    best.point = next_point;
                    best.run = *next_run;
                    best.layer = layer_id;
                    continue;
                }

                Vec2d unused;
                if (nearest_outer_contour_point(model_layer, predicted, best.run.normal, continuity, unused)) {
                    ++contact_top_clamped;
                    break;
                }
                ++geometry_fallback;
                break;
            }
            shifted_to_contact_top += best.layer > center_site.layer;
            best.distance_squared = (best.point - center).squaredNorm() +
                std::pow(object.get_layer(int(best.layer))->slice_z - target_z, 2);
            const Vec3d &surface_normal = best.run.macro_normal;
            const double horizontal = surface_normal.head<2>().norm();
            if (horizontal < min_fin_macro_horizontal) {
                ++without_contour;
                return std::nullopt;
            }
            const Vec2d macro_contact_normal = surface_normal.head<2>() / horizontal;
            if (macro_contact_normal.dot(best.run.normal) < min_fin_contact_alignment) {
                ++without_contour;
                return std::nullopt;
            }
            const FinShapeLayer *shape_layer = best.layer < shape.layers.size() ? &shape.layers[best.layer] : nullptr;
            const auto [resolved, follows_intent] = resolve_direction(
                target.macro_direction, shape_layer, target.direction, target.direction_is_fixed);
            if (resolved.direction.squaredNorm() <= EPSILON * EPSILON ||
                !fin_contact_aligned(resolved.direction, macro_contact_normal,
                                     target.minimum_candidate_alignment)) {
                ++without_contour;
                return std::nullopt;
            }
            best.direction = resolved.direction;
            best.demand = resolved.demand;
            best.follows_intent = follows_intent;

            const Layer &model_layer = *object.get_layer(int(best.layer));
            const PrintRegion *region = fin_wall_region(model_layer, best.point, best.run.normal, width);
            const FinSupportDimensions dimensions = fin_support_dimensions(
                object, region != nullptr ? *region : object.printing_region(0));
            const double minimum_area = (dimensions.contact_width + width) * config.fin_support_contact_height.value;
            smallest_area_slack = std::min(smallest_area_slack, target.area - minimum_area);
            if (target.area + EPSILON < minimum_area) {
                ++too_small;
                return std::nullopt;
            }
            Vec3d candidate_macro_normal = target.macro_normal;
            if (candidate_macro_normal.squaredNorm() <= EPSILON * EPSILON)
                candidate_macro_normal = surface_normal;
            else {
                candidate_macro_normal.normalize();
                if (candidate_macro_normal.head<2>().dot(best.run.normal) < 0)
                    candidate_macro_normal = -candidate_macro_normal;
            }
            const double candidate_macro_horizontal = candidate_macro_normal.head<2>().norm();
            if (candidate_macro_horizontal < min_fin_macro_horizontal) {
                ++without_contour;
                return std::nullopt;
            }
            const Vec2d candidate_macro_direction =
                candidate_macro_normal.head<2>() / candidate_macro_horizontal;
            const double lean = -candidate_macro_normal.z() / candidate_macro_horizontal *
                                std::max(0., candidate_macro_direction.dot(best.direction));
            const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
            FinCandidate candidate{best.point, best.direction, best.run.normal, best.layer, dimensions,
                config.fin_support_expansion_factor.value * weight * weight};
            candidate.surface_normals = FinSurfaceNormalPolicy::painted_region;
            candidate.requires_painted_contact = false;
            candidate.macro_normal = candidate_macro_normal;
            candidate.wall_follow_island = island_id;
            candidate.intent_island = island_id;
            candidate.intent_edge = target.edge;
            candidate.intent_parent_edge = target.parent_edge;
            candidate.intent_nodes = target.nodes;
            candidate.intent_root = target.root;
            candidate.intent_line = target.line;
            candidate.intent_parent_point = target.parent_point;
            candidate.source_parent_junction = target.source_parent_junction;
            candidate.axis_limit = target.axis_limit;
            axis_limited_candidates += candidate.axis_limit.has_value();
            intent_direction_used += best.follows_intent;
            direction_adjusted += best.direction.dot(best.run.normal) < 1 - 1e-9;
            max_center_distance = std::max(max_center_distance, std::sqrt(best.distance_squared));
            return candidate;
        };

        for (const IntentSource &source : sources) {
            std::optional<FinCandidate> line = source.line.has_value() ? resolve_target(*source.line) : std::nullopt;
            std::optional<FinCandidate> point = resolve_target(source.point);
            if (line.has_value()) {
                FinCandidateProposal proposal{std::move(*line), std::nullopt};
                if (point.has_value())
                    proposal.point_fallback = std::move(*point);
                proposals.push_back(std::move(proposal));
                ++resolved_line_candidates;
            } else if (point.has_value()) {
                proposals.push_back({std::move(*point), std::nullopt});
                ++resolved_point_candidates;
                line_to_point_fallbacks += source.line.has_value() || source.point.edge.has_value();
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=painted-shape-candidates islands=" << painted_sides.islands.size()
        << " joined_painted_islands=" << painted_sides.joined_painted_islands
        << " point_intents=" << point_intents << " line_intents=" << line_intents
        << " branched_intents=" << branched_intents << " intent_strokes=" << intent_strokes
        << " unreliable_intent_charts=" << painted_sides.unreliable_intent_charts
        << " pruned_intent_strokes=" << painted_sides.pruned_intent_strokes
        << " raw_intent_paths=" << painted_sides.raw_intent_paths
        << " logical_intent_paths=" << painted_sides.logical_intent_paths
        << " joined_intent_paths=" << painted_sides.joined_intent_paths
        << " intent_junctions=" << painted_sides.intent_junctions
        << " paired_intent_junctions=" << painted_sides.paired_intent_junctions
        << " pruned_intent_branches=" << painted_sides.pruned_intent_branches
        << " blue_intent_groups=" << painted_sides.blue_intent_groups
        << " connector_blue_intent_groups=" << painted_sides.connector_blue_intent_groups
        << " discarded_blue_intent_groups=" << painted_sides.discarded_blue_intent_groups
        << " point_blue_intent_charts=" << painted_sides.point_blue_intent_charts
        << " trimmed_intent_samples=" << painted_sides.trimmed_intent_samples
        << " coarsened_intent_samples=" << painted_sides.coarsened_intent_samples
        << " junction_split_intent_strokes=" << painted_sides.junction_split_intent_strokes
        << " blue_intent_roots=" << painted_sides.blue_intent_roots
        << " merging_intent_strokes=" << painted_sides.merging_intent_strokes
        << " branch_fit_candidates=" << painted_sides.branch_fit_candidates
        << " branch_fit_rejects=" << painted_sides.branch_fit_rejects
        << " branch_fit_continuations=" << painted_sides.branch_fit_continuations
        << " branch_fit_ties=" << painted_sides.branch_fit_ties
        << " branch_parent_stops=" << painted_sides.branch_parent_stops
        << " branch_connection_misses=" << painted_sides.branch_connection_misses
        << " branch_cycle_cancellations=" << painted_sides.branch_cycle_cancellations
        << " fit_split_intent_strokes=" << painted_sides.fit_split_intent_strokes
        << " crease_intent_strokes=" << painted_sides.crease_intent_strokes
        << " macro_intent_patches=" << painted_sides.macro_intent_patches
        << " persistent_crease_edges=" << painted_sides.persistent_crease_edges
        << " line_plane_targets=" << line_plane_targets
        << " macro_plane_targets=" << macro_plane_targets
        << " line_to_point_fallbacks=" << line_to_point_fallbacks
        << " line_plane_alignment_rejects=" << line_plane_alignment_rejects
        << " line_plane_missing_macro=" << line_plane_missing_macro
        << " axis_limited_candidates=" << axis_limited_candidates
        << " plane_intersection_misses=" << plane_intersection_misses
        << " contact_span_site_rejects=" << contact_span_site_rejects
        << " resolved_line_candidates=" << resolved_line_candidates
        << " resolved_point_candidates=" << resolved_point_candidates
        << " candidates=" << proposals.size() << " too_small=" << too_small
        << " without_contour=" << without_contour
        << " contact_top_clamped=" << contact_top_clamped
        << " geometry_fallback=" << geometry_fallback
        << " shifted_to_contact_top=" << shifted_to_contact_top
        << " intent_direction_used=" << intent_direction_used
        << " direction_adjusted=" << direction_adjusted
        << " smallest_area_slack_mm2=" << (std::isfinite(smallest_area_slack) ? smallest_area_slack : 0.)
        << " max_center_distance_mm=" << max_center_distance;
    return proposals;
}

void generate_candidate_fin_support(PrintObject &object, const std::vector<PaintedLayer> &painted,
                                    const std::vector<Polygons> &blockers, const Polygon &bed,
                                    double line_width, const std::vector<FinCandidateProposal> &proposals,
                                    const char *stage, const char *diagnostic_stage,
                                    bool merge_intersecting = false)
{
    if (object.print()->canceled())
        return;
    std::vector<PaintedLayerGeometryCache> geometry_cache;
    if (merge_intersecting && !proposals.empty()) {
        size_t cache_layers = 0;
        for (const FinCandidateProposal &proposal : proposals) {
            cache_layers = std::max(
                cache_layers, proposal.primary.top_layer + 1);
            if (proposal.point_fallback.has_value())
                cache_layers = std::max(
                    cache_layers,
                    proposal.point_fallback->top_layer + 1);
        }
        geometry_cache.resize(
            std::min(cache_layers, size_t(object.layer_count())));
        const double gap = object.config().support_object_xy_distance.value;
        for (size_t layer_id = 0; layer_id < geometry_cache.size(); ++layer_id) {
            const Layer &layer = *object.get_layer(int(layer_id));
            PaintedLayerGeometryCache &cached = geometry_cache[layer_id];
            cached.line_width =
                support_material_flow(&object, float(layer.height)).width();
            cached.model_gap = gap > EPSILON ?
                offset_ex(
                    layer.lslices, float(scale_(gap)),
                    ClipperLib::jtRound, DefaultMiterLimit) :
                layer.lslices;
            cached.final_model_clearance = offset_ex(
                layer.lslices,
                float(scale_(gap + 0.5 * cached.line_width)),
                ClipperLib::jtRound, DefaultMiterLimit);
            cached.bed_center = offset_ex(
                bed, -float(scale_(0.5 * cached.line_width)));
        }
    }
    const auto *painted_geometry_cache =
        !geometry_cache.empty() ? &geometry_cache : nullptr;
    if (merge_intersecting)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-geometry-cache-complete"
            << " layers=" << geometry_cache.size();

    struct PendingFin {
        TracedFin fin;
        FinComponentCenterGeometry component_geometry;
        bool valid = true;
        bool owns_seed = true;
        // Assembled in center space. No model clearance or physical-width
        // expansion has been applied while the intent graph is still mutable.
        std::vector<ExPolygons> assembled_body_centers;
        std::vector<FinComponentCenterGeometry> assembled_components;
    };
    std::vector<PendingFin> pending;
    pending.reserve(proposals.size());
    FinDiagnostics diagnostics;
    FinDiagnostics fallback_diagnostics;
    size_t rejected_geometry = 0;
    size_t line_geometry_fallbacks = 0;
    size_t root_intent_fins = 0;
    std::vector<std::pair<size_t, size_t>> realized_source_roots;
    size_t promoted_intent_roots = 0;
    size_t attached_point_fallbacks = 0;
    size_t composite_intent_fins = 0;
    size_t composite_intent_segments = 0;
    size_t continuous_intent_envelopes = 0;
    struct StageFailureSummary {
        size_t count = 0;
        const char *reason = "none";
        long long layer = -1;
        double z = -1.;
        size_t island = std::numeric_limits<size_t>::max();
        size_t parent_edge = std::numeric_limits<size_t>::max();
        size_t edge = std::numeric_limits<size_t>::max();
        size_t value = 0;

        void record(const char *failure_reason, long long failure_layer,
                    double failure_z, size_t failure_island,
                    size_t failure_edge,
                    size_t failure_parent_edge = std::numeric_limits<size_t>::max(),
                    size_t failure_value = 0)
        {
            if (count++ > 0)
                return;
            reason = failure_reason;
            layer = failure_layer;
            z = failure_z;
            island = failure_island;
            parent_edge = failure_parent_edge;
            edge = failure_edge;
            value = failure_value;
        }
    };
    StageFailureSummary composite_merge_failures;
    StageFailureSummary assembly_failures;
    StageFailureSummary finalization_failures;
    const char *composite_merge_failure_stage = "none";
    size_t composite_merge_failure_layer = std::numeric_limits<size_t>::max();
    if (merge_intersecting)
        BOOST_LOG_TRIVIAL(debug)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-realization-begin"
            << " proposals=" << proposals.size();
    struct ProposalResult {
        bool complete = false;
        bool line = false;
        std::optional<size_t> pending;
    };
    std::vector<ProposalResult> proposal_results(proposals.size());
    auto proposal_for_edge = [&](size_t island, size_t edge) -> std::optional<size_t> {
        for (size_t i = 0; i < proposals.size(); ++i) {
            const FinCandidate &candidate = proposals[i].primary;
            if (candidate.intent_island.has_value() && candidate.intent_edge.has_value() &&
                *candidate.intent_island == island && *candidate.intent_edge == edge)
                return i;
        }
        return std::nullopt;
    };
    size_t completed_proposals = 0;
    while (completed_proposals < proposals.size()) {
        bool progressed = false;
        for (size_t proposal_id = 0; proposal_id < proposals.size(); ++proposal_id) {
            ProposalResult &result = proposal_results[proposal_id];
            if (result.complete)
                continue;
            if (object.print()->canceled())
                throw CanceledException();
            const FinCandidateProposal &proposal = proposals[proposal_id];
            std::optional<size_t> parent_proposal;
            if (proposal.primary.intent_island.has_value() && proposal.primary.intent_edge.has_value() &&
                proposal.primary.intent_parent_edge.has_value())
                parent_proposal = proposal_for_edge(
                    *proposal.primary.intent_island, *proposal.primary.intent_parent_edge);
            if (parent_proposal.has_value() && !proposal_results[*parent_proposal].complete)
                continue;
            const bool parent_line = parent_proposal.has_value() &&
                proposal_results[*parent_proposal].line && proposal_results[*parent_proposal].pending.has_value();
            const bool owns_seed = !proposal.primary.intent_line ||
                proposal.primary.intent_root.has_value() || !parent_line;
            TracedFin fin;
            FinComponentCenterGeometry component_geometry;
            if (realize_fin_candidate(
                    object, proposal.primary, bed, painted, blockers, line_width, fin,
                    owns_seed, merge_intersecting ? &component_geometry :
                        (proposal.primary.intent_line ? &component_geometry : nullptr),
                    painted_geometry_cache)) {
                result.pending = pending.size();
                result.line = proposal.primary.intent_line;
                component_geometry.source = pending.size();
                pending.push_back({std::move(fin), std::move(component_geometry), true, owns_seed});
                if (proposal.primary.intent_line && owns_seed) {
                    if (proposal.primary.intent_root) {
                        const auto key = std::make_pair(*proposal.primary.intent_island,
                                                       proposal.primary.intent_root->node);
                        if (std::find(realized_source_roots.begin(), realized_source_roots.end(), key) ==
                            realized_source_roots.end()) {
                            realized_source_roots.push_back(key);
                            ++root_intent_fins;
                        }
                    } else {
                        ++root_intent_fins;
                        promoted_intent_roots += parent_proposal.has_value();
                    }
                }
            } else if (!proposal.point_fallback.has_value()) {
                diagnostics.merge(fin.diagnostics);
                ++rejected_geometry;
            } else {
                fallback_diagnostics.merge(fin.diagnostics);
                TracedFin point;
                FinComponentCenterGeometry point_geometry;
                if (!realize_fin_candidate(
                        object, *proposal.point_fallback, bed, painted, blockers, line_width, point,
                        true, merge_intersecting ? &point_geometry : nullptr,
                        painted_geometry_cache)) {
                    diagnostics.merge(point.diagnostics);
                    ++rejected_geometry;
                } else {
                    result.pending = pending.size();
                    point_geometry.source = pending.size();
                    pending.push_back({std::move(point), std::move(point_geometry), true, true});
                    ++line_geometry_fallbacks;
                }
            }
            result.complete = true;
            ++completed_proposals;
            progressed = true;
        }
        if (progressed)
            continue;
        // Attachment hosts are established before their dependents. A cycle is
        // malformed data, not permission to invent another generation root.
        BOOST_LOG_TRIVIAL(error) << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-members reject=attachment-cycle";
        for (size_t proposal_id = 0; proposal_id < proposals.size(); ++proposal_id)
            if (!proposal_results[proposal_id].complete) {
                diagnostics.reject(FinReject::topology,
                    object.get_layer(int(proposals[proposal_id].primary.top_layer))->print_z);
                ++rejected_geometry;
            }
        break;
    }
    if (merge_intersecting)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-members-complete"
            << " proposals=" << proposals.size()
            << " pending=" << pending.size()
            << " rejected_geometry=" << rejected_geometry;

    // Every surviving line has completed its own contact validation, downward
    // per-layer contact construction and fixed-plane clipping at this point.
    // Preserve that completed result and its member-local body ownership. The
    // compound pass below may assemble them, but never reruns a member merely
    // because another child is added.
    // Related fits are pieces of one sheet. After all line members are complete,
    // the resolved graph root owns the configured outer depth, their body axes
    // form one centerline network, and thickness is applied once per layer.
    // Point fallbacks attach only after line-component completion.
    auto nearest_on_segment = [](const Vec2d &point, const Vec2d &a, const Vec2d &b) -> Vec2d {
        const Vec2d edge = b - a;
        if (edge.squaredNorm() <= EPSILON * EPSILON)
            return a;
        return a + std::clamp((point - a).dot(edge) / edge.squaredNorm(), 0., 1.) * edge;
    };
    auto nearest_on_area = [&](const Vec2d &point, const ExPolygon &area) {
        if (area.contains(scaled_point(point)))
            return point;
        Vec2d nearest = point;
        double best = std::numeric_limits<double>::infinity();
        auto visit = [&](const Polygon &polygon) {
            for (size_t i = 0; i < polygon.points.size(); ++i) {
                const Vec2d a = unscale(polygon.points[i]);
                const Vec2d b = unscale(polygon.points[(i + 1) % polygon.points.size()]);
                const Vec2d candidate = nearest_on_segment(point, a, b);
                const double distance = (candidate - point).squaredNorm();
                if (distance < best) {
                    best = distance;
                    nearest = candidate;
                }
            }
        };
        visit(area.contour);
        for (const Polygon &hole : area.holes)
            visit(hole);
        return nearest;
    };
    auto minimum_projection = [](const ExPolygons &areas, const Vec2d &origin, const Vec2d &direction) {
        double result = std::numeric_limits<double>::infinity();
        auto visit = [&](const Polygon &polygon) {
            for (const Point &point : polygon.points)
                result = std::min(result, (unscale(point) - origin).dot(direction));
        };
        for (const ExPolygon &area : areas) {
            visit(area.contour);
            for (const Polygon &hole : area.holes)
                visit(hole);
        }
        return result;
    };

    const double gap = object.config().support_object_xy_distance.value;
    const double bed_span = unscale(get_extents(bed).size()).norm() + 4 * line_width;
    auto offset_centerline_network = [&](const std::vector<FinBodyAxis> &axes,
                                         double radius, double cap_inset) {
        ExPolygons envelope;
        for (const FinBodyAxis &axis : axes) {
            const auto cap = fin_cap_segment(
                axis.inner, axis.direction, axis.cap_normal, radius,
                axis.inner_junction ? 0. : cap_inset);
            if (!cap.has_value())
                return ExPolygons{};
            // Intersect the axial strip with its analytical cap. Near the top
            // of a child, the junction can still lie behind that cap: its body
            // contribution is then empty, not an inverted cap-to-junction quad.
            const double begin_distance = std::min(
                (cap->first - axis.inner).dot(axis.direction),
                (cap->second - axis.inner).dot(axis.direction));
            if ((axis.outer - axis.inner).dot(axis.direction) <= begin_distance)
                continue;
            ExPolygons member;
            member.emplace_back(fin_rectangle(
                axis.inner + begin_distance * axis.direction,
                axis.outer, axis.direction, 2 * radius));
            if (axis.outer_junction) {
                Polygon joint = make_circle(scale_(radius), scale_(0.005));
                joint.translate(scaled_point(axis.outer));
                member.emplace_back(std::move(joint));
            }
            if (axis.inner_junction) {
                Polygon joint = make_circle(scale_(radius), scale_(0.005));
                joint.translate(scaled_point(axis.inner));
                member.emplace_back(std::move(joint));
                expolygons_append(envelope, union_ex(member));
                continue;
            }
            const Vec2d cap_center = axis.inner + cap_inset * axis.cap_normal;
            const Polygon cap_limit = fin_rectangle(
                cap_center, cap_center + bed_span * axis.cap_normal,
                axis.cap_normal, 2 * bed_span);
            expolygons_append(envelope, intersection_ex(union_ex(member), Polygons{cap_limit}));
        }
        return union_ex(envelope);
    };
    auto composite_line_body_center = [&](size_t layer_id, const std::vector<FinBodyAxis> &axes,
                                          double thickness) {
        const Layer &layer = *object.get_layer(int(layer_id));
        const double width = support_material_flow(&object, float(layer.height)).width();
        const double radius = 0.5 * (thickness - width);
        if (radius <= EPSILON)
            return ExPolygons{};
        const ExPolygons computed_bed_center =
            painted_geometry_cache == nullptr ||
                    layer_id >= painted_geometry_cache->size() ?
                offset_ex(bed, -float(scale_(0.5 * width))) :
                ExPolygons{};
        const ExPolygons &bed_center =
            painted_geometry_cache != nullptr &&
                    layer_id < painted_geometry_cache->size() ?
                (*painted_geometry_cache)[layer_id].bed_center :
                computed_bed_center;
        ExPolygons centers = intersection_ex(
            offset_centerline_network(axes, radius, 0.5 * width),
            bed_center);
        if (layer_id == 0 && !centers.empty()) {
            const double expansion = object.config().raft_first_layer_expansion.value;
            ExPolygons foot = intersection_ex(
                offset_ex(centers, float(scale_(expansion)),
                          jtRound, double(scale_(0.005))),
                bed_center);
            expolygons_append(centers, std::move(foot));
            centers = union_ex(centers);
        }
        return centers;
    };

    struct PreparedFinLayer {
        ExPolygons centers;
        ExPolygons contact_centers;
        // Contact sections and their plane-clipped downward transitions cross the
        // model-clearance band by construction. Keep that exemption separate
        // from semantic contact ownership; it is applied once to the complete
        // layer-wide clearance mask.
        ExPolygons clearance_exemptions;
        std::vector<ExPolygons> projection_sections;
    };
    auto assemble_component_section = [&](
        size_t layer_id, ExPolygons body_centers,
        const std::vector<FinComponentCenterGeometry> &components,
        PreparedFinLayer &prepared, const char *&failure_stage) {
        failure_stage = "none";
        prepared = {};
        for (const FinComponentCenterGeometry &component : components) {
            if (!component.complete) {
                failure_stage = "component-incomplete";
                return false;
            }
        }
        ExPolygons contact_sections;
        for (const FinComponentCenterGeometry &component : components) {
            if (component.source >= pending.size()) {
                failure_stage = "component-source";
                return false;
            }
            for (const FinCenterExtension &extension : component.extensions) {
                if (extension.state != FinExtensionState::complete) {
                    failure_stage = "extension-incomplete";
                    return false;
                }
                if (extension.layers.size() != extension.layer_states.size()) {
                    failure_stage = "extension-layer-count";
                    return false;
                }
                if (layer_id >= extension.layers.size())
                    continue;
                const FinExtensionLayerState layer_state = extension.layer_states[layer_id];
                if (layer_state == FinExtensionLayerState::invalid) {
                    failure_stage = "extension-layer-invalid";
                    return false;
                }
                const bool owns_geometry =
                    layer_state == FinExtensionLayerState::contact ||
                    layer_state == FinExtensionLayerState::materialized;
                if (owns_geometry != !extension.layers[layer_id].empty()) {
                    failure_stage = "extension-layer-geometry";
                    return false;
                }
                if (!owns_geometry)
                    continue;
                if (layer_state == FinExtensionLayerState::materialized) {
                    // Every member keeps its solved transition identifiable
                    // until the component completion barrier. It contributes
                    // support geometry and the clearance exemption required
                    // to connect the contact segment to its A1 cap.
                    expolygons_append(body_centers, extension.layers[layer_id]);
                    expolygons_append(
                        prepared.clearance_exemptions,
                        extension.layers[layer_id]);
                    prepared.projection_sections.push_back(
                        extension.layers[layer_id]);
                } else {
                    // Contact sections are part of the completed support and
                    // carve their own exemption out of the clearance mask.
                    expolygons_append(
                        prepared.contact_centers,
                        extension.layers[layer_id]);
                    expolygons_append(
                        prepared.clearance_exemptions,
                        extension.layers[layer_id]);
                    expolygons_append(contact_sections, extension.layers[layer_id]);
                }
            }
        }

        contact_sections = union_ex(contact_sections);
        prepared.contact_centers = union_ex(prepared.contact_centers);
        prepared.clearance_exemptions = union_ex(
            prepared.clearance_exemptions);
        expolygons_append(body_centers, contact_sections);
        prepared.centers = union_ex(body_centers);
        if (prepared.centers.empty()) {
            failure_stage = "empty-completed-center";
            return false;
        }
        return true;
    };

    auto prepare_line_intent_component = [&](
        TracedFin &hub, const std::vector<size_t> &members,
        std::vector<ExPolygons> &assembled_body_centers,
        std::vector<FinComponentCenterGeometry> &assembled_components) {
        composite_merge_failure_stage = "none";
        composite_merge_failure_layer = std::numeric_limits<size_t>::max();
        auto fail = [&](const char *failure_stage,
                        size_t layer_id = std::numeric_limits<size_t>::max()) {
            composite_merge_failure_stage = failure_stage;
            composite_merge_failure_layer = layer_id;
            return false;
        };
        const size_t layer_count = hub.body_axes.size();
        if (members.empty() || layer_count == 0)
            return fail("source-shape");
        // This is the component completion barrier. Do not construct even a
        // temporary compound layer until every member has finished its own
        // local contact construction and fixed-plane clipping.
        assembled_components.clear();
        assembled_components.reserve(members.size());
        std::vector<bool> member_complete(members.size(), false);
        double thickness = hub.seed.dimensions.thickness;
        for (size_t member_id = 0; member_id < members.size(); ++member_id) {
            const size_t member = members[member_id];
            if (member >= pending.size())
                return fail("member-source");
            const FinComponentCenterGeometry &component_geometry =
                pending[member].component_geometry;
            if (!component_geometry.complete || component_geometry.source != member)
                return fail("member-incomplete");
            FinComponentCenterGeometry completed_component;
            completed_component.source = component_geometry.source;
            completed_component.complete = component_geometry.complete;
            completed_component.extensions = component_geometry.extensions;
            assembled_components.push_back(std::move(completed_component));
            member_complete[member_id] = true;
            thickness = std::max(
                thickness, pending[member].fin.seed.dimensions.thickness);
        }
        if (!std::all_of(member_complete.begin(), member_complete.end(), [](bool complete) {
                return complete;
            }))
            return fail("member-incomplete");

        assembled_body_centers.assign(layer_count, {});
        for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
            if (object.print()->canceled())
                throw CanceledException();
            if (hub.body_axes[layer_id].empty())
                return fail("body-path-shape", layer_id);
            assembled_body_centers[layer_id] = composite_line_body_center(
                layer_id, hub.body_axes[layer_id], thickness);
            if (assembled_body_centers[layer_id].empty())
                return fail("empty-body-center", layer_id);
            // Retain each member's contribution after its axis was attached.
            // Sibling filling must not infer ownership from the compound union.
            for (FinComponentCenterGeometry &component : assembled_components) {
                const FinCandidate &seed = pending[component.source].fin.seed;
                if (!seed.source_parent_junction)
                    continue;
                component.body_centers.resize(layer_count);
                std::vector<FinBodyAxis> axes;
                for (const FinBodyAxis &axis : hub.body_axes[layer_id])
                    if (axis.intent_edge == seed.intent_edge)
                        axes.push_back(axis);
                if (!axes.empty())
                    component.body_centers[layer_id] = composite_line_body_center(
                        layer_id, axes, thickness);
            }
        }

        std::vector<bool> merged_contact_flags(layer_count, false);
        FinDiagnostics merged_diagnostics;
        size_t planned_contact_bands = 0;
        size_t accepted_contact_bands = 0;
        size_t seed_contact_bands = 0;
        size_t suppressed_contact_bands = 0;
        size_t contact_projection_attempts = 0;
        size_t contact_projection_layers = 0;
        size_t contact_projection_max_layers = 0;
        size_t contact_projection_zero_layers = 0;
        size_t contact_projection_exposed_layers = 0;
        std::array<size_t, size_t(ProjectionStop::count)> contact_projection_stops{};
        for (const size_t member : members) {
            const TracedFin &component = pending[member].fin;
            for (size_t layer_id = 0; layer_id < component.contacts.size(); ++layer_id)
                merged_contact_flags[layer_id] = merged_contact_flags[layer_id] ||
                    component.contacts[layer_id];
            merged_diagnostics.merge(component.diagnostics);
            planned_contact_bands += component.planned_contact_bands;
            accepted_contact_bands += component.accepted_contact_bands;
            seed_contact_bands += component.seed_contact_bands;
            suppressed_contact_bands += component.suppressed_contact_bands;
            contact_projection_attempts += component.contact_projection_attempts;
            contact_projection_layers += component.contact_projection_layers;
            contact_projection_max_layers = std::max(
                contact_projection_max_layers, component.contact_projection_max_layers);
            contact_projection_zero_layers += component.contact_projection_zero_layers;
            contact_projection_exposed_layers += component.contact_projection_exposed_layers;
            for (size_t stop = 0; stop < contact_projection_stops.size(); ++stop)
                contact_projection_stops[stop] += component.contact_projection_stops[stop];
        }

        // Final physical geometry is deliberately absent here. Point members
        // may still join this component; model clearance and bead expansion
        // happen once only after that graph is closed.
        hub.areas.clear();
        hub.centers.clear();
        hub.contact_centers.clear();
        hub.seed.dimensions.thickness = thickness;
        hub.diagnostics = std::move(merged_diagnostics);
        hub.planned_contact_bands = planned_contact_bands;
        hub.accepted_contact_bands = accepted_contact_bands;
        hub.seed_contact_bands = seed_contact_bands;
        hub.suppressed_contact_bands = suppressed_contact_bands;
        hub.contact_projection_attempts = contact_projection_attempts;
        hub.contact_projection_layers = contact_projection_layers;
        hub.contact_projection_max_layers = contact_projection_max_layers;
        hub.contact_projection_zero_layers = contact_projection_zero_layers;
        hub.contact_projection_exposed_layers = contact_projection_exposed_layers;
        hub.contact_projection_retained_layers = 0;
        hub.contact_projection_stops = contact_projection_stops;
        hub.intent_component_size = members.size();
        hub.contacts = std::move(merged_contact_flags);
        return true;
    };

    auto prepared_layer_center = [](
        const std::vector<ExPolygons> &body_centers,
        const std::vector<FinComponentCenterGeometry> &components,
        size_t layer_id) {
        ExPolygons result;
        if (layer_id < body_centers.size())
            result = body_centers[layer_id];
        for (const FinComponentCenterGeometry &component : components)
            for (const FinCenterExtension &extension : component.extensions)
                if (layer_id < extension.layers.size())
                    expolygons_append(result, extension.layers[layer_id]);
        return union_ex(result);
    };
    auto prepared_member_layer_center = [](
        const FinComponentCenterGeometry &component, size_t layer_id) {
        ExPolygons result;
        if (layer_id < component.body_centers.size())
            result = component.body_centers[layer_id];
        for (const FinCenterExtension &extension : component.extensions)
            if (layer_id < extension.layers.size())
                expolygons_append(result, extension.layers[layer_id]);
        return union_ex(result);
    };
    auto nearest_on_areas = [&](const Vec2d &point, const ExPolygons &areas) {
        Vec2d nearest = point;
        double best = std::numeric_limits<double>::infinity();
        for (const ExPolygon &area : areas) {
            const Vec2d candidate = nearest_on_area(point, area);
            const double distance = (candidate - point).squaredNorm();
            if (distance < best) {
                best = distance;
                nearest = candidate;
            }
        }
        return nearest;
    };
    auto merge_intent_segment = [&](PendingFin &hub_item, PendingFin &segment_item,
                                    const Vec2d &node) {
        auto fail = [&](const char *stage,
                        size_t layer_id = std::numeric_limits<size_t>::max()) {
            composite_merge_failure_stage = stage;
            composite_merge_failure_layer = layer_id;
            return false;
        };
        TracedFin &hub = hub_item.fin;
        const TracedFin &segment = segment_item.fin;
        if (!segment_item.component_geometry.complete ||
            segment_item.component_geometry.body_centers.empty())
            return fail("point-source-shape");
        if (hub_item.assembled_body_centers.empty()) {
            if (!hub_item.component_geometry.complete ||
                hub_item.component_geometry.body_centers.empty())
                return fail("point-parent-shape");
            hub_item.assembled_components = {hub_item.component_geometry};
            hub_item.assembled_body_centers =
                std::move(hub_item.component_geometry.body_centers);
        }

        FinComponentCenterGeometry attached = segment_item.component_geometry;
        const size_t segment_layers = attached.body_centers.size();
        hub_item.assembled_body_centers.resize(std::max(
            hub_item.assembled_body_centers.size(), segment_layers));
        for (size_t layer_id = 0; layer_id < segment_layers; ++layer_id) {
            if (object.print()->canceled())
                throw CanceledException();
            const Layer &layer = *object.get_layer(int(layer_id));
            ExPolygons base = prepared_layer_center(
                hub_item.assembled_body_centers,
                hub_item.assembled_components, layer_id);
            ExPolygons point_center =
                prepared_member_layer_center(attached, layer_id);
            if (point_center.empty())
                return fail("point-empty-center", layer_id);
            if (base.empty()) {
                expolygons_append(
                    hub_item.assembled_body_centers[layer_id],
                    attached.body_centers[layer_id]);
                continue;
            }

            ExPolygons overlap = intersection_ex(base, point_center);
            ExPolygons bridge;
            if (overlap.empty()) {
                const Vec2d joint = node;
                const Vec2d first = nearest_on_areas(joint, base);
                const Vec2d second = nearest_on_areas(joint, point_center);
                const Vec2d delta = second - first;
                const double distance = delta.norm();
                const double thickness = std::max(
                    hub.seed.dimensions.thickness, segment.seed.dimensions.thickness);
                if (distance > std::max(2 * line_width, thickness) + EPSILON)
                    return fail("point-attach-distance", layer_id);
                const Vec2d direction = distance > EPSILON ? delta / distance : hub.seed.normal;
                const Vec2d begin = distance > EPSILON ? first : first - 0.5 * line_width * direction;
                const Vec2d end = distance > EPSILON ? second : first + 0.5 * line_width * direction;
                const double center_thickness = thickness -
                    support_material_flow(&object, float(layer.height)).width();
                if (center_thickness <= EPSILON)
                    return fail("point-bridge-width", layer_id);
                bridge = intersection_ex(
                    Polygons{fin_rectangle(
                        begin, end, direction, center_thickness)},
                    painted_geometry_cache != nullptr &&
                            layer_id < painted_geometry_cache->size() ?
                        (*painted_geometry_cache)[layer_id].bed_center :
                        offset_ex(
                            bed, -float(scale_(0.5 * support_material_flow(
                                &object, float(layer.height)).width()))));
                if (bridge.empty())
                    return fail("point-bridge-empty", layer_id);
                expolygons_append(base, bridge);
                base = union_ex(base);
                overlap = intersection_ex(base, point_center);
                if (overlap.empty())
                    return fail("point-bridge-attach", layer_id);
            }

            const Vec2d origin = segment.body_start[layer_id];
            const Vec2d direction = segment.seed.normal;
            const double connection = minimum_projection(overlap, origin, direction);
            if (!std::isfinite(connection))
                return fail("point-connection", layer_id);
            const double behind = object.config().support_object_xy_distance.value + 2 * line_width;
            const double depth = std::clamp(connection + line_width, 2 * line_width, bed_span);
            const Polygon limit = fin_rectangle(
                origin - behind * direction, origin + depth * direction,
                direction, 2 * bed_span);
            attached.body_centers[layer_id] = intersection_ex(
                attached.body_centers[layer_id], Polygons{limit});
            for (FinCenterExtension &extension : attached.extensions) {
                if (layer_id >= extension.layers.size() ||
                    extension.layers[layer_id].empty())
                    continue;
                extension.layers[layer_id] = intersection_ex(
                    extension.layers[layer_id], Polygons{limit});
                if (extension.layers[layer_id].empty())
                    return fail("point-extension-empty", layer_id);
            }
            ExPolygons retained =
                prepared_member_layer_center(attached, layer_id);
            if (retained.empty())
                return fail("point-retained-empty", layer_id);
            expolygons_append(
                hub_item.assembled_body_centers[layer_id],
                attached.body_centers[layer_id]);
            expolygons_append(
                hub_item.assembled_body_centers[layer_id], bridge);
            expolygons_append(base, retained);
            base = union_ex(base);
            // Closed holes are evaluated after all sibling members complete.
            if (base.size() != 1)
                return fail("point-completed-topology", layer_id);
        }
        hub_item.assembled_components.push_back(std::move(attached));
        // The graph is closed before this center-space attachment. The merged
        // component no longer has one ordered line path, but it is still not
        // finalized physical geometry.
        hub.body_axes.clear();
        hub.seed.top_layer = std::max(
            hub.seed.top_layer, segment.seed.top_layer);
        hub.diagnostics.merge(segment.diagnostics);
        hub.planned_contact_bands += segment.planned_contact_bands;
        hub.accepted_contact_bands += segment.accepted_contact_bands;
        hub.seed_contact_bands += segment.seed_contact_bands;
        hub.suppressed_contact_bands += segment.suppressed_contact_bands;
        hub.contact_projection_attempts += segment.contact_projection_attempts;
        hub.contact_projection_layers += segment.contact_projection_layers;
        hub.contact_projection_max_layers = std::max(
            hub.contact_projection_max_layers,
            segment.contact_projection_max_layers);
        hub.contact_projection_zero_layers +=
            segment.contact_projection_zero_layers;
        hub.contact_projection_exposed_layers +=
            segment.contact_projection_exposed_layers;
        for (size_t stop = 0; stop < hub.contact_projection_stops.size(); ++stop)
            hub.contact_projection_stops[stop] +=
                segment.contact_projection_stops[stop];
        if (hub.contacts.size() < segment.contacts.size())
            hub.contacts.resize(segment.contacts.size(), false);
        for (size_t i = 0; i < segment.contacts.size(); ++i)
            hub.contacts[i] = hub.contacts[i] || segment.contacts[i];
        return true;
    };

    std::vector<std::optional<size_t>> intent_parent(pending.size());
    auto pending_for_edge = [&](size_t island, size_t edge) -> std::optional<size_t> {
        for (size_t i = 0; i < pending.size(); ++i) {
            const FinCandidate &candidate = pending[i].fin.seed;
            if (pending[i].valid && candidate.intent_island.has_value() &&
                candidate.intent_edge.has_value() && *candidate.intent_island == island &&
                *candidate.intent_edge == edge)
                return i;
        }
        return std::nullopt;
    };
    for (size_t child = 0; child < pending.size(); ++child) {
        const FinCandidate &candidate = pending[child].fin.seed;
        if (!pending[child].valid || !candidate.intent_island.has_value() ||
            !candidate.intent_parent_edge.has_value())
            continue;
        const std::optional<size_t> parent = pending_for_edge(
            *candidate.intent_island, *candidate.intent_parent_edge);
        if (!parent.has_value())
            continue;
        // A point result retains its skeleton relationship, but only a
        // surviving parent line can own a composite sheet. A line below a
        // point result is a new root and keeps its own seed.
        if (!pending[*parent].fin.seed.intent_line)
            continue;
        intent_parent[child] = *parent;
    }
    // Graph rooting precedes line/point resolution. If that arbitrary root
    // direction leaves a point above a surviving line, attach the point back
    // to one directly adjacent line and let that line remain the component
    // root. Other line children still begin their own components.
    for (size_t point = 0; point < pending.size(); ++point) {
        const FinCandidate &candidate = pending[point].fin.seed;
        if (!pending[point].valid || candidate.intent_line || intent_parent[point].has_value() ||
            !candidate.intent_island.has_value() || !candidate.intent_edge.has_value())
            continue;
        std::optional<size_t> line_child;
        for (size_t child = 0; child < pending.size(); ++child) {
            const FinCandidate &child_candidate = pending[child].fin.seed;
            if (!pending[child].valid || !child_candidate.intent_line ||
                child_candidate.intent_island != candidate.intent_island ||
                child_candidate.intent_parent_edge != candidate.intent_edge)
                continue;
            if (!line_child.has_value() ||
                child_candidate.top_layer > pending[*line_child].fin.seed.top_layer)
                line_child = child;
        }
        if (line_child.has_value()) {
            intent_parent[point] = *line_child;
        }
    }
    auto shared_intent_node = [&](size_t parent, size_t child) -> std::optional<Vec2d> {
        const FinCandidate &first = pending[parent].fin.seed;
        const FinCandidate &second = pending[child].fin.seed;
        if (first.intent_island != second.intent_island)
            return std::nullopt;
        for (const FinCandidate::IntentNode &a : first.intent_nodes)
            for (const FinCandidate::IntentNode &b : second.intent_nodes)
                if (a.node == b.node && (a.point - b.point).norm() <= EPSILON)
                    return a.point;
        return std::nullopt;
    };
    auto log_composite_failure = [&](size_t parent, size_t child) {
        const bool has_failure_layer =
            composite_merge_failure_layer < object.layer_count();
        const long long failure_layer = has_failure_layer ?
            static_cast<long long>(composite_merge_failure_layer) : -1;
        const double failure_z = has_failure_layer ?
            object.get_layer(int(composite_merge_failure_layer))->print_z : -1.;
        const size_t island = pending[child].fin.seed.intent_island.value_or(
            std::numeric_limits<size_t>::max());
        const size_t parent_edge = pending[parent].fin.seed.intent_edge.value_or(
            std::numeric_limits<size_t>::max());
        const size_t child_edge = pending[child].fin.seed.intent_edge.value_or(
            std::numeric_limits<size_t>::max());
        composite_merge_failures.record(
            composite_merge_failure_stage, failure_layer, failure_z,
            island, child_edge, parent_edge);
        BOOST_LOG_TRIVIAL(debug)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-composite-merge"
            << " reject=" << composite_merge_failure_stage
            << " layer=" << failure_layer
            << " z=" << failure_z
            << " island=" << island
            << " parent_edge=" << parent_edge
            << " child_edge=" << child_edge;
    };
    auto promote_intent_root = [&](size_t member) {
        if (!pending[member].valid || pending[member].owns_seed)
            return pending[member].valid;
        TracedFin root;
        FinComponentCenterGeometry component_geometry;
        component_geometry.source = member;
        if (!realize_fin_candidate(
                object, pending[member].fin.seed, bed, painted, blockers,
                line_width, root, true, &component_geometry,
                painted_geometry_cache)) {
            diagnostics.merge(root.diagnostics);
            pending[member].valid = false;
            ++rejected_geometry;
            return false;
        }
        pending[member].fin = std::move(root);
        pending[member].component_geometry = std::move(component_geometry);
        pending[member].owns_seed = true;
        ++root_intent_fins;
        ++promoted_intent_roots;
        return true;
    };

    struct LineConnection {
        size_t member;
        FinCandidate::IntentNode node;
    };
    std::vector<std::vector<LineConnection>> line_connections(pending.size());
    // Validate the actual fixed A axes before assigning composite membership.
    // Two independent roots can connect, and a member can join at both ends.
    for (size_t first = 0; first < pending.size(); ++first) {
        if (!pending[first].valid || !pending[first].fin.seed.intent_line)
            continue;
        for (size_t second = first + 1; second < pending.size(); ++second) {
            if (!pending[second].valid || !pending[second].fin.seed.intent_line ||
                pending[first].fin.seed.intent_island != pending[second].fin.seed.intent_island)
                continue;
            for (const FinCandidate::IntentNode &a : pending[first].fin.seed.intent_nodes) {
                const auto &nodes = pending[second].fin.seed.intent_nodes;
                const auto match = std::find_if(nodes.begin(), nodes.end(),
                    [&](const FinCandidate::IntentNode &b) { return a.node == b.node; });
                if (match == nodes.end())
                    continue;
                bool valid = (a.point - match->point).norm() <= EPSILON;
                composite_merge_failure_layer = std::numeric_limits<size_t>::max();
                const size_t count = std::min(pending[first].fin.body_axes.size(),
                                              pending[second].fin.body_axes.size());
                for (size_t layer = 0; valid && layer < count; ++layer) {
                    for (const size_t member : {first, second}) {
                        const auto &axes = pending[member].fin.body_axes[layer];
                        if (axes.size() != 1 || std::abs(cross_2d(
                                a.point - axes.front().inner, axes.front().direction)) > EPSILON) {
                            valid = false;
                            composite_merge_failure_layer = layer;
                            break;
                        }
                    }
                }
                if (!valid) {
                    composite_merge_failure_stage = "junction-axis";
                    log_composite_failure(first, second);
                    continue;
                }
                line_connections[first].push_back({second, a});
                line_connections[second].push_back({first, a});
            }
        }
    }

    std::vector<bool> line_processed(pending.size(), false);
    std::vector<size_t> line_owner(pending.size(), std::numeric_limits<size_t>::max());
    std::vector<bool> emitted_composite(pending.size(), false);
    for (size_t start = 0; start < pending.size(); ++start) {
        if (!pending[start].valid || !pending[start].fin.seed.intent_line || line_processed[start])
            continue;
        std::vector<size_t> members{start};
        line_processed[start] = true;
        for (size_t i = 0; i < members.size(); ++i)
            for (const LineConnection &connection : line_connections[members[i]])
                if (!line_processed[connection.member]) {
                    line_processed[connection.member] = true;
                    members.push_back(connection.member);
                }
        // This representative only owns storage. It neither creates a root nor
        // suppresses any other root's seed, height bounds or contact construction.
        const size_t root = *std::max_element(members.begin(), members.end(), [&](size_t a, size_t b) {
            return pending[a].fin.seed.top_layer < pending[b].fin.seed.top_layer;
        });
        const bool has_seed = std::any_of(members.begin(), members.end(),
            [&](size_t member) { return pending[member].owns_seed; });
        if (!has_seed && !promote_intent_root(root)) {
            for (const size_t member : members) {
                if (pending[member].valid)
                    ++rejected_geometry;
                pending[member].valid = false;
            }
            continue;
        }
        TracedFin completed = pending[root].fin;
        completed.body_axes.assign(completed.seed.top_layer + 1, {});
        struct RootExtension {
            size_t node;
            Vec2d point;
            std::vector<size_t> members;
            Vec2d direction = Vec2d::Zero();
        };
        std::vector<RootExtension> extensions;
        std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
            return pending[a].fin.seed.intent_edge < pending[b].fin.seed.intent_edge;
        });
        for (const size_t member : members) {
            const TracedFin &fin = pending[member].fin;
            if (fin.seed.intent_root) {
                const auto &source = *fin.seed.intent_root;
                auto extension = std::find_if(extensions.begin(), extensions.end(),
                    [&](const RootExtension &item) { return item.node == source.node; });
                if (extension == extensions.end()) {
                    extensions.push_back({source.node, source.point, {member}});
                } else
                    extension->members.push_back(member);
            }
            for (size_t layer = 0; layer < fin.body_axes.size(); ++layer) {
                FinBodyAxis axis = fin.body_axes[layer].front();
                std::optional<Vec2d> outer;
                if (fin.seed.intent_root)
                    outer = fin.seed.intent_root->point;
                for (const LineConnection &connection : line_connections[member])
                    if (!outer || (connection.node.point - *outer).dot(axis.direction) > 0.)
                        outer = connection.node.point;
                // Promoted disconnected members retain their independently solved
                // outer end. Connected members terminate at their farthest actual
                // junction on A, preserving their own A1 and analytical cap.
                if (outer && (fin.seed.intent_root || !pending[member].owns_seed)) {
                    axis.outer = *outer;
                    axis.outer_junction = true;
                }
                // A tail merge is the model-facing end of this member. Once
                // reached, join it as a junction rather than cutting it back
                // with the half-bead model cap. The final gap clip still applies.
                axis.inner_junction = fin.seed.axis_limit && fin.wall_detach_layer &&
                    layer <= *fin.wall_detach_layer &&
                    std::any_of(line_connections[member].begin(), line_connections[member].end(),
                        [&](const LineConnection &connection) {
                            return (connection.node.point - *fin.seed.axis_limit).norm() <= EPSILON;
                        });
                completed.body_axes[layer].push_back(std::move(axis));
            }
        }
        completed.intent_root_count = extensions.size() + size_t(std::count_if(
            members.begin(), members.end(), [&](size_t member) {
                return pending[member].owns_seed && !pending[member].fin.seed.intent_root;
            }));
        for (RootExtension &extension : extensions) {
            std::vector<std::pair<double, size_t>> angles;
            for (size_t i = 0; i < extension.members.size(); ++i) {
                const Vec2d &direction = pending[extension.members[i]].fin.seed.intent_root->outgoing;
                double angle = std::atan2(direction.y(), direction.x());
                if (angle < 0.)
                    angle += 2. * M_PI;
                angles.emplace_back(angle, i);
            }
            std::stable_sort(angles.begin(), angles.end());
            double largest = -1.;
            size_t first = angles.size();
            double middle = 0.;
            for (size_t i = 0; i < angles.size(); ++i) {
                const double next = i + 1 < angles.size() ? angles[i + 1].first : angles.front().first + 2. * M_PI;
                const double gap = next - angles[i].first;
                if (gap > largest || (gap == largest && angles[i].second < first)) {
                    largest = gap;
                    first = angles[i].second;
                    middle = angles[i].first + 0.5 * gap;
                }
            }
            extension.direction = Vec2d(std::cos(middle), std::sin(middle));
            for (size_t layer = 0; layer < completed.body_axes.size(); ++layer) {
                double reach = 0.;
                for (const size_t member : extension.members) {
                    const TracedFin &fin = pending[member].fin;
                    if (layer >= fin.tips.size())
                        continue;
                    // Each member has already stopped/continued at its own XY limit.
                    // Coincident root extensions are united geometrically, not by
                    // inventing a shared stop height or tracing another contact.
                    reach = std::max(reach, (fin.tips[layer] - fin.seed.point).dot(fin.seed.normal));
                }
                const double width = support_material_flow(
                    &object, float(object.get_layer(int(layer))->height)).width();
                if (reach <= 0.5 * width)
                    continue;
                completed.body_axes[layer].push_back({
                    extension.point + (reach - 0.5 * width) * extension.direction,
                    extension.point, extension.direction, extension.direction,
                    std::nullopt, false, true});
            }
        }
        std::vector<ExPolygons> assembled_body_centers;
        std::vector<FinComponentCenterGeometry> assembled_components;
        if (!prepare_line_intent_component(completed, members, assembled_body_centers, assembled_components)) {
            log_composite_failure(root, root);
            // An invalid completed component must not silently acquire another
            // topology or duplicate shared root extensions as standalone fins.
            for (const size_t member : members) {
                pending[member].valid = false;
                ++rejected_geometry;
            }
            continue;
        }
        pending[root].fin = std::move(completed);
        pending[root].assembled_body_centers = std::move(assembled_body_centers);
        pending[root].assembled_components = std::move(assembled_components);
        for (const size_t member : members) {
            line_owner[member] = root;
            if (member != root)
                pending[member].valid = false;
        }
        if (members.size() > 1) {
            composite_intent_segments += members.size() - 1;
            emitted_composite[root] = true;
            std::map<size_t, std::vector<size_t>> node_members;
            for (const size_t member : members)
                for (const LineConnection &connection : line_connections[member]) {
                    auto &incident = node_members[connection.node.node];
                    if (std::find(incident.begin(), incident.end(), member) == incident.end())
                        incident.push_back(member);
                }
            continuous_intent_envelopes += std::count_if(node_members.begin(), node_members.end(),
                [](const auto &entry) { return entry.second.size() == 2; });
        }
    }

    // Point fallbacks remain prepared center geometry. Attach them only after
    // every line component has completed its downward/upward work, but before
    // the one layer-wide model-clearance pass.
    for (size_t point = 0; point < pending.size(); ++point) {
        if (!pending[point].valid || pending[point].fin.seed.intent_line ||
            !intent_parent[point].has_value())
            continue;
        const size_t parent = *intent_parent[point];
        if (parent >= line_owner.size() ||
            line_owner[parent] == std::numeric_limits<size_t>::max())
            continue;
        const size_t hub = line_owner[parent];
        const std::optional<Vec2d> joint = shared_intent_node(parent, point);
        composite_merge_failure_stage = "point-geometry";
        composite_merge_failure_layer = std::numeric_limits<size_t>::max();
        if (!joint.has_value() ||
            !merge_intent_segment(pending[hub], pending[point], *joint)) {
            log_composite_failure(parent, point);
            continue;
        }
        pending[point].valid = false;
        pending[hub].fin.intent_component_size += 1;
        ++composite_intent_segments;
        ++attached_point_fallbacks;
        emitted_composite[hub] = true;
    }
    composite_intent_fins += std::count(
        emitted_composite.begin(), emitted_composite.end(), true);
    if (merge_intersecting)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-components-complete"
            << " pending=" << pending.size()
            << " composite_fins=" << composite_intent_fins
            << " merge_failures=" << composite_merge_failures.count;

    if (merge_intersecting) {
        // The intent graph is immutable from this point onward. First assemble
        // every component in center space. The layer-wide union is then clipped
        // by one contact-and-transition-exempted model mask exactly once;
        // ownership is mapped back afterward without applying model clearance
        // again.
        std::vector<std::vector<PreparedFinLayer>> prepared(pending.size());
        std::vector<ExPolygons> layer_centers(object.layer_count());
        std::vector<ExPolygons> layer_clearance_exemptions(object.layer_count());
        size_t projection_exemption_sections = 0;
        for (size_t item_id = 0; item_id < pending.size(); ++item_id) {
            PendingFin &item = pending[item_id];
            if (!item.valid)
                continue;
            if (item.assembled_body_centers.empty()) {
                if (!item.component_geometry.complete ||
                    item.component_geometry.body_centers.empty()) {
                    item.valid = false;
                    ++rejected_geometry;
                    continue;
                }
                item.assembled_components = {item.component_geometry};
                item.assembled_body_centers =
                    std::move(item.component_geometry.body_centers);
            }
            const size_t layer_count = item.assembled_body_centers.size();
            prepared[item_id].resize(layer_count);
            bool assembled = true;
            for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
                const char *failure_stage = "none";
                if (!assemble_component_section(
                        layer_id, item.assembled_body_centers[layer_id],
                        item.assembled_components,
                        prepared[item_id][layer_id], failure_stage)) {
                    const double failure_z =
                        object.get_layer(int(layer_id))->print_z;
                    const size_t failure_island =
                        item.fin.seed.intent_island.value_or(
                            std::numeric_limits<size_t>::max());
                    const size_t failure_edge =
                        item.fin.seed.intent_edge.value_or(
                            std::numeric_limits<size_t>::max());
                    assembly_failures.record(
                        failure_stage, static_cast<long long>(layer_id),
                        failure_z, failure_island, failure_edge);
                    BOOST_LOG_TRIVIAL(debug)
                        << "Fin support object=" << object.model_object()->name
                        << " stage=painted-shape-assemble"
                        << " reject=" << failure_stage
                        << " layer=" << layer_id
                        << " z=" << failure_z
                        << " island=" << failure_island
                        << " edge=" << failure_edge;
                    item.fin.diagnostics.reject(
                        FinReject::topology,
                        object.get_layer(int(layer_id))->print_z);
                    assembled = false;
                    break;
                }
            }
            if (!assembled) {
                item.valid = false;
                diagnostics.merge(item.fin.diagnostics);
                ++rejected_geometry;
                continue;
            }
            for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
                expolygons_append(
                    layer_centers[layer_id],
                    prepared[item_id][layer_id].centers);
                expolygons_append(
                    layer_clearance_exemptions[layer_id],
                    prepared[item_id][layer_id].clearance_exemptions);
                projection_exemption_sections +=
                    prepared[item_id][layer_id].projection_sections.size();
            }
        }

        // Group by immutable island/parent/junction, not the corrected node or
        // current component owner. Detached siblings can belong to different
        // components, and the parent itself never contributes to this union.
        struct SiblingMember {
            size_t owner;
            const FinComponentCenterGeometry *component;
        };
        std::map<std::tuple<size_t, size_t, size_t>, std::vector<SiblingMember>> sibling_groups;
        for (size_t owner = 0; owner < pending.size(); ++owner) {
            if (!pending[owner].valid)
                continue;
            for (const FinComponentCenterGeometry &component : pending[owner].assembled_components) {
                const FinCandidate &seed = pending[component.source].fin.seed;
                if (seed.intent_island && seed.source_parent_junction)
                    sibling_groups[{*seed.intent_island, seed.source_parent_junction->first,
                                    seed.source_parent_junction->second}].push_back({owner, &component});
            }
        }
        size_t sibling_cavity_sections = 0;
        for (const auto &entry : sibling_groups) {
            const auto &members = entry.second;
            if (members.size() < 2)
                continue;
            for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
                if (object.print()->canceled())
                    throw CanceledException();
                ExPolygons siblings;
                std::vector<ExPolygons> member_centers(members.size());
                size_t active = 0;
                for (size_t i = 0; i < members.size(); ++i) {
                    member_centers[i] = prepared_member_layer_center(*members[i].component, layer_id);
                    if (!member_centers[i].empty()) {
                        ++active;
                        expolygons_append(siblings, member_centers[i]);
                    }
                }
                if (active < 2)
                    continue;
                // Only bounded holes of this sibling union are filled. Open
                // notches and cavities requiring a parent/other group stay open.
                siblings = union_ex(siblings);
                for (const ExPolygon &area : siblings) {
                    for (Polygon hole : area.holes) {
                        hole.reverse();
                        const ExPolygons cavity{ExPolygon(std::move(hole))};
                        // Assign once to a touching member's owner so the final
                        // ownership intersection retains the fill. This changes
                        // no junction or parent relationship.
                        for (size_t i = 0; i < members.size(); ++i) {
                            const size_t owner = members[i].owner;
                            if (layer_id >= prepared[owner].size() || member_centers[i].empty() ||
                                intersection_ex(cavity, offset_ex(member_centers[i],
                                    float(scale_(EPSILON)))).empty())
                                continue;
                            expolygons_append(prepared[owner][layer_id].centers, cavity);
                            prepared[owner][layer_id].centers = union_ex(prepared[owner][layer_id].centers);
                            expolygons_append(layer_centers[layer_id], cavity);
                            ++sibling_cavity_sections;
                            break;
                        }
                    }
                }
            }
        }
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-sibling-cavities"
            << " filled_sections=" << sibling_cavity_sections;

        std::vector<ExPolygons> clipped_layer_centers(object.layer_count());
        size_t nonempty_support_layers = 0;
        size_t model_gap_clip_layers = 0;
        size_t clearance_exemption_layers = 0;
        for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
            if (layer_centers[layer_id].empty())
                continue;
            ++nonempty_support_layers;
            layer_centers[layer_id] = union_ex(layer_centers[layer_id]);
            layer_clearance_exemptions[layer_id] =
                union_ex(layer_clearance_exemptions[layer_id]);
            clearance_exemption_layers +=
                !layer_clearance_exemptions[layer_id].empty();
            ExPolygons clearance =
                painted_geometry_cache != nullptr &&
                        layer_id < painted_geometry_cache->size() ?
                    (*painted_geometry_cache)[layer_id].final_model_clearance :
                    offset_ex(
                        object.get_layer(int(layer_id))->lslices,
                        float(scale_(gap + 0.5 *
                            support_material_flow(
                                &object, float(object.get_layer(
                                    int(layer_id))->height)).width())),
                        ClipperLib::jtRound, DefaultMiterLimit);
            clearance = diff_ex(
                clearance, layer_clearance_exemptions[layer_id],
                ApplySafetyOffset::Yes);
            // This is the only model-gap subtraction for this entire layer.
            clipped_layer_centers[layer_id] = diff_ex(
                layer_centers[layer_id], clearance);
            ++model_gap_clip_layers;
        }
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-layer-finalization"
            << " layers=" << nonempty_support_layers
            << " model_gap_clips=" << model_gap_clip_layers
            << " clearance_exemption_layers=" << clearance_exemption_layers
            << " projection_exemption_sections=" << projection_exemption_sections;

        for (size_t item_id = 0; item_id < pending.size(); ++item_id) {
            PendingFin &item = pending[item_id];
            if (!item.valid)
                continue;
            const size_t layer_count = prepared[item_id].size();
            item.fin.areas.resize(layer_count);
            item.fin.centers.resize(layer_count);
            item.fin.contact_centers.resize(layer_count);
            item.fin.contact_projection_retained_layers = 0;
            bool finalized = true;
            for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
                const Layer &layer = *object.get_layer(int(layer_id));
                const double width = painted_geometry_cache != nullptr &&
                        layer_id < painted_geometry_cache->size() ?
                    (*painted_geometry_cache)[layer_id].line_width :
                    support_material_flow(&object, float(layer.height)).width();
                item.fin.centers[layer_id] = intersection_ex(
                    clipped_layer_centers[layer_id],
                    prepared[item_id][layer_id].centers);
                item.fin.contact_centers[layer_id] =
                    prepared[item_id][layer_id].contact_centers;
                for (const ExPolygons &projection :
                        prepared[item_id][layer_id].projection_sections)
                    item.fin.contact_projection_retained_layers +=
                        diff_ex(projection, item.fin.centers[layer_id]).empty();
                ExPolygons body = offset_ex(
                    item.fin.centers[layer_id],
                    float(scale_(0.5 * width)),
                    jtRound, double(scale_(0.005)));
                const char *failure_stage = "none";
                size_t failure_value = 0;
                bool section_valid = true;
                const bool multiple_roots = item.fin.intent_root_count > 1;
                const auto valid_section = [](const ExPolygon &section) {
                    return section.holes.empty() && section.contour.is_valid();
                };
                if (item.fin.centers[layer_id].empty() ||
                    (!multiple_roots && item.fin.centers[layer_id].size() != 1) ||
                    !std::all_of(item.fin.centers[layer_id].begin(), item.fin.centers[layer_id].end(), valid_section)) {
                    failure_stage = "center-topology";
                    failure_value = item.fin.centers[layer_id].size();
                    section_valid = false;
                } else if (body.empty() || (!multiple_roots && body.size() != 1) ||
                           !std::all_of(body.begin(), body.end(), valid_section)) {
                    failure_stage = "expanded-topology";
                    failure_value = body.size();
                    section_valid = false;
                } else if (std::any_of(body.begin(), body.end(), [&](const ExPolygon &section) {
                        return offset_ex(ExPolygons{section}, -float(scale_(width - EPSILON))).empty();
                    })) {
                    failure_stage = "thin-body";
                    section_valid = false;
                }
                if (!section_valid) {
                    const size_t failure_island =
                        item.fin.seed.intent_island.value_or(
                            std::numeric_limits<size_t>::max());
                    const size_t failure_edge =
                        item.fin.seed.intent_edge.value_or(
                            std::numeric_limits<size_t>::max());
                    finalization_failures.record(
                        failure_stage, static_cast<long long>(layer_id),
                        layer.print_z, failure_island, failure_edge,
                        std::numeric_limits<size_t>::max(),
                        failure_value);
                    BOOST_LOG_TRIVIAL(info)
                        << "Fin support object=" << object.model_object()->name
                        << " stage=painted-shape-finalize"
                        << " reject=" << failure_stage
                        << " layer=" << layer_id
                        << " z=" << layer.print_z
                        << " island=" << failure_island
                        << " edge=" << failure_edge
                        << " rejection_scope=whole-fin"
                        << " component_size=" << item.fin.intent_component_size
                        << " before_clearance_regions=" << prepared[item_id][layer_id].centers.size()
                        << " after_clearance_regions=" << item.fin.centers[layer_id].size()
                        << " after_clearance_holes=" << std::accumulate(
                            item.fin.centers[layer_id].begin(), item.fin.centers[layer_id].end(), size_t(0),
                            [](size_t count, const ExPolygon &area) { return count + area.holes.size(); });
                    item.fin.diagnostics.reject(
                        FinReject::topology, layer.print_z);
                    finalized = false;
                    break;
                }
                item.fin.areas[layer_id] = std::move(body);
            }
            if (finalized && item.fin.intent_root_count > 1) {
                // Per-layer islands are allowed only for a genuinely multi-root
                // component. Every island must have material below it, and the
                // entire stack must be one connected body after model clearance.
                const auto &layers = item.fin.areas;
                std::vector<size_t> offsets(layers.size() + 1, 0);
                for (size_t layer = 0; layer < layers.size(); ++layer)
                    offsets[layer + 1] = offsets[layer] + layers[layer].size();
                std::vector<std::vector<size_t>> neighbors(offsets.back());
                size_t failure_layer = 0;
                const char *failure_stage = "none";
                for (size_t layer = 1; finalized && layer < layers.size(); ++layer) {
                    if (object.print()->canceled())
                        throw CanceledException();
                    for (size_t upper = 0; upper < layers[layer].size(); ++upper) {
                        const size_t upper_id = offsets[layer] + upper;
                        const BoundingBox upper_bounds = get_extents(layers[layer][upper]);
                        bool supported = false;
                        for (size_t lower = 0; lower < layers[layer - 1].size(); ++lower) {
                            if (!upper_bounds.overlap(get_extents(layers[layer - 1][lower])))
                                continue;
                            const ExPolygons overlap = intersection_ex(
                                ExPolygons{layers[layer][upper]}, ExPolygons{layers[layer - 1][lower]});
                            if (!std::any_of(overlap.begin(), overlap.end(), [](const ExPolygon &area) {
                                    return area.area() * SCALING_FACTOR * SCALING_FACTOR > EPSILON * EPSILON;
                                }))
                                continue;
                            const size_t lower_id = offsets[layer - 1] + lower;
                            neighbors[upper_id].push_back(lower_id);
                            neighbors[lower_id].push_back(upper_id);
                            supported = true;
                        }
                        if (!supported) {
                            finalized = false;
                            failure_stage = "unsupported-section";
                            failure_layer = layer;
                            break;
                        }
                    }
                }
                if (finalized) {
                    std::vector<bool> seen(neighbors.size(), false);
                    std::vector<size_t> connected{0};
                    seen.front() = true;
                    for (size_t i = 0; i < connected.size(); ++i)
                        for (const size_t next : neighbors[connected[i]])
                            if (!seen[next]) {
                                seen[next] = true;
                                connected.push_back(next);
                            }
                    if (connected.size() != neighbors.size()) {
                        finalized = false;
                        failure_stage = "disconnected-stack";
                        const size_t first_missing = size_t(std::distance(seen.begin(),
                            std::find(seen.begin(), seen.end(), false)));
                        failure_layer = size_t(std::upper_bound(offsets.begin(), offsets.end(), first_missing) -
                                               offsets.begin() - 1);
                    }
                }
                if (!finalized) {
                    const double z = object.get_layer(int(failure_layer))->print_z;
                    finalization_failures.record(failure_stage, static_cast<long long>(failure_layer), z,
                        item.fin.seed.intent_island.value_or(std::numeric_limits<size_t>::max()),
                        item.fin.seed.intent_edge.value_or(std::numeric_limits<size_t>::max()));
                    item.fin.diagnostics.reject(FinReject::topology, z);
                    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                        << " stage=painted-shape-finalize reject=" << failure_stage
                        << " layer=" << failure_layer << " z=" << z
                        << " rejection_scope=whole-fin roots=" << item.fin.intent_root_count;
                }
            }
            if (!finalized) {
                item.valid = false;
                diagnostics.merge(item.fin.diagnostics);
                ++rejected_geometry;
                continue;
            }
            cache_fin_bounds(item.fin);
        }
    }

    std::vector<TracedFin> fins;
    fins.reserve(pending.size());
    size_t superseded_count = 0;
    size_t merged_overlap_candidates = 0;
    for (PendingFin &item : pending) {
        if (!item.valid)
            continue;
        if (object.print()->canceled())
            throw CanceledException();
        if (!merge_intersecting && !generate_fin_toolpaths(object, item.fin)) {
            diagnostics.merge(item.fin.diagnostics);
            ++rejected_geometry;
            continue;
        }
        diagnostics.merge(item.fin.diagnostics);
        const FinPlacement placement = evaluate_fin_placement(object, item.fin, fins);
        if (placement.overlap_layer && !merge_intersecting) {
            diagnostics.reject(FinReject::overlap,
                object.get_layer(int(*placement.overlap_layer))->print_z);
            ++rejected_geometry;
            continue;
        }
        merged_overlap_candidates += placement.overlap_layer.has_value() && merge_intersecting;
        if (!merge_intersecting)
            for (auto it = placement.superseded.rbegin(); it != placement.superseded.rend(); ++it) {
                fins.erase(fins.begin() + *it);
                ++superseded_count;
            }
        fins.push_back(std::move(item.fin));
    }
    size_t lower_contacts_accepted = 0;
    size_t suppressed_contact_bands = 0;
    size_t wall_detached_fins = 0;
    size_t axis_limit_detached_fins = 0;
    size_t single_projection_attempts = 0;
    size_t single_projection_layers = 0;
    size_t single_projection_max_layers = 0;
    size_t single_projection_zero_layers = 0;
    size_t single_projection_exposed_layers = 0;
    size_t single_projection_retained_layers = 0;
    size_t composite_projection_attempts = 0;
    size_t composite_projection_layers = 0;
    size_t composite_projection_max_layers = 0;
    size_t composite_projection_zero_layers = 0;
    size_t composite_projection_exposed_layers = 0;
    size_t composite_projection_retained_layers = 0;
    std::array<size_t, size_t(ProjectionStop::count)> composite_projection_stops{};
    std::array<size_t, size_t(ProjectionStop::count)> single_projection_stops{};
    double highest_wall_detach_z = 0.;
    for (const TracedFin &fin : fins) {
        lower_contacts_accepted += fin.accepted_contact_bands -
            std::min(fin.accepted_contact_bands, fin.seed_contact_bands);
        suppressed_contact_bands += fin.suppressed_contact_bands;
        if (fin.intent_component_size > 1) {
            composite_projection_attempts += fin.contact_projection_attempts;
            composite_projection_layers += fin.contact_projection_layers;
            composite_projection_max_layers = std::max(
                composite_projection_max_layers, fin.contact_projection_max_layers);
            composite_projection_zero_layers += fin.contact_projection_zero_layers;
            composite_projection_exposed_layers += fin.contact_projection_exposed_layers;
            composite_projection_retained_layers += fin.contact_projection_retained_layers;
            for (size_t stop = 0; stop < composite_projection_stops.size(); ++stop)
                composite_projection_stops[stop] += fin.contact_projection_stops[stop];
        } else {
            single_projection_attempts += fin.contact_projection_attempts;
            single_projection_layers += fin.contact_projection_layers;
            single_projection_max_layers = std::max(
                single_projection_max_layers, fin.contact_projection_max_layers);
            single_projection_zero_layers += fin.contact_projection_zero_layers;
            single_projection_exposed_layers += fin.contact_projection_exposed_layers;
            single_projection_retained_layers += fin.contact_projection_retained_layers;
            for (size_t stop = 0; stop < single_projection_stops.size(); ++stop)
                single_projection_stops[stop] += fin.contact_projection_stops[stop];
        }
        if (fin.wall_detach_layer.has_value()) {
            ++wall_detached_fins;
            axis_limit_detached_fins += fin.detached_at_axis_limit;
            highest_wall_detach_z = std::max(
                highest_wall_detach_z,
                object.get_layer(int(*fin.wall_detach_layer))->print_z);
        }
    }
    double candidate_top_z = 0.;
    for (const FinCandidateProposal &proposal : proposals)
        candidate_top_z = std::max(candidate_top_z,
            object.get_layer(int(proposal.primary.top_layer))->print_z);
    double emitted_top_z = 0.;
    for (const TracedFin &fin : fins)
        emitted_top_z = std::max(emitted_top_z, object.get_layer(int(fin.seed.top_layer))->print_z);
    FinEmitStats emit_stats;
    if (!fins.empty()) {
        if (merge_intersecting)
            BOOST_LOG_TRIVIAL(debug)
                << "Fin support object=" << object.model_object()->name
                << " stage=painted-shape-emit-begin"
                << " fins=" << fins.size();
        emit_stats = emit_fin_support(object, fins, merge_intersecting);
    }
    std::ostringstream stage_failure_summary;
    bool has_stage_failures = false;
    auto append_stage_failure = [&](const char *name,
                                    const StageFailureSummary &failure,
                                    bool include_parent) {
        if (failure.count == 0)
            return;
        if (has_stage_failures)
            stage_failure_summary << ';';
        stage_failure_summary << name
            << "(count=" << failure.count
            << ",reject=" << failure.reason
            << ",layer=" << failure.layer
            << ",z=" << failure.z
            << ",island=" << failure.island;
        if (include_parent)
            stage_failure_summary << ",parent_edge=" << failure.parent_edge;
        stage_failure_summary << ",edge=" << failure.edge
            << ",value=" << failure.value << ')';
        has_stage_failures = true;
    };
    append_stage_failure("composite-merge", composite_merge_failures, true);
    append_stage_failure("assembly", assembly_failures, false);
    append_stage_failure("finalization", finalization_failures, false);
    if (!has_stage_failures)
        stage_failure_summary << "none";
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=" << stage << " candidates=" << proposals.size() << " fins=" << fins.size()
        << " candidate_top_z=" << candidate_top_z
        << " emitted_top_z=" << emitted_top_z
        << " line_geometry_fallbacks=" << line_geometry_fallbacks
        << " root_intent_fins=" << root_intent_fins
        << " promoted_intent_roots=" << promoted_intent_roots
        << " attached_point_fallbacks=" << attached_point_fallbacks
        << " composite_intent_fins=" << composite_intent_fins
        << " composite_intent_segments=" << composite_intent_segments
        << " continuous_intent_envelopes=" << continuous_intent_envelopes
        << " composite_intent_merge_failures=" << composite_merge_failures.count
        << " assembly_failures=" << assembly_failures.count
        << " finalization_failures=" << finalization_failures.count
        << " stage_failure_samples=" << stage_failure_summary.str()
        << " lower_contacts_accepted=" << lower_contacts_accepted
        << " suppressed_seed_projection=" << suppressed_contact_bands
        << " single_projection_attempts=" << single_projection_attempts
        << " single_projection_layers=" << single_projection_layers
        << " single_projection_max_layers=" << single_projection_max_layers
        << " single_projection_zero_layers=" << single_projection_zero_layers
        << " single_projection_exposed_layers=" << single_projection_exposed_layers
        << " single_projection_retained_layers=" << single_projection_retained_layers
        << " single_projection_bed=" <<
            single_projection_stops[size_t(ProjectionStop::bed)]
        << " single_projection_no_downward=" <<
            single_projection_stops[size_t(ProjectionStop::no_downward)]
        << " single_projection_absorbed=" <<
            single_projection_stops[size_t(ProjectionStop::absorbed)]
        << " single_projection_outside=" <<
            single_projection_stops[size_t(ProjectionStop::outside)]
        << " single_projection_clipped=" <<
            single_projection_stops[size_t(ProjectionStop::clipped)]
        << " composite_projection_attempts=" << composite_projection_attempts
        << " composite_projection_layers=" << composite_projection_layers
        << " composite_projection_max_layers=" << composite_projection_max_layers
        << " composite_projection_zero_layers=" << composite_projection_zero_layers
        << " composite_projection_exposed_layers=" << composite_projection_exposed_layers
        << " composite_projection_retained_layers=" << composite_projection_retained_layers
        << " composite_projection_bed=" <<
            composite_projection_stops[size_t(ProjectionStop::bed)]
        << " composite_projection_no_downward=" <<
            composite_projection_stops[size_t(ProjectionStop::no_downward)]
        << " composite_projection_absorbed=" <<
            composite_projection_stops[size_t(ProjectionStop::absorbed)]
        << " composite_projection_outside=" <<
            composite_projection_stops[size_t(ProjectionStop::outside)]
        << " composite_projection_clipped=" <<
            composite_projection_stops[size_t(ProjectionStop::clipped)]
        << " wall_detached_fins=" << wall_detached_fins
        << " axis_limit_detached_fins=" << axis_limit_detached_fins
        << " highest_wall_detach_z=" << highest_wall_detach_z
        << " superseded=" << superseded_count
        << " merged_overlap_candidates=" << merged_overlap_candidates
        << " merged_outline_failures=" << emit_stats.merged_outline_failures
        << " merged_outline_fallbacks=" << emit_stats.merged_outline_fallbacks
        << " merged_outline_splits=" << emit_stats.merged_outline_splits
        << " rejected_geometry=" << rejected_geometry;
    diagnostics.log(diagnostic_stage, object, merge_intersecting);
    fallback_diagnostics.log("painted-shape-line-fallback", object, merge_intersecting);
}

} // namespace

void generate_fin_support(PrintObject &object)
{
    if (object.layer_count() < 2 || object.has_raft()) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: insufficient layers or raft";
        return;
    }
    const PrintObjectConfig &config = object.config();
    const Layer &first = *object.get_layer(0);
    if (first.lslices.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: empty first-layer footprint";
        return;
    }
    if (config.fin_support_style.value == fssPaintedShape)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-begin";
    const PaintedSides painted_data = painted_sides(object, config.fin_support_style.value == fssPaintedShape);
    if (config.fin_support_style.value == fssPaintedShape)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-painted-sides-complete"
            << " islands=" << painted_data.islands.size();
    const std::vector<PaintedLayer> &painted = painted_data.layers;
    const auto blockers = object.slice_support_blockers();
    const double line_width = support_material_flow(&object).width();
    const Polygon bed = fin_bed_shape(object);
    if (config.fin_support_style.value == fssSimple) {
        const std::vector<FinCandidate> candidates = select_simple_fin_candidates(object, painted);
        std::vector<FinCandidateProposal> proposals;
        proposals.reserve(candidates.size());
        for (const FinCandidate &candidate : candidates)
            proposals.push_back({candidate, std::nullopt});
        generate_candidate_fin_support(object, painted, blockers, bed, line_width, proposals,
                                       "simple", "simple-geometry");
        return;
    }
    if (config.fin_support_style.value == fssPaintedShape) {
        const FinShapeAnalysis shape = analyze_fin_shape(object, line_width);
        BOOST_LOG_TRIVIAL(debug)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-analysis-complete";
        const std::vector<FinCandidateProposal> proposals =
            select_painted_shape_fin_candidates(object, painted_data, shape);
        generate_candidate_fin_support(object, painted, blockers, bed, line_width, proposals,
                                       "painted-shape", "painted-shape-geometry", true);
        return;
    }
    FinDiagnostics diagnostics;

    const FinShapeAnalysis shape = analyze_fin_shape(object, line_width);
    if (shape.base_area <= EPSILON * EPSILON) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: first-layer area=" << shape.base_area;
        return;
    }
    if (!shape.has_base_edges)
        return;
    std::vector<std::vector<PaintedSegment>> runs(painted.size());
    for (size_t i = 1; i < painted.size(); ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        runs[i] = simulated_candidate_runs(painted[i]);
        for (PaintedSegment &run : runs[i]) {
            run.face_center = 0.5 * (run.a + run.b);
            run.face_span = (run.b - run.a).norm();
            // Many short cuts indicate a faceted curved outline; its D-ray
            // ranking needs no full-contour scan for each facet.
            if (runs[i].size() <= 16)
                locate_fin_face(*object.get_layer(int(i)), run, line_width);
        }
    }

    const std::vector<FinShapeLayer> &states = shape.layers;
    const double bed_z = shape.bed_z;
    if (shape.peak_risk <= 0.8) {
        BOOST_LOG_TRIVIAL(info) << "Fin support skipped: peak risk <=0.8";
        return;
    }

    struct CapacityOverride { FinCandidate initial_seed; double fraction; };
    struct TopLimit { FinCandidate initial_seed; size_t layer; };
    auto same_seed = [](const FinCandidate &a, const FinCandidate &b) {
        return a.top_layer == b.top_layer && (a.point - b.point).squaredNorm() < 1e-8 &&
               (a.normal - b.normal).squaredNorm() < 1e-10;
    };
    std::vector<CapacityOverride> capacity_overrides;
    std::vector<TopLimit> top_limits;
    std::vector<TracedFin> fins;
    std::vector<FinCandidate> rejected;
    size_t planning_passes = 0;
    size_t geometry_attempts = 0;
    size_t superseded_count = 0;
    bool retry;
    do {
        ++planning_passes;
        std::vector<FinPlan> plans;
        // One upward traversal builds all candidate histories. Existing plans
        // contribute before another seed is considered on the same layer.
        for (size_t layer_id = 1; layer_id < states.size(); ++layer_id) {
            if (object.print()->canceled())
                throw CanceledException();
            std::vector<FinCandidate> previous_tops;
            previous_tops.reserve(plans.size());
            for (FinPlan &plan : plans) {
                previous_tops.push_back(plan.seed);
                if (plan.active && plan.seed.top_layer + 1 == layer_id) {
                    const auto limit = std::find_if(top_limits.begin(), top_limits.end(), [&](const TopLimit &item) {
                        return same_seed(item.initial_seed, plan.initial_seed);
                    });
                    if ((limit != top_limits.end() && layer_id > limit->layer) ||
                        !extend_fin_plan(object, painted, blockers, runs[layer_id], layer_id,
                                         states[layer_id].risk, plan, diagnostics))
                        plan.active = false;
                }
            }
            // Converging plans share future demand. Stop the later one at its
            // previous valid top while preserving both lower fin histories.
            for (size_t j = 0; j < plans.size(); ++j) {
                if (!plans[j].active || plans[j].seed.top_layer != layer_id ||
                    previous_tops[j].top_layer == layer_id)
                    continue;
                for (size_t k = 0; k < j; ++k)
                    if (plans[k].active && plans[k].seed.top_layer == layer_id &&
                        plans[k].seed.normal.dot(plans[j].seed.normal) >= fin_direction_merge_alignment &&
                        (plans[k].seed.point - plans[j].seed.point).norm() <
                            0.5 * (plans[k].seed.dimensions.thickness + plans[j].seed.dimensions.thickness)) {
                        plans[j].seed = previous_tops[j];
                        plans[j].tops.pop_back();
                        plans[j].active = false;
                        break;
                    }
            }
            std::vector<FinDemand> residual = states[layer_id].demand;
            std::vector<bool> unavailable(residual.size(), false);
            auto deduct_plans = [&] {
                residual = states[layer_id].demand;
                for (size_t j = 0; j < residual.size(); ++j)
                    residual[j].blocked = unavailable[j];
                for (const FinPlan &plan : plans) {
                    double capacity = planned_fin_capacity(object, plan, layer_id, bed_z);
                    for (const CapacityOverride &override : capacity_overrides)
                        if (override.initial_seed.top_layer == plan.initial_seed.top_layer &&
                            (override.initial_seed.point - plan.initial_seed.point).squaredNorm() < 1e-8 &&
                            (override.initial_seed.normal - plan.initial_seed.normal).squaredNorm() < 1e-10) {
                            capacity *= override.fraction;
                            break;
                        }
                    for (FinDemand &need : residual) {
                        const double alignment = std::max(0., plan.seed.normal.dot(need.direction));
                        need.remaining = std::max(0., need.remaining - capacity * alignment * alignment);
                    }
                }
            };
            deduct_plans();
            // Closed lower plans still support their printed heights, but do
            // not consume every slot needed by later shape changes.
            size_t added_on_layer = 0;
            size_t active_count = std::count_if(plans.begin(), plans.end(),
                [](const FinPlan &plan) { return plan.active; });
            while (plans.size() < 32 && active_count < 8 && added_on_layer < 4 && !residual.empty()) {
                size_t best = residual.size();
                double greatest = 0;
                for (size_t j = 0; j < residual.size(); ++j)
                    if (!residual[j].blocked && residual[j].remaining > 0.15 * residual[j].initial &&
                        residual[j].remaining > greatest) {
                        greatest = residual[j].remaining;
                        best = j;
                    }
                if (best == residual.size())
                    break;
                FinCandidate seed;
                if (!select_simulated_fin_candidate(object, painted, blockers, runs[layer_id], layer_id,
                        states[layer_id].center, residual[best].direction, states[layer_id].risk,
                        plans, rejected, diagnostics, seed)) {
                    unavailable[best] = true;
                    residual[best].blocked = true;
                    continue;
                }
                plans.push_back({seed, seed, true, seed.point.dot(seed.normal), {seed}});
                ++active_count;
                ++added_on_layer;
                deduct_plans();
            }
        }
        fins.clear();
        superseded_count = 0;
        std::vector<size_t> accepted_plan_indices;
        retry = false;
        for (size_t plan_id = 0; plan_id < plans.size(); ++plan_id) {
            const FinPlan &plan = plans[plan_id];
            ++geometry_attempts;
            TracedFin fin;
            const bool geometry_ok = realize_fin_candidate(
                object, plan.seed, bed, painted, blockers, line_width, fin);
            const FinPlacement placement = geometry_ok ?
                evaluate_fin_placement(object, fin, fins) : FinPlacement{};
            const bool overlapping = placement.overlap_layer.has_value();
            if (!geometry_ok || overlapping) {
                diagnostics.merge(fin.diagnostics);
                if (overlapping)
                    diagnostics.reject(FinReject::overlap,
                        object.get_layer(int(*placement.overlap_layer))->print_z);
                // The final top may fail while the original contact remains
                // printable. Keep the highest printable earlier top and let
                // the next upward sweep fill the demand it no longer covers.
                bool earlier_top_valid = false;
                if (plan.tops.size() > 1) {
                    auto valid_top = [&](const FinCandidate &candidate) {
                        ++geometry_attempts;
                        TracedFin shorter;
                        if (!realize_fin_candidate(
                                object, candidate, bed, painted, blockers, line_width, shorter))
                            return false;
                        return !evaluate_fin_placement(object, shorter, fins).overlap_layer;
                    };
                    if (valid_top(plan.tops.front())) {
                        // Most extension failures form an upper invalid range.
                        // Bracket a valid top exponentially, then refine; the
                        // exceptional retry must not rebuild every lower layer.
                        size_t lower = 0, upper = plan.tops.size() - 1, step = 1;
                        while (upper > 1) {
                            const size_t probe = upper > step ? upper - step : 0;
                            if (valid_top(plan.tops[probe])) {
                                lower = probe;
                                break;
                            }
                            upper = probe;
                            step = std::min(step * 2, plan.tops.size());
                        }
                        while (upper > lower + 1) {
                            const size_t middle = lower + (upper - lower) / 2;
                            if (valid_top(plan.tops[middle]))
                                lower = middle;
                            else
                                upper = middle;
                        }
                        auto limit = std::find_if(top_limits.begin(), top_limits.end(), [&](const TopLimit &item) {
                            return same_seed(item.initial_seed, plan.initial_seed);
                        });
                        if (limit == top_limits.end())
                            top_limits.push_back({plan.initial_seed, plan.tops[lower].top_layer});
                        else
                            limit->layer = std::min(limit->layer, plan.tops[lower].top_layer);
                        earlier_top_valid = true;
                    }
                }
                if (!earlier_top_valid)
                    rejected.push_back(plan.initial_seed);
                retry = true;
                // Collect all failed plans in this sweep before replanning.
                // Earlier fins remain available for overlap checks below.
                continue;
            }
            diagnostics.merge(fin.diagnostics);
            for (auto it = placement.superseded.rbegin(); it != placement.superseded.rend(); ++it) {
                fins.erase(fins.begin() + *it);
                accepted_plan_indices.erase(accepted_plan_indices.begin() + *it);
                ++superseded_count;
            }
            fins.push_back(std::move(fin));
            accepted_plan_indices.push_back(plan_id);
        }
        if (retry)
            continue;
        // Band validity is known only after geometry. Correct the optimistic
        // planning score and rerun the inexpensive upward sweep when needed.
        for (size_t i = 0; i < fins.size(); ++i) {
            const double fraction = double(fins[i].accepted_contact_bands) /
                                    std::max<size_t>(fins[i].planned_contact_bands, 1);
            auto found = std::find_if(capacity_overrides.begin(), capacity_overrides.end(), [&](const CapacityOverride &item) {
                return item.initial_seed.top_layer == plans[accepted_plan_indices[i]].initial_seed.top_layer &&
                       (item.initial_seed.point - plans[accepted_plan_indices[i]].initial_seed.point).squaredNorm() < 1e-8 &&
                       (item.initial_seed.normal - plans[accepted_plan_indices[i]].initial_seed.normal).squaredNorm() < 1e-10;
            });
            if (found == capacity_overrides.end()) {
                if (fraction < 0.98) {
                    capacity_overrides.push_back({plans[accepted_plan_indices[i]].initial_seed, fraction});
                    retry = true;
                }
            } else if (fraction + 0.02 < found->fraction) {
                found->fraction = fraction;
                retry = true;
            }
        }
        if (retry)
            continue;
        for (size_t i = 0; i < fins.size(); ++i)
            if (!generate_fin_toolpaths(object, fins[i])) {
                diagnostics.reject(FinReject::no_outline,
                    fins[i].diagnostics.failures[size_t(FinReject::no_outline)].first_z);
                rejected.push_back(plans[accepted_plan_indices[i]].initial_seed);
                retry = true;
                break;
            }
    } while (retry);

    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                            << " accepted=" << fins.size() << " planning_passes=" << planning_passes
                            << " geometry_attempts=" << geometry_attempts
                            << " superseded=" << superseded_count;
    diagnostics.log("all-attempts", object);
    for (size_t i = 0; i < fins.size(); ++i) {
        const TracedFin &fin = fins[i];
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name << " selected=" << i << " seed=(" << fin.seed.point.x() << ','
            << fin.seed.point.y() << ',' << object.get_layer(int(fin.seed.top_layer))->print_z << ") A="
            << std::atan2(fin.seed.normal.y(), fin.seed.normal.x()) * 180 / M_PI << " bands="
            << fin.accepted_contact_bands << '/' << fin.planned_contact_bands
            << " suppressed_seed_projection=" << fin.suppressed_contact_bands;
        fin.diagnostics.log("selected-fin", object);
    }

    emit_fin_support(object, fins);
}

} // namespace Slic3r
