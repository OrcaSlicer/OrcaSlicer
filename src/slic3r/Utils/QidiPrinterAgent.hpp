#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class QidiPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Override filament sync (Qidi-specific implementation)
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Builds the SAVE_VARIABLE lines that make each T<n> macro load the Box slot picked for filament n
    static std::string build_tool_slot_script(const std::string& ams_mapping);

protected:
    // Saves the Box slot picked for each filament in the print dialog, then starts the print through Moonraker
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;
    std::string ams_unit_name(int ams_id) const override { return "Qidi Box " + std::to_string(ams_id + 1); }

private:
    struct QidiFilamentDict
    {
        std::map<int, std::string> colors;
        std::map<int, std::string> filaments;
    };

    // Qidi-specific methods
    bool fetch_slot_info(const std::string&        base_url,
                         const std::string&        api_key,
                         const QidiFilamentDict&   dict,
                         const std::string&        series_id,
                         std::vector<AmsTrayData>& trays,
                         int&                      box_count,
                         std::string&              error);
    bool fetch_filament_dict(const std::string& base_url, const std::string& api_key, QidiFilamentDict& dict, std::string& error) const;
    std::string normalize_filament_type(const std::string& filament_type);
    std::string infer_series_id(const std::string& model_id, const std::string& dev_name);
    std::string normalize_model_key(std::string value);

    // Static helpers
    static void parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result);
    static void parse_filament_sections(const std::string& content, std::map<int, std::string>& result);
    static std::string map_filament_type_to_setting_id(const std::string& filament_type);
};

} // namespace Slic3r

#endif
