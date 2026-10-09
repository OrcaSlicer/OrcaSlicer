// ORCA-Belt: backend of the belt purge tower (the belt replacement for the
// classic wipe/prime tower).
//
// Kept in its own translation unit so the belt-purge logic stays out of the way
// of unrelated upstream changes to Print.cpp / PrintObjectSlice.cpp and carries
// no regression risk for normal printers: none of these methods do anything
// unless the print is a belt printer with the belt purge tower enabled.
//
//   Print::has_belt_purge_tower()      - is the belt purge tower active?
//   Print::_align_belt_purge_layers()  - snap the prism's layer grid onto the
//                                        printed objects' grid
//   Print::_plan_belt_purge()          - route filament-change purging into the
//                                        prism (flush-into-objects), no wipe tower
//   PrintObject::belt_shift_layer_grid()      - shift a sliced layer grid
//   PrintObject::belt_truncate_layers_above() - cancel the prism past the last swap
//
// (Declarations live in Print.hpp alongside the rest of the Print interface.)

#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Exception.hpp"
#include "GCode/ToolOrdering.hpp"
#include "Layer.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "LocalesUtils.hpp"
#include "libslic3r.h"
#include "BeltBrim.hpp"
#include "PrintBase.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include <boost/log/trivial.hpp>
#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace Slic3r {

// Belt purge prism: purging after filament changes is routed into a sliced
// prism object via the flush-into-objects machinery instead of a wipe tower.
bool Print::has_belt_purge_tower() const
{
    // Its own purge-tower "type", gated by the belt-only enable_belt_purge_tower
    // option (not the classic enable_prime_tower).
    if (!(m_config.belt_printer.value
          && m_config.enable_belt_purge_tower.value
          && !m_config.spiral_mode.value
          && m_config.filament_diameter.values.size() > 1))
        return false;

    return std::any_of(m_objects.begin(), m_objects.end(), [](const PrintObject *object) {
        return object->config().belt_purge_tower_object.value;
    });
}

// Belt mode: align ALL objects on the plate (the printed objects AND the purge
// prism) onto one common layer grid, so the prism can absorb every toolchange.
//
// After belt slicing each object's layer print_z carries a per-object global z
// offset (mesh-vertex-scan belt_z_shift + instance-Y-dependent terms), so
// objects at different belt-Y positions get layer grids with DIFFERENT residues
// (mod layer height). Purge marking looks absorbers up with
// get_layer_at_printz(lt.print_z, EPSILON), so a toolchange on object B only
// absorbs into the prism if the prism has a layer at B's print_z. Snapping only
// the prism to one object therefore worked for a single (assembled) multi-color
// object but failed with multiple separate objects — the prism could follow only
// one grid, and toolchanges on the others went unabsorbed ("multiple layer
// grids" warning).
//
// Fix: pick one reference grid (the tallest object) and shift every object onto
// it. Each shift is at most half a layer height — a sub-100µm move along the
// belt, the very same mechanism the per-object global_z_offset already uses, and
// it keeps each object internally consistent (belt_shift_layer_grid moves the
// object's layers, its support layers, and its belt floor together). Equal layer
// height across objects is enforced by Print::validate(), so once residues match
// every object steps on the same lattice {ref_offset + k*h} and every toolchange
// layer coincides with a prism layer.
void Print::_align_belt_purge_layers()
{
    PrintObject *prism = nullptr;
    for (PrintObject *po : m_objects)
        if (po->config().belt_purge_tower_object.value && !po->layers().empty()) {
            prism = po;
            break;
        }
    if (prism == nullptr || prism->layers().empty())
        return;

    const double h = prism->config().layer_height.value;
    if (h <= EPSILON)
        return;

    // Grid residue of an object's layer grid: identical for all of an object's
    // layers above the first since they step by h.
    auto grid_offset = [h](const PrintObject *po) -> double {
        if (po->layers().empty())
            return 0.;
        const double z = po->layers().front()->print_z;
        return z - std::floor(z / h) * h; // in [0, h)
    };

    // Reference grid: the tallest non-prism object (proxy for the object with
    // the most toolchange layers — minimizes how far the rest must move).
    const PrintObject *ref     = nullptr;
    double             ref_top = -std::numeric_limits<double>::max();
    for (const PrintObject *po : m_objects) {
        if (po->config().belt_purge_tower_object.value || po->layers().empty())
            continue;
        const double top = po->layers().back()->print_z;
        if (top > ref_top) {
            ref_top = top;
            ref     = po;
        }
    }
    if (ref == nullptr)
        return;

    const double ref_offset = grid_offset(ref);

    // Snap every object (printed objects AND the prism) onto the reference grid.
    for (PrintObject *po : m_objects) {
        if (po->layers().empty())
            continue;
        double delta = ref_offset - grid_offset(po);
        if (delta > 0.5 * h)
            delta -= h;
        else if (delta <= -0.5 * h)
            delta += h;
        po->belt_shift_layer_grid(delta); // no-op for the reference object (delta ~ 0)
    }
}

// Belt mode replacement for _make_wipe_tower(): plan filament-change purging
// into the belt purge prism (and any other flush_into_* object) using the
// flush-into-objects machinery, without generating classic wipe tower G-code.
// The toolchange itself is emitted by GCode::set_extruder() via the
// change_filament_gcode macro; the overrides marked here make the new
// filament's first extrusions land in the purge prism.
void Print::_plan_belt_purge()
{
    m_wipe_tower_data.clear();

    // psWipeTower may be invalidated without posSlice (for example after a
    // filament-map or tool-ordering change). Restore a prism shortened by the
    // previous plan so a newly higher toolchange can use its original layers.
    for (PrintObject *po : m_objects)
        if (po->config().belt_purge_tower_object.value)
            po->belt_undo_purge_plan();

    // Must run before ToolOrdering is built: LayerTools merge per-object layer
    // print_z values, and the prism only absorbs purge where its (snapped)
    // layers coincide with the toolchange layers.
    this->_align_belt_purge_layers();

    const unsigned int number_of_extruders = (unsigned int) m_config.filament_colour.values.size();

    // No initial priming extrusions: there is no tower to prime on.
    m_wipe_tower_data.tool_ordering = ToolOrdering(*this, (unsigned int) -1, false);
    m_wipe_tower_data.tool_ordering.sort_and_build_data(*this, (unsigned int) -1, false);

    if (m_wipe_tower_data.tool_ordering.empty() || m_wipe_tower_data.tool_ordering.last_extruder() == unsigned(-1))
        throw Slic3r::SlicingError("The print is empty. The model is not printable with current print settings.");

    // Is there any filament change at all? Not ToolOrdering::has_wipe_tower(): that reads the
    // FIRST layer's flag, and on a belt the first layer may be a brim apron band, which carries
    // neither object nor support and so never gets the flag even when the print changes filament.
    {
        bool         any_change = false;
        unsigned int cur        = m_wipe_tower_data.tool_ordering.first_extruder();
        for (const auto &lt : m_wipe_tower_data.tool_ordering.layer_tools())
            for (const unsigned int e : lt.extruders)
                if (e != cur) { any_change = true; cur = e; }
        if (! any_change)
            return;
    }

    this->throw_if_canceled();

    // Flush volumes per filament pair, mirroring the generic wipe tower path:
    // full flush matrix for single extruder multi material with purging enabled,
    // plain prime volume otherwise.
    std::vector<float> flush_matrix(cast<float>(
        get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, 0, m_config.nozzle_diameter.values.size())));
    std::vector<std::vector<float>> wipe_volumes;
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * number_of_extruders,
                                                  flush_matrix.begin() + (i + 1) * number_of_extruders));
    const bool  use_flush_matrix = m_config.purge_in_prime_tower && m_config.single_extruder_multi_material;
    const float flush_multiplier = (float) m_config.flush_multiplier.get_at(0);

    // Cancel the purge prism early: pre-scan the tool ordering for the highest
    // print_z that actually has a toolchange, then drop the prism's layers above
    // it so the tower stops at the last color swap (saves filament/time). This
    // MUST happen before the marking loop below: ensure_perimeters_infills_order
    // force-overrides the prism's extrusions on every layer (it is a dedicated
    // flush object), so truncating afterwards would leave dangling overrides
    // pointing into deleted layers.
    {
        // The tool ordering covers the WHOLE print, and the prism is a printed
        // object in it. Left unbounded, the scan below sees the prism's own
        // toolchanges on layers above every model object -- the prism runs past
        // them by design (ramp/height compensation at the tilted ends) -- so
        // last_tc_z lands at the prism's own top and the truncation cancels
        // nothing. The tower ends up justifying its own existence.
        //
        // Nothing above the tallest printed object can require a color change,
        // so bound the scan there. On MCTEST5 that is 197 toolchanges spanning
        // z=154.00..193.20 with the tallest object topping out at 153.80, i.e.
        // 39.4 mm of tower that no swap ever needed.
        // Support layers count too: on a belt they can extend above the object's
        // own top, and a toolchange there is a real one.
        double obj_top_z = -1.;
        for (const PrintObject *po : m_objects) {
            if (po->config().belt_purge_tower_object.value)
                continue;
            if (!po->layers().empty())
                obj_top_z = std::max(obj_top_z, po->layers().back()->print_z);
            if (!po->support_layers().empty())
                obj_top_z = std::max(obj_top_z, po->support_layers().back()->print_z);
        }

        double       last_tc_z   = -1.;
        unsigned int cur_ext     = m_wipe_tower_data.tool_ordering.first_extruder();
        for (const auto &lt : m_wipe_tower_data.tool_ordering.layer_tools()) {
            // layer_tools() is ordered by print_z ascending.
            if (obj_top_z >= 0. && lt.print_z > obj_top_z + EPSILON)
                break;
            for (const unsigned int e : lt.extruders)
                if (e != cur_ext) { last_tc_z = lt.print_z; cur_ext = e; }
        }
        // Deliberately NOT cancelling the prism outright when no object toolchange
        // exists: belt_truncate_layers_above(0.) empties m_layers, and an object
        // with zero layers is not something the rest of the pipeline expects. The
        // GUI already declines to create a prism unless more than one filament is
        // in use, so this case is a stale prism, not a hot path -- leave it whole
        // rather than risk a zero-layer object.
        if (last_tc_z >= 0.)
            for (PrintObject *po : m_objects)
                if (po->config().belt_purge_tower_object.value && !po->layers().empty()) {
                    po->belt_truncate_layers_above(last_tc_z);
                    break;
                }
    }

    // The prism only absorbs purge at toolchange layers whose print_z coincides
    // with one of its own layers.
    PrintObject *prism_po = nullptr;
    for (PrintObject *po : m_objects)
        if (po->config().belt_purge_tower_object.value && !po->layers().empty()) { prism_po = po; break; }

    float  total_leftover      = 0.f;
    float  worst_layer_leftover = 0.f;
    double worst_layer_z       = 0.;

    unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.first_extruder();
    for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) {
        float layer_leftover = 0.f;
        for (const unsigned int extruder_id : layer_tools.extruders) {
            if (extruder_id == current_extruder_id)
                continue;
            float volume_to_wipe = use_flush_matrix ?
                wipe_volumes[current_extruder_id][extruder_id] * flush_multiplier :
                (float) m_config.prime_volume;
            float leftover = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id,
                                                                                    volume_to_wipe);
            layer_leftover += leftover;
            current_extruder_id = extruder_id;
        }

        // Plastic saving: drop the prism's fills that no toolchange on this layer
        // claimed. At this point the prism's OVERRIDDEN fills are exactly the
        // purge; the rest would print as solid infill in the prism's own filament
        // for nothing -- which is the whole prism on a layer with no toolchange
        // (141 of 692 layers on MCTEST5 before the truncation fix). Perimeters are
        // left alone so the bar keeps a continuous wall along the belt.
        //
        // Non-destructive: the entities are stashed with their positions and put
        // back by belt_restore_dropped_fills() at the top of the next plan. An
        // earlier version deleted them outright, which broke replanning when a
        // later tool ordering needed what this one had not claimed -- that is why
        // it was removed rather than kept.
        if (prism_po != nullptr) {
            const auto &we = layer_tools.wiping_extrusions();
            prism_po->belt_drop_unclaimed_fills(
                prism_po->get_layer_at_printz(layer_tools.print_z, EPSILON),
                [&we, prism_po](const ExtrusionEntity *e) { return we.is_entity_overridden(e, prism_po, 0); });
        }

        layer_tools.wiping_extrusions().ensure_perimeters_infills_order(*this);
        if (layer_leftover > 0.f) {
            total_leftover += layer_leftover;
            if (layer_leftover > worst_layer_leftover) {
                worst_layer_leftover = layer_leftover;
                worst_layer_z        = layer_tools.print_z;
            }
        }
        this->throw_if_canceled();
    }

    if (total_leftover > 1.f) {
        this->active_step_add_warning(
            PrintStateBase::WarningLevel::CRITICAL,
            Slic3r::format(_u8L("The belt purge tower cannot absorb the full purge volume: %1% mm³ in total could not "
                                "be purged (worst layer: %2% mm³ at height %3%). The print may show color bleeding. "
                                "Increase the belt purge tower width, or reduce flushing volumes."),
                           int(std::ceil(total_leftover)), int(std::ceil(worst_layer_leftover)),
                           Slic3r::float_to_string_decimal_point(worst_layer_z, 2)));
    }
}

// Belt mode: shift the sliced layer grid by delta. Mirrors the global_z_offset
// application in slice() — layer print_z and belt_floor_z_shift move together
// so belt floor clipping stays consistent with the shifted grid. Used by
// Print::_align_belt_purge_layers() to snap the purge prism onto the printed
// objects' layer grid; |delta| <= half a layer height, i.e. a sub-layer shift
// of the prism along the belt.
void PrintObject::belt_shift_layer_grid(double delta)
{
    if (std::abs(delta) < EPSILON)
        return;
    for (Layer *layer : m_layers)
        layer->print_z += delta;
    for (SupportLayer *layer : m_support_layers)
        layer->print_z += delta;
    // The brim's apron bands below the first layer carry their own print_z.
    for (BeltBrimBand &band : m_belt_brim_prologue)
        band.print_z += delta;
    m_slicing_params.belt_floor_z_shift += delta;
    // The grid stays shifted across a support-only or brim-only change (posSlice does
    // not rerun), so everything slice() derived from it has to follow: the cached floor
    // that update_slicing_parameters() restores, and the global offset the organic
    // support layers and the adaptive infill octree are placed with. Left alone, the
    // next alignment finds a delta of 0 and the floor and the supports sit up to half
    // a layer off the grid, unlike a fresh slice.
    if (m_belt_floor_z_shift_cache_valid)
        m_belt_floor_z_shift_cached += delta;
    m_belt_global_z_offset += delta;
}

// Belt mode: drop layers strictly above z (used to cancel the purge prism early
// once there are no more toolchanges above z, so the tower stops at the last
// color swap instead of wasting filament up the rest of the belt). Each layer's
// cross-section is already sliced, so removing upper layers does not affect the
// last toolchange's coverage. Deletes the Layer objects and clears the new top
// layer's upper-layer link. Returns the number of layers removed.
size_t PrintObject::belt_truncate_layers_above(coordf_t z)
{
    // A repeated plan always starts from the restored full layer set.
    assert(m_belt_truncated_layers.empty());
    size_t keep = m_layers.size();
    while (keep > 0 && m_layers[keep - 1]->print_z > z + EPSILON)
        --keep;
    if (keep >= m_layers.size())
        return 0;
    const size_t removed = m_layers.size() - keep;
    m_belt_truncated_layers.assign(m_layers.begin() + keep, m_layers.end());
    m_layers.resize(keep);
    if (!m_layers.empty())
        m_layers.back()->upper_layer = nullptr;
    return removed;
}

// Plastic saving on the purge prism: keep only the fills a toolchange claimed.
//
// Called per layer from _plan_belt_purge(), after the real-purge marking and
// BEFORE ensure_perimeters_infills_order() -- that pass force-overrides every
// remaining fill on the prism (it is a dedicated flush object), so afterwards
// everything looks claimed and nothing could be distinguished.
size_t PrintObject::belt_drop_unclaimed_fills(Layer *layer, const std::function<bool(const ExtrusionEntity*)> &claimed)
{
    if (layer == nullptr)
        return 0;
    size_t dropped = 0;
    for (size_t ri = 0; ri < layer->regions().size(); ++ri) {
        LayerRegion *lr = layer->get_region(ri);
        auto        &ents = lr->fills.entities;
        ExtrusionEntitiesPtr keep;
        keep.reserve(ents.size());
        for (size_t i = 0; i < ents.size(); ++i) {
            if (claimed(ents[i])) {
                keep.emplace_back(ents[i]);
            } else {
                // Stash with its original index so the restore is exact.
                m_belt_dropped_fills.push_back(BeltDroppedFill{ layer, ri, i, ents[i] });
                ++dropped;
            }
        }
        ents = std::move(keep);
    }
    return dropped;
}

void PrintObject::belt_restore_dropped_fills()
{
    if (m_belt_dropped_fills.empty())
        return;
    // Ascending index per (layer, region): inserting in that order lands every
    // entity back at its original position, because each insertion shifts only
    // the entries after it, which are themselves still to be inserted.
    std::stable_sort(m_belt_dropped_fills.begin(), m_belt_dropped_fills.end(),
                     [](const BeltDroppedFill &a, const BeltDroppedFill &b) {
                         if (a.layer != b.layer)           return a.layer < b.layer;
                         if (a.region_idx != b.region_idx) return a.region_idx < b.region_idx;
                         return a.index < b.index;
                     });
    for (const BeltDroppedFill &d : m_belt_dropped_fills) {
        auto &ents = d.layer->get_region(d.region_idx)->fills.entities;
        ents.insert(ents.begin() + std::min(d.index, ents.size()), d.entity);
    }
    m_belt_dropped_fills.clear();
}

void PrintObject::belt_restore_truncated_layers()
{
    if (m_belt_truncated_layers.empty())
        return;

    m_layers.insert(m_layers.end(), m_belt_truncated_layers.begin(), m_belt_truncated_layers.end());
    m_belt_truncated_layers.clear();
    for (size_t i = 0; i < m_layers.size(); ++i) {
        m_layers[i]->lower_layer = i == 0 ? nullptr : m_layers[i - 1];
        m_layers[i]->upper_layer = i + 1 < m_layers.size() ? m_layers[i + 1] : nullptr;
    }
}

} // namespace Slic3r
