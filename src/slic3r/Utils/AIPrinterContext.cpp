#include "AIPrinterContext.hpp"

#include <boost/log/trivial.hpp>
#include <boost/beast/core/detail/base64.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Preset.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "Http.hpp"

namespace Slic3r {

// ---------------------------------------------------------------------------
// Slicer context (pure, testable — no GUI, no network)
// ---------------------------------------------------------------------------

nlohmann::json AIPrinterContext::slicer_context_from_config(const DynamicPrintConfig &cfg)
{
    // opt_serialize() renders ANY option type (scalar, per-extruder vector,
    // enum, point-list) to a stable string, so we never guess a C++ type and
    // never dereference a missing option. Guard on existence first.
    auto put = [&cfg](nlohmann::json &dst, const char *out, const char *key) {
        if (cfg.option(key) != nullptr)
            dst[out] = cfg.opt_serialize(key);
    };

    nlohmann::json machine = nlohmann::json::object();
    put(machine, "nozzle_diameter",   "nozzle_diameter");     // per-extruder (mm)
    put(machine, "nozzle_type",       "nozzle_type");
    put(machine, "printable_height",  "printable_height");    // max Z (mm)
    put(machine, "printable_area",    "printable_area");      // bed polygon (NOT "bed_shape")
    put(machine, "gcode_flavor",      "gcode_flavor");        // e.g. klipper / marlin
    put(machine, "max_speed_x",       "machine_max_speed_x");
    put(machine, "max_speed_y",       "machine_max_speed_y");
    put(machine, "max_speed_z",       "machine_max_speed_z");
    put(machine, "max_speed_e",       "machine_max_speed_e");
    put(machine, "max_accel",         "machine_max_acceleration_extruding");
    put(machine, "max_accel_travel",  "machine_max_acceleration_travel");

    nlohmann::json filament = nlohmann::json::object();
    put(filament, "type",                    "filament_type");
    put(filament, "diameter",                "filament_diameter");
    put(filament, "flow_ratio",              "filament_flow_ratio");
    put(filament, "nozzle_temperature",      "nozzle_temperature");
    put(filament, "nozzle_temp_first_layer", "nozzle_temperature_initial_layer");
    put(filament, "bed_temperature",         "hot_plate_temp");
    put(filament, "bed_temp_first_layer",    "hot_plate_temp_initial_layer");

    nlohmann::json process = nlohmann::json::object();
    put(process, "layer_height",         "layer_height");
    put(process, "first_layer_height",   "initial_layer_print_height"); // NOT "first_layer_height"
    put(process, "sparse_infill_density","sparse_infill_density");

    nlohmann::json j = nlohmann::json::object();
    j["machine"]  = std::move(machine);
    j["filament"] = std::move(filament);
    j["process"]  = std::move(process);
    return j;
}

// ---------------------------------------------------------------------------
// Assembly helpers (GUI + network)
// ---------------------------------------------------------------------------

namespace {

// "192.168.1.11" / "http://192.168.1.11:7125/" -> "http://192.168.1.11:7125"
std::string normalize_base(std::string host)
{
    if (host.empty())
        return host;
    if (host.find("://") == std::string::npos)
        host = "http://" + host;
    while (! host.empty() && host.back() == '/')
        host.pop_back();
    return host;
}

// Extract just the bare host (no scheme, no port, no path): used to reach the
// camera on its own port. "http://192.168.1.11:7125/x" -> "192.168.1.11"
std::string extract_hostname(const std::string &host)
{
    std::string h = host;
    auto scheme = h.find("://");
    if (scheme != std::string::npos)
        h = h.substr(scheme + 3);
    h = h.substr(0, h.find_first_of("/"));   // drop path
    h = h.substr(0, h.find_first_of(":"));   // drop port
    return h;
}

std::string base64_jpeg(const std::string &bytes)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out;
    out.resize(b64::encoded_size(bytes.size()));
    out.resize(b64::encode(&out[0], bytes.data(), bytes.size()));
    return out;
}

} // namespace

AIPrinterContext AIPrinterContext::gather()
{
    AIPrinterContext ctx;

    auto *bundle = GUI::wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return ctx; // nothing available (shouldn't happen in a running GUI)

    // 1) Slicer settings — always.
    try {
        ctx.slicer = slicer_context_from_config(bundle->full_config());
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "AIPrinterContext: slicer context failed: " << e.what();
    }

    // 2/3) Live host + camera — only when a Physical Printer is selected.
    DynamicPrintConfig *pcfg = bundle->physical_printers.get_selected_printer_config();
    if (pcfg == nullptr)
        return ctx;

    const std::string host = pcfg->opt_string("print_host");
    if (host.empty())
        return ctx;

    PrintHostType host_type = htOctoPrint;
    if (auto *ht = pcfg->option<ConfigOptionEnum<PrintHostType>>("host_type"))
        host_type = ht->value;

    // Live telemetry is Moonraker/OctoPrint-shaped; degrade for other hosts.
    if (host_type == htMoonraker || host_type == htOctoPrint) {
        const std::string url = normalize_base(host) +
            "/printer/objects/query?extruder&heater_bed&toolhead&print_stats"
            "&bed_mesh&gcode_move&probe&configfile";
        std::string body; bool ok = false;
        Http::get(url)
            .timeout_connect(3)
            .on_complete([&](std::string b, unsigned status) { if (status >= 200 && status < 300) { body = std::move(b); ok = true; } })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(info) << "AIPrinterContext: live query failed (" << status << "): " << error;
            })
            .perform_sync();
        if (ok) {
            try {
                nlohmann::json j = nlohmann::json::parse(body);
                if (j.contains("result") && j["result"].contains("status")) {
                    ctx.live = j["result"]["status"];
                    ctx.live_available = true;
                }
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(info) << "AIPrinterContext: live parse failed: " << e.what();
            }
        }
    }

    // 3) Camera snapshot (mjpg-streamer convention: :8080/?action=snapshot).
    const std::string cam_host = extract_hostname(host);
    if (! cam_host.empty()) {
        const std::string cam_url = "http://" + cam_host + ":8080/?action=snapshot";
        std::string jpeg; bool ok = false;
        Http::get(cam_url)
            .timeout_connect(3)
            .on_complete([&](std::string b, unsigned status) { if (status >= 200 && status < 300 && ! b.empty()) { jpeg = std::move(b); ok = true; } })
            .on_error([](std::string, std::string, unsigned) { /* no camera: fine */ })
            .perform_sync();
        if (ok) {
            ctx.camera_jpeg_base64 = base64_jpeg(jpeg);
            ctx.camera_available   = ! ctx.camera_jpeg_base64.empty();
        }
    }

    return ctx;
}

// ---------------------------------------------------------------------------
// Prompt rendering
// ---------------------------------------------------------------------------

std::string AIPrinterContext::to_prompt() const
{
    nlohmann::json j = nlohmann::json::object();
    j["slicer"] = slicer;
    if (live_available)
        j["live_printer"] = live;
    j["camera_available"] = camera_available;
    return j.dump(2);
}

} // namespace Slic3r
