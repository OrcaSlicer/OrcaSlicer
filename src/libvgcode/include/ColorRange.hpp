///|/ Copyright (c) Prusa Research 2023 Enrico Turri @enricoturri1966, Pavel Mikuš @Godrak
///|/
///|/ libvgcode is released under the terms of the AGPLv3 or higher
///|/
#ifndef VGCODE_COLORRANGE_HPP
#define VGCODE_COLORRANGE_HPP

#include "../include/Types.hpp"

#include <cfloat>

namespace libvgcode {

static const Palette DEFAULT_RANGES_COLORS{ {
    {  11,  44, 122 }, // bluish
    {  19,  89, 133 },
    {  28, 136, 145 },
    {   4, 214,  15 },
    { 170, 242,   0 },
    { 252, 249,   3 },
    { 245, 206,  10 },
    { 227, 136,  32 },
    { 209, 104,  48 },
    { 194,  82,  60 },
    { 148,  38,  22 }  // reddish
} };

class ColorRange
{
public:
    //
    // Constructor
    //
    explicit ColorRange(EColorRangeType type = EColorRangeType::Linear);
    //
    // Return the type of this ColorRange.
    //
    EColorRangeType get_type() const;
    //
    // Return the palette used by this ColorRange.
    // Default is DEFAULT_RANGES_COLORS
    //
    const Palette& get_palette() const;
    //
    // Set the palette to be used by this ColorRange.
    // The given palette must contain at least two colors.
    //
    void set_palette(const Palette& palette);
    //
    // Return the interpolated color at the given value.
    // Value is clamped to [get_range()[0]..get_range()[1]].
    //
    Color get_color_at(float value) const;
    //
    // Return the range of this ColorRange.
    // The range is detected during the call to Viewer::load().
    // [0] -> min
    // [1] -> max
    //
    const std::array<float, 2>& get_range() const;
    //
    // Return the values corresponding to the detected color bins of this ColorRange.
    // The size of the returned vector can be:
    // 1                    - If only one value was detected while setting up this ColorRange.
    // 2                    - If only two values were detected while setting up this ColorRange.
    // get_palette().size() - If more than two distinct values were detected while setting up this ColorRange.
    //
    std::vector<float> get_values() const;
    //
    // Return the size of the palette, in bytes
    //
    std::size_t size_in_bytes_cpu() const;

    static const ColorRange DUMMY_COLOR_RANGE;

private:
    EColorRangeType m_type{ EColorRangeType::Linear };
    //
    // The palette used by this ColorRange
    // 
    Palette m_palette;
    //
    // [0] = min
    // [1] = max
    //
    std::array<float, 2> m_range{ FLT_MAX, -FLT_MAX };
    //
    // Count of different values passed to update()
    // 
    std::size_t m_count{ 0 };

    //
    // Use the passed value to update the range.
    //
    void update(float value);
    //
    // Reset the range
    // Call this method before reuse an instance of ColorRange.
    //
    void reset();

    friend class ViewerImpl;
};

} // namespace libvgcode

#endif // VGCODE_COLORRANGE_HPP
