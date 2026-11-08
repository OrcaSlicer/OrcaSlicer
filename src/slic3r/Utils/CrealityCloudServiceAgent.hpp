#pragma once

#include "CloudProvider.hpp"
#include "ICloudServiceAgent.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/ProjectTask.hpp"

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

// The Creality Cloud account, kept separate from the Orca, Bambu and Elegoo accounts.
//
// It only signs in and out: the session lives in CrealityCloud::SessionStore, where every
// CrealityCloudPrinterAgent reads it. Sign-in goes through the shared login dialog, which loads the
// bundled resources/web/creality/login.html and hands its "user_login" message to change_user().
// Settings sync, model mall, tracking and the other Bambu-shaped services are not offered.
class CrealityCloudServiceAgent : public ICloudServiceAgent
{
public:
    explicit CrealityCloudServiceAgent(std::string log_dir);
    ~CrealityCloudServiceAgent() override = default;

    std::string get_id() const override { return CREALITY_CLOUD_PROVIDER; }

    int init_log() override { return BAMBU_NETWORK_SUCCESS; }
    int set_config_dir(std::string config_dir) override;
    int set_cert_file(std::string, std::string) override { return BAMBU_NETWORK_SUCCESS; }
    int set_country_code(std::string) override { return BAMBU_NETWORK_SUCCESS; }
    int start() override { return BAMBU_NETWORK_SUCCESS; }

    int         change_user(std::string user_info) override;
    bool        is_user_login() override;
    int         user_logout(bool request = false) override;
    std::string get_user_id() override;
    std::string get_user_name() override;
    std::string get_user_avatar() override;
    std::string get_user_nickname() override;

    std::string build_login_cmd() override;
    std::string build_logout_cmd() override;
    std::string build_login_info() override;

    std::string get_access_token() const override;
    std::string get_refresh_token() const override { return {}; }
    bool        ensure_token_fresh(const std::string& reason) override;

    std::string get_cloud_service_host() override;
    std::string get_cloud_login_url(const std::string& language = "") override;
    int         connect_server() override { return BAMBU_NETWORK_SUCCESS; }
    bool        is_server_connected() override { return is_user_login(); }
    int         refresh_connection() override { return BAMBU_NETWORK_SUCCESS; }
    int         start_subscribe(std::string) override { return BAMBU_NETWORK_SUCCESS; }
    int         stop_subscribe(std::string) override { return BAMBU_NETWORK_SUCCESS; }
    int         add_subscribe(std::vector<std::string>) override { return BAMBU_NETWORK_SUCCESS; }
    int         del_subscribe(std::vector<std::string>) override { return BAMBU_NETWORK_SUCCESS; }
    void        enable_multi_machine(bool) override {}

    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    std::string request_setting_id(std::string, std::map<std::string, std::string>*, unsigned int*) override { return {}; }
    int put_setting(std::string, std::string, std::map<std::string, std::string>*, unsigned int*, bool = false) override
    {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int get_setting_list(std::string, ProgressFn = nullptr, WasCancelledFn = nullptr) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_setting_list2(std::string, CheckFn, ProgressFn = nullptr, WasCancelledFn = nullptr) override
    {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int delete_setting(std::string) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }

    int get_my_message(int, int, int, unsigned int*, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int check_user_task_report(int*, bool*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    // The account's printers as {"devices":[{dev_id, dev_name, dev_type, online, ...}]}.
    int get_user_print_info(unsigned int* http_code, std::string* http_body) override;
    int get_user_tasks(TaskQueryParams, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_printer_firmware(std::string, unsigned*, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_task_plate_index(std::string, int*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_user_info(int* identifier) override;
    int get_subtask_info(std::string, std::string*, unsigned int*, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_slice_info(std::string, std::string, int, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int query_bind_status(std::vector<std::string>, unsigned int*, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int modify_printer_name(std::string, std::string) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }

    int get_camera_url(std::string, std::function<void(std::string)>) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_design_staffpick(int, int, std::function<void(std::string)>) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int start_publish(PublishParams, OnUpdateStatusFn, WasCancelledFn, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_model_publish_url(std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_subtask(BBLModelTask*, OnGetSubTaskFn) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_model_mall_home_url(std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_model_mall_detail_url(std::string*, std::string) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_my_profile(std::string, unsigned int*, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int get_my_token(std::string, unsigned int*, std::string*) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }

    int  track_enable(bool) override { return BAMBU_NETWORK_SUCCESS; }
    int  track_remove_files() override { return BAMBU_NETWORK_SUCCESS; }
    int  track_event(std::string, std::string) override { return BAMBU_NETWORK_SUCCESS; }
    int  track_header(std::string) override { return BAMBU_NETWORK_SUCCESS; }
    int  track_update_property(std::string, std::string, std::string = "string") override { return BAMBU_NETWORK_SUCCESS; }
    int  track_get_property(std::string, std::string&, std::string = "string") override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    bool get_track_enable() override { return false; }

    int put_model_mall_rating(int, int, std::string, std::vector<std::string>, unsigned int&, std::string&) override
    {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int get_oss_config(std::string&, std::string, unsigned int&, std::string&) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int put_rating_picture_oss(std::string&, std::string&, std::string, int, unsigned int&, std::string&) override
    {
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;
    }
    int get_model_mall_rating_result(int, std::string&, unsigned int&, std::string&) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }

    int         get_mw_user_preference(std::function<void(std::string)>) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    int         get_mw_user_4ulist(int, int, std::function<void(std::string)>) override { return BAMBU_NETWORK_ERR_INVALID_HANDLE; }
    std::string get_version() override;

    int set_on_server_connected_fn(AppOnServerConnectedFn fn) override;
    int set_on_http_error_fn(AppOnHttpErrorFn fn) override;
    int set_get_country_code_fn(GetCountryCodeFn) override { return BAMBU_NETWORK_SUCCESS; }
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    // Error text of the last failed sign-in, for the caller to show.
    std::string last_error() const;

private:
    void report_login_failure(const std::string& error);

    mutable std::mutex     m_mutex;
    std::string            m_log_dir;
    std::string            m_last_error;
    AppOnServerConnectedFn m_on_server_connected;
    AppOnHttpErrorFn       m_on_http_error;
    QueueOnMainFn          m_queue_on_main;
};

} // namespace Slic3r
