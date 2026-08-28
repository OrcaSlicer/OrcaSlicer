#include "PublishSettingsDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "Tab.hpp"
#include "ConfigValueFormatter.hpp"
#include "FilamentBitmapUtils.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/FilamentMixer.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <wx/display.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/dcgraph.h>
#include <wx/image.h>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <tuple>

namespace Slic3r { namespace GUI {
namespace {

// Menu ids for show_menu(): dedicated range so the popup cannot collide with application-level
// bindings (e.g. MainFrame's recent-files wxID_FILE1.. range).
enum {
    kPublishSelectAll = wxID_HIGHEST + 1,
    kPublishDeselectAll,
    kPublishSelectVisible,
    kPublishDeselectVisible,
    kPublishFilterSelected,
    kPublishFilterNonSelected
};

// Tree-style flags shared by the outer tabs and every group's inner tabs.
constexpr long s_tab_style = wxTR_NO_BUTTONS | wxTR_HIDE_ROOT | wxTR_SINGLE | wxTR_NO_LINES | wxBORDER_NONE | wxWANTS_CHARS |
                             wxTR_FULL_ROW_HIGHLIGHT;

// The author's hex colour for a filament slot (empty when the slot is out of range); shared by
// the header chip, the inner-tab chip and the DPI rescale paths.
std::string filament_color_hex(const DynamicPrintConfig& full, size_t slot)
{
    if (const auto* colours = full.opt<ConfigOptionStrings>("filament_colour"))
        if (slot < colours->size())
            return colours->get_at(slot);
    return std::string();
}

PublishMaterialIdentity material_identity(size_t slot, const DynamicPrintConfig& full)
{
    PublishMaterialIdentity identity;
    if (const auto* types = full.opt<ConfigOptionStrings>("filament_type"))
        if (slot < types->size())
            identity.type = types->get_at(slot);
    if (const auto* vendors = full.opt<ConfigOptionStrings>("filament_vendor"))
        if (slot < vendors->size())
            identity.vendor = vendors->get_at(slot);
    if (const auto* ids = full.opt<ConfigOptionStrings>("filament_ids"))
        if (slot < ids->size())
            identity.id = ids->get_at(slot);
    return identity;
}

// "Generic PLA @System" -> "Generic PLA"; mirrors the alias derivation in PresetBundle.cpp.
std::string material_display_name(const std::string& preset_name)
{
    const size_t at = preset_name.find_first_of('@');
    if (at == std::string::npos)
        return preset_name;
    std::string bare = preset_name.substr(0, at);
    boost::trim_right(bare);
    return bare.empty() ? preset_name : bare;
}

// Section title for a filament slot: the resolved preset name, then the filament type, then
// the generic "Material".
wxString material_title(size_t slot, const PresetBundle* bundle, const DynamicPrintConfig& full)
{
    if (slot < bundle->filament_presets.size()) {
        const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot]);
        if (preset != nullptr && !preset->name.empty())
            return from_u8(material_display_name(preset->name));
    }
    const PublishMaterialIdentity identity = material_identity(slot, full);
    if (!identity.type.empty())
        return from_u8(identity.type);
    return _L("Material");
}

// The component slots of a mixed filament ("1,2,3"), 1-based, or empty when out of range.
std::vector<unsigned int> mixed_slot_components(const DynamicPrintConfig& full, size_t slot)
{
    const auto* comp_opt = full.opt<ConfigOptionStrings>("filament_mixed_components");
    if (comp_opt == nullptr || slot >= comp_opt->size())
        return {};
    return parse_mixed_components(comp_opt->values[slot]);
}

// Human-readable label of a mixed filament, mirroring the sidebar's mixed filament rows: the
// 1-based component slot numbers with their blend percentages (or a "->" gradient arrow),
// e.g. "1 (60%) + 2 (40%)". Used as the slot's tab/header title in place of a preset name.
wxString mixed_filament_label(const DynamicPrintConfig& full, size_t slot)
{
    const std::vector<unsigned int> comps = mixed_slot_components(full, slot);
    if (comps.empty())
        return _L("Mixed filament");
    const auto* grad_opt   = full.opt<ConfigOptionBools>("filament_mixed_gradient");
    const bool is_gradient = grad_opt != nullptr && slot < grad_opt->size() && grad_opt->values[slot];
    const auto* ratios_opt = full.opt<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    wxString label;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (i > 0)
            label += is_gradient ? wxString::FromUTF8(" \u2192 ") : wxString::FromUTF8(" + ");
        label += wxString::Format("%u", comps[i]);
        if (!is_gradient) {
            double ratio = 100.0 / comps.size();
            if (ratios_opt != nullptr && slot < ratios_opt->size()) {
                const std::vector<double> rs = parse_mixed_ratios(ratios_opt->values[slot], comps.size());
                if (i < rs.size())
                    ratio = rs[i] * 100.0;
            }
            label += wxString::Format(" (%d%%)", int(ratio + 0.5));
        }
    }
    return label;
}

// Blended representative colour of a mixed slot, computed exactly like the sidebar's swatches
// (recompute_mixed_slot_colors): sublayer slots blend by their ratios, gradient slots by their
// two end colours, broken references fall back to grey.
wxColour mixed_filament_blend_color(const DynamicPrintConfig& full, size_t slot)
{
    std::vector<wxColour> colors;
    if (const auto* colours = full.opt<ConfigOptionStrings>("filament_colour")) {
        colors.reserve(colours->values.size());
        for (const std::string& hex : colours->values) {
            const wxColour c(hex);
            colors.push_back(c.IsOk() ? c : wxColour(0, 0, 0));
        }
    }
    while (colors.size() <= slot)
        colors.push_back(wxColour("#D9D9D9"));
    recompute_mixed_slot_colors(colors, full);
    return slot < colors.size() ? colors[slot] : wxColour("#D9D9D9");
}

// The slot's own chip as the main GUI renders it: a curve-sampled gradient swatch for a
// gradient mixed slot, the static blended colour otherwise. Returns wxNullBitmap when nothing
// could be rendered (the caller falls back to a plain label).
wxBitmap mixed_filament_chip_bitmap(const DynamicPrintConfig& full, size_t slot, int swatch_sz)
{
    const std::string label = std::to_string(slot + 1);
    const auto* grad_opt    = full.opt<ConfigOptionBools>("filament_mixed_gradient");
    const bool is_gradient  = grad_opt != nullptr && slot < grad_opt->size() && grad_opt->values[slot];
    wxBitmap* icon          = nullptr;
    if (is_gradient) {
        // The same curve-sampled ramp the sidebar chips use; an empty ramp (broken definition)
        // degrades to a plain fade between the slot's two component colours.
        const std::vector<wxColour> ramp = mixed_gradient_ramp(full, slot, swatch_sz);
        if (!ramp.empty()) {
            icon = get_extruder_color_icon(std::vector<std::string>(), true, label, swatch_sz, swatch_sz, &ramp);
        } else {
            std::vector<std::string> hexes;
            for (const unsigned int comp : mixed_slot_components(full, slot)) {
                std::string hex = filament_color_hex(full, size_t(comp) - 1);
                if (hex.empty())
                    hex = "#D9D9D9";
                hexes.push_back(std::move(hex));
            }
            if (hexes.size() >= 2)
                icon = get_extruder_color_icon(std::move(hexes), true, label, swatch_sz, swatch_sz);
        }
    }
    if (icon == nullptr) {
        const wxColour blend = mixed_filament_blend_color(full, slot);
        const std::string blend_hex = blend.IsOk() ?
                                          std::string(wxString::Format("#%02X%02X%02X", blend.Red(), blend.Green(), blend.Blue()).ToUTF8()) :
                                          std::string("#808080");
        icon = get_extruder_color_icon(blend_hex, label, swatch_sz, swatch_sz);
    }
    return icon != nullptr ? *icon : wxNullBitmap;
}

// Tab-strip bitmap for a mixed slot: the mix's own chip, then its component swatches each
// followed by their percent share (or a "->" arrow for gradients), mirroring the main GUI's
// sidebar rows - e.g. "[3 purple]: [1 red] 50% + [2 blue] 50%". The whole composition is one
// bitmap because a TabCtrl item cannot interleave images into its text; the tab's text is
// therefore empty. Transparent background like the other chips.
wxBitmap mixed_filament_tab_bitmap(const DynamicPrintConfig& full, size_t slot, int swatch_sz)
{
    const std::vector<unsigned int> comps = mixed_slot_components(full, slot);
    if (comps.empty())
        return wxNullBitmap;
    const auto* grad_opt   = full.opt<ConfigOptionBools>("filament_mixed_gradient");
    const bool is_gradient = grad_opt != nullptr && slot < grad_opt->size() && grad_opt->values[slot];
    const auto* ratios_opt = full.opt<ConfigOptionStrings>("filament_mixed_sublayer_ratios");
    std::vector<double> ratio_pct(comps.size(), 100.0 / comps.size());
    if (!is_gradient && ratios_opt != nullptr && slot < ratios_opt->size()) {
        const std::vector<double> rs = parse_mixed_ratios(ratios_opt->values[slot], comps.size());
        for (size_t i = 0; i < rs.size() && i < comps.size(); ++i)
            ratio_pct[i] = rs[i] * 100.0;
    }

    const auto* colours     = full.opt<ConfigOptionStrings>("filament_colour");
    const wxString lead_sep = wxString::FromUTF8(":");
    const wxString comp_sep = is_gradient ? wxString::FromUTF8("\u2192") : wxString::FromUTF8("+");

    // Phase 1 (layout): render every swatch and measure every text piece so the composite
    // width is known before drawing; the dummy bitmap keeps GetTextExtent reliable.
    struct Piece
    {
        enum Kind { Swatch, Text } kind{Text};
        wxBitmap bmp;
        wxString text;
    };
    std::vector<Piece> pieces;
    bool has_lead = false;
    wxBitmap dummy(1, 1);
    wxMemoryDC measure_dc;
    measure_dc.SelectObject(dummy);
    measure_dc.SetFont(::Label::Body_12);
    const int gap = wxWindow::FromDIP(4, nullptr);

    auto push_swatch = [&](const std::string& hex, const std::string& label) {
        wxBitmap* icon = get_extruder_color_icon(hex, label, swatch_sz, swatch_sz);
        if (icon == nullptr)
            return;
        pieces.push_back({Piece::Swatch, *icon, wxString()});
    };
    auto push_text = [&](const wxString& text) { pieces.push_back({Piece::Text, wxNullBitmap, text}); };

    {
        const wxBitmap chip = mixed_filament_chip_bitmap(full, slot, swatch_sz);
        has_lead = chip.IsOk();
        if (has_lead)
            pieces.push_back({Piece::Swatch, chip, wxString()});
    }
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        if (pieces.empty())
            break;
        push_text(has_lead && pieces.size() == 1 ? lead_sep : comp_sep); // lead chip may have failed to render
        std::string hex = "#D9D9D9";
        if (colours != nullptr && comps[ci] >= 1 && comps[ci] <= colours->size())
            hex = colours->values[comps[ci] - 1];
        const size_t before = pieces.size();
        push_swatch(hex, std::to_string(comps[ci]));
        if (pieces.size() == before)
            break; // swatch failed: stop cleanly before an orphaned separator/percent pair
        if (!is_gradient) {
            push_text(wxString::Format("%d%%", int(ratio_pct[ci] + 0.5)));
        }
    }
    if (pieces.empty())
        return wxNullBitmap;

    int width = 0;
    for (const Piece& p : pieces)
        width += (p.kind == Piece::Swatch ? swatch_sz : measure_dc.GetTextExtent(p.text).x + gap);

    // Phase 2 (draw): transparent background like the page-header chips.
    wxBitmap composite(width, swatch_sz);
    wxMemoryDC memdc;
#ifdef __WXOSX__
    composite.UseAlpha();
    memdc.SelectObject(composite);
#else
    {
        wxImage img(width, swatch_sz);
        img.InitAlpha();
        memset(img.GetAlpha(), 0, width * swatch_sz);
        composite = wxBitmap(std::move(img));
    }
    memdc.SelectObject(composite);
#endif
    {
#ifdef __WXMSW__
        wxGCDC dc(memdc);
#else
        wxDC& dc = memdc;
#endif
        dc.SetBackgroundMode(wxTRANSPARENT);
        dc.SetFont(::Label::Body_12);
        dc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#262E30")));
        int x = 0;
        for (const Piece& p : pieces) {
            if (p.kind == Piece::Swatch) {
                dc.DrawBitmap(p.bmp, x, 0);
                x += swatch_sz;
            } else {
                const wxSize tsz = measure_dc.GetTextExtent(p.text);
                dc.DrawText(p.text, x + gap / 2, (swatch_sz - tsz.y) / 2);
                x += tsz.x + gap;
            }
        }
    }
    memdc.SelectObject(wxNullBitmap);
    return composite;
}

} // namespace

// Warning shown on OK when an enabled mixed-filament slot relies on a filament that would ship
// without its material. One row per unmet dependency: the mixed slot's colour chip, the
// component filament's colour chip, and the reason. "Cancel" is the safe choice and keeps the
// dialog open; "Publish anyway" continues.
class MixedFilamentWarningDialog : public MsgDialog
{
public:
    MixedFilamentWarningDialog(wxWindow* parent, const DynamicPrintConfig& full, const std::vector<MixedDependencyIssue>& issues)
        : MsgDialog(parent, _L("Warning"), wxEmptyString, wxOK | wxCANCEL | wxICON_WARNING)
    {
        auto* content = new wxBoxSizer(wxVERTICAL);

        auto* intro = new wxStaticText(this, wxID_ANY, _L("Some mixed filaments rely on filaments that will not be published:"));
        intro->SetFont(Label::Body_13);
        intro->Wrap(FromDIP(400));
        content->Add(intro, 0, wxEXPAND);
        content->AddSpacer(FromDIP(10));

        const int swatch = FromDIP(20);
        for (const MixedDependencyIssue& issue : issues) {
            auto* row = new wxBoxSizer(wxHORIZONTAL);

            // The mixed slot as just its own chip (gradient-aware, numbered like its tab);
            // falls back to a plain label when the chip cannot be built.
            const wxString mix_label = wxString::Format(_L("Filament %d (mixed)"), int(issue.mixed_slot) + 1);
            const wxBitmap mix_bmp   = mixed_filament_chip_bitmap(full, issue.mixed_slot, swatch);
            if (mix_bmp.IsOk()) {
                auto* bmp = new wxStaticBitmap(this, wxID_ANY, mix_bmp);
                bmp->SetToolTip(mix_label);
                row->Add(bmp, 0, wxALIGN_CENTER_VERTICAL);
            } else {
                auto* label = new wxStaticText(this, wxID_ANY, mix_label);
                label->SetFont(Label::Body_12);
                row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            }

            auto* needs = new wxStaticText(this, wxID_ANY, _L("needs"));
            needs->SetFont(Label::Body_12);
            needs->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
            row->Add(needs, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));

            // The component filament's colour chip, numbered like the tab strips; the slot
            // name stays on hover to keep the row itself short.
            std::string hex = filament_color_hex(full, issue.component_slot);
            if (hex.empty())
                hex = "#D9D9D9";
            if (wxBitmap* chip = get_extruder_color_icon(hex, std::to_string(issue.component_slot + 1), swatch, swatch)) {
                auto* comp_bmp = new wxStaticBitmap(this, wxID_ANY, *chip);
                comp_bmp->SetToolTip(wxString::Format(_L("Filament %d"), int(issue.component_slot) + 1));
                row->Add(comp_bmp, 0, wxALIGN_CENTER_VERTICAL);
            }

            auto* reason = new wxStaticText(this, wxID_ANY,
                issue.reason == MixedDependencyIssue::Reason::Disabled ? _L("not enabled") : _L("material not published"));
            reason->SetFont(Label::Body_12);
            reason->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#989898")));
            row->Add(reason, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

            content->Add(row, 0, wxLEFT, FromDIP(10));
            content->AddSpacer(FromDIP(6));
        }

        auto* hint = new wxStaticText(this, wxID_ANY,
            _L("To publish a mixed filament, enable every filament it uses and choose Full Publish or check its Type requirement."));
        hint->SetFont(Label::Body_12);
        hint->Wrap(FromDIP(380));
        content->Add(hint, 0, wxEXPAND | wxTOP, FromDIP(4));

        content_sizer->Add(content, 0, wxEXPAND);

        SetButtonLabel(wxID_OK, _L("Publish anyway"));
        SetButtonLabel(wxID_CANCEL, _L("Cancel"), true); // safe choice gets the focus

        finalize();
    }
};

PublishSettingsDialog::MixedVisualSpec PublishSettingsDialog::make_mixed_visual_spec(const DynamicPrintConfig& full, size_t slot)
{
    MixedVisualSpec spec;
    const std::vector<unsigned int> comps = mixed_slot_components(full, slot);
    if (comps.empty())
        return spec;

    const auto* grad_opt = full.opt<ConfigOptionBools>("filament_mixed_gradient");
    spec.is_gradient     = grad_opt != nullptr && slot < grad_opt->size() && grad_opt->values[slot];

    const auto* colours = full.opt<ConfigOptionStrings>("filament_colour");
    for (unsigned int cid : comps) {
        std::string hex = "#D9D9D9";
        if (colours != nullptr && cid >= 1 && cid <= colours->size())
            hex = colours->values[cid - 1];
        const wxColour c(hex);
        spec.component_colours.push_back(c.IsOk() ? c : wxColour("#D9D9D9"));
    }

    if (!spec.is_gradient) {
        // Sublayer shares; parse_mixed_ratios already falls back to equal shares and
        // normalizes to sum 1.
        spec.ratios.assign(comps.size(), 1.0 / comps.size());
        if (const auto* ratios_opt = full.opt<ConfigOptionStrings>("filament_mixed_sublayer_ratios"))
            if (slot < ratios_opt->size()) {
                const std::vector<double> rs = parse_mixed_ratios(ratios_opt->values[slot], comps.size());
                if (rs.size() == comps.size())
                    spec.ratios = rs;
            }
        if (comps.size() == 3)
            spec.tri_weights = spec.ratios; // the picker's barycentric shares
    } else {
        const Slic3r::GradientCurve curve = mixed_gradient_curve(full, slot);
        constexpr int kSamples            = 64;
        for (int i = 0; i <= kSamples; ++i) {
            const double t = double(i) / kSamples;
            spec.gradient_samples.emplace_back(t, sample_gradient_curve(curve, t));
        }
        for (const Slic3r::GradientAnchor& anchor : curve.points)
            spec.gradient_anchors.emplace_back(anchor.x, anchor.y);
    }

    spec.valid = true;
    return spec;
}

PublishSettingsDialog::PublishSettingsDialog(wxWindow* parent)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Publish 3MF..."),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxFULL_REPAINT_ON_RESIZE)
    , m_search(this, "search", 16)
    , m_menu(this, "filter", 16)
{
    SetBackgroundColour(*wxWHITE);

    // --- filter bar: search box, All/None, menu button ---
    wxPanel* f_bar = new wxPanel(this, wxID_ANY);
    f_bar->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* f_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_filter_box = new TextInput(f_bar, "", "", "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
    m_filter_box->SetIcon(m_search.bmp());
    m_filter_box->SetMinSize(FromDIP(wxSize(200, 24)));
    m_filter_box->SetSize(FromDIP(wxSize(-1, 24)));
    m_filter_box->SetFocus();
    m_filter_ctrl = m_filter_box->GetTextCtrl();
    m_filter_ctrl->SetFont(Label::Body_13);
    m_filter_ctrl->SetSize(wxSize(-1, FromDIP(16))); // centers text vertically
    m_filter_ctrl->SetHint(_L("Type to filter..."));
    m_filter_ctrl->Bind(wxEVT_TEXT, [this](auto&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_filter_ctrl->Bind(wxEVT_TEXT_ENTER, [this](auto&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_filter_ctrl->Bind(wxEVT_LEFT_DOWN, [this](auto& e) {
        if (m_filter_mode != FilterMode::Text)
            apply_filter(m_filter_ctrl->GetValue());
        e.Skip();
    });
    f_sizer->Add(m_filter_box, 1, wxEXPAND);

    m_fb_sizer      = new wxBoxSizer(wxHORIZONTAL);
    auto create_btn = [this, f_bar](wxString title, bool select) {
        auto btn = new wxStaticText(f_bar, wxID_ANY, title);
        btn->SetForegroundColour("#009687");
        btn->SetCursor(wxCURSOR_HAND);
        btn->SetFont(Label::Body_13);
        btn->Bind(wxEVT_LEFT_DOWN, [this, select](wxMouseEvent&) { select_all(select); });
        m_fb_sizer->Add(btn, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));
    };
    f_sizer->Add(m_fb_sizer, 0, wxALIGN_CENTER_VERTICAL);
    create_btn(_L("All"), true);
    create_btn(_L("None"), false);

    // Indicator for the menu's pseudo filters: sits where All/None go while they are hidden.
    // Labels reuse the menu entries (no new strings); clicking the chip returns to text
    // filtering, keeping the search box contents.
    m_pseudo_chip = new wxStaticText(f_bar, wxID_ANY, "");
    m_pseudo_chip->SetForegroundColour("#009687");
    m_pseudo_chip->SetCursor(wxCURSOR_HAND);
    m_pseudo_chip->SetFont(Label::Body_13);
    m_pseudo_chip->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) { apply_filter(m_filter_ctrl->GetValue()); });
    m_pseudo_chip->Hide();
    f_sizer->Add(m_pseudo_chip, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    m_menu_button = new wxStaticBitmap(f_bar, wxID_ANY, m_menu.bmp());
    m_menu_button->SetCursor(wxCURSOR_HAND);
    m_menu_button->Bind(wxEVT_LEFT_DOWN, &PublishSettingsDialog::show_menu, this);
    f_sizer->Add(m_menu_button, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(10));

    f_bar->SetSizerAndFit(f_sizer);

    m_outer_tabs = new TabCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
    m_outer_tabs->SetFont(Label::Body_14);
    m_outer_tabs->SetBackgroundColour(GetBackgroundColour());

    m_outer_host = new wxPanel(this, wxID_ANY);
    m_outer_host->SetBackgroundColour(GetBackgroundColour());
    m_outer_host_sizer = new wxBoxSizer(wxVERTICAL);
    m_outer_host->SetSizer(m_outer_host_sizer);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY, _L("Select which settings to embed in the 3MF file"));
    msg->SetFont(Label::Body_13);
    msg->Wrap(-1);
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(10));

    w_sizer->Add(f_bar, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_outer_tabs, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_outer_host, 1, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));

    build_option_model();

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        // Publish is always allowed: no settings selected means no settings override. Warn only
        // when an enabled mixed filament would ship without the material of one of its
        // components; "Publish anyway" accepts that and publishes anyway.
        if (const std::vector<MixedDependencyIssue> issues = unpublished_mixed_components(); !issues.empty()) {
            const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
            MixedFilamentWarningDialog warn(this, full, issues);
            if (warn.ShowModal() != wxID_OK)
                return; // Cancel: dismiss the warning and stay in this dialog
        }
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    w_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizerAndFit(w_sizer);
    fit_to_content(); // initial size only; the dialog is resizable
    wxGetApp().UpdateDlgDarkUI(this);
}

// Size the window to its content: width follows the widest tab strip so no filament tab is
// hidden (TabCtrl::relayout hides overflowing buttons), height scales proportionally. Both
// are floored at the 600x500 base and capped at hard DIP limits - deliberately not the whole
// display - with one last-resort clamp so the dialog can never open larger than the screen.
// Also owns the resize floor: the window cannot be resized below what the tabs need, so
// shrinking never re-hides a filament tab.
void PublishSettingsDialog::fit_to_content()
{
    static const wxSize BASE{600, 500};
    static const wxSize CAP{1300, 850};

    int strip = m_outer_tabs->GetFullSize();
    for (const SectionGroup& section : m_sections) {
        strip = std::max(strip, section.tabs->GetFullSize());
        if (section.mixed_tabs != nullptr)
            strip = std::max(strip, section.mixed_tabs->GetFullSize());
    }

    // Minimum width: whatever keeps every tab visible (never below the base). strip is device
    // pixels (Button min sizes); BASE/CAP are DIP and converted over.
    const int min_w = std::max(strip + 2 * FromDIP(10), FromDIP(BASE.x));

    // Initial size: prefer proportional growth within the caps.
    const int w    = std::clamp(min_w, FromDIP(BASE.x), FromDIP(CAP.x));
    const double f = double(w) / FromDIP(BASE.x);
    const int h    = std::clamp(int(FromDIP(BASE.y) * f), FromDIP(BASE.y), FromDIP(CAP.y));

    const wxRect area = wxDisplay(this).GetClientArea();
    const int max_w   = area.width * 9 / 10;
    const int max_h   = area.height * 9 / 10;

    SetMinSize(wxSize(std::min(min_w, max_w), std::min(FromDIP(BASE.y), max_h)));
    SetSize(std::min(w, max_w), std::min(h, max_h));
}

PublishSettingsDialog::~PublishSettingsDialog() {}

void PublishSettingsDialog::build_option_model()
{
    // Structural / non-publishable keys, shared with the published-3MF overlay path.
    const std::set<std::string>& denylist = publish_structural_keys();
    // Base keys already added in the print section (dedup across pages/optgroups). The printer
    // section keeps its own printer_added set keyed by the full per-extruder "#N" opt_id, so
    // every extruder gets its own row (see Phase 1 below).
    std::set<std::string> added;

    PresetBundle* bundle    = wxGetApp().preset_bundle;
    DynamicPrintConfig full = bundle->full_config();

    m_info_nonsel = _L("No selected items...");
    m_info_allsel = _L("All items selected...");
    m_info_empty  = _L("No matching items...");
    m_info_mix    = _L("Mixed filament - published as a whole when \"Enable\" above is selected");

    // Tab order differs from Section's enum order (Print, Printer, Material): the dialog
    // presents Printer, Filament, Process.
    m_sections.reserve(3);
    const Section tab_order[] = {Section::Printer, Section::Material, Section::Print};
    for (Section kind : tab_order)
        section_group_for(kind);
    bind_tab_events();

    // Shared per-option label/value computation; returns false when the option must be skipped
    // (denylisted / unknown / empty label). value is the stringified value; unit the translated
    // sidetext (may be empty).
    auto option_text = [&denylist, &full](const std::string& opt_id, const std::string& pure_key, wxString& label, wxString& value,
                                          wxString& unit) -> bool {
        if (denylist.count(pure_key) > 0)
            return false;
        const ConfigOptionDef* def = print_config_def.get(pure_key);
        if (def == nullptr)
            return false;
        label = _(def->full_label.empty() ? def->label : def->full_label);
        if (label.IsEmpty())
            return false;
        value = get_string_value(opt_id, full);
        unit  = _(def->sidetext);
        return true;
    };

    // --- Phase 1: printer per-extruder retraction settings (first, mirroring the sidebar's
    // Printer group), from the printer tab's "Extruder"/"Extruder N" pages. One inner tab per
    // extruder (e.g. "Left Extruder"/"Right Extruder" via Tab::translate_category), each holding
    // that extruder's Retraction and Z-Hop rows with per-extruder "#N" values.
    {
        size_t g = section_group_for(Section::Printer);
        std::set<std::string> printer_added;
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINTER)
                continue;
            for (const PageShp& page : tab->m_pages) {
                if (!page->title().StartsWith("Extruder"))
                    continue;
                // The extruder index of this page: its options are appended with the same
                // "#N" opt_index (opt.second.second), so derive the tab's index from the first
                // allowlisted option; skip the page when none is found (defensive).
                int extruder_idx = -1;
                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    if (optgroup->title != "Retraction" && optgroup->title != "Z-Hop")
                        continue;
                    for (const auto& opt : optgroup->opt_map())
                        if (extruder_idx < 0)
                            extruder_idx = opt.second.second;
                    if (extruder_idx >= 0)
                        break;
                }
                if (extruder_idx < 0)
                    continue;
                const wxString page_title = Tab::translate_category(page->title(), tab->m_type);
                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    // Allowlist on the untranslated optgroup title; the "Retraction when
                    // switching material" group is intentionally skipped.
                    if (optgroup->title != "Retraction" && optgroup->title != "Z-Hop")
                        continue;
                    const wxString subcategory = _(optgroup->title);
                    for (const auto& opt : optgroup->opt_map()) {
                        const std::string& opt_id   = opt.first;
                        const std::string& pure_key = opt.second.first;
                        // Rows are keyed by the full per-extruder "#N" opt_id so each extruder
                        // tab publishes its own value; GetPublishedKeys() emits the checked rows
                        // as-is.
                        if (!printer_added.insert(opt_id).second)
                            continue;
                        wxString label, value, unit;
                        if (!option_text(opt_id, pure_key, label, value, unit))
                            continue;
                        size_t cat_index = category_index_for(page_title, Section::Printer, g, size_t(extruder_idx));
                        size_t sub_index = subcategory_index_for(cat_index, subcategory, optgroup->icon);
                        add_row_ui(opt_id, label, value, unit, cat_index, sub_index);
                    }
                }
            }
        }
    }

    // --- Phase 2: per-material sections synthesized from the filament tab's "Setting
    // Overrides" page, under the Filament group.
    {
        size_t g          = section_group_for(Section::Material);
        Tab* filament_tab = nullptr;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->m_type == Preset::TYPE_FILAMENT) {
                filament_tab = tab;
                break;
            }
        if (filament_tab != nullptr) {
            const Page* overrides_page = nullptr;
            for (const PageShp& page : filament_tab->m_pages)
                if (page->title() == "Setting Overrides") {
                    overrides_page = page.get();
                    break;
                }

            if (overrides_page != nullptr) {
                // Mixed-color slots are virtual: they carry no per-key Material/Retraction
                // settings; their "Enable" toggle always embeds the mix definition (components,
                // ratios, gradient). Detect them via the project-level flag.
                const ConfigOptionBools* is_mixed_opt = full.opt<ConfigOptionBools>("filament_is_mixed");
                // One section per filament slot (a 4-slot printer shows 4 pages), each
                // disambiguated by its colour chip and slot identity while showing the bare name.
                for (size_t slot = 0; slot < bundle->filament_presets.size(); ++slot) {
                    const bool is_mixed = is_mixed_opt != nullptr && slot < is_mixed_opt->size() && is_mixed_opt->values[slot];
                    const PublishMaterialIdentity identity = material_identity(slot, full);
                    // A mixed slot's title is its component composition (e.g. "1 (60%) + 2
                    // (40%)"), not the cloned preset's name shown in the main GUI.
                    const wxString title        = is_mixed ? mixed_filament_label(full, slot) : material_title(slot, bundle, full);
                    const size_t category_index = category_index_for(title, Section::Material, g, slot, identity, is_mixed);

                    if (is_mixed) {
                        // A mixed slot publishes as one unit (its definition); nothing to select
                        // per-key. Its component filaments are auto-enabled + Full Published when
                        // "Enable" is checked (see on_enable_toggle). Its page still shows what
                        // would be published: a ratio bar, or the gradient graph.
                        add_mixed_visual(category_index, make_mixed_visual_spec(full, slot));
                        continue;
                    }

                    // Material requirement rows: an optional filament colour and/or a
                    // vendor-agnostic material type for this slot, in their own optgroup so they
                    // stay visually separated from the setting rows.
                    {
                        const size_t req_sub = subcategory_index_for(category_index, _L("Material"), "custom-gcode_filament");
                        add_row_ui("filament_colour", _L("Color"), from_u8(filament_color_hex(full, slot)), wxString(), category_index,
                                   req_sub, RowKind::Color);
                        std::string type;
                        if (const auto* types = full.opt<ConfigOptionStrings>("filament_type"))
                            if (slot < types->size())
                                type = types->get_at(slot);
                        add_row_ui("filament_type", _L("Type"), from_u8(normalize_filament_type(type)), wxString(), category_index, req_sub,
                                   RowKind::Type);
                    }

                    // A material section must not repeat a key; the same key may appear in other
                    // material sections - that is intended.
                    std::set<std::string> material_added;

                    for (const ConfigOptionsGroupShp& optgroup : overrides_page->m_optgroups) {
                        // Allowlist on the untranslated optgroup title; "Ironing" is skipped.
                        if (optgroup->title != "Retraction" && optgroup->title != "Retraction when switching material")
                            continue;
                        for (const auto& opt : optgroup->opt_map()) {
                            // Row keys are base keys; the load side applies them positionally.
                            const std::string& opt_id = opt.first;
                            std::string base          = publish_base_key(opt_id);
                            if (!material_added.insert(base).second)
                                continue;
                            // Show the value of this slot; fall back to slot 0 if out of range.
                            std::string value_opt_id = base + "#" + std::to_string(slot);
                            if (const ConfigOption* opt_cfg = full.option(base))
                                if (const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(opt_cfg))
                                    if (vec->size() > 0 && slot >= vec->size())
                                        value_opt_id = base + "#0";
                            wxString label, value, unit;
                            if (!option_text(value_opt_id, base, label, value, unit))
                                continue;
                            size_t sub_index = subcategory_index_for(category_index, _(optgroup->title), optgroup->icon);
                            add_row_ui(base, label, value, unit, category_index, sub_index);
                        }
                    }
                }
            }
        }
    }

    // --- Phase 3: process (print) settings; grouping matches the process tab.
    {
        size_t g = section_group_for(Section::Print);
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINT)
                continue;
            size_t page_index = 0;
            for (const PageShp& page : tab->m_pages) {
                wxString category           = Tab::translate_category(page->title(), tab->m_type);
                const size_t category_index = category_index_for(category, Section::Print, g, page_index);

                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    for (const auto& opt : optgroup->opt_map()) {
                        // opt_map() key is the opt_id (may carry "#N"); the value is
                        // (pure_opt_key, opt_index).
                        const std::string& opt_id   = opt.first;
                        const std::string& pure_key = opt.second.first;
                        // A key may appear in more than one page/group; keep the first.
                        if (!added.insert(pure_key).second)
                            continue;
                        wxString label, value, unit;
                        if (!option_text(opt_id, pure_key, label, value, unit))
                            continue;
                        size_t sub_index = subcategory_index_for(category_index, _(optgroup->title), optgroup->icon);
                        add_row_ui(opt_id, label, value, unit, category_index, sub_index);
                    }
                }
                ++page_index;
            }
        }
    }

    // Pre-check the dirty (modified) settings and mark them bold (per-slot match, across all
    // sections; collect_dirty_settings_keys unions the prints, printers and filaments).
    // Dirty keys carry a "#N" per-extruder/per-slot suffix (deep_diff), so the base key alone
    // cannot distinguish which extruder/filament slot changed: match the row's exact key, and
    // for material rows (base key + per-slot value) the base key plus the section's slot.
    std::set<std::string> dirty_keys;
    for (const std::string& key : collect_dirty_settings_keys(*wxGetApp().preset_bundle))
        dirty_keys.insert(key);
    for (Row& row : m_rows) {
        // The Color/Type requirement rows are not "dirty overrides": never auto-checked.
        if (row.kind != RowKind::Setting)
            continue;
        bool dirty = dirty_keys.count(row.key) != 0;
        if (!dirty && row.section == Section::Material)
            dirty = dirty_keys.count(publish_base_key(row.key) + "#" + std::to_string(m_categories[row.inner_index].filament_slot)) != 0;
        row.dirty = dirty;
        if (row.dirty) {
            row.check->SetValue(true);
            set_row_bold(row, true);
        }
    }

    // Wire the "Enable" checkboxes: toggling one reveals/hides the slot's settings below the
    // header (and, for a mixed slot, auto-selects its components). Bind by index so the lambda
    // stays valid even if the vector is reallocated later.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].enable_check != nullptr)
            m_categories[c].enable_check->Bind(wxEVT_CHECKBOX, [this, c](wxCommandEvent&) { on_enable_toggle(c); });

    // Wire the "Full Publish" checkboxes (physical slots): toggling one disables/enables the
    // material's rows.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].full_check != nullptr)
            m_categories[c].full_check->Bind(wxEVT_CHECKBOX, [this, c](wxCommandEvent&) { on_full_toggle(c); });

    // No filter is active at startup: every row matches until the user types.
    for (Row& row : m_rows)
        row.matches_filter = true;
    apply_visibility();

    for (Category& category : m_categories) {
        category.scroll->FitInside();
        category.list_sizer->Layout();
    }
    for (SectionGroup& section : m_sections)
        if (!section.categories.empty())
            section.tabs->SelectItem(0);
    if (!m_sections.empty()) {
        m_outer_tabs->SelectItem(0);
        show_outer_page(0);
    }
}

size_t PublishSettingsDialog::section_group_for(Section kind)
{
    for (size_t i = 0; i < m_sections.size(); ++i)
        if (m_sections[i].kind == kind)
            return i;

    SectionGroup section;
    section.kind           = kind;
    const size_t new_index = m_sections.size();
    switch (kind) {
    case Section::Printer:
        section.title     = _L("Printer");
        section.icon_name = "printer";
        break;
    case Section::Material:
        section.title     = _L("Filament");
        section.icon_name = "filament";
        break;
    case Section::Print:
        section.title     = _L("Process");
        section.icon_name = "process";
        break;
    }
    section.icon_bmp = ScalableBitmap(this, section.icon_name, 16);

    section.page = new wxPanel(m_outer_host, wxID_ANY);
    section.page->SetBackgroundColour(GetBackgroundColour());
    auto* page_sizer = new wxBoxSizer(wxVERTICAL);
    section.tabs     = new TabCtrl(section.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
    section.tabs->SetFont(Label::Body_14);
    section.tabs->SetBackgroundColour(GetBackgroundColour());
    page_sizer->Add(section.tabs, 0, wxEXPAND);
    // Mixed-color filament slots get a second tab strip below the physical filament tabs; only
    // the Material section has them.
    if (kind == Section::Material) {
        section.mixed_tabs = new TabCtrl(section.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
        section.mixed_tabs->SetFont(Label::Body_14);
        section.mixed_tabs->SetBackgroundColour(GetBackgroundColour());
        // The mixed tabs carry full swatch compositions: give them extra room to breathe so
        // neighbouring compositions do not read as one long row (must precede AppendItem).
        section.mixed_tabs->SetItemSpace(FromDIP(5));
        page_sizer->Add(section.mixed_tabs, 0, wxEXPAND | wxTOP, FromDIP(2));
        section.mixed_tabs->Hide();
    }
    section.page_host = new wxPanel(section.page, wxID_ANY);
    section.page_host->SetBackgroundColour(GetBackgroundColour());
    section.page_host_sizer = new wxBoxSizer(wxVERTICAL);
    section.page_host->SetSizer(section.page_host_sizer);
    page_sizer->Add(section.page_host, 1, wxEXPAND | wxTOP, FromDIP(4));
    section.page->SetSizer(page_sizer);

    if (section.icon_bmp.bmp().IsOk())
        m_outer_tabs->AppendItem(section.title, section.icon_bmp.bmp());
    else
        m_outer_tabs->AppendItem(section.title);
    m_outer_host_sizer->Add(section.page, 1, wxEXPAND);
    section.page->Hide();
    m_sections.push_back(std::move(section));
    return new_index;
}

size_t PublishSettingsDialog::category_index_for(
    const wxString& title, Section section, size_t group, size_t source_index, const PublishMaterialIdentity& identity, bool is_mixed)
{
    // Dedup across both tab rows (physical + mixed); mixed slots are never duplicated anyway.
    auto match = [&](const Category& existing) {
        return existing.title == title && existing.section == section && existing.source_index == source_index &&
               existing.filament_id == identity.id && existing.filament_type == identity.type &&
               existing.filament_vendor == identity.vendor;
    };
    for (size_t i : m_sections[group].categories)
        if (match(m_categories[i]))
            return i;
    for (size_t i : m_sections[group].mixed_categories)
        if (match(m_categories[i]))
            return i;

    Category category;
    category.title           = title;
    category.section         = section;
    category.group           = group;
    category.source_index    = source_index;
    category.filament_type   = identity.type;
    category.filament_vendor = identity.vendor;
    category.filament_id     = identity.id;
    category.filament_slot   = source_index;
    category.is_mixed        = is_mixed;
    category.page            = new wxPanel(m_sections[group].page_host, wxID_ANY);
    category.page->SetBackgroundColour(GetBackgroundColour());
    auto* page_sizer = new wxBoxSizer(wxVERTICAL);

    // The physical slot's colour chip decorates the page header and the inner tab; it carries
    // the 1-based slot number, mirroring the main GUI's filament swatches. A mixed slot has no
    // header at all: its page is just the Enable toggle above the (always visible) definition
    // preview - the identification lives in the tab strip's composition bitmap.
    std::string hex;
    std::string chip_label;
    if (section == Section::Material && !is_mixed) {
        const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
        hex                           = filament_color_hex(full, source_index);
        chip_label                    = std::to_string(source_index + 1);
    }

    if (section == Section::Material) {
        if (is_mixed) {
            // No chip/title: the lone "Enable" checkbox tops the page.
            auto* enable_sizer    = new wxBoxSizer(wxHORIZONTAL);
            category.enable_check = new wxCheckBox(category.page, wxID_ANY, _L("Enable"));
            category.enable_check->SetFont(Label::Body_13);
            category.enable_check->SetToolTip(_L("Publish this mixed filament and enable + Full Publish its component filaments"));
            enable_sizer->Add(category.enable_check, 0, wxALIGN_CENTER_VERTICAL);
            page_sizer->Add(enable_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));
        } else {
            // Line 1: [chip] [title] [Enable]. The Enable checkbox gates the whole slot: while
            // it is unchecked nothing below the title is shown and nothing of it is published.
            auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);
            if (wxBitmap* chip = get_extruder_color_icon(hex, chip_label, FromDIP(20), FromDIP(20))) {
                category.filament_color_chip = new wxStaticBitmap(category.page, wxID_ANY, *chip);
                header_sizer->Add(category.filament_color_chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
            }
            category.title_label = new wxStaticText(category.page, wxID_ANY, title);
            category.title_label->SetFont(Label::Head_14);
            header_sizer->Add(category.title_label, 0, wxALIGN_CENTER_VERTICAL);
            category.enable_check = new wxCheckBox(category.page, wxID_ANY, _L("Enable"));
            category.enable_check->SetFont(Label::Body_13);
            category.enable_check->SetToolTip(_L("Publish this filament slot in the 3MF file"));
            header_sizer->Add(category.enable_check, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
            page_sizer->Add(header_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));

            // Line 2: the "Full Publish" toggle, on its own line below the title (hidden until
            // the slot is enabled).
            auto* full_sizer    = new wxBoxSizer(wxHORIZONTAL);
            category.full_check = new wxCheckBox(category.page, wxID_ANY, _L("Full Publish"));
            category.full_check->SetFont(Label::Body_13);
            category.full_check->SetToolTip(_L("Embed the entire filament of this slot in the 3MF file"));
            full_sizer->Add(category.full_check, 0, wxALIGN_CENTER_VERTICAL);
            category.full_line_item = page_sizer->Add(full_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(2));
        }
    }

    category.scroll = new wxScrolledWindow(category.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    category.scroll->SetScrollRate(0, 10);
    category.scroll->SetBackgroundColour(GetBackgroundColour());
    category.list_sizer = new wxBoxSizer(wxVERTICAL);
    category.scroll->SetSizer(category.list_sizer);
    category.scroll->DisableFocusFromKeyboard();
    category.scroll->Bind(wxEVT_RIGHT_DOWN, &PublishSettingsDialog::show_menu, this);
    category.info = new wxStaticText(category.scroll, wxID_ANY, is_mixed ? m_info_mix : m_info_empty);
    category.info->SetFont(Label::Body_13);
    category.list_sizer->Add(category.info, 1, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(10));
    category.info->Hide();
    page_sizer->Add(category.scroll, 1, wxEXPAND | wxALL, FromDIP(4));
    // A material slot starts disabled: its rows (and its Full Publish line) stay hidden until
    // "Enable" is checked.
    if (section == Section::Material)
        category.scroll->Hide();
    category.page->SetSizer(page_sizer);
    category.page->Hide();

    const size_t category_index = m_categories.size();
    m_categories.push_back(std::move(category));
    // Mixed slots live in a second tab row below the physical filament tabs.
    if (is_mixed)
        m_sections[group].mixed_categories.push_back(category_index);
    else
        m_sections[group].categories.push_back(category_index);
    if (section == Section::Material) {
        TabCtrl* target = is_mixed ? m_sections[group].mixed_tabs : m_sections[group].tabs;
        if (is_mixed) {
            // The tab's whole composition (mix chip + components + percents) lives in one
            // bitmap; the text is empty because a TabCtrl item cannot interleave images into
            // its text.
            const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
            const wxBitmap tab_bmp            = mixed_filament_tab_bitmap(full_cfg, source_index, FromDIP(20));
            if (tab_bmp.IsOk())
                target->AppendItem(wxString(), tab_bmp);
            else
                target->AppendItem(title);
        } else if (wxBitmap* chip = get_extruder_color_icon(hex, chip_label, FromDIP(20), FromDIP(20))) {
            target->AppendItem(title, *chip);
        } else {
            target->AppendItem(title);
        }
    } else {
        m_sections[group].tabs->AppendItem(title);
    }
    m_sections[group].page_host_sizer->Add(m_categories[category_index].page, 1, wxEXPAND);
    if (is_mixed) {
        if (m_sections[group].selected_mixed < 0)
            m_sections[group].selected_mixed = 0;
        m_sections[group].mixed_tabs->Show();
    } else if (m_sections[group].selected_inner < 0) {
        m_sections[group].selected_inner = 0;
    }
    return category_index;
}

size_t PublishSettingsDialog::subcategory_index_for(size_t category_index, const wxString& title, const wxString& icon)
{
    Category& category = m_categories[category_index];
    for (size_t i = 0; i < category.subs.size(); ++i)
        if (category.subs[i].title == title)
            return i;

    Subcategory sub;
    sub.title = title;
    if (!title.IsEmpty()) {
        sub.header = new ::StaticLine(category.scroll, false, title, icon);
        sub.header->SetFont(Label::Head_14);
        sub.header->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        auto* wrap = new wxBoxSizer(wxVERTICAL);
        wrap->Add(sub.header, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(6));
        sub.item = category.list_sizer->Add(wrap, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(22));
    }
    category.subs.push_back(std::move(sub));
    return category.subs.size() - 1;
}

void PublishSettingsDialog::add_row_ui(const std::string& key,
                                       const wxString& label,
                                       const wxString& value,
                                       const wxString& unit,
                                       size_t category_index,
                                       size_t subcategory_index,
                                       RowKind kind)
{
    Category& category = m_categories[category_index];
    Row row;
    row.key                = key;
    row.label              = label;
    row.value              = value;
    row.unit               = unit;
    row.kind               = kind;
    row.category           = category.title;
    row.subcategory        = category.subs[subcategory_index].title;
    row.section            = category.section;
    row.section_title      = m_sections[category.group].title;
    row.outer_index        = category.group;
    row.inner_index        = category_index;
    const size_t row_index = m_rows.size();
    m_rows.push_back(std::move(row));
    Row& current  = m_rows[row_index];
    current.check = new wxCheckBox(category.scroll, wxID_ANY, label);
    current.check->SetFont(Label::Body_13);
    auto* row_sizer = new wxBoxSizer(wxHORIZONTAL);
    row_sizer->Add(current.check, 0, wxALIGN_CENTER_VERTICAL);
    // The value is read-only text (incl. the Type row: the published type is the slot's
    // normalized type, not author-editable).
    current.value_label = new wxStaticText(category.scroll, wxID_ANY, value, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    current.value_label->SetFont(Label::Body_13);
    current.value_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    current.value_label->SetToolTip(unit.IsEmpty() ? value : value + " " + unit);
    if (kind == RowKind::Color && !value.IsEmpty()) {
        // The colour swatch carries this slot's 1-based number, like the main GUI swatches.
        const std::string chip_label = std::to_string(category.filament_slot + 1);
        if (wxBitmap* chip = get_extruder_color_icon(value.ToStdString(), chip_label, FromDIP(20), FromDIP(20))) {
            current.color_chip = new wxStaticBitmap(category.scroll, wxID_ANY, *chip);
            row_sizer->Add(current.color_chip, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
        }
    }
    row_sizer->Add(current.value_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    if (!unit.IsEmpty()) {
        current.unit_label = new wxStaticText(category.scroll, wxID_ANY, unit);
        current.unit_label->SetFont(Label::Body_13);
        current.unit_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        row_sizer->Add(current.unit_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    }
    current.item = category.list_sizer->Add(row_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
    category.rows.push_back(row_index);
    category.subs[subcategory_index].rows.push_back(row_index);
}

void PublishSettingsDialog::on_full_toggle(size_t category_index)
{
    Category& cat   = m_categories[category_index];
    const bool full = cat.full_check->GetValue();
    for (size_t r : cat.rows)
        m_rows[r].check->Enable(!full);
}

void PublishSettingsDialog::on_enable_toggle(size_t category_index)
{
    Category& cat      = m_categories[category_index];
    const bool enabled = cat.enable_check->GetValue();

    // A published mixed filament needs its component filaments published too: mark the slots it
    // uses as "Enable"d and Full Published so the mix's physical components always ship their
    // identities.
    if (cat.is_mixed && enabled) {
        const DynamicPrintConfig full              = wxGetApp().preset_bundle->full_config();
        const std::vector<unsigned int> components = mixed_slot_components(full, cat.filament_slot);
        for (const unsigned int component : components) {
            // Components are 1-based physical filament indices.
            const size_t component_slot = size_t(component) - 1;
            for (size_t c = 0; c < m_categories.size(); ++c) {
                Category& comp_cat = m_categories[c];
                if (comp_cat.section != Section::Material || comp_cat.is_mixed || comp_cat.filament_slot != component_slot)
                    continue;
                if (comp_cat.enable_check != nullptr)
                    comp_cat.enable_check->SetValue(true);
                if (comp_cat.full_check != nullptr)
                    comp_cat.full_check->SetValue(true);
                on_full_toggle(c);
            }
        }
    }

    // Reveal/hide everything below the slot's header (rows, info, Full Publish line) and refresh
    // the visibility of the auto-selected component slots.
    apply_visibility();
    if (cat.page != nullptr)
        cat.page->GetSizer()->Layout();
}

void PublishSettingsDialog::add_mixed_visual(size_t category_index, const MixedVisualSpec& spec)
{
    if (category_index >= m_categories.size() || !spec.valid)
        return;
    Category& category = m_categories[category_index];
    if (category.page == nullptr || category.page->GetSizer() == nullptr || category.scroll == nullptr || spec.component_colours.empty())
        return;

    auto* viz = new wxPanel(category.page, wxID_ANY);
    viz->SetBackgroundStyle(wxBG_STYLE_PAINT);
    // Per-panel fill-bitmap cache for the ternary branch; rebuilt only when size or colours
    // change (shared_ptr keeps the lifetime independent of this method's locals).
    struct TriCache
    {
        wxBitmap bmp;
        wxSize sz{0, 0};
        wxColour c0, c1, c2;
    };
    auto tri_cache = std::make_shared<TriCache>();
    // Theme colours and DIP metrics are resolved inside the paint handler so dark-mode toggles
    // and DPI changes are picked up on the next repaint without any explicit listener.
    viz->Bind(wxEVT_PAINT, [this, panel = viz, spec, tri_cache](wxPaintEvent&) {
        const wxColour bg = StateColor::darkModeColorFor(*wxWHITE);
        wxBufferedPaintDC pdc(panel);
        pdc.SetBackground(wxBrush(bg));
        pdc.Clear();
        const wxRect rc = panel->GetClientRect();
        if (rc.width <= 0 || rc.height <= 0)
            return;

        const size_t n = spec.component_colours.size();

        if (spec.tri_weights.size() == 3 && n == 3 && !spec.is_gradient) {
            // Ternary mix: a read-only miniature of the MixedFilamentDialog's triangle picker.
            // Per-pixel barycentric fill is cached into a bitmap keyed on size + colours; the
            // marker and labels are redrawn on top every paint.
            const wxColour tri_bg   = StateColor::darkModeColorFor(*wxWHITE);
            const wxColour outline  = StateColor::darkModeColorFor(wxColour("#CECECE"));
            const wxColour ring     = StateColor::darkModeColorFor(wxColour("#262E30"));
            const wxColour label_c  = StateColor::darkModeColorFor(wxColour(107, 107, 107)); // grey 700
            const double margin_dip = 24.0;
            auto& cache             = *tri_cache;

            auto vertices_for = [&](const wxSize& sz) -> std::tuple<TriPoint, TriPoint, TriPoint> {
                const double pw = sz.GetWidth(), ph = sz.GetHeight();
                const int margin   = FromDIP(int(margin_dip));
                const double avail = std::min(pw, ph) - 2.0 * margin;
                const double side  = avail;
                const double tri_h = side * std::sqrt(3.0) / 2.0;
                const double cx    = pw / 2.0;
                const double top_y = (ph - tri_h) / 2.0;
                return {{cx, top_y}, {cx - side / 2.0, top_y + tri_h}, {cx + side / 2.0, top_y + tri_h}};
            };

            pdc.SetFont(::Label::Body_12);
            const wxColour& c0 = spec.component_colours[0];
            const wxColour& c1 = spec.component_colours[1];
            const wxColour& c2 = spec.component_colours[2];

            if (!cache.bmp.IsOk() || cache.sz != rc.GetSize() || cache.c0 != c0 || cache.c1 != c1 || cache.c2 != c2) {
                auto [v0, v1, v2] = vertices_for(rc.GetSize());
                cache.bmp         = wxBitmap(rc.width, rc.height, 32);
                wxMemoryDC mdc(cache.bmp);
                mdc.SetBrush(wxBrush(tri_bg));
                mdc.SetPen(*wxTRANSPARENT_PEN);
                mdc.DrawRectangle(0, 0, rc.width, rc.height);

                const int min_y = int(std::min({v0.y, v1.y, v2.y}));
                const int max_y = int(std::max({v0.y, v1.y, v2.y}));
                const int min_x = int(std::min({v0.x, v1.x, v2.x}));
                const int max_x = int(std::max({v0.x, v1.x, v2.x}));
                for (int py = min_y; py <= max_y; ++py)
                    for (int px = min_x; px <= max_x; ++px) {
                        const TriPoint p = {double(px), double(py)};
                        if (!tri_contains(p, v0, v1, v2))
                            continue;
                        double w0, w1, w2;
                        tri_barycentric(p, v0, v1, v2, w0, w1, w2);
                        unsigned char mr, mg, mb;
                        if (w0 + w1 > 1e-6) {
                            float t01 = static_cast<float>(w1 / (w0 + w1));
                            Slic3r::filament_mixer_lerp(c0.Red(), c0.Green(), c0.Blue(), c1.Red(), c1.Green(), c1.Blue(), t01, &mr, &mg,
                                                        &mb);
                            Slic3r::filament_mixer_lerp(mr, mg, mb, c2.Red(), c2.Green(), c2.Blue(), static_cast<float>(w2), &mr, &mg, &mb);
                        } else {
                            mr = c2.Red();
                            mg = c2.Green();
                            mb = c2.Blue();
                        }
                        mdc.SetPen(wxPen(wxColour(mr, mg, mb)));
                        mdc.DrawPoint(px, py);
                    }

                mdc.SetPen(wxPen(outline, 1));
                mdc.SetBrush(*wxTRANSPARENT_BRUSH);
                const wxPoint pts[3] = {{int(v0.x), int(v0.y)}, {int(v1.x), int(v1.y)}, {int(v2.x), int(v2.y)}};
                mdc.DrawPolygon(3, pts);
                mdc.SelectObject(wxNullBitmap);

                cache.sz = rc.GetSize();
                cache.c0 = c0;
                cache.c1 = c1;
                cache.c2 = c2;
            }
            pdc.DrawBitmap(cache.bmp, 0, 0);

            // Published-ratio marker (read-only twin of the editor's drag handle).
            {
                auto [v0, v1, v2] = vertices_for(rc.GetSize());
                const double w0 = spec.tri_weights[0], w1 = spec.tri_weights[1], w2 = spec.tri_weights[2];
                const int hx = int(w0 * v0.x + w1 * v1.x + w2 * v2.x);
                const int hy = int(w0 * v0.y + w1 * v1.y + w2 * v2.y);
                pdc.SetBrush(*wxWHITE_BRUSH);
                pdc.SetPen(wxPen(ring, FromDIP(2)));
                pdc.DrawCircle(hx, hy, FromDIP(5));

                // Percent label beside each vertex.
                for (int i = 0; i < 3; ++i) {
                    const wxString text = wxString::Format("%d%%", int(std::lround(spec.tri_weights[i] * 100.0)));
                    const wxSize tsz    = pdc.GetTextExtent(text);
                    const TriPoint vtx  = (i == 0) ? v0 : (i == 1) ? v1 : v2;
                    int lx              = int(vtx.x - tsz.GetWidth() / 2.0);
                    int ly              = (i == 0) ? int(vtx.y - tsz.GetHeight()) : int(vtx.y + FromDIP(3));
                    ly                  = std::clamp(ly, 0, rc.height - tsz.GetHeight());
                    lx                  = std::clamp(lx, 0, rc.width - tsz.GetWidth());
                    pdc.SetTextForeground(label_c);
                    pdc.DrawText(text, lx, ly);
                }
            }
        } else if (!spec.is_gradient) {
            // Stacked ratio bar: one solid segment per component, widths proportional to the
            // published shares. Integer widths accumulate left to right; the last segment takes
            // the rounding remainder so the bar always fills exactly.
            std::vector<double> shares = spec.ratios;
            double total               = 0.0;
            for (double r : shares)
                total += r;
            if (shares.size() != n || total <= 0.0) {
                shares.assign(n, 1.0 / n);
                total = 1.0;
            }
            auto share_to_px = [&](double share_sum) { return rc.x + int(std::lround(share_sum / total * double(rc.width))); };
            std::vector<wxRect> segs(n);
            int x0 = rc.x;
            for (size_t i = 0; i < n; ++i) {
                int x1 = rc.x + rc.width;
                if (i + 1 < n)
                    x1 = share_to_px(std::accumulate(shares.begin(), shares.begin() + i + 1, 0.0));
                segs[i] = wxRect(x0, rc.y, std::max(1, x1 - x0), rc.height);
                x0      = segs[i].GetRight() + 1;
            }

            for (size_t i = 0; i < n; ++i) {
                pdc.SetPen(*wxTRANSPARENT_PEN);
                pdc.SetBrush(wxBrush(spec.component_colours[i]));
                pdc.DrawRectangle(segs[i]);
            }
            pdc.SetBrush(*wxTRANSPARENT_BRUSH);
            pdc.SetPen(wxPen(StateColor::darkModeColorFor(wxColour("#ACACAC")), 1));
            pdc.DrawRectangle(rc);

            // Percent label centred in each segment wide enough to hold it.
            pdc.SetFont(::Label::Body_12);
            for (size_t i = 0; i < n; ++i) {
                const wxString text = wxString::Format("%d%%", int(std::lround(shares[i] / total * 100.0)));
                const wxSize tsz    = pdc.GetTextExtent(text);
                if (tsz.GetWidth() + FromDIP(4) > segs[i].GetWidth())
                    continue;
                // Label contrast follows the swatch itself, not the theme.
                const wxColour& c = spec.component_colours[i];
                const double lum  = 0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue();
                pdc.SetTextForeground(lum > 140 ? wxColour("#262E30") : *wxWHITE);
                pdc.DrawText(text, segs[i].x + (segs[i].GetWidth() - tsz.GetWidth()) / 2, rc.y + (rc.height - tsz.GetHeight()) / 2);
            }
        } else {
            // Gradient: compact "Material Ratio" over "Model Height" graph, a read-only
            // miniature of the GradientCurveEditor plot. Component order matches the config;
            // the second component's curve is the mirror of the first's.
            const wxColour grid_color  = StateColor::darkModeColorFor(wxColour(238, 238, 238)); // grey 300
            const wxColour axis_color  = StateColor::darkModeColorFor(wxColour(107, 107, 107)); // grey 700
            const wxColour label_muted = StateColor::darkModeColorFor(wxColour(107, 107, 107));
            const wxColour point_fill  = StateColor::darkModeColorFor(*wxWHITE);

            const int pad_left   = FromDIP(34);
            const int pad_right  = FromDIP(10);
            const int pad_top    = FromDIP(18);
            const int pad_bottom = FromDIP(16);
            const wxRect plot(rc.x + pad_left, rc.y + pad_top, std::max(1, rc.width - pad_left - pad_right),
                              std::max(1, rc.height - pad_top - pad_bottom));

            constexpr int kGridDivisions = 5;
            pdc.SetPen(wxPen(grid_color, 1));
            for (int i = 0; i <= kGridDivisions; ++i) {
                const int gx = plot.x + plot.width * i / kGridDivisions;
                const int gy = plot.y + plot.height * i / kGridDivisions;
                pdc.DrawLine(gx, plot.y, gx, plot.y + plot.height);
                pdc.DrawLine(plot.x, gy, plot.x + plot.width, gy);
            }

            // Axes with small filled arrowheads along the plot's left and bottom edges.
            const int arrow_len  = FromDIP(7);
            const int arrow_half = FromDIP(3);
            pdc.SetPen(wxPen(axis_color, 1));
            pdc.SetBrush(wxBrush(axis_color));
            pdc.DrawLine(plot.x, plot.y + plot.height, plot.x, plot.y);
            {
                wxPoint tri[3] = {wxPoint(plot.x, plot.y - arrow_len), wxPoint(plot.x - arrow_half, plot.y),
                                  wxPoint(plot.x + arrow_half, plot.y)};
                pdc.DrawPolygon(3, tri);
            }
            pdc.DrawLine(plot.x, plot.y + plot.height, plot.x + plot.width, plot.y + plot.height);
            {
                wxPoint tri[3] = {wxPoint(plot.x + plot.width + arrow_len, plot.y + plot.height),
                                  wxPoint(plot.x + plot.width, plot.y + plot.height - arrow_half),
                                  wxPoint(plot.x + plot.width, plot.y + plot.height + arrow_half)};
                pdc.DrawPolygon(3, tri);
            }

            wxFont label_font = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
            label_font.SetPointSize(std::max(7, label_font.GetPointSize() - 1));
            pdc.SetFont(label_font);
            pdc.SetTextForeground(label_muted);
            pdc.DrawText(_L("Material Ratio"), plot.x + FromDIP(4), plot.y - pdc.GetTextExtent(_L("Material Ratio")).GetHeight());
            const wxString height_title = _L("Model Height");
            pdc.DrawText(height_title, plot.x + plot.width - pdc.GetTextExtent(height_title).GetWidth(), plot.y + plot.height + FromDIP(2));

            if (spec.gradient_samples.size() >= 2 && n >= 2) {
                auto curve_point = [&](double t, double ratio) {
                    return wxPoint(plot.x + int(std::lround(t * plot.width)), plot.y + int(std::lround((1.0 - ratio) * plot.height)));
                };
                // First component's ratio solid, its mirror dashed-free twin for the other.
                const wxColour& col_a = spec.component_colours[0];
                const wxColour& col_b = spec.component_colours[1];
                std::vector<wxPoint> pts_a, pts_b;
                pts_a.reserve(spec.gradient_samples.size());
                pts_b.reserve(spec.gradient_samples.size());
                for (const auto& [t, r] : spec.gradient_samples) {
                    pts_a.push_back(curve_point(t, r));
                    pts_b.push_back(curve_point(t, 1.0 - r));
                }
                pdc.SetPen(wxPen(col_b, 2));
                for (size_t i = 0; i + 1 < pts_b.size(); ++i)
                    pdc.DrawLine(pts_b[i], pts_b[i + 1]);
                pdc.SetPen(wxPen(col_a, 2));
                for (size_t i = 0; i + 1 < pts_a.size(); ++i)
                    pdc.DrawLine(pts_a[i], pts_a[i + 1]);

                // Control-point anchors of the stored curve on the first component's line.
                pdc.SetBrush(wxBrush(point_fill));
                pdc.SetPen(wxPen(col_a, 1));
                for (const auto& [t, r] : spec.gradient_anchors) {
                    const wxPoint c = curve_point(t, r);
                    pdc.DrawCircle(c, FromDIP(3));
                }
            }
        }
    });

    // Fixed DIP size, left-aligned: the visualization keeps its proportions no matter how the
    // dialog is resized (the paint handler draws into whatever client rect the panel ends up
    // with, so nothing else has to change).
    const int viz_h = spec.is_gradient ? 150 : (spec.tri_weights.size() == 3 ? 180 : 30);
    const wxSize viz_sz(FromDIP(240), FromDIP(viz_h));
    viz->SetMinSize(viz_sz);
    viz->SetMaxSize(viz_sz);
    // Parented to the page right above the scroll area, so it is always shown with the tab:
    // the "Enable" toggle keeps gating only the rows/info below, never this preview.
    wxSizer* page_sizer = category.page->GetSizer();
    int scroll_idx      = -1;
    for (size_t i = 0; i < page_sizer->GetChildren().size(); ++i)
        if (page_sizer->GetChildren()[i]->GetWindow() == category.scroll) {
            scroll_idx = int(i);
            break;
        }
    if (scroll_idx < 0)
        page_sizer->Add(viz, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10)); // defensive: no scroll item
    else
        page_sizer->Insert(scroll_idx, viz, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
}

void PublishSettingsDialog::set_row_bold(Row& row, bool bold)
{
    // Rebase on the dialog's body font so clearing bold restores the exact original font.
    row.check->SetFont(bold ? Label::Body_13.Bold() : Label::Body_13);
}

void PublishSettingsDialog::save_scroll_position(Category& category)
{
    if (category.scroll != nullptr)
        category.scroll->GetViewStart(&category.scroll_pos.x, &category.scroll_pos.y);
}

void PublishSettingsDialog::show_outer_page(size_t section_index)
{
    if (section_index >= m_sections.size())
        return;
    if (m_selected_outer >= 0 && m_selected_outer < static_cast<int>(m_sections.size())) {
        SectionGroup& old_section = m_sections[m_selected_outer];
        if (old_section.selected_inner >= 0 && old_section.selected_inner < static_cast<int>(old_section.categories.size()))
            save_scroll_position(m_categories[old_section.categories[old_section.selected_inner]]);
        if (old_section.selected_mixed >= 0 && old_section.selected_mixed < static_cast<int>(old_section.mixed_categories.size()))
            save_scroll_position(m_categories[old_section.mixed_categories[old_section.selected_mixed]]);
        m_sections[m_selected_outer].page->Hide();
    }
    m_selected_outer      = static_cast<int>(section_index);
    SectionGroup& section = m_sections[section_index];
    section.page->Show();
    // Restore whichever tab row (physical or mixed) was active.
    if (section.selected_inner >= 0)
        show_inner_page(section_index, section.selected_inner);
    else if (section.selected_mixed >= 0)
        show_mixed_page(section_index, section.selected_mixed);
    m_outer_host_sizer->Layout();
}

void PublishSettingsDialog::show_inner_page(size_t section_index, int inner_index)
{
    if (section_index >= m_sections.size())
        return;
    SectionGroup& section = m_sections[section_index];
    if (inner_index < 0 || inner_index >= static_cast<int>(section.categories.size()))
        return;
    if (section.selected_inner >= 0 && section.selected_inner < static_cast<int>(section.categories.size())) {
        save_scroll_position(m_categories[section.categories[section.selected_inner]]);
        m_categories[section.categories[section.selected_inner]].page->Hide();
    }
    if (section.selected_mixed >= 0 && section.selected_mixed < static_cast<int>(section.mixed_categories.size())) {
        save_scroll_position(m_categories[section.mixed_categories[section.selected_mixed]]);
        m_categories[section.mixed_categories[section.selected_mixed]].page->Hide();
        section.selected_mixed = -1;
    }
    section.selected_inner = inner_index;
    Category& category     = m_categories[section.categories[inner_index]];
    category.page->Show();
    category.scroll->FitInside();
    category.scroll->Scroll(category.scroll_pos.x, category.scroll_pos.y);
    if (section.mixed_tabs != nullptr)
        section.mixed_tabs->Unselect();
    section.page_host_sizer->Layout();
}

void PublishSettingsDialog::show_mixed_page(size_t section_index, int mixed_index)
{
    if (section_index >= m_sections.size())
        return;
    SectionGroup& section = m_sections[section_index];
    if (mixed_index < 0 || mixed_index >= static_cast<int>(section.mixed_categories.size()))
        return;
    if (section.selected_mixed >= 0 && section.selected_mixed < static_cast<int>(section.mixed_categories.size())) {
        save_scroll_position(m_categories[section.mixed_categories[section.selected_mixed]]);
        m_categories[section.mixed_categories[section.selected_mixed]].page->Hide();
    }
    if (section.selected_inner >= 0 && section.selected_inner < static_cast<int>(section.categories.size())) {
        save_scroll_position(m_categories[section.categories[section.selected_inner]]);
        m_categories[section.categories[section.selected_inner]].page->Hide();
        section.selected_inner = -1;
    }
    section.selected_mixed = mixed_index;
    Category& category     = m_categories[section.mixed_categories[mixed_index]];
    category.page->Show();
    category.scroll->FitInside();
    category.scroll->Scroll(category.scroll_pos.x, category.scroll_pos.y);
    section.tabs->Unselect();
    section.page_host_sizer->Layout();
}

void PublishSettingsDialog::on_outer_tab_changed(wxCommandEvent& event)
{
    const int selection = event.GetInt();
    if (selection >= 0 && selection < static_cast<int>(m_sections.size()))
        show_outer_page(static_cast<size_t>(selection));
}

void PublishSettingsDialog::on_inner_tab_changed(size_t section_index, wxCommandEvent& event)
{
    const int selection = event.GetInt();
    if (section_index < m_sections.size() && selection >= 0 && selection < static_cast<int>(m_sections[section_index].categories.size()))
        show_inner_page(section_index, selection);
}

void PublishSettingsDialog::on_mixed_tab_changed(size_t section_index, wxCommandEvent& event)
{
    const int selection = event.GetInt();
    if (section_index < m_sections.size() && selection >= 0 &&
        selection < static_cast<int>(m_sections[section_index].mixed_categories.size()))
        show_mixed_page(section_index, selection);
}

void PublishSettingsDialog::bind_tab_events()
{
    m_outer_tabs->Bind(wxEVT_TAB_SEL_CHANGED, &PublishSettingsDialog::on_outer_tab_changed, this);
    for (size_t section_index = 0; section_index < m_sections.size(); ++section_index) {
        m_sections[section_index].tabs->Bind(wxEVT_TAB_SEL_CHANGED,
                                             [this, section_index](wxCommandEvent& event) { on_inner_tab_changed(section_index, event); });
        if (m_sections[section_index].mixed_tabs != nullptr)
            m_sections[section_index].mixed_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this, section_index](wxCommandEvent& event) {
                on_mixed_tab_changed(section_index, event);
            });
    }
}

void PublishSettingsDialog::apply_filter(const wxString& filter_text)
{
    m_filter_mode = FilterMode::Text;
    refresh_filter(filter_text.Lower());
}

void PublishSettingsDialog::apply_pseudo_filter(bool selected_only)
{
    m_filter_mode = selected_only ? FilterMode::SelectedOnly : FilterMode::UnselectedOnly;
    refresh_filter(wxString()); // the pseudo modes ignore the search text
}

void PublishSettingsDialog::refresh_filter(const wxString& filter)
{
    Freeze();
    const bool pseudo       = m_filter_mode != FilterMode::Text;
    const bool want_checked = m_filter_mode == FilterMode::SelectedOnly;
    m_fb_sizer->Show(!pseudo);
    if (pseudo) {
        // "×" marks the chip as a dismissible filter state (U+00D7, present in all UI fonts).
        m_pseudo_chip->SetLabel((want_checked ? _L("Filter selected") : _L("Filter non-selected")) + " " + wxString::FromUTF8("\u00d7"));
    }
    m_pseudo_chip->Show(pseudo);

    // Row matches are computed first; page and optgroup visibility is applied below.
    if (pseudo) {
        for (Row& row : m_rows)
            row.matches_filter = row.check->IsEnabled() && row.check->GetValue() == want_checked;
    } else {
        const bool clear = filter.IsEmpty();
        for (Row& row : m_rows) {
            row.matches_filter = clear || (row.section_title + " " + row.category + " " + row.subcategory + " " + row.label + " " +
                                           row.value + " " + row.unit)
                                              .Lower()
                                              .Contains(filter);
        }
    }

    size_t first_outer    = 0;
    int first_inner       = -1;
    bool active_has_match = false;
    for (size_t s = 0; s < m_sections.size(); ++s) {
        for (size_t inner = 0; inner < m_sections[s].categories.size(); ++inner) {
            Category& category = m_categories[m_sections[s].categories[inner]];
            bool has_match     = false;
            for (size_t r : category.rows)
                has_match = has_match || m_rows[r].matches_filter;
            category.info->Show(!has_match);
            if (!has_match)
                // A mixed slot has no selectable rows: always explain it is published whole.
                category.info->SetLabel(category.is_mixed ? m_info_mix :
                                                            (pseudo ? (want_checked ? m_info_nonsel : m_info_allsel) : m_info_empty));
            if (has_match && first_inner < 0) {
                first_outer = s;
                first_inner = static_cast<int>(inner);
            }
            if (static_cast<int>(s) == m_selected_outer && static_cast<int>(inner) == m_sections[s].selected_inner)
                active_has_match = has_match;
        }
        // Mixed slots have no rows: they can never match a filter, so they always fall back to
        // their explanatory hint (shown once the slot is enabled).
        for (size_t mixed : m_sections[s].mixed_categories) {
            Category& category = m_categories[mixed];
            category.info->Show(true);
            category.info->SetLabel(m_info_mix);
        }
    }
    if (!active_has_match && first_inner >= 0 &&
        (m_selected_outer != static_cast<int>(first_outer) || m_sections[first_outer].selected_inner != first_inner)) {
        if (m_selected_outer != static_cast<int>(first_outer)) {
            m_outer_tabs->SelectItem(static_cast<int>(first_outer));
            show_outer_page(first_outer);
        }
        m_sections[first_outer].tabs->SelectItem(first_inner);
        show_inner_page(first_outer, first_inner);
    }

    apply_visibility();
    for (Category& category : m_categories) {
        save_scroll_position(category);
        category.scroll->FitInside();
        category.list_sizer->Layout();
        category.scroll->Scroll(category.scroll_pos.x, category.scroll_pos.y);
    }
    Layout();
    Thaw();
}

void PublishSettingsDialog::apply_visibility()
{
    Freeze();
    for (Category& category : m_categories) {
        // A disabled material slot hides everything below its header (rows, info and the Full
        // Publish line); enable it first to reveal its settings.
        const bool enabled = category.section != Section::Material ||
                             (category.enable_check != nullptr && category.enable_check->GetValue());
        if (category.scroll != nullptr)
            category.scroll->Show(enabled);
        if (category.full_line_item != nullptr)
            category.full_line_item->Show(enabled);
        bool category_any = false;
        for (size_t r : category.rows)
            category_any = category_any || m_rows[r].matches_filter;
        category.info->Show(enabled && !category_any);
        for (Subcategory& sub : category.subs) {
            bool sub_any = false;
            for (size_t r : sub.rows)
                sub_any = sub_any || m_rows[r].matches_filter;
            if (sub.header != nullptr)
                sub.item->Show(enabled && sub_any);
            for (size_t r : sub.rows)
                m_rows[r].item->Show(enabled && m_rows[r].matches_filter);
        }
        category.list_sizer->Layout();
        category.scroll->FitInside();
    }
    Thaw();
}

void PublishSettingsDialog::select_all(bool value)
{
    // "All" skips disabled (gated) rows; "None" leaves a gated row's preserved value.
    for (Row& row : m_rows)
        if (row.check->IsEnabled())
            row.check->SetValue(value);
    // "All" also enables every material slot (so its rows/Full Publish become visible and the
    // selection is actually exported); "None" disables them all again.
    for (Category& cat : m_categories)
        if (cat.section == Section::Material && cat.enable_check != nullptr)
            cat.enable_check->SetValue(value);
    // wxCheckBox::SetValue does not emit wxEVT_CHECKBOX, so re-run the enable handlers to
    // propagate mixed-slot components and refresh visibility as if the user had clicked.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].section == Section::Material)
            on_enable_toggle(c);
    apply_visibility();
}

bool PublishSettingsDialog::row_is_visible(const Row& row) const
{
    if (m_selected_outer < 0 || m_selected_outer >= static_cast<int>(m_sections.size()) ||
        row.outer_index != static_cast<size_t>(m_selected_outer) || m_sections[m_selected_outer].selected_inner < 0 ||
        m_sections[m_selected_outer].selected_inner >= static_cast<int>(m_sections[m_selected_outer].categories.size()) ||
        row.inner_index != m_sections[m_selected_outer].categories[m_sections[m_selected_outer].selected_inner] || !row.matches_filter ||
        !row.check->IsEnabled())
        return false;
    return row.item->IsShown();
}

void PublishSettingsDialog::select_visible(bool value)
{
    // In a pseudo-filter view the rows being toggled would all disappear; drop the filter
    // afterwards so the result stays visible.
    const bool clear_pseudo = (m_filter_mode == FilterMode::UnselectedOnly && !value) ||
                              (m_filter_mode == FilterMode::SelectedOnly && value);

    // Toggle the rows visible under the *current* filter.
    for (Row& row : m_rows)
        if (row_is_visible(row))
            row.check->SetValue(value);

    if (clear_pseudo) {
        // Note: SetValue() may fire wxEVT_TEXT on some platforms, re-entering apply_filter() -
        // that is fine; the rows above were already toggled and the trailing call is idempotent.
        m_filter_ctrl->ChangeValue("");
        apply_filter(""); // resync visibility and the All/None bar
    }
}

void PublishSettingsDialog::show_menu(wxMouseEvent& evt)
{
    bool filtering  = !m_filter_ctrl->GetValue().IsEmpty() || m_filter_mode != FilterMode::Text;
    bool list_empty = true;
    if (m_selected_outer >= 0) {
        for (const Row& row : m_rows)
            if (row_is_visible(row)) {
                list_empty = false;
                break;
            }
    }

    wxMenu m;
    m.Append(kPublishSelectAll, _L("Select All"))->Enable(!filtering);
    m.Append(kPublishDeselectAll, _L("Deselect All"))->Enable(!filtering);
    m.AppendSeparator();
    m.Append(kPublishSelectVisible, _L("Select visible"))->Enable(!list_empty && filtering);
    m.Append(kPublishDeselectVisible, _L("Deselect visible"))->Enable(!list_empty && filtering);
    m.AppendSeparator();
    m.AppendCheckItem(kPublishFilterSelected, _L("Filter selected"));
    m.AppendCheckItem(kPublishFilterNonSelected, _L("Filter non-selected"));
    m.Check(kPublishFilterSelected, m_filter_mode == FilterMode::SelectedOnly);
    m.Check(kPublishFilterNonSelected, m_filter_mode == FilterMode::UnselectedOnly);

    m.Bind(
        wxEVT_MENU,
        [this](wxCommandEvent& e) {
            switch (e.GetId()) {
            case kPublishSelectAll: select_all(true); break;
            case kPublishDeselectAll: select_all(false); break;
            case kPublishSelectVisible: select_visible(true); break;
            case kPublishDeselectVisible: select_visible(false); break;
            case kPublishFilterSelected:
                // Clicking the active entry again clears the pseudo filter.
                if (m_filter_mode == FilterMode::SelectedOnly)
                    apply_filter(m_filter_ctrl->GetValue());
                else
                    apply_pseudo_filter(true);
                break;
            case kPublishFilterNonSelected:
                if (m_filter_mode == FilterMode::UnselectedOnly)
                    apply_filter(m_filter_ctrl->GetValue());
                else
                    apply_pseudo_filter(false);
                break;
            default: break;
            }
        },
        kPublishSelectAll, kPublishFilterNonSelected);

    wxWindow* src = dynamic_cast<wxWindow*>(evt.GetEventObject());
    if (!src)
        return;
    wxPoint screen_pos = src->ClientToScreen(evt.GetPosition());
    wxPoint local_pos  = ScreenToClient(screen_pos);
    PopupMenu(&m, local_pos);
}

std::vector<std::string> PublishSettingsDialog::GetPublishedKeys() const
{
    std::vector<std::string> out;
    // Process and printer sections both travel through published_keys (the load-side overlay
    // applies process keys to the prints edited preset and the allowlisted printer keys to the
    // printers edited preset); material keys use a separate API. Printer rows carry the full
    // per-extruder "#N" opt_id (built in Phase 1), so a checked row publishes exactly that
    // extruder's value - per-extruder selection is independent.
    for (const Row& row : m_rows) {
        if ((row.section != Section::Print && row.section != Section::Printer) || !row.check->GetValue())
            continue;
        out.push_back(row.key);
    }
    return out;
}

std::vector<MixedDependencyIssue> PublishSettingsDialog::unpublished_mixed_components() const
{
    std::vector<MixedDependencyIssue> out;
    for (const Category& cat : m_categories) {
        if (!cat.is_mixed || cat.section != Section::Material)
            continue;
        // Only enabled mixed slots depend on their components being published.
        if (cat.enable_check == nullptr || !cat.enable_check->GetValue())
            continue;
        const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
        // Components are 1-based physical filament indices.
        for (const unsigned int component : mixed_slot_components(full, cat.filament_slot)) {
            const size_t component_slot = size_t(component) - 1;
            // Find the component slot's material category (one exists per physical slot).
            const Category* comp_cat = nullptr;
            for (const Category& other : m_categories) {
                if (other.section == Section::Material && !other.is_mixed && other.filament_slot == component_slot) {
                    comp_cat = &other;
                    break;
                }
            }
            if (comp_cat == nullptr || comp_cat->enable_check == nullptr)
                continue;
            // Missing when the component's "Enable" is off, or it is enabled with neither
            // "Full Publish" nor the "Type" requirement row checked. Colour never counts:
            // the receiver renders the mix from its own components' colours.
            if (!comp_cat->enable_check->GetValue()) {
                out.push_back({cat.filament_slot, component_slot, MixedDependencyIssue::Reason::Disabled});
                continue;
            }
            if (comp_cat->full_check != nullptr && comp_cat->full_check->GetValue())
                continue;
            bool type_checked = false;
            for (const size_t r : comp_cat->rows) {
                const Row& row = m_rows[r];
                if (row.kind == RowKind::Type && row.check->GetValue()) {
                    type_checked = true;
                    break;
                }
            }
            if (!type_checked)
                out.push_back({cat.filament_slot, component_slot, MixedDependencyIssue::Reason::MaterialNotPublished});
        }
    }
    // Deterministic order for the warning rows: by mixed slot, then component slot. The same
    // (mix, component) pair cannot repeat: each mix's components come from a config list.
    std::sort(out.begin(), out.end(), [](const MixedDependencyIssue& a, const MixedDependencyIssue& b) {
        return std::tie(a.mixed_slot, a.component_slot) < std::tie(b.mixed_slot, b.component_slot);
    });
    return out;
}

std::vector<Slic3r::PublishedMaterialEntry> PublishSettingsDialog::GetPublishedMaterialKeys() const
{
    std::vector<Slic3r::PublishedMaterialEntry> out;
    for (const Category& cat : m_categories) {
        if (cat.section != Section::Material)
            continue;
        // A slot that is not "Enable"d publishes nothing at all.
        if (cat.enable_check != nullptr && !cat.enable_check->GetValue())
            continue;
        Slic3r::PublishedMaterialEntry entry;
        entry.filament_type   = cat.filament_type;
        entry.filament_vendor = cat.filament_vendor;
        entry.filament_id     = cat.filament_id;
        entry.slot            = static_cast<int>(cat.filament_slot);
        // The author's preset id distinguishes exact variants that share filament_id
        // ("Generic PLA" vs "Generic PLA Matte"), so the receiver can match precisely; the
        // preset name is the most direct identity and is matched first on load.
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle != nullptr && cat.filament_slot < bundle->filament_presets.size()) {
            if (const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[cat.filament_slot], false, true)) {
                entry.setting_id  = preset->setting_id;
                entry.preset_name = preset->name;
            }
        }
        // A mixed slot publishes as one unit: its definition (components, ratios, gradient)
        // always travels (its Enable implies this), and the component filaments are enabled +
        // Full Published by on_enable_toggle into their own entries.
        if (cat.is_mixed) {
            Slic3r::PublishedMaterialEntry mixed_entry;
            mixed_entry.filament_type   = cat.filament_type;
            mixed_entry.filament_vendor = cat.filament_vendor;
            mixed_entry.filament_id     = cat.filament_id;
            mixed_entry.slot            = static_cast<int>(cat.filament_slot);
            // The mix's own colour (blended) is a property of the definition, not a
            // requirement row; carry it so the receiver renders the swatch.
            const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
            const std::string mix_color       = filament_color_hex(full_cfg, cat.filament_slot);
            if (!mix_color.empty()) {
                mixed_entry.publish_color = true;
                mixed_entry.color         = mix_color;
            }
            for (const std::string& key : publish_mixed_keys())
                mixed_entry.keys.emplace_back(key);
            out.push_back(std::move(mixed_entry));
            continue;
        }
        // "Full Publish": the whole filament preset is embedded; type and colour are implicitly
        // published, and the per-key rows are disabled / their state ignored.
        if (cat.full_check != nullptr && cat.full_check->GetValue()) {
            entry.full               = true;
            entry.full_keys          = full_keys_for_slot();
            entry.publish_type       = true;
            entry.publish_type_value = normalize_filament_type(cat.filament_type);
            for (size_t r : cat.rows) {
                const Row& row = m_rows[r];
                if (row.kind == RowKind::Color && !row.value.IsEmpty()) {
                    entry.publish_color = true;
                    entry.color         = row.value.ToStdString();
                }
            }
            out.push_back(std::move(entry));
            continue;
        }
        for (size_t r : cat.rows) {
            const Row& row = m_rows[r];
            if (!row.check->GetValue())
                continue;
            if (row.kind == RowKind::Color) {
                entry.publish_color = true;
                entry.color         = row.value.ToStdString();
            } else if (row.kind == RowKind::Type) {
                entry.publish_type       = true;
                entry.publish_type_value = row.value.ToStdString();
            } else {
                entry.keys.push_back(row.key);
            }
        }
        // Nothing checked at all -> nothing to write.
        if (!entry.keys.empty() || entry.publish_type || entry.publish_color)
            out.push_back(std::move(entry));
    }
    return out;
}

std::vector<std::string> PublishSettingsDialog::full_keys_for_slot() const
{
    // The canonical filament preset keys minus the structural keys the published overlay must
    // never touch (inherits, compatibility, *_settings_id, ...), plus filament_colour (not a
    // member of Preset::filament_options). Values travel in the exported config, masked to
    // this slot, and are applied on load onto the receiver's slot.
    const std::set<std::string>& denylist = publish_structural_keys();
    std::vector<std::string> keys;
    for (const std::string& key : Preset::filament_options())
        if (denylist.count(key) == 0)
            keys.emplace_back(key);
    keys.emplace_back("filament_colour");
    return keys;
}

void PublishSettingsDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    // Rescale toolbar bitmaps and icons; collapse chevrons are vector-drawn and repaint.
    m_search.msw_rescale();
    m_menu.msw_rescale();
    m_filter_box->SetIcon(m_search.bmp());
    m_menu_button->SetBitmap(m_menu.bmp());
    m_outer_tabs->Rescale();

    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();

    for (Category& cat : m_categories) {
        if (cat.full_check != nullptr)
            cat.full_check->Refresh();
        if (cat.enable_check != nullptr)
            cat.enable_check->Refresh();
        if (cat.title_label != nullptr)
            cat.title_label->Refresh();
        if (cat.filament_color_chip != nullptr) {
            // Mixed pages have no header chip; this only ever fires for physical slots.
            if (wxBitmap* chip = get_extruder_color_icon(filament_color_hex(full, cat.filament_slot), std::to_string(cat.filament_slot + 1),
                                                         FromDIP(20), FromDIP(20))) {
                cat.filament_color_chip->SetBitmap(*chip);
            }
        }
        cat.scroll->FitInside();
        cat.list_sizer->Layout();
    }

    for (size_t s = 0; s < m_sections.size(); ++s) {
        SectionGroup& section = m_sections[s];
        section.icon_bmp.msw_rescale();
        if (section.icon_bmp.bmp().IsOk())
            m_outer_tabs->SetItemBitmap(s, section.icon_bmp.bmp());
        section.tabs->Rescale();
        if (section.mixed_tabs != nullptr)
            section.mixed_tabs->Rescale();
    }

    // Refresh the per-row Color chips at the new DPI (they carry the slot number too).
    for (Row& row : m_rows) {
        if (row.color_chip != nullptr && !row.value.IsEmpty()) {
            const std::string chip_label = (row.inner_index < m_categories.size()) ?
                                               std::to_string(m_categories[row.inner_index].filament_slot + 1) :
                                               "";
            if (wxBitmap* chip = get_extruder_color_icon(row.value.ToStdString(), chip_label, FromDIP(20), FromDIP(20)))
                row.color_chip->SetBitmap(*chip);
        }
    }

    for (size_t category_index = 0; category_index < m_categories.size(); ++category_index) {
        const Category& category = m_categories[category_index];
        if (category.section != Section::Material)
            continue;
        const SectionGroup& section = m_sections[category.group];
        if (category.is_mixed) {
            // The tab carries the full composition bitmap (mix chip + components + percents);
            // the page header has no chip/title to refresh.
            const wxBitmap tab_bmp = mixed_filament_tab_bitmap(full, category.filament_slot, FromDIP(20));
            if (tab_bmp.IsOk()) {
                const auto iter = std::find(section.mixed_categories.begin(), section.mixed_categories.end(), category_index);
                if (iter != section.mixed_categories.end() && section.mixed_tabs != nullptr)
                    section.mixed_tabs->SetItemBitmap(static_cast<unsigned int>(iter - section.mixed_categories.begin()), tab_bmp);
            }
        } else if (wxBitmap* chip = get_extruder_color_icon(filament_color_hex(full, category.filament_slot),
                                                            std::to_string(category.filament_slot + 1), FromDIP(20), FromDIP(20))) {
            const auto iter = std::find(section.categories.begin(), section.categories.end(), category_index);
            if (iter != section.categories.end())
                m_sections[category.group].tabs->SetItemBitmap(static_cast<unsigned int>(iter - section.categories.begin()), *chip);
        }
    }

    fit_to_content(); // tab buttons' min widths grew with the DPI: re-fit (incl. resize floor)
    Refresh();
}

}} // namespace Slic3r::GUI
