///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_VIEWRANGE_HPP
#define VGCODE_VIEWRANGE_HPP

#include "Range.hpp"

namespace libvgcode {

class ViewRange
{
public:
    const Interval& get_full() const { return m_full.get(); }
    void set_full(const Range& other) { set_full(other.get()); }
    void set_full(const Interval& range) { set_full(range[0], range[1]); }
    void set_full(Interval::value_type min, Interval::value_type max);

    const Interval& get_enabled() const { return m_enabled.get(); }
    void set_enabled(const Range& other) { set_enabled(other.get()); }
    void set_enabled(const Interval& range) { set_enabled(range[0], range[1]); }
    void set_enabled(Interval::value_type min, Interval::value_type max);

    const Interval& get_visible() const { return m_visible.get(); }
    void set_visible(const Range& other) { set_visible(other.get()); }
    void set_visible(const Interval& range) { set_visible(range[0], range[1]); }
    void set_visible(Interval::value_type min, Interval::value_type max);

    void reset();

private:
    //
    // Full range
    // The range of moves that could potentially be visible.
    // It is usually equal to the enabled range, unless Settings::top_layer_only_view_range is set to true.
    //
    Range m_full;

    //
    // Enabled range
    // The range of moves that are enabled for visualization.
    // It is usually equal to the full range, unless Settings::top_layer_only_view_range is set to true.
    //
    Range m_enabled;

    //
    // Visible range
    // The range of moves that are currently rendered.
    //
    Range m_visible;
};

} // namespace libvgcode

#endif // VGCODE_VIEWRANGE_HPP