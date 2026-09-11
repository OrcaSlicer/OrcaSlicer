#ifndef slic3r_GUI_FilamentBitmapUtils_hpp_
#define slic3r_GUI_FilamentBitmapUtils_hpp_

#include <wx/bitmap.h>
#include <wx/colour.h>
#include <wx/dc.h>
#include <wx/gdicmn.h>
#include <wx/geometry.h>
#include <wx/graphics.h>
#include <array>
#include <vector>

// Orca: forward-declare so the header is self-contained outside libslic3r_gui's
// force-included pch (the GUI test suite includes it directly).
namespace Slic3r { class DynamicPrintConfig; struct GradientCurve; }

namespace Slic3r { namespace GUI {

// Barycentric utilities for a ternary (triangle) ratio picker, shared by the mixed-filament
// editor and the Publish dialog's read-only definition preview.
struct TriPoint { double x, y; };

double tri_signed_area2(TriPoint a, TriPoint b, TriPoint c);
bool tri_contains(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2);
void tri_barycentric(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2,
                     double& w0, double& w1, double& w2);
TriPoint tri_clamp(TriPoint p, TriPoint v0, TriPoint v1, TriPoint v2);

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

// Resolve the curve a gradient slot is sampled with: the custom curve wins when it has at
// least two points, otherwise a straight line between gradient_range's endpoints, otherwise
// the 0.10 -> 0.90 default. Mirrors the slicer's ToolOrdering fallback so every preview
// agrees with what gets sliced. Always returns a two-point curve.
Slic3r::GradientCurve mixed_gradient_curve(const Slic3r::DynamicPrintConfig& cfg, size_t slot);

// Fill rect with a ramp, ramp.front() along the bottom edge.
void fill_gradient_ramp_rect(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& ramp);

// Swatch bitmap for a gradient mixed filament, drawn bottom to top from the ramp.
wxBitmap create_gradient_ramp_bitmap(const std::vector<wxColour>& ramp, const wxSize& size);

// Recompute blended representative colors for mixed (virtual) filament slots.
// Reads mixed-filament config keys from cfg and writes back into colors[i]
// for every slot where filament_is_mixed[i] is true.
void recompute_mixed_slot_colors(std::vector<wxColour>& colors,
                                 const Slic3r::DynamicPrintConfig& cfg);

// --- Gradient plot (shared by GradientCurveEditor and the Publish dialog's read-only
// preview). The plot is a square 1:1 rect laid out with the editor's ratios so both
// render identically; curves are drawn as sub-pixel anti-aliased polylines through
// wxGCDC so they never quantize to whole pixels.

// One curve of the plot: screen-space sub-pixel points already mapped into the plot
// rect, the stroke colour and the stroke width in DIP.
struct MixedGradientCurve
{
    std::vector<wxPoint2DDouble> points;
    wxColour                     colour;
    int                          stroke_dip;
};

// Theme tokens, resolved by the caller through StateColor::darkModeColorFor.
struct MixedGradientTheme
{
    wxColour background;   // for near-background outline detection
    wxColour grid;         // grid line
    wxColour axis;         // axis + arrow fill
    wxColour label;        // "Material Ratio" / "Model Height"
    wxColour label_strong; // "100%"
    wxColour outline;      // near-background curve lift
    wxColour point_fill;   // anchor fill
};

// Square 1:1 plot rect inside `canvas`, using the editor's plot ratios.
wxRect mixed_gradient_plot_rect(const wxSize& canvas);

// Draw the whole plot (grid, axes + arrowheads, axis labels, each curve with an optional
// near-background outline, and anchor circles). `anchors` are empty when the caller has
// none to show. `dc` is the caller's buffered paint DC; a wxGCDC is created inside so the
// geometry gets anti-aliased.
void draw_mixed_gradient_plot(wxDC& dc, const wxSize& canvas,
                              const std::vector<MixedGradientCurve>& curves,
                              const std::vector<wxPoint2DDouble>& anchors,
                              const MixedGradientTheme& theme);

// --- Ratio bar (2-component continuous blend + divider, matching MixedFilamentDialog).
// Colours blend first->second across the rect; the divider marks `second_fraction` of the
// rect's width (the second component's share, 0..1).
void draw_mixed_ratio_blend_bar(wxDC& dc, const wxRect& rect, const wxColour& first,
                                const wxColour& second, double second_fraction);

// Fallback ratio bar for N>2 non-gradient slots: one solid segment per component,
// widths proportional to shares. Label text (the "NN%" inside wide-enough segments) is
// the caller's concern.
void draw_mixed_ratio_segments(wxDC& dc, const wxRect& rect, const std::vector<wxColour>& colours,
                               const std::vector<double>& shares);

// --- Triangle picker (3-component), shared by MixedFilamentDialog and the Publish preview.
struct MixedTriangleTheme
{
    wxColour background;
    wxColour outline;     // triangle border
    wxColour ring;        // drag-handle ring
    wxColour label;       // "Ratio" title + per-vertex labels
};

// The three vertices of the read-only/miniature triangle inside a `size` square panel,
// with `margin_dip` inset. Order: top, bottom-left, bottom-right.
std::array<TriPoint, 3> mixed_triangle_vertices(const wxSize& size, double margin_dip = 20.0);

// Draw background, the cached barycentric fill, the outline and the drag-handle marker.
// `weights` are the three barycentric shares (sum 1). The "Ratio" title and per-vertex
// percentage labels are drawn by the caller so the interactive editor can keep its own
// live child labels while the read-only preview draws them as text.
void draw_mixed_triangle_picker(wxDC& dc, const wxSize& size, const std::array<wxColour, 3>& colours,
                                const std::array<double, 3>& weights, const MixedTriangleTheme& theme);

// Draw the "Ratio" title plus one "NN%" label per vertex (used by the read-only preview;
// the interactive editor positions its own live labels instead).
void draw_mixed_triangle_labels(wxDC& dc, const wxSize& size, const std::array<double, 3>& weights,
                                const MixedTriangleTheme& theme);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FilamentBitmapUtils_hpp_