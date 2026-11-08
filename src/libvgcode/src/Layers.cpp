///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "Layers.hpp"

#include "../include/PathVertex.hpp"
#include "Utils.hpp"

#include <assert.h>
#include <algorithm>
#include "../include/Types.hpp"
#include <cstdint>
#include <cstddef>
#include <vector>
#include <iterator>

namespace libvgcode {

static bool is_colorprint_option(const PathVertex& v)
{
    return v.type == EMoveType::PausePrint || v.type == EMoveType::CustomGCode;
}

void Layers::update(const PathVertex& vertex, uint32_t vertex_id)
{
    // UNLAYERED_MOD: track the Z extent and the tool axis of the layer's real extrusions. A tilted
    // layer has no single Z, so the slab it occupies is what the labels and shell clipping can use.
    auto accumulate_extrusion = [this, &vertex](Item& item) {
        if (vertex.type != EMoveType::Extrude || vertex.role == EGCodeExtrusionRole::Custom)
            return;
        const float z = vertex.position[2];
        item.z = z;
        item.z_min = std::min(item.z_min, z);
        item.z_max = std::max(item.z_max, z);
        // The first real extrusion sets the layer's orientation. In ByGroup mode a single layer can
        // interleave differently oriented chunks; the layer then simply is not planar, and its one
        // Z was meaningless to begin with.
        if (item.tool_axis_id == 0)
            item.tool_axis_id = vertex.tool_axis_id;
        if (item.tool_axis_id != 0 || item.z_max - item.z_min > NONPLANAR_Z_SPAN)
            m_all_planar = false;
    };

    // UNLAYERED_MOD: keyed on the source layer id changing, not on it equalling the item count.
    // This code assumes gcode paths are sent sequentially, one layer after the other, and used to
    // assume every layer change is followed by at least one move. A layer change with no move (an
    // empty layer) leaves a gap in the ids: with `layer_id == size()` every later vertex fell into
    // the previous item for the rest of the file (the preview showed one giant last layer and
    // lost everything after the gap), and `>=` would open a new item for each of the first few
    // vertices after the gap. Remembering the id of the current item keeps one item per source
    // layer that has moves, whatever ids were skipped.
    if (m_items.empty() || vertex.layer_id != m_last_layer_id) {
        m_last_layer_id = vertex.layer_id;
        Item& item = m_items.emplace_back(Item());
        accumulate_extrusion(item);
        item.range.set(vertex_id, vertex_id);
        item.times = vertex.times;
        item.contains_colorprint_options |= is_colorprint_option(vertex);
    }
    else {
        Item& item = m_items.back();
        accumulate_extrusion(item);
        item.range.set_max(vertex_id);
        for (size_t i = 0; i < TIME_MODES_COUNT; ++i) {
            item.times[i] += vertex.times[i];
        }
        item.contains_colorprint_options |= is_colorprint_option(vertex);
    }
}

void Layers::reset()
{
    m_items.clear();
    m_view_range.reset();
    m_all_planar = true;
    m_last_layer_id = 0;
}

std::vector<float> Layers::get_times(ETimeMode mode) const
{
    std::vector<float> ret;
    if (mode < ETimeMode::COUNT) {
        for (const Item& item : m_items) {
            ret.emplace_back(item.times[static_cast<size_t>(mode)]);
        }
    }
    return ret;
}

std::vector<float> Layers::get_zs() const
{
    std::vector<float> ret;
    ret.reserve(m_items.size());
    for (const Item& item : m_items) {
        ret.emplace_back(item.z);
    }
    return ret;
}

size_t Layers::get_layer_id_at(float z) const
{
    if (m_all_planar) {
        auto iter = std::upper_bound(m_items.begin(), m_items.end(), z, [](float z, const Item& item) { return item.z > z; });
        return std::distance(m_items.begin(), iter);
    }

    // UNLAYERED_MOD: with tilted layers the per-layer z is not monotonic, so the binary search above
    // would return nonsense. Fall back to a linear scan for the first layer whose slab contains z,
    // and failing that the layer whose slab starts closest above it.
    size_t best = m_items.size();
    float best_dist = FLT_MAX;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const float z_min = get_layer_z_min(i);
        const float z_max = get_layer_z_max(i);
        if (z >= z_min && z <= z_max)
            return i;
        const float dist = (z < z_min) ? (z_min - z) : (z - z_max);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

size_t Layers::size_in_bytes_cpu() const
{
    size_t ret = STDVEC_MEMSIZE(m_items, Item);
    return ret;
}

} // namespace libvgcode
