#include "ColorDecomposeDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <wx/colour.h>
#include <string>
#include <wx/panel.h>
#include <wx/gdicmn.h>
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include <vector>
#include "slic3r/GUI/GUI_Utils.hpp"
#include <utility>
#include <wx/event.h>
#include <wx/peninfobase.h>
#include <wx/sizer.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/dcbuffer.h>
#include <wx/toplevel.h>
#include <wx/string.h>
#include <wx/tglbtn.h>
#include "wx/graphics.h"

#include "I18N.hpp"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "format.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DropDown.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/Label.hpp"
#include "wxExtensions.hpp"
#include "ColorDecomposeSupport.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"

namespace Slic3r {
namespace GUI {

static const wxColour COLOR_BRAND("#009688");
static const wxColour COLOR_BORDER_NORMAL("#EEEEEE");
static const wxColour COLOR_BG_CARD("#F8F8F8");
static const wxColour COLOR_LABEL_GREY("#ACACAC");
static const wxColour COLOR_TEXT_DARK("#262E30");
static const wxColour COLOR_DIVIDER("#EEEEEE");

// Standard CMYW base colors
static const wxColour CMYW_CYAN(0, 255, 255);
static const wxColour CMYW_MAGENTA(255, 0, 255);
static const wxColour CMYW_YELLOW(255, 255, 0);
static const wxColour CMYW_WHITE(255, 255, 255);

// Standard RYBW base colors
static const wxColour RYBW_RED(255, 0, 0);
static const wxColour RYBW_YELLOW(255, 255, 0);
static const wxColour RYBW_BLUE(0, 0, 255);
static const wxColour RYBW_WHITE(255, 255, 255);

// CMYK+W base colors (the CMYW colors plus a near-black K)
static const wxColour CMYKW_CYAN(0, 255, 255);
static const wxColour CMYKW_MAGENTA(255, 0, 255);
static const wxColour CMYKW_YELLOW(255, 255, 0);
static const wxColour CMYKW_BLACK(26, 26, 26);   // #1A1A1A near-black filament
static const wxColour CMYKW_WHITE(255, 255, 255);

static size_t mode_index(DecomposeMode mode)
{
    return static_cast<size_t>(mode);
}

static ColorDecomposeRgb wx_colour_to_recipe_rgb(const wxColour& color)
{
    return {
        static_cast<unsigned char>(color.Red()),
        static_cast<unsigned char>(color.Green()),
        static_cast<unsigned char>(color.Blue())
    };
}

static wxColour hex_to_wx_colour(const std::string& hex, const wxColour& fallback)
{
    wxColour color(hex);
    return color.IsOk() ? color : fallback;
}

static bool same_rgb(const wxColour& lhs, const wxColour& rhs)
{
    return lhs.Red() == rhs.Red() && lhs.Green() == rhs.Green() && lhs.Blue() == rhs.Blue();
}

static DecomposeBaseColor standard_base_color_from_key(const std::string& key)
{
    if (key == "Cyan")    return DecomposeBaseColor::Cyan;
    if (key == "Magenta") return DecomposeBaseColor::Magenta;
    if (key == "Yellow")  return DecomposeBaseColor::Yellow;
    if (key == "White")   return DecomposeBaseColor::White;
    if (key == "Black")   return DecomposeBaseColor::Black;
    if (key == "Red")     return DecomposeBaseColor::Red;
    if (key == "Green")   return DecomposeBaseColor::Green;
    if (key == "Blue")    return DecomposeBaseColor::Blue;
    return DecomposeBaseColor::None;
}

static wxColour pure_color_for_base(DecomposeBaseColor base)
{
    switch (base) {
    case DecomposeBaseColor::Cyan:    return CMYW_CYAN;
    case DecomposeBaseColor::Magenta: return CMYW_MAGENTA;
    case DecomposeBaseColor::Yellow:  return CMYW_YELLOW;
    case DecomposeBaseColor::White:   return CMYW_WHITE;
    case DecomposeBaseColor::Black:   return CMYKW_BLACK;
    case DecomposeBaseColor::Red:     return RYBW_RED;
    case DecomposeBaseColor::Blue:    return RYBW_BLUE;
    default:                          return *wxBLACK;
    }
}

static DecomposeBaseColor standard_base_color_for(DecomposeMode mode, const wxColour& color)
{
    if (mode == DecomposeMode::CMYW) {
        if (same_rgb(color, CMYW_CYAN))    return DecomposeBaseColor::Cyan;
        if (same_rgb(color, CMYW_MAGENTA)) return DecomposeBaseColor::Magenta;
        if (same_rgb(color, CMYW_YELLOW))  return DecomposeBaseColor::Yellow;
        if (same_rgb(color, CMYW_WHITE))   return DecomposeBaseColor::White;
    } else if (mode == DecomposeMode::RYBW) {
        if (same_rgb(color, RYBW_RED))     return DecomposeBaseColor::Red;
        if (same_rgb(color, RYBW_YELLOW))  return DecomposeBaseColor::Yellow;
        if (same_rgb(color, RYBW_BLUE))    return DecomposeBaseColor::Blue;
        if (same_rgb(color, RYBW_WHITE))   return DecomposeBaseColor::White;
    } else if (mode == DecomposeMode::CMYKW) {
        if (same_rgb(color, CMYKW_CYAN))    return DecomposeBaseColor::Cyan;
        if (same_rgb(color, CMYKW_MAGENTA)) return DecomposeBaseColor::Magenta;
        if (same_rgb(color, CMYKW_YELLOW))  return DecomposeBaseColor::Yellow;
        if (same_rgb(color, CMYKW_BLACK))   return DecomposeBaseColor::Black;
        if (same_rgb(color, CMYKW_WHITE))   return DecomposeBaseColor::White;
    }
    return DecomposeBaseColor::None;
}

static ColorDecomposeResult to_dialog_result(const ColorDecomposeRecipeResult& recipe,
                                             const wxColour& fallback)
{
    ColorDecomposeResult result;
    result.mode = recipe.mode;
    result.matched_color = hex_to_wx_colour(recipe.matched_color_hex, fallback);
    for (const auto& comp_recipe : recipe.components) {
        DecomposeComponent comp;
        comp.colour = hex_to_wx_colour(comp_recipe.color_hex, fallback);
        comp.ratio = comp_recipe.ratio;
        comp.filament_index = static_cast<int>(comp_recipe.filament_index);
        comp.base_color = standard_base_color_from_key(comp_recipe.base_color);
        if (comp.base_color == DecomposeBaseColor::None)
            comp.base_color = standard_base_color_for(recipe.mode, comp.colour);
        result.components.push_back(comp);
    }
    return result;
}

static wxPanel* create_h_divider(wxWindow* parent, int fixed_width = -1)
{
    const int h = parent->FromDIP(1);
    int w = fixed_width > 0 ? fixed_width : -1;
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(w, h));
    panel->SetMinSize(wxSize(w, h));
    if (fixed_width > 0)
        panel->SetMaxSize(wxSize(fixed_width, h));
    panel->SetBackgroundColour(StateColor::darkModeColorFor(COLOR_DIVIDER));
    return panel;
}

static wxStaticText* create_mode_group_label(wxWindow* parent, const wxString& text)
{
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetFont(Label::Body_11);
    label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_GREY));
    return label;
}

static void match_parent_bg(wxWindow* w, const wxColour& bg)
{
    w->SetBackgroundColour(bg);
}

// The standard modes (CMYW, RYBW) are built from PLA Basic base colors, which suits every PLA type.
static bool is_pla_family(const std::string& type)
{
    return type == kDecomposePlaShortType || type.rfind(std::string(kDecomposePlaShortType) + " ", 0) == 0;
}

static bool material_type_matches(const std::string& a, const std::string& b)
{
    if (a.empty() || b.empty())
        return false;
    return a == b || a == b + " Basic" || b == a + " Basic";
}


ColorDecomposeDialog::ColorDecomposeDialog(wxWindow* parent,
                                           int filament_idx,
                                           const wxColour& target_color,
                                           const std::vector<std::string>& physical_colors,
                                           const std::vector<std::string>& filament_names,
                                           const std::vector<std::string>& filament_types,
                                           size_t current_filament_count,
                                           size_t max_filament_count,
                                           std::vector<size_t> physical_config_indices)
    : DPIDialog(parent, wxID_ANY, _L("Decompose Color"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    , m_filament_idx(filament_idx)
    , m_target_color(target_color)
    , m_physical_colors(physical_colors)
    , m_filament_names(filament_names)
    , m_filament_types(filament_types)
    , m_current_filament_count(current_filament_count)
    , m_max_filament_count(max_filament_count)
    , m_physical_config_indices(std::move(physical_config_indices))
{
    for (const auto& t : m_filament_types) {
        if (std::find(m_project_types.begin(), m_project_types.end(), t) == m_project_types.end())
            m_project_types.push_back(t);
    }

    if (m_filament_idx >= 0 && static_cast<size_t>(m_filament_idx) < m_filament_types.size())
        m_preferred_type = m_filament_types[m_filament_idx];
    else if (!m_project_types.empty())
        m_preferred_type = m_project_types.front();

    build_ui();
    wxGetApp().UpdateDlgDarkUI(this);
    // Restore target swatch after dark mode color remapping
    if (m_target_swatch) {
        m_target_swatch->SetBackgroundColour(m_target_color);
        m_target_swatch->Refresh();
    }

    update_card_visibility();
    fit_around_center();
    compute_decomposition();
    update_matched_color_display();
    update_ok_button_state();
    // The size changed since build_ui() centered the dialog.
    CenterOnParent();
}

// Fit to the content. Once the dialog is shown it keeps its center, so a change of the content (a note that
// appears, a longer result row) does not push the dialog to one side or make it jump.
void ColorDecomposeDialog::fit_around_center()
{
    if (!IsShown()) {
        Fit();
        return;
    }
    const wxSize old_size = GetSize();
    const wxPoint old_pos = GetPosition();
    Fit();
    const wxSize new_size = GetSize();
    if (new_size == old_size)
        return;
    wxPoint pos(old_pos.x + (old_size.x - new_size.x) / 2, old_pos.y + (old_size.y - new_size.y) / 2);
    const int display_idx = wxDisplay::GetFromWindow(this);
    if (display_idx != wxNOT_FOUND) {
        const wxRect area = wxDisplay(static_cast<unsigned int>(display_idx)).GetClientArea();
        pos.x = std::max(area.x, std::min(pos.x, area.GetRight() - new_size.x + 1));
        pos.y = std::max(area.y, std::min(pos.y, area.GetBottom() - new_size.y + 1));
    }
    Move(pos);
}

void ColorDecomposeDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    (void)suggested_rect;
    fit_around_center();
    Refresh();
}

void ColorDecomposeDialog::build_ui()
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    const int selector_side_margin = FromDIP(26);
    const int selector_top_gap = FromDIP(22);
    const int content_side_margin = FromDIP(30);

    main_sizer->AddSpacer(selector_top_gap);
    main_sizer->Add(create_filament_selector(), 0, wxEXPAND | wxLEFT | wxRIGHT, selector_side_margin);
    main_sizer->AddSpacer(FromDIP(16));
    main_sizer->Add(create_mode_selection_section(), 0, wxEXPAND | wxLEFT | wxRIGHT, content_side_margin);
    main_sizer->AddSpacer(FromDIP(16));
    main_sizer->Add(create_button_panel(), 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, content_side_margin);

    SetSizer(main_sizer);
    SetMinSize(wxSize(FromDIP(477), FromDIP(380)));
    fit_around_center();
    CenterOnParent();
}

wxBoxSizer* ColorDecomposeDialog::create_filament_selector()
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);

    m_type_combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                wxSize(-1, FromDIP(36)), 0, nullptr, wxCB_READONLY);
    m_type_combo->SetFont(Label::Body_13);

    m_combo_item_types.clear();
    int default_sel = -1;

    // --- Group 1: Project filament list (deduplicated by type) ---
    m_type_combo->Append(_L("Project Filament List"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM | DD_ITEM_STYLE_DISABLED);
    m_combo_item_types.push_back(std::string());

    std::set<std::string> seen_types;
    for (size_t i = 0; i < m_filament_names.size(); ++i) {
        const std::string& type = (i < m_filament_types.size()) ? m_filament_types[i] : "PLA";
        if (!seen_types.insert(type).second)
            continue;
        int idx = m_type_combo->Append(wxString::FromUTF8(m_filament_names[i]));
        m_combo_item_types.push_back(type);
        if (type == m_preferred_type && default_sel < 0)
            default_sel = idx;
    }

    // --- Group 2: Standard mode material recommendations ---
    static const char* kStandardTypes[] = {
        kDecomposePlaBasicType
    };

    m_type_combo->Append(_L("Standard Mode Recommendations"), wxNullBitmap, DD_ITEM_STYLE_SPLIT_ITEM | DD_ITEM_STYLE_DISABLED);
    m_combo_item_types.push_back(std::string());

    for (size_t s = 0; s < sizeof(kStandardTypes) / sizeof(kStandardTypes[0]); ++s) {
        // Always show standard recommendations, even if the same type already
        // appears in the project filament list above.
        const std::string label = std::string(kDecomposeBambuPresetPrefix) + kStandardTypes[s];
        int idx = m_type_combo->Append(wxString::FromUTF8(label));
        m_combo_item_types.push_back(kStandardTypes[s]);
        if (kStandardTypes[s] == m_preferred_type && default_sel < 0)
            default_sel = idx;
    }

    if (default_sel < 0) {
        for (int i = 0; i < static_cast<int>(m_combo_item_types.size()); ++i) {
            if (!m_combo_item_types[i].empty()) {
                default_sel = i;
                break;
            }
        }
    }

    if (default_sel >= 0) {
        m_type_combo->SetSelection(default_sel);
        if (!m_combo_item_types[default_sel].empty())
            m_preferred_type = m_combo_item_types[default_sel];
    }

    m_type_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.StopPropagation();
        int sel = m_type_combo->GetSelection();
        if (sel >= 0 && static_cast<size_t>(sel) < m_combo_item_types.size()
            && !m_combo_item_types[sel].empty()) {
            m_preferred_type = m_combo_item_types[sel];
        }
        update_card_visibility();
        compute_decomposition();
        update_matched_color_display();
        update_ok_button_state();
    });

    sizer->Add(m_type_combo, 1, wxEXPAND);
    return sizer;
}

// Paints a swatch of color `c` with its filament number, `y_offset` below the top of the DC.
static void draw_swatch(wxDC& dc, const wxSize& sz, const wxColour& c, int filament_id, int y_offset = 0)
{
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(c));
    dc.DrawRectangle(0, y_offset, sz.GetWidth(), sz.GetHeight());
    // Mirror sidebar (FilamentBitmapUtils::create_single_filament_bitmap):
    // gray border for near-white in light mode so white swatches stay
    // visible on a white background; light border for near-black in dark mode.
    const bool light_mode = !wxGetApp().dark_mode();
    if ((light_mode && c.Red() > 224 && c.Green() > 224 && c.Blue() > 224) ||
        (!light_mode && c.Red() < 45 && c.Green() < 45 && c.Blue() < 45)) {
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.SetPen(wxPen(light_mode ? wxColour(130, 130, 128) : wxColour(207, 207, 207),
                        1, wxPENSTYLE_SOLID));
        dc.DrawRectangle(0, y_offset, sz.GetWidth(), sz.GetHeight());
    }
    if (filament_id > 0) {
        // Filament number, in the color that stays readable on the swatch.
        const wxString text = wxString::Format("%d", filament_id);
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(c.GetLuminance() < 0.51 ? *wxWHITE : *wxBLACK);
        const wxSize ts = dc.GetTextExtent(text);
        dc.DrawText(text, (sz.GetWidth() - ts.x) / 2, y_offset + (sz.GetHeight() - ts.y) / 2);
    }
}

static wxPanel* create_color_swatch(wxWindow* parent, const wxColour& color, int size, int filament_id = 0)
{
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size));
    panel->SetBackgroundColour(color);
    panel->SetMinSize(wxSize(size, size));
    panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    panel->Bind(wxEVT_PAINT, [panel, filament_id](wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(panel);
        draw_swatch(dc, panel->GetClientSize(), panel->GetBackgroundColour(), filament_id);
    });
    return panel;
}

static wxString format_rgb(const wxColour& color)
{
    return wxString::Format("RGB: %d, %d, %d", color.Red(), color.Green(), color.Blue());
}

static wxPanel* create_v_divider(wxWindow* parent)
{
    const int w = parent->FromDIP(1);
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(w, -1));
    panel->SetMinSize(wxSize(w, parent->FromDIP(24)));
    panel->SetBackgroundColour(StateColor::darkModeColorFor(COLOR_DIVIDER));
    return panel;
}

static wxPanel* create_result_arrow(wxWindow* parent)
{
    const int w = parent->FromDIP(34);
    const int h = parent->FromDIP(12);
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(w, h), wxBORDER_NONE);
    panel->SetMinSize(wxSize(w, h));
    panel->SetMaxSize(wxSize(w, h));
    panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    panel->Bind(wxEVT_PAINT, [panel](wxPaintEvent&) {
        wxPaintDC dc(panel);
        const wxSize sz = panel->GetClientSize();
        wxWindow* host = panel->GetParent();
        dc.SetBackground(wxBrush(host ? host->GetBackgroundColour() : *wxWHITE));
        dc.Clear();
        const wxColour c = StateColor::darkModeColorFor(COLOR_LABEL_GREY);
        const int pen_w = std::max(1, panel->FromDIP(1));
        dc.SetPen(wxPen(c, pen_w));
        const int y = sz.y / 2;
        const int head = panel->FromDIP(4);
        dc.DrawLine(0, y, sz.x - 1 - head / 2, y);
        dc.DrawLine(sz.x - 1, y, sz.x - 1 - head, y - head);
        dc.DrawLine(sz.x - 1, y, sz.x - 1 - head, y + head);
    });
    return panel;
}

static wxPanel* create_rounded_result_card(wxWindow* parent)
{
    auto* card = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetBackgroundStyle(wxBG_STYLE_PAINT);
    card->Bind(wxEVT_PAINT, [card](wxPaintEvent&) {
        wxBufferedPaintDC dc(card);
        wxSize sz = card->GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
        dc.Clear();
        wxColour border_col = StateColor::darkModeColorFor(COLOR_BORDER_NORMAL);
        const int border_width = card->FromDIP(1);
        const double inset = border_width / 2.0;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetPen(wxPen(border_col, border_width));
            gc->SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
            gc->DrawRoundedRectangle(inset, inset, sz.x - 2 * inset, sz.y - 2 * inset, card->FromDIP(8));
        } else {
            dc.SetPen(wxPen(border_col, border_width));
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, card->FromDIP(8));
        }
    });
    return card;
}

static wxStaticText* create_result_caption(wxWindow* parent, const wxString& text)
{
    auto* label = new wxStaticText(parent, wxID_ANY, text);
    label->SetFont(Label::Body_12);
    label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_GREY));
    return label;
}

// A swatch of the result row. A "New" badge marks a filament that is created on confirmation.
// `reserve_new_space` keeps the badge band free so rows with and without badges stay aligned.
static wxWindow* create_result_component_swatch(wxWindow* parent, const wxColour& color, int size,
                                                int filament_id, bool show_new, bool reserve_new_space)
{
    if (!show_new && !reserve_new_space)
        return create_color_swatch(parent, color, size, filament_id);

    const int overhang = parent->FromDIP(8);
    auto* panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(size, size + overhang));
    panel->SetMinSize(wxSize(size, size + overhang));
    panel->SetMaxSize(wxSize(size, size + overhang));
    panel->SetBackgroundStyle(wxBG_STYLE_PAINT);
    panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));

    panel->Bind(wxEVT_PAINT, [panel, color, size, filament_id, overhang, show_new](wxPaintEvent&) {
        wxBufferedPaintDC dc(panel);
        dc.SetBackground(wxBrush(panel->GetBackgroundColour()));
        dc.Clear();
        draw_swatch(dc, wxSize(size, size), color, filament_id, overhang);
        if (!show_new)
            return;
        // TRN Badge on a filament that is created when the decomposition is confirmed
        const wxString text = _L("New");
        dc.SetFont(Label::Body_10);
        const wxSize ts = dc.GetTextExtent(text);
        const int pad_x = panel->FromDIP(3);
        const int pad_y = panel->FromDIP(1);
        const int bw = ts.x + pad_x * 2;
        const int bh = std::max(ts.y + pad_y * 2, panel->FromDIP(10));
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(StateColor::darkModeColorFor(COLOR_BRAND)));
        dc.DrawRoundedRectangle(0, 0, bw, bh, panel->FromDIP(6));
        dc.SetTextForeground(*wxWHITE);
        dc.DrawText(text, pad_x, (bh - ts.y) / 2);
    });
    return panel;
}

static wxPanel* create_component_plus_panel(wxWindow* parent, int plus_gap, int swatch_sz, const wxColour& bg)
{
    auto* plus_panel = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(plus_gap, swatch_sz));
    plus_panel->SetMinSize(wxSize(plus_gap, swatch_sz));
    plus_panel->SetMaxSize(wxSize(plus_gap, swatch_sz));
    plus_panel->SetBackgroundColour(bg);
    auto* plus_sizer = new wxBoxSizer(wxVERTICAL);
    auto* plus_label = new wxStaticText(plus_panel, wxID_ANY, "+");
    plus_label->SetFont(Label::Body_13);
    plus_label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_TEXT_DARK));
    plus_label->SetBackgroundColour(bg);
    plus_sizer->AddStretchSpacer();
    plus_sizer->Add(plus_label, 0, wxALIGN_CENTER_HORIZONTAL);
    plus_sizer->AddStretchSpacer();
    plus_panel->SetSizer(plus_sizer);
    return plus_panel;
}

static void append_result_component(wxWindow* parent, wxBoxSizer* sizer, const wxColour& color, int ratio,
                                    int swatch_sz, int filament_id, const wxColour& bg, bool show_new,
                                    bool reserve_new_space)
{
    auto* col = new wxBoxSizer(wxVERTICAL);
    auto* swatch = create_result_component_swatch(parent, color, swatch_sz, filament_id, show_new, reserve_new_space);
    swatch->SetBackgroundColour(bg);
    col->Add(swatch, 0, wxALIGN_CENTER_HORIZONTAL);

    auto* ratio_text = new wxStaticText(parent, wxID_ANY, wxString::Format("%d%%", ratio));
    ratio_text->SetFont(Label::Body_13);
    ratio_text->SetForegroundColour(StateColor::darkModeColorFor(COLOR_TEXT_DARK));
    ratio_text->SetBackgroundColour(bg);
    col->Add(ratio_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, parent->FromDIP(4));
    sizer->Add(col, 0, wxALIGN_TOP);
}

static void append_result_plus(wxWindow* parent, wxBoxSizer* sizer, int plus_gap, int swatch_sz,
                               const wxColour& bg, int plus_top_pad)
{
    sizer->AddStretchSpacer();
    sizer->Add(create_component_plus_panel(parent, plus_gap, swatch_sz, bg), 0, wxALIGN_TOP | wxTOP, plus_top_pad);
    sizer->AddStretchSpacer();
}

static int result_components_inner_width(wxWindow* host, size_t count)
{
    const int pad = host->FromDIP(12);
    const int two_color_card = host->FromDIP(128);
    if (count <= 2)
        return two_color_card - 2 * pad;
    const int swatch = host->FromDIP(32);
    const int plus = host->FromDIP(24);
    const int n = static_cast<int>(count);
    return n * swatch + (n - 1) * plus;
}

wxBoxSizer* ColorDecomposeDialog::create_result_section()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* title = new wxStaticText(this, wxID_ANY, _L("Decomposition result for this combination"));
    title->SetFont(Label::Head_14);
    title->SetForegroundColour(StateColor::darkModeColorFor(COLOR_TEXT_DARK));
    sizer->Add(title, 0, wxBOTTOM, FromDIP(8));

    auto* row = new wxBoxSizer(wxHORIZONTAL);
    const int swatch_sz = FromDIP(32);
    const int pad = FromDIP(12);
    const wxColour box_bg = StateColor::darkModeColorFor(*wxWHITE);

    // Source filament color
    auto* src_col = new wxBoxSizer(wxVERTICAL);
    auto* filament_caption = create_result_caption(this, _L("Filament Color"));
    src_col->Add(filament_caption, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));

    m_filament_card = create_rounded_result_card(this);
    auto* filament_sizer = new wxBoxSizer(wxVERTICAL);
    auto* filament_inner = new wxBoxSizer(wxVERTICAL);
    m_target_swatch = create_color_swatch(m_filament_card, m_target_color, swatch_sz);
    m_target_swatch->SetBackgroundColour(m_target_color);
    filament_inner->Add(m_target_swatch, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(4));
    m_target_rgb_text = new wxStaticText(m_filament_card, wxID_ANY, format_rgb(m_target_color));
    m_target_rgb_text->SetFont(Label::Body_12);
    m_target_rgb_text->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_GREY));
    m_target_rgb_text->SetBackgroundColour(box_bg);
    filament_inner->Add(m_target_rgb_text, 0, wxALIGN_CENTER_HORIZONTAL);
    filament_sizer->AddSpacer(pad);
    filament_sizer->AddStretchSpacer(1);
    filament_sizer->Add(filament_inner, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, pad);
    filament_sizer->AddStretchSpacer(1);
    filament_sizer->AddSpacer(pad);
    m_filament_card->SetSizer(filament_sizer);
    filament_sizer->SetSizeHints(m_filament_card);
    src_col->Add(m_filament_card, 0, wxALIGN_CENTER_HORIZONTAL);
    row->Add(src_col, 0, wxALIGN_TOP);

    // Arrow, centered on the cards
    auto* arrow_col = new wxBoxSizer(wxVERTICAL);
    arrow_col->AddSpacer(filament_caption->GetBestSize().GetHeight() + FromDIP(8));
    arrow_col->AddStretchSpacer(1);
    m_result_arrow = create_result_arrow(this);
    arrow_col->Add(m_result_arrow, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
    arrow_col->AddStretchSpacer(1);
    row->Add(arrow_col, 0, wxEXPAND);

    // Mixed color and the components it is made of
    auto* dst_col = new wxBoxSizer(wxVERTICAL);
    m_decomposed_label = create_result_caption(this, _L("Decomposed Colors"));
    dst_col->Add(m_decomposed_label, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));

    m_decomposed_container = create_rounded_result_card(this);
    auto* box_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto* mixed_col = new wxBoxSizer(wxVERTICAL);
    m_matched_swatch = create_color_swatch(m_decomposed_container, m_target_color, swatch_sz);
    m_matched_swatch->SetBackgroundColour(m_target_color);
    mixed_col->Add(m_matched_swatch, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(4));
    m_matched_rgb_text = new wxStaticText(m_decomposed_container, wxID_ANY, format_rgb(m_target_color));
    m_matched_rgb_text->SetFont(Label::Body_12);
    m_matched_rgb_text->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_GREY));
    m_matched_rgb_text->SetBackgroundColour(box_bg);
    mixed_col->Add(m_matched_rgb_text, 0, wxALIGN_CENTER_HORIZONTAL);
    box_sizer->Add(mixed_col, 0, wxALIGN_CENTER_VERTICAL | wxALL, pad);

    m_result_v_divider = create_v_divider(m_decomposed_container);
    box_sizer->Add(m_result_v_divider, 0, wxEXPAND | wxTOP | wxBOTTOM, pad);

    m_result_components_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* components_outer = new wxBoxSizer(wxHORIZONTAL);
    components_outer->Add(m_result_components_sizer, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(32));
    box_sizer->Add(components_outer, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, pad);

    m_decomposed_container->SetSizer(box_sizer);
    dst_col->Add(m_decomposed_container, 0, wxALIGN_CENTER_HORIZONTAL);
    row->Add(dst_col, 0, wxALIGN_TOP);
    row->AddStretchSpacer(1);

    sizer->Add(row, 0, wxEXPAND);
    return sizer;
}

wxPanel* ColorDecomposeDialog::create_mode_card(wxWindow* parent, DecomposeMode mode,
                                                 const wxString& title)
{
    const int pad       = FromDIP(12);

    auto* card = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    card->SetBackgroundStyle(wxBG_STYLE_PAINT);

    auto* card_sizer = new wxBoxSizer(wxVERTICAL);

    auto* title_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* title_label = new wxStaticText(card, wxID_ANY, title);
    title_label->SetFont(Label::Body_14);
    title_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6A6A")));
    match_parent_bg(title_label, StateColor::darkModeColorFor(COLOR_BG_CARD));
    title_sizer->Add(title_label, 1, wxALIGN_CENTER_VERTICAL);

    auto* chk = new ::CheckBox(card);
    chk->SetValue(mode == m_selected_mode);
    match_parent_bg(chk, StateColor::darkModeColorFor(COLOR_BG_CARD));
    switch (mode) {
    case DecomposeMode::MaterialList: m_chk_material_list = chk; break;
    case DecomposeMode::CMYW:         m_chk_cmyw  = chk; break;
    case DecomposeMode::RYBW:         m_chk_rybw  = chk; break;
    case DecomposeMode::CMYKW:        m_chk_cmykw = chk; break;
    }
    chk->Bind(wxEVT_TOGGLEBUTTON, [this, mode](wxCommandEvent& e) {
        select_mode(mode);
        e.Skip();  // let CheckBox::update() re-sync its bitmap to GetValue()
    });
    title_sizer->Add(chk, 0, wxALIGN_CENTER_VERTICAL);

    card_sizer->Add(title_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad);

    card_sizer->Add(create_h_divider(card), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    auto* colors_sizer = new wxBoxSizer(wxHORIZONTAL);
    card_sizer->Add(colors_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP | wxBOTTOM, pad);

    auto& controls = m_mode_cards[mode_index(mode)];
    controls.card = card;
    controls.components_sizer = colors_sizer;

    card->SetSizer(card_sizer);
    card->SetMinSize(wxSize(FromDIP(128), FromDIP(111)));
    card->SetMaxSize(wxSize(FromDIP(128), FromDIP(111)));

    card->Bind(wxEVT_PAINT, [this, card, mode](wxPaintEvent&) {
        wxBufferedPaintDC dc(card);
        wxSize sz = card->GetClientSize();
        dc.SetBackground(wxBrush(StateColor::darkModeColorFor(*wxWHITE)));
        dc.Clear();

        bool selected = (m_selected_mode == mode);
        wxColour border_col = selected
            ? StateColor::darkModeColorFor(COLOR_BRAND)
            : StateColor::darkModeColorFor(COLOR_BORDER_NORMAL);
        const int border_width = FromDIP(selected ? 2 : 1);
        const double inset = border_width / 2.0;
        std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
        if (gc) {
            gc->SetPen(wxPen(border_col, border_width));
            gc->SetBrush(wxBrush(StateColor::darkModeColorFor(COLOR_BG_CARD)));
            gc->DrawRoundedRectangle(inset, inset, sz.x - 2 * inset, sz.y - 2 * inset, FromDIP(8));
        } else {
            const int fallback_inset = (border_width + 1) / 2;
            dc.SetPen(wxPen(border_col, border_width));
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(COLOR_BG_CARD)));
            dc.DrawRoundedRectangle(fallback_inset, fallback_inset, sz.x - 2 * fallback_inset, sz.y - 2 * fallback_inset, FromDIP(8));
        }
    });

    std::function<void(wxWindow*)> bind_click;
    bind_click = [this, mode, chk, &bind_click](wxWindow* w) {
        if (w == chk || dynamic_cast<::CheckBox*>(w))
            return;
        w->Bind(wxEVT_LEFT_UP, [this, mode](wxMouseEvent&) {
            select_mode(mode);
        });
        w->SetCursor(wxCursor(wxCURSOR_HAND));
        for (auto* child : w->GetChildren())
            bind_click(child);
    };
    bind_click(card);

    return card;
}

wxBoxSizer* ColorDecomposeDialog::create_mode_selection_section()
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* section_label = new wxStaticText(this, wxID_ANY, _L("Select Color Decomposition"));
    section_label->SetFont(Label::Head_14);
    section_label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_TEXT_DARK));
    sizer->Add(section_label, 0, wxBOTTOM, FromDIP(4));

    auto* modes_sizer = new wxBoxSizer(wxHORIZONTAL);

    // --- Arbitrary mode column (wrapped in a panel so the whole column hides together) ---
    m_arb_column_panel = new wxPanel(this, wxID_ANY);
    m_arb_column_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    auto* arb_col = new wxBoxSizer(wxVERTICAL);
    {
        auto* arb_header_sizer = new wxBoxSizer(wxHORIZONTAL);
        arb_header_sizer->Add(create_mode_group_label(m_arb_column_panel, _L("Arbitrary Mode")),
                              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        arb_header_sizer->Add(create_h_divider(m_arb_column_panel, FromDIP(88)), 0, wxALIGN_CENTER_VERTICAL);
        arb_col->Add(arb_header_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

        m_card_material_list = create_mode_card(m_arb_column_panel, DecomposeMode::MaterialList,
            _L("Material List"));
        arb_col->Add(m_card_material_list, 0, wxEXPAND);
    }
    m_arb_column_panel->SetSizer(arb_col);
    modes_sizer->Add(m_arb_column_panel, 0, wxEXPAND | wxRIGHT, FromDIP(16));

    // --- Standard mode column ---
    auto* std_col = new wxBoxSizer(wxVERTICAL);
    {
        auto* std_header_sizer = new wxBoxSizer(wxHORIZONTAL);
        std_header_sizer->Add(create_mode_group_label(this, _L("Standard Mode")),
                              0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));
        std_header_sizer->Add(create_h_divider(this), 1, wxALIGN_CENTER_VERTICAL);
        std_col->Add(std_header_sizer, 0, wxEXPAND | wxBOTTOM, FromDIP(8));

        auto* cards_sizer = new wxBoxSizer(wxHORIZONTAL);

        m_card_cmyw = create_mode_card(this, DecomposeMode::CMYW, "CMYW");
        cards_sizer->Add(m_card_cmyw, 0, wxRIGHT, FromDIP(12));

        m_card_rybw = create_mode_card(this, DecomposeMode::RYBW, "RYBW");
        cards_sizer->Add(m_card_rybw, 0, wxRIGHT, FromDIP(12));

        m_card_cmykw = create_mode_card(this, DecomposeMode::CMYKW, "CMYK+W");
        cards_sizer->Add(m_card_cmykw, 0);

        std_col->Add(cards_sizer, 0, wxEXPAND);
    }
    modes_sizer->Add(std_col, 0, wxEXPAND);

    sizer->Add(modes_sizer, 0, wxEXPAND);

    m_no_card_hint = new wxStaticText(this, wxID_ANY,
        _L("At least two filaments of the same material type are required for decomposition"));
    m_no_card_hint->SetFont(Label::Body_13);
    m_no_card_hint->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6A6A")));
    m_no_card_hint->Wrap(FromDIP(400));
    m_no_card_hint->Hide();
    sizer->Add(m_no_card_hint, 0, wxTOP, FromDIP(8));

    m_basic_warning_panel = new wxPanel(this, wxID_ANY);
    m_basic_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    auto* basic_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_basic_warning_text = new wxStaticText(m_basic_warning_panel, wxID_ANY, wxEmptyString);
    m_basic_warning_text->SetFont(Label::Body_13);
    m_basic_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6A6A")));
    // The text is wrapped to the width of the dialog when it is shown. Without a small minimum width, its
    // unwrapped width would count as the width the dialog needs.
    m_basic_warning_text->SetMinSize(wxSize(FromDIP(100), -1));
    basic_sizer->Add(m_basic_warning_text, 1, wxEXPAND);
    m_basic_warning_panel->SetSizer(basic_sizer);
    m_basic_warning_panel->Hide();
    sizer->Add(m_basic_warning_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    sizer->Add(create_result_section(), 0, wxEXPAND | wxTOP, FromDIP(16));

    m_limit_warning_panel = new wxPanel(this, wxID_ANY);
    m_limit_warning_panel->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    auto* warning_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto* warn_bmp = new wxStaticBitmap(m_limit_warning_panel, wxID_ANY,
        create_scaled_bitmap("obj_warning", m_limit_warning_panel, 16),
        wxDefaultPosition, wxSize(FromDIP(16), FromDIP(16)));
    m_limit_warning_text = new wxStaticText(m_limit_warning_panel, wxID_ANY, wxEmptyString);
    m_limit_warning_text->SetFont(Label::Body_13);
    m_limit_warning_text->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#D01B1B")));
    m_limit_warning_text->Wrap(FromDIP(400));
    warning_sizer->Add(warn_bmp, 0, wxALIGN_TOP | wxRIGHT, FromDIP(6));
    warning_sizer->Add(m_limit_warning_text, 1, wxEXPAND);
    m_limit_warning_panel->SetSizer(warning_sizer);
    m_limit_warning_panel->Hide();
    sizer->Add(m_limit_warning_panel, 0, wxEXPAND | wxTOP, FromDIP(8));

    return sizer;
}

wxBoxSizer* ColorDecomposeDialog::create_button_panel()
{
    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->AddStretchSpacer();

    m_btn_cancel = new Button(this, _L("Cancel"));
    m_btn_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    m_btn_ok = new Button(this, _L("OK"));
    m_btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        EndModal(wxID_OK);
    });

    sizer->Add(m_btn_cancel, 0, wxRIGHT, FromDIP(12));
    sizer->Add(m_btn_ok, 0);

    return sizer;
}

void ColorDecomposeDialog::select_mode(DecomposeMode mode)
{
    m_selected_mode = mode;
    m_result = m_mode_results[mode_index(mode)];
    update_card_styles();
    update_basic_support_warning();
    update_matched_color_display();
    update_ok_button_state();
}

void ColorDecomposeDialog::update_card_styles()
{
    if (m_card_material_list) m_card_material_list->Refresh();
    if (m_card_cmyw)          m_card_cmyw->Refresh();
    if (m_card_rybw)          m_card_rybw->Refresh();
    if (m_card_cmykw)         m_card_cmykw->Refresh();

    if (m_chk_material_list)
        m_chk_material_list->SetValue(m_selected_mode == DecomposeMode::MaterialList);
    if (m_chk_cmyw)
        m_chk_cmyw->SetValue(m_selected_mode == DecomposeMode::CMYW);
    if (m_chk_rybw)
        m_chk_rybw->SetValue(m_selected_mode == DecomposeMode::RYBW);
    if (m_chk_cmykw)
        m_chk_cmykw->SetValue(m_selected_mode == DecomposeMode::CMYKW);
}

void ColorDecomposeDialog::update_card_visibility()
{
    // Count physical filaments of the same type (excluding the source filament)
    int same_type_count = 0;
    for (size_t i = 0; i < m_filament_types.size(); ++i) {
        if (static_cast<int>(i) == m_filament_idx)
            continue;
        if (material_type_matches(m_filament_types[i], m_preferred_type))
            ++same_type_count;
    }

    bool show_arb  = (same_type_count >= 2);
    bool show_cmyw = is_pla_family(m_preferred_type);
    bool show_rybw = is_pla_family(m_preferred_type);
    // CMYK+W is computed analytically, so it needs no filament table.
    bool show_cmykw = m_cmykw_enabled;

    if (m_arb_column_panel)   m_arb_column_panel->Show(show_arb);
    if (m_card_material_list) m_card_material_list->Show(show_arb);
    if (m_card_cmyw)          m_card_cmyw->Show(show_cmyw);
    if (m_card_rybw)          m_card_rybw->Show(show_rybw);
    if (m_card_cmykw)         m_card_cmykw->Show(show_cmykw);

    bool any_visible = show_arb || show_cmyw || show_rybw || show_cmykw;
    if (m_no_card_hint)
        m_no_card_hint->Show(!any_visible);

    // Auto-select a visible mode when current selection becomes hidden
    if (any_visible) {
        bool cur_visible = false;
        if (m_selected_mode == DecomposeMode::MaterialList && show_arb)  cur_visible = true;
        if (m_selected_mode == DecomposeMode::CMYW && show_cmyw)         cur_visible = true;
        if (m_selected_mode == DecomposeMode::RYBW && show_rybw)         cur_visible = true;
        if (m_selected_mode == DecomposeMode::CMYKW && show_cmykw)       cur_visible = true;
        if (!cur_visible) {
            if (show_arb)       select_mode(DecomposeMode::MaterialList);
            else if (show_cmyw) select_mode(DecomposeMode::CMYW);
            else if (show_rybw) select_mode(DecomposeMode::RYBW);
            else                select_mode(DecomposeMode::CMYKW);
        }
    }

    Layout();
    update_basic_support_warning();
    update_ok_button_state();
}

void ColorDecomposeDialog::update_basic_support_warning()
{
    if (!m_basic_warning_panel || !m_basic_warning_text)
        return;

    const bool show = is_pla_family(m_preferred_type)
        && (m_selected_mode == DecomposeMode::CMYW || m_selected_mode == DecomposeMode::RYBW);
    const bool was_shown = m_basic_warning_panel->IsShown();
    if (!show) {
        if (was_shown) {
            m_basic_warning_panel->Hide();
            Layout();
            fit_around_center();
        }
        return;
    }

    const wxString text = (m_selected_mode == DecomposeMode::CMYW)
        ? _L("A closer mix is calculated from four base colors: cyan, magenta, yellow, and white, using PLA Basic. If matching filaments are not in the project, they will be added automatically.")
        : _L("A closer mix is calculated from four base colors: red, yellow, blue, and white, using PLA Basic. If matching filaments are not in the project, they will be added automatically.");
    // Show first so the panel gets the width of the dialog, then wrap the text to it.
    m_basic_warning_panel->Show();
    m_basic_warning_text->SetLabel(text);
    Layout();
    const int avail = m_basic_warning_panel->GetClientSize().x;
    if (avail > FromDIP(50))
        m_basic_warning_text->Wrap(avail);
    Layout();
    // Only resize when the banner appears, so switching cards does not make the dialog jump.
    if (!was_shown)
        fit_around_center();
}

void ColorDecomposeDialog::update_filament_limit_warning()
{
    if (!m_limit_warning_panel || !m_limit_warning_text)
        return;

    size_t missing_new = 0;
    if (m_missing_calculator) {
        missing_new = m_missing_calculator(m_result);
    } else {
        const size_t source_physical_idx = m_filament_idx >= 0 ? static_cast<size_t>(m_filament_idx) : size_t(-1);
        const std::vector<size_t>* indices =
            m_physical_config_indices.empty() ? nullptr : &m_physical_config_indices;
        missing_new = count_decompose_new_physical_filaments(
            m_result, m_physical_colors, m_filament_types, source_physical_idx, indices);
    }
    // A result with fewer than 2 components (e.g. target color is already a
    // standard base color shown as "100%") creates no mixed filament and no new
    // physical filament, so it can never exceed the limit. CMYK+W only shows its mix and creates nothing either.
    const bool creates_mixed = m_result.components.size() >= 2 && m_result.mode != DecomposeMode::CMYKW;
    // +1 for the mixed filament slot that will be created after decomposition.
    const size_t needed = m_current_filament_count + missing_new + 1;
    const bool blocked = creates_mixed && needed > m_max_filament_count;

    const bool was_shown = m_limit_warning_panel->IsShown();

    if (!blocked) {
        if (was_shown) {
            m_limit_warning_panel->Hide();
            Layout();
            fit_around_center();
        }
        return;
    }

    const unsigned int max_count = static_cast<unsigned int>(m_max_filament_count);
    const wxString warning_text = m_selected_mode == DecomposeMode::MaterialList ?
        format_wxstr(_L_PLURAL("The material list supports at most %1% color. After Material List decomposition, the material count would exceed %1%. Please delete unused filaments on the main screen before decomposing.",
                               "The material list supports at most %1% colors. After Material List decomposition, the material count would exceed %1%. Please delete unused filaments on the main screen before decomposing.",
                               max_count), max_count) :
        // TRN %2% is the decomposition mode name, "CMYW" or "RYBW"
        format_wxstr(_L_PLURAL("The material list supports at most %1% color. After %2% decomposition, the material count would exceed %1%. Please delete unused filaments on the main screen before decomposing.",
                               "The material list supports at most %1% colors. After %2% decomposition, the material count would exceed %1%. Please delete unused filaments on the main screen before decomposing.",
                               max_count), max_count, m_selected_mode == DecomposeMode::CMYW ? "CMYW" : "RYBW");

    // Show first so the panel is laid out and the text control gets its real
    // width, then wrap to that width so the paragraph fills the content area.
    m_limit_warning_panel->Show();
    Layout();
    const int avail = m_limit_warning_text->GetClientSize().x;
    m_limit_warning_text->SetLabel(warning_text);
    if (avail > FromDIP(50))
        m_limit_warning_text->Wrap(avail);

    Layout();
    // Only resize when the warning panel actually toggled from hidden to shown.
    // While already visible, switching modes must not re-Fit the dialog, which
    // would make it jump on every card switch. Fit keeps the user-moved position.
    if (!was_shown) {
        fit_around_center();
    }
}

void ColorDecomposeDialog::set_missing_physical_calculator(std::function<size_t(const ColorDecomposeResult&)> fn)
{
    m_missing_calculator = std::move(fn);
    update_ok_button_state();
}

void ColorDecomposeDialog::set_cmykw_enabled(bool enabled)
{
    m_cmykw_enabled = enabled;
    update_card_visibility();
    fit_around_center();
}

void ColorDecomposeDialog::set_show_filament_ids(bool show)
{
    m_show_filament_ids = show;
    update_mode_card_contents();
}

DecomposePreviewIds ColorDecomposeDialog::preview_ids_for(const ColorDecomposeResult& result) const
{
    return preview_decompose_filament_ids(result, m_filament_idx, m_current_filament_count,
                                          m_physical_colors, m_filament_types, m_physical_config_indices);
}

void ColorDecomposeDialog::update_ok_button_state()
{
    if (!m_btn_ok) return;
    update_filament_limit_warning();
    const bool any_card_visible = has_usable_card();
    const bool blocked = m_limit_warning_panel && m_limit_warning_panel->IsShown();
    m_btn_ok->Enable(any_card_visible && !blocked);
    Layout();
}

void ColorDecomposeDialog::update_mode_card_content(DecomposeMode mode)
{
    auto& controls = m_mode_cards[mode_index(mode)];
    auto* sizer = controls.components_sizer;
    auto* card = controls.card;
    if (!sizer || !card)
        return;

    sizer->Clear(true);
    const auto& components = m_mode_results[mode_index(mode)].components;
    const size_t count = components.size();
    if (count == 0) {
        card->Layout();
        card->Refresh();
        return;
    }

    const int swatch_sz = FromDIP(24);
    const int plus_gap  = FromDIP(24);
    const wxFont& ratio_font = Label::Body_13;
    auto bind_select = [this, mode](wxWindow* w) {
        w->Bind(wxEVT_LEFT_UP, [this, mode](wxMouseEvent&) {
            select_mode(mode);
        });
        w->SetCursor(wxCursor(wxCURSOR_HAND));
    };

    // The Material List card shows the sidebar numbers of its filaments, CMYK+W its fixed slots.
    DecomposePreviewIds preview;
    if (m_show_filament_ids && (mode == DecomposeMode::MaterialList || mode == DecomposeMode::CMYKW))
        preview = preview_ids_for(m_mode_results[mode_index(mode)]);

    for (size_t i = 0; i < count; ++i) {
        auto* col = new wxBoxSizer(wxVERTICAL);
        const int filament_id = i < preview.component_ids.size() ? preview.component_ids[i] : 0;
        auto* swatch = create_color_swatch(card, components[i].colour, swatch_sz, filament_id);
        bind_select(swatch);
        col->Add(swatch, 0, wxALIGN_CENTER_HORIZONTAL);
        auto* ratio_text = new wxStaticText(card, wxID_ANY, wxString::Format("%d%%", components[i].ratio));
        ratio_text->SetFont(ratio_font);
        ratio_text->SetForegroundColour(StateColor::darkModeColorFor(COLOR_TEXT_DARK));
        match_parent_bg(ratio_text, StateColor::darkModeColorFor(COLOR_BG_CARD));
        bind_select(ratio_text);
        col->Add(ratio_text, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(4));
        sizer->Add(col, 0, wxALIGN_TOP);

        if (i + 1 < count) {
            sizer->AddStretchSpacer();
            auto* plus_panel = new wxPanel(card, wxID_ANY, wxDefaultPosition, wxSize(plus_gap, swatch_sz));
            plus_panel->SetMinSize(wxSize(plus_gap, swatch_sz));
            plus_panel->SetMaxSize(wxSize(plus_gap, swatch_sz));
            plus_panel->SetBackgroundColour(StateColor::darkModeColorFor(COLOR_BG_CARD));
            auto* plus_sizer = new wxBoxSizer(wxVERTICAL);
            auto* plus_label = new wxStaticText(plus_panel, wxID_ANY, "+");
            plus_label->SetFont(Label::Body_13);
            plus_label->SetForegroundColour(StateColor::darkModeColorFor(COLOR_TEXT_DARK));
            match_parent_bg(plus_label, StateColor::darkModeColorFor(COLOR_BG_CARD));
            bind_select(plus_panel);
            bind_select(plus_label);
            plus_sizer->AddStretchSpacer();
            plus_sizer->Add(plus_label, 0, wxALIGN_CENTER_HORIZONTAL);
            plus_sizer->AddStretchSpacer();
            plus_panel->SetSizer(plus_sizer);
            sizer->Add(plus_panel, 0, wxALIGN_TOP);
            sizer->AddStretchSpacer();
        }
    }

    const int card_width = FromDIP(128 + (count > 2 ? static_cast<int>(count - 2) * 31 : 0));
    card->SetMinSize(wxSize(card_width, FromDIP(111)));
    card->SetMaxSize(wxSize(card_width, FromDIP(111)));

    card->Layout();
    card->Refresh();
}

void ColorDecomposeDialog::update_mode_card_contents()
{
    update_mode_card_content(DecomposeMode::MaterialList);
    update_mode_card_content(DecomposeMode::CMYW);
    update_mode_card_content(DecomposeMode::RYBW);
    update_mode_card_content(DecomposeMode::CMYKW);
    Layout();
    fit_around_center();
}

bool ColorDecomposeDialog::is_existing_physical_id(int preview_id) const
{
    if (preview_id <= 0)
        return false;
    if (m_physical_config_indices.empty())
        return preview_id >= 1 && static_cast<size_t>(preview_id) <= m_physical_colors.size();
    for (size_t cfg_idx : m_physical_config_indices) {
        if (static_cast<int>(cfg_idx + 1) == preview_id)
            return true;
    }
    return false;
}

bool ColorDecomposeDialog::has_usable_card() const
{
    return (m_card_material_list && m_card_material_list->IsShown())
        || (m_card_cmyw && m_card_cmyw->IsShown())
        || (m_card_rybw && m_card_rybw->IsShown())
        || (m_card_cmykw && m_card_cmykw->IsShown());
}

void ColorDecomposeDialog::update_matched_color_display()
{
    if (!m_result.matched_color.IsOk())
        m_result.matched_color = m_target_color;

    if (m_matched_swatch) {
        m_matched_swatch->SetBackgroundColour(m_result.matched_color);
        m_matched_swatch->Refresh();
    }
    if (m_matched_rgb_text)
        m_matched_rgb_text->SetLabel(format_rgb(m_result.matched_color));

    // Components of the result with the filament numbers they get; "New" marks filaments that are created.
    // CMYK+W uses fixed slots and creates nothing.
    DecomposePreviewIds preview;
    if (m_show_filament_ids)
        preview = preview_ids_for(m_result);
    const bool can_create = m_show_filament_ids && m_result.mode != DecomposeMode::CMYKW;

    if (m_result_components_sizer && m_decomposed_container) {
        m_decomposed_container->Freeze();
        m_result_components_sizer->Clear(true);
        const auto& components = m_result.components;
        const wxColour box_bg = StateColor::darkModeColorFor(*wxWHITE);
        const int swatch_sz = FromDIP(32);
        const int plus_gap = FromDIP(24);
        auto id_of = [&preview](size_t i) { return i < preview.component_ids.size() ? preview.component_ids[i] : 0; };
        auto is_new = [&](size_t i) { return can_create && id_of(i) > 0 && !is_existing_physical_id(id_of(i)); };
        bool any_new = false;
        for (size_t i = 0; i < components.size(); ++i)
            any_new = any_new || is_new(i);
        const int plus_top_pad = any_new ? FromDIP(8) : 0;
        for (size_t i = 0; i < components.size(); ++i) {
            append_result_component(m_decomposed_container, m_result_components_sizer, components[i].colour,
                                    components[i].ratio, swatch_sz, id_of(i), box_bg, is_new(i), any_new);
            if (i + 1 < components.size())
                append_result_plus(m_decomposed_container, m_result_components_sizer, plus_gap, swatch_sz, box_bg, plus_top_pad);
        }
        if (!components.empty())
            m_result_components_sizer->SetMinSize(result_components_inner_width(m_decomposed_container, components.size()), -1);
        else
            m_result_components_sizer->SetMinSize(wxDefaultSize);
        m_decomposed_container->Layout();
        if (wxSizer* box = m_decomposed_container->GetSizer())
            box->SetSizeHints(m_decomposed_container);
        m_decomposed_container->Thaw();
    }

    const bool has_result = has_usable_card();
    if (m_result_arrow)
        m_result_arrow->Show(has_result);
    if (m_decomposed_label)
        m_decomposed_label->Show(has_result);
    if (m_decomposed_container)
        m_decomposed_container->Show(has_result);

    // Both cards of the result row have the same height.
    if (m_filament_card) {
        if (has_result && m_decomposed_container) {
            if (wxSizer* fs = m_filament_card->GetSizer())
                fs->SetSizeHints(m_filament_card);
            const int w = m_filament_card->GetBestSize().GetWidth();
            const int h = m_decomposed_container->GetBestSize().GetHeight();
            m_filament_card->SetMinSize(wxSize(w, h));
            m_filament_card->SetMaxSize(wxSize(w, h));
        } else {
            m_filament_card->SetMinSize(wxDefaultSize);
            m_filament_card->SetMaxSize(wxDefaultSize);
            if (wxSizer* fs = m_filament_card->GetSizer())
                fs->SetSizeHints(m_filament_card);
        }
        m_filament_card->Layout();
        m_filament_card->Refresh();
    }

    Layout();
    fit_around_center();
}

bool ColorDecomposeDialog::try_build_single_base_result(DecomposeMode mode, ColorDecomposeResult& out) const
{
    // Gate by preferred type, matching card visibility: CMYW and RYBW only for PLA Basic.
    if (mode == DecomposeMode::CMYW || mode == DecomposeMode::RYBW) {
        if (!is_pla_family(m_preferred_type))
            return false;
    } else {
        return false;
    }

    static const DecomposeBaseColor cmyw_bases[] = {
        DecomposeBaseColor::Cyan, DecomposeBaseColor::Magenta,
        DecomposeBaseColor::Yellow, DecomposeBaseColor::White
    };
    static const DecomposeBaseColor rybw_bases[] = {
        DecomposeBaseColor::Red, DecomposeBaseColor::Yellow,
        DecomposeBaseColor::Blue, DecomposeBaseColor::White
    };
    const DecomposeBaseColor* bases = (mode == DecomposeMode::CMYW) ? cmyw_bases : rybw_bases;
    const size_t base_count = (mode == DecomposeMode::CMYW)
        ? sizeof(cmyw_bases) / sizeof(cmyw_bases[0])
        : sizeof(rybw_bases) / sizeof(rybw_bases[0]);

    const std::string target_hex = decompose_normalize_color_hex(
        m_target_color.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());

    for (size_t i = 0; i < base_count; ++i) {
        const DecomposeBaseColor base = bases[i];
        DecomposeOfficialComponent official =
            lookup_decompose_official_component(kDecomposePlaBasicType, base, pure_color_for_base(base));
        if (decompose_normalize_color_hex(official.color_hex) != target_hex)
            continue;

        out = ColorDecomposeResult{};
        out.mode = mode;
        out.matched_color = hex_to_wx_colour(official.color_hex, m_target_color);
        DecomposeComponent comp;
        comp.colour = out.matched_color;
        comp.ratio = 100;
        comp.filament_index = -1;
        comp.base_color = base;
        out.components.push_back(comp);
        return true;
    }
    return false;
}

void ColorDecomposeDialog::compute_decomposition()
{
    auto fallback_result = [this](DecomposeMode mode, const std::vector<DecomposeComponent>& components) {
        ColorDecomposeResult result;
        result.mode = mode;
        result.components = components;
        int total = 0;
        double r = 0.0, g = 0.0, b = 0.0;
        for (const auto& comp : result.components)
            total += comp.ratio;
        if (total <= 0)
            total = 100;
        for (const auto& comp : result.components) {
            const double w = static_cast<double>(comp.ratio) / total;
            r += comp.colour.Red() * w;
            g += comp.colour.Green() * w;
            b += comp.colour.Blue() * w;
        }
        result.matched_color = result.components.empty()
            ? m_target_color
            : wxColour(static_cast<unsigned char>(std::clamp(r, 0.0, 255.0)),
                       static_cast<unsigned char>(std::clamp(g, 0.0, 255.0)),
                       static_cast<unsigned char>(std::clamp(b, 0.0, 255.0)));
        return result;
    };

    std::vector<ColorDecomposePhysicalFilament> physical_filaments;
    physical_filaments.reserve(m_physical_colors.size());
    for (size_t i = 0; i < m_physical_colors.size(); ++i) {
        if (m_filament_idx >= 0 && i == static_cast<size_t>(m_filament_idx))
            continue;
        ColorDecomposePhysicalFilament filament;
        filament.color_hex = m_physical_colors[i];
        filament.name = i < m_filament_names.size() ? m_filament_names[i] : "";
        filament.type = i < m_filament_types.size() ? m_filament_types[i] : "";
        filament.filament_index = static_cast<unsigned int>(i + 1);
        physical_filaments.push_back(std::move(filament));
    }

    const ColorDecomposeRgb target_rgb = wx_colour_to_recipe_rgb(m_target_color);

    auto material_recipe = recommend_from_physical_filaments(target_rgb, physical_filaments, m_preferred_type);
    if (material_recipe.valid) {
        m_mode_results[mode_index(DecomposeMode::MaterialList)] =
            to_dialog_result(material_recipe, m_target_color);
    } else {
        std::vector<DecomposeComponent> components;
        for (size_t i = 0; i < std::min<size_t>(2, physical_filaments.size()); ++i) {
            DecomposeComponent comp;
            comp.colour = wxColour(physical_filaments[i].color_hex);
            comp.ratio = 50;
            comp.filament_index = static_cast<int>(physical_filaments[i].filament_index);
            components.push_back(comp);
        }
        if (components.empty()) {
            components.push_back({m_target_color, 100, -1});
        } else if (components.size() == 1) {
            components.front().ratio = 100;
        }
        m_mode_results[mode_index(DecomposeMode::MaterialList)] =
            fallback_result(DecomposeMode::MaterialList, components);
    }

    ColorDecomposeResult single_base;
    if (try_build_single_base_result(DecomposeMode::CMYW, single_base)) {
        m_mode_results[mode_index(DecomposeMode::CMYW)] = single_base;
    } else {
        auto cmyw_recipe = lookup_standard_recipe(target_rgb, ColorDecomposeRecipeMode::CMYW, m_preferred_type);
        m_mode_results[mode_index(DecomposeMode::CMYW)] = cmyw_recipe.valid
            ? to_dialog_result(cmyw_recipe, m_target_color)
            : fallback_result(DecomposeMode::CMYW, {
                {CMYW_YELLOW, 50, -1, DecomposeBaseColor::Yellow},
                {CMYW_CYAN,   50, -1, DecomposeBaseColor::Cyan}
            });
    }

    if (try_build_single_base_result(DecomposeMode::RYBW, single_base)) {
        m_mode_results[mode_index(DecomposeMode::RYBW)] = single_base;
    } else {
        auto rybw_recipe = lookup_standard_recipe(target_rgb, ColorDecomposeRecipeMode::RYBW, m_preferred_type);
        m_mode_results[mode_index(DecomposeMode::RYBW)] = rybw_recipe.valid
            ? to_dialog_result(rybw_recipe, m_target_color)
            : fallback_result(DecomposeMode::RYBW, {
                {RYBW_YELLOW, 50, -1, DecomposeBaseColor::Yellow},
                {RYBW_BLUE,   50, -1, DecomposeBaseColor::Blue}
            });
    }

    // CMYK+W is computed analytically, no recipe table is needed.
    {
        auto cmykw_recipe = lookup_standard_recipe(target_rgb, ColorDecomposeRecipeMode::CMYKW,
                                                   std::string());
        m_mode_results[mode_index(DecomposeMode::CMYKW)] = cmykw_recipe.valid
            ? to_dialog_result(cmykw_recipe, m_target_color)
            : fallback_result(DecomposeMode::CMYKW, {
                {CMYKW_CYAN, 25, -1, DecomposeBaseColor::Cyan},
                {CMYKW_MAGENTA, 25, -1, DecomposeBaseColor::Magenta},
                {CMYKW_YELLOW, 25, -1, DecomposeBaseColor::Yellow},
                {CMYKW_WHITE, 25, -1, DecomposeBaseColor::White}
            });
    }

    m_result = m_mode_results[mode_index(m_selected_mode)];
    update_mode_card_contents();
    update_ok_button_state();
}

namespace {

// Shows the CMYK+W mix of a color: the target, the color the mix gives and the share of each of the five
// fixed filament slots.
class CmykwMixDialog : public DPIDialog
{
public:
    CmykwMixDialog(wxWindow* parent, const ColorDecomposeResult& result, const wxColour& target_color)
        : DPIDialog(parent, wxID_ANY, _L("CMYK+W Mix"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
    {
        build_ui(result, target_color);
        wxGetApp().UpdateDlgDarkUI(this);
        // Dark mode remaps the swatch backgrounds, so set them again.
        for (const auto& fix : m_swatch_colors)
            fix.first->SetBackgroundColour(fix.second);
        Fit();
        CenterOnParent();
    }

protected:
    void on_dpi_changed(const wxRect&) override
    {
        Fit();
        Refresh();
    }

private:
    void build_ui(const ColorDecomposeResult& result, const wxColour& target_color)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        const int margin = FromDIP(30);
        const wxColour box_bg = StateColor::darkModeColorFor(*wxWHITE);
        auto* main_sizer = new wxBoxSizer(wxVERTICAL);
        main_sizer->AddSpacer(FromDIP(22));

        // Target color -> color of the mix
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(create_color_card(_L("Target Color"), target_color), 0, wxALIGN_TOP);
        auto* arrow_col = new wxBoxSizer(wxVERTICAL);
        arrow_col->AddSpacer(FromDIP(26));
        arrow_col->AddStretchSpacer(1);
        arrow_col->Add(create_result_arrow(this), 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, FromDIP(16));
        arrow_col->AddStretchSpacer(1);
        row->Add(arrow_col, 0, wxEXPAND);
        row->Add(create_color_card(_L("Decomposed Colors"), result.matched_color.IsOk() ? result.matched_color : target_color),
                 0, wxALIGN_TOP);
        main_sizer->Add(row, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT, margin);
        main_sizer->AddSpacer(FromDIP(16));

        // One row per slot, in slot order
        struct Slot { DecomposeBaseColor base; const wxColour& color; };
        const Slot slots[] = {
            {DecomposeBaseColor::Cyan, CMYKW_CYAN},       {DecomposeBaseColor::Magenta, CMYKW_MAGENTA},
            {DecomposeBaseColor::Yellow, CMYKW_YELLOW},   {DecomposeBaseColor::Black, CMYKW_BLACK},
            {DecomposeBaseColor::White, CMYKW_WHITE},
        };
        auto* card = create_rounded_result_card(this);
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        const int swatch_sz = FromDIP(32);
        for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
            const int slot = static_cast<int>(i) + 1;
            int ratio = 0;
            for (const DecomposeComponent& comp : result.components)
                if (comp.filament_index == slot)
                    ratio = comp.ratio;
            // Slots that are not part of the mix stay in the list, so it is clear what each slot holds.
            const wxColour text_color = StateColor::darkModeColorFor(ratio > 0 ? COLOR_TEXT_DARK : COLOR_LABEL_GREY);

            auto* slot_row = new wxBoxSizer(wxHORIZONTAL);
            auto* swatch = create_color_swatch(card, slots[i].color, swatch_sz, slot);
            m_swatch_colors.emplace_back(swatch, slots[i].color);
            slot_row->Add(swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(14));
            auto* name = new wxStaticText(card, wxID_ANY, decompose_base_color_display(slots[i].base));
            name->SetFont(Label::Body_14);
            name->SetForegroundColour(text_color);
            name->SetBackgroundColour(box_bg);
            slot_row->Add(name, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(24));
            auto* percent = new wxStaticText(card, wxID_ANY, wxString::Format("%d%%", ratio),
                                             wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT);
            percent->SetFont(Label::Head_14);
            percent->SetForegroundColour(text_color);
            percent->SetBackgroundColour(box_bg);
            percent->SetMinSize(wxSize(FromDIP(48), -1));
            slot_row->Add(percent, 0, wxALIGN_CENTER_VERTICAL);
            card_sizer->Add(slot_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(14));
        }
        card_sizer->AddSpacer(FromDIP(14));
        card->SetSizer(card_sizer);
        main_sizer->Add(card, 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

        auto* hint = new wxStaticText(this, wxID_ANY,
            _L("Load filaments of these colors into slots 1 to 5 and use layer dithering for full-color printing."));
        hint->SetFont(Label::Body_13);
        hint->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6A6A")));
        hint->SetMinSize(wxSize(FromDIP(100), -1));
        main_sizer->Add(hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, margin);

        auto* ok = new Button(this, _L("OK"));
        ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
        main_sizer->AddSpacer(FromDIP(16));
        main_sizer->Add(ok, 0, wxALIGN_RIGHT | wxLEFT | wxRIGHT | wxBOTTOM, margin);

        SetSizer(main_sizer);
        SetMinSize(wxSize(FromDIP(477), -1));
        Fit();
        // Wrap the hint to the width of the dialog, then fit again for its height.
        hint->Wrap(GetClientSize().x - 2 * margin);
        Fit();
    }

    // A rounded card with a caption, a swatch and its RGB value.
    wxSizer* create_color_card(const wxString& caption, const wxColour& color)
    {
        const wxColour box_bg = StateColor::darkModeColorFor(*wxWHITE);
        auto* col = new wxBoxSizer(wxVERTICAL);
        col->Add(create_result_caption(this, caption), 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(8));

        auto* card = create_rounded_result_card(this);
        auto* inner = new wxBoxSizer(wxVERTICAL);
        auto* swatch = create_color_swatch(card, color, FromDIP(32));
        m_swatch_colors.emplace_back(swatch, color);
        inner->Add(swatch, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, FromDIP(4));
        auto* rgb = new wxStaticText(card, wxID_ANY, format_rgb(color));
        rgb->SetFont(Label::Body_12);
        rgb->SetForegroundColour(StateColor::darkModeColorFor(COLOR_LABEL_GREY));
        rgb->SetBackgroundColour(box_bg);
        inner->Add(rgb, 0, wxALIGN_CENTER_HORIZONTAL);
        auto* card_sizer = new wxBoxSizer(wxVERTICAL);
        card_sizer->Add(inner, 0, wxALL, FromDIP(12));
        card->SetSizer(card_sizer);
        col->Add(card, 0, wxALIGN_CENTER_HORIZONTAL);
        return col;
    }

    std::vector<std::pair<wxPanel*, wxColour>> m_swatch_colors;
};

} // namespace

void show_cmykw_mix_dialog(wxWindow* parent, const ColorDecomposeResult& result, const wxColour& target_color)
{
    CmykwMixDialog dlg(parent, result, target_color);
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
