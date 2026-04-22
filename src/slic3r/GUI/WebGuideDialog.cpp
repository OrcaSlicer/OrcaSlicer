#include "WebGuideDialog.hpp"
#include "ConfigWizard.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iostreams/detail/select.hpp>
#include <exception>
#include <string.h>
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r_version.h"

#include <string_view>
#include <wx/sizer.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <wx/wx.h>
#include <wx/display.h>
#include <wx/fileconf.h>
#include <wx/file.h>
#include <wx/wfstream.h>

#include <boost/cast.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>
#include <chrono>
#include <unordered_map>

#include "MainFrame.hpp"
#include <boost/dll.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/Utils/Http.hpp>
#include <libslic3r/miniz_extension.hpp>
#include <libslic3r/Utils.hpp>
#include "CreatePresetsDialog.hpp"

#include <simdjson/simdjson.h>

using namespace nlohmann;

namespace Slic3r { namespace GUI {

static std::string simd_string_or_empty(const simdjson::dom::element& element)
{
    auto value = element.get_string();
    return value.error() ? std::string() : std::string(value.value());
}

static std::string simd_string_or_empty(const simdjson::dom::object& object, const char* key)
{
    auto value = object[key];
    return value.error() ? std::string() : simd_string_or_empty(value.value());
}

static std::string simd_first_array_string_or_empty(const simdjson::dom::object& object, const char* key)
{
    auto value = object[key];
    if (value.error())
        return {};

    auto array = value.value().get_array();
    if (array.error())
        return {};

    for (simdjson::dom::element item : array.value()) {
        return simd_string_or_empty(item);
    }

    return {};
}

static int get_filament_info_simd(const std::string&                                  vendor_directory,
                                  const std::unordered_map<std::string, std::string>& filament_paths,
                                  const std::string&                                  orca_fila_lib_path,
                                  const std::string&                                  filepath,
                                  std::string&                                        vendor,
                                  std::string&                                        type)
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " VendorDirectory - " << vendor_directory << ", Filepath - " << filepath;

    try {
        simdjson::dom::parser  parser;
        simdjson::dom::element jLocal = parser.load(filepath);
        simdjson::dom::object  object = jLocal.get_object();

        if (vendor.empty())
            vendor = simd_first_array_string_or_empty(object, "filament_vendor");

        if (type.empty())
            type = simd_first_array_string_or_empty(object, "filament_type");

        if (!vendor.empty() && !type.empty())
            return 0;

        std::string inherits = simd_string_or_empty(object, "inherits");
        if (!inherits.empty()) {
            auto it = filament_paths.find(inherits);
            if (it == filament_paths.end()) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " filament_paths missing inherits filament: " << inherits;
                return -1;
            }

            boost::filesystem::path inherits_path = (boost::filesystem::path(vendor_directory) / it->second).make_preferred();
            if (!boost::filesystem::exists(inherits_path))
                inherits_path = (boost::filesystem::path(orca_fila_lib_path) / it->second).make_preferred();

            if (!boost::filesystem::exists(inherits_path)) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " inherits file does not exist: " << inherits_path;
                return -1;
            }

            return get_filament_info_simd(vendor_directory, filament_paths, orca_fila_lib_path, inherits_path.string(), vendor, type);
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << filepath << " does not contain inherits";
        if (type.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " filament type is empty";
            return -1;
        }

        vendor = "Generic";
        return 0;
    } catch (const simdjson::simdjson_error& err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << filepath << " got a simdjson error, reason = " << err.what();
        return -1;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << filepath << " got exception: " << e.what();
        return -1;
    }
}

static std::string join_materials(const std::vector<std::string>& materials)
{
    std::string result;
    for (const std::string& material : materials) {
        if (material.empty())
            continue;
        if (!result.empty())
            result += ";";
        result += material;
    }
    return result;
}

static json guide_profile_to_json(const GuideProfile& profile)
{
    json response               = json::object();
    response["model"]           = json::array();
    response["machine"]         = json::object();
    response["filament"]        = json::object();
    response["process"]         = json::array();
    response["region"]          = profile.region;
    response["network_plugin_install"] = profile.network_plugin_install;
    response["network_plugin_compability"] = profile.network_plugin_compability;
    response["stealth_mode"]    = profile.stealth_mode;

    for (const GuideModel& model : profile.model) {
        json model_json                 = json::object();
        model_json["model"]             = model.model;
        model_json["sub_path"]          = model.sub_path;
        model_json["name"]              = model.name;
        model_json["vendor"]            = model.vendor;
        model_json["nozzle_diameter"]   = model.nozzle_diameter;
        model_json["cover"]             = model.cover;
        model_json["nozzle_selected"]   = model.nozzle_selected;
        if (model.materials.size() <= 1)
            model_json["materials"] = model.materials.empty() ? "" : model.materials.front();
        else
            model_json["materials"] = model.materials;
        response["model"].push_back(model_json);
    }

    for (const auto& [key, machine] : profile.machine) {
        json machine_json        = json::object();
        machine_json["name"]     = machine.name;
        machine_json["sub_path"] = machine.sub_path;
        machine_json["model"]    = machine.model;
        machine_json["nozzle"]   = machine.nozzle;
        response["machine"][key] = machine_json;
    }

    for (const auto& [key, filament] : profile.filament) {
        json filament_json        = json::object();
        filament_json["name"]     = filament.name;
        filament_json["sub_path"] = filament.sub_path;
        filament_json["vendor"]   = filament.vendor;
        filament_json["type"]     = filament.type;
        filament_json["models"]   = filament.models;
        filament_json["selected"] = filament.selected;
        response["filament"][key] = filament_json;
    }

    for (const GuideProcess& process : profile.process) {
        json process_json        = json::object();
        process_json["name"]     = process.name;
        process_json["sub_path"] = process.sub_path;
        response["process"].push_back(process_json);
    }

    return response;
}

static wxString update_custom_filaments()
{
    json m_Res                                                                    = json::object();
    m_Res["command"]                                                              = "update_custom_filaments";
    m_Res["sequence_id"]                                                          = "2000";
    json                                              m_CustomFilaments           = json::array();
    PresetBundle*                                     preset_bundle               = wxGetApp().preset_bundle;
    std::map<std::string, std::vector<Preset const*>> temp_filament_id_to_presets = preset_bundle->filaments.get_filament_presets();

    std::vector<std::pair<std::string, std::string>> need_sort;
    bool                                             need_delete_some_filament = false;
    for (std::pair<std::string, std::vector<Preset const*>> filament_id_to_presets : temp_filament_id_to_presets) {
        std::string filament_id = filament_id_to_presets.first;
        if (filament_id.empty())
            continue;
        if (filament_id == "null") {
            need_delete_some_filament = true;
        }
        bool        filament_with_base_id = false;
        bool        not_need_show         = false;
        std::string filament_name;
        for (const Preset* preset : filament_id_to_presets.second) {
            if (preset->is_system || preset->is_project_embedded) {
                not_need_show = true;
                break;
            }
            if (preset->inherits() != "")
                continue;
            if (!preset->base_id.empty())
                filament_with_base_id = true;

            if (!not_need_show) {
                auto filament_vendor = dynamic_cast<ConfigOptionStrings*>(
                    const_cast<Preset*>(preset)->config.option("filament_vendor", false));
                if (filament_vendor && filament_vendor->values.size() && filament_vendor->values[0] == "Generic")
                    not_need_show = true;
            }

            if (filament_name.empty()) {
                std::string preset_name = preset->name;
                size_t      index_at    = preset_name.find(" @");
                if (std::string::npos != index_at) {
                    preset_name = preset_name.substr(0, index_at);
                }
                filament_name = preset_name;
            }
        }
        if (not_need_show)
            continue;
        if (!filament_name.empty()) {
            if (filament_with_base_id) {
                need_sort.push_back(std::make_pair(into_u8(_L("[Action Required] ")) + filament_name, filament_id));
            } else {
                need_sort.push_back(std::make_pair(filament_name, filament_id));
            }
        }
    }
    std::sort(need_sort.begin(), need_sort.end(),
              [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) { return a.first < b.first; });
    if (need_delete_some_filament) {
        need_sort.push_back(std::make_pair(into_u8(_L("[Action Required]")), "null"));
    }
    json temp_j;
    for (std::pair<std::string, std::string>& filament_name_to_id : need_sort) {
        temp_j["name"] = filament_name_to_id.first;
        temp_j["id"]   = filament_name_to_id.second;
        m_CustomFilaments.push_back(temp_j);
    }
    m_Res["data"]  = m_CustomFilaments;
    wxString strJS = wxString::Format("HandleStudio(%s)", wxString::FromUTF8(m_Res.dump(-1, ' ', false, json::error_handler_t::ignore)));
    return strJS;
}

GuideFrame::GuideFrame(GUI_App* pGUI, long style)
    : DPIDialog((wxWindow*) (pGUI->mainframe), wxID_ANY, "OrcaSlicer", wxDefaultPosition, wxDefaultSize, style), m_appconfig_new()
{
    SetBackgroundColour(*wxWHITE);
    // INI
    m_SectionName    = "firstguide";
    PrivacyUse       = false;
    StealthMode      = false;
    InstallNetplugin = false;

    m_MainPtr = pGUI;

    // set the frame icon
    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    wxString TargetUrl = SetStartPage(BBL_WELCOME, false);
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  set start page to welcome ");

    // Create the webview
    m_browser = WebView::CreateWebView(this, TargetUrl);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
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

    int     screenheight = wxSystemSettings::GetMetric(wxSYS_SCREEN_Y, NULL);
    int     screenwidth  = wxSystemSettings::GetMetric(wxSYS_SCREEN_X, NULL);
    int     MaxY         = (screenheight - pSize.y) > 0 ? (screenheight - pSize.y) / 2 : 0;
    wxPoint tmpPT((screenwidth - pSize.x) / 2, MaxY);
    Move(tmpPT);
#ifdef __WXMSW__
    this->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& e) {
        if ((m_page == BBL_FILAMENT_ONLY || m_page == BBL_MODELS_ONLY) && e.GetKeyCode() == WXK_ESCAPE) {
            if (this->IsModal())
                this->EndModal(wxID_CANCEL);
            else
                this->Close();
        } else
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
    m_destroy = true;
    if (m_load_task && m_load_task->joinable()) {
        m_load_task->join();
        delete m_load_task;
        m_load_task = nullptr;
    }
    if (m_browser) {
        delete m_browser;
        m_browser = nullptr;
    }
}

void GuideFrame::load_url(wxString& url)
{
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " enter, url=" << url.ToStdString();
    WebView::LoadUrl(m_browser, url);
    m_browser->SetFocus();
    UpdateState();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " exit";
}

wxString GuideFrame::SetStartPage(GuidePage startpage, bool load)
{
    m_page = startpage;
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" enter, load=%1%, start_page=%2%") % load % int(startpage);
    // wxLogMessage("GUIDE: webpage_1  %s", (boost::filesystem::path(resources_dir()) /
    // "web\\guide\\1\\index.html").make_preferred().string().c_str() );
    wxString TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=1").make_preferred().string());
    // wxLogMessage("GUIDE: webpage_2  %s", TargetUrl.mb_str());

    if (startpage == BBL_WELCOME) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=1").make_preferred().string());
    } else if (startpage == BBL_REGION) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=11").make_preferred().string());
    } else if (startpage == BBL_MODELS) {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=21").make_preferred().string());
    } else if (startpage == BBL_FILAMENTS) {
        SetTitle(_L("Setup Wizard"));

        int nSize = static_cast<int>(m_guide_profile.model.size());

        if (nSize > 0)
            TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=22").make_preferred().string());
        else
            TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=21").make_preferred().string());
    } else if (startpage == BBL_FILAMENT_ONLY) {
        SetTitle("");
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=23").make_preferred().string());
    } else if (startpage == BBL_MODELS_ONLY) {
        SetTitle("");
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=24").make_preferred().string());
    } else {
        SetTitle(_L("Setup Wizard"));
        TargetUrl = from_u8((boost::filesystem::path(resources_dir()) / "web/guide/0/index.html?target=21").make_preferred().string());
    }

    wxString strlang = wxGetApp().current_language_code_safe();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", strlang=%1%") % into_u8(strlang);
    if (strlang != "")
        TargetUrl = wxString::Format("%s&lang=%s", w2s(TargetUrl), strlang);

    TargetUrl = "file://" + TargetUrl;
    if (load)
        load_url(TargetUrl);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " exit";
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

void GuideFrame::OnIdle(wxIdleEvent& WXUNUSED(evt))
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
void GuideFrame::OnNavigationRequest(wxWebViewEvent& evt)
{
    // wxLogMessage("%s", "Navigation request to '" + evt.GetURL() + "'
    // (target='" + evt.GetTarget() + "')");

    UpdateState();
}

/**
 * Callback invoked when a navigation request was accepted
 */
void GuideFrame::OnNavigationComplete(wxWebViewEvent& evt)
{
    // wxLogMessage("%s", "Navigation complete; url='" + evt.GetURL() + "'");
    if (!bFirstComplete) {
        m_load_task = new boost::thread(boost::bind(&GuideFrame::LoadProfileData, this));
        // boost::thread LoadProfileThread(boost::bind(&GuideFrame::LoadProfileData, this));
        // LoadProfileThread.detach();

        bFirstComplete = true;
    }

    m_browser->Show();
    Layout();

    wxString NewUrl = evt.GetURL();

    UpdateState();
}

/**
 * Callback invoked when a page is finished loading
 */
void GuideFrame::OnDocumentLoaded(wxWebViewEvent& evt)
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
void GuideFrame::OnNewWindow(wxWebViewEvent& evt)
{
    wxString flag = " (other)";

    wxString NewUrl = evt.GetURL();
    wxLaunchDefaultBrowser(NewUrl);
    // if (evt.GetNavigationAction() == wxWEBVIEW_NAV_ACTION_USER) { flag = " (user)"; }
    //  wxLogMessage("%s", "New window; url='" + evt.GetURL() + "'" + flag);

    // If we handle new window events then just load them in this window as we
    // are a single window browser
    // if (m_tools_handle_new_window->IsChecked())
    //    m_browser->LoadURL(evt.GetURL());

    UpdateState();
}

void GuideFrame::OnTitleChanged(wxWebViewEvent& evt)
{
    // SetTitle(evt.GetString());
    // wxLogMessage("%s", "Title changed; title='" + evt.GetString() + "'");
}

void GuideFrame::OnFullScreenChanged(wxWebViewEvent& evt)
{
    // wxLogMessage("Full screen changed; status = %d", evt.GetInt());
    ShowFullScreen(evt.GetInt() != 0);
}

void GuideFrame::OnScriptMessage(wxWebViewEvent& evt)
{
    try {
        wxString strInput = evt.GetString();
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;OnRecv:" << strInput.c_str();
        json j = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;Command:" << strCmd;

        if (strCmd == "close_page") {
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
        } else if (strCmd == "request_userguide_profile") {
            json m_Res           = json::object();
            m_Res["command"]     = "response_userguide_profile";
            m_Res["sequence_id"] = "10001";
            m_Res["response"]    = guide_profile_to_json(m_guide_profile);

            // wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', false, json::error_handler_t::ignore));
            wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', true));

            BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;request_userguide_profile:" << strJS.c_str();
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        } else if (strCmd == "request_custom_filaments") {
            wxString strJS = update_custom_filaments();
            wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
        } else if (strCmd == "create_custom_filament") {
            this->EndModal(wxID_OK);
            wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_CREATE_FILAMENT));
        } else if (strCmd == "modify_custom_filament") {
            m_editing_filament_id = j["id"];
            this->EndModal(wxID_EDIT);
        } else if (strCmd == "save_userguide_models") {
            json MSelected = j["data"];

            for (GuideModel& model : m_guide_profile.model) {
                model.nozzle_selected = "";

                for (auto it = MSelected.begin(); it != MSelected.end(); ++it) {
                    json OneSelect = it.value();

                    wxString s1 = model.model;
                    wxString s2 = OneSelect["model"];
                    if (s1.compare(s2) == 0) {
                        model.nozzle_selected = model.nozzle_diameter;

                        // Automatically select default materials for this printer model
                        // This mirrors the behavior of the old ConfigWizard::select_default_materials_for_printer_model()
                        std::string materials_str = join_materials(model.materials);
                        if (!materials_str.empty()) {
                            boost::trim(materials_str);
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Processing default_materials for printer: " << s1.ToStdString()
                                                    << " - materials: " << materials_str;

                            // Use the same parsing logic as ConfigWizard::select_default_materials_for_printer_model()
                            // This calls unescape_strings_cstyle() just like Preset.cpp:298 does
                            std::vector<std::string> materials;
                            if (Slic3r::unescape_strings_cstyle(materials_str, materials)) {
                                for (const std::string& material : materials) {
                                    if (!material.empty()) {
                                        // Mark this filament as selected if it exists in our filament list
                                        // This mirrors appconfig_new.set(section, material, "true") from ConfigWizard.cpp:2150
                                        auto filament_it = m_guide_profile.filament.find(material);
                                        if (filament_it != m_guide_profile.filament.end()) {
                                            filament_it->second.selected = 1;
                                            BOOST_LOG_TRIVIAL(info)
                                                << __FUNCTION__ << " Automatically selected default filament: " << material;
                                        } else {
                                            BOOST_LOG_TRIVIAL(warning)
                                                << __FUNCTION__ << " Default filament '" << material
                                                << "' not found in available filaments for printer: " << s1.ToStdString();
                                        }
                                    }
                                }
                            } else {
                                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Malformed default_materials field: " << materials_str
                                                         << " for printer: " << s1.ToStdString();
                            }
                        } else {
                            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " No default_materials defined for printer: " << s1.ToStdString();
                        }
                        break;
                    }
                }
            }
        } else if (strCmd == "save_userguide_filaments") {
            // reset
            for (auto& [name, filament] : m_guide_profile.filament) {
                filament.selected = 0;
            }

            json fSelected = j["data"]["filament"];
            int  nF        = fSelected.size();
            for (int m = 0; m < nF; m++) {
                std::string fName = fSelected[m];

                auto filament_it = m_guide_profile.filament.find(fName);
                if (filament_it != m_guide_profile.filament.end())
                    filament_it->second.selected = 1;
            }
        } else if (strCmd == "user_guide_finish") {
            SaveProfile();

            std::string oldregion = m_guide_profile.region;
            bool        bLogin    = false;
            if (m_Region != oldregion) {
                AppConfig*    config       = GUI::wxGetApp().app_config;
                std::string   country_code = config->get_country_code();
                NetworkAgent* agent        = wxGetApp().getAgent();
                if (agent) {
                    agent->set_country_code(country_code);
                    if (wxGetApp().is_user_login()) {
                        bLogin = true;
                        agent->user_logout();
                    }
                }
            }

            this->EndModal(wxID_OK);

            if (InstallNetplugin)
                GUI::wxGetApp().CallAfter([this] { GUI::wxGetApp().ShowDownNetPluginDlg(); });

            if (bLogin)
                GUI::wxGetApp().CallAfter([this] { login(); });
        } else if (strCmd == "user_guide_cancel") {
            this->EndModal(wxID_CANCEL);
            this->Close();
        } else if (strCmd == "save_region") {
            m_Region = j["region"];
        } else if (strCmd == "network_plugin_install") {
            std::string sAction = j["data"]["action"];

            if (sAction == "yes") {
                if (!network_plugin_ready)
                    InstallNetplugin = true;
                else // already ready
                    InstallNetplugin = false;
            } else
                InstallNetplugin = false;
        } else if (strCmd == "save_stealth_mode") {
            wxString strAction = j["data"]["action"];

            if (strAction == "yes") {
                StealthMode = true;
            } else {
                StealthMode = false;
            }
        }
    } catch (std::exception& e) {
        // wxMessageBox(e.what(), "json Exception", MB_OK);
        BOOST_LOG_TRIVIAL(trace) << "GuideFrame::OnScriptMessage;Error:" << e.what();
    }
}

void GuideFrame::RunScript(const wxString& javascript)
{
    // Remember the script we run in any case, so the next time the user opens
    // the "Run Script" dialog box, it is shown there for convenient updating.
    // m_javascript = javascript;

    // wxLogMessage("Running JavaScript:\n%s\n", javascript);

    if (!m_browser)
        return;

    WebView::RunScript(m_browser, javascript);
}

#if wxUSE_WEBVIEW_IE
void GuideFrame::OnRunScriptObjectWithEmulationLevel(wxCommandEvent& WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var person = new Object();person.name = 'Foo'; \
    person.lastName = 'Bar';return person;}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void GuideFrame::OnRunScriptDateWithEmulationLevel(wxCommandEvent& WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){var d = new Date('10/08/2017 21:30:40'); \
    var tzoffset = d.getTimezoneOffset() * 60000; return \
    new Date(d.getTime() - tzoffset);}f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}

void GuideFrame::OnRunScriptArrayWithEmulationLevel(wxCommandEvent& WXUNUSED(evt))
{
    wxWebViewIE::MSWSetModernEmulationLevel();
    RunScript("function f(){ return [\"foo\", \"bar\"]; }f();");
    wxWebViewIE::MSWSetModernEmulationLevel(false);
}
#endif

/**
 * Callback invoked when a loading error occurs
 */
void GuideFrame::OnError(wxWebViewEvent& evt)
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

void GuideFrame::OnScriptResponseMessage(wxCommandEvent& WXUNUSED(evt)) {}

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
    // privacy
    // if (PrivacyUse == true) {
    //     m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "privacyuse", "1");
    // } else
    //     m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "privacyuse", "0");

    m_MainPtr->app_config->set("region", m_Region);
    m_MainPtr->app_config->set_bool("stealth_mode", StealthMode);

    // finish
    m_MainPtr->app_config->set(std::string(m_SectionName.mb_str()), "finish", "1");

    m_MainPtr->app_config->save();

    std::string strAll = guide_profile_to_json(m_guide_profile).dump(-1, ' ', false, json::error_handler_t::ignore);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "before save to app_config: " << std::endl << strAll;

    // set filaments to app_config
    const std::string&                 section_name = AppConfig::SECTION_FILAMENTS;
    std::map<std::string, std::string> section_new;
    m_appconfig_new.clear_section(section_name);
    for (const auto& [name, filament] : m_guide_profile.filament) {
        if (filament.selected == 1) {
            section_new[name] = "true";
        }
    }
    m_appconfig_new.set_section(section_name, section_new);

    // set vendors to app_config
    Slic3r::AppConfig::VendorMap empty_vendor_map;
    m_appconfig_new.set_vendors(empty_vendor_map);
    for (const GuideModel& model : m_guide_profile.model) {
        std::string selected = model.nozzle_selected;
        boost::trim(selected);
        std::string nozzle;
        while (selected.size() > 0) {
            auto pos = selected.find(';');
            if (pos != std::string::npos) {
                nozzle = selected.substr(0, pos);
                m_appconfig_new.set_variant(model.vendor, model.model, nozzle, "true");
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__
                                        << boost::format("vendor_name %1%, model_name %2%, nozzle %3% selected") % model.vendor %
                                               model.model % nozzle;
                selected = selected.substr(pos + 1);
                boost::trim(selected);
            } else {
                m_appconfig_new.set_variant(model.vendor, model.model, selected, "true");
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__
                                        << boost::format("vendor_name %1%, model_name %2%, nozzle %3% selected") % model.vendor %
                                               model.model % selected;
                break;
            }
        }
    }

    // m_appconfig_new

    return 0;
}

static std::set<std::string> get_new_added_presets(const std::map<std::string, std::string>& old_data,
                                                   const std::map<std::string, std::string>& new_data)
{
    auto get_aliases = [](const std::map<std::string, std::string>& data) {
        std::set<std::string> old_aliases;
        for (auto item : data) {
            const std::string& name = item.first;
            size_t             pos  = name.find("@");
            old_aliases.emplace(pos == std::string::npos ? name : name.substr(0, pos - 1));
        }
        return old_aliases;
    };

    std::set<std::string> old_aliases = get_aliases(old_data);
    std::set<std::string> new_aliases = get_aliases(new_data);
    std::set<std::string> diff;
    std::set_difference(new_aliases.begin(), new_aliases.end(), old_aliases.begin(), old_aliases.end(), std::inserter(diff, diff.begin()));

    return diff;
}

static std::string get_first_added_preset(const std::map<std::string, std::string>& old_data,
                                          const std::map<std::string, std::string>& new_data)
{
    std::set<std::string> diff = get_new_added_presets(old_data, new_data);
    if (diff.empty())
        return std::string();
    return *diff.begin();
}

bool GuideFrame::apply_config(AppConfig* app_config, PresetBundle* preset_bundle, const PresetUpdater* updater, bool& apply_keeped_changes)
{
    const auto enabled_vendors     = m_appconfig_new.vendors();
    const auto old_enabled_vendors = app_config->vendors();

    const auto enabled_filaments     = m_appconfig_new.has_section(AppConfig::SECTION_FILAMENTS) ?
                                           m_appconfig_new.get_section(AppConfig::SECTION_FILAMENTS) :
                                           std::map<std::string, std::string>();
    const auto old_enabled_filaments = app_config->has_section(AppConfig::SECTION_FILAMENTS) ?
                                           app_config->get_section(AppConfig::SECTION_FILAMENTS) :
                                           std::map<std::string, std::string>();

    bool                     check_unsaved_preset_changes = false;
    std::vector<std::string> install_bundles;
    std::vector<std::string> remove_bundles;
    const auto               vendor_dir = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
    for (const auto& it : enabled_vendors) {
        if (it.second.size() > 0) {
            auto vendor_file = vendor_dir / (it.first + ".json");
            if (!fs::exists(vendor_file)) {
                install_bundles.emplace_back(it.first);
            }
        }
    }

    // add the removed vendor bundles
    for (const auto& it : old_enabled_vendors) {
        if (it.second.size() > 0) {
            if (enabled_vendors.find(it.first) != enabled_vendors.end())
                continue;
            auto vendor_file = vendor_dir / (it.first + ".json");
            if (fs::exists(vendor_file)) {
                remove_bundles.emplace_back(it.first);
            }
        }
    }

    check_unsaved_preset_changes = (enabled_vendors != old_enabled_vendors) || (enabled_filaments != old_enabled_filaments);
    wxString header              = _L("The configuration package is changed in previous Config Guide");
    wxString caption             = _L("Configuration package changed");
    int      act_btns            = ActionButtons::KEEP | ActionButtons::SAVE;

    if (check_unsaved_preset_changes && !wxGetApp().check_and_keep_current_preset_changes(caption, header, act_btns, &apply_keeped_changes))
        return false;

    if (install_bundles.size() > 0) {
        // Install bundles from resources.
        // Don't create snapshot - we've already done that above if applicable.
        if (!updater->install_bundles_rsrc(std::move(install_bundles), false))
            return false;
    } else {
        BOOST_LOG_TRIVIAL(info) << "No bundles need to be installed from resource directory";
    }

    // Not remove, because these bundles may be updated
    // if (remove_bundles.size() > 0) {
    //    //remove unused bundles
    //    for (const auto &it : remove_bundles) {
    //        auto vendor_file = vendor_dir/(it + ".json");
    //        auto sub_dir = vendor_dir/(it);
    //        if (fs::exists(vendor_file))
    //            fs::remove(vendor_file);
    //        if (fs::exists(sub_dir))
    //            fs::remove_all(sub_dir);
    //    }
    //} else {
    //    BOOST_LOG_TRIVIAL(info) << "No bundles need to be removed";
    //}

    std::string       preferred_model;
    std::string       preferred_variant;
    PrinterTechnology preferred_pt   = ptFFF;
    auto get_preferred_printer_model = [preset_bundle, enabled_vendors, old_enabled_vendors, preferred_pt](const std::string& bundle_name,
                                                                                                           std::string&       variant) {
        const auto config = enabled_vendors.find(bundle_name);
        if (config == enabled_vendors.end())
            return std::string();

        const VendorProfile&                                printer_profile = preset_bundle->vendors[bundle_name];
        const std::map<std::string, std::set<std::string>>& model_maps      = config->second;
        // for (const auto& vendor_profile : preset_bundle->vendors) {
        for (const auto& model_it : model_maps) {
            if (model_it.second.size() > 0) {
                variant = *model_it.second.begin();
                if (model_it.second.size() > 1) {
                    if (printer_profile.models.size() > 0) {
                        const VendorProfile::PrinterModel& printer_model = *std::find_if(printer_profile.models.begin(),
                                                                                         printer_profile.models.end(),
                                                                                         [id = model_it.first](auto& m) {
                                                                                             return m.id == id;
                                                                                         });
                        for (auto& vt : printer_model.variants) {
                            if (std::find(model_it.second.begin(), model_it.second.end(), vt.name) != model_it.second.end()) {
                                variant = vt.name;
                                break;
                            }
                        }
                    } else if (variant != PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT) {
                        if (std::find(model_it.second.begin(), model_it.second.end(), PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT) !=
                            model_it.second.end())
                            variant = PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT;
                    }
                }

                const auto config_old = old_enabled_vendors.find(bundle_name);
                if (config_old == old_enabled_vendors.end())
                    return model_it.first;
                const auto model_it_old = config_old->second.find(model_it.first);
                if (model_it_old == config_old->second.end())
                    return model_it.first;
                else if (model_it_old->second != model_it.second) {
                    for (const auto& var : model_it.second)
                        if (model_it_old->second.find(var) == model_it_old->second.end()) {
                            variant = var;
                            return model_it.first;
                        }
                }
            }
        }
        //}
        if (!variant.empty())
            variant.clear();
        return std::string();
    };
    // Orca "custom" printers are considered first, then 3rd party.
    if (preferred_model = get_preferred_printer_model(PresetBundle::ORCA_DEFAULT_BUNDLE, preferred_variant); preferred_model.empty()) {
        for (const auto& bundle : enabled_vendors) {
            if (bundle.first == PresetBundle::ORCA_DEFAULT_BUNDLE) {
                continue;
            }
            if (preferred_model = get_preferred_printer_model(bundle.first, preferred_variant); !preferred_model.empty())
                break;
        }
    }

    std::string first_added_filament;
    auto        get_first_added_material_preset = [this, app_config](const std::string& section_name, std::string& first_added_preset) {
        if (m_appconfig_new.has_section(section_name)) {
            // get first of new added preset names
            const std::map<std::string, std::string>& old_presets = app_config->has_section(section_name) ?
                                                                        app_config->get_section(section_name) :
                                                                        std::map<std::string, std::string>();
            first_added_preset = get_first_added_preset(old_presets, m_appconfig_new.get_section(section_name));
        }
    };
    // Not switch filament
    // get_first_added_material_preset(AppConfig::SECTION_FILAMENTS, first_added_filament);

    // update the app_config
    app_config->set_section(AppConfig::SECTION_FILAMENTS, enabled_filaments);
    app_config->set_vendors(m_appconfig_new);

    if (check_unsaved_preset_changes)
        preset_bundle->load_presets(*app_config, ForwardCompatibilitySubstitutionRule::Enable,
                                    {preferred_model, preferred_variant, first_added_filament, std::string()});

    // If the active filament is not in the wizard-selected filaments, switch to the first
    // compatible wizard-selected filament. This handles the first-run case where load_presets
    // falls back to "Generic PLA" even though the user selected a different filament.
    bool active_filament_selected = enabled_filaments.empty() || enabled_filaments.count(preset_bundle->filament_presets.front()) > 0;
    if (!active_filament_selected) {
        for (const auto& [filament_name, _] : enabled_filaments) {
            const Preset* preset = preset_bundle->filaments.find_preset(filament_name);
            if (preset && preset->is_visible && preset->is_compatible) {
                preset_bundle->filaments.select_preset_by_name(filament_name, true);
                preset_bundle->filament_presets.front() = preset_bundle->filaments.get_selected_preset_name();
                break;
            }
        }
    }

    // Update the selections from the compatibilty.
    preset_bundle->export_selections(*app_config);

    return true;
}

bool GuideFrame::run()
{
    // BOOST_LOG_TRIVIAL(info) << boost::format("Running ConfigWizard, reason: %1%, start_page: %2%") % reason % start_page;

    GUI_App& app = wxGetApp();

    // p->set_run_reason(reason);
    // p->set_start_page(start_page);
    app.preset_bundle->export_selections(*app.app_config);

    BOOST_LOG_TRIVIAL(info) << "GuideFrame before ShowModal";
    // display position
    int main_frame_display_index = wxDisplay::GetFromWindow(wxGetApp().mainframe);
    int guide_display_index      = wxDisplay::GetFromWindow(this);
    if (main_frame_display_index != guide_display_index) {
        wxDisplay display    = wxDisplay(main_frame_display_index);
        wxRect    screenRect = display.GetGeometry();
        int       guide_x    = screenRect.x + (screenRect.width - this->GetSize().GetWidth()) / 2;
        int       guide_y    = screenRect.y + (screenRect.height - this->GetSize().GetHeight()) / 2;
        this->SetPosition(wxPoint(guide_x, guide_y));
    }

    int result = this->ShowModal();
    if (result == wxID_OK) {
        bool apply_keeped_changes = false;
        BOOST_LOG_TRIVIAL(info) << "GuideFrame returned ok";
        if (!this->apply_config(app.app_config, app.preset_bundle, app.preset_updater, apply_keeped_changes))
            return false;

        if (apply_keeped_changes)
            app.apply_keeped_preset_modifications();

        app.app_config->set_legacy_datadir(false);
        app.update_mode();
        // BBS
        // app.obj_manipul()->update_ui_from_settings();
        BOOST_LOG_TRIVIAL(info) << "GuideFrame applied";
        this->Close();
        return true;
    } else if (result == wxID_CANCEL) {
        BOOST_LOG_TRIVIAL(info) << "GuideFrame cancelled";
        if (app.preset_bundle->printers.only_default_printers()) {
            // we install the default here
            bool apply_keeped_changes = false;
            // clear filament section and use default materials
            app.app_config->set_variant(PresetBundle::ORCA_DEFAULT_BUNDLE, PresetBundle::ORCA_DEFAULT_PRINTER_MODEL,
                                        PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT, "true");
            app.app_config->clear_section(AppConfig::SECTION_FILAMENTS);
            app.preset_bundle->load_selections(*app.app_config,
                                               {PresetBundle::ORCA_DEFAULT_PRINTER_MODEL, PresetBundle::ORCA_DEFAULT_PRINTER_VARIANT,
                                                PresetBundle::ORCA_DEFAULT_FILAMENT, std::string()});

            app.app_config->set_legacy_datadir(false);
            app.update_mode();
            return true;
        } else
            return false;
    } else if (result == wxID_EDIT) {
        this->Close();
        Filamentinformation* filament_info = new Filamentinformation();
        filament_info->filament_id         = m_editing_filament_id;
        wxQueueEvent(wxGetApp().plater(), new SimpleEvent(EVT_MODIFY_FILAMENT, filament_info));
        return false;
    } else
        return false;
}

int GuideFrame::LoadProfileData()
{
    const auto start_time = std::chrono::steady_clock::now();
    try {
        m_guide_profile  = GuideProfile{};
        m_orca_fila_list = OrcaFilaList{};

        vendor_dir      = (boost::filesystem::path(Slic3r::data_dir()) / PRESET_SYSTEM_DIR).make_preferred();
        rsrc_vendor_dir = (boost::filesystem::path(resources_dir()) / "profiles").make_preferred();

        // Orca: add custom as default
        // Orca: add json logic for vendor bundle
        orca_bundle_rsrc = true;

        // search if there exists a .json file in vendor_dir folder, if exists, set orca_bundle_rsrc to false
        for (const auto& entry : boost::filesystem::directory_iterator(vendor_dir)) {
            if (!boost::filesystem::is_directory(entry) && boost::iequals(entry.path().extension().string(), ".json") &&
                !boost::iequals(entry.path().stem().string(), PresetBundle::ORCA_FILAMENT_LIBRARY)) {
                orca_bundle_rsrc = false;
                break;
            }
        }

        // load the default filament library first
        std::set<std::string> loaded_vendors;
        auto filament_library_name = boost::filesystem::path(PresetBundle::ORCA_FILAMENT_LIBRARY).replace_extension(".json");
        if (boost::filesystem::exists(vendor_dir / filament_library_name)) {
            m_OrcaFilaLibPath = (vendor_dir / PresetBundle::ORCA_FILAMENT_LIBRARY).string();
            LoadProfileFamily(PresetBundle::ORCA_FILAMENT_LIBRARY, (vendor_dir / filament_library_name).string());
        } else {
            m_OrcaFilaLibPath = (rsrc_vendor_dir / PresetBundle::ORCA_FILAMENT_LIBRARY).string();
            LoadProfileFamily(PresetBundle::ORCA_FILAMENT_LIBRARY, (rsrc_vendor_dir / filament_library_name).string());
        }
        loaded_vendors.insert(PresetBundle::ORCA_FILAMENT_LIBRARY);

        // load custom bundle from user data path
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
            if (m_destroy)
                return 0;
        }

        boost::filesystem::directory_iterator others_endIter;
        for (boost::filesystem::directory_iterator iter(rsrc_vendor_dir); iter != others_endIter; iter++) {
            if (!boost::filesystem::is_directory(*iter)) {
                wxString strVendor    = from_u8(iter->path().string()).BeforeLast('.');
                strVendor             = strVendor.AfterLast('\\');
                strVendor             = strVendor.AfterLast('/');
                wxString strExtension = from_u8(iter->path().string()).AfterLast('.').Lower();
                if (strExtension.CmpNoCase("json") != 0 || loaded_vendors.find(w2s(strVendor)) != loaded_vendors.end())
                    continue;

                LoadProfileFamily(w2s(strVendor), iter->path().string());
                loaded_vendors.insert(w2s(strVendor));
            }
            if (m_destroy)
                return 0;
        }

        wxGetApp().CallAfter([this] {
            if (!m_destroy) {
                // Sync selections from app config into the typed guide state before notifying the webview.
                SaveProfileData();
                RunScript(R"(HandleStudio({"command":"userguide_profile_load_finish","sequence_id":"10001"}))");
            }
        });
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", error: " << e.what() << std::endl;
    }

    const auto end_time     = std::chrono::steady_clock::now();
    const auto elapsed_ms   = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    const auto elapsed_secs = std::chrono::duration_cast<std::chrono::duration<double>>(end_time - start_time).count();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " completed in " << elapsed_ms << " ms (" << elapsed_secs << " s)";

    return 0;
}

int GuideFrame::SaveProfileData()
{
    try {
        const auto enabled_filaments = wxGetApp().app_config->has_section(AppConfig::SECTION_FILAMENTS) ?
                                           wxGetApp().app_config->get_section(AppConfig::SECTION_FILAMENTS) :
                                           std::map<std::string, std::string>();
        m_appconfig_new.set_vendors(*wxGetApp().app_config);
        m_appconfig_new.set_section(AppConfig::SECTION_FILAMENTS, enabled_filaments);

        for (auto& model : m_guide_profile.model) {
            std::string model_name      = model.model;
            std::string vendor_name     = model.vendor;
            std::string nozzle_diameter = model.nozzle_diameter;
            std::string selected;
            boost::trim(nozzle_diameter);
            std::string nozzle;
            bool        enabled = false, first = true;
            while (nozzle_diameter.size() > 0) {
                auto pos = nozzle_diameter.find(';');
                if (pos != std::string::npos) {
                    nozzle  = nozzle_diameter.substr(0, pos);
                    enabled = m_appconfig_new.get_variant(vendor_name, model_name, nozzle);
                    if (enabled) {
                        if (!first)
                            selected += ";";
                        selected += nozzle;
                        first = false;
                    }
                    nozzle_diameter = nozzle_diameter.substr(pos + 1);
                    boost::trim(nozzle_diameter);
                } else {
                    enabled = m_appconfig_new.get_variant(vendor_name, model_name, nozzle_diameter);
                    if (enabled) {
                        if (!first)
                            selected += ";";
                        selected += nozzle_diameter;
                    }
                    break;
                }
            }
            model.nozzle_selected = selected;
        }

        if (m_guide_profile.model.size() == 1) {
            std::string strNozzle                    = m_guide_profile.model[0].nozzle_diameter;
            m_guide_profile.model[0].nozzle_selected = strNozzle;
        }

        for (auto& [key, filament] : m_guide_profile.filament) {
            if (enabled_filaments.find(key) != enabled_filaments.end())
                m_guide_profile.filament[key].selected = 1;
        }

        //----region
        m_Region               = wxGetApp().app_config->get("region");
        m_guide_profile.region = m_Region;

        m_guide_profile.network_plugin_install     = wxGetApp().app_config->get("app", "installed_networking");
        m_guide_profile.network_plugin_compability = wxGetApp().is_compatibility_version() ? "1" : "0";
        network_plugin_ready                       = wxGetApp().is_compatibility_version();

        StealthMode                  = wxGetApp().app_config->get_bool("app", "stealth_mode");
        m_guide_profile.stealth_mode = StealthMode;
    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ", error: " << e.what() << std::endl;
    }
    return 0;
}

void StringReplace(string& strBase, string strSrc, string strDes)
{
    string::size_type pos    = 0;
    string::size_type srcLen = strSrc.size();
    string::size_type desLen = strDes.size();
    pos                      = strBase.find(strSrc, pos);
    while ((pos != string::npos)) {
        strBase.replace(pos, srcLen, strDes);
        pos = strBase.find(strSrc, (pos + desLen));
    }
}

int GuideFrame::LoadProfileFamily(std::string strVendor, std::string strFilePath)
{
    boost::filesystem::path file_path(strFilePath);
    boost::filesystem::path vendor_dir = boost::filesystem::absolute(file_path.parent_path() / strVendor).make_preferred();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  vendor path %1%.") % vendor_dir.string();

    try {
        simdjson::dom::parser  parser;
        simdjson::dom::element jLocal = parser.load(strFilePath);
        simdjson::dom::object  root   = jLocal.get_object();

        simdjson::dom::array pmodels = root["machine_model_list"].get_array();
        int                  nsize   = pmodels.size();

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% machine models") % nsize;

        for (auto elem : pmodels) {
            simdjson::dom::object one_model = elem.get_object();
            std::string           s1        = simd_string_or_empty(one_model, "name");
            std::string           s2        = simd_string_or_empty(one_model, "sub_path");

            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path))
                continue;

            std::string           sub_file = sub_path.string();
            simdjson::dom::parser sub_parser;
            simdjson::dom::object pm = sub_parser.load(sub_file).get_object();

            std::string NozzleOpt = simd_string_or_empty(pm, "nozzle_diameter");
            StringReplace(NozzleOpt, " ", "");

            std::string             cover_file = s1 + "_cover.png";
            boost::filesystem::path cover_path = boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "/profiles/" /
                                                                             strVendor / cover_file)
                                                     .make_preferred();
            if (!boost::filesystem::exists(cover_path)) {
                cover_path = (boost::filesystem::absolute(boost::filesystem::path(resources_dir()) / "/web/image/printer/") / cover_file)
                                 .make_preferred();
            }

            GuideModel modelEntry;
            modelEntry.model           = s1;
            modelEntry.sub_path        = s2;
            modelEntry.name            = simd_string_or_empty(pm, "name");
            modelEntry.vendor          = strVendor;
            modelEntry.nozzle_diameter = NozzleOpt;
            modelEntry.materials       = {simd_string_or_empty(pm, "default_materials")};
            modelEntry.cover           = cover_path.string();
            modelEntry.nozzle_selected = "";

            m_guide_profile.model.emplace_back(modelEntry);
        }

        simdjson::dom::array pmachine = root["machine_list"].get_array();
        nsize                         = pmachine.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% machines") % nsize;
        for (auto elem : pmachine) {
            simdjson::dom::object one_machine = elem.get_object();
            std::string           s1          = simd_string_or_empty(one_machine, "name");
            std::string           s2          = simd_string_or_empty(one_machine, "sub_path");

            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path))
                continue;

            simdjson::dom::parser sub_parser;
            simdjson::dom::object pm = sub_parser.load(sub_path.string()).get_object();

            std::string strInstant = simd_string_or_empty(pm, "instantiation");
            if (strInstant.compare("true") == 0) {
                GuideMachine machineEntry;
                machineEntry.name     = s1;
                machineEntry.sub_path = s2;
                machineEntry.model    = simd_string_or_empty(pm, "printer_model");
                machineEntry.nozzle   = simd_first_array_string_or_empty(pm, "nozzle_diameter");
                m_guide_profile.machine[s1] = machineEntry;
            }
        }

        simdjson::dom::array                         pFilament = root["filament_list"].get_array();
        std::unordered_map<std::string, std::string> filament_paths;
        OrcaFilaList                                 orca_fila_list = m_orca_fila_list;
        for (const auto& [name, filament_ref] : m_orca_fila_list) {
            filament_paths[name] = filament_ref.sub_path;
        }

        nsize = pFilament.size();

        for (auto elem : pFilament) {
            simdjson::dom::object one_filament = elem.get_object();
            std::string           s1           = simd_string_or_empty(one_filament, "name");
            std::string           s2           = simd_string_or_empty(one_filament, "sub_path");

            filament_paths[s1] = s2;
            OrcaFilamentRef filamentRef;
            filamentRef.sub_path = s2;
            orca_fila_list[s1] = filamentRef;
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Vendor: " << strVendor << ", tFilaList Add: " << s1;
        }

        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% filaments") % nsize;
        for (auto elem : pFilament) {
            simdjson::dom::object one_filament = elem.get_object();
            std::string           s1           = simd_string_or_empty(one_filament, "name");
            std::string           s2           = simd_string_or_empty(one_filament, "sub_path");

            if (m_guide_profile.filament.find(s1) == m_guide_profile.filament.end()) {
                boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
                if (!boost::filesystem::exists(sub_path))
                    continue;

                std::string           sub_file = sub_path.string();
                simdjson::dom::parser sub_parser;
                simdjson::dom::object pm = sub_parser.load(sub_file).get_object();

                std::string strInstant = simd_string_or_empty(pm, "instantiation");
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "Load Filament:" << s1 << ",Path:" << sub_file << ",instantiation?"
                                        << strInstant;

                if (strInstant == "true") {
                    std::string sV;
                    std::string sT;

                    int nRet = get_filament_info_simd(vendor_dir.string(), filament_paths, m_OrcaFilaLibPath, sub_file, sV, sT);
                    if (nRet != 0) {
                        BOOST_LOG_TRIVIAL(info)
                            << __FUNCTION__ << "Load Filament:" << s1 << ",GetFilamentInfo Failed, Vendor:" << sV << ",Type:" << sT;
                        continue;
                    }

                    std::string ModelList           = "";
                    auto        compatible_printers = pm["compatible_printers"].get_array();
                    if (!compatible_printers.error()) {
                        for (auto printer : compatible_printers.value()) {
                            std::string sP = simd_string_or_empty(printer);
                            if (m_guide_profile.machine.find(sP) != m_guide_profile.machine.end()) {
                                std::string mModel   = m_guide_profile.machine[sP].model;
                                std::string mNozzle  = m_guide_profile.machine[sP].nozzle;
                                std::string NewModel = mModel + "++" + mNozzle;

                                ModelList = (boost::format("%1%[%2%]") % ModelList % NewModel).str();
                            }
                        }
                    }

                    GuideFilament filamentEntry;
                    filamentEntry.name     = s1;
                    filamentEntry.sub_path = s2;
                    filamentEntry.vendor   = sV;
                    filamentEntry.type     = sT;
                    filamentEntry.models   = ModelList;
                    filamentEntry.selected = 0;
                    m_guide_profile.filament[s1] = filamentEntry;
                } else
                    continue;
            }
        }
        if (strVendor == PresetBundle::ORCA_FILAMENT_LIBRARY)
            m_orca_fila_list = orca_fila_list;

        simdjson::dom::array pProcess = root["process_list"].get_array();
        nsize                         = pProcess.size();
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(",  got %1% processes") % nsize;
        for (auto elem : pProcess) {
            simdjson::dom::object one_process = elem.get_object();
            std::string           s1          = simd_string_or_empty(one_process, "name");
            std::string           s2          = simd_string_or_empty(one_process, "sub_path");

            boost::filesystem::path sub_path = boost::filesystem::absolute(vendor_dir / s2).make_preferred();
            if (!boost::filesystem::exists(sub_path))
                continue;

            simdjson::dom::parser sub_parser;
            simdjson::dom::object pm = sub_parser.load(sub_path.string()).get_object();

            std::string bInstall = simd_string_or_empty(pm, "instantiation");
            if (bInstall == "true") {
                GuideProcess processEntry;
                processEntry.name     = s1;
                processEntry.sub_path = s2;
                m_guide_profile.process.emplace_back(processEntry);
            }
        }
    } catch (const simdjson::simdjson_error& err) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << strFilePath << " got a simdjson error, reason = " << err.what();
        return -1;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": parse " << strFilePath << " got exception: " << e.what();
        return -1;
    }

    return 0;
}

void GuideFrame::StrReplace(std::string& strBase, std::string strSrc, std::string strDes)
{
    int pos    = 0;
    int srcLen = strSrc.size();
    int desLen = strDes.size();
    pos        = strBase.find(strSrc, pos);
    while ((pos != std::string::npos)) {
        strBase.replace(pos, srcLen, strDes);
        pos = strBase.find(strSrc, (pos + desLen));
    }
}

std::string GuideFrame::w2s(wxString sSrc) { return std::string(sSrc.mb_str()); }

void GuideFrame::GetStardardFilePath(std::string& FilePath)
{
    StrReplace(FilePath, "\\", w2s(wxString::Format("%c", boost::filesystem::path::preferred_separator)));
    StrReplace(FilePath, "/", w2s(wxString::Format("%c", boost::filesystem::path::preferred_separator)));
}

int GuideFrame::DownloadPlugin()
{
    return wxGetApp().download_plugin(
        "plugins", "network_plugin.zip",
        [this](int status, int percent, bool& cancel) { return ShowPluginStatus(status, percent, cancel); }, nullptr);
}

int GuideFrame::InstallPlugin()
{
    return wxGetApp().install_plugin("plugins", "network_plugin.zip",
                                     [this](int status, int percent, bool& cancel) { return ShowPluginStatus(status, percent, cancel); });
}

int GuideFrame::ShowPluginStatus(int status, int percent, bool& cancel)
{
    // TODO
    return 0;
}

}} // namespace Slic3r::GUI
