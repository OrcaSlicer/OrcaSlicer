#include "ExportPresetBundleDialog.hpp"
#include <slic3r/GUI/Widgets/WebView.hpp>
#include "GUI_App.hpp"
#include "ConfigWizard.hpp"
#include "I18N.hpp"
#include "GUI_App.hpp"
#include <libslic3r/Config.hpp>
#include <wx/app.h>
#include <wx/event.h>
#include <wx/filename.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <libslic3r/PresetBundle.hpp>
#include <wx/string.h>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <miniz.h>
#include <slic3r/GUI/MsgDialog.hpp>
namespace Slic3r { namespace GUI {

ExportPresetBundleDialog::ExportPresetBundleDialog(
    wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style)
    : DPIDialog(parent, id, _L("ExportPresetBundle"), pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    SetMinSize(DESIGN_WINDOW_SIZE);
    Init();
    wxGetApp().UpdateDlgDarkUI(this);
}

ExportPresetBundleDialog::~ExportPresetBundleDialog()
{
    for (std::pair<std::string, Preset*> printer_preset : m_printer_presets) {
        Preset* preset = printer_preset.second;
        if (preset) {
            delete preset;
            preset = nullptr;
        }
    }
}

void ExportPresetBundleDialog::LoadUrl(wxString& url)
{
    if (!m_browser)
        return;
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << " enter, url=" << url.ToStdString();
    WebView::LoadUrl(m_browser, url);
    m_browser->SetFocus();

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " exit";
}

void ExportPresetBundleDialog::on_dpi_changed(const wxRect& suggested_rect) { this->Refresh(); }

void ExportPresetBundleDialog::Init()
{
    wxString TargetUrl = from_u8(
        (boost::filesystem::path(resources_dir()) / "web/dialog/ExportPresetDialog/index.html").make_preferred().string());
    wxString strlang = wxGetApp().current_language_code_safe();
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", strlang=%1%") % into_u8(strlang);
    if (strlang != "")
        TargetUrl = wxString::Format("%s?lang=%s", std::string(TargetUrl.mb_str()), strlang);
    TargetUrl = "file://" + TargetUrl;

    // Create the webview
    m_browser = WebView::CreateWebView(this, TargetUrl);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);
    SetTitle(_L("Export Preset Bundle"));
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

    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ExportPresetBundleDialog::OnScriptMessage, this, m_browser->GetId());

    LoadUrl(TargetUrl);
}

void ExportPresetBundleDialog::RunScript(const wxString& s)
{
    if (!m_browser)
        return;

    WebView::RunScript(m_browser, s);
}

void ExportPresetBundleDialog::OnScriptMessage(wxWebViewEvent& e)
{
    try {
        wxString strInput = e.GetString();
        BOOST_LOG_TRIVIAL(trace) << "ExportPresetBundleDialog::OnScriptMessage;OnRecv:" << strInput.c_str();
        json j = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];
        BOOST_LOG_TRIVIAL(trace) << "ExportPresetBundleDialog::OnScriptMessage;Command:" << strCmd;

        if (strCmd == "close_page") {
            this->EndModal(wxID_CANCEL);
        } else if (strCmd == "request_export_preset_profile") {
            InitExportData();
            OnRequestPresets();
        } else if (strCmd == "export_local") {
            wxFileDialog dlg(this, _L("Save preset bundle"), "", "export.orca_bundle", "Orca Preset Bundle (*.orca_bundle)|*.orca_bundle",
                             wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
            wxString path;
            wxString name;
            if (dlg.ShowModal() == wxID_OK) {
                path = dlg.GetPath();
                wxFileName file_name(path);
                name = file_name.GetName();
                if (file_name.GetExt().empty()) {
                    file_name.SetExt("orca_bundle");
                    path = file_name.GetFullPath();
                }
            }
            OnExportData(path, name, j["data"]);
        }

    } catch (std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << "ExportPresetBundleDialog::OnScriptMessage;Error:" << e.what();
    }
}

static std::string get_machine_name(const std::string& preset_name)
{
    size_t index_at = preset_name.find_last_of("@");
    if (std::string::npos == index_at) {
        return "";
    } else {
        return preset_name.substr(index_at + 1);
    }
}

static std::string get_filament_name(std::string& preset_name)
{
    size_t index_at = preset_name.find_last_of("@");
    if (std::string::npos == index_at) {
        return preset_name;
    } else {
        return preset_name.substr(0, index_at - 1);
    }
}

static std::string get_vendor_name(std::string& preset_name)
{
    if (preset_name.empty())
        return "";
    std::string vendor_name = preset_name.substr(preset_name.find_first_not_of(' ')); // remove the name prefix space
    size_t index_at         = vendor_name.find(" ");
    if (std::string::npos == index_at) {
        return vendor_name;
    } else {
        vendor_name = vendor_name.substr(0, index_at);
        return vendor_name;
    }
}

static std::string get_curr_time(const char* format = "%Y_%m_%d_%H_%M_%S")
{
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

    std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm local_time = *std::localtime(&time);
    std::ostringstream time_stream;
    time_stream << std::put_time(&local_time, format);

    std::string current_time = time_stream.str();
    return current_time;
}

static mz_bool initial_zip_archive(mz_zip_archive& zip_archive, const std::string& file_path)
{
    mz_zip_zero_struct(&zip_archive);
    mz_bool status;

    // Initialize the ZIP file to write to the structure, using memory storage

    std::string export_dir = encode_path(file_path.c_str());
    status                 = mz_zip_writer_init_file(&zip_archive, export_dir.c_str(), 0);
    return status;
}

void ExportPresetBundleDialog::InitExportData()
{
    // Delete the Temp folder
    boost::filesystem::path folder(data_dir() + "/" + PRESET_USER_DIR + "/" + "Temp");
    if (boost::filesystem::exists(folder))
        boost::filesystem::remove_all(folder);

    boost::system::error_code ec;
    boost::filesystem::path user_folder(data_dir() + "/" + PRESET_USER_DIR);
    bool temp_folder_exist = true;
    if (!boost::filesystem::exists(user_folder)) {
        if (!boost::filesystem::create_directories(user_folder, ec)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << user_folder << " " << ec.message();
            temp_folder_exist = false;
        }
    }
    boost::filesystem::path temp_folder(user_folder / "Temp");
    if (!boost::filesystem::exists(temp_folder)) {
        if (!boost::filesystem::create_directories(temp_folder, ec)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << temp_folder << " " << ec.message();
            temp_folder_exist = false;
        }
    }
    if (!temp_folder_exist) {
        MessageDialog dlg(this, _L("Failed to create temporary folder, please try Export Configs again."),
                          wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"), wxYES_NO | wxYES_DEFAULT | wxCENTRE);
        dlg.ShowModal();
        EndModal(wxCANCEL);
    }

    PresetBundle preset_bundle(*wxGetApp().preset_bundle);

    const std::deque<Preset>& printer_presets = preset_bundle.printers.get_presets();
    // for all the printers
    for (const Preset& printer_preset : printer_presets) {
        std::string preset_name = printer_preset.name;
        if (!printer_preset.is_visible || printer_preset.is_default || printer_preset.is_project_embedded)
            continue;
        if (preset_bundle.printers.select_preset_by_name(preset_name, true)) {
            preset_bundle.update_compatible(PresetSelectCompatibleType::Always);

            const std::deque<Preset>& filament_presets = preset_bundle.filaments.get_presets();
            for (const Preset& filament_preset : filament_presets) {
                if (!filament_preset.is_user())
                    continue;
                if (filament_preset.is_compatible) {
                    Preset* new_filament_preset = new Preset(filament_preset);
                    m_filament_presets[preset_name].push_back(new_filament_preset);
                }
            }

            const std::deque<Preset>& process_presets = preset_bundle.prints.get_presets();
            for (const Preset& process_preset : process_presets) {
                if (!process_preset.is_user())
                    continue;
                if (process_preset.is_compatible) {
                    Preset* new_prpcess_preset = new Preset(process_preset);
                    m_process_presets[preset_name].push_back(new_prpcess_preset);
                }
            }
            // make new and erase sensitive information
            Preset* new_printer_preset = new Preset(printer_preset);
            if (new_printer_preset->type == Preset::Type::TYPE_PRINTER) {
                boost::filesystem::path file_path(data_dir() + "/" + PRESET_USER_DIR + "/" + "Temp" + "/" +
                                                  (new_printer_preset->name + ".json"));
                new_printer_preset->file = file_path.make_preferred().string();

                DynamicPrintConfig& config = new_printer_preset->config;
                config.erase("print_host");
                config.erase("print_host_webui");
                config.erase("printhost_apikey");
                config.erase("printhost_cafile");
                config.erase("printhost_user");
                config.erase("printhost_password");
                config.erase("printhost_port");

                new_printer_preset->save(nullptr);
            }
            m_printer_presets[preset_name] = new_printer_preset;
        }
    }
    const std::deque<Preset>& filament_presets = preset_bundle.filaments.get_presets();
    for (const Preset& filament_preset : filament_presets) {
        if (!filament_preset.can_overwrite())
            continue;
        Preset* new_filament_preset        = new Preset(filament_preset);
        const Preset* base_filament_preset = preset_bundle.filaments.get_preset_base(*new_filament_preset);

        if (base_filament_preset == nullptr) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Failed to find base preset";
            continue;
        }
        std::string filament_preset_name = base_filament_preset->name;
        std::string machine_name         = get_machine_name(filament_preset_name);
        m_filament_name_to_presets[get_filament_name(filament_preset_name)].push_back(
            std::make_pair(get_vendor_name(machine_name), new_filament_preset));
    }
}

void ExportPresetBundleDialog::OnRequestPresets()
{
    PresetBundle* preset_bundle = wxGetApp().preset_bundle;
    json res;
    res["command"]     = "response_export_preset_profile";
    res["sequence_id"] = "2000";
    res["data"]        = json::object();

    res["data"]["printers"]  = json::array();
    res["data"]["filaments"] = json::array();
    res["data"]["process"]   = json::array();

    for (std::pair<std::string, Preset*> preset : m_printer_presets) {
        if (preset.second->is_system)
            continue;
        res["data"]["printers"].push_back(preset.first);
    }

    for (std::pair<std::string, std::vector<std::pair<std::string, Preset*>>> filament_name_to_preset : m_filament_name_to_presets) {
        if (filament_name_to_preset.second.empty())
            continue;
        res["data"]["filaments"].push_back(filament_name_to_preset.first);
    }

    for (std::pair<std::string, std::vector<const Preset*>> presets : m_process_presets) {
        Preset* printer_preset = preset_bundle->printers.find_preset(presets.first, false);
        if (!printer_preset)
            continue;
        if (!printer_preset->is_system)
            continue;
        if (preset_bundle->printers.get_preset_base(*printer_preset) != printer_preset)
            continue;
        for (const Preset* preset : presets.second) {
            if (!preset->is_system) {
                res["data"]["process"].push_back(preset->name);
                break;
            }
        }
    }

    wxString strJS = wxString::Format("HandleStudio(%s)", wxString::FromUTF8(res.dump(-1, ' ', false, json::error_handler_t::ignore)));
    wxGetApp().CallAfter([this, strJS] { RunScript(strJS); });
}

void ExportPresetBundleDialog::OnExportData(const wxString& path, const wxString& filename, json data)
{
    auto get_names = [&](const char* key) {
        std::vector<std::string> out;
        auto it = data.find(key);
        if (it == data.end() || !it->is_array())
            return out;
        for (const auto& v : *it) {
            if (v.is_string())
                out.push_back(v.get<std::string>());
        }
        return out;
    };

    // JS BuildResultPayload uses: machines / filaments / presets
    const auto machine_names  = get_names("machines");
    const auto filament_names = get_names("filaments");
    const auto process_names  = get_names("presets"); // or "process" if your JS sends that

    std::vector<Preset*> selected_printers;
    for (const auto& name : machine_names) {
        auto it = m_printer_presets.find(name);
        if (it != m_printer_presets.end() && it->second)
            selected_printers.push_back(it->second);
    }

    std::vector<Preset*> selected_filaments;
    for (const auto& name : filament_names) {
        auto it = m_filament_name_to_presets.find(name); // name -> vector<pair<vendor, Preset*>>
        if (it == m_filament_name_to_presets.end())
            continue;
        for (const auto& vp : it->second) {
            if (vp.second)
                selected_filaments.push_back(vp.second);
        }
    }

    std::vector<Preset*> selected_processes;
    for (const auto& name : process_names) {
        if (Preset* p = wxGetApp().preset_bundle->prints.find_preset(name, false))
            selected_processes.push_back(p);
    }

    std::string export_path = into_u8(path);
    if (export_path.empty() || "initial_failed" == export_path)
        return;

    boost::filesystem::path export_file_path = boost::filesystem::path(export_path).make_preferred();
    if (export_file_path.extension().empty())
        export_file_path += ".orca_bundle";

    const boost::filesystem::path export_dir = export_file_path.parent_path();
    if (!export_dir.empty() && !boost::filesystem::exists(export_dir)) {
        boost::system::error_code ec;
        if (!boost::filesystem::create_directories(export_dir, ec)) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " create directory failed: " << export_dir << " " << ec.message();
            return;
        }
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " export file path: " << export_file_path.string();

    BOOST_LOG_TRIVIAL(info) << "Export printer preset bundle";

    json bundle_structure;
    NetworkAgent* agent = wxGetApp().getAgent();
    std::string clock   = get_curr_time();
    if (agent) {
        bundle_structure["version"]   = agent->get_version();
        bundle_structure["bundle_id"] = agent->get_user_id() + "_" + std::string(filename.utf8_string()) + "_" + clock;
    } else {
        bundle_structure["version"] = "";
        std::string id;
        bundle_structure["bundle_id"] = id + "offline_" + "_" + clock;
    }
    bundle_structure["bundle_type"]         = "printer config bundle";
    bundle_structure["printer_preset_name"] = "export";
    json printer_config                     = json::array();
    json filament_configs                   = json::array();
    json process_configs                    = json::array();
    mz_zip_archive zip_archive;
    mz_bool status = initial_zip_archive(zip_archive, export_file_path.string());
    if (MZ_FALSE == status) {
        BOOST_LOG_TRIVIAL(info) << "Failed to initialize ZIP archive";
        show_export_result(ExportCase::INITIALIZE_FAIL);
    }
    for (auto& printer : selected_printers) {
        boost::filesystem::path printer_file_path = boost::filesystem::path(printer->file);
        std::string preset_path                   = printer_file_path.make_preferred().string();
        if (preset_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Export printer preset: " << printer->name << " skip because of the preset file path is empty.";
            continue;
        }

        // Add a file to the ZIP file
        std::string printer_config_file_name = "printer/" + printer_file_path.filename().string();
        status = mz_zip_writer_add_file(&zip_archive, printer_config_file_name.c_str(), encode_path(preset_path.c_str()).c_str(), NULL, 0,
                                        MZ_DEFAULT_COMPRESSION);
        if (MZ_FALSE == status) {
            BOOST_LOG_TRIVIAL(info) << printer->name << " Failed to add file to ZIP archive";
            mz_zip_writer_end(&zip_archive);
            show_export_result(ExportCase::ADD_FILE_FAIL);
            return;
        }
        printer_config.push_back(printer_config_file_name);
        BOOST_LOG_TRIVIAL(info) << "Printer preset json add successful: " << printer->name;
    }
    for (auto& filament : selected_filaments) {
        boost::filesystem::path filament_file_path = boost::filesystem::path(filament->file);
        std::string filament_preset_path           = filament_file_path.make_preferred().string();
        if (filament_preset_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Export filament preset: " << filament->name << " skip because of the preset file path is empty.";
            continue;
        }
        std::string filament_config_file_name = "filament/" + filament_file_path.filename().string();
        status = mz_zip_writer_add_file(&zip_archive, filament_config_file_name.c_str(), encode_path(filament_preset_path.c_str()).c_str(),
                                        NULL, 0, MZ_DEFAULT_COMPRESSION);
        if (MZ_FALSE == status) {
            BOOST_LOG_TRIVIAL(info) << filament->name << " Failed to add file to ZIP archive";
            mz_zip_writer_end(&zip_archive);
            show_export_result(ExportCase::ADD_FILE_FAIL);
            return;
        }
        filament_configs.push_back(filament_config_file_name);
        BOOST_LOG_TRIVIAL(info) << "Filament preset json add successful.";
    }

    for (auto& process : selected_processes) {
        boost::filesystem::path process_file_path = boost::filesystem::path(process->file);
        std::string process_preset_path           = process_file_path.make_preferred().string();
        if (process_preset_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << "Export process preset: " << process->name << " skip because of the preset file path is empty.";
            continue;
        }

        std::string process_config_file_name = "process/" + process_file_path.filename().string();
        status = mz_zip_writer_add_file(&zip_archive, process_config_file_name.c_str(), encode_path(process_preset_path.c_str()).c_str(),
                                        NULL, 0, MZ_DEFAULT_COMPRESSION);
        if (MZ_FALSE == status) {
            BOOST_LOG_TRIVIAL(info) << process->name << " Failed to add file to ZIP archive";
            mz_zip_writer_end(&zip_archive);
            show_export_result(ExportCase::ADD_FILE_FAIL);
            return;
        }
        process_configs.push_back(process_config_file_name);
        BOOST_LOG_TRIVIAL(info) << "Process preset json add successful: ";
    }

    bundle_structure["printer_config"]  = printer_config;
    bundle_structure["filament_config"] = filament_configs;
    bundle_structure["process_config"]  = process_configs;

    std::string bundle_structure_str = bundle_structure.dump();
    status = mz_zip_writer_add_mem(&zip_archive, BUNDLE_STRUCTURE_JSON_NAME, bundle_structure_str.data(), bundle_structure_str.size(),
                                   MZ_DEFAULT_COMPRESSION);
    if (MZ_FALSE == status) {
        BOOST_LOG_TRIVIAL(info) << " Failed to add file: " << BUNDLE_STRUCTURE_JSON_NAME;
        mz_zip_writer_end(&zip_archive);
        show_export_result(ExportCase::ADD_BUNDLE_STRUCTURE_FAIL);
        return;
    }
    BOOST_LOG_TRIVIAL(info) << " Success to add file: " << BUNDLE_STRUCTURE_JSON_NAME;

    // Complete writing of ZIP file
    mz_bool s = mz_zip_writer_finalize_archive(&zip_archive);
    if (MZ_FALSE == s) {
        BOOST_LOG_TRIVIAL(info) << "Failed to finalize ZIP archive";
        mz_zip_writer_end(&zip_archive);
        show_export_result(ExportCase::FINALIZE_FAIL);
        return;
    }

    // Release ZIP file to write structure and related resources
    mz_zip_writer_end(&zip_archive);
    // if (ExportCase::CASE_COUNT != save_result) return save_result;
    BOOST_LOG_TRIVIAL(info) << "ZIP archive created successfully";
}

void ExportPresetBundleDialog::show_export_result(const ExportCase& export_case)
{
    MessageDialog* msg_dlg = nullptr;
    switch (export_case) {
    case ExportCase::INITIALIZE_FAIL:
        msg_dlg = new MessageDialog(this, _L("initialize fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                    wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::ADD_FILE_FAIL:
        msg_dlg = new MessageDialog(this, _L("add file fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                    wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::ADD_BUNDLE_STRUCTURE_FAIL:
        msg_dlg = new MessageDialog(this, _L("add bundle structure file fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                    wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::FINALIZE_FAIL:
        msg_dlg = new MessageDialog(this, _L("finalize fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                    wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::OPEN_ZIP_WRITTEN_FILE:
        msg_dlg = new MessageDialog(this, _L("open zip written fail"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                    wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    case ExportCase::EXPORT_SUCCESS:
        msg_dlg = new MessageDialog(this, _L("Export successful"), wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Info"),
                                    wxYES | wxYES_DEFAULT | wxCENTRE);
        break;
    }

    if (msg_dlg) {
        msg_dlg->ShowModal();
        delete msg_dlg;
        msg_dlg = nullptr;
    }
}

}} // namespace Slic3r::GUI
