#include "DevConfigUtil.h"

#include <algorithm>
#include <cctype>

#include <wx/dir.h>
#include <boost/filesystem.hpp>

using namespace nlohmann;

namespace Slic3r
{

std::string DevPrinterConfigUtil::m_resource_file_path = "";


std::map<std::string, std::string> DevPrinterConfigUtil::get_all_model_id_with_name()
{
    {
        std::map<std::string, std::string> models;

        wxDir dir(m_resource_file_path + "/printers/");
        if (!dir.IsOpened())
        {
            return models;
        }

        wxString filename;
        std::vector<wxString> m_files;
        bool hasFile = dir.GetFirst(&filename, wxEmptyString, wxDIR_FILES);
        while (hasFile)
        {
            m_files.push_back(filename);
            hasFile = dir.GetNext(&filename);
        }

        for (wxString file : m_files)
        {
            if (!file.Lower().ends_with(".json")) continue;

            std::string config_file = m_resource_file_path + "/printers/" + file.ToStdString();
            boost::nowide::ifstream json_file(config_file.c_str());

            try
            {
                json jj;
                if (json_file.is_open())
                {
                    json_file >> jj;
                    if (jj.contains("00.00.00.00"))
                    {
                        json const& printer = jj["00.00.00.00"];

                        std::string model_id;
                        std::string display_name;
                        if (printer.contains("model_id")) { model_id = printer["model_id"].get<std::string>(); }
                        if (printer.contains("display_name")) { display_name = printer["display_name"].get<std::string>(); }
                        models[display_name] = model_id;
                    }
                }
            }
            catch (...) {}
        }

        return models;
    }
}

PrinterArch DevPrinterConfigUtil::get_printer_arch(std::string type_str)
{
    const std::string& arch_str = get_value_from_config<std::string>(type_str, "printer_arch");
    if (arch_str == "i3")
    {
        return PrinterArch::ARCH_I3;
    }
    else if (arch_str == "core_xy")
    {
        return PrinterArch::ARCH_CORE_XY;
    }

    return PrinterArch::ARCH_CORE_XY;
}

std::string DevPrinterConfigUtil::get_printer_ext_img(const std::string& type_str, int pos)
{
    const auto& vec = get_value_from_config<std::vector<std::string>>(type_str, "printer_ext_image");
    return (vec.size() > pos) ? vec[pos] : std::string();
};

std::string DevPrinterConfigUtil::get_fan_text(const std::string& type_str, const std::string& key)
{
    std::vector<std::string> filaments;
    std::string              config_file = m_resource_file_path + "/printers/" + type_str + ".json";
    boost::nowide::ifstream  json_file(config_file.c_str());
    try
    {
        json jj;
        if (json_file.is_open())
        {
            json_file >> jj;
            if (jj.contains("00.00.00.00"))
            {
                json const& printer = jj["00.00.00.00"];
                if (printer.contains("fan") && printer["fan"].contains(key))
                {
                    return printer["fan"][key].get<std::string>();
                }
            }
        }
    }
    catch (...) {}
    return std::string();
}

std::string DevPrinterConfigUtil::get_fan_text(const std::string& type_str, int airduct_mode, int airduct_func, int submode)
{
    std::vector<std::string> filaments;
    std::string              config_file = m_resource_file_path + "/printers/" + type_str + ".json";
    boost::nowide::ifstream  json_file(config_file.c_str());
    try
    {
        json jj;
        if (json_file.is_open())
        {
            json_file >> jj;
            if (jj.contains("00.00.00.00"))
            {
                json const& printer = jj["00.00.00.00"];
                if (!printer.contains("fan"))
                {
                    return std::string();
                }

                json const& fan_item = printer["fan"];
                const auto& airduct_mode_str = std::to_string(airduct_mode);
                if (!fan_item.contains(airduct_mode_str))
                {
                    return std::string();
                }

                json const& airduct_item = fan_item[airduct_mode_str];
                const auto& airduct_func_str = std::to_string(airduct_func);
                if (airduct_item.contains(airduct_func_str))
                {
                    const auto& airduct_func_item = airduct_item[airduct_func_str];
                    if (airduct_func_item.is_object())
                    {
                        return airduct_func_item[std::to_string(submode)].get<std::string>();
                    }
                    else if (airduct_func_item.is_string())
                    {
                        return airduct_func_item.get<std::string>();
                    }
                }
            }
        }
    }
    catch (...) {}
    return std::string();
}

std::map<std::string, std::vector<std::string>> DevPrinterConfigUtil::get_all_subseries(std::string type_str)
{
    std::map<std::string, std::vector<std::string>> subseries;

#if !BBL_RELEASE_TO_PUBLIC
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": path= " << m_resource_file_path + "/printers/";
#endif

    try
    {
        const auto& from_dir = m_resource_file_path + "/printers/";
        if (!boost::filesystem::exists(from_dir))
        {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": direction does not exist ";
            return subseries;
        }

        for (const auto& entry : boost::filesystem::directory_iterator(from_dir))
        {
            const boost::filesystem::path& file_path = entry.path();
            if (boost::filesystem::is_regular_file(file_path) && file_path.extension() == ".json")
            {
                try
                {
                    json jj;
                    boost::nowide::ifstream json_file(file_path.string());
                    if (json_file.is_open())
                    {
                        json_file >> jj;
                        if (jj.contains("00.00.00.00"))
                        {
                            json const& printer = jj["00.00.00.00"];
                            if (printer.contains("subseries"))
                            {
                                std::vector<std::string> subs;
                                std::string model_id = printer["model_id"].get<std::string>();
                                if (model_id == type_str || type_str.empty())
                                {
                                    for (auto res : printer["subseries"])
                                    {
                                        subs.emplace_back(res.get<std::string>());
                                    }
                                }
                                subseries.insert(make_pair(model_id, subs));
                            }
                        }
                    }
                }
                catch (...)
                {
                    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": failed to load " << file_path.filename().string();
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": std::exception: " << e.what();
    }
    catch (...)
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": unknown exception";
    }

#if !BBL_RELEASE_TO_PUBLIC
    wxString result_str;
    for (auto item : subseries)
    {
        wxString item_str = item.first;
        item_str += ": ";
        for (auto to_item : item.second)
        {
            item_str += to_item;
            item_str += " ";
        }

        result_str += item_str + ", ";
    }

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": result= " << result_str;
#endif

    if (subseries.empty())
    {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ": result= " << "empty";
    }

    return subseries;
}

std::string DevPrinterConfigUtil::get_toolhead_display_name(
    const std::string& type_str,
    int ext_id,
    ToolHeadComponent component,
    ToolHeadNameCase name_case,
    bool short_name)
{
    static const std::map<ToolHeadComponent, std::string> comp_keys = {
        { ToolHeadComponent::Extruder, "extruder" },
        { ToolHeadComponent::Nozzle,   "nozzle" },
        { ToolHeadComponent::Hotend,   "hotend" }
    };

    const int case_index = static_cast<int>(name_case);
    const std::string role_key = std::to_string(ext_id);
    const std::string& comp_key = comp_keys.at(component);

    std::string result;
    auto names_json = get_value_from_config<json>(type_str, "tool_head_display_names");
    if (!names_json.is_null() && names_json.contains(role_key) && names_json[role_key].contains(comp_key)) {
        auto& arr = names_json[role_key][comp_key];
        if (arr.is_array() && case_index < static_cast<int>(arr.size()))
            result = arr[case_index].get<std::string>();
    }

    if (result.empty()) {
        const std::string side = ext_id == DEPUTY_EXTRUDER_ID ? "Left" : "Right";
        const std::string component_name = component == ToolHeadComponent::Extruder ? "Extruder" :
                                           component == ToolHeadComponent::Hotend ? "Hotend" : "Nozzle";
        result = side + " " + component_name;
        if (name_case == ToolHeadNameCase::SentenceCase && result.size() > side.size() + 1)
            result[side.size() + 1] = static_cast<char>(std::tolower(static_cast<unsigned char>(result[side.size() + 1])));
        else if (name_case == ToolHeadNameCase::LowerCase)
            std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }

    if (short_name) {
        auto sp = result.find(' ');
        if (sp != std::string::npos)
            result = result.substr(0, sp);
    }

    return result;
}

};
