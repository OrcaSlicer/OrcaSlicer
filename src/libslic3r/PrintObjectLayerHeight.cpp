#include <string>
#include "Flow.hpp"
#include "Polygon.hpp"
#include "ExPolygon.hpp"
// Adapted from BambuStudio, AGPL-3.0, commit 56a57d00a591360cf41bf21383915ee5bf357be5.
#include "AdaptiveLayerMerge.hpp"
#include "ClipperUtils.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Surface.hpp"
#include "SurfaceCollection.hpp"
#include "BoundingBox.hpp"
#include "libslic3r.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/log/trivial.hpp>

namespace Slic3r {
// Regions share the used tools' fine slicing grid. Each keeps contours at the tops of
// bands matching its requested layer height, with extrusion flow covering the full band.
void PrintObject::compute_adaptive_merge_groups()
{
    for (Layer *layer : m_layers)
        for (LayerRegion *layerm : layer->regions())
            layerm->adaptive_merge_patches.clear();

    // Unused tools must not activate per-extruder layer planning.
    if (! has_extruder_layer_heights(*this))
        return;

    const PrintConfig &print_config = this->print()->config();
    for (Layer *layer : m_layers)
        for (LayerRegion *region : layer->regions())
            if (region->raw_slices.empty())
                region->raw_slices = to_expolygons(region->slices.surfaces);
    const size_t       region_count = this->num_printing_regions();
    const size_t       layer_count  = m_layers.size();
    if (region_count == 0 || layer_count < 2)
        return;

    std::vector<double> layer_heights;
    layer_heights.reserve(layer_count);
    for (const Layer *layer : m_layers)
        layer_heights.emplace_back(layer->height);

    std::vector<double> region_nozzles(region_count, 0.);
    for (size_t region_id = 0; region_id < region_count; ++ region_id) {
        const PrintRegionConfig &config = this->printing_region(region_id).config();
        const unsigned int filament = this->printing_region(region_id).extruder(frExternalPerimeter);
        region_nozzles[region_id] = nozzle_diameter_for_filament(print_config, filament, this->print()->is_BBL_printer());
    }
    const double min_nozzle = m_slicing_params.layer_height;
    if (min_nozzle <= EPSILON)
        return;

    // A colour transition changes ownership inside a band, not the nozzle's Z ladder. The normalization
    // pass has already aligned painted ownership to each band's anchor contour.
    const std::vector<uint8_t> band_tops;

    BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] layers=" << layer_count << " min_nozzle=" << min_nozzle
                             << " band_tops=none (fixed nozzle ladders)";

    // Plan every region on the fixed ladder shared by its nozzle diameter, then arbitrate the resulting
    // slabs from bottom to top. Colours never create a new ladder phase.
    struct MergeEvent {
        size_t                   region_id;
        double                   nozzle;
        AdaptiveLayerHeightRange range;
        AdaptiveLayerBand        band;
        size_t                    source_bottom_idx;
        bool                      merged;
    };
    std::vector<MergeEvent> events;
    auto span_height = [&layer_heights](size_t bottom_idx, size_t top_idx) {
        double height = 0.;
        for (size_t layer_idx = bottom_idx; layer_idx <= top_idx; ++ layer_idx)
            height += layer_heights[layer_idx];
        return height;
    };
    // Keep the previous slab's floor and the tail's top fixed. Borrow only enough steps from the
    // previous slab to make the tail printable, e.g. 0.5 + 0.1 becomes 0.4 + 0.2 for a 0.8 nozzle.
    // Callers try this only once growing a neighbour inside the preferred window has failed.
    // A slab resting on the bed belongs to the first band, whose height is fixed; it never donates steps.
    auto split_before_tail = [&](size_t bottom_idx, size_t previous_top_idx, size_t tail_top_idx,
                                 const AdaptiveLayerHeightRange &range, size_t &split_top_idx) {
        if (bottom_idx == 0 || bottom_idx >= previous_top_idx || previous_top_idx >= tail_top_idx)
            return false;
        double tail_height = span_height(previous_top_idx + 1, tail_top_idx);
        if (tail_height >= range.min_height - EPSILON)
            return false;
        size_t tail_bottom_idx = previous_top_idx + 1;
        while (tail_bottom_idx > bottom_idx && tail_height < range.min_height - EPSILON)
            tail_height += layer_heights[-- tail_bottom_idx];
        if (tail_bottom_idx == bottom_idx || ! range.contains(tail_height) ||
            ! range.contains(span_height(bottom_idx, tail_bottom_idx - 1)))
            return false;
        split_top_idx = tail_bottom_idx - 1;
        return true;
    };
    // A run of steps a region owns that no band of its nozzle could take. It is carried to the end of
    // planning as an explicit record instead of being suppressed right away, because absorbing it needs to
    // know the space the other nozzles occupy. Recovering it afterwards by subtracting the planned
    // extrusions from the raw slices would also report the hairline by which every step inside a band
    // differs from that band's anchor contour, and those hairlines are not holes.
    struct PendingOrphan {
        size_t                   region_id;
        AdaptiveLayerHeightRange range;
        size_t                   bottom_idx;
        size_t                   top_idx;
        // "no-band" - no band of this nozzle covers the step. "too-thin" - a band covers it, but everything
        // under the step is occupied and the clear span left is thinner than this nozzle can extrude.
        const char              *source;
        // What was lost. Recovery takes back only the column over it, so the rest of the slab that takes it
        // keeps its own span.
        ExPolygons               area;
        // A too-thin bottom face resting on nothing. It is dropped rather than absorbed: the slab above keeps
        // its own floor, and the step above the face is typed as the bridge over it instead.
        bool                     floating { false };
    };
    std::vector<PendingOrphan> pending_orphans;
    for (size_t region_id = 0; region_id < region_count; ++ region_id) {
        const double nozzle = region_nozzles[region_id];
        if (nozzle <= EPSILON)
            continue;
        const AdaptiveLayerHeightRange range = extruder_layer_height_range(print_config, this->printing_region(region_id).extruder(frExternalPerimeter));
        const size_t band_size = size_t(std::max(1LL, std::llround(extruder_layer_height_for_filament(print_config, this->printing_region(region_id).extruder(frExternalPerimeter), m_config.layer_height.value) / m_slicing_params.layer_height)));

        std::vector<unsigned char> has_material(layer_count, 0);
        for (size_t layer_idx = 0; layer_idx < layer_count; ++ layer_idx)
            has_material[layer_idx] = ! m_layers[layer_idx]->get_region(region_id)->raw_slices.empty();

        if (band_size < 2) {
            // This nozzle extrudes the slicing grid itself, so it prints every layer exactly as sliced.
            for (size_t layer_idx = 0; layer_idx < layer_count; ++ layer_idx)
                if (has_material[layer_idx])
                    events.push_back({ region_id, nozzle, range,
                                       { layer_idx, layer_idx, layer_heights[layer_idx] },
                                       layer_idx, false });
            BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] region=" << region_id << " nozzle=" << nozzle
                                     << " band_size=1 -> prints every layer";
            continue;
        }

        // One diameter owns one ladder. Region/colour start layers do not create a new phase.
        const std::vector<AdaptiveLayerBand> bands =
            build_adaptive_layer_bands(layer_heights, band_size, range, band_tops);

        std::vector<AdaptiveLayerBand> live_bands;
        std::vector<unsigned char>     covered(layer_count, 0);
        for (const AdaptiveLayerBand &band : bands) {
            AdaptiveLayerBand live = band;
            // The first band is a snapshot of the object base and keeps its grid top whatever it holds.
            if (band.bottom_idx != 0 &&
                ! pull_adaptive_band_top_to_material(live, layer_heights, range, has_material))
                continue;
            for (size_t layer_idx = live.bottom_idx; layer_idx <= live.top_idx; ++ layer_idx)
                covered[layer_idx] = 1;
            live_bands.emplace_back(live);
        }

        // Layers this region owns that no band covers: a colour tail ending inside a band, or a step the
        // ladder had to bridge with a single-layer group. A run the nozzle can extrude gets bands of its own.
        // Printing a thinner one as sliced is what put a 0.1 mm extrusion under a 0.6 nozzle, so it is folded
        // into a neighbouring band while that band's height stays printable, and otherwise recorded as an
        // orphan for the resolution pass below.
        std::string orphans;
        for (size_t layer_idx = 0; layer_idx < layer_count; ) {
            if (covered[layer_idx] || ! has_material[layer_idx]) {
                ++ layer_idx;
                continue;
            }
            size_t run_top = layer_idx;
            while (run_top + 1 < layer_count && ! covered[run_top + 1] && has_material[run_top + 1])
                ++ run_top;
            orphans += "[" + std::to_string(layer_idx) + "," + std::to_string(run_top) + "]";

            // A run this nozzle can extrude on its own is not thin, it is just off the ladder: 0.1 under a 0.4
            // nozzle. It is cut into printable bands of its own, which then go through arbitration like every
            // other band. Only a run thinner than one extrusion is left to the repairs below.
            std::vector<std::pair<size_t, size_t>> own_bands;
            if (split_adaptive_layer_span(layer_heights, layer_idx, run_top, band_size, range, own_bands)) {
                for (const std::pair<size_t, size_t> &own : own_bands)
                    live_bands.push_back({ own.first, own.second, span_height(own.first, own.second) });
                for (size_t idx = layer_idx; idx <= run_top; ++ idx)
                    covered[idx] = 1;
                layer_idx = run_top + 1;
                continue;
            }

            AdaptiveLayerBand *below = nullptr;
            AdaptiveLayerBand *above = nullptr;
            for (AdaptiveLayerBand &band : live_bands) {
                if (layer_idx > 0 && band.top_idx == layer_idx - 1)
                    below = &band;
                if (band.bottom_idx == run_top + 1)
                    above = &band;
            }
            // Growing the band above pulls its floor down; growing the one below lifts this colour's top face
            // by the run. Either keeps the material, so prefer them over giving the run away. The band above
            // goes first: it keeps its group top, so no new print Z appears off the ladder.
            auto absorb = [&](double ceiling) {
                if (above != nullptr) {
                    const double height = span_height(layer_idx, above->top_idx);
                    if (height >= range.min_height - EPSILON && height <= ceiling + EPSILON) {
                        above->bottom_idx    = layer_idx;
                        above->merged_height = height;
                        return true;
                    }
                }
                if (below != nullptr) {
                    const double height = span_height(below->bottom_idx, run_top);
                    if (height >= range.min_height - EPSILON && height <= ceiling + EPSILON) {
                        below->top_idx       = run_top;
                        below->merged_height = height;
                        return true;
                    }
                }
                return false;
            };
            // Re-cut the band below and the run into two slabs inside the window, keeping both outer faces.
            auto split = [&]() {
                size_t split_top_idx;
                if (below == nullptr ||
                    ! split_before_tail(below->bottom_idx, below->top_idx, run_top, range, split_top_idx))
                    return false;
                below->top_idx       = split_top_idx;
                below->merged_height = span_height(below->bottom_idx, split_top_idx);
                // Invalidates below and above, so nothing may follow a successful split.
                live_bands.push_back({ split_top_idx + 1, run_top, span_height(split_top_idx + 1, run_top) });
                return true;
            };
            // Prefer a whole slab before re-cutting the run with its neighbor, within the printer limits.
            if (absorb(range.max_height) || split())
                for (size_t idx = layer_idx; idx <= run_top; ++ idx)
                    covered[idx] = 1;
            else {
                Polygons run_area;
                for (size_t idx = layer_idx; idx <= run_top; ++ idx)
                    polygons_append(run_area, to_polygons(m_layers[idx]->get_region(region_id)->raw_slices));
                pending_orphans.push_back({ region_id, range, layer_idx, run_top, "no-band",
                                            union_ex(run_area) });
            }
            layer_idx = run_top + 1;
        }

        std::string span;
        for (const AdaptiveLayerBand &band : live_bands) {
            span += "[" + std::to_string(band.bottom_idx) + "," + std::to_string(band.top_idx) + "]";
            events.push_back({ region_id, nozzle, range, band, band.bottom_idx, true });
        }

        BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] region=" << region_id << " nozzle=" << nozzle
                                 << " band_size=" << band_size
                                 << " range=[" << range.min_height << "," << range.max_height << "]"
                                 << " bands=" << live_bands.size() << " " << span
                                 << " orphans=" << (orphans.empty() ? "none" : orphans);
    }
    // Bottom to top for merged slabs. Outside the layer-0 reservation of a first band, fixed-grid events are
    // registered as immutable occupancy before this arbitration, so a coarse band cannot claim a layer that
    // a fine nozzle will extrude later. Where two merged ladders start together the coarser one still wins.
    std::stable_sort(events.begin(), events.end(), [](const MergeEvent &lhs, const MergeEvent &rhs) {
        if (lhs.band.bottom_idx != rhs.band.bottom_idx)
            return lhs.band.bottom_idx < rhs.band.bottom_idx;
        if (std::abs(lhs.nozzle - rhs.nozzle) > EPSILON)
            return lhs.nozzle > rhs.nozzle;
        if (lhs.band.top_idx != rhs.band.top_idx)
            return lhs.band.top_idx < rhs.band.top_idx;
        return lhs.region_id < rhs.region_id;
    });

    // Physical occupancy on the fine slicing grid. A merged patch occupies every step of its Z span even
    // though its G-code is emitted only on the group-top layer.
    std::vector<ExPolygons> occupied(layer_count);
    auto occupy = [&occupied](size_t layer_idx, const ExPolygons &area) {
        if (area.empty())
            return;
        Polygons combined = to_polygons(occupied[layer_idx]);
        polygons_append(combined, to_polygons(area));
        occupied[layer_idx] = union_ex(combined);
    };
    struct FirstBandClaim {
        size_t     region_id;
        size_t     top_idx;
        ExPolygons area;
    };
    std::vector<FirstBandClaim> first_band_claims;
    // Resolved slabs, kept in planning order until the orphan resolution below can no longer change them.
    struct PlannedPatch {
        size_t             region_id;
        // The band's planned floor. Marker copies start here even when the patch itself was clipped higher.
        size_t             store_bottom_idx;
        AdaptiveMergePatch patch;
    };
    std::vector<PlannedPatch> planned;

    // The first coarse band is anchored to its true layer-0 contour, which owns that footprint throughout
    // the band. Reserve it before registering fixed-grid regions; later coarse bands still yield to those
    // immutable fine-nozzle extrusions.
    std::vector<ExPolygons> first_band_reservations(layer_count);
    for (const MergeEvent &event : events) {
        if (! event.merged || event.band.bottom_idx != 0)
            continue;
        const ExPolygons &anchor = m_layers.front()->get_region(event.region_id)->raw_slices;
        if (anchor.empty())
            continue;
        for (size_t layer_idx = 0; layer_idx <= event.band.top_idx; ++ layer_idx) {
            Polygons combined = to_polygons(first_band_reservations[layer_idx]);
            polygons_append(combined, to_polygons(anchor));
            first_band_reservations[layer_idx] = union_ex(combined);
        }
    }

    // Fixed-grid regions are emitted exactly as sliced and have no later clipping pass. Register their
    // unreserved footprint first, including events whose layer lies inside a non-first merged band's span.
    for (const MergeEvent &event : events) {
        if (event.merged)
            continue;
        ExPolygons area = m_layers[event.band.top_idx]->get_region(event.region_id)->raw_slices;
        if (! first_band_reservations[event.band.top_idx].empty())
            area = diff_ex(area, first_band_reservations[event.band.top_idx]);
        occupy(event.band.top_idx, area);
    }

    for (const MergeEvent &event : events) {
        m_print->throw_if_canceled();
        const size_t region_id = event.region_id;
        if (! event.merged)
            continue;

        const AdaptiveLayerBand &band       = event.band;
        const bool               first_band = band.bottom_idx == 0;
        // Planning already resolved the top against the material this region owns.
        const AdaptiveLayerBand &live = band;
        if (first_band && m_layers.front()->get_region(region_id)->raw_slices.empty()) {
            AdaptiveMergePatch patch;
            patch.bottom_idx    = live.bottom_idx;
            patch.top_idx       = live.top_idx;
            patch.merged_height = live.merged_height;
            patch.set_area({});
            for (size_t layer_idx = live.bottom_idx; layer_idx <= live.top_idx; ++ layer_idx) {
                AdaptiveMergePatch stored = patch;
                stored.group_top = layer_idx == live.top_idx;
                m_layers[layer_idx]->get_region(region_id)->adaptive_merge_patches.emplace_back(std::move(stored));
            }
            continue;
        }
        // The first band is a snapshot of the true first layer. Later bands use their top contour.
        const size_t anchor_idx  = first_band ? 0 : live.top_idx;
        const ExPolygons  anchor = m_layers[anchor_idx]->get_region(region_id)->raw_slices;
        const double top_area    = ::Slic3r::area(anchor) * SCALING_FACTOR * SCALING_FACTOR;

        // A band shows one contour over its whole Z span, so a volume whose top face falls inside the band
        // is not in that contour at all: on the steps it does occupy it would be neither walled nor filled.
        // Give what ends inside the band its own group top, on the highest step where it still exists.
        //
        // The fringe by which a shrinking shape - a cone, a dome - exceeds the contour above it is not such a
        // volume: it cannot hold a bead, and the ladder has always accepted it as inherent underfill. Only
        // what this nozzle could really lay a bead into is worth a ceiling of its own.
        //
        // The first band is excluded. Its anchor is the object base by design, and pulling side-wall
        // material up into it is exactly what that choice exists to prevent.
        struct Slab {
            size_t     top_idx;
            ExPolygons area;
        };
        std::vector<Slab> slabs { { live.top_idx, anchor } };
        if (! first_band) {
            Polygons inside;
            for (size_t layer_idx = live.bottom_idx; layer_idx < live.top_idx; ++ layer_idx)
                polygons_append(inside, to_polygons(m_layers[layer_idx]->get_region(region_id)->raw_slices));
            ExPolygons ends_inside =
                adaptive_printable_core(diff_ex(union_ex(inside), anchor), event.nozzle);
            // Top down, so each piece is claimed by the highest step it still exists on.
            for (size_t layer_idx = live.top_idx; layer_idx-- > live.bottom_idx && ! ends_inside.empty();) {
                ExPolygons here = adaptive_printable_core(
                    intersection_ex(ends_inside, m_layers[layer_idx]->get_region(region_id)->raw_slices),
                    event.nozzle);
                if (here.empty())
                    continue;
                ends_inside = diff_ex(ends_inside, here);
                slabs.push_back({ layer_idx, std::move(here) });
            }
        }

        std::vector<AdaptiveMergePatch> patches;
        // Too-thin orphans recorded for this band, kept to decide whether the empty marker is still needed.
        struct BandOrphan {
            size_t     bottom_idx;
            size_t     top_idx;
            ExPolygons area;
        };
        std::vector<BandOrphan> band_orphans;
        for (const Slab &slab : slabs) {
            ExPolygons area = occupied[slab.top_idx].empty() ?
                slab.area : diff_ex(slab.area, occupied[slab.top_idx]);

            // Partition the XY footprint by the highest occupied step underneath it. Every piece keeps this
            // slab's group top, but starts immediately above its own local obstruction. This avoids both
            // overlap and the gap that would result from raising the complete patch for a partial conflict.
            ExPolygons assigned;
            // The first step whose clear span is too thin to extrude. From there up this slab can place
            // nothing, so whatever footprint is still unassigned has lost its owner.
            size_t     thin_bottom_idx = slab.top_idx + 1;
            // A merged extrusion is laid onto whatever is below its floor, so it cannot reach down into a step
            // that holds no object material under its footprint: a colour starting on a bridge in the middle
            // of a band would otherwise hang below the real bottom face. The first band rests on the bed.
            // Fringes too narrow to hold a bead of this nozzle are the usual underfill of a sloped wall.
            //
            // Only a face too thin for this nozzle is held off, and then dropped as an orphan. A face that could be
            // extruded on its own is not cut out of the slab: a patch of its own would wall it off from the
            // material it rests against, so it hangs from the band floor with the rest of the contour.
            std::vector<ExPolygons> unsupported(slab.top_idx + 1);
            if (! first_band && ! area.empty()) {
                Polygons thin_faces;
                for (size_t layer_idx = live.bottom_idx; layer_idx < slab.top_idx; ++ layer_idx) {
                    unsupported[layer_idx] = adaptive_printable_pieces(
                        diff_ex(area, m_layers[layer_idx]->lslices), event.nozzle);
                    // Missing on this step, so it starts on the step above at the earliest.
                    if (span_height(layer_idx + 1, slab.top_idx) < event.range.min_height - EPSILON)
                        polygons_append(thin_faces, to_polygons(unsupported[layer_idx]));
                }
                const ExPolygons thin_area = union_ex(thin_faces);
                for (size_t layer_idx = live.bottom_idx; layer_idx < slab.top_idx; ++ layer_idx)
                    unsupported[layer_idx] = thin_area.empty() ? ExPolygons{} : adaptive_printable_pieces(
                        intersection_ex(unsupported[layer_idx], thin_area), event.nozzle);
            }
            for (size_t bottom_idx = live.bottom_idx; bottom_idx <= slab.top_idx && ! area.empty(); ++ bottom_idx) {
                const double height = span_height(bottom_idx, slab.top_idx);
                // Orphan recovery must preserve the physical extruder height limit.
                if (height > event.range.max_height + EPSILON)
                    continue;
                if (height < event.range.min_height - EPSILON && !(first_band && bottom_idx == 0)) {
                    thin_bottom_idx = bottom_idx;
                    break;
                }

                Polygons blockers;
                for (size_t layer_idx = bottom_idx; layer_idx < slab.top_idx; ++ layer_idx) {
                    polygons_append(blockers, to_polygons(occupied[layer_idx]));
                    polygons_append(blockers, to_polygons(unsupported[layer_idx]));
                }
                ExPolygons clear = blockers.empty() ? area : diff_ex(area, union_ex(blockers));
                if (! assigned.empty())
                    clear = diff_ex(clear, assigned);
                if (clear.empty())
                    continue;

                AdaptiveMergePatch patch;
                patch.bottom_idx    = bottom_idx;
                patch.top_idx       = slab.top_idx;
                patch.merged_height = height;
                patch.set_area(std::move(clear));
                Polygons combined = to_polygons(assigned);
                polygons_append(combined, to_polygons(patch.area()));
                assigned = union_ex(combined);
                patches.emplace_back(std::move(patch));
            }

            // Steps the partition had to give up on are the second way a step loses its owner: a slab does
            // cover them, but everything underneath is occupied and the clear span left is below this
            // nozzle's minimum. Recorded for the same reason as a step no band could take - the alternative
            // is rediscovering it later from the raw slices, where it cannot be told from a fringe.
            const ExPolygons unassigned = diff_ex(area, assigned);
            if (thin_bottom_idx <= slab.top_idx && ! unassigned.empty()) {
                const bool floating = thin_bottom_idx > 0 && ! adaptive_printable_pieces(
                    diff_ex(unassigned, m_layers[thin_bottom_idx - 1]->lslices), event.nozzle).empty();
                pending_orphans.push_back({ region_id, event.range,
                                            thin_bottom_idx, slab.top_idx, "too-thin", unassigned, floating });
                band_orphans.push_back({ thin_bottom_idx, slab.top_idx, unassigned });
            }

            BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] region=" << region_id
                                     << " band=[" << band.bottom_idx << "," << band.top_idx << "]"
                                     << " slab=[" << live.bottom_idx << "," << slab.top_idx << "]"
                                     << (slab.top_idx == live.top_idx ? " anchor" : " local_top")
                                     << " raw_area="
                                     << ::Slic3r::area(slab.area) * SCALING_FACTOR * SCALING_FACTOR
                                     << " after_occupied="
                                     << ::Slic3r::area(area) * SCALING_FACTOR * SCALING_FACTOR
                                     << " unassigned="
                                     << ::Slic3r::area(unassigned) * SCALING_FACTOR * SCALING_FACTOR
                                     << " thin_from=" << (thin_bottom_idx > slab.top_idx ?
                                                          std::string("none") : std::to_string(thin_bottom_idx))
                                     << (area.empty() ? " DROPPED" : " KEPT");
        }

        // The empty marker suppresses the whole region over the band. It can be left out only when every piece
        // of material the region holds inside the band belongs to one of its too-thin orphans: orphan resolution
        // then absorbs or suppresses all of it, while the marker would void the orphan's own fill (a bridge,
        // say) before the slab absorbing it could lift it. Anything else - material under another nozzle's
        // occupancy, a fringe no slab took - would otherwise fall back to the base layer height.
        bool orphans_own_band = ! band_orphans.empty();
        for (size_t layer_idx = live.bottom_idx; layer_idx <= live.top_idx && orphans_own_band; ++ layer_idx) {
            const ExPolygons &raw = m_layers[layer_idx]->get_region(region_id)->raw_slices;
            if (raw.empty())
                continue;
            Polygons owned;
            for (const BandOrphan &orphan : band_orphans)
                if (orphan.bottom_idx <= layer_idx && layer_idx <= orphan.top_idx)
                    polygons_append(owned, to_polygons(orphan.area));
            // Only numerical slivers between the two polygonisations may be left over.
            orphans_own_band = ! owned.empty() &&
                opening_ex(diff_ex(raw, union_ex(owned)), float(scale_(0.01))).empty();
        }
        if (patches.empty() && ! orphans_own_band) {
            AdaptiveMergePatch patch;
            patch.bottom_idx    = live.bottom_idx;
            patch.top_idx       = live.top_idx;
            patch.merged_height = live.merged_height;
            patch.set_area({});
            patches.emplace_back(std::move(patch));
        }

        BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] region=" << region_id
                                 << " band=[" << band.bottom_idx << "," << band.top_idx << "]"
                                 << " live=[" << live.bottom_idx << "," << live.top_idx << "]"
                                 << " top_raw_area=" << top_area
                                 << " slabs=" << slabs.size()
                                 << " patches=" << patches.size();

        for (AdaptiveMergePatch &patch : patches) {
            for (size_t layer_idx = patch.bottom_idx; layer_idx <= patch.top_idx; ++ layer_idx)
                occupy(layer_idx, patch.area());

            // Held back until orphan resolution below has had its say: it may still drop a slab's floor,
            // and the marker layers under a locally clipped patch have to grow with it.
            planned.push_back({ region_id, event.source_bottom_idx, std::move(patch) });
        }
    }

    // Resolve the runs recorded while planning, in the same order as a run is resolved during planning:
    // first extend the slab above, then the slab below, then re-cut the run with the slab below.
    // Every resulting slab must stay within the configured extruder height range.
    // A floating face skips all of them: the slab above keeps its floor and bridges over it.
    //
    // A run nobody can absorb keeps its empty group top. That marker is the established "nothing is
    // extruded here": it clears the perimeters and makes combine_adaptive_infill() void the fill, so the
    // step cannot fall back to a base-grid extrusion this nozzle is unable to lay down.
    enum class Recovery { FromAbove, FromBelow, Split };
    for (const PendingOrphan &orphan : pending_orphans) {
        m_print->throw_if_canceled();
        ExPolygons remaining = orphan.area;

        // Take the run back column by column into a slab of its region touching it. Only the column over the
        // lost footprint changes; the rest of that slab keeps its own span.
        //   FromAbove - the slab above lowers the column's floor to the run's bottom, keeping its group top.
        //   FromBelow - the slab below lifts the column's top to the run's top, keeping its floor.
        //   Split     - the column is re-cut with the slab below into two slabs inside the window, keeping
        //               the slab's floor and the run's top.
        // A slab resting on the bed belongs to the first band, whose height is fixed; it is never lifted or cut.
        auto recover = [&](Recovery mode, double ceiling) {
            const size_t planned_count = planned.size();
            for (size_t planned_idx = 0; planned_idx < planned_count && ! remaining.empty(); ++ planned_idx) {
                PlannedPatch &donor = planned[planned_idx];
                if (donor.region_id != orphan.region_id || donor.patch.area().empty())
                    continue;
                if (mode == Recovery::FromAbove ? donor.patch.bottom_idx != orphan.top_idx + 1 :
                        donor.patch.bottom_idx == 0 || donor.patch.top_idx + 1 != orphan.bottom_idx)
                    continue;
                size_t split_top_idx = 0;
                if (mode == Recovery::Split) {
                    if (! split_before_tail(donor.patch.bottom_idx, donor.patch.top_idx, orphan.top_idx,
                                            orphan.range, split_top_idx))
                        continue;
                } else if (mode == Recovery::FromAbove ?
                               span_height(orphan.bottom_idx, donor.patch.top_idx) > ceiling + EPSILON :
                               span_height(donor.patch.bottom_idx, orphan.top_idx) > ceiling + EPSILON)
                    continue;

                m_print->throw_if_canceled();
                ExPolygons recovered = intersection_ex(donor.patch.area(), remaining);
                // The column has to exist on every step from where a new group top starts: the run's bottom, or
                // the split when cut, where the lower slab now ends and shows its contour. Below that the donor
                // already holds it. Only the run is checked against other occupancy: the donor owns its own steps.
                // A nozzle cannot dip below an extrusion standing on the run, so a conflict only removes its own
                // part of the column.
                const size_t check_from = mode == Recovery::Split ? split_top_idx : orphan.bottom_idx;
                for (size_t idx = check_from; idx <= orphan.top_idx && ! recovered.empty(); ++ idx) {
                    recovered = intersection_ex(recovered, m_layers[idx]->get_region(orphan.region_id)->raw_slices);
                    if (idx >= orphan.bottom_idx && ! occupied[idx].empty())
                        recovered = diff_ex(recovered, occupied[idx]);
                }
                recovered = adaptive_printable_pieces(std::move(recovered), region_nozzles[orphan.region_id]);
                if (recovered.empty())
                    continue;

                std::vector<PlannedPatch> pieces;
                if (mode == Recovery::Split) {
                    PlannedPatch lower = donor;
                    lower.patch.top_idx       = split_top_idx;
                    lower.patch.merged_height = span_height(lower.patch.bottom_idx, split_top_idx);
                    lower.patch.set_area(recovered);
                    pieces.emplace_back(std::move(lower));
                }
                PlannedPatch column = donor;
                if (mode == Recovery::FromAbove)
                    column.patch.bottom_idx = orphan.bottom_idx;
                else {
                    if (mode == Recovery::Split) {
                        column.store_bottom_idx = split_top_idx + 1;
                        column.patch.bottom_idx = split_top_idx + 1;
                    }
                    column.patch.top_idx = orphan.top_idx;
                }
                column.patch.merged_height = span_height(column.patch.bottom_idx, column.patch.top_idx);
                column.patch.set_area(recovered);
                pieces.emplace_back(std::move(column));

                ExPolygons unchanged = diff_ex(donor.patch.area(), recovered);
                remaining = diff_ex(remaining, recovered);
                // Occupancy outside the run is unchanged; only the run is filled in.
                for (size_t idx = orphan.bottom_idx; idx <= orphan.top_idx; ++ idx)
                    occupy(idx, recovered);
                const AdaptiveMergePatch &taken = pieces.back().patch;
                if (mode == Recovery::Split)
                    BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] orphan region=" << orphan.region_id
                                             << " [" << orphan.bottom_idx << "," << orphan.top_idx << "]"
                                             << " " << orphan.source << " rebalanced from below, split=" << split_top_idx
                                             << " h=" << pieces.front().patch.merged_height << "+" << taken.merged_height;
                else
                    BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] orphan region=" << orphan.region_id
                                             << " [" << orphan.bottom_idx << "," << orphan.top_idx << "]"
                                             << " " << orphan.source
                                             << (mode == Recovery::FromAbove ? " absorbed from above into [" :
                                                                               " lifted from below into [")
                                             << taken.bottom_idx << "," << taken.top_idx << "] h=" << taken.merged_height
                                             << (taken.merged_height > orphan.range.max_height + EPSILON ?
                                                 " RELAXED_MAX" : "");
                // The donor itself is reused for the first piece, or keeps what the column left of it.
                // Do not use donor after appending: growing planned may invalidate the reference.
                size_t first_new = 0;
                if (unchanged.empty())
                    donor = std::move(pieces[first_new ++]);
                else
                    donor.patch.set_area(std::move(unchanged));
                for (size_t idx = first_new; idx < pieces.size(); ++ idx)
                    planned.emplace_back(std::move(pieces[idx]));
            }
            return remaining.empty();
        };

        if (! orphan.floating &&
            (recover(Recovery::FromAbove, orphan.range.max_height) ||
             recover(Recovery::FromBelow, orphan.range.max_height) ||
             recover(Recovery::Split, orphan.range.max_height)))
            continue;

        for (size_t idx = orphan.bottom_idx; idx <= orphan.top_idx; ++ idx) {
            AdaptiveMergePatch patch;
            patch.bottom_idx    = idx;
            patch.top_idx       = idx;
            patch.group_top     = true;
            patch.merged_height = layer_heights[idx];
            patch.set_area({});
            patch.set_suppressed_area(intersection_ex(
                remaining, m_layers[idx]->get_region(orphan.region_id)->raw_slices));
            m_layers[idx]->get_region(orphan.region_id)->adaptive_merge_patches.emplace_back(std::move(patch));
        }
        BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] orphan region=" << orphan.region_id
                                 << " [" << orphan.bottom_idx << "," << orphan.top_idx << "]"
                                 << " " << orphan.source << (orphan.floating ? " floating" : "") << " suppressed";
    }

    for (const PlannedPatch &planned_patch : planned) {
        // Orphan recovery may lower a patch after the initial partition. Build claims from the final span,
        // but only for patches whose source band and resolved floor are both truly anchored at layer 0.
        if (planned_patch.store_bottom_idx == 0 && planned_patch.patch.bottom_idx == 0 &&
            ! planned_patch.patch.area().empty())
            first_band_claims.push_back(
                { planned_patch.region_id, planned_patch.patch.top_idx, planned_patch.patch.area() });

        // Marker copies reach down to the band's planned floor as well as to the absorbed one: every step
        // the slab stands on is swallowed and must not fall back to a thin-layer toolpath.
        const size_t store_bottom = std::min(planned_patch.store_bottom_idx, planned_patch.patch.bottom_idx);
        for (size_t layer_idx = store_bottom; layer_idx <= planned_patch.patch.top_idx; ++ layer_idx) {
            AdaptiveMergePatch stored = planned_patch.patch;
            stored.group_top = layer_idx == planned_patch.patch.top_idx;
            m_layers[layer_idx]->get_region(planned_patch.region_id)
                ->adaptive_merge_patches.emplace_back(std::move(stored));
        }
    }

    // The layer-0 snapshot owns its XY footprint through its group top against fixed-grid regions. Merged
    // regions were already partitioned through patch.area(); trimming their source slices here would corrupt
    // the layer-0 contour from which their own first-band walls are generated.
    for (const FirstBandClaim &claim : first_band_claims)
        for (size_t layer_idx = 0; layer_idx <= claim.top_idx; ++ layer_idx)
            for (size_t region_id = 0; region_id < region_count; ++ region_id) {
                if (region_id == claim.region_id)
                    continue;
                LayerRegion *region = m_layers[layer_idx]->get_region(region_id);
                if (! region->adaptive_merge_patches.empty())
                    continue;
                Surfaces kept;
                for (const Surface &surface : region->slices.surfaces)
                    for (ExPolygon &expolygon : diff_ex(ExPolygons{ surface.expolygon }, claim.area))
                        kept.emplace_back(surface, std::move(expolygon));
                region->slices.set(std::move(kept));
            }
}

// Real overhangs of a merged span are already typed on the physical layers by detect_surfaces_type()
// and bridge_over_infill(). Only those surfaces are lifted to the group top, so nothing here can invent a
// bridge the unmerged slice did not have.
//
// Every layer of the span is voided by combine_adaptive_infill(), so the merged extrusion rests on the
// layer below the span, not on anything the span used to hold. Sparse fill inside the span is therefore no
// reason to drop a bridge - it is the very thing that gets taken away, which is why a shell that starts
// mid-band needs its internal bridge more than the unmerged slice did, not less.
static Surfaces collect_span_bridges(const LayerPtrs  &layers,
                                     size_t            region_id,
                                     size_t            bottom_idx,
                                     size_t            top_idx,
                                     const ExPolygons &merge_area)
{
    Surfaces out;
    Polygons claimed;

    auto claim = [&out, &claimed](const Surface &templ, ExPolygons &&area) {
        if (! claimed.empty())
            area = diff_ex(area, union_ex(claimed));
        for (ExPolygon &expolygon : area) {
            out.emplace_back(templ, std::move(expolygon));
            polygons_append(claimed, to_polygons(out.back().expolygon));
        }
    };

    for (size_t layer_idx = bottom_idx; layer_idx <= top_idx; ++ layer_idx)
        for (const Surface &surface : layers[layer_idx]->get_region(region_id)->fill_surfaces.surfaces)
            if (surface.is_bridge())
                claim(surface, intersection_ex(ExPolygons{ surface.expolygon }, merge_area));
    return out;
}

// Move the fill of a merge span onto its group-top layer. Fill using the wall nozzle follows the wall
// patches; fill using another nozzle diameter is planned independently with its own height window.
void PrintObject::combine_adaptive_infill()
{
    const PrintConfig &print_config = this->print()->config();

    if (! has_extruder_layer_heights(*this))
        return;

    const size_t region_count = this->num_printing_regions();
    const size_t layer_count = m_layers.size();

    std::vector<double> layer_heights;
    layer_heights.reserve(layer_count);
    for (const Layer *layer : m_layers)
        layer_heights.emplace_back(layer->height);

    // Measured against the same reference compute_adaptive_merge_groups() uses, so a fill band and a wall
    // band of one diameter always cover the same steps.
    const double min_nozzle = m_slicing_params.layer_height;
    if (min_nozzle <= EPSILON)
        return;

    // Fill follows the same fixed nozzle ladder as walls. Colour transitions do not split either grid.
    const std::vector<uint8_t> band_tops;

    // The merged extrusion of the group-top layer already fills this volume, so the fill of the layers below
    // must disappear. Surfaces are rebuilt one by one to keep the template (bridge angle, thickness) of the
    // parts that stay outside the area.
    auto void_fill = [this](LayerRegion *layerm, const ExPolygons &area) {
        if (area.empty())
            return;
        const BoundingBox area_bbox = get_extents(area);
        Surfaces   kept;
        ExPolygons voided;
        kept.reserve(layerm->fill_surfaces.surfaces.size());
        for (const Surface &surface : layerm->fill_surfaces.surfaces) {
            m_print->throw_if_canceled();
            // Splitting a void surface again leaves it void; it only multiplies the surfaces every later
            // call has to clip.
            const BoundingBox surface_bbox = get_extents(surface.expolygon);
            if (surface.surface_type == stInternalVoid || ! surface_bbox.overlap(area_bbox)) {
                kept.emplace_back(surface);
                continue;
            }
            // The area may cover a whole layer while a surface is small; clipping it to the surface's
            // bounding box first keeps the result and avoids feeding every point of it to each operation.
            const Polygons clip = ClipperUtils::clip_clipper_polygons_with_subject_bbox(
                area, surface_bbox.inflated(SCALED_EPSILON));
            if (clip.empty()) {
                kept.emplace_back(surface);
                continue;
            }
            for (ExPolygon &expolygon : diff_ex(ExPolygons{ surface.expolygon }, clip))
                kept.emplace_back(surface, std::move(expolygon));
            expolygons_append(voided, intersection_ex(ExPolygons{ surface.expolygon }, clip));
        }
        if (voided.empty())
            return;
        layerm->fill_surfaces.set(std::move(kept));
        layerm->fill_surfaces.append(union_ex(to_polygons(voided)), stInternalVoid);
    };
    // A layer whose whole region is swallowed extrudes nothing of it, so every fill surface turns void at
    // once. Fill and G-code export skip only stInternalVoid, and a merged-out layer must have no extrusions.
    auto void_whole_fill = [](LayerRegion *layerm) {
        if (layerm->fill_surfaces.empty())
            return;
        layerm->fill_surfaces.set(union_ex(to_polygons(layerm->fill_surfaces.surfaces)), stInternalVoid);
    };

    // The merged extrusion covers the whole Z span, so a solid requirement of any layer inside the span has
    // to be honored on the group top. Only the footprint that asks for solid is promoted: promoting the
    // complete merge area would turn every sparse column of the span into solid fill.
    auto span_solid_area = [](const Polygons &solid, const ExPolygons &merge_area) {
        return solid.empty() ? ExPolygons{} : intersection_ex(merge_area, union_ex(solid));
    };

    // Surface types were detected on the fine slicing grid, but a merged extrusion is exposed according
    // to the footprint of the next physical slab. A patch stored on a swallowed layer still describes
    // that slab because all copies across its span share the group-top geometry.
    const bool spiral_mode      = print_config.spiral_mode.value;
    const bool interface_shells = ! spiral_mode && m_config.interface_shells.value;
    std::unordered_map<size_t, ExPolygons> extrusion_footprint_cache;
    auto extrusion_footprint = [this, interface_shells, region_count, &extrusion_footprint_cache](
                                  size_t layer_idx, size_t region_id) -> const ExPolygons& {
        static const ExPolygons empty;
        if (layer_idx >= m_layers.size())
            return empty;

        const size_t cache_region = interface_shells ? region_id : 0;
        const size_t cache_key    = layer_idx * (interface_shells ? region_count : 1) + cache_region;
        if (auto cached = extrusion_footprint_cache.find(cache_key); cached != extrusion_footprint_cache.end())
            return cached->second;

        m_print->throw_if_canceled();
        Polygons footprint;
        const size_t first_region = interface_shells ? region_id : 0;
        const size_t last_region  = interface_shells ? region_id + 1 : region_count;
        for (size_t current_region = first_region; current_region < last_region; ++ current_region) {
            const LayerRegion *layerm = m_layers[layer_idx]->get_region(current_region);
            if (layerm->adaptive_merge_patches.empty()) {
                polygons_append(footprint, to_polygons(layerm->slices.surfaces));
            } else {
                for (const AdaptiveMergePatch &patch : layerm->adaptive_merge_patches)
                    polygons_append(footprint, to_polygons(patch.area()));
            }
        }
        return extrusion_footprint_cache.emplace(cache_key, union_ex(footprint)).first->second;
    };

    // deferred_void: when set, the swallowed-layer voids are collected there per layer instead of applied now.
    auto apply_merged_fill = [&void_fill, &span_solid_area, &extrusion_footprint, spiral_mode, this](
                                 size_t region_id, size_t bottom_idx, size_t top_idx, double merged_height,
                                 const ExPolygons &merge_area,
                                 bool includes_sparse, bool includes_solid, bool void_lower_layers,
                                 std::vector<Polygons> *deferred_void) {
        Polygons solid;
        if (includes_solid)
            for (size_t layer_idx = bottom_idx; layer_idx <= top_idx; ++layer_idx)
                for (const Surface &surface : m_layers[layer_idx]->get_region(region_id)->fill_surfaces.surfaces)
                    // Bridges are solids for fill flow, but they are promoted on their own by
                    // collect_span_bridges(); keep them out of the solid footprint.
                    if (surface.is_solid() && ! surface.is_bridge())
                        polygons_append(solid, to_polygons(surface.expolygon));
        const ExPolygons solid_area = span_solid_area(solid, merge_area);

        Surfaces bottom_bridges;
        if (includes_solid)
            bottom_bridges = collect_span_bridges(m_layers, region_id, bottom_idx, top_idx, merge_area);
        Polygons object_bottom;
        if (includes_solid && bottom_idx == 0)
            for (const Surface &surface : m_layers.front()->get_region(region_id)->fill_surfaces.surfaces)
                if (surface.is_bottom() && ! surface.is_bridge())
                    polygons_append(object_bottom, to_polygons(surface.expolygon));
        const ExPolygons object_bottom_area = object_bottom.empty() ?
            ExPolygons{} : intersection_ex(merge_area, union_ex(object_bottom));

        if (void_lower_layers)
            for (size_t layer_idx = bottom_idx; layer_idx < top_idx; ++layer_idx) {
                if (deferred_void)
                    polygons_append((*deferred_void)[layer_idx], to_polygons(merge_area));
                else
                    void_fill(m_layers[layer_idx]->get_region(region_id), merge_area);
            }

        LayerRegion *layerm = m_layers[top_idx]->get_region(region_id);
        // The fine layer above may be swallowed by a later coarse-nozzle slab. Compare against that
        // slab's actual footprint, otherwise an exposed group top keeps the stale internal-solid role.
        const PrintRegionConfig &region_config = layerm->region().config();
        const bool detect_top = spiral_mode || region_config.top_shell_layers.value > 0;
        ExPolygons top_area;
        if (includes_solid && detect_top) {
            top_area = opening_ex(
                diff_ex(solid_area, extrusion_footprint(top_idx + 1, region_id), ApplySafetyOffset::Yes),
                layerm->flow(frExternalPerimeter).scaled_width() / 10.f);
            // The group top keeps the top surfaces typed on the fine grid. process_external_surfaces() grew
            // them into the solid under the material above, so the top pattern reaches the walls there.
            // The slab footprint above only adds faces the fine grid could not see; it must not shrink them.
            Polygons top_polygons = to_polygons(top_area);
            for (const Surface &surface : layerm->fill_surfaces.surfaces)
                if (surface.surface_type == stTop)
                    polygons_append(top_polygons, to_polygons(surface.expolygon));
            top_area = union_ex(top_polygons);
            if (! object_bottom_area.empty())
                top_area = diff_ex(top_area, object_bottom_area);
        }

        Surfaces   kept;
        ExPolygons covered;
        const BoundingBox merge_bbox = get_extents(merge_area);
        kept.reserve(layerm->fill_surfaces.surfaces.size());
        for (const Surface &surface : layerm->fill_surfaces.surfaces) {
            m_print->throw_if_canceled();
            const BoundingBox surface_bbox = get_extents(surface.expolygon);
            const Polygons    clip         = surface_bbox.overlap(merge_bbox) ?
                ClipperUtils::clip_clipper_polygons_with_subject_bbox(merge_area, surface_bbox.inflated(SCALED_EPSILON)) :
                Polygons{};
            if (clip.empty()) {
                kept.emplace_back(surface);
                continue;
            }
            for (ExPolygon &expolygon : diff_ex(ExPolygons{ surface.expolygon }, clip))
                kept.emplace_back(surface, std::move(expolygon));
            expolygons_append(covered, intersection_ex(ExPolygons{ surface.expolygon }, clip));
        }
        if (covered.empty()) {
            if (bottom_idx != 0)
                return;
            // The first-band wall may consume the entire narrow slice. Fall back to the cavity left by
            // perimeter generation, not the full slice, or infill will overlap the merged wall.
            covered = intersection_ex(merge_area, layerm->fill_expolygons);
            if (covered.empty())
                return;
        } else {
            covered = union_ex(to_polygons(covered));
        }

        // A merged extrusion is supported below the group bottom. Preserve bridge surfaces from that
        // layer on the group top; otherwise the retained contours inside the span hide the void below.
        // The visible face is the exception: an internal bridge exists only to carry the solid shell
        // above it, and the group top is that shell. Merging both into one extrusion is the same as
        // slicing the shell at the merged height, which is a plain top surface over sparse infill.
        Surfaces promoted_bridges;
        Polygons bridge_claimed;
        for (const Surface &surface : bottom_bridges) {
            ExPolygons bridge_area = intersection_ex(ExPolygons{ surface.expolygon }, covered);
            if (! top_area.empty())
                bridge_area = diff_ex(bridge_area, top_area);
            if (! bridge_claimed.empty())
                bridge_area = diff_ex(bridge_area, union_ex(bridge_claimed));
            for (ExPolygon &expolygon : bridge_area) {
                promoted_bridges.emplace_back(surface, std::move(expolygon));
                Surface &promoted = promoted_bridges.back();
                promoted.thickness = merged_height;
                promoted.thickness_layers = (unsigned short)(top_idx - bottom_idx + 1);
                polygons_append(bridge_claimed, to_polygons(promoted.expolygon));
            }
        }
        ExPolygons remaining = diff_ex(covered, union_ex(bridge_claimed));

        // A first band has one extrusion representing the complete slab. Bottom contact wins where its
        // top and bottom faces overlap, matching detect_surfaces_type().
        ExPolygons top_fill = intersection_ex(remaining, top_area);
        ExPolygons bottom_fill = diff_ex(intersection_ex(remaining, object_bottom_area), top_fill);
        Polygons visible_fill = to_polygons(bottom_fill);
        polygons_append(visible_fill, to_polygons(top_fill));
        ExPolygons solid_fill = diff_ex(intersection_ex(remaining, solid_area), union_ex(visible_fill));
        Polygons claimed = bridge_claimed;
        polygons_append(claimed, to_polygons(bottom_fill));
        polygons_append(claimed, to_polygons(top_fill));
        polygons_append(claimed, to_polygons(solid_fill));
        ExPolygons sparse_fill = includes_sparse ? diff_ex(covered, union_ex(claimed)) : ExPolygons{};

        Surface templ(stInternal, ExPolygon());
        templ.thickness = merged_height;
        templ.thickness_layers = (unsigned short)(top_idx - bottom_idx + 1);

        layerm->fill_surfaces.set(std::move(kept));
        layerm->fill_surfaces.append(std::move(promoted_bridges));
        if (! bottom_fill.empty()) {
            templ.surface_type = stBottom;
            layerm->fill_surfaces.append(std::move(bottom_fill), templ);
        }
        if (! top_fill.empty()) {
            templ.surface_type = stTop;
            layerm->fill_surfaces.append(std::move(top_fill), templ);
        }
        if (! solid_fill.empty()) {
            templ.surface_type = stInternalSolid;
            layerm->fill_surfaces.append(std::move(solid_fill), templ);
        }
        if (! sparse_fill.empty()) {
            templ.surface_type = stInternal;
            layerm->fill_surfaces.append(std::move(sparse_fill), templ);
        }
    };

    for (size_t region_id = 0; region_id < region_count; ++region_id) {
        m_print->throw_if_canceled();
        const PrintRegionConfig &config = this->printing_region(region_id).config();
        const unsigned int wall_filament   = this->printing_region(region_id).extruder(frExternalPerimeter);
        const unsigned int sparse_filament = this->printing_region(region_id).extruder(frInfill);
        const unsigned int solid_filament  = this->printing_region(region_id).extruder(frSolidInfill);

        const double wall_nozzle   = nozzle_diameter_for_filament(print_config, wall_filament, this->print()->is_BBL_printer());
        const double sparse_nozzle = nozzle_diameter_for_filament(print_config, sparse_filament, this->print()->is_BBL_printer());
        const double solid_nozzle  = nozzle_diameter_for_filament(print_config, solid_filament, this->print()->is_BBL_printer());
        // Explicit infill combination owns the sparse-infill Z planning. Leave sparse surfaces on the
        // base-layer grid here so combine_infill() can merge them up to their own nozzle diameter.
        const bool sparse_follows_adaptive_bands = ! config.infill_combination.value;
        const bool sparse_uses_wall_nozzle = sparse_follows_adaptive_bands &&
                                             std::abs(sparse_nozzle - wall_nozzle) <= EPSILON &&
            std::abs(extruder_layer_height_for_filament(print_config, sparse_filament, m_config.layer_height.value) - extruder_layer_height_for_filament(print_config, wall_filament, m_config.layer_height.value)) <= EPSILON;
        const bool solid_uses_wall_nozzle  = std::abs(solid_nozzle - wall_nozzle) <= EPSILON &&
            std::abs(extruder_layer_height_for_filament(print_config, solid_filament, m_config.layer_height.value) - extruder_layer_height_for_filament(print_config, wall_filament, m_config.layer_height.value)) <= EPSILON;

        // Reuse each wall group-top patch as the XY/Z range for fill roles that share the wall nozzle.
        // inherit_wall_column: sparse and solid both use that nozzle, so lower layers inherit the whole
        // wall column (they may already be void after the wall merge) and leftover fill inside the column
        // is voided so it cannot overlap the thick wall. Otherwise only the matching roles are collected.
        auto merge_fill_along_wall_patches = [&](bool includes_sparse, bool includes_solid, bool inherit_wall_column) {
            for (size_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
                for (const AdaptiveMergePatch &wall_patch :
                     m_layers[layer_idx]->get_region(region_id)->adaptive_merge_patches) {
                    if (! wall_patch.group_top)
                        continue;
                    m_print->throw_if_canceled();

                    if (wall_patch.area().empty()) {
                        // A local orphan suppresses only the XY footprint that failed planning. Legacy empty
                        // markers without such a footprint still mean that the complete region is suppressed.
                        const ExPolygons &suppressed_patch_area = wall_patch.suppressed_area();
                        const bool        has_suppressed_area   = wall_patch.has_suppressed_area();
                        // An explicitly empty local footprint means this orphan has no geometry on the
                        // current layer. It is not the legacy whole-region marker represented by "unset".
                        if (has_suppressed_area && suppressed_patch_area.empty())
                            continue;
                        for (size_t idx = wall_patch.bottom_idx; idx <= wall_patch.top_idx; ++ idx) {
                            LayerRegion *suppressed = m_layers[idx]->get_region(region_id);
                            if (inherit_wall_column) {
                                if (has_suppressed_area)
                                    void_fill(suppressed, suppressed_patch_area);
                                else
                                    void_whole_fill(suppressed);
                                continue;
                            }
                            Polygons same_nozzle_fill;
                            for (const Surface &surface : suppressed->fill_surfaces.surfaces) {
                                const bool sparse_surface = surface.surface_type == stInternal;
                                if ((includes_sparse && sparse_surface) || (includes_solid && surface.is_solid()))
                                    polygons_append(same_nozzle_fill, to_polygons(surface.expolygon));
                            }
                            if (! same_nozzle_fill.empty()) {
                                ExPolygons area = union_ex(same_nozzle_fill);
                                if (has_suppressed_area)
                                    area = intersection_ex(area, suppressed_patch_area);
                                void_fill(suppressed, area);
                            }
                        }
                        continue;
                    }

                    // The fill follows the wall band, it is not planned again: the wall is extruded on the
                    // group top and nowhere else, so its fill has to land on exactly that layer. Like the
                    // wall, the first band keeps the true first layer; later bands keep their top layer.
                    const LayerRegion *layerm =
                        m_layers[wall_patch.bottom_idx == 0 ? 0 : wall_patch.top_idx]->get_region(region_id);
                    ExPolygons         fill_area;
                    if (inherit_wall_column) {
                        // Both fill roles use the wall nozzle, so the fill inherits the whole wall column.
                        Polygons blocked;
                        for (const Surface &surface : layerm->fill_surfaces.surfaces)
                            if ((surface.is_bottom() && wall_patch.bottom_idx != 0) ||
                                surface.surface_type == stInternalVoid)
                                polygons_append(blocked, to_polygons(surface.expolygon));
                        fill_area = diff_ex(wall_patch.area(), union_ex(blocked));
                    } else {
                        Polygons eligible;
                        for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                            if ((surface.is_bottom() && wall_patch.bottom_idx != 0) ||
                                surface.surface_type == stInternalVoid)
                                continue;
                            const bool sparse_surface = surface.surface_type == stInternal;
                            if ((includes_sparse && sparse_surface) || (includes_solid && surface.is_solid()))
                                polygons_append(eligible, to_polygons(surface.expolygon));
                        }
                        fill_area = intersection_ex(union_ex(eligible), wall_patch.area());
                    }
                    // With the whole column inherited, the swallowed-layer pass below voids at least the
                    // patch area on every lower layer, which covers fill_area; voiding it here is redundant.
                    if (! fill_area.empty())
                        apply_merged_fill(region_id, wall_patch.bottom_idx, wall_patch.top_idx,
                                          wall_patch.merged_height, fill_area, includes_sparse, includes_solid,
                                          ! inherit_wall_column, nullptr);
                    for (size_t idx = wall_patch.bottom_idx; idx < wall_patch.top_idx; ++idx) {
                        LayerRegion *swallowed = m_layers[idx]->get_region(region_id);
                        // A shape that closes inside the band - the top face of a step, say - has no
                        // counterpart on the group top, so the patch area never reaches it and it is
                        // left behind alone on its own layer, extruded at the base layer height. This
                        // swallowed layer has no walls to go with it: Layer::make_perimeters() drops
                        // those for the whole region, not just inside the patch. Drop every fill role
                        // sharing that nozzle across the region while preserving roles assigned to
                        // another nozzle.
                        //
                        // Local Z repair may grow one slab down over the group top of the band below it.
                        // That layer does extrude its own walls, so only the footprint this slab covers is
                        // swallowed there - clearing the whole region would delete the fill that group top
                        // owns, leaving walls around an empty layer.
                        const bool extrudes_on_its_own = std::any_of(
                            swallowed->adaptive_merge_patches.begin(), swallowed->adaptive_merge_patches.end(),
                            [](const AdaptiveMergePatch &patch) {
                                return patch.group_top && ! patch.area().empty();
                            });
                        if (inherit_wall_column) {
                            if (extrudes_on_its_own)
                                void_fill(swallowed, wall_patch.area());
                            else
                                void_whole_fill(swallowed);
                        } else {
                            Polygons same_nozzle_fill;
                            for (const Surface &surface : swallowed->fill_surfaces.surfaces) {
                                const bool sparse_surface = surface.surface_type == stInternal;
                                if ((includes_sparse && sparse_surface) || (includes_solid && surface.is_solid()))
                                    polygons_append(same_nozzle_fill, to_polygons(surface.expolygon));
                            }
                            if (! same_nozzle_fill.empty()) {
                                ExPolygons area = union_ex(same_nozzle_fill);
                                if (extrudes_on_its_own)
                                    area = intersection_ex(area, wall_patch.area());
                                if (! area.empty())
                                    void_fill(swallowed, area);
                            }
                        }
                    }
                }
            }
        };

        // Three mutually exclusive cases when sparse fill follows adaptive bands:
        //   both roles share the wall nozzle  -> one pass along wall patches, inherit the whole column
        //   exactly one role shares it        -> that role follows wall patches; the other is planned below
        //   neither shares it                 -> both roles are planned independently
        // With explicit infill combination, sparse fill is excluded from all three and handled later.
        if (sparse_uses_wall_nozzle && solid_uses_wall_nozzle) {
            merge_fill_along_wall_patches(true, true, true);
        } else {
            if (sparse_uses_wall_nozzle || solid_uses_wall_nozzle)
                merge_fill_along_wall_patches(sparse_uses_wall_nozzle, solid_uses_wall_nozzle, false);

            struct FillGroup {
                unsigned int filament;
                bool         sparse;
                bool         solid;
            };
            std::vector<FillGroup> fill_groups;
            if (sparse_follows_adaptive_bands && ! sparse_uses_wall_nozzle)
                fill_groups.push_back({ sparse_filament, true, false });
            if (! solid_uses_wall_nozzle) {
                auto group = std::find_if(fill_groups.begin(), fill_groups.end(),
                    [solid_filament](const FillGroup &candidate) { return candidate.filament == solid_filament; });
                if (group == fill_groups.end())
                    fill_groups.push_back({ solid_filament, false, true });
                else
                    group->solid = true;
            }

            // Fill using a nozzle diameter different from the wall gets its own band grid, anchored like
            // every other one. Sparse and solid surfaces share a grid only when they use the same filament;
            // otherwise each tool is decimated on its own.
            for (const FillGroup &group : fill_groups) {
                const double nozzle = nozzle_diameter_for_filament(print_config, group.filament, this->print()->is_BBL_printer());
                const AdaptiveLayerHeightRange       range     = extruder_layer_height_range(print_config, group.filament);
                const size_t                         band_size = size_t(std::max(1LL, std::llround(extruder_layer_height_for_filament(print_config, group.filament, m_config.layer_height.value) / m_slicing_params.layer_height)));

                std::vector<ExPolygons> mergeable(layer_count);
                for (size_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
                    Polygons eligible;
                    for (const Surface &surface : m_layers[layer_idx]->get_region(region_id)->fill_surfaces.surfaces) {
                        if ((surface.is_bottom() && layer_idx != 0) ||
                            surface.surface_type == stInternalVoid)
                            continue;
                        const bool sparse_surface = surface.surface_type == stInternal;
                        if ((group.sparse && sparse_surface) || (group.solid && surface.is_solid()))
                            polygons_append(eligible, to_polygons(surface.expolygon));
                    }
                    mergeable[layer_idx] = union_ex(eligible);
                }

                const std::vector<AdaptiveLayerBand> bands =
                    build_adaptive_layer_bands(layer_heights, band_size, range, band_tops);
                if (bands.empty())
                    continue;

                // Per-XY bookkeeping drives the residual repair below: it has to know which footprint of a
                // layer is already spoken for.
                std::vector<ExPolygons> carried(layer_count);
                auto mark_carried = [&carried](size_t layer_idx, const ExPolygons &area) {
                    if (area.empty())
                        return;
                    Polygons combined = to_polygons(carried[layer_idx]);
                    polygons_append(combined, to_polygons(area));
                    carried[layer_idx] = union_ex(combined);
                };
                struct PlannedFillPatch {
                    size_t     bottom_idx;
                    size_t     top_idx;
                    double     merged_height;
                    ExPolygons area;
                };
                std::vector<PlannedFillPatch> planned_fill;
                for (const AdaptiveLayerBand &band : bands) {
                    const bool first_band = band.bottom_idx == 0;
                    if (first_band && mergeable.front().empty()) {
                        for (size_t layer_idx = band.bottom_idx; layer_idx <= band.top_idx; ++ layer_idx) {
                            LayerRegion *suppressed = m_layers[layer_idx]->get_region(region_id);
                            Polygons same_nozzle_fill;
                            for (const Surface &surface : suppressed->fill_surfaces.surfaces) {
                                const bool sparse_surface = surface.surface_type == stInternal;
                                if ((group.sparse && sparse_surface) || (group.solid && surface.is_solid()))
                                    polygons_append(same_nozzle_fill, to_polygons(surface.expolygon));
                            }
                            if (! same_nozzle_fill.empty()) {
                                ExPolygons suppressed_area = union_ex(same_nozzle_fill);
                                void_fill(suppressed, suppressed_area);
                                mark_carried(layer_idx, suppressed_area);
                            }
                        }
                        continue;
                    }
                    AdaptiveLayerBand live = band;
                    std::vector<unsigned char> has_material(band.top_idx + 1, 0);
                    for (size_t layer_idx = band.bottom_idx; layer_idx <= band.top_idx; ++ layer_idx)
                        has_material[layer_idx] = ! mergeable[layer_idx].empty();
                    if (! first_band && ! pull_adaptive_band_top_to_material(live, layer_heights, range, has_material))
                        continue;
                    ExPolygons area = mergeable[first_band ? 0 : live.top_idx];
                    if (area.empty())
                        continue;
                    // The wall of this region is still extruded on every layer of the band, so a bottom
                    // shell inside it is a real floor and must survive. Voiding it would drop the surface
                    // the merged fill is laid onto.
                    Polygons blocked;
                    for (size_t layer_idx = live.bottom_idx; layer_idx < live.top_idx; ++ layer_idx)
                        for (const Surface &surface : m_layers[layer_idx]->get_region(region_id)->fill_surfaces.surfaces)
                            if (surface.is_bottom() && live.bottom_idx != 0)
                                polygons_append(blocked, to_polygons(surface.expolygon));
                    if (! blocked.empty())
                        area = diff_ex(area, union_ex(blocked));
                    if (area.empty())
                        continue;

                    for (size_t layer_idx = live.bottom_idx; layer_idx <= live.top_idx; ++ layer_idx)
                        mark_carried(layer_idx, area);
                    planned_fill.push_back(
                        { live.bottom_idx, live.top_idx, live.merged_height, std::move(area) });
                }

                std::vector<ExPolygons> remainder(layer_count);
                for (size_t layer_idx = 0; layer_idx < layer_count; ++ layer_idx) {
                    if (mergeable[layer_idx].empty())
                        continue;
                    remainder[layer_idx] = carried[layer_idx].empty() ?
                        mergeable[layer_idx] : diff_ex(mergeable[layer_idx], carried[layer_idx]);
                }

                // Residuals first form their own shortest band inside the configured extruder height range.
                // Only what is still too thin after this pass may use the neighboring-band repair.
                for (size_t top_idx = layer_count; top_idx-- > 0;) {
                    while (! remainder[top_idx].empty()) {
                        ExPolygons common = remainder[top_idx];
                        double     height = 0.;
                        size_t     bottom_idx = top_idx;
                        bool       found = false;
                        for (size_t candidate = top_idx + 1; candidate-- > 0;) {
                            height += layer_heights[candidate];
                            if (height > range.max_height + EPSILON)
                                break;
                            if (candidate != top_idx)
                                common = intersection_ex(common, remainder[candidate]);
                            if (common.empty())
                                break;
                            if (range.contains(height)) {
                                bottom_idx = candidate;
                                found = true;
                                break;
                            }
                        }
                        if (! found)
                            break;
                        // Slivers left by Clipper rounding stay in the remainder and are voided below.
                        common = adaptive_printable_pieces(std::move(common), nozzle);
                        if (common.empty())
                            break;

                        // Commit only after diff_ex() really removed the patch. A patch left in the remainder
                        // would be voided again below, wiping the merged fill just written on its group top.
                        // Without progress nothing is committed: the remainder stays as is for the repair
                        // passes, and whatever they cannot take is voided at the end.
                        std::vector<ExPolygons> shrunk;
                        shrunk.reserve(top_idx - bottom_idx + 1);
                        for (size_t layer_idx = bottom_idx; layer_idx <= top_idx; ++ layer_idx)
                            shrunk.emplace_back(diff_ex(remainder[layer_idx], common));
                        if (area(remainder[top_idx]) - area(shrunk.back()) < 0.9 * area(common))
                            break;

                        for (size_t layer_idx = bottom_idx; layer_idx <= top_idx; ++ layer_idx) {
                            mark_carried(layer_idx, common);
                            remainder[layer_idx] = std::move(shrunk[layer_idx - bottom_idx]);
                        }
                        planned_fill.push_back({ bottom_idx, top_idx, height, std::move(common) });
                        BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] fill remainder region=" << region_id
                                                 << " band=[" << bottom_idx << "," << top_idx << "]"
                                                 << " h=" << height;
                    }
                }

                // Residual-only relaxation: first give the remaining footprint to the slab directly below
                // it. This keeps the new group top on a layer where the residual geometry really exists.
                // Recovery must also respect the configured extruder height limit.
                for (size_t layer_idx = 0; layer_idx < layer_count; ++ layer_idx) {
                    if (remainder[layer_idx].empty())
                        continue;
                    std::vector<PlannedFillPatch> grown_fill;
                    const size_t planned_count = planned_fill.size();
                    for (size_t planned_idx = 0; planned_idx < planned_count; ++ planned_idx) {
                        if (remainder[layer_idx].empty())
                            break;
                        PlannedFillPatch &patch = planned_fill[planned_idx];
                        if (layer_idx == 0 || patch.top_idx != layer_idx - 1 || patch.area.empty())
                            continue;
                        const double height = patch.merged_height + layer_heights[layer_idx];
                        if (height < range.min_height - EPSILON || height > range.max_height + EPSILON)
                            continue;

                        ExPolygons extended_area = intersection_ex(patch.area, remainder[layer_idx]);
                        if (extended_area.empty())
                            continue;
                        ExPolygons remaining_area = diff_ex(patch.area, extended_area);
                        const bool split = ! remaining_area.empty();
                        if (! split) {
                            patch.top_idx       = layer_idx;
                            patch.merged_height = height;
                        } else {
                            grown_fill.push_back(
                                { patch.bottom_idx, layer_idx, height, std::move(extended_area) });
                            patch.area = std::move(remaining_area);
                        }
                        const ExPolygons &claimed_area =
                            split ? grown_fill.back().area : patch.area;
                        mark_carried(layer_idx, claimed_area);
                        remainder[layer_idx] = diff_ex(remainder[layer_idx], claimed_area);
                        BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] fill repair-up region=" << region_id
                                                 << " patch=[" << patch.bottom_idx << "," << layer_idx << "]"
                                                 << " h=" << height
                                                 << (split ? " split" : " whole")
                                                 << (height > range.max_height + EPSILON ?
                                                     " RELAXED_MAX" : "");
                    }
                    for (PlannedFillPatch &grown : grown_fill)
                        planned_fill.emplace_back(std::move(grown));
                }

                // Anything not accepted from below gets one more chance to extend the slab directly above
                // it downwards, under the same configured height limit.
                for (size_t layer_idx = layer_count; layer_idx-- > 0;) {
                    if (remainder[layer_idx].empty())
                        continue;
                    std::vector<PlannedFillPatch> grown_fill;
                    const size_t planned_count = planned_fill.size();
                    for (size_t planned_idx = 0; planned_idx < planned_count; ++ planned_idx) {
                        if (remainder[layer_idx].empty())
                            break;
                        PlannedFillPatch &patch = planned_fill[planned_idx];
                        if (patch.bottom_idx != layer_idx + 1 || patch.area.empty())
                            continue;
                        const double height = patch.merged_height + layer_heights[layer_idx];
                        if (height < range.min_height - EPSILON || height > range.max_height + EPSILON)
                            continue;

                        ExPolygons extended_area = intersection_ex(patch.area, remainder[layer_idx]);
                        if (extended_area.empty())
                            continue;
                        ExPolygons remaining_area = diff_ex(patch.area, extended_area);
                        const bool split = ! remaining_area.empty();
                        if (! split) {
                            patch.bottom_idx    = layer_idx;
                            patch.merged_height = height;
                        } else {
                            grown_fill.push_back(
                                { layer_idx, patch.top_idx, height, std::move(extended_area) });
                            patch.area = std::move(remaining_area);
                        }
                        const ExPolygons &claimed_area =
                            split ? grown_fill.back().area : patch.area;
                        mark_carried(layer_idx, claimed_area);
                        remainder[layer_idx] = diff_ex(remainder[layer_idx], claimed_area);
                        BOOST_LOG_TRIVIAL(debug) << "[adaptive_merge] fill repair region=" << region_id
                                                 << " patch=[" << layer_idx << "," << patch.top_idx << "]"
                                                 << " h=" << height
                                                 << (split ? " split" : " whole")
                                                 << (height > range.max_height + EPSILON ?
                                                     " RELAXED_MAX" : "");
                    }
                    for (PlannedFillPatch &grown : grown_fill)
                        planned_fill.emplace_back(std::move(grown));
                }

                // Planned patches never overlap on a layer (see carried above), and apply_merged_fill() only
                // reads a layer inside its own merge area, so the swallowed-layer voids of every patch can be
                // collected and applied once per layer instead of once per patch.
                std::vector<Polygons> deferred_void(layer_count);
                for (const PlannedFillPatch &patch : planned_fill)
                    apply_merged_fill(region_id, patch.bottom_idx, patch.top_idx, patch.merged_height,
                                      patch.area, group.sparse, group.solid, true, &deferred_void);

                // A footprint that exists for less than this nozzle's minimum printable height cannot be
                // assigned to any neighboring band. This branch plans fill whose nozzle differs from the
                // wall nozzle, so another XY patch touching the layer does not make this remainder legal:
                // leaving it behind would extrude it at the base layer height.
                for (size_t layer_idx = 0; layer_idx < layer_count; ++ layer_idx) {
                    if (! remainder[layer_idx].empty())
                        polygons_append(deferred_void[layer_idx], to_polygons(remainder[layer_idx]));
                    if (! deferred_void[layer_idx].empty())
                        void_fill(m_layers[layer_idx]->get_region(region_id), union_ex(deferred_void[layer_idx]));
                }
            }
        }
    }
}


} // namespace Slic3r
