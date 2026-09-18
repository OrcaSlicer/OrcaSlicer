#include "UploadDialog.hpp"
#include "CloudServer.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "slic3r/Utils/Http.hpp"
#include <nlohmann/json.hpp>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <thread>
#include <stdexcept>

namespace Slic3r { namespace GUI {

UploadDialog::UploadDialog(wxWindow* parent, const std::string& username, const std::string& password,
                           const std::string& gcode_path, const std::string& filename)
    : wxDialog(parent, wxID_ANY, _L("Send to cloud"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
      m_username(username), m_password(password), m_gcode_path(gcode_path), m_filename(filename), m_timer(this)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    m_refresh = new wxButton(this, wxID_ANY, _L("Refresh"));
    sizer->Add(m_refresh, 0, wxALL, FromDIP(10));
    sizer->Add(new wxStaticText(this, wxID_ANY, _L("Select printers, or send only to the cloud.")), 0, wxALL, FromDIP(10));
    m_printers = new wxCheckListBox(this, wxID_ANY);
    sizer->Add(m_printers, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));
    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    m_send = new wxButton(this, wxID_ANY, _L("Send"));
    m_cloud_only = new wxButton(this, wxID_ANY, _L("Send only to cloud"));
    buttons->AddStretchSpacer();
    buttons->Add(m_send, 0, wxALL, FromDIP(5));
    buttons->Add(m_cloud_only, 0, wxALL, FromDIP(5));
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Cancel")), 0, wxALL, FromDIP(5));
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(5));
    SetSizer(sizer);
    SetMinSize(FromDIP(wxSize(540, 400)));
    SetSize(FromDIP(wxSize(620, 500)));
    CentreOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { get_online_printers(); });
    m_send->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_file(false); });
    m_cloud_only->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { send_file(true); });
    Bind(wxEVT_TIMER, &UploadDialog::finish_request, this, m_timer.GetId());
    CallAfter([this] { get_online_printers(); });
}

UploadDialog::~UploadDialog()
{
    m_timer.Stop();
    if (m_result) {
        m_result->cancelled = true;
        notify(m_uploading ? _u8L("Cloud upload cancelled. A file already received by the server may still be processed.")
                           : _u8L("Loading online printers cancelled."));
    }
}

void UploadDialog::notify(const std::string& message, bool error)
{
    wxGetApp().plater()->get_notification_manager()->push_notification(
        NotificationType::CustomNotification,
        error ? NotificationManager::NotificationLevel::ErrorNotificationLevel
              : NotificationManager::NotificationLevel::RegularNotificationLevel, message);
}

void UploadDialog::get_online_printers()
{
    m_printers->Clear();
    m_printer_ids.clear();
    start_request(false);
}

void UploadDialog::send_file(bool cloud_only)
{
    std::vector<std::string> selected;
    if (!cloud_only) {
        for (size_t i = 0; i < m_printer_ids.size(); ++i)
            if (m_printers->IsChecked(i)) selected.push_back(m_printer_ids[i]);
    }
    start_request(true, std::move(selected));
}

void UploadDialog::start_request(bool upload, std::vector<std::string> printers)
{
    if (m_result) return;
    if (m_username.empty() || m_password.empty()) {
        notify(_u8L("Please enter your cloud username and password in Preferences."), true);
        return;
    }
    const auto server = cloud_server_url(wxGetApp().app_config->get("cloud_server_url"));
    if (server.empty()) {
        notify(_u8L("Please enter a valid server URL in Preferences."), true);
        return;
    }
    m_uploading = upload;
    m_result = std::make_shared<RequestResult>();
    m_refresh->Disable();
    m_send->Disable();
    m_cloud_only->Disable();
    m_printers->Disable();
    notify(upload ? _u8L("Sending file to cloud...") : _u8L("Loading online printers..."));
    try {
        std::thread([result = m_result, username = m_username, password = m_password,
                     path = m_gcode_path, filename = m_filename, upload, server, printers = std::move(printers)] {
            try {
                const std::string base = server + "/api/";
                auto request = upload ? Http::post(base + "share-file/")
                                      : Http::get(base + "get-online-printer/?username=" + Http::url_encode(username) +
                                                  "&password=" + Http::url_encode(password));
                request.tls_verify(server.rfind("https", 0) == 0).timeout_connect(10).timeout_max(upload ? 600 : 30);
                if (upload) {
                    request.form_add("username", username).form_add("password", password).form_add("filename", filename);
                    for (const auto& printer : printers) request.form_add("printers", printer);
                    request.form_add_file("file", boost::filesystem::path(path), filename);
                }
                request.on_progress([result](Http::Progress, bool& cancel) { cancel = result->cancelled.load(); })
                    .on_complete([result](std::string body, unsigned status) {
                        if (status >= 200 && status < 300) result->body = std::move(body);
                        else result->error = "HTTP " + std::to_string(status);
                    })
                    .on_error([result](std::string, std::string, unsigned status) {
                        // Do not echo transport errors: they can contain the credential-bearing URL.
                        result->error = status ? "HTTP " + std::to_string(status) : _u8L("Network request failed.");
                    }).perform_sync();
            } catch (const std::exception&) {
                result->error = _u8L("Could not complete the cloud request.");
            }
            result->done = true;
        }).detach();
        m_timer.Start(100);
    } catch (const std::exception&) {
        m_result->error = _u8L("Could not start the cloud request.");
        m_result->done = true;
        m_timer.Start(100);
    }
}

void UploadDialog::finish_request(wxTimerEvent&)
{
    if (!m_result || !m_result->done.load()) return;
    m_timer.Stop();
    auto result = std::move(m_result);
    m_refresh->Enable();
    m_send->Enable();
    m_cloud_only->Enable();
    m_printers->Enable();
    const auto failure = m_uploading ? _u8L("Cloud upload failed: ") : _u8L("Could not load online printers: ");
    if (!result->error.empty()) {
        notify(failure + result->error, true);
        return;
    }
    try {
        const auto response = nlohmann::json::parse(result->body);
        if (!response.at("status").get<bool>()) {
            notify(failure + response.value("message", std::string("Unknown error")), true);
            return;
        }
        if (m_uploading) {
            notify(_u8L("File sent successfully."));
            EndModal(wxID_OK);
            return;
        }
        const auto& printers = response.at("data");
        if (!printers.is_array()) throw std::runtime_error("Invalid printer list");
        std::vector<std::string> ids;
        wxArrayString labels;
        for (const auto& printer : printers) {
            auto id = printer.at("printer_id").get<std::string>();
            const auto name = printer.at("name").get<std::string>();
            const auto model = printer.at("model").get<std::string>();
            if (id.empty()) throw std::runtime_error("Empty printer ID");
            labels.Add(wxString::FromUTF8(name + "  (" + model + ")  " + id));
            ids.push_back(std::move(id));
        }
        m_printer_ids = std::move(ids);
        m_printers->Append(labels);
        notify(m_printer_ids.empty() ? _u8L("No online printers. You can still send only to the cloud.")
                                    : _u8L("Online printers loaded successfully."));
    } catch (const std::exception&) {
        notify(failure + _u8L("Invalid response from cloud server."), true);
    }
}

}} // namespace Slic3r::GUI
