#ifndef slic3r_AIPrinterContext_hpp_
#define slic3r_AIPrinterContext_hpp_

#include <string>

#include <nlohmann/json.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// ---------------------------------------------------------------------------
// AIPrinterContext
//
// A structured snapshot of the machine + material + process settings, plus
// (when a Physical Printer host is configured and reachable) live telemetry
// and an optional camera frame. It accompanies every AI request so generated
// geometry / g-code respects the REAL printer (bed size, nozzle, material,
// build volume, live temps, bed mesh, ...).
//
// Assembly never throws and never blocks the UI indefinitely: each source
// degrades gracefully when unavailable (offline slicing still yields the full
// slicer context; no host -> no live/camera).
// ---------------------------------------------------------------------------
struct AIPrinterContext
{
    // Slicer-side settings — ALWAYS available, even fully offline.
    nlohmann::json slicer = nlohmann::json::object();

    // Live printer telemetry — only when a Physical Printer host is configured
    // and answered (temps, bed mesh, homed axes, job state, ...).
    nlohmann::json live = nlohmann::json::object();
    bool           live_available = false;

    // A single camera frame as a base64-encoded JPEG (for vision models).
    std::string camera_jpeg_base64;
    bool        camera_available = false;

    // Assemble from the active slicer presets, plus live host + camera when a
    // Physical Printer is selected. GUI entry point (uses wxGetApp()).
    static AIPrinterContext gather();

    // Assemble ONLY the slicer portion from an explicit merged config. No GUI,
    // no network — this is the unit-testable core.
    static nlohmann::json slicer_context_from_config(const DynamicPrintConfig &full_config);

    // Compact JSON text suitable for a system prompt.
    std::string to_prompt() const;

    // The camera frame as base64 JPEG (empty when none). For multimodal calls.
    const std::string &to_image() const { return camera_jpeg_base64; }
    bool has_image() const { return camera_available && ! camera_jpeg_base64.empty(); }
};

} // namespace Slic3r

#endif // slic3r_AIPrinterContext_hpp_
