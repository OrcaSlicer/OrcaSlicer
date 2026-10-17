#pragma once

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"

#include "nlohmann/json.hpp"

#include <map>
#include <string>

namespace Slic3r {

// Flashforge printers running Zmod (https://github.com/ghzserg/zmod): the Creator 5 / 5 Pro tool
// changer and the AD5X with its material station. Filament comes from Zmod's spool slots. Without
// Flashforge's own screen a print starts through Zmod's PRINT_ZCOLOR, which assigns the file's
// tools to slots and takes the dialog's bed leveling and Auto PA choices; with that screen,
// Flashforge's app runs the printer, and the print goes to the app's local API the way
// Flashforge's slicer sends it. The dialog's Timelapse option is Zmod's Timelapse plugin, offered
// while the plugin is enabled.
class ZmodPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit ZmodPrinterAgent(std::string log_dir);
    ~ZmodPrinterAgent() override = default;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

protected:
    int  upload_and_start(const PrintParams&      params,
                          const std::string&      gcode_path,
                          const std::string&      filename,
                          const OnUpdateStatusFn& update_fn,
                          const WasCancelledFn&   cancel_fn) override;
    int  start_uploaded_print(const PrintParams& params, const std::string& filename) override;
    bool supports_flow_calibration() const override { return true; }
    bool supports_timelapse() const override;

private:
    bool           moonraker_get(const std::string& path, std::string& body) const;
    // Turns the Timelapse plugin (moonraker-timelapse) on or off for the next print.
    void           set_timelapse_plugin(bool enabled) const;
    // Zmod's zmod_color status: its slots, and whether Flashforge's screen runs ("display").
    nlohmann::json zmod_color_status() const;
    // Where Flashforge's app serves its local API: the printer's Wi-Fi address.
    std::string flashforge_app_host() const;
    // Slot index (0-based) for each tool number of the uploaded file.
    std::map<int, int> tool_slots(const PrintParams& params) const;
    int                upload_to_flashforge_app(const PrintParams&      params,
                                                const std::string&      gcode_path,
                                                const std::string&      filename,
                                                const nlohmann::json&   zmod_color,
                                                const OnUpdateStatusFn& update_fn,
                                                const WasCancelledFn&   cancel_fn);
};

} // namespace Slic3r
