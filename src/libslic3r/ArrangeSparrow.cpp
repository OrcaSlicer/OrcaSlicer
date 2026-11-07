#include "ArrangeSparrow.hpp"
#include "ArrangeProgress.hpp"

#include "BoundingBox.hpp"
#include "ClipperUtils.hpp"
#include "I18N.hpp"
#include "Print.hpp"

#include <libnest2d/common.hpp> // MAX_NUM_PLATES

#include <sparrow_arrange.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

//! macro used to mark string used at localization, return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r { namespace arrangement {

namespace {

// process_arrangeable() clamps inflation up to this; match it so spacing is identical.
const coord_t SPARROW_MIN_SEPARATION = scale_(1.0);

// Outlines arrive at mesh resolution, and sparrow's cost grows with the vertex count.
// Simplifying within this tolerance cuts it 3-28x for at most this much extra margin.
const coord_t SPARROW_SIMPLIFY_TOLERANCE = scale_(0.05);

// Key used to recognize the same exclusion polygon replicated across beds.
std::string poly_key(const Polygon &p)
{
    std::string k;
    k.reserve(p.points.size() * 24);
    for (const Point &pt : p.points) {
        k += std::to_string(pt.x());
        k.push_back(',');
        k += std::to_string(pt.y());
        k.push_back(';');
    }
    return k;
}

// Same jtSquare offset libnest2d's Item::inflate applies; keeps the largest contour.
bool inflate_contour(const Polygon &src, coord_t infl, Polygon &out)
{
    if (src.points.size() < 3)
        return false;
    if (infl <= 0) {
        out = src;
        return true;
    }

    Polygons res       = offset(src, float(infl), jtSquare);
    size_t   best      = res.size();
    double   best_area = 0.;
    for (size_t i = 0; i < res.size(); ++i) {
        double a = std::abs(res[i].area());
        if (a > best_area) { best_area = a; best = i; }
    }
    if (best == res.size() || res[best].points.size() < 3) {
        // Offset collapsed the contour; keep the raw one.
        out = src;
        return true;
    }
    out = std::move(res[best]);
    return true;
}

// Inflate the solid while shrinking its openings by the same amount. This gives
// the final cleanup both sides of the requested separation: movable footprints
// are inflated too. Keep the largest result, matching the contour-only path.
bool inflate_expolygon(const ExPolygon &src, coord_t infl, ExPolygon &out)
{
    if (src.contour.points.size() < 3)
        return false;
    if (infl <= 0) {
        out = src;
        return true;
    }
    ExPolygons res = offset_ex(src, float(infl), jtSquare);
    size_t best = res.size();
    double best_area = 0.;
    for (size_t i = 0; i < res.size(); ++i) {
        const double a = std::abs(res[i].area());
        if (a > best_area) { best_area = a; best = i; }
    }
    if (best == res.size() || res[best].contour.points.size() < 3)
        return false;
    out = std::move(res[best]);
    return true;
}

// Replaces `inflated` (src inflated by infl) with a simplified outline that still contains
// it: inflate by the tolerance as well, then simplify within it. Douglas-Peucker deviates
// at most the tolerance, so the result cannot cut into `inflated`; the diff confirms it.
bool simplify_inflated(const ExPolygon &src, coord_t infl, ExPolygon &inflated)
{
    ExPolygon wider;
    if (!inflate_expolygon(src, infl + SPARROW_SIMPLIFY_TOLERANCE, wider))
        return false;
    ExPolygons simple = wider.simplify(double(SPARROW_SIMPLIFY_TOLERANCE));
    if (simple.size() != 1 || simple.front().contour.points.size() < 3 || !diff_ex(ExPolygons{inflated}, simple).empty())
        return false;
    inflated = std::move(simple.front());
    return true;
}

struct SparrowShapeStorage
{
    std::vector<sp_point>              outline;
    std::vector<std::vector<sp_point>> opening_points;
    std::vector<sp_polygon>            openings;
};

std::vector<sp_point> to_sp_points(const Polygon &p, const Point &origin)
{
    std::vector<sp_point> pts;
    pts.reserve(p.points.size());
    for (const Point &q : p.points)
        pts.push_back(sp_point{unscaled<double>(q.x() - origin.x()),
                               unscaled<double>(q.y() - origin.y())});
    return pts;
}

// Same tiny shrink arrange() applies to fixed items.
coord_t fixed_inflation(const ArrangePolygon &ap)
{
    coord_t infl = std::max(ap.inflation, SPARROW_MIN_SEPARATION) - scaled<coord_t>(2. * EPSILON);
    return std::max(infl, coord_t(0));
}

Polygon world_contour(const ArrangePolygon &ap)
{
    Polygon w = ap.poly.contour;
    w.rotate(ap.rotation);
    w.translate(ap.translation.x(), ap.translation.y());
    return w;
}

// Shared closure for both FFI callbacks: sp_input has one `user` pointer.
struct SparrowCallbackCtx
{
    const std::function<bool(void)> *              stopcondition = nullptr;
    const std::function<void(unsigned, std::string)> *progressind = nullptr;
    int                                            last_bed      = -1;
    int                                            last_placed   = -1;
    const std::function<void(double, std::string)> *fraction = nullptr;
    ArrangeProgress progress;
    double budget = 0.;
    double estimated_plates = 1.;
    std::chrono::steady_clock::time_point bed_start, last_tick;
    std::string message;

    // For the live layout callback.
    const ArrangeParams *    params      = nullptr;
    const ArrangePolygons *  arrangables = nullptr;
    const std::vector<size_t> *movable_src = nullptr; // sparrow's movable items -> arrangables
    Point                    bed_min;

    void tick()
    {
        if (!fraction || last_bed < 0)
            return;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_tick < std::chrono::milliseconds(100))
            return;
        last_tick = now;
        const double elapsed = std::chrono::duration<double>(now - bed_start).count();
        (*fraction)(progress.advance(elapsed, budget), message);
    }
};

int sparrow_should_stop_cb(void *user)
{
    auto *ctx = static_cast<SparrowCallbackCtx *>(user);
    if (ctx == nullptr)
        return 0;
    if (ctx->stopcondition && *ctx->stopcondition && (*ctx->stopcondition)())
        return 1;
    ctx->tick();
    return 0;
}

// Runs on the worker thread, at bed start and after each committed item.
void sparrow_on_progress_cb(void *user, int bed_idx, int placed, int total)
{
    auto *ctx = static_cast<SparrowCallbackCtx *>(user);
    if (ctx == nullptr)
        return;
    // A bed start repeats the previous placed count; only report real changes.
    if (bed_idx == ctx->last_bed && placed == ctx->last_placed)
        return;
    if (bed_idx >= 0 && bed_idx != ctx->last_bed) {
        // Completed plates provide a better estimate than the initial area bound.
        const double remaining = bed_idx > 0 && placed > 0
            ? std::ceil(double(total - placed) * bed_idx / placed)
            : ctx->estimated_plates;
        ctx->progress.begin_plate(total > 0 ? double(placed) / total : 0., remaining);
        ctx->bed_start = ctx->last_tick = std::chrono::steady_clock::now();
    }
    ctx->last_bed    = bed_idx;
    ctx->last_placed = placed;

    // The GUI shows _L("Arranging") + " " + str, so emit only the suffix.
    // bed_idx is -1 once the final cleanup starts.
    std::string msg = bed_idx < 0 ? std::string(L("final cleanup")) :
        (boost::format(L("plate %1%, %2%/%3% objects placed")) % (bed_idx + 1) % placed % total).str();
    BOOST_LOG_TRIVIAL(debug) << "sparrow: progress st=" << placed << " " << msg;
    // progressind takes the count done, so pass placed.
    ctx->message = msg;
    if (ctx->progressind && *ctx->progressind)
        (*ctx->progressind)(unsigned(placed), msg);
    if (ctx->fraction)
        (*ctx->fraction)(ctx->progress.observe(total > 0 ? 0.99 * placed / total : 0.), msg);
}

// Runs on the worker thread whenever sparrow's layout improves.
void sparrow_on_layout_cb(void *user, const sp_placement *layout, size_t)
{
    const auto *ctx = static_cast<SparrowCallbackCtx *>(user);
    if (ctx == nullptr || ctx->params == nullptr || !ctx->params->on_layout)
        return;
    std::vector<ArrangeParams::Pose> poses;
    poses.reserve(ctx->arrangables->size());
    for (const ArrangePolygon &ap : *ctx->arrangables)
        poses.push_back({ap.bed_idx, ap.translation, ap.rotation});
    for (size_t k = 0; k < ctx->movable_src->size(); ++k)
        poses[(*ctx->movable_src)[k]] = {layout[k].bed_idx,
                                         {scaled<coord_t>(layout[k].x) + ctx->bed_min.x(), scaled<coord_t>(layout[k].y) + ctx->bed_min.y()},
                                         layout[k].rotation};
    ctx->params->on_layout(poses);
}

} // namespace

bool arrange_sparrow(ArrangePolygons &      arrangables,
                     const ArrangePolygons &excludes,
                     const BoundingBox &    bed,
                     const ArrangeParams &  params)
{
    if (arrangables.empty())
        return true; // nothing to place; both backends agree on the empty result
    if (!bed.defined)
        return false;

    // Sparrow has no material metadata. Conservatively use the stock arranger
    // if any movable/fixed objects could require separate plates.
    std::vector<int> temperature_types;
    auto collect_materials = [&](const ArrangePolygons &polygons) {
        for (const ArrangePolygon &ap : polygons) {
            if (ap.is_virt_object)
                continue;
            temperature_types.push_back(ap.filament_temp_type);
        }
    };
    collect_materials(arrangables);
    collect_materials(excludes);
    if (!Print::is_filaments_compatible(temperature_types)) {
        BOOST_LOG_TRIVIAL(info) << "sparrow: material constraints require the stock arranger";
        return false;
    }

    // `bed` is already shrunk by get_shrink_bedpts(); do not shrink again. The edge
    // margin comes from item inflation. If that prevents a near-bed-size part
    // fitting, the stock retry below can relax bed-edge inflation.
    const Point  bed_min = bed.min;
    const double bed_w   = unscaled<double>(bed.max.x() - bed.min.x());
    const double bed_h   = unscaled<double>(bed.max.y() - bed.min.y());
    if (bed_w <= 0. || bed_h <= 0.)
        return false;

    // Point buffers must outlive the FFI call; reserve so nothing is reallocated.
    std::vector<std::vector<sp_point>> pt_store;
    pt_store.reserve(arrangables.size() + excludes.size() + params.excluded_regions.size());

    std::vector<sp_item>    items;
    std::vector<sp_polygon> holes;
    double movable_area = 0.;
    std::vector<size_t>     movable_src; // items[k] -> arrangables[movable_src[k]]
    std::vector<SparrowShapeStorage> shape_store;
    shape_store.reserve(arrangables.size() + excludes.size());

    // Filaments per item, as a mask over 0-based filament indices. Sparrow packs each
    // filament set onto beds of its own and applies the sharing rules set below.
    bool filaments_fit_mask = true;
    auto filament_mask = [&](const ArrangePolygon &ap) {
        uint64_t mask = 0;
        for (int id : ap.extrude_ids) {
            if (id < 1 || id > 64) {
                filaments_fit_mask = false;
                continue;
            }
            mask |= uint64_t(1) << (id - 1);
        }
        return mask;
    };

    auto store_shape = [&](const ArrangePolygon &ap, coord_t infl, const Point &origin,
                           sp_item &it, double *area_mm2 = nullptr) -> bool {
        ExPolygon expanded;
        if (!inflate_expolygon(ap.poly, infl, expanded))
            return false;
        simplify_inflated(ap.poly, infl, expanded); // keeps the full outline if it cannot
        shape_store.emplace_back();
        SparrowShapeStorage &storage = shape_store.back();
        storage.outline = to_sp_points(expanded.contour, origin);
        storage.opening_points.reserve(expanded.holes.size());
        storage.openings.reserve(expanded.holes.size());
        for (const Polygon &opening : expanded.holes) {
            if (opening.points.size() < 3)
                continue;
            storage.opening_points.emplace_back(to_sp_points(opening, origin));
            const auto &pts = storage.opening_points.back();
            storage.openings.push_back(sp_polygon{pts.data(), pts.size()});
        }
        it.outline   = sp_polygon{storage.outline.data(), storage.outline.size()};
        it.openings  = storage.openings.empty() ? nullptr : storage.openings.data();
        it.n_openings = storage.openings.size();
        if (area_mm2)
            *area_mm2 = std::abs(expanded.contour.area()) * SCALING_FACTOR * SCALING_FACTOR;
        return true;
    };

    // ---- movable items -----------------------------------------------------
    // An existing prime tower stays where it is on its own plate; it is pinned with the
    // fixed items below, after the movable ones.
    std::vector<const ArrangePolygon *> towers;
    bool have_filament_info = false;
    for (size_t i = 0; i < arrangables.size(); ++i) {
        const ArrangePolygon &ap = arrangables[i];
        if (ap.is_virt_object && ap.is_wipe_tower) {
            towers.push_back(&ap);
            continue;
        }
        sp_item it{};
        double area_mm2 = 0.;
        if (!store_shape(ap, std::max(ap.inflation, SPARROW_MIN_SEPARATION), Point(0, 0), it, &area_mm2))
            continue; // degenerate outline; process_arrangeable() drops these too
        movable_area += area_mm2;
        it.fixed          = 0;
        it.bed_idx        = -1;
        it.x              = unscaled<double>(ap.translation.x() - bed_min.x());
        it.y              = unscaled<double>(ap.translation.y() - bed_min.y());
        it.rotation       = ap.rotation;
        // allowed_rotations is never populated and libnest2d ignores it too.
        it.allow_rotation = params.allow_rotations ? 1 : 0;
        it.filaments      = filament_mask(ap);
        have_filament_info |= it.filaments != 0;
        items.push_back(it);
        movable_src.push_back(i);
    }
    if (items.empty())
        return false;
    const size_t n_movable = items.size();

    // ---- fixed items and exclusion holes ----------------------------------
    // Zones replicated on every bed become one hole; anything else is a fixed
    // item pinned to its bed.
    std::vector<Polygon>                 virt_world(excludes.size());
    std::vector<std::string>             virt_keys(excludes.size());
    std::map<std::string, std::set<int>> virt_beds;
    for (size_t i = 0; i < excludes.size(); ++i) {
        const ArrangePolygon &ap = excludes[i];
        // Prime towers stay per bed: sparrow reserves one only where its bed prints 2+ filaments.
        if (!ap.is_virt_object || ap.bed_idx < 0 || (ap.is_wipe_tower && !params.prime_tower_always))
            continue;
        virt_world[i] = world_contour(ap);
        virt_keys[i]  = poly_key(virt_world[i]);
        virt_beds[virt_keys[i]].insert(ap.bed_idx);
    }

    // A key present on beds {0..k-1}, k >= 2, is a per-bed replica: collapse it to
    // one hole. Check each key against its own k: callers clone zones over different
    // bed counts (the CLI uses 16 for exclusion zones, 36 for wipe towers).
    std::set<std::string> hole_keys;
    for (const auto &kv : virt_beds) {
        const std::set<int> &beds_seen = kv.second;
        if (beds_seen.size() >= 2 && *beds_seen.begin() == 0 &&
            *beds_seen.rbegin() == int(beds_seen.size()) - 1)
            hole_keys.insert(kv.first);
    }

    // Beds beyond the zone coverage get no holes; libnest2d has the same gap.
    std::set<std::string> emitted_holes;
    auto add_hole = [&](const Polygon &world, const std::string &key, coord_t infl) {
        if (!emitted_holes.insert(key).second)
            return;
        Polygon infl_poly;
        if (!inflate_contour(world, infl, infl_poly))
            return;
        pt_store.emplace_back(to_sp_points(infl_poly, bed_min));
        holes.push_back(sp_polygon{pt_store.back().data(), pt_store.back().size()});
    };

    // Without filament data on the movable items every plate counts as printing a prime tower,
    // as before: the tower is then an obstacle everywhere.
    const bool tower_per_bed = have_filament_info && !params.prime_tower_always;
    auto add_fixed = [&](const ArrangePolygon &ap, coord_t infl) {
        sp_item it{};
        if (!store_shape(ap, infl, Point(0, 0), it))
            return;
        it.fixed          = 1;
        it.bed_idx        = ap.bed_idx;
        it.x              = unscaled<double>(ap.translation.x() - bed_min.x());
        it.y              = unscaled<double>(ap.translation.y() - bed_min.y());
        it.rotation       = ap.rotation;
        it.allow_rotation = 0;
        it.filaments      = filament_mask(ap);
        it.tower          = ap.is_virt_object && ap.is_wipe_tower && tower_per_bed;
        items.push_back(it);
    };
    // Callers may supply a zero-height/width rectangle when no wipe tower is needed. It
    // occupies no area and must not become an invalid hazard that disables the
    // conservative opening cleanup.
    auto empty_tower = [](const ArrangePolygon &ap) {
        const auto size = ap.poly.contour.bounding_box().size();
        return ap.is_virt_object && ap.is_wipe_tower && (size.x() == 0 || size.y() == 0);
    };
    for (const ArrangePolygon *tower : towers)
        if (tower->bed_idx >= 0 && !empty_tower(*tower))
            add_fixed(*tower, fixed_inflation(*tower));
    for (size_t i = 0; i < excludes.size(); ++i) {
        const ArrangePolygon &ap = excludes[i];
        if (ap.bed_idx < 0 || empty_tower(ap))
            continue;
        const coord_t infl = fixed_inflation(ap);
        if (ap.is_virt_object && hole_keys.count(virt_keys[i])) {
            add_hole(virt_world[i], virt_keys[i], infl);
            continue;
        }
        add_fixed(ap, infl);
    }

    // excluded_regions apply to every bed, matching libnest2d's m_excluded_items.
    for (const ArrangePolygon &ap : params.excluded_regions) {
        Polygon w = world_contour(ap);
        add_hole(w, poly_key(w), fixed_inflation(ap));
    }

    // libnest2d only penalizes the calibration strip; the ABI has no soft regions,
    // so it becomes a hard hole here.
    if (params.avoid_extrusion_cali_region) {
        for (const ArrangePolygon &ap : params.nonprefered_regions) {
            Polygon w = world_contour(ap);
            add_hole(w, poly_key(w), fixed_inflation(ap));
        }
    }

    BOOST_LOG_TRIVIAL(info) << "sparrow: bed " << bed_w << "x" << bed_h
                            << " mm, movable=" << n_movable
                            << ", fixed=" << (items.size() - n_movable)
                            << ", holes=" << holes.size()
                            << ", excludes_in=" << excludes.size()
                            << ", excluded_regions=" << params.excluded_regions.size()
                            << ", nonprefered_regions=" << params.nonprefered_regions.size();
    // ---- call the backend --------------------------------------------------
    sp_input in{};
    in.bed_w        = bed_w;
    in.bed_h        = bed_h;
    in.holes        = holes.empty() ? nullptr : holes.data();
    in.n_holes      = holes.size();
    in.items        = items.data();
    in.n_items      = items.size();
    in.max_beds     = MAX_NUM_PLATES; // same cap FirstFitSelection enforces
    in.time_limit_s = params.sparrow_time_limit_s;
    in.seed         = 0;

    // Filaments beyond the 64 the mask holds need the stock arranger. Incompatible filaments
    // never get here: the check at the top hands them to the stock arranger.
    if (!filaments_fit_mask) {
        BOOST_LOG_TRIVIAL(info) << "sparrow: filament ids beyond 64 require the stock arranger";
        return false;
    }
    in.allow_mixing          = params.allow_multi_materials_on_same_plate ? 1 : 0;
    in.max_filaments_per_bed = params.sparrow_max_filaments_per_plate;

    SparrowCallbackCtx cb_ctx;
    cb_ctx.params      = &params;
    cb_ctx.arrangables = &arrangables;
    cb_ctx.movable_src = &movable_src;
    cb_ctx.bed_min     = bed_min;
    if (params.stopcondition) cb_ctx.stopcondition = &params.stopcondition;
    if (params.progressind)   cb_ctx.progressind   = &params.progressind;
    if (params.progress_fraction) cb_ctx.fraction = &params.progress_fraction;
    cb_ctx.budget = params.sparrow_time_limit_s;
    cb_ctx.estimated_plates = std::max(1., std::ceil(movable_area / (bed_w * bed_h)));
    in.user        = &cb_ctx;
    in.should_stop = params.stopcondition || params.progress_fraction ? &sparrow_should_stop_cb : nullptr;
    in.on_progress = params.progressind || params.progress_fraction ? &sparrow_on_progress_cb : nullptr;
    in.on_layout   = params.on_layout ? &sparrow_on_layout_cb : nullptr;

    std::vector<sp_placement> out(items.size());

    // Empty name: the GUI prefixes it with "Arranging". Report 0 before and all
    // placed after, since ArrangeJob feeds the value to update_status(num_finished).
    if (params.progressind) params.progressind(0, "");
    const int rc = ::sparrow_arrange(&in, out.data());
    if (params.progressind) params.progressind(unsigned(n_movable), "");

    if (rc != 0) {
        BOOST_LOG_TRIVIAL(error) << "sparrow_arrange failed with rc=" << rc
                                 << ", falling back to the libnest2d arranger";
        return false;
    }

    std::set<int> used_beds;
    for (size_t k = 0; k < n_movable; ++k)
        used_beds.insert(out[k].bed_idx);
    used_beds.erase(UNARRANGED);
    const size_t unplaced = std::count_if(out.begin(), out.begin() + n_movable, [](const sp_placement &p) { return p.bed_idx == UNARRANGED; });
    BOOST_LOG_TRIVIAL(info) << "sparrow: placed " << n_movable - unplaced << " of " << n_movable << " on " << used_beds.size() << " plates";

    // The stock arranger can reduce bed-edge inflation for near-bed-size parts. Unless
    // sparrow_only, retry the entire input before committing any poses if Sparrow could not
    // place everything (arrange() otherwise has it finish the rest). Never restart a
    // cancelled search.
    if (!params.sparrow_only && unplaced > 0 && !(params.stopcondition && params.stopcondition())) {
        BOOST_LOG_TRIVIAL(info) << "sparrow: unplaced objects require a stock arrange retry";
        return false;
    }

    // ---- write the result back --------------------------------------------
    // Same as arrange()'s tail, minus itemid: sparrow keeps caller ids.
    for (size_t k = 0; k < n_movable; ++k) {
        ArrangePolygon &ap = arrangables[movable_src[k]];
        ap.bed_idx         = out[k].bed_idx;
        if (ap.bed_idx == UNARRANGED)
            continue; // leave translation/rotation untouched, like libnest2d does
        ap.translation = {scaled<coord_t>(out[k].x) + bed_min.x(),
                          scaled<coord_t>(out[k].y) + bed_min.y()};
        ap.rotation    = out[k].rotation;

        // Raw footprint in bed coordinates for the bed and hole checks.
        Polygon w = ap.poly.contour;
        w.rotate(ap.rotation);
        w.translate(ap.translation.x() - bed_min.x(), ap.translation.y() - bed_min.y());
        const BoundingBox wb = w.bounding_box();
        BOOST_LOG_TRIVIAL(debug) << "sparrow: placed \"" << ap.name << "\" bed=" << ap.bed_idx
                                 << " bed-bbox (" << unscaled<double>(wb.min.x()) << ","
                                 << unscaled<double>(wb.min.y()) << ")-("
                                 << unscaled<double>(wb.max.x()) << ","
                                 << unscaled<double>(wb.max.y()) << ")";
    }

    return true;
}

}} // namespace Slic3r::arrangement
