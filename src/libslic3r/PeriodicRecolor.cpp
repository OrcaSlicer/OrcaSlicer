#include "PeriodicRecolor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>

#include <boost/log/trivial.hpp>

#include "ExtrusionEntityCollection.hpp"
#include "Layer.hpp"
#include "Print.hpp"

namespace Slic3r {

// The lowest period a pattern may have.
static constexpr double MIN_PERIOD = 0.01;

// The longest distance expected between start and end.
static constexpr double MAX_SPAN_MM = 1000.;

// Most bands a pattern may have, which bounds the work of placing them.
static constexpr size_t MAX_BANDS_PER_PATTERN = size_t(MAX_SPAN_MM / MIN_PERIOD);

// Half a nanometer (half of SCALING_FACTOR_INTERNAL). Heights are rounded to multiples of it, so floating point
// drift cannot change which layer a band lands on at a layer boundary.
static constexpr double Z_QUANTUM = 0.0000005;

// Largest quantized value, leaving room to double it (Middle compares doubled heights).
static constexpr int64_t ZQ_LIMIT = int64_t(1) << 61;

// Rounds a height to the Z_QUANTUM grid, clamped to [-ZQ_LIMIT, ZQ_LIMIT].
static int64_t zq(double z)
{
    const double u = z / Z_QUANTUM;
    return u >=  double(ZQ_LIMIT) ?  ZQ_LIMIT :
           u <= -double(ZQ_LIMIT) ? -ZQ_LIMIT : int64_t(std::llround(u));
}

// Adds two non-negative quantized heights, clamped to ZQ_LIMIT.
static int64_t zq_add(int64_t a, int64_t b)
{
    return b > ZQ_LIMIT - a ? ZQ_LIMIT : a + b;
}

// Whether an extrusion's role matches the pattern's; an outer wall pattern also matches overhang walls.
static bool periodic_recolor_roles_match(ExtrusionRole selected_role, ExtrusionRole actual_role)
{
    return selected_role == erExternalPerimeter ?
               actual_role == erExternalPerimeter || actual_role == erOverhangPerimeter :
               selected_role == actual_role;
}

// ---------------------------------------------------------------------------------------
// Pattern operations
// ---------------------------------------------------------------------------------------

bool PeriodicRecolorPattern::is_valid(size_t num_filaments) const
{
    if (this->filament < 1 || size_t(this->filament) > num_filaments)
        return false;
    if (! contains(PERIODIC_RECOLOR_ROLES, this->role))
        return false;
    if (! std::isfinite(this->start) || ! std::isfinite(this->end) ||
        ! std::isfinite(this->band_vertical_height) || ! std::isfinite(this->period))
        return false;
    if (this->band_vertical_height <= 0. || this->period < MIN_PERIOD)
        return false;
    if (this->start < 0.)
        return false;
    if (this->end < this->start)
        return false;
    // Reject patterns with more than MAX_BANDS_PER_PATTERN marks from start to end.
    const int64_t period_q = std::max<int64_t>(zq(this->period), 1);
    if ((zq(this->end) - zq(this->start)) / period_q >= int64_t(MAX_BANDS_PER_PATTERN))
        return false;
    return true;
}

void PeriodicRecolorPatterns::collect_filaments(size_t num_filaments, std::vector<unsigned int> &out_zero_based) const
{
    for (const PeriodicRecolorPattern &pattern : this->patterns)
        if (pattern.enabled && pattern.is_valid(num_filaments))
            out_zero_based.emplace_back(unsigned(pattern.filament - 1));
}

void PeriodicRecolorPatterns::delete_filament(size_t deleted, int replacement)
{
    const int deleted_1based = int(deleted) + 1;
    for (PeriodicRecolorPattern &pattern : this->patterns) {
        if (pattern.filament > deleted_1based)
            --pattern.filament;
        else if (pattern.filament == deleted_1based) {
            // Disabled rather than removed, so the user can retarget it.
            if (replacement < 0)
                pattern.enabled = false;
            else
                pattern.filament = replacement + 1;
        }
    }
}

static const size_t PERIODIC_FIELDS_PER_PATTERN = 8;

// The alignments a pattern may name.
static constexpr std::array<PeriodicRecolorAlignment, 3> PERIODIC_RECOLOR_ALIGNMENTS = {{
    PeriodicRecolorAlignment::Bottom,
    PeriodicRecolorAlignment::Middle,
    PeriodicRecolorAlignment::Top,
}};

std::vector<double> PeriodicRecolorPatterns::to_doubles() const
{
    std::vector<double> out;
    out.reserve(this->patterns.size() * PERIODIC_FIELDS_PER_PATTERN);
    for (const PeriodicRecolorPattern &pattern : this->patterns) {
        out.push_back(pattern.enabled ? 1. : 0.);
        out.push_back(double(pattern.filament));
        out.push_back(double(int(pattern.role)));
        out.push_back(pattern.start);
        out.push_back(pattern.end);
        out.push_back(pattern.band_vertical_height);
        out.push_back(pattern.period);
        out.push_back(double(int(pattern.alignment)));
    }
    return out;
}

PeriodicRecolorPatterns PeriodicRecolorPatterns::from_doubles(const std::vector<double> &v)
{
    PeriodicRecolorPatterns out;
    // An empty value deserializes to a single 0, which holds no patterns either.
    if (v.size() <= 1)
        return out;
    if (v.size() % PERIODIC_FIELDS_PER_PATTERN != 0) {
        BOOST_LOG_TRIVIAL(error) << "periodic_recolor_patterns: expected " << PERIODIC_FIELDS_PER_PATTERN
                                 << "N values, got " << v.size();
        return out;
    }

    for (size_t i = 0; i < v.size(); i += PERIODIC_FIELDS_PER_PATTERN) {
        PeriodicRecolorPattern pattern;
        pattern.enabled  = v[i] != 0.;
        const double filament = v[i + 1];
        if (! (filament >= double(std::numeric_limits<int>::min()) &&
               filament <= double(std::numeric_limits<int>::max())))
            continue;
        pattern.filament = int(filament);
        const double role = v[i + 2];
        if (! (role >= 0. && role < double(int(erCount))))
            continue;
        pattern.role = ExtrusionRole(int(role));
        if (! contains(PERIODIC_RECOLOR_ROLES, pattern.role))
            continue;
        pattern.start                = v[i + 3];
        pattern.end                  = v[i + 4];
        pattern.band_vertical_height = v[i + 5];
        pattern.period               = v[i + 6];
        const double alignment = v[i + 7];
        const auto match = std::find_if(PERIODIC_RECOLOR_ALIGNMENTS.begin(), PERIODIC_RECOLOR_ALIGNMENTS.end(),
                                        [&](PeriodicRecolorAlignment e) { return alignment == double(int(e)); });
        if (match == PERIODIC_RECOLOR_ALIGNMENTS.end())
            continue;
        pattern.alignment = *match;
        out.patterns.emplace_back(pattern);
    }
    return out;
}

// ---------------------------------------------------------------------------------------
// Layer rules
// ---------------------------------------------------------------------------------------

// Whether any extrusion inside `entity` has a role that `selected_role` matches.
static bool periodic_recolor_contains_selected_role(const ExtrusionEntity &entity, ExtrusionRole selected_role)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            if (periodic_recolor_contains_selected_role(*child, selected_role))
                return true;
        return false;
    }
    return periodic_recolor_roles_match(selected_role, entity.role());
}

int PeriodicRecolorLayerRules::first_matching_filament(const ExtrusionEntity &entity) const
{
    // Patterns are checked in order; the first one on this layer whose feature is present wins.
    for (const auto &rule : m_rules)
        if (periodic_recolor_contains_selected_role(entity, rule.first))
            return rule.second;
    return -1;
}

// ---------------------------------------------------------------------------------------
// Plan: which layers each pattern covers
// ---------------------------------------------------------------------------------------

PeriodicRecolorPlan PeriodicRecolorPlan::build(const PrintObject &object)
{
    PeriodicRecolorPlan plan;
    const PeriodicRecolorPatterns patterns =
        PeriodicRecolorPatterns::from_doubles(object.config().periodic_recolor_patterns.values);
    const auto layers = object.layers();
    if (patterns.patterns.empty() || layers.empty())
        return plan;

    const double object_print_z_min = object.slicing_parameters().object_print_z_min;
    const size_t num_filaments      = object.print()->config().filament_diameter.size();

    // Quantized layer tops, measured from object_print_z_min, so a raft does not shift the bands.
    std::vector<int64_t> layer_tops_q(layers.size());
    for (size_t i = 0; i < layers.size(); ++i)
        layer_tops_q[i] = zq(layers[i]->print_z - object_print_z_min);

    // `layers` must be sorted by print_z.
    assert(std::is_sorted(layer_tops_q.begin(), layer_tops_q.end()));

    const int64_t object_top_q = layer_tops_q.back();

    // Quantized bottom of layer t: the previous layer's top, or 0 (the object's bottom) for the first layer.
    auto layer_bottom_q = [&layer_tops_q](size_t t) -> int64_t { return t == 0 ? int64_t(0) : layer_tops_q[t - 1]; };
    // Quantized thickness of layer t.
    auto layer_height_q = [&layer_tops_q, &layer_bottom_q](size_t t) -> int64_t {
        return layer_tops_q[t] - layer_bottom_q(t); };

    // Copy the layers into the plan, and find the thinnest layer height, which sets the most
    // layers a band can span.
    double h_min = std::numeric_limits<double>::max();
    plan.m_layers.resize(layers.size());
    for (size_t i = 0; i < layers.size(); ++i) {
        plan.m_layers[i].print_z = layers[i]->print_z;
        plan.m_layers[i].height  = layers[i]->height;
        h_min = std::min(h_min, layers[i]->height);
    }

    for (const PeriodicRecolorPattern &pattern : patterns.patterns) {
        if (! pattern.enabled || ! pattern.is_valid(num_filaments))
            continue;

        const std::pair<ExtrusionRole, int> rule(pattern.role, pattern.filament - 1);
        // Adds this pattern's rule to a layer once. Patterns are placed one after another, so a rule this pattern has
        // already added is still the layer's last one.
        auto claim = [&plan, &rule](size_t layer_idx) {
            auto &layer_rules = plan.m_layers[layer_idx].rules.m_rules;
            if (layer_rules.empty() || layer_rules.back() != rule)
                layer_rules.push_back(rule);
        };

        const int64_t start_q       = zq(pattern.start);
        const int64_t band_height_q = zq(pattern.band_vertical_height);
        const int64_t period_q      = std::max<int64_t>(zq(pattern.period), 1);

        // How far a band reaches below its mark.
        const int64_t below = pattern.alignment == PeriodicRecolorAlignment::Top    ? band_height_q :
                              pattern.alignment == PeriodicRecolorAlignment::Middle ? band_height_q / 2 : int64_t(0);
        // How far a band reaches above its mark.
        const int64_t above = band_height_q - below;
        // Lowest height a mark may sit at with this alignment. Ignores pattern.start.
        const int64_t floor_mark_q = pattern.alignment == PeriodicRecolorAlignment::Bottom ?
            int64_t(0) : layer_tops_q.front() - layer_height_q(0) / 2;
        // Highest height a mark may sit at with this alignment, ignoring pattern.end: half the last layer above the
        // object's top, and low enough that the band's bottom is still below the top.
        const int64_t ceiling_mark_q = std::min(object_top_q + layer_height_q(layers.size() - 1) / 2,
                                                object_top_q + below - 1);
        // The same, with pattern.end applied.
        const int64_t last_mark_q = std::min(zq(pattern.end), ceiling_mark_q);

        // The first mark from pattern.start that is at or above floor_mark_q.
        int64_t first_mark_q = start_q;
        if (first_mark_q < floor_mark_q)
            first_mark_q += ((floor_mark_q - first_mark_q) + period_q - 1) / period_q * period_q;

        if (first_mark_q > last_mark_q)
            continue;
        // How many steps of `period` from first to last mark.
        const int64_t steps_q = (last_mark_q - first_mark_q) / period_q;
        // The last mark on the period grid at or below last_mark_q.
        const int64_t final_q = first_mark_q + steps_q * period_q;
        // The longest run worth trying: the thinnest layer decides how many fit inside the band.
        const size_t n_max = size_t(std::min(std::ceil(pattern.band_vertical_height / h_min) + 1.,
                                             double(layers.size())));

        // Places the band for one mark and claims the layers it covers.
        auto place_mark = [&](int64_t mark_q) {
            const int64_t low  = mark_q - below;
            const int64_t high = mark_q + above;

            // The first layer whose top is at or above the mark.
            const size_t p = size_t(std::lower_bound(layer_tops_q.begin(), layer_tops_q.end(), mark_q) -
                                    layer_tops_q.begin());

            size_t seed = 0;
            if (pattern.alignment == PeriodicRecolorAlignment::Bottom) {
                // Start at the next layer when the mark is closer to this layer's top than its bottom.
                seed = (p < layers.size() && mark_q - layer_bottom_q(p) > layer_tops_q[p] - mark_q) ? p + 1 : p;
                seed = std::min(seed, layers.size() - 1);
            } else if (pattern.alignment == PeriodicRecolorAlignment::Top) {
                // End at the previous layer when the mark is at least as close to this layer's bottom as its top.
                size_t q = p;
                if (q == layers.size())
                    q = layers.size() - 1;
                else if (q > 0 && mark_q - layer_tops_q[q - 1] <= layer_tops_q[q] - mark_q)
                    -- q;
                seed = q;
            }

            // The first layer of the run of `n` layers this alignment places for this mark; the run ends
            // at run_low + n - 1. `n` never exceeds the layers available.
            auto place = [&](size_t n) -> size_t {
                if (pattern.alignment == PeriodicRecolorAlignment::Top)
                    return seed - (n - 1);
                if (pattern.alignment == PeriodicRecolorAlignment::Bottom)
                    return seed;
                // Middle: binary search for the run whose center is closest to the mark. A run's center is
                // (bottom(i) + top(i+n-1)) / 2. Compare the sum with 2 * mark rather than dividing, which
                // would round.
                const int64_t mark2 = 2 * mark_q;
                // Search bounds for the first layer of the run.
                size_t lo_i = 0, hi_i = layers.size() - n;
                while (lo_i < hi_i) {
                    const size_t mid_i = (lo_i + hi_i) / 2;
                    if (layer_bottom_q(mid_i) + layer_tops_q[mid_i + n - 1] < mark2)
                        lo_i = mid_i + 1;
                    else
                        hi_i = mid_i;
                }
                // Keep whichever of the runs starting at lo_i and lo_i - 1 is centered closer to the mark.
                // A tie takes the lower run.
                if (lo_i > 0) {
                    const int64_t c_high = layer_bottom_q(lo_i)     + layer_tops_q[lo_i + n - 1];
                    const int64_t c_low  = layer_bottom_q(lo_i - 1) + layer_tops_q[lo_i + n - 2];
                    if (mark2 - c_low <= c_high - mark2)
                        -- lo_i;
                }
                return lo_i;
            };
            // The quantized thickness of a run of `n` layers starting at run_low.
            auto run_height_q = [&](size_t run_low, size_t n) {
                return layer_tops_q[run_low + n - 1] - layer_bottom_q(run_low);
            };

            // Try runs of 1, 2, ... layers, up to n_max and as many as fit, and keep the one whose height is
            // closest to the band thickness. A tie keeps the thinner run, which recolors fewer layers.
            const size_t n_limit = pattern.alignment == PeriodicRecolorAlignment::Top    ? seed + 1 :
                                   pattern.alignment == PeriodicRecolorAlignment::Bottom ? layers.size() - seed :
                                                                                           layers.size();
            size_t  best_low = place(1), best_n = 1;
            int64_t best_err = std::llabs(run_height_q(best_low, best_n) - band_height_q);
            for (size_t n = 2; n <= std::min(n_max, n_limit); ++n) {
                const size_t  run_low    = place(n);
                const int64_t run_height = run_height_q(run_low, n);
                if (std::llabs(run_height - band_height_q) < best_err) {
                    best_err = std::llabs(run_height - band_height_q);
                    best_low = run_low;
                    best_n   = n;
                }
                if (run_height >= band_height_q)
                    // Once the run height passes the band height, longer runs only drift further from it.
                    break;
            }

            // When the run touches the first or last layer, drop end layers that lie wholly outside the band:
            // from the top, layers whose bottom is at or above the band top, and from the bottom, layers whose
            // top is at or below the band bottom. At least one layer is kept.
            size_t claim_low = best_low, claim_high = best_low + best_n - 1;
            if (claim_low == 0 || claim_high == layers.size() - 1) {
                while (claim_high > claim_low && layer_bottom_q(claim_high) >= high)
                    -- claim_high;
                while (claim_low < claim_high && layer_tops_q[claim_low] <= low)
                    ++ claim_low;
            }
            for (size_t t = claim_low; t <= claim_high; ++t)
                claim(t);
        };

        // Merge into one long span when a pattern's bands overlap (thickness >= period).
        if (pattern.band_vertical_height >= pattern.period) {
            const int64_t span_low  = first_mark_q - below;
            const int64_t span_high = final_q      + above;
            for (size_t i = 0; i < layers.size(); ++i) {
                // Claim every layer that overlaps the span, including layers that cross its edges.
                if (layer_tops_q[i] <= span_low)
                    continue;
                if (layer_bottom_q(i) >= span_high)
                    continue;
                claim(i);
            }
        } else {
            int64_t mark_q = first_mark_q;
            for (int64_t k = 0; k <= steps_q; ++k, mark_q = zq_add(mark_q, period_q))
                place_mark(mark_q);
        }
    }

    // Drop layers no pattern covers.
    plan.m_layers.erase(std::remove_if(plan.m_layers.begin(), plan.m_layers.end(),
                                       [](const Entry &e) { return ! e.rules.active(); }),
                        plan.m_layers.end());
    return plan;
}

const PeriodicRecolorLayerRules &PeriodicRecolorPlan::rules_for(double print_z, double height) const
{
    auto it = std::lower_bound(m_layers.begin(), m_layers.end(), print_z,
                               [](const Entry &e, double value) { return e.print_z < value; });
    for (; it != m_layers.end() && it->print_z == print_z; ++it)
        if (it->height == height)
            return it->rules;
    // No entry for this layer.
    static const PeriodicRecolorLayerRules none;
    return none;
}

std::vector<std::pair<double, double>> periodic_recolor_ideal_bands(const PeriodicRecolorPattern &pattern,
                                                                   double object_height,
                                                                   size_t max_bands)
{
    std::vector<std::pair<double, double>> out;
    if (pattern.band_vertical_height <= 0. || pattern.period <= 0. || object_height <= 0.)
        return out;

    const double lo = pattern.start;
    const double below = pattern.alignment == PeriodicRecolorAlignment::Top    ? pattern.band_vertical_height :
                         pattern.alignment == PeriodicRecolorAlignment::Middle ? 0.5 * pattern.band_vertical_height :
                         0.;
    const double above = pattern.band_vertical_height - below;

    const int64_t start_q     = zq(lo);
    const int64_t period_q    = std::max<int64_t>(zq(pattern.period), 1);
    const int64_t last_mark_q = std::min(zq(pattern.end), zq(object_height));
    if (start_q > last_mark_q)
        return out;

    const int64_t count = (last_mark_q - start_q) / period_q + 1;
    const size_t n_bands = size_t(std::min<int64_t>(count, int64_t(max_bands)));

    out.reserve(n_bands);
    int64_t mark_q = start_q;
    for (size_t k = 0; k < n_bands; ++k, mark_q = zq_add(mark_q, period_q)) {
        const double mark = double(mark_q) * Z_QUANTUM;
        const double band_lo = std::max(mark - below, 0.);
        const double band_hi = std::min(mark + above, object_height);
        if (band_lo < band_hi)
            out.emplace_back(band_lo, band_hi);
    }
    return out;
}

PeriodicRecolorPatterns periodic_recolor_patterns_of(const ConfigBase &config)
{
    const auto *opt = config.option<ConfigOptionFloats>("periodic_recolor_patterns");
    if (opt == nullptr)
        return {};
    return PeriodicRecolorPatterns::from_doubles(opt->values);
}

void periodic_recolor_append_targets(const PrintObject &object, std::vector<unsigned int> &out)
{
    const size_t num_filaments = object.print()->config().filament_diameter.values.size();
    PeriodicRecolorPatterns::from_doubles(object.config().periodic_recolor_patterns.values)
        .collect_filaments(num_filaments, out);
}

} // namespace Slic3r
