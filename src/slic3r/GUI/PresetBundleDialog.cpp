#include "PresetBundleDialog.hpp"
#include "ExportPresetBundleDialog.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include <libslic3r/Config.hpp>
#include <libslic3r/Thread.hpp>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <libslic3r/PresetBundle.hpp>
#include <wx/string.h>
#include "MainFrame.hpp"
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <miniz.h>
#include <OrcaCloudServiceAgent.hpp>
#include <wx/event.h>
#include <wx/utils.h>
namespace Slic3r { namespace GUI {

PresetBundleDialog::PresetBundleDialog(
    wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style)
    : DPIDialog(parent, id, _L("PresetBundle"), pos, size, style)
{
    wxGetApp().preset_bundle->bundles.PauseRead(); // for the entirety of the preset bundle dialog, we want the update thread to yield.
    SetBackgroundColour(*wxWHITE);
    SetMinSize(DESIGN_WINDOW_SIZE);
    create();
    wxGetApp().UpdateDlgDarkUI(this);

    m_watcher = new wxFileSystemWatcher();
    m_watcher->SetOwner(this);

    Bind(wxEVT_FSWATCHER, &PresetBundleDialog::OnFSWatch, this);
    Bind(EVT_UPDATE_BUNDLE_COMPLETE, &PresetBundleDialog::OnBundleUpdate, this);

    m_watcher->Add(wxFileName(wxGetApp().preset_bundle->dir_user_presets_local.c_str()));      // _local
    m_watcher->Add(wxFileName(wxGetApp().preset_bundle->dir_user_presets_subscribed.c_str())); // _subscribed

    RefreshBundleMap();

    StartDialogWorker(); // start worker thread;
}

PresetBundleDialog::~PresetBundleDialog()
{
    StopDialogWorker();
    wxGetApp().preset_bundle->bundles.UnpauseRead(); // yield for update thread
    if (m_watcher) {
        m_watcher->RemoveAll();
        delete m_watcher;
    }
}

void PresetBundleDialog::OnFSWatch(wxFileSystemWatcherEvent& e)
{
    GUI::wxGetApp().preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::EnableSilentDisableSystem);
    wxGetApp().mainframe->update_side_preset_ui();

    // ListBundles();
    m_update_bundles.store(true);
    e.Skip();
}

void PresetBundleDialog::StartDialogWorker()
{
    if (m_dialog_worker_token) {
        return;
    }

    m_dialog_worker_token = std::make_shared<int>(0);

    m_dialog_worker_thread = Slic3r::create_thread([this, token = std::weak_ptr<int>(m_dialog_worker_token)] {
        while (!token.expired()) {
            // after comparing version with the cloud, if there is an update, we will update the rows to reflect

            if (m_check_update_pending.exchange(false, std::memory_order_relaxed)) {
                if (CheckUpdateCloud()) {
                    ListBundles();
                }
            }
            if (m_update_bundles.exchange(false, std::memory_order_relaxed)) {
                RefreshBundleMap();
                ListBundles();
            }

            boost::this_thread::sleep_for(boost::chrono::milliseconds(500));
        }
    });
}

// true if b>a
bool PresetBundleDialog::CompareVer(const std::string& a, const std::string& b)
{
    // Compare versions using Semver
    auto local_version  = Semver::parse(a);
    auto remote_version = Semver::parse(b);

    if (!local_version || !remote_version) {
        return false;
    }

    if (local_version < remote_version) {
        return true;
    }

    return false;
}

bool PresetBundleDialog::CheckUpdateCloud()
{
    bool has_update = false;
    if (!wxGetApp().getAgent() || !wxGetApp().getAgent()->is_user_login())
        return false;
    auto orca_agent = std::dynamic_pointer_cast<OrcaCloudServiceAgent>(wxGetApp().getAgent()->get_cloud_agent());
    if (!orca_agent)
        return false;

    BOOST_LOG_TRIVIAL(info) << "Preset Bundle Dialog: checking for bundle updates";

    // Fetch all subscribed bundles from cloud
    std::vector<std::pair<std::string, std::string>> subscribed_bundles;
    std::vector<std::string> notfound;
    std::vector<std::string> unauthorized;
    int result = orca_agent->get_subscribed_bundles(&subscribed_bundles, notfound, unauthorized);

    if (result != 0) {
        BOOST_LOG_TRIVIAL(warning) << "Preset Bundle Dialog: failed to fetch subscribed bundles, result=" << result;
        return false;
    }

    // if unauthorized or not found it should be a warning icon to the ui shit

    // check bundle copy with the subscribed bundles
    for (auto& b : subscribed_bundles) {
        if (bundle_copy.find(b.first) != bundle_copy.end()) {
            if (CompareVer(bundle_copy[b.first].version, b.second)) {
                bundle_copy[b.first].update_available = true;
                bundle_copy[b.first].unauthorized     = false;
                has_update                            = true;

            } else {
                // we count it as an update to the UI if we need to update the unauthorized state
                if (bundle_copy[b.first].unauthorized) {
                    bundle_copy[b.first].unauthorized = false;
                    has_update                        = true;
                }
                bundle_copy[b.first].update_available = false;
            }
        }
    }

    for (auto& a : unauthorized) {
        if (bundle_copy.find(a) != bundle_copy.end()) {
            bundle_copy[a].unauthorized = true;
            has_update                  = true;
        }
    }
    return has_update;
}

void PresetBundleDialog::StopDialogWorker()
{
    if (!m_dialog_worker_token) {
        return;
    }

    m_dialog_worker_token.reset();

    if (m_dialog_worker_thread.joinable()) {
        m_dialog_worker_thread.join();
    }
}

void PresetBundleDialog::OnBundleUpdate(wxCommandEvent& evt)
{
    // const std::string bundle_id = evt.GetString().ToStdString();
    m_update_bundles.store(true);
}

void PresetBundleDialog::RefreshBundleMap()
{
    wxGetApp().preset_bundle->bundles.ReadLock();
    bundle_copy = wxGetApp().preset_bundle->bundles.m_bundles;
    wxGetApp().preset_bundle->bundles.ReadUnlock();
}

void PresetBundleDialog::load_url(wxString& url)
{
    if (!m_browser)
        return;
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " enter, url=" << url.ToStdString();
    WebView::LoadUrl(m_browser, url);
    m_browser->SetFocus();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " exit";
}

void PresetBundleDialog::create()
{
    app_config = get_app_config();

    wxString TargetUrl = from_u8(
        (boost::filesystem::path(resources_dir()) / "web/dialog/PresetBundleDialog/index.html").make_preferred().string());
    wxString strlang = wxGetApp().current_language_code_safe();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", strlang=%1%") % into_u8(strlang);
    if (strlang != "")
        TargetUrl = wxString::Format("%s?lang=%s", std::string(TargetUrl.mb_str()), strlang);

    TargetUrl = "file://" + TargetUrl;

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    SetTitle(_L("Preset Bundle"));

    m_browser = WebView::CreateWebView(this, TargetUrl);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    SetSizer(topsizer);
    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Set a more sensible size for web browsing
    wxSize pSize = FromDIP(wxSize(820, 660));
    SetSize(pSize);

    int screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
    int screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
    int MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
    wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
    Move(tmpPT);

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PresetBundleDialog::OnScriptMessage, this, m_browser->GetId());

    load_url(TargetUrl);
}

bool PresetBundleDialog::DeleteBundleById(const wxString& id)
{
    auto* b = wxGetApp().preset_bundle;
    if (id.empty()) {
        return false;
    }

    const std::string bundle_id = id.ToStdString();

    wxGetApp().preset_bundle->bundles.ReadLock();
    auto it = b->bundles.m_bundles.find(bundle_id);
    if (it == b->bundles.m_bundles.end()) {
        wxGetApp().preset_bundle->bundles.ReadUnlock();
        return false;
    }

    const std::string metadata_path          = it->second.path;
    const boost::filesystem::path bundle_dir = boost::filesystem::path(metadata_path).parent_path();

    const BundleType bundle_type = it->second.bundle_type;
    wxGetApp().preset_bundle->bundles.ReadUnlock();

    if (bundle_type == BundleType::Subscribed) {
        // do unsubscribe before deleting locally
    }

    wxGetApp().preset_bundle->bundles.WriteLock();
    b->bundles.m_bundles.erase(it);
    wxGetApp().preset_bundle->bundles.WriteUnlock();

    auto remove_from_collection = [&](PresetCollection& c) {
        std::vector<std::string> to_delete;
        for (const auto& p : c.get_presets()) {
            if (p.bundle_id == bundle_id)
                to_delete.push_back(p.name);
        }
        for (const auto& name : to_delete)
            c.delete_preset(name);
    };

    remove_from_collection(b->prints);
    remove_from_collection(b->filaments);
    remove_from_collection(b->printers);

    boost::system::error_code ec;
    if (!bundle_dir.empty() && boost::filesystem::exists(bundle_dir))
        boost::filesystem::remove_all(bundle_dir, ec);

    wxGetApp().preset_bundle->update_compatible(PresetSelectCompatibleType::Always);

    return true;
}

bool PresetBundleDialog::UnsubscribeBundleById(const std::string& id) { return wxGetApp().unsubscribe_bundle(id); }

void PresetBundleDialog::on_dpi_changed(const wxRect& suggested_rect) { this->Refresh(); }

void PresetBundleDialog::RunScript(const wxString& s)
{
    if (!m_browser)
        return;

    WebView::RunScript(m_browser, s);
}

void PresetBundleDialog::OnScriptMessage(wxWebViewEvent& e)
{
    try {
        wxString strInput = e.GetString();
        BOOST_LOG_TRIVIAL(trace) << "PresetBundleDialog::OnScriptMessage;OnRecv:" << strInput.c_str();
        json j = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];
        BOOST_LOG_TRIVIAL(trace) << "PresetBundleDialog::OnScriptMessage;Command:" << strCmd;

        if (strCmd == "request_bundles") {
            ListBundles();
        } else if (strCmd == "refresh_bundles") {
            // use the thread to check for updates.
            m_check_update_pending.store(true, std::memory_order_relaxed);
        } else if (strCmd == "update_bundle") {
            std::string id = j["bundle_id"];

            auto* evt = new wxCommandEvent(EVT_UPDATE_PRESET_BUNDLE);
            evt->SetString(wxString::FromUTF8(id));
            wxQueueEvent(&wxGetApp(), evt); // dialog -> GUI_App
        } else if (strCmd == "set_auto_update") {
            bool enabled = j.value("enabled", false);

            // Example persistence location. Adjust key name if you already have one.
            app_config->set_bool("preset_bundle_auto_update", enabled ? true : false);
            app_config->save();
        } else if (strCmd == "close_page") {
            this->EndModal(wxID_CANCEL);
        } else if (strCmd == "export_page") {
            wxGetApp().CallAfter([this]() {
                ExportPresetBundleDialog dlg(this);
                dlg.ShowModal();
            });
        } else if (strCmd == "top_row_menu_action") {
            if (j["action"] == "open_folder") {
                std::string id = j["bundle_id"];
                OpenFolder(id);
            } else if (j["action"] == "delete_bundle") {
                std::string id = j["bundle_id"];
                DeleteBundle(id);
            } else if (j["action"] == "unsubscribe_bundle") {
                std::string id = j["bundle_id"];
                UnsubscribeBundle(id);
            }
        } else if (strCmd == "open_bundle_on_cloud") {
            std::string bundle_id = j["bundle_id"];
            OpenBundleOnCloud(bundle_id);
        }

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << "PresetBundleDialog::OnScriptMessage;Error:" << e.what();
    }
}

// call on dialog create to populate the js local store
void PresetBundleDialog::ListBundles()
{
    json res;
    res["command"]     = "list_bundles";
    res["sequence_id"] = "2000";
    res["data"]        = json::array();

    const auto& all_bundles = bundle_copy;

    auto strip_prefix = [](const std::vector<std::string>& names) {
        json arr = json::array();
        for (const auto& name : names)
            arr.push_back(boost::filesystem::path(name).filename().string());
        return arr;
    };

    for (const auto& bundle : all_bundles) {
        const auto& metadata = bundle.second;
        json temp;
        temp["id"]          = metadata.id;
        temp["name"]        = metadata.name;
        temp["type"]        = metadata.bundle_type == Subscribed ? "Subscribed" : metadata.bundle_type == Local ? "Local" : "Default";
        temp["author"]      = metadata.author;
        temp["version"]     = metadata.version;
        temp["description"] = metadata.description;
        temp["path"]        = metadata.path;

        temp["printers"]  = strip_prefix(metadata.printer_presets);
        temp["filaments"] = strip_prefix(metadata.filament_presets);
        temp["presets"]   = strip_prefix(metadata.print_presets);

        temp["update_available"]   = metadata.update_available;
        temp["unauthorized"]       = metadata.unauthorized;
        res["auto_update_enabled"] = app_config->get_bool("preset_bundle_auto_update");

        res["data"].push_back(std::move(temp));
    }

    wxString strJS = wxString::Format("HandleStudio(%s)", wxString::FromUTF8(res.dump(-1, ' ', false, json::error_handler_t::ignore)));
    wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
}

void PresetBundleDialog::OpenFolder(const std::string& id)
{
    wxGetApp().preset_bundle->bundles.ReadLock();
    wxString target = _L(wxGetApp().preset_bundle->bundles.m_bundles.find(id)->second.path);
    wxGetApp().preset_bundle->bundles.ReadUnlock();
    wxFileName fn(target);
    if (fn.FileExists())
        target = fn.GetPath();

    if (target.empty() || !wxFileName::DirExists(target)) {
        wxMessageBox(_L("Bundle folder does not exist."), _L("Open Folder"), wxOK | wxICON_WARNING, this);
        return;
    }

    if (!wxLaunchDefaultApplication(target)) {
        wxMessageBox(_L("Failed to open folder."), _L("Open Folder"), wxOK | wxICON_ERROR, this);
    }
}

void PresetBundleDialog::DeleteBundle(const std::string& id)
{
    if (id.empty())
        return;

    const int rc = wxMessageBox(_L("Delete selected bundle from folder and all presets loaded from it?"), _L("Delete Bundle"),
                                wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);

    if (rc != wxYES)
        return;

    if (!DeleteBundleById(id)) {
        wxMessageBox(_L("Failed to remove bundle."), _L("Remove Bundle"), wxOK | wxICON_ERROR, this);
        return;
    }
    wxGetApp().mainframe->update_side_preset_ui();
}

void PresetBundleDialog::UnsubscribeBundle(const std::string& id)
{
    if (id.empty())
        return;

    const int rc = wxMessageBox(_L("Unsubscribe bundle?"), _L("UnsubscribeBundle"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);

    if (rc != wxYES)
        return;

    if (!UnsubscribeBundleById(id)) {
        wxMessageBox(_L("Failed to unsubscribe bundle."), _L("Unsubscribe Bundle"), wxOK | wxICON_ERROR, this);
        return;
    }

    wxGetApp().mainframe->update_side_preset_ui();
}

void PresetBundleDialog::OpenBundleOnCloud(const std::string& id)
{
    if (id.empty())
        return;

    if (!wxGetApp().getAgent())
        return;

    auto orca_agent = std::dynamic_pointer_cast<OrcaCloudServiceAgent>(wxGetApp().getAgent()->get_cloud_agent());
    if (!orca_agent)
        return;

    wxLaunchDefaultBrowser(wxString::FromUTF8(orca_agent->get_bundle_url(id)));
}
}} // namespace Slic3r::GUI
