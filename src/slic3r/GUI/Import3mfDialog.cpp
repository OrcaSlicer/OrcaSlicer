#include "Import3mfDialog.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "MainFrame.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/RadioGroup.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/DropDown.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <algorithm>

namespace Slic3r {
namespace GUI {

//------------------------------------------------------------------------------
// FilamentMappingRow implementation
//------------------------------------------------------------------------------

FilamentMappingRow::FilamentMappingRow(wxWindow* parent, int filament_index, const wxColour& color,
                                       const std::vector<std::string>& available_filaments)
    : wxPanel(parent, wxID_ANY)
    , m_filament_index(filament_index)
    , m_color(color)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    
    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    
    // Color swatch (custom painted panel) - shows project's filament color
    m_color_swatch = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
    m_color_swatch->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
    m_color_swatch->Bind(wxEVT_PAINT, &FilamentMappingRow::on_paint_swatch, this);
    sizer->Add(m_color_swatch, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    
    // Slot dropdown - maps project filament to user's filament preset
    m_slot_dropdown = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, 
                                   wxSize(FromDIP(180), -1), 0, NULL, wxCB_READONLY);
    
    populate_slot_dropdown(available_filaments);
    
    // Default to first actual item (skip separator at index 0 if present)
    if (m_slot_dropdown->GetCount() > 0) {
        int first_item = m_separator_indices.empty() ? 0 : 1;
        if (first_item < (int)m_slot_dropdown->GetCount()) {
            m_slot_dropdown->SetSelection(first_item);
        }
    }
    
    sizer->Add(m_slot_dropdown, 1, wxALIGN_CENTER_VERTICAL);
    
    SetSizer(sizer);
}

void FilamentMappingRow::populate_slot_dropdown(const std::vector<std::string>& available_filaments)
{
    if (!m_slot_dropdown) return;
    
    m_slot_dropdown->Clear();
    m_filament_names.clear();
    m_separator_indices.clear();
    
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return;
    
    // Group filaments similar to sidebar: User presets first (no submenu), then system by vendor
    // Using the same approach as PlaterPresetComboBox::update()
    struct PresetInfo {
        std::string name;       // Internal preset name
        std::string label;      // Display label
        std::string vendor;     // Vendor for grouping
        std::string type;       // Filament type for sorting
    };
    
    std::vector<PresetInfo> user_presets;
    std::vector<PresetInfo> system_presets;
    
    for (const std::string& preset_name : available_filaments) {
        const Preset* preset = preset_bundle->filaments.find_preset(preset_name, false);
        if (!preset) continue;
        
        PresetInfo info;
        info.name = preset_name;
        info.label = preset->label(false);  // Use alias if available
        
        // Get vendor from config
        const ConfigOptionStrings* vendor_opt = preset->config.option<ConfigOptionStrings>("filament_vendor");
        if (vendor_opt && !vendor_opt->values.empty() && !vendor_opt->values[0].empty()) {
            info.vendor = vendor_opt->values[0];
            // Normalize "Bambu Lab" to "Bambu" like sidebar does
            if (info.vendor == "Bambu Lab") info.vendor = "Bambu";
        } else {
            info.vendor = "Other";
        }
        
        // Get filament type for sorting
        const ConfigOptionStrings* type_opt = preset->config.option<ConfigOptionStrings>("filament_type");
        if (type_opt && !type_opt->values.empty()) {
            info.type = type_opt->values[0];
        }
        
        if (!preset->is_system) {
            user_presets.push_back(info);
        } else {
            system_presets.push_back(info);
        }
    }
    
    // Sort user presets alphabetically by label
    std::sort(user_presets.begin(), user_presets.end(), 
              [](const auto& a, const auto& b) { return a.label < b.label; });
    
    // Sort system presets by vendor priority, then type priority, then alphabetically
    // Same priority as sidebar: Bambu first, then Generic, then others
    std::vector<std::string> priority_vendors = {"Bambu", "Generic"};
    std::vector<std::string> priority_types = {"PLA", "PETG", "ABS", "TPU"};
    
    std::sort(system_presets.begin(), system_presets.end(), 
              [&priority_vendors, &priority_types](const auto& a, const auto& b) {
        // Compare vendor priority
        auto vendor_idx_a = std::find(priority_vendors.begin(), priority_vendors.end(), a.vendor);
        auto vendor_idx_b = std::find(priority_vendors.begin(), priority_vendors.end(), b.vendor);
        if (vendor_idx_a != vendor_idx_b)
            return vendor_idx_a < vendor_idx_b;
        
        // Compare type priority  
        auto type_idx_a = std::find(priority_types.begin(), priority_types.end(), a.type);
        auto type_idx_b = std::find(priority_types.begin(), priority_types.end(), b.type);
        if (type_idx_a != type_idx_b)
            return type_idx_a < type_idx_b;
        
        // Alphabetical by label
        return a.label < b.label;
    });
    
    // Add user presets at top level (no submenu, just a separator header like sidebar)
    if (!user_presets.empty()) {
        // Add separator header for user presets
        m_separator_indices.push_back(m_slot_dropdown->GetCount());
        m_slot_dropdown->Append(_L("User presets"), wxNullBitmap, wxEmptyString, nullptr, DD_ITEM_STYLE_SPLIT_ITEM);
        
        for (const auto& info : user_presets) {
            m_slot_dropdown->Append(from_u8(info.label), wxNullBitmap);
            m_filament_names.push_back(info.name);
        }
    }
    
    // Add system presets separator header, then grouped by vendor in submenus
    if (!system_presets.empty()) {
        m_separator_indices.push_back(m_slot_dropdown->GetCount());
        m_slot_dropdown->Append(_L("System presets"), wxNullBitmap, wxEmptyString, nullptr, DD_ITEM_STYLE_SPLIT_ITEM);
        
        for (const auto& info : system_presets) {
            m_slot_dropdown->Append(from_u8(info.label), wxNullBitmap, from_u8(info.vendor));
            m_filament_names.push_back(info.name);
        }
    }
}

void FilamentMappingRow::on_paint_swatch(wxPaintEvent& evt)
{
    wxPaintDC dc(m_color_swatch);
    wxSize size = m_color_swatch->GetClientSize();
    
    // Fill with color
    dc.SetBrush(wxBrush(m_color));
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, 3);
    
    // Border
    wxColour border_color = wxGetApp().dark_mode() ? wxColour(80, 80, 80) : wxColour(180, 180, 180);
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(border_color, 1));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, 3);
}

std::string FilamentMappingRow::get_selected_filament() const
{
    if (!m_slot_dropdown) return "";
    
    int sel = m_slot_dropdown->GetSelection();
    if (sel == wxNOT_FOUND || sel < 0) return "";
    
    // Count how many separators are before this selection index
    int separator_count = 0;
    for (int sep_idx : m_separator_indices) {
        if (sep_idx < sel) {
            separator_count++;
        }
    }
    
    // The actual filament index is selection minus separators before it
    int filament_idx = sel - separator_count;
    
    if (filament_idx >= 0 && filament_idx < (int)m_filament_names.size()) {
        return m_filament_names[filament_idx];
    }
    
    return "";
}

//------------------------------------------------------------------------------
// Import3mfDialog implementation
//------------------------------------------------------------------------------

Import3mfDialog::Import3mfDialog(const std::string& filename)
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Import 3MF File"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_action(1)  // Default to "Open as project"
{
    // Background color
    SetBackgroundColour(wxGetApp().dark_mode() ? wxColour(45, 45, 49) : wxColour(255, 255, 255));
    m_def_color = GetBackgroundColour();

    // Icon
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    // Collect available filament presets for mapping dropdowns
    collect_available_filaments();

    // Main sizer
    m_main_sizer = new wxBoxSizer(wxVERTICAL);

    // Top accent line
    m_top_line = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(2)));
    m_top_line->SetBackgroundColour(wxColour(0, 150, 136));  // Teal accent
    m_main_sizer->Add(m_top_line, 0, wxEXPAND);

    m_main_sizer->AddSpacer(FromDIP(16));

    // Title and filename section
    wxBoxSizer* title_sizer = new wxBoxSizer(wxVERTICAL);
    
    m_fname_title = new wxStaticText(this, wxID_ANY, _L("Please select an action"));
    m_fname_title->SetFont(::Label::Body_14);
    m_fname_title->SetForegroundColour(wxGetApp().dark_mode() ? wxColour(180, 180, 180) : wxColour(107, 107, 107));
    title_sizer->Add(m_fname_title, 0, wxBOTTOM, FromDIP(4));

    // Filename display (may wrap to two lines)
    wxBoxSizer* fname_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_fname_f = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_fname_f->SetFont(::Label::Head_14);
    m_fname_f->SetForegroundColour(wxGetApp().dark_mode() ? wxColour(220, 220, 220) : wxColour(38, 46, 48));
    fname_sizer->Add(m_fname_f, 1);
    title_sizer->Add(fname_sizer, 0, wxEXPAND);

    m_fname_s = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_fname_s->SetFont(::Label::Head_14);
    m_fname_s->SetForegroundColour(wxGetApp().dark_mode() ? wxColour(220, 220, 220) : wxColour(38, 46, 48));
    title_sizer->Add(m_fname_s, 0, wxEXPAND);

    m_main_sizer->Add(title_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    m_main_sizer->AddSpacer(FromDIP(12));

    // Radio buttons for main action
    // Index 0 = "Open as project" (m_action = 1 = LoadType::OpenProject)
    // Index 1 = "Import geometry only" (m_action = 2 = LoadType::LoadGeometry)
    auto* radio_group = new RadioGroup(this, {
        _L("Open as project"),
        _L("Import geometry only")
    }, wxVERTICAL);
    radio_group->SetSelection(get_action() - 1);
    radio_group->Bind(wxEVT_COMMAND_RADIOBOX_SELECTED, [this, radio_group](wxCommandEvent& e) {
        int selection = radio_group->GetSelection();
        set_action(selection + 1);
        on_action_changed(selection);
    });
    m_main_sizer->Add(radio_group, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    // Create conditional project settings sections
    create_printer_settings_section(m_main_sizer);
    create_filament_settings_section(m_main_sizer);

    m_main_sizer->AddSpacer(FromDIP(12));

    // Dialog buttons
    auto* dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &Import3mfDialog::on_select_ok, this);
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, &Import3mfDialog::on_select_cancel, this);
    m_main_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(m_main_sizer);

    // Set initial size and state
    update_dialog_size();
    on_action_changed(get_action() - 1);

    // Process filename for display
    wxString file_name(filename);
    int max_width = FromDIP(WIDTH_SMALL - 60);
    wxString first_line, second_line;
    int current_width = 0;
    
    for (size_t i = 0; i < file_name.length(); ++i) {
        int char_width = m_fname_f->GetTextExtent(file_name[i]).GetWidth();
        if (current_width + char_width > max_width && second_line.empty()) {
            second_line = file_name.Mid(i);
            break;
        }
        first_line += file_name[i];
        current_width += char_width;
    }
    
    m_fname_f->SetLabel(first_line);
    m_fname_s->SetLabel(second_line);

    // Apply dark mode styling
    wxGetApp().UpdateDlgDarkUI(this);

    Centre(wxBOTH);
}

wxColour Import3mfDialog::hex_to_colour(const std::string& hex)
{
    if (hex.empty()) return wxColour(128, 128, 128);  // Default gray
    
    std::string h = hex;
    if (h[0] == '#') h = h.substr(1);
    
    // Validate hex string length
    if (h.length() < 6) {
        BOOST_LOG_TRIVIAL(warning) << "Invalid hex color string: " << hex;
        return wxColour(128, 128, 128);
    }
    
    unsigned long val = 0;
    if (sscanf(h.c_str(), "%lx", &val) == 1) {
        return wxColour((val >> 16) & 0xFF, (val >> 8) & 0xFF, val & 0xFF);
    }
    return wxColour(128, 128, 128);
}

void Import3mfDialog::collect_available_filaments()
{
    m_available_filaments.clear();
    
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return;
    
    // Collect filaments exactly like PlaterPresetComboBox::update() does:
    // Only include presets that are:
    // 1. Visible (user has configured/installed them)
    // 2. Compatible with current printer
    for (const Preset& preset : preset_bundle->filaments) {
        if (preset.is_default)
            continue;
        
        // Skip invisible presets
        if (!preset.is_visible)
            continue;
        
        // Skip incompatible presets (like sidebar does)
        if (!preset.is_compatible)
            continue;
            
        m_available_filaments.push_back(preset.name);
    }
    
    // Ensure at least one filament is available
    if (m_available_filaments.empty()) {
        // Get whatever is currently selected
        const Preset& current = preset_bundle->filaments.get_selected_preset();
        if (!current.is_default) {
            m_available_filaments.push_back(current.name);
        }
    }
}

void Import3mfDialog::create_printer_settings_section(wxBoxSizer* parent_sizer)
{
    // Container panel (for show/hide)
    m_printer_settings_panel = new wxPanel(this);
    m_printer_settings_panel->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxVERTICAL);

    // Checkbox row with styled CheckBox
    wxBoxSizer* cb_sizer = new wxBoxSizer(wxHORIZONTAL);
    cb_sizer->AddSpacer(FromDIP(20));  // Indent

    m_cb_printer_settings = new CheckBox(m_printer_settings_panel);
    m_cb_printer_settings->SetValue(m_import_printer_settings);
    cb_sizer->Add(m_cb_printer_settings, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_cb_printer_label = new wxStaticText(m_printer_settings_panel, wxID_ANY, _L("Import project printer settings"));
    m_cb_printer_label->SetFont(::Label::Body_13);
    cb_sizer->Add(m_cb_printer_label, 0, wxALIGN_CENTER_VERTICAL);

    panel_sizer->Add(cb_sizer, 0, wxEXPAND | wxTOP, FromDIP(10));

    // Printer reassign panel (shown when checkbox is unchecked)
    m_printer_reassign_panel = new wxPanel(m_printer_settings_panel);
    m_printer_reassign_panel->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* reassign_sizer = new wxBoxSizer(wxHORIZONTAL);
    reassign_sizer->AddSpacer(FromDIP(48));  // Further indent

    m_printer_reassign_label = new wxStaticText(m_printer_reassign_panel, wxID_ANY, _L("Use printer:"));
    m_printer_reassign_label->SetFont(::Label::Body_13);
    m_printer_reassign_label->SetForegroundColour(wxGetApp().dark_mode() ? wxColour(180, 180, 180) : wxColour(107, 107, 107));
    reassign_sizer->Add(m_printer_reassign_label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_printer_dropdown = new ComboBox(m_printer_reassign_panel, wxID_ANY, wxEmptyString,
                                      wxDefaultPosition, wxSize(FromDIP(200), -1), 0, NULL, wxCB_READONLY);
    populate_printer_dropdown();
    reassign_sizer->Add(m_printer_dropdown, 1, wxALIGN_CENTER_VERTICAL);

    m_printer_reassign_panel->SetSizer(reassign_sizer);
    panel_sizer->Add(m_printer_reassign_panel, 0, wxEXPAND | wxTOP, FromDIP(6));

    // Warning label (shown if project printer not found)
    m_printer_warning_label = new wxStaticText(m_printer_settings_panel, wxID_ANY, wxEmptyString);
    m_printer_warning_label->SetFont(::Label::Body_12);
    m_printer_warning_label->SetForegroundColour(wxColour(255, 150, 0));  // Orange warning
    m_printer_warning_label->Hide();
    panel_sizer->Add(m_printer_warning_label, 0, wxLEFT | wxTOP, FromDIP(48));

    m_printer_settings_panel->SetSizer(panel_sizer);
    parent_sizer->Add(m_printer_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    // Bind checkbox events
    m_cb_printer_settings->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        m_import_printer_settings = m_cb_printer_settings->GetValue();
        update_conditional_sections();
        update_dialog_size();
        e.Skip();
    });
    
    // Also bind click on label
    m_cb_printer_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        m_cb_printer_settings->SetValue(!m_cb_printer_settings->GetValue());
        m_import_printer_settings = m_cb_printer_settings->GetValue();
        update_conditional_sections();
        update_dialog_size();
    });
}

void Import3mfDialog::populate_printer_dropdown()
{
    if (!m_printer_dropdown) return;
    
    m_printer_dropdown->Clear();
    
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    if (!preset_bundle) return;

    // Group printers similar to filament dropdown style
    struct PrinterInfo {
        std::string name;       // Internal preset name
        std::string label;      // Display label
        std::string vendor;     // Vendor for grouping
    };
    
    std::vector<PrinterInfo> user_printers;
    std::vector<PrinterInfo> system_printers;
    
    const Preset& current = preset_bundle->printers.get_selected_preset();
    std::string selected_name = current.name;
    
    for (const Preset& preset : preset_bundle->printers) {
        if (!preset.is_visible || preset.is_default)
            continue;
        
        PrinterInfo info;
        info.name = preset.name;
        info.label = preset.label(false);  // Use alias if available
        
        // Get vendor from config
        const ConfigOptionString* vendor_opt = preset.config.option<ConfigOptionString>("printer_vendor");
        if (vendor_opt && !vendor_opt->value.empty()) {
            info.vendor = vendor_opt->value;
            // Normalize "Bambu Lab" to "Bambu" like sidebar does
            if (info.vendor == "Bambu Lab") info.vendor = "Bambu";
        } else {
            info.vendor = "Other";
        }
        
        if (!preset.is_system) {
            user_printers.push_back(info);
        } else {
            system_printers.push_back(info);
        }
    }
    
    // Sort user presets alphabetically by label
    std::sort(user_printers.begin(), user_printers.end(), 
              [](const auto& a, const auto& b) { return a.label < b.label; });
    
    // Sort system presets by vendor priority, then alphabetically
    std::vector<std::string> priority_vendors = {"Bambu", "Prusa"};
    std::sort(system_printers.begin(), system_printers.end(), 
              [&priority_vendors](const auto& a, const auto& b) {
        // Compare vendor priority
        auto vendor_idx_a = std::find(priority_vendors.begin(), priority_vendors.end(), a.vendor);
        auto vendor_idx_b = std::find(priority_vendors.begin(), priority_vendors.end(), b.vendor);
        if (vendor_idx_a != vendor_idx_b)
            return vendor_idx_a < vendor_idx_b;
        
        // Alphabetical by label
        return a.label < b.label;
    });
    
    int selected_idx = -1;
    int current_idx = 0;
    
    // Add user presets with separator header (like filament dropdown)
    if (!user_printers.empty()) {
        m_printer_dropdown->Append(_L("User presets"), wxNullBitmap, wxEmptyString, nullptr, DD_ITEM_STYLE_SPLIT_ITEM);
        
        for (const auto& info : user_printers) {
            m_printer_dropdown->Append(from_u8(info.label), wxNullBitmap);
            if (info.name == selected_name) selected_idx = current_idx;
            current_idx++;
        }
    }
    
    // Add system presets with separator header and vendor grouping
    if (!system_printers.empty()) {
        m_printer_dropdown->Append(_L("System presets"), wxNullBitmap, wxEmptyString, nullptr, DD_ITEM_STYLE_SPLIT_ITEM);
        
        for (const auto& info : system_printers) {
            m_printer_dropdown->Append(from_u8(info.label), wxNullBitmap, from_u8(info.vendor));
            if (info.name == selected_name) selected_idx = current_idx;
            current_idx++;
        }
    }
    
    // Select current printer or first item
    if (selected_idx >= 0) {
        m_printer_dropdown->SetSelection(selected_idx);
    } else if (m_printer_dropdown->GetCount() > 0) {
        m_printer_dropdown->SetSelection(0);
    }
}

void Import3mfDialog::create_filament_settings_section(wxBoxSizer* parent_sizer)
{
    // Container panel (for show/hide)
    m_filament_settings_panel = new wxPanel(this);
    m_filament_settings_panel->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* panel_sizer = new wxBoxSizer(wxVERTICAL);

    // Checkbox row with styled CheckBox
    wxBoxSizer* cb_sizer = new wxBoxSizer(wxHORIZONTAL);
    cb_sizer->AddSpacer(FromDIP(20));  // Indent

    m_cb_filament_settings = new CheckBox(m_filament_settings_panel);
    m_cb_filament_settings->SetValue(m_import_filament_settings);
    cb_sizer->Add(m_cb_filament_settings, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));

    m_cb_filament_label = new wxStaticText(m_filament_settings_panel, wxID_ANY, _L("Import project filament settings"));
    m_cb_filament_label->SetFont(::Label::Body_13);
    cb_sizer->Add(m_cb_filament_label, 0, wxALIGN_CENTER_VERTICAL);

    panel_sizer->Add(cb_sizer, 0, wxEXPAND | wxTOP, FromDIP(8));

    // Filament mapping panel (shown when checkbox is unchecked)
    m_filament_mapping_panel = new wxPanel(m_filament_settings_panel);
    m_filament_mapping_panel->SetBackgroundColour(GetBackgroundColour());
    wxBoxSizer* mapping_outer_sizer = new wxBoxSizer(wxVERTICAL);
    
    // Info label
    auto* info_label = new wxStaticText(m_filament_mapping_panel, wxID_ANY, 
        _L("Map project colors to your filaments:"));
    info_label->SetFont(::Label::Body_12);
    info_label->SetForegroundColour(wxGetApp().dark_mode() ? wxColour(150, 150, 150) : wxColour(120, 120, 120));
    mapping_outer_sizer->Add(info_label, 0, wxLEFT | wxBOTTOM, FromDIP(28));

    // Scrolled window for filament rows (in case of many filaments)
    m_filament_scroll = new wxScrolledWindow(m_filament_mapping_panel, wxID_ANY, 
                                             wxDefaultPosition, wxSize(-1, FromDIP(140)));
    m_filament_scroll->SetScrollRate(0, FromDIP(20));
    m_filament_scroll->SetBackgroundColour(GetBackgroundColour());
    
    // Use a 2-column FlexGridSizer for the filament mapping grid
    wxFlexGridSizer* grid_sizer = new wxFlexGridSizer(2, FromDIP(16), FromDIP(12));  // 2 cols, hgap, vgap
    grid_sizer->AddGrowableCol(0, 1);
    grid_sizer->AddGrowableCol(1, 1);
    m_filament_scroll->SetSizer(grid_sizer);
    
    mapping_outer_sizer->Add(m_filament_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(28));

    m_filament_mapping_panel->SetSizer(mapping_outer_sizer);
    panel_sizer->Add(m_filament_mapping_panel, 0, wxEXPAND | wxTOP, FromDIP(6));

    m_filament_settings_panel->SetSizer(panel_sizer);
    parent_sizer->Add(m_filament_settings_panel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(20));

    // Bind checkbox events
    m_cb_filament_settings->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent& e) {
        m_import_filament_settings = m_cb_filament_settings->GetValue();
        update_conditional_sections();
        update_dialog_size();
        e.Skip();
    });
    
    // Also bind click on label
    m_cb_filament_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        m_cb_filament_settings->SetValue(!m_cb_filament_settings->GetValue());
        m_import_filament_settings = m_cb_filament_settings->GetValue();
        update_conditional_sections();
        update_dialog_size();
    });
}

void Import3mfDialog::populate_filament_mapping()
{
    if (!m_filament_scroll) return;
    
    // Clear existing rows
    for (auto* row : m_filament_rows) {
        row->Destroy();
    }
    m_filament_rows.clear();
    
    wxFlexGridSizer* grid_sizer = dynamic_cast<wxFlexGridSizer*>(m_filament_scroll->GetSizer());
    if (!grid_sizer) return;
    
    grid_sizer->Clear(false);  // Don't delete windows, we already destroyed them
    
    // Create a row for each project filament in 2-column layout
    for (size_t i = 0; i < m_project_filament_colors.size(); ++i) {
        wxColour color = hex_to_colour(m_project_filament_colors[i]);
        
        auto* row = new FilamentMappingRow(m_filament_scroll, (int)(i + 1), color, m_available_filaments);
        m_filament_rows.push_back(row);
        grid_sizer->Add(row, 1, wxEXPAND);
    }
    
    // If odd number of filaments, add an empty spacer to keep grid balanced
    if (m_project_filament_colors.size() % 2 == 1) {
        grid_sizer->AddSpacer(0);
    }
    
    m_filament_scroll->FitInside();
    m_filament_scroll->Layout();
}

void Import3mfDialog::set_project_info(const Slic3r::Bbs3mfProjectInfo& info)
{
    m_has_printer_settings = info.has_printer_settings;
    m_has_filament_settings = info.has_filament_settings;
    m_project_filament_colors = info.filament_colors;
    
    // Update printer warning if project printer not found
    if (m_printer_warning_label && !info.printer_preset_name.empty()) {
        PresetBundle* preset_bundle = wxGetApp().preset_bundle;
        if (preset_bundle) {
            bool found = false;
            for (const Preset& preset : preset_bundle->printers) {
                if (preset.name == info.printer_preset_name) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                m_printer_warning_label->SetLabel(
                    wxString::Format(_L("Project printer '%s' not found"), from_u8(info.printer_preset_name)));
                m_printer_warning_label->Show();
            }
        }
    }
    
    // Populate filament mapping rows
    populate_filament_mapping();
    
    // Update visibility
    update_conditional_sections();
    update_dialog_size();
}

void Import3mfDialog::on_action_changed(int selection)
{
    update_conditional_sections();
    update_dialog_size();
}

void Import3mfDialog::on_printer_checkbox_changed()
{
    if (m_cb_printer_settings)
        m_import_printer_settings = m_cb_printer_settings->GetValue();
    update_conditional_sections();
    update_dialog_size();
}

void Import3mfDialog::on_filament_checkbox_changed()
{
    if (m_cb_filament_settings)
        m_import_filament_settings = m_cb_filament_settings->GetValue();
    update_conditional_sections();
    update_dialog_size();
}

void Import3mfDialog::update_conditional_sections()
{
    bool is_open_project = (get_action() == 1);

    // Show/hide main panels based on radio selection
    if (m_printer_settings_panel)
        m_printer_settings_panel->Show(is_open_project);
    if (m_filament_settings_panel)
        m_filament_settings_panel->Show(is_open_project);

    // Show/hide printer reassign panel based on checkbox
    if (m_printer_reassign_panel)
        m_printer_reassign_panel->Show(!m_import_printer_settings);
    if (m_printer_warning_label && m_import_printer_settings)
        m_printer_warning_label->Hide();

    // Show/hide filament mapping panel based on checkbox
    if (m_filament_mapping_panel)
        m_filament_mapping_panel->Show(!m_import_filament_settings);

    Layout();
}

int Import3mfDialog::get_required_width() const
{
    bool is_open_project = (get_action() == 1);
    
    if (!is_open_project) {
        return WIDTH_SMALL;
    }
    
    // If either checkbox is unchecked, we need more space
    if (!m_import_printer_settings || !m_import_filament_settings) {
        return WIDTH_LARGE;
    }
    
    return WIDTH_SMALL;
}

void Import3mfDialog::update_dialog_size()
{
    int width = FromDIP(get_required_width());
    
    SetMinSize(wxSize(width, -1));
    SetMaxSize(wxSize(width, -1));
    
    Layout();
    Fit();
    
    // Keep width fixed after fit
    wxSize size = GetSize();
    size.SetWidth(width);
    SetSize(size);
}

void Import3mfDialog::on_select_ok(wxCommandEvent& event)
{
    EndModal(wxID_OK);
}

void Import3mfDialog::on_select_cancel(wxCommandEvent& event)
{
    EndModal(wxID_CANCEL);
}

void Import3mfDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    if (m_cb_printer_settings) m_cb_printer_settings->Rescale();
    if (m_cb_filament_settings) m_cb_filament_settings->Rescale();
    
    update_dialog_size();
    Refresh();
}

Import3mfSettings Import3mfDialog::get_import_settings() const
{
    Import3mfSettings settings;
    settings.import_printer_settings = m_import_printer_settings;
    settings.import_filament_settings = m_import_filament_settings;

    // Get reassign printer name if not importing printer settings
    if (!m_import_printer_settings && m_printer_dropdown) {
        int sel = m_printer_dropdown->GetSelection();
        if (sel != wxNOT_FOUND && sel >= 0) {
            settings.reassign_printer = m_printer_dropdown->GetStringSelection().ToStdString();
        }
    }

    // Collect filament remapping from rows
    if (!m_import_filament_settings) {
        for (const auto* row : m_filament_rows) {
            int filament_idx = row->get_filament_index();
            std::string preset_name = row->get_selected_filament();
            if (!preset_name.empty()) {
                settings.filament_color_remapping[filament_idx] = preset_name;
            }
        }
    }

    return settings;
}

//------------------------------------------------------------------------------
// Global state for passing import settings from dialog to loader
// Note: This pattern is used because determine_3mf_load_type() is called
// before load_files() and we need to pass the user's choices to the loader.
// The settings are cleared after use in Plater::open_3mf_file().
//------------------------------------------------------------------------------

static Import3mfSettings g_pending_3mf_import_settings;
static bool g_has_pending_3mf_import_settings = false;

LoadType determine_3mf_load_type(std::string filename, std::string override_setting)
{
    std::string setting;

    if (!override_setting.empty()) {
        setting = override_setting;
    } else {
        setting = wxGetApp().app_config->get(SETTING_PROJECT_LOAD_BEHAVIOUR);
    }

    // Pre-parse the 3MF to get project info
    Slic3r::Bbs3mfProjectInfo project_info;
    bool pre_parsed = Slic3r::bbs_3mf_preparse_project_info(filename.c_str(), project_info);

    if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY) {
        // Even for geometry-only, we need filament count for expansion
        g_pending_3mf_import_settings = Import3mfSettings();
        g_pending_3mf_import_settings.project_filament_count = project_info.filament_count;
        g_pending_3mf_import_settings.project_filament_colors = project_info.filament_colors;
        g_pending_3mf_import_settings.import_printer_settings = false;
        g_pending_3mf_import_settings.import_filament_settings = false;
        g_has_pending_3mf_import_settings = pre_parsed;
        return LoadType::LoadGeometry;
        
    } else if (setting == OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK) {
        Import3mfDialog dlg(filename);
        if (pre_parsed) {
            dlg.set_project_info(project_info);
        }
        
        if (dlg.ShowModal() == wxID_OK) {
            int choice = dlg.get_action();
            LoadType load_type = static_cast<LoadType>(choice);
            wxGetApp().app_config->set("import_project_action", std::to_string(choice));

            // Store user's import settings
            g_pending_3mf_import_settings = dlg.get_import_settings();
            
            // Copy pre-parsed project info
            g_pending_3mf_import_settings.project_filament_count = project_info.filament_count;
            g_pending_3mf_import_settings.project_filament_colors = project_info.filament_colors;
            g_pending_3mf_import_settings.project_printer_name = project_info.printer_preset_name;
            g_pending_3mf_import_settings.project_filament_preset_names = project_info.filament_preset_names;
            g_pending_3mf_import_settings.project_has_printer_settings = project_info.has_printer_settings;
            g_pending_3mf_import_settings.project_has_filament_settings = project_info.has_filament_settings;
            
            g_has_pending_3mf_import_settings = true;

            wxGetApp().mainframe->select_tab(MainFrame::tp3DEditor);
            return load_type;
        }

        g_has_pending_3mf_import_settings = false;
        return LoadType::Unknown;  // Cancelled
        
    } else {
        // Default: Open as Project (Load All behavior)
        // Check if user has enabled skip options in preferences
        bool skip_printer_pref = wxGetApp().app_config->get(SETTING_PROJECT_SKIP_PRINTER) == "true";
        bool skip_filament_pref = wxGetApp().app_config->get(SETTING_PROJECT_SKIP_FILAMENT) == "true";
        
        g_pending_3mf_import_settings = Import3mfSettings();
        g_pending_3mf_import_settings.project_filament_count = project_info.filament_count;
        g_pending_3mf_import_settings.project_filament_colors = project_info.filament_colors;
        g_pending_3mf_import_settings.project_printer_name = project_info.printer_preset_name;
        g_pending_3mf_import_settings.project_filament_preset_names = project_info.filament_preset_names;
        g_pending_3mf_import_settings.project_has_printer_settings = project_info.has_printer_settings;
        g_pending_3mf_import_settings.project_has_filament_settings = project_info.has_filament_settings;
        
        // Apply skip preferences
        g_pending_3mf_import_settings.import_printer_settings = !skip_printer_pref;
        g_pending_3mf_import_settings.import_filament_settings = !skip_filament_pref;
        
        g_has_pending_3mf_import_settings = pre_parsed;
        return LoadType::OpenProject;
    }
}

bool has_pending_3mf_import_settings()
{
    return g_has_pending_3mf_import_settings;
}

Import3mfSettings get_pending_3mf_import_settings()
{
    return g_pending_3mf_import_settings;
}

void clear_pending_3mf_import_settings()
{
    g_has_pending_3mf_import_settings = false;
    g_pending_3mf_import_settings = Import3mfSettings();
}

const std::map<int, int>* get_pending_3mf_filament_remap()
{
    // Note: filament_color_remapping is now std::map<int, std::string> for preset name assignment
    // Geometry extruder remapping is not needed - we keep original extruder IDs and just assign
    // different presets to slots in Plater.cpp after loading
    return nullptr;
}

} // namespace GUI
} // namespace Slic3r
