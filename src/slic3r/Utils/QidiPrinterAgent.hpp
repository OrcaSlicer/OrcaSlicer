#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

class QidiPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override;

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Override filament sync (Qidi-specific implementation)
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    // Qidi's gcode dialect replaces four BBL JSON commands that Moonraker's handle_request
    // would otherwise swallow: RFID read becomes READ_RFID SLOT, tray selection has no
    // printer-side counterpart (Box macros pick the slot themselves) and the camera is an
    // HTTP stream owned by get_camera_url() above.
    int command_ams_refresh_rfid(std::string dev_id, int ams_id, int slot_id, int sequence_id, bool lan_mode) override;
    int command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override;
    int command_start_camera(std::string dev_id) override;

    // Live view: Qidi printers serve the camera through mjpg-streamer (/webcam/), not the
    // Bambu port-6000 stream, so the probe below resolves the snapshot endpoint of the
    // connected printer and MediaPlayCtrl polls it as an agent-owned stream.
    CameraStreamMode get_camera_stream_mode() const override
    {
        resolve_camera();
        return m_camera_mode;
    }
    std::string get_camera_url() const override
    {
        resolve_camera();
        return m_camera_url;
    }

    // Builds the SAVE_VARIABLE lines that make each T<n> macro load the Box slot picked for filament n.
    // ams_mapping_info (optional) carries the raw target per filament (255 = external spool holder),
    // which the v0 ams_mapping array has already flattened to -1.
    static std::string build_tool_slot_script(const std::string& ams_mapping, const std::string& ams_mapping_info = std::string());
    // Builds the body of Moonraker's print start for an uploaded file, naming the plate to print when it is a 3MF
    static nlohmann::json print_start_request(const PrintParams& params, const std::string& filename);

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
    // Starts (once per connection) a background probe for the printer's webcam endpoint;
    // getters above return none/empty until the probe commits its result.
    void resolve_camera() const;
    // Runs the actual probe on a worker thread: tries the Moonraker webcam registry first,
    // then mjpg-streamer's /webcam/ on the connection origin and on the default port.
    void probe_camera(std::string base_url, std::string api_key) const;
    // GETs url and reports whether it answered 200 (cheap liveness check for snapshot URLs)
    static bool probe_snapshot(const std::string& url, const std::string& api_key);

    mutable std::mutex       m_camera_mutex;
    // base_url the committed (or in-flight) probe belongs to; a change re-arms the probe
    mutable std::string      m_camera_probe_base;
    mutable std::string      m_camera_url;
    mutable CameraStreamMode m_camera_mode { CameraStreamMode::none };
    mutable bool             m_camera_probe_running { false };
    mutable std::thread      m_camera_probe;

    bool fetch_slot_info(const std::string&        base_url,
                         const std::string&        api_key,
                         const QidiFilamentDict&   dict,
                         const std::string&        series_id,
                         std::vector<AmsTrayData>& trays,
                         int&                      box_count,
                         std::string&              error);
    bool fetch_filament_dict(const std::string& base_url, const std::string& api_key, QidiFilamentDict& dict, std::string& error) const;
    // Publishes the external spool holder (Virtual Tray) to MachineObject::vt_slot so the
    // mapping dialogs show an Ext tile next to the Box slots. tray == nullptr clears it.
    void publish_external_spool(const AmsTrayData* tray) const;
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
