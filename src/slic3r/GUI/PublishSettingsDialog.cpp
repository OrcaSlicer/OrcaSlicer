#include "PublishSettingsDialog.hpp"

#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "ColorPickerDialog.hpp"
#include "I18N.hpp"
#include "OptionStateRegistry.hpp"
#include "OptionsGroup.hpp"
#include "Field.hpp"
#include "PresetComboBoxes.hpp"
#include "PublishTabs.hpp"
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

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PublishSelection.hpp"
#include "libslic3r/PublishSettings.hpp"
#include "libslic3r/FilamentMixer.hpp"
#include "libslic3r/Model.hpp"

#include <boost/algorithm/string/trim.hpp>
#include <wx/anybutton.h>
#include <cstddef>
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
#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
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

// Keys the file always carries whole for every slot. Publishing one per key would rewrite the
// receiver's identity instead of gating it; a publishing slot's colour and type travel with its
// entry regardless.
const std::set<std::string>& publish_material_identity_keys()
{
    static const std::set<std::string> keys = {
        "filament_colour", "filament_type", "filament_vendor", "filament_ids",
        "filament_self_index", "filament_extruder_variant", "filament_diameter",
    };
    return keys;
}

// A control that performs an action (a colour picker, a plugin dialog, a device picker) is not a
// value published per key: it gets no row and no publish checkbox.
bool is_publish_action(const ConfigOptionDef& def) { return Slic3r::publish_is_action_control(def); }

// The author's hex colour for a filament slot (empty when the slot is out of range); shared by
// the picker badge, the inner-tab chip and the DPI rescale paths.
std::string filament_color_hex(const DynamicPrintConfig& full, size_t slot)
{
    if (const auto* colours = full.opt<ConfigOptionStrings>("filament_colour"))
        if (slot < colours->size())
            return colours->get_at(slot);
    return std::string();
}

// The slot's colour swatch, drawn with the same helper as the sidebar's swatch and the tab strip,
// as a button in the sidebar's colour-button style so it opens the same colour picker. The badge
// itself never writes the project; its click handler does.
wxBitmapButton* make_filament_badge(wxWindow* parent, const std::string& hex, const std::string& label, int size)
{
    wxBitmap* chip = get_extruder_color_icon(hex, label, size, size);
    if (chip == nullptr)
        return nullptr;
    auto* button = new wxBitmapButton(parent, wxID_ANY, *chip, wxDefaultPosition, wxSize(size, size),
                                      wxBU_EXACTFIT | wxBU_AUTODRAW | wxBORDER_NONE);
    button->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    button->SetToolTip(_L("Click to select filament color"));
#ifdef __WXGTK__
    RemoveButtonBorder(button);
#endif
    return button;
}

// The slot colour the shared picker opens on: the primary colour plus its stored multi-colour
// spelling, as the sidebar's combo reads it before showing the same dialog.
FilamentColorPickerValue slot_colour_value(const DynamicPrintConfig& project, size_t slot)
{
    FilamentColorPickerValue value;
    if (const auto* primary = project.opt<ConfigOptionStrings>("filament_colour"))
        if (slot < primary->values.size())
            value.primary = primary->values[slot];
    if (const auto* multi = project.opt<ConfigOptionStrings>("filament_multi_colour"))
        if (slot < multi->values.size()) {
            std::istringstream stream(multi->values[slot]);
            for (std::string colour; stream >> colour;)
                value.colors.push_back(std::move(colour));
        }
    if (value.colors.empty() && !value.primary.empty())
        value.colors.push_back(value.primary);
    if (const auto* types = project.opt<ConfigOptionStrings>("filament_colour_type"))
        if (slot < types->values.size())
            value.gradient = value.colors.size() > 1 && types->values[slot] == "0";
    return value;
}

// Mirrors PlaterPresetComboBox::sync_colour_config for one slot: the primary colour, its
// multi-colour spelling and the colour type that goes with it.
bool write_project_slot_colour(DynamicPrintConfig& project, size_t slot, const std::vector<std::string>& colours, bool gradient)
{
    if (colours.empty())
        return false;
    auto* primary = project.opt<ConfigOptionStrings>("filament_colour");
    auto* multi   = project.opt<ConfigOptionStrings>("filament_multi_colour");
    auto* type    = project.opt<ConfigOptionStrings>("filament_colour_type");
    if (primary == nullptr || multi == nullptr || type == nullptr)
        return false;
    for (ConfigOptionStrings* values : { primary, multi, type })
        if (values->values.size() <= slot)
            values->values.resize(slot + 1);
    std::string joined;
    for (const std::string& hex : colours)
        joined += hex + " ";
    if (!joined.empty())
        joined.pop_back();
    primary->values[slot] = colours.front();
    multi->values[slot]   = joined;
    type->values[slot]    = gradient ? "0" : "1";
    return true;
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

// Section title for a filament preset: the name the sidebar shows, then the filament type, then
// the generic "Material".
wxString material_title_for(const Preset* preset, const std::string& type)
{
    if (preset != nullptr && !preset->name.empty()) {
        // Display the name the sidebar does: the alias (already bare-name + variant-tail stripped
        // for root filament presets).
        const std::string& display = preset->alias.empty() ? get_preset_bare_name(preset->name) : preset->alias;
        return from_u8(material_display_name(display));
    }
    if (!type.empty())
        return from_u8(type);
    return _L("Material");
}

// Section title for a filament slot: the resolved preset name, then the filament type, then
// the generic "Material".
wxString material_title(size_t slot, const PresetBundle* bundle, const DynamicPrintConfig& full)
{
    const PublishMaterialIdentity identity = material_identity(slot, full);
    const Preset* preset = slot < bundle->filament_presets.size() ?
                               bundle->filaments.find_preset(bundle->filament_presets[slot]) :
                               nullptr;
    return material_title_for(preset, identity.type);
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

// Copies `src`'s element `index` into `dst`'s element `index`. A scalar option is copied whole.
// Returns false when either side cannot address that element.
bool copy_option_element_at(Slic3r::DynamicPrintConfig& dst, const std::string& key,
                            const Slic3r::DynamicPrintConfig& src, size_t index)
{
    const Slic3r::ConfigOption* from = src.option(key);
    Slic3r::ConfigOption*       to   = dst.option(key, true);
    if (from == nullptr || to == nullptr || from->type() != to->type())
        return false;
    if (to->is_scalar()) {
        to->set(from);
        return true;
    }
    auto*       dst_values = dynamic_cast<Slic3r::ConfigOptionVectorBase*>(to);
    const auto* src_values = dynamic_cast<const Slic3r::ConfigOptionVectorBase*>(from);
    if (dst_values == nullptr || src_values == nullptr || index >= src_values->size())
        return false;
    dst_values->set_at(src_values, index, index);
    return true;
}

// Whether the Line draws this option through an indexed field ("base#N"), the way per-variant rows
// are built. Such a field edits the element the tab's switch resolves; an unindexed field always
// edits index 0.
bool line_option_indexed(const Slic3r::GUI::Line& line, const std::string& base_key)
{
    for (const Slic3r::GUI::Option& option : line.get_options())
        if (Slic3r::publish_base_key(option.opt_id) == base_key)
            return Slic3r::publish_key_index(option.opt_id) >= 0;
    return false;
}

} // namespace

// The sidebar's filament preset combo, made inert: a pick is recorded and handed to the dialog,
// which assigns it to the slot live, instead of updating the slot's preset, the project colours or
// anything else the sidebar's own handler does. The colour button and the preset menu the combo
// creates for the sidebar are hidden, so the picker is only the list and its swatches; the slot's
// colour is shown by the dialog's own badge beside it.
class PublishFilamentPicker : public PlaterPresetComboBox
{
public:
    PublishFilamentPicker(wxWindow* parent, int filament_idx, std::function<void(const std::string&)> on_pick)
        : PlaterPresetComboBox(parent, Preset::TYPE_FILAMENT)
        , m_on_pick(std::move(on_pick))
    {
        set_filament_idx(filament_idx);
        if (clr_picker != nullptr)
            clr_picker->Hide();
        if (edit_btn != nullptr)
            edit_btn->Hide();
    }

    // The item's preset is the dialog's to read, not the bundle's to switch to.
    void OnSelect(wxCommandEvent& evt) override
    {
        const int item = evt.GetSelection();
        if (item < 0)
            return;
        const Marker marker = reinterpret_cast<Marker>(GetClientData(item));
        if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX) {
            SetSelection(m_last_selected); // a group label or a wizard entry
            return;
        }
        m_last_selected = item;
        SetSelection(item);
        if (m_on_pick == nullptr)
            return;
        // The sidebar resolves an item the same way: its stored internal name first, then the
        // alias of the shown text.
        std::string preset_name;
        const wxString stored = GetItemAlias(item);
        if (!stored.empty())
            preset_name = Preset::remove_suffix_modified(stored.ToUTF8().data());
        else
            preset_name = wxGetApp().preset_bundle->get_preset_name_by_alias(
                Preset::TYPE_FILAMENT, Preset::remove_suffix_modified(GetString(item).ToUTF8().data()));
        if (!preset_name.empty())
            m_on_pick(preset_name);
    }

    // Shows `preset_name` again after the combo refreshed its list from the bundle.
    void show_preset(const std::string& preset_name)
    {
        if (preset_name.empty())
            return;
        // An AMS item carries no stored preset name (add_ams_filaments does not set an alias), so
        // its displayed text is resolved the way the pick's own callback resolves it.
        int fallback = -1;
        for (int item = 0; item < GetCount(); ++item) {
            const Marker marker = reinterpret_cast<Marker>(GetClientData(item));
            if (marker >= LABEL_ITEM_DISABLED && marker < LABEL_ITEM_MAX)
                continue;
            std::string name;
            const wxString stored = GetItemAlias(item);
            if (!stored.empty())
                name = Preset::remove_suffix_modified(stored.ToUTF8().data());
            else
                name = wxGetApp().preset_bundle->get_preset_name_by_alias(
                    Preset::TYPE_FILAMENT, Preset::remove_suffix_modified(GetString(item).ToUTF8().data()));
            if (name != preset_name)
                continue;
            // The AMS item carries the swatch the pick was made from, so it wins over the plain
            // preset item of the same name.
            if (GetFlag(item) == int(FilamentAMSType::FROM_AMS)) {
                m_last_selected = item;
                SetSelection(item);
                return;
            }
            if (fallback < 0)
                fallback = item;
        }
        if (fallback >= 0) {
            m_last_selected = fallback;
            SetSelection(fallback);
        }
    }

private:
    std::function<void(const std::string&)> m_on_pick;
};

// Warning shown on OK when a publishing mixed-filament slot relies on a filament that would ship
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
            // A component either publishes its material (checkbox or Full Publish) or does not;
            // the same sentence serves both, so no reason branch is needed.
            add_sentence_with_chips(this, row, _L("%1% needs %2%, whose material will not be published."),
                                    {mix_chip, comp_chip});
            content->Add(row, 0, wxEXPAND | wxLEFT, FromDIP(10));
            content->AddSpacer(FromDIP(6));
        }

        auto* hint = new wxStaticText(this, wxID_ANY, _L("To publish a mixed filament, publish every filament it uses."));
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

    // Edits reach the project at once; the plater re-reads the config once per burst of edits.
    m_live_refresh_timer.SetOwner(this);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { flush_live_refresh(); });
    // Closing from the title bar ends the dialog without the buttons: settle the refresh first.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        flush_live_refresh();
        event.Skip();
    });

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
        // when a publishing mixed filament would ship without the material of one of its
        // components; "Publish anyway" accepts that and publishes anyway.
        if (const std::vector<MixedDependencyIssue> issues = unpublished_mixed_components(); !issues.empty()) {
            const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
            MixedFilamentWarningDialog warn(this, full, issues);
            if (warn.ShowModal() != wxID_OK)
                return; // Cancel: dismiss the warning and stay in this dialog
        }
        // Every edit already reached the project as it was made, so OK only settles the pending
        // refresh and closes.
        flush_live_refresh();
        EndModal(wxID_OK);
    });
    // Cancel closes without restoring anything: the edits are the project's now.
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        flush_live_refresh();
        EndModal(wxID_CANCEL);
    });

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
// shrinking never re-hides a filament tab. The cap wins over the tab width: a project whose
// strip needs more than the cap cannot show every tab at once, and relayout hides the
// overflowing ones rather than growing the dialog past the cap.
void PublishSettingsDialog::fit_to_content()
{
    static const wxSize BASE{530, 530}; // base size in DIP, the minimum the dialog can shrink to
    static const wxSize CAP{1300, 850}; // hard cap in DIP, the maximum the dialog can grow to
    // TabCtrl separates its buttons with proportional stretch spacers, which collapse to zero
    // when the strip is sized to its bare width - the indicator dot on a dirty slot's tab would
    // sit flush against the next slot's colour chip. Reserve this much per tab item as slack.
    static const int TAB_SLACK_DIP = 10;

    const int slack  = FromDIP(TAB_SLACK_DIP);
    auto strip_width = [slack](const TabCtrl* tabs) { return tabs->GetFullSize() + slack * int(tabs->GetCount()); };

    int strip = strip_width(m_outer_tabs);
    for (const SectionGroup& section : m_sections) {
        strip = std::max(strip, strip_width(section.tabs));
        if (section.mixed_tabs != nullptr)
            strip = std::max(strip, strip_width(section.mixed_tabs));
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

PublishSettingsDialog::~PublishSettingsDialog()
{
    // A refresh scheduled for after the dialog closed has no meaning.
    m_live_refresh_timer.Stop();
    // The hosted tabs outlive this dialog's members, so drop the callbacks that capture it.
    for (Category& category : m_categories) {
        if (category.publish_tab == nullptr)
            continue;
        category.publish_tab->m_on_variant_changed = nullptr;
        category.publish_tab->m_on_rules_applied   = nullptr;
        category.publish_tab->m_on_publish_edit    = nullptr;
    }
}

void PublishSettingsDialog::build_option_model()
{
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

    m_snapshot = uncompacted;

    // --- Phase 1: the printer settings, drawn by one dialog-hosted Printer tab over a preset
    // seeded from the edited printer preset (spec 7.4). Its own extruder switch selects the
    // variant, and its pages are pruned to the published-3MF allowlist, so the rows below are the
    // sidebar's Retraction and Z-Hop groups.
    if (bundle->printers.get_edited_preset().printer_technology() == ptFFF) {
        size_t g = section_group_for(Section::Printer);
        const size_t category_index = category_index_for(_L("Printer"), Section::Printer, g, 0);
        add_printer_tab_rows(category_index);
        bind_publish_tab(category_index);
    }

    // --- Phase 2: per-material sections, one per used slot, each drawing the sidebar's own
    // Filament tab (spec 7.4) over a preset seeded from that slot (Task 14).
    {
        size_t g = section_group_for(Section::Material);

        // Mixed-color slots are virtual: they carry no per-key Material/Retraction
        // settings; their publish checkbox embeds the mix definition (components, ratios,
        // gradient). Detect them via the project-level flag.
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
            m_categories[category_index].filament         = bundle->filament_presets[slot];
            m_categories[category_index].pending_filament = bundle->filament_presets[slot];
            m_selection.set_owner_info(int(slot), std::move(owner_info));
            // Nothing is published until the author selects something: the material checkbox
            // starts off and its explicit per-slot state is kept.
            m_selection.set_owner_published(int(slot), false);
            m_selection.set_full_publish(int(slot), false);
            if (m_categories[category_index].owner_check != nullptr)
                m_categories[category_index].owner_check->SetValue(false);

            if (is_mixed) {
                // A mixed slot publishes as one unit (its definition); nothing to select
                // per-key. Its component filaments are auto-published + Full Published when
                // its checkbox is checked (see on_owner_toggle). Its page still shows what
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

            // The slot's settings, straight off its hosted tab's lines. Colour and type are not
            // rows: the slot's "Material" checkbox publishes them.
            add_material_tab_rows(category_index);
            bind_publish_tab(category_index);
        }
    }

    // --- Phase 3: the process settings, drawn by one dialog-hosted Process tab over a preset
    // seeded from the edited print preset (spec 7.4). The tab's own extruder switch and page strip
    // are the navigation, and its page set is kept whole.
    {
        size_t       g              = section_group_for(Section::Print);
        const size_t category_index = category_index_for(_L("Process"), Section::Print, g, 0);
        add_print_tab_rows(category_index);
        bind_publish_tab(category_index);
    }

    // Pre-check the dirty (modified) settings. Printer and process dirty keys already carry the
    // column of their own preset, which is the file column, so an exact match is right for them.
    // A filament preset's dirty keys carry its *local* variant index, and several slots routinely
    // share one preset: publish_dirty_material_keys resolves them to each sharing slot's own
    // "base#file_column" (spec 7.5).
    const std::vector<std::string> dirty = collect_dirty_settings_keys(*bundle);
    std::vector<bool>              slot_is_mixed(bundle->filament_presets.size(), false);
    if (const auto* mixed_flags = full.opt<ConfigOptionBools>("filament_is_mixed"))
        for (size_t slot = 0; slot < slot_is_mixed.size() && slot < mixed_flags->values.size(); ++slot)
            slot_is_mixed[slot] = mixed_flags->values[slot];
    const std::vector<std::string> dirty_material =
        publish_dirty_material_keys(uncompacted, bundle->filament_presets, slot_is_mixed,
                                    bundle->filaments.get_selected_preset_name(), dirty);
    const std::set<std::string> dirty_keys(dirty.begin(), dirty.end());
    const std::set<std::string> dirty_material_keys(dirty_material.begin(), dirty_material.end());
    for (size_t r = 0; r < m_rows.size(); ++r) {
        Row& row = m_rows[r];
        // A row whose control is an action has no publish checkbox to check; only key rows are
        // "dirty overrides".
        if (row.id.kind != Slic3r::PublishRowKind::Key || !row.publishable)
            continue;
        // A row is one publish unit, so any of its keys being modified marks the whole row.
        const bool material = row.id.section == Slic3r::PublishSection::Material;
        bool       row_dirty = false;
        for (const std::string& key : row.id.keys) {
            if (!material) {
                row_dirty = dirty_keys.count(key) != 0;
            } else if (dirty_material_keys.count(key) != 0) {
                // A per-slot key rides on every column's row of a mixed line: only the column this
                // slot prints with counts, so a dirty per-slot value never pre-checks another
                // variant. A "base#file_column" key names its own row.
                row_dirty = publish_key_index(key) >= 0 || row.id.column < 0 ||
                            row.id.column == m_categories[row.inner_index].column;
            }
            if (row_dirty)
                break;
        }
        // A print/printer row with no column suffix covers the whole setting, so any index of it
        // counts as a match; a material row's dirty keys are already per-slot publish keys.
        const std::string& row_key = row.id.keys.front();
        if (!row_dirty && !material && row.id.column < 0 && row_key.find('#') == std::string::npos) {
            const std::string prefix = row_key + "#";
            row_dirty = std::any_of(dirty_keys.begin(), dirty_keys.end(), [&prefix](const std::string& key) {
                return key.compare(0, prefix.size(), prefix) == 0;
            });
        }
        row.dirty = row_dirty;
        if (row.dirty)
            set_row_checked(r, true);
    }
    // A page may already have been built while the model was still being assembled, so paint the
    // marker once more now that every row knows whether it is dirty.
    for (size_t r = 0; r < m_rows.size(); ++r)
        apply_row_label_colour(r);

    // Wire the owners' publish checkboxes: a physical slot's material checkbox publishes its
    // colour and type; a mixed slot's checkbox publishes the definition and auto-selects its
    // components. Bind by index so the lambda stays valid even if the vector is reallocated.
    for (size_t c = 0; c < m_categories.size(); ++c)
        if (m_categories[c].owner_check != nullptr)
            m_categories[c].owner_check->Bind(wxEVT_TOGGLEBUTTON, [this, c](wxCommandEvent& event) {
                on_owner_toggle(c);
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
    // The hosted tabs built and enabled their fields while the pages activated, before the row
    // hooks existed. Gate every row once now, so an unchecked row starts read-only instead of
    // staying enabled until the first extruder or variant switch.
    for (size_t c = 0; c < m_categories.size(); ++c)
        apply_row_field_gating(c);
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
    // Printer and Process: one hosted page each, so the strip is the selection model only and the
    // tab's own pages and extruder switch are the visible navigation.
    if (kind != Section::Material)
        section.tabs->Hide();
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
                                                 bool is_mixed)
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

    // The physical slot's colour chip sits left of its preset picker, where the sidebar draws its
    // swatch, and the inner tab carries the same chip; both carry the 1-based slot number,
    // mirroring the main GUI's filament swatches. A mixed slot has no header at all: its page is
    // just its publish checkbox above the (always visible) definition preview - the identification
    // lives in the tab strip's composition bitmap.
    std::string hex;
    std::string chip_label;
    if (section == Section::Material && !is_mixed) {
        const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();
        hex                           = filament_color_hex(full, source_index);
        chip_label                    = std::to_string(source_index + 1);
    }

    if (section == Section::Material) {
        if (is_mixed) {
            // No chip/title: the lone publish checkbox tops the page.
            auto* owner_sizer    = new wxBoxSizer(wxHORIZONTAL);
            category.owner_check = new ::CheckBox(category.page, wxID_ANY);
            add_checkbox_label(category.page, owner_sizer, category.owner_check, _L("Enable"),
                               _L("Publish this mixed filament and enable + Full Publish its component filaments"));
            page_sizer->Add(owner_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));
        } else {
            // Line 1: the owner title. The tab below is always shown; a row's own checkbox keeps
            // its value read-only until it is checked.
            auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);
            category.title_label = new wxStaticText(category.page, wxID_ANY, title);
            category.title_label->SetFont(Label::Head_14);
            header_sizer->Add(category.title_label, 0, wxALIGN_CENTER_VERTICAL);
            page_sizer->Add(header_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));

            // Line 2: the "Full Publish" toggle, on its own line below the title, aligned with the
            // colour badge further down.
            auto* full_sizer    = new wxBoxSizer(wxHORIZONTAL);
            category.full_check = new ::CheckBox(category.page, wxID_ANY);
            add_checkbox_label(category.page, full_sizer, category.full_check, _L("Full Publish"),
                               _L("Embed the entire filament of this slot in the 3MF file"));
            category.full_line_item = page_sizer->Add(full_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));

            // Line 3: the slot's "Material" checkbox immediately left of its colour badge, then
            // the sidebar's own filament picker. The checkbox publishes the slot's colour and
            // material type; the key rows below publish independently. Choosing a preset assigns
            // it to this slot at once; clicking the badge writes the slot's colour at once.
            auto* picker_sizer = new wxBoxSizer(wxHORIZONTAL);
            category.owner_check = new ::CheckBox(category.page, wxID_ANY);
            add_checkbox_label(category.page, picker_sizer, category.owner_check, _L("Material"),
                               _L("Publish this filament slot's color and material type in the 3MF file"));
            category.filament_color_chip = make_filament_badge(category.page, hex, chip_label, FromDIP(20));
            if (category.filament_color_chip != nullptr) {
                category.filament_color_chip->Bind(wxEVT_BUTTON,
                                                   [this, source_index](wxCommandEvent&) { on_filament_color_picked(source_index); });
                picker_sizer->Add(category.filament_color_chip, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
            }
            category.filament_picker = new PublishFilamentPicker(
                category.page, int(source_index),
                [this, source_index](const std::string& preset_name) { on_filament_picked(source_index, preset_name); });
            picker_sizer->Add(category.filament_picker, 1, wxEXPAND);
            category.filament_picker->update();
            category.filament_picker_item =
                page_sizer->Add(picker_sizer, 0, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, FromDIP(6));
        }
    }

    if (section == Section::Material && !is_mixed) {
        // The Q1 notice, filled once the slot's resolved column is known.
        category.variant_notice = new wxStaticText(category.page, wxID_ANY, wxEmptyString);
        category.variant_notice->SetFont(Label::Body_13);
        category.variant_notice->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#C0392B")));
        category.variant_notice->Hide();
        page_sizer->Add(category.variant_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

        // The slot's settings are the sidebar's own Filament tab, over a private preset seeded
        // from this slot (spec 7.4). Its own pages and variant switch are the navigation.
        auto* host = new PublishParamsPanel(category.page);
        category.publish_host = host;
        build_material_tab(category);
        page_sizer->Add(host, 1, wxEXPAND | wxTOP, FromDIP(4));
    } else if (section == Section::Printer) {
        // The printer settings are the sidebar's own Printer tab, over a private preset seeded
        // from the edited printer preset (spec 7.4). Its own extruder switch selects the variant,
        // and its pages are pruned to the publish allowlist.
        auto* host = new PublishParamsPanel(category.page);
        auto* tab  = new PublishTabPrinter(host);
        const DynamicPrintConfig seed = wxGetApp().preset_bundle->printers.get_edited_preset().config;
        tab->create_publish_tab(seed);
        host->set_publish_tab(tab);
        category.publish_host = host;
        category.publish_tab  = tab;
        page_sizer->Add(host, 1, wxEXPAND | wxTOP, FromDIP(4));
    } else if (section == Section::Print) {
        // The process settings are the sidebar's own Process tab, over a private preset seeded
        // from the edited print preset (spec 7.4). Its own extruder switch and page strip are the
        // navigation; its page set is kept whole.
        auto* host = new PublishParamsPanel(category.page);
        auto* tab  = new PublishTabPrint(host);
        const DynamicPrintConfig seed = wxGetApp().preset_bundle->prints.get_edited_preset().config;
        tab->create_publish_tab(seed);
        host->set_publish_tab(tab);
        category.publish_host = host;
        category.publish_tab  = tab;
        page_sizer->Add(host, 1, wxEXPAND | wxTOP, FromDIP(4));
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
    // A hosted page's scroll holds no setting rows: it only reports an empty filter, so it takes
    // no height. A mixed slot's scroll keeps the room for its hint.
    if (category.publish_host != nullptr)
        page_sizer->Add(category.scroll, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
    else
        page_sizer->Add(category.scroll, 1, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(4));
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

void PublishSettingsDialog::build_material_tab(Category& category)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (category.publish_host == nullptr || bundle == nullptr)
        return;

    const size_t slot = category.filament_slot;
    const std::string preset_name = !category.pending_filament.empty() ? category.pending_filament :
                                    (slot < bundle->filament_presets.size() ? bundle->filament_presets[slot] : std::string());
    const Preset* slot_preset = preset_name.empty() ? nullptr : bundle->filaments.find_preset(preset_name, false, true);
    DynamicPrintConfig seed;
    if (slot_preset != nullptr)
        seed = slot_preset->config;
    if (seed.empty())
        seed = bundle->filaments.get_edited_preset().config;
    // A filament preset carries no filament_colour (it is not in Preset::filament_options): the
    // slot's colour is project state, so seed it for the Tab's own rules. The type is the picked
    // preset's own value, which is what the tab edits and the payload carries.
    const DynamicPrintConfig full = bundle->full_config();
    auto full_value = [&full, slot](const char* key) {
        if (const auto* values = full.opt<ConfigOptionStrings>(key))
            if (slot < values->size())
                return values->get_at(slot);
        return std::string();
    };
    std::string type;
    if (const auto* types = seed.opt<ConfigOptionStrings>("filament_type"); types != nullptr && !types->values.empty())
        type = types->values.front();
    seed.set_key_value("filament_colour", new ConfigOptionStrings{full_value("filament_colour")});
    seed.set_key_value("filament_type", new ConfigOptionStrings{type.empty() ? full_value("filament_type") : type});

    auto* tab = new PublishTabFilament(category.publish_host);
    tab->create_publish_tab(seed);
    category.publish_tab = tab;
    category.publish_host->set_publish_tab(tab);
    // A rebuilt tab is built long after the dialog's theme pass ran, so it takes the host's
    // background here rather than at the next colour change.
    tab->adopt_publish_host_theme();
}

void PublishSettingsDialog::remove_category_rows(size_t category_index)
{
    Category& category = m_categories[category_index];
    std::vector<size_t> rows = category.rows;
    std::sort(rows.begin(), rows.end());
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        m_rows.erase(m_rows.begin() + std::ptrdiff_t(*it));
        m_row_ids.erase(m_row_ids.begin() + std::ptrdiff_t(*it));
    }
    // Every index-based view of the rows moved with them; rebuild them from the rows' own owner.
    for (size_t c = 0; c < m_categories.size(); ++c) {
        std::vector<size_t>& index_rows = m_categories[c].rows;
        index_rows.clear();
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (m_rows[r].inner_index == c)
                index_rows.push_back(r);
    }
}

void PublishSettingsDialog::refresh_material_layout()
{
    // The project's own slot presets are the truth: a pick is assigned when it is made.
    m_snapshot = wxGetApp().preset_bundle->full_config(/*apply_extruder=*/false);

    // The selection is keyed by the row id, so a slot's remap must not read a state another slot
    // already rewrote: every row's state is read before any id moves.
    struct RowState
    {
        Slic3r::PublishRowId id;
        bool                 checked{false};
        bool                 enabled{true};
    };
    std::vector<RowState> states;
    states.reserve(m_rows.size());
    for (const Row& row : m_rows)
        states.push_back({ row.id, m_selection.checked(row.id), m_selection.enabled(row.id) });
    std::vector<bool> dropped(m_rows.size(), false);

    for (size_t c = 0; c < m_categories.size(); ++c) {
        Category& category = m_categories[c];
        if (category.section != Section::Material || category.is_mixed)
            continue;
        const std::vector<size_t>           columns  = publish_filament_columns(m_snapshot, category.filament_slot);
        const Slic3r::PublishResolvedColumn resolved = publish_resolved_column(m_snapshot, category.filament_slot);
        const std::vector<size_t>           previous = category.columns;
        category.columns                             = columns;
        category.column = resolved.file_column.has_value() ? int(*resolved.file_column) : -1;
        if (previous == columns)
            continue;

        // An assigned preset with another variant count moved the file columns this slot's rows
        // name: follow the same local variant to its new column.
        auto remap = [&previous, &columns](int file_column) {
            if (file_column < 0)
                return file_column;
            const auto found = std::find(previous.begin(), previous.end(), size_t(file_column));
            if (found == previous.end())
                return file_column;
            const size_t local = size_t(found - previous.begin());
            return local < columns.size() ? int(columns[local]) : -1;
        };

        for (const size_t r : category.rows) {
            Row&            row    = m_rows[r];
            const RowState& before = states[r];
            row.id.column          = remap(before.id.column);
            row.id.keys.clear();
            bool lost = row.id.column < 0 && before.id.column >= 0;
            for (const std::string& key : before.id.keys) {
                const int index = publish_key_index(key);
                if (index < 0) {
                    row.id.keys.push_back(key);
                    continue;
                }
                const int mapped = remap(index);
                if (mapped < 0) {
                    lost = true;
                    continue;
                }
                row.id.keys.push_back(publish_base_key(key) + "#" + std::to_string(mapped));
            }
            for (RowOption& option : row.options)
                if (option.index >= 0 && Slic3r::publish_is_variant_bearing(option.key))
                    option.index = remap(option.index);
            // A row the new layout cannot place is gone: keep it out rather than leave its old
            // "base#col" keys on a row that no longer names a column.
            if (lost || row.id.keys.empty()) {
                dropped[r] = true;
                continue;
            }
            m_selection.set_checked(row.id, before.checked);
            m_selection.set_enabled(row.id, before.enabled);
        }
    }

    if (std::find(dropped.begin(), dropped.end(), true) != dropped.end()) {
        std::vector<Row> kept;
        kept.reserve(m_rows.size());
        for (size_t r = 0; r < m_rows.size(); ++r)
            if (!dropped[r])
                kept.push_back(std::move(m_rows[r]));
        m_rows = std::move(kept);
        // Every index-based view of the rows moved with them; rebuild them from the rows' own owner.
        for (Category& category : m_categories)
            category.rows.clear();
        for (size_t r = 0; r < m_rows.size(); ++r)
            m_categories[m_rows[r].inner_index].rows.push_back(r);
    }

    m_row_ids.clear();
    m_row_ids.reserve(m_rows.size());
    for (const Row& row : m_rows)
        m_row_ids.push_back(row.id);
    sync_all_row_widgets();
}

std::string PublishSettingsDialog::staged_badge_color(const Category& category, const DynamicPrintConfig& full) const
{
    // The colour last picked on the badge; it is in the project too, so the badge and the payload
    // identity show the same colour the file will carry.
    if (!category.pending_colours.empty())
        return category.pending_colours.front();
    return filament_color_hex(full, category.filament_slot);
}

PublishSettingsDialog::Category* PublishSettingsDialog::material_category(size_t slot)
{
    for (size_t c = 0; c < m_categories.size(); ++c) {
        Category& candidate = m_categories[c];
        if (candidate.section == Section::Material && !candidate.is_mixed && candidate.filament_slot == slot)
            return &candidate;
    }
    return nullptr;
}

void PublishSettingsDialog::refresh_slot_colour(size_t category_index)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    Category&          category = m_categories[category_index];
    const std::string hex      = staged_badge_color(category, bundle->full_config());
    const std::string label    = std::to_string(category.filament_slot + 1);

    // The badge and the tab strip's chip show one colour for the slot.
    if (category.filament_color_chip != nullptr) {
        if (wxBitmap* chip = get_extruder_color_icon(hex, label, FromDIP(20), FromDIP(20))) {
            category.filament_color_chip->SetBitmap(*chip);
            category.filament_color_chip->SetMinSize(chip->GetSize());
            category.filament_color_chip->InvalidateBestSize();
        }
    }
    const SectionGroup& section = m_sections[category.group];
    const auto          item    = std::find(section.categories.begin(), section.categories.end(), category_index);
    if (item != section.categories.end())
        if (wxBitmap* chip = get_extruder_color_icon(hex, label, FromDIP(20), FromDIP(20)))
            m_sections[category.group].tabs->SetItemBitmap(static_cast<unsigned int>(item - section.categories.begin()), *chip);

    // The payload's identity colour is the colour the badge shows.
    Slic3r::PublishOwnerInfo info;
    const auto               registered = m_selection.owners().find(int(category.filament_slot));
    if (registered != m_selection.owners().end())
        info = registered->second;
    info.colour = hex;
    m_selection.set_owner_info(int(category.filament_slot), std::move(info));
}

void PublishSettingsDialog::on_filament_color_picked(size_t slot)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    Category*     category = bundle != nullptr ? material_category(slot) : nullptr;
    if (category == nullptr)
        return;

    // The sidebar's own colour picker, over the colour the badge shows now.
    FilamentColorPickerValue value;
    if (!category->pending_colours.empty()) {
        value.colors   = category->pending_colours;
        value.gradient = category->pending_gradient;
        value.primary  = category->pending_colours.front();
    } else {
        value = slot_colour_value(bundle->project_config, slot);
        // A picked preset's own colour is what the badge shows, so the picker opens on it.
        const std::string badge_hex = staged_badge_color(*category, bundle->full_config());
        if (!badge_hex.empty() && badge_hex != value.primary) {
            value.colors.assign(1, badge_hex);
            value.primary  = badge_hex;
            value.gradient = false;
        }
    }
    const ColorSelection initial = filament_color_picker_initial(value);
    // Parented to the dialog and anchored at the badge, as the sidebar opens it from its combo.
    ColorPickerDialog dialog(this, initial, { true, true }, !filament_color_picker_selection(value),
                             category->filament_color_chip);
    if (dialog.ShowModal() != wxID_OK || !dialog.selection().has_value() ||
        !filament_color_picker_changed(initial, dialog.selection()))
        return;
    const FilamentColorPickerValue picked = filament_color_picker_result(*dialog.selection());
    if (picked.colors.empty())
        return;

    const size_t category_index = size_t(category - m_categories.data());
    // The colour is project state, and the project carries it now; the badged value stays beside the
    // picker as the value the badge and the payload identity show.
    category->pending_colours  = picked.colors;
    category->pending_gradient = picked.gradient;
    if (write_project_slot_colour(bundle->project_config, slot, picked.colors, picked.gradient)) {
        if (Plater* plater = wxGetApp().plater()) {
            // The sidebar's own colour handler pushes the changed keys immediately: the plater's
            // diff repaints the object list and the scene, and the slot's combo repaints its
            // swatch. Without both, the dialog's chips move but the main GUI keeps the old colour
            // until some later config event.
            DynamicPrintConfig colour_cfg;
            for (const char* key : { "filament_colour", "filament_multi_colour", "filament_colour_type" })
                if (const ConfigOption* opt = bundle->project_config.option(key))
                    colour_cfg.set_key_value(key, opt->clone());
            if (!colour_cfg.keys().empty())
                plater->on_config_change(colour_cfg);

            Sidebar& sidebar = plater->sidebar();
            std::vector<PlaterPresetComboBox*>& combos = sidebar.combos_filament();
            if (slot < combos.size() && combos[slot] != nullptr)
                combos[slot]->update();

            // A colour change invalidates the AMS colour cache and the flush volumes.
            auto* evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
            evt->SetInt(int(slot));
            wxQueueEvent(plater, evt);
        }
    }
    // A picker or colour change always refreshes the plater, without waiting for the idle.
    flush_live_refresh();
    refresh_slot_colour(category_index);
}

void PublishSettingsDialog::on_filament_picked(size_t slot, const std::string& preset_name)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || preset_name.empty())
        return;
    Category* category = material_category(slot);
    if (category == nullptr)
        return;
    // A name no preset answers is not a pick: keep the slot as it is rather than fall back to the
    // first visible preset the rebuilt tab would seed from.
    const Preset* found = bundle->filaments.find_preset(preset_name, false, true);
    if (found == nullptr) {
        if (category->filament_picker != nullptr)
            category->filament_picker->show_preset(category->pending_filament.empty() ? category->filament : category->pending_filament);
        return;
    }
    if (preset_name == category->pending_filament)
        return;
    // Selecting the picked preset below can rebuild the collection, so the page keeps its own copy
    // of the preset it draws.
    const Preset picked_copy = *found;
    const Preset* picked     = &picked_copy;

    const size_t category_index = size_t(category - m_categories.data());
    // The old preset's rows describe another filament: drop them and uncheck what replaces them.
    remove_category_rows(category_index);
    category = &m_categories[category_index];
    // The picked preset becomes the slot's preset now: the project is the truth, and the page and
    // the badge are rebuilt from it. Another preset's declared colour replaces an explicit pick.
    const bool was_support = is_support_filament(int(slot));
    bundle->set_filament_preset(slot, preset_name);
    // A one-slot project exports through full_fff_config's edited-preset branch, which the slot
    // list cannot reach: select the picked preset as the sidebar's own pick does, or the export
    // carries the previous filament.
    if (bundle->filament_presets.size() <= 1) {
        if (Tab* filament_tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT))
            filament_tab->select_preset(preset_name);
    }
    category->filament         = bundle->filament_presets[slot];
    category->pending_filament = preset_name;
    category->pending_colours.clear();
    category->pending_gradient = false;

    // The picked preset declares the slot's colour when the badge holds no explicit pick.
    bool recoloured = false;
    if (const std::string colour = picked->config.opt_string("default_filament_colour", 0u); !colour.empty())
        recoloured = write_project_slot_colour(bundle->project_config, slot, {colour}, false);

    // The variant columns of every slot follow the assigned presets, this one's included.
    refresh_material_layout();

    // Swap the slot's tab for one built from the picked preset, on the same host.
    if (Tab* old_tab = category->publish_tab) {
        old_tab->m_on_variant_changed = nullptr;
        old_tab->m_on_rules_applied   = nullptr;
        old_tab->m_on_publish_edit    = nullptr;
        category->publish_host->set_publish_tab(nullptr);
        old_tab->Destroy();
        category->publish_tab = nullptr;
    }
    build_material_tab(*category);
    add_material_tab_rows(category_index);
    bind_publish_tab(category_index);

    // The payload's identity is the picked preset's, not the assigned one's.
    auto preset_value = [picked](const char* key) {
        const auto* values = picked->config.opt<ConfigOptionStrings>(key);
        return values != nullptr && !values->values.empty() ? values->values.front() : std::string();
    };
    const std::string type   = preset_value("filament_type");
    const std::string vendor = preset_value("filament_vendor");
    category->filament_type   = type;
    category->filament_vendor = vendor;
    category->filament_id     = picked->filament_id;

    Slic3r::PublishOwnerInfo owner_info;
    const auto               registered = m_selection.owners().find(int(slot));
    if (registered != m_selection.owners().end())
        owner_info = registered->second;
    owner_info.filament_type   = category->filament_type;
    owner_info.filament_vendor = category->filament_vendor;
    owner_info.filament_id     = category->filament_id;
    owner_info.setting_id      = picked->setting_id;
    owner_info.preset_name     = picked->name;
    m_selection.set_owner_info(int(slot), std::move(owner_info));

    const wxString title = material_title_for(picked, type);
    category->title      = title;
    if (category->title_label != nullptr)
        category->title_label->SetLabel(title);
    const SectionGroup& section = m_sections[category->group];
    const auto          item    = std::find(section.categories.begin(), section.categories.end(), category_index);
    if (item != section.categories.end())
        m_sections[category->group].tabs->SetItemText(static_cast<unsigned int>(item - section.categories.begin()), title);
    // The tab strip and the header show the picked filament; the badge beside the picker shows the
    // picked preset's declared colour, and the tab strip's chip follows it, so a slot never shows
    // two colours.
    refresh_slot_colour(category_index);
    for (const size_t r : category->rows)
        m_rows[r].category = title;

    // Q1: the picked preset may not define the variant the printer needs.
    if (category->variant_notice != nullptr) {
        const std::string notice = publish_variant_notice(m_snapshot, slot);
        category->variant_notice->SetLabel(from_u8(notice));
        if (!notice.empty())
            category->variant_notice->Wrap(FromDIP(460));
        category->variant_notice->Show(!notice.empty());
    }

    // Every replacement row starts unchecked: the old checks named another preset. The material
    // checkbox and Full Publish stay as the owner set them, and Full Publish keeps the rows
    // unchecked-out.
    const bool full = m_selection.full_publish(int(slot));
    for (const size_t r : category->rows) {
        set_row_checked(r, false);
        set_row_enabled(r, !full);
    }

    // Filter and gating run the same rules as on any other row set.
    if (m_filter_mode != FilterMode::Text || !m_filter_ctrl->GetValue().IsEmpty())
        refresh_filter(m_filter_ctrl->GetValue().Lower());
    else {
        for (const size_t r : category->rows)
            m_rows[r].matches_filter = true;
        apply_visibility();
    }
    apply_row_label_colours();
    apply_row_field_gating(category_index);
    if (category->page != nullptr)
        category->page->GetSizer()->Layout();
    refresh_tab_indicators();

    // The sidebar's own slot assignment, run live: export the selection, rebuild the dynamic
    // filament list, then update the slot's combo and the dependent UI.
    bundle->export_selections(*wxGetApp().app_config);
    if (Plater* plater = wxGetApp().plater()) {
        Sidebar& sidebar = plater->sidebar();
        sidebar.update_dynamic_filament_list();
        // Fetch the combo after update_dynamic_filament_list(), which may rebuild the vector.
        std::vector<PlaterPresetComboBox*>& combos = sidebar.combos_filament();
        if (slot < combos.size() && combos[slot] != nullptr)
            combos[slot]->update();
        plater->on_filament_change(slot);
        // Assigning (or leaving) a support filament changes which filaments the flush rules cover,
        // so the normal flow recalculates the volumes.
        if ((is_support_filament(int(slot)) != was_support) &&
            wxGetApp().app_config->get("auto_calculate_flush") == "all")
            sidebar.auto_calc_flushing_volumes(int(slot));
        // A pick always refreshes the plater; the slot's colour changed with it.
        if (recoloured) {
            auto* evt = new wxCommandEvent(EVT_FILAMENT_COLOR_CHANGED);
            evt->SetInt(int(slot));
            wxQueueEvent(plater, evt);
        }
        flush_live_refresh();
    }
}

size_t PublishSettingsDialog::add_tab_row(Row row, size_t category_index)
{
    Category& category  = m_categories[category_index];
    row.category        = category.title;
    row.section_title   = m_sections[category.group].title;
    row.outer_index     = category.group;
    row.inner_index     = category_index;
    const size_t row_index = m_rows.size();
    m_rows.push_back(std::move(row));
    m_row_ids.push_back(m_rows.back().id);
    category.rows.push_back(row_index);
    return row_index;
}

void PublishSettingsDialog::add_material_tab_rows(size_t category_index)
{
    Category& category = m_categories[category_index];
    Tab*      tab      = category.publish_tab;
    if (tab == nullptr)
        return;

    const std::set<std::string>& denylist = publish_structural_keys();
    const std::set<std::string>& identity = publish_material_identity_keys();
    const int                    slot     = int(category.filament_slot);
    // One publish key per slot: a variant key is "base#file_column", a per-slot key the plain base.
    std::set<std::string> variant_added;
    std::set<std::string> slot_added;

    // Index this slot's config entry, the way the export path masks it.
    auto index_in_range = [this](const std::string& key, int index) {
        const ConfigOption* opt = m_snapshot.option(key);
        if (opt == nullptr)
            return false;
        if (index < 0)
            return true;
        const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(opt);
        return values == nullptr || size_t(index) < values->size();
    };

    for (const PageShp& page : tab->m_pages) {
        // Dependencies and Notes hold no publishable setting.
        if (page->title() == "Dependencies" || page->title() == "Notes")
            continue;
        for (const ConfigOptionsGroupShp& group : page->m_optgroups) {
            bool touched = false;
            for (const Line& src : group->get_lines()) {
                if (src.is_separator() || src.get_options().empty())
                    continue;

                // Every option of one Line is one selection unit, drawn by the Line itself. A
                // variant-bearing option exists once per file column; the others once per slot.
                std::vector<std::string> variant_options;
                std::vector<RowOption>   slot_options;
                for (const Option& option : src.get_options()) {
                    const std::string base = publish_base_key(option.opt_id);
                    if (denylist.count(base) != 0 || identity.count(base) != 0 || is_publish_action(option.opt))
                        continue;
                    // Without a variant layout the key stays slot-indexed, as the legacy export
                    // path masks it.
                    if (publish_is_variant_bearing(base) && !category.columns.empty()) {
                        if (std::find(variant_options.begin(), variant_options.end(), base) == variant_options.end())
                            variant_options.push_back(base);
                        continue;
                    }
                    if (!index_in_range(base, slot))
                        continue;
                    if (std::none_of(slot_options.begin(), slot_options.end(),
                                     [&base](const RowOption& o) { return o.key == base; }))
                        slot_options.push_back({base, slot});
                }
                if (variant_options.empty() && slot_options.empty())
                    continue;

                auto fill_row = [&src, &group, slot](Row& row) {
                    row.label       = src.label;
                    row.subcategory = _(group->title); // filter text only
                    row.id.owner    = slot;
                    row.id.section  = Slic3r::PublishSection::Material;
                    row.id.kind     = Slic3r::PublishRowKind::Key;
                };
                auto add_values = [this](Row& row) {
                    // The first publish key names the row in the filter. A per-slot key has no
                    // "#N" suffix, so read the value at the row's own index instead of the
                    // vector's first entry.
                    const RowOption& first = row.options.front();
                    std::string      key   = row.id.keys.front();
                    if (key.find('#') == std::string::npos && first.index >= 0)
                        key += "#" + std::to_string(first.index);
                    row.value = get_string_value(key, m_snapshot);
                    if (const ConfigOptionDef* def = print_config_def.get(first.key)) {
                        row.unit = _(def->sidetext);
                        if (row.unit == "%" && row.value.EndsWith("%"))
                            row.unit.clear();
                    }
                };

                // A variant-bearing line becomes one row per file column of the slot; a per-slot
                // option in the same line rides along on every column's row.
                std::vector<Row> rows;
                if (!variant_options.empty()) {
                    for (const size_t column : category.columns) {
                        Row                      row;
                        std::vector<std::string> keys;
                        fill_row(row);
                        row.id.column = int(column);
                        bool ok = true;
                        for (const std::string& base : variant_options) {
                            const std::string key = base + "#" + std::to_string(column);
                            if (variant_added.count(key) != 0)
                                continue;
                            if (!index_in_range(base, int(column))) {
                                ok = false;
                                break;
                            }
                            keys.push_back(key);
                            row.options.push_back({base, int(column)});
                        }
                        if (!ok || row.options.empty())
                            continue;
                        variant_added.insert(keys.begin(), keys.end());
                        row.id.keys = std::move(keys);
                        for (const RowOption& option : slot_options) {
                            row.options.push_back(option);
                            row.id.keys.push_back(option.key);
                        }
                        add_values(row);
                        rows.push_back(std::move(row));
                    }
                    // Every variant key of this line was already taken by an earlier line: the
                    // line still owns its per-slot options, which would otherwise vanish. Emit
                    // them once, at slot level.
                    if (rows.empty() && !slot_options.empty()) {
                        Row row;
                        fill_row(row);
                        row.id.column = -1;
                        for (const RowOption& option : slot_options) {
                            if (!slot_added.insert(option.key).second)
                                continue;
                            row.options.push_back(option);
                            row.id.keys.push_back(option.key);
                        }
                        if (!row.options.empty()) {
                            add_values(row);
                            rows.push_back(std::move(row));
                        }
                    }
                } else {
                    Row row;
                    fill_row(row);
                    row.id.column = -1;
                    for (const RowOption& option : slot_options) {
                        if (!slot_added.insert(option.key).second)
                            continue;
                        row.options.push_back(option);
                        row.id.keys.push_back(option.key);
                    }
                    if (row.options.empty())
                        continue;
                    add_values(row);
                    rows.push_back(std::move(row));
                }
                if (rows.empty())
                    continue;

                // The checkbox the Tab's own line carries, once per line: it draws the row of the
                // column the tab's switch currently selects. Use the line being walked, not a
                // fresh lookup by key: a key can bind another line of this group, or none after a
                // per-slot key was masked. The vector is not appended during the walk.
                Line* line = const_cast<Line*>(&src);
                if (!line->row_toggle)
                    line->row_toggle = RowToggle();
                line->row_toggle->on_change = [this, category_index, line](bool checked) {
                    if (const size_t r = tab_row_for_line(category_index, line); r != size_t(-1))
                        set_row_checked(r, checked);
                };
                touched = true;

                for (Row& row : rows) {
                    row.tab_line = line;
                    add_tab_row(std::move(row), category_index);
                }
            }
            // The toggles added a column to the group's rows: re-align the value and unit columns.
            if (touched)
                group->msw_rescale();
        }
    }
}

void PublishSettingsDialog::add_printer_tab_rows(size_t category_index)
{
    Category&     category = m_categories[category_index];
    Tab*          tab      = category.publish_tab;
    PresetBundle* bundle   = wxGetApp().preset_bundle;
    if (tab == nullptr || bundle == nullptr)
        return;

    const DynamicPrintConfig& printer_config = bundle->printers.get_edited_preset().config;

    // The columns the tab's extruder switch can show, from the same rule the export side reads.
    // Retraction and Z-Hop values are stored per variant column, not per extruder, so an extruder
    // whose column this printer preset does not define contributes none.
    std::vector<size_t> columns;
    for (size_t extruder = 0; extruder < size_t(bundle->get_printer_extruder_count()); ++extruder)
        for (const Slic3r::PublishExtruderEntry& entry :
             publish_extruder_rule(printer_config, printer_config, bundle->project_config, extruder,
                                   /*split_hybrid=*/false, "printer_extruder_id", "printer_extruder_variant"))
            if (std::find(columns.begin(), columns.end(), entry.column) == columns.end())
                columns.push_back(entry.column);
    if (columns.empty())
        return; // no column this preset defines: nothing to key a row by
    category.column = int(columns.front());

    const std::set<std::string>& allow    = publishable_printer_keys();
    const std::set<std::string>& denylist = publish_structural_keys();
    std::set<std::string>        added; // "base#column" rows already added

    auto index_in_range = [this](const std::string& key, int index) {
        const ConfigOption* opt = m_snapshot.option(key);
        if (opt == nullptr)
            return false;
        if (index < 0)
            return true;
        const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(opt);
        return values == nullptr || size_t(index) < values->size();
    };

    for (const PageShp& page : tab->m_pages) {
        for (const ConfigOptionsGroupShp& group : page->m_optgroups) {
            bool touched = false;
            for (const Line& src : group->get_lines()) {
                if (src.is_separator() || src.get_options().empty())
                    continue;

                // Every option of one Line is one selection unit; each exists once per column.
                std::vector<RowOption> options;
                for (const Option& option : src.get_options()) {
                    const std::string base = publish_base_key(option.opt_id);
                    if (allow.count(base) == 0 || denylist.count(base) != 0 || is_publish_action(option.opt))
                        continue;
                    if (std::none_of(options.begin(), options.end(), [&base](const RowOption& o) { return o.key == base; }))
                        options.push_back({base, -1});
                }
                if (options.empty())
                    continue;

                Line*        line      = const_cast<Line*>(&src);
                const size_t first_row = m_rows.size();
                for (const size_t column : columns) {
                    Row row;
                    row.label       = src.label;
                    row.subcategory = _(group->title); // filter text only
                    row.id.owner    = -1;
                    row.id.column   = int(column);
                    row.id.section  = Slic3r::PublishSection::Printer;
                    row.id.kind     = Slic3r::PublishRowKind::Key;

                    bool                     row_ok = true;
                    std::vector<std::string> keys;
                    for (const RowOption& option : options) {
                        const std::string key = option.key + "#" + std::to_string(column);
                        if (added.count(key) != 0 || !index_in_range(option.key, int(column))) {
                            row_ok = false;
                            break;
                        }
                        keys.push_back(key);
                        row.options.push_back({option.key, int(column)});
                    }
                    if (!row_ok || row.options.empty())
                        continue;
                    added.insert(keys.begin(), keys.end());
                    row.id.keys = std::move(keys);

                    // The first option's value and unit name the row in the filter.
                    const RowOption& first = row.options.front();
                    row.value = get_string_value(first.key + "#" + std::to_string(first.index), m_snapshot);
                    if (const ConfigOptionDef* def = print_config_def.get(first.key)) {
                        row.unit = _(def->sidetext);
                        if (row.unit == "%" && row.value.EndsWith("%"))
                            row.unit.clear();
                    }
                    row.tab_line = line;
                    add_tab_row(std::move(row), category_index);
                }
                // Only a line that produced at least one row gets a checkbox: a line whose every
                // column failed would otherwise toggle nothing.
                if (m_rows.size() > first_row) {
                    if (!line->row_toggle)
                        line->row_toggle = RowToggle();
                    line->row_toggle->on_change = [this, category_index, line](bool checked) {
                        if (const size_t r = tab_row_for_line(category_index, line); r != size_t(-1))
                            set_row_checked(r, checked);
                    };
                    touched = true;
                }
            }
            // The toggles added a column to the group's rows: re-align the value and unit columns.
            if (touched)
                group->msw_rescale();
        }
    }
}

void PublishSettingsDialog::add_print_tab_rows(size_t category_index)
{
    Category&     category = m_categories[category_index];
    Tab*          tab      = category.publish_tab;
    PresetBundle* bundle   = wxGetApp().preset_bundle;
    if (tab == nullptr || bundle == nullptr)
        return;

    // The columns the Process tab's extruder switch can show, from the same rule the export side
    // reads. The switch is populated only when several extruders run different hardware, and a
    // hybrid extruder then shows one entry per sub-nozzle type; otherwise there is one page.
    const bool                split_hybrid   = bundle->get_printer_extruder_count() >= 2 && bundle->support_different_extruders();
    const DynamicPrintConfig& print_config   = bundle->prints.get_edited_preset().config;
    const DynamicPrintConfig& printer_config = bundle->printers.get_edited_preset().config;
    const int                 extruders      = split_hybrid ? bundle->get_printer_extruder_count() : 1;
    std::vector<size_t>       columns;
    for (int extruder = 0; extruder < extruders; ++extruder)
        for (const Slic3r::PublishExtruderEntry& entry :
             publish_extruder_rule(print_config, printer_config, bundle->project_config, size_t(extruder), split_hybrid,
                                   "print_extruder_id", "print_extruder_variant"))
            if (std::find(columns.begin(), columns.end(), entry.column) == columns.end())
                columns.push_back(entry.column);
    if (!columns.empty())
        category.column = int(columns.front());

    const std::set<std::string>& denylist = publish_structural_keys();
    std::set<std::string>        variant_added; // "base#column" rows already added
    std::set<std::string>        added;         // plain rows already added, by base key

    auto index_in_range = [this](const std::string& key, int index) {
        const ConfigOption* opt = m_snapshot.option(key);
        if (opt == nullptr)
            return false;
        if (index < 0)
            return true;
        const auto* values = dynamic_cast<const ConfigOptionVectorBase*>(opt);
        return values == nullptr || size_t(index) < values->size();
    };

    for (const PageShp& page : tab->m_pages) {
        // The page title is the row's filter category.
        const wxString page_title = Tab::translate_category(page->title(), tab->type());
        for (const ConfigOptionsGroupShp& group : page->m_optgroups) {
            bool touched = false;
            for (const Line& src : group->get_lines()) {
                if (src.is_separator() || src.get_options().empty())
                    continue;

                // A line made only of per-variant settings is shown once per extruder column, as
                // the sidebar's extruder switch shows it; every other line publishes its keys whole.
                std::vector<RowOption> options; // key, its own line index (-1 without a '#N')
                bool                   per_variant = !columns.empty();
                for (const Option& option : src.get_options()) {
                    const std::string     base = publish_base_key(option.opt_id);
                    const ConfigOptionDef* def  = print_config_def.get(base);
                    // Unknown, structural or action options carry no publish checkbox, and a row
                    // needs a label to filter on.
                    if (def == nullptr || denylist.count(base) != 0 || is_publish_action(option.opt))
                        continue;
                    if (_(def->full_label.empty() ? def->label : def->full_label).IsEmpty())
                        continue;
                    if (std::none_of(options.begin(), options.end(), [&base](const RowOption& o) { return o.key == base; }))
                        options.push_back({base, publish_key_index(option.opt_id)});
                    per_variant = per_variant && print_options_with_variant.count(base) != 0;
                }
                if (options.empty())
                    continue;

                Line* line = const_cast<Line*>(&src);

                std::vector<Row> rows;
                if (per_variant) {
                    for (const size_t column : columns) {
                        Row                      row;
                        bool                     row_ok = true;
                        std::vector<std::string> keys;
                        for (const RowOption& option : options) {
                            const std::string key = option.key + "#" + std::to_string(column);
                            if (variant_added.count(key) != 0 || !index_in_range(option.key, int(column))) {
                                row_ok = false;
                                break;
                            }
                            keys.push_back(key);
                            row.options.push_back({option.key, int(column)});
                        }
                        if (!row_ok || row.options.empty())
                            continue;
                        variant_added.insert(keys.begin(), keys.end());
                        row.id.keys    = std::move(keys);
                        row.id.column  = int(column);
                        rows.push_back(std::move(row));
                    }
                } else {
                    Row                      row;
                    bool                     row_ok = true;
                    std::vector<std::string> keys;
                    for (const RowOption& option : options) {
                        // A per-variant option inside a mixed line keeps the key its line gave it,
                        // so the payload names the same setting it always did.
                        const std::string key = option.key + (option.index >= 0 ? "#" + std::to_string(option.index) : "");
                        if (!added.insert(option.key).second || !index_in_range(option.key, option.index)) {
                            row_ok = false;
                            break;
                        }
                        keys.push_back(key);
                        row.options.push_back(option);
                    }
                    if (row_ok && !row.options.empty()) {
                        row.id.keys   = std::move(keys);
                        row.id.column = -1;
                        rows.push_back(std::move(row));
                    }
                }
                if (rows.empty())
                    continue;

                // Only a line that produced at least one row gets a checkbox: a line whose every
                // column failed would otherwise toggle nothing.
                if (!line->row_toggle)
                    line->row_toggle = RowToggle();
                line->row_toggle->on_change = [this, category_index, line](bool checked) {
                    if (const size_t r = tab_row_for_line(category_index, line); r != size_t(-1))
                        set_row_checked(r, checked);
                };
                touched = true;

                for (Row& row : rows) {
                    row.label       = src.label;
                    row.subcategory = _(group->title); // filter text only
                    row.id.owner    = -1;
                    row.id.section  = Slic3r::PublishSection::Process;
                    row.id.kind     = Slic3r::PublishRowKind::Key;

                    // The first option's value and unit name the row in the filter.
                    const RowOption& first = row.options.front();
                    row.value              = get_string_value(row.id.keys.front(), m_snapshot);
                    if (const ConfigOptionDef* def = print_config_def.get(first.key)) {
                        row.unit = _(def->sidetext);
                        if (row.unit == "%" && row.value.EndsWith("%"))
                            row.unit.clear();
                    }
                    row.tab_line             = line;
                    const size_t added_index = add_tab_row(std::move(row), category_index);
                    m_rows[added_index].category = page_title;
                }
            }
            // The toggles added a column to the group's rows: re-align the value and unit columns.
            if (touched)
                group->msw_rescale();
        }
    }
}

bool PublishSettingsDialog::tab_line_visible(const Tab& tab, const Row& row) const
{
    // A Tab hides a line through toggle_line(), which records the reason per option key: a row is
    // visible only while none of its options is hidden.
    const OptionStateRegistry& registry = tab.option_state_registry();
    return std::none_of(row.options.begin(), row.options.end(),
                        [&registry](const RowOption& option) { return registry.hidden_reason(option.key) != nullptr; });
}

size_t PublishSettingsDialog::tab_row_for_line(size_t category_index, const Line* line) const
{
    for (const size_t r : m_categories[category_index].rows)
        if (m_rows[r].tab_line == line && row_on_selected_variant(r))
            return r;
    return size_t(-1);
}

Line* PublishSettingsDialog::row_line(size_t row_index) const
{
    return m_rows[row_index].tab_line;
}

int PublishSettingsDialog::tab_active_column(Tab* tab) const
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (tab == nullptr || bundle == nullptr)
        return -1;

    // The tab's switch remembers the column switch_excluder() resolved on its last successful
    // switch; that is the column whose row its Line draws.
    if (const int resolved = tab->last_resolved_column(); resolved >= 0)
        return resolved;

    // The tab has no switch, or never resolved a column: derive it from the rule. The switch
    // names the extruder (and, for a hybrid, the sub-nozzle type) it shows; the rule then names
    // that entry's column. Without a switch there is one extruder, and the rule's single entry is
    // its column.
    int              extruder  = 0;
    NozzleVolumeType nozzle    = NozzleVolumeType::nvtStandard;
    const int        selection = tab->switch_selection();
    if (selection >= 0)
        tab->parse_extruder_selection(selection, extruder, nozzle);

    const bool                process        = tab->type() == Preset::TYPE_PRINT;
    // The Process tab's switch lists a hybrid extruder as two tags (Standard, then High Flow);
    // the Printer tab keeps one tag per extruder and reads the Standard column.
    const bool                split_hybrid   = process && bundle->get_printer_extruder_count() >= 2 && bundle->support_different_extruders();
    const DynamicPrintConfig& target         = process ? bundle->prints.get_edited_preset().config :
                                                         bundle->printers.get_edited_preset().config;
    const DynamicPrintConfig& printer_config = bundle->printers.get_edited_preset().config;
    const std::vector<Slic3r::PublishExtruderEntry> entries =
        publish_extruder_rule(target, printer_config, bundle->project_config, size_t(extruder), split_hybrid,
                              process ? "print_extruder_id" : "printer_extruder_id",
                              process ? "print_extruder_variant" : "printer_extruder_variant");
    if (entries.empty())
        return -1;
    if (split_hybrid)
        for (const Slic3r::PublishExtruderEntry& entry : entries)
            if (entry.nozzle_type == int(nozzle))
                return int(entry.column);
    return int(entries.front().column);
}

bool PublishSettingsDialog::row_on_selected_variant(size_t row_index) const
{
    const Row& row = m_rows[row_index];
    if (row.id.column < 0 || row.inner_index >= m_categories.size())
        return true; // slot-level rows belong to every variant
    const Category& category = m_categories[row.inner_index];
    // The hosted Printer and Process tabs show one extruder entry at a time: the switch decides
    // which column's row the shared Line draws.
    if (row.id.section == Slic3r::PublishSection::Printer || row.id.section == Slic3r::PublishSection::Process) {
        if (category.publish_tab == nullptr)
            return true;
        // The tab's switch remembers the column switch_excluder() last resolved; a tab with no
        // switch falls back to the column the page started on.
        const int column = tab_active_column(category.publish_tab);
        return row.id.column == (column >= 0 ? column : category.column);
    }
    if (row.id.section != Slic3r::PublishSection::Material)
        return true;
    // The tab's switch selects the local variant; keys carry the file column it maps to.
    if (category.publish_tab != nullptr) {
        if (const std::optional<size_t> column =
                publish_filament_local_to_file(m_snapshot, category.filament_slot, category.publish_tab->active_variant_index());
            column.has_value())
            return row.id.column == int(*column);
    }
    return row.id.column == category.column;
}

void PublishSettingsDialog::apply_row_label_colour(size_t row_index)
{
    const Row& row = m_rows[row_index];
    if (row.tab_line == nullptr || row.options.empty())
        return;
    // The Tab draws and colours the row. Only the row the tab's switch shows paints the Line they
    // all share.
    if (row.inner_index >= m_categories.size() || !row_on_selected_variant(row_index))
        return;
    const Tab* tab = m_categories[row.inner_index].publish_tab;
    if (tab == nullptr)
        return;
    // A dirty pre-checked Printer/Process (or material) row marks its line and fields with the
    // Tab's modified colour; the line returns to the Tab's default colour when not dirty.
    const wxColour* colour = row.dirty ? &tab->modified_label_colour() : &tab->default_text_colour();
    row.tab_line->set_label_colour(colour);
    // The label is drawn from the first option's field colour when the line has a field. Paint the
    // fields the Line actually owns: a row's column index is a file column, not the "#N" id the tab
    // gave the field (the switch re-targets one built field).
    if (row.dirty)
        for (const Option& option : row.tab_line->get_options())
            if (Field* field = tab->get_field(option.opt_id))
                field->set_label_colour(colour);
}

void PublishSettingsDialog::apply_row_label_colours()
{
    for (size_t r = 0; r < m_rows.size(); ++r)
        apply_row_label_colour(r);
}

void PublishSettingsDialog::sync_row_widgets(size_t row_index)
{
    // One Line carries one checkbox and draws the row of the variant the tab shows, so only that
    // row's state may reach it.
    if (!row_on_selected_variant(row_index))
        return;
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
}

void PublishSettingsDialog::set_row_checked(size_t row_index, bool checked)
{
    Row& row = m_rows[row_index];
    m_selection.set_checked(row.id, checked);
    sync_row_widgets(row_index);
    // The row's value is editable exactly while it is checked and writable. A row of another
    // column than the one the tab shows does not draw its line, so it leaves it alone.
    apply_line_field_gating(row_index);
}

void PublishSettingsDialog::set_row_enabled(size_t row_index, bool enabled)
{
    Row& row = m_rows[row_index];
    m_selection.set_enabled(row.id, enabled && row.publishable);
    sync_row_widgets(row_index);
}

bool PublishSettingsDialog::row_writable(size_t row_index) const
{
    const Row& row = m_rows[row_index];
    if (row.tab_line == nullptr || row.inner_index >= m_categories.size())
        return false;
    const Category& category = m_categories[row.inner_index];
    if (category.publish_tab == nullptr || category.publish_tab->m_config == nullptr)
        return false;
    const bool full = row.id.section == Slic3r::PublishSection::Material &&
                      m_selection.full_publish(int(category.filament_slot));
    return Slic3r::publish_write_location(row.id, m_snapshot, *category.publish_tab->m_config, full).target !=
           Slic3r::PublishWriteTarget::None;
}

void PublishSettingsDialog::apply_line_field_gating(size_t row_index)
{
    const Row& row = m_rows[row_index];
    if (row.tab_line == nullptr || row.inner_index >= m_categories.size())
        return;
    if (!row_on_selected_variant(row_index))
        return; // the line draws another column's row just now
    Tab* tab = m_categories[row.inner_index].publish_tab;
    if (tab == nullptr)
        return;
    const bool editable = m_selection.checked(row.id) && row_writable(row_index);
    // Only the options this row publishes: a line may also carry a value the dialog does not publish,
    // and that one must stay inert.
    for (const RowOption& published : row.options)
        for (const Option& option : row.tab_line->get_options())
            if (Slic3r::publish_base_key(option.opt_id) == published.key)
                tab->set_publish_field_enabled(option.opt_id, editable);
}

void PublishSettingsDialog::apply_row_field_gating(size_t category_index)
{
    if (category_index >= m_categories.size())
        return;
    const Category& category = m_categories[category_index];
    Tab*           tab       = category.publish_tab;
    if (tab == nullptr)
        return;
    // A Full Publish slot copies its preset whole: no field of it is edited.
    if (category.section == Section::Material && m_selection.full_publish(int(category.filament_slot))) {
        tab->set_publish_fields_enabled(false);
        return;
    }
    for (const size_t r : category.rows)
        apply_line_field_gating(r);
}

void PublishSettingsDialog::bind_publish_tab(size_t category_index)
{
    if (category_index >= m_categories.size())
        return;
    Tab* tab = m_categories[category_index].publish_tab;
    if (tab == nullptr)
        return;
    // The switch decides which column's row a checkbox paints; the tab handles its own switch
    // first (values, greying), then this repaints the publish controls.
    tab->m_on_variant_changed = [this, category_index](int) {
        sync_all_row_widgets();
        apply_row_field_gating(category_index);
        apply_visibility();
        apply_row_label_colours();
    };
    // The tab rewrites its lines' visibility and its label colours on a page switch; re-apply the
    // filter and the dirty markers on top.
    tab->m_on_rules_applied = [this, category_index] {
        apply_row_field_gating(category_index);
        apply_visibility();
        apply_row_label_colours();
    };
    // Every edit reaches the project at once, through the row it is drawn by (spec 7.10).
    tab->m_on_publish_edit = [this, category_index](const std::string& opt_id, const boost::any& value) {
        on_publish_edit(category_index, opt_id, value);
    };
}

void PublishSettingsDialog::schedule_live_refresh()
{
    // Debounce: a keystroke restarts the timer, so a burst of edits costs one plater refresh.
    m_live_refresh_timer.StartOnce(250);
}

void PublishSettingsDialog::flush_live_refresh()
{
    m_live_refresh_timer.Stop();
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    // The project's dirty state follows the presets; the plater re-reads the whole config so the
    // scene, the slice and the mixed-filament list agree with what the dialog wrote.
    plater->update_project_dirty_from_presets();
    plater->on_config_change(wxGetApp().preset_bundle->full_config());
}

size_t PublishSettingsDialog::row_for_edit(size_t category_index, const std::string& opt_id) const
{
    if (category_index >= m_categories.size())
        return size_t(-1);
    const std::string base = Slic3r::publish_base_key(opt_id);
    // One Line may draw the rows of several variant columns: only the row of the column the tab
    // shows is the one the edited field belongs to.
    for (const size_t r : m_categories[category_index].rows) {
        const Row& row = m_rows[r];
        if (row.tab_line == nullptr || !row_on_selected_variant(r))
            continue;
        const auto owns = std::any_of(row.tab_line->get_options().begin(), row.tab_line->get_options().end(),
                                      [&base](const Option& option) {
                                          return Slic3r::publish_base_key(option.opt_id) == base;
                                      });
        if (!owns)
            continue;
        for (const RowOption& option : row.options)
            if (option.key == base)
                return r;
    }
    return size_t(-1);
}

void PublishSettingsDialog::on_publish_edit(size_t category_index, const std::string& opt_id, const boost::any& value)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr || category_index >= m_categories.size())
        return;
    Category& category = m_categories[category_index];
    Tab*      tab      = category.publish_tab;
    if (tab == nullptr || tab->m_config == nullptr)
        return;

    const size_t row_index = row_for_edit(category_index, opt_id);
    if (row_index == size_t(-1))
        return; // a line value the dialog publishes no row for

    const Row& row = m_rows[row_index];
    // The gate: a field is editable only while its row is checked and writable. A programmatic
    // population of a disabled field must never reach the project; it is ignored, not asserted:
    // a stray event in a debug build must not abort the app.
    if (!m_selection.checked(row.id))
        return;
    if (!row_writable(row_index))
        return;

    const bool full = row.id.section == Slic3r::PublishSection::Material &&
                      m_selection.full_publish(int(category.filament_slot));
    const Slic3r::PublishWriteLocation location =
        Slic3r::publish_write_location(row.id, m_snapshot, *tab->m_config, full);
    if (location.target == Slic3r::PublishWriteTarget::None)
        return;

    const std::string base = Slic3r::publish_base_key(opt_id);
    const RowOption*  published = nullptr;
    for (const RowOption& option : row.options)
        if (option.key == base) {
            published = &option;
            break;
        }
    if (published == nullptr)
        return;
    // The element the tab's own field edits: an indexed line addresses the resolved variant, an
    // unindexed one always element 0. A material key is written at the row's local variant index;
    // a printer or process key at the column it names. The private preset and the real one are
    // seeded from the same config, so their indices match.
    const bool   indexed = row.tab_line != nullptr && line_option_indexed(*row.tab_line, published->key);
    const size_t index   = location.target == Slic3r::PublishWriteTarget::FilamentPreset
                               ? (indexed ? location.option_index : size_t(0))
                               : (published->index >= 0 ? size_t(published->index)
                                                        : (indexed ? location.option_index : size_t(0)));

    switch (location.target) {
    case Slic3r::PublishWriteTarget::FilamentPreset: {
        if (location.owner < 0 || size_t(location.owner) >= bundle->filament_presets.size())
            break;
        const std::string& name = bundle->filament_presets[size_t(location.owner)];
        bool handled = false;
        // The preset the sidebar's Filament tab shows goes through the Tab, so its dirty state,
        // validators and undo see the edit exactly as a sidebar edit.
        if (name == bundle->filaments.get_selected_preset_name())
            if (Tab* filament_tab = wxGetApp().get_tab(Preset::TYPE_FILAMENT)) {
                filament_tab->load_key_value(published->key, value, false, int(index));
                handled = true;
            }
        if (!handled) {
            // Any other slot's preset is its stored copy, which full_config() reads for that slot.
            // It is written without being selected: the sidebar tab shows the value when that
            // preset is selected.
            Preset* target = bundle->filaments.find_preset(name, false);
            if (target == nullptr ||
                !copy_option_element_at(target->config, published->key, *tab->m_config, index))
                break;
            target->set_dirty(true);
        }
        break;
    }
    case Slic3r::PublishWriteTarget::PrinterPreset:
        if (!copy_option_element_at(bundle->printers.get_edited_preset().config, published->key, *tab->m_config, index))
            break;
        if (Tab* printer_tab = wxGetApp().get_tab(Preset::TYPE_PRINTER)) {
            printer_tab->update_dirty();
            printer_tab->reload_config();
            printer_tab->update();
        }
        break;
    case Slic3r::PublishWriteTarget::ProcessPreset:
        if (!copy_option_element_at(bundle->prints.get_edited_preset().config, published->key, *tab->m_config, index))
            break;
        if (Tab* print_tab = wxGetApp().get_tab(Preset::TYPE_PRINT)) {
            print_tab->update_dirty();
            print_tab->reload_config();
            print_tab->update();
        }
        break;
    case Slic3r::PublishWriteTarget::Project:
        if (!copy_option_element_at(bundle->project_config, published->key, *tab->m_config, index))
            break;
        break;
    case Slic3r::PublishWriteTarget::None:
        return;
    }

    // The sidebar Tab of the edited preset is refreshed above; the plater's own re-read is
    // coalesced so typing does not refresh it once per keystroke.
    schedule_live_refresh();
}

void PublishSettingsDialog::on_full_toggle(size_t category_index)
{
    Category& cat   = m_categories[category_index];
    const bool full = cat.full_check->GetValue();
    m_selection.set_full_publish(int(cat.filament_slot), full);
    for (size_t r : cat.rows)
        set_row_enabled(r, !full);
    // Full Publish serializes the preset whole and publishes colour and type itself, so the
    // material checkbox has no effect while it is on; it comes back when Full is turned off.
    if (cat.owner_check != nullptr)
        cat.owner_check->Enable(!full);
    // Full Publish serializes the preset whole, so no row value is editable; unchecking it restores
    // the per-row gating.
    if (cat.publish_tab != nullptr) {
        if (full)
            cat.publish_tab->set_publish_fields_enabled(false);
        else
            apply_row_field_gating(category_index);
    }
    // The row toggles changed state; repaint the hosted rows so the greyed toggles show at once.
    apply_visibility();
    refresh_tab_indicators();
}

void PublishSettingsDialog::on_owner_toggle(size_t category_index)
{
    Category& cat        = m_categories[category_index];
    const bool published = cat.owner_check->GetValue();
    m_selection.set_owner_published(int(cat.filament_slot), published);

    // A published mixed filament needs its component filaments published too: mark the slots it
    // uses as material-published and Full Published so the mix's physical components always ship
    // their identities. A physical slot's checkbox governs only its own colour and type.
    if (cat.is_mixed && published) {
        const DynamicPrintConfig full              = wxGetApp().preset_bundle->full_config();
        const std::vector<unsigned int> components = mixed_slot_components(full, cat.filament_slot);
        for (const unsigned int component : components) {
            // Components are 1-based physical filament indices.
            const size_t component_slot = size_t(component) - 1;
            for (size_t c = 0; c < m_categories.size(); ++c) {
                Category& comp_cat = m_categories[c];
                if (comp_cat.section != Section::Material || comp_cat.is_mixed || comp_cat.filament_slot != component_slot)
                    continue;
                if (comp_cat.owner_check != nullptr) {
                    comp_cat.owner_check->SetValue(true);
                    m_selection.set_owner_published(int(component_slot), true);
                }
                if (comp_cat.full_check != nullptr) {
                    comp_cat.full_check->SetValue(true);
                    m_selection.set_full_publish(int(component_slot), true);
                }
                on_full_toggle(c);
            }
        }
    }

    // A mixed slot's hint follows its checkbox; a physical slot's tab stays shown either way.
    // Refresh the visibility of the auto-selected component slots too.
    apply_visibility();
    apply_row_field_gating(category_index);
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
    // Parented to the page right above the scroll area, so it is always shown with the tab: the
    // publish checkbox never hides this preview.
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
        // their explanatory hint.
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
        // A physical slot's tab is always shown: its rows stay read-only until checked. A mixed
        // slot's hint follows its publish checkbox.
        const bool shown = category.section != Section::Material || !category.is_mixed ||
                           (category.owner_check != nullptr && category.owner_check->GetValue());
        if (category.publish_host != nullptr)
            category.publish_host->Show(shown);
        if (category.scroll != nullptr)
            category.scroll->Show(shown);
        if (category.full_line_item != nullptr)
            category.full_line_item->Show(shown);
        if (category.filament_picker_item != nullptr)
            category.filament_picker_item->Show(shown);
        bool category_any = false;
        for (size_t r : category.rows)
            category_any = category_any || m_rows[r].matches_filter;
        category.info->Show(shown && !category_any);
        // A hosted slot's rows hide and show through the tab's own lines: the row of the column
        // the switch shows owns its line's visibility, on top of the Tab's own hide rule.
        bool tab_rows = false;
        for (size_t r : category.rows) {
            Row& row = m_rows[r];
            if (row.tab_line == nullptr || category.publish_tab == nullptr)
                continue;
            tab_rows = true;
            // Read the tab's own rule now rather than a cached copy: the filter must never make a
            // row the Tab hid reappear.
            if (row_on_selected_variant(r))
                row.tab_line->toggle_visible = row.matches_filter && tab_line_visible(*category.publish_tab, row);
        }
        if (tab_rows) {
            for (const PageShp& page : category.publish_tab->m_pages)
                for (const ConfigOptionsGroupShp& group : page->m_optgroups) {
                    // Re-run the group's own show/hide against the lines' toggle_visible, as the
                    // Tab does after its own rules.
                    group->update_visibility(comDevelop);
                    group->refresh();
                }
            // The same relayout the Tab runs after it changes a line's visibility.
            if (wxScrolledWindow* page_view = category.publish_tab->m_page_view) {
                page_view->Layout();
                if (wxWindow* view_host = page_view->GetParent())
                    view_host->Layout();
            }
            if (category.publish_host != nullptr)
                category.publish_host->Layout();
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
    // "All" also publishes every material slot's identity (physical colour/type, mixed definition);
    // "None" unpublishes them.
    for (size_t c = 0; c < m_categories.size(); ++c) {
        Category& cat = m_categories[c];
        if (cat.section != Section::Material || cat.owner_check == nullptr)
            continue;
        if (cat.owner_check->GetValue() != value)
            cat.owner_check->SetValue(value);
        // CheckBox::SetValue does not emit wxEVT_TOGGLEBUTTON, so re-run the owner handler to
        // propagate mixed-slot components and refresh visibility as if the user had clicked.
        on_owner_toggle(c);
    }
    // "None" drops Full Publish too: a Full entry publishes its slot on its own, so leaving it on
    // would keep the slot published after the user asked for nothing.
    if (!value)
        for (size_t c = 0; c < m_categories.size(); ++c)
            if (m_categories[c].section == Section::Material && m_categories[c].full_check != nullptr &&
                m_categories[c].full_check->GetValue()) {
                m_categories[c].full_check->SetValue(false);
                on_full_toggle(c);
            }
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
        if (cat.owner_check != nullptr)
            cat.owner_check->SetValue(false);
        if (cat.full_check != nullptr)
            cat.full_check->SetValue(false);
        // A Full Publish seeded below disables the material checkbox; the reset restores it.
        if (cat.owner_check != nullptr)
            cat.owner_check->Enable(true);
        m_selection.set_owner_published(int(cat.filament_slot), false);
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
        // A file entry publishes the owner's identity when it carried colour or type. A mixed
        // entry exists only while the mix was published, so its presence alone publishes it.
        // A partial entry with neither flag ships its keys only.
        if (entry_mixed || entry.publish_color || entry.publish_type) {
            if (cat.owner_check != nullptr)
                cat.owner_check->SetValue(true);
            m_selection.set_owner_published(int(cat.filament_slot), true);
        }
        if (entry.full && cat.full_check != nullptr) {
            cat.full_check->SetValue(true);
            on_full_toggle(cat_idx);
        } else {
            // "key#col" names the row it was written from. A plain key from a file written before
            // the keys carried their column maps to the column this slot prints with (spec 7.5).
            const Slic3r::PublishResolvedColumn resolved = publish_resolved_column(m_snapshot, cat.filament_slot);
            const int                           column   = resolved.file_column.has_value() ? int(*resolved.file_column) : -1;
            auto has_key = [&](const Row& row, const std::string& needle) {
                return std::any_of(row.id.keys.begin(), row.id.keys.end(),
                                   [&needle](const std::string& k) { return k == needle; });
            };
            auto check_first = [&](auto&& match) {
                for (const size_t r : cat.rows) {
                    const Row& row = m_rows[r];
                    if (row.id.kind == Slic3r::PublishRowKind::Key && match(row)) {
                        set_row_checked(r, true);
                        return true;
                    }
                }
                return false;
            };
            for (const std::string& key : entry.keys) {
                const std::string base = publish_base_key(key);
                // A key that names a column must find that column; never seed another variant.
                if (publish_key_index(key) >= 0) {
                    check_first([&](const Row& row) { return has_key(row, key); });
                    continue;
                }
                if (check_first([&](const Row& row) { return row.id.column == column && has_key(row, base); }))
                    continue;
                // A per-slot key has its own row, with no column.
                if (check_first([&](const Row& row) { return row.id.column < 0 && has_key(row, base); }))
                    continue;
                // A variant-bearing key from such a file: the row carries "base#resolved_column".
                if (column >= 0)
                    check_first([&](const Row& row) { return has_key(row, base + "#" + std::to_string(column)); });
            }
            // The material checkbox was seeded from the entry's colour/type flags above; the
            // key rows seed independently of it.
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
    return Slic3r::unpublished_mixed_components(m_selection, m_snapshot);
}

std::vector<Slic3r::PublishedMaterialEntry> PublishSettingsDialog::GetPublishedMaterialKeys() const
{
    // Publishing owners only, in slot order; the model owns the Full Publish and per-key rules.
    return m_selection.material_entries(m_row_ids);
}

bool PublishSettingsDialog::category_has_selection(const Category& cat) const
{
    // A material slot publishes when its identity checkbox is on, one of its rows is checked or it
    // has Full Publish - the same rule the payload applies.
    if (cat.section == Section::Material)
        return m_selection.owner_publishes(int(cat.filament_slot), m_row_ids);
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
        if (cat.owner_check != nullptr)
            cat.owner_check->Refresh();
        if (cat.title_label != nullptr)
            cat.title_label->Refresh();
        if (cat.filament_picker != nullptr) {
            // The combo rebuilds its list from the bundle on a rescale, which can lose the slot's
            // current name: put it back on top.
            cat.filament_picker->msw_rescale();
            cat.filament_picker->show_preset(cat.pending_filament);
        }
        if (cat.filament_color_chip != nullptr) {
            // Mixed pages have no badge; this only ever fires for physical slots.
            if (wxBitmap* chip = get_extruder_color_icon(staged_badge_color(cat, full), std::to_string(cat.filament_slot + 1),
                                                         FromDIP(20), FromDIP(20))) {
                cat.filament_color_chip->SetBitmap(*chip);
                cat.filament_color_chip->SetMinSize(chip->GetSize());
                cat.filament_color_chip->InvalidateBestSize();
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

    // Rescale the hosted tabs with the same entry point the sidebar's Page uses.
    for (Category& category : m_categories)
        if (category.publish_tab != nullptr)
            category.publish_tab->msw_rescale();

    for (size_t category_index = 0; category_index < m_categories.size(); ++category_index) {
        const Category& category = m_categories[category_index];
        if (category.section != Section::Material || category.is_mixed)
            continue;
        if (wxBitmap* chip = get_extruder_color_icon(staged_badge_color(category, full), std::to_string(category.filament_slot + 1),
                                                     FromDIP(20), FromDIP(20))) {
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
    // Re-apply the theme to every hosted tab. The colour pass resets the label colours, so the
    // modified marker goes back on afterwards, as the Tab does.
    for (Category& category : m_categories) {
        if (category.publish_tab != nullptr)
            category.publish_tab->sys_color_changed();
        if (category.filament_picker != nullptr) {
            category.filament_picker->sys_color_changed();
            category.filament_picker->show_preset(category.pending_filament);
        }
        if (category.filament_color_chip != nullptr) {
            // The badge is a button: its background is the theme's, resolved again here.
            category.filament_color_chip->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
            category.filament_color_chip->Refresh();
        }
    }
    for (size_t r = 0; r < m_rows.size(); ++r)
        apply_row_label_colour(r);
    Refresh();
}

}} // namespace Slic3r::GUI
