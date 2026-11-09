#include "MultiMachineCameraPage.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "LanMonitorManager.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "wxMediaCtrl3.h"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/BambuCameraSource.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <utility>
#include <wx/colour.h>
#include <wx/event.h>
#include <wx/gdicmn.h>
#include <wx/mediactrl.h>
#include <wx/panel.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/uri.h>
#include <wx/timer.h>

namespace Slic3r::GUI {

class LanCameraTile : public wxPanel
{
public:
    explicit LanCameraTile(wxWindow* parent, std::string id) : wxPanel(parent), m_id(std::move(id))
    {
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        m_name = new Label(this, Label::Head_14, {}, wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_viewport = new wxPanel(this);
        m_viewport->SetBackgroundColour(*wxBLACK);
        m_video_sizer = new wxBoxSizer(wxVERTICAL);
        m_viewport->SetSizer(m_video_sizer);
        m_job = new Label(this, Label::Body_13, _L("Unknown"), wxST_ELLIPSIZE_END | wxST_NO_AUTORESIZE);
        m_name->SetMinSize(wxSize(1, -1));
        m_job->SetMinSize(wxSize(1, -1));
        // A text update must not resize siblings and re-clip a video already showing frames.
        m_status = new Label(this, Label::Body_13, _L("Connecting"), wxST_NO_AUTORESIZE);
        m_progress = new Label(this, Label::Body_13, _L("N/A"), wxST_NO_AUTORESIZE);
        m_camera_status = new Label(this, Label::Body_12, _L("Connecting to camera"), wxST_NO_AUTORESIZE);
        m_retry = new Button(this, _L("Retry"));
        m_retry->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
        m_retry->Bind(wxEVT_BUTTON, [this](wxCommandEvent& event) {
            event.Skip();
            if (wxGetApp().get_lan_monitor()) wxGetApp().get_lan_monitor()->retry(m_id);
            stop();
            m_next_retry = {};
        });
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        footer->Add(m_camera_status, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        footer->Add(m_retry, 0, wxALIGN_CENTER_VERTICAL);
        auto* sizer = new wxBoxSizer(wxVERTICAL);
        const int gap = FromDIP(10);
        sizer->Add(m_name, 0, wxEXPAND | wxALL, gap);
        sizer->Add(m_viewport, 0, wxEXPAND);
        sizer->Add(m_job, 0, wxEXPAND | wxALL, gap);
        sizer->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT, gap);
        sizer->Add(m_progress, 0, wxEXPAND | wxALL, gap);
        sizer->Add(footer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
        SetSizer(sizer);
        rescale();
    }

    ~LanCameraTile() override { stop(); }

    void update(MachineObject& machine, bool visible)
    {
        m_name->SetLabel(from_u8(machine.get_dev_name()));
        const auto* monitor = wxGetApp().get_lan_monitor();
        const auto* status = monitor ? monitor->status(m_id) : nullptr;
        m_job->SetLabel(status && status->live() && !status->job.job_name.empty() ? from_u8(status->job.job_name) : _L("Unknown"));
        m_status->SetLabel(lan_monitor_status_text(status));
        m_progress->SetLabel(lan_monitor_progress_text(status));
        if (!visible) { stop(); return; }
        BambuCameraSource source;
        source.host = machine.get_dev_ip();
        source.password = machine.get_access_code();
        source.device = m_id;
        source.device_version = machine.get_ota_version();
        source.network_version = NetworkAgent::get_version();
        source.client_id = wxGetApp().app_config->get("slicer_uuid");
        source.client_version = SLIC3R_VERSION;
        if (!source.same_connection(m_source)) { stop(); m_next_retry = {}; }
        m_source = std::move(source);
        if (!m_player && Clock::now() >= m_next_retry) {
            if (!NetworkAgent::get_bambu_source_entry()) {
                m_camera_status->SetLabel(_L("Camera unavailable. Retrying…"));
                m_next_retry = Clock::now() + std::chrono::seconds(10);
                return;
            }
            m_player = new wxMediaCtrl3(m_viewport);
            m_player->SetIdleImage(from_u8(resources_dir() + "/images/live_stream_default.png"));
            m_video_sizer->Add(m_player, 1, wxEXPAND);
            m_viewport->Layout();
            m_camera_status->SetLabel(_L("Connecting to camera"));
            m_player->Load(wxURI(from_u8(m_source.url())));
            m_started = Clock::now();
        }
        if (m_player) {
            // SDK log diagnostics may be nonzero while frames continue (e.g. P1 LAN code 3).
            // Retry only when the decoder/transport exits, or initialization times out.
            if (m_player->GetPlaybackError() != 0 ||
                (!m_player->HasVideoFrame() && Clock::now() - m_started > std::chrono::seconds(30))) {
                stop();
                m_camera_status->SetLabel(_L("Camera unavailable. Retrying…"));
                m_next_retry = Clock::now() + std::chrono::seconds(10);
            } else if (m_player->HasVideoFrame()) {
                m_camera_status->SetLabel(_L("Live"));
            }
        }
    }

    void stop()
    {
        if (!m_player) return;
        m_video_sizer->Detach(m_player);
        // Destroy joins the decoder and closes this tile's tunnel before module reload.
        m_player->Destroy();
        m_player = nullptr;
    }

    void resize_video(int width)
    {
        const int height = std::max(1, width * 9 / 16);
        if (m_viewport->GetMinSize() != wxSize(1, height)) {
            m_viewport->SetMinSize(wxSize(1, height));
            Layout();
        }
    }

    void rescale()
    {
        m_retry->Rescale();
        on_sys_color_changed();
    }

    void on_sys_color_changed()
    {
        SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#FFFFFF")));
        for (auto* label : {m_name, m_job, m_status, m_progress, m_camera_status})
            label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3C")));
        wxGetApp().UpdateDarkUI(m_retry);
        Refresh();
    }

private:
    using Clock = std::chrono::steady_clock;
    std::string m_id;
    BambuCameraSource m_source;
    wxPanel* m_viewport;
    wxBoxSizer* m_video_sizer;
    wxMediaCtrl3* m_player = nullptr;
    Label* m_name;
    Label* m_job;
    Label* m_status;
    Label* m_progress;
    Label* m_camera_status;
    Button* m_retry;
    Clock::time_point m_started{};
    Clock::time_point m_next_retry{};
};

MultiMachineCameraPage::MultiMachineCameraPage(wxWindow* parent) : wxPanel(parent), m_timer(this)
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F5F5F5")));
    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetBackgroundColour(GetBackgroundColour());
    m_scroll->SetScrollRate(0, FromDIP(20));
    m_grid = new wxGridSizer(1, FromDIP(16), FromDIP(16));
    auto* content = new wxBoxSizer(wxVERTICAL);
    m_empty = new Label(m_scroll, _L("Select paired P1S LAN printers on the Device page to view their cameras."), LB_AUTO_WRAP);
    content->Add(m_empty, 0, wxEXPAND | wxALL, FromDIP(16));
    content->Add(m_grid, 0, wxEXPAND | wxALL, FromDIP(16));
    m_scroll->SetSizer(content);
    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_scroll, 1, wxEXPAND);
    SetSizer(sizer);
    Bind(wxEVT_TIMER, [this](wxTimerEvent&) { refresh(); });
    m_scroll->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { event.Skip(); layout_grid(); });
}

MultiMachineCameraPage::~MultiMachineCameraPage()
{
    m_timer.Stop();
    for (auto& [id, tile] : m_tiles) tile->stop();
}

bool MultiMachineCameraPage::Show(bool show)
{
    m_visible = show;
    const bool result = wxPanel::Show(show);
    if (show && !m_suspended) {
        // Let the notebook finish assigning page geometry before starting any camera.
        Layout();
        CallAfter([this] { refresh(); });
        m_timer.Start(1000);
    }
    else { m_timer.Stop(); for (auto& [id, tile] : m_tiles) tile->stop(); }
    return result;
}

void MultiMachineCameraPage::suspend()
{
    m_suspended = true;
    m_timer.Stop();
    for (auto& [id, tile] : m_tiles) tile->stop();
}

void MultiMachineCameraPage::resume()
{
    m_suspended = false;
    if (m_visible) { refresh(); m_timer.Start(1000); }
}

void MultiMachineCameraPage::refresh()
{
    if (m_suspended || !m_visible || !IsShownOnScreen() || wxGetApp().is_closing()) return;
    auto* devices = wxGetApp().getDeviceManager();
    if (!devices) return;
    const auto machines = devices->get_multi_machine_list();
    std::set<std::string> wanted;
    bool layout_changed = false;
    for (unsigned i = 0; i < 6; ++i) {
        const auto id = wxGetApp().app_config->get("multi_devices", std::to_string(i));
        const auto found = machines.find(id);
        if (found == machines.end() || !found->second->supports_lan_monitoring() || !wanted.insert(id).second) continue;
        auto& tile = m_tiles[id];
        if (!tile) {
            tile = new LanCameraTile(m_scroll, id);
            m_grid->Add(tile, 0, wxEXPAND);
            layout_changed = true;
        }
    }
    for (auto it = m_tiles.begin(); it != m_tiles.end();) {
        if (wanted.count(it->first)) { ++it; continue; }
        m_grid->Detach(it->second);
        it->second->Destroy();
        it = m_tiles.erase(it);
        layout_changed = true;
    }
    layout_changed |= m_empty->Show(m_tiles.empty());
    if (layout_changed) layout_grid();
    // Use final tile geometry, after removing the empty-state label and updating scrollbars.
    const wxRect viewport(wxPoint(0, 0), m_scroll->GetClientSize());
    for (auto& [id, tile] : m_tiles)
        tile->update(*machines.at(id), tile->GetRect().Intersects(viewport));
}

void MultiMachineCameraPage::layout_grid()
{
    const int width = std::max(1, m_scroll->GetClientSize().x - FromDIP(32));
    const int columns = width >= FromDIP(320 * 2 + 16) ? 2 : 1;
    m_grid->SetCols(columns);
    for (auto& [id, tile] : m_tiles) tile->resize_video((width - FromDIP(16) * (columns - 1)) / columns);
    m_scroll->Layout();
    m_scroll->FitInside();
}

void MultiMachineCameraPage::rescale()
{
    m_scroll->SetScrollRate(0, FromDIP(20));
    m_grid->SetHGap(FromDIP(16));
    m_grid->SetVGap(FromDIP(16));
    for (auto& [id, tile] : m_tiles) tile->rescale();
    layout_grid();
}

void MultiMachineCameraPage::on_sys_color_changed()
{
    SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F5F5F5")));
    m_scroll->SetBackgroundColour(GetBackgroundColour());
    m_empty->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#323A3C")));
    for (auto& [id, tile] : m_tiles) tile->on_sys_color_changed();
    Refresh();
}

} // namespace Slic3r::GUI
