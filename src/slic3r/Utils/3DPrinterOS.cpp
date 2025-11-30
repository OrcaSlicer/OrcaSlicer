#include "3DPrinterOS.hpp"

#include <algorithm>
#include <sstream>
#include <exception>
#include <boost/format.hpp>
#include <boost/log/trivial.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/algorithm/string/predicate.hpp>

#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/event.h>
#include <wx/dialog.h>
#include <wx/radiobut.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include "Http.hpp"
#include <wx/busyinfo.h>


namespace fs = boost::filesystem;
namespace pt = boost::property_tree;

namespace {

class UploadOptionsDialog : public Slic3r::GUI::DPIDialog
{
public:
    UploadOptionsDialog(wxWindow*            parent,
                        const wxArrayString& cloud_projects,
                        const wxArrayString& cloud_printer_types,
                        const wxString       preset_name)
        : Slic3r::GUI::DPIDialog(parent,
                                 wxID_ANY,
                                 "3DPrinterOS Cloud upload options",
                                 wxDefaultPosition,
                                 wxSize(100 * Slic3r::GUI::wxGetApp().em_unit(), -1),
                                 wxDEFAULT_DIALOG_STYLE),
        okButton(nullptr)
    {
        SetFont(Slic3r::GUI::wxGetApp().normal_font());
        SetBackgroundColour(*wxWHITE);
        SetForegroundColour(*wxBLACK);

        singleRadio                = new wxRadioButton(this, wxID_ANY, "Single file", wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
        projectRadio               = new wxRadioButton(this, wxID_ANY, "Project File");
        projectsLabel              = new wxStaticText(this, wxID_ANY, "Project:");
        wxStaticText* printerLabel = new wxStaticText(this, wxID_ANY, "Printer type:");

        projectsComboBox    = new wxComboBox(this, wxID_ANY, wxString(""), wxDefaultPosition, wxDefaultSize, 0, nullptr, DD_NO_CHECK_ICON);
        printerTypeComboBox = new wxComboBox(this, wxID_ANY, wxString(""), wxDefaultPosition, wxDefaultSize, 0, nullptr, DD_NO_CHECK_ICON | wxTE_READONLY);

        printerWarningLabel = new wxStaticText(this, wxID_ANY, "Printer type not found, please select manually.");
        printerWarningLabel->SetForegroundColour(*wxRED);
        printerWarningLabel->Hide();

        for (int i = 0; i < cloud_projects.size(); i++) {
            projectsComboBox->Append(cloud_projects[i]);
        }
        if (cloud_printer_types.size() > 0) {
            for (int i = 0; i < cloud_printer_types.size(); i++) {
                printerTypeComboBox->Append(cloud_printer_types[i]);
                if (cloud_printer_types[i].Find(preset_name) != wxNOT_FOUND && printerTypeComboBox->GetSelection() == -1) {
                    printerTypeComboBox->SetSelection(i);
                }
            }
            if (printerTypeComboBox->GetCount() > 1) {
                printerWarningLabel->Show();
            } else {
                printerTypeComboBox->SetSelection(0);
            }
        }

        okButton               = new wxButton(this, wxID_OK, "OK");
        wxButton* cancelButton = new wxButton(this, wxID_CANCEL, "Cancel");

        wxBoxSizer* radioSizer = new wxBoxSizer(wxHORIZONTAL);
        wxBoxSizer* btnSizer   = new wxBoxSizer(wxHORIZONTAL);
        radioSizer->Add(singleRadio, 0, wxALL, 5);
        radioSizer->Add(projectRadio, 0, wxALL, 5);
        btnSizer->Add(okButton, 0, wxALL | wxALIGN_CENTER, 5);
        btnSizer->Add(cancelButton, 0, wxALL | wxALIGN_CENTER, 5);

        wxBoxSizer* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(radioSizer, 0, wxALL, 5);
        sizer->Add(projectsLabel, 0, wxALL, 5);
        sizer->Add(projectsComboBox, 0, wxALL | wxEXPAND, 5);
        sizer->Add(printerLabel, 0, wxALL, 5);
        sizer->Add(printerTypeComboBox, 0, wxALL | wxEXPAND, 5);
        sizer->Add(printerWarningLabel, 0, wxLEFT | wxRIGHT | wxBOTTOM, 5);
        sizer->Add(btnSizer, 0, wxALL | wxALIGN_CENTER, 5);
        SetSizer(sizer);
        sizer->Fit(this);
        projectsComboBox->Hide();
        projectsLabel->Hide();
        projectRadio->Bind(wxEVT_RADIOBUTTON, &UploadOptionsDialog::OnRadioButtonSelected, this);
        singleRadio->Bind(wxEVT_RADIOBUTTON, &UploadOptionsDialog::OnRadioButtonSelected, this);
        // Bind combo box selection change to validation
        printerTypeComboBox->Bind(wxEVT_COMBOBOX, &UploadOptionsDialog::OnPrinterTypeChanged, this);
        ValidateOkButton(); // Initial validation
        Slic3r::GUI::wxGetApp().UpdateDlgDarkUI(this);
        
        CenterOnParent();
    }

    void OnRadioButtonSelected(wxCommandEvent& event)
    {
        wxRadioButton* selectedRadio = dynamic_cast<wxRadioButton*>(event.GetEventObject());
        if (selectedRadio) {
            wxString label = selectedRadio->GetLabel();
            if (label == wxString("Project File")) {
                projectsComboBox->Show();
                projectsLabel->Show();
            } else {
                projectsComboBox->Hide();
                projectsLabel->Hide();
            }
            Layout();
        }
    }

    void on_dpi_changed(const wxRect& suggested_rect) {}

    void OnPrinterTypeChanged(wxCommandEvent& event)
    {
        ValidateOkButton();
        event.Skip();
    }

    void ValidateOkButton()
    {
        bool hasSelection = (printerTypeComboBox->GetSelection() != wxNOT_FOUND);
        okButton->Enable(hasSelection);
    }

    void GetValues(std::string& project, std::string& printer_type)
    {
        project      = projectRadio->GetValue() ? std::string(projectsComboBox->GetValue().c_str()) : "";
        printer_type = std::string(printerTypeComboBox->GetValue().c_str());
    }

private:
    wxComboBox*    projectsComboBox;
    wxComboBox*    printerTypeComboBox;
    wxStaticText*  projectsLabel;
    wxStaticText*  printerWarningLabel;
    wxRadioButton* singleRadio;
    wxRadioButton* projectRadio;
    wxButton*      okButton;
};


class TokenAuthDialog : public Slic3r::GUI::DPIDialog
{
public:
    TokenAuthDialog(wxWindow* parent, const std::string &url, const std::string& token, const std::string &cafile, pt::ptree& resp)
        : Slic3r::GUI::DPIDialog(parent,
                                 wxID_ANY,
                                 "3DPrinterOS",
                                 wxDefaultPosition,
                                 wxSize(45 * Slic3r::GUI::wxGetApp().em_unit(), -1),
                                 wxDEFAULT_DIALOG_STYLE)
        , m_url(url)
        , m_token(token)
        , m_cafile(cafile)
        , m_resp(resp)
    {
        SetFont(Slic3r::GUI::wxGetApp().normal_font());
        SetBackgroundColour(*wxWHITE);
        SetForegroundColour(*wxBLACK);

        auto* sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(this, wxID_ANY, "Authorizing..."), 1, wxALL | wxCENTER, 10);
        auto* cancelBtn = new wxButton(this, wxID_CANCEL, "Cancel");
        sizer->Add(cancelBtn, 0, wxALL | wxALIGN_CENTER, 10);
        SetSizerAndFit(sizer);

        Bind(wxEVT_THREAD, [this](wxThreadEvent& e) { EndModal(e.GetId()); });
        Bind(wxEVT_TIMER, &TokenAuthDialog::OnRetry, this);
        Bind(wxEVT_SHOW, &TokenAuthDialog::OnShow, this);
        Bind(wxEVT_BUTTON, &TokenAuthDialog::OnCancel, this, wxID_CANCEL);

        m_timer.SetOwner(this);
        Slic3r::GUI::wxGetApp().UpdateDlgDarkUI(this);
        CenterOnParent();
    }

    void on_dpi_changed(const wxRect& suggested_rect) {}

private:

    void OnShow(wxShowEvent& event)
    {
        if (event.IsShown() && !m_started) {
            m_started = true;
            SendRequest();
        }
        event.Skip();
    }

    void OnCancel(wxCommandEvent&)
    {
        m_cancelled = true;
        if (m_http_ptr) {
            m_http_ptr->cancel(); // abort the background request
        }
        EndModal(wxID_CANCEL);
    }

    void OnRetry(wxTimerEvent&) { SendRequest(); }

    void SendRequest()
    {
        if (m_cancelled || m_attempt >= m_max_retries) {
            if (m_attempt >= m_max_retries) {
                m_resp.put("result", false);
                m_resp.put("message", "Maximum login retries exceeded");
            }
            wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD, wxID_ABORT));
            return;
        }

        m_attempt++;
        std::string postBody = "token=" + m_token;
        auto http = Slic3r::Http::post(m_url);
        http.timeout_max(60);
        if (!m_cafile.empty()) {
            http.ca_file(m_cafile);
        }
        http.header("Content-Length", std::to_string(postBody.size()));
        http.set_post_body(postBody);
        http.on_error([this](std::string, std::string error, unsigned status) {
                if (!m_cancelled) {
                    m_resp.put("result", false);
                    m_resp.put("message", (status != 200) ? "HTTP error: " + std::to_string(status) : error);
                    wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD, wxID_ABORT));
                }
            })
            .on_complete([this](std::string body, unsigned status) {
                if (!m_cancelled) {
                    if (status != 200) {
                        m_resp.put("result", false);
                        m_resp.put("message", "HTTP error: " + std::to_string(status));
                        wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD, wxID_ABORT));
                        return;
                    }

                    try {
                        std::stringstream ss(body);
                        pt::read_json(ss, m_resp);
                    } catch (...) {
                        m_resp.put("result", false);
                        m_resp.put("message", "Could not parse server response");
                    }

                    if (m_resp.get<bool>("result", false) && m_resp.get_optional<std::string>("message.session").has_value()) {
                        wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD, wxID_OK));
                    } else if (m_resp.get<bool>("result", false)) {
                        if (m_attempt < m_max_retries)
                            m_timer.StartOnce(m_retry_delay_ms);
                        else
                            wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD, wxID_ABORT));
                    } else {
                        wxQueueEvent(this, new wxThreadEvent(wxEVT_THREAD, wxID_ABORT));
                    }
                }
            });
        m_http_ptr = http.perform();
    }

private:
    std::string                    m_token;
    std::string                    m_url;
    std::string                    m_cafile;
    pt::ptree&                     m_resp;
    wxTimer                        m_timer;
    std::shared_ptr<Slic3r::Http>  m_http_ptr;
    bool                           m_cancelled{false};
    bool                           m_started{false};
    int                            m_attempt{0};
    const int                      m_max_retries{10};
    const int                      m_retry_delay_ms{500};
};
} // namespace

namespace Slic3r {

static const std::string API_CREDENTIALS_PATH = "3dprinteros_api_cred.json";

C3DPrinterOS::C3DPrinterOS(DynamicPrintConfig *config)
    : m_host(config->opt_string("print_host"))
    , m_apikey(config->opt_string("printhost_apikey"))
    , m_preset_name(config->opt_string("printer_model"))
{
    m_api_session_file_path = (boost::filesystem::path(Slic3r::data_dir()) / API_CREDENTIALS_PATH)
                                  .make_preferred()
                                  .string();
    load_api_session();
}

const char *C3DPrinterOS::get_name() const { return "3DPrinterOS"; }

bool C3DPrinterOS::test(wxString &msg) const 
{
    return check_session(msg);
}

bool C3DPrinterOS::login(wxString& msg) const 
{
    // Get token for auth
    msg.clear();
    std::string token = get_api_auth_token(msg);
    if (token.empty()) {
        msg = "Error. Can't get api token for authorization";
        return false;
    }

    auto login_url = make_url("noauth/apiglobal_login_with_token/" + token);
    wxLaunchDefaultBrowser(login_url);
    pt::ptree login_resp;
    login_with_token(login_resp, token);
    std::string session, email;
    try {
        if (login_resp.get<bool>("result")) {
            session = login_resp.get<std::string>("message.session");
            email   = login_resp.get<std::string>("message.email");
        } else {
            msg = wxString(login_resp.get<std::string>("message").c_str());
            return false;
        }
    } catch (const std::exception&) {
        msg = "Could not parse server response";
        return false;
    }
    bool res = save_api_session(session, email);
    if (!res) {
        msg = "Error saving session to file";
    }
    return res;
}

wxString C3DPrinterOS::get_test_ok_msg() const 
{
    return _("Connection to 3DPrinterOS cloud works correctly.") + (!m_username.empty() ? "" + _(" Logined as user: ") + m_username : "");
}

wxString C3DPrinterOS::get_test_failed_msg(wxString &msg) const 
{
    return GUI::format_wxstr("%s: %s\n\n", _L("Error session check"), msg);
}

bool C3DPrinterOS::upload(
    PrintHostUpload upload_data, ProgressFn prorgess_fn, ErrorFn error_fn, InfoFn info_fn
) const 
{
    const char *name = get_name();
    const auto upload_filename = upload_data.upload_path.filename();
    const auto upload_parent_path = upload_data.upload_path.parent_path();
    wxString test_msg;
    if (!check_session(test_msg)) {
        error_fn(std::move(test_msg));
        return false;
    }

    pt::ptree cloud_project_resp;
    pt::ptree cloud_printer_types_resp;

    get_cloud_projects_list(cloud_project_resp);
    get_cloud_printer_types(cloud_printer_types_resp, m_preset_name);
    wxArrayString cloud_projects_list;
    wxArrayString cloud_printer_types_list;

    try {
        if (cloud_project_resp.get<bool>("result")) {
            for (const auto &messageItem : cloud_project_resp.get_child("message")) {
                cloud_projects_list.Add(messageItem.second.get<std::string>("name"));
            }
        }

        if (cloud_printer_types_resp.get<bool>("result")) {
            for (const auto &messageItem : cloud_printer_types_resp.get_child("message")) {
                cloud_printer_types_list.Add(messageItem.second.get<std::string>("description"));
            }
        }
    } catch (const std::exception &) {
        error_fn("Could not parse server response");
        return false;
    }
    
    // Show "Confirm cloud printer type and project for 3DPrinterOS upload
    
    UploadOptionsDialog dlg(GUI::wxGetApp().GetTopWindow(), cloud_projects_list, cloud_printer_types_list, m_preset_name);

    if (dlg.ShowModal() != wxID_OK) {
        error_fn("Canceled");
        return false;
    }
    
    std::string selected_project;
    std::string selected_printer_type;
    dlg.GetValues(selected_project, selected_printer_type);
    std::string project_id;
    std::string printer_type_id;

    // search for cloud project_id by name
    if (!selected_project.empty()) {
        for (const auto& messageItem : cloud_project_resp.get_child("message")) {
            if (messageItem.second.get<std::string>("name", "") == selected_project) {
                project_id = messageItem.second.get<std::string>("id", "");
                break;
            }
        }
    }

    // search for cloud printer_type_id by name
    for (const auto& messageItem : cloud_printer_types_resp.get_child("message")) {
        if (messageItem.second.get<std::string>("description", "") == selected_printer_type) {
            printer_type_id = messageItem.second.get<std::string>("id", "");
            break;
        }
    }

    bool res = true;
    auto url = make_url("apiglobal/upload");
    std::string file_id;
    pt::ptree uploadResponse;
    auto http = Http::post(std::move(url));
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
    http.form_add("session", m_apikey)
        .form_add("upload_type_id", "7")
        .form_add("upload_soft_name", "OrcaSlicer")
        .form_add("zip", "false")
        .form_add_file("file", upload_data.source_path.string(), upload_filename.string());

    if (!project_id.empty()) {
        http.form_add("project_id", project_id);
    } else if (!selected_project.empty()) {
        http.form_add("project_name", selected_project);
        http.form_add("project_color", "grey");
    }

    http.on_complete([&](std::string body, unsigned status) {
            std::stringstream ss(body);
            try {
                pt::read_json(ss, uploadResponse);
            } catch (const std::exception &) {
                uploadResponse.put("result", false);
                uploadResponse.put("message", "Could not parse server response");
            }
        })
        .on_error([&](std::string body, std::string error, unsigned status) {
            error_fn(format_error(body, error, status));
            res = false;
        })
        .on_progress([&](Http::Progress progress, bool &cancel) {
            prorgess_fn(std::move(progress), cancel);
            if (cancel) {
                res = false;
            }
        })
        .perform_sync();

    try {
        if (uploadResponse.get<bool>("result")) {
            file_id = uploadResponse.get<std::string>("message.file_id");
        } else {
            res = false;
            error_fn(uploadResponse.get<std::string>("message"));
        }
    } catch (const std::exception &) { 
        res = false;
        error_fn("Error during file upload");
    }
    // set printer type for uploaded gcode
    if (res) {
        pt::ptree update_file_response;
        update_file(update_file_response, file_id, printer_type_id, "OrcaSlicer");
        try {
            if (!update_file_response.get<bool>("result")) {
                const std::string msg = update_file_response.get<std::string>("message", "Unknown update error");
                BOOST_LOG_TRIVIAL(warning) << "Failed to update uploaded file: " << msg;
            }
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "Could not parse update response: " << ex.what();
        }
        if (upload_data.post_action == PrintHostPostUploadAction::StartPrint && !upload_data.use_3mf) {
            auto quick_print_url = make_url("quickprint?file_id=" + file_id);
            wxLaunchDefaultBrowser(quick_print_url);
        }
    }

    return res;
}

void C3DPrinterOS::log_out() const 
{ 
    boost::filesystem::remove(m_api_session_file_path.c_str());
}

bool C3DPrinterOS::validate_version_text(const boost::optional<std::string> &version_text) const 
{
    return version_text ? boost::starts_with(*version_text, "3DPrinterOS") : true;
}

std::string C3DPrinterOS::make_url(const std::string &path) const 
{
    if (m_host.find("http://") == 0 || m_host.find("https://") == 0) {
        if (m_host.back() == '/') {
            return (boost::format("%1%%2%") % m_host % path).str();
        } else {
            return (boost::format("%1%/%2%") % m_host % path).str();
        }
    } else {
        return (boost::format("https://%1%/%2%") % m_host % path).str();
    }
}

std::string C3DPrinterOS::get_api_auth_token(wxString &err) const 
{
    std::string result;
    pt::ptree resp;
    std::string postBody = "app_type=plugin&app_name=" + Http::url_encode("OrcaSlicer");
    send_form("apiglobal/generate_login_token", postBody, resp);
    try {
        if (resp.get<bool>("result")) {
            result = resp.get<std::string>("message");
        } else {
            err = wxString(resp.get<std::string>("message").c_str());
        }
    } catch (const std::exception &) {
        err = "Could not parse server response";
    }
    return result;
}

void C3DPrinterOS::login_with_token(pt::ptree &resp, const std::string &token) const {
    auto url = make_url("apiglobal/login_with_token");
    TokenAuthDialog dlg(GUI::wxGetApp().GetTopWindow(), url, token, m_cafile, resp);
    dlg.ShowModal();
}

bool C3DPrinterOS::check_session(wxString &msg) const {
    std::string postBody = "session=" + m_apikey;
    pt::ptree resp;
    send_form("apiglobal/check_session", postBody, resp);
    try {
        if (resp.get<bool>("result")) {
            return true;
        } else {
            msg = wxString(resp.get<std::string>("message").c_str());
            return false;
        }

    } catch (const std::exception &) {
        msg = wxString("Could not parse server response");
        return false;
    }
    return false;
}

bool C3DPrinterOS::save_api_session(const std::string &session, const std::string &email) const {
    pt::ptree j;
    j.put("session", session);
    j.put("email", email);
    try {
        auto temp_path = m_api_session_file_path + ".tmp";
        pt::write_json(temp_path, j);
        boost::filesystem::rename(temp_path, m_api_session_file_path);
    } catch (const std::exception &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed to write json to file. Path = "
                                 << m_api_session_file_path
                                 << " Reason = " << err.what();
        return false;
    }
    return true;
}

void C3DPrinterOS::load_api_session() 
{
    m_apikey.clear();
    if (boost::filesystem::exists(m_api_session_file_path)) {
        pt::ptree j;
        try {
            pt::read_json(m_api_session_file_path, j);
            m_apikey = j.get<std::string>("session");
            m_username = j.get<std::string>("email");
        } catch (const std::exception &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": load_api_session failed, reason = " << err.what();
            // remove corrupted file to avoid repeated failures
            try {
                boost::filesystem::remove(m_api_session_file_path);
            } catch (...) {}
        }
    };
}

void C3DPrinterOS::send_form(
    const std::string &endpoint,
    const std::string &postBody,
    boost::property_tree::ptree &responseTree
) const 
{
    responseTree.clear();
    auto url = make_url(endpoint);
    auto http = Http::post(std::move(url));
    if (!m_cafile.empty()) {
        http.ca_file(m_cafile);
    }
    http.header("Content-length", std::to_string(postBody.size()));
    http.set_post_body(postBody);
    http.on_error([&](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << boost::format("Error sending form: %1%") % error;
            responseTree.put("result", false);
            responseTree.put("message", error);
        })
        .on_complete([&, this](std::string body, unsigned) {
            std::stringstream ss(body);
            try {
                pt::read_json(ss, responseTree);
            } catch (const std::exception &) {
                responseTree.put("result", false);
                responseTree.put("message", "Could not parse server response");
            }
        })
        .perform_sync();
}

void C3DPrinterOS::get_cloud_projects_list(boost::property_tree::ptree &response) const 
{
    std::string postBody = std::string("session=" + m_apikey);
    send_form("apiglobal/get_projects", postBody, response);
}

void C3DPrinterOS::get_cloud_printer_types(boost::property_tree::ptree &response, const std::string &query) const 
{
    std::string postBody = std::string("session=" + m_apikey);
    if (!query.empty()) {
        postBody += "&description=" + Http::url_encode(query) + "&software_version=" + Http::url_encode("OrcaSlicer");
    }
    send_form("apiglobal/get_printer_types", postBody, response);
}

void C3DPrinterOS::update_file(boost::property_tree::ptree &response, const std::string &file_id, const std::string &ptype, const std::string &gtype) const 
{
    std::string postBody = "session=" + m_apikey 
        + "&updates[" + file_id + "][ptype]=" + ptype
        + "&updates[" + file_id + "][gtype]=" + Http::url_encode(gtype) 
        + "&updates[" + file_id + "][zip]=false";
    send_form("apiglobal/file_update", postBody, response);
}

};
 // namespace Slic3r
