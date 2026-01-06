///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#include "../include/Types.hpp"

#include <algorithm>

namespace libvgcode {

// mapping from EMoveType to EOptionType
EOptionType move_type_to_option(EMoveType type)
{
    switch (type)
    {
    case EMoveType::Travel:      { return EOptionType::Travels; }
    case EMoveType::Wipe:        { return EOptionType::Wipes; }
    case EMoveType::Retract:     { return EOptionType::Retractions; }
    case EMoveType::Unretract:   { return EOptionType::Unretractions; }
    case EMoveType::Seam:        { return EOptionType::Seams; }
    case EMoveType::ToolChange:  { return EOptionType::ToolChanges; }
    case EMoveType::ColorChange: { return EOptionType::ColorChanges; }
    case EMoveType::PausePrint:  { return EOptionType::PausePrints; }
    case EMoveType::CustomGCode: { return EOptionType::CustomGCodes; }
    default:                     { return EOptionType::COUNT; }
    }
}

static uint8_t lerp(uint8_t f1, uint8_t f2, float t)
{
    const float one_minus_t = 1.0f - t;
    return static_cast<uint8_t>(one_minus_t * static_cast<float>(f1) + t * static_cast<float>(f2));
}

// It will be possible to replace this with std::lerp when using c++20
Color lerp(const Color& c1, const Color& c2, float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return { lerp(c1[0], c2[0], t), lerp(c1[1], c2[1], t), lerp(c1[2], c2[2], t) };
}

} // namespace libvgcode
