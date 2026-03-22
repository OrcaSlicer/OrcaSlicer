///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_LAYERS_HPP
#define VGCODE_LAYERS_HPP

#include "Range.hpp"

namespace libvgcode {

struct PathVertex;

class Layers
{
public:
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
        Range range;
        std::array<float, TIME_MODES_COUNT> times{ 0.0f, 0.0f };
        bool contains_colorprint_options{ false };
    };
    
    std::vector<Item> m_items;
    Range m_view_range;
};

} // namespace libvgcode

#endif // VGCODE_LAYERS_HPP
