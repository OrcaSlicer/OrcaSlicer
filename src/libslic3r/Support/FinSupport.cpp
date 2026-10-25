#include "FinSupport.hpp"
#include "SupportCommon.hpp"

#include <boost/log/trivial.hpp>

#include "../Fill/Fill.hpp"
#include "../AABBTreeLines.hpp"
#include "../AABBTreeIndirect.hpp"
#include "../AABBMesh.hpp"
#include "../CSGMesh/ModelToCSGMesh.hpp"
#include "../CSGMesh/PerformCSGMeshBooleans.hpp"
#include "../Flow.hpp"
#include "../Geometry.hpp"
#include "../Geometry/ArcWelder.hpp"
#include "../Geometry/ConvexHull.hpp"
#include "../Geometry/MedialAxis.hpp"
#include "../Layer.hpp"
#include "../Line.hpp"
#include "../Model.hpp"
#include "../MutablePolygon.hpp"
#include "../NormalUtils.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../SVG.hpp"
#include "../TriangleMesh.hpp"
#include "../TriangleMeshSlicer.hpp"
#include "../Utils.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Slic3r {
FinSupportDimensions fin_support_dimensions(const PrintObject &object)
{
    const double reference = support_material_flow(&object).width();
    return {object.config().fin_support_contact_width.get_abs_value(reference),
            object.config().fin_support_thickness.get_abs_value(reference)};
}

namespace {

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

struct PaintedIntentSurfacePoint {
    Vec3d point = Vec3d::Zero();
    size_t face = std::numeric_limits<size_t>::max();
};

struct PaintedIntentSource {
    size_t patch = 0;
    size_t chart = 0;
    Point point;
};

// A standalone blue point has no medial-axis width measurement or line direction.
struct PaintedIntentPoint {
    Vec2d chart = Vec2d::Zero();
    PaintedIntentSurfacePoint surface;
    PaintedIntentSource source;
    Vec3d macro_normal = Vec3d::Zero();
    double area = 0;
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
    bool starts_at_root = false;
    // Endpoint nodes first, followed by any junctions inside the finite segment.
    std::vector<size_t> nodes;
    // Head attachment host (or tail when there is no head attachment), used by
    // normal correction and sibling provenance, never generation ownership.
    // Seed ownership is independent. All connections, including tail merges,
    // are represented by nodes and their attachment hosts.
    std::optional<size_t> attachment_host_edge;
};

struct PaintedIntentNode {
    Vec2d point = Vec2d::Zero();
    double surface_z = 0;
    std::vector<size_t> edges;
    std::optional<PaintedIntentSource> source;
    std::optional<size_t> attachment_host_edge;
};

struct PaintedCreaseIntent {
    size_t crease, pair, part;
    // Immutable source geometry, with one vertex for a point intent.
    std::vector<Vec3d> points;
    // Contact source is independent of point/ray versus finite body classification.
    std::vector<Vec3d> contact_source;
    std::array<Vec3d, 2> root_normals;
    // Independent nodes: no geometric welding to ordinary or other crease paths.
    std::vector<size_t> nodes;

    // Query the complete intent, never a generated root stem or a gap-clipped
    // XY fragment. Shared-vertex hits are one point; no extrapolation or choice
    // among distinct intersections is made here.
    struct Position {
        size_t vertex;
        double fraction;
        Vec2d contact;
    };

    std::optional<Position> position_at_z(double z) const
    {
        std::optional<Position> hit;
        auto include = [&](size_t vertex, double fraction, const Vec2d &xy) {
            if (hit && (hit->contact - xy).norm() > EPSILON)
                return false;
            if (!hit)
                hit = Position{vertex, fraction, xy};
            return true;
        };
        for (size_t i = 0; i < contact_source.size(); ++i)
            if (std::abs(contact_source[i].z() - z) <= EPSILON && !include(i, 0., contact_source[i].head<2>()))
                return std::nullopt;
        for (size_t i = 1; i < contact_source.size(); ++i) {
            const Vec3d &a = contact_source[i - 1], &b = contact_source[i];
            // Endpoints, including a coplanar segment, were handled above.
            if (std::abs(a.z() - z) <= EPSILON || std::abs(b.z() - z) <= EPSILON ||
                z <= std::min(a.z(), b.z()) || z >= std::max(a.z(), b.z()))
                continue;
            const double fraction = (z - a.z()) / (b.z() - a.z());
            if (!include(i - 1, fraction, Linef3(a, b).intersect_plane(z).head<2>()))
                return std::nullopt;
        }
        return hit;
    }
};

struct PaintedIntentGraph {
    Vec3d macro_normal = Vec3d::Zero();
    std::vector<PaintedIntentNode> nodes;
    std::vector<PaintedIntentStroke> strokes;
    std::vector<PaintedIntentPoint> points;
    std::vector<std::shared_ptr<const PaintedCreaseIntent>> creases;
};

struct PaintedIsland {
    double area = 0;
    PaintedIntentGraph intent;
};

struct PaintedSides {
    size_t crease_run = 0;
    std::vector<PaintedLayer> layers;
    std::vector<PaintedIsland> islands;
    size_t persistent_crease_edges = 0;
    size_t undersized_painted_islands = 0;
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
    size_t compact_intent_components = 0;
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

enum class FinGeometryMode {
    projected,
    painted_shape
};

struct FinCandidate {
    // Resolved finite geometry. Ordinary green nodes use resolved surface heights;
    // direct crease inputs retain their original source-surface heights.
    struct Path {
        // Ordered whole generation path. Interior junctions do not split it.
        std::vector<Vec3d> points;
        bool start_junction = false;
        bool end_junction = false;
        size_t source_edge = 0;
    };
    Vec2d point;
    Vec2d normal; // Ray direction, or representative orientation for a finite path.
    Vec2d contact_normal; // B: direction from the painted wall to the sheet.
    size_t top_layer;
    FinSupportDimensions dimensions;
    double expansion_slope;
    FinSurfaceNormalPolicy surface_normals = FinSurfaceNormalPolicy::local_non_upward;
    // Geometry construction is independent of brush-mask authorization.
    FinGeometryMode geometry_mode = FinGeometryMode::projected;
    bool requires_painted_contact = true;
    Vec3d macro_normal = Vec3d::Zero();
    // Source painted-island identity for candidate ownership and overlap
    // comparisons. Contact authorization is selected independently above.
    std::optional<size_t> wall_follow_island;
    // Resolved XY sheet junctions, independent of the source stroke endpoints
    // and their Z range. Correction may split one source junction into several.
    struct IntentNode {
        size_t node = std::numeric_limits<size_t>::max();
        Vec2d point = Vec2d::Zero();
        std::optional<size_t> attachment_host_edge;
        double source_z = 0.;
        double z = 0.; // Resolved shared-node height, including interior junctions.
    };
    struct IntentRoot {
        size_t node;
        Vec2d point;
        Vec2d extension_direction = Vec2d::Zero();
        double z = 0.;
    };
    std::optional<size_t> intent_island;
    std::optional<size_t> intent_edge;
    std::vector<IntentNode> intent_nodes;
    std::optional<IntentRoot> intent_root;
    bool intent_line = false;
    // Immutable attachment provenance for sibling cavity filling, even after a
    // corrected junction splits or its connection is detached.
    std::optional<std::pair<size_t, size_t>> source_attachment_junction; // Host edge, node.
    std::optional<Path> path;
    // The complete immutable 3D intent, shared with its producer and root stem.
    // A member's local path can be a stem; it must not replace this geometry.
    std::shared_ptr<const PaintedCreaseIntent> crease_intent;
};

struct FinCandidateProposal {
    FinCandidate primary;
};

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
    std::optional<Line> center_cap; // Owned terminal cap, quantized once before contact construction.
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
    std::vector<Vec2d> boundary; // Resolved wall-bead contact after geometry preparation.
    std::vector<std::optional<Vec2d>> contact_sources; // Valid model/crease anchors, separate from wall-bead hits.
    // Painted Shape resolves the finite contact axis independently from the
    // fin plane. All directions point outward from the model.
    std::vector<Vec2d> contact_directions; // Resolved contact-to-A1 axis; ordinary contacts also query along it.
    std::vector<Vec2d> contact_front_normals; // Front orientation; crease contacts use the source section normal.
    std::vector<Vec2d> body_cap_normals;
    // One quantization of each member's printable-center attachment. Body,
    // contact and taper share these endpoints; compound assembly cannot recut them.
    std::vector<std::optional<Line>> center_caps;
    std::vector<bool> wall_present; // Printed wall-bead hits only, populated by geometry preparation.
    // Empty only for the legacy ray-generated styles. Fin-path groups may
    // have no body on an individual layer before component lifetime is solved.
    std::vector<bool> path_present;
    std::vector<Vec2d> body_start;
    std::vector<Vec2d> tips;
    // Start-preserving clipped paths in source order, before thickening.
    // Source Z stays on seed.path; all points here belong to one slice.
    std::vector<Polyline> layer_paths;
    std::vector<bool> contacts;
    // Each local path segment retains its direction, cap and group ownership.
    // The ordered member path includes root depth before offsetting. Only its
    // actual outer and inner endpoints are capped; internal vertices stay open.
    std::vector<std::vector<FinBodyAxis>> body_axes;
    // Open perimeter from one contact attachment to the other, via the root
    // flat end. This is the boundary used for final contact assembly; normalized
    // body regions are separate query snapshots, never extra output bodies.
    std::vector<Polyline> body_boundaries;
    // Actual contact regions retain semantic ownership separately from the
    // completed center contours used for geometry and toolpaths.
    std::vector<ExPolygons> contact_centers;
    std::vector<ExPolygons> areas;
    BoundingBoxes area_bounds;
    // Legacy styles keep finalized per-fin centers. Painted Shape keeps
    // pre-clearance source contours for attribution only; its common clipped
    // layer centers are passed separately to the output stage.
    std::vector<ExPolygons> centers;
    std::vector<Polylines> outlines;
};

Point scaled_point(const Vec2d &p) { return Point::new_scale(p.x(), p.y()); }

// Failure-only snapshots. Callers supply actual intermediate geometry, not
// reconstructed/re-solved substitutes. Keep this local to Fin Support.
struct FinGeometryDebugSet {
    std::string label;
    ExPolygons regions;
    Polylines paths;
    Points points;
};

void export_fin_geometry_failure(const PrintObject &object, const TracedFin &fin,
                                 size_t layer_id, const char *stage, const char *reason,
                                 std::vector<FinGeometryDebugSet> sets)
{
    static std::atomic_size_t sequence{0};
    const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const std::string filename = debug_out_path("fin-support/geometry-failure-%lld-%llu.svg",
        static_cast<long long>(stamp), static_cast<unsigned long long>(sequence.fetch_add(1)));
    const bool has_layer = layer_id < object.layer_count();
    std::ostringstream context;
    context.precision(17);
    context << "failure_stage=" << stage << " reason=" << reason
            << " layer=" << (has_layer ? static_cast<long long>(layer_id) : -1)
            << " island=" << fin.seed.intent_island.value_or(std::numeric_limits<size_t>::max())
            << " edge=" << fin.seed.intent_edge.value_or(std::numeric_limits<size_t>::max())
            << " roots=" << fin.intent_root_count
            << " thickness=" << fin.seed.dimensions.thickness
            << " contact_width=" << fin.seed.dimensions.contact_width
            << " gap=" << object.config().support_object_xy_distance.value;
    if (has_layer) {
        const Layer &layer = *object.get_layer(int(layer_id));
        context << " slice_z=" << layer.slice_z << " print_z=" << layer.print_z;
        sets.insert(sets.begin(), {"model at slice_z", layer.lslices, {}, {}});
    }
    if (fin.seed.crease_intent)
        context << " crease=" << fin.seed.crease_intent->crease
                << " pair=" << fin.seed.crease_intent->pair << " part=" << fin.seed.crease_intent->part;
    if (fin.seed.path) {
        Polyline input;
        for (const Vec3d &point : fin.seed.path->points)
            input.points.push_back(scaled_point(point.head<2>()));
        sets.push_back({"input path XY (all source heights)", {}, {std::move(input)}, {}});
    } else {
        sets.push_back({"point intent", {}, {}, {scaled_point(fin.seed.point)}});
    }
    if (layer_id < fin.layer_paths.size())
        sets.push_back({"retained path after gap", {}, {fin.layer_paths[layer_id]}, {}});
    if (layer_id < fin.path_present.size() && fin.path_present[layer_id] && layer_id < fin.body_start.size())
        sets.push_back({"A1", {}, {}, {scaled_point(fin.body_start[layer_id])}});
    if (layer_id < fin.wall_present.size() && fin.wall_present[layer_id] && layer_id < fin.boundary.size())
        sets.push_back({"actual contact point",
                        {}, {}, {scaled_point(fin.boundary[layer_id])}});
    if (layer_id < fin.contact_sources.size() && fin.contact_sources[layer_id])
        sets.push_back({"contact-axis source on model", {}, {}, {scaled_point(*fin.contact_sources[layer_id])}});
    if (layer_id < fin.wall_present.size() && fin.wall_present[layer_id] &&
        layer_id < fin.contact_directions.size() && layer_id < fin.contact_front_normals.size()) {
        const Vec2d &wall = fin.boundary[layer_id];
        sets.push_back({"contact axis (unit outward)", {},
            {Polyline{scaled_point(wall), scaled_point(wall + fin.contact_directions[layer_id])}}, {}});
        sets.push_back({"contact front normal (unit outward)", {},
            {Polyline{scaled_point(wall), scaled_point(wall + fin.contact_front_normals[layer_id])}}, {}});
    }
    if (layer_id < fin.body_boundaries.size())
        sets.push_back({"owned open member boundary", {}, {fin.body_boundaries[layer_id]}, {}});
    if (layer_id < fin.body_axes.size()) {
        FinGeometryDebugSet members{"member axes including root depth", {}, {}, {}};
        for (const auto &axis : fin.body_axes[layer_id])
            members.paths.push_back(
                Polyline{scaled_point(axis.outer), scaled_point(axis.inner)});
        sets.push_back(std::move(members));
    }
    BoundingBox bounds;
    for (const auto &set : sets) {
        for (const auto &region : set.regions) {
            bounds.merge(region.contour.points);
            for (const auto &hole : region.holes)
                bounds.merge(hole.points);
        }
        for (const auto &path : set.paths)
            bounds.merge(path.points);
        bounds.merge(set.points);
    }
    Vec2d size = Vec2d::Zero();
    if (bounds.defined)
        size = unscale(Point(bounds.max - bounds.min));
    const double panel_width = std::max(65., size.x() + 8.);
    const double panel_height = std::max(30., size.y() + 12.);
    const size_t panels = sets.size() + 1, columns = std::min(size_t(3), panels);
    const size_t rows = (panels + columns - 1) / columns;
    SVG svg(filename, BoundingBox(Point(0, 0),
        scaled_point(Vec2d(columns * panel_width, rows * panel_height + 8.))), scale_(1.));
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=geometry-failure-dump " << context.str() << " svg=" << filename
        << " opened=" << svg.is_opened();
    if (!svg.is_opened())
        return;
    svg.add_comment(context.str());
    if (fin.seed.path) {
        std::ostringstream source;
        source.precision(17);
        source << "input_xyz_mm=";
        for (const Vec3d &point : fin.seed.path->points)
            source << point.x() << "," << point.y() << "," << point.z() << ";";
        svg.add_comment(source.str());
    }
    svg.add_comment("Panels share scale and world bounds. raw_points are original scaled integer XY, not panel coordinates.");
    const std::array<const char *, 8> colors{{"#777777", "#009e73", "#e69f00", "#0072b2",
                                           "#cc79a7", "#d55e00", "#56b4e9", "#202020"}};
    auto offset = [&](size_t panel) {
        return Point(scaled_point(Vec2d((panel % columns) * panel_width + 4.,
            (panel / columns) * panel_height + 4.)) - bounds.min);
    };
    auto raw = [&](size_t set, const std::string &kind, const Points &points) {
        std::ostringstream record;
        record << "set=" << set << " kind=" << kind << " raw_points=";
        for (const Point &point : points)
            record << point.x() << "," << point.y() << ";";
        svg.add_comment(record.str());
    };
    auto draw = [&](const FinGeometryDebugSet &set, size_t panel, const char *color, bool labels) {
        const Point shift = offset(panel);
        for (size_t r = 0; r < set.regions.size(); ++r) {
            ExPolygon region = set.regions[r];
            region.translate(shift);
            svg.draw(region, color, 0.15f);
            svg.draw_outline(region, color, color, scale_(0.06));
            if (labels && !region.contour.points.empty())
                svg.draw_text(region.contour.points.front(), ("R" + std::to_string(r)).c_str(), color, 1);
        }
        for (Polyline path : set.paths) {
            path.translate(shift);
            if (path.points.size() >= 2)
                svg.draw(path, color, scale_(0.1));
        }
        for (const Point &point : set.points)
            svg.draw(Point(point + shift), color, scale_(0.15));
    };
    svg.draw_text(scaled_point(Vec2d(2., panel_height - 3.)), "OVERLAY (see numbered panels)", "#202020", 1);
    for (size_t i = 0; i < sets.size(); ++i) {
        const auto &set = sets[i];
        const char *color = colors[i % colors.size()];
        const size_t holes = std::accumulate(set.regions.begin(), set.regions.end(), size_t(0),
            [](size_t n, const ExPolygon &region) { return n + region.holes.size(); });
        const std::string legend = std::to_string(i) + " " + set.label + " regions=" +
            std::to_string(set.regions.size()) + " holes=" + std::to_string(holes);
        svg.add_comment(legend);
        svg.draw_text(scaled_point(Vec2d(((i + 1) % columns) * panel_width + 2.,
            ((i + 1) / columns + 1) * panel_height - 3.)), legend.c_str(), color, 1);
        draw(set, 0, color, false);
        draw(set, i + 1, color, true);
        for (size_t r = 0; r < set.regions.size(); ++r) {
            raw(i, "region-" + std::to_string(r), set.regions[r].contour.points);
            for (size_t h = 0; h < set.regions[r].holes.size(); ++h)
                raw(i, "region-" + std::to_string(r) + "-hole-" + std::to_string(h), set.regions[r].holes[h].points);
        }
        for (size_t q = 0; q < set.paths.size(); ++q)
            raw(i, "path-" + std::to_string(q), set.paths[q].points);
        raw(i, "points", set.points);
    }
    svg.draw_text(scaled_point(Vec2d(2., rows * panel_height + 4.)), context.str().c_str(), "#202020", 1);
}

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
constexpr double max_fin_vertical_point_normal_z = 0.17364817766693034885; // cos(80 degrees).
constexpr double fin_crease_alignment = 0.70710678118654752440; // cos(45 degrees).
constexpr double fin_intent_fragment_alignment = 0.86602540378443864676; // cos(30 degrees).
constexpr double fin_intent_strong_continuation_alignment = 0.86602540378443864676; // cos(30 degrees).
constexpr double fin_intent_weak_continuation_alignment = 0.5; // cos(60 degrees).
constexpr double fin_intent_continuation_margin = 0.1;
constexpr double fin_intent_coarsening_width_ratio = 1.;
constexpr double fin_blue_group_alignment = 0.96592582628906828675; // cos(15 degrees).

// Diagnostic-only projection of a persistent crease. Segment metadata keeps
// the two adjacent source faces identifiable across separate patch SVGs.
struct PaintedCreaseProbePath {
    size_t id;
    std::vector<Vec2d> points;
    struct Segment {
        std::array<size_t, 2> faces;
        unsigned patch_sides; // Bit 0/1: adjacent face belongs to this patch.
        bool convex;
        std::array<Vec3d, 2> normals;
    };
    std::vector<Segment> segments;
    std::vector<Vec3d> surface_points;
    std::vector<double> surface_lengths; // Cumulative source-crease arc length.
};

struct PaintedCreaseProbe {
    size_t path = std::numeric_limits<size_t>::max();
    size_t segment = 0;
    Vec2d foot = Vec2d::Zero();
    Vec2d average = Vec2d::Zero();
    Vec3d surface_foot = Vec3d::Zero();
    double position = 0.; // Arc length on the original 3D crease, not chart XY.
    std::vector<Vec2d> window;
    double distance = 0.;
    double sampled = 0.;
    std::optional<double> angle;
    bool direction_pass = false;
    const char *result = "no-perpendicular-foot";
    const char *window_stop = "not-sampled";
};

PaintedCreaseProbe probe_painted_crease(const PaintedIntentSample &sample, const Vec2d &towards,
                                      const std::vector<PaintedCreaseProbePath> &creases)
{
    PaintedCreaseProbe probe;
    double nearest = std::numeric_limits<double>::infinity();
    for (size_t path = 0; path < creases.size(); ++path) {
        const auto &crease = creases[path];
        for (size_t segment = 0; segment < crease.segments.size(); ++segment) {
            const auto &source = crease.segments[segment];
            if (!source.convex || source.patch_sides == 0)
                continue;
            const Vec2d delta = crease.points[segment + 1] - crease.points[segment];
            const double length_squared = delta.squaredNorm();
            if (length_squared <= EPSILON * EPSILON)
                continue;
            const double t = (sample.chart - crease.points[segment]).dot(delta) / length_squared;
            // Do not clamp an exterior projection to an endpoint.
            if (t < 0. || t > 1.)
                continue;
            const Vec2d foot = crease.points[segment] + t * delta;
            const double distance_squared = (sample.chart - foot).squaredNorm();
            // Exact ties retain source traversal order.
            if (distance_squared >= nearest)
                continue;
            nearest = distance_squared;
            probe.path = path;
            probe.segment = segment;
            probe.foot = foot;
            probe.surface_foot = crease.surface_points[segment] +
                t * (crease.surface_points[segment + 1] - crease.surface_points[segment]);
            probe.position = crease.surface_lengths[segment] +
                t * (crease.surface_lengths[segment + 1] - crease.surface_lengths[segment]);
        }
    }
    if (!std::isfinite(nearest))
        return probe;
    probe.distance = std::sqrt(nearest);
    const auto &crease = creases[probe.path];
    const Vec2d blue = towards - sample.chart;
    const Vec2d local = crease.points[probe.segment + 1] - crease.points[probe.segment];
    const double alignment = blue.dot(local);
    if (blue.norm() <= EPSILON || alignment == 0.) {
        probe.result = "no-traversal-direction";
        return probe;
    }
    const bool forward = alignment > 0.;
    double remaining = 0.5 * sample.width;
    if (remaining <= 0.) {
        probe.result = "zero-width";
        return probe;
    }
    size_t segment = probe.segment;
    Vec2d position = probe.foot;
    Vec2d weighted_direction = Vec2d::Zero();
    probe.window.push_back(position);
    probe.window_stop = "radius";
    while (remaining > 0.) {
        const auto &source = crease.segments[segment];
        if (!source.convex || source.patch_sides == 0) {
            probe.window_stop = source.convex ? "patch-boundary" : "non-convex";
            break;
        }
        const Vec2d delta = crease.points[forward ? segment + 1 : segment] - position;
        const double length = delta.norm();
        if (length > 0.) {
            const double taken = std::min(remaining, length);
            const Vec2d displacement = (taken / length) * delta;
            weighted_direction += displacement; // Length times unit segment direction.
            position += displacement;
            probe.sampled += taken;
            probe.window.push_back(position);
            if (taken == remaining)
                break;
            remaining -= taken;
        }
        if ((forward && segment + 1 == crease.segments.size()) || (!forward && segment == 0)) {
            probe.window_stop = "crease-end";
            break;
        }
        segment = forward ? segment + 1 : segment - 1;
    }
    if (probe.sampled <= EPSILON || weighted_direction.norm() <= EPSILON) {
        probe.result = probe.sampled <= EPSILON ? "empty-window" : "cancelled-directions";
        return probe;
    }
    probe.average = weighted_direction.normalized();
    const double cosine = std::clamp(blue.normalized().dot(probe.average), -1., 1.);
    probe.angle = std::acos(cosine) * 180. / M_PI;
    probe.direction_pass = cosine >= fin_blue_group_alignment;
    probe.result = probe.direction_pass ? "direction-pass" : "angle-over-15";
    return probe;
}

struct PaintedCreaseSelection {
    struct Node {
        PaintedIntentSample sample;
        size_t path;
        size_t point;
        std::vector<size_t> directions;
        bool selected = false;
        const char *reason = "no-qualified-direction";
    };
    struct Direction {
        size_t node;
        size_t neighbor;
        PaintedCreaseProbe probe;
        size_t junction_rank = 0; // Zero for non-junctions; otherwise one-based.
        bool branch_selected = true;
    };
    struct JunctionRecheck {
        size_t junction;
        std::array<size_t, 2> ends;
        std::array<PaintedCreaseProbe, 2> probes;
        bool accepted = false;
    };
    std::vector<Node> nodes;
    std::vector<Direction> directions; // Same path/point/prev-next order as the SVG probes.
    std::vector<JunctionRecheck> rechecks;
    std::vector<std::vector<size_t>> chains; // Selected node indices, ordered on each 3D crease.
};

PaintedCreaseSelection select_painted_crease_points(
    const PrintObject &object,
    const std::vector<std::vector<PaintedIntentSample>> &blue_paths,
    const std::vector<PaintedCreaseProbePath> &creases)
{
    PaintedCreaseSelection selection;
    // This call owns one source patch/chart. Merge only exact source vertices,
    // never nearby points or coincident world-XY projections.
    std::unordered_map<Point, size_t, PointHash> node_by_source;
    std::vector<std::vector<size_t>> path_nodes(blue_paths.size());
    for (size_t path = 0; path < blue_paths.size(); ++path)
        for (size_t point = 0; point < blue_paths[path].size(); ++point) {
            const auto &sample = blue_paths[path][point];
            const auto [it, inserted] = node_by_source.emplace(sample.source.point, selection.nodes.size());
            if (inserted)
                selection.nodes.push_back({sample, path, point, {}});
            else
                selection.nodes[it->second].sample.junction |= sample.junction;
            path_nodes[path].push_back(it->second);
        }
    for (size_t path = 0; path < blue_paths.size(); ++path) {
        const auto &samples = blue_paths[path];
        for (size_t point = 0; point < samples.size(); ++point) {
            if (object.print()->canceled())
                throw CanceledException();
            for (int step : {-1, 1}) {
                if ((step < 0 && point == 0) || (step > 0 && point + 1 == samples.size()))
                    continue;
                const size_t neighbor = step < 0 ? point - 1 : point + 1;
                const size_t node = path_nodes[path][point];
                selection.nodes[node].directions.push_back(selection.directions.size());
                selection.directions.push_back({node, path_nodes[path][neighbor],
                    probe_painted_crease(samples[point], samples[neighbor].chart, creases)});
            }
        }
    }
    for (auto &node : selection.nodes) {
        if (!node.sample.junction)
            continue;
        std::vector<size_t> ranked = node.directions;
        std::stable_sort(ranked.begin(), ranked.end(), [&](size_t a, size_t b) {
            return selection.directions[a].probe.angle.value_or(std::numeric_limits<double>::infinity()) <
                   selection.directions[b].probe.angle.value_or(std::numeric_limits<double>::infinity());
        });
        for (size_t rank = 0; rank < ranked.size(); ++rank) {
            auto &direction = selection.directions[ranked[rank]];
            direction.junction_rank = rank + 1;
            direction.branch_selected = rank < 2;
        }
    }
    for (size_t node_id = 0; node_id < selection.nodes.size(); ++node_id) {
        auto &node = selection.nodes[node_id];
        size_t qualified = 0;
        for (const size_t direction : node.directions) {
            const auto &entry = selection.directions[direction];
            qualified += entry.branch_selected && entry.probe.direction_pass;
        }
        // Ordinary points are supplied by qualified directions. A source
        // junction must qualify through both of its two lowest-angle branches.
        node.selected = qualified >= (node.sample.junction ? 2u : 1u);
        node.reason = node.selected ? "selected" :
            node.sample.junction ? "junction-pair-rejected" : "no-qualified-direction";
    }
    // Recheck only rejected junctions, using the original branch ranking.
    // Accepted chords supply ordinary endpoints without changing the source graph
    // or letting a rescued endpoint change another junction's ranking.
    for (size_t node_id = 0; node_id < selection.nodes.size(); ++node_id) {
        auto &node = selection.nodes[node_id];
        if (!node.sample.junction || node.selected || node.directions.size() < 2)
            continue;
        if (object.print()->canceled())
            throw CanceledException();
        std::array<size_t, 2> ends{};
        for (const size_t direction : node.directions) {
            const auto &entry = selection.directions[direction];
            if (entry.junction_rank >= 1 && entry.junction_rank <= 2)
                ends[entry.junction_rank - 1] = entry.neighbor;
        }
        auto &a = selection.nodes[ends[0]];
        auto &b = selection.nodes[ends[1]];
        // Adjacent junctions are intentionally not bypassed recursively. Only
        // two ordinary outer points participate in this local recheck.
        if (a.sample.junction || b.sample.junction || ends[0] == ends[1])
            continue;
        PaintedCreaseSelection::JunctionRecheck recheck{node_id, ends,
            {probe_painted_crease(a.sample, b.sample.chart, creases),
             probe_painted_crease(b.sample, a.sample.chart, creases)}};
        const size_t crease = selection.directions[node.directions.front()].probe.path;
        recheck.accepted = recheck.probes[0].direction_pass && recheck.probes[1].direction_pass &&
            recheck.probes[0].path == crease && recheck.probes[1].path == crease;
        if (recheck.accepted) {
            node.reason = "junction-bypassed";
            for (const size_t end : ends) {
                auto &outer = selection.nodes[end];
                if (!outer.selected)
                    outer.reason = "junction-recheck-selected";
                outer.selected = true;
            }
        }
        selection.rechecks.push_back(std::move(recheck));
    }
    selection.chains.resize(creases.size());
    for (size_t node_id = 0; node_id < selection.nodes.size(); ++node_id) {
        const auto &node = selection.nodes[node_id];
        if (node.selected) {
            const auto &probe = selection.directions[node.directions.front()].probe;
            selection.chains[probe.path].push_back(node_id);
        }
    }
    for (auto &chain : selection.chains)
        std::stable_sort(chain.begin(), chain.end(), [&](size_t a, size_t b) {
            return selection.directions[selection.nodes[a].directions.front()].probe.position <
                   selection.directions[selection.nodes[b].directions.front()].probe.position;
        });
    // Chords deliberately cross discarded samples and original path breaks.
    // Equal crease positions retain source-list order; source surface samples,
    // their Z and width measurements are never reconstructed from the chord.
    return selection;
}

struct PaintedCreasePosition {
    size_t segment;
    double fraction;
};

PaintedCreasePosition painted_crease_position(const std::vector<double> &lengths, double position)
{
    const auto upper = std::upper_bound(lengths.begin(), lengths.end(), position);
    const size_t segment = upper == lengths.begin() ? 0 :
        std::min(size_t(upper - lengths.begin() - 1), lengths.size() - 2);
    const double length = lengths[segment + 1] - lengths[segment];
    return {segment, length > 0. ? std::clamp((position - lengths[segment]) / length, 0., 1.) : 0.};
}

Vec3d painted_crease_surface_point(const PaintedCreaseProbePath &crease, double position)
{
    const auto at = painted_crease_position(crease.surface_lengths, position);
    return crease.surface_points[at.segment] +
        at.fraction * (crease.surface_points[at.segment + 1] - crease.surface_points[at.segment]);
}

struct PaintedCreaseCapSpan {
    double begin = 0., end = 0.; // Untrimmed painted coverage, in source 3D arc length.
    double trimmed_begin = 0., trimmed_end = 0.;
    double chart_length = 0.;
    std::array<double, 2> widths{};
    std::array<std::pair<size_t, size_t>, 2> width_sources{}; // Retained blue path/point.
    enum class Kind { trimmed, blue_point, exhausted, no_samples } kind = Kind::no_samples;

    const char *status() const {
        switch (kind) {
        case Kind::trimmed:    return "trimmed";
        case Kind::blue_point: return "blue-point";
        case Kind::exhausted:  return "cap-trim-exhausted";
        case Kind::no_samples: return "no-selected-point";
        }
        return "no-selected-point";
    }
};

std::string painted_crease_cap_record(size_t run, size_t chart, size_t crease,
                                      size_t index, const PaintedCreaseCapSpan &span)
{
    std::ostringstream record;
    record << "stage=painted-crease-cap run=" << run << " chart=" << chart
           << " crease=" << crease << " span=" << index << " result=" << span.status()
           << " raw_begin_mm=" << span.begin << " raw_end_mm=" << span.end
           << " chart_length_mm=" << span.chart_length
           << " width_space=surface-chart position_space=source-crease-3d";
    if (span.kind == PaintedCreaseCapSpan::Kind::trimmed ||
        span.kind == PaintedCreaseCapSpan::Kind::exhausted)
        for (size_t side = 0; side < 2; ++side)
            record << " width" << side << "_mm=" << span.widths[side]
                   << " source" << side << "=P" << span.width_sources[side].first
                   << "." << span.width_sources[side].second;
    if (span.kind == PaintedCreaseCapSpan::Kind::trimmed)
        record << " trimmed_begin_mm=" << span.trimmed_begin << " trimmed_end_mm=" << span.trimmed_end;
    return record.str();
}

std::vector<std::vector<PaintedCreaseCapSpan>> painted_crease_cap_spans(
    const PrintObject &object, const ExPolygon &area,
    const std::vector<PaintedCreaseProbePath> &creases, const PaintedCreaseSelection &selection,
    bool blue_point_fallback)
{
    std::vector<std::vector<PaintedCreaseCapSpan>> result(creases.size());
    for (size_t c = 0; c < creases.size(); ++c) {
        const auto &crease = creases[c];
        auto &spans = result[c];
        std::vector<double> chart_lengths{0.};
        for (size_t i = 1; i < crease.points.size(); ++i)
            chart_lengths.push_back(chart_lengths.back() + (crease.points[i] - crease.points[i - 1]).norm());
        for (size_t segment = 0; segment < crease.segments.size(); ++segment) {
            if (object.print()->canceled())
                throw CanceledException();
            const auto &source = crease.segments[segment];
            const Vec2d &a = crease.points[segment];
            const Vec2d delta = crease.points[segment + 1] - a;
            if (!source.convex || source.patch_sides == 0 || delta.norm() <= EPSILON)
                continue;
            // Use the existing clipper for boundary crossings, then classify
            // intervals inclusively. Open-path clipping alone treats coincident
            // polygon edges differently by orientation; actual creases often
            // lie exactly on that painted boundary.
            std::vector<double> cuts{0., 1.};
            for (const Polyline &outside : diff_pl(
                    Polyline{scaled_point(a), scaled_point(a + delta)}, area))
                for (const Point &point : outside.points)
                    cuts.push_back(std::clamp((unscale(point) - a).dot(delta) / delta.squaredNorm(), 0., 1.));
            std::sort(cuts.begin(), cuts.end());
            cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
            const double source_length = crease.surface_lengths[segment + 1] - crease.surface_lengths[segment];
            for (size_t i = 1; i < cuts.size(); ++i) {
                const double begin = crease.surface_lengths[segment] + cuts[i - 1] * source_length;
                const double end = crease.surface_lengths[segment] + cuts[i] * source_length;
                if (end - begin <= EPSILON)
                    continue;
                const Point middle = scaled_point(a + (0.5 * (cuts[i - 1] + cuts[i])) * delta);
                if (!area.contains(middle, true) && !area.on_boundary(middle, SCALED_EPSILON))
                    continue;
                if (!spans.empty() && begin <= spans.back().end + EPSILON)
                    spans.back().end = end;
                else
                    spans.push_back({begin, end});
            }
        }
        auto chart_position = [&](double position) {
            const auto at = painted_crease_position(crease.surface_lengths, position);
            return chart_lengths[at.segment] + at.fraction *
                (chart_lengths[at.segment + 1] - chart_lengths[at.segment]);
        };
        auto surface_position = [&](double position) {
            const auto at = painted_crease_position(chart_lengths, position);
            return crease.surface_lengths[at.segment] + at.fraction *
                (crease.surface_lengths[at.segment + 1] - crease.surface_lengths[at.segment]);
        };
        for (auto &span : spans) {
            span.chart_length = chart_position(span.end) - chart_position(span.begin);
            if (blue_point_fallback) {
                span.kind = PaintedCreaseCapSpan::Kind::blue_point;
                continue;
            }
            if (selection.chains[c].empty())
                continue;
            for (size_t side = 0; side < 2; ++side) {
                const auto at = painted_crease_position(crease.surface_lengths, side == 0 ? span.begin : span.end);
                const Vec2d endpoint = crease.points[at.segment] +
                    at.fraction * (crease.points[at.segment + 1] - crease.points[at.segment]);
                size_t nearest = std::numeric_limits<size_t>::max();
                double nearest_distance = std::numeric_limits<double>::infinity();
                for (const size_t node : selection.chains[c]) {
                    const double distance = (selection.nodes[node].sample.chart - endpoint).squaredNorm();
                    // The chain is sorted on the crease, but exact ties use the
                    // original source-list node order, not the reordered chain.
                    if (distance < nearest_distance || (distance == nearest_distance && node < nearest)) {
                        nearest = node;
                        nearest_distance = distance;
                    }
                }
                const auto &node = selection.nodes[nearest];
                span.widths[side] = node.sample.width;
                span.width_sources[side] = {node.path, node.point};
            }
            const double begin = chart_position(span.begin) + span.widths[0];
            const double end = chart_position(span.end) - span.widths[1];
            if (end - begin <= EPSILON) {
                span.kind = PaintedCreaseCapSpan::Kind::exhausted;
                continue;
            }
            span.trimmed_begin = surface_position(begin);
            span.trimmed_end = surface_position(end);
            span.kind = PaintedCreaseCapSpan::Kind::trimmed;
        }
    }
    return result;
}

struct PaintedBlueDiagnostic {
    struct Compact {
        double length, width, threshold;
        bool point;
    };
    std::vector<Compact> components;
    struct Path {
        std::vector<PaintedIntentSample> samples; // Immediately before cap trimming.
        double front_trim, back_trim;
        size_t begin, end; // Surviving source interval, end exclusive.
    };
    std::vector<Path> paths;
    size_t groups = 0;
    double longest_length = 0., group_width = 0.;
    bool point = false;
    std::optional<PaintedIntentPoint> output_point;
};

void export_painted_crease_probes(const PrintObject &object, size_t run, size_t chart,
                                 size_t patch, size_t source_chart, const ExPolygon &area,
                                 const ThickPolylines &raw_skeleton,
                                 const std::vector<std::vector<PaintedIntentSample>> &blue_paths,
                                 const std::vector<PaintedCreaseProbePath> &creases,
                                 const PaintedCreaseSelection &selection,
                                 const std::vector<std::vector<PaintedCreaseCapSpan>> &cap_spans,
                                 const PaintedBlueDiagnostic &blue_diagnostic)
{
    const std::string filename = debug_out_path(
        "fin-support/painted-shape-crease-probe-%d-%d.svg", int(run), int(chart));
    BoundingBox bounds = get_extents(area);
    for (const auto &path : blue_diagnostic.paths)
        for (const auto &sample : path.samples)
            bounds.merge(scaled_point(sample.chart));
    const size_t cap_discarded = std::count_if(blue_diagnostic.paths.begin(), blue_diagnostic.paths.end(),
        [](const auto &path) { return path.end - path.begin < 2; });
    const char *blue_reason = !blue_diagnostic.point ? "retained-groups" :
        blue_diagnostic.groups > 0 ? "longest-below-width" :
        !blue_diagnostic.components.empty() &&
            std::all_of(blue_diagnostic.components.begin(), blue_diagnostic.components.end(),
                [](const auto &component) { return component.point; }) ? "compact-components" :
        !blue_diagnostic.paths.empty() && cap_discarded == blue_diagnostic.paths.size() ?
            "cap-trim-no-line" : "preparation-no-line";
    std::ostringstream blue_status;
    blue_status << (blue_diagnostic.point ? "POINT" : "LINE") << " / " << blue_reason;
    if (blue_diagnostic.groups > 0)
        blue_status << " L=" << blue_diagnostic.longest_length << " W=" << blue_diagnostic.group_width;
    size_t convex_segments = 0, nonconvex_segments = 0, collapsed_segments = 0;
    for (const auto &crease : creases)
        for (size_t i = 0; i < crease.segments.size(); ++i)
            if (crease.segments[i].patch_sides != 0) {
                bounds.merge(scaled_point(crease.points[i]));
                bounds.merge(scaled_point(crease.points[i + 1]));
                convex_segments += crease.segments[i].convex;
                nonconvex_segments += !crease.segments[i].convex;
                collapsed_segments += (crease.points[i + 1] - crease.points[i]).norm() <= EPSILON;
            }
    double maximum_radius = 0.;
    for (const auto &path : blue_paths)
        for (const auto &sample : path) {
            bounds.merge(scaled_point(sample.chart));
            maximum_radius = std::max(maximum_radius, 0.5 * sample.width);
        }
    const Point margin = scaled_point(Vec2d(maximum_radius, maximum_radius));
    bounds.min -= margin;
    bounds.max += margin;
    // Keep raw direction results separate from the final point selection.
    const Vec2d minimum = unscale(bounds.min);
    const Vec2d extent = unscale(bounds.size());
    const double panel_width = std::max(60., 4. * extent.x() + 4.);
    auto display = [&](const Vec2d &point, size_t panel) {
        return scaled_point(Vec2d(4. * (point - minimum) + Vec2d(panel * panel_width, 0.)));
    };
    const BoundingBox canvas(Point(0, 0),
        scaled_point(Vec2d(4. * panel_width, 4. * extent.y() + 11. + 2.5 * blue_diagnostic.components.size())));
    SVG svg(filename, canvas, scale_(1.));
    if (svg.is_opened()) {
        svg.add_comment("Diagnostic only; surface-chart XY, display x4, measurements in mm. "
                        "Gray=paint; blue=retained blue paths; purple=convex crease; "
                        "gray crease=non-convex; cyan=sample window; orange=average direction. "
                        "First panel: pink=raw medial axis before skeleton pruning; "
                        "orange=prepared skeleton before cap trimming (O labels); "
                        "blue=final retained blue paths (P labels); black=standalone blue point output. "
                        "POINT/LINE is the chart's blue classification. "
                        "Fourth panel: teal=selected-point chain; red=discarded point; "
                        "magenta=accepted junction recheck chord, red=rejected recheck chord. "
                        "Yellow=painted crease span; pink=single-side cap-trimmed span. "
                        "A direction pass does not establish paired paint semantics. "
                        "No artificial crease extension or fin geometry is changed.");
        const std::array<const char *, 4> headings{
            "SOURCE / pink=raw O=orange path R=crease P=blue point", "DIRECTION PASS ONLY (15 deg or less)", "REJECTED / UNAVAILABLE",
            "SELECTED POINTS / ORDERED ON 3D CREASE"};
        for (size_t panel = 0; panel < headings.size(); ++panel) {
            svg.draw_text(scaled_point(Vec2d(panel * panel_width, 4. * extent.y() + 5.)),
                          headings[panel], "#202020", 1);
            if (panel == 0) {
                const char *status = blue_paths.empty() ? "No retained blue paths" :
                    creases.empty() ? "No persistent crease in this patch" :
                    "Measurements: SVG comments / debug log";
                svg.draw_text(scaled_point(Vec2d(0., 4. * extent.y() + 2.5)), status, "#202020", 1);
                svg.draw_text(scaled_point(Vec2d(0., 4. * extent.y() + 7.5)),
                              blue_status.str().c_str(), "#202020", 1);
                for (size_t component = 0; component < blue_diagnostic.components.size(); ++component) {
                    const auto &compact = blue_diagnostic.components[component];
                    std::ostringstream label;
                    label << "C" << component << " compact=" << (compact.point ? "POINT" : "PASS")
                          << " L=" << compact.length << " W=" << compact.width << " threshold=" << compact.threshold;
                    svg.draw_text(scaled_point(Vec2d(0., 4. * extent.y() + 10. + 2.5 * component)),
                                  label.str().c_str(), "#202020", 1);
                }
            }
            auto outline = [&](const Polygon &polygon) {
                Polyline line;
                for (const Point &point : polygon.points)
                    line.points.push_back(display(unscale(point), panel));
                if (!line.points.empty())
                    line.points.push_back(line.points.front());
                svg.draw(line, "#aaaaaa", scale_(0.08));
            };
            outline(area.contour);
            for (const Polygon &hole : area.holes)
                outline(hole);
            if (panel == 0) {
                for (size_t path = 0; path < raw_skeleton.size(); ++path) {
                    const ThickPolyline &raw = raw_skeleton[path];
                    Polyline line;
                    std::ostringstream detail;
                    detail.precision(17);
                    detail << "raw-medial-axis path=" << path << " points_scaled=";
                    for (const Point &point : raw.points) {
                        line.points.push_back(display(unscale(point), panel));
                        detail << point.x() << ',' << point.y() << ' ';
                    }
                    detail << " widths_scaled=";
                    for (const auto width : raw.width)
                        detail << width << ' ';
                    svg.add_comment(detail.str());
                    svg.draw(line, "#cc79a7", scale_(0.08));
                }
            }
            for (const auto &crease : creases)
                for (size_t i = 0; i < crease.segments.size(); ++i) {
                    const auto &source = crease.segments[i];
                    if (source.patch_sides == 0)
                        continue;
                    svg.draw(Line(display(crease.points[i], panel), display(crease.points[i + 1], panel)),
                             source.convex ? "#8e44ad" : "#888888", scale_(0.16));
                    if (panel == 0) {
                        const std::string label = "R" + std::to_string(crease.id) + "." + std::to_string(i);
                        svg.draw_text(display(0.5 * (crease.points[i] + crease.points[i + 1]), panel),
                                      label.c_str(), source.convex ? "#8e44ad" : "#888888", 1);
                    }
                }
            if (panel == 0)
                for (size_t path = 0; path < blue_diagnostic.paths.size(); ++path) {
                    Polyline line;
                    for (const auto &sample : blue_diagnostic.paths[path].samples) {
                        const Point xy = display(sample.chart, panel);
                        line.points.push_back(xy);
                        svg.draw(xy, "#e69f00", scale_(0.08));
                    }
                    svg.draw(line, "#e69f00", scale_(0.22));
                    if (!line.points.empty()) {
                        const std::string label = "O" + std::to_string(path);
                        svg.draw_text(line.points.front(), label.c_str(), "#e69f00", 1);
                    }
                }
            for (size_t path = 0; path < blue_paths.size(); ++path) {
                Polyline line;
                for (size_t point = 0; point < blue_paths[path].size(); ++point) {
                    const Point xy = display(blue_paths[path][point].chart, panel);
                    line.points.push_back(xy);
                    svg.draw(xy, "#0072b2", scale_(0.12));
                    if (panel == 0) {
                        const std::string label = "P" + std::to_string(path) + "." + std::to_string(point);
                        svg.draw_text(xy, label.c_str(), "#0072b2", 1);
                    }
                }
                if (panel != 3)
                    svg.draw(line, "#0072b2", scale_(0.12));
            }
            if (panel == 0 && blue_diagnostic.output_point) {
                const Point xy = display(blue_diagnostic.output_point->chart, panel);
                svg.draw(xy, "#000000", scale_(0.5));
                svg.draw_text(xy, "POINT", "#000000", 1);
            }
        }
    }
    for (size_t path = 0; path < blue_diagnostic.paths.size(); ++path) {
        const auto &source = blue_diagnostic.paths[path];
        std::ostringstream record;
        record << "stage=painted-blue-cap run=" << run << " chart=" << chart << " path=O" << path
               << " input_points=" << source.samples.size()
               << " front_trim_mm=" << source.front_trim << " back_trim_mm=" << source.back_trim
               << " begin=" << source.begin << " end_exclusive=" << source.end
               << " result=" << (source.end - source.begin >= 2 ? "retained" : "discarded");
        BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << record.str();
        if (svg.is_opened())
            svg.add_comment(record.str());
    }
    for (size_t component = 0; component < blue_diagnostic.components.size(); ++component) {
        const auto &compact = blue_diagnostic.components[component];
        std::ostringstream record;
        record << "stage=painted-compact-component run=" << run << " chart=" << chart
               << " component=" << component << " result=" << (compact.point ? "point" : "pass")
               << " fitted_length_mm=" << compact.length << " original_mean_width_mm=" << compact.width
               << " core_length_mm=" << std::max(0., compact.length - compact.width)
               << " threshold_mm=" << compact.threshold;
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name << " " << record.str();
        if (svg.is_opened())
            svg.add_comment(record.str());
    }
    std::ostringstream classification;
    classification << "stage=painted-blue-classification run=" << run << " chart=" << chart
                   << " patch_seed=" << patch << " source_chart=" << source_chart
                   << " result=" << (blue_diagnostic.point ? "point" : "line") << " reason=" << blue_reason
                   << " orange_paths=" << blue_diagnostic.paths.size() << " cap_discarded_paths=" << cap_discarded
                   << " groups=" << blue_diagnostic.groups << " longest_length_mm=" << blue_diagnostic.longest_length
                   << " group_width_mm=" << blue_diagnostic.group_width << " retained_paths=" << blue_paths.size();
    if (blue_diagnostic.point) {
        classification << " point_emitted=" << blue_diagnostic.output_point.has_value();
        if (blue_diagnostic.output_point) {
            const PaintedIntentPoint &point = *blue_diagnostic.output_point;
            classification << " point_chart_x=" << point.chart.x() << " point_chart_y=" << point.chart.y()
                           << " source_face=" << point.surface.face
                           << " point_x=" << point.surface.point.x() << " point_y=" << point.surface.point.y()
                           << " point_z=" << point.surface.point.z() << " point_area_mm2=" << point.area;
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name << " " << classification.str();
    if (svg.is_opened())
        svg.add_comment(classification.str());
    size_t probes = 0, passed = 0, unavailable = 0, truncated = 0, both_sides = 0;
    for (size_t path = 0; path < blue_paths.size(); ++path) {
        const auto &samples = blue_paths[path];
        for (size_t point = 0; point < samples.size(); ++point) {
            if (object.print()->canceled())
                throw CanceledException();
            for (int step : {-1, 1}) {
                if ((step < 0 && point == 0) || (step > 0 && point + 1 == samples.size()))
                    continue;
                const size_t neighbor = step < 0 ? point - 1 : point + 1;
                const auto &direction = selection.directions[probes];
                const PaintedCreaseProbe &probe = direction.probe;
                const bool valid = probe.direction_pass;
                ++probes;
                passed += valid;
                unavailable += !probe.angle.has_value();
                truncated += !probe.window.empty() && probe.sampled < 0.5 * samples[point].width;
                std::ostringstream record;
                record << "stage=painted-crease-probe run=" << run << " chart=" << chart
                       << " patch_seed=" << patch << " source_chart=" << source_chart
                       << " path=" << path << " point=" << point << " neighbor=" << neighbor
                       << " point_x=" << samples[point].chart.x() << " point_y=" << samples[point].chart.y()
                       << " source_face=" << samples[point].surface.face
                       << " full_width_mm=" << samples[point].width
                       << " radius_mm=" << 0.5 * samples[point].width
                       << " result=" << probe.result
                       << " junction_rank=" << direction.junction_rank
                       << " branch_selected=" << direction.branch_selected
                       << " point_selected=" << selection.nodes[direction.node].selected;
                if (probe.path != std::numeric_limits<size_t>::max()) {
                    const auto &crease = creases[probe.path];
                    const auto &segment = crease.segments[probe.segment];
                    both_sides += segment.patch_sides == 3;
                    record << " crease=" << crease.id << " segment=" << probe.segment
                           << " face0=" << segment.faces[0] << " face1=" << segment.faces[1]
                           << " patch_sides=" << segment.patch_sides
                           << " foot_x=" << probe.foot.x() << " foot_y=" << probe.foot.y()
                           << " crease_position_mm=" << probe.position
                           << " distance_mm=" << probe.distance << " sampled_mm=" << probe.sampled
                           << " window_stop=" << probe.window_stop
                           << " average_x=" << probe.average.x() << " average_y=" << probe.average.y();
                }
                record << " angle_deg=" << (probe.angle ? std::to_string(*probe.angle) : "none");
                BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name
                                         << " " << record.str();
                if (svg.is_opened()) {
                    // Numeric/source identifiers only, so XML comments remain
                    // valid even when the object's display name contains "--".
                    svg.add_comment(record.str());
                    const size_t panel = valid ? 1 : 2;
                    const Vec2d &origin = samples[point].chart;
                    const char *color = valid ? "#009e73" : "#d55e00";
                    svg.draw(Line(display(origin, panel), display(samples[neighbor].chart, panel)),
                             color, scale_(0.22));
                    std::string label = "P" + std::to_string(path) + "." + std::to_string(point) +
                        (step < 0 ? "/prev " : "/next ");
                    if (probe.angle)
                        label += std::to_string(*probe.angle) + " deg";
                    else
                        label += probe.result;
                    if (direction.junction_rank != 0)
                        label += " J#" + std::to_string(direction.junction_rank) +
                            (direction.branch_selected ? "" : " excluded");
                    svg.draw_text(display(origin + Vec2d(0., step < 0 ? 0.3 : -0.3), panel),
                                  label.c_str(), color, 1);
                    if (probe.path != std::numeric_limits<size_t>::max()) {
                        svg.draw(Line(display(origin, panel), display(probe.foot, panel)), "#999999", scale_(0.06));
                        svg.draw(display(probe.foot, panel), "#8e44ad", scale_(0.14));
                        Polyline window;
                        for (const Vec2d &p : probe.window)
                            window.points.push_back(display(p, panel));
                        if (window.points.size() >= 2)
                            svg.draw(window, "#00bcd4", scale_(0.3));
                        if (probe.angle) {
                            const Point average_end = display(probe.foot + probe.sampled * probe.average, panel);
                            svg.draw(Line(display(probe.foot, panel),
                                average_end),
                                "#e69f00", scale_(0.12));
                            svg.draw(average_end, "#e69f00", scale_(0.12));
                        }
                    }
                }
            }
        }
    }
    size_t selected_points = 0, selected_junctions = 0, rejected_junctions = 0, selected_chains = 0;
    for (const auto &node : selection.nodes) {
        selected_points += node.selected;
        selected_junctions += node.sample.junction && node.selected;
        rejected_junctions += node.sample.junction && !node.selected;
        const auto &sample = node.sample;
        std::ostringstream record;
        record << "stage=painted-crease-selection run=" << run << " chart=" << chart
               << " patch_seed=" << patch << " source_chart=" << source_chart
               << " path=" << node.path << " point=" << node.point
               << " junction=" << sample.junction << " degree=" << node.directions.size()
               << " result=" << node.reason
               << " surface_x=" << sample.surface.point.x() << " surface_y=" << sample.surface.point.y()
               << " surface_z=" << sample.surface.point.z() << " source_face=" << sample.surface.face
               << " full_width_mm=" << sample.width;
        if (!node.directions.empty()) {
            const auto &probe = selection.directions[node.directions.front()].probe;
            if (probe.path != std::numeric_limits<size_t>::max())
                record << " crease=" << creases[probe.path].id << " crease_position_mm=" << probe.position
                       << " foot_surface_x=" << probe.surface_foot.x()
                       << " foot_surface_y=" << probe.surface_foot.y()
                       << " foot_surface_z=" << probe.surface_foot.z();
        }
        BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << record.str();
        if (svg.is_opened()) {
            svg.add_comment(record.str());
            const char *color = node.selected ? "#009e73" : "#d55e00";
            svg.draw(display(sample.chart, 3), color, scale_(0.25));
            const std::string label = "P" + std::to_string(node.path) + "." + std::to_string(node.point) +
                (sample.junction ? "/J " : " ") + node.reason;
            svg.draw_text(display(sample.chart, 3), label.c_str(), color, 1);
        }
    }
    for (size_t crease = 0; crease < selection.chains.size(); ++crease) {
        const auto &chain = selection.chains[crease];
        if (chain.empty())
            continue;
        selected_chains += chain.size() >= 2;
        Polyline line;
        std::ostringstream record;
        record << "stage=painted-crease-chain run=" << run << " chart=" << chart
               << " patch_seed=" << patch << " source_chart=" << source_chart
               << " crease=" << creases[crease].id << " points=" << chain.size() << " order=";
        for (size_t i = 0; i < chain.size(); ++i) {
            const auto &node = selection.nodes[chain[i]];
            line.points.push_back(display(node.sample.chart, 3));
            record << (i == 0 ? "" : ",") << "P" << node.path << "." << node.point;
        }
        BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << record.str();
        if (svg.is_opened()) {
            svg.add_comment(record.str());
            if (line.points.size() >= 2)
                svg.draw(line, "#009e73", scale_(0.22));
        }
    }
    size_t accepted_rechecks = 0;
    for (size_t r = 0; r < selection.rechecks.size(); ++r) {
        const auto &recheck = selection.rechecks[r];
        const auto &junction = selection.nodes[recheck.junction];
        accepted_rechecks += recheck.accepted;
        const char *color = recheck.accepted ? "#cc0099" : "#d55e00";
        if (svg.is_opened())
            svg.draw(Line(display(selection.nodes[recheck.ends[0]].sample.chart, 3),
                          display(selection.nodes[recheck.ends[1]].sample.chart, 3)), color, scale_(0.12));
        for (size_t side = 0; side < 2; ++side) {
            const auto &end = selection.nodes[recheck.ends[side]];
            const auto &other = selection.nodes[recheck.ends[1 - side]];
            const auto &probe = recheck.probes[side];
            std::ostringstream record;
            record << "stage=painted-crease-junction-recheck run=" << run << " chart=" << chart
                   << " patch_seed=" << patch << " source_chart=" << source_chart << " recheck=" << r
                   << " junction=P" << junction.path << "." << junction.point
                   << " result=" << (recheck.accepted ? "accepted" : "rejected")
                   << " side=" << side << " from=P" << end.path << "." << end.point
                   << " to=P" << other.path << "." << other.point
                   << " full_width_mm=" << end.sample.width << " radius_mm=" << 0.5 * end.sample.width
                   << " probe_result=" << probe.result
                   << " angle_deg=" << (probe.angle ? std::to_string(*probe.angle) : "none");
            if (probe.path != std::numeric_limits<size_t>::max())
                record << " crease=" << creases[probe.path].id << " crease_position_mm=" << probe.position
                       << " same_crease=" << (probe.path ==
                            selection.directions[junction.directions.front()].probe.path)
                       << " foot_x=" << probe.foot.x() << " foot_y=" << probe.foot.y()
                       << " average_x=" << probe.average.x() << " average_y=" << probe.average.y()
                       << " sampled_mm=" << probe.sampled << " window_stop=" << probe.window_stop;
            BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << record.str();
            if (svg.is_opened()) {
                svg.add_comment(record.str());
                const std::string label = "RECHECK " + std::to_string(r) + "/" + std::to_string(side) +
                    (recheck.accepted ? " PASS " : " FAIL ") +
                    (probe.angle ? std::to_string(*probe.angle) + " deg" : probe.result);
                svg.draw_text(display(end.sample.chart + Vec2d(0., 0.6), 3), label.c_str(), color, 1);
                Polyline window;
                for (const Vec2d &p : probe.window)
                    window.points.push_back(display(p, 3));
                if (window.points.size() >= 2)
                    svg.draw(window, "#00bcd4", scale_(0.15));
                if (probe.angle)
                    svg.draw(Line(display(probe.foot, 3),
                                  display(probe.foot + probe.sampled * probe.average, 3)), "#e69f00", scale_(0.08));
            }
        }
    }
    for (size_t c = 0; c < cap_spans.size(); ++c)
        for (size_t r = 0; r < cap_spans[c].size(); ++r) {
            const auto &span = cap_spans[c][r];
            const std::string record = painted_crease_cap_record(run, chart, creases[c].id, r, span);
            BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << record;
            if (!svg.is_opened())
                continue;
            svg.add_comment(record);
            auto draw_span = [&](double begin, double end, const char *color, double width) {
                const auto &crease = creases[c];
                for (size_t i = 0; i < crease.segments.size(); ++i) {
                    const double lo = std::max(begin, crease.surface_lengths[i]);
                    const double hi = std::min(end, crease.surface_lengths[i + 1]);
                    if (hi - lo <= EPSILON)
                        continue;
                    const double length = crease.surface_lengths[i + 1] - crease.surface_lengths[i];
                    const Vec2d delta = crease.points[i + 1] - crease.points[i];
                    svg.draw(Line(display(crease.points[i] + (lo - crease.surface_lengths[i]) / length * delta, 3),
                                  display(crease.points[i] + (hi - crease.surface_lengths[i]) / length * delta, 3)),
                        color, scale_(width));
                }
            };
            draw_span(span.begin, span.end, "#e69f00", 0.32);
            if (span.kind == PaintedCreaseCapSpan::Kind::trimmed)
                draw_span(span.trimmed_begin, span.trimmed_end, "#cc79a7", 0.18);
            const auto at = painted_crease_position(creases[c].surface_lengths, span.begin);
            const Vec2d endpoint = creases[c].points[at.segment] +
                at.fraction * (creases[c].points[at.segment + 1] - creases[c].points[at.segment]);
            const std::string label = "R" + std::to_string(creases[c].id) + "/cap" +
                std::to_string(r) + " " + span.status();
            svg.draw_text(display(endpoint, 3), label.c_str(), "#202020", 1);
        }
    BOOST_LOG_TRIVIAL(info)
        << "Fin support object=" << object.model_object()->name
        << " stage=painted-crease-probes run=" << run << " chart=" << chart
        << " patch_seed=" << patch << " source_chart=" << source_chart
        << " space=surface-chart blue_paths=" << blue_paths.size() << " probes=" << probes
        << " convex_segments=" << convex_segments << " nonconvex_segments=" << nonconvex_segments
        << " collapsed_segments=" << collapsed_segments
        << " direction_pass=" << passed << " angle_reject=" << probes - passed - unavailable
        << " unavailable=" << unavailable << " truncated_windows=" << truncated
        << " both_sides_in_patch=" << both_sides
        << " selected_points=" << selected_points << " selected_chains=" << selected_chains
        << " selected_junctions=" << selected_junctions << " rejected_junctions=" << rejected_junctions
        << " junction_rechecks=" << selection.rechecks.size() << " accepted_rechecks=" << accepted_rechecks
        << " distance_filter=false paired_semantics=false"
        << " svg_opened=" << svg.is_opened() << " svg=" << filename;
}

// Owned diagnostic snapshots outlive each patch's local blue graph. Pairing
// uses the two source faces of each crease edge, not projected proximity.
struct PaintedCreaseComparison {
    PaintedCreaseProbePath crease;
    struct Chain {
        size_t chart;
        size_t patch;
        size_t source_chart;
        std::vector<unsigned> sides;
        struct Sample {
            PaintedIntentSample source;
            double position;
            size_t path;
            size_t point;
        };
        std::vector<Sample> samples;
        std::vector<PaintedCreaseCapSpan> cap_spans;
        bool blue_point_fallback = false;
    };
    std::vector<Chain> chains;
};

void collect_painted_crease_comparison(
    size_t chart, size_t patch, size_t source_chart,
    const std::vector<PaintedCreaseProbePath> &creases, const PaintedCreaseSelection &selection,
    const std::vector<std::vector<PaintedCreaseCapSpan>> &cap_spans, bool blue_point_fallback,
    std::map<size_t, PaintedCreaseComparison> &comparisons)
{
    for (size_t i = 0; i < creases.size(); ++i) {
        const auto [it, inserted] = comparisons.try_emplace(creases[i].id);
        auto &comparison = it->second;
        if (inserted)
            comparison.crease = creases[i];
        PaintedCreaseComparison::Chain chain{chart, patch, source_chart, {}, {}, cap_spans[i], blue_point_fallback};
        for (const auto &segment : creases[i].segments)
            chain.sides.push_back(segment.patch_sides);
        for (const size_t node_id : selection.chains[i]) {
            const auto &node = selection.nodes[node_id];
            chain.samples.push_back({node.sample,
                selection.directions[node.directions.front()].probe.position, node.path, node.point});
        }
        comparison.chains.push_back(std::move(chain));
    }
}

struct PaintedCreaseSectionHits {
    struct Hit {
        Vec3d point;
        size_t edge;
        double fraction;
    };
    std::vector<Hit> hits;
    std::vector<size_t> coplanar;

    const char *status() const {
        return !coplanar.empty() ? "coplanar" :
            hits.empty() ? "missing" : hits.size() == 1 ? "unique" : "multiple";
    }
    bool unique() const { return coplanar.empty() && hits.size() == 1; }
};

PaintedCreaseSectionHits painted_crease_section_hits(
    const PaintedCreaseComparison::Chain &chain, const Vec3d &origin, const Vec3d &normal)
{
    PaintedCreaseSectionHits result;
    // Linef3::intersect_plane handles only horizontal planes. This finite
    // section query also retains coplanarity and every distinct intersection.
    for (size_t edge = 0; edge + 1 < chain.samples.size(); ++edge) {
        const Vec3d &a = chain.samples[edge].source.surface.point;
        const Vec3d &b = chain.samples[edge + 1].source.surface.point;
        const double da = normal.dot(a - origin), db = normal.dot(b - origin);
        const bool on_a = std::abs(da) <= EPSILON, on_b = std::abs(db) <= EPSILON;
        if (on_a && on_b && (b - a).norm() > EPSILON) {
            result.coplanar.push_back(edge);
            continue;
        }
        if (!on_a && !on_b && ((da < 0.) == (db < 0.)))
            continue;
        const double fraction = on_a ? 0. : on_b ? 1. : da / (da - db);
        const Vec3d point = a + fraction * (b - a);
        // Adjacent edges meeting the plane at one shared vertex are one hit.
        if (std::none_of(result.hits.begin(), result.hits.end(), [&](const auto &hit) {
                return (hit.point - point).norm() <= EPSILON;
            }))
            result.hits.push_back({point, edge, fraction});
    }
    return result;
}

struct PaintedCreaseSection {
    size_t segment;
    double begin, end, position;
    Vec3d origin, normal;
    std::array<PaintedCreaseSectionHits, 2> sides;

    bool accepted() const {
        if (!sides[0].unique() || !sides[1].unique())
            return false;
        const double a = (sides[0].hits.front().point - origin).norm();
        const double b = (sides[1].hits.front().point - origin).norm();
        // Compare the difference with 100% of the mean, without a division
        // or an absolute-distance floor that would change narrow strokes.
        return std::abs(a - b) <= 0.5 * (a + b);
    }
};

struct PaintedCreasePair {
    std::array<size_t, 2> chains;
    std::array<size_t, 2> spans;
    double raw_begin, raw_end;
    double begin, end;
    const char *result;
    bool point;
    std::vector<PaintedCreaseSection> sections;
    struct Intent {
        double begin, end;
        size_t first_section, section_count;
        // One vertex is a point intent. Lines preserve the source crease bends.
        std::vector<Vec3d> points;
    };
    std::vector<Intent> intents;
};

void build_painted_crease_intents(const PaintedCreaseProbePath &crease, PaintedCreasePair &pair)
{
    if (pair.point) {
        pair.intents.push_back({pair.begin, pair.end, 0, 0,
            {painted_crease_surface_point(crease, pair.begin)}});
        return;
    }
    size_t first = 0;
    while (first < pair.sections.size()) {
        if (!pair.sections[first].accepted()) {
            ++first;
            continue;
        }
        size_t end = first + 1;
        while (end < pair.sections.size() && pair.sections[end].accepted() &&
               pair.sections[end].begin <= pair.sections[end - 1].end + EPSILON)
            ++end;
        if (end == first + 1) {
            const auto &section = pair.sections[first];
            pair.intents.push_back({section.position, section.position, first, 1, {section.origin}});
        } else {
            const double lo = pair.sections[first].begin, hi = pair.sections[end - 1].end;
            PaintedCreasePair::Intent intent{lo, hi, first, end - first,
                {painted_crease_surface_point(crease, lo)}};
            for (size_t vertex = 1; vertex + 1 < crease.surface_points.size(); ++vertex)
                if (crease.surface_lengths[vertex] > lo && crease.surface_lengths[vertex] < hi)
                    intent.points.push_back(crease.surface_points[vertex]);
            intent.points.push_back(painted_crease_surface_point(crease, hi));
            pair.intents.push_back(std::move(intent));
        }
        // A failed section or an uncovered interval always separates intents.
        first = end;
    }
}

std::vector<PaintedCreasePair> compare_painted_crease_sides(
    const PrintObject &object, const PaintedCreaseComparison &comparison)
{
    const auto &crease = comparison.crease;
    const auto &chains = comparison.chains;
    std::vector<PaintedCreasePair> pairs;
    for (size_t a = 0; a < chains.size(); ++a)
        for (size_t b = a + 1; b < chains.size(); ++b) {
            if (chains[a].patch == chains[b].patch)
                continue;
            for (size_t ia = 0; ia < chains[a].cap_spans.size(); ++ia)
                for (size_t ib = 0; ib < chains[b].cap_spans.size(); ++ib) {
                    const auto &ca = chains[a].cap_spans[ia], &cb = chains[b].cap_spans[ib];
                    const double raw_begin = std::max(ca.begin, cb.begin), raw_end = std::min(ca.end, cb.end);
                    if (raw_end - raw_begin <= EPSILON)
                        continue;
                    auto opposite = [&](size_t segment) {
                        const unsigned sa = chains[a].sides[segment], sb = chains[b].sides[segment];
                        return crease.segments[segment].convex &&
                            ((sa == 1 && sb == 2) || (sa == 2 && sb == 1));
                    };
                    // Both spans already follow contiguous painted source
                    // edges. Do not pair projections from an unseparated patch.
                    bool has_opposite_faces = false;
                    for (size_t segment = 0; segment < crease.segments.size(); ++segment)
                        has_opposite_faces |= opposite(segment) &&
                            std::min(raw_end, crease.surface_lengths[segment + 1]) -
                            std::max(raw_begin, crease.surface_lengths[segment]) > EPSILON;
                    if (!has_opposite_faces)
                        continue;
                    PaintedCreasePair pair{{a, b}, {ia, ib}, raw_begin, raw_end, raw_begin, raw_end,
                        "no-selected-point", false, {}};
                    using Kind = PaintedCreaseCapSpan::Kind;
                    if (chains[a].blue_point_fallback || chains[b].blue_point_fallback) {
                        pair.point = true;
                        pair.result = "blue-point";
                    } else if (ca.kind == Kind::exhausted || cb.kind == Kind::exhausted) {
                        pair.point = true;
                        pair.result = "cap-trim-exhausted";
                    } else if (ca.kind == Kind::trimmed && cb.kind == Kind::trimmed) {
                        // Fuse positions on the original 3D path, never XYZ
                        // coordinates or already-trimmed blue endpoints.
                        pair.begin = 0.5 * (ca.trimmed_begin + cb.trimmed_begin);
                        pair.end = 0.5 * (ca.trimmed_end + cb.trimmed_end);
                        pair.point = pair.end - pair.begin <= EPSILON;
                        pair.result = pair.point ? "cap-trim-exhausted" : "line-candidate";
                        if (!pair.point)
                            for (size_t segment = 0; segment < crease.segments.size(); ++segment) {
                                if (object.print()->canceled())
                                    throw CanceledException();
                                if (!opposite(segment))
                                    continue;
                                const double lo = std::max(pair.begin, crease.surface_lengths[segment]);
                                const double hi = std::min(pair.end, crease.surface_lengths[segment + 1]);
                                if (hi - lo <= EPSILON)
                                    continue;
                                const Vec3d normal = (crease.surface_points[segment + 1] -
                                    crease.surface_points[segment]).normalized();
                                const double position = lo + 0.5 * (hi - lo);
                                const Vec3d origin = painted_crease_surface_point(crease, position);
                                pair.sections.push_back({segment, lo, hi, position, origin, normal,
                                    {painted_crease_section_hits(chains[a], origin, normal),
                                     painted_crease_section_hits(chains[b], origin, normal)}});
                            }
                    }
                    if (pair.point)
                        pair.begin = pair.end = raw_begin + 0.5 * (raw_end - raw_begin);
                    build_painted_crease_intents(crease, pair);
                    pairs.push_back(std::move(pair));
                }
        }

    return pairs;
}

void export_painted_crease_sections(
    const PrintObject &object, size_t run, const PaintedCreaseComparison &comparison,
    const std::vector<PaintedCreasePair> &pairs)
{
    const auto &crease = comparison.crease;
    const auto &chains = comparison.chains;
    Vec3d minimum = crease.surface_points.front(), maximum = minimum;
    auto include = [&](const Vec3d &p) {
        minimum = minimum.cwiseMin(p);
        maximum = maximum.cwiseMax(p);
    };
    for (const Vec3d &p : crease.surface_points)
        include(p);
    for (const auto &chain : chains)
        for (const auto &sample : chain.samples)
            include(sample.source.surface.point);
    const double view_size = std::max(60., 4. * (maximum - minimum).maxCoeff() + 8.);
    const double plots_height = 45. * std::max(size_t(1), pairs.size());
    const double legend_height = 3. * chains.size() + 8.;
    const std::string filename = debug_out_path(
        "fin-support/painted-shape-crease-sections-%d-%d.svg", int(run), int(crease.id));
    SVG svg(filename, BoundingBox(Point(0, 0),
        scaled_point(Vec2d(3. * view_size, plots_height + view_size + legend_height))), scale_(1.));
    const std::array<std::array<int, 2>, 3> axes{{{{0, 1}}, {{0, 2}}, {{1, 2}}}};
    const std::array<const char *, 3> headings{{"XY / source 3D", "XZ / source 3D", "YZ / source 3D"}};
    const std::array<const char *, 6> colors{{"#0072b2", "#e69f00", "#cc79a7", "#56b4e9", "#a6761d", "#666666"}};
    auto color = [&](size_t chain) { return colors[chain % colors.size()]; };
    auto display = [&](const Vec3d &p, size_t view) {
        return scaled_point(Vec2d(view * view_size + 4. + 4. * (p[axes[view][0]] - minimum[axes[view][0]]),
            plots_height + 4. + 4. * (p[axes[view][1]] - minimum[axes[view][1]])));
    };
    auto record = [&](const std::string &text) {
        BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << text;
        if (svg.is_opened())
            svg.add_comment(text);
    };
    size_t unseparated = 0;
    for (size_t c = 0; c < chains.size(); ++c) {
        const auto &chain = chains[c];
        const bool both = std::find(chain.sides.begin(), chain.sides.end(), 3u) != chain.sides.end();
        unseparated += both;
        std::ostringstream text;
        text << "stage=painted-crease-section-chain run=" << run << " crease=" << crease.id
             << " chart=" << chain.chart << " patch_seed=" << chain.patch << " source_chart=" << chain.source_chart
             << " points=" << chain.samples.size() << " both_sides_in_patch=" << both
             << " blue_point_fallback=" << chain.blue_point_fallback << " painted_spans=" << chain.cap_spans.size();
        record(text.str());
        for (size_t span = 0; span < chain.cap_spans.size(); ++span)
            record(painted_crease_cap_record(run, chain.chart, crease.id, span, chain.cap_spans[span]));
        for (const auto &sample : chain.samples) {
            const Vec3d &point = sample.source.surface.point;
            std::ostringstream detail;
            detail << "stage=painted-crease-section-point run=" << run << " crease=" << crease.id
                   << " chart=" << chain.chart << " path=" << sample.path << " point=" << sample.point
                   << " position_mm=" << sample.position
                   << " x=" << point.x() << " y=" << point.y() << " z=" << point.z()
                   << " source_face=" << sample.source.surface.face << " full_width_mm=" << sample.source.width;
            record(detail.str());
        }
        if (svg.is_opened()) {
            const std::string label = "C" + std::to_string(chain.chart) + " patch=" + std::to_string(chain.patch) +
                " points=" + std::to_string(chain.samples.size()) +
                (chain.blue_point_fallback ? " BLUE-POINT" : "") + (both ? " (SIDES NOT SEPARATED)" : "");
            svg.draw_text(scaled_point(Vec2d(2., plots_height + view_size + 4. + 3. * c)),
                label.c_str(), color(c), 1);
        }
    }
    if (svg.is_opened()) {
        svg.add_comment("Gray=source 3D crease; yellow=common painted coverage; "
                        "pink=fused cap-trimmed candidate; green=accepted line intent; magenta=point intent. "
                        "Chart colors identify selected source-surface chains. M labels identify midpoint sections. "
                        "Distance plots use mm; symmetry difference is at most 100% of the mean. "
                        "Failed sections split intents; an isolated passing section produces a point. "
                        "Source chains precede consumption; accepted intents feed independent fin inputs.");
        for (size_t view = 0; view < axes.size(); ++view) {
            svg.draw_text(scaled_point(Vec2d(view * view_size, plots_height + view_size)),
                headings[view], "#202020", 1);
            Polyline line;
            for (const Vec3d &p : crease.surface_points)
                line.points.push_back(display(p, view));
            svg.draw(line, "#888888", scale_(0.16));
            for (size_t c = 0; c < chains.size(); ++c) {
                line.points.clear();
                for (const auto &sample : chains[c].samples) {
                    const Point xy = display(sample.source.surface.point, view);
                    line.points.push_back(xy);
                    svg.draw(xy, color(c), scale_(0.15));
                    const std::string label = "C" + std::to_string(chains[c].chart) + ":P" +
                        std::to_string(sample.path) + "." + std::to_string(sample.point);
                    svg.draw_text(xy, label.c_str(), color(c), 1);
                }
                if (line.points.size() >= 2)
                    svg.draw(line, color(c), scale_(0.16));
            }
        }
        if (pairs.empty())
            svg.draw_text(scaled_point(Vec2d(2., 30.)),
                "No common painted crease span on distinct opposite patches; no paired candidate.",
                "#d55e00", 1);
    }
    size_t measured = 0, unique_pairs = 0, missing_sides = 0, ambiguous_sides = 0;
    size_t accepted_sections = 0, line_intents = 0, point_intents = 0;
    size_t point_candidates = 0, line_candidates = 0, unavailable_candidates = 0;
    for (size_t p = 0; p < pairs.size(); ++p) {
        const PaintedCreasePair &pair = pairs[p];
        const bool line_candidate = !pair.point &&
            chains[pair.chains[0]].cap_spans[pair.spans[0]].kind == PaintedCreaseCapSpan::Kind::trimmed &&
            chains[pair.chains[1]].cap_spans[pair.spans[1]].kind == PaintedCreaseCapSpan::Kind::trimmed;
        point_candidates += pair.point;
        line_candidates += line_candidate;
        unavailable_candidates += !pair.point && !line_candidate;
        std::ostringstream candidate;
        candidate << "stage=painted-crease-candidate run=" << run << " crease=" << crease.id << " pair=" << p
                  << " chart0=" << chains[pair.chains[0]].chart << " chart1=" << chains[pair.chains[1]].chart
                  << " span0=" << pair.spans[0] << " span1=" << pair.spans[1]
                  << " result=" << pair.result << " raw_begin_mm=" << pair.raw_begin << " raw_end_mm=" << pair.raw_end;
        if (pair.point || line_candidate) {
            const Vec3d begin = painted_crease_surface_point(crease, pair.begin);
            const Vec3d end = painted_crease_surface_point(crease, pair.end);
            candidate << " begin_mm=" << pair.begin << " end_mm=" << pair.end
                      << " begin_x=" << begin.x() << " begin_y=" << begin.y() << " begin_z=" << begin.z()
                      << " end_x=" << end.x() << " end_y=" << end.y() << " end_z=" << end.z();
        }
        record(candidate.str());
        const double bottom = 45. * (pairs.size() - p - 1) + 4.;
        if (svg.is_opened()) {
            auto draw_span = [&](double begin, double end, const char *stroke, double width) {
                for (size_t segment = 0; segment < crease.segments.size(); ++segment) {
                    const double lo = std::max(begin, crease.surface_lengths[segment]);
                    const double hi = std::min(end, crease.surface_lengths[segment + 1]);
                    if (hi - lo <= EPSILON)
                        continue;
                    for (size_t view = 0; view < axes.size(); ++view)
                        svg.draw(Line(display(painted_crease_surface_point(crease, lo), view),
                                      display(painted_crease_surface_point(crease, hi), view)), stroke, scale_(width));
                }
            };
            draw_span(pair.raw_begin, pair.raw_end, "#e69f00", 0.32);
            if (line_candidate)
                draw_span(pair.begin, pair.end, "#cc79a7", 0.12);
            if (pair.sections.empty()) {
                const std::string label = "PAIR " + std::to_string(p) + ": C" +
                    std::to_string(chains[pair.chains[0]].chart) + " / C" +
                    std::to_string(chains[pair.chains[1]].chart) + " " + pair.result + "; no section measurement";
                svg.draw_text(scaled_point(Vec2d(2., bottom + 25.)), label.c_str(), "#202020", 1);
            }
        }
        for (size_t i = 0; i < pair.intents.size(); ++i) {
            const auto &intent = pair.intents[i];
            const bool point = intent.points.size() == 1;
            point_intents += point;
            line_intents += !point;
            const std::string label = "I" + std::to_string(p) + "." + std::to_string(i);
            std::ostringstream detail;
            detail << "stage=painted-crease-intent run=" << run << " crease=" << crease.id
                   << " intent=" << label << " kind=" << (point ? "point" : "line")
                   << " begin_mm=" << intent.begin << " end_mm=" << intent.end
                   << " first_section=" << intent.first_section << " section_count=" << intent.section_count
                   << " vertices=" << intent.points.size();
            record(detail.str());
            for (size_t vertex = 0; vertex < intent.points.size(); ++vertex) {
                const Vec3d &xyz = intent.points[vertex];
                std::ostringstream coordinates;
                coordinates << "stage=painted-crease-intent-vertex run=" << run << " crease=" << crease.id
                            << " intent=" << label << " vertex=" << vertex
                            << " x=" << xyz.x() << " y=" << xyz.y() << " z=" << xyz.z();
                record(coordinates.str());
            }
            if (svg.is_opened())
                for (size_t view = 0; view < axes.size(); ++view) {
                    Polyline line;
                    for (const Vec3d &xyz : intent.points)
                        line.points.push_back(display(xyz, view));
                    if (point)
                        svg.draw(line.first_point(), "#cc0099", scale_(0.45));
                    else
                        svg.draw(line, "#009e73", scale_(0.24));
                    svg.draw_text(line.first_point(), (label + (point ? " POINT" : " LINE")).c_str(),
                                  point ? "#cc0099" : "#009e73", 1);
                }
        }
        if (pair.sections.empty())
            continue;
        double maximum_distance = EPSILON;
        for (const PaintedCreaseSection &section : pair.sections)
            for (const auto &side : section.sides)
                for (const auto &hit : side.hits)
                    maximum_distance = std::max(maximum_distance, (hit.point - section.origin).norm());
        const double left = pair.begin, right = pair.end;
        auto plot = [&](double position, double distance) {
            return scaled_point(Vec2d(8. + (3. * view_size - 16.) * (position - left) / (right - left),
                bottom + 27. * distance / maximum_distance));
        };
        if (svg.is_opened()) {
            const std::string title = "PAIR " + std::to_string(p) + ": C" +
                std::to_string(chains[pair.chains[0]].chart) + " / C" +
                std::to_string(chains[pair.chains[1]].chart) + "   s=[" + std::to_string(left) + "," +
                std::to_string(right) + "] mm; distance=[0," + std::to_string(maximum_distance) +
                "] mm; red=failed section (100% / missing / ambiguous)";
            svg.draw_text(scaled_point(Vec2d(2., bottom + 35.)), title.c_str(), "#202020", 1);
            svg.draw(Line(plot(left, 0.), plot(right, 0.)), "#aaaaaa", scale_(0.08));
            svg.draw(Line(plot(left, 0.), plot(left, maximum_distance)), "#aaaaaa", scale_(0.08));
            for (int tick = 0; tick <= 4; ++tick) {
                const double position = left + 0.25 * tick * (right - left);
                const double distance = 0.25 * tick * maximum_distance;
                svg.draw(Line(plot(left, distance), plot(right, distance)), "#dddddd", scale_(0.04));
                const std::string x_label = std::to_string(position), y_label = std::to_string(distance);
                svg.draw_text(plot(position, -0.08 * maximum_distance), x_label.c_str(), "#666666", 1);
                svg.draw_text(plot(left, distance), y_label.c_str(), "#666666", 1);
            }
        }
        for (size_t s = 0; s < pair.sections.size(); ++s) {
            if (object.print()->canceled())
                throw CanceledException();
            const PaintedCreaseSection &section = pair.sections[s];
            const std::string label = "M" + std::to_string(p) + "." + std::to_string(s);
            ++measured;
            accepted_sections += section.accepted();
            unique_pairs += section.sides[0].unique() && section.sides[1].unique();
            std::ostringstream text;
            text << "stage=painted-crease-section run=" << run << " crease=" << crease.id
                 << " sample=" << label << " segment=" << section.segment
                 << " begin_mm=" << section.begin << " end_mm=" << section.end << " position_mm=" << section.position
                 << " origin_x=" << section.origin.x() << " origin_y=" << section.origin.y() << " origin_z=" << section.origin.z()
                 << " normal_x=" << section.normal.x() << " normal_y=" << section.normal.y() << " normal_z=" << section.normal.z()
                 << " accepted=" << section.accepted();
            for (size_t side = 0; side < 2; ++side) {
                const auto &hits = section.sides[side];
                missing_sides += hits.hits.empty() && hits.coplanar.empty();
                ambiguous_sides += !hits.coplanar.empty() || hits.hits.size() > 1;
                text << " chart" << side << "=" << chains[pair.chains[side]].chart
                     << " status" << side << "=" << hits.status()
                     << " hits" << side << "=" << hits.hits.size()
                     << " coplanar" << side << "=" << hits.coplanar.size();
                if (hits.unique())
                    text << " distance" << side << "_mm=" << (hits.hits.front().point - section.origin).norm();
            }
            if (section.sides[0].unique() && section.sides[1].unique()) {
                const double a = (section.sides[0].hits.front().point - section.origin).norm();
                const double b = (section.sides[1].hits.front().point - section.origin).norm();
                text << " difference_mm=" << std::abs(a - b) << " limit_mm=" << 0.5 * (a + b);
            }
            record(text.str());
            if (svg.is_opened()) {
                for (size_t view = 0; view < axes.size(); ++view) {
                    const char *color = section.accepted() ? "#202020" : "#d55e00";
                    svg.draw(display(section.origin, view), color, scale_(0.15));
                    svg.draw_text(display(section.origin, view), label.c_str(), color, 1);
                }
                svg.draw_text(plot(section.position, maximum_distance), label.c_str(),
                              section.accepted() ? "#202020" : "#d55e00", 1);
            }
            for (size_t side = 0; side < 2; ++side) {
                const auto &chain = chains[pair.chains[side]];
                const auto &hits = section.sides[side];
                const char *hit_color = hits.unique() ? color(pair.chains[side]) : "#d55e00";
                for (const auto &hit : hits.hits) {
                    std::ostringstream detail;
                    detail << "stage=painted-crease-section-hit run=" << run << " crease=" << crease.id
                           << " sample=" << label << " chart=" << chain.chart << " edge=" << hit.edge
                           << " fraction=" << hit.fraction << " x=" << hit.point.x() << " y=" << hit.point.y()
                           << " z=" << hit.point.z() << " distance_mm=" << (hit.point - section.origin).norm();
                    record(detail.str());
                    if (svg.is_opened()) {
                        svg.draw(plot(section.position, (hit.point - section.origin).norm()), hit_color, scale_(0.18));
                        for (size_t view = 0; view < axes.size(); ++view) {
                            svg.draw(Line(display(section.origin, view), display(hit.point, view)), hit_color, scale_(0.06));
                            svg.draw(display(hit.point, view), hit_color, scale_(0.13));
                        }
                    }
                }
                for (const size_t edge : hits.coplanar) {
                    const Vec3d &a = chain.samples[edge].source.surface.point;
                    const Vec3d &b = chain.samples[edge + 1].source.surface.point;
                    std::ostringstream detail;
                    detail << "stage=painted-crease-section-coplanar run=" << run << " crease=" << crease.id
                           << " sample=" << label << " chart=" << chain.chart << " edge=" << edge
                           << " ax=" << a.x() << " ay=" << a.y() << " az=" << a.z()
                           << " bx=" << b.x() << " by=" << b.y() << " bz=" << b.z();
                    record(detail.str());
                    if (svg.is_opened())
                        for (size_t view = 0; view < axes.size(); ++view)
                            svg.draw(Line(display(a, view), display(b, view)), "#d55e00", scale_(0.25));
                }
                if (svg.is_opened()) {
                    if (!hits.unique()) {
                        const std::string status = label + "/C" + std::to_string(chain.chart) + ":" + hits.status();
                        svg.draw_text(plot(section.position, (0.12 + 0.12 * side) * maximum_distance),
                            status.c_str(), "#d55e00", 1);
                    } else if (s > 0 && pair.sections[s - 1].sides[side].unique() &&
                               section.begin <= pair.sections[s - 1].end + EPSILON) {
                        const PaintedCreaseSection &previous = pair.sections[s - 1];
                        svg.draw(Line(plot(previous.position, (previous.sides[side].hits.front().point - previous.origin).norm()),
                                      plot(section.position, (hits.hits.front().point - section.origin).norm())),
                            hit_color, scale_(0.14));
                    }
                }
            }
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=painted-crease-sections run=" << run << " crease=" << crease.id
        << " charts=" << chains.size() << " unseparated_charts=" << unseparated
        << " pairs=" << pairs.size() << " sections=" << measured << " unique_pairs=" << unique_pairs
        << " line_candidates=" << line_candidates << " point_candidates=" << point_candidates
        << " unavailable_candidates=" << unavailable_candidates
        << " missing_sides=" << missing_sides << " ambiguous_sides=" << ambiguous_sides
        << " accepted_sections=" << accepted_sections << " rejected_sections=" << measured - accepted_sections
        << " line_intents=" << line_intents << " point_intents=" << point_intents
        << " symmetry_filter=true symmetry_limit=1.0 cap_trim=true svg_opened=" << svg.is_opened() << " svg=" << filename;
}

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
    std::vector<CoarseIntentPath> paths;
    size_t compact_components = 0;
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
                                             double feature_scale,
                                             std::vector<PaintedBlueDiagnostic::Compact> *compact_diagnostics)
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

    // Classify compact components and protect remaining axes before pruning
    // local branches. Two Dijkstra sweeps recover a tree's exact diameter and
    // a stable main route for
    // the rare cyclic component. Width is measured before axis simplification;
    // the cap allowance here is only a classification, not a geometry trim.
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
        std::vector<size_t> axis_nodes{second};
        std::vector<size_t> axis_edges;
        for (size_t node = second; node != first;) {
            const size_t edge = predecessor[node];
            if (edge == no_edge)
                break;
            axis_edges.push_back(edge);
            node = other_node(edge, node);
            axis_nodes.push_back(node);
        }
        std::reverse(axis_nodes.begin(), axis_nodes.end());
        std::reverse(axis_edges.begin(), axis_edges.end());

        double axis_length = 0., axis_width_moment = 0.;
        std::vector<Vec2d> axis_points;
        axis_points.reserve(axis_nodes.size());
        for (const size_t node : axis_nodes)
            axis_points.push_back(unscale(nodes[node].point));
        std::vector<double> axis_widths(axis_nodes.size(), 0.);
        for (size_t i = 0; i < axis_edges.size(); ++i) {
            const size_t edge = axis_edges[i];
            const double length = edge_length(edge);
            const double from_width = unscale<double>(edges[edge].widths[side_at(edge, axis_nodes[i])]);
            const double to_width = unscale<double>(edges[edge].widths[side_at(edge, axis_nodes[i + 1])]);
            axis_length += length;
            axis_width_moment += 0.5 * length * (from_width + to_width);
            axis_widths[i] += from_width;
            axis_widths[i + 1] += to_width;
        }
        for (size_t i = 1; i + 1 < axis_widths.size(); ++i)
            axis_widths[i] *= 0.5;
        const double axis_width = axis_length > EPSILON ? axis_width_moment / axis_length : 0.;

        // Simplify only the representative axis for classification. Keep its
        // original width integral and leave surviving skeleton geometry intact.
        std::vector<bool> retained(axis_points.size(), false);
        retained.front() = retained.back() = true;
        std::vector<std::pair<size_t, size_t>> pending;
        if (axis_points.size() > 2)
            pending.emplace_back(0, axis_points.size() - 1);
        while (!pending.empty()) {
            const auto [begin, end] = pending.back();
            pending.pop_back();
            if (end <= begin + 1)
                continue;
            const Vec2d chord = axis_points[end] - axis_points[begin];
            double maximum_error = -1.;
            size_t split = begin + 1;
            for (size_t i = begin + 1; i < end; ++i) {
                const double position = chord.squaredNorm() > EPSILON * EPSILON ?
                    std::clamp((axis_points[i] - axis_points[begin]).dot(chord) /
                        chord.squaredNorm(), 0., 1.) : 0.;
                const double tolerance = 0.5 * std::max(feature_scale, axis_widths[i]);
                const double error = (axis_points[i] - (axis_points[begin] + position * chord)).norm() /
                    std::max(tolerance, EPSILON);
                if (error > maximum_error) {
                    maximum_error = error;
                    split = i;
                }
            }
            if (maximum_error <= 1. + EPSILON && painted_area.contains(
                    Line(scaled_point(axis_points[begin]), scaled_point(axis_points[end]))))
                continue;
            retained[split] = true;
            pending.emplace_back(begin, split);
            pending.emplace_back(split, end);
        }
        double fitted_length = 0.;
        size_t previous = 0;
        for (size_t i = 1; i < axis_points.size(); ++i)
            if (retained[i]) {
                fitted_length += (axis_points[i] - axis_points[previous]).norm();
                previous = i;
            }
        const double threshold = std::max(feature_scale, axis_width);
        const bool compact = axis_edges.empty() || std::max(0., fitted_length - axis_width) + EPSILON < threshold;
        if (compact_diagnostics != nullptr)
            compact_diagnostics->push_back({fitted_length, axis_width, threshold, compact});
        if (compact) {
            for (const size_t node : component_nodes)
                for (const size_t edge : nodes[node].edges)
                    edges[edge].active = false;
            ++result.compact_components;
            continue;
        }
        for (const size_t edge : axis_edges)
            edges[edge].protected_axis = true;
    }

    // Repeatedly remove only side branches shorter than their length-weighted
    // mean width. A single broad junction sample must not erase an otherwise
    // significant stroke. The protected diameter can become a leaf as
    // neighboring noise disappears but is never consumed.
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
        if (branch.empty() || length + EPSILON >= std::max(feature_scale, average_width))
            continue;
        for (const size_t edge : branch)
            edges[edge].active = false;
        ++result.pruned_branches;
        if (stop != no_edge && active_degree(stop) == 1)
            leaves.push(stop);
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

// Area and first moments in planar coordinates; second moments about a nearby origin.
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

PaintedIntentGraph solve_painted_blue_paths(
    const PrintObject &object, const std::vector<std::vector<PaintedIntentSample>> &retained_blue_paths,
    const Vec3d &macro_normal, double feature_scale, PaintedSides &result)
{
    using IntentSample = PaintedIntentSample;
    PaintedIntentGraph intent;
    intent.macro_normal = macro_normal;
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
    // Build downstream nodes solely from retained samples. A shared
    // vertex must not inherit its measurement from a discarded path.
    for (const auto &samples : retained_blue_paths) {
        auto append_path = [&](std::vector<IntentSample> oriented) {
            if (oriented.size() < 2)
                return;
            const size_t first = blue_node(oriented.front());
            const size_t last = blue_node(oriented.back());
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
            intent.nodes[node].attachment_host_edge = previous;
            stroke.attachment_host_edge = previous;
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
        const double denominator = cross2(direction, along);
        if (std::abs(denominator) <= 1e-12)
            return std::nullopt;
        const double distance = cross2(host.xy[0] - origin, along) / denominator;
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
        if (attachment.side == 0 || !child.attachment_host_edge)
            child.attachment_host_edge = main;
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
                intent.nodes[node].attachment_host_edge = first_main;
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
                intent.nodes[node].attachment_host_edge = first_main;
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
                    intent.nodes[node].attachment_host_edge = main;
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
    return intent;
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
    const double contact_width = fin_support_dimensions(object).contact_width;
    const double feature_scale = std::max(3 * line_width, contact_width + line_width);
    const bool export_crease_svg = split_painted_islands && get_logging_level() >= 3;
    static std::atomic_size_t next_crease_svg_run{0};
    const size_t crease_svg_run = export_crease_svg ? next_crease_svg_run.fetch_add(1) : 0;
    result.crease_run = crease_svg_run;
    size_t next_diagnostic_crease = 0;
    std::map<size_t, PaintedCreaseComparison> crease_comparisons;
    struct BlueChart {
        size_t island;
        Vec3d macro_normal;
        std::vector<std::vector<PaintedIntentSample>> paths;
        std::optional<PaintedIntentPoint> point;
        std::unordered_set<Point, PointHash> consumed;
    };
    std::vector<BlueChart> blue_charts;
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

        struct SourceIsland {
            std::vector<size_t> faces;
            double area = 0.;
        };
        std::vector<SourceIsland> source_islands;
        std::vector<bool> eligible_faces;
        if (split_painted_islands) {
            // Qualify complete islands before crease extraction and geodesic
            // normal averaging. Edge neighbors cannot cross these
            // vertex-connected islands.
            const double minimum_area = (contact_width + line_width) *
                object.config().fin_support_contact_height.value;
            eligible_faces.assign(facets.indices.size(), false);
            std::vector<std::vector<size_t>> vertex_faces(facets.vertices.size());
            for (size_t face_id = 0; face_id < facets.indices.size(); ++face_id)
                for (int vertex : facets.indices[face_id])
                    vertex_faces[size_t(vertex)].push_back(face_id);
            std::vector<bool> visited(facets.indices.size(), false);
            std::vector<size_t> pending;
            for (size_t seed = 0; seed < facets.indices.size(); ++seed) {
                if ((seed & 63) == 0 && object.print()->canceled())
                    throw CanceledException();
                if (visited[seed])
                    continue;
                SourceIsland island;
                pending.push_back(seed);
                visited[seed] = true;
                while (!pending.empty()) {
                    const size_t face_id = pending.back();
                    pending.pop_back();
                    island.faces.push_back(face_id);
                    island.area += geometry[face_id].area;
                    for (int vertex : facets.indices[face_id])
                        for (const size_t neighbor : vertex_faces[size_t(vertex)])
                            if (!visited[neighbor]) {
                                visited[neighbor] = true;
                                pending.push_back(neighbor);
                            }
                }
                if (island.area < minimum_area) {
                    ++result.undersized_painted_islands;
                    continue;
                }
                for (const size_t face : island.faces)
                    eligible_faces[face] = true;
                source_islands.push_back(std::move(island));
            }
            if (source_islands.empty())
                continue;
        } else {
            SourceIsland island;
            island.faces.resize(facets.indices.size());
            std::iota(island.faces.begin(), island.faces.end(), size_t(0));
            for (const FaceGeometry &face : geometry)
                if (face.area > 0 && face.normal.head<2>().norm() >= 0.5)
                    island.area += face.area;
            source_islands.push_back(std::move(island));
        }

        struct PersistentCreasePath {
            std::vector<Vec3d> points;
            size_t diagnostic_id = 0;
            std::vector<PaintedCreaseProbePath::Segment> diagnostic_segments;
        };
        std::vector<PersistentCreasePath> persistent_creases;
        std::vector<Vec3d> macro_normals(geometry.size(), Vec3d::Zero());
        std::vector<Vec3i32> neighbors;
        std::vector<std::array<bool, 3>> barriers(facets.indices.size());
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
                if (!eligible_faces[face_id] || geometry[face_id].area <= 0)
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
                    path.diagnostic_id = next_diagnostic_crease++;
                    path.points.push_back((transform * facets.vertices[size_t(start_vertex)]).cast<double>());
                    int vertex = start_vertex;
                    size_t edge_id = first_edge;
                    while (!path_edge_visited[edge_id]) {
                        path_edge_visited[edge_id] = true;
                        const SharpEdge &edge = sharp_edges[edge_id];
                        const int next_vertex = edge.a == vertex ? edge.b : edge.a;
                        path.points.push_back((transform * facets.vertices[size_t(next_vertex)]).cast<double>());
                        {
                            // On a convex edge the neighbor lies behind the
                            // outward plane of this face. Only convex segments
                            // participate in paired crease intent construction.
                            const bool convex = geometry[edge.face].normal.dot(
                                geometry[edge.neighbor].center - geometry[edge.face].center) < 0.;
                            path.diagnostic_segments.push_back({
                                {edge.face, edge.neighbor}, 0, convex,
                                {geometry[edge.face].normal, geometry[edge.neighbor].normal}});
                        }
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
                if (!eligible_faces[seed] || geometry[seed].area <= 0)
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

        auto prepare_blue_charts = [&](const std::vector<size_t> &faces, const Vec3d &center) {
            if (object.print()->canceled())
                throw CanceledException();
            PaintedIntentGraph intent;
            auto log_probe_skip = [&](const char *reason) {
                if (export_crease_svg)
                    BOOST_LOG_TRIVIAL(info)
                        << "Fin support object=" << object.model_object()->name
                        << " stage=painted-crease-probes run=" << crease_svg_run
                        << " patch_seed=" << faces.front()
                        << " skipped=" << reason << " svg_opened=false";
            };
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
            if (surface_area <= EPSILON || normal_sum.norm() <= 0.25 * surface_area) {
                ++result.unreliable_intent_charts;
                log_probe_skip("unreliable-chart-normal");
                return;
            }

            const Vec3d chart_normal = normal_sum.normalized();
            const Vec3d reference = std::abs(chart_normal.z()) < 0.9 ? Vec3d::UnitZ() : Vec3d::UnitX();
            const Vec3d chart_u = reference.cross(chart_normal).normalized();
            const Vec3d chart_v = chart_normal.cross(chart_u).normalized();
            std::vector<PaintedCreaseProbePath> diagnostic_creases;
            {
                for (const auto &crease : persistent_creases) {
                    PaintedCreaseProbePath projected{crease.diagnostic_id, {}, crease.diagnostic_segments,
                        crease.points, {0.}};
                    bool touches_patch = false;
                    for (auto &segment : projected.segments) {
                        for (size_t side = 0; side < 2; ++side)
                            if (std::find(faces.begin(), faces.end(), segment.faces[side]) != faces.end())
                                segment.patch_sides |= 1u << side;
                        touches_patch |= segment.patch_sides != 0;
                    }
                    if (!touches_patch)
                        continue;
                    for (const Vec3d &point : crease.points) {
                        const Vec3d relative = point - center;
                        projected.points.emplace_back(relative.dot(chart_u), relative.dot(chart_v));
                    }
                    for (size_t i = 1; i < crease.points.size(); ++i)
                        projected.surface_lengths.push_back(projected.surface_lengths.back() +
                            (crease.points[i] - crease.points[i - 1]).norm());
                    diagnostic_creases.push_back(std::move(projected));
                }
            }
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
                log_probe_skip("empty-projection");
                return;
            }

            // Extract the skeleton directly from the projected painted facets.
            const ExPolygons chart = union_ex(triangles);
            double chart_area = 0;
            for (const ExPolygon &area : chart)
                chart_area += std::abs(area.area()) * SCALING_FACTOR * SCALING_FACTOR;
            // Strong overlap indicates that one orthographic chart folded the
            // surface onto itself. Keep point intent rather than inventing a line.
            if (chart_area < 0.7 * projected_area) {
                ++result.unreliable_intent_charts;
                log_probe_skip("projection-overlap");
                return;
            }

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
                PaintedBlueDiagnostic blue_diagnostic;
                CoarseIntentSkeleton coarse = coarse_intent_skeleton(
                    skeleton, measurements, area, feature_scale,
                    export_crease_svg ? &blue_diagnostic.components : nullptr);
                result.compact_intent_components += coarse.compact_components;
                result.intent_junctions += coarse.junctions;
                result.paired_intent_junctions += coarse.paired_junctions;
                result.pruned_intent_branches += coarse.pruned_branches;
                result.pruned_intent_strokes += coarse.pruned_branches;
                result.joined_intent_paths += coarse.joined_paths;
                result.logical_intent_paths += coarse.paths.size();
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
                    // If no blue path survives, this chart emits its area-center point.
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
                    // degree-one graph ends. Stop at the first junction: an
                    // exhausted terminal span must not consume the next branch.
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
                    const double front_trim = path.boundary_front ? 0.5 * original_average_width : 0.;
                    const double back_trim = path.boundary_back ? 0.5 * original_average_width : 0.;
                    size_t core_begin = 0;
                    size_t core_end = samples.size(); // Exclusive; both trims use the original path.
                    while (core_begin < samples.size() && !samples[core_begin].junction &&
                           arc_length[core_begin] + EPSILON < front_trim)
                        ++core_begin;
                    const double back_limit = original_length - back_trim;
                    while (core_end > 0 && !samples[core_end - 1].junction &&
                           arc_length[core_end - 1] > back_limit + EPSILON)
                        --core_end;
                    if (export_crease_svg)
                        blue_diagnostic.paths.push_back({samples, front_trim, back_trim,
                            core_begin, std::max(core_begin, core_end)});
                    // Keep only original samples inside the surviving interval.
                    // A lone junction stays available through its other paths,
                    // but neither it nor a lone free sample defines a line here.
                    if (core_end <= core_begin || core_end - core_begin < 2) {
                        result.trimmed_intent_samples += samples.size();
                        continue;
                    }
                    if (core_begin > 0 || core_end < samples.size()) {
                        result.trimmed_intent_samples += core_begin + samples.size() - core_end;
                        samples = std::vector<IntentSample>(samples.begin() + core_begin,
                                                            samples.begin() + core_end);
                    }

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
                    for (size_t i = 1; i < samples.size(); ++i) {
                        const double length =
                            (samples[i].surface.point - samples[i - 1].surface.point).norm();
                        weighted_length += length;
                    }
                    if (weighted_length <= EPSILON)
                        continue;

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
                // No surviving line is also an explicit point classification,
                // including paths exhausted by cap trimming before grouping.
                bool blue_point_fallback = blue_groups.empty();
                if (longest_blue_group != no_blue_group) {
                    const BlueGroup &longest = blue_groups[longest_blue_group];
                    blue_group_width = longest.width_moment / longest.length;
                    blue_point_fallback = longest.length < blue_group_width;
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

                // Point placement belongs to the blue output, at the same chart
                // scope as retained paths. Never reconstruct it from island geometry.
                blue_point_fallback = retained_blue_paths.empty();
                result.point_blue_intent_charts += blue_point_fallback;
                if (blue_point_fallback) {
                    const SectionMoments moments = area_moments(ExPolygons{area});
                    if (moments.area > 0.) {
                        PaintedIntentPoint point;
                        point.chart = moments.first / moments.area;
                        point.surface = to_surface(point.chart);
                        point.source = {faces.front(), chart_id, scaled_point(point.chart)};
                        point.macro_normal = intent.macro_normal;
                        point.area = moments.area;
                        if (point.surface.face != std::numeric_limits<size_t>::max())
                            blue_diagnostic.output_point = point;
                    }
                }

                {
                    blue_diagnostic.groups = blue_groups.size();
                    blue_diagnostic.longest_length = longest_blue_group == no_blue_group ?
                        0. : blue_groups[longest_blue_group].length;
                    blue_diagnostic.group_width = blue_group_width;
                    blue_diagnostic.point = blue_point_fallback;
                    const PaintedCreaseSelection selection =
                        select_painted_crease_points(object, retained_blue_paths, diagnostic_creases);
                    const auto cap_spans = painted_crease_cap_spans(
                        object, area, diagnostic_creases, selection, blue_point_fallback);
                    const size_t diagnostic_chart = blue_charts.size();
                    if (export_crease_svg)
                        export_painted_crease_probes(object, crease_svg_run, diagnostic_chart,
                            faces.front(), chart_id, area, skeleton, retained_blue_paths, diagnostic_creases, selection, cap_spans,
                            blue_diagnostic);
                    collect_painted_crease_comparison(diagnostic_chart, faces.front(), chart_id,
                        diagnostic_creases, selection, cap_spans, blue_point_fallback, crease_comparisons);
                }

                blue_charts.push_back({result.islands.size(), intent.macro_normal,
                    std::move(retained_blue_paths), std::move(blue_diagnostic.output_point)});
            }
            return;
        };

        auto append_island = [&](const SourceIsland &source) {
            const auto &faces = source.faces;
            const size_t island_id = result.islands.size();
            PaintedIsland island;
            island.area = source.area;
            for (const size_t face_id : faces) {
                const FaceGeometry &face = geometry[face_id];
                if (face.area <= 0)
                    continue;
                const std::array<Vec3d, 3> &v = face.vertices;
                const Vec3d &normal3 = face.normal;
                Vec2d normal = normal3.head<2>();
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
                if (split_painted_islands) {
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
                        prepare_blue_charts(patch_faces, patch_first_moment / patch_area);
                    }
                }
                result.islands.push_back(island);
            }
        };
        for (const SourceIsland &island : source_islands)
            append_island(island);
    }
    // Pair all source charts before ordinary green fitting. Consumption is
    // committed when source points enter section comparison, even if no intent
    // survives. It is never undone by symmetry or later geometry failures.
    for (const auto &entry : crease_comparisons) {
        if (object.print()->canceled())
            throw CanceledException();
        const auto &comparison = entry.second;
        const auto pairs = compare_painted_crease_sides(object, comparison);
        for (size_t p = 0; p < pairs.size(); ++p) {
            const auto &pair = pairs[p];
            if (!pair.point && pair.sections.empty())
                continue;
            for (const size_t side : pair.chains) {
                const auto &chain = comparison.chains[side];
                auto &chart = blue_charts[chain.chart];
                for (const auto &sample : chain.samples)
                    chart.consumed.insert(sample.source.source.point);
                if (chain.blue_point_fallback)
                    chart.point.reset();
            }
            const size_t island = blue_charts[comparison.chains[pair.chains[0]].chart].island;
            auto &graph = result.islands[island].intent;
            for (size_t i = 0; i < pair.intents.size(); ++i) {
                const auto &output = pair.intents[i];
                PaintedCreaseIntent intent;
                intent.crease = comparison.crease.id;
                intent.pair = p;
                intent.part = i;
                intent.points = output.points;
                if (output.points.size() > 1) {
                    intent.contact_source = output.points;
                } else {
                    // Retain the source interval that produced this point, not
                    // an invented line body or a nearby-corner search region.
                    const double lo = pair.point ? pair.raw_begin : pair.sections[output.first_section].begin;
                    const double hi = pair.point ? pair.raw_end : pair.sections[output.first_section].end;
                    intent.contact_source.push_back(painted_crease_surface_point(comparison.crease, lo));
                    for (size_t v = 1; v + 1 < comparison.crease.surface_points.size(); ++v)
                        if (comparison.crease.surface_lengths[v] > lo && comparison.crease.surface_lengths[v] < hi)
                            intent.contact_source.push_back(comparison.crease.surface_points[v]);
                    intent.contact_source.push_back(painted_crease_surface_point(comparison.crease, hi));
                }
                if (intent.points.back().z() > intent.points.front().z())
                    std::reverse(intent.points.begin(), intent.points.end());
                const auto top = std::max_element(intent.points.begin(), intent.points.end(),
                    [](const Vec3d &a, const Vec3d &b) { return a.z() < b.z(); });
                // Select the source segment at the highest retained vertex.
                // Source arc positions, never XY reprojection, locate its faces.
                double top_position = output.begin;
                if (top == intent.points.begin())
                    top_position = intent.points.front() == output.points.front() ? output.begin : output.end;
                else if (top == intent.points.end() - 1)
                    top_position = intent.points.back() == output.points.back() ? output.end : output.begin;
                else {
                    for (size_t v = 0; v < comparison.crease.surface_points.size(); ++v)
                        if (comparison.crease.surface_points[v] == *top) {
                            top_position = comparison.crease.surface_lengths[v];
                            break;
                        }
                }
                const auto at = painted_crease_position(comparison.crease.surface_lengths, top_position);
                intent.root_normals = comparison.crease.segments[at.segment].normals;
                for (const Vec3d &point : intent.points) {
                    intent.nodes.push_back(graph.nodes.size());
                    graph.nodes.push_back({point.head<2>(), point.z(), {}, std::nullopt});
                }
                graph.creases.push_back(std::make_shared<const PaintedCreaseIntent>(std::move(intent)));
                ++result.crease_intent_strokes;
            }
        }
        if (export_crease_svg)
            export_painted_crease_sections(object, crease_svg_run, comparison, pairs);
    }
    for (size_t chart_id = 0; chart_id < blue_charts.size(); ++chart_id) {
        auto &chart = blue_charts[chart_id];
        auto &graph = result.islands[chart.island].intent;
        std::vector<std::vector<PaintedIntentSample>> remaining;
        size_t isolated_samples = 0;
        for (const auto &path : chart.paths) {
            size_t begin = 0;
            auto emit = [&](size_t end) {
                if (end - begin >= 2)
                    remaining.emplace_back(path.begin() + begin, path.begin() + end);
                else
                    isolated_samples += end - begin;
            };
            for (size_t i = 0; i < path.size(); ++i)
                if (chart.consumed.count(path[i].source.point) != 0) {
                    emit(i);
                    begin = i + 1;
                }
            emit(path.size());
        }
        // Ordinary paths still require two samples. Consumption cannot create
        // a new chart-centroid fallback or a chord across a consumed sample.
        PaintedIntentGraph ordinary = solve_painted_blue_paths(
            object, remaining, chart.macro_normal, feature_scale, result);
        const size_t edge_offset = graph.strokes.size(), node_offset = graph.nodes.size();
        for (auto &node : ordinary.nodes) {
            for (size_t &edge : node.edges)
                edge += edge_offset;
            if (node.attachment_host_edge)
                *node.attachment_host_edge += edge_offset;
            graph.nodes.push_back(std::move(node));
        }
        for (auto &stroke : ordinary.strokes) {
            for (size_t &node : stroke.nodes)
                node += node_offset;
            if (stroke.attachment_host_edge)
                *stroke.attachment_host_edge += edge_offset;
            graph.strokes.push_back(std::move(stroke));
        }
        if (chart.point)
            graph.points.push_back(std::move(*chart.point));
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
            << " stage=painted-crease-consumption run=" << crease_svg_run << " chart=" << chart_id
            << " consumed_points=" << chart.consumed.size() << " remaining_paths=" << remaining.size()
            << " isolated_samples=" << isolated_samples << " ordinary_point=" << chart.point.has_value();
    }
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
        const double denominator = cross2(direction, edge);
        if (std::abs(denominator) <= 1e-12)
            continue;
        const Vec2d delta = a - origin;
        const double distance = cross2(delta, edge) / denominator;
        const double position = cross2(delta, direction) / denominator;
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

// Both source roots and per-layer crease fronts use the oriented horizontal
// model section. Adjacent unit edge normals define the exterior bisector;
// printed-wall coverage and the A1-to-source axis do not rotate it.
std::optional<Vec2d> fin_section_bisector(const ConstPolygonPtrs &sections, const Vec2d &xy,
                                        std::string *diagnostic = nullptr)
{
    std::ostringstream detail;
    detail.precision(17);
    const auto finish = [&](const char *status) {
        if (diagnostic) {
            detail << " section_status=" << status;
            *diagnostic = detail.str();
        }
    };
    std::optional<Vec2d> result;
    for (size_t contour_id = 0; contour_id < sections.size(); ++contour_id) {
        const Points &points = sections[contour_id]->points;
        std::vector<size_t> edges;
        std::vector<Vec2d> normals;
        for (size_t i = 0; i < points.size(); ++i) {
            const Vec2d a = unscale(points[i]);
            const Vec2d delta = unscale(points[(i + 1) % points.size()]) - a;
            const double length = delta.norm();
            if (length <= 0.)
                continue;
            const double fraction = std::clamp((xy - a).dot(delta) / delta.squaredNorm(), 0., 1.);
            if ((xy - a - fraction * delta).squaredNorm() > EPSILON * EPSILON)
                continue;
            edges.push_back(i);
            // Regular slices keep material on the left for both outer
            // contours and holes, so the right normal points out of solid.
            normals.emplace_back(delta.y() / length, -delta.x() / length);
            if (diagnostic)
                detail << " section_edge=" << contour_id << ":" << i << ":"
                       << normals.back().x() << ":" << normals.back().y();
        }
        if (edges.empty())
            continue;
        // One edge is a straight section location. A corner uses exactly
        // its adjacent incoming/outgoing edges, with equal unit weights.
        if (result || edges.size() > 2 ||
            (edges.size() == 2 && (edges[0] + 1) % points.size() != edges[1] &&
                                 (edges[1] + 1) % points.size() != edges[0])) {
            finish("ambiguous");
            return std::nullopt;
        }
        Vec2d sum = normals.front();
        if (normals.size() == 2)
            sum += normals.back();
        if (!sum.allFinite() || sum.norm() <= EPSILON) {
            finish("no-direction");
            return std::nullopt;
        }
        result = sum.normalized();
    }
    finish(result ? "resolved" : "no-boundary");
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

// Positive turn denotes a convex model corner, independent of contour winding.
double fin_contour_turn(const Vec2d &incoming, const Vec2d &outgoing, double orientation)
{
    return std::atan2(orientation * cross2(incoming, outgoing), incoming.dot(outgoing));
}

// Per-run, lazily built wall geometry. A capsule is an extrusion centerline
// segment swept by its actual half width; no layer-wide offset/union is needed.
class FinWallGeometry {
    using Tree = AABBTreeIndirect::Tree<2, double>;
    struct Segment {
        Vec2d a;
        Vec2d b;
        double radius;
    };
    struct LayerWalls {
        std::vector<Segment> segments;
        Tree tree;
    };
    const PrintObject &m_object;
    std::vector<std::unique_ptr<LayerWalls>> m_layers;

    LayerWalls &layer(size_t layer_id)
    {
        if (m_layers[layer_id])
            return *m_layers[layer_id];
        auto walls = std::make_unique<LayerWalls>();
        const auto append_path = [&](const ExtrusionPath &path) {
            if (path.is_force_no_extrusion() || path.width <= 0. || !std::isfinite(path.width))
                return;
            const auto &points = path.polyline.points;
            for (size_t i = 1; i < points.size(); ++i) {
                if (points[i - 1].to_point() == points[i].to_point())
                    continue;
                walls->segments.push_back({points[i - 1].to_point().cast<double>(),
                    points[i].to_point().cast<double>(), 0.5 * double(path.width) / SCALING_FACTOR});
            }
        };
        const auto visit = [&](const auto &self, const ExtrusionEntity &entity) -> void {
            if (m_object.print()->canceled())
                throw CanceledException();
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const ExtrusionEntity *child : collection->entities)
                    self(self, *child);
            } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
                for (const ExtrusionPath &path : loop->paths)
                    append_path(path);
            } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
                for (const ExtrusionPath &path : multi->paths)
                    append_path(path);
            } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
                append_path(*path);
        };
        for (const LayerRegion *region : m_object.get_layer(int(layer_id))->regions())
            visit(visit, region->perimeters);
        struct Input {
            size_t id;
            Tree::BoundingBox bounds;
            Vec2d center;
            size_t idx() const { return id; }
            const Tree::BoundingBox &bbox() const { return bounds; }
            const Vec2d &centroid() const { return center; }
        };
        std::vector<Input> input;
        input.reserve(walls->segments.size());
        for (size_t i = 0; i < walls->segments.size(); ++i) {
            const Segment &segment = walls->segments[i];
            const Vec2d radius = Vec2d::Constant(segment.radius + SCALED_EPSILON);
            input.push_back({i, Tree::BoundingBox(segment.a.cwiseMin(segment.b) - radius,
                segment.a.cwiseMax(segment.b) + radius), 0.5 * (segment.a + segment.b)});
        }
        walls->tree.build(std::move(input));
        m_layers[layer_id] = std::move(walls);
        return *m_layers[layer_id];
    }

public:
    explicit FinWallGeometry(const PrintObject &object) : m_object(object), m_layers(object.layer_count()) {}

    // Painted contacts use signed intervals of the bead union. An interior
    // origin reaches the same outward-facing entry by clipping back along the axis.
    std::optional<Vec2d> contact(size_t layer_id, const Vec2d &origin, const Vec2d &direction,
                                 bool bidirectional = false, Vec2d *surface_normal = nullptr)
    {
        if (!origin.allFinite() || !direction.allFinite() || direction.norm() <= EPSILON)
            return std::nullopt;
        const LayerWalls &walls = layer(layer_id);
        if (walls.tree.empty())
            return std::nullopt;
        const Vec2d unit = direction.normalized();
        const Vec2d scaled_origin = origin / SCALING_FACTOR;
        const auto &bounds = walls.tree.node(0).bbox;
        const double reach = (scaled_origin - bounds.center()).norm() + bounds.sizes().norm() + SCALED_EPSILON;
        const double backward = bidirectional ? reach : 0.;
        const Vec2d start = scaled_origin - backward * unit;
        const Vec2d delta = (reach + backward) * unit;
        const Vec2d end = start + delta;
        struct Interval { double begin, end; size_t segment; };
        std::vector<Interval> intervals;
        double first = std::numeric_limits<double>::infinity();
        AABBTreeIndirect::traverse(walls.tree, [&](const Tree::Node &node) {
            std::pair<double, double> interval;
            return Geometry::liang_barsky_line_clipping_interval(start, delta,
                BoundingBoxf(node.bbox.min(), node.bbox.max()), interval);
        }, [&](const Tree::Node &node) {
            const Segment &segment = walls.segments[node.idx];
            std::pair<double, double> interval;
            if (Geometry::line_rounded_thick_segment_collision(
                    start, end, segment.a, segment.b, segment.radius, interval)) {
                if (bidirectional)
                    intervals.push_back({interval.first - backward, interval.second - backward, node.idx});
                else
                    first = std::min(first, interval.first);
            }
            return bidirectional || first != 0.;
        });
        if (!bidirectional) {
            if (!std::isfinite(first))
                return std::nullopt;
            return origin + unscale<double>(first) * unit;
        }
        std::sort(intervals.begin(), intervals.end(), [](const Interval &a, const Interval &b) {
            return a.begin < b.begin || (a.begin == b.begin && a.segment < b.segment);
        });
        for (size_t i = 0; i < intervals.size();) {
            const Interval entry = intervals[i++];
            double end_distance = entry.end;
            while (i < intervals.size() && intervals[i].begin <= end_distance) {
                end_distance = std::max(end_distance, intervals[i].end);
                ++i;
            }
            if (end_distance < 0.)
                continue;
            const Vec2d hit = scaled_origin + entry.begin * unit;
            if (surface_normal != nullptr) {
                const Segment &segment = walls.segments[entry.segment];
                const Vec2d edge = segment.b - segment.a;
                const double t = std::clamp((hit - segment.a).dot(edge) / edge.squaredNorm(), 0., 1.);
                *surface_normal = (hit - segment.a - t * edge).normalized();
            }
            return origin + unscale<double>(entry.begin) * unit;
        }
        return std::nullopt;
    }
};

struct PaintedContactAxis {
    Vec2d contact_point;
    Vec2d contact_direction;
    Vec2d front_normal;
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
            const double denominator = cross2(fin_direction, edge);
            if (std::abs(denominator) <= 1e-12)
                continue;
            const Vec2d delta = a - origin;
            const double distance = cross2(delta, edge) / denominator;
            const double position = cross2(delta, fin_direction) / denominator;
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

std::optional<PaintedContactAxis> painted_contact_axis(
    const Layer &layer, const Vec2d &body_start, const Vec2d &body_direction,
    const PaintedCreaseIntent *crease)
{
    if (crease != nullptr) {
        const auto at = crease->position_at_z(layer.slice_z);
        if (!at) {
            BOOST_LOG_TRIVIAL(debug) << "Fin support stage=painted-crease-layer-contact"
                << " reason=no-unique-intent-intersection crease=" << crease->crease
                << " pair=" << crease->pair << " part=" << crease->part << " slice_z=" << layer.slice_z;
            return std::nullopt;
        }
        std::string detail;
        const auto normal = fin_section_bisector(to_polygon_ptrs(layer.lslices), at->contact, &detail);
        if (!normal) {
            BOOST_LOG_TRIVIAL(debug) << "Fin support stage=painted-crease-layer-contact"
                << " reason=no-section-front-normal crease=" << crease->crease
                << " pair=" << crease->pair << " part=" << crease->part << " slice_z=" << layer.slice_z << detail;
        }
        const Vec2d delta = body_start - at->contact;
        const Vec2d axis = delta.norm() > EPSILON ? Vec2d(delta.normalized()) : body_direction;
        return PaintedContactAxis{at->contact, axis, normal.value_or(Vec2d::Zero())};
    }
    std::optional<ClosestContourPoint> nearest;
    const auto visit = [&](const Polygon &contour, bool hole) {
        auto point = closest_contour_point(contour, body_start);
        if (point && (!nearest || point->distance < nearest->distance)) {
            if (hole)
                point->normal = -point->normal;
            nearest = point;
        }
    };
    for (const ExPolygon &slice : layer.lslices) {
        visit(slice.contour, false);
        for (const Polygon &hole : slice.holes)
            visit(hole, true);
    }
    if (!nearest)
        return std::nullopt;
    Vec2d axis = body_start - nearest->point;
    axis = axis.norm() > EPSILON ? Vec2d(axis.normalized()) : nearest->normal;
    const double turn = std::clamp(std::atan2(cross2(body_direction, axis), body_direction.dot(axis)),
                                  -M_PI / 6., M_PI / 6.);
    axis = Vec2d(std::cos(turn) * body_direction.x() - std::sin(turn) * body_direction.y(),
                 std::sin(turn) * body_direction.x() + std::cos(turn) * body_direction.y());
    return PaintedContactAxis{nearest->point, axis, nearest->normal};
}


std::optional<PaintedContactAxis> resolve_fin_wall_contact(
    FinWallGeometry &walls, size_t layer, const Vec2d &body_start,
    const PaintedContactAxis &source, bool crease, bool painted_shape)
{
    const Vec2d origin = crease ? source.contact_point : body_start;
    const Vec2d inward = crease ? Vec2d(-source.front_normal) : Vec2d(-source.contact_direction);
    Vec2d front_normal = source.front_normal;
    const auto hit = walls.contact(layer, origin, inward, painted_shape,
        painted_shape && !crease ? &front_normal : nullptr);
    if (!hit)
        return std::nullopt;
    Vec2d axis = source.contact_direction;
    if (crease && (body_start - *hit).norm() > EPSILON)
        axis = (body_start - *hit).normalized();
    return PaintedContactAxis{*hit, axis, front_normal};
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

std::optional<Polyline> clipped_fin_path(const Polyline &path, const ExPolygons &clearance)
{
    // This start belongs to the whole generation group, not an interior
    // junction. Another root cannot rescue a fragment across a gap cut.
    if (path.points.size() < 2)
        return std::nullopt;
    const Vec2d start = unscale(path.first_point());
    Polylines pieces = diff_pl(Polylines{path}, clearance);
    for (Polyline &piece : pieces) {
        piece.points.erase(std::unique(piece.points.begin(), piece.points.end()), piece.points.end());
        if (piece.points.size() < 2)
            continue;
        if ((unscale(piece.first_point()) - start).norm() <= EPSILON)
            return std::move(piece);
        if ((unscale(piece.last_point()) - start).norm() <= EPSILON) {
            std::reverse(piece.points.begin(), piece.points.end());
            return std::move(piece);
        }
    }
    return std::nullopt;
}

Vec2d fin_body_direction(const TracedFin &fin, size_t layer)
{
    if (fin.seed.path) {
        const Points &points = fin.layer_paths[layer].points;
        assert(points.size() >= 2);
        return (unscale(points[points.size() - 2]) - unscale(points.back())).normalized();
    }
    return fin.seed.normal;
}

bool valid_painted_fin_path(const PrintObject &object, const FinCandidate &candidate)
{
    const auto &points = candidate.path->points;
    if (points.size() < 2) {
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
            << " stage=painted-fin-path reject=too-few-vertices"
            << " island=" << candidate.intent_island.value_or(std::numeric_limits<size_t>::max())
            << " edge=" << candidate.path->source_edge << " vertices=" << points.size();
        return false;
    }
    for (size_t i = 1; i < points.size(); ++i)
        if (points[i].z() > points[i - 1].z() + EPSILON) {
            BOOST_LOG_TRIVIAL(info)
                << "Fin support object=" << object.model_object()->name
                << " stage=painted-fin-path reject=ascending-path"
                << " island=" << candidate.intent_island.value_or(std::numeric_limits<size_t>::max())
                << " edge=" << candidate.path->source_edge
                << " from=" << i - 1 << " to=" << i
                << " from_z=" << points[i - 1].z() << " to_z=" << points[i].z();
            return false;
        }
    return true;
}

bool trace_painted_fin_path(const PrintObject &object, const FinCandidate &candidate,
                           double gap, TracedFin &fin,
                           const std::vector<PaintedLayerGeometryCache> *geometry_cache)
{
    if (!valid_painted_fin_path(object, candidate))
        return false;
    const auto &points = candidate.path->points;
    const size_t count = candidate.top_layer + 1;
    fin.boundary.resize(count, points.back().head<2>());
    fin.contact_directions.assign(count, candidate.normal);
    fin.contact_front_normals.assign(count, candidate.normal);
    fin.body_cap_normals.assign(count, candidate.normal);
    fin.contact_sources.assign(count, std::nullopt);
    fin.wall_present.assign(count, false);
    fin.path_present.assign(count, false);
    fin.body_start.resize(count, points.back().head<2>());
    fin.tips.resize(count, points.front().head<2>());
    fin.layer_paths.resize(count);
    Polyline path;
    for (const Vec3d &point : points) {
        const Point xy = scaled_point(point.head<2>());
        // Preserve source heights above, but zero-length XY edges have no
        // slice geometry or local direction.
        if (path.points.empty() || path.points.back() != xy)
            path.points.push_back(xy);
    }
    if (path.points.size() < 2)
        return true;
    for (size_t i = 0; i < count; ++i) {
        if (object.print()->canceled())
            throw CanceledException();
        const Layer &layer = *object.get_layer(int(i));
        const ExPolygons computed = geometry_cache == nullptr || i >= geometry_cache->size() ?
            offset_ex(layer.lslices, float(scale_(gap)), jtRound, DefaultMiterLimit) : ExPolygons{};
        const ExPolygons &clearance = geometry_cache != nullptr && i < geometry_cache->size() ?
            (*geometry_cache)[i].model_gap : computed;
        if (auto retained = clipped_fin_path(path, clearance)) {
            fin.layer_paths[i] = std::move(*retained);
            fin.body_start[i] = unscale(fin.layer_paths[i].last_point());
            fin.tips[i] = unscale(fin.layer_paths[i].first_point());
            fin.path_present[i] = fin.layer_paths[i].length() > scale_(EPSILON);
            if (!fin.path_present[i])
                continue;
            fin.contact_directions[i] = fin_body_direction(fin, i);
            fin.contact_front_normals[i] = fin.contact_directions[i];
            fin.body_cap_normals[i] = fin_body_direction(fin, i);
            // Resolve the contact axis before the shared side-chain cut. The
            // gap boundary locates A1; it does not supply the cap direction.
            if (!candidate.crease_intent) {
                // A finite endpoint on the clearance contour is a contact too.
                // Reaching a natural end away from that contour cannot authorize
                // a new contact by searching past the prescribed path.
                const bool natural_end = (fin.body_start[i] - unscale(path.last_point())).norm() <= EPSILON;
                if (natural_end) {
                    double clearance_distance = std::numeric_limits<double>::infinity();
                    for (const ExPolygon &area : clearance) {
                        const auto consider = [&](const Polygon &contour) {
                            const auto point = closest_contour_point(contour, fin.body_start[i]);
                            if (point)
                                clearance_distance = std::min(clearance_distance, point->distance);
                        };
                        consider(area.contour);
                        for (const Polygon &hole : area.holes)
                            consider(hole);
                    }
                    if (clearance_distance > EPSILON)
                        continue;
                }
            }
            if (const auto contact = painted_contact_axis(
                    layer, fin.body_start[i], fin_body_direction(fin, i), candidate.crease_intent.get())) {
                fin.boundary[i] = contact->contact_point;
                fin.contact_sources[i] = contact->contact_point;
                fin.contact_directions[i] = contact->contact_direction;
                fin.contact_front_normals[i] = contact->front_normal;
                fin.body_cap_normals[i] = contact->contact_direction;
            }
        }
    }
    return true;
}

struct FinContactBand {
    size_t begin;
    size_t end;
};

FinContactBand seed_fin_contact_band(const PrintObject &object, const FinCandidate &seed)
{
    const size_t end = seed.top_layer + 1;
    const double top = object.get_layer(int(seed.top_layer))->slice_z;
    const double bottom = top - object.config().fin_support_contact_height.value;
    size_t begin = 0;
    while (begin < end && object.get_layer(int(begin))->slice_z <= bottom + 1e-9)
        ++begin;
    return {begin, end};
}

bool trace_fin(const PrintObject &object, const Polygon &bed, const FinCandidate &candidate,
               double top_depth, double gap, double thickness, double line_width, TracedFin &fin,
               const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    if (candidate.path)
        return trace_painted_fin_path(object, candidate, gap, fin, geometry_cache);
    if (candidate.geometry_mode == FinGeometryMode::projected && top_depth - gap < 2 * line_width)
        return fin.diagnostics.reject(FinReject::top_depth, object.get_layer(int(candidate.top_layer))->print_z,
                                      top_depth - gap);
    fin.boundary.resize(candidate.top_layer + 1);
    fin.contact_directions.assign(candidate.top_layer + 1, candidate.normal);
    fin.contact_front_normals.assign(candidate.top_layer + 1, candidate.normal);
    fin.body_cap_normals.assign(candidate.top_layer + 1, candidate.normal);
    fin.contact_sources.assign(candidate.top_layer + 1, std::nullopt);
    fin.wall_present.assign(candidate.top_layer + 1, false);
    fin.body_start.resize(candidate.top_layer + 1);
    if (candidate.geometry_mode == FinGeometryMode::painted_shape)
        fin.path_present.assign(candidate.top_layer + 1, false);
    fin.tips.resize(candidate.top_layer + 1);
    const double top_z = object.get_layer(int(candidate.top_layer))->slice_z;
    Vec2d target = candidate.point;
    const Vec2d side(-candidate.normal.y(), candidate.normal.x());
    // A fixes the sheet plane through the seed. Its contour ray selects the
    // physical host; Painted Shape resolves the round-clearance endpoint and
    // finite contact axis separately instead of projecting a wall normal onto A.
    double projected_inner = std::numeric_limits<double>::infinity();
    for (size_t remaining = candidate.top_layer + 1; remaining > 0; --remaining) {
        const size_t i = remaining - 1;
        const Layer &layer = *object.get_layer(int(i));
        const double outer_distance = top_depth + (top_z - layer.slice_z) * candidate.expansion_slope;
        fin.tips[i] = candidate.point + outer_distance * candidate.normal;
        Vec2d point = target;
        Vec2d surface_normal = candidate.contact_normal;
        OuterContourLocation wall_location;
        const bool have_surface = candidate.geometry_mode == FinGeometryMode::projected && projected_outer_contour(
            layer, candidate.point, candidate.normal, outer_distance,
            point, &surface_normal, &wall_location);
        if (candidate.geometry_mode == FinGeometryMode::projected && i == candidate.top_layer &&
            (!have_surface || (point - candidate.point).norm() > line_width))
            return fin.diagnostics.reject(FinReject::top_wall, layer.print_z, (point - candidate.point).norm());
        if (candidate.geometry_mode == FinGeometryMode::painted_shape) {
            const ExPolygons *cached_clearance = geometry_cache != nullptr && i < geometry_cache->size() ?
                &(*geometry_cache)[i].model_gap : nullptr;
            const auto clearance = painted_clearance_hit(layer, candidate.point, candidate.normal,
                outer_distance, gap, line_width, cached_clearance);
            // A ray has no finite inner endpoint. With no clearance crossing it
            // supplies no finite body fragment; do not borrow a previous layer.
            fin.path_present[i] = clearance.has_value();
            if (!clearance)
                continue;
            fin.body_start[i] = clearance->point;
            if (const auto contact = painted_contact_axis(
                    layer, fin.body_start[i], candidate.normal, candidate.crease_intent.get())) {
                fin.boundary[i] = contact->contact_point;
                fin.contact_sources[i] = contact->contact_point;
                fin.contact_directions[i] = contact->contact_direction;
                fin.contact_front_normals[i] = contact->front_normal;
                fin.body_cap_normals[i] = contact->contact_direction;
            }
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
        fin.contact_directions[i] = candidate.normal;
        fin.contact_front_normals[i] = candidate.normal;
        fin.body_cap_normals[i] = candidate.normal;
        if (have_surface)
            fin.contact_sources[i] = point;
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
    if (cap_normal == body_direction) {
        // The segment's own flat end: use its two side endpoints directly,
        // with the existing half-bead inset in printable-center space.
        // The shared side-clearance cut resolves the final attachment later.
        const Vec2d cap_center = center + center_inset * body_direction;
        return std::make_pair(Vec2d(cap_center - half_width * body_side),
                              Vec2d(cap_center + half_width * body_side));
    }
    const Vec2d cap_side(-cap_normal.y(), cap_normal.x());
    const double denominator = cross2(body_direction, cap_side);
    // Both orientations have a unique line intersection; only parallel lines
    // are undefined. Do not impose a facing rule on either fin pipeline here.
    if (std::abs(denominator) <= EPSILON)
        return std::nullopt;
    const Vec2d cap_center = center + center_inset * cap_normal;
    // Materialize the Eigen expression while the local ray is still alive.
    auto intersection = [&](double side) -> Vec2d {
        const Vec2d ray = center + side * half_width * body_side;
        const double distance = cross2(cap_center - ray, cap_side) / denominator;
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

// Clip only a shared junction sector against its incident member constraints.
// Member side boundaries and contact attachments do not use this operation.
Polygon clip_fin_joint(const Polygon &boundary, const Line &cap, const Vec2d &direction)
{
    if (boundary.points.size() < 3)
        return {};
    const Vec2d edge = (cap.b - cap.a).cast<double>();
    Vec2d normal(-edge.y(), edge.x());
    if (normal.dot(direction) < 0.)
        normal = -normal;
    Points kept;
    Point previous = boundary.points.back();
    double previous_side = (previous - cap.a).cast<double>().dot(normal);
    for (const Point &point : boundary.points) {
        const double side = (point - cap.a).cast<double>().dot(normal);
        if ((side >= 0.) != (previous_side >= 0.))
            kept.push_back((previous.cast<double>() + previous_side / (previous_side - side) *
                (point - previous).cast<double>()).cast<coord_t>());
        if (side >= 0.)
            kept.push_back(point);
        previous = point;
        previous_side = side;
    }
    kept.erase(std::unique(kept.begin(), kept.end()), kept.end());
    if (kept.size() > 1 && kept.front() == kept.back())
        kept.pop_back();
    Polygon result;
    result.points = std::move(kept);
    return result;
}

// Open side chains preserve the attachment points shared with contacts and taper.
// Closing and clipping the body first can move or remove these points at short
// segments, leaving gaps or causing attachment failure. Root depth belongs to
// the same continuous boundary so its connection does not create an internal cap.
Polyline fin_path_boundary(const std::vector<FinBodyAxis> &axes, double radius,
                           double cap_inset, const ExPolygons &clearance,
                           std::optional<Line> &attachment, const Line *continuation_cap = nullptr)
{
    attachment.reset();
    if (axes.empty())
        return {};
    std::vector<Vec2d> path{axes.front().outer};
    for (const FinBodyAxis &axis : axes) {
        if (axis.outer != path.back())
            return {};
        if (axis.inner != path.back())
            path.push_back(axis.inner);
    }
    if (path.size() < 2)
        return {};
    std::vector<Vec2d> directions, normals;
    for (size_t i = 1; i < path.size(); ++i) {
        directions.push_back((path[i] - path[i - 1]).normalized());
        normals.emplace_back(-directions.back().y(), directions.back().x());
    }
    const FinBodyAxis &tail = axes.back();
    std::optional<Line> cap = tail.center_cap;
    if (!cap) {
        const auto resolved = fin_cap_segment(tail.inner, tail.direction, tail.cap_normal,
            radius, tail.inner_junction ? 0. : cap_inset);
        if (!resolved)
            return {};
        cap = Line(scaled_point(resolved->first), scaled_point(resolved->second));
    }
    const Vec2d cap_origin = unscale(cap->a);
    const Vec2d cap_edge = unscale(cap->b) - cap_origin;
    if (cap_edge.squaredNorm() == 0.)
        return {};
    Vec2d cap_normal(-cap_edge.y(), cap_edge.x());
    cap_normal.normalize();
    if (cap_normal.dot(tail.direction) < 0.)
        cap_normal = -cap_normal;

    const auto clip_at_cap = [](std::vector<Vec2d> &points, const Vec2d &origin, const Vec2d &normal) {
        if (points.empty())
            return false;
        double before = (points.front() - origin).dot(normal);
        if (before == 0.) {
            points.resize(1);
            return true;
        }
        for (size_t i = 1; i < points.size(); ++i) {
            const double after = (points[i] - origin).dot(normal);
            if (after == 0. || (before < 0.) != (after < 0.)) {
                const Vec2d hit = points[i - 1] + before / (before - after) *
                    (points[i] - points[i - 1]);
                points.resize(i + 1);
                points.back() = hit;
                return true;
            }
            before = after;
        }
        return false;
    };
    const auto clip_at_previous_cap = [&](std::vector<Vec2d> &points) {
        if (continuation_cap == nullptr)
            return false;
        const Vec2d origin = unscale(continuation_cap->a);
        const Vec2d edge = unscale(continuation_cap->b) - origin;
        return clip_at_cap(points, origin, Vec2d(-edge.y(), edge.x()));
    };
    auto make_side = [&](double side) -> std::vector<Vec2d> {
        std::vector<Vec2d> points{path.front() + side * radius * normals.front()};
        for (size_t i = 1; i + 1 < path.size(); ++i) {
            const double turn = cross2(directions[i - 1], directions[i]);
            const double cosine = std::clamp(directions[i - 1].dot(directions[i]), -1., 1.);
            if (side * turn < 0.) {
                // One outside arc joins the two sides of adjacent segments;
                // no segment contributes a closed strip or an endpoint disk.
                const double angle = std::atan2(turn, cosine);
                const size_t steps = Geometry::ArcWelder::arc_discretization_steps(
                    radius, std::abs(angle), 0.005);
                const Vec2d from = side * radius * normals[i - 1];
                for (size_t step = 0; step < steps; ++step) {
                    const double theta = angle * double(step) / double(steps);
                    points.push_back(path[i] + Vec2d(
                        from.x() * std::cos(theta) - from.y() * std::sin(theta),
                        from.x() * std::sin(theta) + from.y() * std::cos(theta)));
                }
                points.push_back(path[i] + side * radius * normals[i]);
            } else {
                // Intersection of the two offset side lines, including their
                // straight continuation. Exact reversal has no finite join;
                // this does not add a fold-detection or repair policy.
                const double denominator = 1. + cosine;
                if (denominator <= std::numeric_limits<double>::epsilon())
                    return {};
                points.push_back(path[i] + side * radius *
                    (normals[i - 1] + normals[i]) / denominator);
            }
        }
        points.push_back(path.back() + side * radius * normals.back());

        // Traverse the complete side from the outer end, including every
        // sampled bend. The first cap-line hit owns this attachment; discard
        // the entire suffix even if it later returns to the body side.
        const bool cap_hit = clip_at_cap(points, cap_origin, cap_normal);
        const bool previous_cap_hit = clip_at_previous_cap(points);
        if (cap_hit || previous_cap_hit)
            return points;
        const double before = (points.back() - cap_origin).dot(cap_normal);
        // Only a side with no finite hit may extend its terminal line to an
        // oblique cap. Never extend backward past an already traversed bend.
        const double denominator = directions.back().dot(cap_normal);
        if (std::abs(denominator) <= std::numeric_limits<double>::epsilon())
            return {};
        const double extension = -before / denominator;
        if (extension < 0.)
            return {};
        points.back() += extension * directions.back();
        // The previous rear edge may shorten the ordinary cap extension, but
        // never extends a side on its own.
        clip_at_previous_cap(points);
        return points;
    };
    // The current cap, optional previous cap and clearance apply to the same
    // complete side in outer-to-inner order. These cuts retain their common
    // prefix. Never recover a later fragment after the first gap crossing.
    // Use the printable-center clearance (gap + half bead), also used by the
    // final layer cut, so the contact is built from the actual surviving ends.
    const auto clip_side = [&](double side) -> Points {
        const std::vector<Vec2d> points = make_side(side);
        Points scaled;
        scaled.reserve(points.size());
        for (const Vec2d &point : points)
            scaled.push_back(scaled_point(point));
        scaled.erase(std::unique(scaled.begin(), scaled.end()), scaled.end());
        auto retained = clipped_fin_path(Polyline(std::move(scaled)), clearance);
        return retained ? std::move(retained->points) : Points{};
    };
    const Points left = clip_side(1.);
    const Points right = clip_side(-1.);
    if (left.empty() || right.empty())
        return {};
    // Leave the attachment side open. The final boundary concatenates this
    // outer arc with the accepted contact/taper arc using these exact points.
    Points outline;
    outline.reserve(left.size() + right.size());
    outline.insert(outline.end(), left.rbegin(), left.rend());
    outline.insert(outline.end(), right.begin(), right.end());
    outline.erase(std::unique(outline.begin(), outline.end()), outline.end());
    if (outline.size() < 3 || outline.front() == outline.back())
        return {};
    attachment = Line(outline.front(), outline.back());
    return Polyline(std::move(outline));
}

// Members own their complete paths, including root depth and internal bends.
// Only junction sectors shared by distinct members remain for assembly.
ExPolygons fin_junction_centers(const std::vector<FinBodyAxis> &axes,
                               double radius, double cap_inset)
{
    struct Incident {
        size_t axis;
        bool at_outer;
    };
    struct Joint {
        Point point;
        std::vector<Incident> incidents;
    };
    std::vector<Joint> joints;
    std::unordered_map<Point, size_t, PointHash> joint_by_point;
    const auto register_joint = [&](const Point &point, size_t axis, bool at_outer) {
        const auto inserted = joint_by_point.emplace(point, joints.size());
        if (inserted.second)
            joints.push_back({point, {}});
        joints[inserted.first->second].incidents.push_back({axis, at_outer});
    };
    std::vector<Line> caps;
    caps.reserve(axes.size());
    ExPolygons envelope;
    for (size_t i = 0; i < axes.size(); ++i) {
        const FinBodyAxis &axis = axes[i];
        std::optional<Line> cap = axis.center_cap;
        if (!cap) {
            const auto resolved = fin_cap_segment(axis.inner, axis.direction, axis.cap_normal,
                radius, axis.inner_junction ? 0. : cap_inset);
            if (!resolved)
                return {};
            cap = Line(scaled_point(resolved->first), scaled_point(resolved->second));
        }
        caps.push_back(*cap);
        if (axis.outer_junction)
            register_joint(scaled_point(axis.outer), i, true);
        if (axis.inner_junction)
            register_joint(scaled_point(axis.inner), i, false);
    }
    // Exact printed-coordinate identity combines geometric contributions only;
    // it does not create or change any intent-graph connection.
    for (const Joint &joint : joints) {
        if (joint.incidents.size() < 2)
            continue; // An endpoint is a flat cap, never a standalone round disk.
        std::map<size_t, size_t> member_incidents;
        bool already_owned = false;
        for (const Incident &incident : joint.incidents) {
            const auto owner = axes[incident.axis].intent_edge;
            if (owner && ++member_incidents[*owner] >= 2) {
                already_owned = true;
                break;
            }
        }
        if (already_owned)
            continue;
        Polygon join = make_circle(scale_(radius), scale_(0.005));
        join.translate(joint.point);
        for (const Incident &incident : joint.incidents) {
            const FinBodyAxis &axis = axes[incident.axis];
            Vec2d toward_segment = axis.direction;
            if (incident.at_outer)
                toward_segment = -toward_segment;
            const Vec2d side(-toward_segment.y(), toward_segment.x());
            // Only the outside sector between incident side strips is missing.
            // A full disk also grows along a short outgoing segment and can
            // protrude through its model-facing cap.
            join = clip_fin_joint(join,
                Line(joint.point, joint.point + scaled_point(side)), -toward_segment);
            if (incident.at_outer && !axis.inner_junction)
                join = clip_fin_joint(join, caps[incident.axis], axis.direction);
            else if (!incident.at_outer && !axis.outer_junction) {
                const Line outer_cap(scaled_point(axis.outer - radius * side),
                                     scaled_point(axis.outer + radius * side));
                join = clip_fin_joint(join, outer_cap, -axis.direction);
            }
        }
        if (join.points.size() >= 3 && join.area() != 0.)
            envelope.emplace_back(std::move(join));
    }
    // Only cross-member junction geometry is returned; no member body or
    // internal root seam is reconstructed here.
    return union_ex(envelope);
}

Polygon fin_contact_center_polygon(const Vec2d &first, const Vec2d &second, Line cap)
{
    const double direct = (first - unscale(cap.a)).squaredNorm() +
                          (second - unscale(cap.b)).squaredNorm();
    const double crossed = (first - unscale(cap.b)).squaredNorm() +
                           (second - unscale(cap.a)).squaredNorm();
    if (crossed < direct)
        std::swap(cap.a, cap.b);
    if (first == second)
        return Polygon{scaled_point(first), cap.a, cap.b};
    return Polygon{scaled_point(first), cap.a, cap.b, scaled_point(second)};
}

const std::optional<Line> &fin_center_cap(const TracedFin &fin, size_t layer_id)
{
    return fin.center_caps[layer_id];
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

struct FinContactSlice {
    Vec2d wall = Vec2d::Zero();
    Vec2d axis = Vec2d::Zero(); // Wall query and fixed taper plane, also retained by carried contacts.
    Vec2d front_normal = Vec2d::Zero(); // Orientation and half-bead placement of the front segment.
    Vec2d first = Vec2d::Zero();
    Vec2d second = Vec2d::Zero();
    Vec3d surface_normal = Vec3d::Zero(); // Simple/Simulated projection only.
};

ExPolygons build_fin_contact_section(const Polygon &bed, const TracedFin &fin,
                                     size_t layer_id, const FinContactSlice &contact,
                                     double contact_width, double thickness, double line_width,
                                     double *failure_stage = nullptr,
                                     const ExPolygons *cached_bed_center = nullptr)
{
    if (failure_stage != nullptr)
        *failure_stage = 1.;
    Polygon neck;
    if (fin.seed.geometry_mode == FinGeometryMode::painted_shape) {
        const auto &cap = fin_center_cap(fin, layer_id);
        if (!cap.has_value())
            return {};
        neck = fin_contact_center_polygon(contact.first, contact.second, *cap);
    } else {
        neck = fin_contact_polygon(
            contact.wall + 0.5 * line_width * contact.front_normal,
            fin.body_start[layer_id], fin.seed.normal,
            fin.body_cap_normals[layer_id], contact.front_normal,
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
    // Contact geometry is anchored to the printed wall boundary. A second cut
    // against the source model would erase contacts inside a rounded tip's
    // nominal outline. Body gap and lower taper clearance remain independent.
    ExPolygons result = intersection_ex(Polygons{neck}, bed_center);
    if (failure_stage != nullptr)
        *failure_stage = result.empty() ? 3. : 0.;
    return result;
}

// Preserve the last actual front and the preceding successful attachment.
// Continued layers add the saved cap to their side cuts and share the resulting
// endpoints with the contact. They do not create a new actual contact.
struct FinContactContinuation {
    FinContactSlice contact;
    Line cap;
};

FinContactSlice make_straight_fin_contact_slice(const Vec2d &wall, const Vec2d &axis, const Vec2d &front_normal,
                                                double contact_width, double line_width)
{
    FinContactSlice contact;
    contact.wall = wall;
    contact.axis = axis.normalized();
    contact.front_normal = front_normal.normalized();
    const Vec2d front = wall + 0.5 * line_width * contact.front_normal;
    const Vec2d tangent(-contact.front_normal.y(), contact.front_normal.x());
    contact.first = front - 0.5 * contact_width * tangent;
    contact.second = front + 0.5 * contact_width * tangent;
    return contact;
}

// Ordinary fronts retain the straight tangent approximation. Lateral samples
// validate its contact coverage without reconstructing the front endpoints.
FinContactSlice make_ordinary_fin_contact_slice(const PaintedContactAxis &wall,
                                                double contact_width, double line_width)
{
    return make_straight_fin_contact_slice(wall.contact_point, wall.contact_direction,
        wall.front_normal, contact_width, line_width);
}

// A crease front is defined by the source exterior bisector. Once this perpendicular
// segment is built, there is no surface-span search or physical touching-length test.
FinContactSlice make_crease_fin_contact_slice(const PaintedContactAxis &wall,
                                              double contact_width, double line_width)
{
    return make_straight_fin_contact_slice(wall.contact_point, wall.contact_direction,
        wall.front_normal, contact_width, line_width);
}

std::optional<PaintedContactAxis> fin_layer_wall_contact(const TracedFin &fin, size_t layer_id)
{
    // Geometry preparation has already resolved this layer's physical wall hit
    // and axis. Read that result without imposing a front-construction policy.
    if (!fin.wall_present[layer_id])
        return std::nullopt;
    const Vec2d &wall = fin.boundary[layer_id];
    const Vec2d &front_normal = fin.contact_front_normals[layer_id];
    const Vec2d &axis = fin.contact_directions[layer_id];
    if (!front_normal.allFinite() || front_normal.norm() <= EPSILON ||
        !axis.allFinite() || axis.norm() <= EPSILON)
        return std::nullopt;
    return PaintedContactAxis{wall, axis, front_normal};
}

std::optional<FinContactSlice> legacy_fin_layer_contact(const TracedFin &fin, size_t layer_id,
                                                        double contact_width, double line_width)
{
    const auto wall = fin_layer_wall_contact(fin, layer_id);
    if (!wall)
        return std::nullopt;
    return make_straight_fin_contact_slice(wall->contact_point, wall->contact_direction,
        wall->front_normal, contact_width, line_width);
}

struct ProjectionClip {
    ExPolygons model;
    ExPolygons bed;
    bool ready = false;
};

struct ProjectionResult {
    size_t layers = 0;
    size_t exposed_layers = 0;
    ProjectionStop stop = ProjectionStop::bed;
};

enum class FinExtensionState { pending, materializing, complete };
enum class FinExtensionLayerState { inactive, contact, materialized };

struct FinCenterExtension {
    FinContactBand band{0, 0};
    FinExtensionState state = FinExtensionState::pending;
    // Geometry and completion state remain tied to this member's body until
    // every taper layer has been evaluated. Failed layers remain inactive;
    // successful taper layers are materialized with their completed body.
    std::vector<ExPolygons> layers;
    // Complete local contours, joined before band/taper publication.
    std::vector<ExPolygons> completed_layers;
    // Source attachment edge; clipping may retain only part of it. A carried
    // contact records the rear endpoints actually reached on this layer's body.
    std::vector<std::optional<Line>> attachments;
    std::vector<FinExtensionLayerState> layer_states;
};

struct FinComponentCenterGeometry {
    size_t source = std::numeric_limits<size_t>::max();
    bool complete = false;
    // Member-local body centers serve exposure queries and diagnostics.
    // Completed contours retain their member ownership through compound assembly.
    std::vector<ExPolygons> body_centers;
    // Contacts are spliced into their own member before any compound union.
    std::vector<ExPolygons> completed_centers;
    // Extensions retain their member identity until the compound completion
    // barrier separates support transitions from actual contact sections.
    std::vector<FinCenterExtension> extensions;
};

void append_fin_component_debug(std::vector<FinGeometryDebugSet> &sets,
                                const FinComponentCenterGeometry &component, size_t layer_id)
{
    const std::string prefix = "source=" + std::to_string(component.source);
    if (layer_id < component.body_centers.size())
        sets.push_back({prefix + " body centers", component.body_centers[layer_id], {}, {}});
    if (layer_id < component.completed_centers.size())
        sets.push_back({prefix + " completed member centers", component.completed_centers[layer_id], {}, {}});
    for (size_t e = 0; e < component.extensions.size(); ++e) {
        const auto &extension = component.extensions[e];
        if (layer_id >= extension.layers.size())
            continue;
        const auto state = layer_id < extension.layer_states.size() ? extension.layer_states[layer_id] :
            FinExtensionLayerState::inactive;
        const char *kind = state == FinExtensionLayerState::contact ? "contact" :
            state == FinExtensionLayerState::materialized ? "taper/carried" : "inactive";
        sets.push_back({prefix + " extension=" + std::to_string(e) + " " + kind +
            " band=" + std::to_string(extension.band.begin) + ":" + std::to_string(extension.band.end),
            extension.layers[layer_id], {}, {}});
        if (layer_id < extension.attachments.size() && extension.attachments[layer_id]) {
            const Line &edge = *extension.attachments[layer_id];
            sets.push_back({prefix + " extension=" + std::to_string(e) + " attachment",
                {}, {Polyline(Points{edge.a, edge.b})}, {edge.a, edge.b}});
        }
    }
}

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
        DefaultJoinType,
        DefaultMiterLimit);
    return diff_ex(body, clearance);
}

ExPolygons fin_body_center_area(const Polygon &bed, TracedFin &fin, size_t layer_id,
                                double thickness, double line_width, const ExPolygons &clearance,
                                const ExPolygons *cached_bed_center = nullptr)
{
    const double radius = 0.5 * (thickness - line_width);
    if (radius <= EPSILON)
        return {};
    fin.body_boundaries[layer_id] = fin_path_boundary(
        fin.body_axes[layer_id], radius, 0.5 * line_width, clearance, fin.center_caps[layer_id]);
    Polygon query_body(fin.body_boundaries[layer_id].points);
    query_body.make_counter_clockwise();
    const ExPolygons centers = union_ex(Polygons{std::move(query_body)});
    if (!fin.body_axes[layer_id].empty())
        fin.body_axes[layer_id].back().center_cap = fin.center_caps[layer_id];
    // The clipped side endpoints are owned by the body, contact and taper.
    // Actual contacts consume this shared attachment. Continuation rebuilds
    // the complete sides with its additional saved-cap cut and shares those
    // resulting endpoints with its own body/contact geometry.
    const ExPolygons computed_bed_center = cached_bed_center == nullptr ?
        offset_ex(bed, -float(scale_(0.5 * line_width))) : ExPolygons{};
    return intersection_ex(
        centers, cached_bed_center != nullptr ? *cached_bed_center : computed_bed_center);
}

void build_fin_body_section(const PrintObject &object, TracedFin &fin, size_t layer_id,
                            const Polygon &bed, double thickness, double gap,
                            ExPolygons &body, ExPolygons &body_center,
                            bool materialize_painted_body = true,
                            const ExPolygons *cached_bed_center = nullptr,
                            ExPolygons *painted_bed_foot = nullptr,
                            const ExPolygons *cached_clearance = nullptr)
{
    if (!fin.path_present.empty() && !fin.path_present[layer_id])
        return;
    const Layer &layer = *object.get_layer(int(layer_id));
    const double width = support_material_flow(&object, float(layer.height)).width();
    if (fin.seed.geometry_mode == FinGeometryMode::painted_shape) {
        const ExPolygons computed_clearance = cached_clearance == nullptr ?
            offset_ex(layer.lslices, float(scale_(gap + 0.5 * width)),
                      ClipperLib::jtRound, DefaultMiterLimit) : ExPolygons{};
        body_center = fin_body_center_area(
            bed, fin, layer_id, thickness, width,
            cached_clearance != nullptr ? *cached_clearance : computed_clearance, cached_bed_center);
        if (layer_id == 0 && !body_center.empty()) {
            ExPolygons foot = offset_ex(
                body_center,
                float(scale_(object.config().raft_first_layer_expansion.value)),
                jtRound, double(scale_(0.005)));
            foot = intersection_ex(
                foot, cached_bed_center != nullptr ?
                    *cached_bed_center :
                    offset_ex(bed, -float(scale_(0.5 * width))));
            if (painted_bed_foot != nullptr)
                *painted_bed_foot = foot;
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
                      DefaultJoinType,
                      DefaultMiterLimit));
        expolygons_append(body, std::move(foot));
        body = union_ex(body);
    }
    body_center = offset_ex(body, -float(scale_(0.5 * width)));
}

// A clipping intersection is quantized to the integer lattice. Only points on
// the explicitly owned attachment are eligible for insertion, within one lattice
// unit; this is not an EPSILON search for unrelated nearby body boundaries.
bool on_fin_attachment(const Point &point, const Line &attachment)
{
    return line_alg::distance_to_squared(attachment, point) <= 1.;
}

bool splice_fin_attachment(Polygon &body, Polygon contact, const Line &attachment)
{
    if (body.points.size() < 3 || contact.points.size() < 3 || attachment.a == attachment.b)
        return false;
    body.make_counter_clockwise();
    contact.make_counter_clockwise();
    const size_t count = contact.points.size();
    // Find the contact's contiguous rear boundary. Plane/bed clipping can
    // shorten the source attachment without changing its ownership.
    size_t begin = count;
    size_t end = count;
    for (size_t i = 0; i < count; ++i) {
        if (on_fin_attachment(contact.points[i], attachment) &&
            !on_fin_attachment(contact.points[(i + count - 1) % count], attachment)) {
            if (begin != count)
                return false;
            begin = i;
        }
    }
    if (begin == count)
        return false;
    end = begin;
    while (on_fin_attachment(contact.points[(end + 1) % count], attachment))
        end = (end + 1) % count;
    if (end == begin)
        return false;
    const Point first = contact.points[begin];
    const Point second = contact.points[end];
    if (first == second)
        return false;
    Points outline = body.points;
    auto insert_endpoint = [&](const Point &point) {
        if (std::find(outline.begin(), outline.end(), point) != outline.end())
            return true;
        for (size_t i = 0; i < outline.size(); ++i) {
            const Point &a = outline[i];
            const Point &b = outline[(i + 1) % outline.size()];
            if (on_fin_attachment(a, attachment) && on_fin_attachment(b, attachment) &&
                on_fin_attachment(point, Line(a, b))) {
                outline.insert(outline.begin() + i + 1, point);
                return true;
            }
        }
        return false;
    };
    if (!insert_endpoint(first) || !insert_endpoint(second))
        return false;
    const size_t a = std::find(outline.begin(), outline.end(), first) - outline.begin();
    const size_t b = std::find(outline.begin(), outline.end(), second) - outline.begin();
    // CCW contours traverse a shared edge in opposite directions. Remove the
    // body's b->a chain and the contact's a->b chain, retaining their outer arcs.
    for (size_t i = b; i != a; i = (i + 1) % outline.size())
        if (!on_fin_attachment(outline[i], attachment))
            return false;
    Points joined;
    for (size_t i = a; i != b; i = (i + 1) % outline.size())
        joined.push_back(outline[i]);
    for (size_t i = end; i != begin; i = (i + 1) % count)
        joined.push_back(contact.points[i]);
    joined.erase(std::unique(joined.begin(), joined.end()), joined.end());
    if (joined.size() < 3)
        return false;
    body.points = std::move(joined);
    return true;
}

bool join_fin_contact_boundary(ExPolygons &body, const ExPolygons &sections,
                               const std::optional<Line> &attachment)
{
    for (const ExPolygon &section : sections) {
        bool joined = false;
        if (attachment && section.holes.empty()) {
            for (ExPolygon &region : body) {
                if (splice_fin_attachment(region.contour, section.contour, *attachment)) {
                    joined = true;
                    break;
                }
            }
        }
        if (!joined) {
            // A carried contact or a taper can overlap the body, or an existing
            // transition can already cover its attachment. Union is for that
            // positive-area overlap, never a substitute for an edge-only join.
            const ExPolygons overlap = intersection_ex(body, ExPolygons{section});
            if (std::none_of(overlap.begin(), overlap.end(), [](const ExPolygon &area) {
                    return area.area() > 0.;
                }))
                return false;
            body.push_back(section);
        }
        // Resolve actual overlaps/bends only after the contact seam is closed.
        body = union_ex(body);
    }
    return true;
}

// Complete the owned perimeter before region normalization or bed clipping.
// Query snapshots are deliberately not accepted as construction input.
ExPolygons complete_fin_contact_boundary(const Polyline &body_boundary, const ExPolygons &sections,
                                        const std::optional<Line> &attachment, const ExPolygons &bed_center)
{
    if (body_boundary.points.size() < 3 || sections.empty())
        return {};
    Polygon contour(body_boundary.points);
    contour.make_counter_clockwise();
    ExPolygons completed{ExPolygon(std::move(contour))};
    if (!join_fin_contact_boundary(completed, sections, attachment))
        return {};
    return intersection_ex(completed, bed_center);
}

bool validate_fin_contact_blocker(const Layer &layer, const Polygons *blockers,
                                  const Vec2d &wall, FinDiagnostics &diagnostics)
{
    if (blockers != nullptr && contains(*blockers, scaled_point(wall)))
        return diagnostics.reject(FinReject::blocked_contact, layer.print_z);
    return true;
}

// Ordinary contact-span validation; crease fronts never enter this sampling path.
bool validate_fin_contact_span(const Layer &layer, size_t layer_id, FinWallGeometry &walls,
                               const Vec2d &wall, const Vec2d &body_start, const Vec2d &axis,
                               double contact_width, double line_width, FinDiagnostics &diagnostics,
                               const Vec2d *front_normal = nullptr, bool painted_shape = false)
{
    const Vec2d &normal = front_normal != nullptr ? *front_normal : axis;
    const Vec2d tangent(-normal.y(), normal.x());
    const double bead_span = contact_width + line_width;
    const int samples = std::max(2, int(std::ceil(bead_span / line_width)));
    const int sample_count = samples + 1; // Both endpoints are sampled.
    // Round half the actual point count up at .5; legacy styles require every point.
    const int required_contacts = painted_shape ? (sample_count + 1) / 2 : sample_count;
    int contacts = 0;
    double rejected_distance = -1.;
    for (int i = 0; i < sample_count; ++i) {
        const Vec2d shift = (double(i) / samples - 0.5) * bead_span * tangent;
        const Vec2d edge = wall + shift;
        const auto hit = walls.contact(layer_id, body_start + shift, -axis, painted_shape);
        const double distance = hit ? (*hit - edge).norm() : -1.;
        if (!hit || distance > line_width) {
            if (!painted_shape)
                return diagnostics.reject(FinReject::contact_contour, layer.print_z, distance);
            rejected_distance = std::max(rejected_distance, distance);
        } else
            ++contacts;
    }
    return contacts >= required_contacts ||
        diagnostics.reject(FinReject::contact_contour, layer.print_z, rejected_distance);
}

struct PaintedContactLayer {
    FinContactSlice contact;
    Line attachment;
    Polyline body_boundary;
};

// Band-owned contact type also governs its taper. Each result cuts the complete
// body sides with that contact's axis, so overlapping bands cannot overwrite one
// another's attachment or change a point body into a finite crease path.
class PaintedFinContacts {
    const PrintObject &m_object;
    const TracedFin &m_fin;
    FinWallGeometry &m_walls;
    const std::vector<Polygons> &m_blockers;
    const std::vector<PaintedLayerGeometryCache> *m_cache;
    std::array<std::map<size_t, std::optional<PaintedContactLayer>>, 2> m_resolved;
public:
    PaintedFinContacts(const PrintObject &object, const TracedFin &fin,
                       FinWallGeometry &walls, const std::vector<Polygons> &blockers,
                       const std::vector<PaintedLayerGeometryCache> *cache)
        : m_object(object), m_fin(fin), m_walls(walls), m_blockers(blockers), m_cache(cache) {}

    const std::optional<PaintedContactLayer> &resolve(size_t i, bool crease)
    {
        auto &layers = m_resolved[crease ? 1 : 0];
        auto found = layers.find(i);
        if (found == layers.end())
            found = layers.emplace(i, build_layer(i, crease)).first;
        return found->second;
    }

private:
    std::optional<PaintedContactLayer> build_layer(size_t i, bool crease) const
    {
        if ((!m_fin.path_present.empty() && !m_fin.path_present[i]) ||
            (m_fin.seed.path && !m_fin.contact_sources[i]))
            return std::nullopt;
        const Layer &layer = *m_object.get_layer(int(i));
        PaintedContactLayer result;
        const double width = support_material_flow(&m_object, float(layer.height)).width();
        const double thickness = m_fin.seed.dimensions.thickness;
        const bool default_contact = crease == bool(m_fin.seed.crease_intent);
        std::optional<PaintedContactAxis> wall;
        if (default_contact) {
            wall = fin_layer_wall_contact(m_fin, i);
        } else {
            const auto source = painted_contact_axis(layer, m_fin.body_start[i], fin_body_direction(m_fin, i),
                crease ? m_fin.seed.crease_intent.get() : nullptr);
            if (!source)
                return std::nullopt;
            wall = resolve_fin_wall_contact(m_walls, i, m_fin.body_start[i], *source, crease, true);
        }
        if (!wall)
            return std::nullopt;
        result.contact = crease ?
            make_crease_fin_contact_slice(*wall, m_fin.seed.dimensions.contact_width, width) :
            make_ordinary_fin_contact_slice(*wall, m_fin.seed.dimensions.contact_width, width);
        if (default_contact) {
            if (!m_fin.center_caps[i] || m_fin.body_boundaries[i].points.size() < 3)
                return std::nullopt;
            result.attachment = *m_fin.center_caps[i];
            result.body_boundary = m_fin.body_boundaries[i];
            return result;
        }
        const auto &contact = result.contact;
        const auto cap = fin_cap_segment(m_fin.body_start[i], fin_body_direction(m_fin, i),
            contact.axis, 0.5 * (thickness - width), 0.5 * width);
        if (!cap || m_fin.body_axes[i].empty())
            return std::nullopt;
        auto axes = m_fin.body_axes[i];
        axes.back().cap_normal = contact.axis;
        axes.back().inner_junction = false;
        axes.back().center_cap = Line(scaled_point(cap->first), scaled_point(cap->second));
        const ExPolygons clearance = m_cache == nullptr ?
            offset_ex(layer.lslices, float(scale_(m_object.config().support_object_xy_distance.value + 0.5 * width)),
                      jtRound, DefaultMiterLimit) : ExPolygons{};
        std::optional<Line> attachment;
        result.body_boundary = fin_path_boundary(axes, 0.5 * (thickness - width), 0.5 * width,
            m_cache != nullptr ? (*m_cache)[i].final_model_clearance : clearance, attachment);
        if (!attachment || result.body_boundary.points.size() < 3)
            return std::nullopt;
        result.attachment = *attachment;
        return result;
    }

public:
    std::optional<PaintedContactLayer> continue_layer(size_t i, const FinContactContinuation &source) const
    {
        if ((!m_fin.path_present.empty() && !m_fin.path_present[i]) || m_fin.body_axes[i].empty())
            return std::nullopt;
        const Layer &layer = *m_object.get_layer(int(i));
        const double width = support_material_flow(&m_object, float(layer.height)).width();
        const double radius = 0.5 * (m_fin.seed.dimensions.thickness - width);
        if (radius <= EPSILON)
            return std::nullopt;
        auto axes = m_fin.body_axes[i];
        // Reconstruct this layer's original cap, rather than using the derived
        // attachment already shortened by the gap cut.
        axes.back().center_cap.reset();
        const ExPolygons clearance = m_cache == nullptr ?
            offset_ex(layer.lslices, float(scale_(m_object.config().support_object_xy_distance.value + 0.5 * width)),
                      jtRound, DefaultMiterLimit) : ExPolygons{};
        PaintedContactLayer result;
        result.contact = source.contact;
        std::optional<Line> attachment;
        result.body_boundary = fin_path_boundary(axes, radius, 0.5 * width,
            m_cache != nullptr ? (*m_cache)[i].final_model_clearance : clearance, attachment, &source.cap);
        if (!attachment || result.body_boundary.points.size() < 3)
            return std::nullopt;
        result.attachment = *attachment;
        return result;
    }

    bool validate(size_t i, const FinContactSlice &contact, bool crease, FinDiagnostics &diagnostics) const
    {
        const Layer &layer = *m_object.get_layer(int(i));
        if (!validate_fin_contact_blocker(layer,
                i < m_blockers.size() ? &m_blockers[i] : nullptr, contact.wall, diagnostics))
            return false;
        // Crease front construction is complete. Only ordinary contacts own
        // the lateral sampling and physical-span acceptance rule below.
        if (crease)
            return true;
        return validate_fin_contact_span(layer, i, m_walls,
            contact.wall, m_fin.body_start[i], contact.axis, m_fin.seed.dimensions.contact_width,
            support_material_flow(&m_object, float(layer.height)).width(), diagnostics,
            &contact.front_normal, true);
    }
};

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
    const Vec2d &contact_normal = fin.contact_directions[bottom];
    const Vec2d &projection_direction = ray->direction;
    const double slope = ray->slope;
    // Project just the front segment (a point for zero length). Its endpoints
    // retain the contact-axis orientation while its center descends in the fin plane
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
                DefaultJoinType,
                DefaultMiterLimit);
            clip.bed = offset_ex(bed, -float(scale_(0.5 * width)));
            clip.ready = true;
        }
        // The front retains the contact-axis orientation while its center
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
    const PrintObject &object, TracedFin &fin, size_t bottom,
    const Polygon &bed,
    const FinContactSlice &bottom_contact,
    const std::vector<ExPolygons> &exposure_body_centers,
    const std::vector<ExPolygons> &body_centers,
    std::vector<ExPolygons> &contacts, std::vector<ProjectionClip> &clips,
    std::vector<ExPolygons> *projection_copy,
    FinCenterExtension *extension,
    const std::vector<PaintedLayerGeometryCache> *geometry_cache,
    const FinContactContinuation *bottom_continuation,
    PaintedFinContacts &painted_contacts, bool crease)
{
    ProjectionResult result;
    auto log_skipped_layer = [&](const char *reason, size_t layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        std::ostringstream context;
        context << " island=" << fin.seed.intent_island.value_or(std::numeric_limits<size_t>::max())
            << " edge=" << fin.seed.intent_edge.value_or(std::numeric_limits<size_t>::max());
        if (fin.seed.crease_intent)
            context << " crease=" << fin.seed.crease_intent->crease
                << " pair=" << fin.seed.crease_intent->pair << " part=" << fin.seed.crease_intent->part;
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
            << " stage=painted-contact-projection skip=" << reason << context.str()
            << " band_bottom_layer=" << bottom
            << " band_bottom_slice_z=" << object.get_layer(int(bottom))->slice_z
            << " layer=" << layer_id << " slice_z=" << layer.slice_z << " print_z=" << layer.print_z
            << " wall_present=" << fin.wall_present[layer_id]
            << " body_regions=" << body_centers[layer_id].size();
    };
    if (extension != nullptr)
        extension->state = FinExtensionState::materializing;

    const double angle = object.config().fin_support_contact_projection_angle.value * M_PI / 180.;
    // Rotate about the direction perpendicular to the saved contact axis.
    // Changing a crease front normal does not rotate the existing taper plane.
    // Its retained half-space is cos(angle)*axis.dot(P.xy-X) +
    // sin(angle)*(P.z-X.z) >= 0. The hinge and plane never change per layer.
    const Vec2d axis = bottom_contact.axis;
    const Vec2d tangent(-axis.y(), axis.x());
    const Vec2d origin = bottom_contact.wall;
    const double bottom_z = object.get_layer(int(bottom))->slice_z;
    const double horizontal_factor = std::cos(angle);
    const double vertical_factor = std::sin(angle);
    std::vector<ExPolygons> planned(bottom);
    std::vector<bool> planned_exposed(bottom, false);
    FinContactContinuation retained_contact = *bottom_continuation;
    for (size_t remaining = bottom; remaining > 0; --remaining) {
        if (object.print()->canceled())
            throw CanceledException();
        const size_t layer_id = remaining - 1;
        const Layer &layer = *object.get_layer(int(layer_id));
        const double width = support_material_flow(&object, float(layer.height)).width();
        if (fin.seed.path && !fin.path_present[layer_id]) {
            result.stop = ProjectionStop::outside;
            break;
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
        ExPolygons transition;
        std::optional<Line> attachment;
        const Polyline *body_boundary = &fin.body_boundaries[layer_id];
        bool carried = true;
        const auto &local = painted_contacts.resolve(layer_id, crease);
        if (local && painted_contacts.validate(layer_id, local->contact, crease, fin.diagnostics)) {
            const Polygon trapezoid = fin_contact_center_polygon(
                local->contact.first, local->contact.second, local->attachment);
            if (trapezoid.points.size() >= 3) {
                transition = intersection_ex(Polygons{trapezoid}, *printable_bed);
                if (!complete_fin_contact_boundary(local->body_boundary, transition,
                        local->attachment, *printable_bed).empty()) {
                    body_boundary = &local->body_boundary;
                    attachment = local->attachment;
                    retained_contact = FinContactContinuation{
                        local->contact, local->attachment};
                    carried = false;
                } else
                    transition.clear();
            }
        }
        std::optional<PaintedContactLayer> continuation;
        if (carried) {
            continuation = painted_contacts.continue_layer(layer_id, retained_contact);
            if (continuation) {
                body_boundary = &continuation->body_boundary;
                attachment = continuation->attachment;
                const Polygon trapezoid = fin_contact_center_polygon(
                    continuation->contact.first, continuation->contact.second, *attachment);
                if (trapezoid.points.size() >= 3)
                    transition = intersection_ex(Polygons{trapezoid}, *printable_bed);
            }
        }
        if (transition.empty()) {
            log_skipped_layer("unavailable-contact-or-continuation", layer_id);
            continue;
        }

        // Bound a half-plane mask by this layer's own polygon, not by an
        // arbitrary global extent. Classify before dividing so 0/90 degrees
        // and planes already past the polygon need no distant coordinates.
        double min_s = std::numeric_limits<double>::infinity();
        double max_s = -min_s;
        double min_t = min_s;
        double max_t = -min_s;
        for (const ExPolygon &area : transition)
            for (const Point &point : area.contour.points) {
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
            projection = union_ex(transition);
        } else {
            const double s = cut / horizontal_factor;
            const Polygon retained{
                scaled_point(origin + s * axis + (min_t - EPSILON) * tangent),
                scaled_point(origin + (max_s + EPSILON) * axis + (min_t - EPSILON) * tangent),
                scaled_point(origin + (max_s + EPSILON) * axis + (max_t + EPSILON) * tangent),
                scaled_point(origin + s * axis + (max_t + EPSILON) * tangent)};
            projection = intersection_ex(transition, Polygons{retained});
        }
        projection = intersection_ex(projection, *printable_bed);
        if (projection.empty())
            continue;
        if (fin.seed.crease_intent && get_logging_level() >= 3) {
            std::vector<FinGeometryDebugSet> sets{
                {"trapezoid BEFORE fixed-plane cut", transition, {}, {}},
                {"trapezoid AFTER fixed-plane and bed cuts", projection, {}, {}},
                {"current member query body", body_centers[layer_id], {}, {}},
                {"fixed-plane origin and unit axis", {},
                    {Polyline{scaled_point(origin), scaled_point(origin + axis)}}, {scaled_point(origin)}}};
            if (attachment)
                sets.push_back({"source rear attachment", {}, {Polyline{attachment->a, attachment->b}}, {}});
            BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                << " stage=crease-taper-layer crease=" << fin.seed.crease_intent->crease
                << " pair=" << fin.seed.crease_intent->pair << " part=" << fin.seed.crease_intent->part
                << " layer=" << layer_id << " slice_z=" << layer.slice_z << " print_z=" << layer.print_z
                << " band_bottom=" << bottom << " band_bottom_slice_z=" << bottom_z
                << " source=" << (carried ? "carried" : "current-layer")
                << " plane_axis=" << axis.transpose() << " plane_origin=" << origin.transpose();
            export_fin_geometry_failure(object, fin, layer_id, "crease-taper-probe",
                carried ? "carried-trapezoid" : "current-layer-trapezoid", std::move(sets));
        }
        auto completed = complete_fin_contact_boundary(*body_boundary, projection, attachment, *printable_bed);
        if (completed.empty()) {
            log_skipped_layer("contact-boundary", layer_id);
            continue;
        }
        if (carried)
            retained_contact.cap = *attachment;
        if (extension != nullptr)
            extension->completed_layers[layer_id] = std::move(completed);
        planned_exposed[layer_id] = !diff_ex(projection, exposure_body_centers[layer_id]).empty();
        planned[layer_id] = std::move(projection);
        if (extension != nullptr) {
            extension->attachments[layer_id] = attachment;
            extension->layer_states[layer_id] = FinExtensionLayerState::materialized;
        }
    }
    // Publish successful taper layers; skipped layers contribute no geometry.
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
    return result;
}

ProjectionResult backfill_contact_projection(
    const PrintObject &object, TracedFin &fin, size_t bottom,
    const Polygon &bed, double thickness, double contact_width, double gap,
    const FinContactSlice &bottom_contact,
    const std::vector<ExPolygons> &exposure_body_centers,
    const std::vector<ExPolygons> &body_centers,
    std::vector<ExPolygons> &contacts, std::vector<ProjectionClip> &clips,
    std::vector<ExPolygons> *projection_copy,
    FinCenterExtension *extension,
    const std::vector<PaintedLayerGeometryCache> *geometry_cache,
    const FinContactContinuation *bottom_continuation,
    PaintedFinContacts &painted_contacts, bool crease)
{
    if (fin.seed.geometry_mode == FinGeometryMode::projected)
        return backfill_legacy_contact_projection(
            object, fin, bottom, bed, thickness, contact_width, gap,
            bottom_contact.surface_normal, body_centers, contacts, clips,
            projection_copy);
    return backfill_painted_contact_projection(
        object, fin, bottom, bed, bottom_contact,
        exposure_body_centers, body_centers,
        contacts, clips, projection_copy, extension, geometry_cache, bottom_continuation, painted_contacts, crease);
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

size_t append_closed_outlines(const ExPolygons &area, const ExPolygons &centerlines,
                              const Vec2d &normal, double inset, double line_width,
                              Polylines &outlines)
{
    // Missing contours are a failure, never a request to reconstruct them from area.
    if (centerlines.empty() ||
        offset_ex(area, -float(scale_(inset + 0.5 * line_width - EPSILON))).empty())
        return 0;
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

bool model_contact_samples(const Layer &layer, const Vec2d &point, const Vec2d &normal,
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
    if (!model_contact_samples(layer, point, normal, contact_width, line_width, walls, diagnostics))
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

// Paint eligibility is checked on source geometry. Actual contact width is
// checked against printed beads, along parallel copies of the resolved axis.

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
        auto dump_finalization = [&](const char *reason) {
            export_fin_geometry_failure(object, fin, i, "legacy-finalization", reason,
                {{"body centers", body_centers[i], {}, {}}, {"contacts", contact_sections[i], {}, {}},
                 {"taper", projections[i], {}, {}}, {"combined centers", fin.centers[i], {}, {}},
                 {"physical envelope", area, {}, {}}});
        };
        if (topology != 0) {
            dump_finalization("topology");
            return fin.diagnostics.reject(
                FinReject::topology, object.get_layer(int(i))->print_z, topology);
        }
        if (offset_ex(area, -float(scale_(width - EPSILON))).empty()) {
            dump_finalization("thin-body");
            return fin.diagnostics.reject(FinReject::thin_body, object.get_layer(int(i))->print_z);
        }
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

bool generate_fin_geometry(const PrintObject &object, TracedFin &fin, const Polygon &bed,
                           FinWallGeometry &walls, const std::vector<PaintedLayer> &painted,
                           const std::vector<Polygons> &blockers,
                           bool include_seed_contact = true,
                           FinComponentCenterGeometry *component_geometry = nullptr,
                           const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr,
                           bool end_has_downstream = false)
{
    const PrintObjectConfig &config = object.config();
    const double thickness = fin.seed.dimensions.thickness;
    const double contact_width = fin.seed.dimensions.contact_width;
    const double gap = config.support_object_xy_distance.value;
    const FinContactBand root_band = seed_fin_contact_band(object, fin.seed);
    const size_t count = fin.boundary.size();
    const bool painted_shape = fin.seed.geometry_mode == FinGeometryMode::painted_shape;
    const bool staged_painted = component_geometry != nullptr && painted_shape;
    std::vector<ExPolygons> bodies(count), body_centers(count);
    PaintedFinContacts painted_contacts(object, fin, walls, blockers, geometry_cache);
    ExPolygons painted_bed_foot;
    std::vector<ExPolygons> contact_sections(count), projections(count);
    auto dump_geometry_failure = [&](size_t layer, const char *reason) {
        std::vector<FinGeometryDebugSet> sets;
        if (layer < count) {
            sets.push_back({"body centers", body_centers[layer], {}, {}});
            sets.push_back({"body physical", bodies[layer], {}, {}});
            sets.push_back({"accepted contacts", contact_sections[layer], {}, {}});
            sets.push_back({"accepted taper", projections[layer], {}, {}});
        }
        if (component_geometry)
            append_fin_component_debug(sets, *component_geometry, layer);
        export_fin_geometry_failure(object, fin, layer, "member-geometry", reason, std::move(sets));
    };
    std::vector<ProjectionClip> clips(count);
    if (component_geometry != nullptr) {
        component_geometry->complete = false;
        component_geometry->body_centers.clear();
        component_geometry->completed_centers.clear();
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
    fin.wall_present.assign(count, false);
    fin.contacts.assign(count, false);
    fin.body_axes.assign(count, {});
    fin.body_boundaries.assign(count, {});
    fin.center_caps.assign(count, std::nullopt);
    bool reported_empty_body = false;
    for (size_t i = 0; i < count; ++i) {
        if (!fin.path_present.empty() && !fin.path_present[i])
            continue;
        // Preserve source topology independently of whether printed walls are
        // found. A missed bead must not turn a model-facing end into a junction.
        const bool has_source_contact = fin.contact_sources[i].has_value();
        if (has_source_contact) {
            const bool crease_contact = bool(fin.seed.crease_intent);
            const Vec2d origin = crease_contact ? *fin.contact_sources[i] : fin.body_start[i];
            const Vec2d inward = crease_contact ?
                Vec2d(-fin.contact_front_normals[i]) : Vec2d(-fin.contact_directions[i]);
            const auto contact = resolve_fin_wall_contact(walls, i, fin.body_start[i],
                PaintedContactAxis{*fin.contact_sources[i], fin.contact_directions[i], fin.contact_front_normals[i]},
                crease_contact, painted_shape);
            fin.wall_present[i] = contact.has_value();
            if (contact) {
                fin.boundary[i] = contact->contact_point;
                fin.contact_directions[i] = contact->contact_direction;
                fin.contact_front_normals[i] = contact->front_normal;
                fin.body_cap_normals[i] = contact->contact_direction;
            }
            BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name
                << " stage=fin-wall-contact island=" << fin.seed.intent_island.value_or(size_t(-1))
                << " edge=" << fin.seed.intent_edge.value_or(size_t(-1))
                << " layer=" << i << " slice_z=" << object.get_layer(int(i))->slice_z
                << " A1=" << fin.body_start[i].transpose()
                << " source=" << fin.contact_sources[i]->transpose()
                << " query_origin=" << origin.transpose() << " query_direction=" << inward.transpose()
                << " axis=" << fin.contact_directions[i].transpose()
                << " hit=" << contact.has_value()
                << " distance=" << (contact ? (contact->contact_point - origin).norm() : -1.)
                << " signed_distance=" << (contact ? (contact->contact_point - origin).dot(inward) : 0.);
        }
        const double line_width = support_material_flow(
            &object, float(object.get_layer(int(i))->height)).width();
        if (fin.seed.geometry_mode == FinGeometryMode::painted_shape) {
            const auto cap = fin_cap_segment(fin.body_start[i], fin_body_direction(fin, i),
                fin.body_cap_normals[i], 0.5 * (thickness - line_width), 0.5 * line_width);
            if (cap)
                fin.center_caps[i] = Line(scaled_point(cap->first), scaled_point(cap->second));
        }
        if (fin.seed.path) {
            const Points &points = fin.layer_paths[i].points;
            for (size_t j = 1; j < points.size(); ++j) {
                const Vec2d outer = unscale(points[j - 1]);
                const Vec2d inner = unscale(points[j]);
                const Vec2d direction = (outer - inner).normalized();
                const bool bend = j + 1 < points.size();
                const bool inner_junction = bend || (!has_source_contact && fin.seed.path->end_junction);
                fin.body_axes[i].push_back({outer, inner, direction,
                    bend ? direction : fin.body_cap_normals[i], fin.seed.intent_edge, true,
                    inner_junction, inner_junction ? std::nullopt : fin.center_caps[i]});
            }
        } else {
            const Vec2d outer_center = fin.seed.geometry_mode == FinGeometryMode::painted_shape ?
                fin.tips[i] - 0.5 * line_width * fin.seed.normal : fin.tips[i];
            fin.body_axes[i].push_back({outer_center, fin.body_start[i],
                fin.seed.normal, fin.body_cap_normals[i], fin.seed.intent_edge, false, false,
                fin.center_caps[i]});
        }
        if (fin.seed.path && fin.seed.intent_root && !fin.body_axes[i].empty()) {
            const auto &root = *fin.seed.intent_root;
            const double drop = std::max(0., root.z - object.get_layer(int(i))->slice_z);
            const double reach = object.config().fin_support_depth.value + drop * fin.seed.expansion_slope;
            if (reach > 0.5 * line_width) {
                const Polyline extension{scaled_point(root.point),
                    scaled_point(root.point + reach * root.extension_direction)};
                const ExPolygons computed = geometry_cache == nullptr || i >= geometry_cache->size() ?
                    offset_ex(object.get_layer(int(i))->lslices, float(scale_(gap)),
                              jtRound, DefaultMiterLimit) : ExPolygons{};
                const ExPolygons &clearance = geometry_cache != nullptr && i < geometry_cache->size() ?
                    (*geometry_cache)[i].model_gap : computed;
                const auto retained = clipped_fin_path(extension, clearance);
                if (retained && (unscale(retained->last_point()) - root.point).norm() > 0.5 * line_width) {
                    // The root-depth end stays flat. Its connection to the
                    // intent is now an internal vertex of the same open path.
                    const Vec2d outer = unscale(retained->last_point()) -
                        0.5 * line_width * root.extension_direction;
                    fin.body_axes[i].insert(fin.body_axes[i].begin(),
                        FinBodyAxis{outer, fin.body_axes[i].front().outer,
                            root.extension_direction, root.extension_direction,
                            fin.seed.intent_edge, false, true});
                }
            }
        }
        const ExPolygons *cached_bed_center =
            geometry_cache != nullptr && i < geometry_cache->size() ?
                &(*geometry_cache)[i].bed_center : nullptr;
        build_fin_body_section(
            object, fin, i, bed, thickness, gap, bodies[i], body_centers[i],
            component_geometry == nullptr || fin.seed.geometry_mode == FinGeometryMode::projected,
            cached_bed_center, &painted_bed_foot,
            geometry_cache != nullptr && i < geometry_cache->size() ?
                &(*geometry_cache)[i].final_model_clearance : nullptr);
        if (fin.seed.geometry_mode == FinGeometryMode::painted_shape && body_centers[i].empty() && !reported_empty_body) {
            dump_geometry_failure(i, "empty-member-body");
            reported_empty_body = true;
        }
    }
    // A band crossing the retained path's stop is clipped there. Its top stays
    // on the original seed/periodic schedule; no lower fragment creates a band.
    // Without a downstream line, losing the finite endpoint starts tapering
    // immediately below the last contact instead of carrying the band onward.
    // A missing printed bead is a contact failure, not a lost source endpoint.
    auto clip_contact_band = [&](FinContactBand band) {
        if (fin.seed.path)
            for (size_t remaining = band.end; remaining > band.begin; --remaining)
                if (!fin.path_present[remaining - 1] ||
                    (fin.seed.path && !end_has_downstream && !fin.contact_sources[remaining - 1])) {
                    band.begin = remaining;
                    break;
                }
        return band;
    };
    struct PreparedContactBand {
        std::vector<ExPolygons> sections;
        std::vector<ExPolygons> completed;
        std::vector<std::optional<Line>> attachments;
        std::vector<FinContactSlice> contacts;
        std::vector<bool> actual_contacts;
        std::optional<FinContactContinuation> bottom_continuation;
    };
    // 0: invalid top, 1: completed band, 2: suppressed, -1: legacy geometry failure.
    auto prepare_band = [&](const FinContactBand &band, PreparedContactBand &prepared,
                            bool crease, const std::vector<ExPolygons> *suppression = nullptr) {
        if (band.begin >= band.end)
            return 0;
        ++fin.planned_contact_bands;
        const size_t band_size = band.end - band.begin;
        prepared.contacts.resize(band_size);
        prepared.actual_contacts.assign(band_size, false);
        prepared.sections.resize(band_size);
        prepared.completed.resize(band_size);
        prepared.attachments.resize(band_size);
        if (!painted_shape) {
            for (size_t i = band.begin; i < band.end; ++i) {
                const size_t index = i - band.begin;
                const Layer &layer = *object.get_layer(int(i));
                const double width = support_material_flow(&object, float(layer.height)).width();
                const Polygons *layer_blockers = i < blockers.size() ? &blockers[i] : nullptr;
                auto contact = legacy_fin_layer_contact(fin, i, contact_width, width);
                if (!contact) {
                    fin.diagnostics.reject(fin.wall_present[i] ? FinReject::contact_axis : FinReject::missing_wall,
                                           layer.print_z);
                    return 0;
                }
                if (fin.seed.requires_painted_contact && !validate_fin_contact(
                        layer, painted[i], layer_blockers, true, *fin.contact_sources[i], fin.seed.normal,
                        contact_width, width, contact->surface_normal, fin.diagnostics, i != fin.seed.top_layer,
                        fin.seed.surface_normals == FinSurfaceNormalPolicy::painted_region ? &fin.seed.macro_normal : nullptr,
                        fin.seed.wall_follow_island.value_or(std::numeric_limits<size_t>::max())))
                    return 0;
                if (!validate_fin_contact_blocker(layer, layer_blockers, contact->wall, fin.diagnostics) ||
                    !validate_fin_contact_span(layer, i, walls, contact->wall,
                        fin.body_start[i], contact->axis, contact_width, width, fin.diagnostics))
                    return 0;
                prepared.contacts[index] = *contact;
                prepared.actual_contacts[index] = true;
            }
            for (size_t i = band.begin; i < band.end; ++i) {
                const size_t index = i - band.begin;
                const Layer &layer = *object.get_layer(int(i));
                const double width = support_material_flow(&object, float(layer.height)).width();
                double failure_stage = 0.;
                prepared.sections[index] = build_fin_contact_section(
                    bed, fin, i, prepared.contacts[index], contact_width, thickness, width, &failure_stage);
                if (prepared.sections[index].empty()) {
                    dump_geometry_failure(i, "empty-neck");
                    fin.diagnostics.reject(FinReject::empty_neck, layer.print_z, failure_stage);
                    return -1;
                }
            }
            return 1;
        }
        std::optional<FinContactContinuation> retained;
        for (size_t step = 0; step < band_size; ++step) {
            const size_t i = band.end - 1 - step;
            const size_t index = i - band.begin;
            const Layer &layer = *object.get_layer(int(i));
            const double width = support_material_flow(&object, float(layer.height)).width();
            const ExPolygons computed_bed = geometry_cache == nullptr ?
                offset_ex(bed, -float(scale_(0.5 * width))) : ExPolygons{};
            const ExPolygons &bed_center = geometry_cache != nullptr ? (*geometry_cache)[i].bed_center : computed_bed;
            const auto &local = painted_contacts.resolve(i, crease);
            ExPolygons section;
            if (local) {
                const Polygon trapezoid = fin_contact_center_polygon(
                    local->contact.first, local->contact.second, local->attachment);
                if (trapezoid.points.size() >= 3)
                    section = intersection_ex(Polygons{trapezoid}, bed_center);
                // Like blockers, seed suppression is contact eligibility. Resolve
                // only this top section before deciding whether to build a band.
                if (step == 0 && suppression != nullptr && !(*suppression)[i].empty() &&
                    !section.empty() && !intersection_ex(
                        offset_ex(section, float(scale_(0.5 * width))),
                        offset_ex((*suppression)[i], float(scale_(0.5 * width)))).empty()) {
                    --fin.planned_contact_bands;
                    ++fin.suppressed_contact_bands;
                    return 2;
                }
                ExPolygons completed;
                if (painted_contacts.validate(i, local->contact, crease, fin.diagnostics))
                    completed = complete_fin_contact_boundary(local->body_boundary, section,
                        local->attachment, bed_center);
                if (!completed.empty()) {
                    prepared.contacts[index] = local->contact;
                    prepared.actual_contacts[index] = true;
                    prepared.attachments[index] = local->attachment;
                    prepared.sections[index] = std::move(section);
                    prepared.completed[index] = std::move(completed);
                    retained = FinContactContinuation{local->contact, local->attachment};
                    continue;
                }
            }
            if (step == 0)
                return 0;
            // All lower failures use the same continuation, including failed
            // queries, blockers, span coverage, caps and owned-boundary joins.
            prepared.contacts[index] = retained->contact;
            auto &attachment = prepared.attachments[index];
            const auto continuation = painted_contacts.continue_layer(i, *retained);
            ExPolygons completed;
            if (continuation) {
                attachment = continuation->attachment;
                const Polygon trapezoid = fin_contact_center_polygon(
                    retained->contact.first, retained->contact.second, *attachment);
                if (trapezoid.points.size() >= 3)
                    section = intersection_ex(Polygons{trapezoid}, bed_center);
                else
                    section.clear();
                completed = complete_fin_contact_boundary(
                    continuation->body_boundary, section, attachment, bed_center);
            }
            if (!completed.empty()) {
                retained->cap = *attachment;
                prepared.sections[index] = std::move(section);
                prepared.completed[index] = std::move(completed);
            } else {
                // A local attachment failure does not reject the member.
                dump_geometry_failure(i, "contact-continuation");
            }
        }
        prepared.bottom_continuation = retained;
        return 1;
    };
    auto append_band = [&](const FinContactBand &band, PreparedContactBand &prepared,
                           std::vector<ExPolygons> *copy) -> std::optional<size_t> {
        ++fin.accepted_contact_bands;
        std::optional<size_t> extension_id;
        if (component_geometry != nullptr) {
            extension_id = component_geometry->extensions.size();
            FinCenterExtension &extension = component_geometry->extensions.emplace_back();
            extension.band = band;
            extension.layers.resize(count);
            extension.completed_layers.resize(count);
            extension.attachments.resize(count);
            extension.layer_states.assign(count, FinExtensionLayerState::inactive);
        }
        for (size_t i = band.begin; i < band.end; ++i) {
            const size_t index = i - band.begin;
            fin.contacts[i] = fin.contacts[i] || prepared.actual_contacts[index];
            if (copy != nullptr)
                expolygons_append((*copy)[i], prepared.sections[index]);
            if (extension_id.has_value()) {
                FinCenterExtension &extension =
                    component_geometry->extensions[*extension_id];
                extension.layers[i] = prepared.sections[index];
                extension.completed_layers[i] = std::move(prepared.completed[index]);
                extension.attachments[i] = prepared.attachments[index];
                extension.layer_states[i] = prepared.sections[index].empty() ? FinExtensionLayerState::inactive :
                    prepared.actual_contacts[index] ?
                    FinExtensionLayerState::contact : FinExtensionLayerState::materialized;
            }
            if (!staged_painted)
                expolygons_append(
                    prepared.actual_contacts[index] ? contact_sections[i] : projections[i],
                    std::move(prepared.sections[index]));
        }
        return extension_id;
    };

    // The seed contact is independent of the periodic bed-anchored schedule.
    // Build it first so its actual downward projection can suppress redundant
    // periodic contacts below it without regenerating either band.
    std::vector<ExPolygons> seed_reach(count);
    if (include_seed_contact) {
        const FinContactBand seed_band = clip_contact_band(root_band);
        PreparedContactBand prepared;
        const int seed_status = prepare_band(seed_band, prepared, bool(fin.seed.crease_intent));
        if (fin.seed.crease_intent) {
            BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                << " stage=crease-root-band crease=" << fin.seed.crease_intent->crease
                << " pair=" << fin.seed.crease_intent->pair << " part=" << fin.seed.crease_intent->part
                << " source_top_z=" << fin.seed.crease_intent->points.front().z()
                << " candidate_slice_z=" << object.get_layer(int(fin.seed.top_layer))->slice_z
                << " candidate_print_z=" << object.get_layer(int(fin.seed.top_layer))->print_z
                << " configured_height=" << config.fin_support_contact_height.value
                << " planned_begin=" << root_band.begin << " planned_end_exclusive=" << root_band.end
                << " retained_begin=" << seed_band.begin << " retained_end_exclusive=" << seed_band.end
                << " planned_layers=" << root_band.end - root_band.begin
                << " retained_layers=" << seed_band.end - seed_band.begin << " status=" << seed_status;
            for (size_t i = root_band.begin; i < root_band.end; ++i) {
                const Layer &layer = *object.get_layer(int(i));
                std::ostringstream detail;
                detail.precision(17);
                detail << " layer=" << i << " slice_z=" << layer.slice_z << " print_z=" << layer.print_z
                       << " path_present=" << (fin.path_present.empty() || fin.path_present[i])
                       << " wall_hit=" << fin.wall_present[i]
                       << " accepted_contact=" << (seed_status == 1 && i >= seed_band.begin &&
                            prepared.actual_contacts[i - seed_band.begin])
                       << " A1=" << fin.body_start[i].transpose()
                       << " axis=" << fin.contact_directions[i].transpose()
                       << " front_normal=" << fin.contact_front_normals[i].transpose();
                if (fin.contact_sources[i])
                    detail << " source_xy=" << fin.contact_sources[i]->transpose();
                if (fin.wall_present[i]) {
                    detail << " physical_hit_xy=" << fin.boundary[i].transpose();
                    if (fin.contact_sources[i]) {
                        const Vec2d delta = fin.boundary[i] - *fin.contact_sources[i];
                        const Vec2d &normal = fin.contact_front_normals[i];
                        detail << " hit_sideways_mm=" << delta.dot(Vec2d(-normal.y(), normal.x()));
                    }
                }
                BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                    << " stage=crease-root-layer crease=" << fin.seed.crease_intent->crease
                    << " pair=" << fin.seed.crease_intent->pair << " part=" << fin.seed.crease_intent->part
                    << detail.str();
            }
        }
        if (seed_status < 0)
            return false;
        if (seed_status == 0) {
            dump_geometry_failure(fin.seed.top_layer, "top-band");
            return fin.diagnostics.reject(FinReject::top_band,
                                          object.get_layer(int(fin.seed.top_layer))->print_z);
        }
        const std::optional<size_t> extension_id =
            append_band(seed_band, prepared, &seed_reach);
        ++fin.seed_contact_bands;
        if (seed_band.begin > 0) {
            FinCenterExtension *extension = extension_id.has_value() ?
                &component_geometry->extensions[*extension_id] : nullptr;
            const ProjectionResult projection = backfill_contact_projection(
                object, fin, seed_band.begin, bed, thickness, contact_width, gap,
                prepared.contacts.front(), body_centers, body_centers,
                projections, clips, &seed_reach, extension, geometry_cache,
                prepared.bottom_continuation ? &*prepared.bottom_continuation : nullptr,
                painted_contacts, bool(fin.seed.crease_intent));
            record_projection(projection);
        }
        if (extension_id.has_value() && seed_band.begin == 0)
            component_geometry->extensions[*extension_id].state = FinExtensionState::complete;
    }

    const std::vector<FinContactBand> periodic_bands = periodic_fin_contact_bands(object, fin);
    for (auto band_it = periodic_bands.rbegin(); band_it != periodic_bands.rend(); ++band_it) {
        const FinContactBand band = clip_contact_band(*band_it);
        PreparedContactBand prepared;
        const int status = prepare_band(band, prepared, fin.seed.path && fin.seed.crease_intent,
            painted_shape ? &seed_reach : nullptr);
        if (status < 0)
            return false;
        if (status == 0 || status == 2)
            continue;
        const size_t top_layer = band.end - 1;
        const Layer &layer = *object.get_layer(int(top_layer));
        const double line_width = support_material_flow(&object, float(layer.height)).width();
        const bool reaches_seed = !painted_shape && !seed_reach[top_layer].empty() && !intersection_ex(
            offset_ex(prepared.sections.back(), float(scale_(0.5 * line_width))),
            offset_ex(seed_reach[top_layer], float(scale_(0.5 * line_width)))).empty();
        if (reaches_seed) {
            --fin.planned_contact_bands;
            ++fin.suppressed_contact_bands;
            continue;
        }
        const std::optional<size_t> extension_id =
            append_band(band, prepared, nullptr);
        if (band.begin > 0) {
            FinCenterExtension *extension = extension_id.has_value() ?
                &component_geometry->extensions[*extension_id] : nullptr;
            const ProjectionResult projection = backfill_contact_projection(
                object, fin, band.begin, bed, thickness, contact_width, gap,
                prepared.contacts.front(), body_centers, body_centers,
                projections, clips, nullptr, extension, geometry_cache,
                prepared.bottom_continuation ? &*prepared.bottom_continuation : nullptr,
                painted_contacts, fin.seed.path && fin.seed.crease_intent);
            record_projection(projection);
        }
        if (extension_id.has_value() && band.begin == 0)
            component_geometry->extensions[*extension_id].state = FinExtensionState::complete;
    }
    if (include_seed_contact && !fin.contacts.back()) {
        dump_geometry_failure(fin.seed.top_layer, "top-band");
        return fin.diagnostics.reject(FinReject::top_band, object.get_layer(int(fin.seed.top_layer))->print_z);
    }

    if (component_geometry != nullptr) {
        // Establish fixed extension invariants before publishing the member.
        // Layer assembly only checks each layer's state and geometry.
        component_geometry->complete = std::all_of(
            component_geometry->extensions.begin(), component_geometry->extensions.end(),
            [](const FinCenterExtension &extension) {
                assert(extension.layers.size() == extension.layer_states.size());
                assert(extension.layers.size() == extension.attachments.size());
                return extension.state == FinExtensionState::complete;
            });
        if (!component_geometry->complete) {
            dump_geometry_failure(std::numeric_limits<size_t>::max(), "incomplete-extensions");
            return false;
        }
        if (fin.seed.geometry_mode == FinGeometryMode::painted_shape) {
            // Normalized body regions support queries; the owned perimeter
            // retains the exact attachment points needed for output assembly.
            component_geometry->body_centers = body_centers;
            component_geometry->completed_centers.assign(count, {});
            for (size_t layer = 0; layer < count; ++layer) {
                auto &completed = component_geometry->completed_centers[layer];
                for (FinCenterExtension &extension : component_geometry->extensions)
                    expolygons_append(completed, std::move(extension.completed_layers[layer]));
                if (completed.empty() && fin.body_boundaries[layer].points.size() >= 3) {
                    Polygon boundary(fin.body_boundaries[layer].points);
                    boundary.make_counter_clockwise();
                    completed.emplace_back(std::move(boundary));
                }
                // Bed feet are intentional additional material, independent of
                // the contact attachment. Query bodies themselves are not added.
                if (layer == 0)
                    expolygons_append(completed, painted_bed_foot);
                const ExPolygons computed_bed = geometry_cache == nullptr || layer >= geometry_cache->size() ?
                    offset_ex(bed, -float(scale_(0.5 * support_material_flow(
                        &object, float(object.get_layer(int(layer))->height)).width()))) : ExPolygons{};
                const ExPolygons &bed_center = geometry_cache != nullptr && layer < geometry_cache->size() ?
                    (*geometry_cache)[layer].bed_center : computed_bed;
                completed = intersection_ex(union_ex(completed), bed_center);
                if (fin.seed.crease_intent && include_seed_contact && get_logging_level() >= 3 &&
                    layer >= root_band.begin && layer < root_band.end) {
                    std::vector<FinGeometryDebugSet> sets;
                    append_fin_component_debug(sets, *component_geometry, layer);
                    export_fin_geometry_failure(object, fin, layer, "crease-root-probe",
                        fin.contacts[layer] ? "actual-contact" : "no-actual-contact", std::move(sets));
                }
            }
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
        if (!fin.path_present.empty() && !fin.path_present[i])
            continue;
        const Layer &layer = *object.get_layer(int(i));
        const Flow flow = support_material_flow(&object, float(layer.height));
        for (int wall = 0; wall < wall_count; ++wall) {
            Polyline outline;
            if (!closed_outline(fin.areas[i], wall == 0 ? fin.centers[i] : ExPolygons{}, fin.seed.normal,
                                0.5 * flow.width() + wall * flow.spacing(), flow.width(), outline))
                break;
            fin.outlines[i].push_back(std::move(outline));
        }
        if (fin.outlines[i].empty()) {
            export_fin_geometry_failure(object, fin, i, "toolpaths", "no-outline",
                {{"physical envelope", fin.areas[i], {}, {}},
                 {"center regions", fin.centers[i], {}, {}}});
            return fin.diagnostics.reject(FinReject::no_outline, layer.print_z);
        }
    }
    return true;
}

void generate_fin_foot_toolpaths(const PrintObject &object, ExPolygons areas, ExPolygons centers,
                                Polylines &paths, Polygons &fill)
{
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
    dimensions = fin_support_dimensions(object);
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
        higher.seed.intent_island != lower.seed.intent_island ||
        higher.seed.intent_edge != lower.seed.intent_edge ||
        higher.seed.intent_line != lower.seed.intent_line ||
        higher.seed.surface_normals != lower.seed.surface_normals ||
        higher.seed.geometry_mode != lower.seed.geometry_mode ||
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
                           TracedFin &fin, FinWallGeometry &walls, bool include_seed_contact = true,
                           FinComponentCenterGeometry *component_geometry = nullptr,
                           const std::vector<PaintedLayerGeometryCache> *geometry_cache = nullptr)
{
    fin = TracedFin{};
    fin.seed = candidate;
    if (!trace_fin(object, bed, candidate, object.config().fin_support_depth.value,
                   object.config().support_object_xy_distance.value,
                   candidate.dimensions.thickness, line_width, fin,
                   geometry_cache) ||
        !generate_fin_geometry(object, fin, bed, walls, painted, blockers,
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
            const FinSupportDimensions dimensions = fin_support_dimensions(object);
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
                const double turn = fin_contour_turn(incoming, outgoing, orientation);
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
    double emitted_top_z = 0.; // Highest layer with actual extrusion entities.
    size_t merged_outline_failures = 0;
    size_t merged_outline_splits = 0;
};

FinEmitStats emit_fin_support(PrintObject &object, const std::vector<TracedFin> &fins,
                              const std::vector<ExPolygons> *merged_layer_centers = nullptr)
{
    FinEmitStats stats;
    size_t support_id = 0;
    for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
        const Layer &layer = *object.get_layer(int(layer_id));
        Polylines paths;
        Polygons feet;
        const Flow flow = support_material_flow(&object, float(layer.height));
        ExPolygons areas, centers;
        if (merged_layer_centers != nullptr) {
            // Painted Shape's common gap result is the output geometry. Expand
            // it once here for the physical envelope, inner walls and bed fill;
            // never split it back into candidates for physical rejection.
            centers = (*merged_layer_centers)[layer_id];
            areas = offset_ex(centers, float(scale_(0.5 * flow.width())),
                              jtRound, double(scale_(0.005)));
        } else if (layer_id == 0) {
            for (const TracedFin &fin : fins) {
                expolygons_append(areas, fin.areas.front());
                if (!fin.centers.empty())
                    expolygons_append(centers, fin.centers.front());
            }
        }
        if (layer_id == 0) {
            generate_fin_foot_toolpaths(object, std::move(areas), std::move(centers), paths, feet);
        } else if (merged_layer_centers != nullptr) {
            const int wall_count = std::max(1, object.config().tree_support_wall_count.value);
            for (size_t area_id = 0; area_id < areas.size(); ++area_id) {
                const ExPolygon &area = areas[area_id];
                const ExPolygons component_centers = intersection_ex(centers, ExPolygons{area});
                Vec2d normal = Vec2d::UnitX();
                const TracedFin *representative = nullptr;
                for (const TracedFin &fin : fins) {
                    // Per-fin centers retain pre-clearance provenance only.
                    if (layer_id < fin.centers.size() && !intersection_ex(
                            ExPolygons{area}, fin.centers[layer_id]).empty()) {
                        normal = fin.seed.normal;
                        representative = &fin;
                        break;
                    }
                }
                for (int wall = 0; wall < wall_count; ++wall) {
                    const double inset = 0.5 * flow.width() + wall * flow.spacing();
                    // Preserve the completed center boundary for the required first wall.
                    // Only optional inner walls are derived by eroding the physical area.
                    const ExPolygons wall_centers = wall == 0 ? component_centers :
                        offset_ex(ExPolygons{area}, -float(scale_(inset)));
                    const size_t generated = append_closed_outlines(
                        ExPolygons{area}, wall_centers, normal, inset, flow.width(), paths);
                    if (generated == 0) {
                        // Inner-wall exhaustion is normal and keeps the preceding walls.
                        if (wall == 0) {
                            ++stats.merged_outline_failures;
                            BOOST_LOG_TRIVIAL(info)
                                << "Fin support object=" << object.model_object()->name
                                << " stage=painted-shape-merged-toolpaths reject=no-outline"
                                << " layer=" << layer_id
                                << " slice_z=" << layer.slice_z << " print_z=" << layer.print_z
                                << " region=" << area_id
                                << " center_regions=" << component_centers.size()
                                << " line_width=" << flow.width()
                                << " skipped_scope=layer-region";
                            if (representative != nullptr)
                                // Source identity belongs to one contributing fin; these
                                // panels describe the complete merged output region.
                                export_fin_geometry_failure(object, *representative, layer_id,
                                    "merged-toolpaths", "no-outline",
                                    {{"merged physical envelope", ExPolygons{area}, {}, {}},
                                     {"merged first-loop centers", component_centers, {}, {}},
                                     {"first-loop thickness probe", offset_ex(ExPolygons{area},
                                         -float(scale_(flow.width() - EPSILON))), {}, {}}});
                        }
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
        if (!support_layer->support_fills.entities.empty())
            stats.emitted_top_z = std::max(stats.emitted_top_z, layer.print_z);
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
        const FinSupportDimensions dimensions = fin_support_dimensions(object);
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
    const Vec2d edge = b - a;
    const double denominator = cross2(edge, direction);
    if (std::abs(denominator) <= 1e-10) {
        if (std::abs(cross2(a - origin, direction)) > 1e-7)
            return std::nullopt;
        const double length_squared = edge.squaredNorm();
        const double position = length_squared > EPSILON * EPSILON ?
            std::clamp((origin - a).dot(edge) / length_squared, 0., 1.) : 0.;
        return a + position * edge;
    }
    const double position = cross2(origin - a, direction) / denominator;
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

struct PaintedFinTarget;
struct PaintedFinSpan;
struct PaintedFinRoot;

// One mutable entity per actual junction. Source measurements stay read-only;
// correction splits references only when the resulting positions differ.
struct PaintedFinNode {
    size_t id;
    const PaintedIntentNode *source;
    Vec2d point;
    PaintedFinTarget *attachment_host = nullptr;
    std::optional<double> z;
    std::vector<PaintedFinSpan *> incoming, outgoing;
    PaintedFinRoot *root = nullptr;
    std::optional<Vec2d> root_direction; // Direct crease source's XY exterior bisector.
};

struct PaintedFinTarget {
    Vec3d point;
    Vec2d macro_direction = Vec2d::Zero();
    Vec2d direction = Vec2d::Zero();
    std::optional<size_t> edge;
    // Endpoints first, followed by interior junction references.
    std::vector<PaintedFinNode *> nodes;
    bool line = false;
    Vec3d macro_normal = Vec3d::Zero();
    bool starts_at_root = false;
    bool usable = true;
    std::optional<std::pair<size_t, size_t>> source_attachment_junction;
};

struct PaintedFinSource {
    PaintedFinTarget point;
    std::optional<PaintedFinTarget> line;
};

struct PaintedFinIsland {
    const PaintedIsland *source;
    // deque preserves entity addresses while correction adds split/free nodes.
    std::deque<PaintedFinNode> nodes;
    std::vector<PaintedFinSource> sources;
};

struct PreparedPaintedFin {
    FinCandidate attributes;
    // Ordered path references, including bends and interior junctions.
    // Empty for a point candidate. The producer supplies traversal order;
    // fin-intent resolution must not recover it from an endpoint chord.
    std::vector<PaintedFinNode *> nodes;
};

struct PaintedFinWork {
    std::vector<std::unique_ptr<PaintedFinIsland>> islands;
    std::vector<PreparedPaintedFin> candidates;
};

struct PaintedFinGroup {
    const FinCandidate *attributes;
    std::vector<PaintedFinNode *> nodes; // Directed whole path, including interior junctions.
};

// Directed adjacency for root detection and missing-height dependencies only.
// A span does not own an activation height, contact schedule or clipped body.
struct PaintedFinSpan {
    PaintedFinGroup *group;
    PaintedFinNode *start;
    PaintedFinNode *end;
};

struct PaintedFinRoot {
    PaintedFinNode *node;
    Vec2d extension_direction = Vec2d::Zero();
    bool valid = false;
};

struct PaintedFinSite {
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
};

double painted_fin_expansion_slope(const Vec3d &normal, const Vec2d &direction, double factor)
{
    const double horizontal = normal.head<2>().norm();
    const double lean = -normal.z() / horizontal *
        std::max(0., Vec2d(normal.head<2>() / horizontal).dot(direction));
    const double weight = 1 - std::clamp(std::atan(lean) / (M_PI / 6.), 0., 1.);
    return factor * weight * weight;
}

// Prepare physical attributes without copying the shared graph. Geometry and
// endpoint heights are materialized only after fin-intent resolution.
FinCandidate make_painted_fin_candidate(const PaintedFinTarget &target, FinCandidate candidate,
                                        const Vec3d &candidate_macro_normal, double expansion_factor,
                                        size_t island_id)
{
    candidate.expansion_slope = target.line ? painted_fin_expansion_slope(
        candidate_macro_normal, candidate.normal, expansion_factor) : expansion_factor;
    candidate.surface_normals = FinSurfaceNormalPolicy::painted_region;
    candidate.geometry_mode = FinGeometryMode::painted_shape;
    candidate.requires_painted_contact = false;
    candidate.macro_normal = candidate_macro_normal;
    candidate.wall_follow_island = island_id;
    candidate.intent_island = island_id;
    candidate.intent_edge = target.edge;
    candidate.intent_line = target.line;
    if (target.line)
        candidate.source_attachment_junction = target.source_attachment_junction;
    return candidate;
}

// Prepare shared nodes and line references without rotating or classifying lines.
void prepare_painted_fin_sources(PaintedFinIsland &work)
{
    const PaintedIsland &island = *work.source;
    auto &sources = work.sources;
    for (const auto &node : island.intent.nodes)
        work.nodes.push_back({work.nodes.size(), &node, node.point});
    sources.reserve(island.intent.strokes.size() + island.intent.points.size());
    auto point_target = [&](const PaintedIntentStroke &stroke, size_t edge_id) {
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
        PaintedFinTarget target{stroke.center, macro_direction};
        target.edge = edge_id;
        target.macro_normal = stroke.macro_normal;
        return target;
    };
    auto line_target = [&](const PaintedIntentStroke &stroke, size_t edge_id) -> std::optional<PaintedFinTarget> {
        if (stroke.points.empty() || stroke.nodes.size() < 2 ||
            (stroke.xy[1] - stroke.xy[0]).norm() <= EPSILON)
            return std::nullopt;
        Vec2d macro_direction = stroke.macro_normal.head<2>();
        if (macro_direction.norm() >= min_fin_macro_horizontal)
            macro_direction.normalize();
        else
            macro_direction = Vec2d::Zero();
        PaintedFinTarget target{stroke.anchor, macro_direction};
        target.edge = edge_id;
        target.line = true;
        target.macro_normal = stroke.macro_normal;
        for (size_t side = 0; side < stroke.nodes.size(); ++side) {
            const size_t node_id = stroke.nodes[side];
            if (node_id >= island.intent.nodes.size())
                continue;
            target.nodes.push_back(&work.nodes[node_id]);
        }
        target.direction = Vec2d(stroke.xy[1] - stroke.xy[0]).normalized();
        target.starts_at_root = stroke.starts_at_root;
        return target;
    };
    // Prepare both alternatives without deciding whether the straight line
    // is acceptable. Source geometry and heights are copied unchanged.
    for (size_t edge_id = 0; edge_id < island.intent.strokes.size(); ++edge_id) {
        const PaintedIntentStroke &stroke = island.intent.strokes[edge_id];
        PaintedFinSource source{point_target(stroke, edge_id), line_target(stroke, edge_id)};
        if (stroke.attachment_host_edge && *stroke.attachment_host_edge < island.intent.strokes.size()) {
            const PaintedIntentStroke &parent = island.intent.strokes[*stroke.attachment_host_edge];
            for (size_t node : stroke.nodes) {
                if (node >= island.intent.nodes.size() ||
                    std::find(parent.nodes.begin(), parent.nodes.end(), node) == parent.nodes.end())
                    continue;
                if (source.line)
                    source.line->source_attachment_junction = std::make_pair(*stroke.attachment_host_edge, node);
                break;
            }
        }
        sources.push_back(std::move(source));
    }
    // Keep stroke indices stable for node/host references. Standalone points
    // follow the lines and retain the blue producer's own location and normal.
    for (const PaintedIntentPoint &blue_point : island.intent.points) {
        Vec2d macro_direction = blue_point.macro_normal.head<2>();
        if (macro_direction.norm() >= min_fin_macro_horizontal)
            macro_direction.normalize();
        else
            macro_direction = Vec2d::Zero();
        PaintedFinTarget point{blue_point.surface.point, macro_direction};
        point.macro_normal = blue_point.macro_normal;
        sources.push_back({std::move(point), std::nullopt});
    }
    // Resolve geometric-host relationships only after the source vector is
    // complete. It is not resized or erased while this working graph is alive.
    for (auto &node : work.nodes) {
        const auto host = node.source->attachment_host_edge;
        if (host && *host < sources.size() && sources[*host].line)
            node.attachment_host = &*sources[*host].line;
    }
}

struct PaintedFinLineCorrection {
    size_t line_planes = 0;
    size_t macro_planes = 0;
    size_t tangent_planes = 0;
    size_t ambiguous_planes = 0;
    size_t missing_macro = 0;
};

// Plan every rotation and attachment against frozen segments. These temporary
// positions are proposals, not independent copies of the shared node state.
PaintedFinLineCorrection correct_painted_fin_lines(PaintedFinIsland &work)
{
    struct Placement {
        PaintedFinNode *origin;
        Vec2d point;
        bool keep = true;
        bool detached = false;
    };
    struct Correction {
        PaintedFinTarget *line = nullptr;
        Vec2d start = Vec2d::Zero(), end = Vec2d::Zero();
        bool changed = false;
        std::vector<Placement> nodes;
    };
    PaintedFinLineCorrection result;
    std::vector<Correction> corrections(work.sources.size());
    std::map<PaintedFinTarget *, Correction *> by_line;
    for (size_t edge = 0; edge < work.sources.size(); ++edge) {
        if (!work.sources[edge].line)
            continue;
        PaintedFinTarget &line = *work.sources[edge].line;
        const PaintedIntentStroke &stroke = work.source->intent.strokes[edge];
        if (line.macro_direction.squaredNorm() <= EPSILON * EPSILON) {
            ++result.missing_macro;
            line.usable = false;
            continue;
        }
        const Vec2d chord = line.nodes[1]->point - line.nodes[0]->point;
        Vec2d direction = chord;
        if (direction.dot(line.macro_direction) < 0.)
            direction = -direction;
        const double length = direction.norm();
        const double deviation = length <= EPSILON ? 0. :
            (direction - length * line.macro_direction).norm();
        Vec2d tangent(-line.macro_direction.y(), line.macro_direction.x());
        if (direction.dot(tangent) < 0.)
            tangent = -tangent;
        const double tangent_deviation = (direction - length * tangent).norm();
        // Both rotations retain the first endpoint and projected length.
        // An equal displacement cannot select either axis unambiguously.
        if (std::abs(deviation - tangent_deviation) <= EPSILON) {
            ++result.ambiguous_planes;
            line.usable = false;
            continue;
        }
        if (std::min(deviation, tangent_deviation) <= stroke.width + EPSILON) {
            if (deviation < tangent_deviation) {
                line.direction = line.macro_direction;
                ++result.macro_planes;
            } else {
                line.direction = tangent;
                ++result.tangent_planes;
            }
        } else {
            line.direction = direction / length;
            ++result.line_planes;
        }
        Correction &correction = corrections[edge];
        correction.line = &line;
        correction.start = line.nodes[0]->point;
        const double orientation = chord.dot(line.direction) < 0. ? -1. : 1.;
        correction.end = correction.start + orientation * chord.norm() * line.direction;
        correction.changed = (correction.end - line.nodes[1]->point).norm() > EPSILON;
        for (size_t slot = 0; slot < line.nodes.size(); ++slot)
            correction.nodes.push_back({line.nodes[slot], slot == 0 ? correction.start :
                (slot == 1 ? correction.end : line.nodes[slot]->point)});
        by_line.emplace(&line, &correction);
    }
    for (Correction &dependent : corrections) {
        if (!dependent.line)
            continue;
        // The original references remain unchanged throughout planning. Host
        // placements appended below must not become new attachment requests.
        for (size_t slot = 0; slot < dependent.line->nodes.size(); ++slot) {
            PaintedFinNode *node = dependent.line->nodes[slot];
            const auto entry = by_line.find(node->attachment_host);
            if (entry == by_line.end() || entry->second == &dependent)
                continue;
            Correction &host = *entry->second;
            if (!dependent.changed && !host.changed)
                continue;
            const bool tail = slot == 1;
            const Vec2d ray_origin = tail ? dependent.start : dependent.end;
            Vec2d ray = tail ? Vec2d(dependent.end - dependent.start) :
                              Vec2d(dependent.start - dependent.end);
            if (ray.norm() <= EPSILON)
                ray = dependent.line->direction;
            else
                ray.normalize();
            std::optional<Vec2d> joint;
            if (dependent.line->starts_at_root &&
                (dependent.start - node->point).norm() <= EPSILON &&
                (host.start - node->point).norm() <= EPSILON)
                joint = node->point;
            else
                joint = fin_plane_contour_intersection(
                    dependent.start, dependent.line->direction, host.start, host.end);

            // An interior junction follows its surviving attachments; it must
            // not also leave a stale copy at its pre-correction position.
            for (size_t i = 2; i < host.line->nodes.size(); ++i)
                if (host.nodes[i].origin == node)
                    host.nodes[i].keep = false;
            Placement &placement = dependent.nodes[slot];
            if (!joint || (*joint - ray_origin).dot(ray) < -EPSILON) {
                placement.keep = slot < 2;
                placement.detached = true;
                continue;
            }
            placement.point = *joint;
            for (const Placement &existing : host.nodes) {
                if (!existing.keep || (existing.point - *joint).norm() > EPSILON)
                    continue;
                // Reuse the whole host entity, including its source identity.
                // Never retain its ID while overwriting a private copy of Z.
                placement.origin = existing.origin;
                break;
            }
            // Keep this accepted attachment even if a later request retires
            // the host's original interior placement. Commit deduplicates refs.
            host.nodes.push_back({placement.origin, *joint});
        }
    }

    // Commit only after all intersections are known. Placements originating at
    // one shared node still reference one entity unless correction splits them.
    std::map<PaintedFinNode *, std::vector<PaintedFinNode *>> resolved;
    for (Correction &correction : corrections) {
        if (!correction.line)
            continue;
        auto &refs = correction.line->nodes;
        refs.clear();
        const Vec2d start = correction.nodes[0].point;
        const Vec2d chord = correction.nodes[1].point - start;
        const double length = chord.norm();
        for (size_t slot = 0; slot < correction.nodes.size(); ++slot) {
            const Placement &placement = correction.nodes[slot];
            if (!placement.keep)
                continue;
            if (slot >= 2) {
                const Vec2d delta = placement.point - start;
                if (length <= EPSILON || std::abs(cross2(delta, chord)) > EPSILON * length ||
                    delta.dot(chord) < -EPSILON * length ||
                    delta.dot(chord) > chord.squaredNorm() + EPSILON * length)
                    continue;
            }
            auto &pieces = resolved[placement.origin];
            PaintedFinNode *node = nullptr;
            if (!placement.detached) {
                for (PaintedFinNode *piece : pieces)
                    if ((piece->point - placement.point).norm() <= EPSILON) {
                        node = piece;
                        break;
                    }
            }
            if (node == nullptr) {
                if (pieces.empty() && !placement.detached) {
                    node = placement.origin;
                    node->point = placement.point;
                } else {
                    work.nodes.push_back({work.nodes.size(), placement.origin->source,
                        placement.point, placement.detached ? nullptr : placement.origin->attachment_host});
                    node = &work.nodes.back();
                }
                if (!placement.detached)
                    pieces.push_back(node);
            }
            if (std::find(refs.begin(), refs.end(), node) == refs.end())
                refs.push_back(node);
        }
    }
    return result;
}

PaintedFinWork select_painted_shape_fin_candidates(const PrintObject &object,
                                                  const PaintedSides &painted_sides,
                                                  const FinShapeAnalysis &shape)
{
    const PrintObjectConfig &config = object.config();
    const double width = support_material_flow(&object).width();
    const double minimum_painted_line_alignment =
        std::cos(config.fin_support_painted_shape_line_angle.value * M_PI / 180.);
    const double local_selection_radius = std::max(
        3 * width, fin_support_dimensions(object).contact_width + width);
    auto better_site = [local_selection_radius, width](const PaintedFinSite &candidate, const PaintedFinSite &current) {
        if (!current.found)
            return true;
        if (candidate.macro_side != current.macro_side)
            return candidate.macro_side;
        const size_t candidate_height_band = size_t(candidate.height_error / width);
        const size_t current_height_band = size_t(current.height_error / width);
        if (candidate_height_band != current_height_band)
            return candidate_height_band < current_height_band;
        const size_t candidate_distance_band = size_t(
            std::sqrt(candidate.planar_distance_squared) / local_selection_radius);
        const size_t current_distance_band = size_t(
            std::sqrt(current.planar_distance_squared) / local_selection_radius);
        if (candidate_distance_band != current_distance_band)
            return candidate_distance_band < current_distance_band;
        if (std::abs(candidate.demand - current.demand) > 1e-9)
            return candidate.demand > current.demand;
        return candidate.distance_squared < current.distance_squared;
    };
    auto resolve_direction = [](const Vec2d &macro_direction, const FinShapeLayer *shape) {
        if (macro_direction.squaredNorm() <= EPSILON * EPSILON)
            return PointFinDirection{Vec2d::Zero(), 0.};
        return painted_shape_fin_direction(macro_direction, shape);
    };

    struct ResolvedPointTarget {
        PaintedFinSite site;
        FinSupportDimensions dimensions;
        Vec3d macro_normal;
    };
    PaintedFinWork work;
    auto &proposals = work.candidates;
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
    size_t tangent_plane_targets = 0;
    size_t ambiguous_plane_targets = 0;
    size_t vertical_point_targets = 0;
    size_t line_to_point_fallbacks = 0;
    size_t line_plane_alignment_rejects = 0;
    size_t line_plane_missing_macro = 0;
    size_t finite_line_candidates = 0;
    size_t resolved_line_candidates = 0;
    size_t resolved_point_candidates = 0;
    double max_center_distance = 0.;
    size_t next_crease_group = painted_sides.islands.size();
    proposals.reserve(painted_sides.islands.size());
    for (size_t island_id = 0; island_id < painted_sides.islands.size(); ++island_id) {
        if (object.print()->canceled())
            throw CanceledException();
        const PaintedIsland &island = painted_sides.islands[island_id];
        point_intents += island.intent.points.size();
        line_intents += island.intent.strokes.size() == 1;
        branched_intents += island.intent.strokes.size() > 1;
        intent_strokes += island.intent.strokes.size();
        work.islands.push_back(std::make_unique<PaintedFinIsland>());
        PaintedFinIsland &graph = *work.islands.back();
        graph.source = &island;
        prepare_painted_fin_sources(graph);
        const PaintedFinLineCorrection correction = correct_painted_fin_lines(graph);
        line_plane_targets += correction.line_planes;
        macro_plane_targets += correction.macro_planes;
        tangent_plane_targets += correction.tangent_planes;
        ambiguous_plane_targets += correction.ambiguous_planes;
        line_plane_missing_macro += correction.missing_macro;

        std::vector<std::vector<PaintedSegment>> contour_runs;
        auto resolve_point_target = [&](const PaintedFinTarget &target) -> std::optional<ResolvedPointTarget> {
            if (contour_runs.empty()) {
                contour_runs.resize(painted_sides.layers.size());
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
            }
            const Vec2d center = target.point.head<2>();
            const bool at_contact_top = target.edge.has_value();
            PaintedFinSite center_site;
            for (size_t layer_id = 1; layer_id < contour_runs.size(); ++layer_id) {
                const Layer &model_layer = *object.get_layer(int(layer_id));
                const double dz = model_layer.slice_z - target.point.z();
                if (at_contact_top && dz > EPSILON)
                    continue;
                // Compare macro faces near this skeleton feature before
                // committing the intent to one actual sliced contour point.
                for (const PaintedSegment &run : contour_runs[layer_id]) {
                    const Vec2d edge = run.b - run.a;
                    if (edge.squaredNorm() <= EPSILON * EPSILON)
                        continue;
                    const double position = std::clamp((center - run.a).dot(edge) / edge.squaredNorm(), 0., 1.);
                    const Vec2d point = run.a + position * edge;
                    PaintedFinSite site;
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
                    const auto resolved = resolve_direction(target.macro_direction, shape_layer);
                    site.direction = resolved.direction;
                    site.demand = resolved.demand;
                    site.macro_side = macro_horizontal >= min_fin_macro_horizontal &&
                        macro_xy.dot(run.normal) >= min_fin_contact_alignment &&
                        fin_contact_aligned(resolved.direction, macro_xy);
                    if (better_site(site, center_site))
                        center_site = std::move(site);
                }
            }
            if (!center_site.found) {
                ++without_contour;
                return std::nullopt;
            }

            // Converted green lines already target their highest source Z.
            // Only standalone center points need the half-contact-height shift.
            const double target_z = target.point.z() +
                (at_contact_top ? 0. : 0.5 * config.fin_support_contact_height.value);
            size_t target_layer = center_site.layer;
            double target_error = std::abs(object.get_layer(int(target_layer))->slice_z - target_z);
            for (size_t layer_id = center_site.layer + 1; layer_id < painted_sides.layers.size(); ++layer_id) {
                if (at_contact_top && object.get_layer(int(layer_id))->slice_z > target_z + EPSILON)
                    break;
                const double error = std::abs(object.get_layer(int(layer_id))->slice_z - target_z);
                if (error > target_error)
                    break;
                target_error = error;
                target_layer = layer_id;
            }

            PaintedFinSite best = center_site;
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
                    const double position =
                        std::clamp((predicted - run.a).dot(edge) / edge.squaredNorm(), 0., 1.);
                    const Vec2d point = run.a + position * edge;
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
            const auto resolved = resolve_direction(target.macro_direction, shape_layer);
            if (!fin_contact_aligned(resolved.direction, macro_contact_normal)) {
                ++without_contour;
                return std::nullopt;
            }
            best.direction = resolved.direction;
            best.demand = resolved.demand;

            const FinSupportDimensions dimensions = fin_support_dimensions(object);
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
            return ResolvedPointTarget{std::move(best), dimensions, candidate_macro_normal};
        };

        for (PaintedFinSource &source : graph.sources) {
            const PaintedFinTarget *target = &source.point;
            const double normal_length = source.point.macro_normal.norm();
            const bool near_vertical = normal_length > EPSILON &&
                std::abs(source.point.macro_normal.z()) <= max_fin_vertical_point_normal_z * normal_length;
            vertical_point_targets += near_vertical && source.point.edge.has_value();
            if (!near_vertical && source.line && source.line->usable &&
                source.line->nodes.size() >= 2 && source.line->edge) {
                // Classify in the source patch frame once, after correction and
                // junction resolution. Accepted lines never enter point-site selection.
                if (fin_contact_aligned(source.line->direction, source.line->macro_direction,
                                        minimum_painted_line_alignment))
                    target = &*source.line;
                else
                    ++line_plane_alignment_rejects;
            }
            if (target->line) {
                // Finite paths obtain their positions and activation heights from
                // shared nodes during path resolution, not from a painted contour site.
                FinCandidate candidate = make_painted_fin_candidate(*target,
                    {target->nodes.front()->point, target->direction, target->macro_direction, 0,
                     fin_support_dimensions(object), 0.},
                    target->macro_normal.normalized(), config.fin_support_expansion_factor.value, island_id);
                // This producer emits straight lines. Convert its endpoint-first
                // references to traversal order before the general path boundary.
                auto refs = target->nodes;
                const Vec2d origin = refs[0]->point;
                const Vec2d chord = refs[1]->point - origin;
                std::stable_sort(refs.begin(), refs.end(), [&](const auto *a, const auto *b) {
                    return (a->point - origin).dot(chord) < (b->point - origin).dot(chord);
                });
                proposals.push_back({std::move(candidate), std::move(refs)});
                ++finite_line_candidates;
                ++intent_direction_used;
                ++resolved_line_candidates;
            } else {
                PaintedFinTarget point_target = *target;
                if (point_target.edge) {
                    // Use final green XY with its immutable source height; this
                    // point follows the contour-site search, not model-height resolution.
                    const PaintedFinNode *top = nullptr;
                    auto consider = [&](const PaintedFinNode &node) {
                        if (top == nullptr || node.source->surface_z > top->source->surface_z)
                            top = &node;
                    };
                    if (source.line) {
                        for (const PaintedFinNode *node : source.line->nodes)
                            consider(*node);
                    } else {
                        // XY-degenerate strokes still retain their green nodes.
                        for (const size_t node : island.intent.strokes[*point_target.edge].nodes)
                            consider(graph.nodes[node]);
                    }
                    if (top != nullptr)
                        point_target.point = Vec3d(top->point.x(), top->point.y(), top->source->surface_z);
                }
                const auto resolved = resolve_point_target(point_target);
                if (!resolved)
                    continue;
                const PaintedFinSite &site = resolved->site;
                FinCandidate candidate = make_painted_fin_candidate(point_target,
                    {site.point, site.direction, site.run.normal, site.layer, resolved->dimensions, 0.},
                    resolved->macro_normal, config.fin_support_expansion_factor.value, island_id);
                direction_adjusted += site.direction.dot(site.run.normal) < 1 - 1e-9;
                max_center_distance = std::max(max_center_distance, std::sqrt(site.distance_squared));
                proposals.push_back({std::move(candidate)});
                ++resolved_point_candidates;
                line_to_point_fallbacks += source.line.has_value() || source.point.edge.has_value();
            }
        }

        // Direct crease geometry bypasses point-site selection and ordinary
        // normal/angle correction. Ordinary contact construction still validates
        // its actual layer contacts during realization.
        for (size_t i = 0; i < island.intent.creases.size(); ++i) {
            const auto &intent = *island.intent.creases[i];
            const bool line = intent.points.size() > 1;
            Vec2d outward = Vec2d::Zero();
            Vec3d macro_normal = Vec3d::Zero();
            // Lines obtain all source normals together from the complete model
            // during path preparation. The old two-face rule belongs to points.
            if (!line) {
                bool available = true;
                for (const Vec3d &normal : intent.root_normals) {
                    const Vec2d xy = normal.head<2>();
                    if (xy.norm() <= EPSILON) {
                        available = false;
                        break;
                    }
                    outward += xy.normalized();
                }
                macro_normal = intent.root_normals[0] + intent.root_normals[1];
                if (!available || outward.norm() <= EPSILON || macro_normal.head<2>().norm() <= EPSILON) {
                    BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name
                        << " stage=painted-crease-direct reject=no-xy-exterior-bisector crease=" << intent.crease
                        << " pair=" << intent.pair << " part=" << intent.part;
                    continue;
                }
                outward.normalize();
                macro_normal.normalize();
            }
            const Vec3d &start = intent.points.front();
            size_t layer_id = 0;
            while (layer_id + 1 < object.layer_count() &&
                   object.get_layer(int(layer_id + 1))->slice_z <= start.z() + EPSILON)
                ++layer_id;
            const auto dimensions = fin_support_dimensions(object);
            FinCandidate candidate{start.head<2>(), outward, outward, layer_id, dimensions,
                line ? 0. : painted_fin_expansion_slope(macro_normal, outward, config.fin_support_expansion_factor.value)};
            candidate.surface_normals = FinSurfaceNormalPolicy::painted_region;
            candidate.geometry_mode = FinGeometryMode::painted_shape;
            candidate.requires_painted_contact = false;
            candidate.macro_normal = macro_normal;
            candidate.wall_follow_island = island_id;
            candidate.intent_island = next_crease_group++;
            candidate.intent_edge = island.intent.strokes.size() + i;
            candidate.intent_line = line;
            candidate.crease_intent = island.intent.creases[i];
            std::vector<PaintedFinNode *> refs;
            if (candidate.intent_line) {
                candidate.path = FinCandidate::Path{intent.points, false, false, *candidate.intent_edge};
                if (!valid_painted_fin_path(object, candidate))
                    continue;
                for (const size_t node_id : intent.nodes) {
                    auto &node = graph.nodes[node_id];
                    node.z = node.source->surface_z;
                    refs.push_back(&node);
                }
            }
            BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                << " stage=painted-crease-direct crease=" << intent.crease
                << " pair=" << intent.pair << " part=" << intent.part
                << " intent_island=" << *candidate.intent_island << " edge=" << *candidate.intent_edge
                << " kind=" << (candidate.intent_line ? "line" : "point")
                << " vertices=" << intent.points.size() << " root_z=" << start.z()
                << " direction_resolved=" << !line
                << " outward_x=" << outward.x() << " outward_y=" << outward.y();
            resolved_line_candidates += candidate.intent_line;
            resolved_point_candidates += !candidate.intent_line;
            proposals.push_back({std::move(candidate), std::move(refs)});
        }
    }
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=painted-shape-candidates islands=" << painted_sides.islands.size()
        << " undersized_painted_islands=" << painted_sides.undersized_painted_islands
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
        << " compact_intent_components=" << painted_sides.compact_intent_components
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
        << " tangent_plane_targets=" << tangent_plane_targets
        << " ambiguous_plane_targets=" << ambiguous_plane_targets
        << " vertical_point_targets=" << vertical_point_targets
        << " line_to_point_fallbacks=" << line_to_point_fallbacks
        << " line_plane_alignment_rejects=" << line_plane_alignment_rejects
        << " line_plane_missing_macro=" << line_plane_missing_macro
        << " finite_line_candidates=" << finite_line_candidates
        << " resolved_line_candidates=" << resolved_line_candidates
        << " resolved_point_candidates=" << resolved_point_candidates
        << " candidates=" << proposals.size()
        << " without_contour=" << without_contour
        << " contact_top_clamped=" << contact_top_clamped
        << " geometry_fallback=" << geometry_fallback
        << " shifted_to_contact_top=" << shifted_to_contact_top
        << " intent_direction_used=" << intent_direction_used
        << " direction_adjusted=" << direction_adjusted
        << " max_center_distance_mm=" << max_center_distance;
    return work;
}

// Own both the transformed model and the acceleration structure: neither a
// painted chart nor the layer silhouettes are a complete model-height query.
class PaintedFinModel {
    TriangleMesh m_mesh;
    std::unique_ptr<AABBMesh> m_tree;
    std::map<float, Polygons> m_horizontal_sections;
public:
    struct NormalDiagnostic {
        const char *location = "unavailable";
        std::string details;
    };

    explicit PaintedFinModel(const PrintObject &object, bool prepare_normals)
    {
        std::vector<csg::CSGPart> parts;
        csg::model_to_csgmesh(*object.model_object(), object.trafo_centered(),
            std::back_inserter(parts), csg::mpartsPositive | csg::mpartsNegative);
        bool boolean_failed = false;
        try {
            auto solid = csg::perform_csgmesh_booleans(range(parts));
            if (solid)
                m_mesh = MeshBoolean::cgal::cgal_to_triangle_mesh(*solid);
            else
                boolean_failed = true;
        } catch (const std::exception &error) {
            boolean_failed = true;
            BOOST_LOG_TRIVIAL(warning) << "Fin support object=" << object.model_object()->name
                << " stage=painted-path-model boolean-error=" << error.what();
        }
        if (boolean_failed) {
            // Failed booleans use unbooleaned positive volumes, matching the
            // common model-surface query policy.
            BOOST_LOG_TRIVIAL(warning) << "Fin support object=" << object.model_object()->name
                << " stage=painted-path-model boolean-fallback=positive-volumes";
            for (const ModelVolume *volume : object.model_object()->volumes)
                if (volume->is_model_part()) {
                    TriangleMesh mesh = volume->mesh();
                    mesh.transform(object.trafo_centered() * volume->get_matrix(), true);
                    m_mesh.merge(mesh);
                }
        }
        if (!m_mesh.empty()) {
            // Exact-coordinate welding changes no surface geometry, but makes
            // all incident faces visible to the normal query's adjacency index,
            // including seams between positive meshes in the Boolean fallback.
            if (prepare_normals)
                its_merge_vertices(m_mesh.its);
            m_tree = std::make_unique<AABBMesh>(m_mesh);
        }
    }

    std::optional<double> height(const Vec2d &xy, double source_z, double upper) const
    {
        if (!m_tree)
            return std::nullopt;
        const double reference = std::min(source_z, upper);
        // A vertical face contains an interval of Z; a parallel ray would
        // otherwise see only its rims. The bounded reference is the nearest
        // admissible height when that point already lies on the surface.
        if (m_tree->squared_distance(Vec3d(xy.x(), xy.y(), reference)) <= EPSILON * EPSILON)
            return reference;
        const Vec3d origin(xy.x(), xy.y(), upper);
        std::optional<double> nearest;
        double distance = std::numeric_limits<double>::infinity();
        for (const auto &hit : m_tree->query_ray_hits(
                origin + EPSILON * Vec3d::UnitZ(), -Vec3d::UnitZ())) {
            if (!hit.is_hit() || hit.position().z() > upper + EPSILON)
                continue;
            const double z = std::min(upper, hit.position().z());
            const double delta = std::abs(z - source_z);
            if (!nearest || delta < distance || (delta == distance && z < *nearest)) {
                nearest = z;
                distance = delta;
            }
        }
        return nearest;
    }

    std::optional<Vec3d> normal(const Vec3d &point,
                               NormalDiagnostic *diagnostic = nullptr) const
    {
        if (!m_tree)
            return std::nullopt;
        int face = -1;
        Vec3d nearest;
        const double squared_distance = m_tree->squared_distance(point, face, nearest);
        std::ostringstream detail;
        detail.precision(17);
        if (diagnostic) {
            detail << " query_mesh=complete-model query_face=" << face
                   << " query_distance_squared=" << squared_distance;
            if (face >= 0)
                detail << " nearest_x=" << nearest.x() << " nearest_y=" << nearest.y()
                       << " nearest_z=" << nearest.z();
            diagnostic->details = detail.str();
        }
        if (squared_distance > EPSILON * EPSILON || face < 0)
            return std::nullopt;
        auto record_face = [&](size_t incident, double weight) {
            if (!diagnostic)
                return;
            const Vec3d normal = m_tree->normal_by_face_id(int(incident));
            detail << " contributor=" << incident << ":" << weight << ":"
                   << normal.x() << ":" << normal.y() << ":" << normal.z();
        };
        const auto &mesh = m_mesh.its;
        const Vec3i32 &triangle = mesh.indices[size_t(face)];
        const auto &incidence = m_tree->vertex_face_index();
        Vec3d sum = Vec3d::Zero();
        bool boundary = false;
        // At a vertex, all incident faces contribute their corner angle.
        for (int corner = 0; corner < 3 && !boundary; ++corner) {
            const int vertex = triangle[corner];
            const Vec3d origin = mesh.vertices[size_t(vertex)].cast<double>();
            if ((point - origin).norm() > EPSILON)
                continue;
            boundary = true;
            if (diagnostic) {
                diagnostic->location = "vertex";
                detail << " model_vertex=" << vertex << " vertex_x=" << origin.x()
                       << " vertex_y=" << origin.y() << " vertex_z=" << origin.z();
            }
            for (const size_t incident : incidence[size_t(vertex)]) {
                const Vec3i32 &indices = mesh.indices[incident];
                for (int k = 0; k < 3; ++k) {
                    if (indices[k] != vertex)
                        continue;
                    const double angle = NormalUtils::indice_angle(k, indices, mesh.vertices);
                    sum += angle * m_tree->normal_by_face_id(int(incident));
                    record_face(incident, angle);
                }
            }
        }
        // In an edge interior, use the unit normals of every incident face.
        for (int edge = 0; edge < 3 && !boundary; ++edge) {
            const int first = triangle[edge], second = triangle[(edge + 1) % 3];
            const Vec3d a = mesh.vertices[size_t(first)].cast<double>();
            const Vec3d direction = mesh.vertices[size_t(second)].cast<double>() - a;
            if (direction.squaredNorm() <= EPSILON * EPSILON)
                continue;
            const double t = (point - a).dot(direction) / direction.squaredNorm();
            if (t < 0. || t > 1. || (point - a - t * direction).norm() > EPSILON)
                continue;
            boundary = true;
            if (diagnostic) {
                diagnostic->location = "edge";
                detail << " model_edge_first=" << first << " model_edge_second=" << second
                       << " edge_fraction=" << t;
            }
            for (const size_t incident : incidence[size_t(first)]) {
                const Vec3i32 &indices = mesh.indices[incident];
                if (indices[0] == second || indices[1] == second || indices[2] == second) {
                    sum += m_tree->normal_by_face_id(int(incident));
                    record_face(incident, 1.);
                }
            }
        }
        if (!boundary) {
            sum = m_tree->normal_by_face_id(face);
            if (diagnostic)
                diagnostic->location = "face";
            record_face(size_t(face), 1.);
        }
        if (diagnostic) {
            detail << " sum_x=" << sum.x() << " sum_y=" << sum.y() << " sum_z=" << sum.z();
            diagnostic->details = detail.str();
        }
        if (!sum.allFinite() || sum.norm() <= EPSILON)
            return std::nullopt;
        return Vec3d(sum.normalized());
    }

    void prepare_horizontal_sections(std::vector<float> heights, const PrintObject &object)
    {
        if (m_mesh.empty() || heights.empty())
            return;
        std::sort(heights.begin(), heights.end());
        heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
        // Reuse the slicer's horizontal-edge ownership and contour orientation.
        // Source Z is converted only to the slicer's float precision, never to
        // a print layer. No additional contour offset, closing or simplification.
        auto sections = slice_mesh(m_mesh.its, heights, MeshSlicingParams{}, [&object] {
            if (object.print()->canceled())
                throw CanceledException();
        });
        for (size_t i = 0; i < heights.size(); ++i)
            m_horizontal_sections.emplace(heights[i], std::move(sections[i]));
    }

    std::optional<Vec2d> horizontal_bisector(const Vec3d &point, std::string *diagnostic) const
    {
        const auto section = m_horizontal_sections.find(float(point.z()));
        ConstPolygonPtrs contours;
        if (section != m_horizontal_sections.end()) {
            contours.reserve(section->second.size());
            for (const Polygon &contour : section->second)
                contours.push_back(&contour);
        }
        std::string section_detail;
        const auto result = section == m_horizontal_sections.end() ? std::optional<Vec2d>{} :
            fin_section_bisector(contours, point.head<2>(), diagnostic ? &section_detail : nullptr);
        if (diagnostic) {
            std::ostringstream detail;
            detail.precision(17);
            detail << " section_z=" << float(point.z())
                   << (section == m_horizontal_sections.end() ? " section_status=unavailable" : section_detail);
            *diagnostic = detail.str();
        }
        return result;
    }
};

Vec2d fin_root_extension_direction(const std::vector<Vec2d> &directions)
{
    std::vector<std::pair<double, size_t>> angles;
    for (size_t i = 0; i < directions.size(); ++i) {
        double angle = std::atan2(directions[i].y(), directions[i].x());
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
    return Vec2d(std::cos(middle), std::sin(middle));
}

// Keep shared working entities through height resolution. Only the completed
// intent is exported as the value snapshots consumed by layer geometry.
std::vector<FinCandidateProposal> resolve_painted_fin_paths(
    const PrintObject &object, PaintedFinWork work)
{
    if (std::none_of(work.candidates.begin(), work.candidates.end(),
            [](const PreparedPaintedFin &candidate) { return !candidate.nodes.empty(); })) {
        std::vector<FinCandidateProposal> result;
        for (const auto &candidate : work.candidates)
            result.push_back({candidate.attributes});
        return result;
    }
    PaintedFinModel model(object, std::any_of(work.candidates.begin(), work.candidates.end(),
        [](const PreparedPaintedFin &candidate) {
            return candidate.attributes.crease_intent && candidate.attributes.path;
        }));
    std::vector<float> crease_heights;
    for (const auto &candidate : work.candidates)
        if (candidate.attributes.crease_intent && candidate.attributes.path)
            crease_heights.push_back(float(candidate.attributes.crease_intent->points.front().z()));
    model.prepare_horizontal_sections(std::move(crease_heights), object);
    const double gap = object.config().support_object_xy_distance.value;
    // Crease geometry enters unchanged. Only its root needs a horizontal
    // direction for the outward stem and a 3D normal for expansion slope.
    work.candidates.erase(std::remove_if(work.candidates.begin(), work.candidates.end(),
        [&](PreparedPaintedFin &candidate) {
            auto &seed = candidate.attributes;
            if (!seed.crease_intent || !seed.path)
                return false;
            const auto &intent = *seed.crease_intent;
            assert(candidate.nodes.size() == intent.points.size());
            if (object.print()->canceled())
                throw CanceledException();
            const Vec3d &source = intent.points.front();
            PaintedFinModel::NormalDiagnostic normal_detail;
            const auto normal = model.normal(source, get_logging_level() >= 3 ? &normal_detail : nullptr);
            std::string section_detail;
            const auto direction = model.horizontal_bisector(source, &section_detail);
            const bool valid = normal && normal->head<2>().norm() > EPSILON && direction;
            if (get_logging_level() >= 3 || !valid) {
                std::ostringstream detail;
                detail.precision(17);
                detail << "stage=painted-crease-root crease=" << intent.crease
                       << " pair=" << intent.pair << " part=" << intent.part
                       << " source_x=" << source.x() << " source_y=" << source.y()
                       << " source_z=" << source.z() << " valid=" << valid
                       << " query_location=" << normal_detail.location << normal_detail.details
                       << section_detail;
                if (normal)
                    detail << " normal_x=" << normal->x() << " normal_y=" << normal->y()
                           << " normal_z=" << normal->z();
                if (direction)
                    detail << " root_direction_x=" << direction->x() << " root_direction_y=" << direction->y();
                BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name << " " << detail.str();
            }
            if (!valid)
                return true;
            for (size_t i = 0; i < intent.points.size(); ++i) {
                candidate.nodes[i]->point = intent.points[i].head<2>();
                candidate.nodes[i]->z = intent.points[i].z();
            }
            candidate.nodes.front()->root_direction = *direction;
            seed.normal = seed.contact_normal = *direction;
            seed.macro_normal = *normal;
            seed.expansion_slope = painted_fin_expansion_slope(
                seed.macro_normal, *direction, object.config().fin_support_expansion_factor.value);
            return false;
        }), work.candidates.end());
    std::deque<PaintedFinGroup> groups;
    std::deque<PaintedFinSpan> spans;
    std::vector<PaintedFinNode *> nodes;
    std::map<PaintedFinNode *, PaintedFinIsland *> node_owners;
    for (const auto &island : work.islands)
        for (PaintedFinNode &node : island->nodes)
            node_owners.emplace(&node, island.get());
    auto add_span = [&](PaintedFinGroup &group, PaintedFinNode *start, PaintedFinNode *end) {
        spans.push_back({&group, start, end});
        start->outgoing.push_back(&spans.back());
        end->incoming.push_back(&spans.back());
    };
    size_t next_edge = 0;
    for (const auto &candidate : work.candidates)
        if (candidate.attributes.intent_edge)
            next_edge = std::max(next_edge, *candidate.attributes.intent_edge + 1);
    for (const PreparedPaintedFin &candidate : work.candidates) {
        if (candidate.nodes.empty()) {
            groups.push_back({&candidate.attributes, {}});
            continue;
        }
        for (PaintedFinNode *node : candidate.nodes)
            if (std::find(nodes.begin(), nodes.end(), node) == nodes.end())
                nodes.push_back(node);
        groups.push_back({&candidate.attributes, candidate.nodes});
        PaintedFinGroup &group = groups.back();
        for (size_t i = 1; i < group.nodes.size(); ++i)
            add_span(group, group.nodes[i - 1], group.nodes[i]);
    }
    std::vector<FinCandidateProposal> result;
    if (nodes.empty()) {
        for (const auto &candidate : work.candidates)
            result.push_back({candidate.attributes});
        return result;
    }
    std::deque<PaintedFinRoot> roots;
    for (PaintedFinNode *node : nodes) {
        if (!node->incoming.empty() || node->outgoing.empty())
            continue;
        std::vector<Vec2d> directions;
        for (const PaintedFinSpan *span : node->outgoing) {
            // Consecutive vertices can share XY while carrying distinct Z.
            // Use the first nonzero outgoing segment, not the endpoint chord.
            for (size_t i = 1; i < span->group->nodes.size(); ++i) {
                const Vec2d direction = span->group->nodes[i]->point - node->point;
                if (direction.norm() > EPSILON) {
                    directions.push_back(direction);
                    break;
                }
            }
        }
        roots.push_back({node});
        if (node->root_direction)
            roots.back().extension_direction = *node->root_direction;
        else if (!directions.empty())
            roots.back().extension_direction = fin_root_extension_direction(directions);
        node->root = &roots.back();
    }
    std::deque<FinCandidate> root_candidates;
    for (PaintedFinRoot &root : roots) {
        if (object.print()->canceled())
            throw CanceledException();
        PaintedFinNode &node = *root.node;
        if (!node.z)
            node.z = model.height(node.point, node.source->surface_z, node.source->surface_z);
        if (!node.z || root.extension_direction.squaredNorm() <= EPSILON * EPSILON)
            continue;
        size_t layer_id = 0;
        while (layer_id + 1 < object.layer_count() &&
               object.get_layer(int(layer_id + 1))->slice_z <= *node.z + EPSILON)
            ++layer_id;
        const Layer &layer = *object.get_layer(int(layer_id));
        const double width = support_material_flow(&object, float(layer.height)).width();
        const ExPolygons clearance = offset_ex(layer.lslices,
            float(scale_(gap + 3. * width)), jtRound, DefaultMiterLimit);
        std::optional<ContourRayHit> exit;
        for (const ExPolygon &area : clearance) {
            auto consider = [&](const Polygon &contour, bool hole) {
                const auto hit = ray_contour_hit(contour, node.point,
                    root.extension_direction, std::numeric_limits<double>::infinity(),
                    hole ? Vec2d(-root.extension_direction) : root.extension_direction);
                if (hit && (!exit || hit->distance < exit->distance))
                    exit = hit;
            };
            consider(area.contour, false);
            for (const Polygon &hole : area.holes)
                consider(hole, true);
        }
        if (!exit)
            continue;
        // Keep the source root as a junction. A single outward stem owns the
        // generation root and seed contact; none of the original lines rotates.
        PaintedFinGroup &source_group = *node.outgoing.front()->group;
        const bool crease_path = source_group.attributes->crease_intent != nullptr;
        FinCandidate attributes = *source_group.attributes;
        // Preserve the existing common-extension maximum without summing the
        // outgoing contacts or giving each branch a separate seed band.
        for (const PaintedFinSpan *span : node.outgoing)
            attributes.expansion_slope = std::max(attributes.expansion_slope,
                span->group->attributes->expansion_slope);
        attributes.dimensions = fin_support_dimensions(object);
        PaintedFinIsland &island = *node_owners.at(&node);
        island.nodes.push_back({island.nodes.size(), node.source, exit->point});
        PaintedFinNode &outer = island.nodes.back();
        outer.z = node.z;
        outer.root = &root;
        root.node = &outer;
        node.root = nullptr;
        nodes.push_back(&outer);
        if (crease_path) {
            // An independent crease is one continuous generation group. Keep
            // its original root as an internal junction and prepend the stem;
            // its seed band then reaches the same per-layer A1 as the body.
            assert(node.outgoing.size() == 1 && source_group.nodes.front() == &node);
            root_candidates.push_back(std::move(attributes));
            source_group.attributes = &root_candidates.back();
            source_group.nodes.insert(source_group.nodes.begin(), &outer);
            add_span(source_group, &outer, &node);
        } else {
            attributes.intent_edge = next_edge++;
            attributes.source_attachment_junction.reset();
            root_candidates.push_back(std::move(attributes));
            groups.push_back({&root_candidates.back(), {&outer, &node}});
            add_span(groups.back(), &outer, &node);
        }
        root.valid = true;
    }
    // Roots and direct crease nodes already have heights. Resolve every
    // ordinary downstream node once, after all of its incoming starts finish.
    std::vector<PaintedFinNode *> unresolved;
    for (PaintedFinNode *node : nodes)
        if (node->root == nullptr && !node->z)
            unresolved.push_back(node);
    size_t resolved_heights = 0;
    size_t missing_heights = 0;
    while (!unresolved.empty()) {
        if (object.print()->canceled())
            throw CanceledException();
        bool progress = false;
        for (auto it = unresolved.begin(); it != unresolved.end();) {
            PaintedFinNode *node = *it;
            const bool waiting = std::any_of(node->incoming.begin(), node->incoming.end(),
                [&](const PaintedFinSpan *span) {
                    return std::find(unresolved.begin(), unresolved.end(), span->start) != unresolved.end();
                });
            if (waiting) {
                ++it;
                continue;
            }
            // A shared junction must not rise above any available upstream
            // start. Failed starts cannot supply a bound or a replacement Z.
            std::optional<double> upper;
            for (const PaintedFinSpan *span : node->incoming) {
                const PaintedFinNode &start = *span->start;
                if (!start.z || (start.root && !start.root->valid))
                    continue;
                if (!upper || *start.z < *upper)
                    upper = *start.z;
            }
            if (upper)
                node->z = model.height(node->point, node->source->surface_z, *upper);
            resolved_heights += node->z.has_value();
            missing_heights += !node->z.has_value();
            if (!node->z && get_logging_level() >= 3) {
                std::ostringstream detail;
                detail.precision(17);
                detail << " stage=painted-fin-height node=" << node->id
                    << " xy=" << node->point.transpose() << " source_z=" << node->source->surface_z;
                if (!node->incoming.empty())
                    detail << " island=" << node->incoming.front()->group->attributes->intent_island.value_or(size_t(-1));
                for (const PaintedFinSpan *span : node->incoming)
                    detail << " incoming_node=" << span->start->id;
                if (upper)
                    detail << " upper_z=" << *upper;
                detail << " status=" << (upper ? "no-surface-hit" : "no-upstream-height");
                BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name << detail.str();
            }
            it = unresolved.erase(it);
            progress = true;
        }
        if (!progress) {
            BOOST_LOG_TRIVIAL(error) << "Fin support stage=painted-fin-intents"
                << " reason=height-dependency-cycle nodes=" << unresolved.size();
            // Keep these nodes unresolved. Only groups whose node heights are
            // resolved are exported; unrelated resolved groups remain usable.
            break;
        }
    }
    size_t discarded = 0;
    size_t line_groups = 0;
    for (const PaintedFinGroup &group : groups) {
        if (object.print()->canceled())
            throw CanceledException();
        FinCandidate seed = *group.attributes;
        if (group.nodes.empty()) {
            result.push_back({std::move(seed)});
            continue;
        }
        const PaintedFinNode &start = *group.nodes.front();
        const PaintedFinNode &end = *group.nodes.back();
        if ((start.root && !start.root->valid) ||
            std::any_of(group.nodes.begin(), group.nodes.end(),
                [](const PaintedFinNode *node) { return !node->z; })) {
            ++discarded;
            continue;
        }
        seed.path = FinCandidate::Path{{},
            start.incoming.size() + start.outgoing.size() > 1,
            end.incoming.size() + end.outgoing.size() > 1,
            *seed.intent_edge};
        for (const PaintedFinNode *node : group.nodes) {
            seed.path->points.emplace_back(node->point.x(), node->point.y(), *node->z);
            seed.intent_nodes.push_back({node->id, node->point, std::nullopt,
                                         node->source->surface_z, *node->z});
        }
        if (start.root)
            seed.intent_root = FinCandidate::IntentRoot{
                start.id, start.point, start.root->extension_direction, *start.z};
        // A representative start direction remains for outline orientation;
        // body/contact geometry uses each retained segment's local direction.
        for (size_t i = 1; i < group.nodes.size(); ++i) {
            const Vec2d direction = start.point - group.nodes[i]->point;
            if (direction.norm() > EPSILON) {
                seed.normal = direction.normalized();
                break;
            }
        }
        seed.point = start.point;
        const double top = *start.z;
        seed.top_layer = 0;
        while (seed.top_layer + 1 < object.layer_count() &&
               object.get_layer(int(seed.top_layer + 1))->slice_z <= top + EPSILON)
            ++seed.top_layer;
        result.push_back({std::move(seed)});
        ++line_groups;
    }
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=painted-fin-intents groups=" << line_groups
        << " roots=" << roots.size() << " root_stems=" << std::count_if(roots.begin(), roots.end(),
            [](const PaintedFinRoot &root) { return root.valid; })
        << " topology_spans=" << spans.size() << " shared_nodes=" << nodes.size()
        << " resolved_node_heights=" << resolved_heights << " missing_node_heights=" << missing_heights
        << " unresolved_node_heights=" << unresolved.size()
        << " discarded_groups=" << discarded;
    return result;
}

void export_painted_crease_fin_paths(const PrintObject &object, const PaintedSides &painted,
                                    const std::vector<FinCandidateProposal> &proposals)
{
    if (get_logging_level() < 3)
        return;
    for (const auto &island : painted.islands)
        for (const auto &shared_intent : island.intent.creases) {
            const auto &intent = *shared_intent;
            if (object.print()->canceled())
                throw CanceledException();
            std::vector<const FinCandidate *> resolved;
            Vec3d minimum = intent.points.front(), maximum = minimum;
            auto include = [&](const Vec3d &point) {
                minimum = minimum.cwiseMin(point);
                maximum = maximum.cwiseMax(point);
            };
            for (const Vec3d &point : intent.points)
                include(point);
            for (const auto &proposal : proposals) {
                const FinCandidate &candidate = proposal.primary;
                if (!candidate.crease_intent || candidate.crease_intent->crease != intent.crease ||
                    candidate.crease_intent->pair != intent.pair || candidate.crease_intent->part != intent.part)
                    continue;
                resolved.push_back(&candidate);
                if (candidate.path) {
                    for (const Vec3d &point : candidate.path->points)
                        include(point);
                }
            }
            const double view_size = std::max(60., 4. * (maximum - minimum).maxCoeff() + 8.);
            const std::string filename = debug_out_path(
                "fin-support/painted-shape-crease-intent-%d-%d-%d-%d.svg",
                int(painted.crease_run), int(intent.crease), int(intent.pair), int(intent.part));
            SVG svg(filename, BoundingBox(Point(0, 0),
                scaled_point(Vec2d(3. * view_size, view_size + 12.))), scale_(1.));
            const std::array<std::array<int, 2>, 3> axes{{{{0, 1}}, {{0, 2}}, {{1, 2}}}};
            const std::array<const char *, 3> headings{{"XY", "XZ", "YZ"}};
            auto display = [&](const Vec3d &point, size_t view) {
                return scaled_point(Vec2d(view * view_size + 4. +
                    4. * (point[axes[view][0]] - minimum[axes[view][0]]),
                    4. + 4. * (point[axes[view][1]] - minimum[axes[view][1]])));
            };
            auto draw_path = [&](const std::vector<Vec3d> &points, const char *color,
                                 double width, const char *label) {
                if (!svg.is_opened())
                    return;
                for (size_t view = 0; view < axes.size(); ++view) {
                    Polyline path;
                    for (const Vec3d &point : points)
                        path.points.push_back(display(point, view));
                    if (points.size() == 1)
                        svg.draw(path.first_point(), color, scale_(width));
                    else
                        svg.draw(path, color, scale_(width));
                    svg.draw_text(path.first_point(), label, color, 1);
                }
            };
            if (svg.is_opened()) {
                svg.add_comment("Source=accepted original crease geometry; green=unshifted input path before gap clipping; "
                                "orange=outward root stem; magenta=point input. "
                                "This is fin intent before physical contact/body generation.");
                for (size_t view = 0; view < axes.size(); ++view)
                    svg.draw_text(scaled_point(Vec2d(view * view_size, view_size)), headings[view], "#202020", 1);
                svg.draw_text(scaled_point(Vec2d(2., view_size + 5.)),
                    resolved.empty() ? "NO RESOLVED INTENT; gray=source" :
                    "gray=contact source; green=unshifted input; orange=root stem; magenta=point", "#202020", 1);
                svg.draw_text(scaled_point(Vec2d(2., view_size + 9.)),
                    "S# = source vertex; layer geometry uses ordinary path/body gap clipping", "#202020", 1);
            }
            draw_path(intent.points, "#888888", 0.45, "SOURCE");
            for (const FinCandidate *candidate : resolved) {
                const bool point = !candidate->path;
                const auto &vertices = point ? intent.points : candidate->path->points;
                const char *kind = point ? "POINT" : "PATH";
                draw_path(vertices, point ? "#cc0099" : "#009e73",
                          point ? 0.28 : 0.18, kind);
                if (!point && candidate->intent_root && vertices.size() >= 2)
                    draw_path({vertices[0], vertices[1]}, "#e69f00", 0.22, "ROOT STEM");
                for (size_t v = 0; v < vertices.size(); ++v) {
                    const Vec3d &xyz = vertices[v];
                    std::ostringstream detail;
                    detail << "stage=painted-crease-resolved-vertex run=" << painted.crease_run
                           << " crease=" << intent.crease << " pair=" << intent.pair << " part=" << intent.part
                           << " edge=" << *candidate->intent_edge << " kind=" << kind << " vertex=" << v
                           << " x=" << xyz.x() << " y=" << xyz.y() << " z=" << xyz.z();
                    if (!point && v > 0) {
                        const Vec3d &source = candidate->crease_intent->points[v - 1];
                        detail << " source_vertex=" << v - 1
                               << " source_x=" << source.x() << " source_y=" << source.y()
                               << " source_z=" << source.z();
                        if (svg.is_opened()) {
                            svg.draw(display(source, 0), "#0072b2", scale_(0.12));
                            svg.draw_text(display(source, 0),
                                ("S" + std::to_string(v - 1)).c_str(), "#0072b2", 1);
                        }
                    }
                    BOOST_LOG_TRIVIAL(debug) << "Fin support object=" << object.model_object()->name << " " << detail.str();
                    if (svg.is_opened())
                        svg.add_comment(detail.str());
                }
            }
            BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
                << " stage=painted-crease-resolved run=" << painted.crease_run
                << " crease=" << intent.crease << " pair=" << intent.pair << " part=" << intent.part
                << " resolved_groups=" << resolved.size() << " svg=" << filename << " opened=" << svg.is_opened();
        }
}

void generate_candidate_fin_support(PrintObject &object, const std::vector<PaintedLayer> &painted,
                                    const std::vector<Polygons> &blockers, const Polygon &bed,
                                    double line_width, FinWallGeometry &walls,
                                    const std::vector<FinCandidateProposal> &proposals,
                                    const char *stage, const char *diagnostic_stage)
{
    const bool painted_shape = object.config().fin_support_style.value == fssPaintedShape;
    if (object.print()->canceled())
        return;
    std::vector<PaintedLayerGeometryCache> geometry_cache;
    if (painted_shape && !proposals.empty()) {
        size_t cache_layers = 0;
        for (const FinCandidateProposal &proposal : proposals) {
            cache_layers = std::max(
                cache_layers, proposal.primary.top_layer + 1);
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
    if (painted_shape)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-geometry-cache-complete"
            << " layers=" << geometry_cache.size();

    struct PendingFin {
        TracedFin fin;
        FinComponentCenterGeometry component_geometry;
        bool valid = true;
        bool owns_seed = true;
        // Only shared joins; members already own their root depth.
        std::vector<ExPolygons> connection_centers;
        std::vector<FinComponentCenterGeometry> assembled_components;
    };
    std::vector<PendingFin> pending;
    pending.reserve(proposals.size());
    FinDiagnostics diagnostics;
    size_t rejected_geometry = 0;
    size_t root_intent_fins = 0;
    std::vector<std::pair<size_t, size_t>> realized_source_roots;
    size_t composite_intent_fins = 0;
    size_t composite_intent_segments = 0;
    size_t continuous_intent_envelopes = 0;
    struct StageFailureSummary {
        size_t count = 0;
        const char *reason = "none";
        long long layer = -1;
        double z = -1.;
        size_t island = std::numeric_limits<size_t>::max();
        size_t attachment_host_edge = std::numeric_limits<size_t>::max();
        size_t edge = std::numeric_limits<size_t>::max();
        size_t value = 0;

        void record(const char *failure_reason, long long failure_layer,
                    double failure_z, size_t failure_island,
                    size_t failure_edge,
                    size_t failure_attachment_host_edge = std::numeric_limits<size_t>::max(),
                    size_t failure_value = 0)
        {
            if (count++ > 0)
                return;
            reason = failure_reason;
            layer = failure_layer;
            z = failure_z;
            island = failure_island;
            attachment_host_edge = failure_attachment_host_edge;
            edge = failure_edge;
            value = failure_value;
        }
    };
    StageFailureSummary composite_merge_failures;
    StageFailureSummary assembly_failures;
    const char *composite_merge_failure_stage = "none";
    size_t composite_merge_failure_layer = std::numeric_limits<size_t>::max();
    if (painted_shape)
        BOOST_LOG_TRIVIAL(debug)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-realization-begin"
            << " proposals=" << proposals.size();
    // Classification and generation ownership are final before realization.
    // A rejected line cannot create a point alternative or promote a child.
    auto log_member_result = [&](const TracedFin &fin, const char *phase, bool accepted) {
        if (!painted_shape)
            return;
        const FinCandidate &seed = fin.seed;
        std::ostringstream context;
        context << "painted-shape-member phase=" << phase
            << " island=" << seed.intent_island.value_or(std::numeric_limits<size_t>::max())
            << " edge=" << seed.intent_edge.value_or(std::numeric_limits<size_t>::max())
            << " kind=" << (!seed.path ? "point" : seed.crease_intent ? "crease-path" :
                             seed.intent_root ? "root-stem" : "path");
        if (seed.crease_intent)
            context << " crease=" << seed.crease_intent->crease
                << " pair=" << seed.crease_intent->pair << " part=" << seed.crease_intent->part;
        BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
            << " stage=" << context.str() << " accepted=" << accepted
            << " path_layers=" << std::count(fin.path_present.begin(), fin.path_present.end(), true)
            << " source_layers=" << std::count_if(fin.contact_sources.begin(), fin.contact_sources.end(),
                   [](const std::optional<Vec2d> &source) { return source.has_value(); })
            << " wall_layers=" << std::count(fin.wall_present.begin(), fin.wall_present.end(), true);
        // A successful member may still have omitted invalid periodic bands.
        // Keep their diagnostics attributed too; acceptance here is not finalization.
        fin.diagnostics.log(context.str().c_str(), object, true);
    };
    for (const FinCandidateProposal &proposal : proposals) {
        if (object.print()->canceled())
            throw CanceledException();
        const bool owns_seed = !proposal.primary.intent_line || proposal.primary.intent_root.has_value();
        TracedFin fin;
        FinComponentCenterGeometry component_geometry;
        bool realized;
        if (proposal.primary.path) {
            // Resolve all finite layer paths before contacts: disconnected or
            // terminated intent must never authorize a later contact band.
            fin.seed = proposal.primary;
            realized = trace_fin(object, bed, fin.seed, object.config().fin_support_depth.value,
                object.config().support_object_xy_distance.value, fin.seed.dimensions.thickness,
                line_width, fin, painted_geometry_cache);
        } else {
            realized = realize_fin_candidate(object, proposal.primary, bed, painted, blockers,
                line_width, fin, walls, owns_seed, painted_shape ? &component_geometry : nullptr,
                painted_geometry_cache);
        }
        log_member_result(fin, proposal.primary.path ? "trace" : "realize", realized);
        if (!realized) {
            diagnostics.merge(fin.diagnostics);
            ++rejected_geometry;
            continue;
        }
        component_geometry.source = pending.size();
        pending.push_back({std::move(fin), std::move(component_geometry), true, owns_seed});
    }
    auto log_composite_failure = [&](size_t parent, size_t child) {
        const bool has_failure_layer =
            composite_merge_failure_layer < object.layer_count();
        const long long failure_layer = has_failure_layer ?
            static_cast<long long>(composite_merge_failure_layer) : -1;
        const double failure_z = has_failure_layer ?
            object.get_layer(int(composite_merge_failure_layer))->print_z : -1.;
        const size_t island = pending[child].fin.seed.intent_island.value_or(
            std::numeric_limits<size_t>::max());
        const size_t attachment_host_edge = pending[parent].fin.seed.intent_edge.value_or(
            std::numeric_limits<size_t>::max());
        const size_t child_edge = pending[child].fin.seed.intent_edge.value_or(
            std::numeric_limits<size_t>::max());
        composite_merge_failures.record(
            composite_merge_failure_stage, failure_layer, failure_z,
            island, child_edge, attachment_host_edge);
        BOOST_LOG_TRIVIAL(debug)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-composite-merge"
            << " reject=" << composite_merge_failure_stage
            << " layer=" << failure_layer
            << " z=" << failure_z
            << " island=" << island
            << " attachment_host_edge=" << attachment_host_edge
            << " child_edge=" << child_edge;
        std::vector<FinGeometryDebugSet> sets;
        append_fin_component_debug(sets, pending[parent].component_geometry, composite_merge_failure_layer);
        if (parent != child)
            append_fin_component_debug(sets, pending[child].component_geometry, composite_merge_failure_layer);
        if (pending[parent].fin.seed.path) {
            Polyline parent_path;
            for (const Vec3d &point : pending[parent].fin.seed.path->points)
                parent_path.points.push_back(scaled_point(point.head<2>()));
            sets.push_back({"attachment host input XY", {}, {std::move(parent_path)}, {}});
        }
        export_fin_geometry_failure(object, pending[child].fin, composite_merge_failure_layer,
            "composite", composite_merge_failure_stage, std::move(sets));
    };
    struct LineConnection {
        size_t member;
        FinCandidate::IntentNode node;
    };
    std::vector<std::vector<LineConnection>> line_connections(pending.size());
    // Whole finite groups connect through resolved endpoint or interior node
    // identities. Sharing a junction does not split a member's generation.
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
                const bool valid = (a.point - match->point).norm() <= EPSILON;
                composite_merge_failure_layer = std::numeric_limits<size_t>::max();
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

    auto nearest_on_segment = [](const Vec2d &point, const Vec2d &a, const Vec2d &b) -> Vec2d {
        const Vec2d edge = b - a;
        if (edge.squaredNorm() <= EPSILON * EPSILON)
            return a;
        return a + std::clamp((point - a).dot(edge) / edge.squaredNorm(), 0., 1.) * edge;
    };
    auto retain_root_paths = [&](const std::vector<size_t> &members) {
        size_t count = 0;
        for (size_t member : members)
            count = std::max(count, pending[member].fin.path_present.size());
        bool terminated = false;
        for (size_t remaining = count; remaining > 0; --remaining) {
            if (object.print()->canceled())
                throw CanceledException();
            const size_t layer = remaining - 1;
            std::vector<size_t> reachable;
            if (!terminated) {
                for (size_t member : members) {
                    const TracedFin &fin = pending[member].fin;
                    if (fin.seed.intent_root && layer < fin.path_present.size() && fin.path_present[layer])
                        reachable.push_back(member);
                }
                for (size_t cursor = 0; cursor < reachable.size(); ++cursor) {
                    const size_t member = reachable[cursor];
                    for (const LineConnection &connection : line_connections[member]) {
                        const size_t next = connection.member;
                        if (!pending[next].valid ||
                            std::find(reachable.begin(), reachable.end(), next) != reachable.end())
                            continue;
                        const auto contains = [&](size_t id) {
                            const TracedFin &fin = pending[id].fin;
                            if (layer >= fin.path_present.size() || !fin.path_present[layer])
                                return false;
                            const Points &points = fin.layer_paths[layer].points;
                            for (size_t i = 1; i < points.size(); ++i)
                                if ((nearest_on_segment(connection.node.point,
                                        unscale(points[i - 1]), unscale(points[i])) -
                                     connection.node.point).norm() <= EPSILON)
                                    return true;
                            return false;
                        };
                        if (contains(member) && contains(next))
                            reachable.push_back(next);
                    }
                }
                if (reachable.empty())
                    terminated = std::any_of(members.begin(), members.end(), [&](size_t member) {
                        const TracedFin &fin = pending[member].fin;
                        return fin.seed.intent_root && layer < fin.path_present.size();
                    });
            }
            for (size_t member : members) {
                auto &present = pending[member].fin.path_present;
                if (layer < present.size() &&
                    std::find(reachable.begin(), reachable.end(), member) == reachable.end())
                    present[layer] = false;
            }
        }
    };
    std::vector<bool> clipped(pending.size(), false);
    for (size_t start = 0; start < pending.size(); ++start) {
        if (!pending[start].fin.seed.path || clipped[start])
            continue;
        std::vector<size_t> members{start};
        clipped[start] = true;
        for (size_t i = 0; i < members.size(); ++i)
            for (const auto &connection : line_connections[members[i]])
                if (!clipped[connection.member]) {
                    clipped[connection.member] = true;
                    members.push_back(connection.member);
                }
        retain_root_paths(members);
        double thickness = 0.;
        for (size_t member : members)
            thickness = std::max(thickness, pending[member].fin.seed.dimensions.thickness);
        // Caps, contacts and shared joins must use one component thickness.
        // Widening a second body after contact completion would reopen its seam.
        for (size_t member : members)
            pending[member].fin.seed.dimensions.thickness = thickness;
    }
    // Structural downstream membership uses the resolved directed node order,
    // including junctions inside a continued line. A terminal multi-inlet
    // junction alone does not authorize carrying a lost contact band. Resolve
    // this before member geometry so the decision is independent of its order.
    std::vector<bool> end_has_downstream(pending.size(), false);
    for (size_t member = 0; member < pending.size(); ++member) {
        const auto &nodes = pending[member].fin.seed.intent_nodes;
        if (nodes.empty())
            continue;
        for (const LineConnection &connection : line_connections[member]) {
            const auto &next = pending[connection.member].fin.seed.intent_nodes;
            if (connection.node.node == nodes.back().node &&
                !next.empty() && next.back().node != connection.node.node) {
                end_has_downstream[member] = true;
                break;
            }
        }
    }
    for (size_t member = 0; member < pending.size(); ++member) {
        auto &item = pending[member];
        if (!item.fin.seed.path)
            continue;
        const bool generated = generate_fin_geometry(object, item.fin, bed, walls, painted, blockers,
                item.owns_seed, &item.component_geometry, painted_geometry_cache,
                end_has_downstream[member]);
        log_member_result(item.fin, "geometry", generated);
        if (!generated) {
            diagnostics.merge(item.fin.diagnostics);
            item.valid = false;
            ++rejected_geometry;
            continue;
        }
        if (item.fin.seed.intent_root) {
            const auto key = std::make_pair(*item.fin.seed.intent_island, item.fin.seed.intent_root->node);
            if (std::find(realized_source_roots.begin(), realized_source_roots.end(), key) ==
                realized_source_roots.end()) {
                realized_source_roots.push_back(key);
                ++root_intent_fins;
            }
        }
    }
    if (painted_shape)
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
    // Related fits are pieces of one sheet. Completed members own their body
    // boundaries including root outer depth; the graph contributes only shared joins.
    // Point intents remain independent of this line-only graph.
    const double gap = object.config().support_object_xy_distance.value;
    auto line_connection_centers = [&](size_t layer_id, const std::vector<FinBodyAxis> &axes,
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
            fin_junction_centers(axes, radius, 0.5 * width),
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
        size_t layer_id, ExPolygons centers,
        const std::vector<FinComponentCenterGeometry> &components,
        PreparedFinLayer &prepared, const char *&failure_stage) {
        failure_stage = "none";
        prepared = {};
        for (const FinComponentCenterGeometry &component : components) {
            if (layer_id < component.completed_centers.size())
                expolygons_append(centers, component.completed_centers[layer_id]);
            for (const FinCenterExtension &extension : component.extensions) {
                if (layer_id >= extension.layers.size())
                    continue;
                const FinExtensionLayerState layer_state = extension.layer_states[layer_id];
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
                }
            }
        }

        prepared.contact_centers = union_ex(prepared.contact_centers);
        prepared.clearance_exemptions = union_ex(
            prepared.clearance_exemptions);
        // Union completed members and shared joins only. Adding query-body
        // copies or separate root strips would restore obsolete caps and seams.
        prepared.centers = union_ex(centers);
        if (prepared.centers.empty()) {
            failure_stage = "empty-completed-center";
            return false;
        }
        return true;
    };

    auto prepare_line_intent_component = [&](
        TracedFin &hub, const std::vector<size_t> &members,
        std::vector<ExPolygons> &connection_centers,
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
        double thickness = hub.seed.dimensions.thickness;
        for (const size_t member : members) {
            assert(member < pending.size());
            const FinComponentCenterGeometry &component_geometry =
                pending[member].component_geometry;
            assert(component_geometry.source == member);
            if (!component_geometry.complete)
                return fail("member-incomplete");
            assembled_components.push_back(component_geometry);
            thickness = std::max(
                thickness, pending[member].fin.seed.dimensions.thickness);
        }

        connection_centers.assign(layer_count, {});
        for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
            if (object.print()->canceled())
                throw CanceledException();
            if (hub.body_axes[layer_id].empty()) {
                if (hub.seed.path)
                    continue;
                return fail("body-path-shape", layer_id);
            }
            connection_centers[layer_id] = line_connection_centers(
                layer_id, hub.body_axes[layer_id], thickness);
            // Empty is normal: straight junctions and members without a root
            // extension need no geometry beyond their completed boundaries.
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

        // Model clearance and bead expansion happen after all line members
        // have supplied their completed center geometry.
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

    auto prepared_member_layer_center = [](
        const FinComponentCenterGeometry &component, size_t layer_id) {
        return layer_id < component.completed_centers.size() ?
            component.completed_centers[layer_id] : ExPolygons{};
    };
    std::vector<bool> line_processed(pending.size(), false);
    std::vector<bool> emitted_composite(pending.size(), false);
    for (size_t start = 0; start < pending.size(); ++start) {
        if (!pending[start].valid || !pending[start].fin.seed.intent_line || line_processed[start])
            continue;
        std::vector<size_t> members{start};
        line_processed[start] = true;
        for (size_t i = 0; i < members.size(); ++i)
            for (const LineConnection &connection : line_connections[members[i]])
                if (pending[connection.member].valid && !line_processed[connection.member]) {
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
        if (!has_seed) {
            for (const size_t member : members) {
                if (pending[member].valid)
                    ++rejected_geometry;
                pending[member].valid = false;
            }
            continue;
        }
        retain_root_paths(members);
        TracedFin completed = pending[root].fin;
        completed.body_axes.assign(completed.seed.top_layer + 1, {});
        std::set<size_t> generation_roots;
        std::sort(members.begin(), members.end(), [&](size_t a, size_t b) {
            return pending[a].fin.seed.intent_edge < pending[b].fin.seed.intent_edge;
        });
        for (const size_t member : members) {
            const TracedFin &fin = pending[member].fin;
            if (fin.seed.intent_root)
                generation_roots.insert(fin.seed.intent_root->node);
            for (size_t layer = 0; layer < fin.body_axes.size(); ++layer)
                if (fin.path_present[layer])
                    for (const FinBodyAxis &axis : fin.body_axes[layer])
                        completed.body_axes[layer].push_back(axis);
        }
        completed.intent_root_count = generation_roots.size();

        // Geometry failure may remove another member after the initial intent
        // clip. Remove its unreachable material without inventing new roots.
        completed.path_present.assign(completed.body_axes.size(), false);
        for (size_t layer = 0; layer < completed.body_axes.size(); ++layer) {
            completed.path_present[layer] = !completed.body_axes[layer].empty();
            for (const size_t member : members) {
                const TracedFin &fin = pending[member].fin;
                if (layer < fin.path_present.size() && fin.path_present[layer])
                    continue;
                auto &component = pending[member].component_geometry;
                if (layer < component.body_centers.size()) {
                    component.body_centers[layer].clear();
                    component.completed_centers[layer].clear();
                }
                for (FinCenterExtension &extension : component.extensions)
                    if (layer < extension.layers.size()) {
                        extension.layers[layer].clear();
                        extension.layer_states[layer] = FinExtensionLayerState::inactive;
                    }
                if (layer < pending[member].fin.contacts.size())
                    pending[member].fin.contacts[layer] = false;
            }
        }
        std::vector<ExPolygons> connection_centers;
        std::vector<FinComponentCenterGeometry> assembled_components;
        if (!prepare_line_intent_component(completed, members, connection_centers, assembled_components)) {
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
        pending[root].connection_centers = std::move(connection_centers);
        pending[root].assembled_components = std::move(assembled_components);
        for (const size_t member : members) {
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

    composite_intent_fins += std::count(
        emitted_composite.begin(), emitted_composite.end(), true);
    if (painted_shape)
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-components-complete"
            << " pending=" << pending.size()
            << " composite_fins=" << composite_intent_fins
            << " merge_failures=" << composite_merge_failures.count;

    std::vector<ExPolygons> painted_layer_centers;
    if (painted_shape) {
        // The intent graph is immutable from this point onward. Assemble all
        // components, then clip the layer union once by the exempted model mask.
        // This common result goes directly to outlining, without per-owner
        // geometry reconstruction or another whole-fin acceptance barrier.
        std::vector<std::vector<PreparedFinLayer>> prepared(pending.size());
        auto dump_prepared_failure = [&](size_t item_id, size_t layer_id, const char *stage,
                                         const char *reason) {
            const auto &item = pending[item_id];
            std::vector<FinGeometryDebugSet> sets;
            if (layer_id < item.connection_centers.size())
                sets.push_back({"shared junction additions", item.connection_centers[layer_id], {}, {}});
            for (const auto &component : item.assembled_components)
                append_fin_component_debug(sets, component, layer_id);
            if (layer_id < prepared[item_id].size()) {
                const auto &p = prepared[item_id][layer_id];
                sets.push_back({"combined centers BEFORE final gap", p.centers, {}, {}});
                sets.push_back({"contact centers", p.contact_centers, {}, {}});
                sets.push_back({"clearance exemptions", p.clearance_exemptions, {}, {}});
            }
            if (painted_geometry_cache && layer_id < painted_geometry_cache->size()) {
                const auto &cache = (*painted_geometry_cache)[layer_id];
                sets.push_back({"path gap mask", cache.model_gap, {}, {}});
                sets.push_back({"final gap mask BEFORE exemptions", cache.final_model_clearance, {}, {}});
            }
            export_fin_geometry_failure(object, item.fin, layer_id, stage, reason, std::move(sets));
        };
        std::vector<ExPolygons> layer_centers(object.layer_count());
        std::vector<ExPolygons> layer_clearance_exemptions(object.layer_count());
        size_t projection_exemption_sections = 0;
        for (size_t item_id = 0; item_id < pending.size(); ++item_id) {
            PendingFin &item = pending[item_id];
            if (!item.valid)
                continue;
            if (item.connection_centers.empty()) {
                assert(item.component_geometry.source == item_id);
                if (!item.component_geometry.complete ||
                    item.component_geometry.body_centers.empty()) {
                    export_fin_geometry_failure(object, item.fin, std::numeric_limits<size_t>::max(),
                        "assembly", "incomplete-member-geometry", {});
                    item.valid = false;
                    ++rejected_geometry;
                    continue;
                }
                item.assembled_components = {item.component_geometry};
                item.connection_centers.resize(item.component_geometry.completed_centers.size());
            }
            const size_t layer_count = item.connection_centers.size();
            prepared[item_id].resize(layer_count);
            bool assembled = true;
            for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
                if (!item.fin.path_present.empty() && !item.fin.path_present[layer_id])
                    continue;
                const char *failure_stage = "none";
                if (!assemble_component_section(
                        layer_id, item.connection_centers[layer_id],
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
                    dump_prepared_failure(item_id, layer_id, "assembly", failure_stage);
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
                if (seed.intent_island && seed.source_attachment_junction)
                    sibling_groups[{*seed.intent_island, seed.source_attachment_junction->first,
                                    seed.source_attachment_junction->second}].push_back({owner, &component});
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
                std::set<size_t> active_sources;
                for (size_t i = 0; i < members.size(); ++i) {
                    member_centers[i] = prepared_member_layer_center(*members[i].component, layer_id);
                    if (!member_centers[i].empty()) {
                        const auto &seed = pending[members[i].component->source].fin.seed;
                        active_sources.insert(seed.path->source_edge);
                        expolygons_append(siblings, member_centers[i]);
                    }
                }
                if (active_sources.size() < 2)
                    continue;
                // Only bounded holes of this sibling union are filled. Open
                // notches and cavities requiring a parent/other group stay open.
                siblings = union_ex(siblings);
                for (const ExPolygon &area : siblings) {
                    for (Polygon hole : area.holes) {
                        hole.reverse();
                        const ExPolygons cavity{ExPolygon(std::move(hole))};
                        // Record a touching source for diagnostics and seam
                        // placement; the fill itself joins the common layer.
                        // This changes no junction or parent relationship.
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

        painted_layer_centers.resize(object.layer_count());
        size_t nonempty_support_layers = 0;
        size_t model_gap_clip_layers = 0;
        size_t clearance_exemption_layers = 0;
        std::set<size_t> reported_contact_rear_cuts;
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
            // Slightly enlarge the exemption so clearance does not cut
            // contact/taper geometry exactly at its shared boundary.
            clearance = diff_ex(
                clearance, layer_clearance_exemptions[layer_id],
                ApplySafetyOffset::Yes);
            // This is the model-gap subtraction of completed center geometry;
            // finite intent paths were clipped separately before contacts.
            painted_layer_centers[layer_id] = diff_ex(
                layer_centers[layer_id], clearance);
            // A contact can remain present while final clearance removes body
            // material immediately behind its shared rear edge. Record that
            // neighborhood once per source, without changing gap exemptions
            // or treating this diagnostic as a new rejection rule.
            const double half_width = 0.5 * support_material_flow(
                &object, float(object.get_layer(int(layer_id))->height)).width();
            for (const PendingFin &item : pending) {
                if (!item.valid)
                    continue;
                for (const FinComponentCenterGeometry &component : item.assembled_components) {
                    if (reported_contact_rear_cuts.count(component.source) ||
                        layer_id >= component.body_centers.size() ||
                        component.body_centers[layer_id].empty())
                        continue;
                    for (const FinCenterExtension &extension : component.extensions) {
                        if (layer_id >= extension.layer_states.size() ||
                            extension.layer_states[layer_id] != FinExtensionLayerState::contact ||
                            layer_id >= extension.attachments.size() || !extension.attachments[layer_id])
                            continue;
                        const Line &cap = *extension.attachments[layer_id];
                        if (cap.a == cap.b)
                            continue;
                        const Polygons neighborhood = offset(Polyline{cap.a, cap.b},
                            float(scale_(half_width)), jtRound, double(scale_(0.005)),
                            ClipperLib::etOpenButt);
                        const ExPolygons rear_before = intersection_ex(
                            component.body_centers[layer_id], neighborhood);
                        const ExPolygons removed = diff_ex(rear_before, painted_layer_centers[layer_id]);
                        if (removed.empty())
                            continue;
                        std::vector<FinGeometryDebugSet> sets;
                        append_fin_component_debug(sets, component, layer_id);
                        sets.push_back({"body within half bead of contact rear BEFORE gap", rear_before, {}, {}});
                        sets.push_back({"body removed near contact rear", removed, {}, {}});
                        sets.push_back({"combined centers BEFORE final gap", layer_centers[layer_id], {}, {}});
                        sets.push_back({"final gap mask AFTER exemptions", clearance, {}, {}});
                        sets.push_back({"merged centers AFTER final gap", painted_layer_centers[layer_id], {}, {}});
                        export_fin_geometry_failure(object, pending[component.source].fin, layer_id,
                            "layer-clearance", "contact-rear-gap-cut", std::move(sets));
                        reported_contact_rear_cuts.insert(component.source);
                        break;
                    }
                }
            }
            if (painted_layer_centers[layer_id].empty()) {
                const Layer &layer = *object.get_layer(int(layer_id));
                BOOST_LOG_TRIVIAL(info)
                    << "Fin support object=" << object.model_object()->name
                    << " stage=painted-shape-layer-clearance reason=empty-center-after-gap"
                    << " layer=" << layer_id
                    << " slice_z=" << layer.slice_z << " print_z=" << layer.print_z
                    << " skipped_scope=layer";
                for (size_t item_id = 0; item_id < pending.size(); ++item_id) {
                    if (!pending[item_id].valid || layer_id >= prepared[item_id].size() ||
                        prepared[item_id][layer_id].centers.empty())
                        continue;
                    export_fin_geometry_failure(object, pending[item_id].fin, layer_id,
                        "layer-clearance", "empty-center-after-gap",
                        {{"combined centers BEFORE final gap", layer_centers[layer_id], {}, {}},
                         {"final gap mask AFTER exemptions", clearance, {}, {}},
                         {"merged centers AFTER final gap", painted_layer_centers[layer_id], {}, {}}});
                    break;
                }
            }
            ++model_gap_clip_layers;
        }
        BOOST_LOG_TRIVIAL(info)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-layer-clearance"
            << " layers=" << nonempty_support_layers
            << " model_gap_clips=" << model_gap_clip_layers
            << " clearance_exemption_layers=" << clearance_exemption_layers
            << " projection_exemption_sections=" << projection_exemption_sections;

        // Source contours are retained only to attribute merged output and
        // choose its seam direction. They are not clipped back out of the layer.
        for (size_t item_id = 0; item_id < pending.size(); ++item_id) {
            PendingFin &item = pending[item_id];
            if (!item.valid)
                continue;
            item.fin.centers.resize(prepared[item_id].size());
            item.fin.contact_projection_retained_layers = 0;
            for (size_t layer_id = 0; layer_id < prepared[item_id].size(); ++layer_id) {
                auto &source = prepared[item_id][layer_id];
                for (const ExPolygons &projection : source.projection_sections)
                    item.fin.contact_projection_retained_layers +=
                        diff_ex(projection, painted_layer_centers[layer_id]).empty();
                item.fin.centers[layer_id] = std::move(source.centers);
            }
        }
    }

    std::vector<TracedFin> fins;
    fins.reserve(pending.size());
    size_t superseded_count = 0;
    for (PendingFin &item : pending) {
        if (!item.valid)
            continue;
        if (object.print()->canceled())
            throw CanceledException();
        if (!painted_shape && !generate_fin_toolpaths(object, item.fin)) {
            diagnostics.merge(item.fin.diagnostics);
            ++rejected_geometry;
            continue;
        }
        diagnostics.merge(item.fin.diagnostics);
        if (!painted_shape) {
            const FinPlacement placement = evaluate_fin_placement(object, item.fin, fins);
            if (placement.overlap_layer) {
                diagnostics.reject(FinReject::overlap,
                    object.get_layer(int(*placement.overlap_layer))->print_z);
                ++rejected_geometry;
                continue;
            }
            for (auto it = placement.superseded.rbegin(); it != placement.superseded.rend(); ++it) {
                fins.erase(fins.begin() + *it);
                ++superseded_count;
            }
        }
        fins.push_back(std::move(item.fin));
    }
    size_t lower_contacts_accepted = 0;
    size_t suppressed_contact_bands = 0;
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

    }
    double candidate_top_z = 0.;
    for (const FinCandidateProposal &proposal : proposals)
        candidate_top_z = std::max(candidate_top_z,
            object.get_layer(int(proposal.primary.top_layer))->print_z);
    FinEmitStats emit_stats;
    if (!fins.empty()) {
        if (painted_shape)
            BOOST_LOG_TRIVIAL(debug)
                << "Fin support object=" << object.model_object()->name
                << " stage=painted-shape-emit-begin"
                << " fins=" << fins.size();
        emit_stats = emit_fin_support(object, fins,
            painted_shape ? &painted_layer_centers : nullptr);
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
            stage_failure_summary << ",attachment_host_edge=" << failure.attachment_host_edge;
        stage_failure_summary << ",edge=" << failure.edge
            << ",value=" << failure.value << ')';
        has_stage_failures = true;
    };
    append_stage_failure("composite-merge", composite_merge_failures, true);
    append_stage_failure("assembly", assembly_failures, false);
    if (!has_stage_failures)
        stage_failure_summary << "none";
    BOOST_LOG_TRIVIAL(info) << "Fin support object=" << object.model_object()->name
        << " stage=" << stage << " candidates=" << proposals.size() << " fins=" << fins.size()
        << " candidate_top_z=" << candidate_top_z
        << " emitted_top_z=" << emit_stats.emitted_top_z
        << " root_intent_fins=" << root_intent_fins
        << " composite_intent_fins=" << composite_intent_fins
        << " composite_intent_segments=" << composite_intent_segments
        << " continuous_intent_envelopes=" << continuous_intent_envelopes
        << " composite_intent_merge_failures=" << composite_merge_failures.count
        << " assembly_failures=" << assembly_failures.count
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
        << " superseded=" << superseded_count
        << " merged_outline_failures=" << emit_stats.merged_outline_failures
        << " merged_outline_splits=" << emit_stats.merged_outline_splits
        << " rejected_geometry=" << rejected_geometry;
    diagnostics.log(diagnostic_stage, object, painted_shape);
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
    FinWallGeometry walls(object);
    if (config.fin_support_style.value == fssSimple) {
        const std::vector<FinCandidate> candidates = select_simple_fin_candidates(object, painted);
        std::vector<FinCandidateProposal> proposals;
        proposals.reserve(candidates.size());
        for (const FinCandidate &candidate : candidates)
            proposals.push_back({candidate});
        generate_candidate_fin_support(object, painted, blockers, bed, line_width, walls, proposals,
                                       "simple", "simple-geometry");
        return;
    }
    if (config.fin_support_style.value == fssPaintedShape) {
        const FinShapeAnalysis shape = analyze_fin_shape(object, line_width);
        BOOST_LOG_TRIVIAL(debug)
            << "Fin support object=" << object.model_object()->name
            << " stage=painted-shape-analysis-complete";
        const std::vector<FinCandidateProposal> proposals = resolve_painted_fin_paths(
            object, select_painted_shape_fin_candidates(object, painted_data, shape));
        export_painted_crease_fin_paths(object, painted_data, proposals);
        generate_candidate_fin_support(object, painted, blockers, bed, line_width, walls, proposals,
                                       "painted-shape", "painted-shape-geometry");
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
                object, plan.seed, bed, painted, blockers, line_width, fin, walls);
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
                                object, candidate, bed, painted, blockers, line_width, shorter, walls))
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
