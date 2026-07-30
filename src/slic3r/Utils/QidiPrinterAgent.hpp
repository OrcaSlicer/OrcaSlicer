#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "MoonrakerPrinterAgent.hpp"
#include "nlohmann/json_fwd.hpp"

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
    bool fetch_filament_info(std::string dev_id) override;

    static bool parse_slot_response(const std::string& response_body,
                                    nlohmann::json&    status,
                                    nlohmann::json&    variables,
                                    std::string&       error);

    // Print operations — emit QiDi multi-color box config, then delegate to base.
    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

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
