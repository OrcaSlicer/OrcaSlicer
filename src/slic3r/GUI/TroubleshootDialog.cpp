#include "TroubleshootDialog.hpp"
#include "I18N.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"

#include <wx/display.h>
#include <wx/wfstream.h>
#include "wx/clipbrd.h"

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"

#include <nlohmann/json.hpp>

#ifdef __WINDOWS__
#include <windows.h>
#include <VersionHelpers.h>
#include <sysinfoapi.h>
#endif

#ifdef __LINUX__
#include <wx/textfile.h>
#include <wx/tokenzr.h>
#include <cstdlib>
#endif

#ifdef __APPLE__
#include <wx/regex.h>
#define Rect     Mac_Rect
#define RectPtr  Mac_RectPtr
#define Point    Mac_Point
#define Size     Mac_Size
#include <CoreGraphics/CoreGraphics.h>
#undef Rect
#undef RectPtr
#undef Point
#undef Size
#endif

#include "NetworkTestDialog.hpp"

#include "Widgets/StaticLine.hpp"
#include "Widgets/HyperLink.hpp"
#include "Widgets/Button.hpp"

namespace Slic3r {
namespace GUI {

wxFlexGridSizer* TroubleshootDialog::create_item_loaded_profiles()
{
    auto create_label = [this](wxString title, wxFont font = Label::Body_14) {
        auto label = new Label(this, font, title);
        label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        return label;
    };

    auto g_sizer = new wxFlexGridSizer(1, 6, FromDIP(3), FromDIP(15));

    g_sizer->AddSpacer(0);
    g_sizer->Add(create_label(_L("Active"), Label::Body_12), 0, wxALIGN_CENTER);
    g_sizer->AddSpacer(0);
    g_sizer->Add(create_label(_L("System"), Label::Body_12), 0, wxALIGN_CENTER);
    g_sizer->AddSpacer(0);
    g_sizer->Add(create_label(_L("User"), Label::Body_12)  , 0, wxALIGN_CENTER);

    auto gen_stats = GetProfilesOverview();
    gen_stats      = ""; // clear mem. not needed after generating m_..._act, m_..._usr variables
   
    auto add_sizer = [this, g_sizer, create_label](PresetCollection* col, wxString label, int in_use, int user) {
        int sys = 0;
        for (auto it = col->begin(); it != col->end(); it++) {
            if (it->is_system)
                sys++;
        }
        g_sizer->Add(create_label(label));
        g_sizer->Add(create_label(wxString::Format("%d", in_use)), 0, wxALIGN_CENTER);
        g_sizer->Add(create_label("/", Label::Body_12)           , 0, wxALIGN_CENTER);
        g_sizer->Add(create_label(wxString::Format("%d", sys   )), 0, wxALIGN_CENTER);
        g_sizer->Add(create_label("+", Label::Body_12)           , 0, wxALIGN_CENTER);
        g_sizer->Add(create_label(wxString::Format("%d", user  )), 0, wxALIGN_CENTER);
    };

    auto preset_bundle = wxGetApp().preset_bundle;
    add_sizer(&preset_bundle->printers , _L("Printers") , m_printers__act, m_printers__usr);
    add_sizer(&preset_bundle->filaments, _L("Filaments"), m_filaments_act, m_filaments_usr);
    add_sizer(&preset_bundle->prints   , _L("Processes"), m_processes_act, m_processes_usr);

    return g_sizer;
}

ComboBox *TroubleshootDialog::create_item_log_level_combo()
{
    auto combobox = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);

    for (const auto& item : std::vector<wxString>{_L("fatal"), _L("error"), _L("warning"), _L("info"), _L("debug"), _L("trace")})
        combobox->Append(item);

    auto severity_level = wxGetApp().app_config->get("log_severity_level");
    if (!severity_level.empty()) { combobox->SetValue(severity_level); }

    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [](wxCommandEvent &e) {
        auto level = Slic3r::get_string_logging_level(e.GetSelection());
        Slic3r::set_logging_level(Slic3r::level_string_to_boost(level));
        wxGetApp().app_config->set("log_severity_level",level);
        e.Skip();
     });

    return combobox;
}

TroubleshootDialog::TroubleshootDialog()
    : DPIDialog(static_cast<wxWindow*>(wxGetApp().mainframe), wxID_ANY, _L("Troubleshoot Center"),
    wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
{
    SetFont(wxGetApp().normal_font());
    SetBackgroundColour(*wxWHITE);

    auto data_dir   = boost::filesystem::path(Slic3r::data_dir());
    auto app_config = wxGetApp().app_config;
    bool is_dark    = app_config->get("dark_color_mode") == "1";
 
    // LEFT SIZER //////////////////////

    // HEADER
    m_logo            = ScalableBitmap(this, is_dark ? "OrcaSlicer_horizontal_dark" : "OrcaSlicer_horizontal_light", 64);
    m_header_logo     = new wxStaticBitmap(this, wxID_ANY, m_logo.bmp());
    auto logo_line    = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(2)));
    logo_line->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#009687")));
    auto version      = new Label(this, wxString(SoftFever_VERSION), wxALIGN_CENTRE_HORIZONTAL);
    wxFont version_font = GetFont();
    version_font = version_font.Scaled(1.65f); // SetPointSize(18) not works on macOS because it uses a 72 PPI reference
    version->SetFont(version_font);
    version->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));

    auto build = new Button(this, wxString(GIT_COMMIT_HASH));
    build->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    auto hash_url = "https://github.com/OrcaSlicer/OrcaSlicer/commit/" + wxString(GIT_COMMIT_HASH);
    build->SetToolTip(hash_url);
    build->Bind(wxEVT_BUTTON, [hash_url](wxCommandEvent &e) {
         wxLaunchDefaultBrowser(hash_url);
    });

    // SYSTEM INFO
    auto sys_info_lines = [this](bool show_sys) {
        if(!show_sys)
            return std::vector<wxString>{GetOStype()};
        return std::vector<wxString>{
            GetOSinfo(),
            GetPackageType(),
            GetCPUinfo(),
            GetRAMinfo() + " RAM",
            GetGPUinfo(),
            GetMONinfo()
        };
    };

    auto* sys_panel = new CenteredMultiLinePanel(this, sys_info_lines(true));
    sys_panel->SetBackgroundColour(*wxWHITE);
    sys_panel->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));

    auto sys_less_btn = new Button(this, _L("Hide"));
    sys_less_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    sys_less_btn->SetToolTip(_L("Show/Hide system information"));

    auto sys_copy_btn = new Button(this, _L("Copy"));
    sys_copy_btn->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    sys_copy_btn->SetToolTip(_L("Copy system information to clipboard"));

    sys_copy_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        wxClipboardLocker lock;
        if (!lock)
            return false;
        return wxTheClipboard->SetData(new wxTextDataObject(GetSysInfoAll()));
    });

    sys_less_btn->Bind(wxEVT_BUTTON, [this, sys_panel, sys_less_btn, sys_info_lines, sys_copy_btn](wxCommandEvent &e) {
        m_sys_panel_mode = !m_sys_panel_mode;
        sys_panel->SetText(sys_info_lines(m_sys_panel_mode));
        sys_less_btn->SetLabel(m_sys_panel_mode ? _L("Hide") : _L("Show"));
        Layout();
        Fit();
    });

    auto link_wiki = new HyperLink(this, _L("Wiki Guide"), "https://www.orcaslicer.com/wiki/troubleshoot_center");

    // RIGHT SIZER //////////////////////

    auto create_title = [this](wxString title) {
        auto line = new StaticLine(this, false, title);
        line->SetFont(Label::Head_16);
        line->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        return line;
    };

    auto create_label = [this](wxString title, wxString tooltip, Label** label_out = nullptr) {
        wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
        auto label = new Label(this, title, LB_AUTO_WRAP, wxSize(FromDIP(275),-1)); // 400 - 120 - 5
        label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        label->SetToolTip(tooltip);
        sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(15));

        if (label_out) *label_out = label;

        return sizer;
    };

    auto create_btn = [this](wxString title, wxString tooltip) {
        auto btn = new Button(this, title);
        btn->SetToolTip(tooltip);
        btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
        return btn;
    };

    // INFORMATION
    auto create_info_line = [this](wxString title) {
        auto info = new Label(this, title);
        info->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#363636")));
        info->SetMaxSize(wxSize(FromDIP(400),-1));
        info->Wrap(FromDIP(400));
        return info;
    };

    auto info_desc_1 = create_info_line(_L("We need information for diagnosing source of the issue. Check wiki page for detailed guide."));
    auto info_desc_2 = create_info_line(_L("Pack button collects project file and logs of current session onto a zip file."));
    auto info_desc_3 = create_info_line(_L("Any additional visual examples like images or screen recordings might be helpful while reporting the issue."));
    wxBoxSizer *info_desc_sizer = new wxBoxSizer(wxVERTICAL);
    info_desc_sizer->Add(info_desc_1, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    info_desc_sizer->Add(info_desc_2, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    info_desc_sizer->Add(info_desc_3, 0, wxEXPAND);

    auto link_report  = new HyperLink(this, _L("Report issue") + " ");
    link_report->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        auto encodeStr = [](const wxString& text) {
            wxString out;
            const wxScopedCharBuffer utf8 = text.utf8_str();
            for (const unsigned char* p = (const unsigned char*)utf8.data(); *p; ++p) {
                unsigned char c = *p;
                if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    out += (wxChar)c;
                else
                    out += wxString::Format(wxT("%%%02X"), c);
            }
            return out;
        };

        wxString url = "https://github.com/OrcaSlicer/OrcaSlicer/issues/new?template=bug_report.yml";
        wxString os = GetOStype();
        if(!os.IsEmpty())
            url += "&os_type=%22" + os +"%22";
        url += "&version="     + encodeStr(wxString(SoftFever_VERSION));
        url += "&os_version="  + encodeStr(GetOSinfo());
        wxLaunchDefaultBrowser(url);
    });

    auto pack_btn = new Button(this, _L("Pack") + "...");
    pack_btn->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    pack_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        PackAll();
    });

    // PROFILES
    auto prf_sys_cache_tip = _L("Cleans and rebuilds system profiles cache on next launch.");
    auto prf_sys_cache_szr = create_label(_L("Clean system profiles cache"), prf_sys_cache_tip);
    auto prf_sys_cache_btn = create_btn(_L("Clean"), prf_sys_cache_tip);
    prf_sys_cache_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        RebuildSystemProfiles();
    });
    prf_sys_cache_szr->Add(prf_sys_cache_btn, 0, wxALIGN_CENTER_VERTICAL);

    auto prf_loaded_szr = create_label(_L("Loaded profiles overview"), _L("This section shows information for loaded profiles."));
    auto prf_loaded_btn = create_btn(_L("Export") + dots, _L("Exports detailed overview of loaded profiles in json format."));
    prf_loaded_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        return ExportAsJson(GetProfilesOverview(),"ProfilesOverview");
    });
    prf_loaded_szr->Add(prf_loaded_btn, 0, wxALIGN_CENTER_VERTICAL);

    auto profiles_loaded = create_item_loaded_profiles();

    // MORE
    auto cfg_folder_szr = create_label(_L("Configurations folder"), "");
    auto cfg_folder_btn = create_btn(_L("Browse") + "...", _L("Opens configurations folder."));
    cfg_folder_btn->Bind(wxEVT_BUTTON, [this, data_dir](wxCommandEvent &e) {
        BrowseFolder(data_dir.string());
    });
    cfg_folder_szr->Add(cfg_folder_btn, 0, wxALIGN_CENTER_VERTICAL);

    auto log_level_szr = create_label(_L("Log level"), "");
    log_level_szr->Add(create_item_log_level_combo(), 0, wxALIGN_CENTER_VERTICAL);

    auto log_pack_szr = create_label(_L("Stored logs"), _L("Packs all stored logs onto a zip file."));
    auto log_pack_btn = create_btn(_L("Pack") + "...", "");
    log_pack_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        auto data_dir   = boost::filesystem::path(Slic3r::data_dir());
        ExportAsZip({wxString((data_dir / "log").string())}, "OrcaSlicer_Logs_" + GetTimestamp());
    });
    log_pack_szr->Add(log_pack_btn, 0, wxALIGN_CENTER_VERTICAL);

    auto log_clear_szr = create_label("-", "", &m_logs_storage);
    auto log_clear_btn = create_btn(_L("Clear"), "");
    log_clear_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        ClearLogs();
    });
    log_clear_szr->Add(log_clear_btn, 0, wxALIGN_CENTER_VERTICAL);

    UpdateLogsStorage();

    // LAYOUT //////////////////////
    wxBoxSizer *left_sizer  = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *sys_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    sys_btn_sizer->Add(sys_less_btn, 0, wxLEFT | wxRIGHT, FromDIP(5));
    sys_btn_sizer->AddStretchSpacer();
    sys_btn_sizer->Add(sys_copy_btn, 0, wxLEFT | wxRIGHT, FromDIP(5));

    left_sizer->Add(m_header_logo     , 0, wxEXPAND | wxALIGN_CENTER);
    left_sizer->Add(logo_line         , 0, wxEXPAND       | wxTOP, FromDIP(12));
    left_sizer->Add(version           , 0, wxEXPAND       | wxTOP, FromDIP(6));
    left_sizer->Add(build             , 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
    left_sizer->Add(sys_panel         , 0, wxEXPAND       | wxTOP, FromDIP(15));
    left_sizer->AddStretchSpacer();
    left_sizer->Add(sys_btn_sizer     , 0, wxEXPAND       | wxTOP, FromDIP(15));
    left_sizer->Add(link_wiki         , 0, wxALIGN_CENTER | wxTOP, FromDIP(15));
    left_sizer->AddSpacer(FromDIP(5));
    
    wxBoxSizer *right_sizer  = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer *pack_btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    pack_btn_sizer->Add(link_report                   , 0, wxRIGHT, FromDIP(5));
    pack_btn_sizer->AddStretchSpacer();
    pack_btn_sizer->Add(pack_btn);

    right_sizer->Add(create_title(_L("Information")), 0, wxEXPAND);
    right_sizer->Add(info_desc_sizer                , 0, wxEXPAND | wxTOP, FromDIP(5));
    right_sizer->Add(pack_btn_sizer                 , 0, wxEXPAND | wxTOP, FromDIP(12));

    right_sizer->Add(create_title(_L("Profiles"))   , 0, wxEXPAND | wxTOP, FromDIP(12));
    right_sizer->Add(prf_sys_cache_szr              , 0, wxEXPAND | wxTOP, FromDIP(8));
    right_sizer->Add(prf_loaded_szr                 , 0, wxEXPAND | wxTOP, FromDIP(5));
    right_sizer->Add(profiles_loaded                , 0, wxEXPAND | wxTOP, FromDIP(5));
    
    right_sizer->Add(create_title(_L("More"))       , 0, wxEXPAND | wxTOP, FromDIP(12));
    right_sizer->Add(cfg_folder_szr                 , 0, wxEXPAND | wxTOP, FromDIP(8));
    right_sizer->Add(log_level_szr                  , 0, wxEXPAND | wxTOP, FromDIP(5));
    right_sizer->Add(log_pack_szr                   , 0, wxEXPAND | wxTOP, FromDIP(5));
    right_sizer->Add(log_clear_szr                  , 0, wxEXPAND | wxTOP, FromDIP(5));

    wxBoxSizer *m_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_sizer->Add(left_sizer , 0, wxEXPAND | wxTOP | wxBOTTOM | wxLEFT , FromDIP(15));
    m_sizer->AddSpacer(FromDIP(20));
    m_sizer->Add(right_sizer, 0, wxEXPAND | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(15));

    SetSizer(m_sizer);
    Layout();
    Fit();
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

wxString TroubleshootDialog::GetTimestamp()
{
    wxDateTime now = wxDateTime::Now();
    return now.Format("%Y%m%d_%H%M"); // %S
}

wxString TroubleshootDialog::GetSysInfoAll()
{
    wxString info;
    info += "Version   :  " + wxString(SoftFever_VERSION) + "\n"
          + "Build     :  " + wxString(GIT_COMMIT_HASH)   + "\n"
          + "Package   :  " + GetPackageType() + "\n"
          + "Platform  :  " + GetOSinfo()      + "\n"
          + "Processor :  " + GetCPUinfo() + "\n"
          + "Memory    :  " + GetRAMinfo() + "RAM" + "\n"
          + "Renderer  :  " + GetGPUinfo() + "\n"
          + "Monitors  :  " + GetMONinfo();
    return info;
};

/*
wxString TroubleshootDialog::GetConfigStr()
{
    wxString config_path = wxGetApp().app_config->config_path();
    std::ifstream file(config_path.ToUTF8().data());
    if (!file.is_open())
        return "{}";
    nlohmann::json root;
    try {
        file >> root;
    } catch (const nlohmann::json::exception&) {
        return "{}";
    }
    for (const auto& key : std::vector<std::string>{"recent_projects", "recent", "custom_color_list", "orca_presets"})
        root.erase(key);
    if (root.contains("app")) {
        for (const auto& key : std::vector<std::string>{"last_backup_path", "last_export_path", "download_path", "slicer_uuid", "preset_folder"})
            root["app"].erase(key);
    }
    return wxString::FromUTF8(root.dump(4));
};
*/

wxString TroubleshootDialog::GetProfilesOverview()
{
    using ojson = nlohmann::ordered_json;

    auto preset_bundle = wxGetApp().preset_bundle;
    auto app_config    = wxGetApp().app_config;
    auto vendors       = app_config->vendors();

    ojson root = ojson::object();

    m_printers__act = 0; m_printers__usr = 0;
    m_filaments_act = 0; m_filaments_usr = 0;
    m_processes_act = 0; m_processes_usr = 0;

    { // OVERVIEW
        root["Overview"] = "";
    }
    { // PRINTERS - enabled
        ojson arr = ojson::array();
        for (const auto& [vendor_name, models] : vendors)
            for (const auto& [model_name, _] : models){
                arr.push_back(model_name);
                m_printers__act++;
            }
        root["printers_enabled"] = arr;
    }
    { // PRINTERS - user defined
        ojson arr = ojson::array();
        for (auto it = preset_bundle->printers.begin(); it != preset_bundle->printers.end(); ++it) {
            if (!it->is_user()) continue;
            ojson entry;
            entry["name"]     = it->name;
            entry["inherits"] = it->inherits();
            arr.push_back(entry);
            m_printers__usr++;
        }
        root["printers_user"] = arr;
    }
    { // FILAMENTS - enabled
        if(app_config->has_section("filaments")){
            auto filaments = app_config->get_section("filaments");
            if(!filaments.empty()){
                ojson arr = ojson::array();
                for (const auto& filament : app_config->get_section("filaments")){
                    arr.push_back(filament.first);
                    m_filaments_act++;
                }
                root["filaments_enabled"] = arr;
            }
        }
    }
    { // FILAMENTS - user defined
        ojson arr = ojson::array();
        for (auto it = preset_bundle->filaments.begin(); it != preset_bundle->filaments.end(); ++it) {
            if (!it->is_user()) continue;
            ojson entry;
            entry["name"]        = it->name;
            entry["inherits"]    = it->inherits();
            auto *compatible_printers = dynamic_cast<const ConfigOptionStrings*>(it->config.option("compatible_printers"));
            if(compatible_printers != nullptr && !compatible_printers->values.empty()){
                ojson c_arr = ojson::array();
                for (const auto&  c_item : compatible_printers->values)
                    c_arr.push_back(c_item);
                entry["compatible_printers"] = c_arr;
            }
            auto *compatible_processes = dynamic_cast<const ConfigOptionStrings*>(it->config.option("compatible_prints"));
            if(compatible_processes != nullptr && !compatible_processes->values.empty()){
                ojson c_arr = ojson::array();
                for (const auto&  c_item : compatible_processes->values)
                    c_arr.push_back(c_item);
                entry["compatible_processes"] = c_arr;
            }
            arr.push_back(entry);
            m_filaments_usr++;
        }
        root["filaments_user"] = arr;
    }
    { // PROCESSES - enabled, grouped by compatible printer
        ojson obj = ojson::object();
        for (auto it = preset_bundle->printers.begin(); it != preset_bundle->printers.end(); ++it) {
            if (!it->is_visible || !it->is_compatible)
                continue;
            ojson arr = ojson::array();
            for (auto jt = preset_bundle->prints.begin(); jt != preset_bundle->prints.end(); ++jt) {
                if (!jt->is_visible || jt->is_user() || !jt->loaded) continue;
                auto *compatible_printers = dynamic_cast<const ConfigOptionStrings*>(jt->config.option("compatible_printers"));
                bool is_compatible = false;
                if (compatible_printers == nullptr || compatible_printers->values.empty()) {
                    is_compatible = false;
                }
                else {
                    for (const auto& p : compatible_printers->values) {
                        if (p == it->name) {
                            is_compatible = true;
                            break;
                        }
                    }
                }
                if (!is_compatible)
                    continue;
                arr.push_back(jt->name);
                m_processes_act++;
            }
            if (!arr.empty())
                obj[it->name] = arr;
        }
        root["processes_enabled"] = obj;
    }
    { // PROCESSES - user defined
        ojson arr = ojson::array();
        for (auto it = preset_bundle->prints.begin(); it != preset_bundle->prints.end(); ++it) {
            if (!it->is_user()) continue;
            ojson entry;
            entry["name"]        = it->name;
            entry["inherits"]    = it->inherits();
            auto *compatible_printers = dynamic_cast<const ConfigOptionStrings*>(it->config.option("compatible_printers"));
            if(compatible_printers != nullptr && !compatible_printers->values.empty()){
                ojson c_arr = ojson::array();
                for (const auto&  c_item : compatible_printers->values)
                    c_arr.push_back(c_item);
                entry["compatible_printers"] = c_arr;
            }
            arr.push_back(entry);
            m_processes_usr++;
        }
        root["processes_user"] = arr;
    }
    { // OVERVIEW
        ojson entry;
        entry["printers__act"] = m_printers__act;
        entry["printers__usr"] = m_printers__usr;
        entry["filaments_act"] = m_filaments_act;
        entry["filaments_usr"] = m_filaments_usr;
        entry["processes_act"] = m_processes_act;
        entry["processes_usr"] = m_processes_usr;
        root["Overview"] = entry;
    }
    return wxString::FromUTF8(root.dump(4));
}

wxString TroubleshootDialog::GetOStype()
{
    wxString os = "";
    #ifdef __WINDOWS__
        os = "Windows";
    #elif defined(__LINUX__)
        os = "Linux";
    #elif defined(__APPLE__)
        os = "macOS";
    #endif
    return os;
}

wxString TroubleshootDialog::GetOSinfo()
{
    wxString result;
#ifdef __WINDOWS__
    result = GetWinVersion();
#elif defined(__LINUX__)
    result = GetLinuxDistroName() + " " + GetLinuxDisplayServer();
#elif defined(__APPLE__)
    result = wxGetOsDescription();      // returns "macOS Version 26.3 (Build 25D125)"
    result.Replace("Version ", "");     // simplify naming
    result.Replace("Build ", "Build-"); // dash for wrapping build info on next line

    // No public API for naming
    auto GetMacOSName = [](const wxString& ver) -> wxString {
        if (ver.StartsWith("26")) return "Tahoe";
        if (ver.StartsWith("15")) return "Sequoia";
        if (ver.StartsWith("14")) return "Sonoma";
        if (ver.StartsWith("13")) return "Ventura";
        if (ver.StartsWith("12")) return "Monterey";
        if (ver.StartsWith("11")) return "Big Sur";
        return "";
    };

    wxRegEx reVer("([0-9]+\\.[0-9]+)");
    if (reVer.Matches(result)) {
        wxString ver  = reVer.GetMatch(result, 1);
        wxString name = GetMacOSName(ver);
        if (!name.IsEmpty())
            result.Replace("(Build-", name + " (Build-");
    }
#endif
    return result;
}

#ifdef __WINDOWS__
wxString TroubleshootDialog::GetWinDisplayVersion() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return wxString();

    wchar_t buf[64] = {};
    DWORD size = sizeof(buf);
    DWORD type = 0;

    // "DisplayVersion" exists on Windows 10 20H2+ and Windows 11
    LONG res = RegQueryValueExW(hKey, L"DisplayVersion", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
    if (res != ERROR_SUCCESS || type != REG_SZ) { // Fallback: older builds use "ReleaseId" (e.g. "2004", "1909")
        size = sizeof(buf);
        RegQueryValueExW(hKey, L"ReleaseId", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size);
    }

    RegCloseKey(hKey);
    return wxString(buf);
}
wxString TroubleshootDialog::GetWinVersion()
{
    typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (hNtdll) {
        auto RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hNtdll, "RtlGetVersion");
        if (RtlGetVersion) {
            RTL_OSVERSIONINFOW osvi = {};
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            if (RtlGetVersion(&osvi) == 0) {
                int build = osvi.dwBuildNumber;
                wxString win = (build >= 22000) ? "11" 
                             : (build >= 10240) ? "10"
                             : (build >= 9200)  ? "8"
                             : (build >= 7601)  ? "7"
                             : "?";

                wxString displayVer = GetWinDisplayVersion();

                if (!displayVer.IsEmpty())
                    return wxString::Format("Windows %s %s %d", win, displayVer, build);
                else
                    return wxString::Format("Windows %s %d", win, build);
            }
        }
    }
    return "Windows (unknown)";
}
#elif defined(__LINUX__)
wxString TroubleshootDialog::GetLinuxDistroName()
{
    // Try host os-release first (works when running as Flatpak)
    wxArrayString candidates;
    candidates.Add("/run/host/os-release");
    candidates.Add("/run/host/etc/os-release");
    candidates.Add("/etc/os-release");
    candidates.Add("/usr/lib/os-release");

    for (const wxString& path : candidates) {
        if (!wxFileExists(path))
            continue;
        wxTextFile file;
        if (!file.Open(path))
            continue;
        for (wxString line = file.GetFirstLine(); !file.Eof(); line = file.GetNextLine()) {
            if (line.StartsWith("PRETTY_NAME=")) {
                wxString value = line.Mid(12);
                value.Replace("\"", "");
                return value;
            }
        }
    }
    return "Linux";
}

wxString TroubleshootDialog::GetLinuxDisplayServer()
{
    const char* wayland = getenv("WAYLAND_DISPLAY");
    if (wayland && wayland[0] != '\0') // WAYLAND_DISPLAY is set when running under Wayland
        return "Wayland";

    const char* sessionType = getenv("XDG_SESSION_TYPE");
    if (sessionType) { // XDG_SESSION_TYPE is more explicit
        if (wxString(sessionType).IsSameAs("wayland", false))
            return "Wayland";
        if (wxString(sessionType).IsSameAs("x11", false))
            return "X11";
    }

    const char* display = getenv("DISPLAY");
    if (display && display[0] != '\0')
        return "X11"; // DISPLAY being set suggests X11

    return "";
}
#endif

wxString TroubleshootDialog::GetPackageType()
{
    wxString result;
#ifdef __WINDOWS__
    wxString path = wxStandardPaths::Get().GetExecutablePath();
    wxString dir  = wxPathOnly(path);

    if (path.Contains("OrcaSlicer\\build"))
        return "Local Build";

    if (wxFileExists(dir + "\\Uninstall.exe"))
        return "Installed";

    return "Portable";
#elif defined(__LINUX__)
    if (wxGetEnv("APPIMAGE"  , nullptr))   return "AppImage";
    if (wxGetEnv("FLATPAK_ID", nullptr))   return "Flatpak";
    //if (wxGetEnv("SNAP"      , nullptr)) return "Snap";
    if (wxFileExists("/.flatpak-info"))    return "Flatpak";
    //if (wxFileExists("/usr/bin/dpkg"))   return "Debian/Ubuntu (deb)";
    //if (wxFileExists("/usr/bin/rpm"))    return "RPM-based (rpm)";
    //if (wxFileExists("/usr/bin/pacman")) return "Arch (pacman)";

    wxString path = wxStandardPaths::Get().GetExecutablePath();
    if (path.Contains("OrcaSlicer/build")) return "Local Build";
    //if (path.StartsWith("/usr/local"))   return "Compiled (local)";
    if (path.StartsWith("/opt"))           return "Third-party";

    return "Native Package"; // (deb/rpm/etc)
#elif defined(__APPLE__)
    wxString path = wxStandardPaths::Get().GetExecutablePath();
    wxString dir  = wxPathOnly(path);

    if (path.Contains("OrcaSlicer/build"))
        return "Local Build";

    //if (wxDirExists(dir + "/../_MASReceipt"))
    //  return "Mac App Store";

    if (path.Contains("/Cellar/") || wxGetEnv("HOMEBREW_PREFIX", nullptr))
        return "Homebrew";

    if (path.StartsWith("/Volumes/OrcaSlicer")) // running from .dmg
        return "Temporary";

    if (path.StartsWith("/Applications"))
        return "Applications";

    return "Custom"; // running from Downloads, Desktop or custom folder
#endif
    return result;
}

wxString TroubleshootDialog::GetCPUinfo()
{
    wxString info;
#ifdef __WINDOWS__
    info = get_cpu_info_from_registry();
#elif __APPLE__
    std::map<std::string, std::string> cpu_info = parse_lscpu_etc("sysctl -a", ':');
    info = wxString(cpu_info["machdep.cpu.brand_string"]);
#else // linux/BSD
    std::map<std::string, std::string> cpu_info = parse_lscpu_etc("cat /proc/cpuinfo", ':');
    info = wxString(cpu_info["model name"]);
#endif
    info.Trim();
    return info;
}

#ifdef __WINDOWS__
wxString TroubleshootDialog::get_cpu_info_from_registry()
{
    const std::string dir = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\";
    char buf[500] = {};
    DWORD bufsize = sizeof(buf) - 1;

    for (const auto& path : { dir, dir + "0\\" })
        if (RegGetValueA(HKEY_LOCAL_MACHINE, path.c_str(), "ProcessorNameString", RRF_RT_REG_SZ, NULL, buf, &bufsize) == ERROR_SUCCESS)
            return buf;

    return "Unknown";
}
#else // macOS / linux
std::map<std::string, std::string> TroubleshootDialog::parse_lscpu_etc(const std::string& name, char delimiter)
{
    std::map<std::string, std::string> out;
    constexpr size_t max_len = 1000;
    char cline[max_len] = "";
    FILE* fp = popen(name.data(), "r");
    if (fp != NULL) {
        while (fgets(cline, max_len, fp) != NULL) {
            std::string line(cline);
            line.erase(std::remove_if(line.begin(), line.end(),
                [](char c) { return c == '\"' || c == '\r' || c == '\n'; }),
                line.end());
            size_t pos = line.find(delimiter);
            if (pos < line.size() - 1) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim_all(key); // remove leading and trailing spaces
                boost::trim_all(value);
                out.emplace(key, value);
            }
        }
        pclose(fp);
    }
    return out;
}
#endif

wxString TroubleshootDialog::GetRAMinfo()
{
    size_t n = std::round(Slic3r::total_physical_memory() / 107374182.40);
    return std::to_string(n / 10) + "." + std::to_string(n % 10) + " GB";
}

wxString TroubleshootDialog::GetGPUinfo()
{
    auto gl_info = OpenGLManager::get_gl_info();
#if !SLIC3R_OPENGL_ES
    return gl_info.get_renderer() + "  GLSL:" + gl_info.get_glsl_version() + (gl_info.is_core_profile() ? "  Core" : "  Compatibility");
#else
    return gl_info.get_renderer() + "  GLSL:" + gl_info.get_glsl_version() + "  ES";
#endif
}

wxString TroubleshootDialog::GetMONinfo()
{
    wxString m_str;
    int d_count = wxDisplay::GetCount();
    if (d_count <= 0)
        return "Unknown";
    double scale = 1.0;

#if defined(__LINUX__)
    for (int i = 0; i < d_count; ++i) {
        wxDisplay disp(i);
        if (!disp.IsOk()) continue;

        scale     = disp.GetScaleFactor();
        wxRect rc = disp.GetGeometry();

        wxString d_str = wxString::Format("%dx%d-%.0f%%", rc.width, rc.height, scale * 100.0);

        m_str += ((i > 0) ? "  " : "") + d_str;
    }
#elif defined(__APPLE__)
    std::vector<CGDirectDisplayID> cgDisplays;
    uint32_t displayCount = 0;
    CGGetActiveDisplayList(0, nullptr, &displayCount);
    if (displayCount > 0) {
        cgDisplays.resize(displayCount);
        CGGetActiveDisplayList(displayCount, cgDisplays.data(), &displayCount);
    }
    for (int i = 0; i < d_count; ++i) {
        wxDisplay disp(i);
        if (!disp.IsOk()) continue;
        scale     = disp.GetScaleFactor();
        wxRect rc = disp.GetGeometry();
        int physW = static_cast<int>(std::round(rc.width  * scale));
        int physH = static_cast<int>(std::round(rc.height * scale));
        const char* type = "L-"; // Logical fallback. only visible when cant get native res

        CGDirectDisplayID cgDispID = (i < static_cast<int>(cgDisplays.size())) ? cgDisplays[i] : CGMainDisplayID();

        CFArrayRef allModes = CGDisplayCopyAllDisplayModes(cgDispID, nullptr);
        if (allModes) {
            CFIndex count = CFArrayGetCount(allModes);
            for (CFIndex m = 0; m < count; ++m) {
                CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(allModes, m);
                if (CGDisplayModeGetIOFlags(mode) & kDisplayModeNativeFlag) {
                    size_t pw = CGDisplayModeGetPixelWidth(mode);
                    size_t ph = CGDisplayModeGetPixelHeight(mode);
                    if (pw >= 800 && ph >= 600 && pw <= 16384 && ph <= 16384) {
                        physW = static_cast<int>(pw);
                        physH = static_cast<int>(ph);
                        type = ""; // Native
                    }
                    break;
                }
            }
            CFRelease(allModes);
        }

        wxString d_str = wxString::Format("%dx%d-%s%.0f%%", physW, physH, type, scale * 100.0);
        m_str += ((i > 0) ? "  " : "") + d_str;
    }
#elif defined(__WINDOWS__)
    for (int i = 0; i < d_count; ++i) {
        wxDisplay disp(i);
        if (!disp.IsOk()) continue;

        scale     = disp.GetScaleFactor();
        wxRect rc = disp.GetGeometry();

        wxString d_str = wxString::Format("%dx%d-%.0f%%", rc.width, rc.height, scale * 100.0);

        m_str += ((i > 0) ? "  " : "") + d_str;
    }

    UINT dpi = 96;
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser) {
        auto fn = reinterpret_cast<UINT(WINAPI*)()>(GetProcAddress(hUser, "GetDpiForSystem"));
        if (fn)
            dpi = fn();
    }
    double text_scale = dpi / 96.0;
    m_str += wxString::Format("  TextScaling-%.0f%%", text_scale * 100.0);
#endif
    return m_str;
}

void TroubleshootDialog::PackAll()
{
    std::vector<wxString> include_zip;
    //auto data_dir   = boost::filesystem::path(Slic3r::data_dir());
    //include_zip.emplace_back(wxString((data_dir / "log").string()));

    // Collect logs for current session. does not includes debug_network_...
    boost::filesystem::path current_log = get_log_file_name();
    if (!current_log.empty()){
        boost::filesystem::path base = current_log.parent_path() / current_log.stem(); // removes ".0"
        std::vector<boost::filesystem::path> result;
        for (int n = 0; ; ++n) {
            auto candidate = boost::filesystem::path(base.string() + "." + std::to_string(n));
            if (!boost::filesystem::exists(candidate))
                break;
            include_zip.emplace_back(wxString(candidate.string()));
        }
    }

    auto project_name = wxGetApp().plater()->get_project_filename(".3mf");
    if(!project_name.IsEmpty()){
        if (wxGetApp().plater()->is_project_dirty()) {
            auto res = MessageDialog(this, 
                _L("The current project has unsaved changes. Would you like to save before continuing\?") +
                "\n\n" +
                _L("Select NO to close dialog and review project."),
                wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE
            ).ShowModal();
            if (res == wxID_YES)
                wxGetApp().plater()->save_project();
            else {
                if (res == wxID_NO)
                    EndModal(wxID_CLOSE);
                return;
            }
        }
        include_zip.emplace_back(project_name);
    }

    if (project_name.IsEmpty()){
        MessageDialog(this, _L("No project file on current session. Only logs will be included to package"),
            wxString(SLIC3R_APP_FULL_NAME), wxOK | wxICON_WARNING | wxCENTRE
        ).ShowModal();
    }

    ExportAsZip(include_zip, "OrcaSlicer_PackedDebugInfo_" + GetTimestamp());
}

void TroubleshootDialog::RebuildSystemProfiles()
{
    if (wxGetApp().plater()->is_project_dirty()) {
        auto res = MessageDialog(this, 
            _L("The current project has unsaved changes. Would you like to save before continuing\?") +
            "\n\n" +
            _L("Select NO to close dialog and review project."),
            wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE
        ).ShowModal();
        if (res == wxID_YES)
            wxGetApp().plater()->save_project();
        else {
            if (res == wxID_NO)
                EndModal(wxID_CLOSE);
            return;
        }
    }
    
    MessageDialog msg(this,
        _L("Restart Required") + "\n" +
        _L("Please make sure any instances of OrcaSlicer are not running") + "\n" +
        _L("Do you want to continue?")
        , wxString(SLIC3R_APP_FULL_NAME), wxICON_QUESTION | wxOK | wxCANCEL
    );
    if (msg.ShowModal() == wxID_OK){
        auto sys_folder = boost::filesystem::path(Slic3r::data_dir()) / "system";
        if (boost::filesystem::exists(sys_folder)) {
            bool is_deletable = true;
            try {
                for (const auto& entry : boost::filesystem::recursive_directory_iterator(sys_folder)) {
                    if (boost::filesystem::is_regular_file(entry.path())) {
                        std::ofstream file(entry.path().string(), std::ios::in | std::ios::out);
                        if (!file.is_open()) {
                            BOOST_LOG_TRIVIAL(warning) << "File is locked: " << entry.path().string();
                            is_deletable = false;
                        }
                        file.close();
                    }
                }
            }
            catch (const std::exception& e) {
                is_deletable = false;
                BOOST_LOG_TRIVIAL(warning) << e.what();
            }
            if (!is_deletable) {
                MessageDialog(this, _L("System folder cannot be deleted because some files are in use by another application. Please close any applications using these files and try again."),
                    wxString(SLIC3R_APP_FULL_NAME), wxOK | wxICON_WARNING | wxCENTRE
                ).ShowModal();
                return;
            }
            try {
                boost::filesystem::remove_all(sys_folder);
                EndModal(wxID_REMOVE);
                RestartApplication();
            }
            catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(warning) << "Failed to delete system folder..." << e.what();
                MessageDialog(this, _L("Failed to delete system folder..."),
                    wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Error"), wxOK | wxICON_WARNING | wxCENTRE
                ).ShowModal();
            }
        }
    }
}

bool TroubleshootDialog::RestartApplication()
{
    wxString execPath = wxStandardPaths::Get().GetExecutablePath();
    if (execPath.IsEmpty()) {
        MessageDialog(this,
            _L("Failed to determine executable path."),
            wxString(SLIC3R_APP_FULL_NAME),
            wxICON_WARNING | wxOK
        ).ShowModal();
        return false;
    }

    // Reconstruct original argv if you need to forward launch arguments,
    // e.g. "--datadir", profile flags, etc.
    // wxString args = "--some-flag";
    // wxString cmd  = wxString::Format(R"("%s" %s)", execPath, args);

#ifdef __WXMSW__
    wxString cmd = wxString::Format(R"("%s")", execPath);
    
    // CreateProcess needs a mutable buffer for lpCommandLine
    std::vector<wchar_t> cmdBuf(cmd.wc_str(), cmd.wc_str() + cmd.length() + 1);

    STARTUPINFO         si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcess(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        MessageDialog(this, _L("Failed to launch a new instance."), wxString(SLIC3R_APP_FULL_NAME), wxICON_WARNING | wxOK).ShowModal();
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    // macOS / Linux — wxExecute with ASYNC so it doesn't block
    wxString cmd = wxString::Format(R"("%s")", execPath);
    long pid = wxExecute(cmd, wxEXEC_ASYNC);
    if (pid == 0) {
        MessageDialog(this,
            _L("Failed to launch a new instance."),
            wxString(SLIC3R_APP_FULL_NAME),
            wxICON_WARNING | wxOK
        ).ShowModal();
        return false;
    }
#endif
    wxMilliSleep(500);

    // Close this instance cleanly
    wxGetApp().GetTopWindow()->Close(true); // triggers EVT_CLOSE, runs cleanup

    return true;
}

void TroubleshootDialog::ClearLogs()
{
    // Same method with GUI_App::post_init() only LOG_FILES_MAX_NUM replaced with 1
    auto data_dir = boost::filesystem::path(Slic3r::data_dir());
    auto log_folder = data_dir / "log";
    if (boost::filesystem::exists(log_folder)) {
       std::vector<std::pair<time_t, std::string>> files_vec;
       for (auto& it : boost::filesystem::directory_iterator(log_folder)) {
           auto temp_path = it.path();
           try {
               if (it.status().type() == boost::filesystem::regular_file) {
                   std::time_t lw_t = boost::filesystem::last_write_time(temp_path) ;
                   files_vec.push_back({ lw_t, temp_path.filename().string() });
               }
           } catch (const std::exception &) {
           }
       }
       std::sort(files_vec.begin(), files_vec.end(), [](
           std::pair<time_t, std::string> &a, std::pair<time_t, std::string> &b) {
           return a.first > b.first;
       });

       while (files_vec.size() > 1) {
           auto full_path = log_folder / boost::filesystem::path(files_vec[files_vec.size() - 1].second);
           BOOST_LOG_TRIVIAL(info) << "delete log file over " << LOG_FILES_MAX_NUM << ", filename: "<< files_vec[files_vec.size() - 1].second;
           try {
               boost::filesystem::remove(full_path);
           }
           catch (const std::exception& ex) {
               BOOST_LOG_TRIVIAL(error) << "failed to delete log file: "<< files_vec[files_vec.size() - 1].second << ". Error: " << ex.what();
           }
           files_vec.pop_back();
        }
    }
    UpdateLogsStorage();
}

void TroubleshootDialog::UpdateLogsStorage()
{
    boost::filesystem::path logs_path = boost::filesystem::path(Slic3r::data_dir()) / "log";
    
    uintmax_t total_bytes = 0;
    int file_count = 0;
    if (boost::filesystem::exists(logs_path) && boost::filesystem::is_directory(logs_path)) {
        for (const auto& entry : boost::filesystem::recursive_directory_iterator(logs_path)) {
            if (boost::filesystem::is_regular_file(entry.path())){
                total_bytes += boost::filesystem::file_size(entry.path());
                file_count++;
            }
        }
    }

    bool is_mb = total_bytes >= 1024 * 1024;
    wxString label = total_bytes > 0 ? wxString::Format("%d %s (%.2f %s)", file_count, _L("log(s)"), total_bytes / (1024.0 * (is_mb ? 1024.0 : 1.0)), is_mb ? "MB" : "KB") : "";
    m_logs_storage->SetLabel(label);
}

void TroubleshootDialog::BrowseFolder(std::string path)
{
    wxString wxpath = wxString::FromUTF8(path);

    if (!wxpath.IsEmpty() && !wxFileName::IsPathSeparator(wxpath.Last()))
        wxpath += wxFileName::GetPathSeparator();

    if (wxLaunchDefaultApplication(wxpath))
        return;

    auto ShellQuote = [](const wxString& arg) {
    #ifdef __WXMSW__
        wxString result = "\"";
        for (wxChar c : arg) {
            if (c == '"') result += '\\';
            result += c;
        }
        result += "\"";
        return result;
    #else
        wxString result = "'";
        for (wxChar c : arg) {
            if (c == '\'')
                result += wxString("'\\''");
            else
                result += c;
        }
        result += "'";
        return result;
    #endif
    };

    #ifdef _WIN32
        ::wxExecute(L"explorer.exe " + ShellQuote(wxpath), wxEXEC_ASYNC);
    #elif defined(__APPLE__)
        ::wxExecute(wxString::Format("open %s", ShellQuote(wxpath)), wxEXEC_ASYNC);
    #else
        const char* argv[] = { "xdg-open", nullptr, nullptr };

        wxString utf8_path = wxpath.ToUTF8();
        argv[1] = utf8_path.c_str();

        if (wxGetEnv("APPIMAGE", nullptr)) {
            wxEnvVariableHashMap env_vars;
            wxGetEnvMap(&env_vars);

            env_vars.erase("APPIMAGE");
            env_vars.erase("APPDIR");
            env_vars.erase("LD_LIBRARY_PATH");
            env_vars.erase("LD_PRELOAD");
            env_vars.erase("UNION_PRELOAD");

            wxExecuteEnv exec_env;
            exec_env.env = std::move(env_vars);

            wxString owd;
            if (wxGetEnv("OWD", &owd))
                exec_env.cwd = std::move(owd);

            ::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, &exec_env);
        }
        else
            ::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC);
    #endif
}

bool TroubleshootDialog::ExportAsJson(const wxString& json_data, const wxString& export_name)
{
    wxString home = wxGetHomeDir();

    wxFileName desktop(home, "");
    desktop.AppendDir("Desktop");
    wxString defaultPath = wxDirExists(desktop.GetPath()) ? desktop.GetPath() : home;

    wxFileDialog dialog(this, _L("Choose where to save the exported JSON file"), defaultPath,
        export_name.IsEmpty() ? "export.json" : export_name + ".json",
        "JSON files (*.json)|*.json",
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (dialog.ShowModal() != wxID_OK)
        return false;

    wxString jsonPath = dialog.GetPath();

    wxFile file(jsonPath, wxFile::write);
    if (!file.IsOpened()) {
        MessageDialog(this, _L("Export failed\nPlease check write permissions or file in use by another application"),
            wxString(SLIC3R_APP_FULL_NAME), wxICON_WARNING | wxOK
        ).ShowModal();
        return false;
    }

    bool success = file.Write(json_data, wxConvUTF8);
    file.Close();

    if (!success) {
        wxRemoveFile(jsonPath);
        MessageDialog(this, _L("Export failed\nPlease check write permissions or file in use by another application"),
            wxString(SLIC3R_APP_FULL_NAME), wxICON_WARNING | wxOK
        ).ShowModal();
        return false;
    }

    MessageDialog(this, _L("Export successful"), wxString(SLIC3R_APP_FULL_NAME), wxICON_INFORMATION | wxOK).ShowModal();

    return true;
}

bool TroubleshootDialog::ExportAsZip(const std::vector<wxString>& sources, const wxString& export_name)
{
    wxString home = wxGetHomeDir();

    wxFileName desktop(home, "");
    desktop.AppendDir("Desktop");
    wxString defaultPath = wxDirExists(desktop.GetPath()) ? desktop.GetPath() : home;

    wxDirDialog dialog(this, _L("Choose where to save the exported ZIP file"), defaultPath, wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    wxString destDir = (dialog.ShowModal() == wxID_OK) ? dialog.GetPath() : "";
    if (destDir.IsEmpty())
        return false;
    wxString baseName = export_name.IsEmpty() ? wxFileName(sources[0]).GetFullName() : export_name;
    wxString zipPath  = wxFileName(destDir, baseName + ".zip").GetFullPath();
    if (wxFileExists(zipPath)) {
        MessageDialog msg(this, _L("File already exists. Overwrite?"),
             wxString(SLIC3R_APP_FULL_NAME), wxICON_QUESTION | wxYES_NO
        );
        if (msg.ShowModal() != wxID_YES)
            return false;
    }
    if (!SaveAsZip(sources, zipPath)) {
        MessageDialog(this, _L("Export failed\nPlease check write permissions or file in use by another application"),
             wxString(SLIC3R_APP_FULL_NAME), wxICON_WARNING | wxOK
        ).ShowModal();
        return false;
    } 
    else {
        MessageDialog(this, _L("Export successful"), wxString(SLIC3R_APP_FULL_NAME), wxICON_INFORMATION | wxOK).ShowModal();
    }
    return true;
}

bool TroubleshootDialog::AddToZip(wxZipOutputStream& zip, const wxString& fullPathOrTextData, const wxString& rootDir)
{
    if (fullPathOrTextData.StartsWith(wxT("TxtData:"))) { // add text to zip
        // Format: "TxtData:<filename>|<content>"
        wxString payload = fullPathOrTextData.Mid(8); // strip "TxtData:"
        int sep = payload.Find('|');
        if (sep == wxNOT_FOUND)
            return false;
        wxString entryName = payload.Left(sep);
        wxString content   = payload.Mid(sep + 1);

        if (!zip.PutNextEntry(entryName))
            return false;
        wxScopedCharBuffer buf = content.utf8_str();
        zip.Write(buf.data(), buf.length());
        return zip.CloseEntry();
    }

    wxString relPath = fullPathOrTextData.Mid(rootDir.length());
    if (relPath.StartsWith(wxFileName::GetPathSeparator()))
        relPath = relPath.Mid(1);
    relPath.Replace(wxFileName::GetPathSeparator(), wxT("/"));
    if (wxDirExists(fullPathOrTextData)) {
        if (!relPath.IsEmpty()) {
            if (!relPath.EndsWith(wxT("/")))
                relPath += wxT("/");
            if (!zip.PutNextDirEntry(relPath))
                return false;
        }

        wxDir dir(fullPathOrTextData);
        if (!dir.IsOpened())
            return false;

        wxString filename;
        bool cont = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES | wxDIR_DIRS | wxDIR_HIDDEN);
        while (cont) {
            wxString childPath = fullPathOrTextData;
            if (!wxEndsWithPathSeparator(childPath))
                childPath += wxFileName::GetPathSeparator();
            childPath += filename;

            if (!AddToZip(zip, childPath, rootDir))
                return false;

            cont = dir.GetNext(&filename);
        }
    }
    else if (wxFileExists(fullPathOrTextData)) {
        wxFileInputStream in(fullPathOrTextData);
        if (!in.IsOk())
            return false;
        if (!zip.PutNextEntry(relPath))
            return false;
        zip.Write(in);
        if (!zip.CloseEntry())
            return false;
    }
    else
        return false;

    return true;
}

bool TroubleshootDialog::SaveAsZip(const std::vector<wxString>& sourcePaths, const wxString& zipFullPath)
{
    wxFileOutputStream out(zipFullPath);
    if (!out.IsOk())
        return false;
    wxZipOutputStream zip(out);
    if (!zip.IsOk()) {
        out.Close();
        return false;
    }
    bool success = true;
    for (const auto& sourcePath : sourcePaths) {
        if (sourcePath.StartsWith(wxT("TxtData:"))) {
            wxString rootDir; // unused for virtual entries
            if (!AddToZip(zip, sourcePath, rootDir)) {
                success = false;
                break;
            }
            continue;
        }
        if (!wxDirExists(sourcePath) && !wxFileExists(sourcePath)) {
            success = false;
            break;
        }
        wxString rootDir = wxFileName(sourcePath).GetPath();
        if (!wxEndsWithPathSeparator(rootDir))
            rootDir += wxFileName::GetPathSeparator();
        if (!AddToZip(zip, sourcePath, rootDir)) {
            success = false;
            break;
        }
    }
    if (!zip.Close()) success = false;
    if (!out.Close()) success = false;
    if (!success)
        wxRemoveFile(zipFullPath);
    return success;
}

void TroubleshootDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    m_logo.msw_rescale();
    m_header_logo->SetBitmap(m_logo.bmp());

    auto processCtrls = [&](auto&& self, wxWindow* win) -> void {
        if (!win)
            return;
        
        if (Button* btn = dynamic_cast<Button*>(win))
            btn->Rescale();

        if (ComboBox* combo = dynamic_cast<ComboBox*>(win))
            combo->Rescale();

        wxWindowList children = win->GetChildren();
        for (auto child : children)
            self(self, child);
    };

    processCtrls(processCtrls, this);

    Layout();
    Fit();
    Refresh();
}

//TroubleshootDialog::~TroubleshootDialog()
//{
//}

} // namespace GUI
} // namespace Slic3r