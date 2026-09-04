#pragma once

#include "GUI_Utils.hpp"
#include "libslic3r/Color.hpp"
#include "libslic3r/MixedFilamentPaintBake.hpp"

#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// ZR ΔE Match confirm around plan_spectrum_paint_bake. Confirm / Cancel only.
// Not a Snapmaker Auto/Manual Match window; no second 3D canvas.
class SpectrumMatchConfirmDialog : public DPIDialog
{
public:
    SpectrumMatchConfirmDialog(wxWindow                      *parent,
                               const SpectrumPaintBakePlan   &plan,
                               const std::vector<std::string> &source_hexes,
                               const std::vector<ColorRGB>   &physicals,
                               size_t                         mix_base);
    ~SpectrumMatchConfirmDialog() override = default;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;
};

} // namespace GUI
} // namespace Slic3r
