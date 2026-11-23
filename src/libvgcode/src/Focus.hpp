///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_FOCUS_HPP
#define VGCODE_FOCUS_HPP

#include "../include/PathVertex.hpp"
#include "../include/Types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace libvgcode {

//
// ORCA: the key of a vertex the shaders test against the Focus, see in_focus() in Shaders.hpp:
// bits 0-5   its kind, see focus_kind()
// bits 6-13  its extruder
// bits 14-31 the position of its value on the color range of the view, 0 where its color comes from none
//
static constexpr uint32_t FOCUS_KIND_MASK = 63;
static constexpr int FOCUS_KIND_NONE = 63;
static constexpr uint32_t FOCUS_EXTRUDER_SHIFT = 6;
static constexpr uint32_t FOCUS_EXTRUDER_MASK = 255;
static constexpr uint32_t FOCUS_VALUE_SHIFT = 14;
static constexpr uint32_t FOCUS_VALUE_STEPS = (1u << 18) - 2;

static_assert(GCODE_EXTRUSION_ROLES_COUNT + OPTION_TYPES_COUNT <= static_cast<std::size_t>(FOCUS_KIND_NONE));

// ORCA: the focus test of the vertex shaders, shared by the GLSL and GLSL ES ones: the key of a vertex against the Focus
// uniforms. FOCUS_* come from the constants above as defines, see with_focus_defines() in ViewerImpl.cpp.
#define VGCODE_FOCUS_GLSL \
"uniform int focus_kind;\n" \
"uniform int focus_extruder;\n" \
"uniform ivec2 focus_range;\n" \
"flat out int focus_cut;\n" \
"bool no_focus() {\n" \
"  return focus_kind < 0 && focus_extruder < 0 && focus_range.x < 0;\n" \
"}\n" \
"bool key_in_focus(uint key) {\n" \
"  int kind = int(key & FOCUS_KIND_MASK);\n" \
"  if (focus_kind >= 0 && kind != focus_kind)\n" \
"    return false;\n" \
"  if (focus_extruder >= 0 && (kind >= FOCUS_EXTRUSION_KINDS || int((key >> FOCUS_EXTRUDER_SHIFT) & FOCUS_EXTRUDER_MASK) != focus_extruder))\n" \
"    return false;\n" \
"  int value = int(key >> FOCUS_VALUE_SHIFT);\n" \
"  return focus_range.x < 0 || (focus_range.x <= value && value <= focus_range.y);\n" \
"}\n"

inline int focus_kind(const PathVertex& v)
{
    if (v.is_extrusion())
        return focus_kind(v.role);
    const EOptionType option = move_type_to_option(v.type);
    return option != EOptionType::COUNT ? focus_kind(option) : FOCUS_KIND_NONE;
}

inline uint32_t focus_value_position(float position)
{
    return 1 + static_cast<uint32_t>(std::lround(std::clamp(position, 0.0f, 1.0f) * static_cast<float>(FOCUS_VALUE_STEPS)));
}

inline uint32_t encode_focus_key(int kind, uint8_t extruder, std::optional<float> position)
{
    return static_cast<uint32_t>(kind) | (static_cast<uint32_t>(extruder) << FOCUS_EXTRUDER_SHIFT) |
           ((position.has_value() ? focus_value_position(*position) : 0u) << FOCUS_VALUE_SHIFT);
}

inline int focus_key_kind(uint32_t key) { return static_cast<int>(key & FOCUS_KIND_MASK); }
inline int focus_key_extruder(uint32_t key) { return static_cast<int>((key >> FOCUS_EXTRUDER_SHIFT) & FOCUS_EXTRUDER_MASK); }
inline int focus_key_value(uint32_t key) { return static_cast<int>(key >> FOCUS_VALUE_SHIFT); }

// the value positions kept, or { -1, -1 } when the focus has no range
inline std::array<int, 2> focus_value_range(const Focus& focus)
{
    if (focus.range[0] < 0.0f)
        return { -1, -1 };
    return { static_cast<int>(focus_value_position(focus.range[0])), static_cast<int>(focus_value_position(focus.range[1])) };
}

inline bool is_key_in_focus(uint32_t key, const Focus& focus)
{
    if (!focus.any())
        return true;
    const int kind = focus_key_kind(key);
    if (focus.kind >= 0 && kind != focus.kind)
        return false;
    if (focus.extruder >= 0 && (kind >= static_cast<int>(GCODE_EXTRUSION_ROLES_COUNT) || focus_key_extruder(key) != focus.extruder))
        return false;
    const std::array<int, 2> range = focus_value_range(focus);
    const int value = focus_key_value(key);
    return range[0] < 0 || (range[0] <= value && value <= range[1]);
}

} // namespace libvgcode

#endif // VGCODE_FOCUS_HPP
