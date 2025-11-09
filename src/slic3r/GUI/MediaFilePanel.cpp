#include "MediaFilePanel.h"
#include "ImageGrid.h"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/SwitchButton.hpp"
#include "Widgets/Label.hpp"
#include "Printer/PrinterFileSystem.h"
#include "MsgDialog.hpp"
#include "Widgets/ProgressDialog.hpp"
#include <libslic3r/Model.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include "DeviceCore/DevStorage.h"

#ifdef __WXMSW__
#include <shellapi.h>
#endif
#ifdef __APPLE__
#include "../Utils/MacDarkMode.hpp"
#endif

namespace Slic3r {
namespace GUI {

MediaFilePanel::MediaFilePanel(wxWindow * parent)
    : wxPanel(parent, wxID_ANY)
    , m_bmp_loading(this, "media_loading", 0)
    , m_bmp_failed(this, "media_failed", 0)
    , m_bmp_empty(this, "media_empty", 0)
    , m_machine("<null>")
{
    SetBackgroundColour(0xEEEEEE);
    Hide();

    wxBoxSizer * sizer = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer * top_sizer = new wxBoxSizer(wxHORIZONTAL);
    top_sizer->SetMinSize({-1, 75 * em_unit(this) / 10});

    // Time group
    auto time_panel = new wxWindow(this, wxID_ANY);
    time_panel->SetBackgroundColour(0xEEEEEE);
    m_time_panel = new ::StaticBox(time_panel, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_time_panel->SetCornerRadius(0);
    m_button_year  = new ::Button(m_time_panel, _L("Year"), "", wxBORDER_NONE);
    m_button_month = new ::Button(m_time_panel, _L("Month"), "", wxBORDER_NONE);
    m_button_all = new ::Button(m_time_panel, _L("All Files"), "", wxBORDER_NONE);
    m_button_year->SetToolTip(_L("Group files by year, recent first."));
    m_button_month->SetToolTip(_L("Group files by month, recent first."));
    m_button_all->SetToolTip(_L("Show all files, recent first."));
    m_button_all->SetFont(Label::Head_14); // sync with m_last_mode
    for (auto b : {m_button_year, m_button_month, m_button_all}) {
        b->SetBackgroundColor(StateColor());
        b->SetTextColor(StateColor(
            std::make_pair(0x3B4446, (int) StateColor::Checked),
            std::make_pair(*wxLIGHT_GREY, (int) StateColor::Hovered),
            std::make_pair(0xABACAC, (int) StateColor::Normal)
        ));
    }

    wxBoxSizer *time_sizer = new wxBoxSizer(wxHORIZONTAL);
    time_sizer->Add(m_button_year, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24);
    time_sizer->Add(m_button_month, 0, wxALIGN_CENTER_VERTICAL);
    time_sizer->Add(m_button_all, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24);
    m_time_panel->SetSizer(time_sizer);
    wxBoxSizer *time_sizer2 = new wxBoxSizer(wxHORIZONTAL);
    time_sizer2->Add(m_time_panel, 1, wxEXPAND);
    time_panel->SetSizer(time_sizer2);
    top_sizer->Add(time_panel, 1, wxEXPAND);

    // File type
    StateColor background(
        std::make_pair(0xEEEEEE, (int) StateColor::Checked),
        std::make_pair(*wxLIGHT_GREY, (int) StateColor::Hovered),
        std::make_pair(*wxWHITE, (int) StateColor::Normal));
    m_type_panel = new ::StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_type_panel->SetBackgroundColor(*wxWHITE);
    m_type_panel->SetCornerRadius(FromDIP(5));
    m_type_panel->SetMinSize({-1, 48 * em_unit(this) / 10});
    m_button_timelapse = new ::Button(m_type_panel, _L("Timelapse"), "", wxBORDER_NONE);
    m_button_timelapse->SetToolTip(_L("Switch to timelapse files."));
    m_button_video = new ::Button(m_type_panel, _L("Video"), "", wxBORDER_NONE);
    m_button_video->SetToolTip(_L("Switch to video files."));
    m_button_model = new ::Button(m_type_panel, _L("Model"), "", wxBORDER_NONE);
    m_button_video->SetToolTip(_L("Switch to 3MF model files."));
    for (auto b : {m_button_timelapse, m_button_video, m_button_model}) {
        b->SetBackgroundColor(background);
        b->SetCanFocus(false);
    }

    wxBoxSizer *type_sizer = new wxBoxSizer(wxHORIZONTAL);
    type_sizer->Add(m_button_timelapse, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24);
    //type_sizer->Add(m_button_video, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 24);
    m_button_video->Hide();
    type_sizer->Add(m_button_model, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 24);
    m_type_panel->SetSizer(type_sizer);
    top_sizer->Add(m_type_panel, 0, wxALIGN_CENTER_VERTICAL);

    // File management
    m_manage_panel      = new ::StaticBox(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE);
    m_manage_panel->SetBackgroundColor(StateColor());
    m_button_delete     = new ::Button(m_manage_panel, _L("Delete"));
    m_button_delete->SetToolTip(_L("Delete selected files from printer."));
    m_button_download = new ::Button(m_manage_panel, _L("Download"));
    m_button_download->SetToolTip(_L("Download selected files from printer."));
    m_button_management = new ::Button(m_manage_panel, _L("Select"));
    m_button_management->SetToolTip(_L("Batch manage files."));
    m_button_refresh = new ::Button(m_manage_panel, _L("Refresh"));
    m_button_refresh->SetToolTip(_L("Reload file list from printer."));
    for (auto b : {m_button_delete, m_button_download, m_button_refresh, m_button_management}) {
        b->SetFont(Label::Body_12);
        b->SetCornerRadius(12);
        b->SetPaddingSize({10, 6});
        b->SetCanFocus(false);
    }
    m_button_delete->SetBorderColorNormal(wxColor("#FF6F00"));
    m_button_delete->SetTextColorNormal(wxColor("#FF6F00"));
    m_button_management->SetBorderWidth(0);
    m_button_management->SetBackgroundColorNormal(wxColor("#009688"));
    m_button_management->SetTextColorNormal(*wxWHITE);
    m_button_management->Enable(false);
    m_button_refresh->SetBorderWidth(0);
    m_button_refresh->SetBackgroundColorNormal(wxColor("#009688"));
    m_button_refresh->SetTextColorNormal(*wxWHITE);
    m_button_refresh->Enable(false);

    wxBoxSizer *manage_sizer = new wxBoxSizer(wxHORIZONTAL);
    manage_sizer->AddStretchSpacer(1);
    manage_sizer->Add(m_button_download, 0, wxALIGN_CENTER_VERTICAL)->Show(false);
    manage_sizer->Add(m_button_delete, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24)->Show(false);
    manage_sizer->Add(m_button_refresh, 0, wxALIGN_CENTER_VERTICAL);
    manage_sizer->Add(m_button_management, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, 24);
    m_manage_panel->SetSizer(manage_sizer);
    top_sizer->Add(m_manage_panel, 1, wxEXPAND);

    sizer->Add(top_sizer, 0, wxEXPAND);

    m_image_grid = new ImageGrid(this);
    m_image_grid->Bind(EVT_ITEM_ACTION, [this](wxCommandEvent &e) { doAction(size_t(e.GetExtraLong()), e.GetInt()); });
    m_image_grid->SetStatus(m_bmp_failed, _L("No printers."));
    sizer->Add(m_image_grid, 1, wxEXPAND);

    SetSizer(sizer);

    // Time group
    auto time_button_clicked = [this](wxEvent &e) {
        auto mode = PrinterFileSystem::G_NONE;
        if (e.GetEventObject() == m_button_year)
            mode = PrinterFileSystem::G_YEAR;
        else if (e.GetEventObject() == m_button_month)
            mode = PrinterFileSystem::G_MONTH;
        m_image_grid->SetGroupMode(mode);
    };
    m_button_year->Bind(wxEVT_COMMAND_BUTTON_CLICKED, time_button_clicked);
    m_button_month->Bind(wxEVT_COMMAND_BUTTON_CLICKED, time_button_clicked);
    m_button_all->Bind(wxEVT_COMMAND_BUTTON_CLICKED, time_button_clicked);
    m_button_all->SetValue(true);

    // File type
    auto type_button_clicked = [this](wxEvent &e) {
        Button *buttons[]{m_button_timelapse, m_button_video, m_button_model};
        auto    type = std::find(buttons, buttons + sizeof(buttons) / sizeof(buttons[0]), e.GetEventObject()) - buttons;
        if (m_last_type == type)
            return;
        m_image_grid->SetFileType(type, m_external ? "" : "internal");
        buttons[m_last_type]->SetValue(!buttons[m_last_type]->GetValue());
        m_last_type = type;
        buttons[m_last_type]->SetValue(!buttons[m_last_type]->GetValue());
        if (type == PrinterFileSystem::F_MODEL)
            m_image_grid->SetGroupMode(PrinterFileSystem::G_NONE);
    };
    m_button_timelapse->Bind(wxEVT_COMMAND_BUTTON_CLICKED, type_button_clicked);
    m_button_video->Bind(wxEVT_COMMAND_BUTTON_CLICKED, type_button_clicked);
    m_button_model->Bind(wxEVT_COMMAND_BUTTON_CLICKED, type_button_clicked);
    m_button_timelapse->SetValue(true);

    // File management
    m_button_management->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](auto &e) {
        e.Skip();
        SetSelecting(!m_image_grid->IsSelecting());
    });
    m_button_refresh->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](auto &e) {
        e.Skip();
        if (auto fs = m_image_grid->GetFileSystem())
            fs->ListAllFiles();
    });
    m_button_download->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](auto &e) {
        m_image_grid->DoActionOnSelection(1);
        SetSelecting(false);
    });
    m_button_delete->Bind(wxEVT_COMMAND_BUTTON_CLICKED, [this](auto &e) {
        m_image_grid->DoActionOnSelection(0);
        SetSelecting(false);
    });

    auto onShowHide = [this](auto &e) {
        e.Skip();
        if (auto w = dynamic_cast<wxWindow *>(e.GetEventObject()); !w || w->IsBeingDeleted())
            return;
        CallAfter([this] {
            auto fs = m_image_grid ? m_image_grid->GetFileSystem() : nullptr;
            if (fs) IsShownOnScreen() ? fs->Start() : fs->Stop();
        });
    };
    Bind(wxEVT_SHOW, onShowHide);
    parent->GetParent()->Bind(wxEVT_SHOW, onShowHide);

    m_lan_user = "bblp";
}

MediaFilePanel::~MediaFilePanel()
{
    UpdateByObj(nullptr);
}

void MediaFilePanel::UpdateByObj(MachineObject* obj)
{
    bool sdcard_state_changed = false;
    std::string machine = obj ? obj->get_dev_id() : "";
    if (obj) {
        m_lan_mode     = obj->is_lan_mode_printer();
        m_lan_ip       = obj->get_dev_ip();
        m_lan_passwd   = obj->get_access_code();
        m_dev_ver      = obj->get_ota_version();
        m_device_busy  = obj->is_camera_busy_off();
        m_local_proto  = obj->file_local;
        m_remote_proto = obj->get_file_remote();
        m_model_download_support = obj->file_model_download;

        if (m_sdcard_exist != (obj->GetStorage()->get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL)) {
            m_sdcard_exist = obj->GetStorage()->get_sdcard_state() == DevStorage::HAS_SDCARD_NORMAL;
            sdcard_state_changed = true;
        }
    } else {
        m_lan_mode  = false;
        m_lan_ip.clear();
        m_lan_passwd.clear();
        m_dev_ver.clear();
        m_device_busy = false;
        m_local_proto = 0;
        m_remote_proto = 0;
        m_model_download_support = false;

        if (m_sdcard_exist) {
            m_sdcard_exist = false; // reset sdcard state when no object
            sdcard_state_changed = true;
        }
    }

    Enable(obj && obj->is_info_ready() && obj->m_push_count > 0);
    if (machine == m_machine && !sdcard_state_changed) {
        if ((m_waiting_enable && IsEnabled()) || (m_waiting_support && (m_local_proto || m_remote_proto))) {
            auto fs = m_image_grid->GetFileSystem();
            if (fs) fs->Retry();
        }
        return;
    }
    m_machine.swap(machine);
    m_last_errors.clear();
    auto fs = m_image_grid->GetFileSystem();
    if (fs) {
        m_image_grid->SetFileSystem(nullptr);
        fs->Unbind(EVT_MODE_CHANGED, &MediaFilePanel::modeChanged, this);
        fs->Stop(true);
    }
    m_button_refresh->Enable(false);
    m_button_management->Enable(false);
    SetSelecting(false);
    if (m_machine.empty()) {
        m_image_grid->SetStatus(m_bmp_failed, _L("Please confirm if the printer is connected."));
    } else {
        boost::shared_ptr<PrinterFileSystem> fs(new PrinterFileSystem);
        fs->Attached();
        m_image_grid->SetFileSystem(fs);
        m_image_grid->SetFileType(m_last_type, m_external ? "" : "internal");
        fs->Bind(EVT_FILE_CHANGED, [this, wfs = boost::weak_ptr(fs)](auto &e) {
            e.Skip();
            boost::shared_ptr fs(wfs.lock());
            if (fs == nullptr || m_image_grid->GetFileSystem() != fs) // canceled
                return;
            m_time_panel->Show(fs->GetFileType() < PrinterFileSystem::F_MODEL);
            //m_manage_panel->Show(fs->GetFileType() < PrinterFileSystem::F_MODEL);
            m_button_refresh->Enable(fs->GetStatus() == PrinterFileSystem::ListReady);
            m_button_management->Enable(fs->GetCount() > 0);
            bool download_support = fs->GetFileType() < PrinterFileSystem::F_MODEL || m_model_download_support;
            m_image_grid->ShowDownload(download_support);
            if (fs->GetCount() == 0)
                SetSelecting(false);
        });
        fs->Bind(EVT_SELECT_CHANGED, [this, wfs = boost::weak_ptr(fs)](auto &e) {
            e.Skip();
            boost::shared_ptr fs(wfs.lock());
            if (fs == nullptr || m_image_grid->GetFileSystem() != fs) // canceled
                return;
            m_button_delete->Enable(e.GetInt() > 0);
            m_button_download->Enable(e.GetInt() > 0);
        });
        fs->Bind(EVT_MODE_CHANGED, &MediaFilePanel::modeChanged, this);
        fs->Bind(EVT_STATUS_CHANGED, [this, wfs = boost::weak_ptr(fs)](auto& e) {
            e.Skip();
            boost::shared_ptr fs(wfs.lock());
            if (fs == nullptr || m_image_grid->GetFileSystem() != fs) // canceled
                return;
            ScalableBitmap icon;
            wxString msg;
            int status = e.GetInt();
            int extra = e.GetExtraLong();
            switch (status) {
            case PrinterFileSystem::Initializing: icon = m_bmp_loading; msg = _L("Initializing..."); break;
            case PrinterFileSystem::Connecting: icon = m_bmp_loading; msg = _L("Connecting..."); break;
            case PrinterFileSystem::Failed: icon = m_bmp_failed; if (extra != 1) msg = _L("Please check the network and try again. You can restart or update the printer if the issue persists."); break;
            case PrinterFileSystem::ListSyncing: {
                icon = m_bmp_loading;
                msg  = _L("Loading file list...");
                fs->ListAllFiles();
                break;
            }
            case PrinterFileSystem::ListReady: icon = extra == 0 ? m_bmp_empty : m_bmp_failed; msg = extra == 0 ? _L("No files") : _L("Load failed"); break;
            }
            int err = fs->GetLastError();
            if (!e.GetString().IsEmpty())
                msg = e.GetString();
            if (err != 0)
                msg += " [%d]";
            if (fs->GetCount() == 0 && !msg.empty())
                m_image_grid->SetStatus(icon, msg);
            if (e.GetInt() == PrinterFileSystem::Initializing)
                fetchUrl(boost::weak_ptr(fs));

            if ((status == PrinterFileSystem::Failed && m_last_errors.find(err) == m_last_errors.end()) ||
                status == PrinterFileSystem::ListReady) {
                m_last_errors.insert(fs->GetLastError());
            }
        });
        fs->Bind(EVT_DOWNLOAD, [this, wfs = boost::weak_ptr(fs)](auto& e) {
            e.Skip();
            boost::shared_ptr fs(wfs.lock());
            if (fs == nullptr || m_image_grid->GetFileSystem() != fs) // canceled
                return;

            int result = e.GetExtraLong();
            if (result > 1 && !e.GetString().IsEmpty())
                CallAfter([this, m = e.GetString()] {
                    MessageDialog(this, m, _L("Download failed"), wxOK | wxICON_ERROR).ShowModal();
                });

            NetworkAgent* agent = wxGetApp().getAgent();
            if (result > 1 || result == 0) {
                json j;
                j["code"] = result;
                j["dev_id"] = m_machine;
                j["dev_ip"] = m_lan_ip;
                if (result > 1) {
                    // download failed
                    j["result"] = "failed";
                } else if (result == 0) {
                    // download success
                    j["result"] = "success";
                }
            }
            return;
        });
        if (IsShown()) fs->Start();
    }
    wxCommandEvent e(EVT_MODE_CHANGED);
    modeChanged(e);
}

void MediaFilePanel::SwitchStorage(bool external)
{
    if (m_external == external)
        return;
    m_external = external;
    m_type_panel->Show(external);
    if (!external) {
        Button *buttons[]{m_button_timelapse, m_button_video, m_button_model};
        auto button = buttons[PrinterFileSystem::F_MODEL];
        wxCommandEvent event(wxEVT_COMMAND_BUTTON_CLICKED, button->GetId());
        event.SetEventObject(button);
        wxPostEvent(button, event);
    }
    m_image_grid->SetFileType(m_last_type, m_external ? "" : "internal");
}

void MediaFilePanel::Rescale()
{
    m_bmp_loading.msw_rescale();
    m_bmp_failed.msw_rescale();
    m_bmp_empty.msw_rescale();

    auto top_sizer = GetSizer()->GetItem((size_t) 0)->GetSizer();
    top_sizer->SetMinSize({-1, 75 * em_unit(this) / 10});
    m_button_year->Rescale();
    m_button_month->Rescale();
    m_button_all->Rescale();

    m_button_video->Rescale();
    m_button_timelapse->Rescale();
    m_button_model->Rescale();
    m_type_panel->SetMinSize({-1, 48 * em_unit(this) / 10});

    m_button_download->Rescale();
    m_button_delete->Rescale();
    m_button_refresh->Rescale();
    m_button_management->Rescale();

    m_image_grid->Rescale();
}

void MediaFilePanel::SetSelecting(bool selecting)
{
    m_image_grid->SetSelecting(selecting);
    m_button_management->SetLabel(selecting ? _L("Cancel") : _L("Select"));
    auto fs = m_image_grid->GetFileSystem();
    bool download_support = (fs && fs->GetFileType() < PrinterFileSystem::F_MODEL) || m_model_download_support;
    m_manage_panel->GetSizer()->Show(m_button_download, selecting && download_support);
    m_manage_panel->GetSizer()->Show(m_button_delete, selecting);
    m_manage_panel->GetSizer()->Show(m_button_refresh, !selecting);
    m_manage_panel->Layout();
}

void MediaFilePanel::modeChanged(wxCommandEvent& e1)
{
    e1.Skip();
    auto fs = m_image_grid->GetFileSystem();
    auto mode = fs ? fs->GetGroupMode() : 0;
    if (m_last_mode == mode)
        return;
    ::Button* buttons[] = {m_button_all, m_button_month, m_button_year};
    auto b = buttons[m_last_mode];
    b->SetFont(Label::Body_14);
    b->SetValue(false);
    b = buttons[mode];
    b->SetFont(Label::Head_14);
    b->SetValue(true);
    m_last_mode = mode;
}

extern wxString hide_passwd(wxString url, std::vector<wxString> const &passwords);
extern void refresh_agora_url(char const *device, char const *dev_ver, char const *channel, void *context, void (*callback)(void *context, char const *url));

void MediaFilePanel::fetchUrl(boost::weak_ptr<PrinterFileSystem> wfs)
{
    boost::shared_ptr fs(wfs.lock());
    if (!fs || fs != m_image_grid->GetFileSystem()) return;
    if (!IsEnabled()) {
        m_waiting_enable = true;
        m_image_grid->SetStatus(m_bmp_failed, _L("Please confirm if the printer is connected."));
        fs->SetUrl("0");
        return;
    }
    m_waiting_enable = false;
    if (!m_local_proto && !m_remote_proto) {
        m_waiting_support = true;
        m_image_grid->SetStatus(m_bmp_failed, _L("Browsing file in storage is not supported in current firmware. Please update the printer firmware."));
        fs->SetUrl("0");
        return;
    }
    if (m_device_busy) {
        m_image_grid->SetStatus(m_bmp_failed, _L("The printer is currently busy downloading. Please try again after it finishes."));
        fs->SetUrl("0");
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "MediaFilePanel::fetchUrl: " << m_local_proto << m_remote_proto;
    m_waiting_support = false;
    NetworkAgent *agent = wxGetApp().getAgent();
    std::string  agent_version = agent ? agent->get_version() : "";
    if ((m_lan_mode || !m_remote_proto) && m_local_proto && !m_lan_ip.empty()) {
        std::string url = "bambu:///local/" + m_lan_ip + ".?port=6000&user=" + m_lan_user + "&passwd=" + m_lan_passwd;
        url += "&device=" + m_machine;
        url += "&net_ver=" + agent_version;
        url += "&dev_ver=" + m_dev_ver;
        url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid");
        url += "&cli_ver=" + std::string(SLIC3R_VERSION);
        fs->SetUrl(url);
        return;
    }
    if (!m_remote_proto && m_local_proto) { // not support tutk
        m_image_grid->SetStatus(m_bmp_failed, _L("Please enter the IP of printer to connect."));
        fs->SetUrl("0");
        fs.reset();
        if (wxGetApp().show_modal_ip_address_enter_dialog(false, _L("LAN Connection Failed (Failed to view sdcard)"))) {
            if (auto fs = wfs.lock())
                fs->Retry();
        }
        return;
    }
    if (m_lan_mode) {
        m_image_grid->SetStatus(m_bmp_failed, _L("Browsing file in storage is not supported in LAN Only Mode."));
        fs->SetUrl("0");
        return;
    }
    if (agent) {
        std::string protocols[] = {"", "\"tutk\"", "\"agora\"", "\"tutk\",\"agora\""};
        agent->get_camera_url(m_machine + "|" + m_dev_ver + "|" + protocols[m_remote_proto],
            [this, wfs, m = m_machine, v = agent->get_version(), dv = m_dev_ver](std::string url) {
            if (boost::algorithm::starts_with(url, "bambu:///")) {
                url += "&device=" + m;
                url += "&net_ver=" + v;
                url += "&dev_ver=" + dv;
                url += "&refresh_url=" + boost::lexical_cast<std::string>(&refresh_agora_url);
                url += "&cli_id=" + wxGetApp().app_config->get("slicer_uuid");
                url += "&cli_ver=" + std::string(SLIC3R_VERSION);
            }
            BOOST_LOG_TRIVIAL(info) << "MediaFilePanel::fetchUrl: camera_url: " << hide_passwd(url, {"?uid=", "authkey=", "passwd="});
            CallAfter([=] {
                boost::shared_ptr fs(wfs.lock());
                if (!fs || fs != m_image_grid->GetFileSystem()) return;
                if (boost::algorithm::starts_with(url, "bambu:///")) {
                    fs->SetUrl(url);
                } else {
                    m_image_grid->SetStatus(m_bmp_failed, _L("Connection Failed. Please check the network and try again"));
                    std::string res = "3";
                    if (boost::ends_with(url, "]")) {
                        size_t n = url.find_last_of('[');
                        if (n != std::string::npos)
                            res = url.substr(n + 1, url.length() - n - 2);
                    }
                    fs->SetUrl(res);
                }
            });
        });
    }
}

struct MediaProgressDialog : ProgressDialog
{
    MediaProgressDialog(wxString title, wxWindow * parent, std::function<void()> cancel)
        : ProgressDialog(title, "", 100, parent, wxPD_NO_PROGRESS | wxPD_APP_MODAL | wxPD_CAN_ABORT)
        , m_cancel(cancel) {}
    void OnCancel() override{m_cancel(); }
    std::function<void()> m_cancel;
};

void MediaFilePanel::doAction(size_t index, int action)
{
    auto fs = m_image_grid->GetFileSystem();
    if (action == 0) {
        if (index == -1) {
            MessageDialog dlg(this,
                wxString::Format(_L_PLURAL("You are going to delete %u file from printer. Are you sure to continue?",
                                                         "You are going to delete %u files from printer. Are you sure to continue?", fs->GetSelectCount()),
                                               fs->GetSelectCount()),
                _L("Delete files"), wxYES_NO | wxICON_WARNING);
            if (dlg.ShowModal() != wxID_YES)
                return;
        } else {
            MessageDialog dlg(this,
                wxString::Format(_L("Do you want to delete the file '%s' from printer?"), from_u8(fs->GetFile(index).name)),
                _L("Delete file"), wxYES_NO | wxICON_WARNING);
            if (dlg.ShowModal() != wxID_YES)
                return;
        }
        fs->DeleteFiles(index);
    } else if (action == 1) {
        if (fs->GetFileType() == PrinterFileSystem::F_MODEL) {
            if (index != -1) {
                auto dlg = new MediaProgressDialog(_L("Print"), this, [fs] { fs->FetchModelCancel(); });
                dlg->Update(0, _L("Fetching model information..."));
                fs->FetchModel(index, [this, fs, dlg, index](int result, std::string const &data) {
                    dlg->Destroy();
                    if (result == PrinterFileSystem::ERROR_CANCEL)
                        return;
                    if (result != 0) {
                        wxString msg = data.empty() ? _L("Failed to fetch model information from printer.") :
                                                      from_u8(data);
                        CallAfter([this, msg] {
                            MessageDialog(this, msg, _L("Print"), wxOK).ShowModal();
                        });
                        return;
                    }
                    Slic3r::DynamicPrintConfig config;
                    Slic3r::Model              model;
                    Slic3r::PlateDataPtrs      plate_data_list;
                    Slic3r::Semver file_version;
                    std::istringstream is(data);
                    if (!Slic3r::load_gcode_3mf_from_stream(is, &config, &model, &plate_data_list, &file_version)
                            || plate_data_list.empty()) {
                        MessageDialog(this,
                            _L("Failed to parse model information."),
                            _L("Print"), wxOK).ShowModal();
                        return;
                    }


                    auto &file = fs->GetFile(index);

                    std::string file_path = file.path;
                    if (!file_path.empty() && file_path[0] == '/') {
                        file_path.erase(0, 1);
                    }

                    int gcode_file_count = Slic3r::GUI::wxGetApp().plater()->update_print_required_data(config, model, plate_data_list, file.name, file_path);

                    if (gcode_file_count > 0) {
                        wxPostEvent(Slic3r::GUI::wxGetApp().plater(), SimpleEvent(EVT_PRINT_FROM_SDCARD_VIEW));
                    }
                    else {
                        MessageDialog dlg(this, _L("The .gcode.3mf file contains no G-code data. Please slice it with Orca Slicer and export a new .gcode.3mf file."),
                            wxEmptyString, wxICON_WARNING | wxOK);
                        auto res = dlg.ShowModal();
                    }

                });
                return;
            }
        }
        if (index != -1) {
            auto &file = fs->GetFile(index);
            if (file.IsDownload() && file.DownloadProgress() >= -1) {
                if (!file.local_path.empty()) {
                    if (!fs->DownloadCheckFile(index)) {
                        MessageDialog(this,
                            wxString::Format(_L("File '%s' was lost! Please download it again."), from_u8(file.name)),
                            _L("Error"), wxOK).ShowModal();
                        Refresh();
                        return;
                    }
#ifdef __WXMSW__
                    auto             wfile = boost::filesystem::path(file.local_path).wstring();
                    SHELLEXECUTEINFO info{sizeof(info), 0, NULL, NULL, wfile.c_str(), L"", SW_HIDE};
                    ::ShellExecuteEx(&info);
#else
                    wxShell("open " + file.local_path);
#endif
                } else {
                    fs->DownloadCancel(index);
                }
                return;
            }
        }
        fs->DownloadFiles(index, wxGetApp().app_config->get("download_path"));
    } else if (action == 2) {
        if (index != -1) {
            auto &file = fs->GetFile(index);
            if (file.IsDownload() && file.DownloadProgress() >= -1) {
                if (!file.local_path.empty()) {
                    if (!fs->DownloadCheckFile(index)) {
                        MessageDialog(this,
                            wxString::Format(_L("File '%s' was lost! Please download it again."), from_u8(file.name)),
                            _L("Error"), wxOK).ShowModal();
                        Refresh();
                        return;
                    }
                    desktop_open_any_folder(file.local_path);
                } else if (fs->GetFileType() == PrinterFileSystem::F_MODEL) {
                    fs->DownloadCancel(index);
                }
                return;
            }
        }
        fs->DownloadFiles(index, wxGetApp().app_config->get("download_path"));
    }
}

MediaFileFrame::MediaFileFrame(wxWindow* parent)
    : DPIFrame(parent, wxID_ANY, "Media Files", wxDefaultPosition, { 1600, 900 })
{
    m_panel = new MediaFilePanel(this);
    wxBoxSizer * sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_panel, 1, wxEXPAND);
    SetSizer(sizer);

    Bind(wxEVT_CLOSE_WINDOW, [this](auto & e){
        Hide();
        e.Veto();
    });
}

void MediaFileFrame::on_dpi_changed(const wxRect& suggested_rect) { m_panel->Rescale(); Refresh(); }

}}
