#include "ContinuousPrint.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../PrintConfig.hpp"
#include "../ClipperUtils.hpp"
#include "../Flow.hpp"
#include "../Layer.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace Slic3r {

// Arc-length sampling step of ContinuousLayerPlan::sampling, in mm (unscaled).
static constexpr double CONTINUOUS_PRINT_SAMPLING_STEP = 1.0;

// Append the 2D projection of `entity` to `out`, reversed if `reversed` is set,
// skipping the first point when it continues the previous entity (it is already the tail of `out`).
static void append_entity_polyline(const ExtrusionEntity &entity, bool reversed, Points &out)
{
    Polyline polyline = entity.as_polyline();
    if (reversed)
        polyline.reverse();
    if (polyline.points.empty())
        return;
    size_t begin = 0;
    if (! out.empty() && is_approx(out.back(), polyline.points.front()))
        begin = 1;
    out.insert(out.end(), polyline.points.begin() + begin, polyline.points.end());
}

// -----------------------------------------------------------------------------------------------
// Junction splitting (design doc 3.5, "lollipop" extension)
// -----------------------------------------------------------------------------------------------

void flatten_extrusion_entities(const ExtrusionEntity *entity, std::vector<ExtrusionEntity*> &out)
{
    if (entity == nullptr)
        return;
    if (const auto *eec = dynamic_cast<const ExtrusionEntityCollection*>(entity))
        flatten_extrusion_entities(eec->entities, out);
    else
        out.push_back(const_cast<ExtrusionEntity*>(entity));
}

void flatten_extrusion_entities(const ExtrusionEntitiesPtr &entities, std::vector<ExtrusionEntity*> &out)
{
    for (const ExtrusionEntity *entity : entities)
        flatten_extrusion_entities(entity, out);
}

namespace {

// Flattened view of an entity for junction detection and splitting.
struct FlattenedEntity {
    Polyline3            polyline; // Flattened points; for loops a closed ring (last == first).
    std::vector<size_t>  src_seg;  // Source path index per segment (size == points - 1).
    ExtrusionPaths       paths;    // Source paths carrying the extrusion attributes.
    bool                 closed = false;
};

std::optional<FlattenedEntity> flatten_entity(const ExtrusionEntity &entity)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        if (path->polyline.points.size() < 2)
            return std::nullopt;
        FlattenedEntity flat;
        flat.polyline = path->polyline;
        flat.src_seg.assign(flat.polyline.points.size() - 1, 0);
        flat.paths.push_back(*path);
        return flat;
    }
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        FlattenedEntity flat;
        flat.closed = true;
        flat.paths  = loop->paths;
        for (size_t k = 0; k < loop->paths.size(); ++ k) {
            const auto &pts = loop->paths[k].polyline.points;
            // Loop paths are chained: path k starts where path k-1 ends. Skip the duplicate,
            // but only if they really are chained.
            const size_t begin = (k > 0 && ! flat.polyline.points.empty() && pts.front() == flat.polyline.points.back()) ? 1 : 0;
            for (size_t i = begin; i < pts.size(); ++ i) {
                if (! flat.polyline.points.empty())
                    flat.src_seg.push_back(k);
                flat.polyline.points.push_back(pts[i]);
            }
        }
        // Close the ring explicitly.
        if (flat.polyline.points.size() >= 2 && flat.polyline.points.back() != flat.polyline.points.front()) {
            flat.polyline.points.push_back(flat.polyline.points.front());
            flat.src_seg.push_back(flat.src_seg.empty() ? 0 : flat.src_seg.back());
        }
        if (flat.polyline.points.size() < 2)
            return std::nullopt;
        return flat;
    }
    return std::nullopt;
}

// Arc-length prefix sums (2D) of a flattened polyline. Result size == points.size(), [0] == 0.
std::vector<coordf_t> arc_prefix(const Polyline3 &polyline)
{
    std::vector<coordf_t> prefix(polyline.points.size(), 0.);
    for (size_t i = 1; i < polyline.points.size(); ++ i) {
        const Vec2d a(polyline.points[i - 1].x(), polyline.points[i - 1].y());
        const Vec2d b(polyline.points[i].x(), polyline.points[i].y());
        prefix[i] = prefix[i - 1] + (b - a).norm();
    }
    return prefix;
}

// Closest position (arc length) of point `p` on the flattened polyline, and its 2D distance.
coordf_t closest_s_on_polyline(const Polyline3 &polyline, const std::vector<coordf_t> &prefix, const Point &p, coordf_t &out_dist)
{
    const Vec2d  pt(p.x(), p.y());
    coordf_t     best_dist = std::numeric_limits<coordf_t>::max();
    coordf_t     best_s    = 0;
    for (size_t i = 1; i < polyline.points.size(); ++ i) {
        const Vec2d a(polyline.points[i - 1].x(), polyline.points[i - 1].y());
        const Vec2d b(polyline.points[i].x(), polyline.points[i].y());
        const Vec2d ab = b - a;
        const coordf_t len2 = ab.squaredNorm();
        const coordf_t t    = (len2 > 0) ? std::clamp((pt - a).dot(ab) / len2, 0., 1.) : 0.;
        const coordf_t dist = (pt - (a + ab * t)).norm();
        if (dist < best_dist) {
            best_dist = dist;
            best_s    = prefix[i - 1] + t * std::sqrt(len2);
        }
    }
    out_dist = best_dist;
    return best_s;
}

// Point (3D, Z interpolated) at arc length `s` of the flattened polyline; also the source segment index.
Point3 point_at_s(const FlattenedEntity &flat, const std::vector<coordf_t> &prefix, coordf_t s, size_t &out_src)
{
    const auto &pts = flat.polyline.points;
    size_t i = 1;
    while (i + 1 < pts.size() && prefix[i] < s)
        ++ i;
    const Vec3d a = pts[i - 1].cast<coordf_t>();
    const Vec3d b = pts[i].cast<coordf_t>();
    const coordf_t seg_len = prefix[i] - prefix[i - 1];
    const coordf_t t       = (seg_len > 0) ? (s - prefix[i - 1]) / seg_len : 0.;
    const Vec3d p = a + (b - a) * t;
    out_src = flat.src_seg[i - 1];
    return Point3(p.x(), p.y(), p.z()); // Point3(double, double, double) rounds to scaled coords
}

// Linearize a closed ring so that it starts (and ends) at arc length `s0`.
void rotate_ring(FlattenedEntity &flat, coordf_t s0)
{
    assert(flat.closed);
    const std::vector<coordf_t> prefix = arc_prefix(flat.polyline);
    const coordf_t total = prefix.back();
    const auto   &pts    = flat.polyline.points;

    // Original vertices after s0 in ring order (excluding the ring closing duplicate at pts[0]/pts[n-1]).
    std::vector<std::pair<coordf_t, size_t>> vertices; // (arc length measured from s0, point index)
    for (size_t i = 0; i + 1 < pts.size(); ++ i) {
        const coordf_t s = prefix[i] > s0 ? prefix[i] : prefix[i] + total;
        if (s < s0 + total)
            vertices.emplace_back(s, i);
    }
    std::sort(vertices.begin(), vertices.end());

    FlattenedEntity rotated;
    rotated.paths = flat.paths;
    auto emit = [&rotated](const Point3 &p, size_t src) {
        if (! rotated.polyline.points.empty())
            rotated.src_seg.push_back(src);
        rotated.polyline.points.push_back(p);
    };
    size_t src0 = 0;
    const Point3 p0 = point_at_s(flat, prefix, s0, src0);
    emit(p0, src0);
    for (const auto &[s, i] : vertices)
        emit(pts[i], flat.src_seg[i == 0 ? flat.src_seg.size() - 1 : i - 1]);
    emit(p0, src0);
    flat = std::move(rotated);
}

struct PolylinePiece {
    Polyline3           polyline;
    std::vector<size_t> src_seg;
};

// Split an open flattened polyline at the given sorted, interior arc-length positions.
std::vector<PolylinePiece> split_open(const FlattenedEntity &flat, const std::vector<coordf_t> &positions)
{
    const std::vector<coordf_t> prefix = arc_prefix(flat.polyline);
    const coordf_t total = prefix.back();

    std::vector<coordf_t> bounds{0.};
    bounds.insert(bounds.end(), positions.begin(), positions.end());
    bounds.push_back(total);

    std::vector<PolylinePiece> pieces;
    for (size_t b = 0; b + 1 < bounds.size(); ++ b) {
        const coordf_t s0 = bounds[b], s1 = bounds[b + 1];
        PolylinePiece piece;
        auto emit = [&piece](const Point3 &p, size_t src) {
            if (! piece.polyline.points.empty())
                piece.src_seg.push_back(src);
            piece.polyline.points.push_back(p);
        };
        size_t src = 0;
        emit(point_at_s(flat, prefix, s0, src), src);
        for (size_t i = 1; i < flat.polyline.points.size(); ++ i)
            if (prefix[i] > s0 && prefix[i] < s1)
                emit(flat.polyline.points[i], flat.src_seg[i - 1]);
        emit(point_at_s(flat, prefix, s1, src), src); // segment containing s1
        if (piece.polyline.points.size() >= 2)
            pieces.push_back(std::move(piece));
    }
    return pieces;
}

// Emit ExtrusionPath entities from a piece, grouping consecutive segments by their source path
// so that each emitted path inherits the attributes (role / width / height / flow) of its source.
void emit_piece(const PolylinePiece &piece, const ExtrusionPaths &src_paths, std::vector<std::unique_ptr<ExtrusionEntity>> &out)
{
    const auto &pts = piece.polyline.points;
    size_t begin = 0;
    for (size_t i = 0; i < piece.src_seg.size(); ++ i) {
        const bool last = (i + 1 == piece.src_seg.size());
        if (last || piece.src_seg[i + 1] != piece.src_seg[i]) {
            const ExtrusionPath &src = src_paths[piece.src_seg[i]];
            Polyline3 sub;
            for (size_t k = begin; k <= i + 1; ++ k)
                sub.append(pts[k]);
            if (sub.points.size() >= 2)
                out.emplace_back(std::make_unique<ExtrusionPath>(std::move(sub), src));
            begin = i + 1;
        }
    }
}

// Linearize a closed ExtrusionLoop into open ExtrusionPath(s) starting at `start` (projected onto
// the loop, only honored if it actually lies on it) and ending at the same point. This keeps the
// trace closed while giving the emitter a single direction and a deterministic seam, so loops need
// no special handling in the chain emitter.
void linearize_loop_into(const ExtrusionLoop &loop, const Point *start,
                         std::vector<std::unique_ptr<ExtrusionEntity>> &out)
{
    std::optional<FlattenedEntity> flat_opt = flatten_entity(loop);
    if (! flat_opt)
        return;
    FlattenedEntity flat = std::move(*flat_opt);
    if (start != nullptr) {
        const std::vector<coordf_t> prefix = arc_prefix(flat.polyline);
        coordf_t dist = 0;
        const coordf_t s = closest_s_on_polyline(flat.polyline, prefix, *start, dist);
        // Only rotate when the requested start is really on the loop; otherwise keep the stored seam.
        if (dist <= SCALED_EPSILON && s > SCALED_EPSILON && prefix.back() - s > SCALED_EPSILON)
            rotate_ring(flat, s);
    }
    PolylinePiece piece;
    piece.polyline = flat.polyline;
    piece.src_seg  = flat.src_seg;
    emit_piece(piece, flat.paths, out);
}

} // namespace

// An endpoint of entity `entity` (end==0 first, end==1 last) that must be moved onto a junction
// point so the two traces close exactly. The move is bounded by the junction tolerance.
struct EndpointSnap {
    size_t entity;
    int    end;
    Point  target;
};

std::vector<std::unique_ptr<ExtrusionEntity>> split_entities_at_junctions(
    const std::vector<ExtrusionEntity*> &entities,
    double junction_epsilon)
{
    const size_t n = entities.size();
    std::vector<std::unique_ptr<ExtrusionEntity>> out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++ i)
        out.emplace_back(entities[i]->clone());

    // Flatten the splittable entities (ExtrusionPath / ExtrusionLoop); others are passed through.
    std::vector<std::optional<FlattenedEntity>> flats(n);
    for (size_t i = 0; i < n; ++ i)
        if (entities[i] != nullptr)
            flats[i] = flatten_entity(*entities[i]);

    // Collect interior junction positions per entity: endpoints of other entities lying on it.
    // Also record the snap that closes each contact exactly (the physical gap is <= junction_epsilon,
    // so nothing is drawn: the endpoint is moved onto the junction point within tolerance).
    std::vector<std::vector<coordf_t>> junctions(n);
    std::vector<EndpointSnap>          snaps;
    for (size_t i = 0; i < n; ++ i) {
        if (! flats[i])
            continue;
        const std::vector<coordf_t> prefix = arc_prefix(flats[i]->polyline);
        const coordf_t total = prefix.back();
        for (size_t j = 0; j < n; ++ j) {
            if (j == i || entities[j] == nullptr)
                continue;
            const Point ends[2] = { entities[j]->first_point(), entities[j]->last_point() };
            for (int k = 0; k < 2; ++ k) {
                coordf_t dist = 0;
                const coordf_t s = closest_s_on_polyline(flats[i]->polyline, prefix, ends[k], dist);
                if (dist <= junction_epsilon && s > junction_epsilon && total - s > junction_epsilon) {
                    size_t src = 0;
                    const Point3 p = point_at_s(*flats[i], prefix, s, src);
                    junctions[i].push_back(s);
                    snaps.push_back({j, k, p.to_point()});
                }
            }
        }
    }

    // Apply the endpoint snaps (paths/multipaths only; loops keep their closed ring).
    for (const EndpointSnap &snap : snaps) {
        if (auto *path = dynamic_cast<ExtrusionPath*>(out[snap.entity].get())) {
            if (path->polyline.points.empty())
                continue;
            Point3 &p = snap.end == 0 ? path->polyline.points.front() : path->polyline.points.back();
            p = Point3(snap.target.x(), snap.target.y(), p.z());
        } else if (auto *mp = dynamic_cast<ExtrusionMultiPath*>(out[snap.entity].get())) {
            if (mp->paths.empty())
                continue;
            Point3 &p = snap.end == 0 ? mp->paths.front().polyline.points.front()
                                      : mp->paths.back().polyline.points.back();
            p = Point3(snap.target.x(), snap.target.y(), p.z());
        }
    }

    std::vector<std::unique_ptr<ExtrusionEntity>> result;
    result.reserve(n);
    for (size_t i = 0; i < n; ++ i) {
        auto &positions = junctions[i];
        if (! flats[i] || positions.empty()) {
            result.emplace_back(std::move(out[i]));
            continue;
        }
        // Sort and deduplicate junction positions.
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end(), [junction_epsilon](coordf_t a, coordf_t b) {
            return b - a <= junction_epsilon;
        }), positions.end());

        FlattenedEntity flat = std::move(*flats[i]);
        if (flat.closed) {
            // Linearize the ring at the first junction; the remaining junctions shift
            // into the rotated frame (the rotated ring has the same total length).
            const coordf_t s0 = positions.front();
            rotate_ring(flat, s0);
            const coordf_t total = arc_prefix(flat.polyline).back();
            std::vector<coordf_t> rest;
            for (size_t k = 1; k < positions.size(); ++ k) {
                const coordf_t shifted = positions[k] - s0;
                rest.push_back(shifted > 0 ? shifted : shifted + total);
            }
            for (const PolylinePiece &piece : split_open(flat, rest))
                emit_piece(piece, flat.paths, result);
        } else {
            for (const PolylinePiece &piece : split_open(flat, positions))
                emit_piece(piece, flat.paths, result);
        }
    }
    return result;
}

// -----------------------------------------------------------------------------------------------
// Preflight
// -----------------------------------------------------------------------------------------------

namespace {

// Straight connector between two chain neighbors, inheriting the extrusion attributes of `ref`.
std::unique_ptr<ExtrusionPath> make_connector_path(const Point &from, const Point &to, const ExtrusionEntity &ref)
{
    const ExtrusionPath *tmpl = nullptr;
    if (const auto *path = dynamic_cast<const ExtrusionPath*>(&ref))
        tmpl = path;
    else if (const auto *mp = dynamic_cast<const ExtrusionMultiPath*>(&ref); mp != nullptr && ! mp->paths.empty())
        tmpl = &mp->paths.front();
    else if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(&ref); loop != nullptr && ! loop->paths.empty())
        tmpl = &loop->paths.front();
    if (tmpl == nullptr)
        return nullptr;
    Polyline3 polyline;
    polyline.append(Point3(from.x(), from.y(), tmpl->polyline.points.front().z()));
    polyline.append(Point3(to.x(),   to.y(),   tmpl->polyline.points.back().z()));
    return std::make_unique<ExtrusionPath>(std::move(polyline), *tmpl);
}

// Visit small surface/gap-fill fragments while passing their nearest point on the main fill.
// Sorting them after the entire surface creates long hops around a curved boundary. Each local
// excursion retains both original paths and their flow/width, with two bounded extruded links.
// The resulting multipath keeps the main fill's endpoints, so wall ordering remains unchanged.
void splice_surface_fragments(std::vector<std::unique_ptr<ExtrusionEntity>> &entities, double max_join, double junction_epsilon)
{
    // A sparse-to-solid transition may contain an almost closed contour (the slicer clips its seam).
    // Restore only that tiny seam, so the contour can be entered near the adjoining sparse fill.
    // Keep these rings out of fragment hosting: they must remain reseamable closed paths.
    const bool has_sparse_fill = std::any_of(entities.begin(), entities.end(), [](const auto &entity) {
        return entity->role() == erInternalInfill;
    });
    for (auto &entity : entities) {
        auto *path = dynamic_cast<ExtrusionPath*>(entity.get());
        if (has_sparse_fill && path != nullptr && ! path->z_contoured && ! path->empty() &&
            is_solid_infill(path->role()) && path->role() != erIroning && path->length() > max_join &&
            (path->first_point() - path->last_point()).cast<double>().norm() <= std::min(max_join, junction_epsilon)) {
            if (path->first_point() != path->last_point())
                path->polyline.append(path->polyline.points.front());
        }
    }
    // A short curved remnant may be longer than the connector budget even though all of it
    // lies next to the main surface. Visit its pieces locally, retaining each original segment.
    double longest_fill = 0.;
    for (const auto &entity : entities)
        if (is_infill(entity->role()) && entity->role() != erIroning && entity->first_point() != entity->last_point())
            longest_fill = std::max(longest_fill, entity->length());
    const size_t original_size = entities.size();
    for (size_t i = 0; i < original_size; ++ i) {
        const auto *path = dynamic_cast<const ExtrusionPath*>(entities[i].get());
        if (path == nullptr || path->z_contoured || ! is_solid_infill(path->role()) || path->role() == erIroning ||
            path->first_point() == path->last_point() || path->length() <= max_join ||
            path->length() > 2 * max_join || path->length() >= longest_fill)
            continue;
        const auto flat = flatten_entity(*path);
        const auto pieces = split_open(*flat, {path->length() * 0.5});
        for (const auto &piece : pieces)
            entities.emplace_back(std::make_unique<ExtrusionPath>(piece.polyline, *path));
        entities[i].reset();
    }
    entities.erase(std::remove(entities.begin(), entities.end(), nullptr), entities.end());
    struct Visit { size_t fragment; coordf_t s; Point point; };
    std::vector<std::vector<Visit>> visits(entities.size());
    for (size_t i = 0; i < entities.size(); ++ i) {
        const auto *fragment = dynamic_cast<const ExtrusionPath*>(entities[i].get());
        if (fragment == nullptr || fragment->z_contoured || fragment->empty() ||
            !(fragment->role() == erGapFill ||
              (is_solid_infill(fragment->role()) && fragment->role() != erIroning && fragment->length() <= max_join)))
            continue;
        size_t best_host = entities.size();
        double best_cost = std::numeric_limits<double>::max();
        Visit best_visit{};
        for (size_t j = 0; j < entities.size(); ++ j) {
            const auto *host = dynamic_cast<const ExtrusionPath*>(entities[j].get());
            if (j == i || host == nullptr || host->z_contoured || ! is_infill(host->role()) ||
                host->role() == erIroning || host->first_point() == host->last_point() ||
                host->length() <= std::max(max_join, fragment->length()))
                continue;
            const auto flat = flatten_entity(*host);
            if (! flat)
                continue;
            const auto prefix = arc_prefix(flat->polyline);
            const Point a = fragment->first_point(), b = fragment->last_point();
            const Point middle(Vec2d((a.cast<double>() + b.cast<double>()) * 0.5));
            for (const Point &target : {a, b, middle}) {
                coordf_t ignored = 0.;
                const coordf_t pos = closest_s_on_polyline(flat->polyline, prefix, target, ignored);
                size_t src = 0;
                const Point contact = point_at_s(*flat, prefix, pos, src).to_point();
                const double da = (a - contact).cast<double>().norm();
                const double db = (b - contact).cast<double>().norm();
                if (da <= max_join && db <= max_join && da + db < best_cost) {
                    best_cost = da + db;
                    best_host = j;
                    best_visit = {i, pos, contact};
                }
            }
        }
        if (best_host != entities.size())
            visits[best_host].push_back(best_visit);
    }
    for (size_t i = 0; i < entities.size(); ++ i) {
        auto &route = visits[i];
        if (route.empty())
            continue;
        std::stable_sort(route.begin(), route.end(), [](const Visit &a, const Visit &b) { return a.s < b.s; });
        const auto &host = *static_cast<const ExtrusionPath*>(entities[i].get());
        std::vector<coordf_t> cuts;
        for (const Visit &visit : route)
            cuts.push_back(visit.s);
        const auto pieces = split_open(*flatten_entity(host), cuts);
        auto joined = std::make_unique<ExtrusionMultiPath>();
        for (size_t k = 0; k < pieces.size(); ++ k) {
            if (pieces[k].polyline.length() > 0.)
                joined->paths.emplace_back(pieces[k].polyline, host);
            if (k == route.size())
                break;
            const Visit &visit = route[k];
            const auto &fragment = *static_cast<const ExtrusionPath*>(entities[visit.fragment].get());
            if (visit.point != fragment.first_point())
                joined->paths.push_back(*make_connector_path(visit.point, fragment.first_point(), host));
            joined->paths.push_back(fragment);
            if (fragment.last_point() != visit.point)
                joined->paths.push_back(*make_connector_path(fragment.last_point(), visit.point, host));
        }
        if (std::any_of(joined->paths.begin(), joined->paths.end(), [](const ExtrusionPath &path) { return ! path.can_reverse(); }))
            joined->set_reverse();
        entities[i] = std::move(joined);
    }
    // Release fragments only after every host has been constructed from the original entities.
    for (const auto &route : visits)
        for (const Visit &visit : route)
            entities[visit.fragment].reset();
    entities.erase(std::remove(entities.begin(), entities.end(), nullptr), entities.end());
}

// Greedy single-trace ordering. The chain starts near `preferred_start` (or at the first entity) and
// repeatedly consumes the unused entity reachable with the shortest hop, inserting a straight
// connector for each non-coincident hop (bounded by `max_join`). This is what joins wall
// loops to each other and to fill/support: a short extruded link, never a long bridge.
// Returns false when the shortest remaining hop exceeds `max_join` (layer not printable as one trace).
bool build_chain_with_connectors(std::vector<std::unique_ptr<ExtrusionEntity>> &&input,
                                 const Point *preferred_start,
                                 double max_join,
                                 std::vector<std::unique_ptr<ExtrusionEntity>> &out)
{
    std::vector<std::unique_ptr<ExtrusionEntity>> pool = std::move(input);
    const size_t n = pool.size();
    if (n == 0)
        return false;

    auto dist = [](const Point &a, const Point &b) {
        return (a.cast<double>() - b.cast<double>()).norm();
    };
    auto is_wall_role = [](ExtrusionRole role) {
        return role == erExternalPerimeter || role == erPerimeter || role == erOverhangPerimeter;
    };

    // Project onto segments, not only stored vertices: fill endpoints usually meet mid-edge.
    auto nearest_point = [&](const ExtrusionEntity &entity, const Point &p, double &out_dist) {
        const Point nearest = p.projection_onto(entity.as_polyline());
        out_dist = dist(nearest, p);
        return nearest;
    };

    // Rotate a closed path so it starts and ends at the point nearest to `target` (a vertex is
    // inserted there, so the seam is exact even mid-edge).
    auto reseam_closed = [&](ExtrusionPath &path, const Point &target) -> double {
        auto &points = path.polyline.points;
        const size_t m = points.size() >= 2 ? points.size() - 1 : 0; // points[m] == points[0]
        if (m < 3)
            return 0.;
        const Vec2d tp = Vec2d(double(target.x()), double(target.y()));
        double best_d = std::numeric_limits<double>::max();
        size_t best_seg = 0;
        double best_t   = 0.;
        auto v2 = [](const Point3 &p) { return Vec2d(double(p.x()), double(p.y())); };
        for (size_t i = 0; i < m; ++ i) {
            const Vec2d a = v2(points[i]);
            const Vec2d b = v2(points[(i + 1) % m]);
            const Vec2d ab = b - a;
            const double len2 = ab.squaredNorm();
            const double t = len2 > 0. ? std::clamp((tp - a).dot(ab) / len2, 0., 1.) : 0.;
            const double d = (tp - (a + ab * t)).norm();
            if (d < best_d) { best_d = d; best_seg = i; best_t = t; }
        }
        const Vec2d a = v2(points[best_seg]);
        const Vec2d b = v2(points[(best_seg + 1) % m]);
        const Vec2d s = a + (b - a) * best_t;
        const Point3 seam(s.x(), s.y(), double(points[best_seg].z()));
        Polyline3 rotated;
        rotated.append(seam);
        for (size_t k = 1; k <= m; ++ k)
            rotated.append(points[(best_seg + k) % m]);
        rotated.append(seam);
        path.polyline = std::move(rotated);
        return best_d;
    };

    // Group entities by role; walls are ordered outer-to-inner (by contour area).
    std::vector<size_t> walls, fills;
    for (size_t i = 0; i < n; ++ i)
        (is_wall_role(pool[i]->role()) ? walls : fills).push_back(i);
    const bool cp_debug = std::getenv("CP_DEBUG") != nullptr;
    if (cp_debug) {
        std::cerr << "[CP] entities=" << n << " walls=" << walls.size() << " fills=" << fills.size() << "\n";
        for (size_t i = 0; i < n; ++ i)
            std::cerr << "  e" << i << " role=" << int(pool[i]->role())
                      << " closed=" << (pool[i]->first_point() == pool[i]->last_point())
                      << " pts=" << pool[i]->as_polyline().points.size() << "\n";
    }
    auto abs_area = [&](size_t i) { return std::abs(Slic3r::area(pool[i]->as_polyline().points)); };
    std::sort(walls.begin(), walls.end(), [&](size_t a, size_t b) { return abs_area(a) > abs_area(b); });

    // Start at whichever group is closest to the previous layer's end. This alternates
    // wall-first / fill-first between layers (outer->inner->fill, then fill->inner->outer) and keeps
    // the inter-layer weld short.
    // Force every wall loop's seam onto the fill connection point, propagated outwards. The innermost
    // wall then ends exactly where the fill starts, so top/bottom surfaces and internal solid infill
    // (single traces at a fixed angular position) join the walls instead of being rejected just
    // because the chain happened to arrive on the opposite side of the layer.
    std::vector<Point> forced_seam(pool.size(), Point(0, 0));
    std::vector<char>  has_forced(pool.size(), 0);
    const bool has_fill = ! fills.empty();
    Point      fill_entry;
    bool       fill_entry_valid = false;
    if (has_fill && ! walls.empty()) {
        double best = std::numeric_limits<double>::max();
        for (size_t fi : fills) {
            if (pool[fi]->first_point() == pool[fi]->last_point())
                continue; // a closed fill trace has no entry/exit to align to
            for (const Point &endpoint : {pool[fi]->first_point(), pool[fi]->last_point()}) {
                double gap = 0.;
                nearest_point(*pool[walls.back()], endpoint, gap);
                if (gap > max_join)
                    continue;
                // Choose a wall-first entry that also keeps the layer transition short.
                // Always picking the same fill end can strand the next layer on the far side.
                double score = gap;
                if (preferred_start != nullptr) {
                    double ignored = 0.;
                    const Point outer = nearest_point(*pool[walls.front()], endpoint, ignored);
                    score = dist(outer, *preferred_start);
                }
                if (score < best) {
                    best = score;
                    fill_entry = endpoint;
                    fill_entry_valid = true;
                }
            }
        }
        if (fill_entry_valid) {
            Point target = fill_entry;
            for (size_t k = walls.size(); k -- > 0; ) {
                ExtrusionEntity &wall = *pool[walls[k]];
                if (wall.first_point() == wall.last_point()) {
                    if (auto *path = dynamic_cast<ExtrusionPath*>(&wall)) {
                        reseam_closed(*path, target);
                        forced_seam[walls[k]] = wall.first_point();
                        has_forced[walls[k]]  = 1;
                        target = wall.first_point();
                        continue;
                    }
                }
                double d;
                target = nearest_point(wall, target, d);
            }
        }
    }

    // Order fill traces by proximity: internal solid infill is split into 2-3 pieces which must be
    // traversed in one sweep with short connectors.
    auto order_fills = [&](const Point &from) {
        std::vector<size_t> remaining = fills, ordered;
        Point cursor = from;
        while (! remaining.empty()) {
            size_t best_k = 0;
            double best_d = std::numeric_limits<double>::max();
            for (size_t k = 0; k < remaining.size(); ++ k) {
                const ExtrusionEntity &entity = *pool[remaining[k]];
                double d = 0.;
                if (entity.first_point() == entity.last_point())
                    nearest_point(entity, cursor, d);
                else
                    d = std::min(dist(entity.first_point(), cursor), dist(entity.last_point(), cursor));
                if (d < best_d) { best_d = d; best_k = k; }
            }
            const size_t idx = remaining[best_k];
            ordered.push_back(idx);
            if (pool[idx]->first_point() == pool[idx]->last_point()) {
                double ignored = 0.;
                cursor = nearest_point(*pool[idx], cursor, ignored);
            } else
                cursor = dist(pool[idx]->first_point(), cursor) <= dist(pool[idx]->last_point(), cursor)
                    ? pool[idx]->last_point() : pool[idx]->first_point();
            remaining.erase(remaining.begin() + best_k);
        }
        return ordered;
    };

    // Start the layer where the previous one ended; with the seams aligned to the fill this alternates
    // wall-first / fill-first between layers.
    bool walls_first = true;
    if (preferred_start != nullptr && has_fill && ! walls.empty()) {
        const Point wall_start = has_forced[walls.front()] ? forced_seam[walls.front()] : pool[walls.front()]->first_point();
        const double d_wall = dist(wall_start, *preferred_start);
        double d_fill = std::numeric_limits<double>::max();
        for (size_t fi : fills)
            d_fill = std::min(d_fill, std::min(dist(pool[fi]->first_point(), *preferred_start),
                                               dist(pool[fi]->last_point(),  *preferred_start)));
        walls_first = d_wall <= d_fill;
    }

    std::vector<size_t> sequence;
    if (walls_first) {
        sequence = walls;                                                       // outer -> inner
        if (has_fill)
            for (size_t idx : order_fills(fill_entry_valid ? fill_entry :
                     (! walls.empty() ? pool[walls.back()]->last_point() :
                      (preferred_start != nullptr ? *preferred_start : pool[fills.front()]->first_point()))))
                sequence.push_back(idx);
    } else {
        if (has_fill)
            for (size_t idx : order_fills(preferred_start != nullptr ? *preferred_start : fill_entry))
                sequence.push_back(idx);
        sequence.insert(sequence.end(), walls.rbegin(), walls.rend());          // inner -> outer
    }

    Point cur;
    bool  have_cur = false;
    // The previously placed entity is a wall that may be extended into the next part (see below).
    bool  prev_wall_extendable = false;
    for (size_t idx : sequence) {
        ExtrusionEntity &entity = *pool[idx];
        const bool closed = entity.first_point() == entity.last_point();
        double hop = 0.;
        if (have_cur) {
            if (closed) {
                if (auto *path = dynamic_cast<ExtrusionPath*>(&entity)) {
                    // Wall-first chains must meet the fill entry. Fill-first chains must instead
                    // join at the actual fill exit, then propagate that seam outwards.
                    if (has_forced[idx] && walls_first) {
                        reseam_closed(*path, forced_seam[idx]);
                        hop = dist(entity.first_point(), cur);
                    } else
                        hop = reseam_closed(*path, cur);
                } else {
                    double d;
                    nearest_point(entity, cur, d);
                    hop = d;
                }
            } else {
                if (entity.can_reverse() && dist(entity.last_point(), cur) < dist(entity.first_point(), cur))
                    entity.reverse();
                hop = dist(entity.first_point(), cur);
            }
        } else if (preferred_start != nullptr) {
            if (closed) {
                if (auto *path = dynamic_cast<ExtrusionPath*>(&entity))
                    reseam_closed(*path, has_forced[idx] ? forced_seam[idx] : *preferred_start);
            } else if (entity.can_reverse() && dist(entity.last_point(), *preferred_start) < dist(entity.first_point(), *preferred_start)) {
                entity.reverse();
            }
        }
        if (have_cur && hop > max_join) {
            if (cp_debug)
                std::cerr << "[CP] FAIL hop " << unscale_(hop) << "mm at entity " << idx
                          << " (max " << unscale_(max_join) << ")\n";
            return false;
        }
        // Wall -> surface/fill: instead of a separate straight connector, extend the wall itself up to
        // the pattern start ("an incomplete wall segment"), so the junction reads as the wall
        // continuing into the top/bottom surface.
        bool extended_wall = false;
        if (have_cur && hop > SCALED_EPSILON && prev_wall_extendable && ! is_wall_role(entity.role())) {
            if (auto *prev_path = dynamic_cast<ExtrusionPath*>(out.back().get()); prev_path != nullptr && ! prev_path->polyline.points.empty()) {
                const Point  p     = entity.first_point();
                const Point3 &last = prev_path->polyline.points.back();
                const Point3  ext(p.x(), p.y(), last.z());
                if (last != ext)
                    prev_path->polyline.append(ext);
                extended_wall = true;
            }
        }
        if (have_cur && hop > SCALED_EPSILON && ! extended_wall) {
            auto connector = make_connector_path(cur, entity.first_point(), entity);
            if (connector == nullptr)
                return false;
            out.emplace_back(std::move(connector));
        }
        cur = entity.last_point();
        have_cur = true;
        prev_wall_extendable = is_wall_role(entity.role()) && ! extended_wall;
        out.emplace_back(std::move(pool[idx]));
    }
    return true;
}

// Shorter of the two routes on a closed contour, including projected endpoints.
Polyline contour_section(const Polygon &contour, const Point &from, const Point &to)
{
    struct Contact { size_t segment; Point point; double t; };
    const auto &points = contour.points;
    auto contact = [&](const Point &p) {
        Contact best{};
        double nearest = std::numeric_limits<double>::max();
        for (size_t i = 0; i < points.size(); ++ i) {
            const Point q = p.projection_onto(Line(points[i], points[(i + 1) % points.size()]));
            const double d = (q - p).cast<double>().squaredNorm();
            if (d < nearest) {
                nearest = d;
                best = {i, q, (q - points[i]).cast<double>().squaredNorm()};
            }
        }
        return best;
    };
    auto forward = [&](const Contact &a, const Contact &b) {
        Polyline result;
        result.append(a.point);
        if (a.segment != b.segment || a.t > b.t) {
            size_t i = a.segment;
            do {
                i = (i + 1) % points.size();
                result.append(points[i]);
            } while (i != b.segment);
        }
        result.append(b.point);
        return result;
    };
    const Contact a = contact(from), b = contact(to);
    Polyline ab = forward(a, b), ba = forward(b, a);
    if (ab.length() <= ba.length()) return ab;
    ba.reverse();
    return ba;
}

// Clip individual scan segments and move their boundary turnarounds onto the inset contour.
// This keeps each monotonic trace connected while leaving the extension's bead footprint clear.
// Gap fill fully replaced by the extension is removed; material outside that band is retained.
bool inset_surface_paths(std::vector<std::unique_ptr<ExtrusionEntity>> &fills, const Polygon &allowed, bool preserve_chain = false)
{
    const Polyline border = allowed.split_at_first_point();
    for (auto &entity : fills) {
        auto *path = dynamic_cast<ExtrusionPath*>(entity.get());
        if (path == nullptr || path->z_contoured)
            return false;
        const Polyline original = path->as_polyline();
        if (! preserve_chain && intersection_pl(original, Polygons{allowed}).empty()) {
            entity.reset();
            continue;
        }
        Polyline result;
        auto append = [&](const Point &p) {
            if (result.points.empty() || result.last_point() != p)
                result.append(p);
        };
        auto on_body = [&](const Point &p) { return allowed.contains(p) ? p : p.projection_onto(border); };
        auto along_border = [&](const Point &p) {
            if (! result.points.empty() && result.last_point() != p) {
                const Polyline route = contour_section(allowed, result.last_point(), p);
                for (const Point &q : route.points)
                    append(q);
            }
            append(p);
        };
        append(on_body(original.first_point()));
        for (size_t k = 1; k < original.points.size(); ++ k) {
            const Point &a = original.points[k - 1], &b = original.points[k];
            auto pieces = intersection_pl(Polyline(Points{a, b}), Polygons{allowed});
            for (auto &piece : pieces)
                if ((piece.last_point() - a).cast<double>().squaredNorm() < (piece.first_point() - a).cast<double>().squaredNorm())
                    piece.reverse();
            std::sort(pieces.begin(), pieces.end(), [&](const Polyline &x, const Polyline &y) {
                return (x.first_point() - a).cast<double>().squaredNorm() < (y.first_point() - a).cast<double>().squaredNorm();
            });
            for (const auto &piece : pieces) {
                along_border(piece.first_point());
                for (const Point &p : piece.points)
                    append(p);
            }
            along_border(on_body(b));
        }
        if (result.points.size() < 2)
            entity.reset();
        else
            path->polyline = Polyline3(result);
    }
    fills.erase(std::remove(fills.begin(), fills.end(), nullptr), fills.end());
    return ! fills.empty();
}

// Solid layers need a contour route, not the sparse layer's endpoint-distance heuristic.
// Start on the wall actually underneath the nozzle, then print successive nested walls.
// Reserve a local band for an additional inner-wall arc before contracting the surface into it.
bool build_solid_surface_chain(std::vector<std::unique_ptr<ExtrusionEntity>> input,
                               const Point *preferred, const Point *preserved_end, bool connect_previous, double epsilon, double max_join,
                               std::vector<std::unique_ptr<ExtrusionEntity>> &out)
{
    std::vector<std::unique_ptr<ExtrusionEntity>> walls, fills;
    for (auto &entity : input) {
        const auto *path = dynamic_cast<const ExtrusionPath*>(entity.get());
        if (path == nullptr || path->z_contoured || path->empty())
            return false;
        const auto role = path->role();
        if (role == erPerimeter || role == erExternalPerimeter || role == erOverhangPerimeter) {
            if (path->first_point() != path->last_point())
                return false;
            walls.emplace_back(std::move(entity));
        } else
            fills.emplace_back(std::move(entity));
    }
    if (walls.empty() || fills.empty())
        return false;
    std::sort(walls.begin(), walls.end(), [](const auto &a, const auto &b) {
        return std::abs(area(a->as_polyline().points)) > std::abs(area(b->as_polyline().points));
    });
    const auto &inner = *static_cast<const ExtrusionPath*>(walls.back().get());
    Polygon contour(inner.as_polyline().points);
    contour.points.pop_back();
    contour.make_counter_clockwise();
    const auto guide_polygons = offset(contour, -scale_(Flow::rounded_rectangle_extrusion_spacing(inner.width, inner.height)));
    if (guide_polygons.size() != 1)
        return false;
    const Polygon &guide = guide_polygons.front();
    const Point start = preferred != nullptr ? *preferred : walls.front()->first_point();
    auto distance = [](const Point &a, const Point &b) { return (a - b).cast<double>().norm(); };
    auto distance_to = [&](const ExtrusionEntity &entity) {
        return distance(start, start.projection_onto(entity.as_polyline()));
    };
    const double wall_distance = distance_to(*walls.front());
    double fill_distance = std::numeric_limits<double>::max();
    for (const auto &fill : fills) fill_distance = std::min(fill_distance, distance_to(*fill));
    const bool walls_first = preferred == nullptr || wall_distance <= fill_distance;
    if (std::getenv("CP_DEBUG") != nullptr)
        std::cerr << "[CP solid begin] walls=" << walls.size() << " walls_first=" << walls_first << "\n";

    auto clone_all = [](const auto &entities) {
        std::vector<std::unique_ptr<ExtrusionEntity>> copies;
        for (const auto &entity : entities) copies.emplace_back(entity->clone());
        return copies;
    };
    auto order_fill = [&](const auto &entities, const Point &from, auto &ordered) {
        auto attempt = [&](const auto &paths, const Point &seed) {
            ordered.clear();
            return build_chain_with_connectors(clone_all(paths), &seed, max_join, ordered);
        };
        if (attempt(entities, from))
            return true;
        auto local = clone_all(entities);
        splice_surface_fragments(local, max_join, epsilon);
        if (attempt(local, from)) return true;
        // The nearest trace may be the middle of a valid chain. Try its other possible
        // entrances before declaring the surface disconnected; the contour arc reaches
        // the selected entrance without changing the short-link budget inside the fill.
        Points seeds;
        for (const auto &path : entities) {
            seeds.push_back(path->first_point());
            seeds.push_back(path->last_point());
        }
        std::stable_sort(seeds.begin(), seeds.end(), [&](const Point &a, const Point &b) {
            return distance(a, from) < distance(b, from);
        });
        for (const Point &seed : seeds)
            if (attempt(entities, seed) || attempt(local, seed)) return true;
        return false;
    };
    std::vector<std::unique_ptr<ExtrusionEntity>> fill_order;
    if (! order_fill(fills, start, fill_order))
        return false;
    const Point entry = fill_order.front()->first_point();
    Point cursor = start;
    bool have_cursor = connect_previous && preferred != nullptr;
    auto emit = [&](std::unique_ptr<ExtrusionEntity> entity, const ExtrusionEntity &link_flow) {
        if (have_cursor && cursor != entity->first_point()) {
            if (distance(cursor, entity->first_point()) > max_join) {
                if (std::getenv("CP_DEBUG") != nullptr)
                    std::cerr << "[CP solid] FAIL join " << unscale_(distance(cursor, entity->first_point())) << "mm\n";
                return false;
            }
            out.emplace_back(make_connector_path(cursor, entity->first_point(), link_flow));
        }
        cursor = entity->last_point();
        have_cursor = true;
        out.emplace_back(std::move(entity));
        return true;
    };
    auto emit_wall = [&](size_t i) {
        auto path = std::unique_ptr<ExtrusionPath>(static_cast<ExtrusionPath*>(walls[i]->clone()));
        auto flat = *flatten_entity(*path);
        flat.closed = true;
        const auto prefix = arc_prefix(flat.polyline);
        double ignored = 0.;
        rotate_ring(flat, closest_s_on_polyline(flat.polyline, prefix, cursor, ignored));
        path->polyline = std::move(flat.polyline);
        return emit(std::move(path), *walls[i]);
    };
    if (walls_first)
        for (size_t i = 0; i < walls.size(); ++ i)
            if (! emit_wall(i)) return false;

    const Polyline extension = contour_section(guide, cursor, entry);
    Polyline exit_extension;
    if (walls_first && preserved_end != nullptr)
        exit_extension = contour_section(guide, fill_order.back()->last_point(), *preserved_end);
    // The existing wall and this extra arc use adjacent bead spacings. The infill centreline
    // must remain at least half of each bead's width away from the new arc centreline.
    double fill_width = 0.;
    for (const auto &fill : fills)
        fill_width = std::max(fill_width, double(static_cast<const ExtrusionPath*>(fill.get())->width));
    Polylines reserved;
    if (extension.length() > SCALED_EPSILON) reserved.push_back(extension);
    if (exit_extension.length() > SCALED_EPSILON) reserved.push_back(exit_extension);
    if (! reserved.empty()) {
        const auto band = offset(reserved, scale_(0.5 * (inner.width + fill_width)),
                                 ClipperLib::jtRound, scale_(0.0001), ClipperLib::etOpenRound);
        auto remaining = diff_ex(Polygons{contour}, band);
        // Offsetting a sharp corner can leave a tiny pocket containing no original infill.
        // Ignore only such empty pockets; disconnected regions carrying material still reject.
        remaining.erase(std::remove_if(remaining.begin(), remaining.end(), [&](const ExPolygon &region) {
            return std::none_of(fills.begin(), fills.end(), [&](const auto &fill) {
                return ! intersection_pl(fill->as_polyline(), region).empty();
            });
        }), remaining.end());
        if (remaining.size() != 1 || ! remaining.front().holes.empty() ||
            ! inset_surface_paths(fills, remaining.front().contour)) {
            if (std::getenv("CP_DEBUG") != nullptr) std::cerr << "[CP solid] FAIL inset regions=" << remaining.size() << "\n";
            return false;
        }
        fill_order.clear();
        if (! order_fill(fills, entry, fill_order))
            return false;
        // Local gap-fill links may cross a concave end of the reserved band even when
        // their endpoints are legal. Route these links on the inset boundary as well.
        std::vector<std::unique_ptr<ExtrusionEntity>> connected;
        for (const auto &fill : fill_order) {
            if (const auto *multi = dynamic_cast<const ExtrusionMultiPath*>(fill.get()))
                for (const auto &path : multi->paths) connected.emplace_back(path.clone());
            else
                connected.emplace_back(fill->clone());
        }
        // The legacy ordering ignores sub-epsilon joins. Make those contacts exact before
        // projecting both sides onto the same inset boundary.
        for (size_t i = 1; i < connected.size(); ++ i) {
            const Point previous = connected[i - 1]->last_point();
            auto *path = static_cast<ExtrusionPath*>(connected[i].get());
            if (distance(previous, path->first_point()) > SCALED_EPSILON) return false;
            path->polyline.points.front().x() = previous.x();
            path->polyline.points.front().y() = previous.y();
        }
        if (! inset_surface_paths(connected, remaining.front().contour, true)) return false;
        fill_order = std::move(connected);
        const auto tolerance_region = offset(remaining.front().contour, float(scale_(0.002)));
        for (size_t i = 0; i < fill_order.size(); ++ i) {
            if (i > 0 && fill_order[i-1]->last_point() != fill_order[i]->first_point()) {
                if (std::getenv("CP_DEBUG") != nullptr) std::cerr << "[CP solid] FAIL inset continuity\n";
                return false;
            }
            if (! diff_pl(fill_order[i]->as_polyline(), tolerance_region).empty()) {
                if (std::getenv("CP_DEBUG") != nullptr) std::cerr << "[CP solid] FAIL inset containment\n";
                return false;
            }
        }
        if (extension.length() > SCALED_EPSILON &&
            ! emit(std::make_unique<ExtrusionPath>(Polyline3(extension), inner), inner))
            return false;
    }
    for (auto &fill : fill_order)
        if (! emit(std::move(fill), inner)) return false;
    if (! walls_first)
        for (size_t i = walls.size(); i -- > 0; )
            if (! emit_wall(i)) return false;
    // Retain the established handoff into the next sparse layer. Only this solid layer
    // closes the distance, along its contour and successive wall spacings, so the sparse
    // planner receives exactly the same endpoint and keeps its existing toolpath.
    if (preserved_end != nullptr && cursor != *preserved_end) {
        if (walls_first) {
            if (exit_extension.length() > SCALED_EPSILON &&
                ! emit(std::make_unique<ExtrusionPath>(Polyline3(exit_extension), inner), inner))
                return false;
            if (! contour.contains(*preserved_end))
                for (size_t i = walls.size(); i -- > 0; ) {
                    const Point p = preserved_end->projection_onto(walls[i]->as_polyline());
                    if (distance(cursor, p) > max_join) return false;
                    if (cursor != p && ! emit(make_connector_path(cursor, p, *walls[i]), *walls[i])) return false;
                    Polygon wall_polygon(walls[i]->as_polyline().points);
                    if (wall_polygon.contains(*preserved_end)) break;
                }
        } else {
            Polygon outer(walls.front()->as_polyline().points);
            outer.points.pop_back();
            const auto tail = contour_section(outer, cursor, *preserved_end);
            if (tail.length() > SCALED_EPSILON &&
                ! emit(std::make_unique<ExtrusionPath>(Polyline3(tail), *static_cast<const ExtrusionPath*>(walls.front().get())), *walls.front()))
                return false;
        }
        if (distance(cursor, *preserved_end) > max_join) return false;
        if (cursor != *preserved_end && ! emit(make_connector_path(cursor, *preserved_end, inner), inner)) return false;
    }
    if (std::getenv("CP_DEBUG") != nullptr)
        std::cerr << "[CP solid] walls_first=" << walls_first << " arc=" << unscale_(extension.length())
                  << "mm start_gap=" << (preferred ? unscale_(distance(out.front()->first_point(), *preferred)) : 0.) << "mm\n";
    return true;
}

} // namespace

ContinuousPrintVerdict preflight_layer(
    const std::vector<ExtrusionEntity*> &entities,
    [[maybe_unused]] const Layer        *layer,
    [[maybe_unused]] const PrintConfig  &cfg,
    ContinuousLayerPlan                 *out_plan,
    const Point                         *preferred_start,
    double                               junction_epsilon,
    double                               max_join_distance)
{
    assert(out_plan != nullptr);

    // Flatten collections into leaf entities first, so callers may pass raw region entity lists.
    std::vector<ExtrusionEntity*> leaves;
    flatten_extrusion_entities(entities, leaves);
    if (leaves.empty())
        return ContinuousPrintVerdict::Reject;

    // With bounded connectors, preserve whole contours: splitting a wall at both fill endpoints
    // turns it into unrelated open pieces and destroys the outer-to-inner wall ordering.
    // Strict mode still needs junction splitting to find natural contacts without adding geometry.
    std::vector<std::unique_ptr<ExtrusionEntity>> working;
    if (max_join_distance > 0.) {
        for (const ExtrusionEntity *entity : leaves)
            working.emplace_back(entity->clone());
    } else {
        working = split_entities_at_junctions(leaves, junction_epsilon);
    }

    // Linearize remaining closed loops into open paths, so the chain (and the emitter) only deals
    // with ExtrusionPath. A loop that was split at a junction is already a path at this point.
    std::vector<std::unique_ptr<ExtrusionEntity>> linearized;
    linearized.reserve(working.size());
    for (auto &entity : working) {
        if (const auto *loop = dynamic_cast<const ExtrusionLoop*>(entity.get()))
            linearize_loop_into(*loop, preferred_start, linearized);
        else
            linearized.emplace_back(std::move(entity));
    }
    working = std::move(linearized);

    const bool solid_surface = cfg.continuous_print_mode.value && max_join_distance > 0. &&
        std::any_of(working.begin(), working.end(), [](const auto &e) { return is_solid_infill(e->role()); }) &&
        std::none_of(working.begin(), working.end(), [](const auto &e) { return e->role() == erInternalInfill || e->role() == erIroning; }) &&
        std::any_of(working.begin(), working.end(), [](const auto &e) { return e->role() == erExternalPerimeter; });
    std::vector<std::unique_ptr<ExtrusionEntity>> ordered;
    if (solid_surface) {
        Point preserved_end;
        const Point *handoff = nullptr;
        if (layer != nullptr && layer->upper_layer != nullptr) {
            std::vector<ExtrusionEntity*> next_fills;
            for (const LayerRegion *region : layer->upper_layer->regions())
                flatten_extrusion_entities(region->fills.entities, next_fills);
            if (std::any_of(next_fills.begin(), next_fills.end(), [](const auto *e) { return e->role() == erInternalInfill; })) {
                PrintConfig legacy_config = cfg;
                legacy_config.continuous_print_mode.value = false;
                ContinuousLayerPlan legacy_plan;
                if (preflight_layer(entities, nullptr, legacy_config, &legacy_plan, preferred_start,
                                    junction_epsilon, max_join_distance) == ContinuousPrintVerdict::Applicable) {
                    preserved_end = legacy_plan.end_point;
                    handoff = &preserved_end;
                }
            }
        }
        if (! build_solid_surface_chain(std::move(working), preferred_start, handoff, layer != nullptr && layer->id() > 0,
                                       junction_epsilon, max_join_distance, ordered))
            return ContinuousPrintVerdict::Reject;
    } else {

        // Single-trace ordering. Exact contacts are used as-is; short gaps (wall-wall, wall-fill,
        // wall-support) get a straight connector bounded by max_join_distance. Longer hops fail the layer.
        // Keep an already valid whole-path order. Local fragment visits are a second attempt for
        // disconnected surface layers; eagerly splicing can strand otherwise reachable fill endpoints.
        std::vector<std::unique_ptr<ExtrusionEntity>> retry;
        if (max_join_distance > 0.)
            for (const auto &entity : working)
                retry.emplace_back(entity->clone());
        if (! build_chain_with_connectors(std::move(working), preferred_start, max_join_distance, ordered)) {
            if (max_join_distance <= 0.)
                return ContinuousPrintVerdict::Reject;
            ordered.clear();
            splice_surface_fragments(retry, max_join_distance, junction_epsilon);
            if (! build_chain_with_connectors(std::move(retry), preferred_start, max_join_distance, ordered))
                return ContinuousPrintVerdict::Reject;
        }
    }

    ContinuousLayerPlan plan;
    plan.start_point = ordered.front()->first_point();
    plan.end_point   = ordered.back()->last_point();
    plan.is_closed   = is_approx(plan.start_point, plan.end_point);
    plan.entities    = std::move(ordered);
    plan.order.reserve(plan.entities.size());
    for (size_t i = 0; i < plan.entities.size(); ++ i)
        plan.order.emplace_back(i, false);
    for (const auto &entity : plan.entities)
        plan.total_length += unscale_(entity->length()); // ExtrusionEntity::length() is in scaled units

    // XY samples along the chain, interpolated at a fixed arc-length step (plus the exact start/end points).
    Points polyline;
    for (const auto &entity : plan.entities)
        append_entity_polyline(*entity, false, polyline);
    if (! polyline.empty()) {
        const coordf_t step = scale_(CONTINUOUS_PRINT_SAMPLING_STEP);
        plan.sampling.push_back(polyline.front());
        coordf_t s_start     = 0;    // absolute arc length at the start of the current segment
        coordf_t next_sample = step; // absolute arc length where the next sample is emitted
        for (size_t i = 1; i < polyline.size(); ++ i) {
            const Vec2d    prev    = polyline[i - 1].cast<coordf_t>();
            const Vec2d    curr    = polyline[i].cast<coordf_t>();
            const coordf_t seg_len = (curr - prev).norm();
            const coordf_t s_end   = s_start + seg_len;
            while (seg_len > 0 && next_sample <= s_end) {
                const Vec2d p = prev + (curr - prev) * ((next_sample - s_start) / seg_len);
                plan.sampling.push_back(Point(p.x(), p.y())); // Point(double, double) rounds to scaled coords
                next_sample += step;
            }
            s_start = s_end;
        }
        if (! is_approx(plan.sampling.back(), polyline.back()))
            plan.sampling.push_back(polyline.back());
    }

    *out_plan = std::move(plan);
    return ContinuousPrintVerdict::Applicable;
}

// -----------------------------------------------------------------------------------------------
// ContinuousPrint filter (M2 prototype)
// -----------------------------------------------------------------------------------------------

ContinuousPrint::ContinuousPrint(const PrintConfig &config) : m_config(config)
{
    m_reader.z() = (float) m_config.z_offset;
    m_reader.apply_config(m_config);
    // Reuse the spiral smoothing switch; the budget is set via set_max_xy_smoothing
    // from spiral_mode_max_xy_smoothing at the pipeline assembly site (M3).
    m_smooth = config.spiral_mode_smooth;
}

// Adapted from SpiralVase::process_layer (GCode/SpiralVase.cpp), generalized from
// "single closed perimeter loop" to "any single continuous extrusion chain (open or closed)".
// Kept as a separate implementation because M3 will diverge (open-chain transition-point
// enforcement, off-body checks); the M2 equivalence test guards behavioral parity on closed loops.
std::string ContinuousPrint::process_layer(const std::string &gcode, [[maybe_unused]] bool last_layer)
{
    /*  Assumptions (same style as SpiralVase):
        - all layers are processed through it, including those that are not supposed
          to be transformed, in order to update the reader with the XY positions
        - each call to this method includes a full layer, with a single Z move
          at the beginning
        - each layer is emitted as a single continuous extrusion chain (open or closed),
          i.e. the emitter (M3) produced no intra-layer travel; any remaining travel lines
          are filtered out here as a safety net  */

    // If we're not going to modify G-code, just feed it to the reader
    // in order to update positions.
    if (! m_enabled) {
        m_reader.parse_buffer(gcode);
        return gcode;
    }

    // Get total XY length for this layer by summing all extrusion moves.
    float total_layer_length = 0;
    float layer_height = 0;
    float z = 0.f;

    {
        GCodeReader r = m_reader; // clone
        bool set_z = false;
        r.parse_buffer(gcode, [&total_layer_length, &layer_height, &z, &set_z]
            (GCodeReader &reader, const GCodeReader::GCodeLine &line) {
            if (line.cmd_is("G1")) {
                if (line.extruding(reader)) {
                    total_layer_length += line.dist_XY(reader);
                } else if (line.has(Z)) {
                    layer_height += line.dist_Z(reader);
                    if (! set_z) {
                        z = line.new_Z(reader);
                        set_z = true;
                    }
                }
            }
        });
    }

    // Remove layer height from initial Z.
    z -= layer_height;

    std::vector<SpiralVase::SpiralPoint> *current_layer  = new std::vector<SpiralVase::SpiralPoint>();
    std::vector<SpiralVase::SpiralPoint> *previous_layer = m_previous_layer;

    bool smooth = m_smooth;
    std::string new_gcode;
    float max_xy_dist_for_smoothing = m_max_xy_smoothing;
    // Transition tapering works reliably with relative extruder distances only (same as SpiralVase).
    bool transition_in  = m_transition_layer && m_config.use_relative_e_distances.value;
    // A surface chain need not be closed. Replaying it for the vase finishing taper
    // would jump back to its start and print the surface twice. Stop at its endpoint.

    float starting_flowrate  = float(m_config.spiral_starting_flow_ratio.value);
    const float min_segment_length = std::max(float(EPSILON), 2 * float(m_config.resolution.value));

    float len = 0.f;
    SpiralVase::SpiralPoint last_point = previous_layer != nullptr && ! previous_layer->empty() ?
        previous_layer->back() : SpiralVase::SpiralPoint(0, 0);
    m_reader.parse_buffer(gcode, [&new_gcode, &z, total_layer_length, layer_height, transition_in, &len, &current_layer, &previous_layer, smooth, &max_xy_dist_for_smoothing, &last_point, starting_flowrate, min_segment_length]
        (GCodeReader &reader, GCodeReader::GCodeLine line) {
        if (line.cmd_is("G1")) {
            // Filter out retractions (continuous printing does not retract).
            // Small surface fragments are real geometry. Only discard stationary priming;
            // removing a short XY extrusion skips its material and changes the following segment.
            if (line.retracting(reader) || (line.extruding(reader) && line.dist_XY(reader) <= 0.f)) return;
            if (line.has_z() && ! (line.has_x() || line.has_y())) {
                // If this is the initial Z move of the layer, replace it with a
                // (redundant) move to the last Z of the previous layer.
                line.set(Z, z);
                new_gcode += line.raw() + '\n';
                return;
            } else {
                float dist_XY = line.dist_XY(reader);
                if (line.has_x() || line.has_y()) {
                    if (dist_XY > 0 && line.extruding(reader)) {
                        len += dist_XY;
                        float factor = len / total_layer_length;
                        if (transition_in) {
                            // Transition layer: ramp the extrusion from starting_flowrate to 100%.
                            float starting_e_factor = starting_flowrate + (factor * (1.f - starting_flowrate));
                            line.set(E, std::max(0.00001f, line.e() * starting_e_factor), 5 /*decimal_digits*/);
                        }
                        // Core of the continuous print: ramp up Z smoothly along the chain.
                        line.set(Z, z + factor * layer_height);
                        if (smooth) {
                            // Interpolate X/Y towards the previous layer's chain.
                            SpiralVase::SpiralPoint p(line.x(), line.y());
                            current_layer->push_back(p);
                            if (previous_layer != nullptr) {
                                bool  found = false;
                                float dist  = 0;
                                SpiralVase::SpiralPoint nearestp = SpiralVaseHelpers::nearest_point_on_lines(p, previous_layer, found, dist);
                                if (found && dist < max_xy_dist_for_smoothing) {
                                    SpiralVase::SpiralPoint target = SpiralVaseHelpers::add(SpiralVaseHelpers::scale(nearestp, 1 - factor), SpiralVaseHelpers::scale(p, factor));
                                    // M3 hook: verify the smoothed segment stays on the model body (off-body check).
                                    float modified_dist_XY = SpiralVaseHelpers::distance(last_point, target);
                                    if (modified_dist_XY < min_segment_length) {
                                        line.clear();
                                    } else {
                                        line.set(X, target.x);
                                        line.set(Y, target.y);
                                        line.set(E, line.e() * modified_dist_XY / dist_XY, 5 /*decimal_digits*/);
                                        last_point = target;
                                    }
                                } else {
                                    last_point = p;
                                }
                            }
                        }
                        new_gcode += line.raw() + '\n';
                    }
                    // Skip travel moves: in continuous mode the emitter should not produce any;
                    // any leftover travel is dropped here so chains stay welded (see SpiralVase).
                    return;
                }
            }
        }
        new_gcode += line.raw() + '\n';
    });

    delete m_previous_layer;
    m_previous_layer = current_layer;

    return new_gcode;
}

} // namespace Slic3r
