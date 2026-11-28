#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include "Polygon.hpp"
#include "ExPolygon.hpp"
#include "Surface.hpp"
// Adapted from BambuStudio, AGPL-3.0, commit 56a57d00a591360cf41bf21383915ee5bf357be5.
#include "AdaptiveLayerMerge.hpp"

#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "PrintConfig.hpp"
#include "Print.hpp"
#include "Slicing.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Slic3r {

static size_t layer_height_extruder(const PrintConfig &config, unsigned int filament)
{
    const size_t index = filament > 0 ? filament - 1 : 0;
    if ((config.filament_map_mode == fmmManual || config.filament_map_mode == fmmNozzleManual) &&
        index < config.filament_map.size()) {
        const int mapped = config.filament_map.get_at(index);
        if (mapped > 0 && size_t(mapped) <= config.nozzle_diameter.size())
            return size_t(mapped - 1);
    }
    return config.nozzle_diameter.size() == 1 ? 0 : index;
}

void ExtruderLayerHeightRemapper::reset(const std::vector<double> &diameters, const std::vector<double> &heights)
{
    m_diameters = diameters;
    m_last_diameters = diameters;
    m_heights = heights;
}

std::vector<double> ExtruderLayerHeightRemapper::remap(const std::vector<double> &diameters,
                                                     const std::vector<double> &heights)
{
    if (m_diameters.size() != diameters.size() || heights.empty()) {
        reset(diameters, heights);
        return heights;
    }
    if (m_heights != heights) {
        // A process edit establishes a new association with the last observed layout.
        m_diameters = m_last_diameters;
        m_heights = heights;
    }
    m_last_diameters = diameters;
    const size_t count = diameters.size();
    std::vector<size_t> source(count, count);
    std::vector<bool> used(count, false);
    const auto matches = [&](size_t old_slot, size_t new_slot) {
        return std::isfinite(m_diameters[old_slot]) && std::isfinite(diameters[new_slot]) &&
               m_diameters[old_slot] > 0. && diameters[new_slot] > 0. &&
               std::abs(m_diameters[old_slot] - diameters[new_slot]) < 1e-3;
    };
    // Preserve distinct heights of equal-size tools that did not move.
    for (size_t slot = 0; slot < count; ++slot)
        if (matches(slot, slot)) {
            source[slot] = slot;
            used[slot] = true;
        }
    for (size_t slot = 0; slot < count; ++slot) {
        if (source[slot] != count)
            continue;
        for (size_t old_slot = 0; old_slot < count; ++old_slot)
            if (!used[old_slot] && matches(old_slot, slot)) {
                source[slot] = old_slot;
                used[old_slot] = true;
                break;
            }
        if (source[slot] == count)
            return heights; // A partial manual swap or a different installed set; do not guess heights.
    }
    const auto height_at = [&](size_t slot) { return heights[slot < heights.size() ? slot : 0]; };
    std::vector<double> mapped = heights;
    mapped.resize(std::max(count, heights.size()), heights.front());
    bool changed = false;
    for (size_t slot = 0; slot < count; ++slot) {
        mapped[slot] = height_at(source[slot]);
        changed |= mapped[slot] != height_at(slot);
    }
    // Preserve compact/broadcast vectors, including the disabled {0} default, when unchanged.
    if (!changed)
        mapped = heights;
    reset(diameters, mapped);
    return mapped;
}

bool has_extruder_layer_heights(const PrintConfig &config)
{
    return std::any_of(config.extruder_layer_height.values.begin(), config.extruder_layer_height.values.end(),
                       [](double height) { return height > 0.; });
}

bool has_extruder_layer_heights(const PrintConfig &config, const std::vector<unsigned int> &used_filaments)
{
    return std::any_of(used_filaments.begin(), used_filaments.end(), [&](unsigned int filament) {
        return !config.extruder_layer_height.values.empty() &&
               config.extruder_layer_height.get_at(layer_height_extruder(config, filament + 1)) > 0.;
    });
}

bool has_extruder_layer_heights(const PrintObject &object)
{
    return object.slicing_parameters().use_extruder_layer_heights;
}

bool independent_support_layers(const PrintConfig &config, const SlicingParameters &params)
{
    return config.independent_support_layer_height.value || params.use_extruder_layer_heights;
}

double extruder_layer_height_for_filament(const PrintConfig &config, unsigned int filament, double fallback)
{
    const size_t extruder = layer_height_extruder(config, filament);
    const auto &heights = config.extruder_layer_height.values;
    const double height = heights.empty() ? 0. : config.extruder_layer_height.get_at(extruder);
    return height == 0. ? fallback : height;
}

double extruder_layer_height_grid(const PrintConfig &config, double fallback, const std::vector<unsigned int> &used_filaments)
{
    if (!has_extruder_layer_heights(config, used_filaments))
        return fallback;
    // All objects share a micrometre grid, but only tools used by this print contribute to it.
    long long step = 0;
    for (unsigned int filament : used_filaments) {
        const double height = extruder_layer_height_for_filament(config, filament + 1, fallback);
        if (!std::isfinite(height) || height <= 0. || height > 1000.)
            return fallback;
        step = std::gcd(step, std::llround(height * 1000.));
    }
    return double(step) / 1000.;
}

std::vector<double> extruder_support_layer_steps(double span, double height, double minimum)
{
    if (span <= EPSILON)
        return {};
    const size_t count = size_t(std::max(1., std::ceil((span - EPSILON) / height)));
    std::vector<double> steps(count, height);
    steps.back() = span - double(count - 1) * height;
    // Preserve the requested height away from a contact boundary. Share a thin remainder
    // with its neighbor instead of changing every support layer's height.
    if (count > 1 && steps.back() < minimum - EPSILON)
        steps[count - 2] = steps.back() = (height + steps.back()) / 2.;
    return steps;
}

bool AdaptiveLayerHeightRange::contains(double height) const
{
    return height >= min_height - EPSILON && height <= max_height + EPSILON;
}

AdaptiveLayerHeightRange extruder_layer_height_range(const PrintConfig &config, unsigned int filament)
{
    const size_t extruder = layer_height_extruder(config, filament);
    const double nozzle = config.nozzle_diameter.get_at(extruder);
    const double minimum = config.min_layer_height.get_at(extruder);
    const double maximum = config.max_layer_height.get_at(extruder);
    return {minimum > 0. ? minimum : 0.07, maximum > 0. ? maximum : 0.75 * nozzle};
}

ExPolygons adaptive_printable_pieces(ExPolygons pieces, double nozzle_diameter)
{
    const double min_area = 0.25 * nozzle_diameter * nozzle_diameter;
    const float  erosion  = float(scale_(0.5 * nozzle_diameter));
    pieces.erase(std::remove_if(pieces.begin(), pieces.end(), [min_area, erosion](const ExPolygon &piece) {
                     return piece.area() * SCALING_FACTOR * SCALING_FACTOR < min_area ||
                            (erosion > 0.f && shrink_ex(ExPolygons { piece }, erosion).empty());
                 }), pieces.end());
    return pieces;
}

ExPolygons adaptive_printable_core(ExPolygons pieces, double nozzle_diameter)
{
    // The area / erosion test runs first, so degenerate slivers never reach the offset below.
    pieces = adaptive_printable_pieces(std::move(pieces), nozzle_diameter);
    const float radius = float(scale_(0.5 * nozzle_diameter));
    if (pieces.empty() || radius <= 0.f)
        return pieces;
    ExPolygons core;
    for (const ExPolygon &piece : pieces) {
        // Clamp to the original piece: the opening only removes what no bead fits into.
        ExPolygons opened = intersection_ex(opening_ex(ExPolygons{ piece }, radius), ExPolygons{ piece });
        append(core, std::move(opened));
    }
    return adaptive_printable_pieces(std::move(core), nozzle_diameter);
}

ExPolygons adaptive_suppressed_orphan_area(const Layer &layer)
{
    Polygons suppressed;
    Polygons extruded;
    for (const LayerRegion *layerm : layer.regions()) {
        if (layerm->adaptive_merge_patches.empty()) {
            polygons_append(extruded, to_polygons(layerm->slices.surfaces));
            continue;
        }
        for (const AdaptiveMergePatch &patch : layerm->adaptive_merge_patches) {
            // Only an orphan carries an explicit footprint. The whole-region marker of a band is left out: the
            // region's material there lies under another extrusion, which is what stands on that step.
            if (patch.group_top && patch.area().empty() && patch.has_suppressed_area())
                polygons_append(suppressed, to_polygons(patch.suppressed_area()));
            else
                polygons_append(extruded, to_polygons(patch.area()));
        }
    }
    if (suppressed.empty())
        return {};
    // A slab spanning this step may still stand where another region lost its orphan.
    return extruded.empty() ? union_ex(suppressed) : diff_ex(union_ex(suppressed), union_ex(extruded));
}

// Cover [bottom_idx, top_idx] with consecutive bands, every one of them inside the printable window and as
// close to band_size layers as the window allows. Returns false when no legal cover exists, which means the
// span is thinner than a single extrusion of this nozzle.
static bool cover_span_with_bands(
    const std::vector<double>      &layer_heights,
    size_t                          bottom_idx,
    size_t                          top_idx,
    size_t                          band_size,
    const AdaptiveLayerHeightRange &height_range,
    std::vector<AdaptiveLayerBand> &bands)
{
    bands.clear();
    if (bottom_idx > top_idx || top_idx >= layer_heights.size())
        return false;

    const size_t layer_count = top_idx - bottom_idx + 1;
    const size_t unreachable = std::numeric_limits<size_t>::max();
    // cost[i] is how far the bands covering the first i layers stray from band_size in total.
    std::vector<size_t> cost(layer_count + 1, unreachable);
    std::vector<size_t> prev(layer_count + 1, 0);
    cost[0] = 0;
    for (size_t end = 1; end <= layer_count; ++ end) {
        double height = 0.;
        for (size_t begin = end; begin-- > 0;) {
            height += layer_heights[bottom_idx + begin];
            if (height > height_range.max_height + EPSILON)
                break;
            if (cost[begin] == unreachable || ! height_range.contains(height))
                continue;
            const size_t size    = end - begin;
            const size_t penalty = size > band_size ? size - band_size : band_size - size;
            // Ties go to the band found last, i.e. to the thickest one, which is also the fastest.
            if (cost[begin] + penalty <= cost[end]) {
                cost[end] = cost[begin] + penalty;
                prev[end] = begin;
            }
        }
    }
    if (cost[layer_count] == unreachable)
        return false;

    for (size_t end = layer_count; end > 0; end = prev[end]) {
        AdaptiveLayerBand band { bottom_idx + prev[end], bottom_idx + end - 1, 0. };
        for (size_t layer_idx = band.bottom_idx; layer_idx <= band.top_idx; ++ layer_idx)
            band.merged_height += layer_heights[layer_idx];
        bands.emplace_back(band);
    }
    std::reverse(bands.begin(), bands.end());
    return true;
}

// Same cost as cover_span_with_bands(), but a span starting at the object base is laid out from the
// bottom up: on a tie a regular band_size band wins, so the remainder of the stack ends up at the top and
// the first layer of a coarse nozzle no longer depends on the object height.
static bool cover_span_from_base(
    const std::vector<double>      &layer_heights,
    size_t                          bottom_idx,
    size_t                          top_idx,
    size_t                          band_size,
    const AdaptiveLayerHeightRange &height_range,
    std::vector<AdaptiveLayerBand> &bands)
{
    if (bottom_idx != 1)
        return cover_span_with_bands(layer_heights, bottom_idx, top_idx, band_size, height_range, bands);

    bands.clear();
    if (bottom_idx > top_idx || top_idx >= layer_heights.size())
        return false;

    const size_t layer_count = top_idx - bottom_idx + 1;
    const size_t unreachable = std::numeric_limits<size_t>::max();
    // cost[i] is how far the bands covering the layers from i up to the span top stray from band_size.
    std::vector<size_t> cost(layer_count + 1, unreachable);
    std::vector<size_t> next(layer_count + 1, 0);
    cost[layer_count] = 0;
    for (size_t begin = layer_count; begin-- > 0;) {
        double height = 0.;
        for (size_t end = begin + 1; end <= layer_count; ++ end) {
            height += layer_heights[bottom_idx + end - 1];
            if (height > height_range.max_height + EPSILON)
                break;
            if (cost[end] == unreachable || ! height_range.contains(height))
                continue;
            const size_t size    = end - begin;
            const size_t penalty = size > band_size ? size - band_size : band_size - size;
            const size_t total   = cost[end] + penalty;
            // Ties keep a regular band, otherwise go to the thickest one, which is also the fastest.
            if (total < cost[begin] || (total == cost[begin] && next[begin] - begin != band_size)) {
                cost[begin] = total;
                next[begin] = end;
            }
        }
    }
    if (cost[0] == unreachable)
        return false;

    for (size_t begin = 0; begin < layer_count; begin = next[begin]) {
        AdaptiveLayerBand band { bottom_idx + begin, bottom_idx + next[begin] - 1, 0. };
        for (size_t layer_idx = band.bottom_idx; layer_idx <= band.top_idx; ++ layer_idx)
            band.merged_height += layer_heights[layer_idx];
        bands.emplace_back(band);
    }
    return true;
}

std::vector<AdaptiveLayerBand> build_adaptive_layer_bands(
    const std::vector<double>      &layer_heights,
    size_t                          band_size,
    const AdaptiveLayerHeightRange &height_range,
    const std::vector<uint8_t>     &band_tops)
{
    std::vector<AdaptiveLayerBand> bands;
    // The first band starts at the object base. It is still extruded on its top layer, with a height equal
    // to the complete span, so its lower face stays on the bed or on the object's raft/support substrate.
    // Callers propagate that object-base contact separately from the physical group-top layer id.
    const size_t first_layer_idx = 1;
    if (!layer_heights.empty())
        bands.push_back({0, 0, layer_heights.front()});
    if (band_size < 2 || layer_heights.size() < 2)
        return bands;

    const size_t last_idx = layer_heights.size() - 1;
    // Every marked layer closes a segment, and so does the top of the stack. Bands never straddle a
    // segment boundary, which is what keeps a marked layer from being swallowed and decimated away.
    std::vector<size_t> segment_tops;
    for (size_t layer_idx = first_layer_idx; layer_idx < last_idx; ++ layer_idx)
        if (layer_idx < band_tops.size() && band_tops[layer_idx])
            segment_tops.emplace_back(layer_idx);
    segment_tops.emplace_back(last_idx);

    std::vector<AdaptiveLayerBand> segment_bands;
    size_t bottom_idx      = first_layer_idx;
    size_t last_bottom_idx = first_layer_idx;
    size_t last_band_count = 0;
    for (size_t i = 0; i < segment_tops.size(); ++ i) {
        const size_t top_idx = segment_tops[i];
        if (cover_span_from_base(layer_heights, bottom_idx, top_idx, band_size, height_range, segment_bands)) {
            last_bottom_idx = bottom_idx;
            last_band_count = segment_bands.size();
            bands.insert(bands.end(), segment_bands.begin(), segment_bands.end());
            bottom_idx = top_idx + 1;
            continue;
        }
        // Too thin for this nozzle to extrude on its own. Printability outranks the mark, so the segment
        // gives it up and joins the one above by simply not advancing bottom_idx.
        if (i + 1 < segment_tops.size())
            continue;
        // There is nothing above the last segment to join, so take the segment below back and re-cut both
        // at once. This is what keeps the top of the object merged when its tail is shorter than a band.
        if (last_band_count > 0 &&
            cover_span_from_base(layer_heights, last_bottom_idx, top_idx, band_size, height_range, segment_bands)) {
            bands.resize(bands.size() - last_band_count);
            bands.insert(bands.end(), segment_bands.begin(), segment_bands.end());
        }
        // Otherwise the whole stack is thinner than one extrusion of this nozzle and nothing merges.
    }
    // A cover may need single-layer groups to bridge a stretch the window cannot span, but such a group is
    // what not merging looks like, so it is not reported as a band.
    bands.erase(std::remove_if(bands.begin(), bands.end(),
                               [](const AdaptiveLayerBand &band) { return band.top_idx < band.bottom_idx; }),
                bands.end());
    return bands;
}

bool pull_adaptive_band_top_to_material(
    AdaptiveLayerBand                &band,
    const std::vector<double>        &layer_heights,
    const AdaptiveLayerHeightRange   &height_range,
    const std::vector<unsigned char> &has_material)
{
    if (band.bottom_idx > band.top_idx || band.top_idx >= layer_heights.size() ||
        band.top_idx >= has_material.size())
        return false;

    // The grid already placed a printable band here. Only the empty-top case needs a shorter top.
    if (has_material[band.top_idx])
        return true;

    size_t top = band.top_idx;
    while (top > band.bottom_idx && ! has_material[top])
        -- top;
    if (! has_material[top])
        return false;

    double height = 0.;
    for (size_t layer_idx = band.bottom_idx; layer_idx <= top; ++ layer_idx)
        height += layer_heights[layer_idx];
    // A single leftover step is not a band: it is what not merging looks like, and it is often
    // thinner than this nozzle can extrude. The caller then prints that layer unmerged, if at all.
    if (top <= band.bottom_idx || ! height_range.contains(height))
        return false;

    band.top_idx       = top;
    band.merged_height = height;
    return true;
}

bool split_adaptive_layer_span(
    const std::vector<double>              &layer_heights,
    size_t                                  bottom_idx,
    size_t                                  top_idx,
    size_t                                  preferred_band_size,
    const AdaptiveLayerHeightRange         &height_range,
    std::vector<std::pair<size_t, size_t>> &groups)
{
    groups.clear();
    if (bottom_idx > top_idx || top_idx >= layer_heights.size() || preferred_band_size == 0)
        return false;
    const size_t layer_count = top_idx - bottom_idx + 1;
    // cost[i] is the total distance from the preferred band size over the first i layers.
    const size_t unreachable = std::numeric_limits<size_t>::max();
    std::vector<size_t> cost(layer_count + 1, unreachable);
    std::vector<size_t> prev(layer_count + 1, 0);
    cost[0] = 0;
    for (size_t end = 1; end <= layer_count; ++end) {
        double height = 0.;
        for (size_t begin = end; begin-- > 0;) {
            height += layer_heights[bottom_idx + begin];
            if (height > height_range.max_height + EPSILON)
                break;
            if (cost[begin] == unreachable || ! height_range.contains(height))
                continue;
            const size_t size    = end - begin;
            const size_t penalty = size > preferred_band_size ? size - preferred_band_size :
                                                                  preferred_band_size - size;
            // Prefer the thicker final band when two covers deviate equally from the fixed grid.
            if (cost[begin] + penalty <= cost[end]) {
                cost[end] = cost[begin] + penalty;
                prev[end] = begin;
            }
        }
    }
    if (cost[layer_count] == unreachable)
        return false;

    for (size_t end = layer_count; end > 0; end = prev[end])
        groups.emplace_back(bottom_idx + prev[end], bottom_idx + end - 1);
    std::reverse(groups.begin(), groups.end());
    return true;
}

} // namespace Slic3r
