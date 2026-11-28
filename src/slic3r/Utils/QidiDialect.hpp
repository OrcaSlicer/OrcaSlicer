#ifndef slic3r_QidiDialect_hpp_
#define slic3r_QidiDialect_hpp_

#include <string>
#include <nlohmann/json.hpp>

namespace Slic3r {

class MachineObject;

// Qidi: the one place where BBL-style command JSON is translated into the gcode
// dialect Qidi printers speak (Klipper macros / Marlin codes - the same local
// scripts QIDI Studio's PrinterTaskDispatcher dispatches).
//
// MachineObject::publish_json() funnels every command_* JSON builder through
// here when the printer's agent is QIDI_PRINTER_AGENT_ID:
//   PassThrough - the JSON (e.g. "gcode_line") goes to the printer untouched;
//   SendGcode   - gcode_out holds a ready-made gcode script, sent via publish_gcode();
//   Drop        - nothing is sent; publish_json() acknowledges with 0.
//
// Translations whose source data is not fully present in the JSON (e.g.
// ams_filament_settings tray_vendor) stay as local branches in the command_*
// methods; those bypass this module.
class QidiDialect {
public:
    enum class Verdict { PassThrough, SendGcode, Drop };

    // j      - the full BBL-style publish JSON (j["print"] / j["system"] / j["xcam"]);
    // ctx    - the MachineObject issuing the command (source of m_partskip_names etc.);
    // gcode_out - filled with the script when the verdict is SendGcode.
    static Verdict translate(const nlohmann::json& j, const MachineObject* ctx, std::string& gcode_out);
};

} // namespace Slic3r

#endif