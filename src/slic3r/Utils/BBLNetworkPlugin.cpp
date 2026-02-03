#include "BBLNetworkPlugin.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <boost/log/trivial.hpp>
#include <boost/format.hpp>
#include <boost/filesystem.hpp>
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/FileTransferUtils.hpp"

#if !defined(_MSC_VER) && !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace Slic3r {

#define BAMBU_SOURCE_LIBRARY "BambuSource"

// ============================================================================
// Singleton Implementation
// ============================================================================

// Static pointer initialization (null by default, created on first access)
BBLNetworkPlugin* BBLNetworkPlugin::s_instance = nullptr;

BBLNetworkPlugin& BBLNetworkPlugin::instance()
{
    static std::once_flag flag;
    std::call_once(flag, [] {
        s_instance = new BBLNetworkPlugin();
    });
    return *s_instance;
}

void BBLNetworkPlugin::shutdown()
{
    // Note: Do not call instance() after shutdown() - the singleton is destroyed.
    if (s_instance) {
        delete s_instance;
        s_instance = nullptr;
    }
}

BBLNetworkPlugin::BBLNetworkPlugin() = default;

BBLNetworkPlugin::~BBLNetworkPlugin()
{
    destroy_agent();
    unload();
}

// ============================================================================
// Module Lifecycle
// ============================================================================

int BBLNetworkPlugin::initialize(bool using_backup, const std::string& version)
{
    clear_load_error();

    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

    if (using_backup) {
        plugin_folder = plugin_folder / "backup";
    }

    if (version.empty()) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": version is required but not provided";
        set_load_error(
            "Network library version not specified",
            "A version must be specified to load the network library",
            ""
        );
        return -1;
    }

    // Auto-migration: If loading legacy version and versioned library doesn't exist,
    // but unversioned legacy library does exist, rename it to versioned format
    if (version == BAMBU_NETWORK_AGENT_VERSION_LEGACY) {
        boost::filesystem::path versioned_path;
        boost::filesystem::path legacy_path;
#if defined(_MSC_VER) || defined(_WIN32)
        versioned_path = plugin_folder / (std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".dll");
        legacy_path = plugin_folder / (std::string(BAMBU_NETWORK_LIBRARY) + ".dll");
#elif defined(__WXMAC__)
        versioned_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".dylib");
        legacy_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".dylib");
#else
        versioned_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".so");
        legacy_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".so");
#endif
        if (!boost::filesystem::exists(versioned_path) && boost::filesystem::exists(legacy_path)) {
            try {
                boost::filesystem::rename(legacy_path, versioned_path);
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed to rename legacy library: " << e.what();
            }
        }
    }

    // Load versioned library
#if defined(_MSC_VER) || defined(_WIN32)
    library = plugin_folder.string() + "\\" + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".dll";
#else
    #if defined(__WXMAC__)
    std::string lib_ext = ".dylib";
    #else
    std::string lib_ext = ".so";
    #endif
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + lib_ext;
#endif

#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t lib_wstr[256];
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    m_networking_module = LoadLibrary(lib_wstr);
    if (!m_networking_module) {
        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_NETWORK_LIBRARY));
        if (library_path.empty()) {
            set_load_error(
                "Network library not found",
                "Could not locate versioned library: " + library,
                library
            );
            return -1;
        }
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        m_networking_module = LoadLibrary(lib_wstr);
    }
#else
    m_networking_module = dlopen(library.c_str(), RTLD_LAZY);
    if (!m_networking_module) {
        char* dll_error = dlerror();
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": dlopen failed: " << (dll_error ? dll_error : "unknown error");
        set_load_error(
            "Failed to load network library",
            dll_error ? std::string(dll_error) : "Unknown dlopen error",
            library
        );
    }
#endif

    if (!m_networking_module) {
        if (!m_load_error.has_error) {
            set_load_error(
                "Network library failed to load",
                "LoadLibrary/dlopen returned null",
                library
            );
        }
        return -1;
    }

    // Load file transfer interface
    InitFTModule(m_networking_module);

    // Load all function pointers
    load_all_function_pointers();

    if (m_get_version) {
        (void) m_get_version();
    }

    return 0;
}

int BBLNetworkPlugin::unload()
{
    UnloadFTModule();

#if defined(_MSC_VER) || defined(_WIN32)
    if (m_networking_module) {
        FreeLibrary(m_networking_module);
        m_networking_module = NULL;
    }
    if (m_source_module) {
        FreeLibrary(m_source_module);
        m_source_module = NULL;
    }
#else
    if (m_networking_module) {
        dlclose(m_networking_module);
        m_networking_module = NULL;
    }
    if (m_source_module) {
        dlclose(m_source_module);
        m_source_module = NULL;
    }
#endif

    clear_all_function_pointers();

    return 0;
}

bool BBLNetworkPlugin::is_loaded() const
{
    return m_networking_module != nullptr;
}

std::string BBLNetworkPlugin::get_version() const
{
    bool consistent = true;
    // Check the debug consistent first
    if (m_check_debug_consistent) {
#if defined(NDEBUG)
        consistent = m_check_debug_consistent(false);
#else
        consistent = m_check_debug_consistent(true);
#endif
    }
    if (!consistent) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", inconsistent library, return 00.00.00.00!");
        return "00.00.00.00";
    }
    if (m_get_version) {
        return m_get_version();
    }
    BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << boost::format(", get_version not supported, return 00.00.00.00!");
    return "00.00.00.00";
}

// ============================================================================
// Agent Lifecycle
// ============================================================================

void* BBLNetworkPlugin::create_agent(const std::string& log_dir)
{
    if (m_agent) {
        return m_agent;
    }

    if (m_create_agent) {
        m_agent = m_create_agent(log_dir);
    }

    return m_agent;
}

int BBLNetworkPlugin::destroy_agent()
{
    int ret = 0;
    if (m_agent && m_destroy_agent) {
        ret = m_destroy_agent(m_agent);
    }
    m_agent = nullptr;
    return ret;
}

// ============================================================================
// DLL Module Accessors
// ============================================================================

#if defined(_MSC_VER) || defined(_WIN32)
HMODULE BBLNetworkPlugin::get_source_module()
#else
void* BBLNetworkPlugin::get_source_module()
#endif
{
    if ((m_source_module) || (!m_networking_module))
        return m_source_module;

    std::string library;
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t lib_wstr[128];

    library = plugin_folder.string() + "/" + std::string(BAMBU_SOURCE_LIBRARY) + ".dll";
    memset(lib_wstr, 0, sizeof(lib_wstr));
    ::MultiByteToWideChar(CP_UTF8, NULL, library.c_str(), strlen(library.c_str())+1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
    m_source_module = LoadLibrary(lib_wstr);
    if (!m_source_module) {
        std::string library_path = get_libpath_in_current_directory(std::string(BAMBU_SOURCE_LIBRARY));
        if (library_path.empty()) {
            return m_source_module;
        }
        memset(lib_wstr, 0, sizeof(lib_wstr));
        ::MultiByteToWideChar(CP_UTF8, NULL, library_path.c_str(), strlen(library_path.c_str()) + 1, lib_wstr, sizeof(lib_wstr) / sizeof(lib_wstr[0]));
        m_source_module = LoadLibrary(lib_wstr);
    }
#else
#if defined(__WXMAC__)
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".dylib";
#else
    library = plugin_folder.string() + "/" + std::string("lib") + std::string(BAMBU_SOURCE_LIBRARY) + ".so";
#endif
    m_source_module = dlopen(library.c_str(), RTLD_LAZY);
#endif

    return m_source_module;
}

void* BBLNetworkPlugin::get_function(const char* name)
{
    void* function = nullptr;

    if (!m_networking_module)
        return function;

#if defined(_MSC_VER) || defined(_WIN32)
    function = GetProcAddress(m_networking_module, name);
#else
    function = dlsym(m_networking_module, name);
#endif

    return function;
}

// ============================================================================
// Utility Methods
// ============================================================================

std::string BBLNetworkPlugin::get_libpath_in_current_directory(const std::string& library_name)
{
    std::string lib_path;
#if defined(_MSC_VER) || defined(_WIN32)
    wchar_t file_name[512];
    DWORD ret = GetModuleFileNameW(NULL, file_name, 512);
    if (!ret) {
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
    (void)library_name;
#endif
    return lib_path;
}

std::string BBLNetworkPlugin::get_versioned_library_path(const std::string& version)
{
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

#if defined(_MSC_VER) || defined(_WIN32)
    return (plugin_folder / (std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".dll")).string();
#elif defined(__WXMAC__)
    return (plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".dylib")).string();
#else
    return (plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_" + version + ".so")).string();
#endif
}

bool BBLNetworkPlugin::versioned_library_exists(const std::string& version)
{
    if (version.empty()) return false;
    std::string path = get_versioned_library_path(version);

    if (boost::filesystem::exists(path)) return true;

    if (version == BAMBU_NETWORK_AGENT_VERSION_LEGACY) {
        return legacy_library_exists();
    }

    return false;
}

bool BBLNetworkPlugin::legacy_library_exists()
{
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

#if defined(_MSC_VER) || defined(_WIN32)
    auto legacy_path = plugin_folder / (std::string(BAMBU_NETWORK_LIBRARY) + ".dll");
#elif defined(__WXMAC__)
    auto legacy_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".dylib");
#else
    auto legacy_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".so");
#endif
    return boost::filesystem::exists(legacy_path);
}

void BBLNetworkPlugin::remove_legacy_library()
{
    std::string data_dir_str = data_dir();
    boost::filesystem::path data_dir_path(data_dir_str);
    auto plugin_folder = data_dir_path / "plugins";

#if defined(_MSC_VER) || defined(_WIN32)
    auto legacy_path = plugin_folder / (std::string(BAMBU_NETWORK_LIBRARY) + ".dll");
#elif defined(__WXMAC__)
    auto legacy_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".dylib");
#else
    auto legacy_path = plugin_folder / (std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + ".so");
#endif

    if (boost::filesystem::exists(legacy_path)) {
        boost::system::error_code ec;
        boost::filesystem::remove(legacy_path, ec);
    }
}

std::vector<std::string> BBLNetworkPlugin::scan_plugin_versions()
{
    std::vector<std::string> discovered_versions;
    std::string data_dir_str = data_dir();
    boost::filesystem::path plugin_folder = boost::filesystem::path(data_dir_str) / "plugins";

    if (!boost::filesystem::is_directory(plugin_folder)) {
        return discovered_versions;
    }

#if defined(_MSC_VER) || defined(_WIN32)
    std::string prefix = std::string(BAMBU_NETWORK_LIBRARY) + "_";
    std::string extension = ".dll";
#elif defined(__WXMAC__)
    std::string prefix = std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_";
    std::string extension = ".dylib";
#else
    std::string prefix = std::string("lib") + std::string(BAMBU_NETWORK_LIBRARY) + "_";
    std::string extension = ".so";
#endif

    boost::system::error_code ec;
    for (auto& entry : boost::filesystem::directory_iterator(plugin_folder, ec)) {
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": error iterating directory: " << ec.message();
            break;
        }
        if (!boost::filesystem::is_regular_file(entry.status()))
            continue;

        std::string filename = entry.path().filename().string();

        if (filename.rfind(prefix, 0) != 0)
            continue;
        if (filename.size() <= extension.size() ||
            filename.compare(filename.size() - extension.size(), extension.size(), extension) != 0)
            continue;

        std::string version = filename.substr(prefix.size(),
                                               filename.size() - prefix.size() - extension.size());
        discovered_versions.push_back(version);
    }

    return discovered_versions;
}

// ============================================================================
// Error Handling
// ============================================================================

void BBLNetworkPlugin::clear_load_error()
{
    m_load_error = NetworkLibraryLoadError{};
}

void BBLNetworkPlugin::set_load_error(const std::string& message,
                                       const std::string& technical_details,
                                       const std::string& attempted_path)
{
    m_load_error.has_error = true;
    m_load_error.message = message;
    m_load_error.technical_details = technical_details;
    m_load_error.attempted_path = attempted_path;
}

// ============================================================================
// Legacy Helper
// ============================================================================

PrintParams_Legacy BBLNetworkPlugin::as_legacy(PrintParams& param)
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

// ============================================================================
// Function Pointer Loading
// ============================================================================

void BBLNetworkPlugin::load_all_function_pointers()
{
    m_check_debug_consistent = reinterpret_cast<func_check_debug_consistent>(get_function("bambu_network_check_debug_consistent"));
    m_get_version = reinterpret_cast<func_get_version>(get_function("bambu_network_get_version"));
    m_create_agent = reinterpret_cast<func_create_agent>(get_function("bambu_network_create_agent"));
    m_destroy_agent = reinterpret_cast<func_destroy_agent>(get_function("bambu_network_destroy_agent"));
    m_init_log = reinterpret_cast<func_init_log>(get_function("bambu_network_init_log"));
    m_set_config_dir = reinterpret_cast<func_set_config_dir>(get_function("bambu_network_set_config_dir"));
    m_set_cert_file = reinterpret_cast<func_set_cert_file>(get_function("bambu_network_set_cert_file"));
    m_set_country_code = reinterpret_cast<func_set_country_code>(get_function("bambu_network_set_country_code"));
    m_start = reinterpret_cast<func_start>(get_function("bambu_network_start"));
    m_set_on_ssdp_msg_fn = reinterpret_cast<func_set_on_ssdp_msg_fn>(get_function("bambu_network_set_on_ssdp_msg_fn"));
    m_set_on_user_login_fn = reinterpret_cast<func_set_on_user_login_fn>(get_function("bambu_network_set_on_user_login_fn"));
    m_set_on_printer_connected_fn = reinterpret_cast<func_set_on_printer_connected_fn>(get_function("bambu_network_set_on_printer_connected_fn"));
    m_set_on_server_connected_fn = reinterpret_cast<func_set_on_server_connected_fn>(get_function("bambu_network_set_on_server_connected_fn"));
    m_set_on_http_error_fn = reinterpret_cast<func_set_on_http_error_fn>(get_function("bambu_network_set_on_http_error_fn"));
    m_set_get_country_code_fn = reinterpret_cast<func_set_get_country_code_fn>(get_function("bambu_network_set_get_country_code_fn"));
    m_set_on_subscribe_failure_fn = reinterpret_cast<func_set_on_subscribe_failure_fn>(get_function("bambu_network_set_on_subscribe_failure_fn"));
    m_set_on_message_fn = reinterpret_cast<func_set_on_message_fn>(get_function("bambu_network_set_on_message_fn"));
    m_set_on_user_message_fn = reinterpret_cast<func_set_on_user_message_fn>(get_function("bambu_network_set_on_user_message_fn"));
    m_set_on_local_connect_fn = reinterpret_cast<func_set_on_local_connect_fn>(get_function("bambu_network_set_on_local_connect_fn"));
    m_set_on_local_message_fn = reinterpret_cast<func_set_on_local_message_fn>(get_function("bambu_network_set_on_local_message_fn"));
    m_set_queue_on_main_fn = reinterpret_cast<func_set_queue_on_main_fn>(get_function("bambu_network_set_queue_on_main_fn"));
    m_connect_server = reinterpret_cast<func_connect_server>(get_function("bambu_network_connect_server"));
    m_is_server_connected = reinterpret_cast<func_is_server_connected>(get_function("bambu_network_is_server_connected"));
    m_refresh_connection = reinterpret_cast<func_refresh_connection>(get_function("bambu_network_refresh_connection"));
    m_start_subscribe = reinterpret_cast<func_start_subscribe>(get_function("bambu_network_start_subscribe"));
    m_stop_subscribe = reinterpret_cast<func_stop_subscribe>(get_function("bambu_network_stop_subscribe"));
    m_add_subscribe = reinterpret_cast<func_add_subscribe>(get_function("bambu_network_add_subscribe"));
    m_del_subscribe = reinterpret_cast<func_del_subscribe>(get_function("bambu_network_del_subscribe"));
    m_enable_multi_machine = reinterpret_cast<func_enable_multi_machine>(get_function("bambu_network_enable_multi_machine"));
    m_send_message = reinterpret_cast<func_send_message>(get_function("bambu_network_send_message"));
    m_connect_printer = reinterpret_cast<func_connect_printer>(get_function("bambu_network_connect_printer"));
    m_disconnect_printer = reinterpret_cast<func_disconnect_printer>(get_function("bambu_network_disconnect_printer"));
    m_send_message_to_printer = reinterpret_cast<func_send_message_to_printer>(get_function("bambu_network_send_message_to_printer"));
    m_check_cert = reinterpret_cast<func_check_cert>(get_function("bambu_network_update_cert"));
    m_install_device_cert = reinterpret_cast<func_install_device_cert>(get_function("bambu_network_install_device_cert"));
    m_start_discovery = reinterpret_cast<func_start_discovery>(get_function("bambu_network_start_discovery"));
    m_change_user = reinterpret_cast<func_change_user>(get_function("bambu_network_change_user"));
    m_is_user_login = reinterpret_cast<func_is_user_login>(get_function("bambu_network_is_user_login"));
    m_user_logout = reinterpret_cast<func_user_logout>(get_function("bambu_network_user_logout"));
    m_get_user_id = reinterpret_cast<func_get_user_id>(get_function("bambu_network_get_user_id"));
    m_get_user_name = reinterpret_cast<func_get_user_name>(get_function("bambu_network_get_user_name"));
    m_get_user_avatar = reinterpret_cast<func_get_user_avatar>(get_function("bambu_network_get_user_avatar"));
    m_get_user_nickanme = reinterpret_cast<func_get_user_nickanme>(get_function("bambu_network_get_user_nickanme"));
    m_build_login_cmd = reinterpret_cast<func_build_login_cmd>(get_function("bambu_network_build_login_cmd"));
    m_build_logout_cmd = reinterpret_cast<func_build_logout_cmd>(get_function("bambu_network_build_logout_cmd"));
    m_build_login_info = reinterpret_cast<func_build_login_info>(get_function("bambu_network_build_login_info"));
    m_ping_bind = reinterpret_cast<func_ping_bind>(get_function("bambu_network_ping_bind"));
    m_bind_detect = reinterpret_cast<func_bind_detect>(get_function("bambu_network_bind_detect"));
    m_set_server_callback = reinterpret_cast<func_set_server_callback>(get_function("bambu_network_set_server_callback"));
    m_bind = reinterpret_cast<func_bind>(get_function("bambu_network_bind"));
    m_unbind = reinterpret_cast<func_unbind>(get_function("bambu_network_unbind"));
    m_get_bambulab_host = reinterpret_cast<func_get_bambulab_host>(get_function("bambu_network_get_bambulab_host"));
    m_get_user_selected_machine = reinterpret_cast<func_get_user_selected_machine>(get_function("bambu_network_get_user_selected_machine"));
    m_set_user_selected_machine = reinterpret_cast<func_set_user_selected_machine>(get_function("bambu_network_set_user_selected_machine"));
    m_start_print = reinterpret_cast<func_start_print>(get_function("bambu_network_start_print"));
    m_start_local_print_with_record = reinterpret_cast<func_start_local_print_with_record>(get_function("bambu_network_start_local_print_with_record"));
    m_start_send_gcode_to_sdcard = reinterpret_cast<func_start_send_gcode_to_sdcard>(get_function("bambu_network_start_send_gcode_to_sdcard"));
    m_start_local_print = reinterpret_cast<func_start_local_print>(get_function("bambu_network_start_local_print"));
    m_start_sdcard_print = reinterpret_cast<func_start_sdcard_print>(get_function("bambu_network_start_sdcard_print"));
    m_get_user_presets = reinterpret_cast<func_get_user_presets>(get_function("bambu_network_get_user_presets"));
    m_request_setting_id = reinterpret_cast<func_request_setting_id>(get_function("bambu_network_request_setting_id"));
    m_put_setting = reinterpret_cast<func_put_setting>(get_function("bambu_network_put_setting"));
    m_get_setting_list = reinterpret_cast<func_get_setting_list>(get_function("bambu_network_get_setting_list"));
    m_get_setting_list2 = reinterpret_cast<func_get_setting_list2>(get_function("bambu_network_get_setting_list2"));
    m_delete_setting = reinterpret_cast<func_delete_setting>(get_function("bambu_network_delete_setting"));
    m_get_studio_info_url = reinterpret_cast<func_get_studio_info_url>(get_function("bambu_network_get_studio_info_url"));
    m_set_extra_http_header = reinterpret_cast<func_set_extra_http_header>(get_function("bambu_network_set_extra_http_header"));
    m_get_my_message = reinterpret_cast<func_get_my_message>(get_function("bambu_network_get_my_message"));
    m_check_user_task_report = reinterpret_cast<func_check_user_task_report>(get_function("bambu_network_check_user_task_report"));
    m_get_user_print_info = reinterpret_cast<func_get_user_print_info>(get_function("bambu_network_get_user_print_info"));
    m_get_user_tasks = reinterpret_cast<func_get_user_tasks>(get_function("bambu_network_get_user_tasks"));
    m_get_printer_firmware = reinterpret_cast<func_get_printer_firmware>(get_function("bambu_network_get_printer_firmware"));
    m_get_task_plate_index = reinterpret_cast<func_get_task_plate_index>(get_function("bambu_network_get_task_plate_index"));
    m_get_user_info = reinterpret_cast<func_get_user_info>(get_function("bambu_network_get_user_info"));
    m_request_bind_ticket = reinterpret_cast<func_request_bind_ticket>(get_function("bambu_network_request_bind_ticket"));
    m_get_subtask_info = reinterpret_cast<func_get_subtask_info>(get_function("bambu_network_get_subtask_info"));
    m_get_slice_info = reinterpret_cast<func_get_slice_info>(get_function("bambu_network_get_slice_info"));
    m_query_bind_status = reinterpret_cast<func_query_bind_status>(get_function("bambu_network_query_bind_status"));
    m_modify_printer_name = reinterpret_cast<func_modify_printer_name>(get_function("bambu_network_modify_printer_name"));
    m_get_camera_url = reinterpret_cast<func_get_camera_url>(get_function("bambu_network_get_camera_url"));
    m_get_design_staffpick = reinterpret_cast<func_get_design_staffpick>(get_function("bambu_network_get_design_staffpick"));
    m_start_publish = reinterpret_cast<func_start_pubilsh>(get_function("bambu_network_start_publish"));
    m_get_model_publish_url = reinterpret_cast<func_get_model_publish_url>(get_function("bambu_network_get_model_publish_url"));
    m_get_subtask = reinterpret_cast<func_get_subtask>(get_function("bambu_network_get_subtask"));
    m_get_model_mall_home_url = reinterpret_cast<func_get_model_mall_home_url>(get_function("bambu_network_get_model_mall_home_url"));
    m_get_model_mall_detail_url = reinterpret_cast<func_get_model_mall_detail_url>(get_function("bambu_network_get_model_mall_detail_url"));
    m_get_my_profile = reinterpret_cast<func_get_my_profile>(get_function("bambu_network_get_my_profile"));
    m_track_enable = reinterpret_cast<func_track_enable>(get_function("bambu_network_track_enable"));
    m_track_remove_files = reinterpret_cast<func_track_remove_files>(get_function("bambu_network_track_remove_files"));
    m_track_event = reinterpret_cast<func_track_event>(get_function("bambu_network_track_event"));
    m_track_header = reinterpret_cast<func_track_header>(get_function("bambu_network_track_header"));
    m_track_update_property = reinterpret_cast<func_track_update_property>(get_function("bambu_network_track_update_property"));
    m_track_get_property = reinterpret_cast<func_track_get_property>(get_function("bambu_network_track_get_property"));
    m_put_model_mall_rating = reinterpret_cast<func_put_model_mall_rating_url>(get_function("bambu_network_put_model_mall_rating"));
    m_get_oss_config = reinterpret_cast<func_get_oss_config>(get_function("bambu_network_get_oss_config"));
    m_put_rating_picture_oss = reinterpret_cast<func_put_rating_picture_oss>(get_function("bambu_network_put_rating_picture_oss"));
    m_get_model_mall_rating_result = reinterpret_cast<func_get_model_mall_rating_result>(get_function("bambu_network_get_model_mall_rating"));
    m_get_mw_user_preference = reinterpret_cast<func_get_mw_user_preference>(get_function("bambu_network_get_mw_user_preference"));
    m_get_mw_user_4ulist = reinterpret_cast<func_get_mw_user_4ulist>(get_function("bambu_network_get_mw_user_4ulist"));
}

void BBLNetworkPlugin::clear_all_function_pointers()
{
    m_check_debug_consistent = nullptr;
    m_get_version = nullptr;
    m_create_agent = nullptr;
    m_destroy_agent = nullptr;
    m_init_log = nullptr;
    m_set_config_dir = nullptr;
    m_set_cert_file = nullptr;
    m_set_country_code = nullptr;
    m_start = nullptr;
    m_set_on_ssdp_msg_fn = nullptr;
    m_set_on_user_login_fn = nullptr;
    m_set_on_printer_connected_fn = nullptr;
    m_set_on_server_connected_fn = nullptr;
    m_set_on_http_error_fn = nullptr;
    m_set_get_country_code_fn = nullptr;
    m_set_on_subscribe_failure_fn = nullptr;
    m_set_on_message_fn = nullptr;
    m_set_on_user_message_fn = nullptr;
    m_set_on_local_connect_fn = nullptr;
    m_set_on_local_message_fn = nullptr;
    m_set_queue_on_main_fn = nullptr;
    m_connect_server = nullptr;
    m_is_server_connected = nullptr;
    m_refresh_connection = nullptr;
    m_start_subscribe = nullptr;
    m_stop_subscribe = nullptr;
    m_add_subscribe = nullptr;
    m_del_subscribe = nullptr;
    m_enable_multi_machine = nullptr;
    m_send_message = nullptr;
    m_connect_printer = nullptr;
    m_disconnect_printer = nullptr;
    m_send_message_to_printer = nullptr;
    m_check_cert = nullptr;
    m_install_device_cert = nullptr;
    m_start_discovery = nullptr;
    m_change_user = nullptr;
    m_is_user_login = nullptr;
    m_user_logout = nullptr;
    m_get_user_id = nullptr;
    m_get_user_name = nullptr;
    m_get_user_avatar = nullptr;
    m_get_user_nickanme = nullptr;
    m_build_login_cmd = nullptr;
    m_build_logout_cmd = nullptr;
    m_build_login_info = nullptr;
    m_ping_bind = nullptr;
    m_bind_detect = nullptr;
    m_set_server_callback = nullptr;
    m_bind = nullptr;
    m_unbind = nullptr;
    m_get_bambulab_host = nullptr;
    m_get_user_selected_machine = nullptr;
    m_set_user_selected_machine = nullptr;
    m_start_print = nullptr;
    m_start_local_print_with_record = nullptr;
    m_start_send_gcode_to_sdcard = nullptr;
    m_start_local_print = nullptr;
    m_start_sdcard_print = nullptr;
    m_get_user_presets = nullptr;
    m_request_setting_id = nullptr;
    m_put_setting = nullptr;
    m_get_setting_list = nullptr;
    m_get_setting_list2 = nullptr;
    m_delete_setting = nullptr;
    m_get_studio_info_url = nullptr;
    m_set_extra_http_header = nullptr;
    m_get_my_message = nullptr;
    m_check_user_task_report = nullptr;
    m_get_user_print_info = nullptr;
    m_get_user_tasks = nullptr;
    m_get_printer_firmware = nullptr;
    m_get_task_plate_index = nullptr;
    m_get_user_info = nullptr;
    m_request_bind_ticket = nullptr;
    m_get_subtask_info = nullptr;
    m_get_slice_info = nullptr;
    m_query_bind_status = nullptr;
    m_modify_printer_name = nullptr;
    m_get_camera_url = nullptr;
    m_get_design_staffpick = nullptr;
    m_start_publish = nullptr;
    m_get_model_publish_url = nullptr;
    m_get_subtask = nullptr;
    m_get_model_mall_home_url = nullptr;
    m_get_model_mall_detail_url = nullptr;
    m_get_my_profile = nullptr;
    m_track_enable = nullptr;
    m_track_remove_files = nullptr;
    m_track_event = nullptr;
    m_track_header = nullptr;
    m_track_update_property = nullptr;
    m_track_get_property = nullptr;
    m_put_model_mall_rating = nullptr;
    m_get_oss_config = nullptr;
    m_put_rating_picture_oss = nullptr;
    m_get_model_mall_rating_result = nullptr;
    m_get_mw_user_preference = nullptr;
    m_get_mw_user_4ulist = nullptr;
}

std::vector<NetworkLibraryVersionInfo> get_all_available_versions()
{
    std::vector<NetworkLibraryVersionInfo> result;
    std::set<std::string> known_base_versions;
    std::set<std::string> all_known_versions;

    for (size_t i = 0; i < AVAILABLE_NETWORK_VERSIONS_COUNT; ++i) {
        result.push_back(NetworkLibraryVersionInfo::from_static(AVAILABLE_NETWORK_VERSIONS[i]));
        known_base_versions.insert(AVAILABLE_NETWORK_VERSIONS[i].version);
        all_known_versions.insert(AVAILABLE_NETWORK_VERSIONS[i].version);
    }

    std::vector<std::string> discovered = BBLNetworkPlugin::scan_plugin_versions();

    std::vector<std::pair<std::string, std::string>> suffixed_versions;

    for (const auto& version : discovered) {
        if (all_known_versions.count(version) > 0)
            continue;

        std::string base = extract_base_version(version);
        std::string suffix = extract_suffix(version);

        if (suffix.empty())
            continue;

        if (known_base_versions.count(base) == 0)
            continue;

        suffixed_versions.emplace_back(base, version);
        all_known_versions.insert(version);
    }

    std::sort(suffixed_versions.begin(), suffixed_versions.end(),
              [](const auto& a, const auto& b) {
                  if (a.first != b.first) return a.first > b.first;
                  return a.second < b.second;
              });

    for (const auto& [base, full] : suffixed_versions) {
        size_t insert_pos = 0;
        for (size_t i = 0; i < result.size(); ++i) {
            if (result[i].base_version == base) {
                insert_pos = i + 1;
                while (insert_pos < result.size() &&
                       result[insert_pos].base_version == base) {
                    ++insert_pos;
                }
                break;
            }
        }

        std::string sfx = extract_suffix(full);
        result.insert(result.begin() + insert_pos,
                      NetworkLibraryVersionInfo::from_discovered(full, base, sfx));
    }

    return result;
}


} // namespace Slic3r
