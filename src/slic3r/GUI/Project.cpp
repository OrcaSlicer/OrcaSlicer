#include "Tab.hpp"
#include "Project.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"

#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/button.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>

#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>
#include <wx/settings.h>
#include <wx/filedlg.h>
#include <wx/wupdlock.h>
#include <wx/dataview.h>
#include <wx/tokenzr.h>
#include <wx/arrstr.h>
#include <wx/tglbtn.h>

#include "wxExtensions.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "MainFrame.hpp"
#include <slic3r/GUI/Widgets/WebView.hpp>

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(EVT_PROJECT_RELOAD, wxCommandEvent);

const std::vector<std::string> license_list = {
    "BSD License",
    "Apache License",
    "GPL License",
    "LGPL License",
    "MIT License",
    "CC License"
};

ProjectPanel::ProjectPanel(wxWindow *parent, wxWindowID id, const wxPoint &pos, const wxSize &size, long style) : wxPanel(parent, id, pos, size, style)
{
    m_project_home_url = wxString::Format("file://%s/web/model/index.html", from_u8(resources_dir()));
    wxString strlang = wxGetApp().current_language_code_safe();
    if (strlang != "")
        m_project_home_url = wxString::Format("file://%s/web/model/index.html?lang=%s", from_u8(resources_dir()), strlang);

    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    m_browser = WebView::CreateWebView(this, m_project_home_url);
    if (m_browser == nullptr) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format("load web view of project page failed");
        return;
    }
    //m_browser->Hide();
    main_sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATED, &ProjectPanel::on_navigated, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &ProjectPanel::OnScriptMessage, this, m_browser->GetId());
    Bind(wxEVT_WEBVIEW_NAVIGATING, &ProjectPanel::onWebNavigating, this, m_browser->GetId());

    Bind(EVT_PROJECT_RELOAD, &ProjectPanel::on_reload, this);

    m_auxiliary = new AuxiliaryPanel(this);
    m_auxiliary->Hide();
    main_sizer->Add(m_auxiliary, wxSizerFlags().Expand().Proportion(1));
    Bind(EVT_AUXILIARY_DONE, [this](wxCommandEvent& e) { update_model_data();});

    SetSizer(main_sizer);
    Layout();
    Fit();
}

ProjectPanel::~ProjectPanel() {}

// Helper to convert newlines to <br>
static std::string convert_newlines_to_br(const std::string& text) {
    std::string result = text;
    size_t pos = 0;
    while ((pos = result.find('\n', pos)) != std::string::npos) {
        result.replace(pos, 1, "<br>");
        pos += 4;
    }
    return result;
}

void ProjectPanel::onWebNavigating(wxWebViewEvent& evt)
{
    wxString tmpUrl = evt.GetURL();
    //wxString NowUrl = m_browser->GetCurrentURL();

    if (boost::starts_with(tmpUrl, "http://") || boost::starts_with(tmpUrl, "https://")) {
        m_browser->Stop();
        evt.Veto();
        wxLaunchDefaultApplication(tmpUrl);
    }
}

void ProjectPanel::on_reload(wxCommandEvent& evt)
{
    boost::thread reload = boost::thread([this] {
        std::string update_type;
        std::string license;
        std::string model_name;
        std::string model_author;
        std::string cover_file;
        std::string description;
        std::string p_name;
        std::string p_author;
        std::string p_description;
        std::string p_cover_file;

        std::map<std::string, std::vector<json>> files;

        Model model = wxGetApp().plater()->model();

        auto model_info = model.model_info;
        if (model_info != nullptr) {
            license     = model_info->license;
            model_name  = model_info->model_name;
            cover_file  = model_info->cover_file;
            description = model_info->description;
            update_type = model_info->origin;

            try {
                if (!model_info->copyright.empty()) {
                    json copy_right = json::parse(model_info->copyright);

                    if (copy_right.is_array()) {
                        for (auto it = copy_right.begin(); it != copy_right.end(); it++) {
                            if ((*it).contains("author")) {
                                model_author = (*it)["author"].get<std::string>();
                            }
                        }
                    }
                }
            } catch (...) {
                ;
            }
        }

        if (model_author.empty() && model.design_info != nullptr)
            model_author = model.design_info->Designer;

        if (model.profile_info != nullptr) {
            p_name = model.profile_info->ProfileTile;
            p_description = model.profile_info->ProfileDescription;
            p_cover_file = model.profile_info->ProfileCover;
            p_author = model.profile_info->ProfileUserName;
        }

        // file info
        std::string file_path = encode_path(wxGetApp().plater()->model().get_auxiliary_file_temp_path().c_str());
        if (!file_path.empty()) {
            files = Reload(file_path);
            wxGetApp().CallAfter([this, file_path, files] { m_auxiliary->Reload(file_path, files); });
        } else {
            clear_model_info();
            return;
        }

        bool has_content = false;
        for (const string& v : {
                 update_type,
                 license,
                 model_name,
                 model_author,
                 cover_file,
                 description,
                 p_name,
                 p_author,
                 p_description,
                 p_cover_file,
             }) {
            if (!v.empty()) {
                has_content = true;
                break;
            }
        }
        if (!has_content) {
            for (const auto & file : files) {
                if (!file.second.empty()) {
                    has_content = true;
                    break;
                }
            }
        }
        if (!has_content) {
            // Nothing to show, just return
            clear_model_info();
            return;
        }

        json j;
        j["model"]["license"] = license;
        j["model"]["name"] = wxGetApp().url_encode(model_name);
        j["model"]["author"] = wxGetApp().url_encode(model_author);;
        j["model"]["cover_img"] = wxGetApp().url_encode(cover_file);
        j["model"]["description"] = wxGetApp().url_encode(convert_newlines_to_br(description));
        j["model"]["preview_img"] = files["Model Pictures"];
        j["model"]["upload_type"] = update_type;

        j["file"]["BOM"] = files["Bill of Materials"];
        j["file"]["Assembly"] = files["Assembly Guide"];
        j["file"]["Other"] = files["Others"];

        j["profile"]["name"] = wxGetApp().url_encode(p_name);
        j["profile"]["author"] = wxGetApp().url_encode(p_author);
        j["profile"]["description"] = wxGetApp().url_encode(p_description);
        j["profile"]["cover_img"] = wxGetApp().url_encode(p_cover_file);
        j["profile"]["preview_img"] = files["Profile Pictures"];

        json m_Res = json::object();
        m_Res["command"] = "show_3mf_info";
        m_Res["sequence_id"] = std::to_string(ProjectPanel::m_sequence_id++);
        m_Res["model"] = j;

        wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', false, json::error_handler_t::ignore));

        if (m_web_init_completed) {
            wxGetApp().CallAfter([this, strJS] {
                RunScript(strJS.ToStdString());
                });
        }
    });
}

void ProjectPanel::msw_rescale() 
{
    m_auxiliary->msw_rescale();
}

void ProjectPanel::on_size(wxSizeEvent &event)
{
    event.Skip();
}

void ProjectPanel::on_navigated(wxWebViewEvent& event)
{
    event.Skip();
}

void ProjectPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    try {
        wxString strInput = evt.GetString();
        json     j = json::parse(strInput.utf8_string());

        wxString strCmd = j["command"];

        if (strCmd == "open_3mf_accessory") {
            wxString accessory_path =  j["accessory_path"];

            if (!accessory_path.empty()) {
                std::string decode_path = wxGetApp().url_decode(accessory_path.ToStdString());
                fs::path path(decode_path);

                if (fs::exists(path)) {
                    wxLaunchDefaultApplication(path.wstring(), 0);
                }
            }
        }
        else if (strCmd == "request_3mf_info") {
            m_web_init_completed = true;
        }
        else if (strCmd == "edit_project_info") {
            show_info_editor(true);
        }
        else if (strCmd == "debug_info") {
            //wxString msg =  j["msg"];
            //OutputDebugString(wxString::Format("Model_Web: msg = %s \r\n", msg));
            //BOOST_LOG_TRIVIAL(info) << wxString::Format("Model_Web: msg = %s", msg);
        }

    }
    catch (std::exception&) {
        // wxMessageBox(e.what(), "json Exception", MB_OK);
    }
}

void ProjectPanel::show_info_editor(bool show)
{
    m_browser->Show(!show);
    m_auxiliary->Show(show);
    Layout();
}

void ProjectPanel::update_model_data()
{
    Model model = wxGetApp().plater()->model();
    show_info_editor(false);
    clear_model_info();

    m_auxiliary->init_auxiliary();

    //basics info
    //if (model.model_info == nullptr)
    //    return;
    
    auto event = wxCommandEvent(EVT_PROJECT_RELOAD);
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

void ProjectPanel::clear_model_info()
{
    json m_Res = json::object();
    m_Res["command"] = "clear_3mf_info";
    m_Res["sequence_id"] = std::to_string(ProjectPanel::m_sequence_id++);

    wxString strJS = wxString::Format("HandleStudio(%s)", m_Res.dump(-1, ' ', false, json::error_handler_t::ignore));

    wxGetApp().CallAfter([this, strJS] {
        RunScript(strJS.ToStdString());
    });
}

std::map<std::string, std::vector<json>> ProjectPanel::Reload(wxString aux_path)
{
    std::vector<fs::path>                           dir_cache;
    fs::directory_iterator                          iter_end;
    std::map<std::string, std::vector<json>> m_paths_list;

    const static std::array<wxString, 5> s_default_folders = {
        ("Model Pictures"),
        ("Bill of Materials"),
        ("Assembly Guide"),
        ("Others"),
        //(".thumbnails"),
        ("Profile Pictures"),
    };

    for (auto folder : s_default_folders)
        m_paths_list[folder.ToStdString()] = std::vector<json>{};


    fs::path new_aux_path(aux_path.ToStdWstring());

    // Check new path. If not exist, create a new one.
    if (!fs::exists(new_aux_path)) {
        fs::create_directory(new_aux_path);
    }

    // Create default folders if they are not loaded
    for (auto folder : s_default_folders) {
        wxString folder_path = aux_path + "/" + folder;
        if (fs::exists(folder_path.ToStdWstring())) continue;
        fs::create_directory(folder_path.ToStdWstring());
    }

    // Load from new path
    for (fs::directory_iterator iter(new_aux_path); iter != iter_end; iter++) {
        wxString path = iter->path().generic_wstring();
        dir_cache.push_back(iter->path());
    }


    for (auto dir : dir_cache) {
        for (fs::directory_iterator iter(dir); iter != iter_end; iter++) {
            if (fs::is_directory(iter->path())) continue;

            json pfile_obj;

            std::string file_path = iter->path().string();
            fs::path file_path_obj = fs::path(iter->path().string());

            for (auto folder : s_default_folders) {
                auto idx = file_path.find(folder.ToStdString());
                if (idx != std::string::npos) {
                    
                    wxStructStat strucStat;
                    wxString file_name = encode_path(file_path.c_str());
                    wxStat(file_name, &strucStat);
                    wxFileOffset filelen = strucStat.st_size;

                    pfile_obj["_filepath"] = file_path;
                    pfile_obj["filename"] = wxGetApp().url_encode(file_path_obj.filename().string().c_str());
                    pfile_obj["size"] = formatBytes((unsigned long)filelen);

                    std::string file_extension = file_path_obj.extension().string();
                    boost::algorithm::to_lower(file_extension);

                    //image
                    if (file_extension == ".jpg"    ||
                        file_extension == ".jpeg"   ||
                        file_extension == ".pjpeg"  ||
                        file_extension == ".png"    ||
                        file_extension == ".jfif"   ||
                        file_extension == ".pjp"    ||
                        file_extension == ".webp"   ||
                        file_extension == ".bmp")
                    {

                        wxString base64_str = to_base64(file_path);
                        pfile_obj["filepath"] = base64_str.ToStdString();
                        m_paths_list[folder.ToStdString()].push_back(pfile_obj);
                        break;
                    }
                    else {
                        pfile_obj["filepath"] = wxGetApp().url_encode(file_path);
                        m_paths_list[folder.ToStdString()].push_back(pfile_obj);
                        break;
                    }
                }
            }
        }
    }

    return m_paths_list;
}

std::string ProjectPanel::formatBytes(unsigned long bytes)
{
    double dValidData = round(double(bytes) / (1024 * 1024) * 1000) / 1000;
    return wxString::Format("%.2fMB", dValidData).ToStdString();
}

wxString ProjectPanel::to_base64(std::string file_path) 
{

    std::ifstream imageFile(encode_path(file_path.c_str()), std::ios::binary);
    if (!imageFile) {
        return wxEmptyString;
    }

    std::ostringstream imageStream;
    imageStream << imageFile.rdbuf();

    std::string binaryImageData = imageStream.str();

    std::string extension;
    size_t last_dot = file_path.find_last_of(".");
   
    if (last_dot != std::string::npos) {
        extension = file_path.substr(last_dot + 1);
    }

    wxString bease64_head = wxString::Format("data:image/%s;base64,", extension);


    std::wstringstream wss;
    wss << bease64_head;
    wss << wxBase64Encode(binaryImageData.data(), binaryImageData.size());

    wxString base64_str = wss.str();
    return  base64_str;
}

void ProjectPanel::RunScript(std::string content)
{
    WebView::RunScript(m_browser, content);
}

bool ProjectPanel::Show(bool show) 
{
    if (show) update_model_data();
    return wxPanel::Show(show); 
}

}} // namespace Slic3r::GUI
