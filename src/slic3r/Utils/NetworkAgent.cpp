#include <stdio.h>
#include <stdlib.h>
#if defined(_MSC_VER) || defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#endif

#include <boost/log/trivial.hpp>
#include "libslic3r/Utils.hpp"
#include "NetworkAgent.hpp"

#include "slic3r/Utils/FileTransferUtils.hpp"

using namespace BBL;

namespace Slic3r {

#define BAMBU_SOURCE_LIBRARY "BambuSource"

#if defined(_MSC_VER) || defined(_WIN32)
static HMODULE netwoking_module = NULL;
static HMODULE source_module = NULL;
#else
static void* netwoking_module = NULL;
static void* source_module = NULL;
#endif

bool NetworkAgent::use_legacy_network = true;

typedef int (*func_start_print_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_with_record_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_send_gcode_to_sdcard_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn);
typedef int (*func_start_local_print_legacy)(void *agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_start_sdcard_print_legacy)(void* agent, PrintParams_Legacy params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);
typedef int (*func_send_message_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);
typedef int (*func_send_message_to_printer_legacy)(void* agent, std::string dev_id, std::string json_str, int qos);

func_check_debug_consistent         NetworkAgent::check_debug_consistent_ptr = nullptr;
func_get_version                    NetworkAgent::get_version_ptr = nullptr;
func_create_agent                   NetworkAgent::create_agent_ptr = nullptr;
func_destroy_agent                  NetworkAgent::destroy_agent_ptr = nullptr;
func_init_log                       NetworkAgent::init_log_ptr = nullptr;
func_set_config_dir                 NetworkAgent::set_config_dir_ptr = nullptr;
func_set_cert_file                  NetworkAgent::set_cert_file_ptr = nullptr;
func_set_country_code               NetworkAgent::set_country_code_ptr = nullptr;
func_start                          NetworkAgent::start_ptr = nullptr;
func_set_on_ssdp_msg_fn             NetworkAgent::set_on_ssdp_msg_fn_ptr = nullptr;
func_set_on_user_login_fn           NetworkAgent::set_on_user_login_fn_ptr = nullptr;
func_set_on_printer_connected_fn    NetworkAgent::set_on_printer_connected_fn_ptr = nullptr;
func_set_on_server_connected_fn     NetworkAgent::set_on_server_connected_fn_ptr = nullptr;
func_set_on_http_error_fn           NetworkAgent::set_on_http_error_fn_ptr = nullptr;
func_set_get_country_code_fn        NetworkAgent::set_get_country_code_fn_ptr = nullptr;
func_set_on_subscribe_failure_fn    NetworkAgent::set_on_subscribe_failure_fn_ptr = nullptr;
func_set_on_message_fn              NetworkAgent::set_on_message_fn_ptr = nullptr;
func_set_on_user_message_fn         NetworkAgent::set_on_user_message_fn_ptr = nullptr;
func_set_on_local_connect_fn        NetworkAgent::set_on_local_connect_fn_ptr = nullptr;
func_set_on_local_message_fn        NetworkAgent::set_on_local_message_fn_ptr = nullptr;
func_set_queue_on_main_fn           NetworkAgent::set_queue_on_main_fn_ptr = nullptr;
func_connect_server                 NetworkAgent::connect_server_ptr = nullptr;
func_is_server_connected            NetworkAgent::is_server_connected_ptr = nullptr;
func_refresh_connection             NetworkAgent::refresh_connection_ptr = nullptr;
func_start_subscribe                NetworkAgent::start_subscribe_ptr = nullptr;
func_stop_subscribe                 NetworkAgent::stop_subscribe_ptr = nullptr;
func_add_subscribe                  NetworkAgent::add_subscribe_ptr = nullptr;
func_del_subscribe                  NetworkAgent::del_subscribe_ptr = nullptr;
func_enable_multi_machine           NetworkAgent::enable_multi_machine_ptr = nullptr;
func_send_message                   NetworkAgent::send_message_ptr = nullptr;
func_connect_printer                NetworkAgent::connect_printer_ptr = nullptr;
func_disconnect_printer             NetworkAgent::disconnect_printer_ptr = nullptr;
func_send_message_to_printer        NetworkAgent::send_message_to_printer_ptr = nullptr;
func_check_cert                     NetworkAgent::check_cert_ptr = nullptr;
func_install_device_cert            NetworkAgent::install_device_cert_ptr = nullptr;
func_start_discovery                NetworkAgent::start_discovery_ptr = nullptr;
func_change_user                    NetworkAgent::change_user_ptr = nullptr;
func_is_user_login                  NetworkAgent::is_user_login_ptr = nullptr;
func_user_logout                    NetworkAgent::user_logout_ptr = nullptr;
func_get_user_id                    NetworkAgent::get_user_id_ptr = nullptr;
func_get_user_name                  NetworkAgent::get_user_name_ptr = nullptr;
func_get_user_avatar                NetworkAgent::get_user_avatar_ptr = nullptr;
func_get_user_nickanme              NetworkAgent::get_user_nickanme_ptr = nullptr;
func_build_login_cmd                NetworkAgent::build_login_cmd_ptr = nullptr;
func_build_logout_cmd               NetworkAgent::build_logout_cmd_ptr = nullptr;
func_build_login_info               NetworkAgent::build_login_info_ptr = nullptr;
func_ping_bind                      NetworkAgent::ping_bind_ptr = nullptr;
func_bind_detect                    NetworkAgent::bind_detect_ptr = nullptr;
func_set_server_callback            NetworkAgent::set_server_callback_ptr = nullptr;
func_bind                           NetworkAgent::bind_ptr = nullptr;
func_unbind                         NetworkAgent::unbind_ptr = nullptr;
func_get_bambulab_host              NetworkAgent::get_bambulab_host_ptr = nullptr;
func_get_user_selected_machine      NetworkAgent::get_user_selected_machine_ptr = nullptr;
func_set_user_selected_machine      NetworkAgent::set_user_selected_machine_ptr = nullptr;
func_start_print                    NetworkAgent::start_print_ptr = nullptr;
func_start_local_print_with_record  NetworkAgent::start_local_print_with_record_ptr = nullptr;
func_start_send_gcode_to_sdcard     NetworkAgent::start_send_gcode_to_sdcard_ptr = nullptr;
func_start_local_print              NetworkAgent::start_local_print_ptr = nullptr;
func_start_sdcard_print             NetworkAgent::start_sdcard_print_ptr = nullptr;
func_get_user_presets               NetworkAgent::get_user_presets_ptr = nullptr;
func_request_setting_id             NetworkAgent::request_setting_id_ptr = nullptr;
func_put_setting                    NetworkAgent::put_setting_ptr = nullptr;
func_get_setting_list               NetworkAgent::get_setting_list_ptr = nullptr;
func_get_setting_list2              NetworkAgent::get_setting_list2_ptr = nullptr;
func_delete_setting                 NetworkAgent::delete_setting_ptr = nullptr;
func_get_studio_info_url            NetworkAgent::get_studio_info_url_ptr = nullptr;
func_set_extra_http_header          NetworkAgent::set_extra_http_header_ptr = nullptr;
func_get_my_message                 NetworkAgent::get_my_message_ptr = nullptr;
func_check_user_task_report         NetworkAgent::check_user_task_report_ptr = nullptr;
func_get_user_print_info            NetworkAgent::get_user_print_info_ptr = nullptr;
func_get_user_tasks                 NetworkAgent::get_user_tasks_ptr = nullptr;
func_get_printer_firmware           NetworkAgent::get_printer_firmware_ptr = nullptr;
func_get_task_plate_index           NetworkAgent::get_task_plate_index_ptr = nullptr;
func_get_user_info                  NetworkAgent::get_user_info_ptr = nullptr;
func_request_bind_ticket            NetworkAgent::request_bind_ticket_ptr = nullptr;
func_get_subtask_info               NetworkAgent::get_subtask_info_ptr = nullptr;
func_get_slice_info                 NetworkAgent::get_slice_info_ptr = nullptr;
func_query_bind_status              NetworkAgent::query_bind_status_ptr = nullptr;
func_modify_printer_name            NetworkAgent::modify_printer_name_ptr = nullptr;
func_get_camera_url                 NetworkAgent::get_camera_url_ptr = nullptr;
func_get_design_staffpick           NetworkAgent::get_design_staffpick_ptr = nullptr;
func_start_pubilsh                  NetworkAgent::start_publish_ptr = nullptr;
func_get_model_publish_url          NetworkAgent::get_model_publish_url_ptr = nullptr;
func_get_model_mall_home_url        NetworkAgent::get_model_mall_home_url_ptr = nullptr;
func_get_model_mall_detail_url      NetworkAgent::get_model_mall_detail_url_ptr = nullptr;
func_get_subtask                    NetworkAgent::get_subtask_ptr = nullptr;
func_get_my_profile                 NetworkAgent::get_my_profile_ptr = nullptr;
func_track_enable                   NetworkAgent::track_enable_ptr = nullptr;
func_track_remove_files             NetworkAgent::track_remove_files_ptr = nullptr;
func_track_event                    NetworkAgent::track_event_ptr = nullptr;
func_track_header                   NetworkAgent::track_header_ptr = nullptr;
func_track_update_property          NetworkAgent::track_update_property_ptr = nullptr;
func_track_get_property             NetworkAgent::track_get_property_ptr = nullptr;
func_put_model_mall_rating_url      NetworkAgent::put_model_mall_rating_url_ptr = nullptr;
func_get_oss_config                 NetworkAgent::get_oss_config_ptr = nullptr;
func_put_rating_picture_oss         NetworkAgent::put_rating_picture_oss_ptr = nullptr;
func_get_model_mall_rating_result   NetworkAgent::get_model_mall_rating_result_ptr  = nullptr;

func_get_mw_user_preference         NetworkAgent::get_mw_user_preference_ptr = nullptr;
func_get_mw_user_4ulist             NetworkAgent::get_mw_user_4ulist_ptr     = nullptr;

static PrintParams_Legacy as_legacy(PrintParams& param)
{
    PrintParams_Legacy l;

    l.dev_id                = std::move(param.dev_id);
    l.task_name             = std::move(param.task_name);
    l.project_name          = std::move(param.project_name);
    l.preset_name           = std::move(param.preset_name);
    l.filename              = std::move(param.filename);
    l.config_filename       = std::move(param.config_filename);
    l.plate_index           = param.plate_index;
    l.ftp_folder            = std::move(param.ftp_folder);
    l.ftp_file              = std::move(param.ftp_file);
    l.ftp_file_md5          = std::move(param.ftp_file_md5);
    l.ams_mapping           = std::move(param.ams_mapping);
    l.ams_mapping_info      = std::move(param.ams_mapping_info);
    l.connection_type       = std::move(param.connection_type);
    l.comments              = std::move(param.comments);
    l.origin_profile_id     = param.origin_profile_id;
    l.stl_design_id         = param.stl_design_id;
    l.origin_model_id       = std::move(param.origin_model_id);
    l.print_type            = std::move(param.print_type);
    l.dst_file              = std::move(param.dst_file);
    l.dev_name              = std::move(param.dev_name);
    l.dev_ip                = std::move(param.dev_ip);
    l.use_ssl_for_ftp       = param.use_ssl_for_ftp;
    l.use_ssl_for_mqtt      = param.use_ssl_for_mqtt;
    l.username              = std::move(param.username);
    l.password              = std::move(param.password);
    l.task_bed_leveling     = param.task_bed_leveling;
    l.task_flow_cali        = param.task_flow_cali;
    l.task_vibration_cali   = param.task_vibration_cali;
    l.task_layer_inspect    = param.task_layer_inspect;
    l.task_record_timelapse = param.task_record_timelapse;
    l.task_use_ams          = param.task_use_ams;
    l.task_bed_type         = std::move(param.task_bed_type);
    l.extra_options         = std::move(param.extra_options);

    return l;
}

NetworkAgent::NetworkAgent(std::string log_dir)
{
    if (create_agent_ptr) {
        network_agent = create_agent_ptr(log_dir);
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", this %1%, network_agent=%2%, create_agent_ptr=%3%, log_dir=%4%")%this %network_agent %create_agent_ptr %log_dir;
}

NetworkAgent::~NetworkAgent()
{
    int ret = 0;
    if (network_agent && destroy_agent_ptr) {
        ret = destroy_agent_ptr(network_agent);
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", this %1%, network_agent=%2%, destroy_agent_ptr=%3%, ret %4%")%this %network_agent %destroy_agent_ptr %ret;
}

std::string NetworkAgent::get_libpath_in_current_directory(std::string library_name)
{
    std::string lib_path;
#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t file_name[512];
    DWORD ret = GetModuleFileNameW(NULL, file_name, 512);
    if (!ret) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", GetModuleFileNameW return error, can not Load Library for %1%") % library_name;
        return lib_path;
    }
    int size_needed = ::WideCharToMultiByte(0, 0, file_name, wcslen(file_name), nullptr, 0, nullptr, nullptr);
    std::string file_name_string(size_needed, 0);
    ::WideCharToMultiByte(0, 0, file_name, wcslen(file_name), file_name_string.data(), size_needed, nullptr, nullptr);

    std::size_t found = file_name_string.find("orca-slicer.exe");
    if (found == (file_name_string.size() - 16)) {
        lib_path = library_name + ".dll";
        lib_path = file_name_string.replace(found, 16, lib_path);
    }
#else
#endif
    return lib_path;
}


int NetworkAgent::initialize_network_module(bool using_backup)
{
    //int ret = -1;
    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

    if (using_backup) {
        plugin_folder = plugin_folder/"backup";
    }

    //first load the library
#if defined(_MSC_VER) || defined(_WIN32)
    library = plugin_folder.string() + "\\" + std::string(BAMBU_NETWORK_LIBRARY) + ".dll";
    wchar_t lib_wstr[128];
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    netwoking_module = LoadLibrary(lib_wstr);
    /*if (!netwoking_module) {
        library = std::string(BAMBU_NETWORK_LIBRARY) + ".dll";
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        netwoking_module = LoadLibrary(lib_wstr);
    }*/
    if (!netwoking_module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", try load library directly from current directory");

        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_NETWORK_LIBRARY));
        if (library_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", can not get path in current directory for %1%") % BAMBU_NETWORK_LIBRARY;
            return -1;
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", current path %1%")%library_path;
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        netwoking_module = LoadLibrary(lib_wstr);
    }
#else
    #if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".dylib";
    #else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".so";
    #endif
    printf("loading network module at %s\n", library.c_str());
    netwoking_module = dlopen( library.c_str(), RTLD_LAZY);
    if (!netwoking_module) {
        /*#if defined(__WXMAC__)
        library = std::string("lib") + BAMBU_NETWORK_LIBRARY + ".dylib";
        #else
        library = std::string("lib") + BAMBU_NETWORK_LIBRARY + ".so";
        #endif*/
        //netwoking_module = dlopen( library.c_str(), RTLD_LAZY);
        char* dll_error = dlerror();
        printf("error, dlerror is %s\n", dll_error);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", error, dlerror is %1%")%dll_error;
    }
    printf("after dlopen, network_module is %p\n", netwoking_module);
#endif

    if (!netwoking_module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", can not Load Library for %1%")%library;
        return -1;
    }
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", successfully loaded library %1%, module %2%")%library %netwoking_module;

    // load file transfer interface
    InitFTModule(netwoking_module);

    //load the functions
    check_debug_consistent_ptr        =  reinterpret_cast<func_check_debug_consistent>(get_network_function("bambu_network_check_debug_consistent"));
    get_version_ptr                   =  reinterpret_cast<func_get_version>(get_network_function("bambu_network_get_version"));
    create_agent_ptr                  =  reinterpret_cast<func_create_agent>(get_network_function("bambu_network_create_agent"));
    destroy_agent_ptr                 =  reinterpret_cast<func_destroy_agent>(get_network_function("bambu_network_destroy_agent"));
    init_log_ptr                      =  reinterpret_cast<func_init_log>(get_network_function("bambu_network_init_log"));
    set_config_dir_ptr                =  reinterpret_cast<func_set_config_dir>(get_network_function("bambu_network_set_config_dir"));
    set_cert_file_ptr                 =  reinterpret_cast<func_set_cert_file>(get_network_function("bambu_network_set_cert_file"));
    set_country_code_ptr              =  reinterpret_cast<func_set_country_code>(get_network_function("bambu_network_set_country_code"));
    start_ptr                         =  reinterpret_cast<func_start>(get_network_function("bambu_network_start"));
    set_on_ssdp_msg_fn_ptr            =  reinterpret_cast<func_set_on_ssdp_msg_fn>(get_network_function("bambu_network_set_on_ssdp_msg_fn"));
    set_on_user_login_fn_ptr          =  reinterpret_cast<func_set_on_user_login_fn>(get_network_function("bambu_network_set_on_user_login_fn"));
    set_on_printer_connected_fn_ptr   =  reinterpret_cast<func_set_on_printer_connected_fn>(get_network_function("bambu_network_set_on_printer_connected_fn"));
    set_on_server_connected_fn_ptr    =  reinterpret_cast<func_set_on_server_connected_fn>(get_network_function("bambu_network_set_on_server_connected_fn"));
    set_on_http_error_fn_ptr          =  reinterpret_cast<func_set_on_http_error_fn>(get_network_function("bambu_network_set_on_http_error_fn"));
    set_get_country_code_fn_ptr       =  reinterpret_cast<func_set_get_country_code_fn>(get_network_function("bambu_network_set_get_country_code_fn"));
    set_on_subscribe_failure_fn_ptr   =  reinterpret_cast<func_set_on_subscribe_failure_fn>(get_network_function("bambu_network_set_on_subscribe_failure_fn"));
    set_on_message_fn_ptr             =  reinterpret_cast<func_set_on_message_fn>(get_network_function("bambu_network_set_on_message_fn"));
    set_on_user_message_fn_ptr        =  reinterpret_cast<func_set_on_user_message_fn>(get_network_function("bambu_network_set_on_user_message_fn"));
    set_on_local_connect_fn_ptr       =  reinterpret_cast<func_set_on_local_connect_fn>(get_network_function("bambu_network_set_on_local_connect_fn"));
    set_on_local_message_fn_ptr       =  reinterpret_cast<func_set_on_local_message_fn>(get_network_function("bambu_network_set_on_local_message_fn"));
    set_queue_on_main_fn_ptr          = reinterpret_cast<func_set_queue_on_main_fn>(get_network_function("bambu_network_set_queue_on_main_fn"));
    connect_server_ptr                =  reinterpret_cast<func_connect_server>(get_network_function("bambu_network_connect_server"));
    is_server_connected_ptr           =  reinterpret_cast<func_is_server_connected>(get_network_function("bambu_network_is_server_connected"));
    refresh_connection_ptr            =  reinterpret_cast<func_refresh_connection>(get_network_function("bambu_network_refresh_connection"));
    start_subscribe_ptr               =  reinterpret_cast<func_start_subscribe>(get_network_function("bambu_network_start_subscribe"));
    stop_subscribe_ptr                =  reinterpret_cast<func_stop_subscribe>(get_network_function("bambu_network_stop_subscribe"));
    add_subscribe_ptr                 =  reinterpret_cast<func_add_subscribe>(get_network_function("bambu_network_add_subscribe"));
    del_subscribe_ptr                 =  reinterpret_cast<func_del_subscribe>(get_network_function("bambu_network_del_subscribe"));
    enable_multi_machine_ptr          =  reinterpret_cast<func_enable_multi_machine>(get_network_function("bambu_network_enable_multi_machine"));
    send_message_ptr                  =  reinterpret_cast<func_send_message>(get_network_function("bambu_network_send_message"));
    connect_printer_ptr               =  reinterpret_cast<func_connect_printer>(get_network_function("bambu_network_connect_printer"));
    disconnect_printer_ptr            =  reinterpret_cast<func_disconnect_printer>(get_network_function("bambu_network_disconnect_printer"));
    send_message_to_printer_ptr       =  reinterpret_cast<func_send_message_to_printer>(get_network_function("bambu_network_send_message_to_printer"));
    check_cert_ptr                    =  reinterpret_cast<func_check_cert>(get_network_function("bambu_network_update_cert"));
    install_device_cert_ptr           =  reinterpret_cast<func_install_device_cert>(get_network_function("bambu_network_install_device_cert"));
    start_discovery_ptr               =  reinterpret_cast<func_start_discovery>(get_network_function("bambu_network_start_discovery"));
    change_user_ptr                   =  reinterpret_cast<func_change_user>(get_network_function("bambu_network_change_user"));
    is_user_login_ptr                 =  reinterpret_cast<func_is_user_login>(get_network_function("bambu_network_is_user_login"));
    user_logout_ptr                   =  reinterpret_cast<func_user_logout>(get_network_function("bambu_network_user_logout"));
    get_user_id_ptr                   =  reinterpret_cast<func_get_user_id>(get_network_function("bambu_network_get_user_id"));
    get_user_name_ptr                 =  reinterpret_cast<func_get_user_name>(get_network_function("bambu_network_get_user_name"));
    get_user_avatar_ptr               =  reinterpret_cast<func_get_user_avatar>(get_network_function("bambu_network_get_user_avatar"));
    get_user_nickanme_ptr             =  reinterpret_cast<func_get_user_nickanme>(get_network_function("bambu_network_get_user_nickanme"));
    build_login_cmd_ptr               =  reinterpret_cast<func_build_login_cmd>(get_network_function("bambu_network_build_login_cmd"));
    build_logout_cmd_ptr              =  reinterpret_cast<func_build_logout_cmd>(get_network_function("bambu_network_build_logout_cmd"));
    build_login_info_ptr              =  reinterpret_cast<func_build_login_info>(get_network_function("bambu_network_build_login_info"));
    ping_bind_ptr                     =  reinterpret_cast<func_ping_bind>(get_network_function("bambu_network_ping_bind"));
    bind_detect_ptr                   =  reinterpret_cast<func_bind_detect>(get_network_function("bambu_network_bind_detect"));
    set_server_callback_ptr           =  reinterpret_cast<func_set_server_callback>(get_network_function("bambu_network_set_server_callback"));
    bind_ptr                          =  reinterpret_cast<func_bind>(get_network_function("bambu_network_bind"));
    unbind_ptr                        =  reinterpret_cast<func_unbind>(get_network_function("bambu_network_unbind"));
    get_bambulab_host_ptr             =  reinterpret_cast<func_get_bambulab_host>(get_network_function("bambu_network_get_bambulab_host"));
    get_user_selected_machine_ptr     =  reinterpret_cast<func_get_user_selected_machine>(get_network_function("bambu_network_get_user_selected_machine"));
    set_user_selected_machine_ptr     =  reinterpret_cast<func_set_user_selected_machine>(get_network_function("bambu_network_set_user_selected_machine"));
    start_print_ptr                   =  reinterpret_cast<func_start_print>(get_network_function("bambu_network_start_print"));
    start_local_print_with_record_ptr =  reinterpret_cast<func_start_local_print_with_record>(get_network_function("bambu_network_start_local_print_with_record"));
    start_send_gcode_to_sdcard_ptr    =  reinterpret_cast<func_start_send_gcode_to_sdcard>(get_network_function("bambu_network_start_send_gcode_to_sdcard"));
    start_local_print_ptr             =  reinterpret_cast<func_start_local_print>(get_network_function("bambu_network_start_local_print"));
    start_sdcard_print_ptr            =  reinterpret_cast<func_start_sdcard_print>(get_network_function("bambu_network_start_sdcard_print"));
    get_user_presets_ptr              =  reinterpret_cast<func_get_user_presets>(get_network_function("bambu_network_get_user_presets"));
    request_setting_id_ptr            =  reinterpret_cast<func_request_setting_id>(get_network_function("bambu_network_request_setting_id"));
    put_setting_ptr                   =  reinterpret_cast<func_put_setting>(get_network_function("bambu_network_put_setting"));
    get_setting_list_ptr              = reinterpret_cast<func_get_setting_list>(get_network_function("bambu_network_get_setting_list"));
    get_setting_list2_ptr             = reinterpret_cast<func_get_setting_list2>(get_network_function("bambu_network_get_setting_list2"));
    delete_setting_ptr                =  reinterpret_cast<func_delete_setting>(get_network_function("bambu_network_delete_setting"));
    get_studio_info_url_ptr           =  reinterpret_cast<func_get_studio_info_url>(get_network_function("bambu_network_get_studio_info_url"));
    set_extra_http_header_ptr         =  reinterpret_cast<func_set_extra_http_header>(get_network_function("bambu_network_set_extra_http_header"));
    get_my_message_ptr                =  reinterpret_cast<func_get_my_message>(get_network_function("bambu_network_get_my_message"));
    check_user_task_report_ptr        =  reinterpret_cast<func_check_user_task_report>(get_network_function("bambu_network_check_user_task_report"));
    get_user_print_info_ptr           =  reinterpret_cast<func_get_user_print_info>(get_network_function("bambu_network_get_user_print_info"));
    get_user_tasks_ptr                =  reinterpret_cast<func_get_user_tasks>(get_network_function("bambu_network_get_user_tasks"));
    get_printer_firmware_ptr          =  reinterpret_cast<func_get_printer_firmware>(get_network_function("bambu_network_get_printer_firmware"));
    get_task_plate_index_ptr          =  reinterpret_cast<func_get_task_plate_index>(get_network_function("bambu_network_get_task_plate_index"));
    get_user_info_ptr                 =  reinterpret_cast<func_get_user_info>(get_network_function("bambu_network_get_user_info"));
    request_bind_ticket_ptr           =  reinterpret_cast<func_request_bind_ticket>(get_network_function("bambu_network_request_bind_ticket"));
    get_subtask_info_ptr              =  reinterpret_cast<func_get_subtask_info>(get_network_function("bambu_network_get_subtask_info"));
    get_slice_info_ptr                =  reinterpret_cast<func_get_slice_info>(get_network_function("bambu_network_get_slice_info"));
    query_bind_status_ptr             =  reinterpret_cast<func_query_bind_status>(get_network_function("bambu_network_query_bind_status"));
    modify_printer_name_ptr           =  reinterpret_cast<func_modify_printer_name>(get_network_function("bambu_network_modify_printer_name"));
    get_camera_url_ptr                =  reinterpret_cast<func_get_camera_url>(get_network_function("bambu_network_get_camera_url"));
    get_design_staffpick_ptr          =  reinterpret_cast<func_get_design_staffpick>(get_network_function("bambu_network_get_design_staffpick"));
    start_publish_ptr                 =  reinterpret_cast<func_start_pubilsh>(get_network_function("bambu_network_start_publish"));
    get_model_publish_url_ptr         =  reinterpret_cast<func_get_model_publish_url>(get_network_function("bambu_network_get_model_publish_url"));
    get_subtask_ptr                   =  reinterpret_cast<func_get_subtask>(get_network_function("bambu_network_get_subtask"));
    get_model_mall_home_url_ptr       =  reinterpret_cast<func_get_model_mall_home_url>(get_network_function("bambu_network_get_model_mall_home_url"));
    get_model_mall_detail_url_ptr     =  reinterpret_cast<func_get_model_mall_detail_url>(get_network_function("bambu_network_get_model_mall_detail_url"));
    get_my_profile_ptr                =  reinterpret_cast<func_get_my_profile>(get_network_function("bambu_network_get_my_profile"));
    track_enable_ptr                  =  reinterpret_cast<func_track_enable>(get_network_function("bambu_network_track_enable"));
    track_remove_files_ptr            =  reinterpret_cast<func_track_remove_files>(get_network_function("bambu_network_track_remove_files"));
    track_event_ptr                   =  reinterpret_cast<func_track_event>(get_network_function("bambu_network_track_event"));
    track_header_ptr                  =  reinterpret_cast<func_track_header>(get_network_function("bambu_network_track_header"));
    track_update_property_ptr         = reinterpret_cast<func_track_update_property>(get_network_function("bambu_network_track_update_property"));
    track_get_property_ptr            = reinterpret_cast<func_track_get_property>(get_network_function("bambu_network_track_get_property"));
    put_model_mall_rating_url_ptr     = reinterpret_cast<func_put_model_mall_rating_url>(get_network_function("bambu_network_put_model_mall_rating"));
    get_oss_config_ptr                = reinterpret_cast<func_get_oss_config>(get_network_function("bambu_network_get_oss_config"));
    put_rating_picture_oss_ptr        = reinterpret_cast<func_put_rating_picture_oss>(get_network_function("bambu_network_put_rating_picture_oss"));
    get_model_mall_rating_result_ptr  = reinterpret_cast<func_get_model_mall_rating_result>(get_network_function("bambu_network_get_model_mall_rating"));

    get_mw_user_preference_ptr = reinterpret_cast<func_get_mw_user_preference>(get_network_function("bambu_network_get_mw_user_preference"));
    get_mw_user_4ulist_ptr     = reinterpret_cast<func_get_mw_user_4ulist>(get_network_function("bambu_network_get_mw_user_4ulist"));

    return 0;
}

int NetworkAgent::unload_network_module()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", network module %1%")%netwoking_module;
    UnloadFTModule();
#if defined(_MSC_VER) || defined(_WIN32)
    if (netwoking_module) {
        FreeLibrary(netwoking_module);
        netwoking_module = NULL;
    }
    if (source_module) {
        FreeLibrary(source_module);
        source_module = NULL;
    }
#else
    if (netwoking_module) {
        dlclose(netwoking_module);
        netwoking_module = NULL;
    }
    if (source_module) {
        dlclose(source_module);
        source_module = NULL;
    }
#endif

    check_debug_consistent_ptr        =  nullptr;
    get_version_ptr                   =  nullptr;
    create_agent_ptr                  =  nullptr;
    destroy_agent_ptr                 =  nullptr;
    init_log_ptr                      =  nullptr;
    set_config_dir_ptr                =  nullptr;
    set_cert_file_ptr                 =  nullptr;
    set_country_code_ptr              =  nullptr;
    start_ptr                         =  nullptr;
    set_on_ssdp_msg_fn_ptr            =  nullptr;
    set_on_user_login_fn_ptr          =  nullptr;
    set_on_printer_connected_fn_ptr   =  nullptr;
    set_on_server_connected_fn_ptr    =  nullptr;
    set_on_http_error_fn_ptr          =  nullptr;
    set_get_country_code_fn_ptr       =  nullptr;
    set_on_subscribe_failure_fn_ptr   =  nullptr;
    set_on_message_fn_ptr             =  nullptr;
    set_on_user_message_fn_ptr        =  nullptr;
    set_on_local_connect_fn_ptr       =  nullptr;
    set_on_local_message_fn_ptr       =  nullptr;
    set_queue_on_main_fn_ptr          = nullptr;
    connect_server_ptr                =  nullptr;
    is_server_connected_ptr           =  nullptr;
    refresh_connection_ptr            =  nullptr;
    start_subscribe_ptr               =  nullptr;
    stop_subscribe_ptr                =  nullptr;
    send_message_ptr                  =  nullptr;
    connect_printer_ptr               =  nullptr;
    disconnect_printer_ptr            =  nullptr;
    send_message_to_printer_ptr       =  nullptr;
    check_cert_ptr                    =  nullptr;
    start_discovery_ptr               =  nullptr;
    change_user_ptr                   =  nullptr;
    is_user_login_ptr                 =  nullptr;
    user_logout_ptr                   =  nullptr;
    get_user_id_ptr                   =  nullptr;
    get_user_name_ptr                 =  nullptr;
    get_user_avatar_ptr               =  nullptr;
    get_user_nickanme_ptr             =  nullptr;
    build_login_cmd_ptr               =  nullptr;
    build_logout_cmd_ptr              =  nullptr;
    build_login_info_ptr              =  nullptr;
    ping_bind_ptr                     =  nullptr;
    bind_ptr                          =  nullptr;
    unbind_ptr                        =  nullptr;
    get_bambulab_host_ptr             =  nullptr;
    get_user_selected_machine_ptr     =  nullptr;
    set_user_selected_machine_ptr     =  nullptr;
    start_print_ptr                   =  nullptr;
    start_local_print_with_record_ptr =  nullptr;
    start_send_gcode_to_sdcard_ptr    =  nullptr;
    start_local_print_ptr             =  nullptr;
    start_sdcard_print_ptr             =  nullptr;
    get_user_presets_ptr              =  nullptr;
    request_setting_id_ptr            =  nullptr;
    put_setting_ptr                   =  nullptr;
    get_setting_list_ptr              =  nullptr;
    get_setting_list2_ptr             =  nullptr;
    delete_setting_ptr                =  nullptr;
    get_studio_info_url_ptr           =  nullptr;
    set_extra_http_header_ptr         =  nullptr;
    get_my_message_ptr                =  nullptr;
    check_user_task_report_ptr        =  nullptr;
    get_user_print_info_ptr           =  nullptr;
    get_user_tasks_ptr                =  nullptr;
    get_printer_firmware_ptr          =  nullptr;
    get_task_plate_index_ptr          =  nullptr;
    get_user_info_ptr                 =  nullptr;
    get_subtask_info_ptr              =  nullptr;
    get_slice_info_ptr                =  nullptr;
    query_bind_status_ptr             =  nullptr;
    modify_printer_name_ptr           =  nullptr;
    get_camera_url_ptr                =  nullptr;
    get_design_staffpick_ptr          =  nullptr;
    start_publish_ptr                 =  nullptr;
    get_model_publish_url_ptr         =  nullptr;
    get_subtask_ptr                   =  nullptr;
    get_model_mall_home_url_ptr       =  nullptr;
    get_model_mall_detail_url_ptr     =  nullptr;
    get_my_profile_ptr                =  nullptr;
    track_enable_ptr                  =  nullptr;
    track_remove_files_ptr            =  nullptr;
    track_event_ptr                   =  nullptr;
    track_header_ptr                  =  nullptr;
    track_update_property_ptr         =  nullptr;
    track_get_property_ptr            =  nullptr;
    get_oss_config_ptr                =  nullptr;
    put_rating_picture_oss_ptr        =  nullptr;
    put_model_mall_rating_url_ptr     =  nullptr;
    get_model_mall_rating_result_ptr  = nullptr;

    get_mw_user_preference_ptr        = nullptr;
    get_mw_user_4ulist_ptr            = nullptr;

    return 0;
}

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE NetworkAgent::get_bambu_source_entry()
#else
void* NetworkAgent::get_bambu_source_entry()
#endif
{
    if ((source_module) || (!netwoking_module))
        return source_module;

    //int ret = -1;
    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";
#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t lib_wstr[128];

    //goto load bambu source
    library = plugin_folder.string() + "/" + std::string(BAMBU_SOURCE_LIBRARY) + ".dll";
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    source_module = LoadLibrary(lib_wstr);
    if (!source_module) {
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", try load BambuSource directly from current directory");
        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_SOURCE_LIBRARY));
        if (library_path.empty()) {
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", can not get path in current directory for %1%") % BAMBU_SOURCE_LIBRARY;
            return source_module;
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(", current path %1%")%library_path;
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        source_module = LoadLibrary(lib_wstr);
    }
#else
#if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".dylib";
#else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".so";
#endif
    source_module = dlopen( library.c_str(), RTLD_LAZY);
    /*if (!source_module) {
#if defined(__WXMAC__)
        library = std::string("lib") + BAMBU_SOURCE_LIBRARY + ".dylib";
#else
        library = std::string("lib") + BAMBU_SOURCE_LIBRARY + ".so";
#endif
        source_module = dlopen( library.c_str(), RTLD_LAZY);
    }*/
#endif

    return source_module;
}

void* NetworkAgent::get_network_function(const char* name)
{
    void* function = nullptr;

    if (!netwoking_module)
        return function;

#if defined(_MSC_VER) || defined(_WIN32)
    function = GetProcAddress(netwoking_module, name);
#else
    function = dlsym(netwoking_module, name);
#endif

    if (!function) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", can not find function %1%")%name;
    }
    return function;
}

std::string NetworkAgent::get_version()
{
    bool consistent = true;
    //check the debug consistent first
    if (check_debug_consistent_ptr) {
#if defined(NDEBUG)
        consistent = check_debug_consistent_ptr(false);
#else
        consistent = check_debug_consistent_ptr(true);
#endif
    }
    if (!consistent) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", inconsistent library,return 00.00.00.00!");
        return "00.00.00.00";
    }
    if (get_version_ptr) {
        return get_version_ptr();
    }
    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", get_version not supported,return 00.00.00.00!");
    return "00.00.00.00";
}

int NetworkAgent::init_log()
{
    int ret = 0;
    if (network_agent && init_log_ptr) {
        ret = init_log_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_config_dir(std::string config_dir)
{
    int ret = 0;
    if (network_agent && set_config_dir_ptr) {
        ret = set_config_dir_ptr(network_agent, config_dir);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, config_dir=%3%")%network_agent %ret %config_dir ;
    }
    return ret;
}

int NetworkAgent::set_cert_file(std::string folder, std::string filename)
{
    int ret = 0;
    if (network_agent && set_cert_file_ptr) {
        ret = set_cert_file_ptr(network_agent, folder, filename);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, folder=%3%, filename=%4%")%network_agent %ret %folder %filename;
    }
    return ret;
}

int NetworkAgent::set_country_code(std::string country_code)
{
    int ret = 0;
    if (network_agent && set_country_code_ptr) {
        ret = set_country_code_ptr(network_agent, country_code);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, country_code=%3%")%network_agent %ret %country_code ;
    }
    return ret;
}

int NetworkAgent::start()
{
    int ret = 0;
    if (network_agent && start_ptr) {
        ret = start_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_ssdp_msg_fn_ptr) {
        ret = set_on_ssdp_msg_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_user_login_fn(OnUserLoginFn fn)
{
    int ret = 0;
    if (network_agent && set_on_user_login_fn_ptr) {
        ret = set_on_user_login_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_printer_connected_fn_ptr) {
        ret = set_on_printer_connected_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_server_connected_fn(OnServerConnectedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_server_connected_fn_ptr) {
        ret = set_on_server_connected_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_http_error_fn(OnHttpErrorFn fn)
{
    int ret = 0;
    if (network_agent && set_on_http_error_fn_ptr) {
        ret = set_on_http_error_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_get_country_code_fn(GetCountryCodeFn fn)
{
    int ret = 0;
    if (network_agent && set_get_country_code_fn_ptr) {
        ret = set_get_country_code_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn fn)
{
    int ret = 0;
    if (network_agent && set_on_subscribe_failure_fn_ptr) {
        ret = set_on_subscribe_failure_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::set_on_message_fn(OnMessageFn fn)
{
    int ret = 0;
    if (network_agent && set_on_message_fn_ptr) {
        ret = set_on_message_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_user_message_fn(OnMessageFn fn)
{
    int ret = 0;
    if (network_agent && set_on_user_message_fn_ptr) {
        ret = set_on_user_message_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    int ret = 0;
    if (network_agent && set_on_local_connect_fn_ptr) {
        ret = set_on_local_connect_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_on_local_message_fn(OnMessageFn fn)
{
    int ret = 0;
    if (network_agent && set_on_local_message_fn_ptr) {
        ret = set_on_local_message_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    int ret = 0;
    if (network_agent && set_queue_on_main_fn_ptr) {
        ret = set_queue_on_main_fn_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::connect_server()
{
    int ret = 0;
    if (network_agent && connect_server_ptr) {
        ret = connect_server_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

bool NetworkAgent::is_server_connected()
{
    bool ret = false;
    if (network_agent && is_server_connected_ptr) {
        ret = is_server_connected_ptr(network_agent);
        //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::refresh_connection()
{
    int ret = 0;
    if (network_agent && refresh_connection_ptr) {
        ret = refresh_connection_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::start_subscribe(std::string module)
{
    int ret = 0;
    if (network_agent && start_subscribe_ptr) {
        ret = start_subscribe_ptr(network_agent, module);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, module=%3%")%network_agent %ret %module ;
    }
    return ret;
}

int NetworkAgent::stop_subscribe(std::string module)
{
    int ret = 0;
    if (network_agent && stop_subscribe_ptr) {
        ret = stop_subscribe_ptr(network_agent, module);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, module=%3%")%network_agent %ret %module ;
    }
    return ret;
}

int NetworkAgent::add_subscribe(std::vector<std::string> dev_list)
{
    int ret = 0;
    if (network_agent && add_subscribe_ptr) {
        ret = add_subscribe_ptr(network_agent, dev_list);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::del_subscribe(std::vector<std::string> dev_list)
{
    int ret = 0;
    if (network_agent && del_subscribe_ptr) {
        ret = del_subscribe_ptr(network_agent, dev_list);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

void NetworkAgent::enable_multi_machine(bool enable)
{
    if (network_agent && enable_multi_machine_ptr) {
        enable_multi_machine_ptr(network_agent, enable);
    }
}

int NetworkAgent::send_message(std::string dev_id, std::string json_str, int qos, int flag)
{
    int ret = 0;
    if (network_agent && send_message_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_send_message_legacy>(send_message_ptr))(network_agent, dev_id, json_str, qos);
        } else {
            ret = send_message_ptr(network_agent, dev_id, json_str, qos, flag);
        }
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%, json_str=%4%, qos=%5%")%network_agent %ret %dev_id %json_str %qos;
    }
    return ret;
}

int NetworkAgent::connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    int ret = 0;
    if (network_agent && connect_printer_ptr) {
        ret = connect_printer_ptr(network_agent, dev_id, dev_ip, username, password, use_ssl);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << (boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%, dev_ip=%4%, username=%5%, password=%6%")
                % network_agent % ret % dev_id % dev_ip % username % password).str();
    }
    return ret;
}

int NetworkAgent::disconnect_printer()
{
    int ret = 0;
    if (network_agent && disconnect_printer_ptr) {
        ret = disconnect_printer_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag)
{
    int ret = 0;
    if (network_agent && send_message_to_printer_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_send_message_to_printer_legacy>(send_message_to_printer_ptr))(network_agent, dev_id, json_str, qos);
        } else {
            ret = send_message_to_printer_ptr(network_agent, dev_id, json_str, qos, flag);
        }
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%, json_str=%4%, qos=%5%")
                %network_agent %ret %dev_id %json_str %qos;
    }
    return ret;
}

int NetworkAgent::check_cert()
{
    int ret = 0;
    if (network_agent && check_cert_ptr) {
        ret = check_cert_ptr(network_agent);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

void NetworkAgent::install_device_cert(std::string dev_id, bool lan_only)
{
    if (network_agent && install_device_cert_ptr) {
        install_device_cert_ptr(network_agent, dev_id, lan_only);
    }
}

bool NetworkAgent::start_discovery(bool start, bool sending)
{
    bool ret = false;
    if (network_agent && start_discovery_ptr) {
        ret = start_discovery_ptr(network_agent, start, sending);
        //BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, start=%3%, sending=%4%")%network_agent %ret %start %sending;
    }
    return ret;
}

int  NetworkAgent::change_user(std::string user_info)
{
    int ret = 0;
    if (network_agent && change_user_ptr) {
        ret = change_user_ptr(network_agent, user_info);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, user_info=%3%")%network_agent %ret %user_info ;
    }
    return ret;
}

bool NetworkAgent::is_user_login()
{
    bool ret = false;
    if (network_agent && is_user_login_ptr) {
        ret = is_user_login_ptr(network_agent);
    }
    return ret;
}

int  NetworkAgent::user_logout(bool request)
{
    int ret = 0;
    if (network_agent && user_logout_ptr) {
        ret = user_logout_ptr(network_agent, request);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

std::string NetworkAgent::get_user_id()
{
    std::string ret;
    if (network_agent && get_user_id_ptr) {
        ret = get_user_id_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_name()
{
    std::string ret;
    if (network_agent && get_user_name_ptr) {
        ret = get_user_name_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_avatar()
{
    std::string ret;
    if (network_agent && get_user_avatar_ptr) {
        ret = get_user_avatar_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_nickanme()
{
    std::string ret;
    if (network_agent && get_user_nickanme_ptr) {
        ret = get_user_nickanme_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::build_login_cmd()
{
    std::string ret;
    if (network_agent && build_login_cmd_ptr) {
        ret = build_login_cmd_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::build_logout_cmd()
{
    std::string ret;
    if (network_agent && build_logout_cmd_ptr) {
        ret = build_logout_cmd_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::build_login_info()
{
    std::string ret;
    if (network_agent && build_login_info_ptr) {
        ret = build_login_info_ptr(network_agent);
    }
    return ret;
}

int NetworkAgent::ping_bind(std::string ping_code)
{
    int ret = 0;
    if (network_agent && ping_bind_ptr) {
        ret = ping_bind_ptr(network_agent, ping_code);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, pin code=%3%")
            % network_agent % ret % ping_code;
    }
    return ret;
}

int NetworkAgent::bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect)
{
    int ret = 0;
    if (network_agent && bind_detect_ptr) {
        ret = bind_detect_ptr(network_agent, dev_ip, sec_link, detect);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_ip=%3%")
            % network_agent % ret % dev_ip;
    }
    return ret;
}

int NetworkAgent::set_server_callback(OnServerErrFn fn)
{
    int ret = 0;
    if (network_agent && set_server_callback_ptr) {
        ret = set_server_callback_ptr(network_agent, fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")
            % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::bind(std::string dev_ip, std::string dev_id, std::string sec_link, std::string timezone,  bool improved, OnUpdateStatusFn update_fn)
{
    int ret = 0;
    if (network_agent && bind_ptr) {
        ret = bind_ptr(network_agent, dev_ip, dev_id, sec_link, timezone, improved, update_fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_ip=%3%, timezone=%4%")
                %network_agent %ret %dev_ip %timezone;
    }
    return ret;
}

int NetworkAgent::unbind(std::string dev_id)
{
    int ret = 0;
    if (network_agent && unbind_ptr) {
        ret = unbind_ptr(network_agent, dev_id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, user_info=%3%")%network_agent %ret %dev_id ;
    }
    return ret;
}

std::string NetworkAgent::get_bambulab_host()
{
    std::string ret;
    if (network_agent && get_bambulab_host_ptr) {
        ret = get_bambulab_host_ptr(network_agent);
    }
    return ret;
}

std::string NetworkAgent::get_user_selected_machine()
{
    std::string ret;
    if (network_agent && get_user_selected_machine_ptr) {
        ret = get_user_selected_machine_ptr(network_agent);
    }
    return ret;
}

int NetworkAgent::set_user_selected_machine(std::string dev_id)
{
    int ret = 0;
    if (network_agent && set_user_selected_machine_ptr) {
        ret = set_user_selected_machine_ptr(network_agent, dev_id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, user_info=%3%")%network_agent %ret %dev_id ;
    }
    return ret;
}

int NetworkAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    int ret = 0;
    if (network_agent && start_print_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_start_print_legacy>(start_print_ptr))(network_agent, as_legacy(params), update_fn, cancel_fn, wait_fn);
        } else {
            ret = start_print_ptr(network_agent, params, update_fn, cancel_fn, wait_fn);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%")
                %network_agent %ret %params.dev_id %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    int ret = 0;
    if (network_agent && start_local_print_with_record_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_start_local_print_with_record_legacy>(start_local_print_with_record_ptr))(network_agent, as_legacy(params), update_fn, cancel_fn, wait_fn);
        } else {
            ret = start_local_print_with_record_ptr(network_agent, params, update_fn, cancel_fn, wait_fn);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%")
                %network_agent %ret %params.dev_id %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn)
{
    int ret = 0;
    if (network_agent && start_send_gcode_to_sdcard_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_start_send_gcode_to_sdcard_legacy>(start_send_gcode_to_sdcard_ptr))(network_agent, as_legacy(params), update_fn, cancel_fn, wait_fn);
        } else {
            ret = start_send_gcode_to_sdcard_ptr(network_agent, params, update_fn, cancel_fn, wait_fn);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%")
            % network_agent % ret % params.dev_id % params.task_name % params.project_name;
    }
    return ret;
}

int NetworkAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && start_local_print_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_start_local_print_legacy>(start_local_print_ptr))(network_agent, as_legacy(params), update_fn, cancel_fn);
        } else {
            ret = start_local_print_ptr(network_agent, params, update_fn, cancel_fn);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%")
                %network_agent %ret %params.dev_id %params.task_name %params.project_name;
    }
    return ret;
}

int NetworkAgent::start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && start_sdcard_print_ptr) {
        if (use_legacy_network) {
            ret = (reinterpret_cast<func_start_sdcard_print_legacy>(start_sdcard_print_ptr))(network_agent, as_legacy(params), update_fn, cancel_fn);
        } else {
            ret = start_sdcard_print_ptr(network_agent, params, update_fn, cancel_fn);
        }
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, task_name=%4%, project_name=%5%")
            % network_agent % ret % params.dev_id % params.task_name % params.project_name;
    }
    return ret;
}

int NetworkAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    int ret = 0;
    if (network_agent && get_user_presets_ptr) {
        ret = get_user_presets_ptr(network_agent, user_presets);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, setting_id count=%3%")%network_agent %ret %user_presets->size() ;
    }
    return ret;
}

std::string NetworkAgent::request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    std::string ret;
    if (network_agent && request_setting_id_ptr) {
        ret = request_setting_id_ptr(network_agent, name, values_map, http_code);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, name=%2%, http_code=%3%, ret.setting_id=%4%")
                %network_agent %name %(*http_code) %ret;
    }
    return ret;
}

int NetworkAgent::put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code)
{
    int ret = 0;
    if (network_agent && put_setting_ptr) {
        ret = put_setting_ptr(network_agent, setting_id, name, values_map, http_code);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, setting_id=%2%, name=%3%, http_code=%4%, ret=%5%")
                %network_agent %setting_id %name %(*http_code) %ret;
    }
    return ret;
}

int NetworkAgent::get_setting_list(std::string bundle_version, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && get_setting_list_ptr) {
        ret = get_setting_list_ptr(network_agent, bundle_version, pro_fn, cancel_fn);
        if (ret) BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, bundle_version=%3%") % network_agent % ret % bundle_version;
    }
    return ret;
}

int NetworkAgent::get_setting_list2(std::string bundle_version, CheckFn chk_fn, ProgressFn pro_fn, WasCancelledFn cancel_fn)
{
    int ret = 0;
    if (network_agent && get_setting_list2_ptr) {
        ret = get_setting_list2_ptr(network_agent, bundle_version, chk_fn, pro_fn, cancel_fn);
        if (ret) BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, bundle_version=%3%") % network_agent % ret % bundle_version;
    } else {
        ret = get_setting_list(bundle_version, pro_fn, cancel_fn);
    }
    return ret;
}

int NetworkAgent::delete_setting(std::string setting_id)
{
    int ret = 0;
    if (network_agent && delete_setting_ptr) {
        ret = delete_setting_ptr(network_agent, setting_id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, setting_id=%3%")%network_agent %ret %setting_id ;
    }
    return ret;
}

std::string NetworkAgent::get_studio_info_url()
{
    std::string ret;
    if (network_agent && get_studio_info_url_ptr) {
        ret = get_studio_info_url_ptr(network_agent);
    }
    return ret;
}

int NetworkAgent::set_extra_http_header(std::map<std::string, std::string> extra_headers)
{
    int ret = 0;
    if (network_agent && set_extra_http_header_ptr) {
        ret = set_extra_http_header_ptr(network_agent, extra_headers);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, extra_headers count=%3%")%network_agent %ret %extra_headers.size() ;
    }
    return ret;
}

int NetworkAgent::get_my_message(int type, int after, int limit, unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_my_message_ptr) {
        ret = get_my_message_ptr(network_agent, type, after, limit, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::check_user_task_report(int* task_id, bool* printable)
{
    int ret = 0;
    if (network_agent && check_user_task_report_ptr) {
        ret = check_user_task_report_ptr(network_agent, task_id, printable);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, task_id=%3%, printable=%4%")%network_agent %ret %(*task_id) %(*printable);
    }
    return ret;
}

int NetworkAgent::get_user_print_info(unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_user_print_info_ptr) {
        ret = get_user_print_info_ptr(network_agent, http_code, http_body);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, http_code=%3%, http_body=%4%")%network_agent %ret %(*http_code) %(*http_body);
    }
    return ret;
}

int NetworkAgent::get_user_tasks(TaskQueryParams params, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_user_tasks_ptr) {
        ret = get_user_tasks_ptr(network_agent, params, http_body);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, http_body=%3%") % network_agent % ret % (*http_body);
    }
    return ret;
}

int NetworkAgent::get_printer_firmware(std::string dev_id, unsigned* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_printer_firmware_ptr) {
        ret = get_printer_firmware_ptr(network_agent, dev_id, http_code, http_body);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, http_code=%4%, http_body=%5%")
                %network_agent %ret %dev_id %(*http_code) %(*http_body);
    }
    return ret;
}

int NetworkAgent::get_task_plate_index(std::string task_id, int* plate_index)
{
    int ret = 0;
    if (network_agent && get_task_plate_index_ptr) {
        ret = get_task_plate_index_ptr(network_agent, task_id, plate_index);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, task_id=%3%")%network_agent %ret %task_id;
    }
    return ret;
}

int NetworkAgent::get_user_info(int* identifier)
{
    int ret = 0;
    if (network_agent && get_user_info_ptr) {
        ret = get_user_info_ptr(network_agent, identifier);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::request_bind_ticket(std::string* ticket)
{
    int ret = 0;
    if (network_agent && request_bind_ticket_ptr) {
        ret = request_bind_ticket_ptr(network_agent, ticket);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_subtask_info(std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && get_subtask_info_ptr) {
        ret = get_subtask_info_ptr(network_agent, subtask_id, task_json, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_slice_info(std::string project_id, std::string profile_id, int plate_index, std::string* slice_json)
{
    int ret = 0;
    if (network_agent && get_slice_info_ptr) {
        ret = get_slice_info_ptr(network_agent, project_id, profile_id, plate_index, slice_json);
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << boost::format(" : network_agent=%1%, project_id=%2%, profile_id=%3%, plate_index=%4%, slice_json=%5%")
                %network_agent %project_id %profile_id %plate_index %(*slice_json);
    }
    return ret;
}

int NetworkAgent::query_bind_status(std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body)
{
    int ret = 0;
    if (network_agent && query_bind_status_ptr) {
        ret = query_bind_status_ptr(network_agent, query_list, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, http_code=%3%, http_body=%4%")
                %network_agent %ret%(*http_code) %(*http_body);
    }
    return ret;
}

int NetworkAgent::modify_printer_name(std::string dev_id, std::string dev_name)
{
    int ret = 0;
    if (network_agent && modify_printer_name_ptr) {
        ret = modify_printer_name_ptr(network_agent, dev_id, dev_name);
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << boost::format(" : network_agent=%1%, ret=%2%, dev_id=%3%, dev_name=%4%")%network_agent %ret %dev_id %dev_name;
    }
    return ret;
}

int NetworkAgent::get_camera_url(std::string dev_id, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_camera_url_ptr) {
        ret = get_camera_url_ptr(network_agent, dev_id, callback);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%, dev_id=%3%")%network_agent %ret %dev_id;
    }
    return ret;
}

int NetworkAgent::get_design_staffpick(int offset, int limit, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_design_staffpick_ptr) {
        ret = get_design_staffpick_ptr(network_agent, offset, limit, callback);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%")%network_agent %ret;
    }
    return ret;
}

int NetworkAgent::get_mw_user_preference(std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_mw_user_preference_ptr) {
        ret = get_mw_user_preference_ptr(network_agent,callback);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}


int NetworkAgent::get_mw_user_4ulist(int seed, int limit, std::function<void(std::string)> callback)
{
    int ret = 0;
    if (network_agent && get_mw_user_4ulist_ptr) {
        ret = get_mw_user_4ulist_ptr(network_agent,seed, limit, callback);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::start_publish(PublishParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, std::string *out)
{
    int ret = 0;
    if (network_agent && start_publish_ptr) {
        ret = start_publish_ptr(network_agent, params, update_fn, cancel_fn, out);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_model_publish_url(std::string* url)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = get_model_publish_url_ptr(network_agent, url);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_subtask(BBLModelTask* task, OnGetSubTaskFn getsub_fn)
{
    int ret = 0;
    if (network_agent && get_subtask_ptr) {
        ret = get_subtask_ptr(network_agent, task, getsub_fn);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }

    return ret;
}

int NetworkAgent::get_model_mall_home_url(std::string* url)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = get_model_mall_home_url_ptr(network_agent, url);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_model_mall_detail_url(std::string* url, std::string id)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = get_model_mall_detail_url_ptr(network_agent, url, id);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_my_profile(std::string token, unsigned int *http_code, std::string *http_body)
{
    int ret = 0;
    if (network_agent && get_my_profile_ptr) {
        ret = get_my_profile_ptr(network_agent, token, http_code, http_body);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_enable(bool enable)
{
    enable_track = false;
    int ret = 0;
    if (network_agent && track_enable_ptr) {
        ret = track_enable_ptr(network_agent, enable);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_remove_files()
{
    int ret = 0;
    if (network_agent && track_remove_files_ptr) {
        ret = track_remove_files_ptr(network_agent);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_event(std::string evt_key, std::string content)
{
    return 0;
    if (!this->enable_track)
        return 0;

    int ret = 0;
    if (network_agent && track_event_ptr) {
        ret = track_event_ptr(network_agent, evt_key, content);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_header(std::string header)
{
    if (!this->enable_track)
        return 0;
    int ret = 0;
    if (network_agent && track_header_ptr) {
        ret = track_header_ptr(network_agent, header);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_update_property(std::string name, std::string value, std::string type)
{
    if (!this->enable_track)
        return 0;

    int ret = 0;
    if (network_agent && track_update_property_ptr) {
        ret = track_update_property_ptr(network_agent, name, value, type);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::track_get_property(std::string name, std::string& value, std::string type)
{
    if (!this->enable_track)
        return 0;

    int ret = 0;
    if (network_agent && track_get_property_ptr) {
        ret = track_get_property_ptr(network_agent, name, value, type);
        if (ret)
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format("error network_agnet=%1%, ret = %2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::put_model_mall_rating(int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && get_model_publish_url_ptr) {
        ret = put_model_mall_rating_url_ptr(network_agent, rating_id, score, content, images, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_oss_config(std::string &config, std::string country_code, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && get_oss_config_ptr) {
        ret = get_oss_config_ptr(network_agent, config, country_code, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::put_rating_picture_oss(std::string &config, std::string &pic_oss_path, std::string model_id, int profile_id, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && put_rating_picture_oss_ptr) {
        ret = put_rating_picture_oss_ptr(network_agent, config, pic_oss_path, model_id, profile_id, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

int NetworkAgent::get_model_mall_rating_result(int job_id, std::string &rating_result, unsigned int &http_code, std::string &http_error)
{
    int ret = 0;
    if (network_agent && get_model_mall_rating_result_ptr) {
        ret = get_model_mall_rating_result_ptr(network_agent, job_id, rating_result, http_code, http_error);
        if (ret) BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << boost::format(" error: network_agent=%1%, ret=%2%") % network_agent % ret;
    }
    return ret;
}

} //namespace
