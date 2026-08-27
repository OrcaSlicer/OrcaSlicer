#ifndef slic3r_GUI_FilamentBitmapUtils_hpp_
#define slic3r_GUI_FilamentBitmapUtils_hpp_

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/gdicmn.h>
#include <vector>

// Orca: forward-declare so the header is self-contained outside libslic3r_gui's
// force-included pch (the GUI test suite includes it directly).
namespace Slic3r { class DynamicPrintConfig; struct GradientCurve; }

namespace Slic3r { namespace GUI {

// Fills a rect with a west->east linear gradient by drawing solid 1px columns.
// Use instead of wxDC::GradientFillLinear, whose CoreGraphics (CGShading) backend
// fails to render on some macOS builds; solid fills are unaffected.
void fill_gradient_rect_east(wxDC& dc, const wxRect& rect, const wxColour& from, const wxColour& to);

enum class FilamentRenderMode {
    Single,
    Dual,
    Triple,
    Quadruple,
    Gradient
};

// Create a colour swatch bitmap. The render mode is chosen automatically from the
// number of colours unless force_gradient is true.
wxBitmap create_filament_bitmap(const std::vector<wxColour>& colors,
                              const wxSize& size,
                              bool force_gradient = false);

// Blend colours at the given relative weights through blend_color_multi, so a measured
// real-world mix is used where one exists instead of a plain channel lerp.
wxColour blend_n_colors(const std::vector<wxColour>& cols, const std::vector<double>& weights);

// Sample a gradient mixed filament the way the slicer builds it: t runs 0..1 over the
// model's height, the curve gives the first component's ratio at t, and the two
// components are blended at that ratio through blend_n_colors. Entry 0 is the bottom
// of the model, the last entry its top.
std::vector<wxColour> sample_gradient_ramp(const wxColour& first,
                                           const wxColour& second,
                                           const Slic3r::GradientCurve& curve,
                                           int steps);

// Same ramp for a project config slot, resolving components, colours and curve (or the
// linear gradient_range fallback) from cfg. Returns empty for any slot that is not a
// two-component gradient mixed filament. steps is the ramp's resolution; pass the
// destination's height in pixels.
std::vector<wxColour> mixed_gradient_ramp(const Slic3r::DynamicPrintConfig& cfg, size_t slot, int steps);

// Fill rect with a ramp, ramp.front() along the bottom edge.
void fill_gradient_ramp_rect(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& ramp);

// Swatch bitmap for a gradient mixed filament, drawn bottom to top from the ramp.
wxBitmap create_gradient_ramp_bitmap(const std::vector<wxColour>& ramp, const wxSize& size);

// Recompute blended representative colors for mixed (virtual) filament slots.
// Reads mixed-filament config keys from cfg and writes back into colors[i]
// for every slot where filament_is_mixed[i] is true.
void recompute_mixed_slot_colors(std::vector<wxColour>& colors,
                                 const Slic3r::DynamicPrintConfig& cfg);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentBitmapUtils_hpp_