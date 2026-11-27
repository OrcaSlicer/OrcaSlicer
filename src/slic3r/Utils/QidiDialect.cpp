#include "QidiDialect.hpp"

#include <boost/format.hpp>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

namespace Slic3r {

namespace {

const nlohmann::json* section(const nlohmann::json& j, const char* key)
{
    if (!j.is_object())
        return nullptr;
    auto it = j.find(key);
    if (it == j.end() || !it->is_object())
        return nullptr;
    return &*it;
}

std::string command_of(const nlohmann::json& s)
{
    return s.value("command", std::string());
}

// Fans 1..3 (AIR_FUN/FanType ids) mapped to the Klipper fan names QIDI Studio's
// PrinterTaskDispatcher sends (EVTSET_COOLLINGFAN/AUXILIARYFAN/CHAMBERFAN_SPEED).
// Returns empty string for fans Qidi does not expose (heatbreak, board, ...).
std::string qidi_fan_name(int fan_id)
{
    switch (fan_id) {
    case 1:  return "cooling_fan";
    case 2:  return "auxiliary_cooling_fan";
    case 3:  return "chamber_circulation_fan";
    default: return std::string();
    }
}

} // namespace

QidiDialect::Verdict QidiDialect::translate(const nlohmann::json& j, const MachineObject* ctx, std::string& gcode_out)
{
    gcode_out.clear();

    if (const nlohmann::json* p = section(j, "print")) {
        const std::string cmd = command_of(*p);

        // command_set_printing_speed: Marlin M220 with the QIDI Studio percent ladder.
        if (cmd == "print_speed") {
            static const int speed_pct[] = {0, 50, 100, 124, 166}; // indexed by DevPrintingSpeedLevel
            int idx = 0;
            try { idx = std::stoi(p->value("param", std::string("0"))); } catch (...) { idx = 0; }
            int pct = 100;
            if (idx > 0 && idx < (int)(sizeof(speed_pct) / sizeof(speed_pct[0])))
                pct = speed_pct[idx];
            gcode_out = (boost::format("M220 S%1%\n") % pct).str();
            return Verdict::SendGcode;
        }

        // command_task_partskip: no BBL skip_objects JSON - one EXCLUDE_OBJECT per
        // selected object, names taken from PartSkipDialog's m_partskip_names.
        if (cmd == "skip_objects") {
            std::string script;
            if (ctx && p->contains("obj_list") && (*p)["obj_list"].is_array()) {
                for (const auto& item : (*p)["obj_list"]) {
                    if (!item.is_number_integer())
                        continue;
                    const int part_id = item.get<int>();
                    auto       it      = ctx->m_partskip_names.find(part_id);
                    if (it == ctx->m_partskip_names.end() || it->second.empty())
                        continue;
                    script += "EXCLUDE_OBJECT NAME=" + it->second + "\n";
                }
            }
            if (script.empty())
                return Verdict::Drop;
            gcode_out = script;
            return Verdict::SendGcode;
        }

        // command_set_chamber / DevChamber::CtrlSetChamberTemp: BBL's set_ctt is
        // silently swallowed by Moonraker's agent - chamber heater is a Klipper pin.
        if (cmd == "set_ctt") {
            gcode_out = (boost::format("SET_HEATER_TEMPERATURE HEATER=chamber TARGET=%1%\n") % (int)p->value("ctt_val", 0)).str();
            return Verdict::SendGcode;
        }

        // command_ams_change_filament: load/unload are Box macros; slot 0..15 = Box
        // slots, 16 = external spool (Orca addresses it as ams 254/255). BBL marks
        // unload with slot_id 255; the real tray rides in tray_id (kept by the caller).
        if (cmd == "ams_change_filament") {
            const int ams         = p->value("ams_id", -1);
            const int slot_id     = p->value("slot_id", 255);
            const bool load       = (slot_id != 255); // the new protocol marks unload with 255
            const int tray        = load ? slot_id : p->value("tray_id", 0);
            int       slot        = 16;
            if (ams >= 0 && ams < 16)
                slot = ams * 4 + tray;
            std::string script;
            if (load) {
                const int new_temp = p->value("tar_temp", 0);
                if (new_temp > 0)
                    script += (boost::format("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=%1%\n") % new_temp).str();
                script += (boost::format("LOAD_FILAMENT_CONTROL SLOT=%1%") % slot).str();
            } else {
                script += (boost::format("UNLOAD_FILAMENT_CONTROL SLOT=%1%") % slot).str();
            }
            gcode_out = script;
            return Verdict::SendGcode;
        }

        // command_ams_user_settings: the two read-detection flags map onto the RFID-read
        // enables QIDI Studio sends (insert -> SET_ENABLE_RFID_READ, power-up ->
        // SET_ENABLE_INIT_READ_RFID); calibrate_remain_flag has no counterpart.
        if (cmd == "ams_user_setting") {
            const int tray_read  = p->value("tray_read_option", false) ? 1 : 0;
            const int start_read = p->value("startup_read_option", false) ? 1 : 0;
            gcode_out = (boost::format("SET_ENABLE_RFID_READ ENABLE=\"%1%\"\nSET_ENABLE_INIT_READ_RFID ENABLE=\"%2%\"\n")
                         % tray_read % start_read).str();
            return Verdict::SendGcode;
        }

        // command_extrusion_cali_set: same dialect QIDI Studio writes in
        // GCodeWriter::set_pressure_advance; only K is applied (N/SMOOTH_TIME has no
        // counterpart), matching the local branch that used to live in DeviceManager.
        if (cmd == "extrusion_cali_set") {
            gcode_out = (boost::format("SET_PRESSURE_ADVANCE ADVANCE=%.4f\n") % (double)p->value("k_value", 0.0)).str();
            return Verdict::SendGcode;
        }

        // command_ams_control: pause -> PAUSE, resume/done -> RESUME; "abort"/"reset"
        // are BBL transport actions with no dialect counterpart -> no-op (the printer
        // keeps its current state, paused handling is done on the printer).
        if (cmd == "ams_control") {
            const std::string action = p->value("param", std::string());
            if (action == "pause")  { gcode_out = "PAUSE";  return Verdict::SendGcode; }
            if (action == "done" || action == "resume") { gcode_out = "RESUME"; return Verdict::SendGcode; }
            return Verdict::Drop; // "abort", "reset"
        }

        // DevFan::command_control_air_duct: no BBL set_airduct - the polar-cooler
        // switch is a flat on/off pin (EVTSET_COOLER_SWITCH).
        if (cmd == "set_airduct") {
            const int value = (p->value("modeId", 0) > 0 || p->value("submode", 0) > 0) ? 1 : 0;
            gcode_out = (boost::format("SET_PIN PIN=polar_cooler VALUE=%1%\n") % value).str();
            return Verdict::SendGcode;
        }

        // DevFan::command_control_fan_new: no BBL set_fan JSON over the Moonraker path;
        // SET_FAN_SPEED takes a 0.00..1.00 fraction (val is a 0..100 percent).
        if (cmd == "set_fan") {
            const std::string name = qidi_fan_name(p->value("fan_index", 0));
            if (name.empty())
                return Verdict::Drop;
            double frac = p->value("speed", 0) / 100.0;
            if (frac < 0.0) frac = 0.0;
            if (frac > 1.0) frac = 1.0;
            gcode_out = (boost::format("SET_FAN_SPEED FAN=%1% SPEED=%.2f\n") % name % frac).str();
            return Verdict::SendGcode;
        }

        // MachineObject::command_extruder_control: no set_extrusion_length - feed/retract
        // directly (val < 0 retracts).
        if (cmd == "set_extrusion_length") {
            gcode_out = (boost::format("M83\nG1 E%1% F300\nG90\n") % p->value("length", 0)).str();
            return Verdict::SendGcode;
        }

        // DevCtrl::command_select_extruder: Qidi printers are single-nozzle - there is no
        // printer-side extruder to select, so acknowledge and keep only the PC-side target.
        if (cmd == "select_extruder")
            return Verdict::Drop;

        // Everything else above all: pass through untouched. "gcode_line" (what
        // publish_gcode builds) must - and does - take this path, so the hook cannot
        // recurse.
        return Verdict::PassThrough;
    }

    if (const nlohmann::json* s = section(j, "system")) {
        if (command_of(*s) == "ledctrl") {
            const std::string node = s->value("led_node", std::string());
            // one caselight pin, switched with SET_PIN (EVTSET_CASE_LIGHT);
            const std::string mode  = s->value("led_mode", std::string());
            const int         value = (mode == "on" || mode == "flashing") ? 1 : 0;
            if (node == "chamber_light") {
                gcode_out = "SET_PIN PIN=caselight VALUE=" + std::to_string(value);
                return Verdict::SendGcode;
            }
            // the second BBL light node has no counterpart - CtrlSetChamberLight fires
            // both, only the first must reach the printer.
            if (node == "chamber_light2")
                return Verdict::Drop;
        }
        return Verdict::PassThrough;
    }

    if (const nlohmann::json* x = section(j, "xcam")) {
        if (command_of(*x) == "xcam_control_set") {
            // noodle/spaghetti detector and the foreign-object (pre-print) check exist
            // printer-side as persistent variables; every other BBL xcam module has no
            // counterpart and is acknowledged silently.
            const std::string module_name = x->value("module_name", std::string());
            const bool        on_off      = x->value("control", false);
            const std::string lvl         = x->value("halt_print_sensitivity", std::string());
            if (module_name.rfind("spaghetti", 0) == 0) {
                std::string script = "SAVE_VARIABLE VARIABLE=enable_noodle_detection VALUE=" + std::string(on_off ? "1" : "0");
                if (!lvl.empty())
                    script += "\nSAVE_VARIABLE VARIABLE=noodle_sensitivity_level VALUE=\"'" + lvl + "'\"";
                gcode_out = script;
                return Verdict::SendGcode;
            }
            if (module_name == "fod_check") {
                gcode_out = "SAVE_VARIABLE VARIABLE=enable_pre_print_model_check VALUE=" + std::string(on_off ? "1" : "0");
                return Verdict::SendGcode;
            }
            return Verdict::Drop;
        }
        return Verdict::PassThrough;
    }

    return Verdict::PassThrough;
}

} // namespace Slic3r