///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "ViewRange.hpp"

namespace libvgcode {

void ViewRange::set_full(Interval::value_type min, Interval::value_type max)
{
    m_full.set(min, max);
    // force the enabled range to stay inside the modified full range
    m_full.clamp(m_enabled);
    // force the visible range to stay inside the modified enabled range
    m_enabled.clamp(m_visible);
}

void ViewRange::set_enabled(Interval::value_type min, Interval::value_type max)
{
    m_enabled.set(min, max);
    // force the visible range to stay inside the modified enabled range
    m_enabled.clamp(m_visible);
}

void ViewRange::set_visible(Interval::value_type min, Interval::value_type max)
{
    m_visible.set(min, max);
    // force the visible range to stay inside the enabled range
    m_enabled.clamp(m_visible);
}

void ViewRange::reset()
{
    m_full.reset();
    m_enabled.reset();
    m_visible.reset();
}

} // namespace libvgcode
