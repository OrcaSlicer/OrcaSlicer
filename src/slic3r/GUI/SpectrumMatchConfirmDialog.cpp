#include "SpectrumMatchConfirmDialog.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "MainFrame.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/MixedFilamentMatch.hpp"

#include <wx/button.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace {

ColorRGB decode_source_hex(const std::string &hex)
{
    ColorRGB          c;
    const std::string norm = normalize_mix_match_hex(hex);
    if ((!norm.empty() && decode_color(norm, c)) || decode_color(hex, c))
        return c;
    return ColorRGB::BLACK();
}

wxPanel *make_swatch(wxWindow *parent, const ColorRGB &c, int dip)
{
    auto *swatch = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(dip, dip));
    swatch->SetBackgroundColour(wxColour(c.r_uchar(), c.g_uchar(), c.b_uchar()));
    return swatch;
}

wxString dest_label_for(size_t dest, size_t mix_base, const MixedFilamentManager &mgr)
{
    if (dest == 0)
        return _L("Unmapped");
    if (dest <= mix_base)
        return wxString::Format(_L("Physical %d"), int(dest));
    const size_t mix_idx = dest - mix_base - 1;
    const auto  &mixes   = mgr.mixed_filaments();
    if (mix_idx < mixes.size())
        return wxString::FromUTF8(mix_surface_label(int(dest), mixes[mix_idx]).c_str());
    return wxString::Format(_L("Mix %d"), int(dest));
}

ColorRGB predicted_for_dest(size_t dest, size_t mix_base, const std::vector<ColorRGB> &physicals,
                            const MixedFilamentManager &mgr)
{
    if (dest >= 1 && dest <= mix_base && dest <= physicals.size())
        return physicals[dest - 1];
    const size_t mix_idx = dest > mix_base ? dest - mix_base - 1 : size_t(-1);
    const auto  &mixes   = mgr.mixed_filaments();
    if (mix_idx < mixes.size())
        return predicted_swatch_for_mix(mixes[mix_idx], physicals);
    const std::vector<ColorRGB> preview = preview_filament_colors(physicals, mgr.serialize_definitions());
    if (dest >= 1 && dest <= preview.size())
        return preview[dest - 1];
    return ColorRGB::BLACK();
}

} // namespace

SpectrumMatchConfirmDialog::SpectrumMatchConfirmDialog(wxWindow                      *parent,
                                                       const SpectrumPaintBakePlan   &plan,
                                                       const std::vector<std::string> &source_hexes,
                                                       const std::vector<ColorRGB>   &physicals,
                                                       size_t                         mix_base)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Match painted colors"),
                wxDefaultPosition,
                wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    MixedFilamentManager mgr;
    mgr.load_definitions(plan.mixed_filament_definitions);

    size_t mix_class_sources = 0;
    for (size_t src = 1; src <= source_hexes.size() && src < plan.slot_map.size(); ++src) {
        if (size_t(plan.slot_map[src]) > mix_base)
            ++mix_class_sources;
    }

    auto *root = new wxBoxSizer(wxVERTICAL);

    auto *hint = new wxStaticText(this, wxID_ANY,
        _L("Paint IDs will become C/M/Y/K physicals and Mix 5+. This is not an AMS 8-color replica. Save a copy first."));
    hint->Wrap(FromDIP(520));
    root->Add(hint, 0, wxEXPAND | wxALL, FromDIP(12));

    if (plan.mix_count < mix_class_sources) {
        auto *banner = new wxStaticText(this, wxID_ANY,
            wxString::Format(
                _L("Some Mix destinations were merged so unique Mix dests fit persist %d."),
                int(SPECTRUM_PAINT_ID_PERSIST_CAP)));
        banner->Wrap(FromDIP(520));
        banner->SetForegroundColour(wxColour(0x8A, 0x5A, 0x00));
        root->Add(banner, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    }

    auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    scroll->SetBackgroundColour(*wxWHITE);
    scroll->SetScrollRate(0, FromDIP(16));
    auto *grid = new wxFlexGridSizer(0, 5, FromDIP(6), FromDIP(10));
    grid->AddGrowableCol(2, 1);

    auto add_header = [&](const wxString &text) {
        auto *h = new wxStaticText(scroll, wxID_ANY, text);
        h->SetFont(Label::Head_14);
        grid->Add(h, 0, wxALIGN_CENTER_VERTICAL);
    };
    add_header(_L("ID"));
    add_header(_L("Original"));
    add_header(_L("Destination"));
    add_header(_L("Predicted"));
    add_header(_L("ΔE00"));

    const int swatch_dip = FromDIP(18);
    for (size_t src = 1; src <= source_hexes.size(); ++src) {
        const ColorRGB original = decode_source_hex(source_hexes[src - 1]);
        const size_t   dest     = (src < plan.slot_map.size()) ? size_t(plan.slot_map[src]) : 0;
        const ColorRGB predicted = predicted_for_dest(dest, mix_base, physicals, mgr);
        const float    de        = mixer_delta_e00(original, predicted);

        auto *id = new wxStaticText(scroll, wxID_ANY, wxString::Format("%d", int(src)));
        grid->Add(id, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(make_swatch(scroll, original, swatch_dip), 0, wxALIGN_CENTER_VERTICAL);
        auto *dest_txt = new wxStaticText(scroll, wxID_ANY, dest_label_for(dest, mix_base, mgr));
        grid->Add(dest_txt, 0, wxALIGN_CENTER_VERTICAL | wxEXPAND);
        grid->Add(make_swatch(scroll, predicted, swatch_dip), 0, wxALIGN_CENTER_VERTICAL);
        auto *de_txt = new wxStaticText(scroll, wxID_ANY, wxString::Format("%.1f", double(de)));
        grid->Add(de_txt, 0, wxALIGN_CENTER_VERTICAL);
    }

    scroll->SetSizer(grid);
    scroll->SetMinSize(wxSize(FromDIP(520), FromDIP(280)));
    scroll->SetMaxSize(wxSize(-1, FromDIP(420)));
    root->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

    auto *btns = new wxBoxSizer(wxHORIZONTAL);
    btns->AddStretchSpacer(1);
    auto *cancel  = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    auto *confirm = new wxButton(this, wxID_OK, _L("Confirm"));
    btns->Add(cancel, 0, wxRIGHT, FromDIP(8));
    btns->Add(confirm, 0);
    root->Add(btns, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizerAndFit(root);
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SpectrumMatchConfirmDialog::on_dpi_changed(const wxRect &)
{
    Layout();
    Fit();
}

} // namespace GUI
} // namespace Slic3r
