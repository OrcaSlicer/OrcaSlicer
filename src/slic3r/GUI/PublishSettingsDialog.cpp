#include "PublishSettingsDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "Tab.hpp"
#include "ConfigValueFormatter.hpp"
#include "FilamentBitmapUtils.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/StaticLine.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/SwitchButton.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <wx/colour.h>
#include <string>
#include "libslic3r/Config.hpp"
#include "slic3r/GUI/GUI.hpp"
#include <vector>
#include "slic3r/GUI/wxExtensions.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/Widgets/TabCtrl.hpp"
#include "Plater.hpp"
#include <wx/display.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/tglbtn.h>
#include <wx/treebase.h>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toplevel.h>
#include <wx/panel.h>
#include <wx/textctrl.h>
#include <wx/stattext.h>
#include <wx/geometry.h>
#include <wx/utils.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/dcgraph.h>
#include <wx/image.h>
#include <wx/wrapsizer.h>
#include <set>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <tuple>
#include <utility>

namespace Slic3r { namespace GUI {
namespace {

// Orca's bitmap checkbox has the established teal checked state on every platform. Keep the
// label separate so it stays clickable like a native wxCheckBox, while the control itself
// remains accessible by keyboard.
wxStaticText* add_checkbox_label(wxWindow* parent,
                                 wxBoxSizer* sizer,
                                 ::CheckBox* check,
                                 const wxString& label,
                                 const wxString& tooltip,
                                 int label_width = 0)
{
    check->SetToolTip(tooltip);
    sizer->Add(check, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, parent->FromDIP(2));

    auto* text = new wxStaticText(parent, wxID_ANY, label);
    text->SetFont(Label::Body_14);
    text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
    if (label_width > 0) {
        text->SetMinSize(wxSize(label_width, -1));
        text->SetMaxSize(wxSize(label_width, -1));
        text->Wrap(label_width);
    }
    text->SetToolTip(tooltip);
    text->SetCursor(wxCURSOR_HAND);
    const auto toggle = [check]() {
        if (!check->IsEnabled())
            return;
        check->SetValue(!check->GetValue());
        wxCommandEvent event(wxEVT_TOGGLEBUTTON, check->GetId());
        event.SetEventObject(check);
        check->GetEventHandler()->ProcessEvent(event);
    };
    text->Bind(wxEVT_LEFT_DOWN, [toggle](wxMouseEvent& event) {
        if (!event.LeftDClick())
            toggle();
    });
    text->Bind(wxEVT_LEFT_DCLICK, [toggle](wxMouseEvent&) {
        toggle();
    });
    sizer->Add(text, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, parent->FromDIP(5));
    return text;
}

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
        if (preset != nullptr && !preset->name.empty()) {
            // Display the name the sidebar does: the alias (already bare-name + variant-tail
            // stripped for root filament presets).
            const std::string& display = preset->alias.empty() ? get_preset_bare_name(preset->name) : preset->alias;
            return from_u8(material_display_name(display));
        }
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
// therefore empty. The bitmap is opaque and bakes tab_bg (the tab strip's background colour):
// its texts are rasterized with plain GDI on that solid ground, like the sidebar rows and the
// color chips. Text on a transparent bitmap has to go through the graphics-context route, and
// on MSW that degrades into jagged ClearType-fallback glyphs with fringe halos - the same
// reason draw_mixed_gradient_plot renders into an opaque buffer (FilamentBitmapUtils.cpp).
wxBitmap mixed_filament_tab_bitmap(const DynamicPrintConfig& full, size_t slot, int swatch_sz, const wxColour& tab_bg)
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
        has_lead            = chip.IsOk();
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

    // Phase 2 (draw): opaque, filled with the tab strip's background colour. Text goes through
    // the plain DC on every platform (see the function comment for why the alpha route is
    // avoided); chip bitmaps keep their own alpha and composite cleanly onto the fill.
    wxBitmap composite(width, swatch_sz);
    wxMemoryDC memdc;
    memdc.SelectObject(composite);
    memdc.SetBackground(wxBrush(tab_bg));
    memdc.Clear();
    memdc.SetBackgroundMode(wxTRANSPARENT);
    memdc.SetFont(::Label::Body_12);
    memdc.SetTextForeground(StateColor::darkModeColorFor(wxColour("#262E30")));
    int x = 0;
    for (const Piece& p : pieces) {
        if (p.kind == Piece::Swatch) {
            memdc.DrawBitmap(p.bmp, x, 0, true);
            x += swatch_sz;
        } else {
            const wxSize tsz = measure_dc.GetTextExtent(p.text);
            memdc.DrawText(p.text, x + gap / 2, (swatch_sz - tsz.y) / 2);
            x += tsz.x + gap;
        }
    }
    memdc.SelectObject(wxNullBitmap);
    return composite;
}

// The filament slots (0-based) the current project actually uses, across every plate: base and
// MMU-painted volume extruders, per-object layer-range and support/wall/infill filament
// overrides, and custom-gcode tool changes. Used mixed slots are preserved as themselves (so an
// in-use mix is publishable) while their physical components are added too - a mix always ships
// with the materials of its components even if a component is not referenced directly anywhere.
std::set<size_t> project_used_filament_slots(const PresetBundle& bundle, const DynamicPrintConfig& full, const Model& model)
{
    std::set<size_t> used; // 0-based used slots, mixed slots not yet expanded
    if (model.objects.empty())
        return used;

    auto add_1based = [&used](int id) {
        if (id > 0)
            used.insert(size_t(id - 1));
    };

    const DynamicPrintConfig& print_cfg = bundle.prints.get_edited_preset().config;
    const int glb_support_intf   = print_cfg.opt_int("support_interface_filament");
    const int glb_support        = print_cfg.opt_int("support_filament");
    const int glb_outer_wall     = print_cfg.opt_int("outer_wall_filament_id");
    const int glb_inner_wall     = print_cfg.opt_int("inner_wall_filament_id");
    const int glb_sparse_infill  = print_cfg.opt_int("sparse_infill_filament_id");
    const int glb_internal_solid = print_cfg.opt_int("internal_solid_filament_id");
    const int glb_top_surface    = print_cfg.opt_int("top_surface_filament_id");
    const int glb_bottom_surface = print_cfg.opt_int("bottom_surface_filament_id");
    const bool glb_support_on    = print_cfg.opt_bool("enable_support") || print_cfg.opt_int("raft_layers") > 0;

    for (ModelObject* mo : model.objects) {
        for (ModelVolume* mv : mo->volumes)
            for (int e : mv->get_extruders())
                add_1based(e);

        for (const auto& range_entry : mo->layer_config_ranges)
            if (const ConfigOption* ext_opt = range_entry.second.option("extruder"))
                add_1based(ext_opt->getInt());

        bool obj_support = glb_support_on;
        if (const ConfigOption* s_opt = mo->config.option("enable_support"); s_opt != nullptr)
            obj_support = s_opt->getBool();
        else if (const ConfigOption* r_opt = mo->config.option("raft_layers"); r_opt != nullptr)
            obj_support = r_opt->getInt() > 0;

        if (obj_support) {
            if (const ConfigOption* opt = mo->config.option("support_interface_filament"); opt != nullptr && opt->getInt() != 0)
                add_1based(opt->getInt());
            else
                add_1based(glb_support_intf);

            if (const ConfigOption* opt = mo->config.option("support_filament"); opt != nullptr && opt->getInt() != 0)
                add_1based(opt->getInt());
            else
                add_1based(glb_support);
        }

        auto obj_id = [&mo](const char* key) {
            const ConfigOption* opt = mo->config.option(key);
            return (opt != nullptr) ? opt->getInt() : 0;
        };

        int obj_outer_wall = obj_id("outer_wall_filament_id");
        if (obj_outer_wall == 0)
            obj_outer_wall = obj_id("inner_wall_filament_id");
        add_1based(obj_outer_wall != 0 ? obj_outer_wall : glb_outer_wall);

        int obj_inner_wall = obj_id("inner_wall_filament_id");
        if (obj_inner_wall == 0)
            obj_inner_wall = obj_id("outer_wall_filament_id");
        add_1based(obj_inner_wall != 0 ? obj_inner_wall : glb_inner_wall);

        const int obj_sparse = obj_id("sparse_infill_filament_id");
        add_1based(obj_sparse != 0 ? obj_sparse : glb_sparse_infill);

        const int obj_internal_solid = obj_id("internal_solid_filament_id");
        add_1based(obj_internal_solid != 0 ? obj_internal_solid : glb_internal_solid);

        int obj_top = obj_id("top_surface_filament_id");
        if (obj_top == 0)
            obj_top = obj_internal_solid;
        add_1based(obj_top != 0 ? obj_top : glb_top_surface);

        int obj_bottom = obj_id("bottom_surface_filament_id");
        if (obj_bottom == 0)
            obj_bottom = obj_internal_solid;
        add_1based(obj_bottom != 0 ? obj_bottom : glb_bottom_surface);
    }

    for (const auto& plate_entry : model.plates_custom_gcodes)
        for (const CustomGCode::Item& item : plate_entry.second.gcodes)
            if (item.type == CustomGCode::Type::ToolChange)
                add_1based(item.extruder);

    // Expand used mixed slots to their physical components and keep each mixed slot itself so the
    // dialog shows the mix. A component both used directly and pulled in via a mix is deduped.
    const auto* is_mixed_opt = full.opt<ConfigOptionBools>("filament_is_mixed");
    const auto* comp_opt     = full.opt<ConfigOptionStrings>("filament_mixed_components");
    if (is_mixed_opt != nullptr && comp_opt != nullptr && has_any_mixed_filament(is_mixed_opt->values)) {
        const std::vector<unsigned int> raw(used.begin(), used.end());
        const std::vector<unsigned int> expanded = expand_mixed_filaments(raw, is_mixed_opt->values, comp_opt->values);
        std::set<size_t> result(expanded.begin(), expanded.end());
        for (unsigned int s : raw)
            if (s < is_mixed_opt->values.size() && is_mixed_opt->values[s])
                result.insert(s);
        return result;
    }

    return used;
}

// Lays a translated sentence out along `row`, replacing each "%1%"-style placeholder with the
// matching window from `chips`. Keeping the sentence in one msgid lets a translation put the
// placeholders wherever its own grammar needs them; spacing comes from the translation itself.
void add_sentence_with_chips(wxWindow* parent, wxSizer* row, const wxString& sentence, const std::vector<wxWindow*>& chips)
{
    std::vector<bool> placed(chips.size(), false);
    auto add_text = [&](wxString text) {
        text.Replace("%%", "%"); // the sentence is a format string
        if (text.IsEmpty())
            return;
        auto* label = new wxStaticText(parent, wxID_ANY, text);
        label->SetFont(Label::Body_12);
        label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
        row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
    };
    auto add_chip = [&](size_t i) {
        if (i < chips.size() && chips[i] != nullptr && !placed[i]) {
            placed[i] = true;
            row->Add(chips[i], 0, wxALIGN_CENTER_VERTICAL);
        }
    };

    size_t literal = 0, pos = 0;
    while ((pos = sentence.find('%', pos)) != wxString::npos) {
        size_t end = pos + 1;
        while (end < sentence.length() && sentence[end] >= '0' && sentence[end] <= '9')
            ++end;
        if (end == pos + 1 || end >= sentence.length() || sentence[end] != '%') {
            ++pos; // a bare '%'
            continue;
        }
        long index = 0;
        sentence.Mid(pos + 1, end - pos - 1).ToLong(&index);
        add_text(sentence.Mid(literal, pos - literal));
        add_chip(size_t(index - 1));
        literal = pos = end + 1;
    }
    add_text(sentence.Mid(literal));
    for (size_t i = 0; i < chips.size(); ++i) // whatever the translation left out
        add_chip(i);
}

} // namespace

// Warning shown on OK when an enabled mixed-filament slot relies on a filament that would ship
// without its material. One row per unmet dependency, each a single translated sentence whose
// two placeholders are the mixed slot's and the component filament's colour chips. "Cancel" is
// the safe choice and keeps the dialog open; "Publish anyway" continues.
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
        // The slot's colour swatch, numbered like its tab, with the slot name on hover; falls
        // back to a label so the sentence always names both filaments.
        auto make_chip = [&](const wxBitmap& bmp, const wxString& name) -> wxWindow* {
            if (bmp.IsOk()) {
                auto* chip = new wxStaticBitmap(this, wxID_ANY, bmp);
                chip->SetToolTip(name);
                return chip;
            }
            auto* label = new wxStaticText(this, wxID_ANY, name);
            label->SetFont(Label::Body_12);
            return label;
        };

        for (const MixedDependencyIssue& issue : issues) {
            std::string hex = filament_color_hex(full, issue.component_slot);
            if (hex.empty())
                hex = "#D9D9D9";
            const wxBitmap* comp_bmp = get_extruder_color_icon(hex, std::to_string(issue.component_slot + 1), swatch, swatch);

            wxWindow* mix_chip  = make_chip(mixed_filament_chip_bitmap(full, issue.mixed_slot, swatch),
                                            wxString::Format(_L("Filament %d (mixed)"), int(issue.mixed_slot) + 1));
            wxWindow* comp_chip = make_chip(comp_bmp != nullptr ? *comp_bmp : wxNullBitmap,
                                            wxString::Format(_L("Filament %d"), int(issue.component_slot) + 1));

            auto* row = new wxWrapSizer(wxHORIZONTAL);
            add_sentence_with_chips(this, row,
                                    issue.reason == MixedDependencyIssue::Reason::Disabled ?
                                        _L("%1% needs %2%, which is not enabled.") :
                                        _L("%1% needs %2%, whose material will not be published."),
                                    {mix_chip, comp_chip});
            content->Add(row, 0, wxEXPAND | wxLEFT, FromDIP(10));
            content->AddSpacer(FromDIP(6));
        }

        auto* hint = new wxStaticText(
            this, wxID_ANY,
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
        constexpr int kSamples            = 256;
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

PublishSettingsDialog::PublishSettingsDialog(wxWindow* parent,
                                             const std::vector<std::string>* published_keys,
                                             const std::vector<Slic3r::PublishedMaterialEntry>* material_keys,
                                             bool legacy_process_selection)
    : DPIDialog(parent ? parent : static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Publish 3MF..."),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER | wxFULL_REPAINT_ON_RESIZE)
    , m_search(this, "search", 16)
    , m_menu(this, "filter", 16)
    , m_legacy_process_selection(legacy_process_selection)
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
#ifdef __WINDOWS__
    m_outer_host->SetDoubleBuffered(true);
#endif
    m_outer_host->SetBackgroundColour(GetBackgroundColour());
    m_outer_host_sizer = new wxBoxSizer(wxVERTICAL);
    m_outer_host->SetSizer(m_outer_host_sizer);

    wxBoxSizer* w_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* msg = new wxStaticText(this, wxID_ANY, _L("Select which settings to be published in the 3MF file"));
    msg->SetFont(Label::Body_13);
    msg->Wrap(-1);
    w_sizer->Add(msg, 0, wxRIGHT | wxLEFT | wxTOP, FromDIP(10));

    w_sizer->Add(f_bar, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_outer_tabs, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(m_outer_host, 1, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));

    build_option_model();

    // Seed from a remembered session selection or a freshly loaded published 3MF (authoritative).
    if (published_keys != nullptr || material_keys != nullptr)
        apply_selection(published_keys != nullptr ? *published_keys : std::vector<std::string>(),
                        material_keys != nullptr ? *material_keys : std::vector<Slic3r::PublishedMaterialEntry>());

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

    // Guide links, bottom-left, sharing the footer row with the OK/Cancel buttons (pushed right).
    wxBoxSizer* links_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto wiki_btn = new Button(this, "", "toolbar_wiki", 0, 15);
    auto wiki_url = "https://www.orcaslicer.com/wiki/publishing_3mf/publish_3mf";
    wiki_btn->SetToolTip(_L("Wiki Guide") + "\n" + wiki_url);
    wiki_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    wiki_btn->SetCanFocus(false);
    wiki_btn->Bind(wxEVT_LEFT_DOWN, ([wiki_url](auto& e) {wxLaunchDefaultBrowser(wiki_url);}));

    auto video_btn  = new Button(this, "", "toolbar_video_guide", 0, 15);
    auto video_url = "https://www.youtube.com/watch?v=-xt1N29UIOg";
    video_btn->SetToolTip(_L("Video Guide") + "\n" + video_url);
    video_btn->SetStyle(ButtonStyle::Confirm, ButtonType::Circle);
    video_btn->SetCanFocus(false);
    video_btn->Bind(wxEVT_LEFT_DOWN, ([video_url](auto& e) {wxLaunchDefaultBrowser(video_url);}));

    links_sizer->Add(wiki_btn , 0, wxLEFT, FromDIP(10));
    links_sizer->Add(video_btn, 0, wxLEFT, FromDIP(10));

    wxBoxSizer* footer = new wxBoxSizer(wxHORIZONTAL);
    footer->Add(links_sizer, 0, wxALIGN_CENTER_VERTICAL);
    footer->AddStretchSpacer();
    footer->Add(dlg_btns, 0, wxALIGN_CENTER_VERTICAL);
    auto* footer_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
    footer_line->SetBackgroundColour(wxColour("#CECECE"));
    footer_line->SetMinSize(wxSize(-1, 1));
    footer_line->SetMaxSize(wxSize(-1, 1));
    w_sizer->Add(footer_line, 0, wxRIGHT | wxLEFT | wxTOP | wxEXPAND, FromDIP(10));
    w_sizer->Add(footer, 0, wxEXPAND, FromDIP(10));

    SetSizerAndFit(w_sizer);
    fit_to_content(); // initial size only; the dialog is resizable
    wxGetApp().UpdateDlgDarkUI(this);
    // The dark-UI walk re-colours the tab strips after the compositions were baked; rebuild so
    // they carry the theme-resolved background (the sidebar resolves its colours at paint time,
    // this dialog bakes, so the bake has to happen after the walk).
    refresh_mixed_tab_bitmaps();
}

// Size the window to its content: width follows the widest tab strip so no filament tab is
// hidden (TabCtrl::relayout hides overflowing buttons), height scales proportionally. Both
// are floored at the 530x530 base and capped at hard DIP limits - deliberately not the whole
// display - with one last-resort clamp so the dialog can never open larger than the screen.
// Also owns the resize floor: the window cannot be resized below what the tabs need, so
// shrinking never re-hides a filament tab.
void PublishSettingsDialog::fit_to_content()
{
    static const wxSize BASE{530, 530}; // base size in DIP, the minimum the dialog can shrink to
    static const wxSize CAP{1300, 850}; // hard cap in DIP, the maximum the dialog can grow to

    int strip = m_outer_tabs->GetFullSize();
    for (const SectionGroup& section : m_sections) {
        strip = std::max(strip, (section.page_tabs != nullptr ? section.page_tabs : section.tabs)->GetFullSize());
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
    // Per-extruder rows (printer and process) index the uncompacted variant columns (as the Tabs do);
    // full_config() compacts them per extruder.
    const DynamicPrintConfig uncompacted = bundle->full_config(false);

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
    auto option_text = [&denylist](const DynamicPrintConfig& cfg, const std::string& opt_id, const std::string& pure_key, wxString& label,
                                   wxString& value, wxString& unit) -> bool {
        if (denylist.count(pure_key) > 0)
            return false;
        const ConfigOptionDef* def = print_config_def.get(pure_key);
        if (def == nullptr)
            return false;
        label = _(def->full_label.empty() ? def->label : def->full_label);
        if (label.IsEmpty())
            return false;
        value = get_string_value(opt_id, cfg);
        unit  = _(def->sidetext);
        if (unit == "%" && value.EndsWith("%"))
            unit.clear();
        return true;
    };

    // A scalar always resolves; a vector only when the index addresses one of its elements, so a
    // short vector hides its row instead of clamping to the first value. An index of -1 means
    // "the option's single entry", which the group resolves the same way the Tab does.
    auto index_in_range = [](const DynamicPrintConfig& cfg, const std::string& key, int index) {
        const ConfigOption* opt = cfg.option(key);
        if (opt == nullptr)
            return false;
        if (index < 0)
            return true;
        const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(opt);
        return values == nullptr || size_t(index) < values->size();
    };

    m_snapshot = uncompacted;

    // The dialog's own requirement rows: Color and Type on a material slot. They publish
    // publish_color / publish_type, not a key.
    auto make_requirement_row = [](std::string key, int slot, Slic3r::PublishRowKind kind, const wxString& label, const wxString& value) {
        Row row;
        row.id.keys.emplace_back(key);
        row.id.owner   = slot;
        row.id.column  = -1;
        row.id.section = Slic3r::PublishSection::Material;
        row.id.kind    = kind;
        RowOption option;
        option.key   = std::move(key);
        option.index = slot;
        if (const ConfigOptionDef* def = print_config_def.get(option.key))
            option.def = *def;
        row.options.push_back(std::move(option));
        row.label    = label;
        row.label_en = label;
        row.value    = value;
        return row;
    };

    // One option of a source Line, remapped to the column this dialog shows it at.
    struct MappedOption
    {
        std::string publish_key;
        RowOption   option;
    };

    // One row from one source Line: every option of the line is one publish unit with one toggle,
    // carrying the line's own label, tooltip and wiki path.
    auto make_line_row = [&option_text](const Line& src, const std::vector<MappedOption>& mapped, int owner, int column,
                                        Slic3r::PublishSection section,
                                        const DynamicPrintConfig& cfg) -> std::optional<Row> {
        if (mapped.empty())
            return std::nullopt;
        Row row;
        row.label         = src.label;
        row.label_en      = src.label_en;
        row.label_tooltip = src.label_tooltip;
        row.label_path    = src.label_path;
        row.id.owner      = owner;
        row.id.column     = column;
        row.id.section    = section;
        row.id.kind       = Slic3r::PublishRowKind::Key;
        for (const MappedOption& m : mapped) {
            row.id.keys.push_back(m.publish_key);
            row.options.push_back(m.option);
        }
        // The first option's value and unit name the row in the filter.
        wxString       label;
        const RowOption& first = row.options.front();
        if (!option_text(cfg, mapped.front().publish_key, first.key, label, row.value, row.unit))
            return std::nullopt;
        return row;
    };

    // --- Phase 1: printer per-extruder retraction settings (first, mirroring the sidebar's
    // Printer group), from the printer tab's "Extruder" page. One inner tab per extruder (named
    // as on the printer tab's switch, e.g. "Left Extruder"/"Right Extruder" via
    // Tab::translate_category), each holding that extruder's Retraction and Z-Hop rows with
    // per-extruder "#N" values.
    {
        size_t g = section_group_for(Section::Printer);
        std::set<std::string> printer_added;
        for (Tab* tab : wxGetApp().tabs_list) {
            // The page's controls edit the extruder chosen on the printer tab's switch, so its
            // option list is read once per extruder.
            auto*       printer_tab = dynamic_cast<TabPrinter*>(tab);
            const Page* page        = printer_tab ? printer_tab->extruder_page() : nullptr;
            if (page == nullptr)
                continue;
            const size_t            extruders_count = printer_tab->m_extruders_count;
            const DynamicPrintConfig& printer_config = bundle->printers.get_edited_preset().config;
            for (size_t extruder_idx = 0; extruder_idx < extruders_count; ++extruder_idx) {
                const wxString page_title = Tab::translate_category(extruders_count > 1 ? wxString::Format("Extruder %d", int(extruder_idx + 1)) : wxString("Extruder"), tab->m_type);
                // Retraction and Z-Hop values are stored per variant column, not per extruder.
                // The column comes from the shared rule, so an extruder whose column this printer
                // preset does not define gets no page instead of another extruder's values.
                const std::vector<Slic3r::PublishExtruderEntry> columns = publish_extruder_rule(
                    printer_config, printer_config, bundle->project_config, extruder_idx, /*split_hybrid=*/false,
                    "printer_extruder_id", "printer_extruder_variant");
                for (const Slic3r::PublishExtruderEntry& column_entry : columns) {
                    const int variant_index = int(column_entry.column);
                    for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                        // Allowlist on the untranslated optgroup title; the "Retraction when
                        // switching material" group is intentionally skipped.
                        if (optgroup->title != "Retraction" && optgroup->title != "Z-Hop")
                            continue;
                        const wxString subcategory = _(optgroup->title);
                        for (const Line& src : optgroup->get_lines()) {
                            if (src.is_separator() || src.get_options().empty())
                                continue;
                            std::vector<MappedOption> mapped;
                            bool                      row_ok = true;
                            for (const Option& option : src.get_options()) {
                                const std::string base = publish_base_key(option.opt_id);
                                // Rows are keyed by the full per-extruder "#N" key so each
                                // extruder page publishes its own value.
                                const std::string key = base + "#" + std::to_string(variant_index);
                                if (!printer_added.insert(key).second || !index_in_range(uncompacted, base, variant_index)) {
                                    row_ok = false;
                                    break;
                                }
                                mapped.push_back({ key, { base, variant_index, option.opt } });
                            }
                            if (!row_ok)
                                continue;
                            size_t cat_index = category_index_for(page_title, Section::Printer, g, extruder_idx, PublishMaterialIdentity(), false, true);
                            m_categories[cat_index].column = variant_index;
                            size_t sub_index = subcategory_index_for(cat_index, subcategory, optgroup->icon, optgroup.get(), page, tab->type());
                            if (auto row = make_line_row(src, mapped, -1, variant_index,
                                                         Slic3r::PublishSection::Printer, uncompacted))
                                add_row(std::move(*row), cat_index, sub_index);
                        }
                    }
                }
            }
        }
    }

    // --- Phase 2: per-material sections built from the filament tab's pages, under the Filament
    // group. Every page the Filament tab shows is a source, so a new Tab setting appears in the
    // dialog without a dialog change (spec 7.4.4).
    {
        size_t g          = section_group_for(Section::Material);
        Tab* filament_tab = nullptr;
        for (Tab* tab : wxGetApp().tabs_list)
            if (tab->m_type == Preset::TYPE_FILAMENT) {
                filament_tab = tab;
                break;
            }
        if (filament_tab != nullptr) {
            // In the Tab's own order; Dependencies and Notes hold no publishable setting.
            std::vector<const Page*> source_pages;
            for (const PageShp& page : filament_tab->m_pages) {
                if (page->title() == "Dependencies" || page->title() == "Notes")
                    continue;
                source_pages.push_back(page.get());
            }

            if (!source_pages.empty()) {
                // Mixed-color slots are virtual: they carry no per-key Material/Retraction
                // settings; their "Enable" toggle always embeds the mix definition (components,
                // ratios, gradient). Detect them via the project-level flag.
                const ConfigOptionBools* is_mixed_opt = full.opt<ConfigOptionBools>("filament_is_mixed");
                // Only the slots the project actually uses: base/MMU-painted volume extruders,
                // support/wall/infill overrides and custom-gcode tool changes, plus the physical
                // components of any in-use mixed slot. Unused slots get no section at all (and are
                // therefore never published). One section per used slot, disambiguated by its
                // colour chip and slot identity while showing the bare name.
                const std::set<size_t> used_slots = project_used_filament_slots(*bundle, full, wxGetApp().plater()->model());
                for (size_t slot = 0; slot < bundle->filament_presets.size(); ++slot) {
                    if (used_slots.find(slot) == used_slots.end())
                        continue;
                    const bool is_mixed = is_mixed_opt != nullptr && slot < is_mixed_opt->size() && is_mixed_opt->values[slot];
                    const PublishMaterialIdentity identity = material_identity(slot, full);
                    // A mixed slot's title is its component composition (e.g. "1 (60%) + 2
                    // (40%)"), not the cloned preset's name shown in the main GUI.
                    const wxString title        = is_mixed ? mixed_filament_label(full, slot) : material_title(slot, bundle, full);
                    const size_t category_index = category_index_for(title, Section::Material, g, slot, identity, is_mixed);

                    // The owner's identity and whole-preset keys travel with every entry.
                    Slic3r::PublishOwnerInfo owner_info;
                    owner_info.mixed            = is_mixed;
                    owner_info.filament_type    = identity.type;
                    owner_info.filament_vendor  = identity.vendor;
                    owner_info.filament_id      = identity.id;
                    owner_info.full_keys        = full_keys_for_slot();
                    owner_info.colour           = filament_color_hex(full, slot);
                    if (const Preset* preset = bundle->filaments.find_preset(bundle->filament_presets[slot], false, true)) {
                        owner_info.setting_id  = preset->setting_id;
                        owner_info.preset_name = preset->name;
                    }
                    m_selection.set_owner_info(int(slot), std::move(owner_info));
                    m_selection.set_owner_enabled(int(slot), false);
                    m_selection.set_full_publish(int(slot), false);

                    if (is_mixed) {
                        // A mixed slot publishes as one unit (its definition); nothing to select
                        // per-key. Its component filaments are auto-enabled + Full Published when
                        // "Enable" is checked (see on_enable_toggle). Its page still shows what
                        // would be published: a ratio bar, or the gradient graph.
                        add_mixed_visual(category_index, make_mixed_visual_spec(full, slot));
                        continue;
                    }

                    // The column this slot prints with, and the file columns its preset defines.
                    const Slic3r::PublishResolvedColumn resolved = publish_resolved_column(uncompacted, slot);
                    const std::vector<size_t>           slot_columns = publish_filament_columns(uncompacted, slot);
                    m_categories[category_index].column  = resolved.file_column.has_value() ? int(*resolved.file_column) : -1;
                    m_categories[category_index].columns = slot_columns;
                    // Q1: a slot whose printer variant its filament does not define publishes the
                    // values it prints with, and the owner is told before they publish.
                    if (const std::string notice = publish_variant_notice(uncompacted, slot); !notice.empty()) {
                        if (wxStaticText* label = m_categories[category_index].variant_notice; label != nullptr) {
                            label->SetLabel(from_u8(notice));
                            label->Wrap(FromDIP(460));
                            label->Show();
                        }
                    }

                    // Material requirement rows: an optional filament colour and/or a
                    // vendor-agnostic material type for this slot, in their own optgroup so they
                    // stay visually separated from the setting rows.
                    {
                        const size_t req_sub = subcategory_index_for(category_index, _L("Material"), "custom-gcode_filament",
                                                                     nullptr, nullptr, filament_tab->type());
                        add_row(make_requirement_row("filament_colour", int(slot), Slic3r::PublishRowKind::Color, _L("Color"),
                                                     from_u8(filament_color_hex(full, slot))),
                                category_index, req_sub);
                        std::string type;
                        if (const auto* types = full.opt<ConfigOptionStrings>("filament_type"))
                            if (slot < types->size())
                                type = types->get_at(slot);
                        add_row(make_requirement_row("filament_type", int(slot), Slic3r::PublishRowKind::Type, _L("Type"),
                                                     from_u8(normalize_filament_type(type))),
                                category_index, req_sub);
                    }

                    // A material section must not repeat a key; the same key may appear in other
                    // material sections - that is intended.
                    std::set<std::string> material_added;
                    // Keys the file always carries whole for every slot. Publishing one per key
                    // would rewrite the receiver's identity instead of gating it, and Color/Type
                    // are already the dialog's own rows.
                    static const std::set<std::string> identity_keys = {
                        "filament_colour", "filament_type", "filament_vendor", "filament_ids",
                        "filament_self_index", "filament_extruder_variant", "filament_diameter",
                    };

                    for (const Page* source_page : source_pages) {
                        for (const ConfigOptionsGroupShp& optgroup : source_page->m_optgroups) {
                            for (const Line& src : optgroup->get_lines()) {
                                if (src.is_separator())
                                    continue;
                                size_t sub_index = subcategory_index_for(category_index, _(optgroup->title), optgroup->icon,
                                                                         optgroup.get(), source_page, filament_tab->type());

                                // One row per source Line, as in Process. A variant-bearing option
                                // is shown once per variant, so the line becomes one row per file
                                // column; a per-slot option is shown once for the owner. A line
                                // that holds both therefore becomes a variant row plus a slot row,
                                // where the Tab draws them together.
                                struct VariantSource
                                {
                                    std::string     base;
                                    ConfigOptionDef def;
                                };
                                std::vector<VariantSource> variant_sources;
                                std::vector<MappedOption>  slot_level;
                                for (const Option& option : src.get_options()) {
                                    const std::string base = publish_base_key(option.opt_id);
                                    if (identity_keys.count(base) != 0 || denylist.count(base) != 0)
                                        continue;
                                    if (!material_added.insert(base).second)
                                        continue;
                                    // Without a variant layout the key stays slot-indexed, as the
                                    // legacy export path masks it.
                                    if (publish_is_variant_bearing(base) && !slot_columns.empty()) {
                                        variant_sources.push_back({ base, option.opt });
                                        continue;
                                    }
                                    if (!index_in_range(uncompacted, base, int(slot)))
                                        continue;
                                    slot_level.push_back({ base, { base, int(slot), option.opt } });
                                }

                                if (!variant_sources.empty()) {
                                    for (const size_t column : slot_columns) {
                                        std::vector<MappedOption> mapped;
                                        bool                      row_ok = true;
                                        for (const VariantSource& source : variant_sources) {
                                            if (!index_in_range(uncompacted, source.base, int(column))) {
                                                row_ok = false;
                                                break;
                                            }
                                            mapped.push_back({ source.base + "#" + std::to_string(column),
                                                               { source.base, int(column), source.def } });
                                        }
                                        if (!row_ok)
                                            continue;
                                        if (auto row = make_line_row(src, mapped, int(slot), int(column),
                                                                     Slic3r::PublishSection::Material, uncompacted))
                                            add_row(std::move(*row), category_index, sub_index);
                                    }
                                }
                                if (!slot_level.empty()) {
                                    if (auto row = make_line_row(src, slot_level, int(slot), -1,
                                                                 Slic3r::PublishSection::Material, uncompacted))
                                        add_row(std::move(*row), category_index, sub_index);
                                }
                            }
                        }
                    }
                    setup_material_variant_switch(category_index);
                }
            }
        }
    }

    // --- Phase 3: process (print) settings; grouping matches the process tab. Per-extruder settings
    // (print_options_with_variant) get one tab per extruder, as the Printer section does, with a row
    // per extruder column ("key#N"); the other settings stay on the page tabs.
    {
        size_t g = section_group_for(Section::Print);

        // One tab per extruder entry of the Process tab's switch (a single "Extruder" tab for one extruder).
        struct ExtruderEntry
        {
            size_t category{0};
            int    variant{-1};
        };
        std::vector<ExtruderEntry> extruder_entries;
        if (auto* print_tab = dynamic_cast<TabPrint*>(wxGetApp().get_tab(Preset::TYPE_PRINT))) {
            const int extruder_count = bundle->get_printer_extruder_count();
            // The process pages follow the sidebar's extruder switch: it is populated only when
            // several extruders run different hardware, and a hybrid extruder then shows one page
            // per sub-nozzle type. Otherwise there is a single "Extruder" page.
            const bool                    split_hybrid   = extruder_count >= 2 && bundle->support_different_extruders();
            const std::vector<wxString>   titles         = split_hybrid ? print_tab->generate_extruder_options() : std::vector<wxString>();
            const DynamicPrintConfig&     print_config   = bundle->prints.get_edited_preset().config;
            const DynamicPrintConfig&     printer_config = bundle->printers.get_edited_preset().config;
            size_t                        title_index    = 0;
            const int                     extruders      = split_hybrid ? extruder_count : 1;
            for (int extruder = 0; extruder < extruders; ++extruder) {
                // Labels pair with the switch's own tags, so the pages read exactly like the Tab.
                for (const Slic3r::PublishExtruderEntry& column : publish_extruder_rule(
                         print_config, printer_config, bundle->project_config, size_t(extruder), split_hybrid,
                         "print_extruder_id", "print_extruder_variant")) {
                    const wxString title = split_hybrid && title_index < titles.size() ?
                                               titles[title_index] :
                                               Tab::translate_category(wxString("Extruder"), Preset::TYPE_PRINT);
                    ++title_index;
                    extruder_entries.push_back({category_index_for(title, Section::Print, g, extruder_entries.size(),
                                                                   PublishMaterialIdentity(), false, split_hybrid),
                                                int(column.column)});
                }
            }
        }

        std::set<std::string> extruder_added; // "key#N" rows already added
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->m_type != Preset::TYPE_PRINT)
                continue;
            size_t page_index = 0;
            for (const PageShp& page : tab->m_pages) {
                const wxString category = Tab::translate_category(page->title(), tab->m_type);
                // Created with the page's first row, so a page holding only per-extruder settings gets no tab.
                size_t category_index = size_t(-1);

                for (const ConfigOptionsGroupShp& optgroup : page->m_optgroups) {
                    // A source Line is one row: its options publish together and share one toggle.
                    for (const Line& src : optgroup->get_lines()) {
                        if (src.is_separator())
                            continue;
                        const std::vector<Option>& src_options = src.get_options();
                        if (src_options.empty())
                            continue;

                        // A line made only of per-variant keys is shown once per extruder column.
                        bool per_variant = !extruder_entries.empty();
                        for (const Option& option : src_options)
                            per_variant = per_variant && print_options_with_variant.count(publish_base_key(option.opt_id)) != 0;

                        if (per_variant) {
                            for (const ExtruderEntry& entry : extruder_entries) {
                                std::vector<MappedOption> mapped;
                                bool                      row_ok = true;
                                for (const Option& option : src_options) {
                                    const std::string base = publish_base_key(option.opt_id);
                                    const std::string key  = base + "#" + std::to_string(entry.variant);
                                    if (!extruder_added.insert(key).second || !index_in_range(uncompacted, base, entry.variant)) {
                                        row_ok = false;
                                        break;
                                    }
                                    mapped.push_back({ key, { base, entry.variant, option.opt } });
                                }
                                if (!row_ok)
                                    continue;
                                size_t sub_index = subcategory_index_for(entry.category, _(optgroup->title), optgroup->icon,
                                                                         optgroup.get(), page.get(), tab->type());
                                if (auto row = make_line_row(src, mapped, -1, entry.variant, Slic3r::PublishSection::Process, uncompacted))
                                    add_row(std::move(*row), entry.category, sub_index);
                            }
                            continue;
                        }

                        // A key may appear in more than one page/group; keep the first.
                        std::vector<MappedOption> mapped;
                        bool                      row_ok = true;
                        for (const Option& option : src_options) {
                            const std::string base  = publish_base_key(option.opt_id);
                            const int         index = publish_key_index(option.opt_id);
                            if (!added.insert(base).second || !index_in_range(uncompacted, base, index)) {
                                row_ok = false;
                                break;
                            }
                            mapped.push_back({ option.opt_id, { base, index, option.opt } });
                        }
                        if (!row_ok)
                            continue;
                        if (category_index == size_t(-1))
                            category_index = category_index_for(category, Section::Print, g, page_index);
                        size_t sub_index = subcategory_index_for(category_index, _(optgroup->title), optgroup->icon,
                                                                 optgroup.get(), page.get(), tab->type());
                        if (auto row = make_line_row(src, mapped, -1, publish_key_index(mapped.front().publish_key),
                                                     Slic3r::PublishSection::Process, uncompacted))
                            add_row(std::move(*row), category_index, sub_index);
                    }
                }
                ++page_index;
            }
        }
    }

    // Pre-check the dirty (modified) settings. Printer and process dirty keys already carry the
    // column of their own preset, which is the file column, so an exact match is right for them.
    // A filament preset's dirty keys carry its *local* variant index, and several slots routinely
    // share one preset, so a material row resolves its own local index and only the slots that
    // use the edited preset are pre-checked (spec 7.5).
    std::set<std::string> dirty_keys;
    for (const std::string& key : collect_dirty_settings_keys(*wxGetApp().preset_bundle))
        dirty_keys.insert(key);
    const std::string edited_filament = bundle->filaments.get_selected_preset_name();
    const auto*       mixed_flags     = full.opt<ConfigOptionBools>("filament_is_mixed");
    auto slot_is_mixed = [mixed_flags](size_t slot) {
        return mixed_flags != nullptr && slot < mixed_flags->size() && mixed_flags->values[slot];
    };
    for (size_t r = 0; r < m_rows.size(); ++r) {
        Row& row = m_rows[r];
        // The Color/Type requirement rows are not "dirty overrides": never auto-checked.
        if (row.id.kind != Slic3r::PublishRowKind::Key)
            continue;
        const std::string& row_key = row.id.keys.front();
        // A row is one publish unit, so any of its keys being modified marks the whole row.
        bool dirty = std::any_of(row.id.keys.begin(), row.id.keys.end(), [&dirty_keys](const std::string& key) {
            return dirty_keys.count(key) != 0;
        });
        // A print/printer row with no column suffix covers the whole setting, so any index of it
        // counts as a match; a per-slot material row must not fall back that way, because the
        // dirty index there is a slot and would check every slot's row.
        if (!dirty && row.id.section != Slic3r::PublishSection::Material && row.id.column < 0 &&
            row_key.find('#') == std::string::npos) {
            const std::string prefix = row_key + "#";
            dirty = std::any_of(dirty_keys.begin(), dirty_keys.end(), [&prefix](const std::string& key) {
                return key.compare(0, prefix.size(), prefix) == 0;
            });
        }
        if (!dirty && row.id.section == Slic3r::PublishSection::Material && row.id.owner >= 0) {
            const size_t slot = size_t(row.id.owner);
            // A mixed slot's preset name is a copy of a component's, and it publishes no per-key
            // rows, so it is never pre-checked from a filament's dirty keys.
            const bool same_preset = slot < bundle->filament_presets.size() &&
                                     bundle->filament_presets[slot] == edited_filament && !slot_is_mixed(slot);
            if (same_preset) {
                for (const std::string& key : row.id.keys) {
                    const std::string base = publish_base_key(key);
                    if (row.id.column >= 0) {
                        const std::vector<size_t> columns = publish_filament_columns(uncompacted, slot);
                        const auto it = std::find(columns.begin(), columns.end(), size_t(row.id.column));
                        if (it == columns.end())
                            continue;
                        if (dirty_keys.count(base + "#" + std::to_string(it - columns.begin())) != 0) {
                            dirty = true;
                            break;
                        }
                    } else if (dirty_keys.count(base + "#0") != 0 || dirty_keys.count(base) != 0) {
                        // A per-slot key holds one value in the preset, so its dirty index is 0.
                        dirty = true;
                        break;
                    }
                }
            }
        }
        row.dirty = dirty;
        if (row.dirty)
            set_row_checked(r, true);
    }
    // A page may already have been built while the model was still being assembled, so paint the
    // marker once more now that every row knows whether it is dirty.
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_rows[r].dirty)
            apply_row_label_colour(r);

    // Wire the "Enable" checkboxes: toggling one reveals/hides the slot's settings below the
    // header (and, for a mixed slot, auto-selects its components). Bind by index so the lambda
    // stays valid even if the vector is reallocated later.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].enable_check != nullptr)
            m_categories[c].enable_check->Bind(wxEVT_TOGGLEBUTTON, [this, c](wxCommandEvent& event) {
                on_enable_toggle(c);
                event.Skip();
            });

    // Wire the "Full Publish" checkboxes (physical slots): toggling one disables/enables the
    // material's rows.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].full_check != nullptr)
            m_categories[c].full_check->Bind(wxEVT_TOGGLEBUTTON, [this, c](wxCommandEvent& event) {
                on_full_toggle(c);
                event.Skip();
            });

    // No filter is active at startup: every row matches until the user types.
    for (Row& row : m_rows)
        row.matches_filter = true;
    // Rows are drawn when their page is first shown, so opening stays close to today's cost
    // even with several variants per slot.
    apply_visibility();

    for (Category& category : m_categories) {
        category.scroll->FitInside();
        category.list_sizer->Layout();
    }
    for (SectionGroup& section : m_sections)
        if (!section.categories.empty())
            section.tabs->SelectItem(0);
    // Orca: several extruders are shown on the same switch as the printer tab's Extruder page.
    for (size_t s = 0; s < m_sections.size(); ++s)
        if (m_sections[s].variant_inners.size() > 1)
            setup_variant_switch(s);
    if (!m_sections.empty()) {
        m_outer_tabs->SelectItem(0);
        show_outer_page(0);
    }
    refresh_tab_indicators();
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
    // Process: the full strip stays hidden as the selection model; the page tabs are shown below the
    // extruder switch instead.
    if (kind == Section::Print) {
        section.page_tabs = new TabCtrl(section.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
        section.page_tabs->SetFont(Label::Body_14);
        section.page_tabs->SetBackgroundColour(GetBackgroundColour());
        page_sizer->Add(section.page_tabs, 0, wxEXPAND);
        section.tabs->Hide();
    }
    // Mixed-color filament slots get a second tab strip below the physical filament tabs; only
    // the Material section has them.
    if (kind == Section::Material) {
        section.mixed_tabs = new TabCtrl(section.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
        section.mixed_tabs->SetFont(Label::Body_14);
        section.mixed_tabs->SetBackgroundColour(GetBackgroundColour());
        page_sizer->Add(section.mixed_tabs, 0, wxEXPAND | wxTOP, FromDIP(2));
        section.mixed_tabs->Hide();
    }
    section.page_host = new wxPanel(section.page, wxID_ANY);
#ifdef __WINDOWS__
    section.page_host->SetDoubleBuffered(true);
#endif
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

size_t PublishSettingsDialog::category_index_for(const wxString& title,
                                                 Section section,
                                                 size_t group,
                                                 size_t source_index,
                                                 const PublishMaterialIdentity& identity,
                                                 bool is_mixed,
                                                 bool is_variant)
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
            category.enable_check = new ::CheckBox(category.page, wxID_ANY);
            category.enable_label = add_checkbox_label(category.page, enable_sizer, category.enable_check, _L("Enable"),
                                                       _L("Publish this mixed filament and enable + Full Publish its component filaments"));
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
            auto* enable_sizer    = new wxBoxSizer(wxHORIZONTAL);
            category.enable_check = new ::CheckBox(category.page, wxID_ANY);
            category.enable_label = add_checkbox_label(category.page, enable_sizer, category.enable_check, _L("Enable"),
                                                       _L("Publish this filament slot in the 3MF file"));
            header_sizer->Add(enable_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));
            page_sizer->Add(header_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));

            // Line 2: the "Full Publish" toggle, on its own line below the title (hidden until
            // the slot is enabled), aligned with the colour chip above it.
            auto* full_sizer    = new wxBoxSizer(wxHORIZONTAL);
            category.full_check = new ::CheckBox(category.page, wxID_ANY);
            category.full_label = add_checkbox_label(category.page, full_sizer, category.full_check, _L("Full Publish"),
                                                     _L("Embed the entire filament of this slot in the 3MF file"));
            category.full_line_item = page_sizer->Add(full_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));
        }
    }

    // The rows are painted at their own em-derived width (a 32 em floor), so the page scrolls
    // sideways rather than clipping a value column at a large DPI.
    if (section == Section::Material && !is_mixed) {
        // The Q1 notice, filled once the slot's resolved column is known.
        category.variant_notice = new wxStaticText(category.page, wxID_ANY, wxEmptyString);
        category.variant_notice->SetFont(Label::Body_13);
        category.variant_notice->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#C0392B")));
        category.variant_notice->Hide();
        page_sizer->Add(category.variant_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

        // The variant switch, above the sub-tabs. Filled and shown only when the slot's preset
        // defines more than one variant.
        category.variant_switch = new MultiSwitchButton(category.page);
        category.variant_switch->SetFont(Label::Body_11);
        category.variant_switch->SetFitToOptions();
        category.variant_switch->Hide();
        page_sizer->Add(category.variant_switch, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(4));

        // One sub-tab per Filament tab page, above the rows (spec 7.4 item 4).
        category.page_tabs = new TabCtrl(category.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, s_tab_style);
        category.page_tabs->SetFont(Label::Body_14);
        category.page_tabs->SetBackgroundColour(GetBackgroundColour());
        page_sizer->Add(category.page_tabs, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));
    }
    category.scroll = new wxScrolledWindow(category.page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
    category.scroll->SetScrollRate(FromDIP(20), FromDIP(20));
    category.scroll->SetBackgroundColour(GetBackgroundColour());
    category.list_sizer = new wxBoxSizer(wxVERTICAL);
    category.scroll->SetSizer(category.list_sizer);
    category.scroll->DisableFocusFromKeyboard();
    category.scroll->Bind(wxEVT_RIGHT_DOWN, &PublishSettingsDialog::show_menu, this);
    category.info = new wxStaticText(category.scroll, wxID_ANY, is_mixed ? m_info_mix : m_info_empty);
    category.info->SetFont(Label::Body_13);
    category.list_sizer->Add(category.info, 1, wxALIGN_CENTER_HORIZONTAL | wxALL, FromDIP(10));
    category.info->Hide();
    page_sizer->Add(category.scroll, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
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
    else {
        m_sections[group].categories.push_back(category_index);
        if (is_variant)
            m_sections[group].variant_inners.push_back(m_sections[group].categories.size() - 1);
    }
    if (section == Section::Material) {
        TabCtrl* target = is_mixed ? m_sections[group].mixed_tabs : m_sections[group].tabs;
        if (is_mixed) {
            // The tab's whole composition (mix chip + components + percents) lives in one
            // bitmap; the text is empty because a TabCtrl item cannot interleave images into
            // its text.
            const DynamicPrintConfig full_cfg = wxGetApp().preset_bundle->full_config();
            const wxBitmap tab_bmp = mixed_filament_tab_bitmap(full_cfg, source_index, FromDIP(20), target->GetBackgroundColour());
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
        if (section == Section::Print && !is_variant && m_sections[group].page_tabs != nullptr) {
            m_sections[group].page_tabs->AppendItem(title);
            m_sections[group].page_inners.push_back(m_sections[group].categories.size() - 1);
        }
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

size_t PublishSettingsDialog::subcategory_index_for(size_t category_index, const wxString& title, const wxString& icon,
                                                    const ConfigOptionsGroup* source, const Page* page, int preset_type)
{
    Category& category = m_categories[category_index];
    for (size_t i = 0; i < category.subs.size(); ++i)
        if (category.subs[i].title == title)
            return i;

    Subcategory sub;
    sub.title       = title;
    sub.icon        = icon;
    sub.preset_type = preset_type;
    // The same layout inputs the sidebar's group is built with; nothing here is a choice of the
    // dialog's (spec 7.4.0). label_width already carries new_optgroup's override when the Tab set
    // one.
    if (source != nullptr)
        sub.label_width = source->label_width;
    if (page != nullptr) {
        sub.split_multi_line      = page->m_split_multi_line;
        sub.option_label_at_right = page->m_option_label_at_right;
        sub.page_title            = page->title();
    }
    category.subs.push_back(std::move(sub));
    return category.subs.size() - 1;
}

size_t PublishSettingsDialog::add_row(Row row, size_t category_index, size_t subcategory_index)
{
    Category& category = m_categories[category_index];
    row.category        = category.title;
    row.subcategory     = category.subs[subcategory_index].title;
    row.section_title   = m_sections[category.group].title;
    row.outer_index     = category.group;
    row.inner_index     = category_index;
    row.sub_index       = subcategory_index;
    const size_t row_index = m_rows.size();
    m_rows.push_back(std::move(row));
    m_row_ids.push_back(m_rows.back().id);
    category.rows.push_back(row_index);
    category.subs[subcategory_index].rows.push_back(row_index);
    return row_index;
}

Line* PublishSettingsDialog::row_line(size_t row_index) const
{
    const Row& row = m_rows[row_index];
    if (row.options.empty())
        return nullptr;
    const Subcategory& sub = m_categories[row.inner_index].subs[row.sub_index];
    if (sub.group == nullptr)
        return nullptr;
    const RowOption& first = row.options.front();
    return sub.group->get_line(first.key + (first.index >= 0 ? "#" + std::to_string(first.index) : ""));
}

void PublishSettingsDialog::build_row_groups(size_t category_index)
{
    Category& category = m_categories[category_index];
    // The strip decides which source page's groups exist, so fill it before building any.
    build_page_tabs(category_index);

    bool built_any = false;
    for (Subcategory& sub : category.subs) {
        if (sub.built || sub.rows.empty())
            continue;
        // A source page's groups are built when its tab is selected.
        if (category.page_tabs != nullptr && sub.page_title != category.selected_page)
            continue;

        // The same group the sidebar's Page::new_optgroup creates: same class, same is_tab_opt
        // flag, same per-page layout flags and label width, no Tab callbacks (spec 7.4.0).
        auto* group           = new ConfigOptionsGroup(category.scroll, sub.title, sub.icon, &m_snapshot, /*is_tab_opt=*/true);
        group->index_in_search      = false;
        group->split_multi_line     = sub.split_multi_line;
        group->option_label_at_right = sub.option_label_at_right;
        group->label_width          = sub.label_width;
        group->set_config_category_and_type(sub.page_title, sub.preset_type);
        // Values are a read-only view of the snapshot: a field event must not write to it.
        group->set_readonly(true);

        sub.group = group;

        for (size_t r : sub.rows) {
            const Row& row = m_rows[r];
            if (row.options.empty())
                continue;

            auto make_option = [group](const RowOption& src) {
                Option option = group->get_option(src.key, src.index);
                // The source line may override the definition (sub-labels such as "Minimal").
                option.opt = src.def;
                return option;
            };

            Line line = group->create_single_option_line(make_option(row.options.front()), row.label_path.ToStdString());
            // The source line's own label and, where it has one, its tooltip and wiki path: the
            // generated tooltip names the option id, which is not what the sidebar shows.
            line.label      = row.label;
            line.label_en   = row.label_en;
            line.label_path = row.label_path.ToStdString();
            if (!row.label_tooltip.IsEmpty())
                line.label_tooltip = row.label_tooltip;

            for (size_t o = 1; o < row.options.size(); ++o)
                line.append_option(make_option(row.options[o]));
            line.row_toggle = RowToggle();
            group->append_line(line);
        }

        group->activate();
        group->update_visibility(comDevelop);
        group->reload_config();
        group->disable(); // every value is read-only

        // Bind the row toggles to the model and paint the dirty marker the way the Tab does.
        for (size_t r : sub.rows) {
            Row& row  = m_rows[r];
            Line* line = row_line(r);
            if (line == nullptr || !line->row_toggle)
                continue;
            line->row_toggle->on_change = [this, r](bool checked) { set_row_checked(r, checked); };
            sync_row_widgets(r);
            if (row.dirty)
                apply_row_label_colour(r);
        }

        sub.item = category.list_sizer->Add(group->sizer, 0, wxEXPAND | wxALL, FromDIP(5));
        sub.built = true;
        built_any  = true;
    }
    if (!built_any)
        return;

    category.list_sizer->Layout();
    if (category.scroll != nullptr)
        category.scroll->FitInside();
    // Rows carry their filter state from the model, so a page built after a filter was applied
    // hides the same rows a page built before it does.
    apply_visibility();
}

bool PublishSettingsDialog::row_on_selected_variant(size_t row_index) const
{
    const Row& row = m_rows[row_index];
    // Only material rows are spread over variants; a printer or process page already shows one
    // column, so its rows are never gated by the owner's switch.
    if (row.id.section != Slic3r::PublishSection::Material || row.id.column < 0 || row.inner_index >= m_categories.size())
        return true; // slot-level rows belong to every variant
    return row.id.column == m_categories[row.inner_index].column;
}

void PublishSettingsDialog::setup_material_variant_switch(size_t category_index)
{
    Category& category = m_categories[category_index];
    if (category.section != Section::Material || category.is_mixed || category.variant_switch == nullptr ||
        category.columns.size() < 2)
        return;

    // The same switch and the same labels the Filament tab uses for its variants.
    const ConfigOptionStrings* variants = m_snapshot.option<ConfigOptionStrings>("filament_extruder_variant");
    std::vector<wxString>      labels;
    int                        selection = 0;
    for (size_t i = 0; i < category.columns.size(); ++i) {
        const size_t column = category.columns[i];
        labels.push_back(variants != nullptr && column < variants->size() ? Tab::filament_variant_label(variants->get_at(column)) :
                                                                           wxString());
        if (int(column) == category.column)
            selection = int(i);
    }
    category.variant_switch->SetOptions(labels);
    category.variant_switch->SetSelection(selection);
    category.variant_switch->Bind(wxCUSTOMEVT_MULTISWITCH_SELECTION, [this, category_index](wxCommandEvent& evt) {
        evt.Skip();
        Category& cat   = m_categories[category_index];
        const int choice = evt.GetInt();
        if (choice < 0 || choice >= static_cast<int>(cat.columns.size()))
            return;
        cat.column = int(cat.columns[choice]);
        apply_visibility();
        if (cat.scroll != nullptr) {
            cat.scroll->Scroll(0, 0);
            cat.scroll->FitInside();
        }
        if (cat.page != nullptr)
            cat.page->Layout();
    });

    category.variant_switch->Show(true);
    if (category.page != nullptr)
        category.page->Layout();
}

void PublishSettingsDialog::build_page_tabs(size_t category_index)
{
    Category& category = m_categories[category_index];
    if (category.page_tabs == nullptr || !category.page_order.empty())
        return;

    for (const Subcategory& sub : category.subs) {
        if (std::find(category.page_order.begin(), category.page_order.end(), sub.page_title) != category.page_order.end())
            continue;
        category.page_order.push_back(sub.page_title);
    }
    if (category.page_order.size() < 2)
        return; // A single source page needs no strip.

    for (const wxString& page : category.page_order)
        category.page_tabs->AppendItem(_(page));
    category.selected_page = category.page_order.front();
    category.page_tabs->Bind(wxEVT_TAB_SEL_CHANGED, [this, category_index](wxCommandEvent& event) {
        Category& cat       = m_categories[category_index];
        const int selection = event.GetInt();
        if (selection < 0 || selection >= static_cast<int>(cat.page_order.size()))
            return;
        cat.selected_page = cat.page_order[selection];
        build_row_groups(category_index);
        apply_visibility();
        if (cat.scroll != nullptr) {
            cat.scroll->Scroll(0, 0);
            cat.scroll->FitInside();
        }
        if (cat.page != nullptr)
            cat.page->Layout();
    });
    category.page_tabs->SelectItem(0);
}

void PublishSettingsDialog::apply_row_label_colour(size_t row_index)
{
    const Row& row = m_rows[row_index];
    if (row.options.empty())
        return;
    Subcategory& sub = m_categories[row.inner_index].subs[row.sub_index];
    if (sub.group == nullptr)
        return;
    const RowOption& first  = row.options.front();
    const std::string opt_id = first.key + (first.index >= 0 ? "#" + std::to_string(first.index) : "");
    if (Field* field = sub.group->get_field(opt_id))
        field->set_label_colour(&wxGetApp().get_label_clr_modified());
}

void PublishSettingsDialog::sync_row_widgets(size_t row_index)
{
    Line* line = row_line(row_index);
    if (line == nullptr || !line->row_toggle)
        return;
    const Row& row            = m_rows[row_index];
    line->row_toggle->checked = m_selection.checked(row.id);
    line->row_toggle->enabled = m_selection.enabled(row.id);
}

void PublishSettingsDialog::sync_all_row_widgets()
{
    for (size_t r = 0; r < m_rows.size(); ++r)
        sync_row_widgets(r);
    for (Category& category : m_categories)
        for (Subcategory& sub : category.subs)
            if (sub.group != nullptr)
                sub.group->refresh();
}

void PublishSettingsDialog::set_row_checked(size_t row_index, bool checked)
{
    Row& row = m_rows[row_index];
    m_selection.set_checked(row.id, checked);
    sync_row_widgets(row_index);
}

void PublishSettingsDialog::set_row_enabled(size_t row_index, bool enabled)
{
    Row& row = m_rows[row_index];
    m_selection.set_enabled(row.id, enabled);
    sync_row_widgets(row_index);
}

void PublishSettingsDialog::on_full_toggle(size_t category_index)
{
    Category& cat   = m_categories[category_index];
    const bool full = cat.full_check->GetValue();
    m_selection.set_full_publish(int(cat.filament_slot), full);
    for (size_t r : cat.rows)
        set_row_enabled(r, !full);
    refresh_tab_indicators();
}

void PublishSettingsDialog::on_enable_toggle(size_t category_index)
{
    Category& cat      = m_categories[category_index];
    const bool enabled = cat.enable_check->GetValue();
    m_selection.set_owner_enabled(int(cat.filament_slot), enabled);

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
                if (comp_cat.enable_check != nullptr) {
                    comp_cat.enable_check->SetValue(true);
                    m_selection.set_owner_enabled(int(component_slot), true);
                }
                if (comp_cat.full_check != nullptr) {
                    comp_cat.full_check->SetValue(true);
                    m_selection.set_full_publish(int(component_slot), true);
                }
                on_full_toggle(c);
            }
        }
    }

    // Reveal/hide everything below the slot's header (rows, info, Full Publish line) and refresh
    // the visibility of the auto-selected component slots.
    apply_visibility();
    if (cat.page != nullptr)
        cat.page->GetSizer()->Layout();
    refresh_tab_indicators();
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
    // Theme colours and DIP metrics are resolved inside the paint handler so dark-mode toggles
    // and DPI changes are picked up on the next repaint without any explicit listener. The
    // ternary fill cache lives inside the shared triangle painter (FilamentBitmapUtils).
    viz->Bind(wxEVT_PAINT, [this, panel = viz, spec](wxPaintEvent&) {
        const wxColour bg = StateColor::darkModeColorFor(*wxWHITE);
        wxBufferedPaintDC pdc(panel);
        pdc.SetBackground(wxBrush(bg));
        pdc.Clear();
        const wxRect rc = panel->GetClientRect();
        if (rc.width <= 0 || rc.height <= 0)
            return;

        const size_t n = spec.component_colours.size();

        if (spec.tri_weights.size() == 3 && n == 3 && !spec.is_gradient) {
            // Ternary mix: read-only miniature of the MixedFilamentDialog's triangle picker.
            const MixedTriangleTheme tri_theme{StateColor::darkModeColorFor(*wxWHITE), StateColor::darkModeColorFor(wxColour("#CECECE")),
                                               StateColor::darkModeColorFor(wxColour("#262E30")),
                                               StateColor::darkModeColorFor(wxColour(107, 107, 107))};
            draw_mixed_triangle_picker(pdc, rc.GetSize(), {spec.component_colours[0], spec.component_colours[1], spec.component_colours[2]},
                                       {spec.tri_weights[0], spec.tri_weights[1], spec.tri_weights[2]}, tri_theme);
            draw_mixed_triangle_labels(pdc, rc.GetSize(), {spec.tri_weights[0], spec.tri_weights[1], spec.tri_weights[2]}, tri_theme);
        } else if (!spec.is_gradient) {
            // Ratio bar. A 2-component slot matches the MixedFilamentDialog's continuous blend +
            // divider (with its two end labels); wider non-gradient mixes (rare) fall back to one
            // solid segment per component.
            if (n == 2) {
                const int bar_h = FromDIP(27);
                draw_mixed_ratio_blend_bar(pdc, wxRect(rc.x, rc.y, rc.width, bar_h), spec.component_colours[0], spec.component_colours[1],
                                           spec.ratios[1]);
                // Read-only twin of the dialog's left/right percentage labels.
                pdc.SetFont(::Label::Body_12);
                pdc.SetTextForeground(StateColor::darkModeColorFor(wxColour(107, 107, 107)));
                const wxString la = wxString::Format("%d%%", int(std::lround(spec.ratios[0] * 100.0)));
                const wxString lb = wxString::Format("%d%%", int(std::lround(spec.ratios[1] * 100.0)));
                const int lab_y   = rc.y + bar_h + FromDIP(2);
                pdc.DrawText(la, rc.x, lab_y);
                pdc.DrawText(lb, rc.x + rc.width - pdc.GetTextExtent(lb).GetWidth(), lab_y);
            } else {
                draw_mixed_ratio_segments(pdc, rc, spec.component_colours, spec.ratios);
                // Percent label centred in each segment wide enough to hold it.
                std::vector<double> shares = spec.ratios;
                double total               = 0.0;
                for (double r : shares)
                    total += r;
                if (total <= 0.0) {
                    shares.assign(n, 1.0 / n);
                    total = 1.0;
                }
                pdc.SetFont(::Label::Body_12);
                int x0 = rc.x;
                for (size_t i = 0; i < n; ++i) {
                    const int x1 = (i + 1 < n) ? rc.x + int(std::lround(std::accumulate(shares.begin(), shares.begin() + i + 1, 0.0) /
                                                                        total * double(rc.width))) :
                                                 rc.x + rc.width;
                    const int w  = std::max(1, x1 - x0);
                    const wxString text = wxString::Format("%d%%", int(std::lround(shares[i] / total * 100.0)));
                    const wxSize tsz    = pdc.GetTextExtent(text);
                    if (tsz.GetWidth() + FromDIP(4) <= w) {
                        const wxColour& c = spec.component_colours[i];
                        const double lum  = 0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue();
                        pdc.SetTextForeground(lum > 140 ? wxColour("#262E30") : *wxWHITE);
                        pdc.DrawText(text, x0 + (w - tsz.GetWidth()) / 2, rc.y + (rc.height - tsz.GetHeight()) / 2);
                    }
                    x0 = x1;
                }
            }
        } else {
            // Gradient: read-only miniature of the GradientCurveEditor plot, drawn through the
            // shared painter so it anti-aliases and keeps the square proportions and labels.
            const MixedGradientTheme grad_theme{StateColor::darkModeColorFor(*wxWHITE),
                                                StateColor::darkModeColorFor(wxColour(238, 238, 238)),
                                                StateColor::darkModeColorFor(wxColour(107, 107, 107)),
                                                StateColor::darkModeColorFor(wxColour(107, 107, 107)),
                                                StateColor::darkModeColorFor(wxColour(38, 46, 48)),
                                                StateColor::darkModeColorFor(wxColour(172, 172, 172)),
                                                StateColor::darkModeColorFor(*wxWHITE)};
            std::vector<MixedGradientCurve> curves;
            std::vector<wxPoint2DDouble> anchors;
            if (spec.gradient_samples.size() >= 2 && n >= 2) {
                const wxRect plot = mixed_gradient_plot_rect(rc.GetSize());
                auto lift_alpha   = [](wxColour c) {
                    if (c.Alpha() == 0)
                        c.Set(c.Red(), c.Green(), c.Blue(), 150);
                    return c;
                };
                // First component's ratio solid, its mirror twin for the other.
                const wxColour col_a = lift_alpha(spec.component_colours[0]);
                const wxColour col_b = lift_alpha(spec.component_colours[1]);
                std::vector<wxPoint2DDouble> pts_a, pts_b;
                pts_a.reserve(spec.gradient_samples.size());
                pts_b.reserve(spec.gradient_samples.size());
                for (const auto& [t, r] : spec.gradient_samples) {
                    pts_a.push_back({plot.x + t * plot.width, plot.y + (1.0 - r) * plot.height});
                    pts_b.push_back({plot.x + t * plot.width, plot.y + r * plot.height});
                }
                // Control-point anchors of the stored curve on the first component's line.
                anchors.reserve(spec.gradient_anchors.size());
                for (const auto& [t, r] : spec.gradient_anchors)
                    anchors.push_back({plot.x + t * plot.width, plot.y + (1.0 - r) * plot.height});
                curves.push_back({std::move(pts_a), col_a, 4});
                curves.push_back({std::move(pts_b), col_b, 2});
            }
            draw_mixed_gradient_plot(pdc, rc.GetSize(), curves, anchors, grad_theme);
        }
    });

    // Fixed DIP size, left-aligned: the visualization keeps its proportions no matter how the
    // dialog is resized (the paint handler draws into whatever client rect the panel ends up
    // with, so nothing else has to change). Sizes mirror the MixedFilamentDialog controls: the
    // gradient plot and triangle picker match the editor's 260x200 / 160x160, the ratio bar is
    // the dialog's 27px bar plus its label line.
    int viz_h;
    int viz_w;
    if (spec.is_gradient) {
        viz_w = 260;
        viz_h = 200;
    } else if (spec.tri_weights.size() == 3 && spec.component_colours.size() == 3) {
        viz_w = 160;
        viz_h = 160;
    } else {
        viz_w = 240;
        viz_h = spec.component_colours.size() == 2 ? 50 : 30;
    }
    const wxSize viz_sz(FromDIP(viz_w), FromDIP(viz_h));
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

// Position of `value` in `positions`, or -1.
static int position_of(const std::vector<size_t>& positions, int value)
{
    for (size_t i = 0; i < positions.size(); ++i)
        if (static_cast<int>(positions[i]) == value)
            return static_cast<int>(i);
    return -1;
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
    // An extruder page selects its switch option; a page tab leaves the switch with no selection.
    if (section.variant_switch != nullptr) {
        if (const int pos = position_of(section.variant_inners, inner_index); pos >= 0)
            section.variant_switch->SetSelection(pos); // fires its event, which ignores the shown page
        else
            section.variant_switch->Unselect();
    }
    if (section.page_tabs != nullptr) {
        if (const int pos = position_of(section.page_inners, inner_index); pos >= 0)
            section.page_tabs->SelectItem(pos);
        else
            section.page_tabs->Unselect();
    }
    Category& category     = m_categories[section.categories[inner_index]];
    build_row_groups(section.categories[inner_index]);
    category.page->Show();
    category.scroll->FitInside();
    category.scroll->Scroll(category.scroll_pos.x, category.scroll_pos.y);
    if (section.mixed_tabs != nullptr)
        section.mixed_tabs->Unselect();
    section.page_host_sizer->Layout();
}

void PublishSettingsDialog::setup_variant_switch(size_t section_index)
{
    SectionGroup& section = m_sections[section_index];
    std::vector<wxString> titles;
    for (size_t inner : section.variant_inners)
        titles.push_back(m_categories[section.categories[inner]].title);

    section.variant_switch = new MultiSwitchButton(section.page);
    section.variant_switch->SetFitToOptions();
    section.variant_switch->SetOptions(titles);
    if (const int pos = position_of(section.variant_inners, section.selected_inner); pos >= 0)
        section.variant_switch->SetSelection(pos);
    section.variant_switch->Bind(wxCUSTOMEVT_MULTISWITCH_SELECTION, [this, section_index](wxCommandEvent& evt) {
        evt.Skip();
        // The hidden tab strip stays the selection model; its event shows the page.
        SectionGroup& sec = m_sections[section_index];
        if (evt.GetInt() < 0 || evt.GetInt() >= static_cast<int>(sec.variant_inners.size()))
            return;
        const int inner = static_cast<int>(sec.variant_inners[evt.GetInt()]);
        if (inner != sec.selected_inner)
            sec.tabs->SelectItem(inner);
    });

    // The switch takes the place of the printer's tab strip, centered like on the printer tab. On the
    // Process section it sits below the page tabs.
    wxSizer* page_sizer = section.page->GetSizer();
    page_sizer->Insert(section.page_tabs != nullptr ? 2 : 1, section.variant_switch, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(4));
    section.tabs->Hide();
    section.page->Layout();
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
    build_row_groups(section.mixed_categories[mixed_index]);
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

void PublishSettingsDialog::on_page_tab_changed(size_t section_index, wxCommandEvent& event)
{
    SectionGroup& section = m_sections[section_index];
    const int selection   = event.GetInt();
    if (selection < 0 || selection >= static_cast<int>(section.page_inners.size()))
        return;
    const int inner = static_cast<int>(section.page_inners[selection]);
    if (inner != section.selected_inner)
        section.tabs->SelectItem(inner);
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
        if (m_sections[section_index].page_tabs != nullptr)
            m_sections[section_index].page_tabs->Bind(wxEVT_TAB_SEL_CHANGED,
                                                      [this, section_index](wxCommandEvent& event) { on_page_tab_changed(section_index, event); });
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
            row.matches_filter = m_selection.enabled(row.id) && m_selection.checked(row.id) == want_checked;
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
            for (size_t r : sub.rows) {
                const bool show = m_rows[r].matches_filter && row_on_selected_variant(r);
                // Full Publish disables a row but keeps it visible, so the filter is the only
                // thing that hides it.
                if (Line* line = row_line(r))
                    line->toggle_visible = show;
                sub_any = sub_any || show;
            }
            if (sub.group != nullptr) {
                sub.group->update_visibility(comDevelop);
                const bool on_page = category.page_tabs == nullptr || sub.page_title == category.selected_page;
                if (sub.item != nullptr)
                    sub.item->Show(enabled && sub_any && on_page);
            }
        }
        category.list_sizer->Layout();
        if (category.scroll != nullptr)
            category.scroll->FitInside();

    }
    Thaw();
}

void PublishSettingsDialog::select_all(bool value)
{
    // "All" skips disabled (gated) rows and rows of a variant that is not shown; "None" leaves a
    // gated row's preserved value.
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_selection.enabled(m_rows[r].id) && row_on_selected_variant(r))
            set_row_checked(r, value);
    // "All" also enables every material slot (so its rows/Full Publish become visible and the
    // selection is actually exported); "None" disables them all again.
    for (Category& cat : m_categories)
        if (cat.section == Section::Material && cat.enable_check != nullptr && cat.enable_check->GetValue() != value)
            cat.enable_check->SetValue(value);
    // CheckBox::SetValue does not emit wxEVT_TOGGLEBUTTON, so re-run the enable handlers to
    // propagate mixed-slot components and refresh visibility as if the user had clicked.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].section == Section::Material)
            on_enable_toggle(c);
    apply_visibility();
    refresh_tab_indicators();
}

bool PublishSettingsDialog::row_is_visible(size_t row_index) const
{
    const Row& row = m_rows[row_index];
    if (m_selected_outer < 0 || m_selected_outer >= static_cast<int>(m_sections.size()) ||
        row.outer_index != static_cast<size_t>(m_selected_outer) || m_sections[m_selected_outer].selected_inner < 0 ||
        m_sections[m_selected_outer].selected_inner >= static_cast<int>(m_sections[m_selected_outer].categories.size()) ||
        row.inner_index != m_sections[m_selected_outer].categories[m_sections[m_selected_outer].selected_inner] || !row.matches_filter ||
        !m_selection.enabled(row.id) || !row_on_selected_variant(row_index))
        return false;
    const Subcategory& sub = m_categories[row.inner_index].subs[row.sub_index];
    if (sub.page_title != m_categories[row.inner_index].selected_page &&
        m_categories[row.inner_index].page_tabs != nullptr)
        return false;
    const Line* line = row_line(row_index);
    return line != nullptr && line->toggle_visible;
}

void PublishSettingsDialog::select_visible(bool value)
{
    // In a pseudo-filter view the rows being toggled would all disappear; drop the filter
    // afterwards so the result stays visible.
    const bool clear_pseudo = (m_filter_mode == FilterMode::UnselectedOnly && !value) ||
                              (m_filter_mode == FilterMode::SelectedOnly && value);

    // Toggle the rows visible under the *current* filter.
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (row_is_visible(r))
            set_row_checked(r, value);

    if (clear_pseudo) {
        // Note: SetValue() may fire wxEVT_TEXT on some platforms, re-entering apply_filter() -
        // that is fine; the rows above were already toggled and the trailing call is idempotent.
        m_filter_ctrl->ChangeValue("");
        apply_filter(""); // resync visibility and the All/None bar
    }
    refresh_tab_indicators();
}

void PublishSettingsDialog::show_menu(wxMouseEvent& evt)
{
    bool filtering  = !m_filter_ctrl->GetValue().IsEmpty() || m_filter_mode != FilterMode::Text;
    bool list_empty = true;
    if (m_selected_outer >= 0) {
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (row_is_visible(r)) {
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

void PublishSettingsDialog::apply_selection(const std::vector<std::string>& published_keys,
                                            const std::vector<Slic3r::PublishedMaterialEntry>& material_keys)
{
    // The supplied selection is authoritative: clear the dirty-default pre-check first so a
    // dirty key the user deselected stays off, then re-select exactly what the selection names.
    for (size_t r = 0; r < m_rows.size(); ++r)
        set_row_checked(r, false);
    for (Category& cat : m_categories) {
        if (cat.section != Section::Material)
            continue;
        if (cat.enable_check != nullptr)
            cat.enable_check->SetValue(false);
        if (cat.full_check != nullptr)
            cat.full_check->SetValue(false);
        m_selection.set_owner_enabled(int(cat.filament_slot), false);
        m_selection.set_full_publish(int(cat.filament_slot), false);
    }

    // Print / printer rows: match by exact publish key (the full "#N" opt_id where present). A
    // process "key#N" from a file without the per-extruder process layout carries the whole
    // vector, so it selects that setting on every extruder.
    for (const std::string& key : published_keys) {
        const std::string base_key = publish_base_key(key);
        const bool legacy_process  = m_legacy_process_selection && key != base_key && print_options_with_variant.count(base_key) != 0;
        for (size_t r = 0; r < m_rows.size(); ++r) {
            const Row& row = m_rows[r];
            if (row.id.section == Slic3r::PublishSection::Material || row.id.kind != Slic3r::PublishRowKind::Key)
                continue;
            const bool match = legacy_process ?
                                   (row.id.section == Slic3r::PublishSection::Process &&
                                    std::any_of(row.id.keys.begin(), row.id.keys.end(), [&base_key](const std::string& k) {
                                        return publish_base_key(k) == base_key;
                                    }))
                                 : std::any_of(row.id.keys.begin(), row.id.keys.end(),
                                               [&key](const std::string& k) { return k == key; });
            if (match)
                set_row_checked(r, true);
        }
    }

    // Per-slot material selections, applied positionally by slot.
    for (const Slic3r::PublishedMaterialEntry& entry : material_keys) {
        if (entry.slot < 0)
            continue;
        bool entry_mixed = false;
        for (const std::string& k : entry.keys)
            if (publish_mixed_keys().count(publish_base_key(k)) != 0) {
                entry_mixed = true;
                break;
            }
        size_t cat_idx = size_t(-1);
        for (size_t c = 0; c < m_categories.size(); ++c) {
            const Category& cat = m_categories[c];
            if (cat.section != Section::Material || cat.is_mixed != entry_mixed || cat.filament_slot != size_t(entry.slot))
                continue;
            cat_idx = c;
            break;
        }
        if (cat_idx == size_t(-1))
            continue; // slot not present in the receiver (out of range / skipped)
        Category& cat = m_categories[cat_idx];
        if (cat.enable_check != nullptr) {
            cat.enable_check->SetValue(true);
            on_enable_toggle(cat_idx);
        }
        if (entry.full && cat.full_check != nullptr) {
            cat.full_check->SetValue(true);
            on_full_toggle(cat_idx);
        } else {
            // Setting rows match by publish key, then by base key for files written before the
            // keys carried their column.
            for (const std::string& key : entry.keys) {
                const std::string base = publish_base_key(key);
                auto has_key = [&](const Row& row, const std::string& needle) {
                    return std::any_of(row.id.keys.begin(), row.id.keys.end(),
                                       [&needle](const std::string& k) { return k == needle; });
                };
                bool matched = false;
                for (const size_t r : cat.rows) {
                    const Row& row = m_rows[r];
                    if (row.id.kind != Slic3r::PublishRowKind::Key)
                        continue;
                    if (has_key(row, key) || (row.id.column < 0 && has_key(row, base))) {
                        set_row_checked(r, true);
                        matched = true;
                        break;
                    }
                }
                if (!matched)
                    for (const size_t r : cat.rows) {
                        const Row& row = m_rows[r];
                        if (row.id.kind == Slic3r::PublishRowKind::Key &&
                            std::any_of(row.id.keys.begin(), row.id.keys.end(),
                                        [&base](const std::string& k) { return publish_base_key(k) == base; })) {
                            set_row_checked(r, true);
                            break;
                        }
                    }
            }
            if (entry.publish_type)
                for (const size_t r : cat.rows)
                    if (m_rows[r].id.kind == Slic3r::PublishRowKind::Type) {
                        set_row_checked(r, true);
                        break;
                    }
            if (entry.publish_color)
                for (const size_t r : cat.rows)
                    if (m_rows[r].id.kind == Slic3r::PublishRowKind::Color) {
                        set_row_checked(r, true);
                        break;
                    }
        }
    }

    apply_visibility();
    refresh_tab_indicators();
}

std::vector<std::string> PublishSettingsDialog::GetPublishedKeys() const
{
    // Process and printer sections both travel through published_keys (the load-side overlay
    // applies process keys to the prints edited preset and the allowlisted printer keys to the
    // printers edited preset); material keys use a separate API. Each row's keys are already
    // publish-ready ("key" or "key#col"), so a checked row publishes exactly that column's value.
    return m_selection.published_keys(m_row_ids);
}

std::vector<MixedDependencyIssue> PublishSettingsDialog::unpublished_mixed_components() const
{
    return Slic3r::unpublished_mixed_components(m_selection, m_row_ids, m_snapshot);
}

std::vector<Slic3r::PublishedMaterialEntry> PublishSettingsDialog::GetPublishedMaterialKeys() const
{
    // Enabled owners only, in slot order; the model owns the Full Publish and per-key rules.
    return m_selection.material_entries(m_row_ids);
}

bool PublishSettingsDialog::category_has_selection(const Category& cat) const
{
    // A material slot publishes (or not) as a whole, gated by "Enable".
    if (cat.section == Section::Material)
        return m_selection.owner_enabled(int(cat.filament_slot));
    // Print/Printer categories have no gate: any checked row counts.
    for (const size_t r : cat.rows)
        if (m_selection.checked(m_rows[r].id))
            return true;
    return false;
}

void PublishSettingsDialog::refresh_tab_indicators()
{
    for (size_t s = 0; s < m_sections.size(); ++s) {
        SectionGroup& section = m_sections[s];
        bool any              = false;
        for (size_t i = 0; i < section.categories.size(); ++i) {
            const bool on = category_has_selection(m_categories[section.categories[i]]);
            section.tabs->SetItemIndicator(static_cast<unsigned int>(i), on);
            any = any || on;
        }
        // The switch has no indicator dot; mark its option text instead.
        if (section.variant_switch != nullptr)
            for (size_t p = 0; p < section.variant_inners.size(); ++p) {
                const Category& category = m_categories[section.categories[section.variant_inners[p]]];
                const bool      on       = category_has_selection(category);
                const wxString  text     = on ? category.title + wxString(" ") + wxString(wxUniChar(0x2022)) : category.title;
                if (section.variant_switch->GetOptionText(static_cast<unsigned int>(p)) != text) {
                    section.variant_switch->SetOptionText(static_cast<unsigned int>(p), text);
                    section.page->Layout();
                }
            }
        if (section.page_tabs != nullptr)
            for (size_t j = 0; j < section.page_inners.size(); ++j)
                section.page_tabs->SetItemIndicator(static_cast<unsigned int>(j),
                                                    category_has_selection(m_categories[section.categories[section.page_inners[j]]]));
        if (section.mixed_tabs != nullptr)
            for (size_t i = 0; i < section.mixed_categories.size(); ++i) {
                const bool on = category_has_selection(m_categories[section.mixed_categories[i]]);
                section.mixed_tabs->SetItemIndicator(static_cast<unsigned int>(i), on);
                any = any || on;
            }
        m_outer_tabs->SetItemIndicator(static_cast<unsigned int>(s), any);
    }
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
        if (section.page_tabs != nullptr)
            section.page_tabs->Rescale();
        if (section.mixed_tabs != nullptr)
            section.mixed_tabs->Rescale();
        if (section.variant_switch != nullptr)
            section.variant_switch->Rescale();
    }

    // Rescale the row groups with the same entry point the sidebar's Page uses.
    for (Category& category : m_categories)
        for (Subcategory& sub : category.subs)
            if (sub.group != nullptr)
                sub.group->msw_rescale();

    for (size_t category_index = 0; category_index < m_categories.size(); ++category_index) {
        const Category& category = m_categories[category_index];
        if (category.section != Section::Material || category.is_mixed)
            continue;
        if (wxBitmap* chip = get_extruder_color_icon(filament_color_hex(full, category.filament_slot),
                                                     std::to_string(category.filament_slot + 1), FromDIP(20), FromDIP(20))) {
            const SectionGroup& section = m_sections[category.group];
            const auto iter             = std::find(section.categories.begin(), section.categories.end(), category_index);
            if (iter != section.categories.end())
                m_sections[category.group].tabs->SetItemBitmap(static_cast<unsigned int>(iter - section.categories.begin()), *chip);
        }
    }
    refresh_mixed_tab_bitmaps();

    fit_to_content(); // tab buttons' min widths grew with the DPI: re-fit (incl. resize floor)
    Refresh();
}

void PublishSettingsDialog::refresh_mixed_tab_bitmaps()
{
    // The mixed-tab composition bitmaps bake the tab strip's background colour and DPI-scaled
    // text, so both a theme flip and a DPI change have to rebuild them.
    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
    for (SectionGroup& section : m_sections) {
        if (section.mixed_tabs == nullptr)
            continue;
        const wxColour tab_bg = section.mixed_tabs->GetBackgroundColour();
        for (size_t i = 0; i < section.mixed_categories.size(); ++i) {
            const Category& category = m_categories[section.mixed_categories[i]];
            const wxBitmap tab_bmp   = mixed_filament_tab_bitmap(full, category.filament_slot, FromDIP(20), tab_bg);
            if (tab_bmp.IsOk())
                section.mixed_tabs->SetItemBitmap(static_cast<unsigned int>(i), tab_bmp);
        }
    }
}

void PublishSettingsDialog::on_sys_color_changed()
{
    // The mixed-tab compositions bake the tab strip's background colour: rebuild them when the
    // theme changes (on_dpi_changed covers rescales).
    refresh_mixed_tab_bitmaps();
    // Re-apply the theme to every built group. The colour pass resets the label colours, so the
    // modified marker goes back on afterwards, as the Tab does.
    for (Category& category : m_categories)
        for (Subcategory& sub : category.subs)
            if (sub.group != nullptr)
                sub.group->sys_color_changed();
    for (size_t r = 0; r < m_rows.size(); ++r)
        if (m_rows[r].dirty)
            apply_row_label_colour(r);
    Refresh();
}

}} // namespace Slic3r::GUI
