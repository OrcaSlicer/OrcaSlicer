///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "Layers.hpp"

#include "../include/PathVertex.hpp"
#include "Utils.hpp"

#include <assert.h>
#include <algorithm>
#include <cmath>
#include <cfloat>
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

static double segment_extrusion_volume(const Vec3& prev, const Vec3& curr, float mm3_per_mm)
{
    if (mm3_per_mm <= 0.0f)
        return 0.0;
    const float dx = curr[0] - prev[0];
    const float dy = curr[1] - prev[1];
    const float dz = curr[2] - prev[2];
    const double length = std::sqrt(static_cast<double>(dx * dx + dy * dy + dz * dz));
    return static_cast<double>(mm3_per_mm) * length;
}

void Layers::update(const PathVertex& vertex, uint32_t vertex_id)
{
    const bool has_prev = m_last_position[0] != FLT_MAX;

    if (m_items.empty() || vertex.layer_id == m_items.size()) {
        // this code assumes that gcode paths are sent sequentially, one layer after the other
        assert(vertex.layer_id == static_cast<uint32_t>(m_items.size()));
        Item& item = m_items.emplace_back(Item());
        if (vertex.type == EMoveType::Extrude && vertex.role != EGCodeExtrusionRole::Custom)
            item.z = vertex.position[2];
        item.range.set(vertex_id, vertex_id);
        item.times = vertex.times;
        item.contains_colorprint_options |= is_colorprint_option(vertex);
        if (vertex.type == EMoveType::Extrude && has_prev)
            item.extrusion_volume += segment_extrusion_volume(m_last_position, vertex.position, vertex.mm3_per_mm);
    }
    else {
        Item& item = m_items.back();
        if (vertex.type == EMoveType::Extrude && vertex.role != EGCodeExtrusionRole::Custom && item.z != vertex.position[2])
            item.z = vertex.position[2];
        item.range.set_max(vertex_id);
        for (size_t i = 0; i < TIME_MODES_COUNT; ++i) {
            item.times[i] += vertex.times[i];
        }
        item.contains_colorprint_options |= is_colorprint_option(vertex);
        if (vertex.type == EMoveType::Extrude && has_prev)
            item.extrusion_volume += segment_extrusion_volume(m_last_position, vertex.position, vertex.mm3_per_mm);
    }

    m_last_position = vertex.position;
}

void Layers::reset()
{
    m_items.clear();
    m_view_range.reset();
    m_last_position = { FLT_MAX, FLT_MAX, FLT_MAX };
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

std::vector<double> Layers::get_extrusion_volumes() const
{
    std::vector<double> ret;
    ret.reserve(m_items.size());
    for (const Item& item : m_items) {
        ret.emplace_back(item.extrusion_volume);
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
    auto iter = std::upper_bound(m_items.begin(), m_items.end(), z, [](float z, const Item& item) { return item.z > z; });
    return std::distance(m_items.begin(), iter);
}

size_t Layers::size_in_bytes_cpu() const
{
    size_t ret = STDVEC_MEMSIZE(m_items, Item);
    return ret;
}

} // namespace libvgcode
