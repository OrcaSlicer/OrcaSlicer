///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_COLORPRINT_HPP
#define VGCODE_COLORPRINT_HPP

#include "../include/Types.hpp"

namespace libvgcode {

struct ColorPrint
{
    uint8_t extruder_id{ 0 };
    uint8_t color_id{ 0 };
    uint32_t layer_id{ 0 };
    std::array<float, TIME_MODES_COUNT> times{ 0.0f, 0.0f };
};

} // namespace libvgcode

#endif // VGCODE_COLORPRINT_HPP