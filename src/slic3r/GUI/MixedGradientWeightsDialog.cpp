#include "MixedGradientWeightsDialog.hpp"
#include "MixedFilamentColorMapPanel.hpp"
#include "I18N.hpp"             // _L()
#include "Widgets/Label.hpp"    // Label::Body_12

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/panel.h>
#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

// ---------------------------------------------------------------------------
// Anonymous-namespace helper: copied verbatim from FullSpectrum Plater.cpp:2558
// ---------------------------------------------------------------------------
namespace {

std::vector<int> normalize_color_match_weights(const std::vector<int> &weights, size_t count)
{
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, count > 0 ? int(100 / int(count)) : 0);

    int sum = 0;
    for (int &value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned          += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }

    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Constructor — verbatim from FullSpectrum Plater.cpp:4506-4583
// ---------------------------------------------------------------------------
MixedGradientWeightsDialog::MixedGradientWeightsDialog(
    wxWindow                        *parent,
    const std::vector<unsigned int> &filament_ids,
    const std::vector<wxColour>     &palette,
    const std::vector<int>          &initial_weights)
    : wxDialog(parent, wxID_ANY, _L("Gradient Mix Weights"),
               wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    m_colors.reserve(filament_ids.size());
    m_weights = normalize_color_match_weights(initial_weights, filament_ids.size());
    for (const unsigned int filament_id : filament_ids) {
        if (filament_id >= 1 && filament_id <= palette.size())
            m_colors.emplace_back(palette[filament_id - 1]);
        else
            m_colors.emplace_back(wxColour("#26A69A"));
    }
    if (m_colors.empty())
        m_colors.emplace_back(wxColour("#26A69A"));

    auto *root = new wxBoxSizer(wxVERTICAL);
    auto *hint = new wxStaticText(this, wxID_ANY,
        _L("Pick a point in the gradient map to control multi-filament mix."));
    root->Add(hint, 0, wxEXPAND | wxALL, FromDIP(10));

    m_color_map = new MixedFilamentColorMapPanel(this, filament_ids, palette, initial_weights,
                                                 wxSize(FromDIP(240), FromDIP(240)));
    root->Add(m_color_map, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    for (size_t i = 0; i < filament_ids.size(); ++i) {
        auto  *row  = new wxBoxSizer(wxHORIZONTAL);
        wxPanel *chip = new wxPanel(this, wxID_ANY, wxDefaultPosition,
                                    wxSize(FromDIP(18), FromDIP(18)), wxBORDER_SIMPLE);
        chip->SetBackgroundColour(m_colors[i]);
        row->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        row->Add(new wxStaticText(this, wxID_ANY,
                                  wxString::Format("F%d", int(filament_ids[i]))),
                 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        auto *label = new wxStaticText(this, wxID_ANY,
                                       wxString::Format("%d%%", m_weights[i]));
        label->SetFont(Label::Body_12);
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        m_weight_labels.emplace_back(label);
    }

    root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(8));
    SetSizerAndFit(root);
    SetMinSize(wxSize(FromDIP(380),
                      std::max(GetSize().GetHeight(), FromDIP(460))));
    update_weight_labels();

    if (m_color_map) {
        m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
            m_weights = m_color_map ? m_color_map->normalized_weights() : m_weights;
            update_weight_labels();
        });
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<int> MixedGradientWeightsDialog::normalized_weights() const
{
    return m_color_map ? m_color_map->normalized_weights() : m_weights;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void MixedGradientWeightsDialog::update_weight_labels()
{
    for (size_t i = 0; i < m_weight_labels.size() && i < m_weights.size(); ++i) {
        if (m_weight_labels[i])
            m_weight_labels[i]->SetLabel(wxString::Format("%d%%", m_weights[i]));
    }
    Layout();
}

} } // namespace Slic3r::GUI
