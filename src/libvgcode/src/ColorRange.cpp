///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "../include/ColorRange.hpp"

#include "Utils.hpp"

#include <algorithm>
#include <array>
#include <assert.h>
#include <cfloat>
#include <cmath>
#include "../include/Types.hpp"
#include <cstddef>
#include <vector>

namespace libvgcode {

const ColorRange ColorRange::DUMMY_COLOR_RANGE = ColorRange();

static float get_step_size(const ColorRange& color_range)
{
    const std::array<float, 2>& range = color_range.get_range();
    const Palette& palette = color_range.get_palette();
    switch (color_range.get_type())
    {
    default:
    case EColorRangeType::Linear:
    {
        return (range[1] - range[0]) / (static_cast<float>(palette.size()) - 1.0f);
    }
    case EColorRangeType::Logarithmic:
    {
        return (range[0] != 0.0f) ? std::log(range[1] / range[0]) / (static_cast<float>(palette.size()) - 1.0f) : 0.0f;
    }
    }
}

ColorRange::ColorRange(EColorRangeType type)
: m_type(type)
, m_palette(DEFAULT_RANGES_COLORS)
{
}

EColorRangeType ColorRange::get_type() const
{
    return m_type;
}

const Palette& ColorRange::get_palette() const
{
    return m_palette;
}

void ColorRange::set_palette(const Palette& palette)
{
    if (palette.size() > 1)
        m_palette = palette;
}

// ORCA
static float get_global_t(const ColorRange& color_range, float value)
{
    // Input value scaled to the colors range
    const std::array<float, 2>& range = color_range.get_range();
    float global_t = 0.0f;
    if (range[0] > range[1])
        return global_t;
    value = std::clamp(value, range[0], range[1]);
    const float step = get_step_size(color_range);
    if (step > 0.0f) {
        if (color_range.get_type() == EColorRangeType::Logarithmic) {
            if (range[0] != 0.0f)
                global_t = std::log(value / range[0]) / step;
        }
        else
            global_t = (value - range[0]) / step;
    }
    return global_t;
}

Color ColorRange::get_color_at(float value) const
{
    const float global_t = get_global_t(*this, value);

    const size_t color_max_idx = m_palette.size() - 1;

    // Compute the two colors just below (low) and above (high) the input value
    const size_t color_low_idx = std::clamp<size_t>(static_cast<size_t>(global_t), 0, color_max_idx);
    const size_t color_high_idx = std::clamp<size_t>(color_low_idx + 1, 0, color_max_idx);

    // Interpolate between the low and high colors to find exactly which color the input value should get
    return lerp(m_palette[color_low_idx], m_palette[color_high_idx], global_t - static_cast<float>(color_low_idx));
}

float ColorRange::get_position_at(float value) const
{
    return std::clamp(get_global_t(*this, value) / (static_cast<float>(m_palette.size()) - 1.0f), 0.0f, 1.0f);
}

const std::array<float, 2>& ColorRange::get_range() const
{
    return m_range;
}

std::vector<float> ColorRange::get_values() const
{
    std::vector<float> ret;

    if (m_count == 1) {
        // single item use case
        ret.emplace_back(m_range[0]);
    }
    else if (m_count == 2) {
        // two items use case
        ret.emplace_back(m_range[0]);
        ret.emplace_back(m_range[1]);
    }
    else {
        const float step_size = get_step_size(*this);
        for (size_t i = 0; i < m_palette.size(); ++i) {
            float value = 0.0f;
            switch (m_type)
            {
            default:
            case EColorRangeType::Linear:      { value = m_range[0] + static_cast<float>(i) * step_size; break; }
            case EColorRangeType::Logarithmic: { value = ::exp(::log(m_range[0]) + static_cast<float>(i) * step_size);  break; }
            }
            ret.emplace_back(value);
        }
    }

    return ret;
}

size_t ColorRange::size_in_bytes_cpu() const
{
    size_t ret = STDVEC_MEMSIZE(m_palette, Color);
    return ret;
}

void ColorRange::update(float value)
{
    if (value != m_range[0] && value != m_range[1])
        ++m_count;

    m_range[0] = std::min(m_range[0], value);
    m_range[1] = std::max(m_range[1], value);
}

void ColorRange::reset()
{
    m_range = { FLT_MAX, -FLT_MAX };
    m_count = 0;
}

} // namespace libvgcode

