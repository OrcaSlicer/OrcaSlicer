#include "ContinuousPrint.hpp"
#include "../PrintConfig.hpp"

#include <algorithm>
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
    for (size_t i = 1; i + 1 < pts.size(); ++ i) {
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
        emit(pts[i], flat.src_seg[i - 1]);
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

} // namespace

std::vector<std::unique_ptr<ExtrusionEntity>> split_entities_at_junctions(
    const std::vector<ExtrusionEntity*> &entities,
    double junction_epsilon)
{
    // Flatten the splittable entities (ExtrusionPath / ExtrusionLoop); others are passed through.
    std::vector<std::optional<FlattenedEntity>> flats(entities.size());
    for (size_t i = 0; i < entities.size(); ++ i)
        if (entities[i] != nullptr)
            flats[i] = flatten_entity(*entities[i]);

    // Collect interior junction positions per entity: endpoints of other entities lying on it.
    std::vector<std::vector<coordf_t>> junctions(entities.size());
    for (size_t i = 0; i < entities.size(); ++ i) {
        if (! flats[i])
            continue;
        const std::vector<coordf_t> prefix = arc_prefix(flats[i]->polyline);
        const coordf_t total = prefix.back();
        for (size_t j = 0; j < entities.size(); ++ j) {
            if (j == i || entities[j] == nullptr)
                continue;
            for (const Point p : {entities[j]->first_point(), entities[j]->last_point()}) {
                coordf_t dist = 0;
                const coordf_t s = closest_s_on_polyline(flats[i]->polyline, prefix, p, dist);
                if (dist <= junction_epsilon && s > junction_epsilon && total - s > junction_epsilon)
                    junctions[i].push_back(s);
            }
        }
    }

    std::vector<std::unique_ptr<ExtrusionEntity>> out;
    out.reserve(entities.size());
    for (size_t i = 0; i < entities.size(); ++ i) {
        auto &positions = junctions[i];
        if (! flats[i] || positions.empty()) {
            out.emplace_back(entities[i]->clone());
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
                emit_piece(piece, flat.paths, out);
        } else {
            for (const PolylinePiece &piece : split_open(flat, positions))
                emit_piece(piece, flat.paths, out);
        }
    }
    return out;
}

// -----------------------------------------------------------------------------------------------
// Preflight
// -----------------------------------------------------------------------------------------------

ContinuousPrintVerdict preflight_layer(
    const std::vector<ExtrusionEntity*> &entities,
    [[maybe_unused]] const Layer        *layer,
    [[maybe_unused]] const PrintConfig  &cfg,
    ContinuousLayerPlan                 *out_plan)
{
    assert(out_plan != nullptr);

    // Working set: entities cloned and split at junction points (design doc 3.5).
    std::vector<std::unique_ptr<ExtrusionEntity>> working = split_entities_at_junctions(entities);
    std::vector<ExtrusionEntity*> view;
    view.reserve(working.size());
    for (const auto &entity : working)
        view.push_back(entity.get());

    // In-layer single-chain check: the entities must form one continuous trace with zero travel.
    std::optional<ExactChainResult> chain = chain_extrusion_entities_exact(view, nullptr);
    if (! chain)
        return ContinuousPrintVerdict::Reject;

    ContinuousLayerPlan plan;
    plan.entities    = std::move(working);
    plan.order       = chain->order;
    plan.start_point = chain->start;
    plan.end_point   = chain->end;
    plan.is_closed   = chain->closed;
    for (const ExtrusionEntity *entity : view)
        plan.total_length += unscale_(entity->length()); // ExtrusionEntity::length() is in scaled units

    // XY samples along the chain, interpolated at a fixed arc-length step (plus the exact start/end points).
    Points polyline;
    for (const auto &[idx, reversed] : plan.order)
        append_entity_polyline(*view[idx], reversed, polyline);
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
std::string ContinuousPrint::process_layer(const std::string &gcode, bool last_layer)
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
    std::string transition_gcode;
    float max_xy_dist_for_smoothing = m_max_xy_smoothing;
    // Transition tapering works reliably with relative extruder distances only (same as SpiralVase).
    bool transition_in  = m_transition_layer && m_config.use_relative_e_distances.value;
    bool transition_out = last_layer && m_config.use_relative_e_distances.value;

    float starting_flowrate  = float(m_config.spiral_starting_flow_ratio.value);
    float finishing_flowrate = float(m_config.spiral_finishing_flow_ratio.value);
    const float min_segment_length = std::max(float(EPSILON), 2 * float(m_config.resolution.value));

    float len = 0.f;
    SpiralVase::SpiralPoint last_point = previous_layer != nullptr && ! previous_layer->empty() ?
        previous_layer->back() : SpiralVase::SpiralPoint(0, 0);
    m_reader.parse_buffer(gcode, [&new_gcode, &z, total_layer_length, layer_height, transition_in, &len, &current_layer, &previous_layer, &transition_gcode, transition_out, smooth, &max_xy_dist_for_smoothing, &last_point, starting_flowrate, finishing_flowrate, min_segment_length]
        (GCodeReader &reader, GCodeReader::GCodeLine line) {
        if (line.cmd_is("G1")) {
            // Filter out retractions (continuous printing does not retract).
            if (line.retracting(reader) || (line.extruding(reader) && line.dist_XY(reader) < min_segment_length)) return;
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
                            line.set(E, line.e() * starting_e_factor, 5 /*decimal_digits*/);
                        } else if (transition_out) {
                            // Ramp the extrusion down on a duplicated final layer (same as SpiralVase).
                            GCodeReader::GCodeLine transitionLine(line);
                            float finishing_e_factor = finishing_flowrate + ((1.f - factor) * (1.f - finishing_flowrate));
                            transitionLine.set(E, line.e() * finishing_e_factor, 5 /*decimal_digits*/);
                            transition_gcode += transitionLine.raw() + '\n';
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
        if (transition_out)
            transition_gcode += line.raw() + '\n';
    });

    delete m_previous_layer;
    m_previous_layer = current_layer;

    return new_gcode + transition_gcode;
}

} // namespace Slic3r
