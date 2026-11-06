#ifndef slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_
#define slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_

#include "PrinterAgentPluginCapability.hpp"
#include "../../PyPluginTrampoline.hpp"

#include "IPrinterAgent.hpp"
#include "slic3r/Utils/bambu_networking.hpp"
#include "pybind11/pybind11.h"
#include <exception>
#include <functional>
#include <pybind11/cast.h>
#include <pybind11/pytypes.h>
#include <boost/log/trivial.hpp>
#include <slic3r/plugin/PluginAuditManager.hpp>
#include "slic3r/plugin/PluginManager.hpp"
#include "slic3r/plugin/PythonInterpreter.hpp"
#include <slic3r/plugin/PythonPluginInterface.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

#include <type_traits>

// IPrinterAgent reports failure through its return values and its callers do not catch, so nothing
// the plugin does may leave the trampoline as an exception: a Python raise, a missing override or a
// wrongly typed return is logged and answered with what NetworkAgent returns when no agent is set.
#define ORCA_PY_AGENT_CATCH(name) \
    catch (const std::exception& ex) { this->log_failure(#name, ex.what()); } \
    catch (...) { this->log_failure(#name, "unknown error"); }

#define ORCA_PY_AGENT_OVERRIDE(ret, name, ...) \
    try { \
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE_PURE, ret, PrinterAgentPluginCapability, name, ##__VA_ARGS__); \
    } ORCA_PY_AGENT_CATCH(name) \
    return printer_agent_failure<ret>()

#define ORCA_PY_AGENT_OVERRIDE_DEFAULT(ret, name, ...) \
    try { \
        ORCA_PY_OVERRIDE_AUDITED([] {}, PYBIND11_OVERRIDE, ret, PrinterAgentPluginCapability, name, ##__VA_ARGS__); \
    } ORCA_PY_AGENT_CATCH(name) \
    return printer_agent_failure<ret>()

namespace Slic3r {
// NetworkAgent's no-agent answer: -1 for a status code, the empty value (false, "", none) otherwise.
template<typename T> T printer_agent_failure()
{
    if constexpr (std::is_same_v<T, int>)
        return -1;
    else if constexpr (!std::is_void_v<T>)
        return T{};
}

class PyPrinterAgentPluginCapabilityTrampoline : public PyPluginCommonTrampoline<PrinterAgentPluginCapability>
{
public:
    using PyPluginCommonTrampoline<PrinterAgentPluginCapability>::PyPluginCommonTrampoline;

    AgentInfo get_agent_info() override
    {
        ORCA_PY_AGENT_OVERRIDE(AgentInfo, get_agent_info);
    }

    int connect_printer(const PrinterConnectionParams& params) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, connect_printer, params);
    }

    int disconnect_printer() override
    {
        // While the app closes, the UI thread waits here; a plugin that never returns would keep it
        // open, so the call gets a deadline and is left running past it.
        if (!PluginManager::instance().is_shutting_down())
            return python_disconnect_printer();
        return disconnect_printer_at_shutdown();
    }

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, send_message, dev_id, json_str, qos, flag);
    }

    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, send_message_to_printer, dev_id, json_str, qos, flag);
    }

    int command_ams_refresh_rfid(std::string dev_id, int ams_id,int slot_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_ams_refresh_rfid, dev_id, ams_id, slot_id, sequence_id, lan_mode);
    }

    int command_ams_calibrate(std::string dev_id, int ams_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_ams_calibrate, dev_id, ams_id, sequence_id, lan_mode);
    }

    int command_ams_select_tray(std::string dev_id, std::string tray_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_ams_select_tray, dev_id, tray_id, sequence_id, lan_mode);
    }

    int command_start_camera(std::string dev_id) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_start_camera, dev_id);
    }

    int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_xyz_abs, dev_id, sequence_id, lan_mode);
    }

    int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_auto_leveling, dev_id, sequence_id, lan_mode);
    }

    int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing,
                        int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_go_home, dev_id, is_printing, supports_mqtt_homing, sequence_id, lan_mode);
    }

    int command_set_bed(std::string dev_id, int temp, bool supports_mqtt_bed_ctrl,
                        int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_set_bed, dev_id, temp, supports_mqtt_bed_ctrl, sequence_id, lan_mode);
    }

    int command_set_nozzle(std::string dev_id, int temp, int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_set_nozzle, dev_id, temp, sequence_id, lan_mode);
    }

    int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val,
                             int speed, bool is_core_xy, bool supports_mqtt_axis_control,
                             int sequence_id, bool lan_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, command_axis_control, dev_id, axis, unit, input_val, speed,
                                       is_core_xy, supports_mqtt_axis_control, sequence_id, lan_mode);
    }

    bool start_discovery(bool start, bool sending) override
    {
        ORCA_PY_AGENT_OVERRIDE(bool, start_discovery, start, sending);
    }

    int bind_detect(std::string dev_ip, std::string sec_link, detectResult& detect) override
    {
        // Passed as a pointer: pybind11 copies a reference argument, so the plugin's writes would be lost.
        ORCA_PY_AGENT_OVERRIDE(int, bind_detect, dev_ip, sec_link, &detect);
    }

    std::string get_user_selected_machine() override
    {
        ORCA_PY_AGENT_OVERRIDE(std::string, get_user_selected_machine);
    }

    int set_user_selected_machine(std::string dev_id) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_user_selected_machine, dev_id);
    }

    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_send_gcode_to_sdcard, params, update_fn, cancel_fn, wait_fn);
    }

    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_local_print, params, update_fn, cancel_fn);
    }

    FilamentSyncMode get_filament_sync_mode() const override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(FilamentSyncMode, get_filament_sync_mode);
    }

    CameraStreamMode get_camera_stream_mode() const override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(CameraStreamMode, get_camera_stream_mode);
    }

    std::string get_camera_url() const override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(std::string, get_camera_url);
    }

    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(bool, fetch_filament_info, dev_id, sync_mode);
    }

    int check_cert() override
    {
        ORCA_PY_AGENT_OVERRIDE(int, check_cert);
    }

    void install_device_cert(std::string dev_id, bool lan_only) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(void, install_device_cert, dev_id, lan_only);
    }

    int ping_bind(std::string ping_code) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, ping_bind, ping_code);
    }

    int bind(std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, bind, dev_ip, dev_id, dev_model, sec_link, timezone, improved, update_fn);
    }

    int unbind(std::string dev_id) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, unbind, dev_id);
    }

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_print, params, update_fn, cancel_fn, wait_fn);
    }

    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_local_print_with_record, params, update_fn, cancel_fn, wait_fn);
    }

    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, start_sdcard_print, params, update_fn, cancel_fn);
    }

    int get_hms_snapshot(std::string dev_id, std::string file_name, std::function<void(std::string, int)> callback) override
    {
        ORCA_PY_AGENT_OVERRIDE_DEFAULT(int, get_hms_snapshot, dev_id, file_name, callback);
    }

    int set_server_callback(OnServerErrFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_server_callback, fn);
    }

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_ssdp_msg_fn, fn);
    }

    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_printer_connected_fn, fn);
    }

    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_subscribe_failure_fn, fn);
    }

    int set_on_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_message_fn, fn);
    }

    int set_on_user_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_user_message_fn, fn);
    }

    int set_on_local_connect_fn(OnLocalConnectedFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_local_connect_fn, fn);
    }

    int set_on_local_message_fn(OnMessageFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_on_local_message_fn, fn);
    }

    int set_queue_on_main_fn(QueueOnMainFn fn) override
    {
        ORCA_PY_AGENT_OVERRIDE(int, set_queue_on_main_fn, fn);
    }

    // request_bind_ticket returns its ticket through a std::string* out-param, which pybind11
    // cannot marshal back through a plain override. We dispatch manually: the Python plugin
    // returns a (result, ticket) tuple, which we unpack into the int result and the out-param.
    // Not required to be implemented, so a missing override answers with the same failure value
    // as the other printer-agent operations. Leave the out-param untouched on failure.
    int request_bind_ticket(std::string* ticket) override
    {
        try {
            ::Slic3r::PluginCapabilityInterface::RefCounter _orca_ref_counter(*this);
            ::Slic3r::PythonGILState gil;
            if (!gil)
                throw std::runtime_error("Python interpreter is shutting down");
            ORCA_PY_AUDIT_SCOPE();
            pybind11::function override =
                pybind11::get_override(static_cast<const PrinterAgentPluginCapability*>(this), "request_bind_ticket");
            if (!override)
                return printer_agent_failure<int>();
            try {
                pybind11::tuple result = override().cast<pybind11::tuple>();
                if (ticket)
                    *ticket = result[1].cast<std::string>();
                return result[0].cast<int>();
            } catch (pybind11::error_already_set& err) {
                ::Slic3r::log_python_exception_keep(err);
                throw;
            }
        } ORCA_PY_AGENT_CATCH(request_bind_ticket)
        return printer_agent_failure<int>();
    }

private:
    int python_disconnect_printer()
    {
        ORCA_PY_AGENT_OVERRIDE(int, disconnect_printer);
    }

    // One disconnect per agent at shutdown: the live agent and the agent cache both disconnect the
    // same object, and a plugin need not handle two concurrent calls. Shared with the call's thread,
    // which can outlive an abandoned wait.
    struct ShutdownDisconnect
    {
        enum : int { Pending, Started, Abandoned };
        std::atomic<int>                      state{Pending};
        std::promise<int>                     result;
        std::shared_future<int>               done{result.get_future().share()};
        std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::now() + std::chrono::seconds(3)};
    };

    int disconnect_printer_at_shutdown()
    {
        std::shared_ptr<ShutdownDisconnect> call;
        bool                                start = false;
        {
            std::lock_guard<std::mutex> lock(m_shutdown_disconnect_mutex);
            if (!m_shutdown_disconnect) {
                m_shutdown_disconnect = std::make_shared<ShutdownDisconnect>();
                start                 = true;
            }
            call = m_shutdown_disconnect;
        }
        if (start) {
            try {
                std::thread([agent = this, call] { run_shutdown_disconnect(agent, *call); }).detach();
            } catch (const std::system_error&) {
                // No thread, so no deadline either: call the plugin directly as before.
                call->state.store(ShutdownDisconnect::Started);
                const int result = python_disconnect_printer();
                call->result.set_value(result);
                return result;
            }
        }
        if (call->done.wait_until(call->deadline) == std::future_status::ready)
            return call->done.get();
        int expected = ShutdownDisconnect::Pending;
        call->state.compare_exchange_strong(expected, ShutdownDisconnect::Abandoned);
        log_failure("disconnect_printer", "did not return within 3 s while the app is closing");
        return printer_agent_failure<int>();
    }

    // Static so that nothing touches the agent, which may already be gone, before the call is claimed.
    static void run_shutdown_disconnect(PyPrinterAgentPluginCapabilityTrampoline* agent, ShutdownDisconnect& call)
    {
        ::Slic3r::PythonGILState gil;
        int                      expected = ShutdownDisconnect::Pending;
        if (!gil || !call.state.compare_exchange_strong(expected, ShutdownDisconnect::Started)) {
            // Abandoned before it started: the agent may already be gone, so it is not touched.
            call.result.set_value(printer_agent_failure<int>());
            return;
        }
        // The plugin may be force-unloaded while the call runs past its deadline. This reference keeps
        // the Python object, and with it the agent, alive until the call has fully unwound; it is
        // released before the GIL.
        const pybind11::object self = pybind11::cast(static_cast<PrinterAgentPluginCapability*>(agent),
                                                     pybind11::return_value_policy::reference);
        call.result.set_value(agent->python_disconnect_printer());
    }

    std::mutex                          m_shutdown_disconnect_mutex;
    std::shared_ptr<ShutdownDisconnect> m_shutdown_disconnect;

    void log_failure(const char* operation, const char* error) const
    {
        BOOST_LOG_TRIVIAL(error) << "Printer agent plugin '" << this->audit_plugin_key() << "': " << operation << " failed: " << error;
    }
};
} // namespace Slic3r

#endif /* slic3r_PrinterAgentPluginCapabilityTrampoline_hpp_ */
