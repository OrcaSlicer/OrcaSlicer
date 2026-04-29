/**
* @file DevConfigUtil.h
* @brief Parses configuration files and provides access to printer options.
*
* This class loads a configuration file and allows querying options by key.
* The configuration file format is expected to be key-value pairs (e.g., INI or simple text).
*/

#pragma once

#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>

#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <wx/string.h>

#include "slic3r/GUI/DeviceCore/DevDefs.h"

namespace Slic3r
{

/// Toolhead component type (extruder / nozzle / hotend)
enum class ToolHeadComponent {
    Extruder,
    Nozzle,
    Hotend
};

/// Name case style for toolhead display names
enum class ToolHeadNameCase {
    TitleCase    = 0,  // [0] "Main Extruder"  — panel titles, section headers
    SentenceCase = 1,  // [1] "Main extruder"  — static box labels
    LowerCase    = 2   // [2] "main extruder"  — inline text, sentence concatenation
};

class dePrinterConfigFactory
{
public:
    dePrinterConfigFactory() = default;
    ~dePrinterConfigFactory() = default;
};


class DevPrinterConfigUtil
{
public:
    DevPrinterConfigUtil() = default;
    ~DevPrinterConfigUtil() = default;

public:
    static void  InitFilePath(const std::string& res_file_path) { m_resource_file_path = res_file_path; };

    /*printer*/
    // info
    static std::map<std::string, std::string> get_all_model_id_with_name();
    static std::string get_printer_type(const std::string& type_str) { return get_value_from_config<std::string>(type_str, "printer_type"); }
    static std::string get_printer_display_name(const std::string& type_str) { return get_value_from_config<std::string>(type_str, "display_name"); }
    static std::string get_printer_series_str(std::string type_str) { return get_value_from_config<std::string>(type_str, "printer_series"); }
    static PrinterArch get_printer_arch(std::string type_str);

    // images
    static std::string get_printer_thumbnail_img(const std::string& type_str) { return get_value_from_config<std::string>(type_str, "printer_thumbnail_image"); }
    static std::string get_printer_connect_help_img(const std::string& type_str) { return get_value_from_config<std::string>(type_str, "printer_connect_help_image"); }
    static std::string get_printer_auto_pa_cali_image(const std::string& type_str) { return get_value_from_config<std::string>(type_str, "auto_pa_cali_thumbnail_image"); }

    /*media*/
    static std::string get_ftp_folder(std::string type_str) { return get_value_from_config<std::string>(type_str, "ftp_folder"); }
    static std::vector<std::string> get_resolution_supported(std::string type_str) { return get_value_from_config<std::vector<std::string>>(type_str, "camera_resolution"); }
    static std::vector<std::string> get_compatible_machine(std::string type_str) { return get_value_from_config<std::vector<std::string>>(type_str, "compatible_machine"); }
    static std::map<std::string, std::vector<std::string>> get_all_subseries(std::string type_str = "");

    /*ams*/
    static std::string get_printer_use_ams_type(std::string type_str) { return get_value_from_config<std::string>(type_str, "use_ams_type"); }
    static std::string get_printer_ams_img(const std::string& type_str) { return get_value_from_config<std::string>(type_str, "printer_use_ams_image"); }
    static std::string get_printer_ext_img(const std::string& type_str, int pos);//printer_ext_image
    static bool        support_ams_fila_change_abort(std::string type_str) { return get_value_from_config<bool>(type_str, "print", "support_ams_filament_change_abort"); }
    static std::string get_filament_load_img(const std::string &type_str, int ext_id, bool has_nozzle_rack = false);

    /*fan*/
    static std::string              get_fan_text(const std::string& type_str, const std::string& key);
    static std::vector<std::string> get_fan_text_params(const std::string& type_str, const std::string& key);
    static std::string get_fan_text(const std::string& type_str, int airduct_mode, int airduct_func, int submode);
    static std::string get_fan_mode_text(const std::string& type_str, int airduct_mode, const std::string& key);

    /*extruder*/
    static bool get_printer_can_set_nozzle(std::string type_str) { return get_value_from_config<bool>(type_str, "enable_set_nozzle_info"); }// can set nozzle from studio

    /*toolhead display names (extruder / nozzle / hotend)*/
    static std::string get_toolhead_display_name(
        const std::string& type_str,
        int ext_id,
        ToolHeadComponent component,
        ToolHeadNameCase name_case = ToolHeadNameCase::TitleCase,
        bool short_name = false);

    /*print job*/
    static bool support_print_check_firmware_for_tpu_left(std::string type_str){ return get_value_from_config<bool>(type_str, "print", "support_print_check_firmware_for_tpu_left"); }
    static bool support_user_first_setup_tpu_check(std::string type_str){ return get_value_from_config<bool>(type_str, "print", "support_user_first_setup_tpu_check"); }
    static std::string support_user_first_setup_tpu_check_url(std::string type_str){ return get_value_from_config<std::string>(type_str, "print", "support_user_first_setup_tpu_check_url"); }
    static bool support_ams_ext_mix_print(std::string type_str) { return get_value_from_config<bool>(type_str, "print", "support_ams_ext_mix_print"); }

    /*calibration*/
    static std::vector<std::string> get_unsupport_auto_cali_filaments(std::string type_str) { return get_value_from_config<std::vector<std::string>>(type_str, "auto_cali_not_support_filaments"); }
    static bool support_disable_cali_flow_type(std::string type_str) { return get_value_from_config<bool>(type_str,"support_disable_cali_flow_type"); }

    /*detection*/
     static bool support_wrapping_detection(const std::string& type_str) { return get_value_from_config<bool>(type_str, "support_wrapping_detection"); }

    /*safety options*/
    static bool support_safety_options(const std::string &type_str) { return get_value_from_config<bool>(type_str, "support_safety_options"); }

    /*print check*/
    static bool support_print_check_extension_fan_f000_mounted(const std::string& type_str) { return get_value_from_config<bool>(type_str, "print", "support_print_check_extension_fan_f000_mounted"); }
    static std::string air_print_detection_position(const std::string &type_str) { return get_value_from_config<std::string>(type_str, "air_print_detection_position"); }

public:
    template<typename T>
    static T get_value_from_config(const std::string& type_str, const std::string& item)
    {
        std::string config_file = m_resource_file_path + "/printers/" + type_str + ".json";
        boost::nowide::ifstream json_file(config_file.c_str());
        try
        {
            nlohmann::json jj;
            if (json_file.is_open())
            {
                json_file >> jj;
                if (jj.contains("00.00.00.00"))
                {
                    nlohmann::json const& printer = jj["00.00.00.00"];
                    if (printer.contains(item))
                    {
                        return printer[item].get<T>();
                    }
                }
            }
        }
        catch (...) { assert(0 && "get_value_from_config failed"); BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed"; }// there are file errors
        return T();
    };

    template<typename T>
    static T get_value_from_config(const std::string& type_str, const std::string& item1, const std::string& item2)
    {
        try
        {
            const auto& json_item1 = get_value_from_config<nlohmann::json>(type_str, item1);
            if (json_item1.contains(item2))
            {
                return json_item1[item2].get<T>();
            }
        }
        catch (...)
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed to get " << item1 << ", " << item2;
        }

        return T();
    }

    static nlohmann::json get_json_from_config(const std::string& type_str, const std::string& key1, const std::string& key2 = std::string())
    {
        std::string config_file = m_resource_file_path + "/printers/" + type_str + ".json";
        boost::nowide::ifstream json_file(config_file.c_str());
        try
        {
            nlohmann::json jj;
            if (json_file.is_open())
            {
                json_file >> jj;
                if (jj.contains("00.00.00.00"))
                {
                    nlohmann::json const& printer = jj["00.00.00.00"];
                    if (printer.contains(key1))
                    {
                        nlohmann::json const& key1_item = printer[key1];
                        if (key2.empty())
                        {
                            return key1_item;
                        }

                        if (key1_item.contains(key2))
                        {
                            return key1_item[key2];
                        }
                    }
                }
            }
        }
        catch (...) { assert(0 && "get_json_from_config failed"); BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " failed"; }// there are file errors
        return nlohmann::json();
    }

private:
    static std::string m_resource_file_path; // Path to the configuration file
};

/*special transform*/
static std::string _parse_printer_type(const std::string &type_str)
{
    static std::unordered_map<std::string, std::string> s_printer_type_lazy_cache;

    if (type_str == "3DPrinter-X1") return "BL-P002";
    if (type_str == "3DPrinter-X1-Carbon") return "BL-P001";
    if (type_str == "BL-P001" || type_str == "BL-P002") return type_str;

    auto cache_it = s_printer_type_lazy_cache.find(type_str);
    if (cache_it != s_printer_type_lazy_cache.end()) {
        return cache_it->second;
    }
    std::string result = DevPrinterConfigUtil::get_printer_type(type_str);
    if (!result.empty()) {
        s_printer_type_lazy_cache[type_str] = result;
        return result;
    }
    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << " Unsupported printer type: " << type_str;
    return type_str;
}

};// namespace Slic3r