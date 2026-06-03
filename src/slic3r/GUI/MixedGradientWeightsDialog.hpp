#pragma once
#include <wx/dialog.h>
#include <wx/colour.h>
#include <wx/stattext.h>
#include <vector>

namespace Slic3r { namespace GUI {

// Forward-declare Task-17 panel: defined in MixedFilamentColorMapPanel.hpp.
class MixedFilamentColorMapPanel;

// ---------------------------------------------------------------------------
// MixedGradientWeightsDialog
//
// A modal dialog that shows a MixedFilamentColorMapPanel and per-filament
// weight labels so the user can pick multi-filament blend weights for a
// gradient mix.
//
// Extracted from FullSpectrum Plater.cpp:4506-4583.
//
// NOTE (Task 17 dependency): the constructor body that instantiates
// MixedFilamentColorMapPanel is guarded with #if 0 until Task 17 lands.
// See MixedGradientWeightsDialog.cpp for details.
// ---------------------------------------------------------------------------
class MixedGradientWeightsDialog : public wxDialog
{
public:
    MixedGradientWeightsDialog(wxWindow                        *parent,
                               const std::vector<unsigned int> &filament_ids,
                               const std::vector<wxColour>     &palette,
                               const std::vector<int>          &initial_weights);

    // Returns the normalised per-filament weight vector chosen by the user.
    std::vector<int> normalized_weights() const;

private:
    void update_weight_labels();

    MixedFilamentColorMapPanel  *m_color_map     { nullptr };
    std::vector<wxColour>        m_colors;
    std::vector<int>             m_weights;
    std::vector<wxStaticText *>  m_weight_labels;
};

} } // namespace Slic3r::GUI
