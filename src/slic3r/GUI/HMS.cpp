#include "HMS.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "DeviceManager.hpp"
#include "DeviceCore/DevManager.h"
#include "DeviceCore/DevUtil.h"
#include "Http.hpp"
#include <nlohmann/json.hpp>
#include "libslic3r/AppConfig.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/directory.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <boost/system/error_code.hpp>
#include <unordered_set>
#include <string>
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"
#include <miniz.h>
#include <iterator>
#include <fstream>
#include <exception>
#include <ios>
#include <ostream>
#include <wx/string.h>
#include <vector>
#include <cstdio>
#include <wx/image.h>
#include <mutex>
#include <ctime>

namespace fs = boost::filesystem;
using json = nlohmann::json;

static const char* HMS_PATH = "hms";
static const char* HMS_LOCAL_IMG_PATH = "hms/local_image";
static const char* HMS_ZIP_FILE = "hms.zip"; // Orca: written by cmake/modules/PackHmsResources.cmake

// the local HMS info
// Orca: dev-id-type set trimmed to the devices Orca ships local HMS images for
static std::unordered_set<std::string> package_dev_id_types {"094", "239", "093", "22E", "20P", "26A", "31B"};

// Orca: HMS should be disabled when stealth mode is on or networking is not installed
static bool should_disable_hms()
{
    Slic3r::AppConfig* config = Slic3r::GUI::wxGetApp().app_config;
    if (!config) return true;
    return config->get_stealth_mode() || !config->get_bool("installed_networking");
}

namespace Slic3r {
namespace GUI {

int get_hms_info_version(std::string& version)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config)
        return -1;
    if (should_disable_hms())
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
        .on_complete([&version](std::string body, unsigned status){
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
    if (should_disable_hms()) return -1;

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
    bool answered      = false; // Orca: the server replied with a result, whatever it is
    json j;

    BOOST_LOG_TRIVIAL(info) << "hms: download url = " << url;
    Slic3r::Http http = Slic3r::Http::get(url);
    http.on_complete([receive_json, hms_type, &to_save_local, &answered, &j, & local_version](std::string body, unsigned status) {
        try {
            j = json::parse(body);
            if (j.contains("result")) {
                answered = true;
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
    return answered ? 0 : -1;
}


// Orca: read a whole file into memory
static bool read_file(const fs::path& file_path, std::string& content)
{
    std::ifstream file(encode_path(fs::path(file_path).make_preferred().string().c_str()), std::ios::binary);
    if (!file.is_open())
        return false;
    content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return !file.bad();
}

// Orca: read a bundled HMS file from hms.zip, or from the plain JSON when there is no zip (source-tree builds).
// Plain files next to hms.zip are leftovers of an older version and are ignored.
static bool read_bundled_hms_file(const std::string& filename, std::string& content)
{
    const fs::path hms_dir  = fs::path(Slic3r::resources_dir()) / HMS_PATH;
    const fs::path zip_path = hms_dir / HMS_ZIP_FILE;
    boost::system::error_code ec;
    if (!fs::exists(zip_path, ec))
        return read_file(hms_dir / filename, content);

    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, zip_path.string())) {
        BOOST_LOG_TRIVIAL(error) << "HMS: failed to open " << zip_path.string();
        return false;
    }
    bool ok = false;
    const int index = mz_zip_reader_locate_file(&zip, filename.c_str(), nullptr, 0);
    size_t size = 0;
    if (void* data = index >= 0 ? mz_zip_reader_extract_to_heap(&zip, mz_uint(index), &size, 0) : nullptr) {
        content.assign(static_cast<const char*>(data), size);
        mz_free(data);
        ok = true;
    } else {
        BOOST_LOG_TRIVIAL(error) << "HMS: failed to extract " << filename << " from " << zip_path.string();
    }
    close_zip_reader(&zip);
    return ok;
}

// Orca: parse HMS file content into load_json, using the same layout for the bundled and cached copies
static int parse_hms_file(const std::string& content, const std::string& hms_type, json* load_json, std::string& load_version)
{
    load_version = "0";
    try {
        const json j = json::parse(content);
        if (hms_type.compare(QUERY_HMS_INFO) == 0) {
            if (j.contains("data")) { (*load_json) = j["data"]; }
        } else if (hms_type.compare(QUERY_HMS_ACTION) == 0) {
            if (j.contains("data")) { (*load_json)["data"] = j["data"]; }
        }

        if (j.contains("version")) {
            load_version = DevJsonValParser::get_longlong_val(j["version"]);
        } else if (j.contains("ver")) {
            load_version = DevJsonValParser::get_longlong_val(j["ver"]);
        } else {
            BOOST_LOG_TRIVIAL(warning) << "HMS: no version info in the HMS file";
        }
    } catch (...) {
        load_version = "0";
        BOOST_LOG_TRIVIAL(error) << "HMS: failed to parse the HMS file";
        return -1;
    }
    return 0;
}

// Orca: only the file a connected printer needs is copied out of the install, instead of the whole folder (STUDIO-9512)
bool HMSQuery::copy_bundled_file_to_local(const std::string& filename)
{
    if (data_dir().empty())
        return false;

    std::string content;
    if (!read_bundled_hms_file(filename, content))
        return false;

    const fs::path to_dir = fs::path(Slic3r::data_dir()) / HMS_PATH;
    try {
        if (!fs::exists(to_dir))
            fs::create_directory(to_dir);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "HMS: failed to create " << to_dir.string() << ": " << e.what();
        return false;
    }
    std::ofstream file(encode_path((to_dir / filename).make_preferred().string().c_str()), std::ios::binary | std::ios::trunc);
    if (!file.is_open() || !file.write(content.data(), std::streamsize(content.size()))) {
        BOOST_LOG_TRIVIAL(error) << "HMS: failed to write " << (to_dir / filename).string();
        return false;
    }
    return true;
}

int HMSQuery::load_bundled(const std::string& hms_type, const std::string& dev_id_type, const std::string& lang, json* load_json)
{
    std::string content;
    if (!read_bundled_hms_file(get_hms_file(hms_type, lang, dev_id_type), content))
        return 0;
    std::string version;
    return parse_hms_file(content, hms_type, load_json, version);
}

// Orca: on an app version change, remove the cached files the install can regenerate, so the new bundled
// catalogs are used. Cloud-only catalogs are kept.
void HMSQuery::remove_bundled_cache()
{
    if (data_dir().empty())
        return;
    const fs::path cache_dir = fs::path(data_dir()) / HMS_PATH;
    boost::system::error_code ec;
    if (!fs::is_directory(cache_dir, ec))
        return;

    std::vector<fs::path> cached_files;
    for (fs::directory_iterator it(cache_dir, ec), end; !ec && it != end; it.increment(ec)) {
        boost::system::error_code file_ec;
        if (fs::is_regular_file(it->path(), file_ec))
            cached_files.push_back(it->path());
    }

    const fs::path hms_dir  = fs::path(Slic3r::resources_dir()) / HMS_PATH;
    const fs::path zip_path = hms_dir / HMS_ZIP_FILE;
    mz_zip_archive zip;
    mz_zip_zero_struct(&zip);
    const bool use_zip = fs::exists(zip_path, ec);
    if (use_zip && !open_zip_reader(&zip, zip_path.string())) {
        BOOST_LOG_TRIVIAL(error) << "HMS: failed to open " << zip_path.string() << ", cache left as is";
        return;
    }

    size_t removed = 0;
    for (const fs::path& file : cached_files) {
        const std::string name    = file.filename().string();
        const bool        bundled = use_zip ? mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE) >= 0 :
                                              fs::is_regular_file(hms_dir / name, ec);
        if (bundled && fs::remove(file, ec))
            ++removed;
    }
    if (use_zip)
        close_zip_reader(&zip);

    // Images are read from resources_dir; this old copy is unused.
    const auto removed_images = fs::remove_all(fs::path(data_dir()) / HMS_LOCAL_IMG_PATH, ec);
    BOOST_LOG_TRIVIAL(info) << "HMS: removed " << removed << " bundled catalogs and " << (ec ? 0 : removed_images)
                            << " entries of the old local_image copy from " << cache_dir.string();
}

int HMSQuery::load_from_local(const std::string& hms_type, const std::string& dev_id_type, json* load_json, std::string& load_version)
{
    if (data_dir().empty()) {
        load_version = "0";
        BOOST_LOG_TRIVIAL(error) << "HMS: load_from_local, data_dir() is empty";
        return -1;
    }
    std::string filename = get_hms_file(hms_type, HMSQuery::hms_language_code(), dev_id_type);
    std::string content;
    load_version = "0";
    if (!read_file(fs::path(data_dir()) / HMS_PATH / filename, content))
        return 0;
    return parse_hms_file(content, hms_type, load_json, load_version);
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
        json_file << save_json.dump() << std::endl; // Orca: compact dump, indentation only grows the cache
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
    // Orca: get_language_code() keeps only "zh" for zh_TW, but the HMS host publishes Traditional Chinese as "zh-tw"
    if (wxGetApp().app_config->get("language") == "zh_TW")
        return "zh-tw";
    // Orca: the HMS host ships no catalog for these locales, so fall back to english to avoid empty texts + retries
    if (lang_code.compare("uk") == 0
        || lang_code.compare("cs") == 0
        || lang_code.compare("ru") == 0
        || lang_code.compare("tr") == 0
        || lang_code.compare("pt") == 0
        || lang_code.compare("ko") == 0
        )
    {
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

std::string HMSQuery::get_dev_id_type(const MachineObject* obj) const
{
    if (obj)
    {
        return obj->get_dev_id().substr(0, 3);
    }

    return std::string();
}

wxString HMSQuery::_query_hms_msg(const std::string& dev_id_type, const std::string& long_error_code, const std::string& lang_code)
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

    // Orca: the bundled English catalog stands in until the user's language has been downloaded
    auto lang_iter = device_hms_json.find(lang_code);
    if (lang_iter == device_hms_json.end())
        lang_iter = device_hms_json.find("en");
    if (lang_iter == device_hms_json.end())
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

    for (const auto& item : *lang_iter)
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

bool HMSQuery::_is_internal_error(const std::string &dev_id_type,
                                  const std::string &error_code,
                                  const std::string &lang_code)
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
    if (should_disable_hms() && result.Contains("http")) {
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
        // Orca: the images are never updated at runtime, so read them from the install instead of a copy in data_dir
        const fs::path& local_img_dir = fs::path(Slic3r::resources_dir()) / HMS_LOCAL_IMG_PATH;
        if (fs::exists(local_img_dir))
        {
            for (const auto &entry : fs::directory_iterator(local_img_dir))
            {
                const fs::path& image_path = entry.path();
                const fs::path& image_name = fs::relative(image_path, local_img_dir);
                // Orca: from_path() for correct UTF-8 path encoding
                m_hms_local_images[from_path(image_name)] = wxImage(from_path(image_path));
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
    m_hms_english_fallback.clear();
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
                // Orca: without a bundled catalog, use English from memory only, so the download still fetches the full file
                if (copy_bundled_file_to_local(get_hms_file(QUERY_HMS_INFO, hms_language_code(), dev_type_id)))
                    load_from_local(QUERY_HMS_INFO, dev_type_id, &m_hms_info_jsons[dev_type_id], load_version);
                else {
                    load_bundled(QUERY_HMS_INFO, dev_type_id, "en", &m_hms_info_jsons[dev_type_id]);
                    m_hms_english_fallback.insert(dev_type_id);
                }
            }
        }

        if (m_hms_action_jsons.count(dev_type_id) == 0) {
            std::string load_version;
            load_from_local(QUERY_HMS_ACTION, dev_type_id, &m_hms_action_jsons[dev_type_id], load_version);/*load from local first*/
            if ((load_version.empty() || load_version == "0") && copy_bundled_file_to_local(get_hms_file(QUERY_HMS_ACTION, "", dev_type_id)))
                load_from_local(QUERY_HMS_ACTION, dev_type_id, &m_hms_action_jsons[dev_type_id], load_version);
        }
    }

    /*download from cloud*/
    time_t info_last_update_time = m_cloud_hms_last_update_time[dev_type_id];

    /* check hms is valid or not */
    bool retry = false;
    // Orca: the English fallback fills the table, so it needs the retry too, or a failed first download waits a day
    if(m_hms_info_jsons[dev_type_id].empty() || m_hms_action_jsons[dev_type_id].empty() || m_hms_english_fallback.count(dev_type_id) != 0){
        retry =time(nullptr) - info_last_update_time > (60 * 1); // retry after 1 minute
    }

    if (time(nullptr) - info_last_update_time > (60 * 60 * 24) || retry)/*do not update in one day to reduce waiting*/
    {
        if (download_hms_related(QUERY_HMS_INFO, dev_type_id, &m_hms_info_jsons[dev_type_id]) == 0)
            m_hms_english_fallback.erase(dev_type_id);
        download_hms_related(QUERY_HMS_ACTION, dev_type_id, &m_hms_action_jsons[dev_type_id]);
        m_cloud_hms_last_update_time[dev_type_id] = time(nullptr);
    }
}

std::string get_hms_wiki_url(std::string error_code)
{
    AppConfig* config = wxGetApp().app_config;
    if (!config) return "";
    if (should_disable_hms()) return "";

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
    if (should_disable_hms()) return "";

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
