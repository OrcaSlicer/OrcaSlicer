#include "AMSDryControl.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "GUI_App.hpp"
#include "I18N.hpp"

#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"

#include "slic3r/GUI/DeviceCore/DevManager.h"

#include "slic3r/GUI/MsgDialog.hpp"

#include "slic3r/GUI/Widgets/AnimaController.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/ProgressBar.hpp"
#include "slic3r/GUI/DeviceCore/DevUtilBackend.h"

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

const int AMS_DRY_STATUS_IMAGE_SIZE = 96;

static std::string get_humidity_level_img_path(int humidity_percent)
{
    const int num_levels = 5;
    int hum_level;

    if (humidity_percent <= 100 / num_levels) {
        hum_level = 5;
    } else if (humidity_percent <= 100 / num_levels * 2) {
        hum_level = 4;
    } else if (humidity_percent <= 100 / num_levels * 3) {
        hum_level = 3;
    } else if (humidity_percent <= 100 / num_levels * 4) {
        hum_level = 2;
    } else {
        hum_level = 1;
    }

    if (wxGetApp().dark_mode()) {
        return "hum_level" + std::to_string(hum_level) + "_no_num_dark";
    } else {
        return "hum_level" + std::to_string(hum_level) + "_no_num_light";
    }

}

static std::string get_dry_status_img_path(DevAmsType type, DevAms::DryStatus status, DevAms::DrySubStatus sub_status)
{
    std::string img_name = "dev_ams_dry_ctr_";
    switch (type) {
        case DevAmsType::N3S:
            img_name += "n3s";
            break;
        case DevAmsType::N3F:
            img_name += "n3f";
            break;
        default:
            return "";
    }

    img_name += "_";
    switch (status) {
        case DevAms::DryStatus::Error:
            img_name += "error";
            return img_name;
    }

    switch (sub_status) {
        case DevAms::DrySubStatus::Heating:
            img_name += "heating";
            return img_name;
        case DevAms::DrySubStatus::Dehumidify:
            img_name += "dehumidifying";
            return img_name;
    }

    return "";
}

FilamentItemPanel::FilamentItemPanel(wxWindow* parent, const wxString& text, const std::string& icon_name, wxWindowID id)
    : wxPanel(parent, id)
    , m_icon_name(icon_name)
{
    SetBackgroundColour(wxColour("#F0F0F1")); // Light gray background
    SetMinSize(wxSize(FromDIP(64), FromDIP(106))); // Width: 64, Height: 106
    SetSize(wxSize(FromDIP(64), FromDIP(106)));    // Fixed size

    // Create sizer for vertical layout
    wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);

    // Top section with text - moved to be closer to center
    wxBoxSizer* top_sizer = new wxBoxSizer(wxVERTICAL);
    top_sizer->AddStretchSpacer(5);

    m_text_label = new Label(this, text);
    m_text_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour(*wxBLACK)));
    m_text_label->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F1")));
    m_text_label->SetFont(Label::Body_12);
    m_text_label->Wrap(FromDIP(40));
    top_sizer->Add(m_text_label, 0, wxALIGN_CENTER_HORIZONTAL);

    top_sizer->AddStretchSpacer(1); // Increased bottom spacer to push text down
    sizer->Add(top_sizer, 1, wxEXPAND);

    // Bottom section with icon - vertically centered in bottom half
    wxBoxSizer* bottom_sizer = new wxBoxSizer(wxVERTICAL);
    bottom_sizer->AddStretchSpacer(1);

    m_icon_bitmap = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
    m_icon_bitmap->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F0F0F1")));
    m_icon_bitmap->SetMinSize(wxSize(FromDIP(24), FromDIP(24)));
    m_icon_bitmap->SetMaxSize(wxSize(FromDIP(24), FromDIP(24)));
    bottom_sizer->Add(m_icon_bitmap, 0, wxALIGN_CENTER_HORIZONTAL);

    bottom_sizer->AddStretchSpacer(1);
    sizer->Add(bottom_sizer, 1, wxEXPAND);

    SetSizer(sizer);

    // Bind events
    Bind(wxEVT_PAINT, &FilamentItemPanel::OnPaint, this);
    Bind(wxEVT_SIZE, &FilamentItemPanel::OnSize, this);

    SetIcon(icon_name);
    wxGetApp().UpdateDarkUI(this);
}

void FilamentItemPanel::SetText(const wxString& text)
{
    m_text_label->SetLabel(text);
    Layout();
}

void FilamentItemPanel::SetIcon(const std::string& icon_name)
{
    m_icon_name = icon_name;
    if (!icon_name.empty()) {
        try {
            m_icon = ScalableBitmap(this, icon_name, 20);
            m_icon.msw_rescale();
            m_icon_bitmap->SetBitmap(m_icon.bmp());
        } catch (Exception&) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": failed to load filament icon";
            m_icon_bitmap->SetBitmap(wxNullBitmap);
        }
    } else {
        m_icon_bitmap->SetBitmap(wxNullBitmap);
    }
    m_icon_bitmap->Refresh();
}

void FilamentItemPanel::msw_rescale()
{
    if (m_icon_bitmap && !m_icon_name.empty()) {
        m_icon.msw_rescale();
        m_icon_bitmap->SetBitmap(m_icon.bmp());
        m_icon_bitmap->Refresh();
    }
    Layout();
}

void FilamentItemPanel::OnPaint(wxPaintEvent& event)
{
    wxPaintDC dc(this);
    wxSize size = GetSize();

    // bool is_dark_mode = wxGetApp().dark_mode();
    wxColour backgroundColor = StateColor::darkModeColorFor(wxColour("#F0F0F1"));
    wxColour borderColor = StateColor::darkModeColorFor(wxColour("#DBDBDB"));

    // Draw white background rectangle with rounded corners inside the thick vertical lines
    dc.SetBrush(wxBrush(backgroundColor));
    dc.SetPen(wxPen(backgroundColor));
    dc.DrawRectangle(FromDIP(6), FromDIP(12), size.GetWidth() - FromDIP(12), size.GetHeight() - 2 * FromDIP(12));

    // Draw much thicker vertical rounded rectangles on left and right (3 times thicker)
    dc.SetBrush(wxBrush(wxColour(borderColor)));
    dc.SetPen(wxPen(wxColour(borderColor)));
    dc.DrawRoundedRectangle(FromDIP(0), FromDIP(0), FromDIP(6), size.GetHeight(), FromDIP(6)); // Left rounded rectangle
    dc.DrawRoundedRectangle(size.GetWidth() - FromDIP(6), FromDIP(0), FromDIP(6), size.GetHeight(), FromDIP(6)); // Right rounded rectangle

    // Draw thin horizontal rounded rectangles on top and bottom (moved closer to center by half the distance)
    dc.SetBrush(wxBrush(wxColour(borderColor)));
    dc.SetPen(wxPen(wxColour(borderColor)));
    dc.DrawRoundedRectangle(FromDIP(6), FromDIP(12), size.GetWidth() - FromDIP(12), FromDIP(3), FromDIP(1)); // Top rounded rectangle
    dc.DrawRoundedRectangle(FromDIP(6), size.GetHeight() - FromDIP(13), size.GetWidth() - FromDIP(12), FromDIP(3), FromDIP(1)); // Bottom rounded rectangle
}

void FilamentItemPanel::OnSize(wxSizeEvent& event)
{
    Refresh();
    event.Skip();
}

// class AMSFilamentPanel

AMSFilamentPanel::AMSFilamentPanel(wxWindow* parent, const wxString& ams_name, wxWindowID id)
    : wxPanel(parent, id)
{
    SetBackgroundColour(wxColour("#DBDBDB"));

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Filament items section
    m_filament_container = new wxPanel(this);
    m_filament_container->SetBackgroundColour(wxColour("#F0F0F1"));
    m_filament_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_filament_container->SetSizer(m_filament_sizer);

    // AMS name section
    m_ams_name_label = new Label(this, ams_name);
    m_ams_name_label->SetForegroundColour(wxColour("#858585"));
    m_ams_name_label->SetFont(Label::Body_14);
    m_ams_name_label->SetBackgroundColour(wxColour("#DBDBDB"));

    main_sizer->Add(m_filament_container, 1, wxEXPAND | wxALL, 0);
    main_sizer->Add(m_ams_name_label, 0, wxALIGN_LEFT | wxALL, FromDIP(5));

    SetSizer(main_sizer);
    wxGetApp().UpdateDarkUI(this);
}

void AMSFilamentPanel::AddFilamentItem(FilamentItemPanel* panel)
{
    if (panel) {
        panel->Reparent(m_filament_container);
        m_filament_sizer->Add(panel, 0, wxALL, FromDIP(5));
        m_filament_items.push_back(panel);
        Layout();
    }
}

void AMSFilamentPanel::AddFilamentItem(const wxString& text, const std::string& icon_name)
{
    FilamentItemPanel* item = new FilamentItemPanel(m_filament_container, text, icon_name);
    m_filament_items.push_back(item);
    m_filament_sizer->Add(item, 0, wxALL, FromDIP(5));
    Layout();
}

void AMSFilamentPanel::SetAmsName(const wxString& ams_name)
{
    m_ams_name_label->SetLabel(ams_name);
    Layout();
}

void AMSFilamentPanel::Clear()
{
    m_filament_sizer->Clear(true);
    m_filament_items.clear();
}

void AMSFilamentPanel::msw_rescale()
{
    for (auto item : m_filament_items) {
        item->msw_rescale();
    }
    Layout();
}


AMSDryCtrWin::AMSDryCtrWin(wxWindow *parent)
    :DPIDialog(parent, wxID_ANY, _L("AMS Dryness Control"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    create();
}

AMSDryCtrWin::~AMSDryCtrWin()
{
    if (m_progress_timer) {
        delete m_progress_timer;
    }
}


wxScrolledWindow* AMSDryCtrWin::create_preview_scrolled_window(wxWindow* parent)
{
    wxScrolledWindow* panel = new wxScrolledWindow(parent, wxID_ANY);
    panel->SetScrollRate(10, 0);
    panel->SetSize(AMS_ITEMS_PANEL_SIZE);
    panel->SetMinSize(AMS_ITEMS_PANEL_SIZE);
    panel->SetBackgroundColour(AMS_CONTROL_DEF_BLOCK_BK_COLOUR);

    return panel;
}

wxBoxSizer* AMSDryCtrWin::create_humidity_status_section(wxPanel* parent)
{
    wxBoxSizer* image_sizer = new wxBoxSizer(wxVERTICAL);

    m_humidity_img = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap);
    image_sizer->Add(m_humidity_img, 1, wxALIGN_CENTER_HORIZONTAL, 0);

    m_humidity_img->SetBitmap(wxNullBitmap);

    // Create a horizontal sizer for description and icon
    wxBoxSizer* desc_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_image_description_icon = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap);
    m_image_description_icon->SetBackgroundColour(*wxWHITE);
    m_image_description_icon->SetMinSize(wxSize(FromDIP(20), FromDIP(20)));
    m_image_description_icon->SetMaxSize(wxSize(FromDIP(20), FromDIP(20)));
    m_image_description_icon->Show(false);
    desc_sizer->Add(m_image_description_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(5));

    m_image_description = new Label(parent, _L("Idle"));
    m_image_description->SetFont(Label::Head_14);
    m_image_description->SetForegroundColour(*wxBLACK);
    m_image_description->SetBackgroundColour(*wxWHITE);
    desc_sizer->Add(m_image_description, 0, wxALIGN_CENTER_VERTICAL);

    image_sizer->Add(desc_sizer, 0, wxALIGN_CENTER | wxALL, FromDIP(5));

    return image_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_description_item(wxPanel* parent, const wxString& title, Label*& dataLabel)
{
    wxBoxSizer* item_sizer = new wxBoxSizer(wxVERTICAL);

    Label* titleLabel = new Label(parent, title);
    titleLabel->SetForegroundColour(*wxBLACK);
    titleLabel->SetFont(Label::Body_16);

    dataLabel = new Label(parent, wxT("--"));
    dataLabel->SetForegroundColour(*wxBLACK);
    dataLabel->SetFont(Label::Body_14);

    item_sizer->AddStretchSpacer();
    item_sizer->Add(titleLabel, 0, wxALIGN_CENTER | wxALL, FromDIP(2));
    item_sizer->Add(dataLabel, 0, wxALIGN_CENTER | wxALL, FromDIP(2));
    item_sizer->AddStretchSpacer();

    return item_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_status_descriptions_section(wxPanel* parent)
{
    wxBoxSizer* desc_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer* hum_desc_sizer = create_description_item(parent, _L("Humidity"), m_humidity_data_label);
    desc_sizer->Add(hum_desc_sizer, 1, wxEXPAND, FromDIP(1));

    wxStaticLine* vert_separator = new wxStaticLine(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL);
    desc_sizer->Add(vert_separator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(5));

    wxBoxSizer* temp_desc_sizer = create_description_item(parent, _L("Temperature"), m_temperature_data_label);
    desc_sizer->Add(temp_desc_sizer, 1, wxEXPAND, FromDIP(1));

    m_time_descrition_container = new wxBoxSizer(wxHORIZONTAL);
    wxStaticLine* vert_separator2 = new wxStaticLine(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLI_VERTICAL);
    m_time_descrition_container->Add(vert_separator2, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(5));

    wxBoxSizer* time_desc_sizer = create_description_item(parent, _L("Left Time"), m_time_data_label);
    m_time_descrition_container->Add(time_desc_sizer, 1, wxEXPAND, FromDIP(1));

    desc_sizer->Add(m_time_descrition_container, 1, wxEXPAND, 0);
    // m_time_descrition_container->Show(false);

    return desc_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_left_panel(wxPanel* parent)
{
    wxBoxSizer* left_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* image_section = create_humidity_status_section(parent);
    left_sizer->Add(image_section, 1, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer* descriptions_section = create_status_descriptions_section(parent);
    left_sizer->Add(descriptions_section, 0, wxEXPAND | wxALL, FromDIP(20));

    return left_sizer;
}

Button* AMSDryCtrWin::create_button(wxPanel* parent, const wxString& title,
    const wxColour& background_color, const wxColour& border_color, const wxColour& text_color)
{
    Button* button = new Button(parent, title);

    // Create state colors for background
    StateColor bg_color(
        std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Disabled),
        std::pair<wxColour, int>(background_color.ChangeLightness(80), StateColor::Pressed),
        std::pair<wxColour, int>(background_color.ChangeLightness(120), StateColor::Hovered),
        std::pair<wxColour, int>(background_color, StateColor::Normal)
    );

    // Create state colors for border
    StateColor bd_color(
        std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Disabled),
        std::pair<wxColour, int>(border_color, StateColor::Enabled)
    );

    button->SetBackgroundColor(bg_color);
    button->SetBorderColor(bd_color);
    button->SetTextColor(text_color);
    button->SetFont(Label::Body_14);

    // Auto-size button based on text content with padding
    wxSize best_size = button->GetBestSize();
    int padding_width = FromDIP(4);
    int padding_height = FromDIP(2);
    wxSize final_size(best_size.GetWidth() + padding_width, best_size.GetHeight() + padding_height);
    button->SetMinSize(final_size);

    return button;
}

wxBoxSizer* AMSDryCtrWin::create_normal_state_panel(wxPanel* parent)
{
    wxBoxSizer* normal_state_sizer = new wxBoxSizer(wxVERTICAL);

    Label* description_label = new Label(parent, _L("Filament Drying Settings"));
    description_label->SetForegroundColour(*wxBLACK);
    description_label->SetFont(Label::Head_14);
    normal_state_sizer->Add(description_label, 0, wxALL, FromDIP(5));

    // Part 2: ComboBox for material selection
    wxBoxSizer* combo_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_trays_combo = new ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(-1, FromDIP(26)), 0, nullptr, wxCB_READONLY);
    m_trays_combo->SetFont(Label::Body_14);

    combo_sizer->Add(m_trays_combo, 1, wxEXPAND);
    normal_state_sizer->Add(combo_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    m_trays_combo->Bind(wxEVT_COMBOBOX, &AMSDryCtrWin::OnFilamentSelectionChanged, this);

    // part 3 time and temperature
    // Temperature part
    wxBoxSizer* temp_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_temperature_input = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(80), -1));
    m_temperature_input->SetMaxLength(3); // Limit to 3 digits

    m_temperature_input->Bind(wxEVT_CHAR, [this](wxKeyEvent& event) {
        int keycode = event.GetKeyCode();
        if (keycode >= '0' && keycode <= '9') {
            event.Skip();
        } else if (keycode == WXK_BACK || keycode == WXK_DELETE || keycode == WXK_LEFT || keycode == WXK_RIGHT) {
            event.Skip();
        }
    });

    m_temperature_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        m_next_button->Disable();
        m_start_button->Disable();
    });

    m_temperature_input->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_temperature_input->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

    Label* temp_unit_label = new Label(parent, wxString::FromUTF8("℃"));
    temp_unit_label->SetForegroundColour(*wxBLACK);
    temp_sizer->Add(m_temperature_input, 1, wxRIGHT, FromDIP(1));
    temp_sizer->Add(temp_unit_label, 0, wxALIGN_CENTER_VERTICAL);

    // Time part
    wxBoxSizer* time_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_time_input = new wxTextCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(100), -1));
    m_time_input->SetMaxLength(3); // Limit to 3 digits

    m_time_input->Bind(wxEVT_CHAR, [this](wxKeyEvent& event) {
        int keycode = event.GetKeyCode();
        if (keycode >= '0' && keycode <= '9') {
            event.Skip();
        } else if (keycode == WXK_BACK || keycode == WXK_DELETE || keycode == WXK_LEFT || keycode == WXK_RIGHT) {
            event.Skip();
        }
    });

    m_time_input->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
        m_next_button->Disable();
        m_start_button->Disable();
    });

    m_time_input->SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_time_input->SetForegroundColour(StateColor::darkModeColorFor(*wxBLACK));

    Label* time_unit_label = new Label(parent, "H");
    time_unit_label->SetForegroundColour(*wxBLACK);
    time_sizer->Add(m_time_input, 1, wxRIGHT, FromDIP(1));
    time_sizer->Add(time_unit_label, 0, wxALIGN_CENTER_VERTICAL);

    wxBoxSizer* input_sizer = new wxBoxSizer(wxHORIZONTAL);
    input_sizer->Add(temp_sizer, 1, wxRIGHT, FromDIP(10));
    input_sizer->Add(time_sizer, 1, 0);
    normal_state_sizer->Add(input_sizer, 0, wxEXPAND | wxALL, FromDIP(5));

    // Part 4: Abnormal description/message area
    m_normal_description = new Label(parent, "");
    m_normal_description->SetForegroundColour(wxColour("#F09A17"));
    m_normal_description->SetFont(Label::Body_12);
    normal_state_sizer->Add(m_normal_description, 0, wxALL, FromDIP(5));

    // Part 5: Start button
    m_next_button = create_button(
        parent,
        _L("Start"),
        AMS_CONTROL_BRAND_COLOUR,  // Background color - green
        AMS_CONTROL_BRAND_COLOUR,  // Border color - green
        wxColour("#FFFFFE")        // Text color - white
    );

    m_next_button->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) {
        m_main_simplebook->SetSelection(1);
    });

    normal_state_sizer->Add(m_next_button, 0, wxALL, FromDIP(5));
    m_next_button->Disable();

    m_stop_button = create_button(
        parent,
        _L("Stop"),
        wxColour(255, 0, 0),       // Background color - red
        wxColour(255, 0, 0),       // Border color - red
        wxColour("#FFFFFE")        // Text color - white
    );

    m_stop_button->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) {
        auto fila_system = get_fila_system();
        if (!fila_system) {
            BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::start_sending_drying_command: Invalid FilaSystem Pointer";
            return;
        }

        fila_system->CtrlAmsStopDrying(std::stoi(m_ams_info.m_ams_id));
        // Temporarily show stopping state, then restore after 2 seconds
        if (m_stop_button) {
            m_stop_button->SetLabel(_L("Stopping"));
            update_button_size(m_stop_button);  // Adjust button size for longer text
            m_stop_button->Disable();
            m_stop_button->Layout();
            m_stop_button->GetParent()->Layout();  // Relayout parent container
            m_stop_button->Refresh();

            m_stop_button_restore_deadline.reset();
            m_stop_button_restore_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    });

    m_stop_button->Show(false);
    normal_state_sizer->Add(m_stop_button, 0, wxALL, FromDIP(5));

    return normal_state_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_cannot_dry_panel(wxPanel* parent)
{
    wxBoxSizer* abnormal_sizer = new wxBoxSizer(wxVERTICAL);

    Label* description_label = new Label(parent, _L("Unable to dry temporarily due to ..."));
    description_label->SetForegroundColour(*wxBLACK);
    description_label->SetFont(Label::Head_16);
    description_label->Wrap(FromDIP(300));
    abnormal_sizer->Add(description_label, 0, wxALIGN_CENTER_VERTICAL, FromDIP(10));

    // Add a description label for the abnormal state
    m_cannot_dry_description_label = new Label(parent, (""));
    m_cannot_dry_description_label->SetForegroundColour(*wxBLACK);
    m_cannot_dry_description_label->SetFont(Label::Body_14);
    m_cannot_dry_description_label->Wrap(FromDIP(250)); // Wrap text to fit within panel

    abnormal_sizer->Add(m_cannot_dry_description_label, 0, wxALIGN_CENTER_VERTICAL, FromDIP(10));

    // Unload button shown only when ConsumableAtAmsOutlet reason is active
    m_unload_button = create_button(
        parent,
        _L("Unload"),
        AMS_CONTROL_BRAND_COLOUR,       // Background color - light gray
        AMS_CONTROL_BRAND_COLOUR,       // Border color - gray
        *wxWHITE                   // Text color - white
    );

    m_unload_button->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) {
        auto fila_system = get_fila_system();
        if (!fila_system) {
            BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::unload_button: Invalid FilaSystem Pointer";
            return;
        }

        MachineObject* obj = fila_system->GetOwner();
        if (!obj) {
            BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::unload_button: Invalid MachineObject Pointer";
            return;
        }

        obj->command_ams_change_filament(false, m_ams_info.m_ams_id, "255");

        m_unload_button->Disable();
        m_unload_button_restore_deadline.reset();
        m_unload_button_restore_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    });

    m_unload_button->Show(false);
    abnormal_sizer->Add(m_unload_button, 0, wxALL, FromDIP(5));

    return abnormal_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_drying_error_panel(wxPanel* parent)
{
    wxBoxSizer* err_sizer = new wxBoxSizer(wxVERTICAL);
    Label* description_label = new Label(parent, _L("Drying Error"));
    description_label->SetForegroundColour(*wxRED);
    description_label->SetFont(Label::Body_14); // Wrap text to fit within panel
    err_sizer->Add(description_label, 0, wxALIGN_CENTER_VERTICAL, FromDIP(10));

    Label* additional_description_label = new Label(parent, _L("Please check the Assistant for troubleshooting"));
    additional_description_label->SetForegroundColour(*wxBLACK);
    additional_description_label->SetFont(Label::Body_14); // Wrap text to fit within panel
    err_sizer->Add(additional_description_label, 0, wxALIGN_CENTER_VERTICAL, FromDIP(10));

    return err_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_right_panel(wxPanel* parent)
{
    wxBoxSizer* right_sizer = new wxBoxSizer(wxHORIZONTAL);

    m_normal_state_sizer = create_normal_state_panel(parent);
    m_cannot_dry_sizer = create_cannot_dry_panel(parent);
    m_dry_error_sizer = create_drying_error_panel(parent);

    right_sizer->Add(m_normal_state_sizer, 1, wxEXPAND);
    right_sizer->Add(m_cannot_dry_sizer, 1, wxEXPAND);

    m_cannot_dry_sizer->Show(false);
    m_dry_error_sizer->Show(false);

    return right_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_main_content_section(wxPanel* parent)
{
    wxBoxSizer* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer* left_panel = create_left_panel(parent);
    content_sizer->Add(left_panel, 2, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    left_panel->SetMinSize(wxSize(FromDIP(250), -1));

    wxBoxSizer* right_panel = create_right_panel(parent);
    content_sizer->Add(right_panel, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    right_panel->SetMinSize(wxSize(FromDIP(250), -1));

    return content_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_main_page_sizer(wxPanel* parent)
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* content_section = create_main_content_section(parent);
    main_sizer->Add(content_section, 1, wxEXPAND | wxALL, FromDIP(5));

    return main_sizer;
}

wxBoxSizer* AMSDryCtrWin::create_guide_info_filament(wxPanel* parent)
{
    wxBoxSizer* filament_section = new wxBoxSizer(wxHORIZONTAL);

    m_ams_filament_panel = new AMSFilamentPanel(parent, "");

    filament_section->Add(m_ams_filament_panel, 0, wxEXPAND | wxALL | wxALIGN_CENTER_HORIZONTAL, FromDIP(5));

    return filament_section;
}

wxBoxSizer* AMSDryCtrWin::create_guide_info_section(wxPanel* parent)
{
    wxBoxSizer* info_section = new wxBoxSizer(wxVERTICAL);

    // Part 1: Title
    m_guide_title_label = new Label(parent, _L("Please remove and store the filament (as shown)."));
    m_guide_title_label->SetForegroundColour(*wxBLACK);
    m_guide_title_label->SetFont(Label::Head_18);
    info_section->Add(m_guide_title_label, 0, wxEXPAND | wxALL, FromDIP(5));

    // Part 2: Description
    m_guide_description_label = new Label(parent, _L("The AMS can rotate the filament which is properly stored, providing better drying results."));
    m_guide_description_label->SetForegroundColour(*wxBLACK);
    m_guide_description_label->SetFont(Label::Body_14);
    m_guide_description_label->Wrap(FromDIP(300)); // Wrap text to fit within panel
    info_section->Add(m_guide_description_label, 0, wxEXPAND | wxALL, FromDIP(5));

    // Part 3: filament panel
    wxBoxSizer* fila_section = create_guide_info_filament(parent);
    info_section->Add(fila_section, 0, wxEXPAND | wxALL, FromDIP(5));

    // Part 4: Circular toggle with description
    wxBoxSizer* toggle_section = new wxBoxSizer(wxHORIZONTAL);

    m_rotate_spool_toggle = new wxCheckBox(parent, wxID_ANY, "");
    m_rotate_spool_toggle->SetValue(false);

    m_rotate_spool_toggle->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent& event) {
        bool is_checked = event.IsChecked();
        // Add toggle behavior logic here
    });

    toggle_section->Add(m_rotate_spool_toggle, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(10));

    // Toggle description
    Label* toggle_description = new Label(parent, _L("Rotate spool when drying"), LB_AUTO_WRAP);
    toggle_description->SetForegroundColour(*wxBLACK);
    toggle_description->SetFont(Label::Body_12);
    toggle_section->Add(toggle_description, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND, 0);

    info_section->Add(toggle_section, 0, wxEXPAND | wxALL, FromDIP(5));

    return info_section;
}

wxBoxSizer* AMSDryCtrWin::create_guide_right_section(wxPanel* parent)
{
    wxBoxSizer* right_section = new wxBoxSizer(wxVERTICAL);

    // Upper part: Image section
    m_image_placeholder = new wxStaticBitmap(parent, wxID_ANY, wxNullBitmap);
    m_guide_image = ScalableBitmap(parent, "dev_ams_dry_ctr_filament_in_chamber", 256);
    m_image_placeholder->SetBitmap(m_guide_image.bmp());

    wxBoxSizer* image_container = new wxBoxSizer(wxHORIZONTAL);
    image_container->AddStretchSpacer(1);
    image_container->Add(m_image_placeholder, 0, wxALL, FromDIP(5));

    right_section->Add(image_container, 0, wxEXPAND | wxALL, FromDIP(5));

    wxBoxSizer* buttons_container = new wxBoxSizer(wxHORIZONTAL);
    buttons_container->AddStretchSpacer(1);

    m_back_button = create_button(
        parent,
        wxString::FromUTF8(_CTX_utf8(L_CONTEXT("Back", "amsdrying"), "amsdrying")),
        wxColour("#F8F8F8"),       // Background color - light gray
        wxColour("#D0D0D0"),       // Border color - gray
        *wxBLACK                   // Text color - black
    );

    m_back_button->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) {
        m_main_simplebook->SetSelection(0);
    });

    buttons_container->Add(m_back_button, 0, wxALL, FromDIP(5));

    m_start_button = create_button(
        parent,
        _L("Start"),
        AMS_CONTROL_BRAND_COLOUR,  // Background color - green
        AMS_CONTROL_BRAND_COLOUR,  // Border color - green
        wxColour("#FFFFFE")        // Text color - white
    );

    m_start_button->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](wxCommandEvent& event) {
        m_main_simplebook->SetSelection(2);
        m_progress_gauge->SetValue(0);

        m_progress_timer->Start(70);
        m_progress_value = 0;
        start_sending_drying_command();
    });

    buttons_container->Add(m_start_button, 0, wxALL, FromDIP(5));

    right_section->Add(buttons_container, 0, wxEXPAND | wxALL, FromDIP(5));

    return right_section;
}

wxBoxSizer* AMSDryCtrWin::create_guide_page_sizer(wxPanel* parent)
{
    wxBoxSizer* guide_sizer = new wxBoxSizer(wxHORIZONTAL);

    wxBoxSizer* info_section = create_guide_info_section(parent);
    guide_sizer->Add(info_section, 1, wxEXPAND | wxALL, FromDIP(10));

    wxBoxSizer* right_section = create_guide_right_section(parent);
    guide_sizer->Add(right_section, 0, wxEXPAND | wxALL, FromDIP(10));

    return guide_sizer;
}

void AMSDryCtrWin::start_sending_drying_command()
{
    auto fila_system = get_fila_system();
    if (!fila_system) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::start_sending_drying_command: Invalid FilaSystem Pointer";
        return;
    }

    if (m_temperature_input->GetValue().IsEmpty() || m_time_input->GetValue().IsEmpty()) {
        // Show error message to user
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::start_sending_drying_command: Time or temperature is empty";
        return;
    }

    long temperature, time;
    if (!m_temperature_input->GetValue().ToLong(&temperature) ||
        !m_time_input->GetValue().ToLong(&time)) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::start_sending_drying_command: Failed to convert temperature or time";
        return;
    }

    int tray_index;
    tray_index = m_trays_combo->GetSelection();
    if (m_tray_ids.empty() || tray_index < 0 || tray_index >= m_tray_ids.size()) {
        BOOST_LOG_TRIVIAL(warning) << "AMSDryCtrWin::start_sending_drying_command: Invalid tray_index " << tray_index
                                   << ", m_tray_ids.size=" << m_tray_ids.size();
        return;
    }

    int cooling_temp = 50;
    std::optional<DevFilamentDryingPreset> preset = DevUtilBackend::GetFilamentDryingPreset(m_tray_ids[tray_index].filament_id);
    if (preset.has_value()) {
        cooling_temp = static_cast<int>(preset.value().filament_dev_drying_softening_temperature);
    }

    fila_system->CtrlAmsStartDryingHour(std::stoi(m_ams_info.m_ams_id), m_tray_ids[tray_index].filament_type,
        temperature, time, m_rotate_spool_toggle->GetValue(), cooling_temp, false);

    m_dry_setting.m_filament_names[m_ams_info.m_ams_id] = m_tray_ids[tray_index].filament_name;
    m_dry_setting.m_filament_type[m_ams_info.m_ams_id] = m_tray_ids[tray_index].filament_type;
    m_dry_setting.m_dry_temp[m_ams_info.m_ams_id] = temperature;
    m_dry_setting.m_dry_time[m_ams_info.m_ams_id] = time;
}

void AMSDryCtrWin::OnProgressTimer(wxTimerEvent& event)
{
    m_progress_value += 1;

    if (m_progress_value <= 100)
        m_progress_gauge->SetValue(m_progress_value);

    if (m_progress_value == 1) { // First tick, reset message index
        m_progress_message_index = 0;
        if (!m_progress_text.empty()) {
            m_progress_title->SetLabel(m_progress_text[0]);
        }
    }
    else if (m_progress_value % 15 == 0) { // Approximately every second
        m_progress_message_index++;
        if (m_progress_message_index < m_progress_text.size()) {
            m_progress_title->SetLabel(m_progress_text[m_progress_message_index]);
            m_progress_title->Refresh();
        }
    }

    if (m_progress_value >= 100 && !is_dry_ctr_idle()) {
        m_progress_value = 0;
        m_progress_timer->Stop();
        m_main_simplebook->SetSelection(0);
    }
}

void AMSDryCtrWin::restore_stop_button_if_deadline_passed()
{
    if (m_stop_button_restore_deadline.has_value()) {
        if (std::chrono::steady_clock::now() >= m_stop_button_restore_deadline.value()) {
            if (m_stop_button) {
                m_stop_button->SetLabel(_L("Stop"));
                update_button_size(m_stop_button);  // Adjust button size back to original
                m_stop_button->Enable();
                m_stop_button->Layout();
                m_stop_button->GetParent()->Layout();
                m_stop_button->Refresh();
            }
            m_stop_button_restore_deadline.reset();
        }
    }
}

void AMSDryCtrWin::restore_unload_button_if_deadline_passed()
{
    if (m_unload_button_restore_deadline.has_value()) {
        if (std::chrono::steady_clock::now() >= m_unload_button_restore_deadline.value()) {
            if (m_unload_button) {
                m_unload_button->Enable();
            }
            m_unload_button_restore_deadline.reset();
        }
    }
}

void AMSDryCtrWin::update_button_size(Button* button)
{
    if (!button) return;

    // Reset MinSize first to avoid accumulation of size
    button->SetMinSize(wxSize(-1, -1));

    // Recalculate button size based on current text and DPI settings
    wxSize best_size = button->GetBestSize();
    int padding_width = FromDIP(4);
    int padding_height = FromDIP(2);
    wxSize final_size(best_size.GetWidth() + padding_width, best_size.GetHeight() + padding_height);
    button->SetMinSize(final_size);
}

void AMSDryCtrWin::OnShow(wxShowEvent& event)
{
    if (event.IsShown()) {
        wxGetApp().UpdateDlgDarkUI(this);
        msw_rescale();
    }
    event.Skip();
}

void AMSDryCtrWin::OnClose(wxCloseEvent& event)
{
    if (m_progress_timer && m_progress_timer->IsRunning()) {
        m_progress_timer->Stop();
    }

    if (m_progress_gauge) {
        m_progress_gauge->SetValue(0);
    }

    m_progress_value = 0;
    m_progress_message_index = 0;

    if (m_main_simplebook) {
        m_main_simplebook->SetSelection(0);
    }

    if (m_progress_title && !m_progress_text.empty()) {
        m_progress_title->SetLabel(m_progress_text[0]);
    }

    // Clean up and restore stop button state
    if (m_stop_button) {
        m_stop_button->SetLabel(_L("Stop"));
        update_button_size(m_stop_button);  // Adjust button size back to original
        m_stop_button->Enable();
        m_stop_button->Layout();
        m_stop_button->GetParent()->Layout();
        m_stop_button->Refresh();
    }
    m_stop_button_restore_deadline.reset();

    // Clean up unload button state
    if (m_unload_button) {
        m_unload_button->Enable();
    }
    m_unload_button_restore_deadline.reset();

    event.Skip();
}

wxBoxSizer* AMSDryCtrWin::create_progress_page_sizer(wxPanel* parent)
{
    wxBoxSizer* progress_sizer = new wxBoxSizer(wxVERTICAL);

    m_progress_title = new Label(parent, m_progress_text[0]);
    m_progress_title->SetForegroundColour(*wxBLACK);
    m_progress_title->SetFont(Label::Body_16);

    progress_sizer->Add(0, 0, 1, wxEXPAND, 0); // Spacer
    progress_sizer->Add(m_progress_title, 0, wxALIGN_CENTER | wxALL, FromDIP(30));

    m_progress_gauge = new ProgressBar(parent, wxID_ANY, 100, wxDefaultPosition, wxSize(FromDIP(300), FromDIP(8)));
    m_progress_gauge->SetValue(0);
    m_progress_gauge->SetHeight(FromDIP(8));

    progress_sizer->Add(m_progress_gauge, 0, wxALIGN_CENTER | wxALL, FromDIP(10));
    progress_sizer->Add(0, 0, 1, wxEXPAND, 0);

    return progress_sizer;
}

void AMSDryCtrWin::create()
{
    // set title icon
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    this->SetDoubleBuffered(true);
    std::string icon_path = (boost::format("%1%/images/OrcaSlicerTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));

    SetSize(wxSize(FromDIP(700), FromDIP(500)));
    SetMinSize(wxSize(FromDIP(700), FromDIP(500)));
    SetMaxSize(wxSize(FromDIP(700), FromDIP(500)));

    m_main_simplebook = new wxSimplebook(this, wxID_ANY, wxDefaultPosition, wxDefaultSize);

    // Create main page
    m_original_page = new wxPanel(m_main_simplebook, wxID_ANY);
    m_original_page->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* main_sizer = create_main_page_sizer(m_original_page);
    m_original_page->SetSizer(main_sizer);

    m_main_simplebook->AddPage(m_original_page, "Main Page");

    m_guide_page = new wxPanel(m_main_simplebook, wxID_ANY);
    m_guide_page->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* guide_sizer = create_guide_page_sizer(m_guide_page);
    m_guide_page->SetSizer(guide_sizer);
    m_main_simplebook->AddPage(m_guide_page, "Guide Page");

    // Create progress page
    m_progress_page = new wxPanel(m_main_simplebook, wxID_ANY);
    m_progress_page->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* progress_sizer = create_progress_page_sizer(m_progress_page);
    m_progress_page->SetSizer(progress_sizer);
    m_main_simplebook->AddPage(m_progress_page, "Progress Page");

    m_progress_timer = new wxTimer(this, wxID_ANY);
    Bind(wxEVT_TIMER, &AMSDryCtrWin::OnProgressTimer, this, m_progress_timer->GetId());

    Bind(wxEVT_CLOSE_WINDOW, &AMSDryCtrWin::OnClose, this);

    wxBoxSizer* top_level_sizer = new wxBoxSizer(wxVERTICAL);
    top_level_sizer->Add(m_main_simplebook, 1, wxEXPAND | wxALL, FromDIP(10));

    Bind(wxEVT_SHOW, &AMSDryCtrWin::OnShow, this);

    SetSizer(top_level_sizer);
    Layout();
    Refresh();
    wxGetApp().UpdateDlgDarkUI(this);
}

void AMSDryCtrWin::on_dpi_changed(const wxRect &suggested_rect)
{
    msw_rescale();
}

void AMSDryCtrWin::msw_rescale()
{
    if (!IsShown()) {
        return;
    }

    if (m_ams_filament_panel) {m_ams_filament_panel->msw_rescale();}
    if (m_trays_combo) {m_trays_combo->Rescale(); m_trays_combo->Layout(); m_trays_combo->SetFont(Label::Body_14), m_trays_combo->Refresh();}
    if (m_humidity_img && m_humidity_image.bmp().IsOk()) {m_humidity_image.msw_rescale(); m_humidity_img->SetBitmap(m_humidity_image.bmp());}
    if (m_image_placeholder && m_guide_image.bmp().IsOk()) {m_guide_image.msw_rescale();m_image_placeholder->SetBitmap(m_guide_image.bmp());}
    if (m_image_description_icon && m_description_icon_bitmap.bmp().IsOk()) {m_description_icon_bitmap.msw_rescale(); m_image_description_icon->SetBitmap(m_description_icon_bitmap.bmp());}

    // Rescale and update button sizes based on text content
    if (m_next_button) {
        m_next_button->Rescale();
        update_button_size(m_next_button);
        m_next_button->Layout();
        m_next_button->Refresh();
    }
    if (m_stop_button) {
        m_stop_button->Rescale();
        update_button_size(m_stop_button);
        m_stop_button->Layout();
        m_stop_button->Refresh();
    }
    if (m_start_button) {
        m_start_button->Rescale();
        update_button_size(m_start_button);
        m_start_button->Layout();
        m_start_button->Refresh();
    }
    if (m_back_button) {
        m_back_button->Rescale();
        update_button_size(m_back_button);
        m_back_button->Layout();
        m_back_button->Refresh();
    }
    if (m_unload_button) {
        m_unload_button->Rescale();
        update_button_size(m_unload_button);
        m_unload_button->Layout();
        m_unload_button->Refresh();
    }

    if (m_guide_page) {m_guide_page->Layout();}
    if (m_original_page) {m_original_page->Layout();}

    Fit();
    Layout();
    Refresh();
}

void AMSDryCtrWin::set_ams_id(const std::string& ams_id)
{
    m_ams_info.m_ams_id = ams_id;
    m_is_ams_changed = true;
}

void AMSDryCtrWin::update_img_description(DevAms::DryStatus status, DevAms::DrySubStatus sub_status)
{
    if (status == DevAms::DryStatus::Off || status == DevAms::DryStatus::Cooling) {
        m_image_description->SetLabel(_L("Idle"));
        m_image_description_icon->Show(false);
        return;
    }

    // Determine label text for non-idle states
    wxString label_text;
    if (status == DevAms::DryStatus::Error) {
        label_text = _L("Drying");
    } else if (sub_status == DevAms::DrySubStatus::Heating) {
        label_text = _L("Drying-Heating");
    } else if (sub_status == DevAms::DrySubStatus::Dehumidify) {
        label_text = _L("Drying-Dehumidifying");
    } else {
        m_image_description_icon->Show(false);
        return;
    }

    m_image_description->SetLabel(label_text);

    try {
        m_description_icon_bitmap = ScalableBitmap(this, "dev_ams_dry_ctr_heating_icon", 20);
        m_description_icon_bitmap.msw_rescale();
        m_image_description_icon->SetBitmap(m_description_icon_bitmap.bmp());
        m_image_description_icon->Show(true);
    } catch (Exception&) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Error loading drying icon";
        m_image_description_icon->Show(false);
    }
}

int AMSDryCtrWin::update_image(DevAmsType model, DevAms::DryStatus status, DevAms::DrySubStatus sub_status, int humidity_percent)
{
    if (model == m_ams_info.m_model && status == m_ams_info.m_dry_status
        && sub_status == m_ams_info.m_dry_sub_status && humidity_percent == m_ams_info.m_humidity_percent) {
        return 0;
    }

    std::string img_path;

    if (status == DevAms::DryStatus::Off && sub_status == DevAms::DrySubStatus::Off) {
        img_path = get_humidity_level_img_path(humidity_percent);
    } else {
        img_path = get_dry_status_img_path(model, status, sub_status);
    }

    if (img_path.empty()) {
        return 0;
    }

    m_humidity_image = ScalableBitmap(this, img_path, AMS_DRY_STATUS_IMAGE_SIZE);
    m_humidity_img->SetBitmap(m_humidity_image.bmp());

    update_img_description(status, sub_status);

    return 1;
}

bool AMSDryCtrWin::check_values_changed(DevAms* dev_ams)
{
    bool changed = false;

    if (m_ams_info.m_model != dev_ams->GetAmsType()) {
        m_ams_info.m_model = dev_ams->GetAmsType();
        changed = true;
    }

    if (dev_ams->GetDryStatus().has_value() && m_ams_info.m_dry_status != dev_ams->GetDryStatus()) {
        m_ams_info.m_dry_status = dev_ams->GetDryStatus().value();
        changed = true;
    }

    if (dev_ams->GetDrySubStatus().has_value() && m_ams_info.m_dry_sub_status != dev_ams->GetDrySubStatus()) {
        m_ams_info.m_dry_sub_status = dev_ams->GetDrySubStatus().value();
        changed = true;
    }

    if (m_ams_info.m_humidity_percent != dev_ams->GetHumidityPercent()) {
        m_ams_info.m_humidity_percent = dev_ams->GetHumidityPercent();
        changed = true;
    }

    return changed;
}

void AMSDryCtrWin::update_normal_description(DevAms* dev_ams)
{
    if (m_tray_ids.empty() || m_trays_combo->GetSelection() >= m_tray_ids.size()) { return; }

    FilamentBaseInfo info = m_tray_ids[m_trays_combo->GetSelection()];
    wxString warning_text;
    bool can_enable_button = true;
    long temp_val = 0, time_val = 0;
    m_temperature_input->GetValue().ToLong(&temp_val);
    m_time_input->GetValue().ToLong(&time_val);
    std::optional<Slic3r::DevFilamentDryingPreset> preset = DevUtilBackend::GetFilamentDryingPreset(info.filament_id);
    auto total_dry = preset.has_value() ? preset.value().ams_limitations : std::unordered_set<DevAmsType>();

    struct AmsTempLimit { DevAmsType type; int min_temp; int max_temp; const char* name; };
    static const AmsTempLimit ams_limits[] = {
        { DevAmsType::N3F, 45, 65, "AMS2" },
        { DevAmsType::N3S, 45, 85, "AMS-S" }
    };

    for (const auto& lim : ams_limits) {
        if (dev_ams->GetAmsType() == lim.type) {
            if (temp_val > lim.max_temp) {
                wxString msg = wxString(lim.name) + _L(" maximum drying temperature is ") + wxString::Format(wxT("%d"), lim.max_temp) + wxString::FromUTF8("°C.");
                warning_text += msg + "\n";
                can_enable_button = false;
            } else if (temp_val < lim.min_temp) {
                wxString msg = wxString(lim.name) + _L(" minimum drying temperature is ") + wxString::Format(wxT("%d"), lim.min_temp) + wxString::FromUTF8("°C.");
                warning_text += msg + "\n";
                can_enable_button = false;
            }
            if (total_dry.find(lim.type) == total_dry.end()) {
                warning_text += _L("This filament may not be completely dried.") + "\n\n";
            }
            break;
        }
    }

    if (m_printer_status.m_is_printing && temp_val  > m_ams_info.m_recommand_dry_temp) {
        warning_text += _L("This AMS is currently printing. To ensure print quality, the drying temperature cannot exceed the recommended drying temperature.") + "\n";
        can_enable_button = false;
    } else if (preset.has_value()) {
        // Only check heat distortion temperature when AMS has filament inserted;
        // empty AMS only needs to respect the device hardware temperature limit.
        bool has_filament_inserted = false;
        for (const auto& tray_pair : dev_ams->GetTrays()) {
            if (tray_pair.second && tray_pair.second->is_exists) {
                has_filament_inserted = true;
                break;
            }
        }
        if (has_filament_inserted) {
            auto limit_temperature = preset.value().filament_dev_ams_drying_heat_distortion_temperature;
            if (temp_val > limit_temperature) {
                warning_text += _L("The temperature shall not exceed the filament's heat distortion temperature") + "(" +
                    wxString::Format(wxT("%d"), static_cast<int>(limit_temperature)) + wxString::FromUTF8("°C)\n");
                can_enable_button = false;
            }
        }
    }

    if (time_val < 1) {
        warning_text += _L("Minimum time value cannot be less than 1.") + "\n";
        can_enable_button = false;
    } else if (time_val > 24) {
        warning_text += _L("Maximum time value cannot be greater than 24.") + "\n";
        can_enable_button = false;
    }

    m_next_button->Enable(can_enable_button);
    m_normal_description->SetLabel(warning_text);
    m_normal_description->Wrap(FromDIP(250));
    m_normal_description->GetParent()->Layout();
}

void AMSDryCtrWin::update_normal_state(DevAms* dev_ams)
{
    if (is_dry_ctr_idle(dev_ams)) {
        m_next_button->Show(true);
        m_normal_description->Show(true);
        m_trays_combo->Enable();
        m_temperature_input->SetEditable(true);
        m_time_input->SetEditable(true);

        m_stop_button->Hide();
        m_dry_error_sizer->Show(false);
    } else if (is_dry_ctr_err(dev_ams)) {
        m_dry_error_sizer->Show(true);
        m_next_button->Hide();
        m_normal_description->Hide();

        m_trays_combo->Hide();
        m_temperature_input->Hide();
        m_time_input->Hide();
        m_stop_button->Show(true);
    } else {
        m_next_button->Hide();
        m_normal_description->Hide();

        m_trays_combo->Disable();
        m_temperature_input->SetEditable(false);
        m_time_input->SetEditable(false);
        m_stop_button->Show(true);
        m_dry_error_sizer->Show(false);
    }

    if (m_temperature_input->GetValue().IsEmpty() || m_time_input->GetValue().IsEmpty()) {
        m_next_button->Disable();
        m_start_button->Disable();
    } else {
        m_next_button->Enable();
    }

    update_normal_description(dev_ams);
}

void AMSDryCtrWin::OnFilamentSelectionChanged(wxCommandEvent& event)
{
    int selectionIndex = event.GetSelection();
    wxString selectedFilament = event.GetString();

    auto fila_system = get_fila_system();
    if (!fila_system) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::OnFilamentSelectionChanged: Invalid FilaSystem Pointer";
        return;
    }

    DevAms* dev_ams = fila_system->GetAmsById(m_ams_info.m_ams_id);
    if (!dev_ams) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::OnFilamentSelectionChanged: Invalid AMS id";
        return;
    }

    std::optional<Slic3r::DevFilamentDryingPreset> preset = DevUtilBackend::GetFilamentDryingPreset(m_tray_ids[selectionIndex].filament_id);
    if (preset.has_value()) {
        DevFilamentDryingPreset info = preset.value();
        if (m_printer_status.m_is_printing) {
            m_temperature_input->SetValue(std::to_string(static_cast<int>(info.filament_dev_ams_drying_temperature_on_print[dev_ams->GetAmsType()])));
            m_time_input->SetValue(std::to_string(static_cast<int>(info.filament_dev_ams_drying_time_on_print[dev_ams->GetAmsType()])));
        } else {
            m_temperature_input->SetValue(std::to_string(static_cast<int>(info.filament_dev_ams_drying_temperature_on_idle[dev_ams->GetAmsType()])));
            m_time_input->SetValue(std::to_string(static_cast<int>(info.filament_dev_ams_drying_time_on_idle[dev_ams->GetAmsType()])));
        }
    }

    update_filament_guide_info(dev_ams);
}

wxString get_cannot_reason_text(DevAms::CannotDryReason reason)
{
    wxString cannot_reason_text;
    switch (reason)
    {
    case DevAms::CannotDryReason::InsufficientPower:
        cannot_reason_text = "*" + _L("Insufficient power") + "\n";
        cannot_reason_text += _L("  Too many AMS drying simultaneously. Please plug in the power or stop other drying processes before starting.") + "\n";
        break;
    case DevAms::CannotDryReason::AmsBusy:
        cannot_reason_text = "*" + _L("AMS is busy") + "\n";
        cannot_reason_text += _L("  AMS is calibrating | reading RFID | loading/unloading material, please wait.") + "\n";
        break;
    case DevAms::CannotDryReason::ConsumableAtAmsOutlet:
        cannot_reason_text = "*" + _L("Filament in AMS outlet") + "\n";
        cannot_reason_text += _L("  The high drying temperature may cause AMS blockage, please unload first.");
        break;
    case DevAms::CannotDryReason::InitiatingAmsDrying:
        cannot_reason_text = "*" + _L("Initiating AMS drying") + "\n";
        break;
    case DevAms::CannotDryReason::NotSupportedIn2dMode:
        cannot_reason_text = "*" + _L("Not supported in 2D mode") + "\n";
        break;
    case DevAms::CannotDryReason::DryingInProgress:
        cannot_reason_text = "*" + _L("Task in progress") + "\n";
        cannot_reason_text += _L("  The AMS might be in use during Task.") + "\n";
        break;
    case DevAms::CannotDryReason::Upgrading:
        cannot_reason_text = "*" + _L("Upgrading") + "\n";
        cannot_reason_text += _L("  Firmware update in progress, please wait...") + "\n";
        break;
    case DevAms::CannotDryReason::InsufficientPowerNeedPluginPower:
        cannot_reason_text = "*" + _L("Insufficient power") + "\n";
        cannot_reason_text += _L("  Please plug in the power and then use the drying function.") + "\n";
        break;
    case DevAms::CannotDryReason::FilamentAtAmsOutletManualUnload:
        cannot_reason_text = "*" + _L("Filament in AMS outlet") + "\n";
        cannot_reason_text += _L("  The high drying temperature may cause AMS blockage. Please unload the filament manually before proceeding.") + "\n";
        break;
    default:
        cannot_reason_text = "*" + _L("System is busy") + "\n";
        cannot_reason_text += _L("  Initiating other drying processes, please wait a few seconds...") + "\n";
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": unknown cannot dry reason";
        break;
    }
    return cannot_reason_text;
}

wxString organize_cannot_reasons_text(std::vector<DevAms::CannotDryReason>& reasons)
{
    wxString cannot_reasons_text;
    if (std::find(reasons.begin(), reasons.end(), DevAms::CannotDryReason::DryingInProgress) != reasons.end()) {
        cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::DryingInProgress);
    } else if (std::find(reasons.begin(), reasons.end(), DevAms::CannotDryReason::AmsBusy) != reasons.end()) {
        cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::AmsBusy);
    } else if (std::find(reasons.begin(), reasons.end(), DevAms::CannotDryReason::ConsumableAtAmsOutlet) != reasons.end()) {
        cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::ConsumableAtAmsOutlet);
    } else if (std::find(reasons.begin(), reasons.end(), DevAms::CannotDryReason::FilamentAtAmsOutletManualUnload) != reasons.end()) {
        cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::FilamentAtAmsOutletManualUnload);
    }

    if (std::find(reasons.begin(), reasons.end(), DevAms::CannotDryReason::InsufficientPower) != reasons.end()) {
        cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::InsufficientPower);
    }

    for (auto reason : reasons) {
        if (reason == DevAms::CannotDryReason::InsufficientPowerNeedPluginPower) {
            cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::InsufficientPowerNeedPluginPower);
        } else if (reason == DevAms::CannotDryReason::NotSupportedIn2dMode) {
            cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::NotSupportedIn2dMode);
        } else if (reason == DevAms::CannotDryReason::InitiatingAmsDrying) {
            cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::InitiatingAmsDrying);
        } else if (reason == DevAms::CannotDryReason::Upgrading) {
            cannot_reasons_text += get_cannot_reason_text(DevAms::CannotDryReason::Upgrading);
        }
    }

    if (cannot_reasons_text.empty()) {
        cannot_reasons_text += "*" + _L("System is busy") + "\n";
        cannot_reasons_text += _L("  Initiating other drying processes, please wait a few seconds...") + "\n";
    }

    return cannot_reasons_text;
}

int AMSDryCtrWin::update_state(DevAms* dev_ams)
{
    std::vector<DevAms::CannotDryReason> cannot_reasons = dev_ams->GetCannotDryReason().has_value()?
        dev_ams->GetCannotDryReason().value(): std::vector<DevAms::CannotDryReason>();

    if (cannot_reasons.size() == 0 ||
        (cannot_reasons.size() == 1 && cannot_reasons[0] == DevAms::CannotDryReason::DryingInProgress)) {
        m_normal_state_sizer->Show(true);
        m_cannot_dry_sizer->Show(false);
        m_unload_button->Show(false);
        update_normal_state(dev_ams);
    } else if (cannot_reasons.size() > 0) {
        m_normal_state_sizer->Show(false);
        m_cannot_dry_sizer->Show(true);

        m_cannot_dry_description_label->SetLabel(organize_cannot_reasons_text(cannot_reasons));
        m_cannot_dry_description_label->Wrap(FromDIP(300));

        // Show unload button when ConsumableAtAmsOutlet reason is present
        bool has_consumable_at_outlet = std::find(cannot_reasons.begin(), cannot_reasons.end(),
            DevAms::CannotDryReason::ConsumableAtAmsOutlet) != cannot_reasons.end();
        m_unload_button->Show(has_consumable_at_outlet);
    }

    restore_stop_button_if_deadline_passed();
    restore_unload_button_if_deadline_passed();

    return 1;
}

int AMSDryCtrWin::update_ams_change(DevAms* dev_ams)
{
    if (!is_ams_changed(dev_ams)) {
        return 0;
    }

    m_ams_info.m_ams_id = dev_ams->GetAmsId();
    if (dev_ams->GetAmsType() == DevAmsType::N3F) {
        m_temperature_input->SetHint("45-65" + wxString::FromUTF8("°C"));
    } else if (dev_ams->GetAmsType() == DevAmsType::N3S) {
        m_temperature_input->SetHint("45-85" + wxString::FromUTF8("°C"));
    }

    m_time_input->SetHint("1-24 h");

    return 0;
}

int AMSDryCtrWin::update_dryness_status(DevAms* dev_ams)
{
    int updated = 0;

    if (m_ams_info.m_humidity_percent != dev_ams->GetHumidityPercent()) {
        updated += 1;
        m_ams_info.m_humidity_percent = dev_ams->GetHumidityPercent();
        m_humidity_data_label->SetLabel(std::to_string(m_ams_info.m_humidity_percent) + "%");
    }

    if (m_ams_info.m_temperature != dev_ams->GetCurrentTemperature()) {
        updated += 1;
        m_ams_info.m_temperature = dev_ams->GetCurrentTemperature();
        m_temperature_data_label->SetLabel(std::to_string(m_ams_info.m_temperature) + wxString::FromUTF8("°C"));
    }

    if (is_dry_ctr_idle(dev_ams)) {
        m_time_descrition_container->Show(false);
        m_original_page->Layout();
    } else {
        if (is_dry_status_changed(dev_ams)) {
            m_time_descrition_container->Show(true);
            m_original_page->Layout();
        }

        if (m_ams_info.m_left_dry_time != dev_ams->GetLeftDryTime()) {
            updated += 1;
            m_ams_info.m_left_dry_time = dev_ams->GetLeftDryTime();
            m_time_data_label->SetLabel(wxString::Format("%02d : %02d",
                m_ams_info.m_left_dry_time / 60, m_ams_info.m_left_dry_time % 60));
        }
    }

    return updated;
}

bool AMSDryCtrWin::is_tray_changed(DevAms* dev_ams)
{
    return false;
}

void AMSDryCtrWin::update_filament_guide_info(DevAms* dev_ams)
{
    if (!IsShown()) {
        return;
    }

    m_guide_page->Freeze();
    m_ams_filament_panel->Clear();
    m_ams_filament_panel->SetAmsName(dev_ams->GetDisplayName());

    // Get the temperature input value
    long input_temp = 0;
    bool valid_temp = !m_temperature_input->GetValue().IsEmpty() &&
                      m_temperature_input->GetValue().ToLong(&input_temp);
    bool can_start = true;

    int slot_count = 0, empty_count = 0;
    for (auto& tray_pair : dev_ams->GetTrays()) {
        if (!tray_pair.second) {
            continue;
        }
        ++slot_count;
        if (!tray_pair.second->is_exists) {
            m_ams_filament_panel->AddFilamentItem("/", "dev_ams_dry_ctr_enable");
            ++empty_count;
            continue;
        }
        auto preset_opt = tray_pair.second->get_ams_drying_preset();
        wxString filament_type = tray_pair.second->get_display_filament_type();
        DevFilamentDryingPreset preset;
        if (filament_type.IsEmpty()) {
            auto fallback_preset = DevUtilBackend::GetFilamentDryingPreset("GFA00");
            preset = fallback_preset.value();
            filament_type = "?";
        } else if (preset_opt.has_value()) {
            preset = preset_opt.value();
        } else {
            auto fallback_preset = DevUtilBackend::GetFilamentDryingPreset("GFA00");
            preset = fallback_preset.value();
        }
        std::string icon_path = "dev_ams_dry_ctr_enable";
        int distortion_temp = static_cast<int>(preset.filament_dev_ams_drying_heat_distortion_temperature);
        if (valid_temp && distortion_temp < input_temp) {
            icon_path = "dev_ams_dry_ctr_disable";
            can_start = false;
        }
        m_ams_filament_panel->AddFilamentItem(filament_type, icon_path);
    }
    if (slot_count == 0) {
        can_start = false;
    }

    if (can_start) {
        m_guide_title_label->SetLabel(_L("For better drying results, remove the filament and allow it to rotate."));
        m_guide_description_label->SetLabel(_L("The AMS will automatically rotate the stored filament slots to enhance the drying performance.") +
            "\n" + _L("Alternatively, you can dry the filament without removing it.") +
            "\n" + "*" + _L("Unknown filaments will be treated as PLA."));
        m_guide_description_label->Wrap(FromDIP(300));
    } else {
        m_guide_title_label->SetLabel(_L("Please store the filament marked with an exclamation mark."));
        m_guide_description_label->SetLabel(_L("Filament left in the feeder during drying may soften because the drying temperature exceeds the softening point of materials like PLA and TPU.") +
            "\n" + "*" + _L("Unknown filaments will be treated as PLA."));
        m_guide_description_label->Wrap(FromDIP(300));
    }

    m_start_button->Enable(can_start);
    m_guide_page->Layout();
    m_guide_page->Thaw();
}

int AMSDryCtrWin::update_filament_list(DevAms* dev_ams, MachineObject* obj)
{
    const bool ams_changed = is_ams_changed(dev_ams);
    bool rebuilt_filament_list = false;

    // Build `m_tray_ids` and the combobox items from the preset bundle.
    // This list is also used as a lookup table in non-idle state (type -> name),
    // so it must be available even when the combobox is disabled.
    auto rebuild_filament_list = [&]() -> bool {
        m_trays_combo->Clear();
        m_tray_ids.clear();

        PresetBundle *preset_bundle = wxGetApp().preset_bundle;
        if (!preset_bundle || !obj) {
            return false;
        }

        std::set<std::string> filament_id_set;
        auto & filaments = preset_bundle->filaments;

        // Get nozzle diameter using the same method as AMSMaterialsSetting::Popup
        std::ostringstream stream;
        int extruder_id = obj->GetFilaSystem()->GetExtruderIdByAmsId(m_ams_info.m_ams_id);
        if (!obj->GetExtderSystem()->GetExtderById(extruder_id)) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " get extruder id failed";
            extruder_id = 0;
        }
        stream << std::fixed << std::setprecision(1) << obj->GetExtderSystem()->GetNozzleDiameter(extruder_id);
        std::string nozzle_diameter_str = stream.str();
        std::set<std::string> printer_names = preset_bundle->get_printer_names_by_printer_type_and_nozzle(
            DevPrinterConfigUtil::get_printer_display_name(obj->printer_type), nozzle_diameter_str);

        for (auto filament_it = filaments.begin(); filament_it != filaments.end(); ++filament_it) {
            Preset& preset = *filament_it;
            // Filter by system preset: root preset and (system preset or user preset is supported)
            if (filaments.get_preset_base(*filament_it) != &preset || (!filament_it->is_system && !obj->is_support_user_preset)) {
                continue;
            }

            ConfigOption *       printer_opt  = filament_it->config.option("compatible_printers");
            ConfigOptionStrings *printer_strs = dynamic_cast<ConfigOptionStrings *>(printer_opt);
            if (!printer_strs) continue;

            for (auto printer_str : printer_strs->values) {
                if (printer_names.find(printer_str) != printer_names.end()) {
                    if (filament_id_set.find(filament_it->filament_id) != filament_id_set.end()) {
                        continue;
                    }

                    filament_id_set.insert(filament_it->filament_id);
                    auto filament_alias = filaments.get_preset_alias(*filament_it, true);
                    if (!filament_alias.empty()) {
                        auto opt_info = preset_bundle->get_filament_by_filament_id(filament_it->filament_id);
                        if (opt_info.has_value()) {
                            auto real_info = opt_info.value();
                            real_info.filament_name = filament_alias;
                            m_tray_ids.push_back(std::move(real_info));
                            m_trays_combo->Append(wxString::FromUTF8(filament_alias));
                        }
                    }
                }
            }
        }

        if (m_tray_ids.empty()) {
            BOOST_LOG_TRIVIAL(warning) << "AMSDryCtrWin::update_filament_list: No valid filaments found";
            return false;
        }

        return true;
    };

    if (ams_changed || m_tray_ids.empty() || m_trays_combo->GetCount() == 0) {
        // Disable start button during rebuild
        if (m_start_button) {
            m_start_button->Disable();
        }

        rebuilt_filament_list = rebuild_filament_list();
    }

    if (!is_dry_ctr_idle(dev_ams)) {
        const std::string& ams_id = dev_ams->GetAmsId();
        const auto settings_opt = dev_ams->GetDrySettings();

        // Set the combobox display label by the filament "type" string.
        // Priority: last saved selection for this AMS (if the type matches) -> fallback to current `m_tray_ids`.
        auto set_filament_label_by_type = [&](const std::string& filament_type) {
            if (filament_type.empty()) {
                return;
            }

            auto it_name = m_dry_setting.m_filament_names.find(ams_id);
            auto it_type = m_dry_setting.m_filament_type.find(ams_id);
            if (it_name != m_dry_setting.m_filament_names.end() && it_type != m_dry_setting.m_filament_type.end()
                && it_type->second == filament_type) {
                m_trays_combo->SetLabel(wxString::FromUTF8(it_name->second));
                return;
            }

            const auto it = std::find_if(m_tray_ids.begin(), m_tray_ids.end(), [&](const FilamentBaseInfo& tray) {
                return tray.filament_type == filament_type;
            });
            if (it != m_tray_ids.end()) {
                m_trays_combo->SetLabel(wxString::FromUTF8(it->filament_name));
            }
        };

        if (settings_opt.has_value()) {
            const auto& settings = settings_opt.value();
            m_temperature_input->SetValue(std::to_string(settings.dry_temp));
            m_time_input->SetValue(std::to_string(settings.dry_hour));
            set_filament_label_by_type(settings.dry_filament);
        } else {
            auto it_name = m_dry_setting.m_filament_names.find(ams_id);
            if (it_name != m_dry_setting.m_filament_names.end()) {
                m_trays_combo->SetLabel(wxString::FromUTF8(it_name->second));
                m_temperature_input->SetValue(std::to_string(m_dry_setting.m_dry_temp[ams_id]));
                m_time_input->SetValue(std::to_string(m_dry_setting.m_dry_time[ams_id]));
            }
        }

        return 0;
    }

    // Idle state: avoid re-selecting / firing selection change unless the list was rebuilt.
    if (!rebuilt_filament_list && !ams_changed && is_dry_ctr_idle()) {
        return 0;
    }

    if (m_tray_ids.empty()) {
        return 0;
    }

    // Select recommended drying temperature and default filament
    float min_dry_temp = std::numeric_limits<float>::max();
    std::string default_filament_id = "GFA00";
    bool has_ready = false;
    const auto fallback_preset = DevUtilBackend::GetFilamentDryingPreset("GFA00");
    for (const auto& tray_pair : dev_ams->GetTrays()) {
        if (!tray_pair.second || !tray_pair.second->is_tray_info_ready()) continue;
        has_ready = true;
        wxString filament_type = tray_pair.second->get_display_filament_type();
        Slic3r::DevFilamentDryingPreset preset;
        if (filament_type.IsEmpty()) {
            // no filament type, is PLA
            if (!fallback_preset) continue;
            preset = fallback_preset.value();
        } else {
            auto preset_opt = tray_pair.second->get_ams_drying_preset();
            if (preset_opt.has_value()) {
                preset = preset_opt.value();
            } else {
                // no preset，is PLA
                if (!fallback_preset) continue;
                preset = fallback_preset.value();
            }
        }
        float cur_temp = m_printer_status.m_is_printing
            ? std::min({preset.filament_dev_ams_drying_temperature_on_print[dev_ams->GetAmsType()],
                preset.filament_dev_drying_softening_temperature, preset.filament_dev_ams_drying_heat_distortion_temperature})
            : preset.filament_dev_ams_drying_temperature_on_idle[dev_ams->GetAmsType()];
        if (cur_temp < min_dry_temp) {
            min_dry_temp = cur_temp;
            default_filament_id = preset.filament_id;
        }
    }
    // if no tray is ready, use PLA as default
    if (!has_ready) {
        if (fallback_preset.has_value()) {
            Slic3r::DevFilamentDryingPreset preset = fallback_preset.value();
            float cur_temp = m_printer_status.m_is_printing
                ? std::min({preset.filament_dev_ams_drying_temperature_on_print[dev_ams->GetAmsType()],
                    preset.filament_dev_drying_softening_temperature, preset.filament_dev_ams_drying_heat_distortion_temperature})
                : preset.filament_dev_ams_drying_temperature_on_idle[dev_ams->GetAmsType()];
            min_dry_temp = cur_temp;
            default_filament_id = preset.filament_id;
        }
    }
    m_ams_info.m_recommand_dry_temp = min_dry_temp;

    // Set default selection
    unsigned int default_index = 0;
    for (unsigned int i = 0; i < m_tray_ids.size(); i++) {
        if (m_tray_ids[i].filament_id == default_filament_id) {
            default_index = i;
            break;
        }
    }
    m_trays_combo->SetSelection(default_index);
    wxCommandEvent evt(wxEVT_COMBOBOX, m_trays_combo->GetId());
    evt.SetInt(default_index);
    OnFilamentSelectionChanged(evt);
    return 1;
}

std::shared_ptr<DevFilaSystem> AMSDryCtrWin::get_fila_system() const
{
    std::shared_ptr<DevFilaSystem> fila_system = m_fila_system.lock();
    if (!fila_system) {
        const_cast<AMSDryCtrWin*>(this)->Close();
    }
    return fila_system;
}

void AMSDryCtrWin::update_printer_state(MachineObject* obj)
{
    m_printer_status.m_is_printing = obj->is_in_printing()
        && obj->GetExtderSystem()->GetCurrentAmsId() == m_ams_info.m_ams_id;
}

void AMSDryCtrWin::update(std::shared_ptr<DevFilaSystem> fila_system, MachineObject* obj)
{
    if (!fila_system || !obj) {
        return;
    }

    update_printer_state(obj);
    m_fila_system = fila_system;

    DevAms* dev_ams = fila_system->GetAmsById(m_ams_info.m_ams_id);
    if (!dev_ams) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::update: Invalid AMS id";
        m_ams_info.m_ams_id = "";
        Close();
        return;
    }

    if (!dev_ams->IsSupportRemoteDry(fila_system->GetOwner())) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrWin::update: Selected AMS does not support remote drying";
        Close();
        return;
    }


    update_ams_change(dev_ams);

    update_image(dev_ams->GetAmsType(),
        dev_ams->GetDryStatus().has_value()? dev_ams->GetDryStatus().value(): DevAms::DryStatus::Off,
        dev_ams->GetDrySubStatus().has_value()? dev_ams->GetDrySubStatus().value(): DevAms::DrySubStatus::Off,
        dev_ams->GetHumidityPercent());

    update_state(dev_ams);
    update_dryness_status(dev_ams);

    update_filament_list(dev_ams, obj);
    update_filament_guide_info(dev_ams);

    m_is_ams_changed = false;

    check_values_changed(dev_ams);

    Layout();
    Refresh();
}

bool AMSDryCtrWin::is_ams_changed(DevAms* dev_ams)
{
    return m_ams_info.m_ams_id != dev_ams->GetAmsId() || m_is_ams_changed;
}

bool AMSDryCtrWin::is_dry_status_changed(DevAms* dev_ams)
{
    if (!dev_ams->GetDryStatus().has_value() || !dev_ams->GetDrySubStatus().has_value()) {
        return true;
    }

    return dev_ams->GetDryStatus().value() != m_ams_info.m_dry_status
        || dev_ams->GetDrySubStatus().value() != m_ams_info.m_dry_sub_status;
}

bool AMSDryCtrWin::is_dry_ctr_idle(DevAms* dev_ams)
{
    if (!dev_ams->GetDryStatus().has_value() || !dev_ams->GetDrySubStatus().has_value()) {
        return true;
    }

    return dev_ams->GetDryStatus().value() == DevAms::DryStatus::Off
        || dev_ams->GetDryStatus().value() == DevAms::DryStatus::Cooling;
}

bool AMSDryCtrWin::is_dry_ctr_idle()
{
    return m_ams_info.m_dry_status == DevAms::DryStatus::Off || m_ams_info.m_dry_status == DevAms::DryStatus::Cooling;
}

bool AMSDryCtrWin::is_dry_ctr_err(DevAms* dev_ams)
{
    if (!dev_ams->GetDryStatus().has_value() || !dev_ams->GetDrySubStatus().has_value()) {
        return false;
    }

    return dev_ams->GetDryStatus().value() == DevAms::DryStatus::Error;
}

} // GUI
} // Slic3r
