#include "ObjectWipeTowerPlacement.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Geometry/ConvexHull.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/libslic3r.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Slic3r {

SequentialClearanceGeometry sequential_clearance_geometry(
    const Polygon &local_hull, const Point &shift, float object_distance, double rod_adjustment, double top, const Polygon *tower)
{
    SequentialClearanceGeometry geometry{local_hull, {}, {}, top};
    auto                        grown = offset(local_hull, object_distance, jtRound, scale_(0.1));
    if (!grown.empty()) {
        geometry.expanded = std::move(grown.front());
        geometry.expanded.translate(shift);
    }
    geometry.hull.translate(shift);
    geometry.rod_box = geometry.expanded.bounding_box();
    geometry.rod_box.offset(rod_adjustment);
    return tower ? sequential_clearance_with_tower(geometry, *tower, object_distance, rod_adjustment) : geometry;
}

SequentialClearanceGeometry sequential_clearance_with_tower(const SequentialClearanceGeometry &base, const Polygon &tower, float object_distance, double rod_adjustment)
{
    SequentialClearanceGeometry geometry = base;
    Points                      points   = geometry.hull.points;
    append(points, tower.points);
    geometry.hull = Geometry::convex_hull(std::move(points));
    auto grown    = offset(geometry.hull, object_distance, jtRound, scale_(0.1));
    if (!grown.empty()) geometry.expanded = std::move(grown.front());
    geometry.rod_box = geometry.expanded.bounding_box();
    geometry.rod_box.offset(rod_adjustment);
    return geometry;
}

bool sequential_clearance_overlaps_in_y(const BoundingBox &a, const BoundingBox &b) { return std::min(a.max.y(), b.max.y()) - std::max(a.min.y(), b.min.y()) > 0; }

namespace {

constexpr size_t max_choices   = 32;
constexpr size_t search_budget = 20000;

struct Choice
{
    ObjectWipeTowerPlacement placement;
    Polygon                  expanded;
    Polygons                 swept;
    BoundingBox              expanded_box;
    BoundingBox              rod_box;
    BoundingBox              tower_box;
    BoundingBox              swept_box;
    double                   top;
    size_t                   id;
};

struct Candidates
{
    size_t              instance_index;
    std::vector<Choice> choices;
};

struct InstanceGeometry : SequentialClearanceGeometry
{ Polygons exclusion_obstacles; };

struct ObstacleZone
{
    Polygons    polygons;
    BoundingBox box;
};
ObstacleZone make_zone(Polygons polygons)
{
    ObstacleZone zone{union_(polygons), {}};
    for (const Polygon &poly : zone.polygons) zone.box.merge(poly.bounding_box());
    return zone;
}

class PlacementSearch
{
public:
    PlacementSearch(const std::vector<ObjectWipeTowerPlacementInstance> &instances, const ObjectWipeTowerPlacementParameters &parameters, const std::function<void()> &cancel)
        : instances(instances), parameters(parameters), throw_if_canceled(cancel), instance_count(instances.size()), bed(diff(parameters.bed, parameters.excluded))
    {}

    ObjectWipeTowerPlacementResult run();

private:
    void                           prepare_geometry();
    void                           generate_candidates();
    std::vector<Point>             generate_translations(const BoundingBox &object_box, const BoundingBox &tower_box, coord_t gap) const;
    ObjectWipeTowerPlacementResult select_placements();
    std::optional<Choice>          admissible_candidate(size_t instance_index, const Point &translation, const ObstacleZone &footprint_zone, const ObstacleZone &clearance_zone);

    const std::vector<ObjectWipeTowerPlacementInstance> &instances;
    const ObjectWipeTowerPlacementParameters            &parameters;
    const std::function<void()>                         &throw_if_canceled;
    const size_t                                         instance_count;
    Polygons                                             bed;
    std::vector<InstanceGeometry>                        geometry;
    std::vector<std::vector<bool>>                       baseline_close;
    std::vector<std::vector<bool>>                       baseline_tall;
    std::vector<Candidates>                              candidates;
    size_t                                               choice_count = 0;
};

void PlacementSearch::prepare_geometry()
{
    for (const auto &instance : instances) {
        InstanceGeometry base;
        static_cast<SequentialClearanceGeometry &>(base) = sequential_clearance_geometry(instance.model_hull, instance.shift, parameters.object_distance,
                                                                                         parameters.rod_adjustment, instance.top);
        for (const Polygon &area : parameters.excluded)
            if (intersection(area, base.hull).empty()) base.exclusion_obstacles.push_back(area);
        base.exclusion_obstacles = union_(base.exclusion_obstacles);
        geometry.push_back(std::move(base));
    }
    // Exempt only the particular baseline pair that already fails without towers.
    baseline_close.assign(instance_count, std::vector<bool>(instance_count, false));
    baseline_tall.assign(instance_count, std::vector<bool>(instance_count, false));
    for (size_t i = 0; i < instance_count; ++i)
        for (size_t j = i + 1; j < instance_count; ++j) {
            const auto &a        = geometry[i];
            const auto &b        = geometry[j];
            const bool  close    = a.expanded.bounding_box().overlap(b.expanded.bounding_box()) && !intersection(a.expanded, b.expanded).empty();
            baseline_close[i][j] = baseline_close[j][i] = close;
            baseline_tall[i][j]                         = a.top > parameters.rod_height && sequential_clearance_overlaps_in_y(a.rod_box, b.rod_box);
        }
}

std::vector<Point> PlacementSearch::generate_translations(const BoundingBox &object_box, const BoundingBox &tower_box, coord_t gap) const
{
    const coord_t step      = scaled(2.);
    const Point   centres[] = {Point(object_box.max.x() + gap - tower_box.min.x(), object_box.center().y() - tower_box.center().y()),
                               Point(object_box.min.x() - gap - tower_box.max.x(), object_box.center().y() - tower_box.center().y()),
                               Point(object_box.center().x() - tower_box.center().x(), object_box.max.y() + gap - tower_box.min.y()),
                               Point(object_box.center().x() - tower_box.center().x(), object_box.min.y() - gap - tower_box.max.y())};
    struct Translation
    {
        Point   point;
        coord_t distance;
        size_t  side;
        bool    negative;
    };
    std::vector<Translation> translations;
    for (size_t side = 0; side < 4; ++side) {
        translations.push_back({centres[side], 0, side, false});
        const size_t axis = side < 2 ? 1 : 0;
        for (bool negative : {false, true}) {
            // At the endpoint the tower is diagonally beyond the corner,
            // with the ring gap in BOTH axes. Include an off-grid end.
            const coord_t limit = negative ? tower_box.max(axis) + centres[side](axis) - object_box.min(axis) + gap :
                                             object_box.max(axis) + gap - tower_box.min(axis) - centres[side](axis);
            for (coord_t distance = std::min(step, limit); distance > 0;) {
                Point translation = centres[side];
                translation(axis) += negative ? -distance : distance;
                translations.push_back({translation, distance, side, negative});
                if (distance == limit) break;
                distance += std::min(step, coord_t(limit - distance));
            }
        }
    }
    std::sort(translations.begin(), translations.end(), [](const Translation &a, const Translation &b) {
        if (a.distance != b.distance) return a.distance < b.distance;
        if (a.side != b.side) return a.side < b.side;
        return a.negative < b.negative;
    });
    std::vector<Point>                    result;
    std::set<std::pair<coord_t, coord_t>> seen_translations;
    for (const Translation &entry : translations) {
        const Point &translation = entry.point;
        if (!seen_translations.emplace(translation.x(), translation.y()).second) continue; // Adjacent sides share their diagonal corner.
        result.push_back(translation);
    }
    return result;
}

std::optional<Choice> PlacementSearch::admissible_candidate(size_t              instance_index,
                                                            const Point        &translation,
                                                            const ObstacleZone &footprint_zone,
                                                            const ObstacleZone &clearance_zone)
{
    const auto  &instance     = instances[instance_index];
    const auto  &base         = geometry[instance_index];
    const double tower_height = instance.tower_height;
    const double unit_top     = std::max(base.top, double(scaled(tower_height)));
    Polygon      candidate    = instance.tower_hull;
    candidate.translate(translation);
    if (!diff(Polygons{candidate}, bed).empty()) return std::nullopt;
    Polygons    swept = offset(candidate, scaled(std::max(0., parameters.clearance)));
    BoundingBox swept_box;
    for (const Polygon &poly : swept) swept_box.merge(poly.bounding_box());
    if (!footprint_zone.polygons.empty() && swept_box.overlap(footprint_zone.box) && !intersection(swept, footprint_zone.polygons).empty()) return std::nullopt;

    auto unit = sequential_clearance_with_tower(base, candidate, parameters.object_distance, parameters.rod_adjustment);
    if (!base.exclusion_obstacles.empty() && !intersection(base.exclusion_obstacles, unit.hull).empty()) return std::nullopt;
    Polygon           expanded     = std::move(unit.expanded);
    const BoundingBox expanded_box = expanded.bounding_box();
    if (!clearance_zone.polygons.empty() && expanded_box.overlap(clearance_zone.box) && !intersection(Polygons{expanded}, clearance_zone.polygons).empty()) return std::nullopt;
    const BoundingBox rod_box  = unit.rod_box;
    bool              too_tall = false;
    for (size_t other = 0; other < instance_count; ++other) {
        if (other == instance_index) continue;
        const size_t earlier     = std::min(instance_index, other);
        const size_t later       = std::max(instance_index, other);
        const double earlier_top = earlier == instance_index ? unit_top : geometry[other].top;
        if (!baseline_tall[earlier][later] && earlier_top > parameters.rod_height && sequential_clearance_overlaps_in_y(rod_box, geometry[other].rod_box)) {
            too_tall = true;
            break;
        }
    }
    if (too_tall) return std::nullopt;
    // The validator also rejects an unsafe descent beside one's OWN
    // compacted tower. Do not select such a tower and fail afterwards.
    if (instance.compacted) {
        const auto zone          = compacted_wipe_tower_zone(*parameters.compacted_config, candidate);
        const auto own_clearance = compacted_wipe_tower_clearance(*parameters.compacted_config, zone, instance.footprint, instance.compacted_rise);
        if (instance.compacted_rise > own_clearance.allowed_rise + EPSILON) return std::nullopt;
    }
    const Vec2d       pos           = unscale(Point(translation - parameters.origin));
    const BoundingBox candidate_box = candidate.bounding_box();
    return Choice{ObjectWipeTowerPlacement{pos.cast<float>(), std::move(candidate), tower_height},
                  std::move(expanded),
                  std::move(swept),
                  expanded_box,
                  rod_box,
                  candidate_box,
                  swept_box,
                  unit_top,
                  choice_count++};
}

void PlacementSearch::generate_candidates()
{
    for (size_t instance_index = 0; instance_index < instance_count; ++instance_index) {
        throw_if_canceled();
        const auto &instance = instances[instance_index];
        if (instance.tower_hull.empty()) continue;
        const double      tower_height = instance.tower_height;
        const BoundingBox tower_box    = instance.tower_hull.bounding_box();
        const BoundingBox object_box   = instance.footprint.bounding_box();
        Candidates        options{instance_index, {}};
        Polygons          footprint_obstacles, clearance_obstacles;
        for (size_t other = 0; other < instance_count; ++other) {
            if (other == instance_index) continue;
            footprint_obstacles.push_back(instances[other].footprint);
            if (!baseline_close[instance_index][other]) clearance_obstacles.push_back(geometry[other].expanded);
        }
        // Each candidate needs at most ONE intersection per kind of obstacle,
        // with no offset per other instance. Preserve the original miter sweep.
        const auto   footprint_zone    = make_zone(std::move(footprint_obstacles));
        const auto   clearance_zone    = make_zone(std::move(clearance_obstacles));
        const auto  &base              = geometry[instance_index];
        const double unit_top          = std::max(base.top, double(scaled(tower_height)));
        const double base_height_limit = instance_index + 1 == instance_count ? parameters.printable_height : parameters.lid_height;

        // Only an entirely empty static ring permits searching farther out.
        // Dynamic tower conflicts do not change the selected ring.
        for (double extra_gap : {0., 5., 10., 20., 30., 40.}) {
            throw_if_canceled();
            const coord_t gap = scaled(2. + extra_gap);
            for (const Point &translation : generate_translations(object_box, tower_box, gap)) {
                throw_if_canceled();
                if (unit_top > base_height_limit && base.top <= base_height_limit) break; // A taller tower must not create a new lid/printable-height error.
                if (auto choice = admissible_candidate(instance_index, translation, footprint_zone, clearance_zone)) options.choices.push_back(std::move(*choice));
                if (options.choices.size() == max_choices) break;
            }
            if (!options.choices.empty()) break;
        }
        candidates.push_back(std::move(options));
    }
}

ObjectWipeTowerPlacementResult PlacementSearch::select_placements()
{
    ObjectWipeTowerPlacementResult result;
    // All indices come from model print order, never from pointer/map order.
    // Compute a pair only when visited, cache both positive and negative results.
    std::vector<std::unordered_map<size_t, bool>> pair_conflicts(choice_count);
    auto                                          conflicting = [&](size_t earlier, size_t earlier_choice, size_t later, size_t later_choice) {
        const auto &a     = candidates[earlier].choices[earlier_choice];
        const auto &b     = candidates[later].choices[later_choice];
        auto       &cache = pair_conflicts[a.id];
        if (auto it = cache.find(b.id); it != cache.end()) return it->second;
        const size_t i = candidates[earlier].instance_index;
        const size_t j = candidates[later].instance_index;
        // Rod spans the whole X axis: test Y even when 2D boxes are disjoint.
        bool collision = !baseline_tall[i][j] && a.top > parameters.rod_height && sequential_clearance_overlaps_in_y(a.rod_box, b.rod_box);
        if (!collision && b.swept_box.overlap(a.tower_box)) collision = !intersection(b.swept, Polygons{a.placement.footprint}).empty();
        if (!collision && !baseline_close[i][j] && a.expanded_box.overlap(b.expanded_box)) collision = !intersection(a.expanded, b.expanded).empty();
        cache.emplace(b.id, collision);
        return collision;
    };
    const size_t        no_choice = max_choices;
    std::vector<size_t> selected(candidates.size(), no_choice), best_selected = selected;
    auto                compatible = [&](size_t index, size_t choice) {
        for (size_t i = 0; i < index; ++i)
            if (selected[i] != no_choice && conflicting(i, selected[i], index, choice)) return false;
        return true;
    };
    // Seed with the first (greedy) descent, independently of the search budget.
    // Even an interrupted optimisation therefore retains a complete assignment.
    size_t best_count = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
        throw_if_canceled();
        for (size_t choice = 0; choice < candidates[i].choices.size(); ++choice)
            if (compatible(i, choice)) {
                selected[i] = choice;
                ++best_count;
                break;
            }
    }
    best_selected = selected;
    std::fill(selected.begin(), selected.end(), no_choice);
    auto better = [&](size_t count) {
        if (count != best_count) return count > best_count;
        // Presence of earlier instances takes priority over ALL position ranks.
        for (size_t i = 0; i < selected.size(); ++i)
            if ((selected[i] != no_choice) != (best_selected[i] != no_choice)) return selected[i] != no_choice;
        return std::lexicographical_compare(selected.begin(), selected.end(), best_selected.begin(), best_selected.end());
    };
    std::vector<size_t> remaining(candidates.size() + 1, 0);
    for (size_t i = candidates.size(); i > 0; --i) remaining[i - 1] = remaining[i] + !candidates[i - 1].choices.empty();
    size_t                              nodes = 0;
    std::function<void(size_t, size_t)> place = [&](size_t index, size_t count) {
        throw_if_canceled();
        if (count + remaining[index] < best_count) return;
        if (index == candidates.size()) {
            if (better(count)) {
                best_count    = count;
                best_selected = selected;
            }
            return;
        }
        if (nodes >= search_budget) return;
        if (count + remaining[index] == best_count) {
            // An optimistic presence mask bounds the first tie-break. When it
            // ties, an already worse candidate prefix cannot improve the best.
            bool presence_better = false;
            for (size_t i = 0; i < candidates.size(); ++i) {
                const bool optimistic = i < index ? selected[i] != no_choice : !candidates[i].choices.empty();
                const bool best       = best_selected[i] != no_choice;
                if (optimistic != best) {
                    if (!optimistic) return;
                    presence_better = true;
                    break;
                }
            }
            if (!presence_better) {
                size_t i = 0;
                while (i < index && selected[i] == best_selected[i]) ++i;
                if (i < index && selected[i] > best_selected[i]) return;
            }
        }
        for (size_t choice = 0; choice < candidates[index].choices.size(); ++choice) {
            if (nodes >= search_budget) return;
            ++nodes; // Count rejected choices too, not just recursive descents.
            if (!compatible(index, choice)) continue;
            selected[index] = choice;
            place(index + 1, count + 1);
            selected[index] = no_choice;
        }
        if (nodes < search_budget) {
            ++nodes;
            place(index + 1, count); // An unplaceable instance never discards unrelated towers.
        }
    };
    place(0, 0);
    for (size_t i = 0; i < candidates.size(); ++i)
        if (best_selected[i] != no_choice)
            result.placed.emplace_back(candidates[i].instance_index, candidates[i].choices[best_selected[i]].placement);
        else
            result.unplaced.push_back(candidates[i].instance_index);
    return result;
}

ObjectWipeTowerPlacementResult PlacementSearch::run()
{
    prepare_geometry();
    generate_candidates();
    return select_placements();
}

} // namespace

ObjectWipeTowerPlacementResult place_object_wipe_towers(const std::vector<ObjectWipeTowerPlacementInstance> &instances,
                                                        const ObjectWipeTowerPlacementParameters            &parameters,
                                                        const std::function<void()>                         &throw_if_canceled)
{ return PlacementSearch(instances, parameters, throw_if_canceled).run(); }

