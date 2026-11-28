#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class QidiPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override { shutdown(); }

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Override filament sync (Qidi-specific implementation)
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    static bool parse_slot_response(const std::string& response_body,
                                    nlohmann::json&    status,
                                    nlohmann::json&    variables,
                                    std::string&       error);

    FilamentSyncMode get_filament_sync_mode() const override;

    // Builds the SAVE_VARIABLE lines that make each T<n> macro load the Box slot picked for filament n
    static std::string build_tool_slot_script(const std::string& ams_mapping);
    // Builds the body of Moonraker's print start for an uploaded file, naming the plate to print when it is a 3MF
    static nlohmann::json print_start_request(const PrintParams& params, const std::string& filename);

protected:
    // Saves the Box config picked in the print dialog, then starts the print through Moonraker. Only the LAN print
    // path uploads and starts a print; the cloud/record/sdcard variants are inherited and return
    // ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED.
    bool print_uploaded_file(const PrintParams& params, const std::string& filename) override;
    std::string ams_unit_name(int ams_id) const override { return "Qidi Box " + std::to_string(ams_id + 1); }

private:
    // Push enable_box + value_t<tool> SAVE_VARIABLEs before a print starts.
    // Returns false if any command fails (caller should abort the print).
    bool apply_box_mapping(const PrintParams& params) const;
    struct QidiFilamentDict
    {
        std::map<int, std::string> colors;
        std::map<int, std::string> filaments;
    };

    // Qidi-specific methods
    bool fetch_slot_info(const ConnectionSettings& connection,
                         const QidiFilamentDict&   dict,
                         const std::string&        series_id,
                         std::vector<AmsTrayData>& trays,
                         int&                      box_count,
                         std::string&              error);
    bool fetch_filament_dict(const ConnectionSettings& connection, QidiFilamentDict& dict, std::string& error) const;
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
