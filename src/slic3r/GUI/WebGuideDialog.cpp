#include "WebGuideDialog.hpp"
#include "CloudProvider.hpp"
#include "ConfigWizard.hpp"

#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/bind/bind.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iostreams/detail/select.hpp>
#include <boost/log/trivial.hpp>
#include <map>
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_Utils.hpp"
#include <memory>
#include <exception>
#include "slic3r/GUI/Event.hpp"
#include "slic3r/GUI/Plater.hpp"
#include <set>
#include <ostream>
#include <iterator>
#include "slic3r/GUI/ParamsDialog.hpp"
#include <ios>
#include <stdexcept>
#include <sstream>
#include <string.h>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Operations/InstallOps.hpp"
#include "slic3r/GUI/Operations/Outcome.hpp"
#include "slic3r/GUI/Operations/PresetOps.hpp"
#include <nlohmann/json.hpp>
#include "libslic3r_version.h"

#include <string>
#include <vector>
#include <utility>
#include <wx/gdicmn.h>
#include <wx/log.h>
#include <wx/settings.h>
#include <wx/event.h>
#include <wx/setup.h>
#include <unordered_set>
#include <system_error>
#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/strconv.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/webview.h>
#include <wx/utils.h>
#include <wx/wx.h>
#include <wx/weakref.h>
#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <unordered_map>

#include "MainFrame.hpp"
#include "Plater.hpp"
#include <boost/dll.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/GUI/Widgets/WebViewHostDialog.hpp>
#include <libslic3r/Utils.hpp>
#include "CreatePresetsDialog.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/PresetUpdater.hpp"

class wxWindow;

using namespace nlohmann;

namespace Slic3r { namespace GUI {

// The strings of every setup wizard page (resources/web/guide), keyed by their data-i18n and T() ids.
// The pages share one web view, so they share one table.
static json guide_strings()
{
    return {
        {"loading", _u8L("Loading…")},
        // TRN Sidebar entry of the printer selection page for the group of custom (user-defined) printers.
        {"custom_vendor", _u8L_CONTEXT("Custom", "Printer vendor")},
        {"welcome", _u8L("Welcome to Orca Slicer")},
        {"setup_intro", _u8L("Orca Slicer will be set up in several steps. Let's start!")},
        {"get_started", _u8L("Get Started")},
        {"login_region", _u8L("Please select your login region")},
        {"asia_pacific", _u8L("Asia-Pacific")},
        {"china", _u8L("China")},
        {"europe", _u8L("Europe")},
        {"north_america", _u8L("North America")},
        {"others", _u8L_CONTEXT("Others", "Region")},
        {"back", _u8L_CONTEXT("Back", "Navigation")},
        {"next", _u8L("Next")},
        {"finish", _u8L("Finish")},
        {"confirm", _u8L("Confirm")},
        {"cancel", _u8L("Cancel")},
        {"close", _u8L("Close")},
        {"ok", _u8L("OK")},
        {"yes", _u8L("Yes")},
        {"no", _u8L("No")},
        {"error", _u8L("Error")},
        {"information", _u8L("Information")},
        {"printer_selection", _u8L("Printer Selection")},
        {"vendors", _u8L("Vendors")},
        {"search", _u8L("Search")},
        {"custom_printer", _u8L("Custom Printer")},
        {"nozzle", _u8L("Nozzle")},
        {"create", _u8L("Create")},
        {"check_new_printers", _u8L("Check for new printers")},
        {"printer_required", _u8L("At least one printer must be selected.")},
        {"new_printers_found", _u8L("New printers found")},
        {"new_vendors_installed", _u8L("New printer vendors installed: %s")},
        {"vendors_declined", _u8L("New printer vendors were found, but installation was cancelled.")},
        {"no_new_printers", _u8L("No new printers found.")},
        {"filament_selection", _u8L("Filament Selection")},
        {"system_filaments", _u8L("System Filaments")},
        {"custom_filaments", _u8L("Custom Filaments")},
        {"create_new", _u8L("Create New")},
        {"printer", _u8L("Printer")},
        {"filament_type", _u8L("Filament type")},
        {"vendor", _u8L("Vendor")},
        {"all", _u8L("All")},
        {"no_items", _u8L("No items")},
        {"filter_items", _u8L("Filter items")},
        // TRN An item of the "Filter by state" menu beside Filter items: no filter on the checked state.
        {"state_all", _u8L_CONTEXT("All", "Filter by state")},
        {"checked", _u8L("Checked")},
        {"unchecked", _u8L("Unchecked")},
        {"select_filtered", _u8L("Select filtered")},
        {"select_visible", _u8L("Select visible")},
        {"clear_all", _u8L("Clear all")},
        {"filament_required", _u8L("At least one filament must be selected.")},
        {"use_default_filament", _u8L("Do you want to use the default filament?")},
        {"stealth_mode", _u8L("Stealth Mode")},
        {"stealth_mode_desc", _u8L("This disables all cloud features, including Orca Cloud profile syncing. Users who prefer to work entirely offline can enable this option.")},
        {"stealth_mode_note", _u8L("Note: When Stealth Mode is enabled, your user profiles will not be backed up to Orca Cloud.")},
        {"enable_stealth_mode", _u8L("Enable Stealth Mode.")},
        {"proprietary_plugins", _u8L("Proprietary Plugins")},
        {"proprietary_plugins_desc", _u8L("Some printer vendors require proprietary plugins for communication with their printers. Please select the corresponding plugin if you use such printers.")},
        {"proprietary_plugins_risk", _u8L("Please be aware that these plugins are not developed or maintained by OrcaSlicer. They should be used at your own discretion and risk.")},
        {"install_bambu_plugin", _u8L("Install Bambu Network plug-in")},
        // Screen reader names for controls and sections that show no text of their own.
        {"jump_to_vendor", _u8L("Jump to vendor")},
        {"view_compact_list", _u8L("Compact list view")},
        {"view_compact_cover", _u8L("Compact cover view")},
        {"view_large_cover", _u8L("Large cover view")},
        // TRN %s is a printer vendor name.
        {"select_all_printers", _u8L("Select all %s printers")},
        // TRN %s is a printer vendor name. Used while a search hides some of the vendor's printers.
        {"select_all_shown_printers", _u8L("Select all shown %s printers")},
        // TRN Screen reader text for a "3 / 12" count: the number selected, then the total, of the printers or filaments
        // it sits beside. The numbers lead; reword freely if your language needs a noun or agreement.
        {"count_selected", _u8L("%s of %s selected")},
        // TRN Screen reader text spoken after a search or filter: how many printers or filaments it leaves.
        // This form is used only for a count of 1; "%s results" covers every other count.
        {"count_result_one", _u8L("%s result")},
        // TRN Screen reader text spoken after a search or filter: how many printers or filaments it leaves.
        // Used for every count except 1. If your language inflects the noun by number, choose a wording
        // that reads correctly with any number.
        {"count_results", _u8L("%s results")},
        // TRN Screen reader text spoken after a search in the filament step's columns: how many entries match, of
        // the column's total. These "_one" forms are used only when the total is 1; the plural forms cover every other
        // total. If your language inflects the noun by number, choose a wording that reads correctly with any number.
        {"count_of_printers_one", _u8L("%s of %s printer")},
        {"count_of_printers", _u8L("%s of %s printers")},
        {"count_of_filament_types_one", _u8L("%s of %s filament type")},
        {"count_of_filament_types", _u8L("%s of %s filament types")},
        {"count_of_vendors_one", _u8L("%s of %s vendor")},
        {"count_of_vendors", _u8L("%s of %s vendors")},
        {"printers", _u8L("Printers")},
        {"filament_types", _u8L("Filament types")},
        {"filaments", _u8L("Filaments")},
        {"filter_by_state", _u8L("Filter by state")},
        // TRN Name of the edit button beside a custom filament. %s is the filament name.
        {"edit_filament_named", _u8L("Edit filament %s")},
    };
}

static wxString update_custom_filaments()
{
    json m_Res                                                                     = json::object();
    m_Res["command"]                                                               = "update_custom_filaments";
    m_Res["sequence_id"]                                                           = "2000";
    json                                               m_CustomFilaments           = json::array();
    PresetBundle *                                     preset_bundle               = wxGetApp().preset_bundle;
    std::map<std::string, std::vector<Preset const *>> temp_filament_id_to_presets = preset_bundle->filaments.get_filament_presets();

    std::vector<std::pair<std::string, std::string>>   need_sort;
    bool                                             need_delete_some_filament = false;
    for (std::pair<std::string, std::vector<Preset const *>> filament_id_to_presets : temp_filament_id_to_presets) {
        std::string filament_id = filament_id_to_presets.first;
        if (filament_id.empty()) continue;
        if (filament_id == "null") {
            need_delete_some_filament = true;
        }
        bool filament_with_base_id = false;
        bool not_need_show = false;
        std::string filament_name;
        for (const Preset *preset : filament_id_to_presets.second) {
            if (preset->is_system || preset->is_project_embedded) {
                not_need_show = true;
                break;
            }
            if (preset->inherits() != "") continue;
            if (!preset->base_id.empty()) filament_with_base_id = true;

            if (!not_need_show) {
                auto filament_vendor = dynamic_cast<ConfigOptionStrings *>(const_cast<Preset *>(preset)->config.option("filament_vendor", false));
                if (filament_vendor && filament_vendor->values.size() && filament_vendor->values[0] == "Generic") not_need_show = true;
            }

            if (filament_name.empty()) {
                std::string preset_name = preset->name;
                size_t      index_at    = preset_name.find(" @");
                if (std::string::npos != index_at) { preset_name = preset_name.substr(0, index_at); }
                filament_name = preset_name;
            }
        }
        if (not_need_show) continue;
        if (!filament_name.empty()) {
            if (filament_with_base_id) {
                need_sort.push_back(std::make_pair(into_u8(_L("[Action Required] ")) + filament_name, filament_id));
            } else {

                need_sort.push_back(std::make_pair(filament_name, filament_id));
            }
        }
    }
    std::sort(need_sort.begin(), need_sort.end(), [](const std::pair<std::string, std::string> &a, const std::pair<std::string, std::string> &b) { return a.first < b.first; });
    if (need_delete_some_filament) {
        need_sort.push_back(std::make_pair(into_u8(_L("[Action Required]")), "null"));
    }
    json temp_j;
    for (std::pair<std::string, std::string> &filament_name_to_id : need_sort) {
        temp_j["name"] = filament_name_to_id.first;
        temp_j["id"]   = filament_name_to_id.second;
        m_CustomFilaments.push_back(temp_j);
    }
    m_Res["data"]  = m_CustomFilaments;
    wxString strJS = wxString::Format("HandleStudio(%s)", wxString::FromUTF8(m_Res.dump(-1, ' ', false, json::error_handler_t::ignore)));
    return strJS;
}

GuideFrame::GuideFrame(GUI_App *pGUI, long style)
    : DPIDialog((wxWindow *) (pGUI->mainframe), wxID_ANY, "OrcaSlicer", wxDefaultPosition, wxDefaultSize, style),
	m_appconfig_new()
{
    SetBackgroundColour(*wxWHITE);
    // INI
    m_SectionName = "firstguide";
    m_page        = BBL_WELCOME;
    PrivacyUse    = false;
    StealthMode   = false;
    InstallNetplugin = false;

    m_MainPtr = pGUI;

    // set the frame icon
    wxBoxSizer *topsizer = new wxBoxSizer(wxVERTICAL);

    // Created empty and loaded by SetStartPage(BBL_REGION) below, after the strings are added, so the
    // first load already has them.
    m_browser = WebView::CreateWebView(this, wxEmptyString);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    m_browser->AddUserScript(wxString::FromUTF8(WebViewHostDialog::localized_strings_script(guide_strings())));
    m_browser->Hide();
    m_browser->SetSize(0, 0);

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    // Log backend information
    // wxLogMessage(wxWebView::GetBackendVersionInfo().ToString());
    // wxLogMessage("Backend: %s Version: %s",
    // m_browser->GetClassInfo()->GetClassName(),wxWebView::GetBackendVersionInfo().ToString());
    // wxLogMessage("User Agent: %s", m_browser->GetUserAgent());

    // Set a more sensible size for web browsing
    wxSize pSize = FromDIP(wxSize(820, 660));
    SetSize(pSize);

    int screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
    int screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
    int MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
    wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
    Move(tmpPT);
#ifdef __WXMSW__
    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if ((m_page == BBL_FILAMENT_ONLY || m_page == BBL_MODELS_ONLY) && e.GetKeyCode() == WXK_ESCAPE) {
            if (this->IsModal())
                this->EndModal(wxID_CANCEL);
            else
                this->Close();
        }
        else
            e.Skip();
        });
#endif
    // Connect the webview events
    Bind(wxEVT_WEBVIEW_NAVIGATING, &GuideFrame::OnNavigationRequest, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATED, &GuideFrame::OnNavigationComplete, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_LOADED, &GuideFrame::OnDocumentLoaded, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_ERROR, &GuideFrame::OnError, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NEWWINDOW, &GuideFrame::OnNewWindow, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_TITLE_CHANGED, &GuideFrame::OnTitleChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_FULLSCREEN_CHANGED, &GuideFrame::OnFullScreenChanged, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &GuideFrame::OnScriptMessage, this, m_browser->GetId());

    // Connect the idle events
    // Bind(wxEVT_IDLE, &GuideFrame::OnIdle, this);
    // Bind(wxEVT_CLOSE_WINDOW, &GuideFrame::OnClose, this);

    // UI
    SetStartPage(BBL_REGION);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  finished");
    wxGetApp().UpdateDlgDarkUI(this);
}

GuideFrame::~GuideFrame()
{
    *m_cancel_token = true; // stop the loading thread and any queued CallAfter lambdas before join
    if (m_load_task && m_load_task->joinable())
        m_load_task->join();
    m_load_task.reset();
    if (m_browser) {
        delete m_browser;
        m_browser = nullptr;
    }
}

void GuideFrame::load_url(wxString &url)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__<< " enter, url=" << url.ToStdString();
    WebView::LoadUrl(m_browser, url);
    m_browser->SetFocus();
    UpdateState();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< " exit";
}

wxString GuideFrame::SetStartPage(GuidePage startpage, bool load)
{
    m_page = startpage;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(" enter, load=%1%, start_page=%2%")%load%int(startpage);
    //wxLogMessage("GUIDE: webpage_1  %s", (boost::filesystem::path(resources_dir()) / "web\\guide\\1\\index.html").make_preferred().string().c_str() );
    const wxString guide_url = file_url_from_path(boost::filesystem::path(resources_dir()) / "web/guide/0/index.html");
    wxString TargetUrl = guide_url + "?target=1";
    //wxLogMessage("GUIDE: webpage_2  %s", TargetUrl.mb_str());

    if (startpage == BBL_WELCOME){
        SetTitle(_L("Setup Wizard"));
        TargetUrl = guide_url + "?target=1";
    } else if (startpage == BBL_REGION) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = guide_url + "?target=11";
    } else if (startpage == BBL_MODELS) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = guide_url + "?target=21";
    } else if (startpage == BBL_FILAMENTS) {
        SetTitle(_L("Setup Wizard"));

        int nSize = m_ProfileJson["model"].size();

        if (nSize>0)
            TargetUrl = guide_url + "?target=22";
        else
            TargetUrl = guide_url + "?target=21";
    } else if (startpage == BBL_FILAMENT_ONLY) {
        // ORCA: these two pages open as standalone dialogs, not as the wizard, so an
        // empty title left them announced as a nameless "dialog" by screen readers.
        SetTitle(_L("Select Filaments"));
        TargetUrl = guide_url + "?target=23";
    } else if (startpage == BBL_MODELS_ONLY) {
        SetTitle(_L("Select Printers"));
        TargetUrl = guide_url + "?target=24";
    }
    else {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = guide_url + "?target=21";
    }

    wxString strlang = wxGetApp().current_language_code_safe();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< boost::format(", strlang=%1%") % into_u8(strlang);
    if (strlang != "")
        TargetUrl = wxString::Format("%s&lang=%s", w2s(TargetUrl), strlang);

    if (load)
        load_url(TargetUrl);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__<< " exit";
    return TargetUrl;
}

/**
 * Method that retrieves the current state from the web control and updates
 * the GUI the reflect this current state.
 */
void GuideFrame::UpdateState()
{
    // SetTitle(m_browser->GetCurrentTitle());
}

void GuideFrame::OnIdle(wxIdleEvent &WXUNUSED(evt))
{
    if (m_browser->IsBusy()) {
        wxSetCursor(wxCURSOR_ARROWWAIT);
    } else {
        wxSetCursor(wxNullCursor);
    }
}

// void GuideFrame::OnClose(wxCloseEvent& evt)
//{
//    this->Hide();
//}

/**
 * Callback invoked when there is a request to load a new page (for instance
 * when the user clicks a link)
 */
void GuideFrame::OnNavigationRequest(wxWebViewEvent &evt)
{
    // wxLogMessage("%s", "Navigation request to '" + evt.GetURL() + "'
    // (target='" + evt.GetTarget() + "')");

    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
// The empty shape every profile-loading path starts from or falls back to.
void GuideFrame::reset_profile_json()
{
    m_ProfileJson["model"]    = json::array();
    m_ProfileJson["machine"]  = json::object();
    m_ProfileJson["filament"] = json::object();
    m_ProfileJson["process"]  = json::array();
}

void GuideFrame::init_guide_paths()
{
    m_ProfileJson = json::parse("{}");
    reset_profile_json();

    vendor_dir      = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    rsrc_vendor_dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();
    orca_bundle_rsrc = true;

    if (boost::filesystem::exists(vendor_dir)) {
        for (const auto& entry : boost::filesystem::directory_iterator(vendor_dir)) {
            if (!boost::filesystem::is_directory(entry) &&
                boost::iequals(entry.path().extension().string(), ".json") &&
                !boost::iequals(entry.path().stem().string(), PresetBundle::ORCA_FILAMENT_LIBRARY)) {
                orca_bundle_rsrc = false;
                break;
            }
        }
    }

    auto lib_json = boost::filesystem::path(PresetBundle::ORCA_FILAMENT_LIBRARY).replace_extension(".json");
    m_OrcaFilaLibPath = boost::filesystem::exists(vendor_dir / lib_json)
        ? (vendor_dir      / PresetBundle::ORCA_FILAMENT_LIBRARY).string()
        : (rsrc_vendor_dir / PresetBundle::ORCA_FILAMENT_LIBRARY).string();
}

void GuideFrame::on_profile_loaded()
{
    // Must be called on the main thread.
    SaveProfileData();
    const std::string strAll = m_ProfileJson.dump(-1, ' ', false, json::error_handler_t::ignore);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ", finished, json contents:\n" << strAll;
    m_profile_ready = true;
    notify_profile_loaded();
}

// The data is often ready before the loading page has run its scripts, and a
// call into a page without HandleStudio is lost, so the message waits for both.
void GuideFrame::notify_profile_loaded()
{
    if (!m_profile_ready || !m_page_ready)
        return;
    json res;
    res["command"]     = "userguide_profile_load_finish";
    res["sequence_id"] = "10001";
    RunScript(wxString::Format("HandleStudio(%s)", res.dump(-1, ' ', true)));
}

void GuideFrame::OnNavigationComplete(wxWebViewEvent &evt)
{
    //wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    if (!bFirstComplete) {
        bFirstComplete = true;
        try {
            init_guide_paths();
            if (BuildProfileDataFromPresetBundle()) {
                if (!*m_cancel_token)
                    on_profile_loaded();
            } else {
                // Presets not yet in memory — delegate to background thread.
                m_load_task = std::make_unique<boost::thread>(boost::bind(&GuideFrame::LoadProfileData, this));
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", init error: " << e.what();
            m_load_task = std::make_unique<boost::thread>(boost::bind(&GuideFrame::LoadProfileData, this));
        }
    }

    m_browser->Show();
    Layout();

    wxString NewUrl = evt.GetURL();

    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void GuideFrame::OnDocumentLoaded(wxWebViewEvent &evt)
{
    // Only notify if the document is the main frame, not a subframe
    wxString tmpUrl = evt.GetURL();
    wxString NowUrl = m_browser->GetCurrentURL();

    if (evt.GetURL() == m_browser->GetCurrentURL()) {
        // wxLogMessage("%s", "Document loaded; url='" + evt.GetURL() + "'");
    }
    UpdateState();

    // wxCommandEvent *event = new
    // wxCommandEvent(EVT_WEB_RESPONSE_MESSAGE,this->GetId()); wxQueueEvent(this,
    // event);
}

/**
 * On new window, we veto to stop extra windows appearing
 */
void GuideFrame::OnNewWindow(wxWebViewEvent &evt)
{
    wxString flag = " (other)";

    wxString NewUrl= evt.GetURL();
    wxLaunchDefaultBrowser(NewUrl);
    //if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }
    // wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    // if (m_tools_handle_new_window->IsChecked())
    //    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void GuideFrame::OnTitleChanged(wxWebViewEvent &evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void GuideFrame::OnFullScreenChanged(wxWebViewEvent &evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

void GuideFrame::OnScriptMessage(wxWebViewEvent &evt)
{
    try {
        wxString strInput = evt.GetString();
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;OnRecv:" << strInput.c_str();
        json     j        = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;Command:" << strCmd;

        if (strCmd == "userguide_loading_page_ready") {
            m_page_ready = true;
            notify_profile_loaded();
        }
        else if (strCmd == "close_page") {
            this->EndModal(wxID_CANCEL);
        }
        if (strCmd == "user_clause") {
            wxString strAction = j["data"]["action"];

            if (strAction == "refuse") {
                // CloseTheApp
                this->EndModal(wxID_OK);

                m_MainPtr->mainframe->Close(); // Refuse Clause, App quit immediately
            }
        } else if (strCmd == "user_private_choice") {
            wxString strAction = j["data"]["action"];

            if (strAction == "agree") {
                PrivacyUse = true;
            } else {
                PrivacyUse = false;
            }
        }
        else if (strCmd == "request_userguide_profile") {
            json m_Res = json::object();
            m_Res["command"] = "response_userguide_profile";
            m_Res["sequence_id"] = "10001";
            m_Res["response"]        = m_ProfileJson;

            //wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', false, json::error_handler_t::ignore));
            wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', true));

            BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;request_userguide_profile:" << strJS.c_str();
            wxGetApp().CallAfter([this,strJS] { RunScript(strJS); });
        }
        else if (strCmd == "request_custom_filaments") {
            wxString strJS = update_custom_filaments();
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        }
        else if (strCmd == "create_custom_filament") {
            this->EndModal(wxID_OK);
            wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_CREATE_FILAMENT));
        } else if (strCmd == "modify_custom_filament") {
            m_editing_filament_id = j["id"];
            this->EndModal(wxID_EDIT);
        }
        else if (strCmd == "save_userguide_models")
        {
            json MSelected = j["data"];

            int nModel = m_ProfileJson["model"].size();
            for (int m = 0; m < nModel; m++) {
                json TmpModel = m_ProfileJson["model"][m];
                m_ProfileJson["model"][m]["nozzle_selected"] = "";

                for (auto it = MSelected.begin(); it != MSelected.end(); ++it) {
                    json OneSelect = it.value();

                    wxString s1 = TmpModel["model"];
                    wxString s2 = OneSelect["model"];
                    if (s1.compare(s2) == 0) {
                        m_ProfileJson["model"][m]["nozzle_selected"] = m_ProfileJson["model"][m]["nozzle_diameter"];

                        // Automatically select default materials for this printer model
                        // This mirrors the behavior of the old ConfigWizard::select_default_materials_for_printer_model()
                        if (TmpModel.contains("materials") && !TmpModel["materials"].is_null()) {
                            std::string materials_str;

                            // Handle both string and JSON array formats for materials
                            if (TmpModel["materials"].is_string()) {
                                materials_str = TmpModel["materials"].get<std::string>();
                            } else if (TmpModel["materials"].is_array()) {
                                // Convert JSON array to semicolon-separated string for unescape_strings_cstyle
                                for (const auto& material : TmpModel["materials"]) {
                                    if (!materials_str.empty()) materials_str += ";";
                                    materials_str += material.get<std::string>();
                                }
                            } else {
                                materials_str = "";
                            }

                            boost::trim(materials_str);
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Processing default_materials for printer: " << s1.ToStdString() << " - materials: " << materials_str;

                            // Use the same parsing logic as ConfigWizard::select_default_materials_for_printer_model()
                            // This calls unescape_strings_cstyle() just like Preset.cpp:298 does
                            std::vector<std::string> materials;
                            if (Slic3r::unescape_strings_cstyle(materials_str, materials)) {
                                for (const std::string& material : materials) {
                                    if (!material.empty()) {
                                        // Mark this filament as selected if it exists in our filament list
                                        // This mirrors appconfig_new.set(section, material, "true") from ConfigWizard.cpp:2150
                                        if (m_ProfileJson["filament"].contains(material)) {
                                            m_ProfileJson["filament"][material]["selected"] = 1;
                                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Automatically selected default filament: " << material;
                                        } else {
                                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << " Default filament '" << material << "' not found in available filaments for printer: " << s1.ToStdString();
                                        }
                                    }
                                }
                            } else {
                                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Malformed default_materials field: " << materials_str << " for printer: " << s1.ToStdString();
                            }
                        } else {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " No default_materials defined for printer: " << s1.ToStdString();
                        }
                        break;
                    }
                }
            }
        }
        else if (strCmd == "save_userguide_filaments") {
            //reset
            for (auto it = m_ProfileJson["filament"].begin(); it != m_ProfileJson["filament"].end(); ++it)
            {
                m_ProfileJson["filament"][it.key()]["selected"] = 0;
            }

            json fSelected = j["data"]["filament"];
            int nF = fSelected.size();
            for (int m = 0; m < nF; m++)
            {
                std::string fName = fSelected[m];

                m_ProfileJson["filament"][fName]["selected"] = 1;
            }
        }
        else if (strCmd == "check_for_new_printers") {
            json response = json::object();
            response["command"] = "check_new_printers_result";
            // Guide pages currently send sequence_id as a number, while older
            // pages may send it as a string. Preserve the value without
            // forcing either representation.
            if (j.contains("sequence_id"))
                response["sequence_id"] = j["sequence_id"];
            else
                response["sequence_id"] = "";

            if (!m_MainPtr->preset_updater) {
                response["error"] = _u8L("Printer update service is unavailable.");
                wxString strJS = wxString::Format("HandleStudio(%s)", response.dump(-1, ' ', true));
                wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
            } else {
                // Orca: enumerate vendors directly from disk rather than from m_ProfileJson["model"]
                // — a vendor with no machine models (e.g. a filament-only bundle, or a test fixture
                // like "test123" with an empty machine_model_list) never gets a "vendor" entry
                // pushed into "model" by LoadProfileFamily(), so it would be invisible to the
                // request body and get endlessly re-offered by the server. Scan both the system dir
                // (already-installed vendors) and the bundled resources dir (shipped-but-not-yet-
                // installed vendors), same as LoadProfileData() does when building loaded_vendors.
                std::set<std::string> system_vendors;
                for (const auto& dir : {vendor_dir, rsrc_vendor_dir}) {
                    if (!boost::filesystem::exists(dir))
                        continue;
                    for (const auto& entry : boost::filesystem::directory_iterator(dir)) {
                        if (!boost::filesystem::is_directory(entry) && boost::iequals(entry.path().extension().string(), ".json"))
                            system_vendors.insert(entry.path().stem().string());
                    }
                }
                // Orca: check_new_vendors() is async (network + confirmation dialog + download
                // all happen off the calling thread apart from the dialog itself); guard against
                // this dialog being closed before the callback fires.
                wxWeakRef<GuideFrame> weak_this(this);
                try {
                    m_MainPtr->preset_updater->check_new_vendors(
                        system_vendors, [weak_this, response](std::vector<std::string> installed_vendors, bool declined) mutable {
                            if (!weak_this)
                                return;

                            // Orca: append the newly installed vendor(s) into the in-memory
                            // profile data (instead of a full LoadProfileData() rescan of every
                            // vendor) and push the refreshed list to the webview, the same way
                            // request_userguide_profile does, so the printer list picks them up
                            // without needing to reopen the guide.
                            for (const auto& vendor_id : installed_vendors) {
                                weak_this->LoadProfileFamily(vendor_id, (weak_this->vendor_dir / (vendor_id + ".json")).string());
                            }
                            if (!installed_vendors.empty()) {
                                json profile_response            = json::object();
                                profile_response["command"]      = "response_userguide_profile";
                                profile_response["sequence_id"]  = "10001";
                                profile_response["response"]     = weak_this->m_ProfileJson;
                                wxString profileJS = wxString::Format("HandleStudio(%s)", profile_response.dump(-1, ' ', true));
                                weak_this->RunScript(profileJS);
                            }

                            response["vendors"]  = installed_vendors;
                            response["declined"] = declined;
                            wxString strJS = wxString::Format("HandleStudio(%s)", response.dump(-1, ' ', true));
                            weak_this->RunScript(strJS);
                        });
                } catch (const std::exception &e) {
                    BOOST_LOG_TRIVIAL(warning) << "Failed to check for new printers: " << e.what();
                    response["error"] = _u8L("Failed to check for new printers.");
                    wxString strJS = wxString::Format("HandleStudio(%s)", response.dump(-1, ' ', true));
                    wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
                }
            }
        }
        else if (strCmd == "user_guide_finish") {
            SaveProfile();

            std::string oldregion = m_ProfileJson["region"];
            if (m_Region != oldregion) {
                AppConfig* config = GUI::wxGetApp().app_config;
                std::string country_code = config->get_country_code();
                NetworkAgent* agent = wxGetApp().getAgent();
                if (agent) {
                    agent->set_country_code(country_code);
                    if (wxGetApp().is_user_login()) {
                        BOOST_LOG_TRIVIAL(info) << "logout: user_logout on user_guide_finish";
                        // agent->user_logout();
                        wxGetApp().request_user_logout(ORCA_CLOUD_PROVIDER, /*user_initiated=*/true);
                    }
                }
            }

            this->EndModal(wxID_OK);

            if (InstallNetplugin)
                GUI::wxGetApp().CallAfter([] { GUI::wxGetApp().ShowDownNetPluginDlg(); });
        }
        else if (strCmd == "user_guide_create_printer") {
            this->EndModal(wxID_CANCEL);
            this->Close();
            GUI::wxGetApp().CallAfter([] {GUI::wxGetApp().sidebar().create_printer_preset();});
        }
        else if (strCmd == "user_guide_cancel") {
            this->EndModal(wxID_CANCEL);
            this->Close();
        } else if (strCmd == "save_region") {
            m_Region = j["region"];
        }
        else if (strCmd == "network_plugin_install") {
            std::string sAction = j["data"]["action"];

            if (sAction == "yes") {
                if (!network_plugin_ready)
                    InstallNetplugin = true;
                else //already ready
                    InstallNetplugin = false;
            }
            else
                InstallNetplugin = false;
        }
        else if (strCmd == "save_stealth_mode") {
            wxString strAction = j["data"]["action"];

            if (strAction == "yes") {
                StealthMode = true;
            } else {
                StealthMode = false;
            }
        }
    } catch (std::exception &e) {
        // wxMessageBox(e.what(), "json Exception", MB_OK);
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;Error:" << e.what();
    }

    wxString strAll = m_ProfileJson.dump(-1,' ',false, json::error_handler_t::ignore);
}

void GuideFrame::RunScript(const wxString &javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    //m_javascript = javascript;

    // wxLogMessage("Running JavaScript:\n%s\n", javascript);

    if (!m_browser) return;

    WebView::RunScript(m_browser, javascript);
}

#if wxUSE_WEBVIEW_IE
void GuideFrame::OnRunScriptObjectWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void GuideFrame::OnRunScriptDateWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void GuideFrame::OnRunScriptArrayWithEmulationLevel(wxCommandEvent &WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void GuideFrame::OnError(wxWebViewEvent &evt)
{
#define WX_ERROR_CASE(type) \
    case type: category = #type; break;

    wxString category;
    switch (evt.GetInt()) {
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CONNECTION);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_CERTIFICATE);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_AUTH);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_SECURITY);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_NOT_FOUND);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_REQUEST);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_USER_CANCELLED);
        WX_ERROR_CASE(wxWEBVIEW_NAV_ERR_OTHER);
    }

    // wxLogMessage("%s", "Error; url='" + evt.GetURL() + "', error='" +
    // category + " (" + evt.GetString() + ")'");

    // Show the info bar with an error
    // m_info->ShowMessage(_L("An error occurred loading ") + evt.GetURL() +
    // "\n" + "'" + category + "'", wxICON_ERROR);
    BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnError: An error occurred loading " << evt.GetURL() << category;

    UpdateState();
}

void GuideFrame::OnScriptResponseMessage(wxCommandEvent &WXUNUSED(evt))
{
}

bool GuideFrame::IsFirstUse()
{
    wxString    strUse;
    std::string strVal = wxGetApp().app_config->get(std::string(m_SectionName.mb_str()), "finish");
    if (strVal == "1")
        return false;

    if (orca_bundle_rsrc == true)
        return true;

    return true;
}

int GuideFrame::SaveProfile()
{
    // SoftFever: don't collect info
    //privacy
    // if (PrivacyUse == true) {
    //     m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "privacyuse", "1");
    // } else
    //     m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "privacyuse", "0");

    m_MainPtr->app_config->set("region", m_Region);
    m_MainPtr->app_config->set_bool("stealth_mode", StealthMode);

    //finish
    m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "finish", true);

    m_MainPtr->app_config->save();

    std::string strAll = m_ProfileJson.dump(-1, ' ', false, json::error_handler_t::ignore);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "before save to app_config: "<< std::endl<<strAll;

    //set filaments to app_config
    const std::string &section_name = AppConfig::SECTION_FILAMENTS;
    std::map<std::string, std::string> section_new;
    m_appconfig_new.clear_section(section_name);
    for (auto it = m_ProfileJson["filament"].begin(); it != m_ProfileJson["filament"].end(); ++it) {
        if (it.value()["selected"] == 1){
            section_new[it.key()] = "true";
        }
    }
    m_appconfig_new.set_section(section_name, section_new);

    //set vendors to app_config
    Slic3r::AppConfig::VendorMap empty_vendor_map;
    m_appconfig_new.set_vendors(empty_vendor_map);
    for (auto it = m_ProfileJson["model"].begin(); it != m_ProfileJson["model"].end(); ++it)
    {
        if (it.value().is_object()) {
            json temp_model = it.value();
            std::string model_name = temp_model["model"];
            std::string vendor_name = temp_model["vendor"];
            std::string selected = temp_model["nozzle_selected"];
            boost::trim(selected);
            std::string nozzle;
            while (selected.size() > 0) {
                auto pos = selected.find(';');
                if (pos != std::string::npos) {
                    nozzle   = selected.substr(0, pos);
                    m_appconfig_new.set_variant(vendor_name, model_name, nozzle, "true");
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("vendor_name %1%, model_name %2%, nozzle %3% selected")%vendor_name %model_name %nozzle;
                    selected = selected.substr(pos + 1);
                    boost::trim(selected);
                }
                else {
                    m_appconfig_new.set_variant(vendor_name, model_name, selected, "true");
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("vendor_name %1%, model_name %2%, nozzle %3% selected")%vendor_name %model_name %selected;
                    break;
                }
            }
        }
    }

    //m_appconfig_new

    return 0;
}

// Applies the whole set the pages collected (SaveProfile()) as a Confirm does: Ops::apply_install_set() asks about
// the presets' unsaved changes, installs the vendor bundles, reloads the presets and selects a newly added printer.
bool GuideFrame::apply_config()
{
    Ops::InstallSet set;
    set.printers = m_appconfig_new.vendors();
    if (m_appconfig_new.has_section(AppConfig::SECTION_FILAMENTS))
        set.filaments = m_appconfig_new.get_section(AppConfig::SECTION_FILAMENTS);
    return bool(Ops::apply_install_set(set, Ops::PresetChanges::Ask));
}

bool GuideFrame::run()
{
    //BOOST_LOG_TRIVIAL(info) << boost::format("Running ConfigWizard, reason: %1%, start_page: %2%") % reason % start_page;

    GUI_App &app = wxGetApp();

    //p->set_run_reason(reason);
    //p->set_start_page(start_page);
    app.preset_bundle->export_selections(*app.app_config);

    BOOST_LOG_TRIVIAL(info) << "GuideFrame before ShowModal";
    // display position
    int main_frame_display_index = wxDisplay::GetFromWindow(wxGetApp().mainframe);
    int guide_display_index = wxDisplay::GetFromWindow(this);
    if (main_frame_display_index != guide_display_index) {
        wxDisplay display    = wxDisplay(main_frame_display_index);
        wxRect    screenRect = display.GetGeometry();
        int       guide_x    = screenRect.x + (screenRect.width - this->GetSize().GetWidth()) / 2;
        int       guide_y    = screenRect.y + (screenRect.height - this->GetSize().GetHeight()) / 2;
        this->SetPosition(wxPoint(guide_x, guide_y));
    }

    int result = this->ShowModal();
    if (result == wxID_OK) {
        BOOST_LOG_TRIVIAL(info) << "GuideFrame returned ok";
        if (! this->apply_config())
            return false;
        BOOST_LOG_TRIVIAL(info) << "GuideFrame applied";
        this->Close();
        return true;
    } else if (result == wxID_CANCEL) {
        BOOST_LOG_TRIVIAL(info) << "GuideFrame cancelled";
        if (app.preset_bundle->printers.only_default_printers()) {
            //we install the default here
            bool apply_keeped_changes = false;
            //clear filament section and use default materials
            app.app_config->set_variant(PresetBundle::ORCA_DEFAULT_BUNDLE,
                PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT, "true");
            app.app_config->clear_section(AppConfig::SECTION_FILAMENTS);
            app.preset_bundle->load_selections(*app.app_config, {PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT, PresetBundle::ORCA_DEFAULT_FILAMENT, std::string()});

            app.app_config->set_legacy_datadir(false);
            app.update_mode();
            return true;
        }
        else
            return false;
    } else if (result == wxID_EDIT) {
        this->Close();
        Filamentinformation *filament_info = new Filamentinformation();
        filament_info->filament_id        = m_editing_filament_id;
        wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_MODIFY_FILAMENT, filament_info));
        return false;
    }
    else
        return false;
}

int GuideFrame::GetFilamentInfo( std::string VendorDirectory, json & pFilaList, std::string filepath, std::string &sVendor, std::string &sType)
{
    std::unordered_set<std::string> visiting;
    return GetFilamentInfo(VendorDirectory, pFilaList, filepath, sVendor, sType, visiting);
}

int GuideFrame::GetFilamentInfo(const std::string& VendorDirectory, json& pFilaList,
                                const std::string& filepath, std::string& sVendor,
                                std::string& sType, std::unordered_set<std::string>& visiting)
{
    //GetStardardFilePath(filepath);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " GetFilamentInfo:VendorDirectory - " << VendorDirectory << ", Filepath - "<<filepath;

    // Path-scoped guard: without it an `inherits` cycle would recurse forever.
    // The repeated file was not inserted, so the cycle hit has nothing to erase.
    if (!visiting.insert(filepath).second) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inherits cycle at " << filepath;
        return -1;
    }

    // Resolve this file's own vendor/type into LOCAL variables, independent of
    // whatever the caller already accumulated. The cache entry for `filepath`
    // must reflect only this file's own inherits chain — passing the caller's
    // in/out sVendor/sType down would poison a shared base file's cache with the
    // type of whichever descendant happened to traverse it first (e.g. an ABS
    // preset reaching fdm_filament_common before a PLA one, making every PLA that
    // later reuses the cached base resolve to "ABS"). Merge into the caller's
    // out-params only at the end, filling empties.
    std::string vendor;
    std::string type;
    int         status = 0;

    const auto cache_it = filament_info_cache.find(filepath);
    if (cache_it != filament_info_cache.end()) {
        vendor = cache_it->second.vendor;
        type   = cache_it->second.type;
        status = cache_it->second.status;
    } else {
        try {
            std::string contents;
            LoadFile(filepath, contents);
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Json Contents: " << contents;
            json jLocal = json::parse(contents);

            if (jLocal.contains("filament_vendor"))
                vendor = jLocal["filament_vendor"][0];
            else
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " - Not Contains filament_vendor";

            if (jLocal.contains("filament_type"))
                type = jLocal["filament_type"][0];
            else
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " - Not Contains filament_type";

            if (jLocal.contains("inherits")) {
                std::string FName = jLocal["inherits"];

                if (!pFilaList.contains(FName)) {
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "pFilaList - Not Contains inherits filaments: " << FName;
                    status = -1;
                } else {
                    std::string FPath = pFilaList[FName]["sub_path"];
                    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Before Format Inherits Path: VendorDirectory - " << VendorDirectory << ", sub_path - " << FPath;
                    wxString strNewFile = wxString::Format("%s%c%s", wxString(VendorDirectory.c_str(), wxConvUTF8), boost::filesystem::path::preferred_separator, FPath);
                    boost::filesystem::path inherits_path(w2s(strNewFile));
                    if (!boost::filesystem::exists(inherits_path))
                        inherits_path = (boost::filesystem::path(m_OrcaFilaLibPath) / boost::filesystem::path(FPath)).make_preferred();

                    if (boost::filesystem::exists(inherits_path)) {
                        // Traverse the full chain for errors; inherited values only fill local gaps.
                        status = GetFilamentInfo(VendorDirectory, pFilaList, inherits_path.string(), vendor, type, visiting);
                    } else {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inherits File Not Exist: " << inherits_path;
                        status = -1;
                    }
                }
            } else {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " - Not Contains inherits";
                status = 0;
            }
        }
        catch (nlohmann::detail::parse_error &err) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << filepath << " got a nlohmann::detail::parse_error, reason = " << err.what();
            status = -1;
        }
        catch (std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << filepath << " got exception: " << e.what();
            status = -1;
        }

        filament_info_cache[filepath] = CachedFilamentInfo{status, vendor, type};
    }

    // Merge this file's resolved values into the caller's out-params (fill empties only).
    if (sVendor.empty()) sVendor = vendor;
    if (sType.empty())   sType   = type;
    visiting.erase(filepath);
    return status;
}

bool GuideFrame::BuildProfileDataFromPresetBundle()
{
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (!pb || pb->vendors.empty())
        return false;
    // If rsrc_vendor_dir has vendors (profile JSONs, or the preset caches a
    // packaged build ships instead) not covered by the app's bundle, the
    // bundle is incomplete (e.g. dev env where data_dir/system only has
    // OrcaFilamentLibrary+Custom). Fall back so the slow path reads both dirs.
    try {
        for (const std::string& name : vendor_names_in(rsrc_vendor_dir)) {
            if (pb->vendors.find(name) == pb->vendors.end()) {
                BOOST_LOG_TRIVIAL(info) << "GuideFrame: vendor '" << name
                    << "' in resources but not in preset_bundle — falling back to JSON loading";
                return false;
            }
        }
    } catch (const std::exception&) {}
    try {
        json data = Ops::profile_data_from(*pb);
        if (data["machine"].empty())
            return false;
        for (const char* key : { "model", "machine", "filament", "process" })
            m_ProfileJson[key] = std::move(data[key]);
        return true;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "GuideFrame::BuildProfileDataFromPresetBundle failed: " << e.what()
                                   << " — falling back to JSON loading";
        reset_profile_json();
        return false;
    }
}

bool GuideFrame::BuildProfileDataFromVendors()
{
    // A refusal sends this open to the scan of the vendors' JSON files, which lists what it can read of every vendor.
    Ops::Outcome<json> data = Ops::printer_profile_data(m_cancel_token.get());
    if (!data) {
        BOOST_LOG_TRIVIAL(info) << "GuideFrame: " << data.refusal().english();
        return false;
    }
    for (const char* key : { "model", "machine", "filament", "process" })
        m_ProfileJson[key] = data.value().at(key);
    return true;
}

int GuideFrame::LoadProfileData()
{
    // Background thread: the fast path in OnNavigationComplete failed (presets not yet loaded).
    // Loading order (fastest to slowest):
    //   1. Load every vendor, from its preset cache wherever one covers it
    //   2. Read all vendor JSONs by hand
    try {
        if (!BuildProfileDataFromVendors()) {
            // Last resort — read all vendor JSONs
            std::set<std::string> loaded_vendors;
            auto filament_library_name = boost::filesystem::path(PresetBundle::ORCA_FILAMENT_LIBRARY).replace_extension(".json");
            if (boost::filesystem::exists(vendor_dir / filament_library_name))
                LoadProfileFamily(PresetBundle::ORCA_FILAMENT_LIBRARY, (vendor_dir / filament_library_name).string());
            else
                LoadProfileFamily(PresetBundle::ORCA_FILAMENT_LIBRARY, (rsrc_vendor_dir / filament_library_name).string());
            loaded_vendors.insert(PresetBundle::ORCA_FILAMENT_LIBRARY);

            boost::filesystem::directory_iterator endIter;
            for (boost::filesystem::directory_iterator iter(vendor_dir); iter != endIter; iter++) {
                if (!boost::filesystem::is_directory(*iter)) {
                    wxString strVendor = from_u8(iter->path().string()).BeforeLast('.');
                    strVendor          = strVendor.AfterLast('\\');
                    strVendor          = strVendor.AfterLast('/');
                    wxString strExtension = from_u8(iter->path().string()).AfterLast('.').Lower();
                    if (strExtension.CmpNoCase("json") != 0 || loaded_vendors.find(w2s(strVendor)) != loaded_vendors.end())
                        continue;
                    LoadProfileFamily(w2s(strVendor), iter->path().string());
                    loaded_vendors.insert(w2s(strVendor));
                }
                if (*m_cancel_token) return 0;
            }

            boost::filesystem::directory_iterator others_endIter;
            for (boost::filesystem::directory_iterator iter(rsrc_vendor_dir); iter != others_endIter; iter++) {
                if (!boost::filesystem::is_directory(*iter)) {
                    wxString strVendor = from_u8(iter->path().string()).BeforeLast('.');
                    strVendor          = strVendor.AfterLast('\\');
                    strVendor          = strVendor.AfterLast('/');
                    wxString strExtension = from_u8(iter->path().string()).AfterLast('.').Lower();
                    if (strExtension.CmpNoCase("json") != 0 || loaded_vendors.find(w2s(strVendor)) != loaded_vendors.end())
                        continue;
                    LoadProfileFamily(w2s(strVendor), iter->path().string());
                    loaded_vendors.insert(w2s(strVendor));
                }
                if (*m_cancel_token) return 0;
            }
        }

        // Capture the cancel token by value (shared_ptr) so the lambda doesn't
        // touch `this` if GuideFrame is destroyed before the event fires.
        auto tok = m_cancel_token;
        wxGetApp().CallAfter([this, tok] {
            if (!*tok)
                on_profile_loaded();
        });
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", error: " << e.what();
    }

    filament_info_cache.clear();
    return 0;
}

int GuideFrame::SaveProfileData()
{
    try {
        const auto enabled_filaments = wxGetApp().app_config->has_section(AppConfig::SECTION_FILAMENTS) ? wxGetApp().app_config->get_section(AppConfig::SECTION_FILAMENTS) : std::map<std::string, std::string>();
        m_appconfig_new.set_vendors(*wxGetApp().app_config);
        m_appconfig_new.set_section(AppConfig::SECTION_FILAMENTS, enabled_filaments);

        for (auto it = m_ProfileJson["model"].begin(); it != m_ProfileJson["model"].end(); ++it)
        {
            if (it.value().is_object()) {
                json& temp_model = it.value();
                std::string model_name = temp_model["model"];
                std::string vendor_name = temp_model["vendor"];
                std::string nozzle_diameter = temp_model["nozzle_diameter"];
                std::string selected;
                boost::trim(nozzle_diameter);
                std::string nozzle;
                bool enabled = false, first=true;
                while (nozzle_diameter.size() > 0) {
                    auto pos = nozzle_diameter.find(';');
                    if (pos != std::string::npos) {
                        nozzle   = nozzle_diameter.substr(0, pos);
                        enabled = m_appconfig_new.get_variant(vendor_name, model_name, nozzle);
                        if (enabled) {
                            if (!first)
                                selected += ";";
                            selected += nozzle;
                            first = false;
                        }
                        nozzle_diameter = nozzle_diameter.substr(pos + 1);
                        boost::trim(nozzle_diameter);
                    }
                    else {
                        enabled = m_appconfig_new.get_variant(vendor_name, model_name, nozzle_diameter);
                        if (enabled) {
                            if (!first)
                                selected += ";";
                            selected += nozzle_diameter;
                        }
                        break;
                    }
                }
                temp_model["nozzle_selected"] = selected;
                //m_ProfileJson["model"][a]["nozzle_selected"]
            }
        }

        if (m_ProfileJson["model"].size() == 1) {
            std::string strNozzle = m_ProfileJson["model"][0]["nozzle_diameter"];
            m_ProfileJson["model"][0]["nozzle_selected"]=strNozzle;
        }


        for (auto it = m_ProfileJson["filament"].begin(); it != m_ProfileJson["filament"].end(); ++it) {
            //json temp_filament = it.value();
            std::string filament_name = it.key();
            if (enabled_filaments.find(filament_name) != enabled_filaments.end())
                m_ProfileJson["filament"][filament_name]["selected"] = 1;
        }

        //----region
        m_Region = wxGetApp().app_config->get("region");
        m_ProfileJson["region"] = m_Region;

        m_ProfileJson["network_plugin_install"] = wxGetApp().app_config->get("app","installed_networking");
        m_ProfileJson["network_plugin_compability"] = wxGetApp().is_compatibility_version() ? "1" : "0";
        network_plugin_ready = wxGetApp().is_compatibility_version();

        StealthMode = wxGetApp().app_config->get_bool("app","stealth_mode");
        m_ProfileJson["stealth_mode"] = StealthMode;
    }
    catch (std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", error: "<< e.what() <<std::endl;
    }

    return 0;
}

void StringReplace(std::string &strBase, std::string strSrc, std::string strDes)
{
    std::string::size_type pos    = 0;
    std::string::size_type srcLen = strSrc.size();
    std::string::size_type desLen = strDes.size();
    pos                      = strBase.find(strSrc, pos);
    while ((pos != std::string::npos)) {
        strBase.replace(pos, srcLen, strDes);
        pos = strBase.find(strSrc, (pos + desLen));
    }
}


int GuideFrame::LoadProfileFamily(std::string strVendor, std::string strFilePath)
{
    // wxString strFolder = strFilePath.BeforeLast(boost::filesystem::path::preferred_separator);
    boost::filesystem::path file_path(strFilePath);
    boost::filesystem::path vendor_dir = boost::filesystem::absolute(file_path.parent_path() / strVendor).make_preferred();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  vendor path %1%.") % vendor_dir.string();
    try {
        // wxLogMessage("GUIDE: json_path1  %s", w2s(strFilePath));

        std::string contents;
        LoadFile(strFilePath, contents);
        // wxLogMessage("GUIDE: json_path1 content: %s", contents);
        json jLocal = json::parse(contents);
        // wxLogMessage("GUIDE: json_path1 Loaded");

        // BBS:models
        json pmodels = jLocal["machine_model_list"];
        int  nsize   = pmodels.size();

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% machine models") % nsize;

        for (int n = 0; n < nsize; n++) {
            json OneModel = pmodels.at(n);

            OneModel["model"] = OneModel["name"];
            OneModel.erase("name");

            std::string s1 = OneModel["model"];
            std::string s2 = OneModel["sub_path"];

            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path)) continue;

            std::string             sub_file = sub_path.string();

            // wxLogMessage("GUIDE: json_path2  %s", w2s(ModelFilePath));
            LoadFile(sub_file, contents);
            // wxLogMessage("GUIDE: json_path2 content: %s", contents);
            json pm = json::parse(contents);
            // wxLogMessage("GUIDE: json_path2  loaded");

            OneModel["name"]      = pm["name"];
            OneModel["vendor"]    = strVendor;
            std::string NozzleOpt = pm["nozzle_diameter"];
            StringReplace(NozzleOpt, " ", "");
            OneModel["nozzle_diameter"] = NozzleOpt;
            OneModel["materials"]       = pm["default_materials"];

            // wxString strCoverPath = wxString::Format("%s\\%s\\%s_cover.png", strFolder, strVendor, std::string(s1.mb_str()));
            std::string cover_file = s1 + "_cover.png";
            boost::filesystem::path cover_path = boost::filesystem::absolute(vendor_dir / cover_file).make_preferred();
            BOOST_LOG_TRIVIAL(info) << "[WebGuideDialog] " << cover_path;
            if (!boost::filesystem::exists(cover_path)) {
                cover_path = boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "/profiles/" / strVendor / cover_file)
                                 .make_preferred();
                if (!boost::filesystem::exists(cover_path))
                    cover_path = (boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "/web/image/printer/") / cover_file)
                                     .make_preferred();
            }
            OneModel["cover"]                  = into_u8(file_url_from_path(cover_path));

            OneModel["nozzle_selected"] = "";

            m_ProfileJson["model"].push_back(OneModel);
        }

        // BBS:Machine
        json pmachine = jLocal["machine_list"];
        nsize         = pmachine.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% machines") % nsize;
        for (int n = 0; n < nsize; n++) {
            json OneMachine = pmachine.at(n);

            std::string s1 = OneMachine["name"];
            std::string s2 = OneMachine["sub_path"];

            // wxString ModelFilePath = wxString::Format("%s\\%s\\%s", strFolder, strVendor, s2);
            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path)) continue;

            std::string             sub_file = sub_path.string();
            LoadFile(sub_file, contents);
            json pm = json::parse(contents);

            std::string strInstant = pm["instantiation"];
            if (strInstant.compare("true") == 0) {
                OneMachine["model"] = pm["printer_model"];
                OneMachine["nozzle"] = pm["nozzle_diameter"][0];

                m_ProfileJson["machine"][s1]=OneMachine;
            }
        }

        // BBS:Filament
        json pFilament = jLocal["filament_list"];
        json tFilaList = m_OrcaFilaList;
        nsize          = pFilament.size();

        for (int n = 0; n < nsize; n++) {
            json OneFF = pFilament.at(n);

            std::string s1    = OneFF["name"];
            std::string s2    = OneFF["sub_path"];

            tFilaList[s1] = OneFF;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Vendor: " << strVendor <<", tFilaList Add: " << s1;
        }

        int nFalse  = 0;
        int nModel  = 0;
        int nFinish = 0;
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% filaments") % nsize;
        for (int n = 0; n < nsize; n++) {
            json OneFF = pFilament.at(n);

            std::string s1 = OneFF["name"];
            std::string s2 = OneFF["sub_path"];

            if (!m_ProfileJson["filament"].contains(s1)) {
                // wxString ModelFilePath = wxString::Format("%s\\%s\\%s", strFolder, strVendor, s2);
                boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
                if (!boost::filesystem::exists(sub_path)) continue;

                std::string             sub_file = sub_path.string();
                LoadFile(sub_file, contents);
                json pm = json::parse(contents);

                std::string strInstant = pm["instantiation"];
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Load Filament:" << s1 << ",Path:" << sub_file << ",instantiation?" << strInstant;

                if (strInstant == "true") {
                    std::string sV;
                    std::string sT;

                    int nRet = GetFilamentInfo(vendor_dir.string(),tFilaList, sub_file, sV, sT);
                    if (nRet != 0 || sV.empty() || sT.empty()) {
                        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Load Filament:" << s1 << ",unresolved vendor/type, Vendor:" << sV << ",Type:"<< sT;
                        continue;
                    }

                    OneFF["vendor"] = sV;
                    OneFF["type"]   = sT;

                    OneFF["models"]   = "";

                    json pPrinters = pm["compatible_printers"];
                    int nPrinter   = pPrinters.size();
                    std::string ModelList = "";
                    for (int i = 0; i < nPrinter; i++)
                    {
                        std::string sP = pPrinters.at(i);
                        if (m_ProfileJson["machine"].contains(sP))
                        {
                            std::string mModel = m_ProfileJson["machine"][sP]["model"];
                            std::string mNozzle = m_ProfileJson["machine"][sP]["nozzle"];
                            std::string NewModel = mModel + "++" + mNozzle;

                            ModelList = (boost::format("%1%[%2%]") % ModelList % NewModel).str();
                        }
                    }

                    OneFF["models"]    = ModelList;
                    OneFF["selected"] = 0;

                    m_ProfileJson["filament"][s1] = OneFF;
                } else
                    continue;

            }
        }
        if(strVendor == PresetBundle::ORCA_FILAMENT_LIBRARY)
            m_OrcaFilaList = tFilaList;

        // process
        json pProcess = jLocal["process_list"];
        nsize         = pProcess.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% processes") % nsize;
        for (int n = 0; n < nsize; n++) {
            json OneProcess = pProcess.at(n);

            std::string s2 = OneProcess["sub_path"];
            // wxString ModelFilePath = wxString::Format("%s\\%s\\%s", strFolder, strVendor, s2);
            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path)) continue;

            std::string             sub_file = sub_path.string();
            LoadFile(sub_file, contents);
            json pm = json::parse(contents);

            std::string bInstall = pm["instantiation"];
            if (bInstall == "true") { m_ProfileJson["process"].push_back(OneProcess); }
        }

    } catch (nlohmann::detail::parse_error &err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << strFilePath << " got a nlohmann::detail::parse_error, reason = " << err.what();
        return -1;
    } catch (std::exception &e) {
        // wxMessageBox(e.what(), "", MB_OK);
        // wxLogMessage("GUIDE: LoadFamily Error: %s", e.what());
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << strFilePath << " got exception: " << e.what();
        return -1;
    }

    return 0;
}



void GuideFrame::StrReplace(std::string &strBase, std::string strSrc, std::string strDes)
{
    int pos    = 0;
    int srcLen = strSrc.size();
    int desLen = strDes.size();
    pos = strBase.find(strSrc, pos);
    while ((pos != std::string::npos)) {
        strBase.replace(pos, srcLen, strDes);
        pos = strBase.find(strSrc, (pos + desLen));
    }
}

std::string GuideFrame::w2s(wxString sSrc)
{
    return std::string(sSrc.mb_str());
}

void GuideFrame::GetStardardFilePath(std::string &FilePath) {
    StrReplace(FilePath, "\\", w2s(wxString::Format("%c", boost::filesystem::path::preferred_separator)));
    StrReplace(FilePath, "/" , w2s(wxString::Format("%c", boost::filesystem::path::preferred_separator)));
}

bool GuideFrame::LoadFile(std::string jPath, std::string &sContent)
{
    try {
        boost::nowide::ifstream t(jPath);
        std::stringstream buffer;
        buffer << t.rdbuf();
        sContent=buffer.str();
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << boost::format(", load %1% into buffer")% jPath;
    }
    catch (std::exception &e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",  got exception: "<<e.what();
        return false;
    }

    return true;
}

int GuideFrame::DownloadPlugin()
{
    return wxGetApp().download_plugin(
        "plugins", "network_plugin.zip",
        [this](int status, int percent, bool& cancel) {
            return ShowPluginStatus(status, percent, cancel);
        }
    , nullptr);
}

int GuideFrame::InstallPlugin()
{
    return wxGetApp().install_plugin("plugins", "network_plugin.zip",
        [this](int status, int percent, bool &cancel) {
            return ShowPluginStatus(status, percent, cancel);
        }
    );
}

int GuideFrame::ShowPluginStatus(int status, int percent, bool& cancel)
{
    //TODO
    return 0;
}



}} // namespace Slic3r::GUI
