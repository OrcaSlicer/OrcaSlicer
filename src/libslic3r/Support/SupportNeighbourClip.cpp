#include "SupportNeighbourClip.hpp"

#include "../ClipperUtils.hpp"
#include "../ExtrusionEntity.hpp"
#include "../ExtrusionEntityCollection.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <typeinfo>

namespace Slic3r {

namespace {

// See SupportNeighbourClip.hpp, "Scope".
bool sliced_in_plate_frame(const PrintObject &object)
{
    const Print &print = *object.print();
    if (print.config().belt_printer.value)
        return false;
    if (object.is_segment())
        // The root slices along +Z exactly and rests on the bed; every other segment builds along
        // its own normal and rebases its print_z onto its own floor.
        return object.is_root_segment();
    return layer_rotation_trafo(print.config(), object.config()).linear().isIdentity(1e-12);
}

// Every non-collection extrusion of a support layer clipped against `obstacle`. Collections are
// clipped in place; a path, multipath or loop that crosses the obstacle is replaced by the plain
// paths left outside it. Returns true when anything changed.
class ExtrusionClipper
{
public:
    ExtrusionClipper(const ExPolygons &obstacle) : m_obstacle(obstacle), m_obstacle_bbox(get_extents(obstacle)) {}

    bool clip(ExtrusionEntityCollection &collection)
    {
        bool               changed = false;
        ExtrusionEntitiesPtr out;
        out.reserve(collection.entities.size());
        for (ExtrusionEntity *ee : collection.entities) {
            if (auto *sub = dynamic_cast<ExtrusionEntityCollection*>(ee)) {
                changed |= this->clip(*sub);
                if (sub->entities.empty())
                    delete ee;
                else
                    out.emplace_back(ee);
                continue;
            }
            Points pts;
            ee->collect_points(pts);
            if (pts.empty() || ! BoundingBox(pts).overlap(m_obstacle_bbox)) {
                out.emplace_back(ee);
                continue;
            }
            const ExtrusionPaths *paths = nullptr;
            ExtrusionPaths        single;
            if (auto *path = dynamic_cast<ExtrusionPath*>(ee)) {
                single.emplace_back(*path);
                paths = &single;
            } else if (auto *multi = dynamic_cast<ExtrusionMultiPath*>(ee))
                paths = &multi->paths;
            else if (auto *loop = dynamic_cast<ExtrusionLoop*>(ee))
                paths = &loop->paths;
            if (paths == nullptr) {
                out.emplace_back(ee);
                continue;
            }
            ExtrusionPaths kept;
            bool           crossed = false;
            for (const ExtrusionPath &path : *paths)
                crossed |= clip_path(path, kept);
            if (! crossed) {
                out.emplace_back(ee);
                continue;
            }
            changed = true;
            delete ee;
            for (ExtrusionPath &piece : kept)
                out.emplace_back(new ExtrusionPath(std::move(piece)));
        }
        collection.entities = std::move(out);
        return changed;
    }

private:
    // Appends what of `path` lies outside the obstacle to `kept`; returns whether it crossed it.
    bool clip_path(const ExtrusionPath &path, ExtrusionPaths &kept) const
    {
        const Polyline flat   = path.polyline.to_polyline();
        const double   length = flat.length();
        Polylines      pieces = diff_pl(Polylines{ flat }, m_obstacle);
        double         left   = 0.;
        for (const Polyline &p : pieces)
            left += p.length();
        if (left > length - SCALED_EPSILON) {
            kept.emplace_back(path);
            return false;
        }
        // Only a plain, flat path is cut. Anything carrying per-point heights or a derived type
        // (sloped, contoured, oriented) cannot be rebuilt from 2D pieces and is dropped whole.
        bool flat_z = typeid(path) == typeid(ExtrusionPath) && ! path.z_contoured;
        const coord_t z = path.polyline.points.empty() ? 0 : path.polyline.points.front().z();
        for (const Point3 &pt : path.polyline.points)
            flat_z &= pt.z() == z;
        if (flat_z) {
            // A stub shorter than the line's width is not worth a start and a stop.
            const double min_length = scale_(std::max(0.f, path.width));
            for (Polyline &p : pieces)
                if (p.points.size() >= 2 && p.length() >= min_length)
                    kept.emplace_back(Polyline3(p, z), path);
        }
        return true;
    }

    const ExPolygons &m_obstacle;
    BoundingBox       m_obstacle_bbox;
};

float max_extrusion_width(const ExtrusionEntityCollection &collection)
{
    float w = 0.f;
    for (const ExtrusionEntity *ee : collection.entities) {
        if (auto *sub = dynamic_cast<const ExtrusionEntityCollection*>(ee))
            w = std::max(w, max_extrusion_width(*sub));
        else if (auto *path = dynamic_cast<const ExtrusionPath*>(ee))
            w = std::max(w, path->width);
        else if (auto *multi = dynamic_cast<const ExtrusionMultiPath*>(ee))
            for (const ExtrusionPath &p : multi->paths) w = std::max(w, p.width);
        else if (auto *loop = dynamic_cast<const ExtrusionLoop*>(ee))
            for (const ExtrusionPath &p : loop->paths) w = std::max(w, p.width);
    }
    return w;
}

struct Placement
{
    const PrintObject *object;   // whose layers are at this placement
    const PrintObject *owner;    // whose support layers those are (the shared object, if any)
    Point              shift;
};

// A neighbour placement seen from one placement of the clipped object: its layers, moved by
// `delta` into the clipped object's coordinates.
struct Obstacle
{
    const PrintObject *object;
    Point              delta;
};

BoundingBox layers_extents(const PrintObject &object)
{
    BoundingBox bb;
    for (const Layer *layer : object.layers())
        for (const ExPolygon &ex : layer->lslices)
            bb.merge(get_extents(ex.contour));
    return bb;
}

} // namespace

size_t clip_supports_to_neighbours(Print &print, const std::function<void()> &throw_if_canceled)
{
    // Placements of every object sliced in the plate frame, grouped by who owns the support layers.
    std::vector<Placement>                                    placements;
    std::map<const PrintObject*, BoundingBox>                 extents;
    for (PrintObject *object : print.objects_mutable()) {
        if (! sliced_in_plate_frame(*object) || object->layers().empty())
            continue;
        const PrintObject *owner = object->get_shared_object() ? object->get_shared_object() : object;
        for (const PrintInstance &instance : object->instances())
            placements.push_back({ object, owner, instance.shift });
        extents[object] = layers_extents(*object);
    }

    std::atomic<size_t> layers_changed { 0 };
    for (PrintObject *owner : print.objects_mutable()) {
        if (owner->get_shared_object() != nullptr)
            continue;   // its support layers belong to the shared object
        SupportLayerPtrs &support_layers = owner->support_layers();
        if (support_layers.empty())
            continue;

        // Restore whatever an earlier run clipped: the neighbours may have moved since. The simplify
        // step runs after this and only once, so a layer saved before it ran is simplified here.
        const bool simplified = owner->is_step_done(posSimplifySupportPath);
        for (SupportLayer *sl : support_layers)
            if (sl->has_unclipped_support) {
                sl->support_fills         = std::move(sl->unclipped_support_fills);
                sl->support_islands       = std::move(sl->unclipped_support_islands);
                sl->unclipped_support_fills.clear();
                sl->unclipped_support_islands.clear();
                sl->has_unclipped_support = false;
                if (simplified && ! sl->unclipped_support_simplified)
                    sl->simplify_support_extrusion_path();
            }

        std::vector<Obstacle> obstacles;
        if (sliced_in_plate_frame(*owner)) {
            // The support's own extent, to drop every neighbour that cannot reach it.
            BoundingBox support_bb;
            for (const SupportLayer *sl : support_layers) {
                Points pts;
                sl->support_fills.collect_points(pts);
                if (! pts.empty())
                    support_bb.merge(BoundingBox(pts));
            }
            const PrintObjectConfig &cfg    = owner->config();
            const coord_t            margin = scale_(std::max(cfg.support_object_xy_distance.value, cfg.support_object_first_layer_gap.value) + 5.);
            if (support_bb.defined)
                support_bb.offset(margin);
            for (const Placement &self : placements) {
                if (self.owner != owner)
                    continue;
                for (const Placement &other : placements) {
                    if (other.object == self.object && other.shift == self.shift)
                        continue;   // the support already keeps clear of its own object
                    const Point delta = other.shift - self.shift;
                    BoundingBox bb    = extents[other.object];
                    if (! bb.defined || ! support_bb.defined)
                        continue;
                    bb.translate(delta);
                    if (bb.overlap(support_bb))
                        obstacles.push_back({ other.object, delta });
                }
            }
        }

        if (! obstacles.empty()) {
            const PrintObjectConfig &cfg          = owner->config();
            const double             gap_xy       = cfg.support_object_xy_distance.value;
            const double             gap_xy_first = std::max(gap_xy, cfg.support_object_first_layer_gap.value);
            const double             gap_above    = cfg.support_bottom_z_distance.value;   // support resting on a neighbour
            const double             gap_below    = cfg.support_top_z_distance.value;      // support touching a neighbour's underside
            tbb::parallel_for(tbb::blocked_range<size_t>(0, support_layers.size()), [&](const tbb::blocked_range<size_t> &range) {
                for (size_t i = range.begin(); i < range.end(); ++ i) {
                    throw_if_canceled();
                    SupportLayer &sl = *support_layers[i];
                    if (sl.support_fills.entities.empty() && sl.support_islands.empty())
                        continue;
                    const double top    = sl.print_z;
                    const double bottom = sl.print_z - sl.height;
                    Points pts;
                    sl.support_fills.collect_points(pts);
                    for (const ExPolygon &ex : sl.support_islands)
                        append(pts, ex.contour.points);
                    if (pts.empty())
                        continue;
                    const double gap   = (bottom < EPSILON ? gap_xy_first : gap_xy) + 0.5 * max_extrusion_width(sl.support_fills);
                    BoundingBox  reach = BoundingBox(pts);
                    reach.offset(scale_(gap) + SCALED_EPSILON);

                    Polygons blocked;
                    for (const Obstacle &o : obstacles) {
                        // Neighbour layers whose Z interval, grown by the support Z gaps, overlaps this one.
                        for (const Layer *layer : o.object->layers()) {
                            const double l_top    = layer->print_z + gap_above;
                            const double l_bottom = layer->print_z - layer->height - gap_below;
                            if (l_bottom >= top - EPSILON)
                                break;
                            if (l_top <= bottom + EPSILON)
                                continue;
                            for (size_t k = 0; k < layer->lslices.size(); ++ k) {
                                BoundingBox bb = k < layer->lslices_bboxes.size() ? layer->lslices_bboxes[k] : get_extents(layer->lslices[k].contour);
                                bb.translate(o.delta);
                                if (! bb.overlap(reach))
                                    continue;
                                ExPolygon moved = layer->lslices[k];
                                moved.translate(o.delta);
                                polygons_append(blocked, to_polygons(std::move(moved)));
                            }
                        }
                    }
                    if (blocked.empty())
                        continue;
                    const ExPolygons obstacle = offset_ex(union_(blocked), float(scale_(gap)));
                    if (obstacle.empty())
                        continue;

                    ExtrusionEntityCollection original_fills   = sl.support_fills;
                    ExPolygons                original_islands = sl.support_islands;
                    ExtrusionClipper          clipper(obstacle);
                    if (clipper.clip(sl.support_fills)) {
                        sl.support_islands           = diff_ex(sl.support_islands, obstacle);
                        sl.unclipped_support_fills   = std::move(original_fills);
                        sl.unclipped_support_islands = std::move(original_islands);
                        sl.has_unclipped_support     = true;
                        sl.unclipped_support_simplified = simplified;
                        // New pieces from a cut have no arc fitting yet.
                        if (simplified)
                            sl.simplify_support_extrusion_path();
                        ++ layers_changed;
                    }
                }
            });
        }
    }
    return layers_changed.load();
}

} // namespace Slic3r
