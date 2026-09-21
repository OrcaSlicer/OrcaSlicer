#ifndef slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_
#define slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_

#include "PrinterAgentPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

#include "IPrinterAgent.hpp"
#include "pybind11/pybind11.h"
#include <slic3r/plugin/PluginAuditManager.hpp>
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <string>

namespace Slic3r {
class PyPrinterAgentPluginCapabilityTrampoline : public PyPluginCommonTrampoline<PrinterAgentPluginCapability>
{
public:
    using PyPluginCommonTrampoline<PrinterAgentPluginCapability>::PyPluginCommonTrampoline;

    AgentInfo get_agent_info() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, AgentInfo, PrinterAgentPluginCapability,
            get_agent_info);
    }

    int connect_printer(std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, connect_printer, dev_id,
            dev_ip, username, password, use_ssl);
    }

    int disconnect_printer() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, disconnect_printer);
    }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, send_message, dev_id,
            json_str, qos, flag);
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, send_message_to_printer,
            dev_id, json_str, qos, flag);
    }

    int command_ams_refresh_rfid(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_ams_refresh_rfid,
            dev_id, tray_id, sequence_id, lan_mode);
    }

    int command_ams_calibrate(std::string dev_id, int ams_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_ams_calibrate,
            dev_id, ams_id, sequence_id, lan_mode);
    }

    int command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_ams_select_tray,
            dev_id, tray_id, sequence_id, lan_mode);
    }

    int command_start_camera(std::string dev_id) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_start_camera, dev_id);
    }

    int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_xyz_abs,
            dev_id, sequence_id, lan_mode);
    }

    int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_auto_leveling,
            dev_id, sequence_id, lan_mode);
    }

    int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing,
                        int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_go_home,
            dev_id, is_printing, supports_mqtt_homing, sequence_id, lan_mode);
    }

    int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl,
                        int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_set_bed,
            dev_id, temp, supports_mqtt_bed_ctrl, sequence_id, lan_mode);
    }

    int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_set_nozzle,
            dev_id, temp, sequence_id, lan_mode);
    }

    int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val,
                             int speed, bool is_core_xy, bool supports_mqtt_axis_control,
                             int sequence_id, bool lan_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, command_axis_control,
            dev_id, axis, unit, input_val, speed, is_core_xy, supports_mqtt_axis_control,
            sequence_id, lan_mode);
    }

    bool start_discovery(bool start, bool sending) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, bool, PrinterAgentPluginCapability, start_discovery, start,
            sending);
    }

    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, bind_detect, dev_ip,
            sec_link, detect);
    }

    std::string get_user_selected_machine() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, std::string, PrinterAgentPluginCapability,
            get_user_selected_machine);
    }

    int set_user_selected_machine(std::string dev_id) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability,
            set_user_selected_machine, dev_id);
    }

    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability,
            start_send_gcode_to_sdcard, params, update_fn, cancel_fn, wait_fn);
    }

    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, start_local_print,
            params, update_fn, cancel_fn);
    }

    FilamentSyncMode get_filament_sync_mode() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, FilamentSyncMode, PrinterAgentPluginCapability,
            get_filament_sync_mode);
    }

    CameraStreamMode get_camera_stream_mode() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, CameraStreamMode, PrinterAgentPluginCapability,
            get_camera_stream_mode);
    }

    std::string get_camera_url() const override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, std::string, PrinterAgentPluginCapability, get_camera_url);
    }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, bool, PrinterAgentPluginCapability, fetch_filament_info, dev_id, sync_mode);
    }

    int check_cert() override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, check_cert);
    }

    void install_device_cert(std::string dev_id, bool lan_only) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, void, PrinterAgentPluginCapability, install_device_cert, dev_id,
            lan_only);
    }

    int ping_bind(std::string ping_code) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, ping_bind, ping_code);
    }

    int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, bind, dev_ip, dev_id,
            dev_model, sec_link, timezone, improved, update_fn);
    }

    int unbind(std::string dev_id) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, unbind, dev_id);
    }

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, start_print, params,
            update_fn, cancel_fn, wait_fn);
    }

    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability,
            start_local_print_with_record, params, update_fn, cancel_fn, wait_fn);
    }

    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, start_sdcard_print, params,
            update_fn, cancel_fn);
    }

    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE, int, PrinterAgentPluginCapability, get_hms_snapshot, dev_id,
            file_name, callback);
    }

    int set_server_callback(OnServerErrFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_server_callback, fn);
    }

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_ssdp_msg_fn, fn);
    }

    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_printer_connected_fn,
            fn);
    }

    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_subscribe_failure_fn,
            fn);
    }

    int set_on_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_message_fn, fn);
    }

    int set_on_user_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_user_message_fn, fn);
    }

    int set_on_local_connect_fn(OnLocalConnectedFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_local_connect_fn, fn);
    }

    int set_on_local_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_on_local_message_fn, fn);
    }

    int set_queue_on_main_fn(QueueOnMainFn fn) override
    {
        ORCA_PY_OVERRIDE_AUDITED(
            [] {}, PYBIND11_OVERRIDE_PURE, int, PrinterAgentPluginCapability, set_queue_on_main_fn, fn);
    }

    // request_bind_ticket returns its ticket through a std::string* out-param, which pybind11
    // cannot marshal back through a plain override. We dispatch manually: the Python plugin
    // returns a (result, ticket) tuple, which we unpack into the int result and the out-param.
    // Not required to be implemented, so a missing override falls back to the base default
    // instead of failing, mirroring what PYBIND11_OVERRIDE does for the other optional methods.
    int request_bind_ticket(std::string* ticket) override
    {
        ORCA_PY_AUDIT_SCOPE();
        ::Slic3r::PluginCapabilityInterface::RefCounter _orca_ref_counter(*this);
        ::Slic3r::PythonGILState gil;
        if (!gil)
            throw std::runtime_error("Python interpreter is shutting down");
        pybind11::function override =
            pybind11::get_override(static_cast<const PrinterAgentPluginCapability*>(this), "request_bind_ticket");
        if (!override)
            return PrinterAgentPluginCapability::request_bind_ticket(ticket);
        try {
            pybind11::tuple result = override().cast<pybind11::tuple>();
            if (ticket)
                *ticket = result[1].cast<std::string>();
            return result[0].cast<int>();
        } catch (pybind11::error_already_set& err) {
            ::Slic3r::log_python_exception_keep(err);
            throw;
        }
    }
};
} // namespace Slic3r

#endif /* slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_ */
