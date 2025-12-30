#include "HMS.hpp"

#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevUtil.h"

#include <boost/log/trivial.hpp>

static const char* HMS_PATH = "hms";
static const char* HMS_LOCAL_IMG_PATH = "hms/local_image";

// the local HMS info
static unordered_set<string> package_dev_id_types {"094", "239", "093", "22E"};

namespace Slic3r {
namespace GUI {

int get_hms_info_version(std::string& version)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config)
        return -1;
    if (config->get_stealth_mode())
        return -1;
    std::string hms_host = config->get_hms_host();
    if(hms_host.empty()) {
        BOOST_LOG_TRIVIAL(error) << "hms_host is empty";
        return -1;
    }
    int result = -1;
    version = "";
    std::string lang;
    std::string query_params = HMSQuery::build_query_params(lang);
    std::string url = (boost::format("https://%1%/GetVersion.php?%2%") % hms_host % query_params).str();
    Slic3r::Http http = Slic3r::Http::get(url);
    http.timeout_max(10)
        .on_complete([&result, &version](std::string body, unsigned status){
            try {
                json j = json::parse(body);
                if (j.contains("ver")) {
                    version = DevJsonValParser::get_longlong_val(j["ver"]);
                }
            } catch (...) {
                ;
            }
        })
        .on_error([&result](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "get_hms_info_version: body = " << body << ", status = " << status << ", error = " << error;
            result = -1;
            })
        .perform_sync();
    return result;
}

// Note:  Download the hms into receive_json
int HMSQuery::download_hms_related(const std::string& hms_type, const std::string& dev_id_type, json* receive_json)
{
    std::string local_version = "0";
    load_from_local(hms_type, dev_id_type, receive_json, local_version);

    AppConfig* config = wxGetApp().app_config;
    if (!config) return -1;
    if (config->get_stealth_mode()) return -1;

    std::string hms_host = wxGetApp().app_config->get_hms_host();
    std::string lang;
    std::string query_params = HMSQuery::build_query_params(lang);
    std::string url;
    if (hms_type.compare(QUERY_HMS_INFO) == 0) {
        url = (boost::format("https://%1%/query.php?%2%") % hms_host % query_params).str();
    }
    else if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
        url = (boost::format("https://%1%/hms/GetActionImage.php?") % hms_host).str();
    }

    if (!local_version.empty()) { url += (url.find('?') != std::string::npos ? "&" : "?") + (boost::format("v=%1%") % local_version).str(); }

    if (!dev_id_type.empty()) { url += (url.find('?') != std::string::npos ? "&" : "?") + (boost::format("d=%1%") % dev_id_type).str(); }


    bool to_save_local = false;
    json j;

    BOOST_LOG_TRIVIAL(info) << "hms: download url = " << url;
    Slic3r::Http http = Slic3r::Http::get(url);
    http.on_complete([this, receive_json, hms_type, &to_save_local, &j, & local_version](std::string body, unsigned status) {
        try {
            j = json::parse(body);
            if (j.contains("result")) {
                if (j["result"] == 0 && j.contains("data")) {

                    if (!j.contains("ver"))
                    {
                        return;
                    }

                    const std::string& remote_ver = DevJsonValParser::get_longlong_val(j["ver"]);
                    if (remote_ver <= local_version)
                    {
                        return;
                    }
                    (*receive_json)["version"] = remote_ver;

                    if (hms_type.compare(QUERY_HMS_INFO) == 0)
                    {
                        (*receive_json) = j["data"];
                        to_save_local = true;
                    }
                    else if (hms_type.compare(QUERY_HMS_ACTION) == 0)
                    {
                        (*receive_json)["data"] = j["data"];
                        to_save_local = true;
                    }
                } else if (j["result"] == 201){
                    BOOST_LOG_TRIVIAL(info) << "HMSQuery: HMS info is the latest version";
                }else{
                    BOOST_LOG_TRIVIAL(info) << "HMSQuery: update hms info error = " << j["result"].get<int>();
                }
            }
        } catch (...) {
            ;
        }
        })
        .timeout_max(20)
        .on_error([](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(error) << "HMSQuery: update hms info error = " << error << ", body = " << body << ", status = " << status;
        }).perform_sync();

        if (to_save_local && !receive_json->empty()) {
            save_to_local(lang, hms_type, dev_id_type, j);
        }
    return 0;
}


static void
_copy_dir(const fs::path& from_dir, const fs::path& to_dir) /* copy and override with local files*/
{
    try
    {
	    if (!fs::exists(from_dir))
	    {
	        return;
	    }

	    if (!fs::exists(to_dir))
	    {
	        fs::create_directory(to_dir);
	    }

	    for (const auto &entry : fs::directory_iterator(from_dir))
	    {
	        const fs::path &source_path   = entry.path();
	        const fs::path &relative_path = fs::relative(source_path, from_dir);
	        const fs::path &dest_path     = to_dir / relative_path;

	        if (fs::is_regular_file(source_path))
            {
                if (fs::exists(dest_path))
                {
                    fs::remove(dest_path);
                }

                copy_file(source_path, dest_path);
            }
            else if (fs::is_directory(source_path))
            {
                _copy_dir(source_path, dest_path);
            }
	    }
    }
    catch (...)
    {

    }
}

void HMSQuery::copy_from_data_dir_to_local()
{
    const fs::path& from_dir = fs::path(Slic3r::resources_dir()) / HMS_PATH;
    const fs::path& to_dir = fs::path(Slic3r::data_dir()) / HMS_PATH;
    _copy_dir(from_dir, to_dir);
}

int HMSQuery::load_from_local(const std::string& hms_type, const std::string& dev_id_type, json* load_json, std::string& load_version)
{
    if (data_dir().empty()) {
        load_version = "0";
        BOOST_LOG_TRIVIAL(error) << "HMS: load_from_local, data_dir() is empty";
        return -1;
    }
    std::string filename = get_hms_file(hms_type, HMSQuery::hms_language_code(), dev_id_type);
    auto hms_folder = (boost::filesystem::path(data_dir()) / "hms");
    if (!fs::exists(hms_folder))
        fs::create_directory(hms_folder);

    std::string dir_str = (hms_folder / filename).make_preferred().string();
    std::ifstream json_file(encode_path(dir_str.c_str()));
    try {
        if (json_file.is_open())
        {
            const json &j = json::parse(json_file);
            if (hms_type.compare(QUERY_HMS_INFO) == 0) {
                if (j.contains("data")) { (*load_json) = j["data"]; }
            } else if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
                if (j.contains("data")) { (*load_json)["data"] = j["data"]; }
            }

            if (j.contains("version")) {
                load_version = DevJsonValParser::get_longlong_val(j["version"]);
            }
            else if (j.contains("ver")) {
                load_version = DevJsonValParser::get_longlong_val(j["ver"]);
            }
            else
            {
                BOOST_LOG_TRIVIAL(warning) << "HMS: load_from_local, no version info";
            }

            return 0;
        }
    } catch(...) {
        load_version = "0";
        BOOST_LOG_TRIVIAL(error) << "HMS: load_from_local failed";
        return -1;
    }
    load_version = "0";
    return 0;
}

int HMSQuery::save_to_local(std::string lang, std::string hms_type, std::string dev_id_type, json save_json)
{
    if (data_dir().empty()) {
        BOOST_LOG_TRIVIAL(error) << "HMS: save_to_local, data_dir() is empty";
        return -1;
    }
    std::string filename = get_hms_file(hms_type,lang, dev_id_type);
    auto hms_folder = (boost::filesystem::path(data_dir()) / "hms");
    if (!fs::exists(hms_folder))
        fs::create_directory(hms_folder);
    std::string dir_str = (hms_folder / filename).make_preferred().string();
    std::ofstream json_file(encode_path(dir_str.c_str()));
    if (json_file.is_open()) {
        json_file << std::setw(4) << save_json << std::endl;
        json_file.close();
        return 0;
    }
    BOOST_LOG_TRIVIAL(error) << "HMS: save_to_local failed";
    return -1;
}

std::string HMSQuery::hms_language_code()
{
    AppConfig* config = wxGetApp().app_config;
    if (!config)
        // set language code to en by default
        return "en";
    std::string lang_code = wxGetApp().app_config->get_language_code();
    if (lang_code.compare("uk") == 0
        || lang_code.compare("cs") == 0
        || lang_code.compare("ru") == 0) {
        BOOST_LOG_TRIVIAL(info) << "HMS: using english for lang_code = " << lang_code;
        return "en";
    }
    else if (lang_code.empty()) {
        // set language code to en by default
        return "en";
    }
    return lang_code;
}

std::string HMSQuery::build_query_params(std::string& lang)
{
    std::string lang_code = HMSQuery::hms_language_code();
    lang = lang_code;
    std::string query_params = (boost::format("lang=%1%") % lang_code).str();
    return query_params;
}

std::string HMSQuery::get_hms_file(std::string hms_type, std::string lang, std::string dev_id_type)
{
    //return hms action filename
    if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
        return (boost::format("hms_action_%1%.json") % dev_id_type).str();
    }
    //return hms filename
    return (boost::format("hms_%1%_%2%.json") % lang % dev_id_type).str();
}

wxString HMSQuery::query_hms_msg(const MachineObject* obj, const std::string& long_error_code)
{
    if (!obj)
    {
        return wxEmptyString;
    }

    AppConfig* config = wxGetApp().app_config;
    if (!config) return wxEmptyString;
    const std::string& lang_code = HMSQuery::hms_language_code();
    return _query_hms_msg(get_dev_id_type(obj),long_error_code, lang_code);
}

wxString HMSQuery::query_hms_msg(const std::string& dev_id, const std::string& long_error_code)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config) return wxEmptyString;
    const std::string& lang_code = HMSQuery::hms_language_code();
    return _query_hms_msg(dev_id.substr(0, 3), long_error_code, lang_code);
}

string HMSQuery::get_dev_id_type(const MachineObject* obj) const
{
    if (obj)
    {
        return obj->get_dev_id().substr(0, 3);
    }

    return string();
}

wxString HMSQuery::_query_hms_msg(const string& dev_id_type, const string& long_error_code, const string& lang_code)
{
    if (long_error_code.empty())
    {
        return wxEmptyString;
    }

    init_hms_info(dev_id_type);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end())
    {
        BOOST_LOG_TRIVIAL(error) << "there are no hms info for the device";
        return wxEmptyString;
    }

    const json& m_hms_info_json = iter->second;
    if (!m_hms_info_json.is_object())
    {
        BOOST_LOG_TRIVIAL(error) << "the hms info is not a valid json object";
        return wxEmptyString;
    }

    const json& device_hms_json = m_hms_info_json.value("device_hms", json());
    if (device_hms_json.is_null() || !device_hms_json.is_object())
    {
        BOOST_LOG_TRIVIAL(error) << "there are no valid json object named device_hms";
        return wxEmptyString;
    }

    const json& device_hms_msg_json = device_hms_json.value(lang_code, json());
    if (device_hms_msg_json.is_null())
    {
        BOOST_LOG_TRIVIAL(error) << "hms: query_hms_msg, do not contains lang_code = " << lang_code;
        if (lang_code.empty()) /*traverse all if lang_code is empty*/
        {
            for (const auto& lang_item : device_hms_json)
            {
                for (const auto& msg_item : lang_item)
                {
                    if (msg_item.is_object())
                    {
                        const std::string& error_code = msg_item.value("ecode", json()).get<std::string>();
                        if (boost::to_upper_copy(error_code) == long_error_code && msg_item.contains("intro"))
                        {
                            BOOST_LOG_TRIVIAL(info) << "retry without lang_code successed.";
                            return wxString::FromUTF8(msg_item["intro"].get<std::string>());
                        }
                    }
                }
            }
        }

        return wxEmptyString;
    }

    for (const auto& item : device_hms_msg_json)
    {
        if (item.is_object())
        {
            const std::string& error_code = item.value("ecode", json()).get<std::string>();
            if (boost::to_upper_copy(error_code) == long_error_code && item.contains("intro"))
            {
                return wxString::FromUTF8(item["intro"].get<std::string>());
            }
        }
    }

    BOOST_LOG_TRIVIAL(error) << "hms: query_hms_msg, do not contains valid message, lang_code = " << lang_code << " long_error_code = " << long_error_code;
    return wxEmptyString;
}

bool HMSQuery::_is_internal_error(const string &dev_id_type,
                                  const string &error_code,
                                  const string &lang_code)
{
    init_hms_info(dev_id_type);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end()) { return false; }

    const json &m_hms_info_json = iter->second;
    if (m_hms_info_json.contains("device_error")) {
        if (m_hms_info_json["device_error"].contains(lang_code)) {
            for (auto item = m_hms_info_json["device_error"][lang_code].begin(); item != m_hms_info_json["device_error"][lang_code].end(); item++) {
                if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == error_code) {
                    if (item->contains("intro")) { return wxString::FromUTF8((*item)["intro"].get<std::string>()).IsEmpty(); }
                }
            }
        } else {
            // return first language
            if (!m_hms_info_json["device_error"].empty()) {
                for (auto lang : m_hms_info_json["device_error"]) {
                    for (auto item = lang.begin(); item != lang.end(); item++) {
                        if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == error_code) {
                            if (item->contains("intro")) { return wxString::FromUTF8((*item)["intro"].get<std::string>()).IsEmpty(); }
                        }
                    }
                }
            }
        }
    }

    return false;
}

wxString HMSQuery::_query_error_msg(const std::string &dev_id_type,
                                    const std::string& error_code,
                                    const std::string& lang_code)
{
    init_hms_info(dev_id_type);
    auto iter = m_hms_info_jsons.find(dev_id_type);
    if (iter == m_hms_info_jsons.end())
    {
        return wxEmptyString;
    }

    const json& m_hms_info_json = iter->second;
    if (m_hms_info_json.contains("device_error")) {
        if (m_hms_info_json["device_error"].contains(lang_code)) {
            for (auto item = m_hms_info_json["device_error"][lang_code].begin(); item != m_hms_info_json["device_error"][lang_code].end(); item++) {
                if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == error_code) {
                    if (item->contains("intro")) {
                        return wxString::FromUTF8((*item)["intro"].get<std::string>());
                    }
                }
            }
            BOOST_LOG_TRIVIAL(info) << "hms: query_error_msg, not found error_code = " << error_code;
        } else {
            BOOST_LOG_TRIVIAL(error) << "hms: query_error_msg, do not contains lang_code = " << lang_code;
            // return first language
            if (!m_hms_info_json["device_error"].empty()) {
                for (auto lang : m_hms_info_json["device_error"]) {
                    for (auto item = lang.begin(); item != lang.end(); item++) {
                        if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == error_code) {
                            if (item->contains("intro")) {
                                return wxString::FromUTF8((*item)["intro"].get<std::string>());
                            }
                        }
                    }
                }
            }
        }
    }
    else {
        BOOST_LOG_TRIVIAL(info) << "device_error is not exists";
        return wxEmptyString;
    }

    return wxEmptyString;
}

wxString HMSQuery::_query_error_image_action(const std::string& dev_id_type, const std::string& long_error_code, std::vector<int>& button_action)
{
    init_hms_info(dev_id_type);

    auto iter = m_hms_action_jsons.find(dev_id_type);
    if (iter == m_hms_action_jsons.end())
    {
        return wxEmptyString;
    }

    const json& m_hms_action_json = iter->second;
    if (m_hms_action_json.contains("data")) {
        for (auto item = m_hms_action_json["data"].begin(); item != m_hms_action_json["data"].end(); item++) {
            if (item->contains("ecode") && boost::to_upper_copy((*item)["ecode"].get<std::string>()) == long_error_code) {
                if (item->contains("device") && (boost::to_upper_copy((*item)["device"].get<std::string>()) == dev_id_type ||
                    (*item)["device"].get<std::string>() == "default")) {
                    if (item->contains("actions")) {
                        for (auto item_actions = (*item)["actions"].begin(); item_actions != (*item)["actions"].end(); item_actions++) {
                            button_action.emplace_back(item_actions->get<int>());
                        }
                    }
                    if (item->contains("image")) {
                        return wxString::FromUTF8((*item)["image"].get<std::string>());
                    }
                }
            }
        }
    }
    else {
        BOOST_LOG_TRIVIAL(info) << "data is not exists";
        return wxEmptyString;
    }
    return wxEmptyString;
}


bool HMSQuery::is_internal_error(const MachineObject *obj, int print_error)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    std::string lang_code = HMSQuery::hms_language_code();
    return _is_internal_error(get_dev_id_type(obj), std::string(buf), lang_code);
}

wxString HMSQuery::query_print_error_msg(const MachineObject *obj, int print_error)
{
    if (!obj)
    {
        return wxEmptyString;
    }

    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    std::string lang_code = HMSQuery::hms_language_code();
    return _query_error_msg(get_dev_id_type(obj), std::string(buf), lang_code);
}

wxString HMSQuery::query_print_error_msg(const std::string& dev_id, int print_error)
{
    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    std::string lang_code = HMSQuery::hms_language_code();
    return _query_error_msg(dev_id.substr(0, 3), std::string(buf), lang_code);
}


wxString HMSQuery::query_print_image_action(const MachineObject* obj, int print_error, std::vector<int>& button_action)
{
    if (!obj)
    {
        return wxEmptyString;
    }

    char buf[32];
    ::sprintf(buf, "%08X", print_error);
    //The first three digits of SN number
    const auto result = _query_error_image_action(get_dev_id_type(obj),std::string(buf), button_action);
    if (wxGetApp().app_config->get_stealth_mode() && result.Contains("http")) {
        return wxEmptyString;
    }
    return result;
}

wxImage HMSQuery::query_image_from_local(const wxString& image_name)
{
    if (image_name.empty() || image_name.Contains("http"))
    {
        return wxImage();
    }

    if (m_hms_local_images.empty())
    {
        const fs::path& local_img_dir = fs::path(Slic3r::data_dir()) / HMS_LOCAL_IMG_PATH;
        if (fs::exists(local_img_dir))
        {
            for (const auto &entry : fs::directory_iterator(local_img_dir))
            {
                const fs::path& image_path = entry.path();
                const fs::path& image_name = fs::relative(image_path, local_img_dir);
                m_hms_local_images[image_name.string()] = wxImage(wxString::FromUTF8(image_path.string()));
            }
        }
    }

    auto iter = m_hms_local_images.find(image_name);
    if (iter != m_hms_local_images.end())
    {
        return iter->second;
    }

    return wxImage();
}

void HMSQuery::clear_hms_info()
{
    std::unique_lock unique_lock(m_hms_mutex);
    m_hms_info_jsons.clear();
    m_hms_action_jsons.clear();
    m_cloud_hms_last_update_time.clear();
}

void HMSQuery::init_hms_info(const std::string& dev_type_id)
{
    std::unique_lock unique_lock(m_hms_mutex);
    if (package_dev_id_types.count(dev_type_id) != 0)
    {
        /*the local one only load once*/
        if (m_hms_info_jsons.count(dev_type_id) == 0) {

            std::string load_version;
            load_from_local(QUERY_HMS_INFO, dev_type_id, &m_hms_info_jsons[dev_type_id], load_version);/*load from local first*/
            if (load_version.empty() || load_version == "0") {
                copy_from_data_dir_to_local(); // STUDIO-9512
                load_from_local(QUERY_HMS_INFO, dev_type_id, &m_hms_info_jsons[dev_type_id], load_version);/*copy files to local, and retry load*/
            }
        }

        if (m_hms_action_jsons.count(dev_type_id) == 0) {
            std::string load_version;
            load_from_local(QUERY_HMS_ACTION, dev_type_id, &m_hms_action_jsons[dev_type_id], load_version);/*load from local first*/
            if (load_version.empty() || load_version == "0") {
                copy_from_data_dir_to_local(); // STUDIO-9512
                load_from_local(QUERY_HMS_ACTION, dev_type_id, &m_hms_action_jsons[dev_type_id], load_version);/*copy files to local, and retry load*/
            }
        }
    }

    /*download from cloud*/
    time_t info_last_update_time = m_cloud_hms_last_update_time[dev_type_id];

    /* check hms is valid or not */
    bool retry = false;
    if(m_hms_info_jsons[dev_type_id].empty() || m_hms_action_jsons[dev_type_id].empty()){
        retry =time(nullptr) - info_last_update_time > (60 * 1); // retry after 1 minute
    }

    if (time(nullptr) - info_last_update_time > (60 * 60 * 24) || retry)/*do not update in one day to reduce waiting*/
    {
        download_hms_related(QUERY_HMS_INFO, dev_type_id, &m_hms_info_jsons[dev_type_id]);
        download_hms_related(QUERY_HMS_ACTION, dev_type_id, &m_hms_action_jsons[dev_type_id]);
        m_cloud_hms_last_update_time[dev_type_id] = time(nullptr);
    }
}

std::string get_hms_wiki_url(std::string error_code)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config) return "";
    if (config->get_stealth_mode()) return "";

    std::string hms_host = wxGetApp().app_config->get_hms_host();
    std::string lang_code = HMSQuery::hms_language_code();
    std::string url = (boost::format("https://%1%/index.php?e=%2%&s=device_hms&lang=%3%")
        % hms_host
        % error_code
        % lang_code).str();

    DeviceManager* dev = Slic3r::GUI::wxGetApp().getDeviceManager();
    if (!dev) return url;
    MachineObject* obj = dev->get_selected_machine();
    if (!obj) return url;

    if (!obj->get_dev_id().empty()) {
        url = (boost::format("https://%1%/index.php?e=%2%&d=%3%&s=device_hms&lang=%4%")
                       % hms_host
                       % error_code
                       % obj->get_dev_id()
                       % lang_code).str();
    }
    return url;
}

std::string get_error_message(int error_code)
{
    if (wxGetApp().app_config->get_stealth_mode()) return "";

	char buf[64];
    std::string result_str = "";
    std::sprintf(buf,"%08X",error_code);
    std::string hms_host = wxGetApp().app_config->get_hms_host();
    std::string get_lang = wxGetApp().app_config->get_language_code();

    std::string url = (boost::format("https://%1%/query.php?lang=%2%&e=%3%")
                        %hms_host
                        %get_lang
                        %buf).str();

    Slic3r::Http http = Slic3r::Http::get(url);
    http.header("accept", "application/json")
        .timeout_max(10)
        .on_complete([get_lang, &result_str](std::string body, unsigned status) {
            try {
                json j = json::parse(body);
                if (j.contains("result")) {
                    if (j["result"].get<int>() == 0) {
                        if (j.contains("data")) {
                            json jj = j["data"];
                            if (jj.contains("device_error")) {
                                if (jj["device_error"].contains(get_lang)) {
                                    if (jj["device_error"][get_lang].size() > 0) {
                                        if (!jj["device_error"][get_lang][0]["intro"].empty() || !jj["device_error"][get_lang][0]["ecode"].empty()) {
                                            std::string error_info = jj["device_error"][get_lang][0]["intro"].get<std::string>();
                                            std::string error_code = jj["device_error"][get_lang][0]["ecode"].get<std::string>();
                                            error_code.insert(4, " ");
                                            result_str = from_u8(error_info).ToStdString() + "[" + error_code + "]";
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (...) {
             ;
        }
        })
        .on_error([](std::string body, std::string error, unsigned status) {
            BOOST_LOG_TRIVIAL(info) << boost::format("[BBL ErrorMessage]: status=%1%, error=%2%, body=%3%") % status % error % body;
        }).perform_sync();

        return result_str;
}

}
}
