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
    // the ids of the layer's first and last vertex, which bound its vertices in any print order
    Interval get_layer_vertex_range(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].range.get() : Interval{ 0, 0 };
    }
    // whether the layer has an infill extrusion of any kind, sparse or solid
    bool layer_has_infill(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].has_infill : false;
    }
    bool layer_has_sparse_infill(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].has_sparse_infill : false;
    }
    // Whether each layer's infill closes every object printed on it, as the caller knows it from
    // the objects the moves belong to: bit 0 as seen from above, bit 1 as seen from below, see
    // GCodeInputData::layers_closed. Ignored unless there is one entry per layer.
    void set_closed_layers(const std::vector<uint8_t>& closed);
    bool layer_closed_from_above(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].closed_from_above : false;
    }
    bool layer_closed_from_below(std::size_t layer_id) const {
        return (layer_id < m_items.size()) ? m_items[layer_id].closed_from_below : false;
    }
    // The layers at the ends of the view range that are drawn whole while the user drags: every
    // layer of the range up to [0], and from [1] on. A cut right under a layer with no infill of its
    // own, as infill combination leaves every layer but one in a few, or a layer of support alone,
    // would cap the print where at rest its infill shows through, so each end runs to the nearest
    // layer that closes the print, at most max_depth layers deep. That is the nearest layer closed
    // as seen from the range's end when the caller has told which layers close every object on
    // them, an object's top shells closing it from above and its bottom shells from below. Otherwise it is
    // judged from the roles of the whole layer: the nearest layer with sparse infill, since
    // another object's solid infill on the layer says nothing about this one's sparse infill, and
    // only when no sparse infill is within reach, as on a print with none, the nearest layer with
    // any infill. The two ends never cross, and a range from the print's first layer has no
    // bottom band, since nothing is cut under that layer.
    Interval get_whole_end_layers(std::size_t max_depth) const;

    std::size_t size_in_bytes_cpu() const;

private:
    struct Item
    {
        float z{ 0.0f };
        Range range;
        std::array<float, TIME_MODES_COUNT> times{ 0.0f, 0.0f };
        bool contains_colorprint_options{ false };
        bool has_infill{ false };
        bool has_sparse_infill{ false };
        bool closed_from_above{ false };
        bool closed_from_below{ false };
    };
    
    std::vector<Item> m_items;
    Range m_view_range;
    // whether Item::closed was set by the caller, see set_closed_layers()
    bool m_closed_known{ false };
};

} // namespace libvgcode

#endif // VGCODE_LAYERS_HPP
