#pragma once
#include <wx/panel.h>
#include <wx/colour.h>
#include <vector>
#include <string>

namespace Slic3r { namespace GUI {

// Preview strip that shows the layer-by-layer or same-layer colour sequence
// produced by a mixed filament definition.  All logic is self-contained; the
// owning panel calls set_data() whenever the underlying MixedFilament changes.
class MixedMixPreview : public wxPanel
{
public:
    explicit MixedMixPreview(wxWindow *parent);

    void set_data(const std::vector<wxColour>       &palette,
                  const std::vector<unsigned int>   &sequence,
                  bool                               same_layer_mode,
                  const std::vector<double>         &surface_offsets_mm,
                  const wxColour                    &fallback,
                  const wxString                    &left_overlay,
                  const wxString                    &right_overlay);

private:
    wxRect   preview_rect() const;
    wxColour color_for_extruder(unsigned int extruder_id) const;
    double   max_active_surface_offset_mm() const;
    int      slot_inset_for_extruder(unsigned int extruder_id, int slot_extent) const;
    void     on_paint(wxPaintEvent &evt);

    std::vector<wxColour>       m_palette;
    std::vector<unsigned int>   m_sequence;
    std::vector<double>         m_surface_offsets_mm;
    bool                        m_same_layer    { false };
    wxColour                    m_fallback      { wxColour(38, 166, 154) };
    wxString                    m_left_overlay;
    wxString                    m_right_overlay;
};

} } // namespace Slic3r::GUI
