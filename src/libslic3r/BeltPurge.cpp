// BELT_MOD -- backend of the belt purge tower, the belt replacement for the classic wipe tower
// (ported from OrcaSlicer's belt branch). The classic tower writes its own G-code text, which never
// passes through the writer's machine frame mapping, so it cannot exist on a belt. Instead the GUI
// keeps a sacrificial prism next to the parts (GUI/BeltPurgeTower.cpp), marked with
// belt_purge_tower_object and flush_into_objects; it is sliced like any object, so it picks up the
// belt rotation, and filament-change purging is routed into its extrusions with the
// flush-into-objects machinery. Nothing here does anything unless the print is a belt printer with
// the belt purge tower enabled.
//
//   Print::has_belt_purge_tower()             - is the belt purge tower active?
//   Print::_plan_belt_purge()                 - the psWipeTower step on a belt
//   PrintObject::belt_truncate_layers_above() - cancel the prism past the last swap
//   PrintObject::belt_restore_truncated_layers()
//
// One layer grid: a toolchange on object B is purged into the prism only if the prism has a layer
// at B's print_z. Orca snapped the sliced grids after the fact; here Print::update_belt_schedule()
// rounds every object's schedule offset to a multiple of the layer height instead, so the grids
// coincide by construction and the per-move un-rotation stays exact.

#include "Print.hpp"
#include "PrintConfig.hpp"
#include "Exception.hpp"
#include "GCode/ToolOrdering.hpp"
#include "Layer.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "LocalesUtils.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>

#include <boost/log/trivial.hpp>

namespace Slic3r {

bool Print::has_belt_purge_tower() const
{
    // Its own purge-tower "type", gated by the belt-only enable_belt_purge_tower option (not the
    // classic enable_prime_tower).
    if (! (m_config.belt_printer.value
           && m_config.enable_belt_purge_tower.value
           && ! m_config.spiral_mode.value
           && m_config.filament_diameter.values.size() > 1))
        return false;
    return std::any_of(m_objects.begin(), m_objects.end(), [](const PrintObject *object) {
        return object->config().belt_purge_tower_object.value;
    });
}

// The psWipeTower step on a belt: plan filament-change purging into the prism (and any other
// flush_into_* object) using the flush-into-objects machinery, without a classic wipe tower. The
// toolchange itself is emitted by GCode::set_extruder() via change_filament_gcode; the overrides
// marked here make the new filament's first extrusions land in the prism.
void Print::_plan_belt_purge()
{
    m_wipe_tower_data.clear();

    // psWipeTower may be invalidated without posSlice (a filament-map or tool-ordering change).
    // Restore a prism shortened by the previous plan so a newly higher toolchange can use its
    // original layers.
    PrintObject *prism = nullptr;
    for (PrintObject *object : m_objects)
        if (object->config().belt_purge_tower_object.value) {
            object->belt_restore_truncated_layers();
            if (prism == nullptr && ! object->layers().empty())
                prism = object;
        }

    const unsigned int number_of_extruders = (unsigned int) m_config.filament_colour.values.size();

    // No initial priming extrusions: there is no tower to prime on. m_wipe_tower_data.tool_ordering
    // is m_tool_ordering, the ordering the G-code stage reads.
    m_wipe_tower_data.tool_ordering = ToolOrdering(*this, (unsigned int) -1, false);
    m_wipe_tower_data.tool_ordering.sort_and_build_data(*this, (unsigned int) -1, false);

    if (m_wipe_tower_data.tool_ordering.empty() || m_wipe_tower_data.tool_ordering.last_extruder() == unsigned(-1))
        throw Slic3r::SlicingError(_u8L("The print is empty. The model is not printable with current print settings."));

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

    // Flush volumes per filament pair, mirroring the classic wipe tower: the full flush matrix for
    // single extruder multi material with purging enabled, the plain prime volume otherwise.
    const std::vector<float> flush_matrix(cast<float>(
        get_flush_volumes_matrix(m_config.flush_volumes_matrix.values, 0, m_config.nozzle_diameter.values.size())));
    std::vector<std::vector<float>> wipe_volumes;
    for (unsigned int i = 0; i < number_of_extruders; ++i)
        wipe_volumes.push_back(std::vector<float>(flush_matrix.begin() + i * number_of_extruders,
                                                  flush_matrix.begin() + (i + 1) * number_of_extruders));
    const bool  use_flush_matrix = m_config.purge_in_prime_tower && m_config.single_extruder_multi_material;
    const float flush_multiplier = (float) m_config.flush_multiplier.get_at(0);

    // Cancel the prism early: the highest print_z with a toolchange is where it may stop, so drop
    // the layers above it. This must happen before the marking loop: ensure_perimeters_infills_order
    // overrides the prism's extrusions on every layer (a dedicated flush object), so truncating
    // afterwards would leave overrides pointing into deleted layers.
    {
        double       last_tc_z = -1.;
        unsigned int cur_ext   = m_wipe_tower_data.tool_ordering.first_extruder();
        for (const auto &lt : m_wipe_tower_data.tool_ordering.layer_tools())
            for (const unsigned int e : lt.extruders)
                if (e != cur_ext) { last_tc_z = lt.print_z; cur_ext = e; }
        if (last_tc_z >= 0. && prism != nullptr)
            prism->belt_truncate_layers_above(last_tc_z);
    }

    int   tc_layers = 0, tc_layers_covered = 0;
    float total_leftover = 0.f, worst_layer_leftover = 0.f;
    double worst_layer_z = 0.;

    unsigned int current_extruder_id = m_wipe_tower_data.tool_ordering.first_extruder();
    for (auto &layer_tools : m_wipe_tower_data.tool_ordering.layer_tools()) {
        float layer_leftover = 0.f;
        bool  layer_has_tc   = false;
        for (const unsigned int extruder_id : layer_tools.extruders) {
            if (extruder_id == current_extruder_id)
                continue;
            if (! layer_has_tc) {
                layer_has_tc = true;
                ++ tc_layers;
                if (prism != nullptr && prism->get_layer_at_printz(layer_tools.print_z, EPSILON) != nullptr)
                    ++ tc_layers_covered;
            }
            const float volume_to_wipe = use_flush_matrix ?
                wipe_volumes[current_extruder_id][extruder_id] * flush_multiplier :
                (float) m_config.prime_volume;
            const float leftover = layer_tools.wiping_extrusions().mark_wiping_extrusions(*this, current_extruder_id, extruder_id, volume_to_wipe);
            layer_leftover += leftover;
            current_extruder_id = extruder_id;
        }
        // Unclaimed fill entities are left alone: psWipeTower can rerun without regenerating
        // infill, and a later tool ordering may need entities this plan did not claim.
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

    BOOST_LOG_TRIVIAL(debug) << "belt purge: " << tc_layers_covered << "/" << tc_layers
        << " toolchange layers land on a prism layer, leftover " << total_leftover << " mm3";

    if (tc_layers > 0 && tc_layers_covered == 0)
        this->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
            _u8L("The belt purge tower has no layer at any filament change, so nothing is purged into it. "
                 "Move the parts or toggle the belt purge tower off and on to regenerate it."));
    else if (total_leftover > 1.f)
        this->active_step_add_warning(PrintStateBase::WarningLevel::CRITICAL,
            Slic3r::format(_u8L("The belt purge tower cannot absorb the full purge volume: %1% mm³ in total could not "
                                "be purged (worst layer: %2% mm³ at height %3%). The print may show colour bleeding. "
                                "Increase the belt purge tower width, or reduce the flushing volumes."),
                           int(std::ceil(total_leftover)), int(std::ceil(worst_layer_leftover)),
                           Slic3r::float_to_string_decimal_point(worst_layer_z, 2)));
}

// Drop the layers strictly above z: the prism stops at the last colour swap instead of running up
// the rest of the belt. Each layer's cross-section is already sliced, so removing upper layers does
// not affect the last toolchange's coverage. The layers are kept for belt_restore_truncated_layers().
size_t PrintObject::belt_truncate_layers_above(coordf_t z)
{
    assert(m_belt_truncated_layers.empty());
    size_t keep = m_layers.size();
    while (keep > 0 && m_layers[keep - 1]->print_z > z + EPSILON)
        -- keep;
    if (keep >= m_layers.size())
        return 0;
    const size_t removed = m_layers.size() - keep;
    m_belt_truncated_layers.assign(m_layers.begin() + keep, m_layers.end());
    m_layers.resize(keep);
    if (! m_layers.empty())
        m_layers.back()->upper_layer = nullptr;
    return removed;
}

void PrintObject::belt_restore_truncated_layers()
{
    if (m_belt_truncated_layers.empty())
        return;
    m_layers.insert(m_layers.end(), m_belt_truncated_layers.begin(), m_belt_truncated_layers.end());
    m_belt_truncated_layers.clear();
    for (size_t i = 0; i < m_layers.size(); ++ i) {
        m_layers[i]->lower_layer = i == 0 ? nullptr : m_layers[i - 1];
        m_layers[i]->upper_layer = i + 1 < m_layers.size() ? m_layers[i + 1] : nullptr;
    }
}

} // namespace Slic3r
