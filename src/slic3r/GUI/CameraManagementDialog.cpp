#include "CameraManagementDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/DialogButtons.hpp"
#include "DeviceCore/DevManager.h"

namespace Slic3r { namespace GUI {

CameraEditDialog::CameraEditDialog(wxWindow* parent,
                                   const std::string& dev_id,
                                   const std::string& url,
                                   CameraSourceType source_type,
                                   bool enabled)
    : DPIDialog(parent, wxID_ANY, _L("Edit Camera Override"), wxDefaultPosition, wxSize(FromDIP(400), FromDIP(250)), wxDEFAULT_DIALOG_STYLE)
    , m_initial_dev_id(dev_id)
{
    create_ui();
    populate_printer_list();

    if (!dev_id.empty()) {
        for (size_t i = 0; i < m_printers.size(); ++i) {
            if (m_printers[i].first == dev_id) {
                m_printer_combo->SetSelection(static_cast<int>(i));
                break;
            }
        }
    }
    m_source_type_combo->SetSelection(static_cast<int>(source_type));
    m_url_input->GetTextCtrl()->SetValue(url);
    m_enabled_checkbox->SetValue(enabled);

    update_url_field_state();

    wxGetApp().UpdateDarkUIWin(this);
}

void CameraEditDialog::create_ui()
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    wxFlexGridSizer* grid_sizer = new wxFlexGridSizer(4, 2, FromDIP(10), FromDIP(10));
    grid_sizer->AddGrowableCol(1);

    wxStaticText* printer_label = new wxStaticText(this, wxID_ANY, _L("Printer:"));
    m_printer_combo = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(250), -1), 0, nullptr, wxCB_READONLY);

    wxStaticText* source_type_label = new wxStaticText(this, wxID_ANY, _L("Source Type:"));
    wxArrayString source_types;
    source_types.Add(_L("Built-in Camera"));
    source_types.Add(_L("Web View"));
    source_types.Add(_L("RTSP Stream"));
    source_types.Add(_L("MJPEG Stream"));
    m_source_type_combo = new wxComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(250), -1), source_types, wxCB_READONLY);
    m_source_type_combo->SetSelection(0);
    m_source_type_combo->Bind(wxEVT_COMBOBOX, &CameraEditDialog::on_source_type_changed, this);

    wxStaticText* url_label = new wxStaticText(this, wxID_ANY, _L("Camera URL:"));
    m_url_input = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(250), -1));
    m_url_input->GetTextCtrl()->SetHint(_L("rtsp://user:pass@camera.local:554/stream"));

    wxStaticText* enabled_label = new wxStaticText(this, wxID_ANY, _L("Enabled:"));
    m_enabled_checkbox = new CheckBox(this);

    grid_sizer->Add(printer_label, 0, wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_printer_combo, 1, wxEXPAND);
    grid_sizer->Add(source_type_label, 0, wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_source_type_combo, 1, wxEXPAND);
    grid_sizer->Add(url_label, 0, wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_url_input, 1, wxEXPAND);
    grid_sizer->Add(enabled_label, 0, wxALIGN_CENTER_VERTICAL);
    grid_sizer->Add(m_enabled_checkbox, 0);

    main_sizer->Add(grid_sizer, 0, wxEXPAND | wxALL, FromDIP(15));

    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, &CameraEditDialog::on_ok, this);
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    SetSizer(main_sizer);
    Layout();
    Fit();
    CenterOnParent();
}

void CameraEditDialog::populate_printer_list()
{
    m_printers.clear();

    const auto& existing_cameras = wxGetApp().app_config->get_all_printer_cameras();
    auto has_existing_override = [&](const std::string& dev_id) {
        if (!m_initial_dev_id.empty()) return false;
        return existing_cameras.find(dev_id) != existing_cameras.end();
    };

    auto* dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager) {
        for (const auto& pair : dev_manager->get_local_machinelist()) {
            if (pair.second && !has_existing_override(pair.second->get_dev_id())) {
                m_printers.emplace_back(pair.second->get_dev_id(), pair.second->get_dev_name());
            }
        }
        for (const auto& pair : dev_manager->get_user_machinelist()) {
            if (pair.second && !has_existing_override(pair.second->get_dev_id())) {
                bool exists = false;
                for (const auto& p : m_printers) {
                    if (p.first == pair.second->get_dev_id()) {
                        exists = true;
                        break;
                    }
                }
                if (!exists) {
                    m_printers.emplace_back(pair.second->get_dev_id(), pair.second->get_dev_name());
                }
            }
        }
    }

    const auto& local_machines = wxGetApp().app_config->get_local_machines();
    for (const auto& pair : local_machines) {
        if (has_existing_override(pair.first)) continue;
        bool exists = false;
        for (const auto& p : m_printers) {
            if (p.first == pair.first) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            m_printers.emplace_back(pair.first, pair.second.dev_name);
        }
    }

    m_printer_combo->Clear();
    for (const auto& p : m_printers) {
        wxString display = wxString::Format("%s (%s)", p.second, p.first);
        m_printer_combo->Append(display);
    }

    if (!m_printers.empty()) {
        m_printer_combo->SetSelection(0);
    }
}

void CameraEditDialog::on_ok(wxCommandEvent& event)
{
    if (m_printer_combo->GetSelection() == wxNOT_FOUND) {
        wxMessageBox(_L("Please select a printer"), _L("Error"), wxOK | wxICON_ERROR, this);
        return;
    }
    CameraSourceType type = get_source_type();
    if (type != CameraSourceType::Builtin && m_url_input->GetTextCtrl()->GetValue().IsEmpty()) {
        wxMessageBox(_L("Please enter a camera URL"), _L("Error"), wxOK | wxICON_ERROR, this);
        return;
    }
    EndModal(wxID_OK);
}

std::string CameraEditDialog::get_dev_id() const
{
    int sel = m_printer_combo->GetSelection();
    if (sel >= 0 && sel < static_cast<int>(m_printers.size())) {
        return m_printers[sel].first;
    }
    return "";
}

std::string CameraEditDialog::get_url() const
{
    return m_url_input->GetTextCtrl()->GetValue().ToStdString();
}

CameraSourceType CameraEditDialog::get_source_type() const
{
    int sel = m_source_type_combo->GetSelection();
    if (sel >= 0 && sel <= static_cast<int>(CameraSourceType::MJPEG)) {
        return static_cast<CameraSourceType>(sel);
    }
    return CameraSourceType::Builtin;
}

bool CameraEditDialog::get_enabled() const
{
    return m_enabled_checkbox->GetValue();
}

void CameraEditDialog::on_source_type_changed(wxCommandEvent& event)
{
    update_url_field_state();
}

void CameraEditDialog::update_url_field_state()
{
    CameraSourceType type = get_source_type();
    bool needs_url = (type != CameraSourceType::Builtin);
    m_url_input->Enable(needs_url);

    if (type == CameraSourceType::RTSP) {
        m_url_input->GetTextCtrl()->SetHint(_L("rtsp://user:pass@camera.local:554/stream"));
    } else if (type == CameraSourceType::MJPEG) {
        m_url_input->GetTextCtrl()->SetHint(_L("http://camera.local/mjpg/video.mjpg"));
    } else if (type == CameraSourceType::WebView) {
        m_url_input->GetTextCtrl()->SetHint(_L("http://camera.local/stream"));
    } else {
        m_url_input->GetTextCtrl()->SetHint(wxEmptyString);
    }
}

void CameraEditDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Layout();
    Fit();
}

CameraManagementDialog::CameraManagementDialog(wxWindow* parent)
    : DPIDialog(parent, wxID_ANY, _L("Camera Overrides"), wxDefaultPosition, wxSize(FromDIP(550), FromDIP(400)), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    create_ui();
    refresh_list();
    wxGetApp().UpdateDarkUIWin(this);
}

CameraManagementDialog::~CameraManagementDialog()
{
    cleanup_list_data();
}

void CameraManagementDialog::create_ui()
{
    SetBackgroundColour(*wxWHITE);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    wxStaticText* title = new wxStaticText(this, wxID_ANY, _L("Configure custom camera URLs for each printer"));
    title->SetFont(Label::Body_14);
    main_sizer->Add(title, 0, wxALL, FromDIP(15));

    m_list_ctrl = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_SINGLE | wxDV_ROW_LINES);
    m_list_ctrl->AppendTextColumn(_L("Printer"), wxDATAVIEW_CELL_INERT, FromDIP(120));
    m_list_ctrl->AppendTextColumn(_L("Serial"), wxDATAVIEW_CELL_INERT, FromDIP(80));
    m_list_ctrl->AppendTextColumn(_L("Type"), wxDATAVIEW_CELL_INERT, FromDIP(70));
    m_list_ctrl->AppendTextColumn(_L("Camera URL"), wxDATAVIEW_CELL_INERT, FromDIP(180));
    m_list_ctrl->AppendTextColumn(_L("Enabled"), wxDATAVIEW_CELL_INERT, FromDIP(60));

    m_list_ctrl->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, &CameraManagementDialog::on_selection_changed, this);
    m_list_ctrl->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, &CameraManagementDialog::on_item_activated, this);

    main_sizer->Add(m_list_ctrl, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));

    auto dlg_btns = new DialogButtons(this, {"Add", "Edit", "Delete", "Close"}, "", 3);
    dlg_btns->SetPrimaryButton(_L("Add"));
    main_sizer->Add(dlg_btns, 0, wxEXPAND);

    m_btn_delete = dlg_btns->GetButtonFromLabel(_L("Delete"));
    m_btn_add = dlg_btns->GetButtonFromLabel(_L("Add"));
    m_btn_edit = dlg_btns->GetButtonFromLabel(_L("Edit"));

    m_btn_delete->Enable(false);
    m_btn_edit->Enable(false);

    m_btn_add->Bind(wxEVT_BUTTON, &CameraManagementDialog::on_add, this);
    m_btn_edit->Bind(wxEVT_BUTTON, &CameraManagementDialog::on_edit, this);
    m_btn_delete->Bind(wxEVT_BUTTON, &CameraManagementDialog::on_delete, this);

    dlg_btns->GetButtonFromLabel(_L("Close"))->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });

    dlg_btns->SetAlertButton(_L("Delete"));

    SetSizer(main_sizer);
    Layout();
    CenterOnParent();
}

static wxString source_type_display_name(CameraSourceType type) {
    switch (type) {
        case CameraSourceType::Builtin: return _L("Built-in");
        case CameraSourceType::WebView: return _L("WebView");
        case CameraSourceType::RTSP:    return _L("RTSP");
        case CameraSourceType::MJPEG:   return _L("MJPEG");
    }
    return _L("Built-in");
}

void CameraManagementDialog::cleanup_list_data()
{
    if (!m_list_ctrl) return;

    for (unsigned int i = 0; i < m_list_ctrl->GetItemCount(); ++i) {
        wxDataViewItem item = m_list_ctrl->RowToItem(i);
        auto* dev_id_ptr = reinterpret_cast<std::string*>(m_list_ctrl->GetItemData(item));
        delete dev_id_ptr;
    }
}

void CameraManagementDialog::refresh_list()
{
    cleanup_list_data();
    m_list_ctrl->DeleteAllItems();

    const auto& cameras = wxGetApp().app_config->get_all_printer_cameras();
    for (const auto& pair : cameras) {
        wxVector<wxVariant> data;
        data.push_back(wxVariant(get_printer_name_for_dev_id(pair.first)));
        data.push_back(wxVariant(truncate_serial(pair.first)));
        data.push_back(wxVariant(source_type_display_name(pair.second.source_type)));
        data.push_back(wxVariant(pair.second.custom_source));
        data.push_back(wxVariant(pair.second.enabled ? _L("Yes") : _L("No")));
        m_list_ctrl->AppendItem(data, reinterpret_cast<wxUIntPtr>(new std::string(pair.first)));
    }

    m_btn_edit->Enable(false);
    m_btn_delete->Enable(false);
}

void CameraManagementDialog::on_add(wxCommandEvent& event)
{
    CameraEditDialog dlg(this);
    if (dlg.ShowModal() == wxID_OK) {
        PrinterCameraConfig config;
        config.dev_id = dlg.get_dev_id();
        config.custom_source = dlg.get_url();
        config.source_type = dlg.get_source_type();
        config.enabled = dlg.get_enabled();
        wxGetApp().app_config->set_printer_camera(config);
        refresh_list();
    }
}

void CameraManagementDialog::on_edit(wxCommandEvent& event)
{
    int row = m_list_ctrl->GetSelectedRow();
    if (row == wxNOT_FOUND) return;

    auto* dev_id_ptr = reinterpret_cast<std::string*>(m_list_ctrl->GetItemData(m_list_ctrl->RowToItem(row)));
    if (!dev_id_ptr) return;

    std::string dev_id = *dev_id_ptr;
    auto config = wxGetApp().app_config->get_printer_camera(dev_id);

    CameraEditDialog dlg(this, dev_id, config.custom_source, config.source_type, config.enabled);
    if (dlg.ShowModal() == wxID_OK) {
        if (dlg.get_dev_id() != dev_id) {
            wxGetApp().app_config->erase_printer_camera(dev_id);
        }

        PrinterCameraConfig new_config;
        new_config.dev_id = dlg.get_dev_id();
        new_config.custom_source = dlg.get_url();
        new_config.source_type = dlg.get_source_type();
        new_config.enabled = dlg.get_enabled();
        wxGetApp().app_config->set_printer_camera(new_config);
        refresh_list();
    }
}

void CameraManagementDialog::on_delete(wxCommandEvent& event)
{
    int row = m_list_ctrl->GetSelectedRow();
    if (row == wxNOT_FOUND) return;

    auto* dev_id_ptr = reinterpret_cast<std::string*>(m_list_ctrl->GetItemData(m_list_ctrl->RowToItem(row)));
    if (!dev_id_ptr) return;

    int result = wxMessageBox(_L("Are you sure you want to delete this camera override?"), _L("Confirm Delete"), wxYES_NO | wxICON_QUESTION, this);
    if (result == wxYES) {
        wxGetApp().app_config->erase_printer_camera(*dev_id_ptr);
        refresh_list();
    }
}

void CameraManagementDialog::on_selection_changed(wxDataViewEvent& event)
{
    bool has_selection = m_list_ctrl->GetSelectedRow() != wxNOT_FOUND;
    m_btn_edit->Enable(has_selection);
    m_btn_delete->Enable(has_selection);
}

void CameraManagementDialog::on_item_activated(wxDataViewEvent& event)
{
    wxCommandEvent evt;
    on_edit(evt);
}

std::string CameraManagementDialog::get_printer_name_for_dev_id(const std::string& dev_id)
{
    auto* dev_manager = wxGetApp().getDeviceManager();
    if (dev_manager) {
        MachineObject* obj = dev_manager->get_local_machine(dev_id);
        if (obj) return obj->get_dev_name();

        obj = dev_manager->get_user_machine(dev_id);
        if (obj) return obj->get_dev_name();
    }

    const auto& local_machines = wxGetApp().app_config->get_local_machines();
    auto it = local_machines.find(dev_id);
    if (it != local_machines.end()) {
        return it->second.dev_name;
    }

    return truncate_serial(dev_id);
}

std::string CameraManagementDialog::truncate_serial(const std::string& dev_id)
{
    return dev_id;
}

void CameraManagementDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    Layout();
}

}} // namespace Slic3r::GUI
