///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_LAYERS_HPP
#define VGCODE_LAYERS_HPP

#include "Range.hpp"
#include <cstdint>
#include <cstddef>
#include "../include/Types.hpp"
#include <array>
#include <vector>

#include <cfloat>

namespace libvgcode {

struct PathVertex;

class Layers
{
public:
    // UNLAYERED_MOD: a layer whose extrusions span more height than this is not horizontal even
    // when the tool stays straight up (nonplanar layers' curved shells; ported from feat/layer-map
    // c5b96b1c74). Z anti-aliasing stays well inside it.
    static constexpr float NONPLANAR_Z_SPAN = 1.0f;

    void update(const PathVertex& vertex, uint32_t vertex_id);
    void reset();
    
    bool empty() const { return m_items.empty(); }
    std::size_t count() const { return m_items.size(); }

    std::vector<float> get_times(ETimeMode mode) const;
    std::vector<float> get_zs() const;
    
    float get_layer_time(ETimeMode mode, std::size_t layer_id) const {
        return (mode < ETimeMode::COUNT&& layer_id < m_items.size()) ?
            m_items[layer_id].times[static_cast<std::size_t>(mode)] : 0.0f;
    }
    float get_layer_z(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].z : 0.0f;
    }
    // UNLAYERED_MOD: a tilted layer spans a range of Z instead of sitting at one height. Layers with
    // no real extrusions (custom G-code only) have an empty extent and fall back to their z.
    float get_layer_z_min(std::size_t layer_id) const {
        if (layer_id >= m_items.size())
            return 0.0f;
        const Item& item = m_items[layer_id];
        return (item.z_min <= item.z_max) ? item.z_min : item.z;
    }
    float get_layer_z_max(std::size_t layer_id) const {
        if (layer_id >= m_items.size())
            return 0.0f;
        const Item& item = m_items[layer_id];
        return (item.z_min <= item.z_max) ? item.z_max : item.z;
    }
    // True when the layer was printed with the tool straight up, i.e. it really is a horizontal
    // slice and its single Z is meaningful.
    bool is_layer_planar(std::size_t layer_id) const {
        if (layer_id >= m_items.size())
            return true;
        const Item& item = m_items[layer_id];
        return item.tool_axis_id == 0 && !(item.z_max - item.z_min > NONPLANAR_Z_SPAN);
    }
    uint16_t get_layer_tool_axis_id(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].tool_axis_id : uint16_t(0);
    }
    // True when every layer is horizontal - the ordinary case, and the one where the layer slider
    // can still be driven by Z values.
    bool all_planar() const { return m_all_planar; }
    std::size_t get_layer_id_at(float z) const;
    
    const Interval& get_view_range() const { return m_view_range.get(); }
    void set_view_range(const Interval& range) { set_view_range(range[0], range[1]); }
    void set_view_range(Interval::value_type min, Interval::value_type max) { m_view_range.set(min, max); }
    
    bool layer_contains_colorprint_options(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].contains_colorprint_options : false;
    }

    std::size_t size_in_bytes_cpu() const;

private:
    struct Item
    {
        float z{ 0.0f };
        // UNLAYERED_MOD: Z extent of the layer's extrusions. Equal to z for a horizontal layer;
        // for a tilted one they bound the slab it occupies, which is what the shell clipping and
        // the layer labels need.
        float z_min{ FLT_MAX };
        float z_max{ -FLT_MAX };
        // Tool axis the layer was printed with; 0 means straight up (a plain horizontal layer).
        uint16_t tool_axis_id{ 0 };
        Range range;
        std::array<float, TIME_MODES_COUNT> times{ 0.0f, 0.0f };
        bool contains_colorprint_options{ false };
    };

    std::vector<Item> m_items;
    // UNLAYERED_MOD: the source layer id of m_items.back(); see update().
    uint32_t m_last_layer_id{ 0 };
    Range m_view_range;
    // UNLAYERED_MOD: cached "no layer is tilted", so callers can take the cheap Z-based paths.
    bool m_all_planar{ true };
};

} // namespace libvgcode

#endif // VGCODE_LAYERS_HPP
