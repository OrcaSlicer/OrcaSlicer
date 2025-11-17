#ifndef slic3r_HMS_hpp_
#define slic3r_HMS_hpp_

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/StepCtrl.hpp"
#include "BitmapCache.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Thread.hpp"
#include "nlohmann/json.hpp"
#include <mutex>

namespace Slic3r {

class MachineObject;

namespace GUI {

#define HMS_INFO_FILE	"hms.json"
#define QUERY_HMS_INFO	"query_hms_info"
#define QUERY_HMS_ACTION	"query_hms_action"

class HMSQuery {

protected:
    std::unordered_map<string, json> m_hms_info_jsons;  // key-> device id type, the first three digits of SN number
    std::unordered_map<string, json> m_hms_action_jsons;// key-> device id type
    std::unordered_map<wxString, wxImage> m_hms_local_images; // key-> image name
    mutable std::mutex m_hms_mutex;

    std::unordered_map<string, time_t> m_cloud_hms_last_update_time;

public:
    HMSQuery() { }
    ~HMSQuery() { clear_hms_info(); };

public:
    // clear hms
    void      clear_hms_info();

    // query
    wxString  query_hms_msg(const MachineObject* obj, const std::string& long_error_code);
    wxString  query_hms_msg(const std::string& dev_id, const std::string& long_error_code);

    bool      is_internal_error(const MachineObject *obj, int print_error);
    wxString  query_print_error_msg(const MachineObject* obj, int print_error);
    wxString  query_print_error_msg(const std::string& dev_id, int print_error);
    wxString  query_print_image_action(const MachineObject* obj, int print_error, std::vector<int>& button_action);

    // query local images
    wxImage   query_image_from_local(const wxString& image_name);

public:
    static std::string hms_language_code();
    static std::string build_query_params(std::string& lang);

private:
    // load hms
    void init_hms_info(const std::string& dev_type_id);
    void copy_from_data_dir_to_local();
    int  download_hms_related(const std::string& hms_type, const std::string& dev_id_type, json* receive_json);
    int  load_from_local(const std::string& hms_type, const std::string& dev_id_type, json* receive_json, std::string& version_info);
    int  save_to_local(std::string lang, std::string hms_type, std::string dev_id_type, json save_json);
    std::string get_hms_file(std::string hms_type, std::string lang = std::string("en"), std::string dev_id_type = "");

    // internal query
    string    get_dev_id_type(const MachineObject* obj) const;
    wxString _query_hms_msg(const string& dev_id_type, const string& long_error_code, const string& lang_code = std::string("en"));

    bool     _is_internal_error(const string &dev_id_type, const string &long_error_code, const string &lang_code = std::string("en"));
    wxString _query_error_msg(const string& dev_id_type, const std::string& long_error_code, const std::string& lang_code = std::string("en"));
    wxString _query_error_image_action(const string& dev_id_type, const std::string& long_error_code, std::vector<int>& button_action);
};

int get_hms_info_version(std::string &version);

std::string get_hms_wiki_url(std::string code);

std::string get_error_message(int error_code);

}
}


#endif