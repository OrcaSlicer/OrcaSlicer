#include "ExtraRenderers.hpp"
#include "wxExtensions.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "BitmapComboBox.hpp"
#include "Plater.hpp"
#include "Widgets/ComboBox.hpp"
#include "GUI_ObjectList.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"

#include "libslic3r/Technologies.hpp"
#include <wx/dataview.h>
#include "slic3r/GUI/ObjectDataViewModel.hpp"
#include <vector>
#include <utility>
#include <string>
#include <map>
#include <cctype>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <wx/dc.h>
#include <wx/setup.h>
#include <wx/rtti.h>
#include <wx/object.h>
#include <wx/variant.h>
#include <wx/gdicmn.h>
#include <wx/string.h>
#include <wx/event.h>
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/DropDown.hpp"
#include <wx/dvrenderers.h>
#ifdef wxHAS_GENERIC_DATAVIEWCTRL
#include "wx/generic/private/markuptext.h"
#include "wx/generic/private/rowheightcache.h"
#include "wx/generic/private/widthcalc.h"
#endif
/*
#ifdef __WXGTK__
#include "wx/gtk/private.h"
#include "wx/gtk/private/value.h"
#endif
*/
#if wxUSE_ACCESSIBILITY
#include "wx/private/markupparser.h"
#endif // wxUSE_ACCESSIBILITY


//-----------------------------------------------------------------------------
// DataViewBitmapText
//-----------------------------------------------------------------------------

wxIMPLEMENT_DYNAMIC_CLASS(DataViewBitmapText, wxObject)

IMPLEMENT_VARIANT_OBJECT(DataViewBitmapText)

// ---------------------------------------------------------
// BitmapTextRenderer
// ---------------------------------------------------------

#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING
BitmapTextRenderer::BitmapTextRenderer(wxDataViewCellMode mode /*= wxDATAVIEW_CELL_EDITABLE*/, 
                                                 int align /*= wxDVR_DEFAULT_ALIGNMENT*/): 
wxDataViewRenderer(wxT("PrusaDataViewBitmapText"), mode, align)
{
    SetMode(mode);
    SetAlignment(align);
}
#endif // ENABLE_NONCUSTOM_DATA_VIEW_RENDERING

BitmapTextRenderer::~BitmapTextRenderer()
{
#ifdef SUPPORTS_MARKUP
    #ifdef wxHAS_GENERIC_DATAVIEWCTRL
    delete m_markupText;
    #endif //wxHAS_GENERIC_DATAVIEWCTRL
#endif // SUPPORTS_MARKUP
}

void BitmapTextRenderer::EnableMarkup(bool enable)
{
#ifdef SUPPORTS_MARKUP
#ifdef wxHAS_GENERIC_DATAVIEWCTRL
    if (enable) {
        if (!m_markupText)
            m_markupText = new wxItemMarkupText(wxString());
    }
    else {
        if (m_markupText) {
            delete m_markupText;
            m_markupText = nullptr;
        }
    }
#else
    is_markupText = enable;
#endif //wxHAS_GENERIC_DATAVIEWCTRL
#endif // SUPPORTS_MARKUP
}

bool BitmapTextRenderer::SetValue(const wxVariant &value)
{
    m_value << value;

#ifdef SUPPORTS_MARKUP
#ifdef wxHAS_GENERIC_DATAVIEWCTRL
    if (m_markupText)
        m_markupText->SetMarkup(m_value.GetText());
    /* 
#else 
#if defined(__WXGTK__)
   GValue gvalue = G_VALUE_INIT;
    g_value_init(&gvalue, G_TYPE_STRING);
    g_value_set_string(&gvalue, wxGTK_CONV_FONT(str.GetText(), GetOwner()->GetOwner()->GetFont()));
    g_object_set_property(G_OBJECT(m_renderer/ *.GetText()* /), is_markupText ? "markup" : "text", &gvalue);
    g_value_unset(&gvalue);
#endif // __WXGTK__
    */
#endif // wxHAS_GENERIC_DATAVIEWCTRL
#endif // SUPPORTS_MARKUP

    return true;
}

bool BitmapTextRenderer::GetValue(wxVariant& WXUNUSED(value)) const
{
    return false;
}

#if ENABLE_NONCUSTOM_DATA_VIEW_RENDERING && wxUSE_ACCESSIBILITY
wxString BitmapTextRenderer::GetAccessibleDescription() const
{
#ifdef SUPPORTS_MARKUP
    if (m_markupText)
        return wxMarkupParser::Strip(m_text);
#endif // SUPPORTS_MARKUP

    return m_value.GetText();
}
#endif // wxUSE_ACCESSIBILITY && ENABLE_NONCUSTOM_DATA_VIEW_RENDERING

bool BitmapTextRenderer::Render(wxRect rect, wxDC *dc, int state)
{
    int xoffset = 0;

    const wxBitmap& icon = m_value.GetBitmap();
    if (icon.IsOk())
    {
#ifdef __APPLE__
        wxSize icon_sz = icon.GetScaledSize();
#else
        wxSize icon_sz = icon.GetSize();
#endif
        dc->DrawBitmap(icon, rect.x, rect.y + (rect.height - icon_sz.y) / 2);
        xoffset = icon_sz.x + 4;
    }

#if defined(SUPPORTS_MARKUP) && defined(wxHAS_GENERIC_DATAVIEWCTRL)
    if (m_markupText)
    {
        rect.x += xoffset;
        m_markupText->Render(GetView(), *dc, rect, 0, GetEllipsizeMode());
    }
    else
#endif // SUPPORTS_MARKUP && wxHAS_GENERIC_DATAVIEWCTRL
#ifdef _WIN32 
        // workaround for Windows DarkMode : Don't respect to the state & wxDATAVIEW_CELL_SELECTED to avoid update of the text color
        RenderText(m_value.GetText(), xoffset, rect, dc, state & wxDATAVIEW_CELL_SELECTED ? 0 :state);
#else
        RenderText(m_value.GetText(), xoffset, rect, dc, state);
#endif

    return true;
}

wxSize BitmapTextRenderer::GetSize() const
{
    if (!m_value.GetText().empty())
    {
        wxSize size;
#if defined(SUPPORTS_MARKUP) && defined(wxHAS_GENERIC_DATAVIEWCTRL)
        if (m_markupText)
        {
            wxDataViewCtrl* const view = GetView();
            wxClientDC dc(view);
            if (GetAttr().HasFont())
                dc.SetFont(GetAttr().GetEffectiveFont(view->GetFont()));

            size = m_markupText->Measure(dc);

            int lines = m_value.GetText().Freq('\n') + 1;
            size.SetHeight(size.GetHeight() * lines);
        }
        else
#endif // SUPPORTS_MARKUP && wxHAS_GENERIC_DATAVIEWCTRL
        {
            size = GetTextExtent(m_value.GetText());
            size.x = size.x * 9 / 8;
        }

        if (m_value.GetBitmap().IsOk())
            size.x += m_value.GetBitmap().GetWidth() + 4;
        return size;
    }
    return wxSize(80, 20);
}


wxWindow* BitmapTextRenderer::CreateEditorCtrl(wxWindow* parent, wxRect labelRect, const wxVariant& value)
{
    if (can_create_editor_ctrl && !can_create_editor_ctrl())
        return nullptr;

    DataViewBitmapText data;
    data << value;

    m_was_unusable_symbol = false;

    wxPoint position = labelRect.GetPosition();
    if (data.GetBitmap().IsOk()) {
        const int bmp_width = data.GetBitmap().GetWidth();
        position.x += bmp_width;
        labelRect.SetWidth(labelRect.GetWidth() - bmp_width);
    }

#ifdef __WXMSW__
    // Case when from some reason we try to create next EditorCtrl till old one was not deleted
    if (auto children = parent->GetChildren(); children.GetCount() > 0)
        for (auto child : children)
            if (dynamic_cast<wxTextCtrl*>(child)) {
                parent->RemoveChild(child);
                child->Destroy();
                break;
            }
#endif // __WXMSW__

    wxTextCtrl* text_editor = new wxTextCtrl(parent, wxID_ANY, data.GetText(),
                                             position, labelRect.GetSize(), wxTE_PROCESS_ENTER);
    text_editor->SetInsertionPointEnd();
    text_editor->SelectAll();
    text_editor->SetBackgroundColour(parent->GetBackgroundColour());
    text_editor->SetForegroundColour(parent->GetForegroundColour());

    return text_editor;
}

bool BitmapTextRenderer::GetValueFromEditorCtrl(wxWindow* ctrl, wxVariant& value)
{
    wxTextCtrl* text_editor = wxDynamicCast(ctrl, wxTextCtrl);
    auto item = GetView()->GetModel()->GetParent(m_item);
    if (!text_editor || (item.IsOk() && text_editor->GetValue().IsEmpty()))
        return false;

    m_was_unusable_symbol = Slic3r::GUI::Plater::has_illegal_filename_characters(text_editor->GetValue());
    if (m_was_unusable_symbol)
        return false;

    // The icon can't be edited so get its old value and reuse it.
    wxVariant valueOld;
    GetView()->GetModel()->GetValue(valueOld, m_item, Slic3r::GUI::ColumnNumber::colName); 
    
    DataViewBitmapText bmpText;
    bmpText << valueOld;

    // But replace the text with the value entered by user.
    bmpText.SetText(text_editor->GetValue());

    value << bmpText;
    return true;
}

// ----------------------------------------------------------------------------
// BitmapChoiceRenderer
// ----------------------------------------------------------------------------

bool BitmapChoiceRenderer::SetValue(const wxVariant& value)
{
    m_value << value;
    return true;
}

bool BitmapChoiceRenderer::GetValue(wxVariant& value) const 
{
    value << m_value;
    return true;
}

// return the human-readable filament name (e.g. "PLA Matte") for the
// extruder index stored as text in the filament column ("1", "2", ... or "default").
// Returns an empty string when no meaningful name is available.
static wxString get_filament_label_for_extruder_text(const wxString& extruder_text)
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;

    const int extruder_idx = atoi(extruder_text.c_str()); // 1-based, 0 means "default"/none
    if (extruder_idx <= 0)
        return wxEmptyString;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return wxEmptyString;

    const std::vector<std::string>& filament_presets = preset_bundle->filament_presets;
    const size_t pos = (size_t)(extruder_idx - 1);
    if (pos >= filament_presets.size())
        return wxEmptyString;

    const Preset* preset = preset_bundle->filaments.find_preset(filament_presets[pos]);
    if (preset == nullptr)
        return wxEmptyString;

    return from_u8(preset->label(false));
}

// for dual-nozzle printers, return the nozzle-side suffix for a 1-based filament index:
// " (L)" if the filament is mapped to the left nozzle, " (R)" for the right nozzle.
// Returns an empty string for single-nozzle printers or when the mapping is unknown.
wxString get_filament_nozzle_suffix(int extruder_idx_1based)
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;

    if (extruder_idx_1based <= 0)
        return wxEmptyString;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return wxEmptyString;

    // Single-nozzle printers have no left/right distinction.
    const auto* nozzle_opt = preset_bundle->printers.get_edited_preset().config
                                 .option<ConfigOptionFloatsNullable>("nozzle_diameter");
    if (nozzle_opt == nullptr || nozzle_opt->values.size() < 2)
        return wxEmptyString;

    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return wxEmptyString;

    // The left/right split is written into "filament_map" when filaments are synced
    // (1 = left, 2 = right). Only meaningful once the printer info has been synced.
    if (!plater->get_machine_sync_status())
        return wxEmptyString;

    // Read the global "filament_map" written by the AMS sync (1 = left, 2 = right). Use the
    // global project config directly — the per-plate map can shadow it with a stale value.
    const auto* map_opt = preset_bundle->project_config.option<ConfigOptionInts>("filament_map");
    if (map_opt == nullptr)
        return wxEmptyString;
    const size_t pos = (size_t)(extruder_idx_1based - 1);
    if (pos >= map_opt->values.size())
        return wxEmptyString;

    switch (map_opt->values[pos]) {
    case 1:  return " (L)";
    case 2:  return " (R)";
    default: return wxEmptyString;
    }
}

// full filament cell label for a given extruder text ("1", "2", ... or "default"):
// the filament name plus the nozzle-side suffix, e.g. "PLA Matte (L)".
static wxString get_filament_cell_label(const wxString& extruder_text)
{
    const wxString name = get_filament_label_for_extruder_text(extruder_text);
    if (name.IsEmpty())
        return wxEmptyString;
    return name + get_filament_nozzle_suffix(atoi(extruder_text.c_str()));
}

// return a human-readable color name for a hex string (e.g. "#1A1A1A" -> "Black"),
// picking the nearest entry from a small table of common colors. Falls back to the input.
static wxString get_color_display_name(const wxString& hex_in)
{
    wxString hex = hex_in;
    if (hex.StartsWith("#") && hex.length() > 7)
        hex = hex.substr(0, 7); // drop any alpha component (#RRGGBBAA -> #RRGGBB)

    wxColour c(hex);
    if (!c.IsOk())
        return hex_in;

    struct NamedColor { const char* name; int r, g, b; };
    static const NamedColor table[] = {
        {L("Black"), 0, 0, 0},        {L("White"), 255, 255, 255},   {L("Gray"), 128, 128, 128},
        {L("Silver"), 192, 192, 192}, {L("Red"), 220, 20, 20},       {L("Dark Red"), 139, 0, 0},
        {L("Orange"), 255, 140, 0},   {L("Yellow"), 240, 225, 40},   {L("Gold"), 212, 175, 55},
        {L("Green"), 30, 160, 60},    {L("Dark Green"), 0, 100, 0},  {L("Lime"), 160, 210, 60},
        {L("Cyan"), 0, 200, 200},     {L("Blue"), 30, 80, 200},      {L("Navy"), 20, 30, 90},
        {L("Sky Blue"), 110, 170, 220}, {L("Purple"), 128, 0, 128},  {L("Magenta"), 200, 40, 160},
        {L("Pink"), 240, 150, 180},   {L("Brown"), 120, 70, 40},     {L("Beige"), 225, 210, 170},
    };

    long best_dist = 2000000000L;
    const char* best_name = "";
    for (const NamedColor& nc : table) {
        const long dr = (long)c.Red() - nc.r;
        const long dg = (long)c.Green() - nc.g;
        const long db = (long)c.Blue() - nc.b;
        const long dist = dr * dr + dg * dg + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best_name = nc.name;
        }
    }
    return best_name[0] ? _L(best_name) : wxString();
}

// for each project filament (0-based), the AMS tray it is loaded in (e.g. "A1", "HT-B",
// "Ext"), derived from the synced AMS list by matching color + filament id. Empty when unknown.
static std::vector<wxString> build_filament_ams_locations()
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;

    std::vector<wxString> result;

    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (preset_bundle == nullptr)
        return result;

    const std::map<int, DynamicPrintConfig>& ams_list = preset_bundle->filament_ams_list;
    if (ams_list.empty())
        return result;

    const auto* color_opt = preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (color_opt == nullptr)
        return result;
    const std::vector<std::string>& proj_colors = color_opt->values;
    result.assign(proj_colors.size(), wxString());

    auto norm_color = [](std::string s) -> std::string {
        if (!s.empty() && s[0] == '#') s.erase(0, 1);
        for (char& c : s) c = (char) ::toupper((unsigned char) c);
        if (s.size() > 6) s = s.substr(0, 6);
        return s;
    };
    auto get_str = [](const DynamicPrintConfig& cfg, const char* key) -> std::string {
        const ConfigOption* opt = cfg.option(key);
        if (opt == nullptr) return std::string();
        if (const auto* s = dynamic_cast<const ConfigOptionStrings*>(opt))
            return s->values.empty() ? std::string() : s->values.front();
        if (const auto* s = dynamic_cast<const ConfigOptionString*>(opt))
            return s->value;
        return std::string();
    };

    struct AmsEntry { std::string color; std::string id; wxString name; bool used; };
    std::vector<AmsEntry> entries;
    entries.reserve(ams_list.size());
    for (const auto& kv : ams_list) {
        AmsEntry e;
        e.color = norm_color(get_str(kv.second, "filament_colour"));
        e.id    = get_str(kv.second, "filament_id");
        e.name  = from_u8(get_str(kv.second, "tray_name"));
        e.used  = false;
        entries.push_back(std::move(e));
    }

    for (size_t i = 0; i < proj_colors.size(); ++i) {
        const std::string pcolor = norm_color(proj_colors[i]);
        std::string pid;
        if (i < preset_bundle->filament_presets.size()) {
            if (const Preset* preset = preset_bundle->filaments.find_preset(preset_bundle->filament_presets[i]))
                pid = get_str(preset->config, "filament_id");
        }

        int found = -1;
        for (size_t j = 0; j < entries.size(); ++j)
            if (!entries[j].used && entries[j].color == pcolor && !pid.empty() && entries[j].id == pid) { found = (int) j; break; }
        if (found < 0)
            for (size_t j = 0; j < entries.size(); ++j)
                if (!entries[j].used && entries[j].color == pcolor) { found = (int) j; break; }
        if (found < 0 && !pid.empty())
            for (size_t j = 0; j < entries.size(); ++j)
                if (!entries[j].used && entries[j].id == pid) { found = (int) j; break; }

        if (found >= 0) {
            entries[found].used = true;
            result[i] = entries[found].name;
        }
    }

    return result;
}

// human-readable AMS location for a tray name ("A1" -> "AMS A1", "HT-B" -> "AMS HT-B",
// "Ext" -> "External spool"). Empty input yields an empty string.
static wxString format_ams_location(const wxString& tray_name)
{
    if (tray_name.IsEmpty())
        return wxEmptyString;
    if (tray_name == "Ext")
        return _L("External spool");
    return _L("AMS") + " " + tray_name;
}

wxString get_filament_column_tooltip(const wxString& extruder_text)
{
    using namespace Slic3r;
    using namespace Slic3r::GUI;

    const int idx = atoi(extruder_text.c_str());

    wxString label = get_filament_label_for_extruder_text(extruder_text);
    if (!label.IsEmpty())
        label += get_filament_nozzle_suffix(idx);

    wxString color_name;
    wxString location;
    if (idx > 0) {
        if (PresetBundle* pb = wxGetApp().preset_bundle) {
            if (const auto* opt = pb->project_config.option<ConfigOptionStrings>("filament_colour")) {
                const size_t pos = (size_t)(idx - 1);
                if (pos < opt->values.size())
                    color_name = get_color_display_name(from_u8(opt->values[pos]));
            }
        }
        const std::vector<wxString> locations = build_filament_ams_locations();
        if ((size_t)(idx - 1) < locations.size())
            location = format_ams_location(locations[idx - 1]);
    }

    // Compose: "<name> — <color> — <AMS location>" (omitting any empty parts).
    wxString out = label;
    auto append = [&out](const wxString& part) {
        if (part.IsEmpty()) return;
        if (!out.IsEmpty()) out += " — ";
        out += part;
    };
    if (label.IsEmpty()) out = color_name; else append(color_name);
    append(location);
    return out;
}

bool BitmapChoiceRenderer::Render(wxRect rect, wxDC* dc, int state)
{
    int xoffset = 0;

    const wxBitmap& icon = m_value.GetBitmap();
    if (icon.IsOk())
    {
        dc->DrawBitmap(icon, rect.x, rect.y + (rect.height - icon.GetHeight()) / 2);
        xoffset = icon.GetWidth() + 4;

        if (rect.height == 0)
          rect.height = icon.GetHeight();
    }

    // filament name (left, after the swatch) and the nozzle side (right-aligned at the cell edge).
#ifdef _WIN32
    // workaround for Windows DarkMode : Don't respect to the state & wxDATAVIEW_CELL_SELECTED to avoid update of the text color
    const int text_state = state & wxDATAVIEW_CELL_SELECTED ? 0 : state;
#else
    const int text_state = state;
#endif

    const wxString name   = get_filament_label_for_extruder_text(m_value.GetText());
    wxString       suffix = name.IsEmpty() ? wxString() : get_filament_nozzle_suffix(atoi(m_value.GetText().c_str()));
    suffix.Trim(false); // drop the leading space used when appended inline

    int suffix_w = 0;
    if (!suffix.IsEmpty())
        suffix_w = dc->GetTextExtent(suffix).x;

    if (!name.IsEmpty()) {
        // Reserve room on the right for the side label so the name doesn't run under it.
        wxRect name_rect = rect;
        if (suffix_w > 0)
            name_rect.width = std::max(0, name_rect.width - (suffix_w + 8));
        RenderText(name, xoffset, name_rect, dc, text_state);
    }

    if (suffix_w > 0) {
        // Right-align the side label at the cell's right edge.
        const int xo = std::max(xoffset, rect.width - suffix_w - 2);
        RenderText(suffix, xo, rect, dc, text_state);
    }

    return true;
}

wxSize BitmapChoiceRenderer::GetSize() const
{
    const wxString label = get_filament_cell_label(m_value.GetText());
    wxSize sz = label.IsEmpty() ? wxSize(0, 0) : GetTextExtent(label);

    if (m_value.GetBitmap().IsOk()) {
        sz.x += m_value.GetBitmap().GetWidth() + 4;
        sz.y = std::max(sz.y, m_value.GetBitmap().GetHeight() + 4);
    }

    return sz;
}


wxWindow* BitmapChoiceRenderer::CreateEditorCtrl(wxWindow* parent, wxRect labelRect, const wxVariant& value)
{
    if (can_create_editor_ctrl && !can_create_editor_ctrl())
        return nullptr;

    // Filaments and their nozzle map may have been synced since the rows were last painted, so
    // repaint the column to match the labels in the dropdown that is about to open.
    if (Slic3r::GUI::ObjectList* obj_list = Slic3r::GUI::wxGetApp().obj_list())
        obj_list->Refresh();

    std::vector<wxBitmap*> icons = get_extruder_color_icons();
    if (icons.empty())
        return nullptr;

    DataViewBitmapText data;
    data << value;

    // Keep the item text visible so the filament name shows next to each color swatch.
    ::ComboBox *c_editor = new ::ComboBox(parent, wxID_ANY, wxEmptyString,
        labelRect.GetTopLeft(), wxSize(labelRect.GetWidth(), -1),
        0, nullptr, wxCB_READONLY | CB_NO_DROP_ICON);
    c_editor->GetDropDown().SetUseContentWidth(true);

    if (has_default_extruder && has_default_extruder())
        c_editor->Append(_L("default"), *get_default_extruder_color_icon());

    // Filament colors and AMS locations for the hover tooltip.
    std::vector<std::string> fila_colors;
    if (Slic3r::PresetBundle* pb = Slic3r::GUI::wxGetApp().preset_bundle) {
        if (const auto* opt = pb->project_config.option<Slic3r::ConfigOptionStrings>("filament_colour"))
            fila_colors = opt->values;
    }
    const std::vector<wxString> ams_locations = build_filament_ams_locations();

    for (size_t i = 0; i < icons.size(); i++) {
        // Label each choice with the filament name (left) and the nozzle side (right-aligned),
        // falling back to the index when no filament name is available.
        const int      idx   = (int)i + 1;
        const wxString name  = get_filament_label_for_extruder_text(wxString::Format("%d", idx));
        const wxString label = name.IsEmpty() ? wxString::Format("%d", idx) : name;
        const int item_index = c_editor->Append(label, *icons[i]);

        wxString suffix = get_filament_nozzle_suffix(idx);
        suffix.Trim(false); // drop the leading space used for inline rendering
        if (!suffix.IsEmpty())
            c_editor->SetItemRightText(item_index, suffix);

        // The tooltip shows the readable color name and the AMS location, e.g. "Black — AMS HT-B".
        wxString tip;
        if (i < fila_colors.size())
            tip = get_color_display_name(Slic3r::GUI::from_u8(fila_colors[i]));
        if (i < ams_locations.size()) {
            const wxString loc = format_ams_location(ams_locations[i]);
            if (!loc.IsEmpty())
                tip = tip.IsEmpty() ? loc : (tip + " — " + loc);
        }
        if (!tip.IsEmpty())
            c_editor->SetItemTooltip(item_index, tip);
    }

    if (has_default_extruder && has_default_extruder())
        c_editor->SetSelection(atoi(data.GetText().c_str()));
    else
        c_editor->SetSelection(atoi(data.GetText().c_str()) - 1);

    c_editor->Bind(wxEVT_SET_FOCUS, [c_editor](wxFocusEvent& evt) {
#ifdef __WXGTK__
        // On wxGTK the data-view editor may receive focus before its native
        // window is mapped. Opening the popup one event later avoids creating
        // the GTK popup without a valid toplevel parent.
        c_editor->CallAfter([c_editor]() {
            if (c_editor->IsShownOnScreen())
                c_editor->ForceDropdownOpen();
        });
#else
        c_editor->ForceDropdownOpen();
#endif
        evt.Skip(); 
    });

    // Close editor after selection is made
    c_editor->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.StopPropagation(); 
        // FinishEditing grabs new selection and triggers config update. We better call
        // it explicitly, automatic update on KILL_FOCUS didn't work on Linux.
        this->FinishEditing();
    });

    return c_editor;
}

bool BitmapChoiceRenderer::GetValueFromEditorCtrl(wxWindow* ctrl, wxVariant& value)
{
    auto* c = dynamic_cast<::ComboBox*>(ctrl);
    if (!c)
        return false;

    int selection = c->GetSelection();
    if (selection < 0)
        return false;
   
    DataViewBitmapText bmpText;

    // The visible item label is the filament name, but the stored value stays the numeric extruder
    // index, so derive it from the selection position rather than from the label text.
    const bool has_default = has_default_extruder && has_default_extruder();
    bmpText.SetText(wxString::Format("%d", has_default ? selection : selection + 1));
    bmpText.SetBitmap(c->GetItemBitmap(selection));

    value << bmpText;
    return true;
}

// ----------------------------------------------------------------------------
// TextRenderer
// ----------------------------------------------------------------------------

bool TextRenderer::SetValue(const wxVariant& value)
{
    m_value = value.GetString();
    return true;
}

bool TextRenderer::GetValue(wxVariant& value) const
{
    return false;
}

bool TextRenderer::Render(wxRect rect, wxDC* dc, int state)
{
#ifdef _WIN32
    // workaround for Windows DarkMode : Don't respect to the state & wxDATAVIEW_CELL_SELECTED to avoid update of the text color
    RenderText(m_value, 0, rect, dc, state & wxDATAVIEW_CELL_SELECTED ? 0 : state);
#else
    RenderText(m_value, 0, rect, dc, state);
#endif

    return true;
}

wxSize TextRenderer::GetSize() const
{
    return GetTextExtent(m_value);
}

