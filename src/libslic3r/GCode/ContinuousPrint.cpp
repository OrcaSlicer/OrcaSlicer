#include "ContinuousPrint.hpp"
#include "../ExtrusionEntity.hpp"

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

ContinuousPrintVerdict preflight_layer(
    const std::vector<ExtrusionEntity*> &entities,
    [[maybe_unused]] const Layer        *layer,
    [[maybe_unused]] const PrintConfig  &cfg,
    ContinuousLayerPlan                 *out_plan)
{
    assert(out_plan != nullptr);

    // In-layer single-chain check: the entities must form one continuous trace with zero travel.
    std::optional<ExactChainResult> chain = chain_extrusion_entities_exact(entities, nullptr);
    if (! chain)
        return ContinuousPrintVerdict::Reject;

    ContinuousLayerPlan plan;
    plan.order       = chain->order;
    plan.start_point = chain->start;
    plan.end_point   = chain->end;
    plan.is_closed   = chain->closed;
    for (const ExtrusionEntity *entity : entities)
        plan.total_length += unscale_(entity->length()); // ExtrusionEntity::length() is in scaled units

    // XY samples along the chain, interpolated at a fixed arc-length step (plus the exact start/end points).
    Points polyline;
    for (const auto &[idx, reversed] : plan.order)
        append_entity_polyline(*entities[idx], reversed, polyline);
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

} // namespace Slic3r
